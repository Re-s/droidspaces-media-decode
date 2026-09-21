/* VAImage 出口：surface 像素回读到 CPU 可访问内存
 *
 * ── 为什么必须走 CPU 路径 ──────────────────────────────────
 * 容器内 ION 完全不可用（legacy EINVAL / modern ENODEV），/dev/dma_heap 不存在
 * （内核 4.14），MediaCodec 的 NDK 公开 API 也拿不到输出缓冲的 dmabuf fd。
 * 所以零拷贝这条路是死的，vaExportSurfaceHandle 保持 UNIMPLEMENTED，
 * 像素一律经普通 heap 内存交付。
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

#include "driver.h"

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

    /* 真正的等帧点。SyncSurface 为了不与 MediaCodec 流水线深度死锁，
     * 对"已提交未就绪"的 surface 直接报成功放行（详见 decode.c 里
     * dmd_SyncSurface2 的注释）。所以像素必须在这里保证就绪 ——
     * 下面要读 s->stride / s->slice_height，它们只有帧回来后才是真值。
     * 必须在取锁之前调用：dmd_surface_wait 自己会加锁。 */
    VAStatus wait = dmd_surface_wait(drv, surface);
    if (wait != VA_STATUS_SUCCESS)
        return wait;

    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, surface);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
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
    struct dmd_image *img = dmd_find_image_locked(drv, image);
    if (!img) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_IMAGE;
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

    /* 请求区域必须落在 surface 缓冲内。stride 是字节，先折回样本坐标。 */
    if ((unsigned int)x + width > s->stride / spb ||
        (unsigned int)y + height > s->slice_height) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("GetImage: 区域 %d,%d %ux%u 超出 surface 缓冲 %u×%u(%u 字节行)\n",
                x, y, width, height, s->stride / spb, s->slice_height, s->stride);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    /* 目标 image 必须装得下请求的尺寸。 */
    if (width > img->image.width || height > img->image.height) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    /* derive 出来的 image 直接指向 surface 内存，同一 surface 时无需拷贝。 */
    if (img->derived_from == surface) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_SUCCESS;
    }

    nv12_copy(img->data, img->image.pitches[0],
              img->image.offsets[1] / img->image.pitches[0], s->data, s->stride,
              s->slice_height, (unsigned int)x, (unsigned int)y, width, height,
              spb);

    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}
