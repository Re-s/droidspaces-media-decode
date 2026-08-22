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

/* 入口符号名必须是 __vaDriverInit_<major>_<minor>：libva 从当前版本向下
 * 逐个 dlsym（va/va.c 的 va_getDriverInitName + compatible_versions 循环），
 * 首个命中即用。libva 头文件没有提供生成该名字的宏，所以在这里自己拼 ——
 * 用 VA_MAJOR/MINOR_VERSION 拼接可随 libva 升级自动跟随，
 * 比硬编码 __vaDriverInit_1_22 更耐版本变化。
 *
 * -fvisibility=hidden 下必须显式标 default 可见性，否则符号不导出，
 * libva 会报 "has no function __vaDriverInit_1_0"。 */
#define DMD_CONCAT_(a, b, c) a##b##_##c
#define DMD_INIT_NAME_(maj, min) DMD_CONCAT_(__vaDriverInit_, maj, min)
#define DMD_DRIVER_INIT DMD_INIT_NAME_(VA_MAJOR_VERSION, VA_MINOR_VERSION)

__attribute__((visibility("default"))) VAStatus
DMD_DRIVER_INIT(VADriverContextP ctx);

__attribute__((visibility("default"))) VAStatus
DMD_DRIVER_INIT(VADriverContextP ctx)
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

    /* config ID 从 1 起，0 不发给调用方。 */
    drv->next_config_id = 1;

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
