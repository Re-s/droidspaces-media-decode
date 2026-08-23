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
4. **add one SELinux allow rule**: allow the daemon's domain to access
   hwservicemanager and the Codec2 HAL (§2.2)

No network or port configuration is needed — the channel is a Unix socket and does not touch the network stack.

> ⚠️ **Item 4 is the only current blocker; do not overlook it.** Once the first three items are done,
> steps 1–7 of the §6 verification checklist **all pass**, and only step 8 (real decoding) fails —
> because the daemon only touches Codec2 when a codec is created, never during the handshake.
> In other words, "everything looks installed" does not mean decoding works. See §2.2.

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

**Alternatives already ruled out** (all tested on device):

| Approach | Conclusion |
|---|---|
| Use root / `su` | No effect — both domains are already uid 0; SELinux is MAC and does not look at uid |
| DroidSpaces `enable_hw_access=1` | No effect — it only passes `/dev` nodes through, does not change the domain, and applies to the container rather than the host-side daemon |
| `untrusted_app` domain | Dead end — it cannot execute a binary labeled `shell_data_file`, and relabeling with `chcon` is denied |
| The remaining domains (`magisk`/`init`/`shell`/`system_server`/`mediaserver`/`media_codec`/`hal_codec2_default`, 11 in total) | None can satisfy "can be entered" and "can bind" at the same time |
| DroidSpaces `selinux_permissive=1` | Works, but it **switches the whole host SELinux to permissive** (help text verbatim: `Set host SELinux to permissive mode`), disabling protection system-wide; not acceptable as a delivered configuration |

**Rule required**: allow `droidspacesd` to access hwservicemanager and the Codec2 HAL.

The platform has done this kind of configuration before — the `droidspaces-gpu` group authorization on `/dev/dri` and `/dev/ashmem` is the precedent, showing the platform both can and routinely does grant domain-level access. (This project does **not** use `/dev/ashmem`; it is cited here only as evidence that the platform has done work of the same kind, not as something to configure — see §5.)

> **Pending platform confirmation**: where this rule should be added, and whether you would rather define a dedicated domain for the daemon that holds both sets of permissions. Until the rule is in place, the driver automatically falls back to TCP (which works in the `ksu` domain); functionality does not regress, but NAT-mode containers cannot use hardware decoding.

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

TCP mode goes through two kernel copies per frame; SHM mode puts frame data into a `memfd` and the socket carries only a 20-byte control message.

**The measurement environment was `tests/test_dmd_client.c` (a standalone test program).** Measured 1080p gains: +12.9% throughput in the steady-state window and **−28.6%** daemon CPU, with frames decoded in the two modes byte-for-byte identical.

> ⚠️ **Do not treat these numbers as the expected gain on the driver side.**
> They come from a standalone test program, whereas the SHM path in the driver
> — once `dlopen`ed into a real consumer (ffmpeg / a browser) — **has never run
> end to end** (see below). The real gain is unknown and may be lower.

⚠️ **It can possibly be used only in host-mode containers; it does not work in NAT-mode containers.**

> **Correction**: an earlier version of this document said "it works in both container types because it does not depend on the net namespace". **That conclusion was wrong.** The SHM memfd handoff does not go over the path-based Unix socket of §2.1; instead the daemon opens a separate **abstract** socket (`src/decode-daemon.c`; search for `sun_path[0] = 0`) and the driver connects to that. An abstract socket **belongs to the net namespace** — a NAT-mode container has its own netns and cannot reach that handoff channel, so `DMD_WANT_SHM=1` there inevitably fails the handoff and then degrades, wasting one timeout per session creation.
>
> Lesson: **the control channel being able to cross netns does not mean the SHM handoff channel can cross it too.** To make zero-copy usable in NAT containers, the memfd handoff has to be moved onto the same path-based Unix socket (passing `SCM_RIGHTS` over the existing connection is enough; no separate socket is needed). That is an independent change and has not been implemented yet.

What has been verified on device: a path-based Unix socket works across mount namespaces, `SCM_RIGHTS` can pass fds across the boundary, and a memfd is mappable across namespaces (content written by the host can be read after `mmap` on the container side). These are the necessary conditions for zero-copy, but the current implementation does not use that channel.

**The platform side needs no configuration for this** — this item is a later optimization, not an integration requirement.

⚠️ **Currently disabled by default.** On the driver side, SHM is requested only when `DMD_WANT_SHM=1` and the Unix socket is already in use (the `want_shm` decision in `vaapi-driver/src/decode.c`; search for `DMD_WANT_SHM`).

This path has **never actually been enabled on the driver side** (`want_shm` has long been hard-coded to 0); only `tests/test_dmd_client.c` has exercised it: 150 frames measured, byte-for-byte identical to TCP for the first 20 frames, no fd leaks. But that is a standalone test program, not the real environment in which the driver is `dlopen`ed into a consumer process.

The risk that has not been verified is whether browser sandboxes (the seccomp filters of the Firefox RDD / Chrome GPU processes) can receive `SCM_RIGHTS`. This project has a precedent: a source-level conclusion claimed that the RDD sandbox returns `EACCES` for all `SYS_SOCKET` calls, yet 713 frames ran through in an on-device test — judgments of this kind can only be settled by measurement.

> **Correction**: an earlier version of this document said "when using Unix socket + SHM, the daemon stops serving after the memfd handoff, root cause not identified". **That attribution was wrong.** The real cause is the SELinux domain permission problem in §2.2 (under `droidspacesd` the daemon has no permission to access Codec2 and SIGABRTs at `CCodec::allocate`), and has nothing to do with SHM — the transport mode in the logs at the time was in fact TCP.

So this item is listed as "a later optimization the platform need not worry about", not an integration requirement.

---

## 5. Things the platform does not need to do

Listed explicitly to avoid over-configuration:

| Item | Conclusion | Basis |
|---|---|---|
| ~~Add SELinux policy~~ | **⚠️ Required, see §2.2** | An earlier version wrongly listed this as "not needed". Creating the socket indeed needs no new rule, but the `droidspacesd` domain **has no permission to access hwservicemanager / Codec2**, and the daemon SIGABRTs at `CCodec::allocate`. This is the only current blocker |
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
| SHM (memfd) zero-copy | **Disabled by default** on the driver side, requires `DMD_WANT_SHM=1`. This path has never actually been enabled on the driver side (only unit tests exercised it: 150 frames, byte-for-byte identical, no fd leaks); not verified in a real consumer environment; whether browser sandboxes can receive `SCM_RIGHTS` has not been measured either (the SHM-disabled-by-default note in `vaapi-driver/src/decode.c`; search for `DMD_WANT_SHM`) |
| **Missing SELinux allow rule** | **The only current blocker.** The `droidspacesd` domain has no permission to access hwservicemanager/Codec2, and the daemon SIGABRTs at `CCodec::allocate`. Under permissive, decoding works under otherwise identical conditions, which proves the channel itself is correct — see §2.2 |
| Browser sandboxes receiving `SCM_RIGHTS` | **Not verified** (the Firefox RDD / Chrome GPU processes have seccomp filters) |
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
