/* DroidSpaces MediaCodec VA-API driver — 内部数据结构与公共声明
 *
 * 本驱动被 libva 以 dlopen 方式加载进消费者进程（vainfo / ffmpeg / Firefox），
 * 因此遵守插件规矩：不 exit/abort、不写 stdout、日志默认静默、全部入口线程安全。
 */

#ifndef DMD_DRIVER_H
#define DMD_DRIVER_H

#include <pthread.h>
#include <stdint.h>

#include <va/va.h>
#include <va/va_backend.h>
/* struct drm_state 定义在这里，va_backend.h 只做前向声明。 */
#include <va/va_drmcommon.h>

#include "stubs.h"

/* 驱动版本，随 vendor 串一起被消费者看到。
 * 注意 ffmpeg 按 vendor 串匹配 vaapi_driver_quirks 名单；我们不在名单内，
 * 走 standard behaviour，即要求语义标准。 */
#define DMD_DRIVER_VERSION "0.1.0"
#define DMD_VENDOR_STRING "DroidSpaces MediaCodec VA-API driver " DMD_DRIVER_VERSION

/* 设备硬件解码能力，来自 /vendor/etc/media_codecs.xml（真机取证）。
 * H.264/HEVC 解码器规格：96x96 ~ 8192x4320，并发实例 16，支持 adaptive-playback。 */
#define DMD_MIN_WIDTH 96
#define DMD_MIN_HEIGHT 96
#define DMD_MAX_WIDTH 8192
#define DMD_MAX_HEIGHT 4320

/* libva 要求 max_* 字段全部 > 0（va/va.c 的 CHECK_MAXIMUM），
 * 且这些值是消费者分配查询数组的依据，必须 >= 我们实际返回的数量。 */
#define DMD_MAX_PROFILES 16
#define DMD_MAX_ENTRYPOINTS 4
#define DMD_MAX_CONFIG_ATTRIBUTES 32
#define DMD_MAX_IMAGE_FORMATS 4
#define DMD_MAX_SUBPIC_FORMATS 4
#define DMD_MAX_DISPLAY_ATTRIBUTES 4

/* 同时存活的 config 对象上限。VA-API 的 config 极轻量，
 * 消费者通常只建 1-2 个；给足余量即可。 */
#define DMD_MAX_CONFIGS 32

/* 一个 VA-API config 对象：profile + entrypoint + 属性集合 */
struct dmd_config {
    int in_use;
    VAConfigID id;
    VAProfile profile;
    VAEntrypoint entrypoint;
    VAConfigAttrib attribs[DMD_MAX_CONFIG_ATTRIBUTES];
    int num_attribs;
};

/* 驱动私有数据，挂在 ctx->pDriverData */
struct dmd_driver {
    pthread_mutex_t lock; /* 保护 configs 与 next_config_id */
    struct dmd_config configs[DMD_MAX_CONFIGS];
    unsigned int next_config_id;
    int drm_fd; /* 来自 ctx->drm_state，仅记录，当前不做 ioctl */
};

/* 从 VADriverContext 取私有数据；ctx 或 pDriverData 为空时返回 NULL。 */
struct dmd_driver *dmd_get_driver(VADriverContextP ctx);

/* 日志：默认静默，设 DMD_VA_LOG=1 后输出到 stderr。
 * 绝不写 stdout —— 那会污染宿主进程的输出。 */
void dmd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- 已实现的 vtable 入口（其余在自动生成的 stubs.c 中） ---- */

VAStatus dmd_Terminate(VADriverContextP ctx);

VAStatus dmd_QueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list,
                                 int *num_profiles);

VAStatus dmd_QueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile,
                                    VAEntrypoint *entrypoint_list,
                                    int *num_entrypoints);

VAStatus dmd_GetConfigAttributes(VADriverContextP ctx, VAProfile profile,
                                 VAEntrypoint entrypoint,
                                 VAConfigAttrib *attrib_list, int num_attribs);

VAStatus dmd_CreateConfig(VADriverContextP ctx, VAProfile profile,
                          VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
                          int num_attribs, VAConfigID *config_id);

VAStatus dmd_DestroyConfig(VADriverContextP ctx, VAConfigID config_id);

VAStatus dmd_QueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id,
                                   VAProfile *profile, VAEntrypoint *entrypoint,
                                   VAConfigAttrib *attrib_list,
                                   int *num_attribs);

VAStatus dmd_QuerySurfaceAttributes(VADriverContextP ctx, VAConfigID config,
                                    VASurfaceAttrib *attrib_list,
                                    unsigned int *num_attribs);

VAStatus dmd_QueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list,
                               int *num_formats);

VAStatus dmd_QuerySubpictureFormats(VADriverContextP ctx,
                                    VAImageFormat *format_list,
                                    unsigned int *flags,
                                    unsigned int *num_formats);

VAStatus dmd_QueryDisplayAttributes(VADriverContextP ctx,
                                    VADisplayAttribute *attr_list,
                                    int *num_attributes);

VAStatus dmd_GetDisplayAttributes(VADriverContextP ctx,
                                  VADisplayAttribute *attr_list,
                                  int num_attributes);

VAStatus dmd_SetDisplayAttributes(VADriverContextP ctx,
                                  VADisplayAttribute *attr_list,
                                  int num_attributes);

/* ---- profiles.c 内部辅助 ---- */

/* 该 profile 是否由本驱动支持（即 Android 侧 MediaCodec 有对应硬件解码器）。 */
int dmd_profile_supported(VAProfile profile);

#endif /* DMD_DRIVER_H */
