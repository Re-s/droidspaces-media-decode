# VA-API Proxy Driver (container side)

> 🌐 **中文版（原文）：[README.md](README.md)** — the Chinese version is authoritative; if the two disagree, follow it.

Exposes the MediaCodec hardware decoding capability of the Android host to the
consumers inside a DroidSpaces container (`vainfo` / ffmpeg / Firefox / Chrome) through
standard VA-API, **without requiring any environment variable**.

Artifact: `msm_drm_drv_video.so`, installed into `/usr/lib/aarch64-linux-gnu/dri/`.

## Current capability boundary

**Implemented: capability query + H.264 / HEVC / VP9 / VP8 decode data path.** `vainfo`
lists the profiles and config attributes; all four have been taken end to end on real
hardware and produce a picture, and the hardware-decoded output is **byte-for-byte
identical** to software decode.

Actual status per codec:

| Profile | daemon codec | Capability query | Decodes to picture | Verification result |
|---------|-------------|---------|-----------|---------|
| `VAProfileH264ConstrainedBaseline` / `Main` / `High` | 0 (H.264) | OK | **Pass** | 1080p 150 frames + 4K 90 frames + long stream 3000 frames, `cmp` byte-for-byte identical to software decode |
| `VAProfileHEVCMain` | 1 (HEVC) | OK | **Pass** | 1080p / 4K / 720p / no reference frames / long stream 1500 frames, `cmp` byte-for-byte identical to software decode |
| `VAProfileVP9Profile0` | 2 (VP9) | OK | **Pass** | 720p 120 frames + 1080p 60 frames, `cmp` byte-for-byte identical to software decode |
| `VAProfileVP8Version0_3` | 3 (VP8) | OK | **Pass** | 720p 120 frames, `cmp` byte-for-byte identical to software decode |

There is one class of HEVC bitstream we do not support: when the SPS has
`num_short_term_ref_pic_sets > 0` the parameter sets cannot be rebuilt (VA-API only
gives the count, not the content), `dmd_hevc_can_build` returns 0, and the driver
returns `VA_STATUS_ERROR_UNIMPLEMENTED` so the layer above falls back to software
decode cleanly. x265 does not produce this kind of bitstream by default.

> **History**: the HEVC profile declaration was withdrawn for a while — back then
> bitstream forwarding was not implemented yet, and declaring it made things worse
> (a consumer would select us and then fail, when it could have gone straight to
> software decode). Before the withdrawal the Firefox probe read HEVC as
> "hardware-decodable" (the bitmap contained `1<<8`).
> The implementation is now complete and verified byte-for-byte, so the declaration is
> **truthful**. But that principle still holds: **never declare a capability that has
> not been verified end to end.**

The difficulty differences between the codecs come from exactly one source —
**how complete the bitstream VA-API passes to the driver is**:

- VP9: `VASliceDataBufferType` already contains **one whole VP9 frame** (including the
  uncompressed header, starting at the 2 bits of `frame_marker`). Zero header
  reconstruction, forwarded as is
- VP8: slice data starts at partition 0 and is only missing the 3-byte frame tag from
  RFC 6386 §9.1 (plus 7 more bytes for a key frame). `first_part_size` can be derived
  exactly from `partition_size[0] + (macroblock_offset+7)/8`
- H.264: slice data is a **complete raw NALU** (including the NAL header and the slice
  header), only the 4-byte start code is missing. But the parameter sets **do not exist
  in bitstream form at all** — after ffmpeg parses SPS/PPS into fields the original
  bitstream is gone, so they must be synthesized in reverse from
  `VAPictureParameterBufferH264`. On top of that, frame-reorder pairing has to be
  handled (see below)
- HEVC: likewise complete NALUs missing only the start code, but there are three
  parameter sets (VPS/SPS/PPS), fewer usable fields in `profile_tier_level`, and
  `conf_win_*` does not exist at all in `va_dec_hevc.h` while most clients fill
  `slice_data_num_emu_prevn_bytes` with 0

### Two H.264 pitfalls (both corrupt the picture instead of raising an error)

**1. Frame-reorder pairing: surfaces must not be paired in submission order.**

> Note: what follows describes the reordering that is mandatory under **display-order
> output** (the decoder's default behaviour).
> The current implementation **no longer depends on output order** — the daemon delivers
> the input unit sequence number belonging to each frame (`CAP_FRAME_PTS`) and the driver
> pairs exactly by that number, see the section "Pairing: exact match by input unit
> sequence number" below. This POC-based reordering logic is only used as a fallback path
> when the daemon does not support that capability bit.
>
> An early version declared the output order with the compile-time constant
> `DMD_DECODE_ORDER_OUTPUT`; when the two sides disagreed the picture was misaligned with
> no error reported (measured 105/150 frames). That constant has **been deleted**.

ffmpeg calls `vaEndPicture` in **decode order**, while MediaCodec emits frames in
**display order**. On this test stream the measured decode order is `I P B B B`
(P is in second position because the B frames reference it) and the display order is
`I B B B P`. FIFO pairing misaligns everything from the second frame onward.
So pairing goes by `CurrPic.TopFieldOrderCnt` (POC): the daemon's k-th frame goes to the
surface with the k-th smallest POC. ffmpeg fills in the already unpacked `field_poc[0]`,
not the 6-bit `poc_lsb` from the bitstream that wraps around, so the values can be
compared directly.

But POC is only monotonic **within one coded video sequence**; every IDR resets it
(measured: at the second IDR it jumped from 65562 back to 65536). So a sequence number
has to be carried as well, and sorting compares seq first and POC second, otherwise
frames of a new sequence cut ahead of unpaired frames of the old one.
**The criterion for a new sequence number must be `frame_num` going back to zero**
(spec 7.4.3), not "POC is smaller than the previous frame" — submission is in decode
order, and POC naturally rises and falls within a GOP.

**2. The PPS `num_ref_idx_l0/l1_default_active_minus1` must copy the effective value of
the current frame verbatim.**
What VA-API gives is "the effective value for this slice": for a slice without
`num_ref_idx_active_override_flag`, the effective value equals the PPS default; for a
slice with the override, the PPS default has no effect. So copying verbatim is always
correct, and there is no need to distinguish `override_flag` (VA-API does not expose it).
Resend the PPS when the value changes — MediaCodec accepts parameter sets that reappear
repeatedly in the stream.

Two wrong turns have both been disproven on device: taking the slice param of the
**first IDR** (an I slice has no such syntax element, it is always 0, so a non-override
B slice ends up using only 1 reference frame); and using `num_ref_frames-1` as a "safe
upper bound" (**l0 may be too large, l1 must not be off by even one** —
`l1_default_active` determines the **entropy-decoding code length** of `ref_idx_l1` in a
non-override B slice, so changing it is a misalignment at the syntax parsing layer, not a
prediction quality issue).

### The end of the stream needs an explicit flush

MediaCodec lags 2-3 units in steady state (measured: after feeding 1/2/3 VCL units no
frame comes out even after waiting 4000 ms; the 4th produces one). The last few frames of
a stream are held inside the decoder, and at that moment ffmpeg is blocked in
`vaSyncSurface` and will not send more data — both sides wait for each other. So when the
wait exceeds half of the total timeout, we do an explicit `shutdown(SHUT_WR)`, once per
session (it is irreversible).

**Therefore seek requires rebuilding the session**: once the write end is closed, this
session can no longer send data. The daemon has no in-connection reset either, so seek
can only be implemented by having the driver rebuild the session (not implemented yet —
no consumer currently seeks on the same context).

### Mid-stream resolution change: DestroyContext must drain first

On a mid-stream resolution change ffmpeg calls `vaDestroyContext` first and then creates
a new one, but **it will still `vaSyncSurface` surfaces of the old context** — those
frames belong to the previous resolution segment and it still wants to collect them.
If `DestroyContext` simply marks surfaces still in PENDING as failed, ffmpeg receives
`VA_STATUS_ERROR_OPERATION_FAILED` and the whole stream cannot be decoded further
(measured: `switch.h264` fed 62 units here but only got 44 frames back, 18 surfaces were
abandoned).

Those frames were not decoded wrongly, they are just still held inside MediaCodec — the
same cause as at the end of a stream. So `DestroyContext` runs one flush + frame-taking
loop before abandoning surfaces (sharing `dmd_pending_take_locked()` with
`vaSyncSurface`). Frame data is `memcpy`ed into the surface's own buffer, so destroying
the session afterwards does not affect later reads.

`switch.h264` (720p→480p) and `grow.h264` (480p→720p) are now both byte-for-byte
identical to software decode.

**No high bit depth is declared** (HEVC Main10, VP9 Profile2, H.264 High10): the hardware
may support it, but it has not been verified. Lying about capabilities makes a consumer
select us and then fail, which is worse than not reporting at all.

### Output path: the CPU path (VAImage) + dmabuf export (not zero-copy)

Surface data lives in ordinary heap memory, and read-back mainly goes through
`vaDeriveImage` / `vaCreateImage`.

**The zero-copy road is a dead end** — a hard limit of the container environment: ION is
completely unavailable (legacy `EINVAL` / modern `ENODEV`), `/dev/dma_heap` does not
exist (kernel 4.14), and the public NDK API of MediaCodec cannot give us the dmabuf fd of
an output buffer either.

But `vaExportSurfaceHandle` **is implemented** (`src/export.c`, 159 lines, registered at
`vtable.inc:60`): it copies the frame content from the heap into a newly allocated dmabuf
and exports the fd. **That is "can export", not "zero-copy"** — there is one extra CPU
copy.

The reason for implementing it is that **Firefox hardware decode must go through this
entry point** (it does not accept the pure VAImage path). ffmpeg uses `vaDeriveImage` and
never touches this path — so when only testing with ffmpeg, problems in the export path
are completely invisible and need separate verification with a browser.

Read-back goes through two paths, `vaDeriveImage` and `vaCreateImage` + `vaGetImage`, and
**both must be implemented**: ffmpeg uses derive while probing, but `hwdownload` is a read
access (`MAP_READ`), so `!(flags & MAP_READ)` in the condition of `vaapi_map_frame` does
not hold and the actual data fetch still goes through `vaCreateImage` + `vaGetImage`
(`hwcontext_vaapi.c:900`/`:910`).

### Three usage notes

**`-hwaccel_output_format vaapi` must be given explicitly.** With only `-hwaccel vaapi`,
ffmpeg automatically downloads frames into a software format, the filter chain receives
nv12 instead of vaapi frames, and `hwdownload` reports "Impossible to convert between the
formats". The correct acceptance command:

```bash
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
       -hwaccel_output_format vaapi \
       -i in.ivf -vf hwdownload,format=nv12 -f rawvideo -y out.yuv
```

**`vaMapBuffer2` must be genuinely implemented, it cannot stay a stub.** libva only falls
back to `vaMapBuffer` when that slot is **NULL**, whereas this driver fills every slot
with a stub function in order to pass `CHECK_VTABLE` — leaving a stub makes ffmpeg's
read-back path (`hwcontext_vaapi.c:928`) receive UNIMPLEMENTED and fail immediately,
never reaching the compatibility branch. This is a real conflict between "fill the whole
vtable" and "libva decides capability by NULL"; watch out for the same kind of trap when
adding new slots.

**`VAImage.offsets[1]` uses the buffer height, not the display height.** The decode
output for 1080p is a 1920x1088 buffer with a 1920x1080 display size.
`VAImage.width/height` must report the display size (otherwise ffmpeg's size check
fails), but `offsets[1] = stride * slice_height` must use **1088**. Computing it with 1080
shifts the chroma plane by `stride*8` bytes; the symptoms are green edges, garbled
pictures and chroma misalignment.

## Consumer contract differences: one driver, three usage patterns

The three consumers expect different things from the driver, and testing only one misses
the pitfalls of the other two. The differences verified on device:

| Behaviour | ffmpeg | Firefox | Chrome |
|---|---|---|---|
| Passes `RTFormat` when creating a config | yes | yes | **no** (expects the driver to declare it on its own) |
| Per-frame `vaSyncSurface` | calls it | calls it | **never calls it** |
| How frames are taken | `vaDeriveImage` | `vaExportSurfaceHandle` | `vaExportSurfaceHandle` (takes them after a large batch of submissions) |
| dmabuf layout | not used | `SEPARATE_LAYERS` | `SEPARATE_LAYERS` |

Two Chrome-specific requirements each caused one "hardware decode completely unusable"
situation:

1. **`vaQueryConfigAttributes` must declare the driver's capability, it must not echo the
   input arguments back.** Chrome creates a config without attributes and then queries
   what the driver supports. In echo mode it receives 0 attributes, cannot read
   `RTFormat`, and `FillProfileInfo_Locked` declares all six profiles unusable.
2. **Frame collection must not happen only inside `vaSyncSurface`.** Chrome never calls
   it, so the pending decode queue fills up → `vaEndPicture` reports an error → Chrome
   gives up on hardware decode. Now, when the queue is full, one frame is collected
   inside `EndPicture` to free a slot.

There is one more hard contract (`FillProfileInfo_Locked` in Chromium's
`vaapi_wrapper.cc`): **`vaQuerySurfaceAttributes` must return `num_attribs > 0` and
include `VASurfaceAttribMaxWidth` / `MaxHeight`**, otherwise the whole profile is
rejected. We do report them in `profiles.c` (`MinWidth`/`MinHeight` are optional,
Chromium falls back to 16 itself).

> ⚠️ This table was obtained by **measurement** (every row corresponds to one verification
> on real hardware), it was not inferred by reading the Chromium source. The related
> source-level investigation is in
> `dmd-vaapi/research/A-1-chrome-vaapi-contract.md`, but that report is based on the
> Chromium source available locally (around 120+), **not** the 151 actually used for
> testing; the feature flag states and newly added contracts in it are unverified.
>
> This project has learned this lesson once: a source-level conclusion ("the RDD sandbox
> returns `EACCES` for `SYS_SOCKET` unconditionally") was overturned by measurement,
> because that code path did not execute as expected in the real environment.
> **A source-level conclusion only holds once it is confirmed by a single-variable
> experiment.**

## Browser (Firefox): the capability is fine, the barrier is the environment

`vainfo` / ffmpeg being able to hardware decode **does not mean** Firefox will hardware
decode too. Firefox's decision on "can this be hardware decoded" passes three gates, and
getting stuck at any one of them only leads to a silent fallback to software decode, with
a single line in the log, `Codec h264 is not accelerated`, that reveals nothing about the
cause.

Firefox's own probe does accept this driver — on startup it runs
`/usr/lib/firefox-esr/vaapitest` (`widget/gtk/GfxInfo.cpp`), and `strace` shows it
dlopens our `.so`:

```
$ /usr/lib/firefox-esr/vaapitest --drm /dev/dri/renderD128
VAAPI_SUPPORTED
TRUE
VAAPI_HWCODECS
112          # = 0b1110000 = H264(1<<4) | VP8(1<<5) | VP9(1<<6)
```

### The three gates (`dom/media/platforms/ffmpeg/FFmpegVideoDecoder.cpp`)

| Gate | Condition | Log when stuck |
|---|---|---|
| 1 | `gfxVars::UseH264HwDecode()` | `Codec h264 is not accelerated` |
| 2 | Hardware WebRender (not software compositing) | `Hardware WebRender is off, VAAPI is disabled` |
| 3 | Decoding happens in the RDD process | `VA-API works in RDD process only` |

Gate 1 has the longest source chain and is the easiest to trip over:

```
FEATURE_H264_HW_DECODE requires mVAAPISupportedCodecs & CODEC_HW_H264
  ← filled in by GetDataVAAPI() running vaapitest
    ← which only runs when probeHWDecode is true:
      probeHWDecode = mIsAccelerated && (status OK || force-enabled pref)
                      ^^^^^^^^^^^^^^
      mIsAccelerated = !mesaAccelerated.Equals("FALSE")   ← comes from glxtest
```

**`media.hardware-video-decoding.force-enabled` cannot bypass `mIsAccelerated`** — it
only takes effect inside the parentheses. No hardware GL means no hardware decode; that
is Firefox's design.

### The two required environment variables inside the container

```bash
MESA_LOADER_DRIVER_OVERRIDE=msm   # otherwise Mesa goes through zink → picks no Vulkan device
                                  # → falls back to llvmpipe → MESA_ACCELERATED FALSE
MOZ_DISABLE_RDD_SANDBOX=1         # otherwise the RDD sandbox forbids this driver from
                                  # connecting to the daemon's TCP
                                  # (driver log: 创建 TCP socket 失败: Permission denied)
```

With the override in place, glxtest reports `RENDERER FD640` (a real Adreno 640 /
freedreno) instead of llvmpipe.

⚠️ **`MOZ_DISABLE_RDD_SANDBOX=1` is a security trade-off**: it weakens the isolation of
the RDD process. The cleaner long-term approach is for the driver to switch to a Unix
socket, or for the sandbox policy to allow this one connection. Use it only when the local
environment is trusted.

One more pref is needed in the profile:

```
media.hardware-video-decoding.force-enabled = true
```

> ⚠️ This used to say that `media.ffmpeg.vaapi.enabled = true` was also needed.
> **That pref was removed around Firefox 137**; the name does not exist at all in the
> libxul of 140 ESR (`strings libxul.so | grep -x` finds nothing), so setting it has no
> effect.
>
> Single-variable experiment: after deleting it from the profile, hardware decode kept
> working (872 frames exported, zero software decode fallback). What actually matters is
> the `force-enabled` above.
>
> The hardware decode switches now all live under `media.hardware-video-decoding.*`.

`tools/firefox-hwdec` bakes the environment above in and comes with a self-check:

```bash
firefox-hwdec --dmd-check    # only checks the environment (DRM device / daemon / driver / probe bitmap)
firefox-hwdec                # starts Firefox with the correct environment
DMD_VA_LOG=1 firefox-hwdec   # show the driver-side log to confirm hardware decode really kicks in
```

Xvfb does not work: it does not provide DRI3, GLX cannot reach freedreno, and
`MESA_ACCELERATED` is always FALSE. A real Wayland/X session is required.

Measured results (real kwin_wayland session, 1080p H.264, `<video>` playback):

| Metric | Value |
|---|---|
| VA-API hardware-decoded frames | 143 (147 frames in the video) |
| Software-decoded frames | **0** |
| `vaExportSurfaceHandle` | 148 calls, all successful ⚠️ see the note below |
| Average decode time | 7.4 ms (frame interval 33.3 ms) |
| Driver-side / browser-side errors | none |
| Actual frame rate | **28.9 fps** (the video is nominally 30fps) — wall clock: exported frame count ÷ the timestamp difference between the first and last frame log |
| Picture correctness | **0 black frames** (judged by mean luma), byte-for-byte identical to the software decode baseline |
| Drain / session rebuild | **0 each** |

> ⚠️ **The three counts in this table are inconsistent with one another; the original log is no longer
> recoverable, so no explanation is offered — it is only annotated as it stands.**
> The number of exports (148) is greater than both the hardware-decoded frame count (143) and the total
> frame count of the video (147).
> There are several possible reasons (the same surface being exported more than once, exports for
> non-decode purposes, a counting basis that counted a retry twice), but **all of them are speculation,
> with no evidence.**
>
> This inconsistency does not affect the core conclusions of that test round — 0 software-decoded frames,
> 0 black frames, and byte-for-byte identical to the software decode baseline; each of those three was
> established by independent evidence. But the counts in this table should not be quoted as exact values.
> A future re-test should first settle the counting basis: state explicitly whether "number of exports"
> includes exporting the same surface repeatedly.

> Frame rate algorithm: `exported frame count ÷ (last frame timestamp - first frame
> timestamp)`, using the wall-clock timestamps of the driver log, **not** "frame count ÷
> video content duration". The latter counts stuttering playback as normal — see the
> lesson below.

⚠️ Note that "zero errors + full frame rate" **does not equal a correct picture**. All of
these metrics were once green while the picture was broken — error logs, frame counts and
decode times cannot detect a destroyed reference chain. Judging decode correctness
requires inspecting pixels, or a byte-for-byte comparison against the software decode
baseline.

This result has been reproduced independently: `git clone` of this branch inside the
container through a socks5 proxy, `make` + `make install` from the cloned code (zero
warnings), environment set up only by `firefox-hwdec`, with the six-stream regression
still byte-for-byte identical.

⚠️ Always use **wall-clock time** when measuring frame rate. One intermediate version got
as far as "hardware decode all the way through" and assumed it was done, dividing the
decoded frame count by the video content duration to get 30fps — in reality a 4.92-second
video took 23 seconds, so the real frame rate was only 6.4 fps. Neither the number of
`VA-API Got one frame` lines nor the `average decode time` Firefox reports itself can
detect this: a single frame really does take only 7.4 ms, what is slow is the **waiting
between frames**.

### Must run as the user who owns the desktop session

Firefox refuses to use a regular user's session as root; it exits immediately leaving
only the line `Running Firefox as root in a regular user's session is not supported.`
In the container the desktop usually runs under uid 1000 while people habitually work as
root, so this is easy to hit:

```bash
su master -s /bin/bash -c 'firefox-hwdec'
```

`firefox-hwdec` checks this in advance and prints an accurate hint, so nobody has to guess
in front of an empty log.

### `vaExportSurfaceHandle` is a hard requirement of browsers

**This is the root cause of "ffmpeg is all green but the browser does nothing"**, worth a
note of its own.

Firefox takes frames only through dmabuf export: as soon as `CreateImageVAAPI` gets a
decoded frame it calls `vaExportSurfaceHandle` asking for a `DRM_PRIME_2` descriptor, and
on failure it returns `NS_ERROR_DOM_MEDIA_DECODE_ERR`, after which the player does
`ProcessFlush()` and rebuilds as **software decode**. There is no error log anywhere in
the process, and **there is no fallback path to a copy**
(`dom/media/platforms/ffmpeg/FFmpegVideoDecoder.cpp:1632`).
The symptom is "hardware decode produces 1 frame and then software decode forever".

The ffmpeg command line, on the other hand, **does not need** this entry point
(`hwdownload` goes through `vaDeriveImage` + `vaMapBuffer`), which is why all six streams
were green on the command line while the browser did not budge.

There is a counter-intuitive point in the implementation: **zero-copy is not required.**
The three earlier grounds for "zero-copy is unworkable" (ION unavailable in the container,
no `/dev/dma_heap`, the NDK does not hand out the fd of MediaCodec's output buffer) all
say the same thing — we cannot get the fd of MediaCodec's memory. But all Firefox wants is
**a dmabuf fd the compositor can import**; it does not require that memory to be the
decoder's original output. So surfaces were moved into a dumb buffer of msm_drm
(`DRM_IOCTL_MODE_CREATE_DUMB` + `MAP_DUMB`) and frames land directly in this exportable
memory, with the **same** number of copies as the original `calloc` scheme.

Descriptor essentials (`src/export.c`): a single object; Firefox passes
`SEPARATE_LAYERS`, so two layers by default — `DRM_FORMAT_R8` for Y and `DRM_FORMAT_GR88`
for UV, with the UV offset being `stride × slice_height` (**buffer height 1088**, not the
display height 1080, the same pitfall as `VAImage.offsets[1]`). The fd must carry
`CLOEXEC`: the driver runs inside the browser process, and leaking into child processes is
a security problem.

### Observability lesson: do not let unimplemented entry points fail silently

The reason this root cause was hard to find is that the 24 unimplemented stubs returned
`VA_STATUS_ERROR_UNIMPLEMENTED` **silently**. The typical symptom of a consumer hitting an
unimplemented entry point is "quietly falling back to software decode" rather than an
error, and silent stubs make that completely unobservable — which cost two detours (first
a misdiagnosis as a pipeline-depth deadlock, then as a wrong flush threshold).

Now `tools/gen_stubs.py` generates one log line for every stub, so it shows up on the
first run:

```
[dmd-va] 未实现入口被调用: vaExportSurfaceHandle
```

### Mutual wait against the decoder pipeline depth (fixed)

In steady state the browser keeps only **3 frames in flight** (determined by the H.264
reorder depth), while MediaCodec with B frames needs to receive the **4th input unit**
before emitting the first frame (1 without B frames, measured in `tools/probe_lag.c`).
The difference is exactly one frame: the browser wants a frame before it will send the
next unit, and the decoder wants one more unit before it will emit a frame.

The daemon-side `low-latency` cannot bring this down — that is the decoder's inherent
pipeline depth. So **this mutual wait can only be broken by an explicit flush from our
side**, and the only question is how long to wait.

The criterion is "**waiting longer cannot possibly produce a frame**", not "we have waited
long enough":

- when the queue depth is below `DMD_PIPELINE_DEPTH` the queue will not change on its own,
  so waiting is futile → flush immediately
- when the queue is deep enough, wait according to `DMD_FLUSH_AFTER_MS` — at that point
  frames really are on their way, and flushing early would interrupt a healthy session for
  nothing

There are two means of breaking the mutual wait, and **the reversible drain is preferred**:

| | `dmd_session_drain()` | `dmd_session_finish_input()` |
|---|---|---|
| Semantics | send EOS to force frames out, then `flush` to reset and resend CSD | `shutdown(SHUT_WR)` |
| Session | **still usable** | invalidated, must be rebuilt |
| Cost per frame | 33.6 ms (full speed) | 155 ms (about 4.6× slower) |
| Protocol | an in-band request of length 0 | close the write end |

### Resolved: the mutual wait that used to cause flickering black screens

**Neither of these means is triggered any more** (measured 0 times each), because the
mutual wait itself has been eliminated. They are kept in the code only as a fallback for
daemons that do not support decode-order output.

The reason it had to be eliminated rather than optimized: **both draining and rebuilding
destroy the reference chain.** H.264 P/B frames must depend on reference frames, and
restarting decoding from a non-IDR position stays black until the next IDR (one every 30
frames in this test stream, so **a single** drain blacks out at most 29 consecutive frames).
In a measured 60-frame sample 54 frames were pure black (`tools/probe_black.c`, mean luma exactly 16,
i.e. the BT.601 black level), which appears as a flickering picture during playback.
`flush` and rebuilding the session make **no difference** on this point.

> 54 black frames out of 60 exceeds the upper bound of a single drain (29), which means that
> **several drains happened** within that sample interval — each one destroying the reference chain
> again, so the black-frame intervals ran end to end.
> That is exactly where the severity of the problem lies: it is not "an occasional black stretch",
> it is drains happening so often that they almost join into one continuous stretch.

**The real fix: make the decoder emit frames following the input order, so the lag drops
from 4 to 1 and the mutual wait disappears.** `tools/probe_keys.c` measured this key by
key (target lag <= 3):

| Configuration | Lag |
|---|---|
| default / `low-latency=1` / `max-output-reorder-frames=0` / `output-delay=0` | 4+ |
| `vendor.qti-ext-dec-low-latency.enable=1` | 4+ |
| **`vendor.qti-ext-dec-picture-order.enable=1`** | **1** |

Only the last one works.

An early version used the compile-time constant `DMD_DECODE_ORDER_OUTPUT` to tell the
driver "in what order the decoder emits frames", and when the two sides disagreed the
picture was misaligned with no error reported (measured 105/150 frames). **That constant
has been deleted** — the driver now does not need to know the output order at all, see the
next section.

### Pairing: exact match by input unit sequence number (decoupled from output order)

The daemon writes the sequence number of each input unit into the
`presentationTimeUs` of `queueInputBuffer`, MediaCodec carries it unchanged onto the
corresponding output frame, and it comes back through the frame header. So "which
submission does this frame belong to" is a **known fact** and needs no inference:

```
daemon:  vcl_in * 1000  →  presentationTimeUs   (why × 1000, see below)
         4th field of the frame header  ←  presentationTimeUs / 1000
driver:  pending_unit[] matches dmd_frame.unit_seq exactly
```

The capability is declared with the 2nd word of the format description block header (the
previously reserved 0) as `CAP_FRAME_PTS`; an old daemon always has 0, the client then
parses a 3-field frame header, which is backward compatible, and in that case the driver
falls back to `(seq, POC)` inference.

**Why × 1000**: the decoder quantizes PTS in milliseconds, so using the sequence number
directly (a step of 1 us) would squash all of them to 0 — measured, the PTS delivered back
for 9 units were all 0 and pairing degenerated into "one number matching several frames".

Verification is done by **running the same driver against both kinds of daemon**:

| daemon configuration | Six streams | Pairing fallbacks |
|---|---|---|
| display-order output (vendor key removed, simulating a non-Qualcomm platform) | all identical | 0 |
| following input order (with the vendor key) | all identical | 0 |

### Drain trigger conditions (root cause of the black screen)

A drain (EOS + flush) **destroys the decoder's reference chain**, and the P/B frames after
it are all black until the next IDR. So the criterion has to be strict:

```c
int wait_is_futile = dmd_session_frames_received(c->session) > 0 &&
                     !c->daemon_has_unit_seq &&
                     (c->pending_count < DMD_PIPELINE_DEPTH);
```

⚠️ All three conditions are necessary, especially **the first one**:
`daemon_has_unit_seq` is a runtime observation that "is only set once the first frame
arrives", so it is necessarily 0 right after a session is established. Looking only at it
would declare waiting futile at 0 ms and drain immediately — measured: in a browser loop
playback of 708 frames, 135 frames were pure black (1 erroneous drain per round, each
ruining 25~27 frames). The log then reads `等了 0 ms 仍无帧，可逆排空（队列 3）`
("waited 0 ms and still no frame, reversible drain (queue 3)") — while the queue was
plainly full.

When not a single frame has been received, "mutual-wait deadlock" cannot be distinguished
from "the first frame is still on its way", and in that case we should wait until the
`flush_after_ms` threshold. See section eight of
`../dmd-vaapi/research/M-9-black-frames.md` for details.

### Long streams and seek

After pairing was changed to go by unit sequence number, these two verifications were
added, again against both daemon configurations. The material is a stream of 3000 frames /
100 IDRs (`-g 30 -bf 2`, with B frames):

```
ffmpeg -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=100" \
       -c:v libx264 -g 30 -bf 2 -pix_fmt yuv420p long3000.h264
```

| Scenario | Result |
|---|---|
| Long stream 3000 frames (input-order daemon) | byte-for-byte identical; fallback/rebuild/drain all 0; sequence numbers continuous up to 3000 |
| Long stream 3000 frames (display-order daemon) | byte-for-byte identical; fallback/rebuild 0 |
| seek to 10/30/55/80 seconds | all identical; fallback/rebuild 0 |
| seek on resolution-switching streams (`switch.h264`/`grow.h264`) | all identical |

The occasional `SyncSurface: 等帧超时 30 ms` ("frame wait timed out") line in the log is
normal non-blocking polling; the layer above retries afterwards and the result is
unaffected.

⚠️ When maintaining the `pending` queue, `pending_unit` must be moved **together with**
`pending`/`pending_poc`/`pending_seq`. Forgetting to move it misaligns sequence numbers
against surfaces and lets the same number be matched repeatedly (measured: `unit 5` and
`unit 9` each appeared twice, numbers 2 and 6 disappeared, 70/150 frames misaligned),
while everything looks fine in the daemon-side log — very hard to track down.

Another road that was verified workable but not adopted (`tools/probe_replay.c`): keep the
drain, but after rebuilding, replay from the nearest IDR and discard the replayed frames;
black frames and duplicate frames are 0 there as well. It was not adopted because it
requires caching a whole GOP in the driver, replaying up to 29 frames in the worst case,
and it lowers the frame rate — decode-order output has none of these costs.

`tools/probe_cost.c` broke down those 155 ms: creating a session takes only 2.1 ms, the
real cost is the 149.8 ms of `flush → first frame` (the inherent latency of MediaCodec
handling EOS).
(The two add up to 151.9 ms; the difference from the 155 ms measured end to end is the rest of the
fixed per-frame overhead, which was not broken down separately.)
So the problem was never "rebuilding a session is expensive" but "getting
one frame requires going through a whole EOS".

An old daemon does not recognize length 0, in which case `drain` fails and the driver
automatically falls back to the transparent rebuild of `finish_input` + `EndPicture`.

> ⚠️ This used to say "functionally correct, only slow", based on
> `tools/probe_rebuild.c`.
> **That basis does not hold** — that probe only counted frames without looking at the
> picture (the frames were all pure black, Y=16), and the file already annotates itself at
> the top that its conclusion is untrustworthy.
>
> The real cost of a rebuild is that **the reference chain is destroyed**: after resuming
> it stays black until the next IDR. It is not "slow", it is "the picture is broken". This
> is exactly the root cause of the browser's "flickering picture" (measured 135/708 frames
> pure black). The countermeasure in the current implementation is to tighten the trigger
> condition so it almost never happens.

⚠️ Two places that must have a brake, both found by measurement:

**Daemon side: the drain wait must have an upper bound.** The output thread may exit early
because of a decode error, `drain_done` then never catches up with `drain_req`, the input
thread is stuck forever → session leak → after accumulating up to the concurrency limit of
8 it starts rejecting new connections, while `decode-daemon` sits at 203% CPU,
`media.codec` at 180% CPU, and the 8-core load average climbs to 18.7.
The symptoms (high CPU + the browser not producing a single frame) are far from the cause,
and the intermediate "leaking up to the limit" layer completely hides the causal
relationship — during investigation this was nearly attributed to an unstable desktop
session.

**Driver side: the reversible drain must happen "only once per wait".** Unlike
`finish_input` it does not set `input_finished` to block itself — after a drain the queue
depth is unchanged and the condition still holds, so without a brake it is a busy loop
(measured: triggered 1.33 million times, producing only 1 frame).

Two negative examples, both found by measurement:

- waiting out the full 2000 ms patience threshold before flushing → 2 seconds per frame,
  playback is effectively frozen (hardware decode stopped after 7 frames)
- judging only by queue depth and never flushing on a shallow queue → the trailing frames
  at the end of a stream cannot be taken out, `test1080` received 2 frames too few and
  reported `TIMEDOUT 38`. Trailing frames also form a shallow queue, and trying to
  distinguish them from "filling up" by "is the upstream still feeding" does not work
  either — the browser stopping its feed and the stream ending are indistinguishable by
  that metric

After switching to the reversible drain, the number of session rebuilds in the browser
scenario dropped to **0** and the frame rate went from 6.4 fps to 29.8 fps (full speed).

> The 29.8 fps here and the 28.9 fps in the Firefox results table above are measurements from
> **different rounds** — the former is the immediate verification taken when the reversible drain
> rework was finished, the latter is a full regression run later on.
> Both use the wall-clock method and both are in the "full speed" range; the difference comes from a
> different video and a different runtime environment, and is not a contradiction.

## The transparent discovery mechanism (rename it and it stops working)

The file name is not arbitrary, it was derived backwards from what libva computes:

1. libva uses `DRM_IOCTL_VERSION` on `/dev/dri/renderD128` to obtain the kernel driver
   name, measured as `msm_drm`
2. libva's DRM→VA driver name mapping table (`map[]` in `va/drm/va_drm_utils.c`) has
   **no msm entry**, so it takes the fallback: use the kernel driver name as is
3. libva tries **exactly one** file name, with no fallback:
   `/usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so`

So the artifact must be named exactly `msm_drm_drv_video.so` (that is `msm_drm_`, not
`msm_`). Once installed into that directory, a bare `vainfo`, ffmpeg or Firefox will find
it automatically — that is what "transparent" means.

The driver directory is a hardcoded value compiled into libva (measured: the string inside
`libva.so.2` matches the `driverdir` of `libva.pc`), it is not probed at runtime.

## Two unavoidable pitfalls

**Pitfall 1: libva enforces that 39 vtable slots are non-NULL.**
`vaInitialize` checks them one by one after driver init returns (`CHECK_VTABLE` in
`va/va.c`), and if any is NULL the whole initialization fails and it `dlclose`s. So
unimplemented entry points **must exist too**, pointing at stub functions that return
`VA_STATUS_ERROR_UNIMPLEMENTED`.
It also checks that 5 `max_*` fields are > 0 and that `str_vendor` is non-NULL
(`CHECK_MAXIMUM` / `CHECK_STRING`).

The `VADriverVTable` in `va_backend.h` has 60 slots in total, all assembled
automatically by `tools/gen_stubs.py` from the header file, avoiding omissions from
copying by hand.

**Pitfall 2: the entry point symbol name has to be assembled yourself.**
`__vaDriverInit_<major>_<minor>` must be exported (libva `dlsym`s downwards one by one
from the current version and uses the first hit). The libva headers do **not** provide a
macro to generate that name, so `driver.c` concatenates it from `VA_MAJOR_VERSION` /
`VA_MINOR_VERSION` — more robust across versions than hardcoding
`__vaDriverInit_1_22`.
And because the build uses `-fvisibility=hidden`, that symbol must be marked explicitly
with `__attribute__((visibility("default")))`, otherwise it is not exported and libva
reports `has no function __vaDriverInit_1_0`.

## Build

**Must be compiled inside the container** (aarch64). A development machine that is x86_64
lacks the aarch64 glibc cross toolchain.

Inside the container a direct `git clone` from GitHub hangs on TLS
(`GnuTLS recv error (-110): TLS 链接非正常地终止了`); going through the local socks5 proxy
works:

```bash
git -c http.proxy=socks5h://127.0.0.1:1080 clone \
    https://github.com/Re-s/droidspaces-media-decode.git
```

Note `socks5h` (letting the proxy do DNS) rather than `socks5`, and that this port does
not accept the `http://` scheme prefix.

Dependencies: `libva-dev` (headers only), `libdrm-dev` (the dumb buffer ioctl definitions
and `drm_fourcc.h`, likewise headers only), `gcc`, `make`, `pkg-config`.
All verified present in the container.

```bash
make            # artifact build/msm_drm_drv_video.so
make check      # confirm the __vaDriverInit_* symbol is exported
make clean
```

The driver is a plugin `dlopen`ed by libva, its symbols are provided by the host process,
and it **does not link libva itself**.

## Install

```bash
sudo make install     # → /usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so
sudo make uninstall   # remove
```

`DESTDIR` / `PREFIX` / `LIBDIR` / `DRIDIR` overrides are supported.

No dpkg package claims this file name (measured), so adding it does not conflict with
package management; uninstalling is deleting a single file, fully reversible.

## Verification

Verify from a temporary directory first, and only install into the system once it is
confirmed good:

```bash
make
mkdir -p /tmp/vatest && cp build/msm_drm_drv_video.so /tmp/vatest/
LIBVA_DRIVERS_PATH=/tmp/vatest vainfo --display drm --device /dev/dri/renderD128
```

After installation the acceptance criterion is a **bare run** (zero environment
variables, zero arguments):

```bash
vainfo
```

It should print
`Driver version: DroidSpaces V4L2 VA-API driver 0.4.3+<git-short-hash>` and 6
`VAEntrypointVLD` profiles, with exit code 0.

> The `+<git-short-hash>` suffix is injected by the Makefile on every build, with
> `-dirty` appended when the working tree has uncommitted changes. Its purpose is
> to confirm **which build a consumer actually `dlopen`ed** when debugging
> browsers: Firefox decodes in a separate RDD process, environment variables do
> not always reach it, and guessing from file timestamps leads to wrong
> conclusions.

Other checks:

```bash
vainfo -a          # goes through vaQuerySurfaceAttributes, prints the config attributes of each profile
ffmpeg -init_hw_device vaapi=va:/dev/dri/renderD128 -v verbose \
       -f lavfi -i nullsrc -frames:v 1 -f null -
```

ffmpeg should report `Initialised VAAPI connection: version 1.22` and our vendor string.

> A bare `vainfo` in a headless SSH session first prints Wayland/X11 connection failures
> and then automatically falls back to the drm path successfully. Those two lines have
> nothing to do with this driver.

## Code layout

```
vaapi-driver/
├── Makefile
├── src/
│   ├── driver.c      # entry point __vaDriverInit_*, vtable assembly, logging
│   ├── driver.h      # internal structures, capability constants, declarations of implemented entry points
│   ├── profiles.c    # profile/entrypoint/config queries and config object management
│   ├── stubs.c       # auto-generated: 24 UNIMPLEMENTED stubs
│   ├── stubs.h       # auto-generated
│   └── vtable.inc    # auto-generated: the assembly list of 60 slots
└── tools/
    ├── gen_vtable.py # extract all vtable signatures from va_backend.h → JSON
    └── gen_stubs.py  # JSON → stubs.c / stubs.h / vtable.inc
```

Regenerate after a libva version change:

```bash
make gen        # requires /usr/include/va/va_backend.h inside the container
```

`gen_vtable.py` has one known trap: in the header file there is a line break between the
return type of `vaExportSurfaceHandle` and `(*name)`, so the regex must allow matching
across lines, otherwise that slot is silently missed. The script handles this and includes
a cross-check (number extracted vs. number of members in the header).

## Plugin engineering constraints

The driver runs inside someone else's process (Firefox, ffmpeg), so the rules are stricter
than for a standalone program:

- no `exit()` / `abort()` / `assert()` — on an invalid argument, return the corresponding
  `VA_STATUS_ERROR_*`
- do not write to stdout (that belongs to the host). Logs go to stderr and are silent by
  default, `DMD_VA_LOG=1` turns them on
- all entry points are thread-safe: the config table is protected by a `pthread_mutex`
- `vaTerminate` releases all resources, and is safe even when called after a failed init

ffmpeg matches a `vaapi_driver_quirks` list by the `str_vendor` string. Our string is not
on the list, so we get standard behaviour — which means **the semantics must be standard**,
especially when implementing surface/buffer lifetimes and `vaSyncSurface` later on: no
shortcuts.

## Related documents

- [Project overview](../README.md)
- [Verified platform facts](../doc/verified-platform-facts.md)
- [VAAPI Proxy architecture research](../doc/vaapi-mediacodec-proxy-research.md)
