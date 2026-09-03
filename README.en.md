# DroidSpaces Media Decode

> 🌐 **中文版本：[README.md](README.md)**

Hardware video decoding for Linux containers: a VA-API driver that talks directly
to Qualcomm's msm_vidc V4L2 interface.

## Overview

Applications inside the container (ffmpeg, Firefox, Chrome) get hardware decoding
through the standard VA-API with **no changes required**. libva `dlopen`s the
driver into the consumer process, which then drives `/dev/video32` directly.

```
container ffmpeg / Firefox
   ↓  standard VA-API calls
libva → dlopen → msm_drm_drv_video.so     ← this project
   ↓  V4L2 stateful decoder ioctls
/dev/video32  (Qualcomm msm_vidc / Venus)
```

**Nothing from this project runs on the Android side.**

> ⚠️ **0.4.0 is an architectural rewrite.** 0.3.x used
> `container → unix socket → Android-side decode-daemon → MediaCodec`,
> which required flashing a KSU module and keeping a daemon resident.
> That entire chain is gone: no daemon, no watchdog, no socket, no SHM,
> no MediaCodec calls. Upgrading means replacing the `.so` in the container
> and uninstalling the `dmd_watchdog` module.
>
> See [`CHANGELOG.md`](CHANGELOG.md) and
> [`doc/release-archive.md`](doc/release-archive.md) for the historical architecture.

## Supported codecs

| Codec | Status | Verified on device |
|---|---|---|
| H.264 | ✅ Working | 300/300 frames, md5 byte-identical to software |
| HEVC Main | ✅ Working | 12/12 frames, md5 byte-identical to software |
| VP9 Profile 0 | ✅ Working | 50/50 frames, md5 byte-identical to software |
| VP8 | ✅ Working | 90/90 frames, md5 byte-identical to software (new in 0.4.2) |
| AV1 Profile 0 | 🚧 Incomplete | Frame count matches dav1d, **pixels do not**; not advertised by default |
| MPEG-2 | 🚧 Incomplete | Synthesis is byte-identical to the original stream, but firmware raises `SYS_ERROR`; not advertised by default |
| HEVC Main10 / VP9 Profile2 | ❌ Firmware limit | Firmware recognises 10-bit but keeps reporting `INSUFFICIENT` and emits no frames |

0.4.4 re-ran the four 1080p regressions with unchanged results (H.264 150 frames,
HEVC 90, VP9 90, VP8 90); the non-1080p suite also passes in full — HEVC at
1280x720, 854x480, 640x360 and 720x1280 (portrait) plus a 1080p→720p→480p
switching stream, 5 passed and 0 failed. H.264 and VP9 at 720p and 360p were
verified byte for byte in 0.4.3. The criterion is always md5 byte-identical to
software decode, not merely "it decodes".

**0.4.2 and earlier show a green screen on anything that is not 1080p** — see
the warning under Build and install. **0.4.3 and earlier stall for seconds when
you seek or the player switches quality** in a browser; fixed in 0.4.4, see
Browser setup.

AV1 is only advertised when built with `-DDMD_ENABLE_AV1`; see
[`doc/av1-v4l2-status.md`](doc/av1-v4l2-status.md) for the outstanding defects.
MPEG-2 requires `-DDMD_ENABLE_MPEG2`; for the 10-bit probe switches see
v0.4.2 in `CHANGELOG.md`, for the root cause of the non-1080p green screen
see v0.4.3, and for the seek / quality-switch stall see v0.4.4.

> One class of HEVC stream cannot be supported: those whose SPS carries
> `st_ref_pic_set` (`num_short_term_ref_pic_sets > 0`). VA-API exposes only the
> count, not the contents, so it cannot be reconstructed. In that case
> `vaEndPicture` returns `UNIMPLEMENTED` so the caller falls back to software.
> x265 emits 0 by default, so common streams are unaffected.

## Build and install

Must be built on **aarch64** (x86_64 cross toolchains lack aarch64 glibc):

```sh
# Dependencies: build-essential pkg-config libva-dev libdrm-dev
cd vaapi-driver
make            # produces build/msm_drm_drv_video.so
make check      # confirms the __vaDriverInit_* entry point is exported
make tests      # unit tests
```

Multi-resolution regression (**needs real hardware** — it exercises actual
hardware decode):

```sh
cd vaapi-driver/tests
FFMPEG=/path/to/ffmpeg DRIVER_DIR=../build ./regress_resolutions.sh test.hevc
```

`FFMPEG` points at an ffmpeg with libx265 and vaapi support, `DRIVER_DIR` at the
directory holding `msm_drm_drv_video.so` (the script uses it as
`LIBVA_DRIVERS_PATH`). Both have defaults (`ffmpeg` / `../build`); the source
stream argument defaults to `test.hevc` and the script exits 77 if it is missing.
It transcodes the source into 1280x720, 854x480 (width not a multiple of 128),
640x360 and 720x1280 (portrait), then concatenates a 1080p→720p→480p switching
stream. The criterion is **hardware and software md5 being byte-identical**, not
merely "it decodes".

> This regression covers the blind spot of 0.4.2 and earlier: all four existing
> regression streams were 1920x1080, which happens to equal msm_vidc's default
> CAPTURE geometry at device open, so the "override the CAPTURE leftovers with
> the negotiated OUTPUT values" branch was never taken — 1080p passed while
> browsers showed a green screen at 720p.

Install into the container (**this is the whole procedure**):

```sh
install -m 0644 build/msm_drm_drv_video.so \
  /usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so
```

> The output filename is fixed: libva derives the driver name `msm_drm` from the
> msm render node and will only try `<dridir>/msm_drm_drv_video.so`, with no fallback.

Prerequisites:

- `libva2` and `libva-drm2` installed in the container
- The container user must be in the `droidspaces-gpu` group (the platform usually
  handles this) — `/dev/video32` is `root:droidspaces-gpu`, mode `crw-rw----`
- The platform must pass through `/dev/dri/renderD128`

Smoke test:

```sh
LIBVA_DRIVER_NAME=msm_drm ffmpeg -hwaccel vaapi \
  -hwaccel_output_format vaapi -i in.mp4 -f null -
```

> ⚠️ **On 0.4.1 and earlier this command hangs** (`internal decoding error`
> after 26 frames, ~40s timeout). That was the CAPTURE back-pressure deadlock,
> fixed in 0.4.2. On older builds it will make you misdiagnose the driver as
> broken — use the file-writing form instead:
> `... -i in.mp4 -pix_fmt yuv420p -f rawvideo out.yuv -y`.

> ⚠️ **0.4.2 and earlier show a green screen on anything that is not 1080p**,
> fixed in 0.4.3 — if you are on 0.4.2, that is the reason to upgrade.
> `G_FMT(CAPTURE)` always returns msm_vidc's default geometry at device open,
> `1920x1088`, and does not follow `S_FMT(OUTPUT)`. The driver did override width
> and height with the negotiated OUTPUT values, but the driver does not write
> back `bytesperline` and `sizeimage`, which stayed at their 1080p values, and the
> existing guard only checked the lower bound (`if (d->stride < d->w * bpp2)` —
> for 1280x720, `1920 < 1280` is false). The bad stride survived, causing damage
> twice over: reading a 1280-wide frame at `stride=1920` shifts **every row by
> 640 bytes**, and deriving slice_height from it gives
> `cap_size*2/(1920*3) = 492`, squeezing the real 736 down to 492 so every frame
> is truncated (`frame needs 1416960 bytes > dumb buffer 1413120 bytes`).
> Measured in Firefox, 351 of 356 exports carried the bad geometry.
> 0.4.3 adds an **upper**-bound check on stride (Venus aligns CAPTURE stride to
> 128, so a legal value must fall in `[align(w,128), align(w,128)+128)`) and
> recomputes `cap_size` from the corrected geometry. The same release fixes a
> solid-green first frame: in NV12, `UV=0` is not colourless but maximum chroma
> offset, converting through limited-range BT.601 to `R≈0, G≈135, B≈0` (the
> neutral value is 128). Firefox calls `vaExportSurfaceHandle` to build a texture
> **before** decoding starts, so it saw the all-zero block left by the surface
> allocation memset. Filling Y with 0 and UV with `0x80` took 3 solid-green
> exports out of 230 down to 0 out of 247.
> **Passing the 1080p smoke test does not mean non-1080p works**: older builds
> are perfectly fine at 1080p.

> ⚠️ Correction (0.4.1): an earlier revision of this file said `vainfo` hangs on
> this platform. That is no longer true — `vainfo` works and reports the driver
> version and profile list. It still only proves the driver loads: the profile
> list is a **static declaration** and does not prove frames come out.

Since 0.4.3 the version string carries a build ID, currently
`DroidSpaces V4L2 VA-API driver 0.4.4+<git short hash>`, with `-dirty` appended
when the working tree has uncommitted changes. This is a debugging tool: browsers
decode in a separate RDD/GPU process and there may be one `.so` in the system
directory and another under `LIBVA_DRIVERS_PATH`, so the suffix tells you which
build was actually `dlopen`ed rather than which one you assumed.

```sh
LIBVA_DRIVER_NAME=msm_drm vainfo 2>&1 | grep 'Driver version'
```

To confirm the hardware is really decoding, check the Venus kernel state
(these values cannot be forged from user space):

```sh
# mvs0_gdsc should read enabled while decoding, disabled when idle
for r in /sys/devices/platform/soc/*gdsc/regulator/regulator.*/; do
  [ "$(cat $r/name)" = mvs0_gdsc ] && echo "mvs0_gdsc=$(cat $r/state)"; done
```

Debug logging: `DMD_VA_LOG=1`.

## Browser setup

Full instructions, flags, profile configuration and verification steps are in
[`doc/browser-vaapi-guide.md`](doc/browser-vaapi-guide.md). Key points:

- **Chrome must run in Wayland mode**: decoded frames are submitted over
  linux-dmabuf; under X11 the decoder is created but sees zero traffic
- **Chrome needs `--render-node-override=/dev/dri/renderD128`**: Chromium only
  enumerates DRM devices on the PCI bus, skipping ARM platform devices
- **Firefox needs `MOZ_DISABLE_RDD_SANDBOX=1`** plus the four VA-API prefs in
  user.js; find the real profile via the Default entry in `installs.ini`
- Firefox is recommended for HEVC playback (Chrome has a platform-level
  presentation-feedback issue on the anland display bridge)
- Quick check: `bash tools/check-browser-vaapi.sh`
- 0.4.3 45-second Firefox soak at 720p: 924 exports, 0 solid-green, 0 truncated,
  0 errors, 3863 frames received for 3863 units fed
- If you hit corrupted output, **do not start from the "CPU cache not flushed to
  the GPU" theory**. It was suspected at length while chasing the green screen in
  0.4.3 and disproved by measurement: re-`mmap`ing the exported dma-buf fd via
  `LD_PRELOAD` in a real decode run yields bytes identical to the
  `vaDeriveImage` CPU path. The paired `DMA_BUF_IOCTL_SYNC` START/END wrapping
  stays (it is what `linux/dma-buf.h` requires) but was not the cause

### Seeking and quality switches: 0.4.3 and earlier stall for seconds

**On 0.4.3 and earlier, dragging the progress bar or letting the player switch
quality stalls playback for seconds, over and over.** Fixed in 0.4.4; if you are
on 0.4.3, that is the reason to upgrade.

The cause is that the resolution-change event was never really handled. The
firmware raises the msm_vidc private event
`PORT_SETTINGS_CHANGED_INSUFFICIENT` (`V4L2_EVENT_MSM_VIDC_START+3`) to demand a
CAPTURE reconfiguration with the new geometry. Older builds did not actually
reconfigure, so the firmware sat in `in_reconfig` and emitted nothing; the layer
above burned a 2 s flush threshold plus a 5 s `SyncSurface` timeout and then
rebuilt the session, looping at **roughly 7 seconds per round** (driver log is
Chinese):

```
PORT_SETTINGS(INSUFFICIENT)
flush 触发: futile=0(recv=0 has_seq=0 pend=5) spent=2000/2000
SyncSurface: 等帧超时 5000 ms
会话已重建（codec=0 864x480）
```

`recv=0` is the fingerprint: units went in, no frame came back.

0.4.4 follows the vendor OMX sequence: `FLUSH_CAPTURE` first and wait for
`FLUSH_DONE` so the firmware hands back every output buffer, then
`STREAMOFF(CAPTURE)` + `REQBUFS(CAPTURE,0)`, release the dma-bufs only after
that, and finally reconfigure to the new geometry and `STREAMON(CAPTURE)`.
**The leading flush cannot be skipped**: without it the firmware still holds all
24 output buffers, and the following `STREAMON(CAPTURE)` always returns `EINVAL`
and cascades into `SYS_ERROR`. Only CAPTURE may be touched — touching OUTPUT
discards the already queued input. The same release fixes `SESSION_CONTINUE`
having been a once-per-session latch: every event sets `in_reconfig` again and
browser seek/ABR retriggers it repeatedly on the same fd, so a single missed
send left the firmware waiting for frames forever. It is now sent per event.
The sequence and its kernel evidence are in
[`doc/verified-platform-facts.md`](doc/verified-platform-facts.md) §12
(Chinese).

Measured in Firefox (H.264/HEVC, including seeks and 856x480 ↔ 1920x1080
switches):

| Metric | 0.4.3 | 0.4.4 |
|---|---|---|
| `SYS_ERROR` | hundreds | **0** |
| 2 s flush spent waiting for nothing | repeatedly | **0** |
| 5 s `SyncSurface` timeout | repeatedly | **0** |
| Session-rebuild loop | continuous | **0** |
| `INSUFFICIENT` | deadlock | 9 events, **9/9 reconfigured** |
| Frame pairing | heavy loss | 2977 frames |

`DestroyContext` in/out counts balance exactly (1080/1080, 850/850, 630/630),
so the pipeline neither dropped frames nor leaked un-returned surfaces after a
reconfiguration.

> The downstream msm_vidc decoder is **not a standard V4L2 stateful decoder**.
> `V4L2_EVENT_SOURCE_CHANGE` has zero occurrences in its three driver files; only
> the private `PORT_SETTINGS_*` events exist, and the reconfiguration sequence
> differs from the kernel specification. That is why upstream FFmpeg
> `v4l2_m2m` and GStreamer `v4l2videodec` cannot drive this device, and why this
> project implements its own driver.

## Power: hardware decode does not save energy

**"Acceleration" refers to throughput only — not to power or CPU savings.**
System-level measurements from the 0.3.x era (host `/proc/stat`, High profile
27.2 Mbps, 300 frames at full speed):

| Metric | Hardware | Software | Delta |
|------|------|------|------|
| System CPU time | 57.70 ms/frame | 57.40 ms/frame | +0.5%, within noise |
| Wall clock | 3557 ms | 2366 ms | hardware **50% slower** |

Whole-device power (screen on, discharging): idle 1208 mA / 4.59 W,
software 1524 mA / 5.74 W, hardware **1630 mA / 6.12 W**.

Reasons: the governor does not downclock for hardware decode, and the load is
spread across CPU plus Venus.

> ⚠️ These numbers were measured on the **0.3.x daemon architecture**. One
> contributor — "the daemon's ~4.4 ms/frame lands on the host's account" — no
> longer exists in 0.4.0 (there is no daemon process).
> **Power for the V4L2 path has not been re-measured.** The qualitative
> conclusion (single-stream hardware decode does not save power; the value is in
> concurrency and freeing the CPU) is expected to hold, but treat the figures as
> pending re-measurement.

## Performance

> ⚠️ **All 0.3.x performance figures are void.** They measured the
> daemon + socket + MediaCodec chain end to end (1080p peak 194 fps and so on),
> which has no bearing on the current architecture.
> **Throughput and latency for the V4L2 path have not been measured**, and there
> is no A/B comparison against the old architecture.
>
> Historical data is in
> [`doc/performance-and-roadmap.md`](doc/performance-and-roadmap.md); note that it
> describes an architecture that has been deleted.

## Documentation

| Document | Contents |
|---|---|
| [`vaapi-driver/README.md`](vaapi-driver/README.md) | Driver internals, V4L2 negotiation details |
| [`doc/av1-v4l2-status.md`](doc/av1-v4l2-status.md) | Full survey of the AV1 defects and method lessons |
| [`doc/platform-integration-contract.en.md`](doc/platform-integration-contract.en.md) | What the platform must provide (0.4.0: device-node permissions only) |
| [`doc/browser-vaapi-guide.md`](doc/browser-vaapi-guide.md) | Browser setup |
| [`doc/verified-platform-facts.md`](doc/verified-platform-facts.md) | Platform facts established by testing |
| [`doc/why-not-v4l2.md`](doc/why-not-v4l2.md) | ⚠️ Historical document whose conclusion has been overturned; kept for its investigation and method lessons |
| [`CHANGELOG.md`](CHANGELOG.md) | Version history |

## Test device

Xiaomi Pad 5 (`nabu`), Snapdragon 855 (SM8150 / Adreno 640), Android 13,
kernel `4.14.336`; container is Debian 13 aarch64 inside DroidSpaces.

AV1 requires newer hardware and is verified on a separate device.

## License

See [LICENSE](LICENSE).
