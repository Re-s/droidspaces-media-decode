# Platform Integration Contract

> 🌐 **中文版（原文）：[platform-integration-contract.md](platform-integration-contract.md)** — the Chinese version is authoritative; if the two disagree, follow it.

For DroidSpaces platform engineers. This document describes only **what the platform side must provide**, together with the measured evidence behind each requirement and the way to verify it.

The devices and environments used to obtain every "measured" conclusion in this document are listed in `doc/verified-platform-facts.md`.
Anything not verified on device is marked "Pending platform confirmation" or "not verified"; no gaps are filled by inference.

---

## 1. One-sentence overview

This component lets standard Linux applications inside a DroidSpaces container (ffmpeg / Firefox / Chrome) use the Android host's **MediaCodec hardware decoding** through **VA-API**, with no application changes; it consists of two parts — the host-side `decode-daemon` and the container-side driver `msm_drm_drv_video.so`, which libva `dlopen`s.

**The platform needs to do four things**:

1. bind mount one host directory into the container (§2.1)
2. start the daemon in the correct SELinux domain (§2.2)
3. pass `/dev/dri/renderD128` through into the container (§2.3)
4. **add SELinux allow rules** so the media **service domains** can transfer
   binder handles to the daemon's domain (§2.2)

No network or port configuration is needed — the channel is a Unix socket and does not touch the network stack.

> ⚠️ **Item 4 is easy to overlook and easy to get backwards.** Once the first three items
> are done, steps 1–7 of the §6 verification checklist **all pass**, and only step 8 (real
> decoding) fails, because the daemon only reaches for a codec when one is created, never
> during the handshake. In other words, "everything looks installed" does not mean decoding
> works.
>
> Getting it backwards: the rules go on the **service domains** as the subject
> (`allow mediacodec droidspacesd binder { call transfer }`), not on the daemon's own
> domain. `binder { transfer }` is checked against the sender, so a permissive receiver
> cannot cover it. See §2.2 for the full reasoning and the verified rule set.

---

## 2. The three things the platform must provide

### 2.1 bind mount of the shared directory

**Requirement**

| Item | Value |
|---|---|
| Host directory | `/data/local/tmp/dmd/` (example path; the actual location is decided by the platform) |
| Container mount point | `/run/dmd/` |
| Mount object | a **directory**, not a single socket file |
| Actual endpoint inside the container | `/run/dmd/decode.sock` |
| Read/write | must be readable and writable (the client has to `connect`) |

The container-side path is the driver's compile-time default `DMD_DEFAULT_SOCK` (`DMD_DEFAULT_SOCK` in `vaapi-driver/src/dmd_client.h`); the file name `decode.sock` is the fixed name `DAEMON_SOCK_NAME` used by the daemon in directory mode (`DAEMON_SOCK_NAME` in `src/decode-daemon.c`). The two sides must match; change one side and the connection fails.

**Why it is required**

The namespace relationship between container and host means only this one transport channel holds for both container types:

- A path-based Unix socket **does not belong to the net namespace**; it is constrained only by the mount namespace, so a bind mount is enough to cross the boundary.
- TCP loopback and abstract sockets both belong to the net namespace. A NAT-mode container has its own netns (measured `4026535650` vs the host's `4026531937`), so `127.0.0.1:20003` is **unreachable** and the number of abstract sockets visible inside the container is **0** (it is 31 both on the host and in a host-mode container).
- The complete path has been verified on device: host creates the Unix socket → bind mount into the container → container `connect` → receive a memfd via `SCM_RIGHTS` → `mmap` and read the content written by the host.
- The DroidSpaces platform itself already uses the same pattern: the host's `/data/local/tmp/anland-<hash>.sock` is bind mounted to the container's `/run/display.sock`; the inodes measured on both sides are identical, confirming it is the same file.

**Why a directory must be mounted rather than a single socket file**

This is an inode-semantics conflict between `bind()` and bind mount, not a matter of preference:

- `bind()` can only **create a new inode**; it cannot bind to an existing file. So every time the daemon restarts, the socket file is a **new inode**.
- A bind mount binds an **inode, not a path**.
- Consequence: if the platform mounts a single socket file, then as soon as the daemon restarts, the container-side mount point still points at the now-orphaned old inode, `connect` immediately becomes `ECONNREFUSED`, and **the mount must be redone**. Worse, at that point `stat` on both sides of the host shows the same inode (both are the orphaned inode), which easily leads to the wrong conclusion that "the mount is still fine".

Mounting a directory does not have this problem: the directory inode is stable, and a socket inside it changing inode does not affect the mount. The platform mounts once, the daemon may restart freely, and the container keeps connecting to `/run/dmd/decode.sock`. This constraint and its countermeasure are documented in the `--sock` directory-mode branch of `src/decode-daemon.c` (search for `S_ISDIR`) and in the comments on the flock liveness check (search for `LOCK_EX`).

**How to verify**

Compare the inodes on both sides; identical means it is the same file:

```bash
# Host (Android side)
stat -c '%i %F %n' /data/local/tmp/dmd/decode.sock
# Inside the container
stat -c '%i %F %n' /run/dmd/decode.sock
```

Expected: the inode numbers in the two outputs are **exactly the same** and both types are `socket`, for example

```
1234567 socket /data/local/tmp/dmd/decode.sock
1234567 socket /run/dmd/decode.sock
```

Then verify that it can actually be connected to (inside the container):

```bash
python3 -c "import socket;s=socket.socket(socket.AF_UNIX);s.connect('/run/dmd/decode.sock');print('connect ok')"
```

Expected output is `connect ok`. If you get `ConnectionRefusedError`, the most likely cause is that the mount point points at an orphaned inode after a daemon restart (see above), or that the daemon is not running.

### 2.2 Starting the daemon in the correct SELinux domain

**Requirement**

```bash
mkdir -p /data/local/tmp/dmd
runcon u:r:droidspacesd:s0 /path/to/decode-daemon --sock /data/local/tmp/dmd
```

Pass a **directory** to `--sock`: when the daemon `stat`s it and finds a directory, it creates the fixed name
`decode.sock` inside it. If the directory does not exist, the daemon **creates it automatically** (0755)
and writes `--sock 目录不存在，已创建` in the log.

`mkdir -p` is still written explicitly above, because the platform normally has to create the directory before it can configure the bind mount —
the mount point must already exist when the container starts.

> **Two pitfalls already fixed, for reference** (fixed before the v0.3.0 release):
> - **Trailing slash**: passing `/data/local/tmp/dmd/` used to produce
>   `/data/local/tmp/dmd//decode.sock`. Functionally fine, but the double slash in the log did not match
>   the string that step 2 of §6 in this document expects, which easily looks like the configuration did not take effect. The trailing slash is now stripped.
> - **Silent degradation when the directory does not exist**: the path used to be bound directly as a socket **file name**,
>   silently producing a single-file socket. A single-file mount inevitably breaks after a daemon restart
>   (the inode changes), and the platform got no warning at all at the time. The directory is now created automatically;
>   if the path ends in `.sock` it is taken as a genuine request for file mode, and in that case a warning is printed explaining
>   that the mount will break after a restart.

Process form of the daemon (section "daemon 的定位" in `README.md`): runs in the foreground, does not daemonize itself, logs to stderr, exits gracefully on `SIGTERM`. **Use the listening endpoint for the liveness check, not the PID** — the PID changes repeatedly.

**Why it is required**

Creating a socket file on the Android side is subject to SELinux. Measured:

- Starting it directly with `su` runs in `u:r:ksu:s0`, and `bind()` returns **`EACCES`**.
- Starting with `runcon u:r:droidspacesd:s0` makes `bind()` succeed.
- The container processes themselves also run in the context `u:r:droidspacesd:s0`.

Creating the socket **does not require any new SELinux rule** — policy already allows it; you just have to use DroidSpaces' own domain. When `bind()` returns `EACCES`, the daemon prints exactly this hint (the `bind()` failure hint in `src/decode-daemon.c`; search for `runcon u:r:droidspacesd`).

### ⚠️ But switching the domain alone is not enough: the platform must add one allow rule

This is the only issue currently **blocking delivery of the Unix socket channel**. Each of the two usable identities has only half of the required permissions:

| Startup identity | Can `bind()` the socket | Can use MediaCodec |
|---|---|---|
| `su` (`u:r:ksu:s0`) | ✗ `EACCES` everywhere | ✓ |
| `runcon u:r:droidspacesd:s0` | ✓ | ✗ SIGABRT |

The crash scene under `droidspacesd`: the top of the tombstone stack is `Codec2Client::GetServiceNames` → `CCodec::allocate`, reporting `Check failed: serviceManager Hardware service manager is not running.`

Three control experiments confirm this has **nothing to do with the transport**:

| Identity + transport | Result |
|---|---|
| `ksu` + TCP | Decodes normally (but that domain is not allowed to bind the socket) |
| `droidspacesd` + TCP | **Same SIGABRT** ← proves it is unrelated to the Unix socket change |
| `droidspacesd` + Unix socket + **SELinux permissive** | **Decodes 9 frames normally** |

The third row is the key one: switching SELinux to permissive with everything else unchanged makes it work, which shows that **the Unix socket channel itself is correct** and all that is missing is one allow rule.

> ⚠️ **Re-corrected in v0.3.2: v0.3.1 overstated this.** Its diagnostic frame was
> right. `binder { transfer }` really is checked against the **sender** (the
> service domain, enforcing), the denial really is `dontaudit`'d, and that really
> was the source of the original misreading.
>
> But it went on to claim that adding rules with `droidspacesd` as the subject
> **does nothing**, and that step is wrong. Measured in v0.3.2: adding only the
> five rules below, with `hwservicemanager` as the subject and `droidspacesd` as
> the target, turns hardware decode from "aborts every time" into byte exact
> output, with 117 consecutive sessions and no new tombstone.
>
> The two findings do not conflict, they are consecutive links in one chain. The
> codec client must first resolve `android.hidl.manager@1.2::IServiceManager`;
> without hwservicemanager it never reaches the IOmx step at all. The
> `EX_TRANSACTION_FAILED for ...::IOmx` that v0.3.1 observed is what happens
> **after** that first link already succeeded.
>
> The lesson: `droidspacesd` being a permissive domain only means access checks
> **with it as the subject** do not block. It does not mean any rule mentioning
> it is inert. In these five rules it is the **target**, the check runs against
> the sender (`hwservicemanager`, enforcing), and it takes effect.
>
> The decisive comparison: `dumpsys -l` (pure enumeration, no handle returned)
> succeeds in both domains, while `dumpsys media.player` (needs a handle back)
> returns `FAILED_TRANSACTION` under `droidspacesd` and works under `ksu`. The
> same pattern reproduces on both the system binder and hwbinder. The first cause
> in the failure chain is
> `getService ... EX_TRANSACTION_FAILED for android.hardware.media.omx@1.0::IOmx`
> → `Cannot obtain IOmx service` → ACodec `-19`. The SIGABRT is a secondary
> effect of the Codec2 fallback, not the root cause.

**Alternatives already ruled out** (all tested on device):

| Approach | Conclusion |
|---|---|
| Use root / `su` | No effect — both domains are already uid 0; SELinux is MAC and does not look at uid |
| DroidSpaces `enable_hw_access=1` | No effect — it only passes `/dev` nodes through, does not change the domain, and applies to the container rather than the host-side daemon |
| `untrusted_app` domain | Dead end — it cannot execute a binary labeled `shell_data_file`, and relabeling with `chcon` is denied |
| The remaining domains (`magisk`/`init`/`shell`/`system_server`/`mediaserver`/`media_codec`/`hal_codec2_default`, 11 in total) | None can satisfy "can be entered" and "can bind" at the same time |
| DroidSpaces `selinux_permissive=1` | Works, but it **switches the whole host SELinux to permissive** (help text verbatim: `Set host SELinux to permissive mode`), disabling protection system-wide; not acceptable as a delivered configuration |

**Rules required** (the subject is always the service domain, `droidspacesd` is the target):

```
allow hwservicemanager droidspacesd binder { call transfer }
allow hwservicemanager droidspacesd fd use
allow hwservicemanager droidspacesd dir search
allow hwservicemanager droidspacesd file { getattr open read }
allow hwservicemanager droidspacesd process getattr

allow mediacodec   droidspacesd binder { call transfer }
allow mediacodec   droidspacesd fd use
allow mediametrics droidspacesd binder { call transfer }
allow mediametrics droidspacesd fd use
```

The five hwservicemanager rules come first because that domain is the gate. The
codec client has to resolve `android.hidl.manager@1.2::IServiceManager` before it
can look up any HAL, and failing that step aborts immediately with
`Check failed: serviceManager Hardware service manager is not running`. That
message reads like a missing IOmx; what is missing is the service manager itself.

Media codecs travel over **hwbinder**, while the `servicemanager` rules already in
the policy only cover **system binder**. That asymmetry is why PulseAudio always
worked while decode always aborted: audio goes over system binder.

The `mediacodec` domain is `media.hwcodec`, the provider of IOmx. Note that the
`hal_codec2_default` type does **not** exist in the policy on the test device
(Codec2 is served from the `mediacodec` domain), and naming a missing type breaks
the whole CIL compile, so do not copy it in.

The platform has done this kind of configuration before, and in **exactly the same
shape** — PulseAudio produces sound thanks to these two lines in `sepolicy.rule`:

```
allow audioserver droidspacesd binder { call transfer }
allow mediaserver droidspacesd binder { call transfer }
```

So hardware decode only needs the OMX-side service domains added. It is a natural
extension of an existing convention, not a new mechanism.

> **Status (v0.3.1)**: implemented and verified on the platform side. The rules
> live in both policy carriers, per platform convention:
> `Android/app/src/main/assets/boot-module/sepolicy.rule` and
> `init/android-service/binary-configuration/droidspaces_binary.cil`. With them in
> place, end-to-end verification is byte exact: 150 frames of 1080p, `vainfo`
> reporting the driver version and 6 VLD profiles, output matching the software
> decode `md5`.
>
> After adding the rules, the `hwservice_manager { find }` denials **are still
> there** and still marked `permissive=1` (one each for IOmx, IAllocator and
> IMapper), but they no longer block, because `find` has `droidspacesd` itself as
> the subject and permissive covers it. This is easy to misread as the rules not
> having taken effect; it is expected.

**How to verify**

```bash
ps -AZ | grep decode-daemon
```

The process label is expected to be `u:r:droidspacesd:s0`.

Also check the daemon's startup log (stderr); two consecutive lines are expected:

```
--sock 是目录，实际监听 /data/local/tmp/dmd/decode.sock
listening on /data/local/tmp/dmd/decode.sock
```

`listening on ...` is the line that marks a successful start; its text is stable and can be matched directly by a supervising script (the TCP branch prints `listening on <port>`, in the same format). **Only when this line appears has the start succeeded**: the daemon does not print it when `bind`/`listen` fails.

**Deployment ordering constraint**

| Step | Action |
|---|---|
| 1 | The daemon starts first and creates the socket file |
| 2 | The platform **then** bind mounts the directory into the container |
| 3 | With a directory mounted, later daemon restarts need no remount |

If for some reason the platform can only mount a single file, step 3 becomes "the mount must be redone after every daemon restart".

> **Pending platform confirmation**: where the daemon binary is placed, the declarative form of the supervision configuration (how the start command and the bind mount entry are written), the start timing (the daemon depends on the Android media services being ready), and the crash-restart and log-collection strategy. These belong to the platform's internal implementation; this project makes no assumptions about them.

### 2.3 `/dev/dri/renderD128` device pass-through

**Requirement**

| Item | Value |
|---|---|
| Device node | `/dev/dri/renderD128` (DRM render node) |
| Owner / group | `root:droidspaces-gpu` |
| Permissions | `crw-rw----` |
| User inside the container | must be in the `droidspaces-gpu` group |

Host-mode containers already have this node; NAT-mode containers did not have it initially — **it appears only after platform configuration**.

**Why it is required**

Both of the following are hard dependencies:

1. **libva relies on it to discover the driver**: libva uses `DRM_IOCTL_VERSION` to get the kernel driver name from `/dev/dri/renderD128`, measured as `msm_drm`; there is no msm entry in the mapping table, so it takes the fallback path of using that name as-is and tries exactly one file name, `/usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so`. Without this node the driver is never loaded at all (section "无感发现机制" in `vaapi-driver/README.md`).
2. **Exportable surfaces depend on it**: the driver allocates surfaces in msm_drm dumb buffers (`DRM_IOCTL_MODE_CREATE_DUMB` + `MAP_DUMB`, the dumb buffer allocation in `vaapi-driver/src/decode.c`), and `vaExportSurfaceHandle` exports a dmabuf fd from them (`vaapi-driver/src/export.c`). **Firefox and Chrome hardware decoding go only through this entry point**; if they cannot get an fd, they silently fall back to software decoding. When the fd is unavailable the driver falls back to ordinary heap memory (the export path in the same file), so ffmpeg's `vaDeriveImage` path still works, but the browser path stops working.

**How to verify**

Inside the container:

```bash
ls -l /dev/dri/renderD128 && id
```

Expected `crw-rw---- 1 root droidspaces-gpu ...`, and the output of `id` contains the `droidspaces-gpu` group.

---

## 3. Differences between the two container types and the support matrix

Measured differences in namespace membership:

| | host-mode | NAT-mode |
|---|---|---|
| net namespace | shared with the host (`4026531937`) | separate (e.g. `4026535650`) |
| IP | holds the real `wlan0` IP directly | `eth0` 172.28.x.x/16, gateway 172.28.0.1 |
| `127.0.0.1:20003` to the host | reachable | **unreachable** |
| Number of visible abstract sockets | 31 (same as the host) | **0** |
| mnt / pid / uts / ipc / cgroup ns | all isolated | all isolated |

This gives the support matrix for the transport channels:

| Transport | Namespace it depends on | host-mode | NAT-mode | Notes |
|---|---|---|---|---|
| **path-based Unix socket** (`--sock`) | mount ns only (crosses the boundary via bind mount) | ⚠️ see below | ⚠️ see below | **the only channel that holds for both container types; recommended** |
| TCP `127.0.0.1:20003` | shared net ns | ✅ | ❌ | a NAT-mode container has its own netns, so loopback does not reach |
| abstract socket (`@` prefix) | shared net ns | ✅ | ❌ | the visible count in a NAT-mode container is 0 |

> ⚠️ **The two ⚠️ marks in the Unix socket row mean: the channel mechanism itself is verified to work, but it currently
> has no effect under SELinux Enforcing** — the allow rule from item 4 of §2.2 is missing.
>
> Measured status:
> - **Under permissive**: both container types work; byte-for-byte comparison of twelve streams is identical 12/12
> - **Under enforcing**: the daemon can create the socket and can complete the handshake, but SIGABRTs when creating the codec,
>   and decoding returns 0 frames (`internal decoding error`)
>
> These two cells only become a real ✅ once the rule is added. Until then, the channel that actually works is TCP
> (host-mode containers only), and the driver falls back automatically.

Endpoint selection logic on the driver side (`session_open` in `vaapi-driver/src/decode.c`):

- `DMD_ENDPOINT=unix:<path>` or `DMD_ENDPOINT=tcp:<port>` can specify it explicitly;
- if unset, it probes automatically: if `/run/dmd/decode.sock` exists and is a socket it is used, otherwise TCP is used;
- when the Unix socket connection fails it **falls back to TCP automatically** — so a host-mode container still works before the platform has set up the mount.

**Conclusion for the platform**: providing only the Unix socket channel covers both container types. Do not set up port forwarding for NAT-mode containers (see section 5).

---

## 4. Optional optimization: memfd zero-copy (SHM transport)

Inline mode (frame data goes directly over the socket byte stream, which is the case for both TCP and Unix sockets) goes through two kernel copies per frame; SHM mode puts frame data into a `memfd` and the socket carries only a 20-byte control message.

**1080p measurements from the standalone test program `tests/test_dmd_client.c`**: +12.9% throughput in the steady-state window and **−28.6%** daemon CPU, with frames decoded in the two modes byte-for-byte identical.

**End-to-end measurements on the driver side** (the driver `dlopen`ed into ffmpeg, over `/run/dmd/decode.sock`): a fixed 1500-frame workload, three alternating paired runs, daemon-side CPU jiffies inline 493/500/489 (median 493) vs SHM 400/367/410 (median 400), a reduction of **about 19%**; the decoded output matches inline mode (150/150 frames).

> ⚠️ **Do not treat the standalone-test number (−28.6%) as the expected gain on the driver side** —
> the measured value in a real consumer environment is **about −19%**; use the latter.

⚠️ **It works only in host-mode containers; it does not work in NAT-mode containers.**

> **Correction**: an earlier version of this document said "it works in both container types because it does not depend on the net namespace". **That conclusion was wrong.** The SHM memfd handoff does not go over the path-based Unix socket of §2.1; instead the daemon opens a separate **abstract** socket (`src/decode-daemon.c`; search for `sun_path[0] = 0`) and the driver connects to that. An abstract socket **belongs to the net namespace** — a NAT-mode container has its own netns and cannot reach that handoff channel. SHM is now enabled by default (see below), so in a NAT-mode container every session creation first fails the handoff and then degrades, wasting one timeout; to save that timeout, set `DMD_WANT_SHM=0` explicitly inside a NAT-mode container.
>
> Lesson: **the control channel being able to cross netns does not mean the SHM handoff channel can cross it too.** To make zero-copy usable in NAT containers, the memfd handoff has to be moved onto the same path-based Unix socket (passing `SCM_RIGHTS` over the existing connection is enough; no separate socket is needed). That is an independent change and has not been implemented yet.

What has been verified on device: a path-based Unix socket works across mount namespaces, `SCM_RIGHTS` can pass fds across the boundary, and a memfd is mappable across namespaces (content written by the host can be read after `mmap` on the container side). These are the necessary conditions for zero-copy, but the current implementation does not use that channel.

**The platform side needs no configuration for this** — the switch and the fallback are internal to this project, not an integration requirement.

✅ **Currently enabled by default (when a Unix socket is in use).** The driver-side decision is `want_shm` in `vaapi-driver/src/decode.c` (search for `DMD_WANT_SHM`): when a Unix socket is in use, leaving `DMD_WANT_SHM` unset means the default value 1; setting `DMD_WANT_SHM=0` explicitly disables it; in TCP mode it is always 0 (SHM is never requested over TCP).

> **Correction**: an earlier version of this document said "currently disabled by default, requires `DMD_WANT_SHM=1` to enable explicitly". **The logic was inverted on 2026-08-26**, so that default-value note is out of date — it is now enabled by default and disabled explicitly with `DMD_WANT_SHM=0`.

Enabling it by default is safe: when the handoff fails on the daemon side it automatically falls back to inline transport (which is exactly what happens in a NAT-mode container), so there is **no hard-failure risk** — the only cost is one extra handoff timeout per session creation.

This path has been verified end to end in a real consumer environment: the driver `dlopen`ed into ffmpeg over `/run/dmd/decode.sock`, with the daemon log confirming `共享内存已交接: 4 槽 x 3133440 字节 (共 12537856)` and `握手成功: video/hevc 1280x720 帧回传=SHM`, and the decoded output matching inline mode (150/150 frames). The earlier `tests/test_dmd_client.c` unit-test conclusions (150 frames, byte-for-byte identical, no fd leaks) still hold as well.

Whether browser sandboxes (the seccomp filters of the Firefox RDD / Chrome GPU processes) can receive `SCM_RIGHTS` **has now been measured and works**: both can establish decoding sessions normally. This project has a precedent: a source-level conclusion claimed that the RDD sandbox returns `EACCES` for all `SYS_SOCKET` calls, yet 713 frames ran through in an on-device test — judgments of this kind can only be settled by measurement.

> ⚠️ **The precise scope of "zero-copy" (platform, please note)**: it only describes the
> **memfd → consumer process** segment. MediaCodec output buffers are allocated by gralloc,
> so the daemon can only take the CPU pointer from `AMediaCodec_getOutputBuffer` and
> `memcpy` it into the memfd (`send_frame_shm` in `src/decode-daemon.c`, around line 949) —
> **the CPU copy from the decoder into the memfd is still there.**
>
> Removing it would require dmabuf passing, and that route has been rejected: it depends on
> private libui/gralloc symbols (pinning the C++ ABI of one specific Android version) for an
> upper-bound gain of only 4.4% of one CPU core.
>
> **What this means for the platform**: supporting SHM does **not** require the platform to
> provide any dmabuf-related capability.

> **Correction**: an earlier version of this document said "when using Unix socket + SHM, the daemon stops serving after the memfd handoff, root cause not identified". **That attribution was wrong.** The real cause is the SELinux domain permission problem in §2.2 (under `droidspacesd` the daemon has no permission to access Codec2 and SIGABRTs at `CCodec::allocate`), and has nothing to do with SHM — the transport mode in the logs at the time was in fact TCP.

So this item is listed as "an internal optimization the platform need not worry about", not an integration requirement; the only difference the platform needs to know about is that NAT-mode containers do not get this gain (see the support matrix in §3).

---

## 5. Things the platform does not need to do

Listed explicitly to avoid over-configuration:

| Item | Conclusion | Basis |
|---|---|---|
| ~~Add SELinux policy~~ | **⚠️ Required, see §2.2** (done by the platform in v0.3.1) | Creating the socket indeed needs no new rule. What is needed is letting the **service domains** transfer a binder handle to `droidspacesd` (`allow mediacodec droidspacesd binder { call transfer }` and friends). Both earlier versions got this wrong: first listed as "not needed", then written with `droidspacesd` itself as the subject |
| Port forwarding / iptables / NAT rules | **Not needed** | The Unix socket channel does not go through the network; NAT-mode containers no longer need TCP reachability |
| Any network configuration (DNS, routing, firewall allow rules) | **Not needed** | The transport does not use the network stack |
| Passing `/dev/ashmem` through | **Not needed** | This node is already passed through (`root:droidspaces-gpu`), but this project **does not use** it — it was replaced by memfd in Android 11+, and the memfd approach is better |
| Providing a tmpfs sharing point (`/dev/shm`, `/run`) | **Not needed** | They are mount points inside the container's mount namespace and do not exist in the Android-side mount table |
| Passing frame data through the shared directory | **Not needed, and should not be done** | `/data/local/tmp` ↔ the container's `/tmp` is backed by f2fs on disk; measured 0.374 ms per frame, 65% slower than memfd's 0.227 ms; usable for the control plane (logs, bitstream files) |
| Installing the VA-API driver into a special path for the container | **Not needed** | The driver is installed in the standard `dri` directory, libva discovers it automatically, zero environment variables |
| A process management module (KSU / Magisk) | **Not needed** | That approach has been abandoned; the daemon is designed as a foreground process supervised by the platform |
| Raising the daemon's privileges or keeping it in a persistent root shell | **Not needed** | Only the correct domain is needed, see 2.2 |

> **Pending platform confirmation**: the target for tightening socket file permissions. The daemon currently does `chmod 0666` (`listen()` and permission setting in `src/decode-daemon.c`; search for `chmod`), and the comments already note that this is a "get it working first" relaxation; a real deployment should tighten it to a specific gid (compare `droidspaces-gpu`, used for `/dev/dri`). **Which gid the platform wants and who sets the group owner** need to be confirmed before the code is changed.
>
> **Pending platform confirmation**: who creates the `/run/dmd/` mount point directory inside the container (`/run` is a tmpfs inside the container), and at which stage of the container startup flow the mount happens.

---

## 6. Verification checklist

Run these in order once the platform configuration is complete. If any step fails, stop there and do not skip ahead.

| # | Location | Command | Expected output |
|---|---|---|---|
| 1 | Host | `ps -AZ \| grep decode-daemon` | The process exists, label `u:r:droidspacesd:s0` |
| 2 | Host | Check the daemon stderr log | Contains `listening on /data/local/tmp/dmd/decode.sock` |
| 3 | Host | `stat -c '%i %F' /data/local/tmp/dmd/decode.sock` | `<inode> socket` (⚠️ when passed through `adb`, see the notes below) |
| 4 | Container | `stat -c '%i %F' /run/dmd/decode.sock` | The inode is **exactly the same** as in step 3, type `socket` |
| 5 | Container | `python3 -c "import socket;s=socket.socket(socket.AF_UNIX);s.connect('/run/dmd/decode.sock');print('connect ok')"` | `connect ok` |
| 6 | Container | `ls -l /dev/dri/renderD128 && id` | `crw-rw---- 1 root droidspaces-gpu`, and `id` contains `droidspaces-gpu` |
| 7 | Container | `vainfo` | `Driver version: DroidSpaces MediaCodec VA-API driver ...` plus 6 `VAEntrypointVLD` profiles, exit code 0 |
| 8 | Container | See the end-to-end ffmpeg command below | `out.yuv` is written normally, and the log contains `Initialised VAAPI connection: version 1.22` and this driver's vendor string |

The full command for step 8 (`-hwaccel_output_format vaapi` must be given explicitly, otherwise ffmpeg automatically downloads into a software format and reports "Impossible to convert between the formats"):

```bash
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
       -hwaccel_output_format vaapi \
       -i in.h264 -vf hwdownload,format=nv12 -f rawvideo -y out.yuv
```

Two auxiliary switches for troubleshooting:

```bash
DMD_VA_LOG=1 <consumer>        # enable driver-side logging (silent by default)
DMD_ENDPOINT=unix:/run/dmd/decode.sock <consumer>   # specify the endpoint explicitly, bypassing auto-probing
```

> Running `vainfo` bare in an SSH session with no display first prints Wayland/X11 connection failures, then automatically falls to the drm path and succeeds. Those two lines have nothing to do with this driver.
>
> ⚠️ Steps 7 and 8 passing **does not mean browsers can hardware-decode either** — ffmpeg goes through `vaDeriveImage` while browsers go through `vaExportSurfaceHandle`; these are two different exits. Browser verification additionally needs a desktop session inside the container and extra environment (see section "浏览器（Firefox）" in `vaapi-driver/README.md`); that part belongs to container image and desktop environment configuration and is out of scope for this contract.

### Notes on running these commands on the Android side

The commands in the table above are written for "run directly in a shell on the target side". When passed in from outside through `adb`, there are two pitfalls,
and we hit both of them during development:

**1. Inner quotes get split apart.** When step 3 is passed through `adb shell su -c`:

```bash
# ✗ Bad: the outer double quotes are processed by the local shell first, and the space in '%i %F' splits it into two arguments
adb shell su -c "stat -c '%i %F' /data/local/tmp/dmd/decode.sock"
#   → stat: '%F': No such file or directory
#     1270438

# ✓ Good: escape the inner quotes
adb shell "su -c \"stat -c '%i %F' /data/local/tmp/dmd/decode.sock\""
#   → 1270438 socket

# ✓ Also good: avoid the space and take the two values separately
adb shell su -c "stat -c %i /data/local/tmp/dmd/decode.sock"
```

Android's `stat` **does support** `%i` and `%F`; the error is purely a quoting-level problem,
so do not mistake it for "Android stat does not support that format specifier".
The type can also be determined with `ls -l` (a socket is shown with a leading `s`, e.g. `srw-rw-rw-`).

**2. The stock Android shell does not support some POSIX syntax.** `for x in a b; do ...; done`
reports a syntax error, and `/dev/tcp/...` does not exist either. When you need a loop or a port test,
run it from the container side, or switch to multiple `adb shell` invocations.

---

## 7. Known limitations

An honest list of items that are currently unresolved or unverified, so the platform can assess them.

| Item | Status |
|---|---|
| SHM (memfd) zero-copy | **Enabled by default** on the driver side when a Unix socket is in use; `DMD_WANT_SHM=0` disables it (the `want_shm` decision in `vaapi-driver/src/decode.c`; search for `DMD_WANT_SHM`). Verified end to end in a real consumer environment (both ffmpeg and browser sandboxes). **Remaining limitations**: ① a NAT-mode container inevitably degrades back to inline because the memfd handoff goes over an abstract socket (see §4); ② "zero-copy" only covers the memfd → consumer segment, the CPU `memcpy` from the decoder into the memfd is still there (the dmabuf approach was rejected, see §4) |
| ~~**Missing SELinux allow rule**~~ | **Resolved in v0.3.1.** What was missing is `binder transfer` for the **service domains** (checked against the sender, so `droidspacesd` being permissive cannot help), not a rule for `droidspacesd`. The platform added it to both policy carriers and verified end to end, byte exact — see §2.2 |
| ~~Browser sandboxes receiving `SCM_RIGHTS`~~ | **Measured and works.** Both the Firefox RDD and the Chrome GPU process (both have seccomp filters) can establish decoding sessions normally. An earlier version of this document listed this as "not verified"; that status is out of date |
| Firefox's `MOZ_DISABLE_RDD_SANDBOX=1` | The original reason for this environment variable was "the RDD sandbox forbids the driver from creating a **TCP** socket". **Whether it is still needed after switching to a Unix socket has not been measured.** It weakens RDD process isolation, so it is a security tradeoff |
| Socket file permissions | Currently `chmod 0666`, so any process inside the container can connect; it should be tightened to a specific gid, pending the platform's confirmation of the target gid |
| Authentication | The Unix socket path relies on file permissions, but at 0666 that provides no isolation; the TCP path has no authentication at all, and loopback is not a boundary in a host-mode container. **Do not use the current version in multi-tenant or untrusted-app environments** |
| Single-file mount scenario | If the platform mounts a single socket file instead of a directory, **the mount must be redone after every daemon restart** (the inode changes) |
| Cleanup of leftover sockets | If the daemon finds a leftover socket file with nobody listening at startup, it `unlink`s it and recreates it, which **changes the inode** and invalidates an existing bind mount (the flock liveness check in `src/decode-daemon.c`; search for `LOCK_EX`) |
| Concurrency limit | `MAX_CLIENTS = 8` while the hardware supports 16; **the behavior at the limit has not been measured**. Beyond the limit new connections are simply closed, and the client only sees the connection being dropped, with no explicit reason for the rejection |
| Codec coverage | H.264 / HEVC / VP9 / VP8 are end-to-end byte-for-byte verified on real hardware. **Not verified**: high bit depth and non-4:2:0 (HEVC Main10, VP9 Profile2, H.264 High 10 / 4:2:2) — the driver does not advertise these capabilities |
| HEVC exception | For streams with `num_short_term_ref_pic_sets > 0` in the SPS, the parameter sets cannot be reconstructed, and the driver returns `VA_STATUS_ERROR_UNIMPLEMENTED` so the layer above falls back cleanly to software decoding |
| Resolution increase out of bounds | When the in-stream resolution grows beyond the upper bound declared during the handshake, the SHM session is terminated (TCP mode has no such limit); the decoder's internal reconfiguration path is **not verified** |
| seek | The driver has not implemented session-rebuild-style seek yet (no consumer currently seeks on the same context); seek at the ffmpeg level has been verified as consistent |
| Platform supervision details | The daemon's placement, start timing, restart and log-collection strategy are all **pending platform confirmation**; this project ships no process management of its own |

---

## Related documents

- [Project overview](../README.md)
- [Verified platform facts](verified-platform-facts.md)
- [VA-API proxy driver (container side)](../vaapi-driver/README.md)
- [Performance measurements and roadmap](performance-and-roadmap.md)
