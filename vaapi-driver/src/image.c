/* VAImage access to the driver's NV12/P010 surface storage.
 * Storage may be a DRM allocation or a CPU-only fallback. Derived images
 * borrow that storage; only actual DRM allocations can supply PRIME handles.
 *
 * ── ffmpeg 的实际回读路径（已读 hwcontext_vaapi.c 确证）──────
 * vaapi_frames_init 先建一个测试 surface 调 vaDeriveImage 探测（:696），
 * 成功且 fourcc 匹配则 derive_works=1（:700）；失败只是 debug 日志，不致命。
 * 但 vaapi_map_frame 的条件是
 *   derive_works && dst->format == sw_format && ((flags & MAP_DIRECT) || !(flags & MAP_READ))
 * hwdownload 是**读**访问，所以即使 derive_works=1 也仍走 else 分支：
 * vaCreateImage（:900）+ vaGetImage（:910）。两条路都必须实现。
 *
 * ── 1088 vs 1080：本文件最关键的一处 ────────────────────────
 * 解码器输出缓冲 1920x1088、显示 1920x1080、stride=1920、slice_height=1088。
 * VAImage.width/height 报**显示尺寸**（1920x1080，否则 ffmpeg 的尺寸校验失败），
 * 但 offsets[1] 必须用**缓冲高**：offsets[1] = stride * slice_height = 1920*1088。
 * 用 1080 算会让 UV 平面整体偏移 1920*8 字节 —— 症状是绿边、花屏、色度错位。
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

/* NV12 与 P010 共用同一套几何算式，因为**这里的 stride 一律是字节**：
 * P010 的一行是 样本宽×2 字节，于是 data_size = stride*slice*3/2、
 * offsets[1] = stride*slice 对两种位深都成立，不用分支。
 * 只有 fourcc 与 bits_per_pixel 要分开报 —— 消费者靠 fourcc 判断
 * 每样本几个字节，报错了它就会按错宽度重采样（症状是画面横向压扁）。 */
void dmd_fill_image_geometry(VAImage *img, unsigned int disp_width,
                             unsigned int disp_height, unsigned int stride,
                             unsigned int slice_height, int ten_bit)
{
    memset(img, 0, sizeof(*img));

    img->format.fourcc = ten_bit ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    img->format.byte_order = VA_LSB_FIRST;
    /* VA 的 bits_per_pixel 记的是**布局名义值**，不是有效位数：
     * P010 每像素 1.5×16 = 24 bit？不 —— 各家驱动（含 Mesa）对 P010
     * 沿用 12，与 NV12 相同，有效位数由 fourcc 本身表达。跟着惯例走，
     * 免得消费者拿它做量程校验时把我们拒掉。 */
    img->format.bits_per_pixel = 12;

    /* 报显示尺寸：ffmpeg 按 sw_format 的尺寸校验，报缓冲尺寸会不匹配。 */
    img->width = (uint16_t)disp_width;
    img->height = (uint16_t)disp_height;

    img->num_planes = 2; /* NV12：Y 平面 + 交织 UV 平面 */
    img->pitches[0] = stride;
    img->pitches[1] = stride; /* UV 交织，每行 2 字节一组，行宽与 Y 相同 */
    img->offsets[0] = 0;
    /* ⚠️ 用 slice_height（缓冲高 1088），不是 disp_height（1080）。 */
    img->offsets[1] = stride * slice_height;

    /* data_size 覆盖到 UV 平面结束：Y 是 stride*slice_height，
     * UV 是它的一半（4:2:0 半平面）。 */
    img->data_size = stride * slice_height * 3 / 2;

    img->num_palette_entries = 0;
    img->entry_bytes = 0;
}

/* 取一个空闲 image 槽位并分配后备存储。调用方持锁。
 * backing 为 NULL 时自行 calloc；非 NULL 时借用（derive 场景，不拥有）。 */
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
    /* image.buf 必须是能被 vaMapBuffer 认出的 ID。用独立的 buffer ID 空间会
     * 与真 buffer 撞号，所以共用 next_buffer_id —— MapBuffer 先查 buffer 表
     * 再查 image 表，两边 ID 不重叠即可。 */
    slot->buf_id = (VABufferID)(drv->next_buffer_id++);
    slot->image = *desc;
    slot->image.image_id = slot->id;
    slot->image.buf = slot->buf_id;
    slot->data = data;
    slot->data_size = desc->data_size;
    slot->acquired_fd = -1;
    /* backing 非空表示数据属于 surface，DestroyImage 不能 free。 */
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
    /* NV12(8bit) 与 P010(10bit) 两种线性半平面。其余格式要在驱动里做
     * 色彩/位深转换，那属于 VPP，本驱动不声明。 */
    if (format->fourcc != VA_FOURCC_NV12 && format->fourcc != VA_FOURCC_P010)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    const int ten_bit = format->fourcc == VA_FOURCC_P010;

    /* 这里拿不到 surface，只能按解码器的对齐规则推导几何 ——
     * 与 surface 的预分配用同一套规则（宽 128、高 32）。真实几何在
     * GetImage 时按 surface 的实际 stride/slice_height 逐行搬运，
     * 所以即使解码器给了不同对齐也不会错位。
     * stride 一律是**字节**，所以 10bit 只要把它翻倍。 */
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
        dmd_log("CreateImage: 槽位耗尽或分配失败（上限 %d）\n", DMD_MAX_IMAGES);
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

    /* derive 是"直接访问 surface 内存"，所以借用 surface->data，
     * 不另分配也不拷贝。DestroyImage 不得 free 它。
     * 几何用 surface 的**实际** stride/slice_height —— 帧回来后
     * 这两个值已按解码器给的格式块更新过。 */
    VAImage desc;
    dmd_fill_image_geometry(&desc, s->width, s->height, s->stride,
                            s->slice_height, s->format == VA_RT_FORMAT_YUV420_10);

    /* surface 缓冲装不下声明的 data_size 时不能 derive：调用方会按
     * data_size 读越界。 */
    if (desc.data_size > s->data_size) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("DeriveImage: surface %u 缓冲 %zu < 需要 %u，拒绝\n",
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
    /* derive 出来的 image 借用 surface 缓冲，绝不能 free。 */
    unsigned char *owned =
        (img->derived_from == VA_INVALID_ID) ? img->data : NULL;
    memset(img, 0, sizeof(*img));
    pthread_mutex_unlock(&drv->lock);

    free(owned);
    return VA_STATUS_SUCCESS;
}

/* 半平面 4:2:0 逐行搬运（NV12 与 P010 共用）。
 *
 * 两个 stride 都是**字节**，所以行拷贝本身与位深无关；唯一要乘
 * sample_bytes 的是平面内的 x 偏移 —— 它是样本坐标。
 * 源与目的可能有不同的 stride/slice_height，整块 memcpy 只在两者
 * 完全一致时才对，其余情况必须按行。 */
static void nv12_copy(unsigned char *dst, unsigned int dst_stride,
                      unsigned int dst_slice, const unsigned char *src,
                      unsigned int src_stride, unsigned int src_slice,
                      unsigned int x, unsigned int y, unsigned int w,
                      unsigned int h, unsigned int sample_bytes)
{
    /* 一切样本坐标，最后统一乘 sample_bytes 换成字节。 */
    const unsigned int src_w_samples = src_stride / sample_bytes;
    const unsigned int dst_w_samples = dst_stride / sample_bytes;
    unsigned int row_samples = w < dst_w_samples ? w : dst_w_samples;
    if (row_samples > src_w_samples - x)
        row_samples = src_w_samples - x;
    const unsigned int row_bytes = row_samples * sample_bytes;
    const unsigned int xoff = x * sample_bytes;

    /* Y 平面 */
    for (unsigned int r = 0; r < h; r++) {
        memcpy(dst + (size_t)r * dst_stride,
               src + (size_t)(y + r) * src_stride + xoff, row_bytes);
    }

    /* UV 平面：4:2:0 半平面，高度减半，x 必须按 2 对齐才不会拆开 U/V 对。 */
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
    /* 色度平面按 2x2 采样，奇数起点会把 U/V 对拆开。 */
    if ((x & 1) || (y & 1))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* 与 DeriveImage 同理：SyncSurface 会放行未就绪的 surface，
     * 拷像素之前必须在这里等到帧真的到手。 */
    VAStatus wait = dmd_surface_wait(drv, surface);
    if (wait != VA_STATUS_SUCCESS)
        return wait;

    /* 诊断：把 surface 原始缓冲整块落盘，用来区分
     * "解码器给的就不对" 和 "拷进 VAImage 时几何算错"。 */
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
                    dmd_log("SURF_DUMP: surface=%u %zu 字节 "
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

    /* surface 还没解出来就 GetImage：按 VA-API 语义隐式同步。
     * ffmpeg 会先 vaSyncSurface，但别的消费者未必。 */
    if (s->state == DMD_SURFACE_PENDING) {
        VAStatus st = dmd_surface_sync_locked(drv, surface,
                                              DMD_FRAME_TIMEOUT_MS);
        if (st != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->lock);
            return st;
        }
        /* sync 期间放过锁，重新查找。 */
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

    /* image 与 surface 的位深必须一致：跨位深搬运要么丢精度（10→8）
     * 要么读出垃圾（8→10），都不是本驱动该做的事（那是 VPP）。
     * 消费者拿 P010 image 配 NV12 surface 时明确拒绝，别给半对的像素。 */
    const int img_ten_bit = img->image.format.fourcc == VA_FOURCC_P010;
    const int surf_ten_bit = s->format == VA_RT_FORMAT_YUV420_10;
    if (img->image.format.fourcc != VA_FOURCC_NV12 && !img_ten_bit) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (img_ten_bit != surf_ten_bit) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("GetImage: 位深不符 image=%.4s(%d) surface=%.4s(%d)\n",
                (const char *)&img->image.format.fourcc, img_ten_bit,
                surf_ten_bit ? "P010" : "NV12", surf_ten_bit);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    const unsigned int spb = surf_ten_bit ? 2u : 1u;

    /* 起点必须落在 surface 内；尺寸超出则**裁到真实帧并把余下填 0**，
     * 而不是整笔拒绝。
     *
     * 为什么不能按旧行为要求"请求区域完全落在 surface 缓冲内"：
     * AV1 换分辨率时 ffmpeg 复用同一个 VA context，surface 池与 VAImage
     * 仍是建池时的 1920x1080，而帧回来时 surface 已缩到 1280x736。
     * 此时两种做法都错：
     *   - 报错 → 客户端拿不到任何像素，整个会话废掉；
     *   - 不裁剪地按 surface 行距整块搬 → 色度平面按 slice_height=1088
     *     去读，而真数据的色度在 736 行处，结果是每帧色度全错
     *     （实测换分辨率后 25/25 帧 md5 不符，而 surface 缓冲本身逐字节
     *      正确 —— 缺陷就在这一拷）。
     * 裁剪 + 补零后，客户端按自己声明的行距取左上角的真实区域，像素对得上。 */
    if ((unsigned int)x >= s->stride / spb || (unsigned int)y >= s->slice_height) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("GetImage: 起点 %d,%d 已在 surface 之外 %u×%u(%u 字节行)\n",
                x, y, s->stride / spb, s->slice_height, s->stride);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    /* 目标 image 必须装得下请求的尺寸。 */
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
    /* 色度 2x2 采样，奇数宽高会把 U/V 对拆开，向下取偶。 */
    copy_w &= ~1u;
    copy_h &= ~1u;

    /* 兜底：色度源平面在 src + stride*slice，只有 data_size 能证明它存在。
     * 正常情况下 surface_store_frame_locked 已保证 stride*slice*3/2 ≤
     * data_size（装不下的帧根本不会入库），这里防的是别的路径漏改。 */
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
