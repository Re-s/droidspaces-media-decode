# Platform Integration Contract

> 🌐 **中文版（原文）：[platform-integration-contract.md](platform-integration-contract.md)** — the Chinese version is authoritative; if the two disagree, follow it.

For DroidSpaces platform engineers. This document describes only **what the platform side must provide**, together with the measured evidence behind each requirement and the way to verify it.

The devices and environments used to obtain every "measured" conclusion in this document are listed in `doc/verified-platform-facts.md`.
Anything not verified on device is marked "Pending platform confirmation" or "not verified"; no gaps are filled by inference.

> ## ⚠️ Requirements shrank substantially in 0.4.0
>
> The 0.3.x architecture was
> `container ffmpeg → unix socket → Android-side decode-daemon → MediaCodec`,
> which required three things from the platform: a shared-directory bind mount,
> starting the daemon under a specific SELinux domain (plus a binder allow rule),
> and device passthrough.
>
> 0.4.0 switched to **in-driver V4L2 passthrough**:
> `container ffmpeg → libva driver .so → /dev/video32`.
> No socket, no daemon, no cross-process communication, no shared memory,
> no Android-side component.
>
> **The first two are no longer needed.** Only device-node permissions remain.
> If the platform already implemented the bind mount and SELinux domain switch
> per the old contract, that work has no effect on 0.4.0 and can be removed.

---

## 1. In one sentence

The platform only has to ensure an unprivileged user inside the container can
read and write two or three device nodes. The driver is a pure user-space `.so`:
no root, no SELinux domain switch, no daemon, no shared memory.

---

## 2. The only thing the platform must provide: device-node permissions

### 2.1 Required nodes

| Node | Purpose | Required? |
|---|---|---|
| `/dev/video32` | msm_vidc decoder (V4L2 M2M) | **Required** |
| `/dev/dma_heap/system` | DMABUF source (kernel 5.x) | **Either this** |
| `/dev/ion` | DMABUF source (kernel 4.14 and similar) | **or this** |

The driver tries `dma_heap` first and falls back to `/dev/ion`, enumerating the
system heap via `ION_IOC_HEAP_QUERY` rather than **hard-coding a heap id**
(ids differ per device — on one measured device the system heap is 25,
not the commonly assumed 0–7).

`/dev/dri/renderD128` is **not** a dependency of this driver. Mesa uses it for
the GL side, and browser scenarios usually need it, but it is unrelated to this
driver's decode path.

### 2.2 Permission requirement and measured evidence

Measured on this machine (unprivileged container user `master`, member of group
`droidspaces-gpu`):

```
/dev/video32   root:droidspaces-gpu 660   read/write ok
/dev/ion       root:droidspaces-gpu 660   read/write ok
```

**Conclusion: `root:<group visible in container> 660` plus adding the container
user to that group is sufficient. 0666 is not needed.**

Verification (inside the container, not root):

```bash
test -r /dev/video32 && test -w /dev/video32 && echo "video32 ok"
{ test -w /dev/ion || test -w /dev/dma_heap/system; } && echo "DMABUF source ok"
```

### 2.3 No root and no SELinux domain required

Code-level evidence: the sources contain no `geteuid` / `setuid` / SELinux /
`/proc/self/attr` calls at all (`grep` across the repository returns 0 hits);
the only dynamic dependency is `libc.so.6` (per `ldd`).

Measured evidence: the read/write checks above were all performed as an
unprivileged user.

---

## 3. Where the driver is installed

libva looks for `<LIBVA_DRIVER_NAME>_drv_video.so` in the directory given by
`LIBVA_DRIVERS_PATH`; the default can be queried:

```bash
pkg-config --variable=driverdir libva
# on this machine: /usr/lib/aarch64-linux-gnu/dri
```

After installing, set the driver name:

```bash
export LIBVA_DRIVER_NAME=msm_drm
```

Trying it without installing (no writes to system directories):

```bash
LIBVA_DRIVERS_PATH=/tmp/dri LIBVA_DRIVER_NAME=msm_drm ffmpeg ...
```

---

## 4. Container type no longer affects usability

0.3.x had to distinguish host-type from NAT-type containers because the
transport depended on the network namespace:

| | host type | NAT type |
|---|---|---|
| net namespace | shared with host (`4026531937`) | separate (e.g. `4026535650`) |
| `127.0.0.1:20003` to host | reachable | unreachable |
| visible abstract sockets | 31 | 0 |

None of this affects 0.4.0 — the driver `open()`s the device nodes directly
inside the container process: no cross-process hop, no network, no abstract
sockets. Both container types are equally usable as long as the permissions in
section 2 are satisfied.

(The namespace figures above are still accurate measurements and are kept for
other purposes.)

---

## 5. What the platform does **not** need to do

All of the following were 0.3.x requirements and are unnecessary in 0.4.0:

- ❌ Shared-directory bind mount (`/run/dmd/`) and its inode-invalidation problem
- ❌ Starting a daemon under a specific SELinux domain, and the accompanying binder allow rule
- ❌ Port forwarding for NAT-type containers
- ❌ Deploying the KernelSU module (`ksu-module/` has been removed entirely)
- ❌ memfd / shared-memory transport
- ❌ Any resident Android-side process, along with its placement, start, restart and logging strategy

---

## 6. Verification checklist

Run inside the container; integration is complete when all pass:

```bash
# 1) Nodes are read/writable (not root)
test -r /dev/video32 && test -w /dev/video32 && echo "1) video32 ok"
{ test -w /dev/ion || test -w /dev/dma_heap/system; } && echo "1) DMABUF ok"

# 2) Decoder identity is correct (no v4l2-utils needed; ask via QUERYCAP)
python3 -c "
import fcntl
b=bytearray(104)
with open('/dev/video32','rb+',buffering=0) as f:
    fcntl.ioctl(f, 0x80685600, b)   # VIDIOC_QUERYCAP
print('driver =', bytes(b[0:16]).split(b\'\\0\')[0].decode())
print('card   =', bytes(b[16:48]).split(b\'\\0\')[0].decode())"
# expect driver=msm_vidc_driver  card=msm_vidc_vdec
# If v4l2-utils is installed, v4l2-ctl -d /dev/video32 --info is equivalent

# 3) Hardware-decode one frame
LIBVA_DRIVER_NAME=msm_drm ffmpeg -hwaccel vaapi \
  -i sample.h264 -frames:v 1 -f null -
```

> ⚠️ **Do not use `vainfo` to verify.** It hangs on this platform, even when
> given a driver name that does not exist. Use the ffmpeg command in step 3.

---

## 7. Known limitations

An honest list of what is unresolved or unverified, so the platform can assess it.

| Item | Status |
|---|---|
| **Device compatibility** | Not every device's `/dev/video32` works. One device was measured (Xiaomi Pad 5 / nabu, kernel 4.14) where the decode session never starts: every step of the two-phase negotiation returns success, but `V4L2_EVENT_SOURCE_CHANGE` never arrives, input buffers are never returned (`DQBUF` always `EAGAIN`), and the msm_vidc IRQ delta is indistinguishable from idle. The **encoder** on the same device (`/dev/video33`) works fine (`screenrecord` succeeds, IRQ delta 753). Ruled out: buffer type, ION heap, physical contiguity, container permissions, feed granularity, controls, STREAMON ordering. This is a decode-path problem on that device, not a platform-configuration problem. Use `vaapi-driver/tools/probe_device_support.c` to decide: receiving `SOURCE_CHANGE` means the device is usable |
| AV1 | **Not yet pixel-exact**, so release builds do not advertise the profile (advertising it makes browsers/ffmpeg hand over work that then fails). Current state in `doc/av1-v4l2-status.md`. Development builds can enable it with `make AV1=1` |
| VP8 | **Unsupported.** `ENUM_FMT` on `/dev/video32` does list `VP80`, but the driver lacks the RFC 6386 §9.1 uncompressed-chunk reconstruction, so it is not advertised |
| Codec coverage | H.264 / HEVC / VP9 verified end-to-end byte-for-byte on device. **Not verified**: high bit depth and non-4:2:0 (HEVC Main10, VP9 Profile2, H.264 High 10 / 4:2:2) — the driver does not advertise these capabilities |
| HEVC exception | For streams with `num_short_term_ref_pic_sets > 0` in the SPS the parameter sets cannot be reconstructed; the driver returns `VA_STATUS_ERROR_UNIMPLEMENTED` so the layer above falls back cleanly to software decoding |
| seek | Session-rebuild-style seek is not implemented (no consumer currently seeks on the same context); seek at the ffmpeg level is verified consistent |
| Access control | 0.4.0 has no cross-process channel, hence no channel authentication problem; access control is entirely the device nodes' file permissions. **If the nodes are opened up to 0666, any process in the container can use the hardware decoder** — multi-tenant environments or those running untrusted apps should use group permissions per §2.2 rather than 0666 |
| Concurrency limit | The hardware supports 16 sessions; the **driver-side limit behaviour is not yet measured**. Each context holds its own `/dev/video32` fd, so exceeding the limit shows up as `open()` or negotiation failure |
| Performance and power | All 0.3.x measurements are void (the architecture changed); the V4L2 path's performance and power draw are **not yet measured**. See the void markers in `doc/performance-and-roadmap.md` |
| Resolution changes | The driver rebuilds the session when the in-stream resolution grows; that path has **no dedicated verification** |

---

## Related documents

- Current capabilities and known limits → `CHANGELOG.md` at the repository root
- Architecture → `README.md` at the repository root
- Driver implementation details and pitfalls → `vaapi-driver/README.md`
- Verified platform facts → `doc/verified-platform-facts.md`
- Why V4L2 was once ruled out, and the correction → `doc/why-not-v4l2.md`
- AV1 current state → `doc/av1-v4l2-status.md`
