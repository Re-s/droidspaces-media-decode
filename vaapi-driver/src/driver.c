/* DroidSpaces MediaCodec VA-API driver — 入口与 vtable 装配
 *
 * 无感发现机制（真机取证结论，改名会直接失效）：
 *   1. libva 用 DRM_IOCTL_VERSION 从 /dev/dri/renderD128 取到内核驱动名 "msm_drm"
 *   2. libva 的 DRM→VA 驱动名映射表（va/drm/va_drm_utils.c 的 map[]）没有 msm 条目，
 *      于是 fallback 为"原样使用内核驱动名"
 *   3. libva 只尝试唯一一个文件名，无 fallback：
 *      /usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so
 * 所以产物必须精确叫 msm_drm_drv_video.so，装到该目录后
 * 裸跑 vainfo / ffmpeg / Firefox 都会自动找到它，不需要任何环境变量。
 *
 * 入口符号名由 VA_DRIVER_INIT_FUNC 宏按头文件版本生成（当前展开为
 * __vaDriverInit_1_22）。libva 会从当前版本向下逐个尝试 __vaDriverInit_1_N，
 * 用宏而非硬编码可以随 libva 升级自动跟随。
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver.h"

/* 日志开关只读一次。driver 被 dlopen 进宿主进程，
 * 每帧调 getenv 是不必要的开销。 */
static int dmd_log_enabled = -1;

void dmd_log(const char *fmt, ...)
{
    if (dmd_log_enabled < 0) {
        const char *env = getenv("DMD_VA_LOG");
        dmd_log_enabled = (env && env[0] == '1') ? 1 : 0;
    }
    if (!dmd_log_enabled)
        return;

    va_list ap;
    va_start(ap, fmt);
    /* 只写 stderr：stdout 属于宿主程序，driver 不得污染。 */
    fputs("[dmd-va] ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

struct dmd_driver *dmd_get_driver(VADriverContextP ctx)
{
    if (!ctx || !ctx->pDriverData)
        return NULL;
    return (struct dmd_driver *)ctx->pDriverData;
}

VAStatus dmd_Terminate(VADriverContextP ctx)
{
    if (!ctx)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv) {
        /* init 中途失败后 libva 仍可能调到这里，必须安全返回。 */
        return VA_STATUS_SUCCESS;
    }

    dmd_log("Terminate\n");

    /* 释放全部对象：消费者理应自己销毁，但异常退出路径下不会。
     * driver 被 dlclose 后这些内存就再也回收不了，所以必须在这里兜。
     *
     * 顺序有讲究：context 先拆（它持有 V4L2 会话与设备 fd），
     * 再拆 image（可能借用 surface 缓冲），最后 surface 与 buffer。 */
    pthread_mutex_lock(&drv->lock);

    int leaked_ctx = 0, leaked_surf = 0, leaked_buf = 0, leaked_img = 0;

    for (int i = 0; i < DMD_MAX_CONTEXTS; i++) {
        if (drv->contexts[i].in_use) {
            leaked_ctx++;
            dmd_context_reset_locked(&drv->contexts[i]);
        }
    }
    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        struct dmd_image *img = &drv->images[i];
        if (!img->in_use)
            continue;
        leaked_img++;
        /* derive 的 image 借用 surface 缓冲，不能重复 free。 */
        if (img->derived_from == VA_INVALID_ID)
            free(img->data);
        memset(img, 0, sizeof(*img));
    }
    for (int i = 0; i < DMD_MAX_SURFACES; i++) {
        if (drv->surfaces[i].in_use) {
            leaked_surf++;
            dmd_surface_reset_locked(&drv->surfaces[i]);
        }
    }
    for (int i = 0; i < DMD_MAX_BUFFERS; i++) {
        if (drv->buffers[i].in_use) {
            leaked_buf++;
            free(drv->buffers[i].data);
            memset(&drv->buffers[i], 0, sizeof(drv->buffers[i]));
        }
    }
    pthread_mutex_unlock(&drv->lock);

    if (leaked_ctx || leaked_surf || leaked_buf || leaked_img)
        dmd_log("Terminate: 兜底回收 context=%d surface=%d buffer=%d image=%d"
                "（消费者未自行销毁）\n",
                leaked_ctx, leaked_surf, leaked_buf, leaked_img);

    pthread_cond_destroy(&drv->io_done);
    pthread_mutex_destroy(&drv->lock);
    free(drv);
    ctx->pDriverData = NULL;

    return VA_STATUS_SUCCESS;
}

/* 装配 vtable：覆盖 va_backend.h 中 VADriverVTable 的全部槽位。
 *
 * libva 在 vaInitialize 里逐个校验 39 个槽位非 NULL（va/va.c 的 CHECK_VTABLE），
 * 任一为 NULL 会导致整个初始化失败并 dlclose ——
 * 所以未实现的入口也必须指向返回 UNIMPLEMENTED 的桩函数。
 * 装配列表由 tools/gen_stubs.py 从头文件生成，避免手抄漏项。 */
static void dmd_init_vtable(struct VADriverVTable *vtable)
{
#include "vtable.inc"
}

/* 入口符号名必须是 __vaDriverInit_<major>_<minor>。libva 头文件没有提供生成
 * 该名字的宏（已确认 va_backend.h 只有 typedef VADriverInit），所以自己拼。
 *
 * 版本选择：libva 从自身版本 1.N 起**降序**逐个 dlsym 到 1.0
 * （va/va.c 的 va_getDriverInitName + compatible_versions 循环），首个命中即用。
 * 因此导出 __vaDriverInit_1_0 的兼容窗口最宽 —— 任意 libva 1.x 都能接受；
 * 只导出 1_22 则在 libva 低于 1.22 时不会被命中。
 * 两者都导出，成本为零：编译期版本一个（跟随头文件），1_0 一个（保底）。
 *
 * -fvisibility=hidden 下必须显式标 default 可见性，否则符号不导出，
 * libva 会报 "has no function __vaDriverInit_1_0"。 */
#define DMD_CONCAT_(a, b, c) a##b##_##c
#define DMD_INIT_NAME_(maj, min) DMD_CONCAT_(__vaDriverInit_, maj, min)
#define DMD_DRIVER_INIT DMD_INIT_NAME_(VA_MAJOR_VERSION, VA_MINOR_VERSION)

static VAStatus dmd_driver_init(VADriverContextP ctx);

__attribute__((visibility("default"))) VAStatus
DMD_DRIVER_INIT(VADriverContextP ctx)
{
    return dmd_driver_init(ctx);
}

/* 保底入口：让低于编译期版本的 libva 也能加载本驱动。
 * 若头文件本身就是 1.0，上面的宏已展开成同名函数，此处不再重复定义。 */
#if VA_MAJOR_VERSION != 1 || VA_MINOR_VERSION != 0
__attribute__((visibility("default"))) VAStatus
__vaDriverInit_1_0(VADriverContextP ctx)
{
    return dmd_driver_init(ctx);
}
#endif

static VAStatus dmd_driver_init(VADriverContextP ctx)
{
    if (!ctx || !ctx->vtable)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    struct dmd_driver *drv = calloc(1, sizeof(*drv));
    if (!drv)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    if (pthread_mutex_init(&drv->lock, NULL) != 0) {
        free(drv);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (pthread_cond_init(&drv->io_done, NULL) != 0) {
        pthread_mutex_destroy(&drv->lock);
        free(drv);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    /* 各类 ID 从 1 起，0 不发给调用方。
     * 各表用独立的 ID 空间，但 image 的 buf_id 借用 buffer 空间 ——
     * MapBuffer 要能用一个 ID 同时在两张表里查（见 image.c 的说明）。 */
    drv->next_config_id = 1;
    drv->next_surface_id = 1;
    drv->next_context_id = 1;
    drv->next_buffer_id = 1;
    drv->next_image_id = 1;

    /* drm_state 由 libva 填好（display_type 0x31 = DRM_RENDERNODES）。
     * 当前只记录 fd，解码路径接入后才会用到。 */
    drv->drm_fd = -1;
    if (ctx->drm_state) {
        const struct drm_state *drm = (const struct drm_state *)ctx->drm_state;
        drv->drm_fd = drm->fd;
    }

    ctx->pDriverData = drv;

    /* libva 传入 version_major/minor 为 0/0，driver 必须自己填。 */
    ctx->version_major = VA_MAJOR_VERSION;
    ctx->version_minor = VA_MINOR_VERSION;

    /* 这 5 个 max 字段必须全部 > 0（va/va.c 的 CHECK_MAXIMUM），
     * 且消费者据此分配查询数组，必须 >= 我们实际返回的数量。 */
    ctx->max_profiles = DMD_MAX_PROFILES;
    ctx->max_entrypoints = DMD_MAX_ENTRYPOINTS;
    ctx->max_attributes = DMD_MAX_CONFIG_ATTRIBUTES;
    ctx->max_image_formats = DMD_MAX_IMAGE_FORMATS;
    ctx->max_subpic_formats = DMD_MAX_SUBPIC_FORMATS;
    ctx->max_display_attributes = DMD_MAX_DISPLAY_ATTRIBUTES;

    /* str_vendor 必须非 NULL（CHECK_STRING）。
     * 注意 ffmpeg 按这个串匹配 vaapi_driver_quirks 名单；
     * 我们不在名单内 → 走 standard behaviour。 */
    ctx->str_vendor = DMD_VENDOR_STRING;

    dmd_init_vtable(ctx->vtable);

    /* display_type 在 VADriverContext 里是 unsigned long，用 %lx。 */
    dmd_log("init: display_type=0x%lx drm_fd=%d va=%d.%d vendor=%s\n",
            (unsigned long)ctx->display_type, drv->drm_fd, ctx->version_major,
            ctx->version_minor, ctx->str_vendor);

    return VA_STATUS_SUCCESS;
}
