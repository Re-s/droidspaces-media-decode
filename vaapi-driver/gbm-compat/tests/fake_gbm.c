/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "fake_gbm.h"

#include <gbm.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct gbm_device { int fd; };
struct gbm_bo {
    struct gbm_device *device;
    uint32_t width, height, format, flags, stride, uv_offset;
    uint64_t modifier;
    size_t size;
    unsigned char *storage;
    int fd;
    void *user_data;
    void (*destroy_user_data)(struct gbm_bo *, void *);
};
struct gbm_surface {
    struct gbm_device *device;
    uint32_t width, height, format, flags;
    struct gbm_bo *front;
};

/* Tests inspect statistics from their calling thread. Separate storage keeps
 * the mock itself race-free while exercising concurrent shim operations. */
static _Thread_local struct fake_gbm_stats stats;
static int native_nv12;
static int native_nv12_import_only;
static int fail_rgb_create;
static int fail_map;

void fake_gbm_reset(void) { memset(&stats, 0, sizeof(stats)); }
void fake_gbm_set_native_nv12(int enabled) { native_nv12 = enabled; }
void fake_gbm_set_native_nv12_import_only(int enabled)
{ native_nv12_import_only = enabled; }
void fake_gbm_set_fail_rgb_create(int enabled) { fail_rgb_create = enabled; }
void fake_gbm_set_fail_map(int enabled) { fail_map = enabled; }
const struct fake_gbm_stats *fake_gbm_get_stats(void) { return &stats; }

static int supports(uint32_t format)
{
    return format == GBM_FORMAT_XRGB8888 || format == GBM_FORMAT_ARGB8888 ||
           (format == GBM_FORMAT_NV12 && native_nv12);
}

struct gbm_device *gbm_create_device(int fd)
{
    struct gbm_device *device = calloc(1, sizeof(*device));
    if (device) device->fd = fd;
    return device;
}
int gbm_device_get_fd(struct gbm_device *device) { return device->fd; }
const char *gbm_device_get_backend_name(struct gbm_device *device)
{ (void)device; return "fake-gbm"; }
void gbm_device_destroy(struct gbm_device *device) { free(device); }
int gbm_device_is_format_supported(struct gbm_device *device, uint32_t format,
                                   uint32_t flags)
{
    (void)device;
    stats.format_queries++;
    stats.query_format = format;
    stats.query_flags = flags;
    return supports(format);
}
int gbm_device_get_format_modifier_plane_count(struct gbm_device *device,
                                               uint32_t format,
                                               uint64_t modifier)
{
    (void)device;
    stats.modifier_plane_queries++;
    if (!supports(format) || modifier != 0) return 0;
    return format == GBM_FORMAT_NV12 ? 2 : 1;
}

static struct gbm_bo *create_bo(struct gbm_device *device, uint32_t width,
                               uint32_t height, uint32_t format, uint32_t flags,
                               uint64_t modifier, unsigned count)
{
    struct gbm_bo *bo;
    uint64_t row_bytes = (uint64_t)width * (format == GBM_FORMAT_NV12 ? 1 : 4);
    uint64_t stride = (row_bytes + 63) & ~UINT64_C(63);
    uint64_t rows = format == GBM_FORMAT_NV12 ?
                    (uint64_t)height + ((uint64_t)height + 1) / 2 : height;
    stats.create_width = width;
    stats.create_height = height;
    stats.create_format = format;
    stats.create_flags = flags;
    stats.create_modifier = modifier;
    stats.create_modifier_count = count;
    if (!width || !height || !supports(format) || modifier != 0 ||
        (format == GBM_FORMAT_XRGB8888 && fail_rgb_create) ||
        stride > UINT32_MAX || rows > SIZE_MAX / (stride ? stride : 1)) {
        errno = EINVAL;
        return NULL;
    }
    bo = calloc(1, sizeof(*bo));
    if (!bo) return NULL;
    bo->fd = memfd_create("dmd-fake-gbm", MFD_CLOEXEC);
    bo->size = (size_t)(stride * rows);
    if (bo->fd < 0 || ftruncate(bo->fd, (off_t)bo->size)) goto fail;
    bo->storage = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       bo->fd, 0);
    if (bo->storage == MAP_FAILED) { bo->storage = NULL; goto fail; }
    bo->device = device;
    bo->width = width;
    bo->height = height;
    bo->format = format;
    bo->flags = flags;
    bo->stride = (uint32_t)stride;
    bo->uv_offset = (uint32_t)stride * height;
    bo->modifier = modifier;
    stats.last_bo = bo;
    return bo;
fail:
    if (bo->fd >= 0) close(bo->fd);
    free(bo);
    return NULL;
}

struct gbm_bo *gbm_bo_create(struct gbm_device *device, uint32_t width,
                            uint32_t height, uint32_t format, uint32_t flags)
{
    stats.creates++;
    return create_bo(device, width, height, format, flags, 0, 0);
}
struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *device,
                                           uint32_t width, uint32_t height,
                                           uint32_t format,
                                           const uint64_t *modifiers,
                                           const unsigned int count)
{
    stats.creates_with_modifiers++;
    return create_bo(device, width, height, format, 0,
                     modifiers && count ? modifiers[0] : UINT64_MAX, count);
}
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *device,
                                            uint32_t width, uint32_t height,
                                            uint32_t format,
                                            const uint64_t *modifiers,
                                            const unsigned int count,
                                            uint32_t flags)
{
    stats.creates_with_modifiers2++;
    return create_bo(device, width, height, format, flags,
                     modifiers && count ? modifiers[0] : UINT64_MAX, count);
}
struct gbm_bo *gbm_bo_import(struct gbm_device *device, uint32_t type,
                            void *buffer, uint32_t flags)
{
    stats.imports++;
    stats.import_type = type;
    stats.import_flags = flags;
    if (type == GBM_BO_IMPORT_FD_MODIFIER) {
        const struct gbm_import_fd_modifier_data *data = buffer;
        struct gbm_bo *bo;
        struct stat fd_stat;
        uint64_t rows;
        stats.import_width = data->width;
        stats.import_height = data->height;
        stats.import_format = data->format;
        stats.import_num_fds = data->num_fds;
        stats.import_stride = (uint32_t)data->strides[0];
        stats.import_offset = (uint32_t)data->offsets[0];
        stats.import_modifier = data->modifier;
        if (data->format == GBM_FORMAT_NV12)
            stats.native_nv12_imports++;
        rows = data->format == GBM_FORMAT_NV12 ?
               (uint64_t)data->height + ((uint64_t)data->height + 1) / 2 :
               data->height;
        if ((!supports(data->format) &&
             !(data->format == GBM_FORMAT_NV12 && native_nv12_import_only)) ||
            !data->width || !data->height ||
            data->modifier != 0 || !data->num_fds ||
            (data->format != GBM_FORMAT_NV12 && data->num_fds != 1) ||
            data->strides[0] <= 0 || data->offsets[0] != 0 ||
            (uint64_t)data->strides[0] < (uint64_t)data->width *
                (data->format == GBM_FORMAT_NV12 ? 1 : 4) ||
            fstat(data->fds[0], &fd_stat) || fd_stat.st_size <= 0 ||
            (uint64_t)fd_stat.st_size < (uint64_t)data->strides[0] * rows) {
            errno = EINVAL;
            return NULL;
        }
        bo = calloc(1, sizeof(*bo));
        if (!bo) return NULL;
        bo->fd = dup(data->fds[0]);
        bo->size = (size_t)fd_stat.st_size;
        if (bo->fd < 0) { free(bo); return NULL; }
        bo->storage = mmap(NULL, bo->size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, bo->fd, 0);
        if (bo->storage == MAP_FAILED) {
            close(bo->fd); free(bo); return NULL;
        }
        bo->device = device;
        bo->width = data->width;
        bo->height = data->height;
        bo->format = data->format;
        bo->flags = flags;
        bo->stride = (uint32_t)data->strides[0];
        bo->uv_offset = data->format == GBM_FORMAT_NV12 ?
                        (uint32_t)data->offsets[1] : 0;
        bo->modifier = data->modifier;
        stats.last_bo = bo;
        return bo;
    }
    if (type == GBM_BO_IMPORT_FD) {
        const struct gbm_import_fd_data *data = buffer;
        return create_bo(device, data->width, data->height, data->format,
                         flags, 0, 0);
    }
    errno = EINVAL;
    return NULL;
}

void *gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t width,
                 uint32_t height, uint32_t flags, uint32_t *stride,
                 void **map_data)
{
    if (bo->format == GBM_FORMAT_NV12 && native_nv12_import_only && !native_nv12) {
        stats.unsupported_nv12_maps++;
        errno = EIO;
        return NULL;
    }
    stats.maps++;
    stats.map_x = x;
    stats.map_y = y;
    stats.map_width = width;
    stats.map_height = height;
    stats.map_flags = flags;
    if (fail_map || !width || !height || x > bo->width || y > bo->height ||
        width > bo->width - x || height > bo->height - y) {
        errno = EINVAL;
        return NULL;
    }
    if (stride) *stride = bo->stride;
    if (map_data) *map_data = bo->storage;
    stats.map_token = bo->storage;
    return bo->storage + (size_t)y * bo->stride +
           x * (bo->format == GBM_FORMAT_NV12 ? 1 : 4);
}
void gbm_bo_unmap(struct gbm_bo *bo, void *map_data)
{ (void)bo; stats.unmaps++; stats.unmap_token = map_data; }

#define GET_U32(name, field) \
    uint32_t name(struct gbm_bo *bo) { return bo->field; }
GET_U32(gbm_bo_get_width, width)
GET_U32(gbm_bo_get_height, height)
GET_U32(gbm_bo_get_stride, stride)
GET_U32(gbm_bo_get_format, format)
uint32_t gbm_bo_get_bpp(struct gbm_bo *bo)
{ return bo->format == GBM_FORMAT_NV12 ? 12 : 32; }
uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane)
{
    if (plane < 0 || plane >= (bo->format == GBM_FORMAT_NV12 ? 2 : 1)) {
        errno = EINVAL;
        return 0;
    }
    return bo->stride;
}
uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane)
{
    if (plane < 0 || plane >= (bo->format == GBM_FORMAT_NV12 ? 2 : 1)) {
        errno = EINVAL;
        return 0;
    }
    return plane ? bo->uv_offset : 0;
}
struct gbm_device *gbm_bo_get_device(struct gbm_bo *bo) { return bo->device; }
union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo)
{ return (union gbm_bo_handle){ .u64 = (uintptr_t)bo }; }
int gbm_bo_get_fd(struct gbm_bo *bo) { stats.get_fds++; return dup(bo->fd); }
uint64_t gbm_bo_get_modifier(struct gbm_bo *bo) { return bo->modifier; }
int gbm_bo_get_plane_count(struct gbm_bo *bo)
{ return bo->format == GBM_FORMAT_NV12 ? 2 : 1; }
union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane)
{
    if (plane < 0 || plane >= gbm_bo_get_plane_count(bo)) {
        errno = EINVAL;
        return (union gbm_bo_handle){ .u64 = 0 };
    }
    return gbm_bo_get_handle(bo);
}
int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane)
{
    stats.get_plane_fds++;
    if (plane < 0 || plane >= gbm_bo_get_plane_count(bo)) {
        errno = EINVAL;
        return -1;
    }
    return dup(bo->fd);
}
int gbm_bo_write(struct gbm_bo *bo, const void *buffer, size_t count)
{
    stats.writes++;
    if (count > bo->size) { errno = EINVAL; return -1; }
    memcpy(bo->storage, buffer, count);
    return 0;
}
void gbm_bo_set_user_data(struct gbm_bo *bo, void *data,
                         void (*destroy_user_data)(struct gbm_bo *, void *))
{ bo->user_data = data; bo->destroy_user_data = destroy_user_data; }
void *gbm_bo_get_user_data(struct gbm_bo *bo) { return bo->user_data; }
void gbm_bo_destroy(struct gbm_bo *bo)
{
    stats.destroys++;
    if (bo->destroy_user_data) {
        stats.user_callbacks++;
        bo->destroy_user_data(bo, bo->user_data);
    }
    munmap(bo->storage, bo->size);
    close(bo->fd);
    free(bo);
}

struct gbm_surface *gbm_surface_create(struct gbm_device *device,
                                      uint32_t width, uint32_t height,
                                      uint32_t format, uint32_t flags)
{
    struct gbm_surface *surface = calloc(1, sizeof(*surface));
    stats.surfaces_created++;
    if (!surface) return NULL;
    surface->device = device;
    surface->width = width;
    surface->height = height;
    surface->format = format;
    surface->flags = flags;
    return surface;
}
struct gbm_surface *gbm_surface_create_with_modifiers(struct gbm_device *device,
                                                     uint32_t width,
                                                     uint32_t height,
                                                     uint32_t format,
                                                     const uint64_t *modifiers,
                                                     const unsigned int count)
{
    (void)modifiers; (void)count;
    return gbm_surface_create(device, width, height, format, 0);
}
struct gbm_surface *gbm_surface_create_with_modifiers2(struct gbm_device *device,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      uint32_t format,
                                                      const uint64_t *modifiers,
                                                      const unsigned int count,
                                                      uint32_t flags)
{
    (void)modifiers; (void)count;
    return gbm_surface_create(device, width, height, format, flags);
}
struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *surface)
{
    stats.surface_locks++;
    if (!surface->front)
        surface->front = create_bo(surface->device, surface->width,
                                   surface->height, surface->format,
                                   surface->flags, 0, 0);
    return surface->front;
}
void gbm_surface_release_buffer(struct gbm_surface *surface, struct gbm_bo *bo)
{ (void)surface; (void)bo; stats.surface_releases++; }
int gbm_surface_has_free_buffers(struct gbm_surface *surface)
{ (void)surface; return 1; }
void gbm_surface_destroy(struct gbm_surface *surface)
{
    stats.surfaces_destroyed++;
    if (surface->front) gbm_bo_destroy(surface->front);
    free(surface);
}
char *gbm_format_get_name(uint32_t format, struct gbm_format_name_desc *desc)
{
    desc->name[0] = format;
    desc->name[1] = format >> 8;
    desc->name[2] = format >> 16;
    desc->name[3] = format >> 24;
    desc->name[4] = '\0';
    return desc->name;
}
