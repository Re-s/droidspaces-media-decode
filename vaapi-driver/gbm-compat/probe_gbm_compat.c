/* SPDX-License-Identifier: Apache-2.0
 * Public-GBM allocation/import probe. Requires a render node, no EGL or VA-API.
 */
#define _GNU_SOURCE
#include <gbm.h>
#include <linux/dma-buf.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define API_SYMBOLS(X) \
    X(gbm_create_device) X(gbm_device_destroy) X(gbm_device_get_backend_name) \
    X(gbm_device_is_format_supported) X(gbm_bo_create) \
    X(gbm_bo_create_with_modifiers) X(gbm_bo_create_with_modifiers2) \
    X(gbm_bo_import) X(gbm_bo_destroy) X(gbm_bo_map) X(gbm_bo_unmap) \
    X(gbm_bo_get_width) X(gbm_bo_get_height) X(gbm_bo_get_format) \
    X(gbm_bo_get_stride) X(gbm_bo_get_stride_for_plane) X(gbm_bo_get_offset) \
    X(gbm_bo_get_plane_count) X(gbm_bo_get_modifier) X(gbm_bo_get_fd_for_plane)

static struct {
#define DECLARE(name) __typeof__(&name) name;
    API_SYMBOLS(DECLARE)
#undef DECLARE
} api;

struct layout {
    uint32_t width, height, stride, uv_offset, rows;
    size_t bytes, last_uv;
    int fd[2];
};

static bool load_api(const char *path)
{
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "dlopen(%s): %s\n", path, dlerror());
        return false;
    }
#define LOAD(name) do { \
        void *symbol = dlsym(library, #name); \
        if (!symbol) { fprintf(stderr, "missing %s\n", #name); return false; } \
        _Static_assert(sizeof(api.name) == sizeof(symbol), "function pointer ABI"); \
        memcpy(&api.name, &symbol, sizeof(symbol)); \
    } while (0);
    API_SYMBOLS(LOAD)
#undef LOAD
    /* Keep this handle alive through every delegate device and BO. */
    return true;
}

static void close_layout(struct layout *layout)
{
    for (unsigned i = 0; i < 2; ++i) {
        if (layout->fd[i] >= 0)
            close(layout->fd[i]);
        layout->fd[i] = -1;
    }
}

static bool inspect_nv12(const char *label, struct gbm_bo *bo, uint32_t width,
                          uint32_t height, struct layout *layout)
{
    struct stat first, second;
    memset(layout, 0, sizeof(*layout));
    layout->fd[0] = layout->fd[1] = -1;
    layout->width = width;
    layout->height = height;
    layout->rows = height + height / 2 + (height & 1);
    layout->stride = api.gbm_bo_get_stride(bo);
    uint64_t modifier = api.gbm_bo_get_modifier(bo);
    layout->uv_offset = api.gbm_bo_get_offset(bo, 1);
    if (api.gbm_bo_get_width(bo) != width ||
        api.gbm_bo_get_height(bo) != height ||
        api.gbm_bo_get_format(bo) != GBM_FORMAT_NV12 ||
        api.gbm_bo_get_plane_count(bo) != 2 || modifier != 0 ||
        layout->stride < width + (width & 1) ||
        api.gbm_bo_get_stride_for_plane(bo, 0) != layout->stride ||
        api.gbm_bo_get_stride_for_plane(bo, 1) != layout->stride ||
        api.gbm_bo_get_offset(bo, 0) != 0 ||
        (uint64_t)layout->stride * height != layout->uv_offset) {
        fprintf(stderr, "%s: invalid NV12 metadata\n", label);
        return false;
    }
    uint64_t bytes = (uint64_t)layout->stride * layout->rows;
    if (bytes > SIZE_MAX)
        return false;
    layout->bytes = (size_t)bytes;
    layout->last_uv = (size_t)layout->stride * (layout->rows - 1) +
                       width + (width & 1) - 1;
    for (unsigned i = 0; i < 2; ++i)
        layout->fd[i] = api.gbm_bo_get_fd_for_plane(bo, (int)i);
    if (layout->fd[0] < 0 || layout->fd[1] < 0 ||
        fstat(layout->fd[0], &first) || fstat(layout->fd[1], &second) ||
        first.st_dev != second.st_dev || first.st_ino != second.st_ino) {
        fprintf(stderr, "%s: NV12 planes do not share one object\n", label);
        close_layout(layout);
        return false;
    }
    off_t capacity = lseek(layout->fd[0], 0, SEEK_END);
    if (capacity < 0 || (uint64_t)capacity < bytes) {
        fprintf(stderr, "%s: fd capacity is smaller than the NV12 layout\n", label);
        close_layout(layout);
        return false;
    }
    printf("%s: %ux%u NV12 LINEAR stride=%u planes=2 UVoffset=%u "
           "capacity=%" PRIdMAX "\n", label, width, height, layout->stride,
           layout->uv_offset, (intmax_t)capacity);
    return true;
}

static int sync_fd(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = {.flags = flags};
    int result;
    do { result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync); }
    while (result < 0 && errno == EINTR);
    if (result < 0 && (errno == ENOTTY || errno == EINVAL)) {
        /* Allows a CPU-only mock memfd to exercise this same probe. */
        static bool reported;
        if (!reported) {
            fprintf(stderr, "note: fd has no DMA_BUF_IOCTL_SYNC support\n");
            reported = true;
        }
        return 0;
    }
    return result;
}

static bool verify_pixels(const char *label, struct gbm_bo *bo,
                           const struct layout *layout, bool write_pixels)
{
    uint32_t map_stride = 0;
    void *token = NULL;
    unsigned char *pixels = api.gbm_bo_map(bo, 0, 0, layout->width,
        layout->height, GBM_BO_TRANSFER_READ_WRITE, &map_stride, &token);
    if (!pixels || pixels == MAP_FAILED || map_stride != layout->stride) {
        fprintf(stderr, "%s: GBM map failed: %s\n", label, strerror(errno));
        if (pixels && pixels != MAP_FAILED)
            api.gbm_bo_unmap(bo, token);
        return false;
    }
    if (write_pixels) {
        pixels[0] = 0x31;
        pixels[layout->last_uv - 1] = 0x75;
        pixels[layout->last_uv] = 0xa9;
    }
    bool valid = pixels[0] == 0x31 && pixels[layout->last_uv - 1] == 0x75 &&
                   pixels[layout->last_uv] == 0xa9;
    api.gbm_bo_unmap(bo, token);
    if (!valid) {
        fprintf(stderr, "%s: GBM view lost Y/UV data\n", label);
        return false;
    }
    pixels = mmap(NULL, layout->bytes, PROT_READ, MAP_SHARED, layout->fd[1], 0);
    if (pixels == MAP_FAILED) {
        fprintf(stderr, "%s: exported fd mmap failed: %s\n", label,
                strerror(errno));
        return false;
    }
    if (sync_fd(layout->fd[1], DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) < 0) {
        fprintf(stderr, "%s: DMA read sync failed: %s\n", label, strerror(errno));
        munmap(pixels, layout->bytes);
        return false;
    }
    valid = pixels[0] == 0x31 && pixels[layout->last_uv - 1] == 0x75 &&
              pixels[layout->last_uv] == 0xa9;
    if (sync_fd(layout->fd[1], DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) < 0)
        valid = false;
    munmap(pixels, layout->bytes);
    if (!valid)
        fprintf(stderr, "%s: exported fd lost Y/UV data\n", label);
    return valid;
}

static bool probe_nv12(struct gbm_device *device, uint32_t width, uint32_t height,
                        unsigned method)
{
    const uint64_t linear = 0;
    const char *label = method == 0 ? "LINEAR" : method == 1 ? "SCANOUT" :
                        method == 2 ? "modifiers+import" : "modifiers2";
    struct gbm_bo *bo;
    if (method < 2)
        bo = api.gbm_bo_create(device, width, height, GBM_FORMAT_NV12,
            method == 0 ? GBM_BO_USE_LINEAR : GBM_BO_USE_SCANOUT);
    else if (method == 2)
        bo = api.gbm_bo_create_with_modifiers(device, width, height,
            GBM_FORMAT_NV12, &linear, 1);
    else
        bo = api.gbm_bo_create_with_modifiers2(device, width, height,
            GBM_FORMAT_NV12, &linear, 1, GBM_BO_USE_LINEAR);
    if (!bo) {
        fprintf(stderr, "%s: allocation failed: %s\n", label, strerror(errno));
        return false;
    }
    struct layout layout;
    bool valid = inspect_nv12(label, bo, width, height, &layout);
    if (valid)
        valid = verify_pixels(label, bo, &layout, true);
    if (valid && method == 2) {
        struct gbm_import_fd_modifier_data input = {
            .width = width, .height = height, .format = GBM_FORMAT_NV12,
            .num_fds = 2, .fds = {layout.fd[0], layout.fd[1], -1, -1},
            .strides = {(int)layout.stride, (int)layout.stride, 0, 0},
            .offsets = {0, (int)layout.uv_offset, 0, 0}, .modifier = 0,
        };
        struct gbm_bo *imported = api.gbm_bo_import(device,
            GBM_BO_IMPORT_FD_MODIFIER, &input, GBM_BO_USE_LINEAR);
        if (!imported) {
            fprintf(stderr, "%s: import failed: %s\n", label, strerror(errno));
            valid = false;
        } else {
            struct layout imported_layout;
            valid = inspect_nv12("imported", imported, width, height,
                                   &imported_layout);
            if (valid)
                valid = verify_pixels("imported", imported, &imported_layout, false);
            close_layout(&imported_layout);
            api.gbm_bo_destroy(imported);
        }
    }
    close_layout(&layout);
    api.gbm_bo_destroy(bo);
    return valid;
}

static bool probe_rgb(struct gbm_device *device)
{
    struct gbm_bo *bo = api.gbm_bo_create(device, 64, 32, GBM_FORMAT_XRGB8888,
                                           GBM_BO_USE_LINEAR);
    if (!bo) {
        fprintf(stderr, "RGB: allocation failed: %s\n", strerror(errno));
        return false;
    }
    bool valid = api.gbm_bo_get_width(bo) == 64 &&
        api.gbm_bo_get_height(bo) == 32 &&
        api.gbm_bo_get_format(bo) == GBM_FORMAT_XRGB8888 &&
        api.gbm_bo_get_plane_count(bo) == 1;
    api.gbm_bo_destroy(bo);
    if (valid)
        puts("RGB: native XRGB8888 metadata preserved");
    return valid;
}

static uint32_t parse_dimension(const char *text)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    return !errno && *text && !*end && value && value <= 16384
        ? (uint32_t)value : 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 5) {
        fprintf(stderr, "usage: %s /dev/dri/renderD128 /absolute/libgbm.so.1 "
                "[width height]\n", argv[0]);
        return 2;
    }
    uint32_t width = argc == 5 ? parse_dimension(argv[3]) : 640;
    uint32_t height = argc == 5 ? parse_dimension(argv[4]) : 480;
    if (!width || !height || argv[2][0] != '/')
        return 2;
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    if (!load_api(argv[2])) {
        close(fd);
        return 1;
    }
    struct gbm_device *device = api.gbm_create_device(fd);
    if (!device) {
        fprintf(stderr, "gbm_create_device: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("library=%s backend=%s\n", argv[2],
        api.gbm_device_get_backend_name(device));
    printf("NV12 support: LINEAR=%d SCANOUT=%d RENDERING=%d PROTECTED=%d\n",
        api.gbm_device_is_format_supported(device, GBM_FORMAT_NV12, GBM_BO_USE_LINEAR),
        api.gbm_device_is_format_supported(device, GBM_FORMAT_NV12, GBM_BO_USE_SCANOUT),
        api.gbm_device_is_format_supported(device, GBM_FORMAT_NV12, GBM_BO_USE_RENDERING),
        api.gbm_device_is_format_supported(device, GBM_FORMAT_NV12, GBM_BO_USE_PROTECTED));
    unsigned failed = 0;
    failed += !probe_nv12(device, width, height, 0);
    failed += !probe_nv12(device, width, height, 1);
    failed += !probe_nv12(device, 1, 1, 2);
    failed += !probe_nv12(device, width, height, 3);
    failed += !probe_rgb(device);
    api.gbm_device_destroy(device);
    close(fd);
    printf("GBM probe: %s (%u failed checks)\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
