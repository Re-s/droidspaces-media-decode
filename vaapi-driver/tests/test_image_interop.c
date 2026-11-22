/* Contract checks for derive/PRIME-import and VA surface export descriptors.
 * A real shared memfd models the exported allocation; only the DRM export
 * ioctl and pending decode completion are mocked. No hardware claim is made.
 * Build: cc -D_GNU_SOURCE -std=c11 -Wall -Wextra -Wno-unused-parameter \
 *   $(pkg-config --cflags libva libdrm) -Isrc tests/test_image_interop.c \
 *   src/image.c -lpthread -o build/test_image_interop
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm_fourcc.h>
#include "driver.h"

/* Keep the descriptor checks on the production export implementation while
 * this executable continues to link image.c and reuse its DRM/memfd fixture. */
#include "../src/export.c"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)
#define OK(x) CHECK((x) == VA_STATUS_SUCCESS)

static int allocation_fd = -1, export_error;
static unsigned export_calls, wait_calls;
static uint32_t expected_prime_flags;
static VAStatus wait_status;
static int remove_during_wait;

struct sync_gate {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int waiting, proceed;
};
static struct sync_gate *active_sync_gate;

int ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (request == DRM_IOCTL_PRIME_HANDLE_TO_FD) {
        struct drm_prime_handle *prime = arg;
        export_calls++;
        CHECK(prime->handle == 42);
        CHECK(prime->flags == expected_prime_flags);
        if (export_error) { errno = export_error; return -1; }
        prime->fd = fcntl(allocation_fd, F_DUPFD_CLOEXEC, 3);
        return prime->fd < 0 ? -1 : 0;
    }
    return (int)syscall(SYS_ioctl, fd, request, arg);
}

struct dmd_driver *dmd_get_driver(VADriverContextP ctx)
{
    return ctx ? ctx->pDriverData : NULL;
}
void dmd_log(const char *fmt, ...) { }
unsigned dmd_align_up(unsigned value, unsigned alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
struct dmd_surface *dmd_find_surface_locked(struct dmd_driver *drv, VASurfaceID id)
{
    for (unsigned i = 0; i < DMD_MAX_SURFACES; i++)
        if (drv->surfaces[i].in_use && drv->surfaces[i].id == id)
            return &drv->surfaces[i];
    return NULL;
}
struct dmd_buffer *dmd_find_buffer_locked(struct dmd_driver *drv, VABufferID id)
{
    for (unsigned i = 0; i < DMD_MAX_BUFFERS; i++)
        if (drv->buffers[i].in_use && drv->buffers[i].id == id)
            return &drv->buffers[i];
    return NULL;
}
VAStatus dmd_surface_wait(struct dmd_driver *drv, VASurfaceID id)
{
    wait_calls++;
    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, id);
    if (s && remove_during_wait) s->in_use = 0;
    if (s && wait_status == VA_STATUS_SUCCESS && !active_sync_gate)
        s->state = DMD_SURFACE_READY;
    pthread_mutex_unlock(&drv->lock);
    return wait_status;
}
VAStatus dmd_surface_sync_locked(struct dmd_driver *drv, VASurfaceID id, int timeout)
{
    struct sync_gate *gate = active_sync_gate;
    if (!gate)
        return VA_STATUS_ERROR_OPERATION_FAILED; /* unexpected in other cases */
    /* Model the production wait: callers must revalidate locks acquired by
     * another thread while the driver's object mutex is released. */
    pthread_mutex_unlock(&drv->lock);
    pthread_mutex_lock(&gate->lock);
    gate->waiting = 1;
    pthread_cond_broadcast(&gate->changed);
    while (!gate->proceed)
        pthread_cond_wait(&gate->changed, &gate->lock);
    pthread_mutex_unlock(&gate->lock);
    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *surface = dmd_find_surface_locked(drv, id);
    CHECK(surface);
    surface->state = DMD_SURFACE_READY;
    return VA_STATUS_SUCCESS;
}

struct fixture {
    struct VADriverContext va;
    struct dmd_driver *drv;
    struct dmd_surface *surface;
    size_t size;
};

static void start(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->drv = calloc(1, sizeof(*f->drv));
    CHECK(f->drv);
    pthread_mutex_init(&f->drv->lock, NULL);
    f->va.pDriverData = f->drv;
    f->drv->next_image_id = 1;
    f->drv->next_buffer_id = 1;
    f->size = 256 * 128 * 3 / 2;
    allocation_fd = memfd_create("image-interop-allocation", MFD_CLOEXEC);
    CHECK(allocation_fd >= 0 && ftruncate(allocation_fd, (off_t)f->size) == 0);
    struct dmd_surface *s = &f->drv->surfaces[0];
    s->in_use = 1; s->id = 7; s->context = VA_INVALID_ID;
    s->width = 130; s->height = 98;
    s->stride = 256; s->slice_height = 128;
    s->format = VA_RT_FORMAT_YUV420;
    s->data = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, allocation_fd, 0);
    CHECK(s->data != MAP_FAILED);
    s->data_size = s->dumb_size = f->size;
    s->exportable = 1; s->dumb_handle = 42; s->dumb_drm_fd = 99;
    s->dumb_sync_fd = -1;
    memset(s->data, 0, 256 * 128);
    memset(s->data + 256 * 128, 128, f->size - 256 * 128);
    s->state = DMD_SURFACE_IDLE;
    s->decode_status = VA_STATUS_SUCCESS;
    f->surface = s;
    export_error = remove_during_wait = 0;
    export_calls = wait_calls = 0;
    expected_prime_flags = DRM_CLOEXEC | DRM_RDWR;
    wait_status = VA_STATUS_SUCCESS;
}

static void finish(struct fixture *f)
{
    for (unsigned i = 0; i < DMD_MAX_IMAGES; i++) CHECK(!f->drv->images[i].in_use);
    munmap(f->surface->data, f->size);
    close(allocation_fd);
    allocation_fd = -1;
    pthread_mutex_destroy(&f->drv->lock);
    free(f->drv);
}

static void export_surface_descriptors(int ten_bit)
{
    struct fixture f; start(&f);
    if (ten_bit) {
        /* Give 130 visible pixels a legal 16bit pitch and retain 128 storage
         * rows for a visible height of 98. This detects UV offset=height bugs. */
        CHECK(munmap(f.surface->data, f.size) == 0);
        f.surface->format = VA_RT_FORMAT_YUV420_10;
        f.surface->stride = 512;
        f.size = (size_t)512 * 128 * 3 / 2;
        CHECK(ftruncate(allocation_fd, (off_t)f.size) == 0);
        f.surface->data = mmap(NULL, f.size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, allocation_fd, 0);
        CHECK(f.surface->data != MAP_FAILED);
        f.surface->data_size = f.surface->dumb_size = f.size;
        memset(f.surface->data, 0, f.size);
        f.surface->data[1] = 0x10; /* Y=64<<6, not low-bit-aligned 64 */
        size_t uv = (size_t)f.surface->stride * f.surface->slice_height;
        f.surface->data[uv + 1] = 0x40; /* Cb=256<<6 */
        f.surface->data[uv + 3] = 0xc0; /* Cr=768<<6 */
    }
    const uint32_t flags[] = {
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
        VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
        0, /* Existing default contract: separate layers, read-only PRIME fd. */
    };
    for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        VADRMPRIMESurfaceDescriptor desc;
        memset(&desc, 0xa5, sizeof(desc));
        expected_prime_flags = DRM_CLOEXEC |
            ((flags[i] & VA_EXPORT_SURFACE_WRITE_ONLY) ? DRM_RDWR : 0);
        f.surface->exported = f.surface->external_write_exported = 0;
        OK(dmd_ExportSurfaceHandle(&f.va, 7, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   flags[i], &desc));
        CHECK(export_calls == i + 1 && wait_calls == 0);
        CHECK(f.surface->state == DMD_SURFACE_IDLE); /* export reads no pixels */
        CHECK(f.surface->exported && f.surface->external_write_exported ==
              !!(flags[i] & VA_EXPORT_SURFACE_WRITE_ONLY));
        CHECK(desc.fourcc == (ten_bit ? VA_FOURCC_P010 : VA_FOURCC_NV12));
        CHECK(desc.width == 130 && desc.height == 98);
        CHECK(desc.num_objects == 1 && desc.objects[0].size == f.size);
        CHECK(desc.objects[0].drm_format_modifier == DRM_FORMAT_MOD_LINEAR);
        CHECK(desc.objects[0].fd >= 0);
        CHECK(fcntl(desc.objects[0].fd, F_GETFD) & FD_CLOEXEC);
        unsigned uv_layer = flags[i] & VA_EXPORT_SURFACE_COMPOSED_LAYERS ? 0 : 1;
        unsigned uv_plane = uv_layer ? 0 : 1;
        CHECK(desc.num_layers == (uv_layer ? 2 : 1));
        CHECK(desc.layers[0].drm_format == (uv_layer ?
              (ten_bit ? DRM_FORMAT_R16 : DRM_FORMAT_R8) :
              (ten_bit ? DRM_FORMAT_P010 : DRM_FORMAT_NV12)));
        CHECK(desc.layers[0].num_planes == (uv_layer ? 1 : 2));
        CHECK(desc.layers[0].object_index[0] == 0 && desc.layers[0].offset[0] == 0);
        CHECK(desc.layers[0].pitch[0] == f.surface->stride);
        if (uv_layer) {
            CHECK(desc.layers[1].drm_format == (ten_bit ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88));
            CHECK(desc.layers[1].num_planes == 1);
        }
        CHECK(desc.layers[uv_layer].object_index[uv_plane] == 0);
        CHECK(desc.layers[uv_layer].pitch[uv_plane] == f.surface->stride);
        size_t uv = (size_t)f.surface->stride * f.surface->slice_height;
        CHECK(desc.layers[uv_layer].offset[uv_plane] == uv);
        CHECK(uv != (size_t)f.surface->stride * desc.height);
        CHECK(uv + (size_t)f.surface->stride * ((desc.height + 1) / 2) <=
              desc.objects[0].size);
        const unsigned char *external = mmap(NULL, f.size, PROT_READ, MAP_SHARED,
                                             desc.objects[0].fd, 0);
        CHECK(external != MAP_FAILED && !memcmp(external, f.surface->data, f.size));
        if (ten_bit) {
            CHECK(external[0] == 0 && external[1] == 0x10);
            CHECK(external[uv] == 0 && external[uv + 1] == 0x40);
            CHECK(external[uv + 2] == 0 && external[uv + 3] == 0xc0);
        }
        CHECK(munmap((void *)external, f.size) == 0);
        CHECK(close(desc.objects[0].fd) == 0); /* one fd/object, not one/layer */
        CHECK(fcntl(allocation_fd, F_GETFD) >= 0); /* parent allocation remains */
    }
    finish(&f);
}

static void export_surface_failures(void)
{
    struct fixture f; start(&f);
    VADRMPRIMESurfaceDescriptor desc;
    const uint32_t type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    const uint32_t flags = VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS;
    CHECK(dmd_ExportSurfaceHandle(&f.va, 7, type, flags, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER);
    CHECK(dmd_ExportSurfaceHandle(&f.va, 7, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME,
                                   flags, &desc) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    CHECK(dmd_ExportSurfaceHandle(&f.va, 99, type, flags, &desc) == VA_STATUS_ERROR_INVALID_SURFACE);
    f.surface->exportable = 0;
    CHECK(dmd_ExportSurfaceHandle(&f.va, 7, type, flags, &desc) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    f.surface->exportable = 1;
    CHECK(export_calls == 0);
    expected_prime_flags = DRM_CLOEXEC;
    export_error = EIO;
    CHECK(dmd_ExportSurfaceHandle(&f.va, 7, type, flags, &desc) == VA_STATUS_ERROR_OPERATION_FAILED);
    CHECK(export_calls == 1 && !f.surface->exported);
    export_error = 0;
    VAImage image;
    OK(dmd_DeriveImage(&f.va, 7, &image));
    struct dmd_image *derived = dmd_find_image_locked(f.drv, image.image_id);
    CHECK(derived);
    derived->mapped = 1;
    CHECK(dmd_ExportSurfaceHandle(&f.va, 7, type, flags, &desc) == VA_STATUS_ERROR_SURFACE_BUSY);
    derived->mapped = 0;
    VABufferInfo info = { 0 };
    expected_prime_flags = DRM_CLOEXEC | DRM_RDWR;
    OK(dmd_AcquireBufferHandle(&f.va, image.buf, &info));
    CHECK(dmd_ExportSurfaceHandle(&f.va, 7, type, flags, &desc) == VA_STATUS_ERROR_SURFACE_BUSY);
    CHECK(export_calls == 2 && wait_calls == 0);
    OK(dmd_ReleaseBufferHandle(&f.va, image.buf));
    OK(dmd_DestroyImage(&f.va, image.image_id));
    finish(&f);
}

static void allocation_probe_and_handle_lifetime(void)
{
    struct fixture f; start(&f);
    VAImage image;
    OK(dmd_DeriveImage(&f.va, 7, &image));
    CHECK(f.surface->state == DMD_SURFACE_IDLE && wait_calls == 0);
    CHECK(image.width == 130 && image.height == 98 && image.offsets[1] == 256 * 128);
    VABufferInfo first = { .mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME };
    OK(dmd_AcquireBufferHandle(&f.va, image.buf, &first));
    CHECK(first.type == VAImageBufferType && first.mem_size == f.size);
    CHECK(first.mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME);
    CHECK(fcntl((int)first.handle, F_GETFD) & FD_CLOEXEC);
    unsigned char *external = mmap(NULL, f.size, PROT_READ, MAP_SHARED, (int)first.handle, 0);
    CHECK(external != MAP_FAILED && !memcmp(external, f.surface->data, f.size));
    f.surface->data[19] = 73;
    CHECK(external[19] == 73); /* same live allocation, not a snapshot or fake fd */
    VABufferInfo again = { 0 };
    OK(dmd_AcquireBufferHandle(&f.va, image.buf, &again));
    CHECK(again.handle == first.handle && export_calls == 1);
    CHECK(dmd_DestroyImage(&f.va, image.image_id) == VA_STATUS_ERROR_SURFACE_BUSY);
    OK(dmd_ReleaseBufferHandle(&f.va, image.buf));
    CHECK(fcntl((int)first.handle, F_GETFD) >= 0);
    OK(dmd_ReleaseBufferHandle(&f.va, image.buf));
    CHECK(fcntl((int)first.handle, F_GETFD) == -1 && errno == EBADF);
    OK(dmd_ReleaseBufferHandle(&f.va, image.buf)); /* cleanup after failed probe */
    munmap(external, f.size);
    OK(dmd_DestroyImage(&f.va, image.image_id));
    finish(&f);
}

static void failure_and_busy_paths(void)
{
    struct fixture f; start(&f);
    VAImage image;
    OK(dmd_DeriveImage(&f.va, 7, &image));
    struct dmd_image *derived = dmd_find_image_locked(f.drv, image.image_id);
    VABufferInfo info = { .mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM };
    CHECK(dmd_AcquireBufferHandle(&f.va, image.buf, &info) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    export_error = EIO;
    CHECK(dmd_AcquireBufferHandle(&f.va, image.buf, &info) == VA_STATUS_ERROR_OPERATION_FAILED);
    CHECK(derived->handle_acquired == 0 && derived->acquired_fd == -1);
    export_error = 0;
    derived->mapped = 1;
    CHECK(dmd_AcquireBufferHandle(&f.va, image.buf, &info) == VA_STATUS_ERROR_SURFACE_BUSY);
    CHECK(dmd_DestroyImage(&f.va, image.image_id) == VA_STATUS_ERROR_SURFACE_BUSY);
    derived->mapped = 0;
    f.surface->exportable = 0;
    CHECK(dmd_AcquireBufferHandle(&f.va, image.buf, &info) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    f.surface->exportable = 1;
    f.surface->decode_status = VA_STATUS_ERROR_DECODING_ERROR;
    CHECK(dmd_AcquireBufferHandle(&f.va, image.buf, &info) == VA_STATUS_ERROR_DECODING_ERROR);
    f.surface->decode_status = VA_STATUS_SUCCESS;
    f.surface->state = DMD_SURFACE_PENDING;
    wait_status = VA_STATUS_ERROR_TIMEDOUT;
    CHECK(dmd_AcquireBufferHandle(&f.va, image.buf, &info) == VA_STATUS_ERROR_TIMEDOUT);
    CHECK(wait_calls == 1 && !derived->handle_acquired);
    wait_status = VA_STATUS_SUCCESS;
    OK(dmd_AcquireBufferHandle(&f.va, image.buf, &info));
    CHECK(wait_calls == 2 && f.surface->state == DMD_SURFACE_READY);
    CHECK(dmd_GetImage(&f.va, 7, 0, 0, 130, 98, image.image_id) == VA_STATUS_ERROR_SURFACE_BUSY);
    CHECK(dmd_PutImage(&f.va, 7, image.image_id, 0, 0, 130, 98, 0, 0, 130, 98) == VA_STATUS_ERROR_SURFACE_BUSY);
    OK(dmd_ReleaseBufferHandle(&f.va, image.buf));
    CHECK(dmd_GetImage(&f.va, 7, 0, 0, 130, 98, image.image_id) == VA_STATUS_ERROR_SURFACE_BUSY);
    OK(dmd_DestroyImage(&f.va, image.image_id));
    CHECK(dmd_ReleaseBufferHandle(&f.va, image.buf) == VA_STATUS_ERROR_INVALID_BUFFER);
    finish(&f);
}

static void validate_storage_and_recheck_after_wait(void)
{
    struct fixture f; start(&f);
    VAImage image;
    f.surface->data_size = 16;
    CHECK(dmd_DeriveImage(&f.va, 7, &image) == VA_STATUS_ERROR_OPERATION_FAILED);
    f.surface->data_size = f.size;
    f.surface->stride = 8;
    CHECK(dmd_DeriveImage(&f.va, 7, &image) == VA_STATUS_ERROR_OPERATION_FAILED);
    f.surface->stride = 256;
    f.surface->decode_status = VA_STATUS_ERROR_DECODING_ERROR;
    CHECK(dmd_DeriveImage(&f.va, 7, &image) == VA_STATUS_ERROR_DECODING_ERROR);
    f.surface->decode_status = VA_STATUS_SUCCESS;
    OK(dmd_DeriveImage(&f.va, 7, &image));
    f.surface->state = DMD_SURFACE_PENDING;
    remove_during_wait = 1;
    VABufferInfo info = { 0 };
    CHECK(dmd_AcquireBufferHandle(&f.va, image.buf, &info) == VA_STATUS_ERROR_INVALID_BUFFER);
    CHECK(export_calls == 0);
    f.surface->in_use = 1;
    OK(dmd_DestroyImage(&f.va, image.image_id));
    VAImageFormat format = { .fourcc = VA_FOURCC_NV12 };
    OK(dmd_CreateImage(&f.va, &format, 130, 98, &image));
    CHECK(dmd_AcquireBufferHandle(&f.va, image.buf, &info) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    OK(dmd_ReleaseBufferHandle(&f.va, image.buf));
    OK(dmd_DestroyImage(&f.va, image.image_id));
    finish(&f);
}

struct get_image_call {
    struct fixture *fixture;
    VASurfaceID surface;
    VAImageID image;
    VAStatus status;
};

static void *get_image_thread(void *opaque)
{
    struct get_image_call *call = opaque;
    call->status = dmd_GetImage(&call->fixture->va, call->surface, 0, 0, 130, 98,
                                call->image);
    return NULL;
}

static void get_image_rechecks_target_after_wait(void)
{
    struct fixture f; start(&f);
    VAImage image;
    VAImageFormat format = { .fourcc = VA_FOURCC_NV12 };
    OK(dmd_CreateImage(&f.va, &format, 130, 98, &image));
    struct dmd_image *target = dmd_find_image_locked(f.drv, image.image_id);
    CHECK(target && target->data_size);
    memset(target->data, 0xa5, target->data_size);
    f.surface->state = DMD_SURFACE_PENDING;
    struct sync_gate gate = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
    };
    active_sync_gate = &gate;
    struct get_image_call call = { .fixture = &f, .surface = 7, .image = image.image_id };
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, get_image_thread, &call) == 0);
    pthread_mutex_lock(&gate.lock);
    while (!gate.waiting)
        pthread_cond_wait(&gate.changed, &gate.lock);
    pthread_mutex_unlock(&gate.lock);

    /* MapBuffer owns the same object mutex; model its completed map while
     * GetImage waits for its unrelated pending source surface. */
    pthread_mutex_lock(&f.drv->lock);
    target->mapped = 1;
    pthread_mutex_unlock(&f.drv->lock);
    pthread_mutex_lock(&gate.lock);
    gate.proceed = 1;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.lock);
    CHECK(pthread_join(thread, NULL) == 0);
    active_sync_gate = NULL;
    CHECK(call.status == VA_STATUS_ERROR_SURFACE_BUSY);
    for (size_t i = 0; i < target->data_size; i++)
        CHECK(target->data[i] == 0xa5); /* mapped client storage was untouched */
    target->mapped = 0;
    OK(dmd_DestroyImage(&f.va, image.image_id));
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.lock);
    finish(&f);
}

static void get_image_fills_chroma_padding(int ten_bit)
{
    struct fixture f; start(&f);
    const unsigned sample_bytes = ten_bit ? 2 : 1;
    /* A source shrunk to 128x64 while the client's image/pool remains 130x98.
     * Both right-edge and bottom padding must be legal neutral chroma. */
    f.surface->format = ten_bit ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    f.surface->stride = 128 * sample_bytes;
    f.surface->slice_height = 64;
    f.surface->data_size = (size_t)f.surface->stride * 64 * 3 / 2;
    size_t plane = (size_t)f.surface->stride * 64;
    memset(f.surface->data, 0, f.surface->data_size);
    for (size_t offset = plane; offset < f.surface->data_size; offset += sample_bytes) {
        f.surface->data[offset] = ten_bit ? 0 : 0x90;
        if (ten_bit) f.surface->data[offset + 1] = 0x90;
    }
    VAImage image;
    VAImageFormat format = { .fourcc = ten_bit ? VA_FOURCC_P010 : VA_FOURCC_NV12 };
    OK(dmd_CreateImage(&f.va, &format, 130, 98, &image));
    struct dmd_image *target = dmd_find_image_locked(f.drv, image.image_id);
    CHECK(target);
    memset(target->data, 0xa5, target->data_size);
    OK(dmd_GetImage(&f.va, 7, 0, 0, 130, 98, image.image_id));
    for (unsigned y = 0; y < 98 / 2; y++) {
        for (unsigned x = 0; x < 130; x++) {
            const unsigned char *sample = target->data + image.offsets[1] +
                                           (size_t)y * image.pitches[1] + x * sample_bytes;
            unsigned value = sample[0] | (ten_bit ? (unsigned)sample[1] << 8 : 0);
            unsigned expected = y < 32 && x < 128 ? (ten_bit ? 0x9000 : 0x90)
                                                    : (ten_bit ? 0x8000 : 0x80);
            CHECK(value == expected);
        }
    }
    OK(dmd_DestroyImage(&f.va, image.image_id));
    finish(&f);
}

static struct dmd_surface *add_other_surface(struct fixture *f)
{
    struct dmd_surface *other = &f->drv->surfaces[1];
    *other = *f->surface;
    other->id = 8;
    other->exportable = 0;
    other->dumb_handle = 0;
    other->dumb_drm_fd = other->dumb_sync_fd = -1;
    other->data = malloc(f->size);
    CHECK(other->data);
    memset(other->data, 0x3c, f->size);
    other->state = DMD_SURFACE_READY;
    return other;
}

static void check_fill(const unsigned char *data, unsigned char value, size_t size)
{
    for (size_t i = 0; i < size; i++) CHECK(data[i] == value);
}

/* This target links image.c alone. Model Map/Unmap's protected reference count
 * exactly as the existing wait test does; actual Map2/Unmap and DMA transactions
 * are covered by the separate test_encode_api driver-vtable executable. */
static void model_map_count(struct fixture *f, struct dmd_image *image, int mapped)
{
    pthread_mutex_lock(&f->drv->lock);
    image->mapped = mapped;
    pthread_mutex_unlock(&f->drv->lock);
}

static void cross_surface_derived_alias_locks(void)
{
    struct fixture f; start(&f);
    struct dmd_surface *other = add_other_surface(&f);
    VAImage first, second;
    OK(dmd_DeriveImage(&f.va, 7, &first));
    OK(dmd_DeriveImage(&f.va, 7, &second));
    struct dmd_image *first_view = dmd_find_image_locked(f.drv, first.image_id);
    CHECK(first_view);
    for (unsigned mapped = 0; mapped < 2; mapped++) {
        memset(f.surface->data, 0xa5, f.size);
        memset(other->data, 0x3c, f.size);
        VABufferInfo info = { 0 };
        if (mapped) model_map_count(&f, first_view, 1);
        else OK(dmd_AcquireBufferHandle(&f.va, first.buf, &info));
        /* The unlocked second view is still an alias of the locked parent.
         * Neither Get's destination nor Put's source may bypass that lock. */
        CHECK(dmd_GetImage(&f.va, 8, 0, 0, 130, 98, second.image_id) ==
              VA_STATUS_ERROR_SURFACE_BUSY);
        check_fill(f.surface->data, 0xa5, f.size);
        CHECK(dmd_PutImage(&f.va, 8, second.image_id,
                          0, 0, 130, 98, 0, 0, 130, 98) == VA_STATUS_ERROR_SURFACE_BUSY);
        check_fill(other->data, 0x3c, f.size);
        if (mapped) model_map_count(&f, first_view, 0);
        else OK(dmd_ReleaseBufferHandle(&f.va, first.buf));
        /* Release/Unmap restores the documented cross-surface image copies. */
        OK(dmd_GetImage(&f.va, 8, 0, 0, 130, 98, second.image_id));
        CHECK(f.surface->data[0] == 0x3c);
        f.surface->data[0] = 0x71;
        OK(dmd_PutImage(&f.va, 8, second.image_id,
                        0, 0, 130, 98, 0, 0, 130, 98));
        CHECK(other->data[0] == 0x71);
    }
    OK(dmd_DestroyImage(&f.va, first.image_id));
    OK(dmd_DestroyImage(&f.va, second.image_id));
    free(other->data);
    memset(other, 0, sizeof(*other));
    finish(&f);
}

static void get_image_rechecks_alias_parent_after_wait(void)
{
    for (unsigned mapped = 0; mapped < 2; mapped++) {
        struct fixture f; start(&f);
        struct dmd_surface *other = add_other_surface(&f);
        VAImage first, second;
        OK(dmd_DeriveImage(&f.va, 7, &first));
        OK(dmd_DeriveImage(&f.va, 7, &second));
        struct dmd_image *first_view = dmd_find_image_locked(f.drv, first.image_id);
        CHECK(first_view);
        memset(f.surface->data, 0xa5, f.size);
        other->state = DMD_SURFACE_PENDING;
        struct sync_gate gate = {
            .lock = PTHREAD_MUTEX_INITIALIZER,
            .changed = PTHREAD_COND_INITIALIZER,
        };
        active_sync_gate = &gate;
        struct get_image_call call = { .fixture = &f, .surface = 8, .image = second.image_id };
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, get_image_thread, &call) == 0);
        struct timespec deadline;
        CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
        deadline.tv_sec += 3;
        pthread_mutex_lock(&gate.lock);
        while (!gate.waiting)
            CHECK(pthread_cond_timedwait(&gate.changed, &gate.lock, &deadline) == 0);
        pthread_mutex_unlock(&gate.lock);
        VABufferInfo info = { 0 };
        if (mapped) model_map_count(&f, first_view, 1);
        else OK(dmd_AcquireBufferHandle(&f.va, first.buf, &info));
        pthread_mutex_lock(&gate.lock);
        gate.proceed = 1;
        pthread_cond_broadcast(&gate.changed);
        pthread_mutex_unlock(&gate.lock);
        CHECK(pthread_join(thread, NULL) == 0);
        active_sync_gate = NULL;
        CHECK(call.status == VA_STATUS_ERROR_SURFACE_BUSY);
        check_fill(f.surface->data, 0xa5, f.size);
        if (mapped) model_map_count(&f, first_view, 0);
        else OK(dmd_ReleaseBufferHandle(&f.va, first.buf));
        OK(dmd_GetImage(&f.va, 8, 0, 0, 130, 98, second.image_id));
        CHECK(f.surface->data[0] == 0x3c);
        OK(dmd_DestroyImage(&f.va, first.image_id));
        OK(dmd_DestroyImage(&f.va, second.image_id));
        pthread_cond_destroy(&gate.changed);
        pthread_mutex_destroy(&gate.lock);
        free(other->data);
        memset(other, 0, sizeof(*other));
        finish(&f);
    }
}

int main(void)
{
    export_surface_descriptors(0);
    export_surface_descriptors(1);
    export_surface_failures();
    allocation_probe_and_handle_lifetime();
    failure_and_busy_paths();
    validate_storage_and_recheck_after_wait();
    get_image_rechecks_target_after_wait();
    get_image_fills_chroma_padding(0);
    get_image_fills_chroma_padding(1);
    cross_surface_derived_alias_locks();
    get_image_rechecks_alias_parent_after_wait();
    puts("image interop: PASS (shared storage, probe, synchronization, lifetime and failures)");
    return 0;
}
