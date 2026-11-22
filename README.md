# DroidSpaces Media Decode

Hardware video decoding and encoding for Linux containers. The VA-API driver
runs inside the application process and talks directly to Qualcomm's msm_vidc
V4L2 interfaces. The default decoder is `/dev/video32` and the default encoder
is `/dev/video33`.

## Supported codecs

| Decode profile | Build and device requirements | Validation |
|---|---|---|
| H.264 Constrained Baseline, Main, High | Default build | Bit-exact FFmpeg output and Chrome 156 playback |
| HEVC Main | Default build | Bit-exact FFmpeg output and Chrome 156 playback |
| HEVC Main10 | `HEVC10=1` | NV12 for 8-bit streams and P010 for 10-bit streams. Bit-exact FFmpeg output and local Chrome 156 playback with ANGLE OpenGL |
| VP9 Profile 0 | Default build | Bit-exact native decoding in earlier tests. A Chrome YouTube run encountered repeated receive timeouts. See [upstream issue #7](https://github.com/Re-s/droidspaces-media-decode/issues/7) |
| VP8 | Default build, conditional on device support | Bit-exact on tested older hardware. The tested Snapdragon 8 Elite setup fell back to software in Chrome |
| AV1 Profile 0, 8-bit and 10-bit | `AV1=1` and compatible firmware | Bit-exact FFmpeg output and local Chrome 156 playback with ANGLE OpenGL |
| VP9 Profile 2 and MPEG-2 | Outside the supported configuration | MPEG-2 remains experimental and is not advertised by default |

The seven-case FFmpeg matrix on the integrated build compared 192 frames per
case with software decoding. It covered three H.264 profiles, HEVC Main and
Main10, and 8-bit and 10-bit AV1. Earlier AV1 tests also covered a 1,800-frame
1080p60 stream, 480p to 4K Chrome playback, three concurrent streams, and MSE
seeking and resolution switches. See the [AV1 status](doc/av1-v4l2-status.md)
and [Main10/P010 validation record](doc/hevc-main10-p010-validation.md) for
hardware and application details.

The 0.5.0-dev encoder probes its selected V4L2 node for H.264 Constrained
Baseline/Main/High, HEVC Main, and VP9 Profile 0. It accepts progressive 8-bit
NV12, I/P frames, one reference, and one slice per picture. Use `-bf 0` with
FFmpeg. H.264 and HEVC Main support CBR/VBR and a VBR-backed constant-QP
request. HRD/VBV and peak bitrate requests are firmware hints. Recreate the
encoding context to change stream settings.

FFmpeg encoding was verified for H.264 and HEVC Main on Snapdragon 8 Elite.
Chrome 156 WebCodecs encoded H.264 Baseline, Main, and High using the optional
NV12 GBM adapter. Chrome rejected the tested HEVC encode configuration.
See the [hardware encoding guide](doc/hardware-encoding.md) for supported
controls, verification, and the limits of the tested combinations.

## Build and install

Build in an AArch64 environment with `build-essential`, `pkg-config`,
`libva-dev`, and `libdrm-dev`. Run these commands from the repository root.

```sh
make -C vaapi-driver check tests
make -C vaapi-driver BUILD=build-all-decode AV1=1 HEVC10=1 check tests
```

The first command builds the default driver. The second enables both opt-in
decoders in a separate build directory. To build Main10 alone, use
`BUILD=build-hevc10 HEVC10=1 AV1=0`.

The container needs `libva2`, `libva-drm2`, and access to the render node, the
decoder, and a DMA-BUF allocator. The platform normally grants access through
the `droidspaces-gpu` group. Encoding also needs read/write access to the
encoder node. See the [platform contract](doc/platform-integration-contract.en.md)
or its [Chinese version](doc/platform-integration-contract.md).

Try the build as your normal user before installing it system-wide.

```sh
export LIBVA_DRIVER_NAME=msm_drm
export LIBVA_DRIVERS_PATH="$PWD/vaapi-driver/build-all-decode"

vainfo --display drm --device /dev/dri/renderD128
ffmpeg -vaapi_device /dev/dri/renderD128 -hwaccel vaapi \
  -hwaccel_output_format vaapi -i input.mp4 -an -f null -
```

`vainfo` reports the loaded build and the available profiles. A real decode
confirms that the device produces frames. The vendor string includes the
driver version and Git build ID, with a `-dirty` suffix for builds with
uncommitted changes.

Install the tested build into the system libva directory when ready.

```sh
sudo make -C vaapi-driver BUILD=build-all-decode AV1=1 HEVC10=1 install
```

libva expects the filename `msm_drm_drv_video.so` for the msm render node. Use
`LIBVA_DRIVERS_PATH` to select a separate build without replacing the system
copy.

## FFmpeg encoding and processing

Encode an H.264 clip after converting the source to NV12 and uploading it to
a VA-API surface.

```sh
LIBVA_DRIVER_NAME=msm_drm ffmpeg -vaapi_device /dev/dri/renderD128 \
  -i input.mp4 -vf format=nv12,hwupload -an \
  -c:v h264_vaapi -bf 0 -g 30 -rc_mode CBR -b:v 6M output.mp4
```

A VA-API hardware decode can also feed the encoder through the driver's NV12
video-processing path.

```sh
LIBVA_DRIVER_NAME=msm_drm ffmpeg \
  -init_hw_device vaapi=va:/dev/dri/renderD128 \
  -hwaccel vaapi -hwaccel_device va -hwaccel_output_format vaapi \
  -i input.mp4 -vf 'scale_vaapi=format=nv12' \
  -c:v h264_vaapi -bf 0 -b:v 2M -an output.mp4
```

Scaling and rotation operate on CPU-backed NV12 surfaces. See the
[driver README](vaapi-driver/README.md) for the rotation example and the
[encoding guide](doc/hardware-encoding.md) for crop and alignment details.

## Browser setup

### Chrome on GNOME Wayland

From the logged-in desktop user's Wayland session, build the driver as shown
above and install the per-user launcher. This example uses the tested Chrome
156 and Turnip setup. If libva already finds the intended system driver,
omit `LIBVA_DRIVERS_PATH`.

```sh
CHROME_BIN=/usr/bin/google-chrome-stable \
MESA_LOADER_DRIVER_OVERRIDE=kgsl TU_DEBUG=noconform \
LIBVA_DRIVERS_PATH="$PWD/vaapi-driver/build-all-decode" \
  sh tools/configure-chrome-vaapi.sh --wrapper-only

"$HOME/.local/bin/chrome-msm-vaapi"
```

Fully exit existing Chrome processes before launching the wrapper. Omit
`--wrapper-only` to configure the user-level desktop entries as well. The script
supports `--verify` and `--uninstall`. For WebCodecs encoding, build the
[optional NV12 GBM adapter](vaapi-driver/gbm-compat/README.md) and pass its
absolute directory using `--gbm-compat-dir`. Chrome 156 worked with the
default GMB readback behavior.

The wrapper selects Wayland, the msm render node, ANGLE OpenGL, and the VA-API
features. It disables `ResolutionBasedDecoderPriority` so Chrome can select
VA-API for small clips such as 320×180. Keep the Chromium feature named
`Vulkan` out of `--enable-features`. ANGLE backend selection is controlled
separately through `DMD_ANGLE`.

Check `chrome://media-internals` for `kVideoDecoderName = VaapiVideoDecoder`
during playback, then check the image and dropped frames. YouTube H.264 and
AV1 were verified. The HEVC Main and Main10 Chrome tests used local MP4 files.
See the [browser guide](doc/browser-vaapi-guide.md) for the exact flags, Vulkan
comparisons, and compositor requirements.

### Firefox

The tested native Firefox 160 AArch64/glibc setup uses the
[RDD compatibility launcher](doc/firefox-sandbox.md). Build it with
`make -C vaapi-driver firefox-compat`. The setup pairs a system policy with a
per-user launcher and retains the Mozilla sandboxes while granting limited KGSL
ioctl access. Run the browser from the desktop session and verify actual
decoded frames alongside `about:support`. The Firefox guide covers installation,
verification, and rollback.

For Chrome receive stalls, see [frame delivery](docs/chrome-frame-delivery.md)
and [known limitations](docs/known-limitations.md). Run
`bash tools/check-browser-vaapi.sh` for a quick check and enable
`DMD_VA_LOG=1` to inspect frame delivery when troubleshooting.

## Kernel compatibility and test devices

The driver tries `REQBUFS(DMABUF)` and defers CAPTURE setup until
`SOURCE_CHANGE` on the newer path. If DMABUF is rejected, it falls back to
USERPTR with the legacy private `PORT_SETTINGS` events and `SESSION_CONTINUE`.

- The legacy path was tested on a Xiaomi Pad 5 (`nabu`), Snapdragon 855
  / Adreno 640, with Android 13, kernel 4.14.336, and a Debian 13 AArch64
  DroidSpaces container.
- The modern path was tested on Snapdragon 8 Elite (SM8750P / Adreno 830)
  with Ubuntu 26.04 AArch64. The current Chrome validation used GNOME Wayland
  and Turnip. AV1 requires compatible firmware.

| Affected release | Symptom | Fix |
|---|---|---|
| 0.4.1 and earlier | FFmpeg `-f null -` decode could hang | 0.4.2 |
| 0.4.2 and earlier | Non-1080p video could appear green | 0.4.3 |
| 0.4.3 and earlier | Browser seeks or quality switches could stall for seconds | 0.4.4 |
| 0.4.5 and earlier | Chrome could present early frames out of order | 0.4.6 |

See [upgrade and pitfalls](doc/upgrade-and-pitfalls.md) for the causes and
current codec limitations. For detailed device behavior, see the
[platform verification notes](doc/verified-platform-facts.md) and the
[driver README](vaapi-driver/README.md).

## Documentation

| Document | Contents |
|---|---|
| [Driver README](vaapi-driver/README.md) | Driver internals, V4L2 negotiation, and verification |
| [Hardware encoding](doc/hardware-encoding.md) | Rate control, device permissions, and validated workflows |
| [Codec telemetry](doc/video-stats-v1.md) | Per-context counters and the shared record layout |
| [Browser frame-order tests](vaapi-driver/tests/browser/README.md) | Pixel verification across browser playback paths |
| [Jellyfin patches](patches/README.md) | Rotation and transcoding integration |
| [Changelog](CHANGELOG.md) | Release history |

## License

See [LICENSE](LICENSE).
