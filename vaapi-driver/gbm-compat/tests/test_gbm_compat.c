/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "fake_gbm.h"

#include <gbm.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s (errno=%d)\n", \
                __FILE__, __LINE__, #condition, errno); \
        exit(1); \
    } \
} while (0)

/* Resolve the shim itself, without linking this test against either GBM. */
#define GBM_APIS(X) \
    X(gbm_create_device) X(gbm_device_get_fd) \
    X(gbm_device_get_backend_name) X(gbm_device_is_format_supported) \
    X(gbm_device_get_format_modifier_plane_count) X(gbm_device_destroy) \
    X(gbm_bo_create) X(gbm_bo_create_with_modifiers) \
    X(gbm_bo_create_with_modifiers2) X(gbm_bo_import) X(gbm_bo_map) \
    X(gbm_bo_unmap) X(gbm_bo_get_width) X(gbm_bo_get_height) \
    X(gbm_bo_get_stride) X(gbm_bo_get_stride_for_plane) X(gbm_bo_get_format) \
    X(gbm_bo_get_bpp) X(gbm_bo_get_offset) X(gbm_bo_get_device) \
    X(gbm_bo_get_handle) X(gbm_bo_get_fd) X(gbm_bo_get_modifier) \
    X(gbm_bo_get_plane_count) X(gbm_bo_get_handle_for_plane) \
    X(gbm_bo_get_fd_for_plane) X(gbm_bo_write) X(gbm_bo_set_user_data) \
    X(gbm_bo_get_user_data) X(gbm_bo_destroy) X(gbm_surface_create) \
    X(gbm_surface_create_with_modifiers) X(gbm_surface_create_with_modifiers2) \
    X(gbm_surface_lock_front_buffer) X(gbm_surface_release_buffer) \
    X(gbm_surface_has_free_buffers) X(gbm_surface_destroy) X(gbm_format_get_name)

static struct {
#define DECLARE(name) __typeof__(&name) name;
    GBM_APIS(DECLARE)
#undef DECLARE
} api;

static struct {
    __typeof__(&fake_gbm_reset) reset;
    __typeof__(&fake_gbm_set_native_nv12) native_nv12;
    __typeof__(&fake_gbm_set_native_nv12_import_only) native_nv12_import_only;
    __typeof__(&fake_gbm_set_fail_rgb_create) fail_rgb_create;
    __typeof__(&fake_gbm_set_fail_map) fail_map;
    __typeof__(&fake_gbm_get_stats) stats;
} fake;

static void load_symbol(void *library, const char *name, void *destination,
                        size_t destination_size)
{
    void *symbol;
    const char *error;
    dlerror();
    symbol = dlsym(library, name);
    error = dlerror();
    if (error) fprintf(stderr, "dlsym(%s): %s\n", name, error);
    CHECK(!error && symbol);
    CHECK(destination_size == sizeof(symbol));
    memcpy(destination, &symbol, sizeof(symbol));
}

static struct gbm_bo *fallback_bo(struct gbm_device *device,
                                  uint32_t width, uint32_t height)
{
    struct gbm_bo *bo = api.gbm_bo_create(device, width, height, GBM_FORMAT_NV12,
                                          GBM_BO_USE_LINEAR);
    CHECK(bo);
    CHECK(bo == fake.stats()->last_bo);
    return bo;
}

static void test_support_queries(struct gbm_device *device)
{
    static const uint32_t allowed[] = {
        GBM_BO_USE_LINEAR,
        GBM_BO_USE_SCANOUT,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_WRITE,
        GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT,
        GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE,
        GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT | GBM_BO_USE_WRITE,
    };
    static const uint32_t refused[] = {
        GBM_BO_USE_RENDERING,
        GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING,
        GBM_BO_USE_LINEAR | GBM_BO_USE_PROTECTED,
        GBM_BO_USE_LINEAR | GBM_BO_USE_CURSOR,
        GBM_BO_USE_LINEAR | GBM_BO_USE_FRONT_RENDERING,
        GBM_BO_USE_LINEAR | GBM_BO_FIXED_COMPRESSION_DEFAULT,
    };
    size_t i;
    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
        CHECK(api.gbm_device_is_format_supported(device, GBM_FORMAT_NV12,
                                                 allowed[i]));
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i)
        CHECK(!api.gbm_device_is_format_supported(device, GBM_FORMAT_NV12,
                                                  refused[i]));
    CHECK(!api.gbm_device_is_format_supported(device, GBM_FORMAT_NV21,
                                               GBM_BO_USE_LINEAR));
    CHECK(api.gbm_device_is_format_supported(device, GBM_FORMAT_XRGB8888,
                                              GBM_BO_USE_RENDERING));
    CHECK(api.gbm_device_get_format_modifier_plane_count(device,
                  GBM_FORMAT_NV12, 0) == 2);
    CHECK(api.gbm_device_get_format_modifier_plane_count(device,
                  GBM_FORMAT_NV12, UINT64_C(0x1234)) == 0);
}

static void check_fd_same_object(int fd0, int fd1, off_t minimum_size)
{
    struct stat stat0, stat1;
    CHECK(fd0 >= 0 && fd1 >= 0 && fd0 != fd1);
    CHECK(!fstat(fd0, &stat0));
    CHECK(!fstat(fd1, &stat1));
    CHECK(stat0.st_dev == stat1.st_dev && stat0.st_ino == stat1.st_ino);
    CHECK(stat0.st_size >= minimum_size);
}

static void test_layout_and_fds(struct gbm_device *device)
{
    struct gbm_bo *bo;
    uint32_t stride;
    int fd, fd0, fd1;
    fake.reset();
    bo = fallback_bo(device, 10, 6); /* Width / 4 must round up. */
    CHECK(fake.stats()->create_width == 3);
    CHECK(fake.stats()->create_height == 9);
    CHECK(fake.stats()->create_format == GBM_FORMAT_XRGB8888);
    CHECK(fake.stats()->create_flags & GBM_BO_USE_LINEAR);
    CHECK(!(fake.stats()->create_flags &
            (GBM_BO_USE_RENDERING | GBM_BO_USE_PROTECTED)));
    CHECK(api.gbm_bo_get_width(bo) == 10);
    CHECK(api.gbm_bo_get_height(bo) == 6);
    CHECK(api.gbm_bo_get_format(bo) == GBM_FORMAT_NV12);
    CHECK(api.gbm_bo_get_bpp(bo) == 12);
    CHECK(api.gbm_bo_get_device(bo) == device);
    CHECK(api.gbm_bo_get_plane_count(bo) == 2);
    CHECK(api.gbm_bo_get_modifier(bo) == 0);
    stride = api.gbm_bo_get_stride(bo);
    CHECK(stride == 64 && stride >= 10);
    CHECK(api.gbm_bo_get_stride_for_plane(bo, 0) == stride);
    CHECK(api.gbm_bo_get_stride_for_plane(bo, 1) == stride);
    CHECK(api.gbm_bo_get_stride_for_plane(bo, -1) == 0);
    CHECK(api.gbm_bo_get_stride_for_plane(bo, 2) == 0);
    CHECK(api.gbm_bo_get_offset(bo, 0) == 0);
    CHECK(api.gbm_bo_get_offset(bo, 1) == stride * 6);
    CHECK(api.gbm_bo_get_offset(bo, 2) == 0);
    CHECK(api.gbm_bo_get_handle(bo).u64 == (uintptr_t)bo);
    CHECK(api.gbm_bo_get_handle_for_plane(bo, 0).u64 == (uintptr_t)bo);
    CHECK(api.gbm_bo_get_handle_for_plane(bo, 1).u64 == (uintptr_t)bo);
    CHECK(api.gbm_bo_get_handle_for_plane(bo, 2).u64 == 0);
    fd = api.gbm_bo_get_fd(bo);
    fd0 = api.gbm_bo_get_fd_for_plane(bo, 0);
    fd1 = api.gbm_bo_get_fd_for_plane(bo, 1);
    check_fd_same_object(fd0, fd1, stride * 9);
    check_fd_same_object(fd, fd0, stride * 9);
    CHECK(api.gbm_bo_get_fd_for_plane(bo, -1) == -1);
    CHECK(api.gbm_bo_get_fd_for_plane(bo, 2) == -1);
    close(fd); close(fd0); close(fd1);
    api.gbm_bo_destroy(bo);
    CHECK(fake.stats()->destroys == 1);
}

static void test_map_and_write(struct gbm_device *device)
{
    struct gbm_bo *bo = fallback_bo(device, 10, 6);
    uint32_t stride = 0;
    void *token = NULL;
    unsigned char *pixels;
    unsigned char byte = 0;
    unsigned char content[576];
    unsigned maps;
    int fd;
    fake.reset();
    pixels = api.gbm_bo_map(bo, 0, 0, 10, 6, GBM_BO_TRANSFER_WRITE,
                            &stride, &token);
    CHECK(pixels && stride == 64 && token);
    CHECK(fake.stats()->map_x == 0 && fake.stats()->map_y == 0);
    CHECK(fake.stats()->map_width == 3 && fake.stats()->map_height == 9);
    CHECK(fake.stats()->map_flags == GBM_BO_TRANSFER_WRITE);
    pixels[64 * 8 + 9] = 0xa5; /* Last visible UV byte is mapped too. */
    api.gbm_bo_unmap(bo, token);
    CHECK(fake.stats()->unmaps == 1);
    CHECK(fake.stats()->unmap_token == fake.stats()->map_token);
    fd = api.gbm_bo_get_fd(bo);
    CHECK(fd >= 0);
    CHECK(pread(fd, &byte, 1, 64 * 8 + 9) == 1 && byte == 0xa5);
    close(fd);

    pixels = api.gbm_bo_map(bo, 3, 2, 4, 2, GBM_BO_TRANSFER_READ_WRITE,
                            &stride, &token);
    CHECK(pixels == (unsigned char *)fake.stats()->map_token + 2 * 64 + 3);
    CHECK(fake.stats()->map_x == 0 && fake.stats()->map_y == 0);
    CHECK(fake.stats()->map_width == 3 && fake.stats()->map_height == 9);
    api.gbm_bo_unmap(bo, token);
    CHECK(fake.stats()->unmaps == 2);
    maps = fake.stats()->maps;
    CHECK(!api.gbm_bo_map(bo, 10, 0, 1, 1, GBM_BO_TRANSFER_READ,
                           &stride, &token));
    CHECK(!api.gbm_bo_map(bo, 0, 6, 1, 1, GBM_BO_TRANSFER_READ,
                           &stride, &token));
    CHECK(!api.gbm_bo_map(bo, 0, 0, 11, 6, GBM_BO_TRANSFER_READ,
                           &stride, &token));
    CHECK(!api.gbm_bo_map(bo, 0, 0, 0, 6, GBM_BO_TRANSFER_READ,
                           &stride, &token));
    CHECK(fake.stats()->maps == maps);
    fake.fail_map(1);
    CHECK(!api.gbm_bo_map(bo, 0, 0, 10, 6, GBM_BO_TRANSFER_READ,
                           &stride, &token));
    fake.fail_map(0);
    memset(content, 0x5a, sizeof(content));
    CHECK(!api.gbm_bo_write(bo, content, sizeof(content)));
    fd = api.gbm_bo_get_fd(bo);
    CHECK(fd >= 0);
    CHECK(pread(fd, &byte, 1, sizeof(content) - 1) == 1 && byte == 0x5a);
    close(fd);
    api.gbm_bo_destroy(bo);
}

static void test_refused_allocations(struct gbm_device *device)
{
    static const uint32_t refused[] = {
        0, GBM_BO_USE_WRITE,
        GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING,
        GBM_BO_USE_LINEAR | GBM_BO_USE_PROTECTED,
        GBM_BO_USE_LINEAR | GBM_BO_USE_CURSOR,
        GBM_BO_USE_LINEAR | GBM_BO_USE_FRONT_RENDERING,
        GBM_BO_USE_LINEAR | GBM_BO_FIXED_COMPRESSION_DEFAULT,
    };
    size_t i;
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i)
        CHECK(!api.gbm_bo_create(device, 10, 6, GBM_FORMAT_NV12, refused[i]));
    CHECK(!api.gbm_bo_create(device, 0, 6, GBM_FORMAT_NV12, GBM_BO_USE_LINEAR));
    CHECK(!api.gbm_bo_create(device, 10, 0, GBM_FORMAT_NV12, GBM_BO_USE_LINEAR));
    CHECK(!api.gbm_bo_create(device, 10, 6, GBM_FORMAT_NV21, GBM_BO_USE_LINEAR));
    fake.fail_rgb_create(1);
    CHECK(!api.gbm_bo_create(device, 10, 6, GBM_FORMAT_NV12, GBM_BO_USE_LINEAR));
    fake.fail_rgb_create(0);
}

static void test_scanout_allocations(struct gbm_device *device)
{
    const uint32_t flags[] = {
        GBM_BO_USE_SCANOUT,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_WRITE,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR,
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        CHECK(api.gbm_device_is_format_supported(device, GBM_FORMAT_NV12,
                                                  flags[i]));
        struct gbm_bo *bo = api.gbm_bo_create(device, 10, 6, GBM_FORMAT_NV12,
                                               flags[i]);
        CHECK(bo);
        CHECK(api.gbm_bo_get_format(bo) == GBM_FORMAT_NV12);
        CHECK(api.gbm_bo_get_modifier(bo) == 0);
        CHECK(api.gbm_bo_get_plane_count(bo) == 2);
        CHECK(api.gbm_bo_get_offset(bo, 1) == api.gbm_bo_get_stride(bo) * 6);
        CHECK(fake.stats()->create_flags == GBM_BO_USE_LINEAR);
        api.gbm_bo_destroy(bo);
    }
}

static void test_modifiers(struct gbm_device *device)
{
    const uint64_t linear[] = { 0 };
    const uint64_t nonlinear[] = { UINT64_C(0x100000000000001) };
    const uint64_t mixed[] = { UINT64_C(0x100000000000001), 0 };
    struct gbm_bo *bo;
    fake.reset();
    bo = api.gbm_bo_create_with_modifiers2(device, 10, 6, GBM_FORMAT_NV12,
                                           linear, 1, GBM_BO_USE_LINEAR);
    CHECK(bo && bo == fake.stats()->last_bo);
    CHECK(api.gbm_bo_get_format(bo) == GBM_FORMAT_NV12);
    CHECK(api.gbm_bo_get_modifier(bo) == 0);
    api.gbm_bo_destroy(bo);
    CHECK(!api.gbm_bo_create_with_modifiers2(device, 10, 6, GBM_FORMAT_NV12,
             nonlinear, 1, GBM_BO_USE_LINEAR));
    CHECK(!api.gbm_bo_create_with_modifiers2(device, 10, 6, GBM_FORMAT_NV12,
             linear, 1, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING));
    CHECK(!api.gbm_bo_create_with_modifiers2(device, 10, 6, GBM_FORMAT_NV12,
             NULL, 0, GBM_BO_USE_LINEAR));
    CHECK(!api.gbm_bo_create_with_modifiers2(device, 10, 6, GBM_FORMAT_NV12,
             NULL, 1, GBM_BO_USE_LINEAR));
    /* A list containing LINEAR permits selection of that modifier. */
    bo = api.gbm_bo_create_with_modifiers2(device, 10, 6, GBM_FORMAT_NV12,
                                           mixed, 2, GBM_BO_USE_LINEAR);
    CHECK(bo);
    CHECK(api.gbm_bo_get_modifier(bo) == 0);
    api.gbm_bo_destroy(bo);
}

static void test_odd_dimensions_and_import(struct gbm_device *device)
{
    static const uint32_t dimensions[][2] = { { 1, 1 }, { 3, 4 }, { 4, 3 } };
    size_t index;
    for (index = 0; index < sizeof(dimensions) / sizeof(dimensions[0]); ++index) {
        const uint32_t width = dimensions[index][0];
        const uint32_t height = dimensions[index][1];
        const uint32_t row_bytes = ((width + 1) / 2) * 2;
        const uint32_t rows = height + (height + 1) / 2;
        struct gbm_import_fd_modifier_data import = { 0 };
        struct gbm_bo *bo, *imported;
        uint32_t stride;
        void *token;
        unsigned char *pixels;
        fake.reset();
        bo = fallback_bo(device, width, height);
        CHECK(fake.stats()->create_width == (row_bytes + 3) / 4);
        CHECK(fake.stats()->create_height == rows);
        CHECK(api.gbm_bo_get_width(bo) == width);
        CHECK(api.gbm_bo_get_height(bo) == height);
        stride = api.gbm_bo_get_stride(bo);
        CHECK(stride >= row_bytes);
        CHECK(api.gbm_bo_get_offset(bo, 1) == stride * height);
        pixels = api.gbm_bo_map(bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE,
                                &stride, &token);
        CHECK(pixels && fake.stats()->map_height == rows);
        /* NV12's last UV sample is two bytes wide even for width == 1. */
        pixels[(size_t)stride * (rows - 1) + row_bytes - 2] = 0x53;
        pixels[(size_t)stride * (rows - 1) + row_bytes - 1] = 0xa7;
        api.gbm_bo_unmap(bo, token);
        import.width = width;
        import.height = height;
        import.format = GBM_FORMAT_NV12;
        import.modifier = 0;
        import.num_fds = 2;
        import.fds[0] = api.gbm_bo_get_fd_for_plane(bo, 0);
        import.fds[1] = api.gbm_bo_get_fd_for_plane(bo, 1);
        import.strides[0] = import.strides[1] = (int)stride;
        import.offsets[0] = 0;
        import.offsets[1] = (int)(stride * height);
        check_fd_same_object(import.fds[0], import.fds[1], stride * rows);
        imported = api.gbm_bo_import(device, GBM_BO_IMPORT_FD_MODIFIER,
                                      &import, GBM_BO_USE_LINEAR);
        CHECK(imported && imported == fake.stats()->last_bo);
        CHECK(fake.stats()->import_type == GBM_BO_IMPORT_FD_MODIFIER);
        CHECK(fake.stats()->import_format == GBM_FORMAT_XRGB8888);
        CHECK(fake.stats()->import_width == (row_bytes + 3) / 4);
        CHECK(fake.stats()->import_height == rows);
        CHECK(fake.stats()->import_num_fds == 1);
        CHECK(fake.stats()->import_stride == stride);
        CHECK(fake.stats()->import_offset == 0);
        CHECK(fake.stats()->import_modifier == 0);
        CHECK(api.gbm_bo_get_format(imported) == GBM_FORMAT_NV12);
        CHECK(api.gbm_bo_get_width(imported) == width);
        CHECK(api.gbm_bo_get_height(imported) == height);
        CHECK(api.gbm_bo_get_plane_count(imported) == 2);
        CHECK(api.gbm_bo_get_offset(imported, 1) == stride * height);
        pixels = api.gbm_bo_map(imported, 0, 0, width, height,
                                GBM_BO_TRANSFER_READ, &stride, &token);
        CHECK(pixels);
        CHECK(pixels[(size_t)stride * (rows - 1) + row_bytes - 2] == 0x53);
        CHECK(pixels[(size_t)stride * (rows - 1) + row_bytes - 1] == 0xa7);
        api.gbm_bo_unmap(imported, token);
        api.gbm_bo_destroy(imported);
        /* Import owns duplicates, rather than consuming the caller's FDs. */
        check_fd_same_object(import.fds[0], import.fds[1], stride * rows);
        close(import.fds[0]); close(import.fds[1]);
        api.gbm_bo_destroy(bo);
    }
}

static void test_refused_imports(struct gbm_device *device)
{
    struct gbm_bo *bo = fallback_bo(device, 10, 6);
    struct gbm_bo *foreign = fallback_bo(device, 10, 6);
    struct gbm_bo *imported;
    const uint32_t stride = api.gbm_bo_get_stride(bo);
    struct gbm_import_fd_modifier_data good = { 0 }, bad;
    int foreign_fd = api.gbm_bo_get_fd(foreign);
    good.width = 10;
    good.height = 6;
    good.format = GBM_FORMAT_NV12;
    good.num_fds = 2;
    good.fds[0] = api.gbm_bo_get_fd_for_plane(bo, 0);
    good.fds[1] = api.gbm_bo_get_fd_for_plane(bo, 1);
    good.strides[0] = good.strides[1] = (int)stride;
    good.offsets[0] = 0;
    good.offsets[1] = (int)(stride * 6);
    good.modifier = 0;
#define REJECT_IMPORT(change) do { \
    bad = good; change; \
    CHECK(!api.gbm_bo_import(device, GBM_BO_IMPORT_FD_MODIFIER, &bad, \
                              GBM_BO_USE_LINEAR)); \
} while (0)
    REJECT_IMPORT(bad.fds[1] = foreign_fd);
    REJECT_IMPORT(bad.fds[0] = -1);
    REJECT_IMPORT(bad.num_fds = 1);
    REJECT_IMPORT(bad.num_fds = 3);
    REJECT_IMPORT(bad.strides[1] += 64);
    REJECT_IMPORT(bad.strides[0] = bad.strides[1] = 8);
    REJECT_IMPORT(bad.strides[0] = bad.strides[1] = -64);
    REJECT_IMPORT(bad.offsets[0] = 1);
    REJECT_IMPORT(bad.offsets[1] += 1);
    REJECT_IMPORT(bad.modifier = UINT64_C(0x100000000000001));
    REJECT_IMPORT(bad.width = 0);
    REJECT_IMPORT(bad.height = 0);
    REJECT_IMPORT(bad.height = 8; bad.offsets[1] = (int)(stride * 8));
    REJECT_IMPORT(bad.strides[0] = bad.strides[1] = 128;
                  bad.offsets[1] = 128 * 6);
#undef REJECT_IMPORT
    CHECK(!api.gbm_bo_import(device, GBM_BO_IMPORT_FD_MODIFIER, &good,
              GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING));
    CHECK(!api.gbm_bo_import(device, GBM_BO_IMPORT_FD_MODIFIER, &good,
              GBM_BO_USE_LINEAR | GBM_BO_USE_PROTECTED));

    /* Successful native NV12 imports keep the native two-plane descriptor. */
    fake.native_nv12(1);
    imported = api.gbm_bo_import(device, GBM_BO_IMPORT_FD_MODIFIER, &good,
                                  GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    CHECK(imported);
    CHECK(fake.stats()->import_format == GBM_FORMAT_NV12);
    CHECK(fake.stats()->import_num_fds == 2);
    CHECK(api.gbm_bo_get_format(imported) == GBM_FORMAT_NV12);
    CHECK(api.gbm_bo_get_width(imported) == 10 &&
          api.gbm_bo_get_height(imported) == 6);
    api.gbm_bo_destroy(imported);
    fake.native_nv12(0);
    close(good.fds[0]); close(good.fds[1]); close(foreign_fd);
    api.gbm_bo_destroy(foreign);
    api.gbm_bo_destroy(bo);
}

static void test_import_without_native_allocation(struct gbm_device *device)
{
    /* Model the target Mesa failure: native NV12 import succeeds despite a
     * false format query, but mapping that native import is unusable.
     */
    struct gbm_bo *source = fallback_bo(device, 1, 1);
    uint32_t stride = api.gbm_bo_get_stride(source), mapped_stride;
    struct gbm_import_fd_modifier_data input = {
        .width = 1, .height = 1, .format = GBM_FORMAT_NV12, .num_fds = 2,
        .fds = {-1, -1, -1, -1}, .strides = {(int)stride, (int)stride, 0, 0},
        .offsets = {0, (int)stride, 0, 0}, .modifier = 0,
    };
    input.fds[0] = api.gbm_bo_get_fd_for_plane(source, 0);
    input.fds[1] = api.gbm_bo_get_fd_for_plane(source, 1);
    void *token = NULL;
    unsigned char *pixels = api.gbm_bo_map(source, 0, 0, 1, 1,
        GBM_BO_TRANSFER_WRITE, &mapped_stride, &token);
    CHECK(pixels && mapped_stride == stride);
    pixels[0] = 0x31;
    pixels[stride] = 0x75;
    pixels[stride + 1] = 0xa9;
    api.gbm_bo_unmap(source, token);
    fake.native_nv12_import_only(1);
    fake.reset();
    CHECK(lseek(input.fds[0], 17, SEEK_SET) == 17);
    struct gbm_bo *imported = api.gbm_bo_import(device, GBM_BO_IMPORT_FD_MODIFIER,
                                                 &input, GBM_BO_USE_LINEAR);
    CHECK(imported);
    CHECK(lseek(input.fds[0], 0, SEEK_CUR) == 17);
    CHECK(fake.stats()->imports == 1);
    CHECK(fake.stats()->import_format == GBM_FORMAT_XRGB8888);
    CHECK(fake.stats()->native_nv12_imports == 0);
    pixels = api.gbm_bo_map(imported, 0, 0, 1, 1, GBM_BO_TRANSFER_READ,
                              &mapped_stride, &token);
    CHECK(pixels && mapped_stride == stride);
    CHECK(pixels[0] == 0x31 && pixels[stride] == 0x75 && pixels[stride + 1] == 0xa9);
    CHECK(fake.stats()->unsupported_nv12_maps == 0);
    CHECK(fake.stats()->map_width == 1 && fake.stats()->map_height == 2);
    api.gbm_bo_unmap(imported, token);
    api.gbm_bo_destroy(imported);

    /* A backend that actually advertises native support keeps its NV12 BO. */
    fake.native_nv12(1);
    fake.reset();
    imported = api.gbm_bo_import(device, GBM_BO_IMPORT_FD_MODIFIER, &input,
                                   GBM_BO_USE_LINEAR);
    CHECK(imported && fake.stats()->import_format == GBM_FORMAT_NV12);
    CHECK(fake.stats()->native_nv12_imports == 1);
    api.gbm_bo_destroy(imported);
    fake.native_nv12(0);

    /* Unsupported fallback usages still delegate unchanged, including on a
     * backend that happens to import them despite not advertising allocation.
     */
    const uint32_t usages[] = {GBM_BO_USE_RENDERING, GBM_BO_USE_PROTECTED};
    for (size_t i = 0; i < sizeof(usages) / sizeof(usages[0]); ++i) {
        fake.reset();
        imported = api.gbm_bo_import(device, GBM_BO_IMPORT_FD_MODIFIER, &input,
                                       usages[i]);
        CHECK(imported && fake.stats()->import_format == GBM_FORMAT_NV12);
        CHECK(fake.stats()->import_flags == usages[i]);
        CHECK(fake.stats()->native_nv12_imports == 1);
        api.gbm_bo_destroy(imported);
    }
    fake.native_nv12_import_only(0);
    close(input.fds[0]); close(input.fds[1]);
    api.gbm_bo_destroy(source);
}

static void test_padded_scanout_import(struct gbm_device *device)
{
    /* Decoder frames can have extra Y rows before UV. Graphical imports must
     * preserve this native descriptor rather than impose CPU staging geometry.
     */
    struct gbm_bo *source = fallback_bo(device, 10, 8);
    uint32_t stride = api.gbm_bo_get_stride(source);
    struct gbm_import_fd_modifier_data input = {
        .width = 10, .height = 6, .format = GBM_FORMAT_NV12, .num_fds = 2,
        .fds = {-1, -1, -1, -1}, .strides = {(int)stride, (int)stride, 0, 0},
        .offsets = {0, (int)(stride * 8), 0, 0}, .modifier = 0,
    };
    input.fds[0] = api.gbm_bo_get_fd_for_plane(source, 0);
    input.fds[1] = api.gbm_bo_get_fd_for_plane(source, 1);
    CHECK(lseek(input.fds[0], 17, SEEK_SET) == 17);
    fake.native_nv12_import_only(1); /* Query remains false; import succeeds. */
    fake.reset();
    struct gbm_bo *imported = api.gbm_bo_import(device, GBM_BO_IMPORT_FD_MODIFIER,
                                                 &input, GBM_BO_USE_SCANOUT);
    CHECK(imported);
    CHECK(fake.stats()->imports == 1 && fake.stats()->creates == 0);
    CHECK(fake.stats()->import_format == GBM_FORMAT_NV12);
    CHECK(fake.stats()->import_flags == GBM_BO_USE_SCANOUT);
    CHECK(fake.stats()->import_width == 10 && fake.stats()->import_height == 6);
    CHECK(fake.stats()->native_nv12_imports == 1);
    CHECK(fake.stats()->unsupported_nv12_maps == 0);
    CHECK(api.gbm_bo_get_format(imported) == GBM_FORMAT_NV12);
    CHECK(api.gbm_bo_get_width(imported) == 10 && api.gbm_bo_get_height(imported) == 6);
    CHECK(api.gbm_bo_get_offset(imported, 1) == stride * 8);
    CHECK(lseek(input.fds[0], 0, SEEK_CUR) == 17);
    api.gbm_bo_destroy(imported);
    fake.native_nv12_import_only(0);
    close(input.fds[0]); close(input.fds[1]);
    api.gbm_bo_destroy(source);
}

struct callback_state {
    struct gbm_device *device;
    unsigned calls;
};
static void destroy_callback(struct gbm_bo *bo, void *opaque)
{
    struct callback_state *state = opaque;
    struct gbm_bo *other;
    state->calls++;
    CHECK(api.gbm_bo_get_user_data(bo) == state);
    CHECK(api.gbm_bo_get_width(bo) == 10);
    CHECK(api.gbm_bo_get_height(bo) == 6);
    CHECK(api.gbm_bo_get_format(bo) == GBM_FORMAT_NV12);
    /* A held global tracking lock would deadlock here. */
    other = fallback_bo(state->device, 4, 2);
    api.gbm_bo_destroy(other);
}
static void test_user_data_reentry(struct gbm_device *device)
{
    struct callback_state state = { .device = device };
    struct gbm_bo *bo = fallback_bo(device, 10, 6);
    fake.reset();
    CHECK(!api.gbm_bo_get_user_data(bo));
    api.gbm_bo_set_user_data(bo, &state, destroy_callback);
    CHECK(api.gbm_bo_get_user_data(bo) == &state);
    api.gbm_bo_destroy(bo);
    CHECK(state.calls == 1);
    CHECK(fake.stats()->destroys == 2);
}

static void test_passthrough(struct gbm_device *device)
{
    const uint64_t linear[] = { 0 };
    struct gbm_bo *bo;
    struct gbm_surface *surface;
    struct gbm_format_name_desc name;
    struct gbm_import_fd_data import = {
        .fd = -1, .width = 17, .height = 11, .stride = 128,
        .format = GBM_FORMAT_ARGB8888,
    };
    unsigned char pixels[8] = { 0 };
    uint32_t stride;
    void *token;
    int data;
    fake.reset();
    CHECK(api.gbm_device_get_fd(device) == 42);
    CHECK(!strcmp(api.gbm_device_get_backend_name(device), "fake-gbm"));
    CHECK(!strcmp(api.gbm_format_get_name(GBM_FORMAT_NV12, &name), "NV12"));
    bo = api.gbm_bo_create(device, 17, 11, GBM_FORMAT_ARGB8888,
                            GBM_BO_USE_RENDERING);
    CHECK(bo && bo == fake.stats()->last_bo);
    CHECK(api.gbm_bo_get_width(bo) == 17 && api.gbm_bo_get_height(bo) == 11);
    CHECK(api.gbm_bo_get_format(bo) == GBM_FORMAT_ARGB8888);
    CHECK(api.gbm_bo_get_plane_count(bo) == 1);
    CHECK(api.gbm_bo_get_bpp(bo) == 32);
    CHECK(api.gbm_bo_get_stride(bo) == 128);
    CHECK(api.gbm_bo_map(bo, 2, 3, 4, 5, GBM_BO_TRANSFER_READ,
                           &stride, &token));
    CHECK(fake.stats()->map_x == 2 && fake.stats()->map_y == 3);
    CHECK(fake.stats()->map_width == 4 && fake.stats()->map_height == 5);
    api.gbm_bo_unmap(bo, token);
    CHECK(!api.gbm_bo_write(bo, pixels, sizeof(pixels)));
    api.gbm_bo_set_user_data(bo, &data, NULL);
    CHECK(api.gbm_bo_get_user_data(bo) == &data);
    api.gbm_bo_destroy(bo);

    bo = api.gbm_bo_create_with_modifiers(device, 17, 11,
                                           GBM_FORMAT_ARGB8888, linear, 1);
    CHECK(bo && api.gbm_bo_get_format(bo) == GBM_FORMAT_ARGB8888);
    api.gbm_bo_destroy(bo);
    bo = api.gbm_bo_create_with_modifiers2(device, 17, 11,
                 GBM_FORMAT_ARGB8888, linear, 1, GBM_BO_USE_RENDERING);
    CHECK(bo && fake.stats()->create_flags == GBM_BO_USE_RENDERING);
    api.gbm_bo_destroy(bo);
    bo = api.gbm_bo_import(device, GBM_BO_IMPORT_FD, &import,
                            GBM_BO_USE_RENDERING);
    CHECK(bo && fake.stats()->imports == 1);
    CHECK(fake.stats()->import_type == GBM_BO_IMPORT_FD);
    CHECK(fake.stats()->import_flags == GBM_BO_USE_RENDERING);
    CHECK(api.gbm_bo_get_width(bo) == import.width);
    api.gbm_bo_destroy(bo);
    surface = api.gbm_surface_create(device, 17, 11, GBM_FORMAT_ARGB8888,
                                      GBM_BO_USE_RENDERING);
    CHECK(surface && api.gbm_surface_has_free_buffers(surface));
    bo = api.gbm_surface_lock_front_buffer(surface);
    CHECK(bo && api.gbm_bo_get_format(bo) == GBM_FORMAT_ARGB8888);
    api.gbm_surface_release_buffer(surface, bo);
    api.gbm_surface_destroy(surface);
    surface = api.gbm_surface_create_with_modifiers(device, 17, 11,
                  GBM_FORMAT_ARGB8888, linear, 1);
    CHECK(surface);
    api.gbm_surface_destroy(surface);
    surface = api.gbm_surface_create_with_modifiers2(device, 17, 11,
                  GBM_FORMAT_ARGB8888, linear, 1, GBM_BO_USE_RENDERING);
    CHECK(surface);
    api.gbm_surface_destroy(surface);
    CHECK(fake.stats()->surfaces_created == 3);
    CHECK(fake.stats()->surfaces_destroyed == 3);
    CHECK(fake.stats()->surface_locks == 1 && fake.stats()->surface_releases == 1);

    /* Native NV12 is never repackaged or subjected to fallback restrictions. */
    fake.native_nv12(1);
    bo = api.gbm_bo_create(device, 18, 12, GBM_FORMAT_NV12,
                            GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    CHECK(bo && bo == fake.stats()->last_bo);
    CHECK(fake.stats()->create_width == 18 && fake.stats()->create_height == 12);
    CHECK(fake.stats()->create_format == GBM_FORMAT_NV12);
    CHECK(api.gbm_bo_get_width(bo) == 18 && api.gbm_bo_get_height(bo) == 12);
    CHECK(api.gbm_bo_get_format(bo) == GBM_FORMAT_NV12);
    CHECK(api.gbm_bo_get_plane_count(bo) == 2);
    api.gbm_bo_destroy(bo);
    fake.native_nv12(0);
}

enum { THREAD_COUNT = 4, THREAD_ITERATIONS = 100 };
struct thread_state {
    struct gbm_device *device;
    pthread_barrier_t *start;
    unsigned index;
    unsigned callbacks;
};

static void thread_destroy_callback(struct gbm_bo *bo, void *opaque)
{
    struct thread_state *state = opaque;
    CHECK(api.gbm_bo_get_user_data(bo) == state);
    CHECK(api.gbm_bo_get_format(bo) == GBM_FORMAT_NV12);
    CHECK(api.gbm_bo_get_plane_count(bo) == 2);
    state->callbacks++;
}

static void *tracking_worker(void *opaque)
{
    struct thread_state *state = opaque;
    unsigned iteration;
    int barrier_result = pthread_barrier_wait(state->start);
    CHECK(!barrier_result || barrier_result == PTHREAD_BARRIER_SERIAL_THREAD);
    for (iteration = 0; iteration < THREAD_ITERATIONS; ++iteration) {
        const uint32_t width = 1 + (iteration + state->index) % 33;
        const uint32_t height = 1 + (iteration * 3 + state->index) % 25;
        const uint32_t uv_row_bytes = ((width + 1) / 2) * 2;
        const uint32_t rows = height + (height + 1) / 2;
        struct gbm_bo *bo = api.gbm_bo_create(state->device, width, height,
                       GBM_FORMAT_NV12, GBM_BO_USE_LINEAR);
        uint32_t stride;
        void *token;
        unsigned char *pixels;
        CHECK(bo);
        CHECK(api.gbm_bo_get_width(bo) == width);
        CHECK(api.gbm_bo_get_height(bo) == height);
        CHECK(api.gbm_bo_get_format(bo) == GBM_FORMAT_NV12);
        CHECK(api.gbm_bo_get_bpp(bo) == 12);
        CHECK(api.gbm_bo_get_device(bo) == state->device);
        CHECK(api.gbm_bo_get_plane_count(bo) == 2);
        stride = api.gbm_bo_get_stride(bo);
        CHECK(stride >= uv_row_bytes);
        CHECK(api.gbm_bo_get_stride_for_plane(bo, 1) == stride);
        CHECK(api.gbm_bo_get_offset(bo, 1) == stride * height);
        pixels = api.gbm_bo_map(bo, 0, 0, width, height,
                                 GBM_BO_TRANSFER_READ_WRITE, &stride, &token);
        CHECK(pixels && token);
        pixels[0] = (unsigned char)iteration;
        pixels[(size_t)stride * (rows - 1) + uv_row_bytes - 1] =
            (unsigned char)state->index;
        CHECK(pixels[0] == (unsigned char)iteration);
        api.gbm_bo_unmap(bo, token);
        api.gbm_bo_set_user_data(bo, state, thread_destroy_callback);
        CHECK(api.gbm_bo_get_user_data(bo) == state);
        api.gbm_bo_destroy(bo);
    }
    return NULL;
}

static void test_concurrent_tracking(struct gbm_device *device)
{
    pthread_t threads[THREAD_COUNT];
    pthread_barrier_t start;
    struct thread_state states[THREAD_COUNT] = { 0 };
    unsigned index, callbacks = 0;
    /* Control values stay constant for the complete concurrent phase. The fake
     * uses per-thread statistics; workers never read global test counters. */
    fake.native_nv12(0);
    fake.fail_rgb_create(0);
    fake.fail_map(0);
    CHECK(!pthread_barrier_init(&start, NULL, THREAD_COUNT));
    for (index = 0; index < THREAD_COUNT; ++index) {
        states[index].device = device;
        states[index].start = &start;
        states[index].index = index;
        CHECK(!pthread_create(&threads[index], NULL, tracking_worker,
                                &states[index]));
    }
    for (index = 0; index < THREAD_COUNT; ++index) {
        CHECK(!pthread_join(threads[index], NULL));
        CHECK(states[index].callbacks == THREAD_ITERATIONS);
        callbacks += states[index].callbacks;
    }
    CHECK(!pthread_barrier_destroy(&start));
    CHECK(callbacks == THREAD_COUNT * THREAD_ITERATIONS);
}

int main(int argc, char **argv)
{
    void *real_library, *compat_library;
    struct gbm_device *device;
    if (argc != 3) {
        fprintf(stderr, "usage: %s /absolute/libgbm-compat.so /absolute/fake-gbm.so\n",
                argv[0]);
        return 2;
    }
    alarm(30); /* In particular, bound the callback reentry deadlock test. */
    CHECK(argv[1][0] == '/' && argv[2][0] == '/');
    CHECK(!setenv("DMD_GBM_REAL_LIBRARY", argv[2], 1));
    real_library = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    if (!real_library) fprintf(stderr, "fake GBM: %s\n", dlerror());
    CHECK(real_library);
#define LOAD_FAKE(field, name) \
    load_symbol(real_library, #name, &fake.field, sizeof(fake.field))
    LOAD_FAKE(reset, fake_gbm_reset);
    LOAD_FAKE(native_nv12, fake_gbm_set_native_nv12);
    LOAD_FAKE(native_nv12_import_only, fake_gbm_set_native_nv12_import_only);
    LOAD_FAKE(fail_rgb_create, fake_gbm_set_fail_rgb_create);
    LOAD_FAKE(fail_map, fake_gbm_set_fail_map);
    LOAD_FAKE(stats, fake_gbm_get_stats);
#undef LOAD_FAKE
    compat_library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!compat_library) fprintf(stderr, "compat GBM: %s\n", dlerror());
    CHECK(compat_library);
#define LOAD(name) load_symbol(compat_library, #name, &api.name, sizeof(api.name));
    GBM_APIS(LOAD)
#undef LOAD
    device = api.gbm_create_device(42);
    CHECK(device);
    test_support_queries(device);
    test_layout_and_fds(device);
    test_map_and_write(device);
    test_refused_allocations(device);
    test_scanout_allocations(device);
    test_modifiers(device);
    test_odd_dimensions_and_import(device);
    test_refused_imports(device);
    test_import_without_native_allocation(device);
    test_padded_scanout_import(device);
    test_user_data_reentry(device);
    test_passthrough(device);
    test_concurrent_tracking(device);
    api.gbm_device_destroy(device);
    CHECK(!dlclose(compat_library));
    CHECK(!dlclose(real_library));
    alarm(0);
    puts("gbm-compat mock tests: PASS");
    return 0;
}
