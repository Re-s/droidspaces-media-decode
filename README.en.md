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
| AV1 Profile 0 | 🚧 Incomplete | Frame count matches dav1d, **pixels do not**; not advertised by default |
| VP8 | ❌ Unsupported | msm_vidc's V4L2 layer has no VP80 format — no hardware path exists |

AV1 is only advertised when built with `-DDMD_ENABLE_AV1`; see
[`doc/av1-v4l2-status.md`](doc/av1-v4l2-status.md) for the outstanding defects.

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

> ⚠️ **Do not use `vainfo` to test this.** It hangs on this platform even when
> pointed at a nonexistent driver name, so a hang tells you nothing about the
> driver. Use the ffmpeg command above.

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
