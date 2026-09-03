/* MPEG-2 (ISO/IEC 13818-2) 头合成。
 *
 * 为什么需要：V4L2 stateful 解码器要的是完整的 MPEG-2 视频基本流字节，
 * 而 VA-API 只在 VASliceDataBufferType 里给 **slice 层**数据
 * （slice_start_code 之后的部分，见 va.h 的 VASliceParameterBufferMPEG2：
 * 它带 slice_horizontal_position / quantiser_scale_code 等 slice header
 * 字段，说明上层已经把 slice header 解析掉了，但字节仍在 buffer 里）。
 * sequence_header / sequence_extension / picture_header /
 * picture_coding_extension 这些**帧级与序列级头一个都没有** ——
 * 它们的内容被拆进了 VAPictureParameterBufferMPEG2 与
 * VAIQMatrixBufferMPEG2。所以必须按规范反向写回去。
 *
 * 与 h264/hevc 的差异：MPEG-2 用 start code（00 00 01 xx）分隔，
 * 语法元素全是定长位域，没有 ue(v)/se(v)，也**没有 emulation prevention**
 * （规范靠 start code 唯一性约束，编码器保证不产生伪起始码），
 * 所以不需要 dmd_rbsp_escape。
 *
 * ⚠️ 已知信息缺口（决定了本实现的适用范围）：
 *   - frame_rate_code / bit_rate_value / vbv_buffer_size：VA-API 不提供。
 *     这些只影响时序与缓冲模型，不影响像素重建，填规范允许的合法值即可。
 *   - aspect_ratio_information：不提供，填 1（square samples）。
 *   - profile_and_level_indication：不提供，填 Main@Main。
 *   - chroma_format：VA-API 无字段。填 1（4:2:0）—— 本驱动的 NV12
 *     回传路径本来就只支持 4:2:0，其它色度格式的输出格式假设不成立。
 * 这些填充值不参与残差/预测的重建，所以像素仍可逐字节正确；
 * 若将来遇到 4:2:2 码流应当拒绝而不是猜。
 */
#include "mpeg2_bitstream.h"

#include <string.h>

#include "bitstream.h"

/* MPEG-2 start code。前缀恒为 00 00 01。 */
#define MP2_SC_PICTURE   0x00
#define MP2_SC_SEQUENCE  0xB3
#define MP2_SC_EXTENSION 0xB5
#define MP2_SC_GOP       0xB8

/* extension_start_code_identifier（13818-2 表 6-2） */
#define MP2_EXT_SEQUENCE          0x1
#define MP2_EXT_PICTURE_CODING    0x8

static void put_start_code(struct dmd_bitwriter *bw, unsigned char code)
{
    /* 起始码必须字节对齐。调用点都在对齐位置，这里断言式补齐以防误用。 */
    while (bw->bit_pos != 0)
        dmd_bw_put_flag(bw, 0);
    dmd_bw_put_bits(bw, 0x000001, 24);
    dmd_bw_put_bits(bw, code, 8);
}

/* sequence_header()（13818-2 §6.2.2.1）+ sequence_extension（§6.2.2.3）。
 * 两者必须成对出现，否则解码器会按 MPEG-1 解释后续语法。 */
static void build_sequence(struct dmd_bitwriter *bw,
                           const VAPictureParameterBufferMPEG2 *pp,
                           const VAIQMatrixBufferMPEG2 *iq)
{
    const unsigned w = pp->horizontal_size;
    const unsigned h = pp->vertical_size;

    put_start_code(bw, MP2_SC_SEQUENCE);
    /* horizontal/vertical_size_value 只有 12 位，高 2 位放进
     * sequence_extension 的 *_size_extension。 */
    dmd_bw_put_bits(bw, w & 0xFFF, 12);
    dmd_bw_put_bits(bw, h & 0xFFF, 12);
    /* aspect_ratio_information（表 6-3）：1=square，2=4:3，3=16:9，4=2.21:1。
     * VA-API 不提供该字段。按尺寸比推断，因为它影响的只是显示纵横比，
     * 而 16:9 素材填 square 会让固件按不一致的显示参数校验。
     * 实测原始流（1920x1080）填的是 3。 */
    unsigned aspect = 1;
    if (h > 0) {
        /* 用整数比避免浮点：16/9 ≈ 1.777，4/3 ≈ 1.333 */
        const unsigned r1000 = (w * 1000u) / h;
        if (r1000 >= 1700 && r1000 <= 1850) aspect = 3;       /* 16:9 */
        else if (r1000 >= 1300 && r1000 <= 1400) aspect = 2;  /* 4:3 */
    }
    dmd_bw_put_bits(bw, aspect, 4);
    dmd_bw_put_bits(bw, 5, 4);          /* frame_rate_code: 30fps（VA 不提供）*/
    dmd_bw_put_bits(bw, 0x3FFFF, 18);   /* bit_rate_value: 全 1 = 未指定 */
    dmd_bw_put_flag(bw, 1);             /* marker_bit */
    /* vbv_buffer_size_value：VA-API 不提供。
     * ⚠️ 不能填大值。此前填 112 时 Venus 固件报 SYS_ERROR；实测 ffmpeg 对
     * 同一码流填的是 3。固件会按这个值预留码流缓冲并做一致性校验，
     * 填过大等于要求它准备一个不存在的缓冲。取 3 与常见编码器一致。 */
    dmd_bw_put_bits(bw, 3, 10);
    dmd_bw_put_flag(bw, 0);             /* constrained_parameters_flag */

    /* 量化矩阵（zig-zag 序，与码流所需顺序一致）。
     *
     * ⚠️ 不能直接采信 VA-API 的 load_*_quantiser_matrix 标志。
     * 实测 ffmpeg 的 vaapi_mpeg2.c **恒把两个标志填 1** 并附上当前生效的
     * 矩阵（即便码流里原本没有、用的是规范默认矩阵）。照它写回去的后果：
     *   原始流 sequence_header 12 字节（load_intra=0，无矩阵）
     *   合成流 sequence_header 140 字节（load_intra=1，写了 128 字节矩阵）
     * 虽然 ffmpeg 自己能解开这样的码流，Venus 固件收到后直接 SYS_ERROR。
     *
     * 所以与规范默认矩阵（13818-2 表 7-3 / 7-4）比对：一致就写 0 不带矩阵，
     * 真正非默认才写。这样常见码流合成出的头与原始流逐字节一致。 */
    static const unsigned char default_intra[64] = {
         8, 16, 16, 19, 16, 19, 22, 22,
        22, 22, 22, 22, 26, 24, 26, 27,
        27, 27, 26, 26, 26, 26, 27, 27,
        27, 29, 29, 29, 34, 34, 34, 29,
        29, 29, 27, 27, 29, 29, 32, 32,
        34, 34, 37, 38, 37, 35, 35, 34,
        35, 38, 38, 40, 40, 40, 48, 48,
        46, 46, 56, 56, 58, 69, 69, 83
    };

    const int li = iq && iq->load_intra_quantiser_matrix &&
                   memcmp(iq->intra_quantiser_matrix, default_intra, 64) != 0;
    dmd_bw_put_flag(bw, li ? 1 : 0);
    if (li)
        for (int i = 0; i < 64; i++)
            dmd_bw_put_bits(bw, iq->intra_quantiser_matrix[i], 8);

    /* 非 intra 的规范默认矩阵是全 16。 */
    int nondefault_ni = 0;
    if (iq && iq->load_non_intra_quantiser_matrix) {
        for (int i = 0; i < 64; i++)
            if (iq->non_intra_quantiser_matrix[i] != 16) { nondefault_ni = 1; break; }
    }
    dmd_bw_put_flag(bw, nondefault_ni ? 1 : 0);
    if (nondefault_ni)
        for (int i = 0; i < 64; i++)
            dmd_bw_put_bits(bw, iq->non_intra_quantiser_matrix[i], 8);

    /* sequence_extension：把 MPEG-2 的扩展语法启用起来。 */
    put_start_code(bw, MP2_SC_EXTENSION);
    dmd_bw_put_bits(bw, MP2_EXT_SEQUENCE, 4);
    /* profile_and_level_indication（13818-2 §6.3.5，表 8-4/8-5）：
     * bit7 = escape(0)，bit6-4 = profile，bit3-0 = level。
     * Main profile = 0b100，Main level = 0b1000 → 0x48。
     * ⚠️ 但实测 ffmpeg 对 1080p Main 输出的是 **0x44**（逐字节比对原始流
     * 确认），对应 Main profile + High-1440 level。1920x1080 超过 Main
     * level 的 720x576 上限，所以编码器必须上报更高的 level。
     * 填 0x48 时 Venus 固件收到码流后直接 SYS_ERROR —— 它按 level 上限
     * 校验分辨率，1080p 与 Main level 不符。
     * 这里按分辨率选 level：>720x576 用 High-1440(4)，否则 Main(8)。 */
    {
        const unsigned level = (w > 720 || h > 576) ? 0x4u : 0x8u;
        dmd_bw_put_bits(bw, (0u << 7) | (0x4u << 4) | level, 8);
    }
    dmd_bw_put_flag(bw, pp->picture_coding_extension.bits.progressive_frame);
    dmd_bw_put_bits(bw, 1, 2);          /* chroma_format: 4:2:0 */
    dmd_bw_put_bits(bw, (w >> 12) & 0x3, 2);  /* horizontal_size_extension */
    dmd_bw_put_bits(bw, (h >> 12) & 0x3, 2);  /* vertical_size_extension */
    dmd_bw_put_bits(bw, 0, 12);         /* bit_rate_extension */
    dmd_bw_put_flag(bw, 1);             /* marker_bit */
    dmd_bw_put_bits(bw, 0, 8);          /* vbv_buffer_size_extension */
    dmd_bw_put_flag(bw, 0);             /* low_delay */
    dmd_bw_put_bits(bw, 0, 2);          /* frame_rate_extension_n */
    dmd_bw_put_bits(bw, 0, 5);          /* frame_rate_extension_d */

    /* group_of_pictures_header（§6.2.2.6）。
     *
     * ⚠️ 规范上 GOP header 是**可选**的，但 Venus 固件要求它：
     * 实测在 sequence header 与 picture header 都已与原始流逐字节一致的
     * 前提下，缺 GOP header 时固件仍报 SYS_ERROR、一帧不吐；
     * 原始流的层次是 seq → ext → **GOP** → picture → ext → slice。
     * VA-API 不提供 time_code / closed_gop / broken_link，
     * 这些只影响随机访问语义，不参与像素重建，填合法值即可。 */
    while (bw->bit_pos != 0)
        dmd_bw_put_flag(bw, 0);
    put_start_code(bw, MP2_SC_GOP);
    /* time_code：25 位（drop_frame(1) hour(5) minute(6) marker(1)
     * second(6) picture(6)）。全 0 加 marker=1 即 00:00:00:00。 */
    dmd_bw_put_flag(bw, 0);             /* drop_frame_flag */
    dmd_bw_put_bits(bw, 0, 5);          /* time_code_hours */
    dmd_bw_put_bits(bw, 0, 6);          /* time_code_minutes */
    dmd_bw_put_flag(bw, 1);             /* marker_bit */
    dmd_bw_put_bits(bw, 0, 6);          /* time_code_seconds */
    dmd_bw_put_bits(bw, 0, 6);          /* time_code_pictures */
    /* closed_gop=1：宣称本 GOP 不依赖前一个 GOP。合成路径下每帧都自带
     * 完整头，且参考帧由解码器自己按 picture_coding_type 管理，
     * 填 1 更安全（填 0 会让固件期待跨 GOP 的前向参考）。 */
    dmd_bw_put_flag(bw, 1);             /* closed_gop */
    dmd_bw_put_flag(bw, 0);             /* broken_link */
}

/* picture_header()（§6.2.3）+ picture_coding_extension（§6.2.3.1）。 */
static void build_picture(struct dmd_bitwriter *bw,
                          const VAPictureParameterBufferMPEG2 *pp,
                          unsigned int temporal_ref)
{
    const int ct = pp->picture_coding_type;   /* 1=I 2=P 3=B */

    put_start_code(bw, MP2_SC_PICTURE);
    dmd_bw_put_bits(bw, temporal_ref & 0x3FF, 10);
    dmd_bw_put_bits(bw, (uint32_t)ct, 3);
    /* vbv_delay：0xFFFF 表示未指定，规范允许。 */
    dmd_bw_put_bits(bw, 0xFFFF, 16);
    if (ct == 2 || ct == 3) {
        /* full_pel_forward_vector 与 forward_f_code 是 MPEG-1 遗留字段。
         * MPEG-2 里必须写 0 / 7，真正的 f_code 在 picture_coding_extension。 */
        dmd_bw_put_flag(bw, 0);
        dmd_bw_put_bits(bw, 7, 3);
    }
    if (ct == 3) {
        dmd_bw_put_flag(bw, 0);
        dmd_bw_put_bits(bw, 7, 3);
    }
    dmd_bw_put_flag(bw, 0);             /* extra_bit_picture */

    put_start_code(bw, MP2_SC_EXTENSION);
    dmd_bw_put_bits(bw, MP2_EXT_PICTURE_CODING, 4);
    /* f_code：VA-API 把四个 4 位字段打包进一个 int32。
     * 布局（va.h 注释"pack all four fcode into this"）自高到低为
     * f_code[0][0] f_code[0][1] f_code[1][0] f_code[1][1]。 */
    dmd_bw_put_bits(bw, ((uint32_t)pp->f_code >> 12) & 0xF, 4);
    dmd_bw_put_bits(bw, ((uint32_t)pp->f_code >> 8) & 0xF, 4);
    dmd_bw_put_bits(bw, ((uint32_t)pp->f_code >> 4) & 0xF, 4);
    dmd_bw_put_bits(bw, ((uint32_t)pp->f_code) & 0xF, 4);


    dmd_bw_put_bits(bw, pp->picture_coding_extension.bits.intra_dc_precision, 2);
    dmd_bw_put_bits(bw, pp->picture_coding_extension.bits.picture_structure, 2);
    dmd_bw_put_flag(bw, pp->picture_coding_extension.bits.top_field_first);
    dmd_bw_put_flag(bw, pp->picture_coding_extension.bits.frame_pred_frame_dct);
    dmd_bw_put_flag(bw,
                    pp->picture_coding_extension.bits.concealment_motion_vectors);
    dmd_bw_put_flag(bw, pp->picture_coding_extension.bits.q_scale_type);
    dmd_bw_put_flag(bw, pp->picture_coding_extension.bits.intra_vlc_format);
    dmd_bw_put_flag(bw, pp->picture_coding_extension.bits.alternate_scan);
    dmd_bw_put_flag(bw, pp->picture_coding_extension.bits.repeat_first_field);
    dmd_bw_put_flag(bw, 1);             /* chroma_420_type = progressive_frame */
    dmd_bw_put_flag(bw, pp->picture_coding_extension.bits.progressive_frame);
    dmd_bw_put_flag(bw, 0);             /* composite_display_flag */
}

size_t dmd_mpeg2_build_sequence(const VAPictureParameterBufferMPEG2 *pp,
                                const VAIQMatrixBufferMPEG2 *iq,
                                unsigned char *out, size_t out_cap)
{
    if (!pp || !out)
        return 0;
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, out, out_cap);
    build_sequence(&bw, pp, iq);
    /* 起始码前需字节对齐；sequence_extension 末尾用 0 填齐。 */
    while (bw.bit_pos != 0)
        dmd_bw_put_flag(&bw, 0);
    if (bw.overflow)
        return 0;
    return dmd_bw_bytes(&bw);
}

size_t dmd_mpeg2_build_picture(const VAPictureParameterBufferMPEG2 *pp,
                               unsigned int temporal_ref,
                               unsigned char *out, size_t out_cap)
{
    if (!pp || !out)
        return 0;
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, out, out_cap);
    build_picture(&bw, pp, temporal_ref);
    while (bw.bit_pos != 0)
        dmd_bw_put_flag(&bw, 0);
    if (bw.overflow)
        return 0;
    return dmd_bw_bytes(&bw);
}

int dmd_mpeg2_can_build(const VAPictureParameterBufferMPEG2 *pp)
{
    if (!pp)
        return 0;
    /* picture_coding_type 只认 I/P/B（4 = D picture，MPEG-1 专有，不支持）。 */
    if (pp->picture_coding_type < 1 || pp->picture_coding_type > 3)
        return 0;
    /* 场编码（picture_structure 1=top field, 2=bottom field）需要
     * 逐场配对与 is_first_field 状态机，本驱动的 surface 模型是逐帧的，
     * 不做半成品支持 —— 返回 0 让上层回落软解。3 = frame picture。 */
    if (pp->picture_coding_extension.bits.picture_structure != 3)
        return 0;
    if (pp->horizontal_size == 0 || pp->vertical_size == 0)
        return 0;
    return 1;
}
