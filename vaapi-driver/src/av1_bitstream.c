/* AV1 OBU 反向合成——变长编码、对齐与 OBU 头。
 *
 * 设计说明见 av1_bitstream.h 顶部。本文件只实现最底层的比特写入原语，
 * 序列头/帧头/tile group 的语法在后续提交里加。
 */
#include <va/va.h>
#include <va/va_dec_av1.h>

#include "av1_bitstream.h"

/* ---------------------------------------------------------------- leb128 */

size_t dmd_av1_leb128_len(uint64_t v)
{
    size_t n = 0;
    do {
        n++;
        v >>= 7;
    } while (v && n < DMD_LEB128_MAX);
    return n;
}

size_t dmd_av1_leb128(uint64_t v, unsigned char *out, size_t out_cap)
{
    size_t n = 0;
    do {
        if (n >= out_cap || n >= DMD_LEB128_MAX)
            return 0;
        unsigned char byte = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (v)
            byte |= 0x80;          /* 还有后续字节 */
        out[n++] = byte;
    } while (v);
    return n;
}

/* ------------------------------------------------------------------ uvlc */

void dmd_av1_put_uvlc(struct dmd_bitwriter *bw, uint32_t v)
{
    /* AV1 规范 4.10.3：leadingZeros 个 0，一个 1，然后 leadingZeros 位尾数。
     * 值 v 编码为 (1 << leadingZeros) - 1 + mantissa。 */
    uint32_t leading_zeros = 0;
    uint64_t val = (uint64_t)v + 1;

    while ((val >> (leading_zeros + 1)) != 0)
        leading_zeros++;

    if (leading_zeros >= 32) {
        bw->overflow = 1;
        return;
    }

    /* leading_zeros 个 0 位；put_bits 不接受 nbits=0，所以判空。 */
    if (leading_zeros > 0)
        dmd_bw_put_bits(bw, 0, (int)leading_zeros);
    dmd_bw_put_flag(bw, 1);
    if (leading_zeros > 0)
        dmd_bw_put_bits(bw, (uint32_t)(val & ((1u << leading_zeros) - 1)),
                        (int)leading_zeros);
}

/* -------------------------------------------------------------------- le */

void dmd_av1_put_le(struct dmd_bitwriter *bw, uint64_t v, int nbytes)
{
    /* AV1 规范 4.10.4：小端字节序，要求调用时已字节对齐。 */
    if (nbytes <= 0 || nbytes > 8 || bw->bit_pos != 0) {
        bw->overflow = 1;
        return;
    }
    for (int i = 0; i < nbytes; i++)
        dmd_bw_put_bits(bw, (uint32_t)((v >> (i * 8)) & 0xff), 8);
}

/* -------------------------------------------------------------------- ns */

void dmd_av1_put_ns(struct dmd_bitwriter *bw, uint32_t v, uint32_t n)
{
    /* AV1 规范 4.10.7 ns(n)：非对称编码，小值省一位。
     *   w = FloorLog2(n) + 1
     *   m = (1 << w) - n
     * v < m 时用 w-1 位直接写；否则写 (v + m) 的 w 位。 */
    if (n == 0) {
        bw->overflow = 1;
        return;
    }
    if (n == 1)
        return;                    /* 只有一个取值，不占位 */

    uint32_t w = 0, t = n;
    while (t) { w++; t >>= 1; }    /* w = FloorLog2(n) + 1 */
    uint32_t m = (1u << w) - n;

    if (v < m) {
        dmd_bw_put_bits(bw, v, (int)(w - 1));
    } else {
        uint32_t enc = v + m;
        dmd_bw_put_bits(bw, enc >> 1, (int)(w - 1));
        dmd_bw_put_bits(bw, enc & 1, 1);
    }
}

/* -------------------------------------------------------------------- su */

void dmd_av1_put_su(struct dmd_bitwriter *bw, int32_t v, int nbits)
{
    /* AV1 规范 4.10.6 su(n)：n 位补码。写入时取低 nbits 位即可，
     * 解码侧按符号位扩展。 */
    if (nbits <= 0 || nbits > 32) {
        bw->overflow = 1;
        return;
    }
    uint32_t mask = (nbits == 32) ? 0xffffffffu : ((1u << nbits) - 1u);
    dmd_bw_put_bits(bw, (uint32_t)v & mask, nbits);
}

/* ------------------------------------------------------------------ 对齐 */

void dmd_av1_byte_align(struct dmd_bitwriter *bw)
{
    /* AV1 规范 5.3.5 byte_alignment()：纯补零，**不写 stop bit**。
     * 这是与 H.264/HEVC rbsp_trailing_bits 的关键差别。 */
    while (bw->bit_pos != 0 && !bw->overflow)
        dmd_bw_put_flag(bw, 0);
}

void dmd_av1_trailing_bits(struct dmd_bitwriter *bw)
{
    /* AV1 规范 5.3.4 trailing_bits()：一个 1，然后补零到字节边界。
     * 注意即使当前已对齐也要写这个 1 —— 它是 payload 结束标记。 */
    dmd_bw_put_flag(bw, 1);
    while (bw->bit_pos != 0 && !bw->overflow)
        dmd_bw_put_flag(bw, 0);
}

/* ------------------------------------------------------- 序列头合成（2/4） */

/* 位宽计算：AV1 规范用 FloorLog2(x)+1 表示"表达 x 需要几位"。 */
static int bits_for(uint32_t v)
{
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n ? n : 1;
}

/* color_config()，AV1 规范 5.5.2。 */
static void put_color_config(struct dmd_bitwriter *bw,
                             const VADecPictureParameterBufferAV1 *p)
{
    const uint32_t depth_idx  = p->bit_depth_idx;
    const uint32_t mono       = p->seq_info_fields.fields.mono_chrome;
    const uint32_t sub_x      = p->seq_info_fields.fields.subsampling_x;
    const uint32_t sub_y      = p->seq_info_fields.fields.subsampling_y;

    /* high_bitdepth / twelve_bit：bit_depth_idx 0/1/2 → 8/10/12 位
     * （va_dec_av1.h:255-260）。只有 profile 2 能到 12 位。 */
    const int high_bitdepth = (depth_idx != 0);
    dmd_bw_put_flag(bw, high_bitdepth);
    if (p->profile == 2 && high_bitdepth)
        dmd_bw_put_flag(bw, depth_idx == 2);       /* twelve_bit */

    if (p->profile != 1)
        dmd_bw_put_flag(bw, (int)mono);            /* mono_chrome */

    /* color_description_present_flag 置 1 并显式给出三个描述符：
     * VA-API 只提供 matrix_coefficients（va_dec_av1.h:263-264），
     * 另两个填 UNSPECIFIED(2)。若置 0，规范会把三者都当 UNSPECIFIED，
     * 那就丢掉了 VA-API 明确给出的 matrix_coefficients。 */
    dmd_bw_put_flag(bw, 1);
    dmd_bw_put_bits(bw, 2, 8);                     /* color_primaries          */
    dmd_bw_put_bits(bw, 2, 8);                     /* transfer_characteristics */
    dmd_bw_put_bits(bw, p->matrix_coefficients, 8);

    if (mono) {
        dmd_bw_put_flag(bw, (int)p->seq_info_fields.fields.color_range);
        return;                                    /* 单色分支到此结束 */
    }

    dmd_bw_put_flag(bw, (int)p->seq_info_fields.fields.color_range);

    /* subsampling 只在 profile 2 且 12 位时显式写入；profile 0 恒 4:2:0、
     * profile 1 恒 4:4:4，由 profile 推导，不占码流位（规范 5.5.2）。 */
    if (p->profile == 2 && depth_idx == 2) {
        dmd_bw_put_flag(bw, (int)sub_x);
        if (sub_x)
            dmd_bw_put_flag(bw, (int)sub_y);
    }

    /* chroma_sample_position 仅 4:2:0 时出现。VA-API 的同名字段已标
     * va_deprecated（:285），故写 UNKNOWN(0)：它只影响色度插值相位假设，
     * 不影响能否解出帧。 */
    if (sub_x && sub_y)
        dmd_bw_put_bits(bw, 0, 2);

    dmd_bw_put_flag(bw, 0);                        /* separate_uv_delta_q */
}

size_t dmd_av1_build_sequence_header(const void *pic_v,
                                     unsigned char *out, size_t out_cap)
{
    const VADecPictureParameterBufferAV1 *p = pic_v;
    if (!p || !out || out_cap < 8)
        return 0;

    /* payload 先写临时缓冲：obu_size 用 leb128 编码，必须先知道 payload
     * 长度才能写头部。序列头很小（实测 20 字节上下），栈上足够。 */
    unsigned char body[128];
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, body, sizeof(body));

    const uint32_t enable_order_hint =
        p->seq_info_fields.fields.enable_order_hint;

    /* ---- sequence_header_obu()，AV1 规范 5.5.1 ---- */

    dmd_bw_put_bits(&bw, p->profile, 3);                    /* seq_profile   */
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.still_picture);
    dmd_bw_put_flag(&bw, 0);   /* reduced_still_picture_header
                                * 恒 0：置 1 会砍掉后面绝大多数字段，而帧头
                                * 解析依赖其中的 enable_* 能力位。 */

    dmd_bw_put_flag(&bw, 0);   /* timing_info_present_flag
                                * VA-API 不提供 timing_info。时序不影响解码，
                                * 显示节奏由上层控制。 */
    dmd_bw_put_flag(&bw, 0);   /* initial_display_delay_present_flag */
    dmd_bw_put_bits(&bw, 0, 5);/* operating_points_cnt_minus_1
                                * 单一 operating point：本驱动不做可扩展层，
                                * 与 OBU 头 extension_flag 恒 0 相一致。 */
    dmd_bw_put_bits(&bw, 0, 12);          /* operating_point_idc[0] */
    dmd_bw_put_bits(&bw, 0, 5);           /* seq_level_idx[0] = 2.0
                                           * VA-API 不提供 level；MediaCodec
                                           * 按实际分辨率分配资源，不用它校验。
                                           * seq_level_idx <= 7 时不写 seq_tier。 */

    /* frame_width_bits 取实际所需位宽而不写死 16 位。
     * frame_width_minus1 按 va_dec_av1.h:332-334 是**上采样后**分辨率，
     * 正是 max_frame_width_minus_1 要的语义。 */
    const uint32_t w_m1  = p->frame_width_minus1;
    const uint32_t h_m1  = p->frame_height_minus1;
    const int      wbits = bits_for(w_m1);
    const int      hbits = bits_for(h_m1);

    dmd_bw_put_bits(&bw, (uint32_t)(wbits - 1), 4);
    dmd_bw_put_bits(&bw, (uint32_t)(hbits - 1), 4);
    dmd_bw_put_bits(&bw, w_m1, wbits);
    dmd_bw_put_bits(&bw, h_m1, hbits);

    dmd_bw_put_flag(&bw, 0);   /* frame_id_numbers_present_flag
                                * 置 0：VA-API 不提供 delta_frame_id，帧头里
                                * 对应字段也就不写 —— 两处必须一致。 */

    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.use_128x128_superblock);
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_filter_intra);
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_intra_edge_filter);
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_interintra_compound);
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_masked_compound);

    /* ⚠️ enable_warped_motion 序列级字段 VA-API **不提供**（全文件 grep
     * 确认）。取 1 是安全侧：它只是能力位，帧级 allow_warped_motion（:439）
     * 才决定该帧是否真用。若这里置 0 而帧级为 1，解码器会拒绝该帧；
     * 置 1 而帧级为 0 则无任何副作用。 */
    dmd_bw_put_flag(&bw, 1);

    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_dual_filter);
    dmd_bw_put_flag(&bw, (int)enable_order_hint);
    if (enable_order_hint) {
        dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_jnt_comp);
        dmd_bw_put_flag(&bw, 1);   /* enable_ref_frame_mvs：同样不提供，
                                    * 同理取安全侧 1（帧级 use_ref_frame_mvs
                                    * 在 :435）。 */
    }

    /* seq_choose_screen_content_tools = 1 时 seq_force_screen_content_tools
     * 取 SELECT_SCREEN_CONTENT_TOOLS 且**不占码流位**，语义是"每帧在帧头
     * 自行声明"。这正合需要：VA-API 只给帧级 allow_screen_content_tools
     * （:429），在序列级猜一个固定值反而可能与帧冲突。
     * seq_choose_integer_mv 同理（帧级 force_integer_mv 在 :430）。 */
    dmd_bw_put_flag(&bw, 1);       /* seq_choose_screen_content_tools */
    dmd_bw_put_flag(&bw, 1);       /* seq_choose_integer_mv           */

    if (enable_order_hint)
        dmd_bw_put_bits(&bw, p->order_hint_bits_minus_1, 3);

    /* ⚠️ enable_superres / enable_restoration 也不提供，用帧级反推：
     *   use_superres（:432）
     *   三个 *frame_restoration_type（:608-610，非 0 即启用）
     * 这两个**必须如实反映**，与 warped_motion 的处理不同 —— 它们会改变
     * 帧头的语法结构（多读或少读字段），置错会直接让帧头错位。 */
    const int use_superres = (int)p->pic_info_fields.bits.use_superres;
    const int any_restoration =
        (p->loop_restoration_fields.bits.yframe_restoration_type  != 0) ||
        (p->loop_restoration_fields.bits.cbframe_restoration_type != 0) ||
        (p->loop_restoration_fields.bits.crframe_restoration_type != 0);

    dmd_bw_put_flag(&bw, use_superres);
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_cdef);
    dmd_bw_put_flag(&bw, any_restoration);

    put_color_config(&bw, p);

    dmd_bw_put_flag(&bw,
        (int)p->seq_info_fields.fields.film_grain_params_present);

    dmd_av1_trailing_bits(&bw);

    if (bw.overflow)
        return 0;

    /* 组装：OBU 头（内含 leb128 的 payload 长度）+ payload。 */
    const size_t body_len = dmd_bw_bytes(&bw);
    const size_t hdr = dmd_av1_obu_header(DMD_OBU_SEQUENCE_HEADER,
                                         body_len, out, out_cap);
    if (hdr == 0 || hdr + body_len > out_cap)
        return 0;
    for (size_t i = 0; i < body_len; i++)
        out[hdr + i] = body[i];
    return hdr + body_len;
}

/* ---------------------------------------------------------------- OBU 头 */

size_t dmd_av1_obu_header(int obu_type, size_t payload_len,
                          unsigned char *out, size_t out_cap)
{
    if (obu_type < 0 || obu_type > 15 || out_cap < 1)
        return 0;

    /* forbidden(1)=0 | type(4) | extension(1)=0 | has_size(1)=1 | reserved(1)=0
     *
     * 即 0x00 | (type << 3) | 0x00 | 0x02 | 0x00 —— has_size 位是 bit1。
     * 举例：SEQUENCE_HEADER(1) → 0x0a，FRAME_HEADER(3) → 0x1a，
     *       TILE_GROUP(4) → 0x22，TEMPORAL_DELIMITER(2) → 0x12。
     *
     * 对照实测的非法值 0xd0 = 1101_0000：forbidden=1（必须 0）、
     * type=10（保留值）—— 一眼就能看出那不是 OBU 头而是裸载荷。 */
    out[0] = (unsigned char)(((obu_type & 0x0f) << 3) | 0x02);

    size_t n = dmd_av1_leb128((uint64_t)payload_len, out + 1, out_cap - 1);
    if (n == 0)
        return 0;
    return 1 + n;
}
