/* profile / entrypoint / config 查询与 config 对象管理
 *
 * 声明的能力严格对应 Android 侧 decode-daemon 能提供的解码器：
 * 协议 codec 取值 0=H.264 1=HEVC 2=VP9 3=VP8，四者均已真机端到端验证。
 * 高位深（HEVC Main10、VP9 Profile2、H.264 High10）未验证，因此不声明 ——
 * 谎报能力会让消费者选中我们然后失败，比不报更糟。
 */

#include <string.h>

#include "driver.h"

/* 本驱动支持的 profile 集合。顺序即 vainfo 的输出顺序。
 *
 * ⚠️ 只列**解码路径真的通了**的 profile。声明了但解不出来比不声明更糟 ——
 * 消费者会选中我们然后失败，而它本来可以直接走软解。
 *
 * HEVC（VAProfileHEVCMain）曾经列在这里，现已移除：码流转发未实现，
 * `vaEndPicture` 返回 UNIMPLEMENTED。这个虚报是有实际后果的 ——
 * Firefox 的 vaapitest 探针会把它读成 CODEC_HW_HEVC（GfxInfo.h:63 的 1<<8），
 * 实测探针输出 368 = H264|VP8|VP9|HEVC，于是 Firefox 认为我们能解 HEVC。
 * 等 HEVC 真正实现后再加回来。
 */
static const VAProfile dmd_profiles[] = {
    VAProfileH264ConstrainedBaseline,
    VAProfileH264Main,
    VAProfileH264High,
    VAProfileVP9Profile0,
    VAProfileVP8Version0_3,
};

#define DMD_NUM_PROFILES ((int)(sizeof(dmd_profiles) / sizeof(dmd_profiles[0])))

int dmd_profile_supported(VAProfile profile)
{
    for (int i = 0; i < DMD_NUM_PROFILES; i++) {
        if (dmd_profiles[i] == profile)
            return 1;
    }
    return 0;
}

/* profile → 协议 codec id。
 * 高位深 profile 未验证故不映射。
 * HEVCMain 保留映射但**不在 dmd_profiles 里声明**：映射本身无害，
 * 等 HEVC 码流转发实现后只需把 profile 加回声明表即可。 */
int dmd_profile_to_codec(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        return DMD_CODEC_H264;
    case VAProfileHEVCMain:
        return DMD_CODEC_HEVC;
    case VAProfileVP9Profile0:
        return DMD_CODEC_VP9;
    case VAProfileVP8Version0_3:
        return DMD_CODEC_VP8;
    default:
        return -1;
    }
}

VAStatus dmd_QueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list,
                                 int *num_profiles)
{
    if (!ctx || !profile_list || !num_profiles)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    memcpy(profile_list, dmd_profiles, sizeof(dmd_profiles));
    *num_profiles = DMD_NUM_PROFILES;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_QueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile,
                                    VAEntrypoint *entrypoint_list,
                                    int *num_entrypoints)
{
    if (!ctx || !entrypoint_list || !num_entrypoints)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (!dmd_profile_supported(profile)) {
        *num_entrypoints = 0;
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    /* 只做解码。编码走 MediaCodec 是另一个项目，这里不声明。 */
    entrypoint_list[0] = VAEntrypointVLD;
    *num_entrypoints = 1;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_GetConfigAttributes(VADriverContextP ctx, VAProfile profile,
                                 VAEntrypoint entrypoint,
                                 VAConfigAttrib *attrib_list, int num_attribs)
{
    if (!ctx || (num_attribs > 0 && !attrib_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_attribs < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (!dmd_profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (entrypoint != VAEntrypointVLD)
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

    /* VA-API 约定：调用方传入想查的 attribute type，driver 填 value；
     * 不支持的项必须填 VA_ATTRIB_NOT_SUPPORTED 而不是报错。 */
    for (int i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            /* MediaCodec 输出 8-bit 4:2:0 NV12。 */
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
            break;
        case VAConfigAttribMaxPictureWidth:
            attrib_list[i].value = DMD_MAX_WIDTH;
            break;
        case VAConfigAttribMaxPictureHeight:
            attrib_list[i].value = DMD_MAX_HEIGHT;
            break;
        case VAConfigAttribDecSliceMode:
            /* daemon 接收的是完整 Annex B 数据单元，对应 normal slice 模式。 */
            attrib_list[i].value = VA_DEC_SLICE_MODE_NORMAL;
            break;
        default:
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }

    return VA_STATUS_SUCCESS;
}

VAStatus dmd_CreateConfig(VADriverContextP ctx, VAProfile profile,
                          VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
                          int num_attribs, VAConfigID *config_id)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !config_id)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_attribs < 0 || (num_attribs > 0 && !attrib_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_attribs > DMD_MAX_CONFIG_ATTRIBUTES)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (!dmd_profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (entrypoint != VAEntrypointVLD)
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

    VAStatus status = VA_STATUS_ERROR_ALLOCATION_FAILED;

    pthread_mutex_lock(&drv->lock);
    for (int i = 0; i < DMD_MAX_CONFIGS; i++) {
        struct dmd_config *cfg = &drv->configs[i];
        if (cfg->in_use)
            continue;

        memset(cfg, 0, sizeof(*cfg));
        cfg->in_use = 1;
        /* ID 从 1 开始：0 留作 VA_INVALID_ID 之外的"未初始化"哨兵，
         * 避免调用方把 0 误当合法 config。 */
        cfg->id = (VAConfigID)(drv->next_config_id++);
        cfg->profile = profile;
        cfg->entrypoint = entrypoint;
        cfg->num_attribs = num_attribs;
        if (num_attribs > 0)
            memcpy(cfg->attribs, attrib_list,
                   (size_t)num_attribs * sizeof(VAConfigAttrib));

        *config_id = cfg->id;
        status = VA_STATUS_SUCCESS;
        break;
    }
    pthread_mutex_unlock(&drv->lock);

    if (status == VA_STATUS_SUCCESS)
        dmd_log("CreateConfig: profile=%d entrypoint=%d -> config=%u\n",
                (int)profile, (int)entrypoint, (unsigned)*config_id);
    else
        dmd_log("CreateConfig: 槽位耗尽（上限 %d）\n", DMD_MAX_CONFIGS);

    return status;
}

/* 在已加锁的前提下按 ID 查找 config。 */
static struct dmd_config *find_config_locked(struct dmd_driver *drv,
                                             VAConfigID id)
{
    for (int i = 0; i < DMD_MAX_CONFIGS; i++) {
        if (drv->configs[i].in_use && drv->configs[i].id == id)
            return &drv->configs[i];
    }
    return NULL;
}

VAStatus dmd_DestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    VAStatus status;

    pthread_mutex_lock(&drv->lock);
    struct dmd_config *cfg = find_config_locked(drv, config_id);
    if (cfg) {
        cfg->in_use = 0;
        status = VA_STATUS_SUCCESS;
    } else {
        status = VA_STATUS_ERROR_INVALID_CONFIG;
    }
    pthread_mutex_unlock(&drv->lock);

    return status;
}

VAStatus dmd_QueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id,
                                   VAProfile *profile, VAEntrypoint *entrypoint,
                                   VAConfigAttrib *attrib_list,
                                   int *num_attribs)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !profile || !entrypoint || !attrib_list || !num_attribs)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    VAStatus status;

    pthread_mutex_lock(&drv->lock);
    struct dmd_config *cfg = find_config_locked(drv, config_id);
    if (cfg) {
        *profile = cfg->profile;
        *entrypoint = cfg->entrypoint;
        *num_attribs = cfg->num_attribs;
        if (cfg->num_attribs > 0)
            memcpy(attrib_list, cfg->attribs,
                   (size_t)cfg->num_attribs * sizeof(VAConfigAttrib));
        status = VA_STATUS_SUCCESS;
    } else {
        status = VA_STATUS_ERROR_INVALID_CONFIG;
    }
    pthread_mutex_unlock(&drv->lock);

    return status;
}

VAStatus dmd_QuerySurfaceAttributes(VADriverContextP ctx, VAConfigID config,
                                    VASurfaceAttrib *attrib_list,
                                    unsigned int *num_attribs)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !num_attribs)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* config 必须有效 —— 消费者据此判断该配置下的 surface 能力。 */
    pthread_mutex_lock(&drv->lock);
    int valid = find_config_locked(drv, config) != NULL;
    pthread_mutex_unlock(&drv->lock);
    if (!valid)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    /* VA-API 约定的两段式查询：attrib_list 为 NULL 时只回报数量。 */
    const unsigned int needed = 5;
    if (!attrib_list) {
        *num_attribs = needed;
        return VA_STATUS_SUCCESS;
    }
    if (*num_attribs < needed) {
        *num_attribs = needed;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    unsigned int n = 0;

    attrib_list[n].type = VASurfaceAttribPixelFormat;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = VA_FOURCC_NV12;
    n++;

    attrib_list[n].type = VASurfaceAttribMinWidth;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = DMD_MIN_WIDTH;
    n++;

    attrib_list[n].type = VASurfaceAttribMinHeight;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = DMD_MIN_HEIGHT;
    n++;

    attrib_list[n].type = VASurfaceAttribMaxWidth;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = DMD_MAX_WIDTH;
    n++;

    attrib_list[n].type = VASurfaceAttribMaxHeight;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = DMD_MAX_HEIGHT;
    n++;

    *num_attribs = n;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_QueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list,
                               int *num_formats)
{
    if (!ctx || !format_list || !num_formats)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* 只支持 NV12：MediaCodec 的输出格式实测为 color-format 21
     * （COLOR_FormatYUV420SemiPlanar，线性 NV12）。 */
    memset(&format_list[0], 0, sizeof(format_list[0]));
    format_list[0].fourcc = VA_FOURCC_NV12;
    format_list[0].byte_order = VA_LSB_FIRST;
    format_list[0].bits_per_pixel = 12; /* 平面式 YUV420 每像素 12 bit */

    *num_formats = 1;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_QuerySubpictureFormats(VADriverContextP ctx,
                                    VAImageFormat *format_list,
                                    unsigned int *flags,
                                    unsigned int *num_formats)
{
    (void)format_list;
    (void)flags;

    if (!ctx || !num_formats)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* 不支持 subpicture（叠加字幕等）。返回 0 个格式而非报错：
     * 消费者据此跳过 subpicture 路径。 */
    *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_QueryDisplayAttributes(VADriverContextP ctx,
                                    VADisplayAttribute *attr_list,
                                    int *num_attributes)
{
    (void)attr_list;

    if (!ctx || !num_attributes)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* 无可调显示属性（亮度/对比度等由消费者自行处理）。 */
    *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_GetDisplayAttributes(VADriverContextP ctx,
                                  VADisplayAttribute *attr_list,
                                  int num_attributes)
{
    if (!ctx || (num_attributes > 0 && !attr_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* 我们不声明任何显示属性，因此任何查询都标记为不支持。 */
    for (int i = 0; i < num_attributes; i++)
        attr_list[i].flags = VA_DISPLAY_ATTRIB_NOT_SUPPORTED;

    return VA_STATUS_SUCCESS;
}

VAStatus dmd_SetDisplayAttributes(VADriverContextP ctx,
                                  VADisplayAttribute *attr_list,
                                  int num_attributes)
{
    (void)attr_list;
    (void)num_attributes;

    if (!ctx)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
}
