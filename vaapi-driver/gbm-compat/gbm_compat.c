/* SPDX-License-Identifier: Apache-2.0
 * Optional public-GBM adapter for CPU-written, linear NV12 buffers.
 * All devices and BOs remain genuine objects owned by the system GBM library.
 */
#define _GNU_SOURCE
#include <gbm.h>

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DMD_GBM_REAL_LIBRARY_PATH
#define DMD_GBM_REAL_LIBRARY_PATH "/usr/lib/aarch64-linux-gnu/libgbm.so.1.0.0"
#endif

#define EXPORT __attribute__((visibility("default")))
#define LINEAR_MODIFIER UINT64_C(0)
#define INVALID_MODIFIER UINT64_MAX
#define FALLBACK_FLAGS (GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT | GBM_BO_USE_WRITE)

/* Keep this list equal to the public API in gbm.h; do not link against -lgbm. */
#define GBM_SYMBOLS(X) \
    X(gbm_create_device) \
    X(gbm_device_get_fd) \
    X(gbm_device_get_backend_name) \
    X(gbm_device_is_format_supported) \
    X(gbm_device_get_format_modifier_plane_count) \
    X(gbm_device_destroy) \
    X(gbm_bo_create) \
    X(gbm_bo_create_with_modifiers) \
    X(gbm_bo_create_with_modifiers2) \
    X(gbm_bo_import) \
    X(gbm_bo_map) \
    X(gbm_bo_unmap) \
    X(gbm_bo_get_width) \
    X(gbm_bo_get_height) \
    X(gbm_bo_get_stride) \
    X(gbm_bo_get_stride_for_plane) \
    X(gbm_bo_get_format) \
    X(gbm_bo_get_bpp) \
    X(gbm_bo_get_offset) \
    X(gbm_bo_get_device) \
    X(gbm_bo_get_handle) \
    X(gbm_bo_get_fd) \
    X(gbm_bo_get_modifier) \
    X(gbm_bo_get_plane_count) \
    X(gbm_bo_get_handle_for_plane) \
    X(gbm_bo_get_fd_for_plane) \
    X(gbm_bo_write) \
    X(gbm_bo_set_user_data) \
    X(gbm_bo_get_user_data) \
    X(gbm_bo_destroy) \
    X(gbm_surface_create) \
    X(gbm_surface_create_with_modifiers) \
    X(gbm_surface_create_with_modifiers2) \
    X(gbm_surface_lock_front_buffer) \
    X(gbm_surface_release_buffer) \
    X(gbm_surface_has_free_buffers) \
    X(gbm_surface_destroy) \
    X(gbm_format_get_name)

struct real_api {
#define DECLARE_SYMBOL(name) __typeof__(&name) name;
    GBM_SYMBOLS(DECLARE_SYMBOL)
#undef DECLARE_SYMBOL
};

static struct real_api real;
static pthread_once_t library_once = PTHREAD_ONCE_INIT;
static bool debug_enabled;

#define DEBUG_LOG(...) do { \
    if (debug_enabled) { \
        int debug_saved_errno = errno; \
        flockfile(stderr); \
        fputs("[dmd-gbm] ", stderr); \
        errno = debug_saved_errno; \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        funlockfile(stderr); \
        errno = debug_saved_errno; \
    } \
} while (0)

static void load_library(void)
{
    const char *debug = getenv("DMD_GBM_DEBUG");
    debug_enabled = debug && *debug && strcmp(debug, "0");
    const char *path = getenv("DMD_GBM_REAL_LIBRARY");
    if (!path || !*path)
        path = DMD_GBM_REAL_LIBRARY_PATH;
    if (path[0] != '/') {
        fprintf(stderr, "dmd-gbm: real library path must be absolute\n");
        return;
    }
    /* Explicit dlsym below selects the real GBM entrypoints. Keep normal ELF
     * allocator interposition: DEEPBIND is incompatible with Chrome's allocator
     * shim when Mesa shares allocations with already-loaded dependencies.
     * Keep the library loaded for every real BO's lifetime.
     */
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "dmd-gbm: cannot load %s: %s\n", path, dlerror());
        return;
    }
#define LOAD_SYMBOL(name) do { \
        void *symbol = dlsym(library, #name); \
        _Static_assert(sizeof(real.name) == sizeof(symbol), "function pointer ABI"); \
        memcpy(&real.name, &symbol, sizeof(symbol)); \
    } while (0);
    GBM_SYMBOLS(LOAD_SYMBOL)
#undef LOAD_SYMBOL
    if (real.gbm_create_device == gbm_create_device) {
        fprintf(stderr, "dmd-gbm: refusing to forward to this adapter itself\n");
        memset(&real, 0, sizeof(real));
    }
    DEBUG_LOG("real library=%s", path);
}

#define REQUIRE(name, failure) do { \
    pthread_once(&library_once, load_library); \
    if (!real.name) { errno = ENOSYS; return failure; } \
} while (0)
#define REQUIRE_VOID(name) do { \
    pthread_once(&library_once, load_library); \
    if (!real.name) { errno = ENOSYS; return; } \
} while (0)

struct nv12_bo {
    struct gbm_bo *bo;
    uint32_t width, height, stride, rgb_width, rgb_height;
    size_t bytes;
    void *user_data;
    void (*destroy_user_data)(struct gbm_bo *, void *);
    bool destroying;
    struct nv12_bo *next;
};

static pthread_mutex_t tracking_lock = PTHREAD_MUTEX_INITIALIZER;
static struct nv12_bo *tracked;

static struct nv12_bo *find_bo_locked(struct gbm_bo *bo)
{
    for (struct nv12_bo *entry = tracked; entry; entry = entry->next)
        if (entry->bo == bo)
            return entry;
    return NULL;
}

/* Copy metadata while holding the lock. Never call a delegate or callback
 * under this lock: both can reenter public GBM. As with GBM itself, a caller
 * must not destroy a BO while another thread is operating on that same BO.
 */
static bool get_layout(struct gbm_bo *bo, struct nv12_bo *layout)
{
    pthread_mutex_lock(&tracking_lock);
    struct nv12_bo *entry = find_bo_locked(bo);
    if (entry)
        *layout = *entry;
    pthread_mutex_unlock(&tracking_lock);
    return entry != NULL;
}

static bool fallback_flags(uint32_t flags)
{
    if (flags & ~FALLBACK_FLAGS)
        return false;
    /* SCANOUT permits selection of a linear modifier even without LINEAR in
     * the request. Keep query/create consistent for Chromium's format gate.
     * The consuming compositor/KMS plane must support the exported NV12 view.
     */
    return (flags & (GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT)) != 0;
}

static bool fill_geometry(struct nv12_bo *layout, uint32_t width, uint32_t height)
{
    if (!width || !height) {
        errno = EINVAL;
        return false;
    }
    /* A chroma row contains one complete interleaved pair for every two Y
     * samples, including odd dimensions used by Chromium's 1x1 BO probe.
     */
    uint64_t row_bytes = (uint64_t)width + (width & 1);
    uint64_t rows = (uint64_t)height + height / 2 + (height & 1);
    if (rows > UINT32_MAX || row_bytes > UINT32_MAX) {
        errno = EOVERFLOW;
        return false;
    }
    layout->width = width;
    layout->height = height;
    layout->rgb_width = (uint32_t)(row_bytes / 4 + (row_bytes % 4 != 0));
    layout->rgb_height = (uint32_t)rows;
    return true;
}

static struct gbm_bo *create_fallback(struct gbm_device *device,
                                      uint32_t width, uint32_t height,
                                      uint32_t flags)
{
    if (!fallback_flags(flags)) {
        errno = ENOTSUP;
        return NULL;
    }
    struct nv12_bo *entry = calloc(1, sizeof(*entry));
    if (!entry)
        return NULL;
    if (!fill_geometry(entry, width, height)) {
        free(entry);
        return NULL;
    }
    if (!real.gbm_bo_create || !real.gbm_bo_get_stride ||
        !real.gbm_bo_get_modifier || !real.gbm_bo_get_plane_count ||
        !real.gbm_bo_get_offset || !real.gbm_bo_get_fd ||
        !real.gbm_bo_destroy) {
        free(entry);
        errno = ENOSYS;
        return NULL;
    }
    struct gbm_bo *bo = real.gbm_bo_create(device, entry->rgb_width,
        entry->rgb_height, GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR);
    if (!bo) {
        free(entry);
        return NULL;
    }
    entry->stride = real.gbm_bo_get_stride(bo);
    uint64_t modifier = real.gbm_bo_get_modifier(bo);
    uint64_t uv_offset = (uint64_t)entry->stride * height;
    uint64_t bytes = (uint64_t)entry->stride * entry->rgb_height;
    bool valid = real.gbm_bo_get_plane_count(bo) == 1 &&
        real.gbm_bo_get_offset(bo, 0) == 0 &&
        (modifier == LINEAR_MODIFIER || modifier == INVALID_MODIFIER) &&
        entry->stride >= (uint64_t)entry->rgb_width * 4 &&
        uv_offset <= UINT32_MAX && bytes <= SIZE_MAX;
    int fd = valid ? real.gbm_bo_get_fd(bo) : -1;
    if (fd < 0) {
        valid = false;
    } else {
        off_t capacity = lseek(fd, 0, SEEK_END);
        valid = capacity >= 0 && (uint64_t)capacity >= bytes;
        close(fd);
    }
    if (!valid) {
        real.gbm_bo_destroy(bo);
        free(entry);
        errno = EINVAL;
        return NULL;
    }
    entry->bo = bo;
    entry->bytes = (size_t)bytes;
    pthread_mutex_lock(&tracking_lock);
    entry->next = tracked;
    tracked = entry;
    pthread_mutex_unlock(&tracking_lock);
    return bo;
}

EXPORT struct gbm_device *gbm_create_device(int fd)
{
    REQUIRE(gbm_create_device, NULL);
    return real.gbm_create_device(fd);
}

EXPORT int gbm_device_is_format_supported(struct gbm_device *device,
                                          uint32_t format, uint32_t flags)
{
    REQUIRE(gbm_device_is_format_supported, 0);
    int supported = real.gbm_device_is_format_supported(device, format, flags);
    if (supported || format != GBM_FORMAT_NV12 || !fallback_flags(flags))
        return supported;
    return real.gbm_device_is_format_supported(device, GBM_FORMAT_XRGB8888,
                                                GBM_BO_USE_LINEAR);
}

EXPORT int gbm_device_get_format_modifier_plane_count(struct gbm_device *device,
                                                       uint32_t format,
                                                       uint64_t modifier)
{
    REQUIRE(gbm_device_get_format_modifier_plane_count, -1);
    int count = real.gbm_device_get_format_modifier_plane_count(device, format,
                                                                 modifier);
    if (count > 0 || format != GBM_FORMAT_NV12 || modifier != LINEAR_MODIFIER)
        return count;
    if (real.gbm_device_is_format_supported &&
        real.gbm_device_is_format_supported(device, GBM_FORMAT_XRGB8888,
                                              GBM_BO_USE_LINEAR))
        return 2;
    return count;
}

EXPORT struct gbm_bo *gbm_bo_create(struct gbm_device *device, uint32_t width,
                                    uint32_t height, uint32_t format,
                                    uint32_t flags)
{
    REQUIRE(gbm_bo_create, NULL);
    struct gbm_bo *bo = real.gbm_bo_create(device, width, height, format, flags);
    if (bo || format != GBM_FORMAT_NV12)
        return bo;
    return create_fallback(device, width, height, flags);
}

static bool has_linear_modifier(const uint64_t *modifiers, unsigned count)
{
    if (!modifiers)
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (modifiers[i] == LINEAR_MODIFIER)
            return true;
    return false;
}

EXPORT struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *device,
    uint32_t width, uint32_t height, uint32_t format, const uint64_t *modifiers,
    unsigned count)
{
    REQUIRE(gbm_bo_create_with_modifiers, NULL);
    struct gbm_bo *bo = real.gbm_bo_create_with_modifiers(device, width, height,
                                                          format, modifiers, count);
    if (bo || format != GBM_FORMAT_NV12)
        return bo;
    if (!has_linear_modifier(modifiers, count)) {
        errno = ENOTSUP;
        return NULL;
    }
    return create_fallback(device, width, height, GBM_BO_USE_LINEAR);
}

EXPORT struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *device,
    uint32_t width, uint32_t height, uint32_t format, const uint64_t *modifiers,
    unsigned count, uint32_t flags)
{
    REQUIRE(gbm_bo_create_with_modifiers2, NULL);
    struct gbm_bo *bo = real.gbm_bo_create_with_modifiers2(device, width, height,
        format, modifiers, count, flags);
    if (bo || format != GBM_FORMAT_NV12)
        return bo;
    if (!has_linear_modifier(modifiers, count) || (flags & ~FALLBACK_FLAGS)) {
        errno = ENOTSUP;
        return NULL;
    }
    return create_fallback(device, width, height, flags | GBM_BO_USE_LINEAR);
}

EXPORT struct gbm_bo *gbm_bo_import(struct gbm_device *device, uint32_t type,
                                   void *buffer, uint32_t flags)
{
    REQUIRE(gbm_bo_import, NULL);
    DEBUG_LOG("import type=0x%x usage=0x%x descriptor=%p", type, flags, buffer);
    if (type != GBM_BO_IMPORT_FD_MODIFIER || !buffer) {
        DEBUG_LOG("import delegate: non-modifier type or null descriptor");
        return real.gbm_bo_import(device, type, buffer, flags);
    }
    const struct gbm_import_fd_modifier_data *input = buffer;
    DEBUG_LOG("import format=0x%08x %ux%u planes=%u modifier=0x%016" PRIx64,
        input->format, input->width, input->height, input->num_fds, input->modifier);
    if (debug_enabled)
        for (unsigned i = 0; i < input->num_fds && i < GBM_MAX_PLANES; ++i)
            DEBUG_LOG("import plane%u fd=%d stride=%d offset=%d", i,
                input->fds[i], input->strides[i], input->offsets[i]);
    if (input->format != GBM_FORMAT_NV12 ||
        input->modifier != LINEAR_MODIFIER || (flags & ~FALLBACK_FLAGS) ||
        !(flags & (GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE)) ||
        !real.gbm_device_is_format_supported ||
        real.gbm_device_is_format_supported(device, GBM_FORMAT_NV12,
                                              GBM_BO_USE_LINEAR)) {
        struct gbm_bo *native = real.gbm_bo_import(device, type, buffer, flags);
        DEBUG_LOG("import delegate result=%p errno=%d (native format/usage path)",
            (void *)native, native ? 0 : errno);
        return native;
    }
    /* Some Mesa backends import NV12 even though their format query/allocation
     * rejects it, then crash in dri2_map_image for that native import. On those
     * backends keep explicit CPU usages (LINEAR/WRITE) in the same real XRGB
     * view as our allocations. SCANOUT-only graphical imports retain native
     * NV12, including valid decoder-specific padding and offsets.
     */
    /* A linear, single-object NV12 allocation is raw storage: two FDs may
     * reference its Y and UV views. Only reinterpret a fully validated layout,
     * never unrelated planes, non-linear modifiers, rendering or protection.
     */
    if (input->modifier != LINEAR_MODIFIER || input->num_fds != 2 ||
        (flags & ~FALLBACK_FLAGS) || input->fds[0] < 0 || input->fds[1] < 0 ||
        input->strides[0] <= 0 || input->strides[1] != input->strides[0] ||
        input->offsets[0] != 0 || input->offsets[1] < 0) {
        DEBUG_LOG("import reject: expected two planes, equal positive pitches, "
            "zero Y offset and nonnegative UV offset");
        errno = ENOTSUP;
        return NULL;
    }
    struct nv12_bo *entry = calloc(1, sizeof(*entry));
    if (!entry)
        return NULL;
    if (!fill_geometry(entry, input->width, input->height)) {
        DEBUG_LOG("import reject: geometry errno=%d", errno);
        free(entry);
        return NULL;
    }
    entry->stride = (uint32_t)input->strides[0];
    uint64_t uv_offset = (uint64_t)entry->stride * entry->height;
    uint64_t bytes = (uint64_t)entry->stride * entry->rgb_height;
    struct stat first, second;
    off_t position = lseek(input->fds[0], 0, SEEK_CUR);
    off_t capacity = lseek(input->fds[0], 0, SEEK_END);
    /* Dma-bufs may not support SEEK_CUR; SEEK_SET(0) is the safe fallback.
     * Preserve the shared file position when importing a seekable caller fd.
     */
    if (capacity >= 0 && lseek(input->fds[0], position >= 0 ? position : 0,
                               SEEK_SET) < 0 && position >= 0)
        capacity = -1;
    DEBUG_LOG("import validate rgb=%ux%u stride=%u expectedUV=%" PRIu64
        " actualUV=%d bytes=%" PRIu64 " capacity=%" PRIdMAX " fdPosition=%" PRIdMAX,
        entry->rgb_width, entry->rgb_height, entry->stride, uv_offset,
        input->offsets[1], bytes, (intmax_t)capacity, (intmax_t)position);
    if ((uint64_t)entry->rgb_width * 4 > entry->stride ||
        uv_offset != (uint32_t)input->offsets[1] ||
        uv_offset > INT_MAX || bytes > SIZE_MAX ||
        fstat(input->fds[0], &first) || fstat(input->fds[1], &second) ||
        first.st_dev != second.st_dev || first.st_ino != second.st_ino ||
        capacity < 0 || (uint64_t)capacity < bytes) {
        if (debug_enabled) {
            struct stat a = {0}, b = {0};
            int first_result = fstat(input->fds[0], &a);
            int second_result = fstat(input->fds[1], &b);
            DEBUG_LOG("import reject: geometry/offset/capacity/object validation; "
                "fstat=%d/%d dev=%ju/%ju ino=%ju/%ju", first_result, second_result,
                (uintmax_t)a.st_dev, (uintmax_t)b.st_dev,
                (uintmax_t)a.st_ino, (uintmax_t)b.st_ino);
        }
        free(entry);
        errno = EINVAL;
        return NULL;
    }
    struct gbm_import_fd_modifier_data rgb = {
        .width = entry->rgb_width,
        .height = entry->rgb_height,
        .format = GBM_FORMAT_XRGB8888,
        .num_fds = 1,
        .fds = {input->fds[0], -1, -1, -1},
        .strides = {input->strides[0], 0, 0, 0},
        .offsets = {0, 0, 0, 0},
        .modifier = LINEAR_MODIFIER,
    };
    struct gbm_bo *bo = real.gbm_bo_import(device, GBM_BO_IMPORT_FD_MODIFIER, &rgb,
                                            GBM_BO_USE_LINEAR);
    if (!bo) {
        DEBUG_LOG("import reject: real XRGB import %ux%u stride=%d errno=%d (%s)",
            rgb.width, rgb.height, rgb.strides[0], errno, strerror(errno));
        free(entry);
        return NULL;
    }
    /* Reject backends that changed the imported stride or modifier. */
    if (!real.gbm_bo_get_stride || !real.gbm_bo_get_height ||
        !real.gbm_bo_get_modifier || !real.gbm_bo_get_plane_count ||
        !real.gbm_bo_get_offset || !real.gbm_bo_destroy ||
        real.gbm_bo_get_stride(bo) != entry->stride ||
        real.gbm_bo_get_height(bo) < entry->rgb_height ||
        real.gbm_bo_get_modifier(bo) != LINEAR_MODIFIER ||
        real.gbm_bo_get_plane_count(bo) != 1 ||
        real.gbm_bo_get_offset(bo, 0) != 0) {
        DEBUG_LOG("import reject: real XRGB result layout changed");
        if (real.gbm_bo_destroy)
            real.gbm_bo_destroy(bo);
        free(entry);
        errno = EINVAL;
        return NULL;
    }
    entry->bo = bo;
    entry->bytes = (size_t)bytes;
    pthread_mutex_lock(&tracking_lock);
    entry->next = tracked;
    tracked = entry;
    pthread_mutex_unlock(&tracking_lock);
    DEBUG_LOG("import XRGB-backed NV12 success bo=%p", (void *)bo);
    return bo;
}

#define LAYOUT_GETTER(name, expression) \
    EXPORT uint32_t name(struct gbm_bo *bo) { \
        struct nv12_bo layout; \
        if (get_layout(bo, &layout)) return (expression); \
        REQUIRE(name, 0); return real.name(bo); \
    }
LAYOUT_GETTER(gbm_bo_get_width, layout.width)
LAYOUT_GETTER(gbm_bo_get_height, layout.height)
LAYOUT_GETTER(gbm_bo_get_stride, layout.stride)
LAYOUT_GETTER(gbm_bo_get_format, GBM_FORMAT_NV12)
/* Nominal storage per luma sample: 8-bit Y plus shared interleaved UV. */
LAYOUT_GETTER(gbm_bo_get_bpp, 12)
#undef LAYOUT_GETTER

EXPORT uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane)
{
    struct nv12_bo layout;
    if (get_layout(bo, &layout)) {
        if (plane < 0 || plane > 1) { errno = EINVAL; return 0; }
        return layout.stride;
    }
    REQUIRE(gbm_bo_get_stride_for_plane, 0);
    return real.gbm_bo_get_stride_for_plane(bo, plane);
}

EXPORT uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane)
{
    struct nv12_bo layout;
    if (get_layout(bo, &layout)) {
        if (plane < 0 || plane > 1) { errno = EINVAL; return 0; }
        return plane == 0 ? 0 : layout.stride * layout.height;
    }
    REQUIRE(gbm_bo_get_offset, 0);
    return real.gbm_bo_get_offset(bo, plane);
}

EXPORT uint64_t gbm_bo_get_modifier(struct gbm_bo *bo)
{
    struct nv12_bo layout;
    if (get_layout(bo, &layout))
        return LINEAR_MODIFIER;
    REQUIRE(gbm_bo_get_modifier, INVALID_MODIFIER);
    return real.gbm_bo_get_modifier(bo);
}

EXPORT int gbm_bo_get_plane_count(struct gbm_bo *bo)
{
    struct nv12_bo layout;
    if (get_layout(bo, &layout))
        return 2;
    REQUIRE(gbm_bo_get_plane_count, -1);
    return real.gbm_bo_get_plane_count(bo);
}

EXPORT union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane)
{
    struct nv12_bo layout;
    if (get_layout(bo, &layout)) {
        if (plane < 0 || plane > 1) {
            errno = EINVAL;
            return (union gbm_bo_handle){.u64 = 0};
        }
        REQUIRE(gbm_bo_get_handle, ((union gbm_bo_handle){.u64 = 0}));
        return real.gbm_bo_get_handle(bo);
    }
    REQUIRE(gbm_bo_get_handle_for_plane, ((union gbm_bo_handle){.u64 = 0}));
    return real.gbm_bo_get_handle_for_plane(bo, plane);
}

EXPORT int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane)
{
    struct nv12_bo layout;
    if (get_layout(bo, &layout)) {
        if (plane < 0 || plane > 1) { errno = EINVAL; return -1; }
        REQUIRE(gbm_bo_get_fd, -1);
        return real.gbm_bo_get_fd(bo);
    }
    REQUIRE(gbm_bo_get_fd_for_plane, -1);
    return real.gbm_bo_get_fd_for_plane(bo, plane);
}

static void *map_layout(struct gbm_bo *bo, const struct nv12_bo *layout,
                        uint32_t flags, void **map_data)
{
    REQUIRE(gbm_bo_map, NULL);
    REQUIRE(gbm_bo_unmap, NULL);
    uint32_t stride = 0;
    void *token = NULL;
    void *address = real.gbm_bo_map(bo, 0, 0, layout->rgb_width,
                                    layout->rgb_height, flags, &stride, &token);
    if (!address || address == MAP_FAILED)
        return NULL;
    if (stride != layout->stride) {
        real.gbm_bo_unmap(bo, token);
        errno = EINVAL;
        return NULL;
    }
    *map_data = token;
    return address;
}

EXPORT void *gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height, uint32_t flags,
                        uint32_t *stride, void **map_data)
{
    struct nv12_bo layout;
    if (!get_layout(bo, &layout)) {
        REQUIRE(gbm_bo_map, NULL);
        return real.gbm_bo_map(bo, x, y, width, height, flags, stride, map_data);
    }
    if (!stride || !map_data || !width || !height || x > layout.width ||
        y > layout.height || width > layout.width - x ||
        height > layout.height - y) {
        errno = EINVAL;
        return NULL;
    }
    void *address = map_layout(bo, &layout, flags, map_data);
    if (!address || address == MAP_FAILED)
        return address;
    *stride = layout.stride;
    return (unsigned char *)address + (size_t)y * layout.stride + x;
}

EXPORT void gbm_bo_unmap(struct gbm_bo *bo, void *map_data)
{
    REQUIRE_VOID(gbm_bo_unmap);
    real.gbm_bo_unmap(bo, map_data);
}

EXPORT int gbm_bo_write(struct gbm_bo *bo, const void *buffer, size_t count)
{
    struct nv12_bo layout;
    if (!get_layout(bo, &layout)) {
        REQUIRE(gbm_bo_write, -1);
        return real.gbm_bo_write(bo, buffer, count);
    }
    if ((count && !buffer) || count > layout.bytes) { errno = EINVAL; return -1; }
    void *token = NULL;
    void *address = map_layout(bo, &layout, GBM_BO_TRANSFER_WRITE, &token);
    if (!address || address == MAP_FAILED)
        return -1;
    if (count)
        memcpy(address, buffer, count);
    real.gbm_bo_unmap(bo, token);
    return 0;
}

EXPORT void gbm_bo_set_user_data(struct gbm_bo *bo, void *data,
    void (*destroy_user_data)(struct gbm_bo *, void *))
{
    pthread_mutex_lock(&tracking_lock);
    struct nv12_bo *entry = find_bo_locked(bo);
    if (entry) {
        entry->user_data = data;
        entry->destroy_user_data = destroy_user_data;
    }
    pthread_mutex_unlock(&tracking_lock);
    if (entry)
        return;
    REQUIRE_VOID(gbm_bo_set_user_data);
    real.gbm_bo_set_user_data(bo, data, destroy_user_data);
}

EXPORT void *gbm_bo_get_user_data(struct gbm_bo *bo)
{
    struct nv12_bo layout;
    if (get_layout(bo, &layout))
        return layout.user_data;
    REQUIRE(gbm_bo_get_user_data, NULL);
    return real.gbm_bo_get_user_data(bo);
}

EXPORT void gbm_bo_destroy(struct gbm_bo *bo)
{
    pthread_mutex_lock(&tracking_lock);
    struct nv12_bo *entry = find_bo_locked(bo);
    if (entry && entry->destroying) {
        pthread_mutex_unlock(&tracking_lock);
        return;
    }
    if (entry)
        entry->destroying = true;
    void (*callback)(struct gbm_bo *, void *) =
        entry ? entry->destroy_user_data : NULL;
    void *user_data = entry ? entry->user_data : NULL;
    pthread_mutex_unlock(&tracking_lock);
    /* Preserve the virtual metadata until after the user callback. */
    if (callback)
        callback(bo, user_data);
    if (entry) {
        pthread_mutex_lock(&tracking_lock);
        struct nv12_bo **link = &tracked;
        while (*link && *link != entry)
            link = &(*link)->next;
        if (*link)
            *link = entry->next;
        pthread_mutex_unlock(&tracking_lock);
        free(entry);
    }
    REQUIRE_VOID(gbm_bo_destroy);
    real.gbm_bo_destroy(bo);
}

/* Unchanged public calls deliberately retain the original object pointers. */
#define FORWARD(ret, name, arguments, values, failure) \
    EXPORT ret name arguments { REQUIRE(name, failure); return real.name values; }
#define FORWARD_VOID(name, arguments, values) \
    EXPORT void name arguments { REQUIRE_VOID(name); real.name values; }

FORWARD(int, gbm_device_get_fd, (struct gbm_device *device), (device), -1)
FORWARD(const char *, gbm_device_get_backend_name,
    (struct gbm_device *device), (device), NULL)
FORWARD_VOID(gbm_device_destroy, (struct gbm_device *device), (device))
FORWARD(struct gbm_device *, gbm_bo_get_device, (struct gbm_bo *bo), (bo), NULL)
FORWARD(union gbm_bo_handle, gbm_bo_get_handle, (struct gbm_bo *bo), (bo),
    ((union gbm_bo_handle){.u64 = 0}))
FORWARD(int, gbm_bo_get_fd, (struct gbm_bo *bo), (bo), -1)
FORWARD(struct gbm_surface *, gbm_surface_create,
    (struct gbm_device *device, uint32_t width, uint32_t height, uint32_t format,
     uint32_t flags), (device, width, height, format, flags), NULL)
FORWARD(struct gbm_surface *, gbm_surface_create_with_modifiers,
    (struct gbm_device *device, uint32_t width, uint32_t height, uint32_t format,
     const uint64_t *modifiers, unsigned count),
    (device, width, height, format, modifiers, count), NULL)
FORWARD(struct gbm_surface *, gbm_surface_create_with_modifiers2,
    (struct gbm_device *device, uint32_t width, uint32_t height, uint32_t format,
     const uint64_t *modifiers, unsigned count, uint32_t flags),
    (device, width, height, format, modifiers, count, flags), NULL)
FORWARD(struct gbm_bo *, gbm_surface_lock_front_buffer,
    (struct gbm_surface *surface), (surface), NULL)
FORWARD_VOID(gbm_surface_release_buffer,
    (struct gbm_surface *surface, struct gbm_bo *bo), (surface, bo))
FORWARD(int, gbm_surface_has_free_buffers,
    (struct gbm_surface *surface), (surface), 0)
FORWARD_VOID(gbm_surface_destroy, (struct gbm_surface *surface), (surface))
FORWARD(char *, gbm_format_get_name,
    (uint32_t format, struct gbm_format_name_desc *desc), (format, desc), NULL)
