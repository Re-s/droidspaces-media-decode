# Optional linear NV12 GBM adapter

This loader-scoped library supplies CPU-written linear NV12 storage when the
system Mesa GBM backend cannot allocate NV12. It forwards every public function
from `gbm.h` to the real library and preserves real GBM device/BO pointers. RGB
allocations and successful native NV12 operations retain their original behavior.
It does not change the VA-API driver's codec capabilities or any system Mesa file.

The fallback allocates a genuine linear XRGB8888 BO as raw storage. The width is
`ceil((2 * ceil(NV12_width / 2)) / 4)` XRGB pixels; the height is
`NV12_height + ceil(NV12_height / 2)`. Its real stride is used for both NV12 planes,
with UV beginning at `stride * NV12_height`. Both exported FDs and GEM handles
refer to that same allocation. The adapter verifies the real modifier, plane
count, stride, offset and exported dma-buf capacity before exposing the NV12 view.
Odd GBM dimensions are valid, including Chromium's 1x1 modifier probe; an encoder
may impose its own even-dimension requirement.

Fallback creation requires `GBM_BO_USE_LINEAR`, `GBM_BO_USE_SCANOUT`, or an
explicit LINEAR modifier. SCANOUT-only requests also select and report a LINEAR
modifier, so the support query and creation result agree. WRITE may accompany
either usage. Rendering, protected, cursor, front-rendering, compression and
unknown flags are rejected. Actual presentation still requires the consuming
compositor/KMS plane to support linear NV12; the tested KWin installation
advertises NV12 LINEAR and INVALID. EGL/Vulkan rendering into the fallback NV12
allocation is not supported. RGB rendering uses the original GBM backend.

For import, native-capable backends and usages outside this compatibility path
are delegated unchanged. When the real backend reports NV12+LINEAR unsupported
and the caller explicitly requests CPU usage through LINEAR or WRITE, the adapter
directly uses a validated XRGB view for linear NV12 dma-buf imports.
On the tested Mesa build, native NV12 import could return a BO despite that
unsupported format query, then crash inside `dri2_map_image`; non-null import
alone therefore does not establish a usable native CPU map.

The compatibility path only accepts `GBM_BO_IMPORT_FD_MODIFIER`, LINEAR NV12,
two descriptors of the same
object (matching `fstat` device/inode), equal positive strides, zero Y offset,
`UV_offset = stride * height`, and sufficient fd capacity. It imports that storage
through the real library as a single-plane XRGB BO and verifies the resulting
layout before attaching the NV12 view. The caller's seekable fd position is
preserved during capacity validation. Separate-object linear NV12 imports are
rejected by this compatibility path; non-linear, rendering and protected imports
retain the original backend's behavior.

SCANOUT-only graphical imports always use the real backend and retain their
original NV12 plane offsets. Decoder buffers may include vertical padding between
Y and UV (for example, a 640x360 view with UV beginning after 384 pitched rows);
that is valid and must not be constrained to the CPU staging buffer's canonical
layout. The adapter does not CPU-map those native graphical imports.

CPU maps cover the entire backing BO, including UV rows. The returned pointer
uses NV12 byte coordinates, while unmap receives the real backend's map token.
User-data callbacks run without the tracking mutex and can still query the NV12
metadata. A caller must obey normal GBM object lifetime rules and unmap before
destroying a BO.

## Build and select for one Chrome process

Build with the target system's `gbm.h` and native compiler:

```sh
make REAL_GBM=/usr/lib/aarch64-linux-gnu/libgbm.so.1.0.0
```

If the target's headers have been staged outside the normal include directory,
add `CPPFLAGS=-I/path/to/staged-headers`. The library and probe both include the
public `gbm.h`; no Mesa private structure definitions are used.

The result is `build/libgbm.so.1`, with that SONAME and no dependency on `-lgbm`.
The absolute real-library path is compiled in; `DMD_GBM_REAL_LIBRARY` can override
it for testing. The real library is loaded with `RTLD_NOW | RTLD_LOCAL`, and each
entrypoint is resolved through `dlsym` on that explicit handle. Keep normal ELF
allocator interposition. Do not add `RTLD_DEEPBIND`: it caused immediate GPU
process SIGSEGVs in the tested Chrome build, before NV12 allocation; removing it
restored startup and hardware encoding.

Place the result in a dedicated optional directory and prepend only that
directory to Chrome's `LD_LIBRARY_PATH`. Do not replace `/usr/lib/.../libgbm.so.1`
or add this directory globally through `ld.so.conf`. For example, after staging
the library in `/usr/local/lib/droidspaces-gbm`:

```sh
LD_LIBRARY_PATH=/usr/local/lib/droidspaces-gbm${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
  chromium --ozone-platform=wayland --use-gl=angle --use-angle=vulkan \
  --gmb-readback-from-texture-disabled-for-debugging
```

Keep the machine's existing VA-API and hardware-encoding feature options. The
readback switch avoids an upstream SHMEM mappable SharedImage being passed
directly to a Linux VEA that expects dma-buf input; CPU input then uses VEA's
native NV12 staging allocation. This adapter does not add a software fallback
when a valid dma-buf or hardware encoder is unavailable.

## Chromium's linear allocation path

In Chromium 154.0.8037.97, the Linux VEA adapter's CPU-input staging pool requests
`VEA_READ_CAMERA_AND_CPU_READ_WRITE`. Its usage mapping includes LINEAR. Desktop
builds without MINIGBM define the extra CAMERA/TEXTURING/ENCODER/SW_READ flags as
zero, leaving a linear allocation request. The Wayland allocator explicitly
selects the LINEAR modifier for this usage. The adapter therefore needs CPU map,
NV12 metadata and dma-buf export/import; it does not require GPU rendering into
the NV12 staging buffer. See the upstream [VEA staging pool](https://github.com/chromium/chromium/blob/154.0.8037.97/media/video/video_encode_accelerator_adapter.cc#L269),
[usage mapping](https://github.com/chromium/chromium/blob/154.0.8037.97/ui/gfx/linux/gbm_util.cc#L38),
[desktop flags](https://github.com/chromium/chromium/blob/154.0.8037.97/ui/gfx/linux/gbm_defines.h#L15),
and [Wayland modifier selection](https://github.com/chromium/chromium/blob/154.0.8037.97/ui/ozone/platform/wayland/gpu/gbm_pixmap_wayland.cc#L68).

Chromium still queries NV12+SCANOUT as a preliminary desktop format check, then
creates and reimports a 1x1 NV12 BO to validate explicit modifiers before creating
the actual frame. The adapter supports that valid odd-sized linear layout and
same-object import. See [format support](https://github.com/chromium/chromium/blob/154.0.8037.97/ui/gfx/linux/gbm_wrapper.cc#L490)
and [modifier verification](https://github.com/chromium/chromium/blob/154.0.8037.97/ui/gfx/linux/gbm_wrapper.cc#L343).

## Repeatable verification

`make check` builds and runs a CPU-only mock backend test without a DRM device.
It checks exported storage, map coordinates, modifier probes/imports, rejected
layouts and flags, callback reentry, 400 concurrent BO lifetimes, and unchanged
RGB/native operations. It also runs the hardware probe against a CPU-only mock
to check the probe itself. These tests do not access a render node.

Build and run the same probe against the target device:

```sh
make probe
DMD_GBM_REAL_LIBRARY=/usr/lib/aarch64-linux-gnu/libgbm.so.1.0.0 \
  ./build/probe_gbm_compat /dev/dri/renderD128 \
  /usr/local/lib/droidspaces-gbm/libgbm.so.1 640 480
```

The optional width/height default to 640x480. The probe prints support queries,
actual NV12 dimensions, modifier, stride, UV offset and exported fd capacity.
It writes Y and the final UV pair through GBM, then reads those bytes from the
exported fd mapping. It tests LINEAR creation, SCANOUT creation selecting LINEAR,
the 1x1 modifier/export/import round trip, modifiers2, and native RGB metadata.
It exits nonzero on any failed check. To record the original backend's baseline,
pass its absolute system-library path as the library argument; NV12 checks are
expected to fail on the affected backend while RGB still succeeds. No EGL,
compositor, VA-API or encoded-video claim is implied by this allocation probe.

The probe brackets exported-fd CPU reads with DMA_BUF_IOCTL_SYNC. ENOTTY/EINVAL
is reported as an unsupported sync ioctl (also used by the mock memfd); other
sync errors fail the check. Allocation and import alone do not test actual KMS
presentation, EGL sampling or hardware encoder consumption.

Device validation on SM8750P/Adreno 830 with Chrome 154.0.8037.97, Wayland and
ANGLE Vulkan preserved the original UI backend. CPU I420 H.264 input and Canvas
H.264 Baseline/Main/High each produced 60 frames; software decoding confirmed
the expected frame count and profile for all four outputs. This result depends
on the separately tested VA-API/V4L2 encoder and its NV12 PRIME import support.
New Chrome/GBM versions or a different compositor still need both this probe
and an actual encode followed by software decoding.

Set `DMD_GBM_DEBUG=1` for opt-in import diagnostics: type, usage flags, format,
dimensions, modifiers, plane FDs/pitches/offsets, validation failure details and
the selected native/XRGB path. It is off by default and preserves errno. This is
useful when separating decoder graphical imports from encoder CPU staging.
