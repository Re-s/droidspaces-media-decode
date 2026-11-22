# VA-API driver for Qualcomm msm_vidc

`msm_drm_drv_video.so` is a libva plugin that opens the V4L2 decoder and encoder
from inside the application process. It assembles compressed decode input for
V4L2 OUTPUT, receives decoded pictures from CAPTURE, and delivers them through
VAImage or DMA-BUF. The encoder queues raw NV12 input and returns the bitstream
through `VACodedBufferSegment`.

## Codec and surface handling

| Decode profile | V4L2 codec ID | Input and output |
|---|---|---|
| H.264 Constrained Baseline, Main, High | 0 | Slice buffers contain the NAL units. The driver adds start codes, synthesizes SPS/PPS, and matches reordered output to submitted surfaces |
| HEVC Main and Main10 | 1 | The driver adds NAL start codes and synthesizes VPS/SPS/PPS. Main10 needs `HEVC10=1` |
| VP9 Profile 0 | 2 | The VA slice buffer already contains the full VP9 frame, including the uncompressed header |
| VP8 Version 0–3 | 3 | The driver reconstructs the frame tag before partition 0. The profile is advertised when the device supports it |
| AV1 Profile 0 | 4 | The driver reconstructs OBUs from tile payloads and VA picture parameters. Enable with `AV1=1` on compatible firmware |

HEVC short-term reference sets in an SPS are rebuilt as explicit slice-local
sets from VA DPB and POC data. The supported path accepts 4:2:0 Main and Main10
with complete long-format slice parameters. Streams that require an unavailable
SPS long-term reference table or custom scaling list, or whose slice boundaries
cannot be recovered, are rejected. Main10 chooses NV12 or P010 from the first
valid picture parameters and target surface. The bit depth stays fixed for the
session. See the [Main10/P010 validation record](../doc/hevc-main10-p010-validation.md).

The driver probes the selected encoder node for H.264, HEVC Main, and VP9
Profile 0 and advertises only the supported profile and format combinations.
The default node is `/dev/video33`. For H.264 and HEVC Main, a VA-API CQP request
uses VBR with the requested QP, or 26 if unspecified. HEVC devices without the
necessary QP controls use VBR alone. See the
[hardware encoding guide](../doc/hardware-encoding.md) for input limits, rate
control, and the validated device and application combinations.

Scaling and rotation use CPU-backed NV12 surfaces. For an input with a
90-degree display transform, pass `-noautorotate` before the input and request
the rotation explicitly. The dimensions below produce a 540×960 output.

```sh
-vf 'scale_vaapi=w=960:h=540:format=nv12,transpose_vaapi=dir=clock'
```

## Capture negotiation and frame delivery

- The driver tries DMABUF first and defers CAPTURE initialization until
  `SOURCE_CHANGE`. The legacy USERPTR path preconfigures CAPTURE and handles
  private `PORT_SETTINGS` events and `SESSION_CONTINUE`.
- Initial `G_FMT(CAPTURE)` can report stale geometry. The driver checks it
  against OUTPUT and corrects stride, slice height, and allocation size before
  using the buffer. During a midstream resolution change, the new CAPTURE
  geometry takes precedence. The private reconfiguration path flushes CAPTURE,
  waits for buffers to return, then reallocates and resumes that queue.
- VA-API associates submissions with surfaces. The driver tracks the input
  unit identity to pair CAPTURE output correctly, including B-frame reorder.
  Parameter sets are submitted without adding a pending surface.
- FFmpeg commonly synchronizes and reads through VAImage. Browsers can import
  a `DRM_PRIME_2` DMA-BUF descriptor. Chrome may export a surface before its
  pixels are ready. The background receiver checks for available output without
  blocking new submissions, and explicit synchronization keeps its timeout
  behavior. If Chrome recycles an exported target, late output still returns
  its CAPTURE buffer and retires the exact pending record.
- A DMA-BUF export shares the driver's surface with the application. Decoded
  pixels may still be copied from V4L2 CAPTURE into that surface. The
  [Chrome frame delivery notes](../docs/chrome-frame-delivery.md) cover ownership,
  pacing, and browser validation in more detail.

## Build and install

Use an AArch64 environment with GCC, Make, pkg-config, libva headers, and libdrm
headers. libva loads the shared object. The driver sends ioctls directly and
uses the two development packages for headers.

Run the following commands from the `vaapi-driver` directory.

```sh
make check tests
make BUILD=build-all-decode AV1=1 HEVC10=1 check tests
```

`check` verifies the exported `__vaDriverInit_*` symbol. `tests` runs
bitstream, profile, image, encode, telemetry, CAPTURE and receive tests without
hardware. Both opt-in decoding flags default to off and can be enabled
independently. Preserve the artifact name `msm_drm_drv_video.so`.

```sh
LIBVA_DRIVER_NAME=msm_drm LIBVA_DRIVERS_PATH="$PWD/build-all-decode" \
  vainfo --display drm --device /dev/dri/renderD128

sudo make BUILD=build-all-decode AV1=1 HEVC10=1 install
```

The default installation location is `/usr/lib/aarch64-linux-gnu/dri/`.
`make uninstall` removes the installed driver. `DESTDIR`, `PREFIX`, `LIBDIR`,
and `DRIDIR` are supported. The vendor string shows the version and build ID,
for example `DroidSpaces V4L2 VA-API driver 0.5.0-dev+<git-short-hash>`.
An explicit DRM display also works in an SSH session without Wayland or X11.

## Hardware verification

Run the multi-resolution regression with an FFmpeg build that supports libx265
and VA-API. Use absolute paths for the chosen driver build and source clip.

```sh
FFMPEG=ffmpeg DRIVER_DIR="$PWD/build-all-decode" \
  ./tests/regress_resolutions.sh /path/to/input.hevc
```

It compares hardware and software output at 1280×720, 854×480, 640×360, and
720×1280, plus a single-session 1080p → 720p → 480p switch. A 1080p-only test
can miss stale CAPTURE geometry because 1920×1088 is firmware's default on
the affected device. Some kernels expose the Venus regulator under
`/sys/devices/platform/soc/*gdsc/regulator/`. On the tested platform,
`mvs0_gdsc` read `enabled` during playback and `disabled` when idle.

For Chrome, confirm `VaapiVideoDecoder` in `chrome://media-internals` and verify
actual presented pixels and dropped frames. For Firefox, use the
[RDD setup guide](../doc/firefox-sandbox.md) and confirm hardware frame delivery
with driver logs. The [browser test harness](tests/browser/README.md) covers
frame order and pixel signatures.

### Diagnostic environment variables

| Variable | Effect |
|---|---|
| `DMD_VA_LOG=1` | Enable driver logs on stderr. Look for `session opened` and `match: frame` during a decode |
| `DMD_TRACE_ORDER=1` | Log input-unit and surface pairing through `ORDER submit` and `ORDER reap` |
| `DMD_VA_LUMA=1` | Sample Y and UV on surface export. For NV12, neutral chroma is around 128 and many near-zero UV samples can explain green output |
| `DMD_HARVEST_EXPORTED_ONLY=0` | Allow post-submit harvesting for targets without an export. By default it is limited to exported targets |
| `DMD_NO_MAP_WAIT=1` | Test Chrome-style sampling by skipping the fallback wait in image readback. See [regress_chrome_contract.sh](tests/regress_chrome_contract.sh) |
| `DMD_CSD_DUMP=1` | Log synthesized HEVC SPS bytes |
| `DMD_VA_TOLERATE_MISSING=1` | Report success on missing output for diagnostic stream sampling |
| `DMD_VA_ENCODE_DEVICE` | Override the default encoder device `/dev/video33` |
| `DMD_VA_ENCODE_TIMEOUT_MS` | Set the encoder hardware wait limit per frame. Defaults to 15000 ms |

Export-time luma and chroma diagnostics are useful for consumers that export
after synchronization. Chrome can export a newly allocated surface before it
has a picture. Use the browser harness to check what Chrome presents.

## Source layout

| Files | Responsibility |
|---|---|
| `src/driver.c`, `src/profiles.c`, `src/vtable.inc` | libva initialization, capabilities, and entry points |
| `src/decode.c`, `src/*_bitstream.c`, `src/hevc_slice.c` | Decode submissions, bitstream reconstruction, and surface pairing |
| `src/v4l2_backend.c`, `src/dmd_v4l2_*.c` | Device negotiation, session state, and receive |
| `src/image.c`, `src/export.c`, `src/vpp.c` | VAImage, DRM PRIME, and CPU-backed NV12 processing |
| `src/encode.c`, `src/v4l2_encoder.c` | Encode frontend and V4L2 backend |
| `src/video_stats.c` | [Per-context codec telemetry](../doc/video-stats-v1.md) |

The plugin returns `VA_STATUS_ERROR_*` on invalid input and logs to stderr
when enabled. Unimplemented vtable entries log the entry point and return
`VA_STATUS_ERROR_UNIMPLEMENTED`. The vtable generator reads libva's
`va_backend.h`, including signatures split across lines. Review changes to
generated stubs and slots when updating the libva headers.

See the [project README](../README.md) for browser setup, FFmpeg examples, and
tested devices. See [known limitations](../docs/known-limitations.md) for
stream-boundary, small-frame hardware-download, and power-testing limits.
