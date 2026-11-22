/* profile / entrypoint / config 查询与 config 对象管理
 *
 * 声明的能力严格对应 /dev/video32 上 msm_vidc 能提供的解码器：
 * 协议 codec 取值 0=H.264 1=HEVC 2=VP9 3=VP8(已废弃) 4=AV1。
 * V4L2 直通下已真机端到端验证：H.264 300/300、HEVC 12/12、VP9 P0 50/50
 * （均逐字节等于软解）。AV1 帧数一致但像素未通过，详见 doc/av1-v4l2-status.md。
 * VP8 未实现：驱动侧缺 VP8 的码流重建（RFC 6386 uncompressed chunk 合成）。
 * ⚠️ 注意不是硬件不支持 —— 实测 /dev/video32 的 OUTPUT 格式确实含 VP80，
 * 见本文件下方声明表的更正说明。
 * HEVC Main10/P010 已在 SM8750P 通过像素验证，使用 HEVC10=1 独立启用；
 * 默认仍关闭，待更多平台/应用验证。VP9 Profile2、H.264 High10 不在此范围。
 */

#include <string.h>

#include "dmd_client.h"
#include "v4l2_backend.h"
#include "driver.h"
#include "v4l2_encoder.h"

static int encode_profile_index(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline: return DMD_ENC_H264_BASELINE;
    case VAProfileH264Main: return DMD_ENC_H264_MAIN;
    case VAProfileH264High: return DMD_ENC_H264_HIGH;
    case VAProfileHEVCMain: return DMD_ENC_HEVC_MAIN;
    case VAProfileVP9Profile0: return DMD_ENC_VP9_0;
    default: return -1;
    }
}

static int encode_profile_supported(VADriverContextP ctx, VAProfile profile)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    int p = encode_profile_index(profile);
    return drv && p >= 0 && (drv->encode_caps & DMD_ENC_CAP(p));
}

static void encode_dimensions(VAProfile profile, unsigned *min_w, unsigned *min_h,
                               unsigned *max_w, unsigned *max_h)
{
    *min_w = DMD_MIN_WIDTH;
    *min_h = DMD_MIN_HEIGHT;
    *max_w = DMD_MAX_WIDTH;
    *max_h = DMD_MAX_HEIGHT;
    unsigned a, b, c, d;
    int p = encode_profile_index(profile);
    if (p >= 0 && dmd_v4l2_enc_get_limits(p, &a, &b, &c, &d) == 0) {
        if (a > *min_w) *min_w = a;
        if (b > *min_h) *min_h = b;
        if (c < *max_w) *max_w = c;
        if (d < *max_h) *max_h = d;
    }
}

/* 本驱动支持的 profile 集合。顺序即 vainfo 的输出顺序。
 *
 * ⚠️ 只列**解码路径真的通了**的 profile。声明了但解不出来比不声明更糟 ——
 * 消费者会选中我们然后失败，而它本来可以直接走软解。
 *
 * HEVC（VAProfileHEVCMain）曾因"码流转发未实现"被移除过一段时间 ——
 * 那时声明它属于虚报，Firefox 的 vaapitest 探针会读成 CODEC_HW_HEVC
 * （GfxInfo.h:63 的 1<<8）。现在 hevc_bitstream.c 已实现 VPS/SPS/PPS 合成
 * 并真机验证（1080p/4K/720p 逐字节一致、长流 1500 帧、seek 全通），
 * 所以重新声明。
 * （那句"探针输出 368 = H264|VP8|VP9|HEVC 名实相符"已过时：
 *   VP8 声明已移除，探针位图不再包含 CODEC_HW_VP8。）
 *
 * HEVC 的 SPS 参考集可在受支持的 Main 8 位流中，根据 VA DPB/POC
 * 重建为显式切片内参考集。SPS 长期参考表、自定义缩放表和未知切片
 * 边界等仍须经过 dmd_hevc_can_build / hevc_slice.c 的限制检查；
 * Main10 复用这些限制，并额外严格核对 10 位参数与 P010 输出；
 * 不能将受限支持解释为任意 HEVC 流均已验证。
 */
static const VAProfile dmd_profiles[] = {
    VAProfileH264ConstrainedBaseline,
    VAProfileH264Main,
    VAProfileH264High,
    VAProfileHEVCMain,
    VAProfileVP9Profile0,
#if defined(DMD_ENABLE_HEVC_MAIN10) || defined(DMD_PROBE_10BIT_PROFILES)
    /* HEVC10=1: independently gated Main10/P010 decode validation. This does
     * not enable 10-bit encoding, VP9 Profile2, or browser plane imports. */
    VAProfileHEVCMain10,
#endif
#ifdef DMD_PROBE_10BIT_PROFILES
    /* 探测专用（-DDMD_PROBE_10BIT_PROFILES）：声明 10bit profile 好让
     * ffmpeg 愿意把 Main10 / VP9 Profile2 码流交过来，配合运行时的
     * DMD_PROBE_10BIT=1 判定固件是否真能解。
     * 此旧诊断开关同时声明尚未验证的 VP9 Profile2；Main10 请用独立
     * HEVC10=1。QP10/Q12A 等私有探测布局不属于标准 P010 支持。 */
    VAProfileVP9Profile2,
#endif
    /* VP8：0.4.2 重新声明。
     *
     * 此前不声明的理由（见下方原注释）是"驱动侧没写码流重建"，但那已经
     * 不成立 —— decode.c 的 vp8_build_frame() 实现了 RFC 6386 §9.1 的
     * uncompressed data chunk 合成（frame tag + key frame 的 start code
     * 与尺寸），参数收集（VASliceParameterBufferVP8 /
     * VAPictureParameterBufferVP8）与 build_unit 的 DMD_CODEC_VP8 分支
     * 也都在位。缺的只是这里的声明和 dmd_profile_to_codec 的映射两道闸门。
     *
     * 硬件侧同样确认可用：VIDIOC_ENUM_FMT 在 /dev/video32 的 OUTPUT 侧
     * 列出 MPG2/H264/HEVC/VP80/VP90，VP80 在列。 */
    VAProfileVP8Version0_3,
#ifdef DMD_ENABLE_MPEG2
    /* MPEG-2：⚠️ **暂不声明**（需 -DDMD_ENABLE_MPEG2 才启用）。
     *
     * 实现是完整的：mpeg2_bitstream.c 合成出的码流与 ffmpeg 原始流
     * **逐字节完全一致**（实测第 1 帧 82620 字节、第 2 帧 129038 字节，
     * 差异数 0；层次 seq → ext → GOP → picture → ext → slice 也一致）。
     * 合成正确性因此可以认为已证实。
     *
     * 但送入 /dev/video32 后 Venus 固件在第 2 个单元处报 SYS_ERROR，
     * 一帧不吐。既然码流与固件自己能解的原始流没有任何字节差异，
     * 问题不在合成，而在这颗固件（SM8150）对 MPG2 的送料路径 ——
     * 待查方向：OUTPUT sizeimage 只有 1958400（H.264/HEVC 是 16588800），
     * 而单帧实际 129038 字节虽然装得下，但固件可能对 MPEG-2 要求
     * 按 GOP/序列边界而非逐帧送料。
     *
     * 在能真正出帧之前声明它属于虚报：ffmpeg 的 -hwaccel vaapi 遇到
     * 已声明的 profile 不会回落软解，会直接失败退出。 */
    VAProfileMPEG2Main,
#endif
#ifdef DMD_ENABLE_AV1
    /* 开发/调试专用：-DDMD_ENABLE_AV1 才声明 AV1。
     * 发布版不声明（理由见下方注释），但本设备仍需要能直接测硬件链路，
     * 所以用编译期开关而不是删掉代码 —— 免得每次调试都手改源文件。 */
    VAProfileAV1Profile0,
#endif
    /* ⚠️ AV1 暂不声明（0.4.0 发布版）。
     *
     * AV1 硬解在本设备上确实可用，但驱动侧的码流合成尚未完全正确：
     * 实测帧数与 dav1d 一致（150），码流合成 145/150 逐字节相同，
     * 而像素只有 17/30 个不同画面（软解 30）——
     * 缺的是 70 个 show_existing_frame 复显帧，详见 doc/av1-v4l2-status.md。
     *
     * 在像素校验通过之前声明 AV1 属于虚报：Firefox / ffmpeg 会据此
     * 把 AV1 解码任务交过来，得到的是重复帧与花屏，比回落软解更糟。
     * 映射（dmd_profile_to_codec 的 VAProfileAV1Profile0 分支）保留 ——
     * 它本身无害，等像素通过后把这一行加回声明表即可。
     * 开发调试用 -DDMD_ENABLE_AV1 编译即可声明（见上方 #ifdef），
     * 不必手改源文件。
     *
     * ⚠️ 实测确认的一个行为差异：发布版（不声明 AV1）下，
     * ffmpeg 的 `-hwaccel vaapi` 遇到未声明的 profile **不会静默回落**，
     * 而是直接失败退出（rc=69）：
     *     No support for codec av1 profile 0.
     *     Your platform doesn't support hardware accelerated AV1 decoding.
     * 这是 ffmpeg 的既有语义（-hwaccel 是硬性要求，非"尽力而为"），
     * 不是本驱动的缺陷 —— 同一码流不带 -hwaccel 时纯软解正常出 30 帧。
     * 若要自动回落需用 `-hwaccel auto` 或不指定 -hwaccel。
     * Firefox 走的是能力探测路径，不声明即不会尝试，不受此影响。
     *
     * ⚠️ VP8 已移除声明（V4L2 直通改造的一部分）。
     *
     * ⚠️⚠️ 更正（第 80 轮实测）：此前这里写"msm_vidc 的 V4L2 层没有
     * VP80 格式"，**那是错的**。在本机 /dev/video32 上用
     * VIDIOC_ENUM_FMT 枚举 OUTPUT 侧格式，实测输出：
     *     MPG2  H264  HEVC  VP80  VP90
     * VP80 明确在列，硬件路径是存在的。
     * （节点身份已核实：driver=msm_vidc_driver card=msm_vidc_vdec，
     *   不是枚举失败或探测了错的节点。）
     *
     * 真正不声明 VP8 的理由是**驱动侧没实现它的码流重建**：
     * VA-API 只给 partition 数据，缺 RFC 6386 §9.1 的 uncompressed chunk，
     * 需要像 h264/hevc/av1 那样单独写一个合成器，目前没写。
     * v4l2_backend.c 的 codec 映射对 VP8 返回 0 是这个决定的结果，
     * 不是硬件能力的反映 —— 此前把它当成了硬件事实，属于因果倒置。
     *
     * 所以在实现合成器之前仍不声明（声明了会虚报，Firefox 的 vaapitest
     * 探针会据此报告 CODEC_HW_VP8 并把任务交过来，然后失败），
     * 但这是**待实现**而非**不可能**。
     *
     * 协议 codec 编号 3 保留不复用（见 dmd_client.h:63、
     * v4l2_backend.h:46），以免与旧 daemon 的线协议撞号。
     * dmd_profile_to_codec 里的映射也一并移除：
     * 留着会让 vaCreateConfig 对 VP8 返回成功。 */
};

#define DMD_NUM_PROFILES ((int)(sizeof(dmd_profiles) / sizeof(dmd_profiles[0])))

int dmd_profile_supported(VAProfile profile)
{
    /* 与 dmd_QueryConfigProfiles 的运行时过滤保持一致：固件不支持的
     * codec 即使在静态表里也不算支持。 */
    if (profile == VAProfileMPEG2Main && !dmd_v4l2_probe(DMD_CODEC_MPEG2))
        return 0;
    if (profile == VAProfileVP8Version0_3 && !dmd_v4l2_probe(DMD_CODEC_VP8))
        return 0;
    if (profile == VAProfileAV1Profile0 && !dmd_v4l2_probe(DMD_CODEC_AV1))
        return 0;
    for (int i = 0; i < DMD_NUM_PROFILES; i++)
    {
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
    /* VP8：0.4.2 恢复映射，码流重建见 decode.c 的 vp8_build_frame()。 */
    case VAProfileVP8Version0_3:
        return DMD_CODEC_VP8;
    /* MPEG-2：Simple 与 Main 走同一条 V4L2 路径（MPG2 fourcc）。
     * Simple 只是限制了 B 帧与色度格式，码流语法是 Main 的真子集。 */
    case VAProfileMPEG2Simple:
    case VAProfileMPEG2Main:
        return DMD_CODEC_MPEG2;
#if defined(DMD_ENABLE_HEVC_MAIN10) || defined(DMD_PROBE_10BIT_PROFILES)
    case VAProfileHEVCMain10:
        return DMD_CODEC_HEVC;
#endif
#ifdef DMD_PROBE_10BIT_PROFILES
    /* Legacy standalone VP9 Profile2 probe; HEVC10 does not enable this. */
    case VAProfileVP9Profile2:
        return DMD_CODEC_VP9;
#endif
    /* AV1 Profile0 = Main（8/10bit 4:2:0），Profile1 = High（含 4:4:4）。
     * 只映射 Profile0：MediaCodec 的 video/av01 不区分 profile，由码流
     * 序列头自述，但 4:4:4 的输出格式不是 NV12，本驱动的帧回传假设不成立。 */
    case VAProfileAV1Profile0:
        return DMD_CODEC_AV1;
    default:
        return -1;
    }
}

VAStatus dmd_QueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list,
                                 int *num_profiles)
{
    if (!ctx || !profile_list || !num_profiles)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* 运行时能力过滤：静态表只是"驱动实现了码流重建"的声明，最终能不能
     * 解还要看本机固件。内核 6.6 的新 msm_vidc 上 MPG2/VP80 已从 OUTPUT
     * 枚举消失（S_FMT 静默回落 H.264，dmesg 报 "unsupported codec"），
     * 按静态表声明的 VP8 会让消费者把任务交过来然后失败 —— 比不声明
     * 更糟。probe 用 ENUM_FMT 一次拿全，代价可忽略。 */
    static int probed = 0;
    static unsigned probed_mask = 0;   /* bit: DMD_CODEC_* 值 */
    if (!probed) {
        static const struct { VAProfile p; int codec; } map[] = {
            { VAProfileMPEG2Main, DMD_CODEC_MPEG2 },
            { VAProfileVP8Version0_3, DMD_CODEC_VP8 },
            { VAProfileAV1Profile0, DMD_CODEC_AV1 },
        };
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (dmd_v4l2_probe(map[i].codec))
                probed_mask |= 1u << map[i].codec;
        probed = 1;
    }

    int n = 0;
    for (int i = 0; i < DMD_NUM_PROFILES; i++) {
        VAProfile p = dmd_profiles[i];
        if (p == VAProfileMPEG2Main && !(probed_mask & (1u << DMD_CODEC_MPEG2)))
            continue;
        if (p == VAProfileVP8Version0_3 && !(probed_mask & (1u << DMD_CODEC_VP8)))
            continue;
        if (p == VAProfileAV1Profile0 && !(probed_mask & (1u << DMD_CODEC_AV1)))
            continue;
        profile_list[n++] = p;
    }
    *num_profiles = n;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_QueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile,
                                    VAEntrypoint *entrypoint_list,
                                    int *num_entrypoints)
{
    if (!ctx || !entrypoint_list || !num_entrypoints)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (profile == VAProfileNone) {
        entrypoint_list[0] = VAEntrypointVideoProc;
        *num_entrypoints = 1;
        return VA_STATUS_SUCCESS;
    }

    int decode = dmd_profile_supported(profile);
    int encode = encode_profile_supported(ctx, profile);
    if (!decode && !encode) {
        *num_entrypoints = 0;
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    int n = 0;
    if (decode)
        entrypoint_list[n++] = VAEntrypointVLD;
    if (encode)
        entrypoint_list[n++] = VAEntrypointEncSlice;
    *num_entrypoints = n;
    return VA_STATUS_SUCCESS;
}

/* 该 profile 允许的 RTFormat。
 *
 * HEVC Main10 and AV1 Profile0 can carry 8- or 10-bit samples. The frame's
 * syntax and target surface select NV12/P010; this does not imply conversion
 * between bit depths. Chrome also requires YUV420 in Main10 capability/config
 * probes even when its actual ten-bit frames use P010 surfaces. */
static uint32_t rt_formats_for_profile(VAProfile profile)
{
    if (profile == VAProfileHEVCMain10)
        return VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10;
    if (profile == VAProfileAV1Profile0)
        return VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10;
    return VA_RT_FORMAT_YUV420;
}

static uint32_t encode_rate_controls(VAProfile profile)
{
    return VA_RC_CBR | VA_RC_VBR |
           (profile == VAProfileVP9Profile0 ? 0 : VA_RC_CQP);
}

VAStatus dmd_GetConfigAttributes(VADriverContextP ctx, VAProfile profile,
                                 VAEntrypoint entrypoint,
                                 VAConfigAttrib *attrib_list, int num_attribs)
{
    if (!ctx || (num_attribs > 0 && !attrib_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_attribs < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    const int vpp = profile == VAProfileNone && entrypoint == VAEntrypointVideoProc;
    if (!vpp && !dmd_profile_supported(profile) && !encode_profile_supported(ctx, profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    const int encoding = entrypoint == VAEntrypointEncSlice;
    if ((encoding && !encode_profile_supported(ctx, profile)) ||
        (!encoding && !vpp && (entrypoint != VAEntrypointVLD || !dmd_profile_supported(profile))))
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

    unsigned min_w, min_h, max_w = DMD_MAX_WIDTH, max_h = DMD_MAX_HEIGHT;
    if (encoding)
        encode_dimensions(profile, &min_w, &min_h, &max_w, &max_h);

    /* VA-API 约定：调用方传入想查的 attribute type，driver 填 value；
     * 不支持的项必须填 VA_ATTRIB_NOT_SUPPORTED 而不是报错。 */
    for (int i = 0; i < num_attribs; i++) {
        if (encoding) {
            switch (attrib_list[i].type) {
            case VAConfigAttribRTFormat: attrib_list[i].value = VA_RT_FORMAT_YUV420; break;
            case VAConfigAttribRateControl: attrib_list[i].value = encode_rate_controls(profile); break;
            case VAConfigAttribEncMaxRefFrames: attrib_list[i].value = 1; break;
            case VAConfigAttribEncMaxSlices: attrib_list[i].value = 1; break;
            case VAConfigAttribEncPackedHeaders:
            case VAConfigAttribEncInterlaced:
            case VAConfigAttribEncSliceStructure: attrib_list[i].value = 0; break;
            case VAConfigAttribPredictionDirection:
                attrib_list[i].value = VA_PREDICTION_DIRECTION_PREVIOUS; break;
            case VAConfigAttribMaxPictureWidth: attrib_list[i].value = max_w; break;
            case VAConfigAttribMaxPictureHeight: attrib_list[i].value = max_h; break;
            default: attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED; break;
            }
            continue;
        }
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            attrib_list[i].value = rt_formats_for_profile(profile);
            break;
        case VAConfigAttribMaxPictureWidth:
            attrib_list[i].value = DMD_MAX_WIDTH;
            break;
        case VAConfigAttribMaxPictureHeight:
            attrib_list[i].value = DMD_MAX_HEIGHT;
            break;
        case VAConfigAttribDecSliceMode:
            /* daemon 接收的是完整 Annex B 数据单元，对应 normal slice 模式。 */
            attrib_list[i].value = vpp ? VA_ATTRIB_NOT_SUPPORTED : VA_DEC_SLICE_MODE_NORMAL;
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

    const int vpp = profile == VAProfileNone && entrypoint == VAEntrypointVideoProc;
    if (!vpp && !dmd_profile_supported(profile) && !encode_profile_supported(ctx, profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    const int encoding = entrypoint == VAEntrypointEncSlice;
    if ((encoding && !encode_profile_supported(ctx, profile)) ||
        (!encoding && !vpp && (entrypoint != VAEntrypointVLD || !dmd_profile_supported(profile))))
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

    if (encoding) {
        for (int i = 0; i < num_attribs; i++) {
            uint32_t value = attrib_list[i].value;
            switch (attrib_list[i].type) {
            case VAConfigAttribRTFormat:
                if (value != VA_RT_FORMAT_YUV420)
                    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
                break;
            case VAConfigAttribRateControl:
                if (!value || (value & (value - 1)) || !(value & encode_rate_controls(profile)))
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                break;
            case VAConfigAttribEncMaxRefFrames:
                if (value > 1) return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                break;
            case VAConfigAttribEncMaxSlices:
                if (value != 1) return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                break;
            case VAConfigAttribEncPackedHeaders:
            case VAConfigAttribEncInterlaced:
            case VAConfigAttribEncSliceStructure:
                if (value) return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                break;
            default:
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
        }
    }

    if (!encoding && profile == VAProfileHEVCMain10) {
        for (int i = 0; i < num_attribs; i++)
            if (attrib_list[i].type == VAConfigAttribRTFormat &&
                (!attrib_list[i].value ||
                 (attrib_list[i].value & ~rt_formats_for_profile(profile))))
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

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

        /* 回报**驱动支持的属性**，不是回显 CreateConfig 的入参。
         *
         * 这里曾直接把 cfg->attribs 抄回去 —— 对 ffmpeg 与 Firefox 有效，
         * 因为它们建 config 时自己就传了 RTFormat，回显恰好等于真值。
         *
         * 但 Chrome **不传任何属性**建 config，然后调本函数查询驱动
         * 支持什么。回显模式下它拿到 num_attribs=0，读不到 RTFormat，
         * 于是 FillProfileInfo_Locked 判定该 profile 不可用 ——
         * 六个 profile 全部枚举失败，硬解完全不可用。
         *
         * VA-API 的语义是"驱动声明能力"，回显是错的。现在无论
         * CreateConfig 传了什么，RTFormat 一定出现在返回集里。 */
        int n = 0;
        int have_rtformat = 0;
        const uint32_t rt = rt_formats_for_profile(cfg->profile);

        for (int i = 0; i < cfg->num_attribs && n < DMD_MAX_CONFIG_ATTRIBUTES; i++) {
            if (cfg->attribs[i].type == VAConfigAttribRTFormat) {
                /* 用驱动真实支持的值覆盖调用方传入的值 */
                attrib_list[n].type = VAConfigAttribRTFormat;
                attrib_list[n].value = rt;
                have_rtformat = 1;
            } else {
                attrib_list[n] = cfg->attribs[i];
            }
            n++;
        }

        if (!have_rtformat && n < DMD_MAX_CONFIG_ATTRIBUTES) {
            attrib_list[n].type = VAConfigAttribRTFormat;
            attrib_list[n].value = rt;
            n++;
        }

        *num_attribs = n;
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
    struct dmd_config *cfg = find_config_locked(drv, config);
    VAProfile cfg_profile = cfg ? cfg->profile : VAProfileNone;
    int encoding = cfg && cfg->entrypoint == VAEntrypointEncSlice;
    pthread_mutex_unlock(&drv->lock);
    if (!cfg)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    unsigned min_w = DMD_MIN_WIDTH, min_h = DMD_MIN_HEIGHT;
    unsigned max_w = DMD_MAX_WIDTH, max_h = DMD_MAX_HEIGHT;
    if (encoding)
        encode_dimensions(cfg_profile, &min_w, &min_h, &max_w, &max_h);

    /* Main10/AV1 expose NV12 and P010 for matching 8/10-bit frame syntax;
     * HEVC Main and all encoders remain NV12-only. */
    const uint32_t rt = encoding ? VA_RT_FORMAT_YUV420 : rt_formats_for_profile(cfg_profile);
    const int eight_bit = (rt & VA_RT_FORMAT_YUV420) != 0;
    const int ten_bit = (rt & VA_RT_FORMAT_YUV420_10) != 0;

    /* VA-API 约定的两段式查询：attrib_list 为 NULL 时只回报数量。 */
    const unsigned int needed = 4u + eight_bit + ten_bit + (encoding ? 1u : 0u);
    if (!attrib_list) {
        *num_attribs = needed;
        return VA_STATUS_SUCCESS;
    }
    if (*num_attribs < needed) {
        *num_attribs = needed;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    unsigned int n = 0;

    if (eight_bit) {
        attrib_list[n].type = VASurfaceAttribPixelFormat;
        attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
        attrib_list[n].value.type = VAGenericValueTypeInteger;
        attrib_list[n].value.value.i = VA_FOURCC_NV12;
        n++;
    }

    if (ten_bit) {
        attrib_list[n].type = VASurfaceAttribPixelFormat;
        attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE |
                               VA_SURFACE_ATTRIB_SETTABLE;
        attrib_list[n].value.type = VAGenericValueTypeInteger;
        attrib_list[n].value.value.i = VA_FOURCC_P010;
        n++;
    }

    attrib_list[n].type = VASurfaceAttribMinWidth;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = min_w;
    n++;

    attrib_list[n].type = VASurfaceAttribMinHeight;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = min_h;
    n++;

    attrib_list[n].type = VASurfaceAttribMaxWidth;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = max_w;
    n++;

    attrib_list[n].type = VASurfaceAttribMaxHeight;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = max_h;
    n++;

    if (encoding) {
        attrib_list[n].type = VASurfaceAttribMemoryType;
        attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
        attrib_list[n].value.type = VAGenericValueTypeInteger;
        attrib_list[n].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
        n++;
    }

    *num_attribs = n;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_QueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list,
                               int *num_formats)
{
    if (!ctx || !format_list || !num_formats)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* 线性半平面两种：NV12(8bit) 与 P010(10bit)。
     *
     * ⚠️ *num_formats 是**入参时也是数组容量**（libva 的 in/out 约定），
     * 只在容量够时才写第二项 —— 否则踩调用方的栈。 */
    const int cap = *num_formats;

    memset(&format_list[0], 0, sizeof(format_list[0]));
    format_list[0].fourcc = VA_FOURCC_NV12;
    format_list[0].byte_order = VA_LSB_FIRST;
    format_list[0].bits_per_pixel = 12; /* 平面式 YUV420 每像素 12 bit */

    *num_formats = 1;
    if (cap < 2)
        return VA_STATUS_SUCCESS;

    memset(&format_list[1], 0, sizeof(format_list[1]));
    format_list[1].fourcc = VA_FOURCC_P010;
    format_list[1].byte_order = VA_LSB_FIRST;
    format_list[1].bits_per_pixel = 12; /* 名义位数；有效 10bit 由 fourcc 表达 */

    *num_formats = 2;
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
