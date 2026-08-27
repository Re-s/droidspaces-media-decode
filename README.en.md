# DroidSpaces Media Decode Daemon

> 🌐 **中文版（原文）：[README.md](README.md)** — the Chinese version is authoritative; if the two disagree, follow it.

An Android MediaCodec hardware decode proxy service that provides hardware video decoding to Linux containers.

## Overview

This project provides a MediaCodec hardware decode daemon that runs on an Android device and exposes hardware decoding to Linux containers (such as the Debian container in DroidSpaces). Applications inside the container can use Android's hardware decoder through the standard VA-API without any modification.

### Key features

- **Hardware-accelerated decoding**: uses the Android MediaCodec API to decode H.264 / HEVC / VP8 / VP9 in hardware
- **Standard VA-API interface**: a VA-API driver is provided on the container side, so applications (ffmpeg, Firefox, Chrome) work unmodified
- **Two transports, selected automatically**:
  - **path-based Unix socket** (recommended): it does not belong to a net namespace and crosses the boundary through a bind mount,
    so **it works for both host-mode and NAT-mode containers**; authorization relies on file permissions and SELinux, and the service stays off the network.
    ⚠️ The daemon currently does `chmod 0666` after creating the socket (a permissive value marked
    "just get it working" in `src/decode-daemon.c`); a real deployment should tighten this to a specific gid.
  - **TCP 127.0.0.1** (fallback): usable only when the container and the host **share a net namespace**
    (host-mode containers do; NAT-mode containers do not).

  The driver either takes an explicit endpoint from `DMD_ENDPOINT`, or probes the default path and falls back to TCP.

  **memfd zero-copy is enabled by default in Unix socket mode** (`vaapi-driver/src/decode.c:483`);
  it can be turned off explicitly with `DMD_WANT_SHM=0`, and a non-Unix-socket (TCP) transport is always inline —
  the memfd handover goes over a separate abstract socket, which belongs to a net namespace,
  so **a NAT-mode container will necessarily be downgraded**.
  This path has been verified end to end in a real consumer environment (the driver `dlopen`ed into ffmpeg over
  `/run/dmd/decode.sock`; the daemon log confirms `共享内存已交接: 4 槽 x 3133440 字节` and
  `握手成功: video/hevc 1280x720 帧回传=SHM`, the decode result matches the inline path 150/150 frames,
  and daemon CPU drops by about 19%). A failed handover on the daemon side falls back to inline automatically,
  so enabling it carries no risk of hard failure.
  A browser sandbox receiving `SCM_RIGHTS` has also been verified on device: both the Firefox RDD and the
  Chrome GPU process establish decode sessions normally (2026-08-26).
  ⚠️ **"Zero-copy" only describes the memfd → consumer process leg**: the MediaCodec output buffer is allocated by
  gralloc, so the daemon can only take a CPU pointer via `AMediaCodec_getOutputBuffer` and `memcpy` it into the memfd —
  **the CPU copy from the decoder into the memfd is still there**.
- **Minimal implementation**: simplified from the libdisplay_daemon library of the anland project; the code is concise and easy to follow
- **Process supervision**: the daemon is designed as a foreground process supervised by the platform (DroidSpace is responsible for starting and keeping it alive)

## Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│                       Android device (root)                        │
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                        decode-daemon                         │  │
│  │                                                              │  │
│  │  • Listens on a Unix socket (--sock, recommended)            │  │
│  │    or on TCP 127.0.0.1:20003 (fallback)                      │  │
│  │  • Receives H.264 / HEVC / VP8 / VP9 bitstreams              │  │
│  │  • Decodes in hardware through MediaCodec                    │  │
│  │  • Returns NV12 frames (over TCP or memfd zero-copy)         │  │
│  └──────────────────────────────┬───────────────────────────────┘  │
│                                 │ Unix socket (bind mount)         │
│                                 │ or TCP 127.0.0.1:20003           │
│                                 │                                  │
│  ┌──────────────────────────────┴───────────────────────────────┐  │
│  │               Linux container (Debian aarch64)               │  │
│  │                                                              │  │
│  │  • decode-client (client/)                                   │  │
│  │      FFmpeg demux → send NALUs                               │  │
│  │      receive NV12 frames → EGL/GLESv2 render or PPM          │  │
│  │  • tools/test_decode.py — reference protocol impl.           │  │
│  └──────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────┘
```

DroidSpaces has **two kinds of containers** with different namespace membership, and that directly determines which transports are available:

| | host-mode | NAT-mode |
|---|---|---|
| net namespace | **shared** with the host (`4026531937`) | **separate** (e.g. `4026535650`) |
| IP | holds the real `wlan0` address directly | `eth0` 172.28.x.x/16, gateway 172.28.0.1 |
| `127.0.0.1:20003` | reachable | **unreachable** |
| number of visible abstract sockets | 31 (same as the host) | **0** |
| mnt / pid / uts / ipc / cgroup ns | all isolated | all isolated |

So **both TCP loopback and abstract socket depend on a shared net namespace** and only work in host-mode
containers. Neither works in a NAT-mode container — an abstract socket belongs to a net namespace
and is not a substitute for TCP.

**A path-based Unix socket is the general solution**: it does not belong to a net namespace, the platform
only has to bind mount the host's socket file into the container, both container kinds can use it, and
authorization comes directly from file permissions, so the service does not have to be exposed on the network.
The full path was verified on device — the container's `connect` succeeded,
the memfd was received through `SCM_RIGHTS`, and `mmap` read back the content written by the host,
which proves three things at once: a path-based Unix socket works across a mount namespace,
`SCM_RIGHTS` can pass an fd across the boundary, and a memfd can be mapped across namespaces.

DroidSpaces' own display channel uses exactly this pattern: host
`/data/local/tmp/anland-<hash>.sock` → container `/run/display.sock`
(the inode was measured to be identical on both sides, confirming it is the same file).

⚠️ **Two deployment constraints + one note on the current status** (the constraints were all learned the hard way on device):

1. **You must mount the directory, not the individual socket file.** `bind()` can only create a new inode,
   and a bind mount binds an **inode, not a path** — when a single file is mounted, the daemon changes its
   inode as soon as it restarts, the container side immediately gets `ECONNREFUSED`, and at that point `stat`
   on both sides may still report the same inode (both see that orphaned inode), which makes misdiagnosis very easy.
   Mounting the directory keeps the inode stable: host `/data/local/tmp/dmd/` → container `/run/dmd/`,
   and the daemon can restart as often as it likes with no effect.

2. **The daemon needs a suitable SELinux domain, and each existing domain has only half of the permissions.**
   This is the only issue currently **blocking delivery of the Unix socket transport**:

   | Launch identity | Can bind a socket | Can use MediaCodec |
   |---|---|---|
   | `su` (`u:r:ksu:s0`) | ✗ `EACCES` everywhere | ✓ |
   | `runcon u:r:droidspacesd:s0` | ✓ | ✗ SIGABRT |

   Under `droidspacesd` it crashes in `CCodec::allocate`; the top of the tombstone stack is
   `Codec2Client::GetServiceNames` and it reports `Hardware service manager is not running`
   — that domain is not allowed to access hwservicemanager / the Codec2 HAL.

   Three control experiments confirm this has nothing to do with the transport: `ksu`+TCP works,
   `droidspacesd`+TCP hits the same SIGABRT, and `droidspacesd`+Unix socket+**SELinux permissive**
   decodes normally. In other words, **the Unix socket transport itself is correct**; all that is missing is one allow rule.

   Alternatives already ruled out: root does not help (both domains are already uid 0, and SELinux is MAC
   and does not look at uid); DroidSpaces' `enable_hw_access=1` does not help (it only passes through `/dev`
   nodes, does not change the domain, and applies to the container rather than to the daemon on the host side);
   the `untrusted_app` domain is a dead end (it cannot execute a binary labelled `shell_data_file`, and
   relabelling is denied); `selinux_permissive=1` works but amounts to turning off protection system-wide,
   so it cannot be a delivery form.

   **The platform needs to add one rule**: allow `droidspacesd` to access hwservicemanager and
   the Codec2 HAL. DroidSpaces has already configured access to `/dev/dri` and `/dev/ashmem`
   for that domain, so this is the same kind of work.

3. **Until that rule is in place, the default path is still TCP.** When the driver cannot find a usable socket
   it falls back to TCP automatically (which works under `u:r:ksu:s0`); the behaviour matches v0.2.0, with no regression.

### Performance

1080p peaks at **194 fps** and 4K at **82 fps**; both meet 60 fps (3.24× / 1.37× headroom).
Latency is 4.46 ms at p50 and 9.77 ms at p95.

> These two peaks were measured **end to end with a single client** (over the TCP transport). The
> 275.5 / 244.0 fps that appear elsewhere in this documentation are a **steady-state window** comparison
> between SHM and TCP, measured on a different basis; they cannot be compared side by side with the figures
> here, and they do not mean that 194 fps is the hardware ceiling. The original bottleneck was the daemon's single-threaded
serial structure (not TCP transport and not the hardware decoder), and it has been removed by splitting
send and receive: single-client throughput improved by 20–30%, and multiple concurrent clients are now
supported (4 streams total about 253 fps).

> **These are wall-clock throughput upper bounds** (`tools/probe_cost.c` times each frame with
> `CLOCK_MONOTONIC`); they mean "how fast continuous decoding can go", **not a real-time playback frame rate**.
> Real-time playback in a browser is about 30 fps — that is limited by playback pacing, not by the decoder's ceiling.
>
> ⚠️ Any quoted frame rate must state whether it is wall-clock or content duration. This project once took
> "143 frames ÷ 5 seconds of content duration" for a real-time playback frame rate; the real figure was only 6.4 fps.
See [Performance measurements and roadmap](doc/performance-and-roadmap.md) for details.

### Wire protocol

- **Input format**: `[4-byte NALU length (big endian)][NALU data]`; the NALU must carry a start code (either the 3-byte `00 00 01` or the 4-byte `00 00 00 01`)
- **Output format**: `[4-byte width][4-byte height][4-byte frame size][4-byte input unit index][NV12 frame data]`, all big endian
  - The fourth field is the **input unit index** for that frame (which data unit it came from, starting at 1):
    the daemon writes it into the `presentationTimeUs` of `queueInputBuffer`, and
    MediaCodec carries it through to the output frame unchanged. The client uses it to pair frames exactly,
    **without needing to know in what order the decoder emits frames**.
  - This field is only present when the format description block declares `CAP_FRAME_PTS` (see below);
    an old daemon does not send it, and the client parses 3 fields.
  - Shared memory mode correspondingly adds one field with the same meaning after `[slot][length]`.
- **The 8 MB maximum unit size constrains the upstream direction only**: `MAX_FRAME` is used solely to validate
  data units sent by the client (`sz > MAX_FRAME` at `src/decode-daemon.c:579`); **there is no size limit check at all
  on downstream frames**. Clients must not use 8 MB to validate downstream data — a single 4K NV12 frame is already
  12441600 bytes, which exceeds it, so an 8 MB check would misjudge a perfectly normal 4K stream as a protocol error.
- **Capability bits in the format description block**: the second word of the block header (formerly a reserved 0)
  declares daemon capabilities, `0x1` = `CAP_FRAME_PTS` (the frame header carries the input unit index). An old daemon always sends 0.
- **Decoder input timeout**: 5 seconds
- **Parameter set handling**: the server recognizes parameter set NALUs (types 7/8 for H.264, 32/33/34 for HEVC), accumulates them as codec-specific data, and submits them with `BUFFER_FLAG_CODEC_CONFIG`; these NALUs do not produce frames
- **Send and receive are asymmetric**: the number of NALUs submitted ≠ the number of frames returned (decoder queuing/reordering); after sending everything the client must `shutdown(SHUT_WR)` to trigger a flush in order to collect all remaining frames
- **Length 0 = a reversible drain request** (not a data unit, and not an invalid length):
  the daemon sends EOS to force the decoder to emit the frames it holds, then after collecting them calls
  `AMediaCodec_flush` to reset and resends the CSD, so **the session remains usable afterwards**. This is used to
  break the deadlock where the consumer waits for a frame while the decoder waits for input —
  a browser keeps only 3 frames in flight, while a decoder with B frames needs a 4th input unit before it emits the first frame.
  The difference from `shutdown(SHUT_WR)` is that it does not invalidate the session: measured at 33.6 ms per frame
  (29.8 fps at full speed), versus 155 ms (6.4 fps) when closing the write end, because that rebuilds the session for every frame.
  After a drain the daemon sends the format description block again.
  Length 0 was chosen as the carrier because it was already treated as invalid, so old clients never send it and no version negotiation is needed.

#### Handshake (required)

The client must complete the handshake before sending any data, declaring the codec, the resolution and the transport mode.
The daemon and the client are released together, and no compatibility path without a handshake is kept:

```
[4B magic 0x444D4400][4B version=2][4B codec][4B width][4B height][4B transport mode]   24 bytes total
```

`codec` values: `0`=H.264 `1`=HEVC `2`=VP9 `3`=VP8.
Transport mode: `0`=TCP, `1`=shared memory (see the next section).

**The version must be exactly 2**: the daemon tests for strict equality (`ver != HELLO_VERSION` at
`src/decode-daemon.c:409`),
does not accept a lower version number, and on mismatch immediately replies `status=1` and disconnects.

Different codecs split data units differently, and the client must send data according to the matching rule:

| Codec | What goes in the length prefix | Start code | Parameter sets |
|------|-----------------|--------|--------|
| H.264 / HEVC | a single NALU | **required** (3 or 4 bytes) | extracted from extradata and sent first |
| VP9 / VP8 | one complete frame | **must not be present** | no separate parameter sets, they are inside the key frame |

Adding an Annex B start code to VP8/VP9 data corrupts the frame content and the decoder will reject the whole bitstream.

The server response is variable length:

```
[4B status][4B transport mode actually used][4B name length n][n bytes of name]
```

`status`: `0`=accepted, `1`=version not supported, `2`=codec not supported,
`3`=resolution outside the hardware range (96×96 ~ 8192×4320), `4`=handshake missing. On a non-zero status the connection is then closed.

A request for SHM may be downgraded to TCP (out of memory, handover timeout, and so on). In TCP mode the name length is 0.

**Note: `mode=SHM` is only a statement of intent, not a guarantee.** The response is sent **before** the memfd handover,
so the daemon may announce SHM first and then silently fall back to TCP when the handover fails (the client never
comes to collect it, or times out). The client **must implement its own fallback**: if collecting the memfd fails it has to be
able to continue parsing in the TCP frame format, and it must not assume that subsequent messages will be slot messages
just because the response said SHM.
Both `client/` and `tools/test_decode.py` in this repository are implemented this way.

#### Shared memory transport (enabled by default in Unix socket mode, saves two copies)

In TCP mode every frame goes through two kernel copies (copied into the socket buffer on send, copied back out on receive).
A 1080p NV12 frame is 3 MB, which at 60 fps means 180 MB/s of extra memory bandwidth.

The driver requests SHM by default whenever it goes over a Unix socket; `DMD_WANT_SHM=0` turns it off explicitly.
TCP mode is always inline (the memfd handover goes over an abstract socket, which belongs to a net namespace).

SHM mode puts the frame data into a `memfd` and the socket carries only a 20-byte control message:

```
[4B width][4B height][4B 0xFFFFFFFE][4B slot number][4B data length]
```

The name in the handshake response is an **abstract socket** (of the form `dmd-shm-<pid>-<session id>`),
named by the daemon and listened on in advance. The client connects to it and the daemon hands over the memfd
with `SCM_RIGHTS`, together with 12 bytes of `[slot count][bytes per slot][total pool bytes]`.

Why an abstract socket rather than a path-based one: the container and Android share a net namespace
(an abstract socket belongs to the net ns and is visible to both sides), but the mount namespace is isolated,
so a path-based Unix socket simply does not exist on the other side.

Why the daemon does the naming: the client has no way to know which connection number it is. Letting the client
guess the name inevitably leads to crosstalk or a failed connection — this is a real defect we hit during implementation.

Pool layout and the return protocol:

```
[control area 4096 bytes][slot 0][slot 1][slot 2][slot 3]
```

The control area begins with one 32-bit status word per slot. The daemon sets it to 1 after writing data, and
**the client must set it back to 0 to return the slot**, otherwise, once the pool is exhausted, the daemon waits about
1 second, decides the client is stuck and ends the session.
Returning slots goes through shared memory rather than the socket, so it does not interleave with the upstream NALU flow.

Do not confuse the two timeouts: **the slot wait is about 1 second** (the client does not return a slot), and
**the memfd handover wait is 3 seconds** (the client got the name but never comes to connect; the downgrade was measured to happen at 3.0~3.5 s).

Slots are sized by the **adaptive-playback limit** (`max(declared width,1920) × max(declared height,1088)`)
rather than by the actual resolution declared in the handshake. The reason: if slots are allocated for the current
resolution only, a mid-stream resolution increase (480p→720p) exceeds the slot and the SHM session is forced to terminate —
for the same bitstream TCP decoded a full 120 frames while SHM produced only 60, a functional regression measured on device.
The cost is that a 720p stream also occupies a 1080p-sized pool (4 × 3133440 ≈ 12 MB), trading memory for correctness.

A boundary that remains: if the resolution exceeds the declared adaptive-playback limit the session is still terminated
(reporting `帧 N 字节超出槽位 M，需重建池`), but there is no out-of-bounds write and other sessions are unaffected.

Measured gains (1080p, same bitstream, two independent measurements; **the measurement environment was
the standalone test program `tests/test_dmd_client.c`**):

| Method | TCP | SHM | Improvement |
|------|-----|-----|------|
| average over the full 2500 frames | 166.8 fps | 194.7 fps | +17% |
| steady-state window over 4500 frames (first 3 s dropped) | 244.0 fps | 275.5 fps | +12.9% |

The difference between the two comes from the choice of window: the full average includes the DVFS ramp-up phase,
while the steady-state window is closer to the real ceiling; for the size of the improvement, the steady-state window's
+12.9% is the more trustworthy figure (SHM varied 1.0% across two runs, TCP 7.2%).

Daemon CPU: 763 → 545 ticks / 1800 frames, **-28.6%**.
The PPMs decoded in the two modes are byte-for-byte identical (12/12 equal across the first 3 NV12 frames).

The driver-side end-to-end figures (the driver `dlopen`ed into ffmpeg over `/run/dmd/decode.sock`) were measured
separately: a fixed 1500-frame workload, three alternating paired runs, daemon-side CPU jiffies — inline
493/500/489 (median 493) vs SHM 400/367/410 (median 400), **about 19% lower**, with ±2% variance within a group.
What is saved is the one copy of 1.38 MB (720p) / 3.11 MB (1080p) per frame that used to go through the socket.

⚠️ **"Zero-copy" only describes the memfd → consumer process leg.** The MediaCodec output buffer is allocated by
gralloc, so the daemon can only take a CPU pointer via `AMediaCodec_getOutputBuffer` and `memcpy` it into the memfd
(`send_frame_shm` in `src/decode-daemon.c`, around line 949) — **the CPU copy from the decoder into the memfd is
still there**. Removing it would require passing a dmabuf, and that route was rejected because it would have to rely
on private libui/gralloc symbols (pinning it to the C++ ABI of a specific Android version) while the upper bound on
the gain is only 4.4% of one CPU core (see `doc/performance-and-roadmap.md`).

#### Format description block

The server introduces the format description block with a **sentinel frame header**; when the client reads it, it updates the format:

```
[4B 0][4B 0][4B 0xFFFFFFFF]                          ← sentinel frame header
[4B buffer width][4B buffer height][4B stride][4B slice_height]
[4B crop_left][4B crop_top][4B crop_right][4B crop_bottom]
```

A legal frame size can never equal `0xFFFFFFFF` (4 GiB, far beyond any NV12 frame), so there is no ambiguity.
crop is a closed interval, so the display width = `crop_right - crop_left + 1`.

The format block always appears once **before the first frame**. **It is sent again when the resolution changes mid-stream**
(adaptive-playback is enabled, so the decoder does not need to be rebuilt), and the client must update
stride and crop from it, otherwise every subsequent frame will be misaligned.

**Why it is needed**: the frame header only gives buffer dimensions, while the output of Qualcomm Venus is aligned to
128/32 — 1080p actually comes out as 1920×1088, and the last 8 rows are padding. Without `stride` the client cannot
locate the UV plane correctly (the UV plane starts at `stride × slice_height`, not at `width × height`),
and without crop it will treat padding rows as picture content. When the width is not a multiple of 128, reading row by row
using the buffer width will shear the whole picture.

**Version pairing**: the client and the daemon are released as one unit, so the protocol has only one form.
The server peeks at the first 4 bytes and rejects the connection with `status=4` if they are not the magic number —
this removes the compatibility fork, so the frame path contains no format-checking branch at all.

> ⚠️ **No authorization**: the server binds loopback only, but any process on the same device can connect. Because the container and Android share a net namespace, loopback **does not constitute an isolation boundary**. Do not use the current version in a multi-tenant or untrusted-app environment.

## Building

### Requirements

- Android NDK r27c or newer
- A build environment that supports the ARM64 architecture
- Android API Level 21+ (for MediaCodec NDK support)

### Build steps

1. **Set up the NDK environment**
   ```bash
   # Download Android NDK r27c
   # https://developer.android.com/ndk/downloads
   
   export NDK=/path/to/android-ndk-r27c
   export TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
   export TARGET=aarch64-linux-android
   export API=21
   ```

2. **Build decode-daemon**
   ```bash
   cd src
   
   # Cross-compile
   $TOOLCHAIN/bin/${TARGET}${API}-clang \
       -O2 -Wall -Wextra \
       -o decode-daemon \
       decode-daemon.c \
       -lmediandk -llog -landroid
   ```

3. **Build flags explained**
   - `-lmediandk`: the MediaCodec NDK library, which provides both the `AMediaCodec_*` and the `AMediaFormat_*` symbols
   - `-llog`: the Android logging library
   - `-landroid`: the Android base library

### Quick build script

The repository ships `build.sh`, which probes the NDK path automatically and reports clear errors:

```bash
./build.sh                          # probe the NDK automatically
NDK=/path/to/android-ndk ./build.sh  # specify it explicitly
API=29 ./build.sh                    # override the target API level
```

The output is `build/decode-daemon` (an aarch64 PIE executable).

## Deployment: supervised by the DroidSpace platform

> **Change note**: the early plan was to implement start-on-boot with a KSU/Magisk module, and the
> `magisk-module/` directory is a product of that approach. **That approach has been abandoned** — starting,
> restarting, supervising and log collection for the daemon are all handled by the DroidSpace platform,
> and this project no longer ships its own process management.
> `magisk-module/` is kept for reference, is no longer maintained, and is not a delivery form.

### The role of the daemon

`decode-daemon` is designed as a **foreground process** suitable for being supervised:

- Runs in the foreground and does not daemonize itself (the supervisor needs to be able to watch the child process directly)
- Logs go to stderr and are collected by the supervisor
- `SIGTERM` shuts it down gracefully
- The liveness check **uses the listening endpoint rather than the PID** (the PID changes repeatedly; this is a trap we fell into)

### Deployment steps (development and testing)

```bash
# 1) Build
./build.sh                     # produces build/decode-daemon

# 2) Push to the device
adb push build/decode-daemon /data/local/tmp/
adb shell chmod 755 /data/local/tmp/decode-daemon

# 3) Start it (listening on 127.0.0.1:20003)
adb shell su -c 'nohup /data/local/tmp/decode-daemon 20003 >/data/local/tmp/dd.log 2>&1 &'

# 4) Confirm it is listening — check the port, not the PID
adb shell 'timeout 3 sh -c "echo > /dev/tcp/127.0.0.1/20003" && echo 在听'
```

⚠️ **The adb port changes.** The port number may differ after the device reconnects, so building and pushing must be
done in a single command; otherwise it is easy to end up with "I changed the code but the device still has the old
binary" — a protocol mismatch between old and new shows up as `帧头数值不合理`, which is very easily
misdiagnosed as a protocol defect.

### Production deployment

Handled by DroidSpace: where the binary is placed, when it is started (the media service must be ready),
crash restart, log collection and health checks. **The integration contract is documented in
[`doc/platform-integration-contract.md`](doc/platform-integration-contract.md)**
— that document lists the three things the platform must provide (a bind mount directory, starting with the correct
SELinux domain, and pass-through of `/dev/dri/renderD128`), the measured evidence and verification command for each,
and the one current blocker (`droidspacesd` is missing an allow rule for accessing Codec2).

## Testing

During development and testing you can simply push the binary and start it by hand (see the "Deployment" section above). Production deployment is supervised by the DroidSpace platform.

### 1. Build and push the daemon

```bash
./build.sh
adb push build/decode-daemon /data/local/tmp/decode-daemon
adb shell "su -c 'chmod 755 /data/local/tmp/decode-daemon'"
```

### 2. Start the daemon on the Android side

```bash
# Use setsid rather than nohup: under su -c, nohup often fails to start, and disown is unavailable
adb shell "su -c 'cd /data/local/tmp && (setsid ./decode-daemon 20003 </dev/null >decode-daemon.log 2>&1 &)'"

# Confirm it is listening
adb shell "su -c 'ps -A | grep decode-daemon'"
adb shell "su -c 'cat /data/local/tmp/decode-daemon.log'"   # should print listening on 20003
```

> **Convenience tip**: Android's `/data/local/tmp` and the container's `/tmp` are the same directory
> (a bind mount on the same f2fs device). So inside the container you can just run
> `tail /tmp/decode-daemon.log` to read the daemon log without `adb pull`,
> and you do not need `scp` or two copies of a test bitstream.
> But **do not use it to carry frame data** — it is disk-backed and 65% slower than memfd;
> see `doc/verified-platform-facts.md` for details.

### 3. Run the test client inside the container

Put the test video **inside the container** (the client is responsible for reading the file; the daemon only receives NALUs over the network):

```bash
python3 tools/test_decode.py 20003 /path/to/test.h264
```

### 4. Measured output

```
Connected to port 20003
Sent 30 NALUs
Frame 1: 1920x1088 3133440 bytes
Frame 2: 1920x1088 3133440 bytes
...
RESULT: 20 frames decoded from /root/decode-test/test1080.h264
```

Two things here are easy to misread:

- **The output is 1920x1088, not 1920x1080.** The height of the Qualcomm Venus decoder is aligned to 16, so each NV12 frame is `1920*1088*1.5 = 3133440` bytes. The client must trust the returned w/h and crop to the actual display size.
- **The number of NALUs sent does not equal the number of frames received.** The decoder has queuing and reordering latency, and SPS/PPS produce no frames; after sending everything the client must `shutdown(SHUT_WR)` to trigger a flush in order to collect all remaining frames.

See [Verified platform facts](doc/verified-platform-facts.md) for details.

### 5. Manual testing

```bash
# Start the daemon on the Android device (for development and testing; production is supervised by DroidSpace)
adb shell
su
./decode-daemon 20003

# Test the connection from inside the container
nc -zv 127.0.0.1 20003
```

The daemon's command-line options:

```
Usage: decode-daemon [port] [--sock path] [-v|-q]
  port          TCP port to listen on (default 20003, binds 127.0.0.1 only)
  --sock path   listen on a Unix socket at this path instead (recommended)
                if a directory is given, decode.sock is created inside it
  -v            per-frame debug logging
  -q            errors only
```

**Recommended usage (Unix socket, supported by both container kinds)**:

```bash
# Host side: note that the droidspacesd domain is required, otherwise bind() returns EACCES
adb shell su -c 'runcon u:r:droidspacesd:s0 \
  /data/local/tmp/decode-daemon --sock /data/local/tmp/dmd'
# On success it prints:
#   --sock 是目录，实际监听 /data/local/tmp/dmd/decode.sock
#   listening on /data/local/tmp/dmd/decode.sock

# The platform bind mounts that directory into the container (host /data/local/tmp/dmd → container /run/dmd)
# Container side:
DMD_ENDPOINT=unix:/run/dmd/decode.sock ffmpeg -hwaccel vaapi ...
# Or leave DMD_ENDPOINT unset and the driver will probe /run/dmd/decode.sock automatically
```

⚠️ **Pass a directory, not an individual socket file**: `bind()` can only create a new inode, while a bind mount
binds an **inode, not a path** — when a single file is mounted, the container side gets `ECONNREFUSED` as soon as
the daemon restarts. When the directory is mounted its inode stays stable and the daemon can restart as often as it likes.

⚠️ **This path currently does not work under Enforcing**: the `droidspacesd` domain can bind but is not allowed to access
Codec2, and will SIGABRT in `CCodec::allocate`. See "Known issues".

**Fallback usage (TCP, host-mode containers only)**:

```bash
adb shell su -c '/data/local/tmp/decode-daemon 20003'
```

The default level prints only connection and session statistics. When investigating decode problems, use `-v` to see
per-frame information (per-frame logging carries considerable sys overhead, so always stay on the default or `-q` when benchmarking).

## Known issues

### 0. The Unix socket transport is unusable under SELinux Enforcing (currently the biggest blocker)

Each of the two existing launch identities **has only half of the permissions**:

| Launch identity | Can `bind()` a socket | Can use MediaCodec |
|---|---|---|
| `su` (`u:r:ksu:s0`) | ✗ `EACCES` everywhere | ✓ |
| `runcon u:r:droidspacesd:s0` | ✓ | ✗ SIGABRT |

Under `droidspacesd` it crashes in `CCodec::allocate`; the top of the tombstone stack is
`Codec2Client::GetServiceNames` and it reports `Hardware service manager is not running`.

Three control experiments confirm this has **nothing** to do with the transport:

| Identity + transport | Result |
|---|---|
| `ksu` + TCP | decodes normally (but that domain may not bind) |
| `droidspacesd` + TCP | **the same SIGABRT** ← proves it is unrelated to the Unix socket |
| `droidspacesd` + Unix socket + **SELinux permissive** | **decodes normally** |

So **the transport itself is correct** (under permissive, twelve streams compared byte for byte were 12/12 identical),
and all that is missing is one allow rule: allow `droidspacesd` to access hwservicemanager and
the Codec2 HAL.

Alternatives already ruled out: root does not help (both domains are already uid 0, and SELinux is MAC
and does not look at uid); DroidSpaces' `enable_hw_access=1` does not help (it only passes through `/dev` nodes and
does not change the domain); `untrusted_app` is a dead end (it cannot execute a binary labelled `shell_data_file`,
and `chcon` is denied); none of the other 11 domains can satisfy both "can be switched into" and
"can bind"; `selinux_permissive=1` works, but that switches the host's SELinux to permissive as a whole
and cannot be a delivery form.

**Until the rule is in place the driver falls back to TCP automatically; the behaviour matches v0.2.0, with no regression.**
See [`doc/platform-integration-contract.md`](doc/platform-integration-contract.md) §2.2 for details.

### 0b. ~~A single connection drops once memfd zero-copy is enabled~~ (a v0.3.0 symptom, now fixed)

When SHM was first enabled on the driver side in v0.3.0, measured on device: `xfer=1`, the 4-slot memfd mapping and
the `帧回传=SHM` handshake all succeed, and then that connection drops (`Broken pipe` / `Connection reset by peer`),
with only 25 frames collected out of 118 input units. But **it does not kill the daemon** — there is no new tombstone,
the socket keeps `accept`ing, and a re-test afterwards on the non-SHM path was byte-for-byte identical, so at the time
this was judged an error-handling problem on that connection rather than a process-level crash.

**Fixed on 2026-08-26 and now enabled by default**: in a real environment (the driver `dlopen`ed into ffmpeg over
`/run/dmd/decode.sock`) the decode result matches the inline path (150/150 frames) and daemon CPU drops by about 19%.

> ⚠️ Two lessons worth keeping:
> 1. To distinguish this from "Known issue 0": that one is an SELinux domain that makes **any** decoding fail,
>    regardless of the transport; this one was specific to the SHM frame delivery path. We once mistook the two for
>    the same thing.
> 2. **Before judging whether a capability is working, confirm its switch is actually on.** This project got burned
>    here: the docs once claimed SHM gave +17% throughput based on standalone-test-program numbers, while `want_shm`
>    was hard-coded to 0 on the driver side and a real consumer process had never executed that path.

Also, the memfd handover for zero-copy goes over **a separate abstract socket**, which belongs to a net namespace,
so it **only works in a host-mode container, and a NAT-mode container will necessarily be downgraded** (this still holds).

### 1. The shared memory pool does not support resolutions above the limit
Slots are already reserved at the adaptive-playback limit (≥1920×1088), which covers common
in-stream resolution changes. But if the resolution grows beyond that limit (for example 4K appears while
1080p was declared), the only remaining option is to report an error and end the session — rebuilding the pool
would mean going through memfd creation and the SCM_RIGHTS handover again, which requires a new class of control
message in the protocol, and that has not been done.
Workaround: declare the maximum expected resolution in the handshake, or switch to TCP mode (which has no such limit).

### 2. Resolution negotiation
Done: the handshake declares the initial dimensions, the server returns stride / slice_height / the display crop region,
and `adaptive-playback` is enabled — when the resolution changes mid-stream the format block is sent again and
the decoder does not need to be rebuilt. A concatenated 720p→480p stream was verified on device to be split correctly (60 frames of 1280×720 + 60 frames of 640×480).

Still not covered:
- when the resolution **increases** beyond the `MAX_WIDTH`/`MAX_HEIGHT` declared in the handshake (the larger of the declared value and 1080p),
  the decoder still needs an internal reconfiguration, and that path is unverified
- changes in frame rate, colour space and bit depth are not handled (only dimensions and crop are tracked)

### 3. Codec coverage
- **All four of H.264 / HEVC / VP9 / VP8 have been verified end to end on a real device.**
  For VP9 / VP8 the decoded frame count **matches the source stream exactly**; that round for
  H.264 / HEVC did not record the source frame count, so strictly speaking all we have is
  "150 frames decoded, no errors", and **frame count matching cannot be claimed**:

  | Codec | Resolution | Source frames | Decoded frames | Match decidable |
  |------|--------|----------|----------|---------------|
  | H.264 | 1080p | not recorded | 150 | ✗ no baseline |
  | HEVC | 720p | not recorded | 150 | ✗ no baseline |
  | VP9 | 720p | 120 | 120 | ✓ |
  | VP8 | 720p | 120 | 120 | ✓ |

  > The correctness of H.264 / HEVC is backed by another, stronger piece of evidence: **twelve streams
  > are byte-for-byte identical to the software decode baseline** (including the 3000-frame and
  > 1500-frame long streams). A byte-for-byte comparison is far stricter than counting frames, so what
  > is missing here is only the completeness of this table, not any doubt about the capability.

- MPEG2 is supported by the hardware but is not listed in the protocol (no actual need)
- Not covered: VP9 10-bit, HEVC Main10, H.264 High 10/4:2:2 and other high bit depth or non-4:2:0 sampling formats

### 4. The concurrency limit is below what the hardware can do
- Multiple concurrent clients are supported: one session thread per connection, each holding its own MediaCodec instance
- 4 simultaneous 1080p decode streams were measured at about 253 fps in total
- The current limit `MAX_CLIENTS` is set to 8 while the hardware supports 16, and the behaviour at the limit has not been measured
- Beyond the limit new connections are simply closed, and the client only sees the connection dropped, with no explicit reason for the rejection

### 5. Frame data still involves one CPU copy (assessed; no plan to remove it)
SHM mode already eliminates TCP's two kernel copies (standalone test program: throughput +17%, daemon CPU −28.6%;
driver-side end to end: daemon CPU −19%), but the copy from the MediaCodec output buffer into shared memory is still
there — that output buffer is allocated by gralloc, so the daemon can only take a CPU pointer via
`AMediaCodec_getOutputBuffer` and `memcpy` it, which is why "zero-copy" covers only the memfd → consumer process leg.

That copy was measured at 0.227 ms per frame (13137 MB/s), which at 194 fps works out to only about
**4.4% of one CPU core**. Removing it would require getting the dmabuf fd of the output buffer,
and the public NDK API has no entry point for that (`AHardwareBuffer` can only `lock` a CPU pointer, and
`sendHandleToUnixSocket` is unusable across containers), so it would have to rely on private `libui`/gralloc symbols,
which would pin it to the C++ ABI of a specific Android version.

Less than a 10% gain at the cost of version fragility, so **this is not being implemented for now**.
For the detailed accounting and the corrected UBWC conclusion, see `doc/performance-and-roadmap.md`.

### 6. TCP communication has no authorization
- Plaintext TCP, no authentication, no encryption
- Binds loopback only (`INADDR_LOOPBACK`), but **any process on the same device (including an ordinary app) can connect and use this decode service**
- Because the container and Android share a net namespace, loopback does not constitute an isolation boundary

### 7. Error handling
- The resource release paths are complete (both configuration and startup failures go through one unified goto cleanup chain)
- A normal client write-end close and an abnormal disconnect can now be distinguished, and session statistics are logged correctly
- What is still missing is retry and degradation: when decoder creation fails the session simply ends, with no attempt at an alternative decoder

### 8. profile / level are not negotiated
- The handshake can declare the codec and the resolution, but profile and level are still inferred by the decoder itself
- It relies on the information carried in the SPS inside the bitstream, which works for the vast majority of streams
- Special profiles (such as High 10 or 4:2:2) are unverified

## Development information

- **Source based on**: the libdisplay_daemon library of the anland project
- **Tech stack**: Android NDK (C), Python test scripts
- **Transport**: path-based Unix socket (recommended, supported by both container kinds) or TCP socket (IPv4 loopback, host-mode containers only)
- **Target platform**: Android ARM64 devices

## Related documents

- [Container-side client](client/README.md) - decode-client build, usage and implementation notes
- [Verified platform facts](doc/verified-platform-facts.md) - namespace relationships, protocol behaviour and the hardware capability list, all verified on a real device
- [Performance measurements and roadmap](doc/performance-and-roadmap.md) - benchmark data, bottleneck attribution, zero-copy feasibility and hard constraints
- [Why not use V4L2 directly](doc/why-not-v4l2.md) - the forensic conclusion that `/dev/video32` is measurably unusable inside the container
- [VAAPI proxy architecture research report](doc/vaapi-mediacodec-proxy-research.md) - a detailed implementation plan for the VA-API proxy driver
- [Magisk module documentation](magisk-module/README.md) - ⚠️ an abandoned approach, kept for reference

## License

This project is licensed under the Apache 2.0 license.

## Contributing

Issues and pull requests are welcome. Before contributing code, please make sure that:
1. The code follows the project's style
2. Necessary comments are added
3. Related documentation is updated
4. The tests pass
