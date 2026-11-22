/* VAImage access to the driver's NV12/P010 surface storage.
 * Storage may be a DRM allocation or a CPU-only fallback. Derived images
 * borrow that storage; only actual DRM allocations can supply PRIME handles.
 *
 * - - ffmpeg 's actual readback path (readed hwcontext_vaapi.c authenticated) - ─ ─ ~ ~ ffmpeg
 * vaapi_frames_init first build a test search search for the vaDeriveImage (:696), which is a very interesting piece of information about the situation in the country.
 * Successful and sourcecc matches derive_works=1 (700); failure is only debug log, not fatal.
 * But the condition of vaapi_map_frame
 *   derive_works && dst->format == sw_format && ((flags & MAP_DIRECT) || !(flags & MAP_READ))
 * hwdownload is **read** access, so even the derive_works=1 leaves the other branch:
 * Both roads must be achieved.
 *
 * ─ - 1088 vs 1080: One of the most critical parts of this document – the one that is -- the one that is – the one that is – the other that is – the other that is – the other that is – the other that is – the other that is – the other that is – the other that is – the other that is – the other that is – the other that is
 * Decoder output buffer 1920x1088, displaying 1920x1080, stride=192, slice_height=1088.
 * VAImage. width/ height** display dimensions** (1920x1080; otherwise ffmpeg's size verification failed),
 * But offsets [1] must be buffered high**: offsets[1] = stride* slice_height = 1920*1088.
 * Using 1080 to offset the UV plane byte of 1920*8 - the symptoms are green edges, flower screens, colouring.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm_fourcc.h>

#include "driver.h"

/* An acquired handle locks its parent against VA operations; mapped derived
 * images additionally forbid writes while CPU access is in progress. */
static int surface_image_busy_locked(struct dmd_driver *drv, VASurfaceID surface,
                                      int include_mapped)
{
    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        const struct dmd_image *img = &drv->images[i];
        if (img->in_use && img->derived_from == surface &&
            (img->handle_acquired || (include_mapped && img->mapped)))
            return 1;
    }
    return 0;
}

struct dmd_import {
    atomic_uint refs;
    unsigned count, width, height;
    struct { int fd; unsigned char *map; size_t size; } objects[4];
    struct { unsigned object, offset, pitch; } planes[2];
};

void dmd_import_ref(struct dmd_import *input)
{
    if (input) atomic_fetch_add_explicit(&input->refs, 1, memory_order_relaxed);
}

void dmd_import_unref(struct dmd_import *input)
{
    if (!input || atomic_fetch_sub_explicit(&input->refs, 1, memory_order_acq_rel) != 1)
        return;
    for (unsigned i = 0; i < input->count; i++) {
        if (input->objects[i].map)
            munmap(input->objects[i].map, input->objects[i].size);
        if (input->objects[i].fd >= 0)
            close(input->objects[i].fd);
    }
    free(input);
}

static VAStatus import_validate_size(int fd, unsigned declared_size)
{
    /* DMA-BUF guarantees SEEK_END(0)/SEEK_SET(0), but may reject SEEK_CUR
     * and need not expose its length through stat. A duplicate still shares
     * the file position, so preserve it when the descriptor supports that. */
    const off_t previous = lseek(fd, 0, SEEK_CUR);
    off_t size = lseek(fd, 0, SEEK_END);
    if (size >= 0) {
        if (lseek(fd, previous >= 0 ? previous : 0, SEEK_SET) < 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
    } else {
        struct stat st;
        if (fstat(fd, &st) < 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        size = st.st_size;
        /* A failed END must not move the offset; restore a known position
         * defensively. Old descriptors without llseek can still use stat. */
        if (previous >= 0 && lseek(fd, previous, SEEK_SET) < 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    return size > 0 && (uint64_t)size >= declared_size ?
        VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_PARAMETER;
}

VAStatus dmd_import_surface_init(struct dmd_surface *surface, unsigned width,
                                  unsigned height, unsigned memory_type,
                                  const void *descriptor)
{
    if (!surface || !descriptor || !width || !height ||
        width > DMD_MAX_WIDTH || height > DMD_MAX_HEIGHT || ((width | height) & 1))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    VADRMPRIMESurfaceDescriptor desc = { 0 };
    if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        memcpy(&desc, descriptor, sizeof(desc));
    } else if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) {
        VASurfaceAttribExternalBuffers ext;
        memcpy(&ext, descriptor, sizeof(ext));
        if (ext.pixel_format != VA_FOURCC_NV12 || ext.num_planes != 2 ||
            ext.num_buffers != 1 || !ext.buffers || ext.buffers[0] > INT32_MAX ||
            ext.flags & ~(VA_SURFACE_EXTBUF_DESC_CACHED |
                          VA_SURFACE_EXTBUF_DESC_UNCACHED | VA_SURFACE_EXTBUF_DESC_WC) ||
            ext.private_data)
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        desc.fourcc = ext.pixel_format;
        desc.width = ext.width;
        desc.height = ext.height;
        desc.num_objects = 1;
        desc.objects[0].fd = (int)ext.buffers[0];
        desc.objects[0].size = ext.data_size;
        desc.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
        desc.num_layers = 1;
        desc.layers[0].drm_format = DRM_FORMAT_NV12;
        desc.layers[0].num_planes = 2;
        for (unsigned p = 0; p < 2; p++) {
            desc.layers[0].offset[p] = ext.offsets[p];
            desc.layers[0].pitch[p] = ext.pitches[p];
        }
    } else {
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if (desc.fourcc != VA_FOURCC_NV12)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (desc.width != width || desc.height != height ||
        desc.num_objects < 1 || desc.num_objects > 4)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct dmd_import *input = calloc(1, sizeof(*input));
    if (!input) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    atomic_init(&input->refs, 1);
    input->count = desc.num_objects;
    input->width = width;
    input->height = height;
    for (unsigned i = 0; i < input->count; i++) input->objects[i].fd = -1;
    VAStatus status = VA_STATUS_ERROR_INVALID_PARAMETER;
    if (desc.num_layers == 1 && desc.layers[0].drm_format == DRM_FORMAT_NV12 &&
        desc.layers[0].num_planes == 2) {
        for (unsigned p = 0; p < 2; p++) {
            input->planes[p].object = desc.layers[0].object_index[p];
            input->planes[p].offset = desc.layers[0].offset[p];
            input->planes[p].pitch = desc.layers[0].pitch[p];
        }
    } else if (desc.num_layers == 2 && desc.layers[0].num_planes == 1 &&
               desc.layers[1].num_planes == 1 &&
               desc.layers[0].drm_format == DRM_FORMAT_R8 &&
               desc.layers[1].drm_format == DRM_FORMAT_GR88) {
        for (unsigned p = 0; p < 2; p++) {
            input->planes[p].object = desc.layers[p].object_index[0];
            input->planes[p].offset = desc.layers[p].offset[0];
            input->planes[p].pitch = desc.layers[p].pitch[0];
        }
    } else {
        status = VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        goto fail;
    }
    uint64_t ends[2];
    for (unsigned p = 0; p < 2; p++) {
        unsigned object = input->planes[p].object;
        unsigned rows = p ? height / 2 : height;
        if (object >= input->count || input->planes[p].pitch < width ||
            (input->planes[p].pitch | input->planes[p].offset) & 1)
            goto fail;
        ends[p] = (uint64_t)input->planes[p].offset +
                  (uint64_t)(rows - 1) * input->planes[p].pitch + width;
        if (ends[p] > desc.objects[object].size) goto fail;
    }
    if (input->planes[0].object == input->planes[1].object &&
        input->planes[0].offset < ends[1] && input->planes[1].offset < ends[0])
        goto fail;
    uint64_t total = 0;
    for (unsigned i = 0; i < input->count; i++) {
        if (desc.objects[i].drm_format_modifier != DRM_FORMAT_MOD_LINEAR) {
            status = VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
            goto fail;
        }
        total += desc.objects[i].size;
        if (desc.objects[i].fd < 0 || !desc.objects[i].size || total > DMD_MAX_FRAME_BYTES)
            goto fail;
        input->objects[i].size = desc.objects[i].size;
        input->objects[i].fd = fcntl(desc.objects[i].fd, F_DUPFD_CLOEXEC, 3);
        if (input->objects[i].fd < 0) {
            status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            goto fail;
        }
        VAStatus size_status = import_validate_size(input->objects[i].fd,
                                                     desc.objects[i].size);
        if (size_status != VA_STATUS_SUCCESS) {
            status = size_status;
            goto fail;
        }
        void *map = mmap(NULL, input->objects[i].size, PROT_READ, MAP_SHARED,
                         input->objects[i].fd, 0);
        if (map == MAP_FAILED) {
            status = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }
        input->objects[i].map = map;
    }
    const unsigned stride = dmd_align_up(width, DMD_WIDTH_ALIGN);
    const unsigned slice = dmd_align_up(height, DMD_HEIGHT_ALIGN);
    const size_t size = (size_t)stride * slice * 3 / 2;
    unsigned char *data = calloc(1, size);
    if (!data) { status = VA_STATUS_ERROR_ALLOCATION_FAILED; goto fail; }
    memset(data + (size_t)stride * slice, 128, (size_t)stride * slice / 2);
    memset(surface, 0, sizeof(*surface));
    surface->width = surface->visible_width = width;
    surface->height = surface->visible_height = height;
    surface->buf_width = surface->stride = stride;
    surface->buf_height = surface->slice_height = slice;
    surface->format = VA_RT_FORMAT_YUV420;
    surface->data = data;
    surface->data_size = size;
    surface->imported = input;
    surface->state = DMD_SURFACE_READY;
    surface->decode_status = VA_STATUS_SUCCESS;
    surface->context = VA_INVALID_ID;
    surface->dumb_sync_fd = surface->dumb_drm_fd = -1;
    return VA_STATUS_SUCCESS;
fail:
    dmd_import_unref(input);
    return status;
}

/* Caller retains imported storage across this potentially blocking CPU access.
 * Import FD lifetime and the application FD lifetime are independent. */
VAStatus dmd_import_copy(const struct dmd_surface *surface, unsigned char *data,
                          size_t capacity)
{
    struct dmd_import *input = surface->imported;
    if (!input || !data || capacity < surface->data_size)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned started = 0;
    VAStatus status = VA_STATUS_SUCCESS;
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    for (; started < input->count; started++)
        if (ioctl(input->objects[started].fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
            status = VA_STATUS_ERROR_OPERATION_FAILED;
            break;
        }
    if (status == VA_STATUS_SUCCESS) {
        memset(data, 0, (size_t)surface->stride * surface->slice_height);
        memset(data + (size_t)surface->stride * surface->slice_height, 128,
                (size_t)surface->stride * surface->slice_height / 2);
        for (unsigned p = 0; p < 2; p++) {
            const unsigned char *src = input->objects[input->planes[p].object].map +
                                        input->planes[p].offset;
            unsigned char *dst = data + (p ? (size_t)surface->stride * surface->slice_height : 0);
            for (unsigned y = 0; y < (p ? input->height / 2 : input->height); y++)
                memcpy(dst + (size_t)y * surface->stride,
                       src + (size_t)y * input->planes[p].pitch, input->width);
        }
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    while (started)
        if (ioctl(input->objects[--started].fd, DMA_BUF_IOCTL_SYNC, &sync) != 0)
            status = VA_STATUS_ERROR_OPERATION_FAILED;
    return status;
}

/* Upload without scaling or colour conversion. FFmpeg uses this when its
 * hardware-frame upload cannot map a VA surface directly. */
VAStatus dmd_PutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image,
                      int src_x, int src_y, unsigned int src_width,
                      unsigned int src_height, int dest_x, int dest_y,
                      unsigned int dest_width, unsigned int dest_height)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv || src_x < 0 || src_y < 0 || dest_x < 0 || dest_y < 0 ||
        !src_width || !src_height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (src_width != dest_width || src_height != dest_height)
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    if ((src_x | src_y | dest_x | dest_y | src_width | src_height) & 1)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *target = dmd_find_surface_locked(drv, surface);
    if (!target) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (target->imported) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    int pending = target->state == DMD_SURFACE_PENDING;
    pthread_mutex_unlock(&drv->lock);
    VAStatus status = pending ? dmd_surface_wait(drv, surface) : VA_STATUS_SUCCESS;
    if (status != VA_STATUS_SUCCESS)
        return status;
    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, surface);
    struct dmd_image *img = dmd_find_image_locked(drv, image);
    if (!s || !img) {
        pthread_mutex_unlock(&drv->lock);
        return !s ? VA_STATUS_ERROR_INVALID_SURFACE : VA_STATUS_ERROR_INVALID_IMAGE;
    }
    if (s->state == DMD_SURFACE_PENDING) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_HW_BUSY;
    }
    if (img->handle_acquired || img->mapped ||
        img->derived_from == surface ||
        (img->derived_from != VA_INVALID_ID &&
         surface_image_busy_locked(drv, img->derived_from, 1)) ||
        surface_image_busy_locked(drv, surface, 1)) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    unsigned bpp = s->format == VA_RT_FORMAT_YUV420_10 ? 2 : 1;
    if (img->image.format.fourcc != (bpp == 2 ? VA_FOURCC_P010 : VA_FOURCC_NV12)) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if ((unsigned)src_x > img->image.width || (unsigned)src_y > img->image.height ||
        src_width > img->image.width - (unsigned)src_x ||
        src_height > img->image.height - (unsigned)src_y ||
        (unsigned)dest_x > s->width || (unsigned)dest_y > s->height ||
        dest_width > s->width - (unsigned)dest_x ||
        dest_height > s->height - (unsigned)dest_y) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    size_t row = (size_t)src_width * bpp;
    size_t bytes = row * src_height * 3 / 2;
    unsigned char *packed = malloc(bytes);
    if (!packed) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    int read_fd = -1;
    if (img->derived_from != VA_INVALID_ID) {
        struct dmd_surface *origin = dmd_find_surface_locked(drv, img->derived_from);
        if (!origin || origin->state == DMD_SURFACE_PENDING) {
            free(packed);
            pthread_mutex_unlock(&drv->lock);
            return origin ? VA_STATUS_ERROR_HW_BUSY : VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (origin->exportable)
            read_fd = origin->dumb_sync_fd;
    }
    struct dma_buf_sync read_sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    if (read_fd >= 0 && ioctl(read_fd, DMA_BUF_IOCTL_SYNC, &read_sync) != 0) {
        free(packed);
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    /* Stage the rectangle so an image derived from this same surface may
     * overlap the destination without corrupting subsequent source rows. */
    for (unsigned plane = 0; plane < 2; plane++) {
        unsigned rows = plane ? src_height / 2 : src_height;
        unsigned sy = plane ? (unsigned)src_y / 2 : (unsigned)src_y;
        size_t offset = plane ? row * src_height : 0;
        for (unsigned y = 0; y < rows; y++)
            memcpy(packed + offset + y * row,
                   img->data + img->image.offsets[plane] +
                   (size_t)(sy + y) * img->image.pitches[plane] + (size_t)src_x * bpp,
                   row);
    }
    read_sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    if (read_fd >= 0 && ioctl(read_fd, DMA_BUF_IOCTL_SYNC, &read_sync) != 0) {
        free(packed);
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    int sync_needed = s->exportable && s->dumb_sync_fd >= 0;
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
    if (sync_needed && ioctl(s->dumb_sync_fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        status = VA_STATUS_ERROR_OPERATION_FAILED;
    } else {
        for (unsigned plane = 0; plane < 2; plane++) {
            unsigned rows = plane ? dest_height / 2 : dest_height;
            unsigned dy = plane ? (unsigned)dest_y / 2 : (unsigned)dest_y;
            size_t dst_offset = plane ? (size_t)s->stride * s->slice_height : 0;
            size_t src_offset = plane ? row * src_height : 0;
            for (unsigned y = 0; y < rows; y++)
                memcpy(s->data + dst_offset + (size_t)(dy + y) * s->stride +
                       (size_t)dest_x * bpp, packed + src_offset + y * row, row);
        }
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
        if (sync_needed && ioctl(s->dumb_sync_fd, DMA_BUF_IOCTL_SYNC, &sync) != 0)
            status = VA_STATUS_ERROR_OPERATION_FAILED;
        s->state = DMD_SURFACE_READY;
        s->decode_status = status;
        s->context = VA_INVALID_ID;
        s->visible_x = s->visible_y = 0;
        s->visible_width = s->width;
        s->visible_height = s->height;
    }
    free(packed);
    pthread_mutex_unlock(&drv->lock);
    return status;
}

/* NV12 shares a geometric algorithm with P010, because **Stride here is always bytes**:
 * P010 line is a sample width x 2 bytes, so data_size = stride*slick*3/2,
 * Offsets [1] = branch*slick for both depths.
 * Only sourcecc and bits_per_pixel will be reported separately - consumers will be judged by sourcecc
 * Each sample has several bytes, which are misreported and re-sampled at the wrong width (symptoms are horizontal flattening). */
void dmd_fill_image_geometry(VAImage *img, unsigned int disp_width,
                             unsigned int disp_height, unsigned int stride,
                             unsigned int slice_height, int ten_bit)
{
    memset(img, 0, sizeof(*img));

    img->format.fourcc = ten_bit ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    img->format.byte_order = VA_LSB_FIRST;
    /* VA's bits_per_pixel keeps ** the notional value of the layout**, not the valid number:
     * P010 per pixel 1.5 x 16 = 24 bit? No - each driver (including Mesa) vs P010
     * Follow 12, same as NV12, the valid digits are expressed by sourcecc itself. Follow the custom.
     * The price of the food is not enough to keep the consumer from taking it for a quantum check. */
    img->format.bits_per_pixel = 12;

    /* Displays size: ffmpeg does not match buffer size by sw_format. */
    img->width = (uint16_t)disp_width;
    img->height = (uint16_t)disp_height;

    img->num_planes = 2; /* NV12:Y plane + knit UV plane */
    img->pitches[0] = stride;
    img->pitches[1] = stride; /* UV interwoven, 2 bytes grouping each line with the same line width as Y */
    img->offsets[0] = 0;
    /* Use slick_height (buffer 1088), not disp_height (1080). */
    img->offsets[1] = stride * slice_height;

    /* Data_size overwrite UV: Y is stride*slick_height,
     * UV is half of it (4:2:0 half plane). */
    img->data_size = stride * slice_height * 3 / 2;

    img->num_palette_entries = 0;
    img->entry_bytes = 0;
}

/* Take an empty Image slot and assign backup storage. The caller holds the lock.
 * Call oc when backing is NULL; borrow (derive scene, not owned) when not NULL. */
static struct dmd_image *image_alloc_locked(struct dmd_driver *drv,
                                            const VAImage *desc,
                                            unsigned char *backing)
{
    struct dmd_image *slot = NULL;
    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        if (!drv->images[i].in_use) {
            slot = &drv->images[i];
            break;
        }
    }
    if (!slot)
        return NULL;

    unsigned char *data = backing;
    if (!data) {
        data = calloc(1, desc->data_size);
        if (!data)
            return NULL;
    }

    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->id = (VAImageID)(drv->next_image_id++);
    /* Image. buff must be an ID that can be recognized by vamapBuffer. Use a separate Buffer ID space conference
     * Collapse with real buffer, so share next_buffer_id — Map Buffer first check the buffer table
     * Check the table for the image, and the ID on both sides is not overlapping. */
    slot->buf_id = (VABufferID)(drv->next_buffer_id++);
    slot->image = *desc;
    slot->image.image_id = slot->id;
    slot->image.buf = slot->buf_id;
    slot->data = data;
    slot->data_size = desc->data_size;
    slot->acquired_fd = -1;
    /* The data is not empty, but data belong to the search Image that cannot be free. */
    slot->derived_from = VA_INVALID_ID;

    return slot;
}

struct dmd_image *dmd_find_image_locked(struct dmd_driver *drv, VAImageID id)
{
    if (id == VA_INVALID_ID)
        return NULL;
    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        if (drv->images[i].in_use && drv->images[i].id == id)
            return &drv->images[i];
    }
    return NULL;
}

VAStatus dmd_CreateImage(VADriverContextP ctx, VAImageFormat *format, int width,
                         int height, VAImage *image)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !format || !image)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (width <= 0 || height <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (width > DMD_MAX_WIDTH || height > DMD_MAX_HEIGHT)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    /* NV12(8bit) and P010(10bit) are two linear semi-plane. The rest of the format is to be done in the drive
     * Colour/bit deep, that belongs to VPP, this driver is not stated. */
    if (format->fourcc != VA_FOURCC_NV12 && format->fourcc != VA_FOURCC_P010)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    const int ten_bit = format->fourcc == VA_FOURCC_P010;

    /* No surface is found here, but only geometry is derived by the rules of alignment of the decoder...
     * The preallocation with the surface is based on the same set of rules (128 wide, 32 high).
     * GetImage moves by line using the actual stride/ slice_height of the surface,
     * So even if the decoder is different, it's not different.
     * The stride is always ** bytes**, so 10bit just double it. */
    unsigned int stride =
        dmd_align_up((unsigned int)width, DMD_WIDTH_ALIGN) * (ten_bit ? 2u : 1u);
    unsigned int slice_h = dmd_align_up((unsigned int)height, DMD_HEIGHT_ALIGN);

    VAImage desc;
    dmd_fill_image_geometry(&desc, (unsigned int)width, (unsigned int)height,
                            stride, slice_h, ten_bit);

    pthread_mutex_lock(&drv->lock);
    struct dmd_image *img = image_alloc_locked(drv, &desc, NULL);
    if (!img) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("CreateImage: no free slots or allocation failed (limit %d)\n", DMD_MAX_IMAGES);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    *image = img->image;
    pthread_mutex_unlock(&drv->lock);

    dmd_log("CreateImage: %dx%d fourcc=%c%c%c%c stride=%u slice_h=%u "
            "offsets=[0,%u] size=%u -> image=%u buf=%u\n",
            width, height,
            (char)(format->fourcc & 0xFF), (char)((format->fourcc >> 8) & 0xFF),
            (char)((format->fourcc >> 16) & 0xFF), (char)((format->fourcc >> 24) & 0xFF),
            stride, slice_h, image->offsets[1], image->data_size,
            (unsigned)image->image_id, (unsigned)image->buf);

    return VA_STATUS_SUCCESS;
}

VAStatus dmd_DeriveImage(VADriverContextP ctx, VASurfaceID surface,
                         VAImage *image)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !image)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, surface);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (s->imported) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (surface_image_busy_locked(drv, surface, 0)) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }

    /* VLC 3's tc_vaegl_get_pool derives a newly allocated surface to test
     * EGL import before any BeginPicture. libva requires usable initialized
     * storage, not a decoded frame. Keep IDLE unchanged: this is allocation
     * access and must never count as a hardware/output completion.
     * Pending decode, in contrast, must really finish before exposing pixels.
     * See videolan/vlc 3.0.23 modules/video_output/opengl/converter_vaapi.c
     * and intel/libva va/va.h vaDeriveImage/vaAcquireBufferHandle. */
    if (s->state == DMD_SURFACE_PENDING) {
        pthread_mutex_unlock(&drv->lock);
        VAStatus wait = dmd_surface_wait(drv, surface);
        if (wait != VA_STATUS_SUCCESS)
            return wait;
        pthread_mutex_lock(&drv->lock);
        s = dmd_find_surface_locked(drv, surface);
        if (!s) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (s->imported || s->state == DMD_SURFACE_PENDING ||
            surface_image_busy_locked(drv, surface, 0)) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
    }
    if (s->decode_status != VA_STATUS_SUCCESS) {
        VAStatus status = s->decode_status;
        pthread_mutex_unlock(&drv->lock);
        return status;
    }
    const unsigned sample_bytes = s->format == VA_RT_FORMAT_YUV420_10 ? 2 : 1;
    if ((s->state != DMD_SURFACE_IDLE && s->state != DMD_SURFACE_READY) ||
        (s->format != VA_RT_FORMAT_YUV420 && s->format != VA_RT_FORMAT_YUV420_10) ||
        !s->data || !s->width || !s->height || !s->stride ||
        s->width > s->stride / sample_bytes || s->height > s->slice_height ||
        (s->slice_height & 1) ||
        (uint64_t)s->stride * s->slice_height * 3 / 2 > UINT32_MAX) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* The deive is "direct access to the memory of the surface," so borrow the surface->data,
     * No more distributions, no copies. Destroy Image can't free it.
     * Geometric surface**actual**stride/ slick_height --when the frame returns
     * These two values have been updated to the format block given by the decoder. */
    VAImage desc;
    dmd_fill_image_geometry(&desc, s->width, s->height, s->stride,
                            s->slice_height, s->format == VA_RT_FORMAT_YUV420_10);

    /* Not when the data_size is buffered.
     * Data_size read across the border. */
    if (desc.data_size > s->data_size) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("DeriveImage: surface %u buffer is %zu bytes, requires %u\n",
                (unsigned)surface, s->data_size, desc.data_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    struct dmd_image *img = image_alloc_locked(drv, &desc, s->data);
    if (!img) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    img->derived_from = surface;
    *image = img->image;
    pthread_mutex_unlock(&drv->lock);

    dmd_log("DeriveImage: surface=%u -> image=%u %ux%u stride=%u "
            "offsets=[0,%u]\n",
            (unsigned)surface, (unsigned)image->image_id, image->width,
            image->height, image->pitches[0], image->offsets[1]);

    return VA_STATUS_SUCCESS;
}

/* Legacy PRIME export used by VLC 3's allocation probe. This owns a separate
 * close-on-exec descriptor until the final matching ReleaseBufferHandle;
 * unlike ExportSurfaceHandle the caller must not close it. Repeated acquire
 * is reference counted, matching Intel's public media-driver convention.
 * The ioctl executes with the object lock held so neither the image nor its
 * DRM allocation can disappear while the handle is being created. */
VAStatus dmd_AcquireBufferHandle(VADriverContextP ctx, VABufferID buf_id,
                                  VABufferInfo *buf_info)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv || !buf_info)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (buf_info->mem_type &&
        !(buf_info->mem_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME))
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;

    pthread_mutex_lock(&drv->lock);
    struct dmd_image *img = NULL;
    for (int i = 0; i < DMD_MAX_IMAGES; i++)
        if (drv->images[i].in_use && drv->images[i].buf_id == buf_id) {
            img = &drv->images[i];
            break;
        }
    if (!img) {
        const VAStatus status = dmd_find_buffer_locked(drv, buf_id) ?
            VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE : VA_STATUS_ERROR_INVALID_BUFFER;
        pthread_mutex_unlock(&drv->lock);
        return status;
    }
    if (img->derived_from == VA_INVALID_ID) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    VASurfaceID parent = img->derived_from;
    struct dmd_surface *s = dmd_find_surface_locked(drv, parent);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (s->state == DMD_SURFACE_PENDING) {
        pthread_mutex_unlock(&drv->lock);
        VAStatus status = dmd_surface_wait(drv, parent);
        if (status != VA_STATUS_SUCCESS)
            return status;
        pthread_mutex_lock(&drv->lock);
        img = NULL;
        for (int i = 0; i < DMD_MAX_IMAGES; i++)
            if (drv->images[i].in_use && drv->images[i].buf_id == buf_id) {
                img = &drv->images[i];
                break;
            }
        s = dmd_find_surface_locked(drv, parent);
        if (!img || img->derived_from != parent || !s) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
    }
    if (s->decode_status != VA_STATUS_SUCCESS) {
        VAStatus status = s->decode_status;
        pthread_mutex_unlock(&drv->lock);
        return status;
    }
    if (s->state == DMD_SURFACE_PENDING || img->mapped) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (s->state != DMD_SURFACE_IDLE && s->state != DMD_SURFACE_READY) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!s->exportable || s->imported || !s->dumb_handle ||
        s->dumb_drm_fd < 0 || img->data != s->data ||
        img->data_size > s->dumb_size || img->data_size > s->data_size ||
        img->image.pitches[0] != s->stride ||
        img->image.offsets[1] != (uint64_t)s->stride * s->slice_height) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if (img->handle_acquired == INT_MAX) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    if (!img->handle_acquired) {
        if (surface_image_busy_locked(drv, parent, 1)) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
        struct drm_prime_handle prime = {
            .handle = s->dumb_handle, .flags = DRM_CLOEXEC | DRM_RDWR, .fd = -1
        };
        if (ioctl(s->dumb_drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0 ||
            prime.fd < 0) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        img->acquired_fd = prime.fd;
    }
    img->handle_acquired++;
    memset(buf_info, 0, sizeof(*buf_info));
    buf_info->handle = (uintptr_t)img->acquired_fd;
    buf_info->type = VAImageBufferType;
    buf_info->mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    buf_info->mem_size = s->dumb_size;
    pthread_mutex_unlock(&drv->lock);
    dmd_log("AcquireBufferHandle: image buffer=%u PRIME fd=%d\n",
            (unsigned)buf_id, (int)buf_info->handle);
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_ReleaseBufferHandle(VADriverContextP ctx, VABufferID buf_id)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&drv->lock);
    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        struct dmd_image *img = &drv->images[i];
        if (!img->in_use || img->buf_id != buf_id)
            continue;
        if (img->handle_acquired && --img->handle_acquired == 0) {
            close(img->acquired_fd);
            img->acquired_fd = -1;
        }
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_SUCCESS;
    }
    VAStatus status = dmd_find_buffer_locked(drv, buf_id) ?
        VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE : VA_STATUS_ERROR_INVALID_BUFFER;
    pthread_mutex_unlock(&drv->lock);
    return status;
}

VAStatus dmd_DestroyImage(VADriverContextP ctx, VAImageID image)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_image *img = dmd_find_image_locked(drv, image);
    if (!img) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    if (img->mapped || img->handle_acquired) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    /* The deive image borrows a surface buffer, never free. */
    unsigned char *owned =
        (img->derived_from == VA_INVALID_ID) ? img->data : NULL;
    memset(img, 0, sizeof(*img));
    pthread_mutex_unlock(&drv->lock);

    free(owned);
    return VA_STATUS_SUCCESS;
}

/* Half-floor 4:2 :00 line-by-line (NV12 shared with P010).
 *
 * Both strides are ** bytes** so the copy itself is not about bits; only to multiply
 * The sample_bytes is a x offset inside the plane -- it's the sample coordinates.
 * Source and purpose may differ
 * The rest would have to follow the lines when there was complete consistency. */
static void nv12_copy(unsigned char *dst, unsigned int dst_stride,
                      unsigned int dst_slice, const unsigned char *src,
                      unsigned int src_stride, unsigned int src_slice,
                      unsigned int x, unsigned int y, unsigned int w,
                      unsigned int h, unsigned int sample_bytes)
{
    /* All sample coordinates, final uniform by sample_bytes to bytes. */
    const unsigned int src_w_samples = src_stride / sample_bytes;
    const unsigned int dst_w_samples = dst_stride / sample_bytes;
    unsigned int row_samples = w < dst_w_samples ? w : dst_w_samples;
    if (row_samples > src_w_samples - x)
        row_samples = src_w_samples - x;
    const unsigned int row_bytes = row_samples * sample_bytes;
    const unsigned int xoff = x * sample_bytes;

    /* Y plane */
    for (unsigned int r = 0; r < h; r++) {
        memcpy(dst + (size_t)r * dst_stride,
               src + (size_t)(y + r) * src_stride + xoff, row_bytes);
    }

    /* UV plane: 4:2:0 half-plane, height halved, x must be 2 paired to not open U/V right. */
    unsigned char *duv = dst + (size_t)dst_stride * dst_slice;
    const unsigned char *suv = src + (size_t)src_stride * src_slice;
    unsigned int uv_h = h / 2;
    unsigned int uv_y = y / 2;
    for (unsigned int r = 0; r < uv_h; r++) {
        memcpy(duv + (size_t)r * dst_stride,
               suv + (size_t)(uv_y + r) * src_stride + xoff, row_bytes);
    }
}

/* P010 stores each 10-bit sample in the high bits of a little-endian word.
 * Neutral chroma is 512 << 6 = 0x8000; byte memset(128) gives 0x8080 and
 * writes nonzero padding bits. NV12 keeps its existing single-byte neutral. */
static void fill_chroma_neutral(unsigned char *dst, size_t bytes,
                                 unsigned int sample_bytes)
{
    if (sample_bytes == 1) {
        memset(dst, 128, bytes);
        return;
    }
    for (size_t offset = 0; offset + 1 < bytes; offset += 2) {
        dst[offset] = 0;
        dst[offset + 1] = 0x80;
    }
}

VAStatus dmd_GetImage(VADriverContextP ctx, VASurfaceID surface, int x, int y,
                      unsigned int width, unsigned int height, VAImageID image)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (x < 0 || y < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (width == 0 || height == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    /* The colour plane is sampled by 2x2 and the odd number starts by opening the U/V pair. */
    if ((x & 1) || (y & 1))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* As with DeriveImage, SyncSurface lets a surface through before it is ready,
     * The copy has to wait here until the frame really gets to it. */
    VAStatus wait = dmd_surface_wait(drv, surface);
    if (wait != VA_STATUS_SUCCESS)
        return wait;

    /* Diagnostic: Distinguishing the original surface buffers from the fall drive
     * "The decoder gives wrong" and "the geometry error of the time you cuffed VAImage." */
    {
        const char *dp = getenv("DMD_SURF_DUMP");
        if (dp) {
            pthread_mutex_lock(&drv->lock);
            struct dmd_surface *sd = dmd_find_surface_locked(drv, surface);
            if (sd && sd->data) {
                FILE *fp = fopen(dp, "wb");
                if (fp) {
                    fwrite(sd->data, 1, sd->data_size, fp);
                    fclose(fp);
                    dmd_log("SURF_DUMP: surface=%u %zu bytes "
                            "stride=%u slice=%u w=%u h=%u\n",
                            (unsigned)surface, sd->data_size,
                            sd->stride, sd->slice_height,
                            sd->buf_width, sd->buf_height);
                }
            }
            pthread_mutex_unlock(&drv->lock);
        }
    }

    pthread_mutex_lock(&drv->lock);

    struct dmd_surface *s = dmd_find_surface_locked(drv, surface);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (s->imported) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    struct dmd_image *img = dmd_find_image_locked(drv, image);
    if (!img) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    /* A different derived image may own the same parent allocation. Check
     * both operands: copying through an inactive alias must not bypass an
     * acquired handle or CPU map of that parent. */
    if (img->mapped || img->handle_acquired || img->derived_from == surface ||
        (img->derived_from != VA_INVALID_ID &&
         surface_image_busy_locked(drv, img->derived_from, 1)) ||
        surface_image_busy_locked(drv, surface, 0)) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }

    /* The search is still open.
     * ffmpeg will be vaSyncSurface first, but other consumers may not be. */
    if (s->state == DMD_SURFACE_PENDING) {
        VAStatus st = dmd_surface_sync_locked(drv, surface,
                                              DMD_FRAME_TIMEOUT_MS);
        if (st != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->lock);
            return st;
        }
        /* Unlock the lock during sync, re-search. */
        s = dmd_find_surface_locked(drv, surface);
        img = dmd_find_image_locked(drv, image);
        if (!s || !img) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (img->mapped || img->handle_acquired || img->derived_from == surface ||
            (img->derived_from != VA_INVALID_ID &&
             surface_image_busy_locked(drv, img->derived_from, 1)) ||
            surface_image_busy_locked(drv, surface, 0)) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
    }

    /* The depth of the image must be consistent with the depth of the surface: cross-positioning deep or disabling (10:8)
     * Either you read the trash (8 ff 10) or you don't drive it (that's VPP).
     * Consumers explicitly refuse to match P010 image with NV12 surface, and don't give half-to-one pixels. */
    const int img_ten_bit = img->image.format.fourcc == VA_FOURCC_P010;
    const int surf_ten_bit = s->format == VA_RT_FORMAT_YUV420_10;
    if (img->image.format.fourcc != VA_FOURCC_NV12 && !img_ten_bit) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (img_ten_bit != surf_ten_bit) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("GetImage: bit depth mismatch image=%.4s(%d) surface=%.4s(%d)\n",
                (const char *)&img->image.format.fourcc, img_ten_bit,
                surf_ten_bit ? "P010" : "NV12", surf_ten_bit);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    const unsigned int spb = surf_ten_bit ? 2u : 1u;

    /* The starting point must be inside the surface; size beyond ** to the real frame and fill the rest 0**,
     * Not a whole rejection.
     *
     * Why can't the request area be left completely within the buffer of the surface, as required by the old behavior:
     * AV1 ffmpeg to repeat the same VA contact, summer pool and VAImage when changing resolution
     * Still the 1920x1080 when the pool was built, and the frame came back shrunk to 1280x736.
     * At this point, both approaches are wrong:
     *   - Wrong report — the client can't get any pixels, the whole session is dead;
     *   - without cropping the surface line and moving the whole block * color level slick_height=1088
     *     To read, the true data is colored at 736 lines, and the result is that every frame is all wrong.
     *     25/25 frame md5 does not match, and the surface buffer itself is bytes.
     *      The right -- the flaw is in this case.
     * After cropping + zero, the client takes the true area of the upper left corner at the line spacing that you declare, and the pixels are right. */
    if ((unsigned int)x >= s->stride / spb || (unsigned int)y >= s->slice_height) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("GetImage: origin %d,%d is outside the %u×%u surface (%u bytes per row)\n",
                x, y, s->stride / spb, s->slice_height, s->stride);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    /* Target image must be fitted with the requested dimensions. */
    if (width > img->image.width || height > img->image.height) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    unsigned int copy_w = width, copy_h = height;
    const unsigned int src_w_samp = s->stride / spb;
    if ((unsigned int)x + copy_w > src_w_samp)
        copy_w = src_w_samp - (unsigned int)x;
    if ((unsigned int)y + copy_h > s->slice_height)
        copy_h = s->slice_height - (unsigned int)y;
    /* Samples of 2x2 colours, with odd widths that open U/V pairs and pull the number down. */
    copy_w &= ~1u;
    copy_h &= ~1u;

    /* Bottom: The color source level is in src + stride*slick, only data_size can prove it exists.
     * Normally, the situation has been secured and the system has been secured.
     * Data_size (the unfilled frame is not in the vault at all) and here is another path. */
    {
        unsigned int avail_rows =
            (unsigned int)(s->data_size / ((size_t)s->stride * 3 / 2));
        if (copy_h > avail_rows) copy_h = avail_rows & ~1u;
    }

    nv12_copy(img->data, img->image.pitches[0],
              img->image.offsets[1] / img->image.pitches[0], s->data, s->stride,
              s->slice_height, (unsigned int)x, (unsigned int)y, copy_w, copy_h,
              spb);

    if (copy_w < width || copy_h < height) {
        const unsigned int dp = img->image.pitches[0];
        const unsigned int ds = img->image.offsets[1] / dp;
        const size_t tail = ((size_t)width - copy_w) * spb;
        unsigned char *duv = img->data + (size_t)dp * ds;
        for (unsigned int r = 0; r < copy_h; r++)
            memset(img->data + ((size_t)y + r) * dp + ((size_t)x + copy_w) * spb,
                   0, tail);
        for (unsigned int r = copy_h; r < height; r++)
            memset(img->data + ((size_t)y + r) * dp + (size_t)x * spb, 0,
                   (size_t)width * spb);
        for (unsigned int r = 0; r < copy_h / 2; r++)
            fill_chroma_neutral(duv + ((size_t)y / 2 + r) * dp +
                                ((size_t)x + copy_w) * spb, tail, spb);
        for (unsigned int r = copy_h / 2; r < height / 2; r++)
            fill_chroma_neutral(duv + ((size_t)y / 2 + r) * dp + (size_t)x * spb,
                                (size_t)width * spb, spb);
    }

    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}
