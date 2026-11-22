# GBM compatibility tests

These tests use a CPU-only fake implementation of the public GBM API. They do
not need a DRM device, EGL, a compositor, or libgbm at link time.

From this directory:

```sh
cc -std=gnu11 -Wall -Wextra -Werror -O2 -fPIC -shared fake_gbm.c \
  -Wl,-soname,libgbm-fake.so -o libgbm-fake.so
cc -std=gnu11 -Wall -Wextra -Werror -O2 test_gbm_compat.c \
  -ldl -pthread -o test_gbm_compat
./test_gbm_compat /absolute/path/to/compat/libgbm.so.1 "$PWD/libgbm-fake.so"
```

The test sets `DMD_GBM_REAL_LIBRARY` to the fake library's absolute path. It
loads the compatibility library with `dlopen` and resolves its API explicitly,
so the executable cannot silently use the system GBM implementation.

Covered behavior:

- Native RGB, native NV12, devices, imports, format names, and surfaces pass
  through to the fake implementation.
- Unsupported NV12 uses a real XRGB8888 object whose width accommodates
  `ceil(width / 2) * 2` bytes and whose height is `height + ceil(height / 2)`,
  with two exported NV12 planes. Odd sizes, including 1×1, retain valid UV data.
- Both exported plane file descriptors refer to the same backing allocation.
- SCANOUT-only support queries and allocation agree; allocation chooses LINEAR
  and exposes the same validated two-plane layout as an explicit LINEAR request.
- CPU mapping covers the complete backing object, including UV data; partial
  mappings return byte offsets in the virtual NV12 layout.
- Unsupported flags, empty dimensions, unsupported modifiers, allocation
  failures, and map failures do not produce a usable fallback object.
- Exported NV12 imports as one real XRGB8888 plane only when both caller FDs
  identify the same allocation, the modifier is LINEAR, both pitches match,
  offsets are canonical, and the allocation has sufficient capacity. Foreign
  FDs and invalid pitch, offset, or capacity are rejected.
- A backend that rejects NV12 format queries but accepts native NV12 imports
  still uses the safe XRGB import view, avoiding an unusable native NV12 map.
  Native-capable backends and rendering/protected import requests keep their
  original descriptors. Capacity checks preserve the caller's fd position.
- SCANOUT-only NV12 imports retain the native object and decoder Y-plane padding
  even when native allocation queries return false. The test preserves both the
  UV plane offset and caller fd position and confirms no XRGB import or map.
- User data remains readable in its destroy callback. That callback creates and
  destroys another NV12 object to detect a held tracking lock; a 30-second alarm
  bounds deadlocks.
- Four threads each create, query, map, set user data on, and destroy 100 distinct
  NV12 objects. Every destroy callback must run exactly once. Mock statistics are
  thread-local so their counters do not introduce a data race into this test.

The fake uses `memfd_create` plus `mmap` and aligns rows to 64 bytes, exercising
padding and FD export without simulating a GPU. This verifies the library's API
contract; it does not establish that a hardware encoder or graphics driver can
import these allocations.
