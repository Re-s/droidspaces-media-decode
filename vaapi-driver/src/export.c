/* vaExportSurfaceHandle：把 surface 导出成 dmabuf（DRM_PRIME_2）。
 *
 * 为什么必须实现：**Firefox 取帧只走这条路**。
 * `FFmpegVideoDecoder::CreateImageVAAPI`（FFmpegVideoDecoder.cpp:1649）拿到
 * 解码帧后立刻调 `GetVAAPISurfaceDescriptor` → `vaExportSurfaceHandle`，
 * 失败就返回 `NS_ERROR_DOM_MEDIA_DECODE_ERR`，播放器随即 ProcessFlush()
 * 并重建成**软解**。整个过程没有任何错误日志，也**没有回退到拷贝的路径** ——
 * 症状就是"硬解出 1 帧然后永久软解"，极难定位（我们靠给未实现桩加日志才发现）。
 *
 * ffmpeg 命令行不需要它：`hwdownload` 走 vaDeriveImage + vaMapBuffer。
 * 所以此前一直"ffmpeg 全绿、浏览器不动"。
 *
 * 这不是零拷贝。MediaCodec 那块输出内存我们拿不到 fd（容器 ION 不可用、
 * 无 /dev/dma_heap、NDK 也不暴露输出缓冲的 fd）。做法是 surface 本身就分配在
 * msm_drm 的 dumb buffer 里（见 decode.c 的 surface_alloc_dumb），
 * daemon 回传的帧直接拷进这块可导出内存 —— 拷贝次数和原来一样，
 * 但多了"能被 GL/合成器导入"这个关键能力。
 */

#include <stdlib.h>
#include <fcntl.h> /* O_CLOEXEC —— drm.h 的 DRM_CLOEXEC 是它的别名 */
#include <string.h>
#include <sys/ioctl.h>

#include <drm/drm.h>
#include <drm_fourcc.h>

#include "driver.h"

VAStatus dmd_ExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id,
                                 uint32_t mem_type, uint32_t flags,
                                 void *descriptor)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !descriptor)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* 只支持 DRM_PRIME_2。Firefox 传的正是这个；老的 DRM_PRIME（v1）
     * 结构不同，不声明支持比给个错结构安全。 */
    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        dmd_log("ExportSurfaceHandle: 不支持的 mem_type=0x%x\n", mem_type);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    /* ⚠️ 不要在这里等帧就绪。
     *
     * 原来这里调 dmd_surface_wait()，理由写的是"Firefox 是在拿到解码帧之后
     * 才导出的"。那对 Firefox 成立，但 **Chrome 的顺序正好相反**：它在
     * CreateSurfaces 之后、提交任何解码之前就调 vaExportSurfaceHandle 拿
     * dmabuf fd 去建输出纹理，拿到 fd 才开始喂码流。
     *
     * 于是 surface 还是 IDLE，dmd_surface_wait() 按设计拒绝（它防的是读一个
     * 从未解码的 surface），export 返回 INVALID_SURFACE，Chrome 侧报
     *     media/gpu/vaapi/vaapi_wrapper.cc:2756
     *     vaExportSurfaceHandle failed, VA error: invalid VASurfaceID
     * 随后整条硬解路径被放弃 —— 实测「送入 0 单元, 收到 0 帧」。
     *
     * 关键区别：**导出 dmabuf 不读像素**，只交出 fd 与几何描述。
     * 缓冲生命周期由 surface 持有，何时写入由解码决定；消费方本来就要
     * 等 vaSyncSurface 才采样。真正需要「必须已解码」这个前提的是
     * GetImage / DeriveImage 那条读像素的路径，那里的 IDLE 拦截不变。 */

    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, surface_id);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (!s->exportable || !s->dumb_handle || s->dumb_drm_fd < 0) {
        /* 分配时回落成普通 heap 了（dumb buffer 不可用）。
         * 明确报不支持，别给出一个假的 fd。 */
        pthread_mutex_unlock(&drv->lock);
        dmd_log("ExportSurfaceHandle: surface %u 不是可导出内存\n",
                (unsigned)surface_id);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    unsigned int stride = s->stride;
    unsigned int slice_h = s->slice_height;
    unsigned int disp_w = s->width;
    unsigned int disp_h = s->height;
    uint32_t handle = s->dumb_handle;
    int drm_fd = s->dumb_drm_fd;
    size_t total = s->dumb_size;
    pthread_mutex_unlock(&drv->lock);

    /* 导出 fd 时不持锁：ioctl 是内核调用，且调用方拿到 fd 后由它负责 close。 */
    struct drm_prime_handle prime;
    memset(&prime, 0, sizeof(prime));
    prime.handle = handle;
    /* DRM_CLOEXEC 就是 O_CLOEXEC 的别名（drm.h），用它避免额外包含 fcntl.h。
     * fd 必须 CLOEXEC：驱动跑在浏览器进程里，泄漏到子进程是安全问题。 */
    prime.flags = DRM_CLOEXEC;
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
        dmd_log("ExportSurfaceHandle: PRIME_HANDLE_TO_FD 失败\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    VADRMPRIMESurfaceDescriptor *desc =
        (VADRMPRIMESurfaceDescriptor *)descriptor;
    memset(desc, 0, sizeof(*desc));

    desc->fourcc = VA_FOURCC_NV12;
    desc->width = disp_w;
    desc->height = disp_h;

    /* 单个 dmabuf object 承载 Y 与 UV 两个平面。 */
    desc->num_objects = 1;
    desc->objects[0].fd = prime.fd;
    desc->objects[0].size = (uint32_t)total;
    desc->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;

    /* Firefox 传 VA_EXPORT_SURFACE_SEPARATE_LAYERS，要求 Y / UV 各成一层。
     * 合并成一层（NV12 单 layer）只有在对方传 COMPOSED_LAYERS 时才合适。 */
    if (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) {
        desc->num_layers = 1;
        desc->layers[0].drm_format = DRM_FORMAT_NV12;
        desc->layers[0].num_planes = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = stride;
        desc->layers[0].object_index[1] = 0;
        desc->layers[0].offset[1] = (uint32_t)stride * slice_h;
        desc->layers[0].pitch[1] = stride;
    } else {
        desc->num_layers = 2;
        /* Y 平面：8bpp 单通道 */
        desc->layers[0].drm_format = DRM_FORMAT_R8;
        desc->layers[0].num_planes = 1;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = stride;
        /* UV 平面：交织的两通道，宽度减半（每个采样 2 字节，
         * 所以 pitch 与 Y 相同）。offset 用 **slice_height** 不是显示高 ——
         * 与 VAImage.offsets[1] 同一个坑。 */
        desc->layers[1].drm_format = DRM_FORMAT_GR88;
        desc->layers[1].num_planes = 1;
        desc->layers[1].object_index[0] = 0;
        desc->layers[1].offset[0] = (uint32_t)stride * slice_h;
        desc->layers[1].pitch[0] = stride;
    }

    /* 诊断黑帧：DMD_VA_LUMA=1 时抽样算 Y 平面亮度均值并打进日志。
     *
     * 为什么需要在这里量：黑帧只在浏览器路径上出现，而 ffmpeg 路径
     * （六条流回归）永远看不到 —— 它不会像浏览器那样中途重启解码器。
     * 导出这一刻是驱动能看到最终像素的最后位置。
     * 纯诊断，默认关闭，不影响正常路径。 */
    if (getenv("DMD_VA_LUMA")) {
        const unsigned char *y = s->data;
        if (y && s->data_size >= (size_t)stride * 16) {
            double sum = 0; int cnt = 0;
            size_t lim = (size_t)stride * slice_h;
            if (lim > s->data_size) lim = s->data_size;
            for (size_t k = 0; k < lim; k += 1499) { sum += y[k]; cnt++; }
            double mean = cnt ? sum / cnt : 0;
            dmd_log("亮度: surface=%u 均值 %.1f%s\n",
                    (unsigned)surface_id, mean,
                    mean < 20.0 ? "  ← 黑帧!" : "");
        }
    }

    dmd_log("ExportSurfaceHandle: surface=%u -> fd=%d %ux%u stride=%u "
            "uv_offset=%u layers=%u\n",
            (unsigned)surface_id, prime.fd, disp_w, disp_h, stride,
            (unsigned)(stride * slice_h), desc->num_layers);

    return VA_STATUS_SUCCESS;
}
