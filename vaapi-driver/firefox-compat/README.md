# Firefox RDD DMA-heap and KGSL compatibility

This optional preload library supports the tested **native Firefox 160,
GNU/Linux AArch64, GNU libc** environment with the local Mesa KGSL backend and
matching VA driver. The installed library is
`/usr/local/lib/droidspaces-firefox/libdmd-firefox-compat.so`.
It provides system DMA-heap allocation and the KGSL access needed by RDD's EGL
frame import. The earlier heap-only helper could allocate decoder buffers but
did not solve RDD's KGSL sandbox failures.

This is a **custom RDD sandbox policy**, not the unmodified Firefox default.
Mozilla's RDD, content and GPU sandboxes remain enabled, while RDD gains the
specific device capabilities described below. Do not set
`MOZ_DISABLE_RDD_SANDBOX` or other sandbox-disable variables. This library is
not a general Firefox, Mesa, architecture or kernel compatibility layer.

## Activation and process scope

The combined library activates only when all of these conditions hold:

- `DMD_FIREFOX_KGSL=1` is set exactly.
- `argv[1]` is exactly `-contentproc`.
- The final argument is exactly `rdd`.

Finding `rdd` somewhere in the argument list is insufficient. The browser
parent, content processes and forkserver do not preopen these devices. The
cache also records its owning PID; a child created by fork cannot use the
parent's active bridge or heap cache.

The glibc ELF constructors run before RDD enters its sandbox and preopen:

| Device | Cached access | Purpose |
| --- | --- | --- |
| `/dev/dma_heap/system` | `O_RDONLY | O_CLOEXEC` | Existing `DMA_HEAP_IOCTL_ALLOC` allocator |
| `/dev/kgsl-3d0` | `O_RDWR | O_CLOEXEC` | Mesa KGSL context, buffers and commands |
| `/dev/dri/renderD128` | `O_RDWR | O_CLOEXEC` | Mesa's initial KGSL capability probe |

Cached descriptors are at least 3 and are checked as character devices with
`fstat` device/inode identity. KGSL and render preopening must both succeed to
activate the KGSL bridge. There is no lazy device preopening inside the sandbox.

Firefox must use direct exec for RDD, so the installed configuration locks
`dom.ipc.forkserver.enable=false` before startup. Forkserver children do not
rerun ELF constructors. This preference changes child launch behavior; the new
RDD device policy comes from this library. Completely exit existing Firefox
processes before changing the launch configuration.

Use the repository's [Firefox installation and rollback guide](../../doc/firefox-sandbox.md)
and [configuration helper](../../tools/configure-firefox-vaapi.sh). The launcher
sets the explicit gate and process-local preload, preserves existing preload
entries, and checks Firefox 160, AArch64 and GNU libc. Do not install this library
in the global loader configuration. Keep the installed library and directory
root-owned and unwritable by ordinary users, typically mode 0644 and 0755.
Native package installations are the validated scope; Snap/Flatpak and other
Firefox versions require separate validation.

## Custom RDD filter

The bridge intercepts the first successful `seccomp(SECCOMP_SET_MODE_FILTER, ...)`
installation using native TSYNC. It accepts exactly the original flags
`TSYNC` (1) or `TSYNC | SPEC_ALLOW` (5), retaining those flags and the original
filter. It places a new BPF prefix before a byte-for-byte copy of the original
program. Original relative jumps remain unchanged, and unmatched calls enter
the original program with its entry register state restored. Installing a
separate ALLOW filter afterward would not relax an existing seccomp denial and
is not this implementation's approach.

Only a successful installation marks the extension installed. Later filter
installations pass through unchanged. The `prctl` path, including Firefox's
non-TSYNC fallback, is not modified; the compatibility path requires native
TSYNC. Original broker and namespace restrictions are not globally removed.

The prefix grants `ioctl` access only for an exact reserved KGSL FD and one of
these ten complete request values, including direction and structure size:

| Request | KGSL operation |
| --- | --- |
| `0xc0180902` | `GETPROPERTY` |
| `0x400c0907` | `WAITTIMESTAMP_CTXTID` |
| `0x40040914` | `DRAWCTXT_DESTROY` |
| `0xc0080913` | `DRAWCTXT_CREATE` |
| `0xc0080935` | `GPUMEM_FREE_ID` |
| `0xc0300947` | `GPUOBJ_INFO` |
| `0xc0200948` | `GPUOBJ_IMPORT` |
| `0xc0300934` | `GPUMEM_ALLOC_ID` |
| `0xc0200933` | `TIMESTAMP_EVENT` |
| `0xc040094a` | `GPU_COMMAND` |

The reserved render FD receives only the exact `GETPROPERTY` request
`0xc0180902`. This lets Mesa perform its real kernel probe, normally receive
`ENOTTY` on that DRM node, and fall back to KGSL. The helper does not fabricate
successful ioctl results. Both full syscall arguments must match; nonzero high
32-bit words in the FD or request do not receive the new ALLOW exception.
Other FDs, requests and syscalls continue through the original filter, except
for the explicit new restrictions below.

The two reserved KGSL/render **FD slots are protected in the kernel filter**:

- Closing either slot is denied with `EPERM`.
- `dup3` cannot overwrite either slot; AArch64 glibc's `dup2` uses `dup3`.
- Any `close_range` interval containing either slot is denied, including calls
  using `CLOEXEC` or `UNSHARE` flags.
- Every `fcntl(F_SETFD)` operation on either slot is denied, preserving CLOEXEC.

These guards also handle the kernel's truncation of integer syscall arguments.
They prevent raw syscalls from closing a reserved device and reusing its numeric
slot for an unrelated object. Ordinary duplicate FDs are not granted the ioctl
exception themselves. The ioctl bridge verifies their device identity and
routes recognized operations to the reserved FD, keeping KGSL operations on the
same open file description.

The prefix additionally returns `EPERM` for `execve`, `execveat`,
`io_uring_setup`, `io_uring_enter` and `io_uring_register`. A syscall architecture
other than AArch64 receives `SECCOMP_RET_KILL_PROCESS`. These are explicit parts
of the new policy, independent of the original filter. In particular, a filter
survives exec while CLOEXEC would discard its protected FDs.

The process-local `open`, `open64`, `openat`, `openat64`, `ioctl`, `syscall` and
`prctl` symbols normally forward non-target operations through their resolved
`RTLD_NEXT` libc functions. An early symbol-resolution fallback uses the native
syscall ABI. The device-open special case accepts only exact absolute device
paths, the matching read/write access mode, and the supported CLOEXEC/LARGEFILE
flags. Other flags and paths keep the ordinary libc/broker behavior. Non-RDD
processes do not receive the device or seccomp special cases.

## Driver heap interface

```c
extern int dmd_firefox_system_heap_dup(void)
    __attribute__((weak, visibility("default")));
```

The exported API returns a new caller-owned CLOEXEC heap FD, at least 3, or
`-1` with errno. This is a heap FD for `DMA_HEAP_IOCTL_ALLOC`, not a DMA-BUF.
Each decoder session owns and closes its own duplicate. The driver can prefer
this API and retain its ordinary heap/ION opening fallback if it is absent or
fails. Verify a hidden-symbol driver's import is `WEAK DEFAULT UND` and the
helper's API is globally exported with `readelf -Ws`.

The heap cache records PID and character-device identity. It duplicates first,
then checks the duplicate's identity, rejecting a closed or replaced cache
rather than returning an unrelated object. Errors include `ENODEV` when no
cache was initialized, initialization errors, `ECHILD` after fork, `EBADF` for
a closed cache, and `ESTALE` for a replaced object. The immutable cache lives
until process exit. The kernel FD-slot guards described above protect the
KGSL/render descriptors; heap-cache validity is checked by this API.

## Build and tests

No Firefox headers or additional libraries beyond native libc/libdl are needed.
Build the combined production library **on AArch64 with GNU libc**:

```sh
make -C vaapi-driver/firefox-compat compat
```

The output is `build/libdmd-firefox-compat.so`. The default `make` target still
builds the retained heap-only `build/libdmd-firefox-heap.so`; it is insufficient
for this device's RDD EGL path. Never install `libdmd-firefox-heap-test.so`,
which substitutes `/dev/null` for the device in host tests.

Portable heap and BPF-generator checks can run on the build host:

```sh
make -C vaapi-driver/firefox-compat check
make -C vaapi-driver/firefox-compat BUILD=build-ubsan \
  CFLAGS='-O1 -g -fsanitize=undefined -fno-omit-frame-pointer' \
  LDFLAGS='-fsanitize=undefined' check
```

These cover strict role selection, glibc constructors, heap FD ownership and
identity checks, concurrent duplication, failure handling, exact ioctl matching,
FD-slot protection, explicit architecture/exec/io_uring restrictions, original
BPF preservation and 100,000 differential interpreter cases. They do not prove
that a device kernel accepts the ioctls or that Firefox actually decodes video.

Build and explicitly run the native kernel test on AArch64:

```sh
make -C vaapi-driver/firefox-compat kernel-test
vaapi-driver/firefox-compat/build/test_kernel
```

The test installs filters only in isolated child processes, checks real kernel
denials and ordinary FD operations, and leaves its parent unfiltered. It does
not test the full preload/Firefox integration; non-AArch64 execution is skipped.

On the validated Gold UID 1000 installation, the combined integration decoded
**1080p H.264, 300/300 frames using hardware, with no software fallback observed**.
That result is specific to the tested driver, Mesa, Firefox and kernel. It does
not establish every codec/profile, arbitrary video playback, zero-copy behavior
or Firefox hardware encoding. After updates, repeat real playback and verify
actual V4L2 output frames, the active custom RDD policy, and the continued
operation of the remaining sandboxes; advertised codec support alone is not a
playback result.
