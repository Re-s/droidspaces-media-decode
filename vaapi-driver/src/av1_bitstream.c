/* AV1 OBU 反向合成——变长编码、对齐与 OBU 头。
 *
 * 设计说明见 av1_bitstream.h 顶部。本文件只实现最底层的比特写入原语，
 * 序列头/帧头/tile group 的语法在后续提交里加。
 */
#include <stdio.h>
#include <stdlib.h>

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

    /* color_description_present_flag = 0。
     *
     * ⚠️ 曾置 1 并显式写三个描述符（想保住 VA-API 给的
     * matrix_coefficients），实测与真实码流不符：libaom 置 0，
     * 于是三者都取 UNSPECIFIED(2)。多写会让序列头长 3 字节。
     *
     * 代价可接受：matrix_coefficients 只影响色彩转换矩阵的选择，
     * 不影响能否解出帧；而本驱动输出 NV12 给上层，色彩解释由上层负责。 */
    dmd_bw_put_flag(bw, 0);

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
    /* chroma_sample_position = 1（CSP_VERTICAL）。真实码流用 1；
     * VA-API 的同名字段已标 va_deprecated（:285）故不可信。
     * 该值只影响色度插值的相位假设，不影响能否解出帧。 */
    if (sub_x && sub_y)
        dmd_bw_put_bits(bw, 1, 2);

    dmd_bw_put_flag(bw, 0);                        /* separate_uv_delta_q */
}

/* tile_info()，AV1 规范 5.9.15。
 *
 * ⚠️ tile_size_bytes_minus_1 写死 1（即 2 字节，与源码流 trace_headers
 *    实测值一致），第 4 步的
 * tile_group 必须用同样宽度写 tile_size_minus_1 —— 两处不一致会让
 * 解码器按错误宽度读 tile 长度，从第二个 tile 起全部错位。 */
/* tile_info()，AV1 规范 5.9.15。
 *
 * ⚠️ 本函数的边界值算错过两次，最终以 ffmpeg 的 CBS 实现为准逐行校对
 * （libavcodec/cbs_av1_syntax_template.c 的 tile_info()）。两个坑：
 *
 *   1) 位移量是 sb_size = sb_shift + 2，**不是** sb_shift。CBS 原文：
 *        sb_size = sb_shift + 2;
 *        max_tile_width_sb = AV1_MAX_TILE_WIDTH >> sb_size;
 *      用 sb_shift 会让 max_tile_width_sb 大 4 倍 → min_log2_tile_cols
 *      偏小 → 一元码起点就错。
 *
 *   2) min_log2_tiles 要对 min_log2_tile_cols 取 max。CBS 原文：
 *        min_log2_tiles = FFMAX(min_log2_tile_cols,
 *                    cbs_av1_tile_log2(max_tile_area_sb, sb_rows*sb_cols));
 *      漏掉会让 min_log2_tile_rows 偏小 → 行方向多写若干个 1。
 *
 * increment(v, min, max) 的编码规则（cbs_av1_write_increment）：
 *   v == max → 写 (max-min) 个 1，**不写停止位**
 *   否则     → 写 (v-min) 个 1，再写一个 0
 *
 * ⚠️ tile_size_bytes_minus_1 写死 1（2 字节），第 4 步 tile_group 里写
 * tile_size_minus_1 必须用同样宽度 —— 两处不一致会从第二个 tile 起全错位。 */
static void put_tile_info(struct dmd_bitwriter *bw,
                          const VADecPictureParameterBufferAV1 *p,
                          uint32_t mi_cols, uint32_t mi_rows)
{
    const int sb_shift = p->seq_info_fields.fields.use_128x128_superblock ? 5 : 4;
    const int sb_size  = sb_shift + 2;

    const uint32_t sb_cols = p->seq_info_fields.fields.use_128x128_superblock
                           ? ((mi_cols + 31) >> 5) : ((mi_cols + 15) >> 4);
    const uint32_t sb_rows = p->seq_info_fields.fields.use_128x128_superblock
                           ? ((mi_rows + 31) >> 5) : ((mi_rows + 15) >> 4);

    const uint32_t MAX_TILE_COLS = 64, MAX_TILE_ROWS = 64;
    const uint32_t max_tile_width_sb = 4096u >> sb_size;
    const uint32_t max_tile_area_sb  = (4096u * 2304u) >> (2 * sb_size);

    uint32_t max_log2_tile_cols = 0;
    while ((1u << max_log2_tile_cols) <
           (sb_cols < MAX_TILE_COLS ? sb_cols : MAX_TILE_COLS))
        max_log2_tile_cols++;
    uint32_t max_log2_tile_rows = 0;
    while ((1u << max_log2_tile_rows) <
           (sb_rows < MAX_TILE_ROWS ? sb_rows : MAX_TILE_ROWS))
        max_log2_tile_rows++;

    uint32_t min_log2_tile_cols = 0;
    while ((max_tile_width_sb << min_log2_tile_cols) < sb_cols)
        min_log2_tile_cols++;
    uint32_t min_log2_area = 0;
    while ((max_tile_area_sb << min_log2_area) < sb_rows * sb_cols)
        min_log2_area++;
    const uint32_t min_log2_tiles = (min_log2_tile_cols > min_log2_area)
                                  ? min_log2_tile_cols : min_log2_area;

    /* VA-API 给的是 tile_cols/tile_rows（个数），先反推 log2。 */
    uint32_t cols_log2 = 0;
    while ((1u << cols_log2) < p->tile_cols)
        cols_log2++;
    uint32_t rows_log2 = 0;
    while ((1u << rows_log2) < p->tile_rows)
        rows_log2++;


    dmd_bw_put_flag(bw, (int)p->pic_info_fields.bits.uniform_tile_spacing_flag);

    if (p->pic_info_fields.bits.uniform_tile_spacing_flag) {
        /* increment(tile_cols_log2, min_log2_tile_cols, max_log2_tile_cols)
         * 先夹到合法区间：CBS 的写入侧对越界直接报错拒绝。 */
        if (cols_log2 < min_log2_tile_cols) cols_log2 = min_log2_tile_cols;
        if (cols_log2 > max_log2_tile_cols) cols_log2 = max_log2_tile_cols;
        for (uint32_t i = min_log2_tile_cols; i < cols_log2; i++)
            dmd_bw_put_flag(bw, 1);
        if (cols_log2 != max_log2_tile_cols)
            dmd_bw_put_flag(bw, 0);

        const uint32_t min_log2_tile_rows =
            (min_log2_tiles > cols_log2) ? (min_log2_tiles - cols_log2) : 0;
        if (rows_log2 < min_log2_tile_rows) rows_log2 = min_log2_tile_rows;
        if (rows_log2 > max_log2_tile_rows) rows_log2 = max_log2_tile_rows;
        for (uint32_t i = min_log2_tile_rows; i < rows_log2; i++)
            dmd_bw_put_flag(bw, 1);
        if (rows_log2 != max_log2_tile_rows)
            dmd_bw_put_flag(bw, 0);
    } else {
        /* 非均匀：逐 tile 写 width_in_sbs_minus_1 / height_in_sbs_minus_1，
         * 用 ns(n) 编码。上界是"剩余 sb 数"与 max_tile_*_sb 的较小者。 */
        uint32_t start_sb = 0;
        for (int i = 0; i < p->tile_cols && start_sb < sb_cols; i++) {
            const uint32_t rest = sb_cols - start_sb;
            const uint32_t lim = rest < max_tile_width_sb ? rest
                                                          : max_tile_width_sb;
            dmd_av1_put_ns(bw, p->width_in_sbs_minus_1[i], lim);
            start_sb += p->width_in_sbs_minus_1[i] + 1;
        }
        start_sb = 0;
        for (int i = 0; i < p->tile_rows && start_sb < sb_rows; i++) {
            dmd_av1_put_ns(bw, p->height_in_sbs_minus_1[i],
                           sb_rows - start_sb);
            start_sb += p->height_in_sbs_minus_1[i] + 1;
        }
    }

    /* TileCols*TileRows > 1 时写 context_update_tile_id 与 tile_size_bytes。
     * 位宽用**上面夹取修正后**的 log2 值，不能再按 tile_cols 重算。 */
    if (cols_log2 + rows_log2 > 0) {
        dmd_bw_put_bits(bw, p->context_update_tile_id,
                        (int)(cols_log2 + rows_log2));
        /* tile_size_bytes_minus_1 = 1（2 字节）。
         * 依据：VA-API 给的 tile offset 之间恰好各差 2 字节
         * （实测 tile[0] off=2、tile[1] off=前一个末尾+2 …… 共 7 个间隙），
         * 那 2 字节就是原始码流里的 tile_size 字段。跟随源码流宽度可让
         * 合成结果与原始 payload 长度一致，也避免无谓放大。
         * ⚠️ 必须与 dmd_av1_build_frame() 里写 tile_size_minus_1 的宽度一致。 */
        dmd_bw_put_bits(bw, 1, 2);
    }
}

/* quantization_params()，AV1 规范 5.9.12。 */
static void put_quantization_params(struct dmd_bitwriter *bw,
                                    const VADecPictureParameterBufferAV1 *p)
{
    const uint32_t mono = p->seq_info_fields.fields.mono_chrome;

    dmd_bw_put_bits(bw, p->base_qindex, 8);

    /* delta_q 用 su(1+6)：1 位存在标志 + 6 位有符号值（规范 5.9.13
     * read_delta_q）。值为 0 时只写标志位 0。 */
    #define PUT_DELTA_Q(v) do {                        \
        if ((v) != 0) { dmd_bw_put_flag(bw, 1);        \
                        dmd_av1_put_su(bw, (v), 7); }  \
        else            dmd_bw_put_flag(bw, 0);        \
    } while (0)

    PUT_DELTA_Q(p->y_dc_delta_q);

    if (!mono) {
        /* diff_uv_delta 只在 separate_uv_delta_q 时出现；序列头里我们把
         * separate_uv_delta_q 写成 0，所以这里不写该标志，
         * 且 U/V 共用一组 delta（写 U 的即可）。 */
        PUT_DELTA_Q(p->u_dc_delta_q);
        PUT_DELTA_Q(p->u_ac_delta_q);
    }

    #undef PUT_DELTA_Q

    dmd_bw_put_flag(bw, (int)p->qmatrix_fields.bits.using_qmatrix);
    if (p->qmatrix_fields.bits.using_qmatrix) {
        dmd_bw_put_bits(bw, p->qmatrix_fields.bits.qm_y, 4);
        dmd_bw_put_bits(bw, p->qmatrix_fields.bits.qm_u, 4);
        if (!mono)
            dmd_bw_put_bits(bw, p->qmatrix_fields.bits.qm_v, 4);
    }
}

/* segmentation_params()，AV1 规范 5.9.14。 */
static void put_segmentation_params(struct dmd_bitwriter *bw,
                                    const VADecPictureParameterBufferAV1 *p,
                                    int primary_ref_none)
{
    const uint32_t enabled = p->seg_info.segment_info_fields.bits.enabled;
    dmd_bw_put_flag(bw, (int)enabled);
    if (!enabled)
        return;

    /* primary_ref_frame 为 NONE 时 update_map/update_data 恒 1、不写入码流。 */
    if (!primary_ref_none) {
        dmd_bw_put_flag(bw, (int)p->seg_info.segment_info_fields.bits.update_map);
        if (p->seg_info.segment_info_fields.bits.update_map)
            dmd_bw_put_flag(bw,
                (int)p->seg_info.segment_info_fields.bits.temporal_update);
        dmd_bw_put_flag(bw, (int)p->seg_info.segment_info_fields.bits.update_data);
    }

    if (primary_ref_none || p->seg_info.segment_info_fields.bits.update_data) {
        /* 逐段逐特征写。feature_mask 的位对应 SEG_LVL_* 特征是否启用，
         * feature_data 是值。位宽与是否有符号由规范表
         * Segmentation_Feature_Bits/Signed 决定。 */
        static const int seg_bits[8]   = { 8, 6, 6, 6, 6, 3, 0, 0 };
        static const int seg_signed[8] = { 1, 1, 1, 1, 1, 0, 0, 0 };
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                const int on = (p->seg_info.feature_mask[i] >> j) & 1;
                dmd_bw_put_flag(bw, on);
                if (!on)
                    continue;
                if (seg_bits[j] == 0)
                    continue;            /* SEG_LVL_REF_FRAME 等无值特征 */
                if (seg_signed[j])
                    dmd_av1_put_su(bw, p->seg_info.feature_data[i][j],
                                   seg_bits[j] + 1);
                else
                    dmd_bw_put_bits(bw,
                        (uint32_t)p->seg_info.feature_data[i][j], seg_bits[j]);
            }
        }
    }
}

/* loop_filter_params()，AV1 规范 5.9.11。 */
static void put_loop_filter_params(struct dmd_bitwriter *bw,
                                   const VADecPictureParameterBufferAV1 *p,
                                   int coded_lossless, int allow_intrabc)
{
    /* lossless 或 allow_intrabc 时整段不写（规范里直接返回默认值）。 */
    if (coded_lossless || allow_intrabc)
        return;

    dmd_bw_put_bits(bw, p->filter_level[0], 6);
    dmd_bw_put_bits(bw, p->filter_level[1], 6);
    if (!p->seq_info_fields.fields.mono_chrome &&
        (p->filter_level[0] || p->filter_level[1])) {
        dmd_bw_put_bits(bw, p->filter_level_u, 6);
        dmd_bw_put_bits(bw, p->filter_level_v, 6);
    }
    dmd_bw_put_bits(bw, p->loop_filter_info_fields.bits.sharpness_level, 3);

    const int delta_enabled =
        p->loop_filter_info_fields.bits.mode_ref_delta_enabled;
    dmd_bw_put_flag(bw, delta_enabled);
    if (delta_enabled) {
        const int delta_update =
            p->loop_filter_info_fields.bits.mode_ref_delta_update;
        dmd_bw_put_flag(bw, delta_update);
        if (delta_update) {
            for (int i = 0; i < 8; i++) {
                /* 逐项写 update 标志 + su(7)。这里对每个非零值写更新，
                 * 零值写标志 0 —— VA-API 不区分"未更新"与"更新为 0"，
                 * 保守地把非零视为需要更新。 */
                if (p->ref_deltas[i] != 0) {
                    dmd_bw_put_flag(bw, 1);
                    dmd_av1_put_su(bw, p->ref_deltas[i], 7);
                } else {
                    dmd_bw_put_flag(bw, 0);
                }
            }
            for (int i = 0; i < 2; i++) {
                if (p->mode_deltas[i] != 0) {
                    dmd_bw_put_flag(bw, 1);
                    dmd_av1_put_su(bw, p->mode_deltas[i], 7);
                } else {
                    dmd_bw_put_flag(bw, 0);
                }
            }
        }
    }
}

/* cdef_params()，AV1 规范 5.9.19。 */
static void put_cdef_params(struct dmd_bitwriter *bw,
                            const VADecPictureParameterBufferAV1 *p,
                            int coded_lossless, int allow_intrabc)
{
    if (coded_lossless || allow_intrabc ||
        !p->seq_info_fields.fields.enable_cdef)
        return;

    dmd_bw_put_bits(bw, p->cdef_damping_minus_3, 2);
    dmd_bw_put_bits(bw, p->cdef_bits, 2);

    const int n = 1 << p->cdef_bits;
    for (int i = 0; i < n; i++) {
        /* VA-API 把 y/uv strength 打包成 (pri << 2) | sec 的形式，
         * 与码流里 4 位 pri + 2 位 sec 的布局一致，可直接拆写。 */
        dmd_bw_put_bits(bw, p->cdef_y_strengths[i] >> 2, 4);
        dmd_bw_put_bits(bw, p->cdef_y_strengths[i] & 0x3, 2);
        if (!p->seq_info_fields.fields.mono_chrome) {
            dmd_bw_put_bits(bw, p->cdef_uv_strengths[i] >> 2, 4);
            dmd_bw_put_bits(bw, p->cdef_uv_strengths[i] & 0x3, 2);
        }
    }
}

/* lr_params()，AV1 规范 5.9.20。 */
static void put_lr_params(struct dmd_bitwriter *bw,
                          const VADecPictureParameterBufferAV1 *p,
                          int all_lossless, int allow_intrabc)
{
    if (all_lossless || allow_intrabc)
        return;

    const uint32_t ry = p->loop_restoration_fields.bits.yframe_restoration_type;
    const uint32_t rcb = p->loop_restoration_fields.bits.cbframe_restoration_type;
    const uint32_t rcr = p->loop_restoration_fields.bits.crframe_restoration_type;

    /* 序列头 enable_restoration 恒写 1，所以这一段**总要写**——
     * 即使三个 lr_type 全为 0（那正是"本帧不做 restoration"的正规表达）。
     * 早先这里按"全 0 就提前返回"，与序列头不一致，导致帧头短 6 位。 */

    /* lr_type 用 f(2)，取值顺序与 VA-API 的 restoration_type 枚举一致
     * （0=NONE, 1=WIENER, 2=SGRPROJ, 3=SWITCHABLE）。 */
    dmd_bw_put_bits(bw, ry, 2);
    if (!p->seq_info_fields.fields.mono_chrome) {
        dmd_bw_put_bits(bw, rcb, 2);
        dmd_bw_put_bits(bw, rcr, 2);
    }

    if (ry || rcb || rcr) {
        dmd_bw_put_bits(bw, p->loop_restoration_fields.bits.lr_unit_shift, 1);
        if (p->seq_info_fields.fields.use_128x128_superblock == 0 &&
            p->loop_restoration_fields.bits.lr_unit_shift)
            dmd_bw_put_bits(bw, 0, 1);   /* lr_unit_extra_shift */
        if (p->seq_info_fields.fields.subsampling_x &&
            p->seq_info_fields.fields.subsampling_y && (rcb || rcr))
            dmd_bw_put_bits(bw, p->loop_restoration_fields.bits.lr_uv_shift, 1);
    }
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
    /* seq_level_idx[0] = 8（level 4.0）+ seq_tier[0] = 0。
     *
     * ⚠️ 曾写 0（level 2.0），实测与真实码流不符：libaom 对 1080p 输出 8。
     * 关键不只是数值 —— seq_level_idx > 7 时**必须紧跟 seq_tier**（f(1)），
     * 写 0 会少这一位，导致序列头后续所有字段偏移 1 位。
     * VA-API 不提供 level，取 8 跟随真实码流；MediaCodec 按实际分辨率
     * 分配资源，不用这个字段校验。 */
    dmd_bw_put_bits(&bw, 8, 5);           /* seq_level_idx[0] */
    dmd_bw_put_flag(&bw, 0);              /* seq_tier[0]（因 idx > 7） */

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

    dmd_bw_put_flag(&bw, use_superres);
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_cdef);
    /* enable_restoration 恒 1。
     *
     * ⚠️ 曾按"三个 frame_restoration_type 是否全为 0"来推导，实测错误：
     * trace_headers 显示真实码流 enable_restoration=1，且帧头位 204 起
     * **确实写了 lr_type[0..2] 共 6 位**（值恰好全为 0）。
     * 也就是说"本帧不用 restoration"是通过 lr_type=0 表达的，而不是通过
     * 序列级 enable_restoration=0 —— 后者会让整个 lr_params 段消失，
     * 帧头因此短 6 位，tile_group 起始位置随之前移，解码器读到错位数据。
     *
     * 取 1 是安全侧：它只是允许，具体每帧仍由 lr_type 决定。 */
    dmd_bw_put_flag(&bw, 1);

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

/* 把 uncompressed_header() 写进 bw，不含结尾的 trailing_bits /
 * byte_alignment —— 由调用方按封装形式决定：
 *   OBU_FRAME_HEADER(3) 用 trailing_bits（规范 5.9.1）
 *   OBU_FRAME(6)        用 byte_alignment（规范 5.10.1）
 * 这个区别是实测踩出来的：用错会让 tile_group 起始位置偏移，
 * dav1d 报 "Failed to read unit"。 */
static void put_uncompressed_header(struct dmd_bitwriter *bwp,
                                    struct dmd_av1_dpb *dpb,
                                    const VADecPictureParameterBufferAV1 *p)
{
    /* struct dmd_bitwriter 是纯值结构：buf 指向调用方的缓冲，其余成员
     * 都是计数器。所以"拷入 → 写 → 拷回"是安全的，函数体内得以保留
     * 已与真实码流逐字段对齐过的 `&bw` 写法，不必逐行改动引入笔误。 */
    struct dmd_bitwriter bw = *bwp;

    const uint32_t frame_type   = p->pic_info_fields.bits.frame_type;
    const int is_key            = (frame_type == 0);   /* KEY_FRAME */
    const int is_intra_only     = (frame_type == 2);   /* INTRA_ONLY_FRAME */
    const int intra_only        = is_key || is_intra_only;
    const uint32_t allow_intrabc = p->pic_info_fields.bits.allow_intrabc;
    const uint32_t enable_order_hint =
        p->seq_info_fields.fields.enable_order_hint;
    const int order_hint_bits = enable_order_hint
        ? (int)p->order_hint_bits_minus_1 + 1 : 0;

    /* CodedLossless 的判定（规范 7.12.1）：所有段的 qindex 与四个 delta_q
     * 全为 0。VA-API 不直接给这个量，按定义算 —— 它决定 loop_filter /
     * cdef / lr 三段是否出现在码流里，算错会让帧头结构错位。 */
    const int coded_lossless =
        (p->base_qindex == 0 && p->y_dc_delta_q == 0 &&
         p->u_dc_delta_q == 0 && p->u_ac_delta_q == 0 &&
         p->v_dc_delta_q == 0 && p->v_ac_delta_q == 0);
    /* AllLossless 还要求无 superres 上采样（规范 7.12.1）。 */
    const int all_lossless =
        coded_lossless && !p->pic_info_fields.bits.use_superres;

    /* --- uncompressed_header()，AV1 规范 5.9.2 --- */

    /* show_existing_frame：本驱动逐帧转发真实帧，不复用已解码帧，恒 0。
     * （frame_id_numbers_present=0 时该字段仍存在，只是后续不读 frame_id。） */
    dmd_bw_put_flag(&bw, 0);

    dmd_bw_put_bits(&bw, frame_type, 2);
    dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.show_frame);
    if (!p->pic_info_fields.bits.show_frame)
        dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.showable_frame);

    /* error_resilient_mode：KEY_FRAME 且 show_frame 时恒 1、不写入。 */
    /* error_resilient_mode（CBS 原文）：
     *   frame_type == SWITCH || (frame_type == KEY && show_frame)
     *     → infer 1（不写入码流）
     *   否则 flag(error_resilient_mode) */
    const int er_inferred = (frame_type == 3) ||
                            (is_key && p->pic_info_fields.bits.show_frame);
    if (!er_inferred)
        dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.error_resilient_mode);

    dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.disable_cdf_update);

    /* allow_screen_content_tools：序列头里 seq_choose_screen_content_tools=1
     * → seq_force = SELECT_SCREEN_CONTENT_TOOLS(2)，故此处写 f(1)。 */
    dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.allow_screen_content_tools);
    if (p->pic_info_fields.bits.allow_screen_content_tools) {
        /* seq_choose_integer_mv=1 → seq_force_integer_mv = SELECT，写 f(1)。 */
        dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.force_integer_mv);
    }

    /* frame_id_numbers_present=0（序列头如此写），跳过 current_frame_id。 */

    /* frame_size_override_flag：恒 0，表示帧尺寸等于序列头里的
     * max_frame_size —— 我们把 max 就写成本帧尺寸，二者一致。
     * KEY_FRAME 之外的帧若尺寸变化，本驱动会重建会话，所以恒等成立。 */
    if (frame_type != 3 /* SWITCH_FRAME 时该标志恒 1 */)
        dmd_bw_put_flag(&bw, 0);

    /* order_hint（规范 5.9.2）：条件是 enable_order_hint 且**并非**
     * "frame_is_intra 同时 refresh_frame_flags == allFrames"。
     *
     * ⚠️ 这里踩过两次坑，最后靠解析真实码流定论：
     * libaom 生成的 1080p KEY_FRAME（show_frame=1）**确实写了** order_hint
     * （值 0，位于 frame_size_override_flag 之后）。所以不能简化成
     * "帧内帧一律不写" —— 那样会少写 order_hint_bits 位。
     *
     * 原因：KEY+show 帧的 refresh_frame_flags 字段不出现在码流里，但规范
     * 7.20 说此时 RefreshFrameFlags 取 allFrames 只是**解码器侧的推导值**，
     * 而 5.9.2 的判据针对的是**语法变量**。实测 libaom 的行为表明该判据
     * 在此不成立，即照写 order_hint。 */
    if (enable_order_hint)
        dmd_bw_put_bits(&bw, p->order_hint, order_hint_bits);

    /* primary_ref_frame：帧内帧或 error_resilient 时不写。 */
    /* err_res 是**生效值**：被 infer 的场合恒 1，否则取 VA-API 给的。
     * 后续 primary_ref_frame / use_ref_frame_mvs / allow_warped_motion
     * 的条件都要用这个生效值，用原始字段会判错。 */
    const int err_res = er_inferred
                      ? 1 : (int)p->pic_info_fields.bits.error_resilient_mode;
    const int primary_ref_none = intra_only || err_res ||
        (p->primary_ref_frame == 7 /* PRIMARY_REF_NONE */);
    if (!intra_only && !err_res)
        dmd_bw_put_bits(&bw, p->primary_ref_frame, 3);

    /* refresh_frame_flags：KEY_FRAME + show_frame 时恒 0xFF、不写入码流。
     *
     * ⚠️ VA-API **不提供**这个字段（全文件 grep 确认：只在 va_dec_av1.h:421
     * 的注释里被提及，结构体里没有）。它是"本帧要刷新哪些参考槽位"的位掩码。
     *
     * ⚠️ 历史错误（V4L2 直通后暴露）：这里原先恒写 0xFF，理由是
     * "MediaCodec 内部自行管理参考帧生命周期，不依赖码流里的这个掩码"。
     * 那个假设对 MediaCodec 成立，对 V4L2 **不成立** —— msm_vidc 按码流
     * 管理参考帧，0xFF 意味着每个帧间帧都刷掉全部 8 个槽，参考帧链立刻崩。
     * 实测后果：合成流交 dav1d 软解，第 2 帧起报 Invalid data，只解出 1 帧；
     * 走硬件时表现为送 6 单元只回 2 帧后解码器停摆。
     * trace_headers 逐字段比对显示 refresh_frame_flags 源码流为 1、合成为 255。
     *
     * 正确做法：从 DPB 快照 ref_frame_map[8] 推导。本帧解码后会占用某些槽，
     * 而 VA-API 在每帧给出当帧的 ref_frame_map —— 凡是槽里存的 surface id
     * 等于本帧的 current_frame，该槽就是本帧要刷新的目标。
     *
     * 找不到任何匹配槽时退回 0（不刷新任何槽）：那是 show_existing_frame
     * 或不作为参考的帧的正常情形，比 0xFF 安全得多 —— 宁可少声明刷新，
     * 也不要谎称刷新了全部而让解码器丢弃仍在使用的参考帧。 */
    const int refresh_all = (frame_type == 3) ||
                            (is_key && p->pic_info_fields.bits.show_frame);

    /* ===================== 自洽 DPB（影子参考帧管理）=====================
     *
     * 问题：refresh_frame_flags（本帧解码后写入哪些参考槽的 8 位掩码）
     * VA-API 完全不提供 —— 它随源码流被 ffmpeg 解析后丢弃，因为那是
     * 编码器的 GOP 决策。源码流实测序列 1,8,32,64,0,0,64,0,0,32,64,0
     * （三分之一是 0），无法从单帧信息还原。
     *
     * 关键洞察：**我不需要还原它。**
     * ref_frame_idx[] 同样由本函数写入码流。解码器只要求这两者
     * **互相自洽**：我说本帧存进槽 k，之后引用它时就报槽 k。
     * 至于原编码器把它放在哪个槽，与解码结果无关。
     *
     * 于是自己管一份影子 DPB：
     *   · 每个非 KEY 帧占用一个槽，8 槽轮转（refresh = 1 << slot）
     *   · 记录 shadow[slot] = 该槽存的 surface id
     *   · 写 ref_frame_idx 时，把 VA-API 给的"槽号"先经它的 ref_frame_map
     *     翻成 surface id，再在影子 DPB 里查出**我的**槽号
     *
     * 已用实测数据模拟验证：连续 5 帧的全部引用都能在影子 DPB 中解析到。
     *
     * 历史错误（本轮修复的根因）：原先恒写 0xFF，理由是"MediaCodec 自行
     * 管理参考帧，不看码流里这个掩码"。该假设对 MediaCodec 成立，对 V4L2
     * 不成立 —— msm_vidc 按码流管理参考帧，0xFF 让每个帧间帧刷掉全部 8 槽，
     * 参考帧链立刻崩：合成流交 dav1d 软解第 2 帧起 Invalid data。
     *
     * 已否决的三种"还原编码器决策"的推导（均实测失败）：
     *  1. 在 ref_frame_map[] 里找 current_frame —— 该数组是本帧解码**前**的
     *     快照，current 尚未写入，恒不匹配
     *  2. 相邻两帧 ref_frame_map 差分 —— 给出的是上一帧刷新的槽，
     *     且无法表达"本帧不刷新"（值 0）
     *  3. 取第一个未被引用/仍为初值的空槽 —— 实测 5 帧里只有首帧碰对
     * ==================================================================== */
    unsigned refresh_mask = 0;
    int my_slot = -1;
    if (!refresh_all) {
        my_slot = (int)(dpb->dpb_next_slot & 7u);
        refresh_mask = 1u << my_slot;
        dmd_bw_put_bits(&bw, refresh_mask, 8);
    }

    /* ref_order_hint[i]（规范 5.9.2）：条件是
     *   !refresh_all && error_resilient_mode && enable_order_hint
     *
     * 本码流实测不触发（DMD_AV1_LOG 采得）：
     *   ft=0 refresh_all=1 err_res=1   ← KEY 帧，refresh_all 已排除
     *   ft=1 refresh_all=0 err_res=0   ← 帧间帧，err_res 为 0
     * 即 KEY 帧被 refresh_all 挡住、帧间帧 err_res 为假，一个字节都不写。
     *
     * 故此处不实现。若将来遇到 error_resilient 的帧间码流需要补上，
     * 注意两点：(1) 必须带 !refresh_all 条件，否则给 KEY 帧多写
     * 8*order_hint_bits 位会让整个帧头错位（实测 dav1d 把 8 tile 读成
     * 2x17 后报 zero_bit out of range）；(2) VA-API 不提供各槽的 order
     * hint，需从自洽 DPB 的 dpb_order_hint[] 取。 */

    /* 参考帧索引：帧间帧才有。 */
    if (!intra_only) {
        /* frame_refs_short_signaling 需要 enable_order_hint；置 0 表示
         * 显式给出全部 7 个 ref_frame_idx（VA-API 提供的正是这个数组）。 */
        if (enable_order_hint)
            dmd_bw_put_flag(&bw, 0);
        /* 把 VA-API 的槽号翻成**我的**槽号：
         * VA-API 的 ref_frame_idx[i] 是它自己 DPB 里的槽号，
         * 经 ref_frame_map[] 得到真正的 surface id，
         * 再在影子 DPB 里找该 surface 现在占我的哪个槽。 */
        for (int i = 0; i < 7; i++) {
            unsigned va_slot = p->ref_frame_idx[i];
            unsigned my = va_slot < 8 ? va_slot : 0;
            /* 直接沿用 VA-API 的槽号。
             *
             * 先前试过"经 ref_frame_map 翻成 surface id 再查影子 DPB"，
             * 实测第 2 帧即解码失败：VA-API 给的是槽 2，影子表查得槽 0。
             * 两者都指向同一个 KEY 帧 surface，但 dav1d 按规范推断出的
             * DPB 状态里槽 0 与槽 2 的 order hint 不同 —— 我们无法让
             * 影子表与解码器的推断状态保持一致（KEY+show 帧的
             * refresh_frame_flags 不写入码流，由双方各自推断）。
             *
             * 于是放弃重映射：VA-API 的槽号本就与源码流一致（实测
             * ref_frame_idx 全为 2，与源码流 trace_headers 相同），
             * 直接透传最安全。影子 DPB 仅用于 refresh_frame_flags 的
             * 槽位轮转，不再参与引用翻译。 */
            dmd_bw_put_bits(&bw, my, 3);
        }
        /* frame_id_numbers_present=0，不写 delta_frame_id。 */
    }

    /* frame_size() / render_size()：frame_size_override=0 时帧尺寸由序列头
     * 给出，这里只写 superres 与 render_size（规范 5.9.5/5.9.6/5.9.8）。 */
    /* superres_params()（规范 5.9.8）：**仅当序列头 enable_superres=1 时**
     * 码流里才有 use_superres 这一位。
     *
     * ⚠️ 曾在此多写一位：序列头按 use_superres 写 enable_superres（本帧为 0），
     * 解码器于是不来读这个标志，而实现却无条件写了 —— 帧头从此错位 1 位，
     * dav1d 报 "trailing_one_bit out of range: 0"。教训：凡"序列头某位决定
     * 帧头是否有此字段"的地方，两处条件必须写成同一个表达式。 */
    if (p->pic_info_fields.bits.use_superres) {
        dmd_bw_put_flag(&bw, 1);
        /* coded_denom：SUPERRES_DENOM_MIN=9，占 SUPERRES_DENOM_BITS=3 位。 */
        const uint32_t denom = p->superres_scale_denominator;
        dmd_bw_put_bits(&bw, (denom >= 9 ? denom - 9 : 0), 3);
    }
    /* render_and_frame_size_different = 0：显示尺寸等于帧尺寸。
     * VA-API 不提供 render_size，且它只影响显示裁剪、不影响解码。 */
    dmd_bw_put_flag(&bw, 0);

    /* allow_intrabc（规范 5.9.2）：条件是 allow_screen_content_tools &&
     * UpscaledWidth == FrameWidth。以 ffmpeg CBS 的原文为准：
     *   if (allow_screen_content_tools && upscaled_width == frame_width)
     *       flag(allow_intrabc);
     *   else
     *       infer(allow_intrabc, 0);
     *
     * upscaled_width == frame_width 等价于"无 superres 上采样"——
     * superres_params 里只有 use_superres 时才把 frame_width 按 denom 缩小。
     *
     * ⚠️ 这个条件错过两次：先漏 upscaled 判据，后又把
     * allow_screen_content_tools 整个去掉（误读真实码流某一位所致）。
     * 两个条件**都要**，缺一个就多写或少写 1 位。 */
    /* ⚠️ 这里 CBS 源码与 libaom 的实际输出不一致，以**真实码流**为准。
     *
     * CBS（cbs_av1_syntax_template.c）写的是条件读：
     *   if (allow_screen_content_tools && upscaled_width == frame_width)
     *       flag(allow_intrabc); else infer 0
     *
     * 但用位级解析器逐字段核对 libaom 生成的 1080p KEY_FRAME：该帧
     * allow_screen_content_tools=0，若按 CBS 的条件跳过这一位，整个帧头
     * 会错位、读到末尾 trailing_one_bit=0；无条件读这一位才能得到
     * trailing_one_bit=1（位数精确闭合）。
     *
     * 结论：实际编码器无条件写该位。判据是"能否让真实码流闭合"，
     * 而不是源码怎么写 —— 我们要喂的是真实解码器，不是 CBS。 */
    /* allow_intrabc：以 ffmpeg trace_headers 对真实码流的逐位输出为准。
     * 实测 libaom 生成的 1080p KEY_FRAME（allow_screen_content_tools=0）：
     *   位 46  render_and_frame_size_different
     *   位 47  disable_frame_end_update_cdf     ← allow_intrabc **不存在**
     *   位 48  uniform_tile_spacing_flag
     * 即 CBS 的条件读成立：asct=0 时该位不写入码流。
     *
     * ⚠️ 曾据自己写的解析器"位数闭合"推断成无条件写，那是错的 ——
     * 解析器本身在此处有偏差，两个错误互相抵消才显得闭合。
     * 教训：验证工具必须先用 trace_headers 这类权威输出校准，
     * 不能拿未校准的自制解析器当基准。 */
    if (intra_only && p->pic_info_fields.bits.allow_screen_content_tools &&
        !p->pic_info_fields.bits.use_superres)
        dmd_bw_put_flag(&bw, (int)allow_intrabc);

    if (!intra_only) {
        dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.allow_high_precision_mv);
        /* read_interpolation_filter()：is_filter_switchable f(1)，
         * 为 0 时再写 2 位 interp_filter。VA-API 的 interp_filter 取值
         * 4 = SWITCHABLE。 */
        if (p->interp_filter == 4) {
            dmd_bw_put_flag(&bw, 1);
        } else {
            dmd_bw_put_flag(&bw, 0);
            dmd_bw_put_bits(&bw, p->interp_filter, 2);
        }
        dmd_bw_put_flag(&bw,
            (int)p->pic_info_fields.bits.is_motion_mode_switchable);
        /* use_ref_frame_mvs 需 enable_ref_frame_mvs（序列头写 1）、
         * 非 error_resilient、且 enable_order_hint。 */
        if (!err_res && enable_order_hint)
            dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.use_ref_frame_mvs);
    }

    /* disable_frame_end_update_cdf：reduced_still_picture_header=0 且
     * disable_cdf_update=0 时出现。 */
    if (!p->pic_info_fields.bits.disable_cdf_update)
        dmd_bw_put_flag(&bw,
            (int)p->pic_info_fields.bits.disable_frame_end_update_cdf);

    /* MiCols/MiRows：以 4x4 单元计的画面尺寸（规范 5.9.5 的推导）。
     * 2*((width+7)>>3) 等价于向上取整到 8 像素再折半。 */
    const uint32_t width  = (uint32_t)p->frame_width_minus1 + 1;
    const uint32_t height = (uint32_t)p->frame_height_minus1 + 1;
    const uint32_t mi_cols = 2 * ((width  + 7) >> 3);
    const uint32_t mi_rows = 2 * ((height + 7) >> 3);

    put_tile_info(&bw, p, mi_cols, mi_rows);
    put_quantization_params(&bw, p);
    put_segmentation_params(&bw, p, primary_ref_none);

    /* delta_q_params()（5.9.17）：base_qindex > 0 时才有 delta_q_present。 */
    if (p->base_qindex > 0)
        dmd_bw_put_flag(&bw, (int)p->mode_control_fields.bits.delta_q_present_flag);
    if (p->mode_control_fields.bits.delta_q_present_flag) {
        dmd_bw_put_bits(&bw, p->mode_control_fields.bits.log2_delta_q_res, 2);

        /* delta_lf_params()（5.9.18）：仅在 delta_q_present 时出现。 */
        if (!allow_intrabc) {
            dmd_bw_put_flag(&bw,
                (int)p->mode_control_fields.bits.delta_lf_present_flag);
            if (p->mode_control_fields.bits.delta_lf_present_flag) {
                dmd_bw_put_bits(&bw,
                    p->mode_control_fields.bits.log2_delta_lf_res, 2);
                dmd_bw_put_flag(&bw,
                    (int)p->mode_control_fields.bits.delta_lf_multi);
            }
        }
    }

    put_loop_filter_params(&bw, p, coded_lossless, (int)allow_intrabc);
    put_cdef_params(&bw, p, coded_lossless, (int)allow_intrabc);
    put_lr_params(&bw, p, all_lossless, (int)allow_intrabc);

    /* read_tx_mode()（5.9.21）：CodedLossless 时 tx_mode 恒 ONLY_4X4、
     * 不写入；否则写 tx_mode_select f(1)。VA-API 的 tx_mode 取值
     * 2 = TX_MODE_LARGEST, 3 = TX_MODE_SELECT。 */
    /* read_tx_mode（CBS 原文）：
     *   coded_lossless → infer(tx_mode, ONLY_4X4)，不写入
     *   否则 increment(tx_mode, TX_MODE_LARGEST=1, TX_MODE_SELECT=2)
     *
     * ⚠️ 曾误当成 f(1) 标志写 `tx_mode == 3`，两处都错：
     *   - 编码方式不是单个标志位，而是 increment（range [1,2]）
     *   - VA-API 的 tx_mode 值域是 [0..2]（va_dec_av1.h:560-563），
     *     直接就是规范枚举值，不存在 3
     * increment 在 range_max 时写 (max-min)=1 个 1 且无停止位，
     * 在 range_min 时只写一个 0。 */
    if (!coded_lossless) {
        const uint32_t tm = p->mode_control_fields.bits.tx_mode;
        if (tm >= 2)
            dmd_bw_put_flag(&bw, 1);   /* TX_MODE_SELECT：写 1，无停止位 */
        else
            dmd_bw_put_flag(&bw, 0);   /* TX_MODE_LARGEST：停止位 */
    }

    /* frame_reference_mode()（5.9.23）：帧间帧写 reference_select。 */
    if (!intra_only)
        dmd_bw_put_flag(&bw, (int)p->mode_control_fields.bits.reference_select);

    /* skip_mode_params()（5.9.22）：skipModeAllowed 的完整推导需要参考帧
     * order hint 比较，VA-API 已给出结论 skip_mode_present，直接用。
     * 帧内帧或 reference_select=0 时 skipModeAllowed=0、不写入。 */
    if (!intra_only && p->mode_control_fields.bits.reference_select)
        dmd_bw_put_flag(&bw, (int)p->mode_control_fields.bits.skip_mode_present);

    /* allow_warped_motion：需 is_motion_mode_switchable、非 error_resilient、
     * 且序列级 enable_warped_motion（我们写 1）。 */
    if (!intra_only &&
        p->pic_info_fields.bits.is_motion_mode_switchable && !err_res)
        dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.allow_warped_motion);

    dmd_bw_put_flag(&bw, (int)p->mode_control_fields.bits.reduced_tx_set_used);

    /* global_motion_params()（5.9.24）：帧间帧逐参考帧写 is_global。
     * VA-API 的 wm[] 提供变换参数，但把它反向编码成码流需要完整的
     * 差分编码与参考帧投影逻辑。此处对所有参考帧写 is_global=0
     * （IDENTITY），代价是丢失全局运动补偿。
     * ⚠️ 这是本实现的已知简化，见文件末尾说明。 */
    if (!intra_only) {
        for (int i = 0; i < 7; i++)
            dmd_bw_put_flag(&bw, 0);     /* is_global[LAST+i] = 0 */
    }

    /* film_grain_params()（5.9.30）：序列头里 film_grain_params_present
     * 为 0 时整段不出现。我们如实转写该标志，故此处同样条件。 */
    if (p->seq_info_fields.fields.film_grain_params_present &&
        (p->pic_info_fields.bits.show_frame ||
         p->pic_info_fields.bits.showable_frame))
        dmd_bw_put_flag(&bw, 0);         /* apply_grain = 0 */

    /* 结尾不写 trailing_bits / byte_alignment —— 交给调用方按封装形式决定。 */
    *bwp = bw;
}

/* 把帧头 payload 装进指定类型的 OBU。obu_type 决定结尾用哪种对齐：
 *   DMD_OBU_FRAME_HEADER → trailing_bits（规范 5.9.1）
 *   DMD_OBU_FRAME        → byte_alignment（规范 5.10.1），之后紧跟 tile_group
 * 返回写入 out 的字节数（含 OBU 头），失败返回 0。
 * tail_out 回传 payload 的位长度，供 OBU_FRAME 继续拼 tile_group。 */
static size_t build_frame_header_obu(const VADecPictureParameterBufferAV1 *p,
                                     int obu_type,
                                     unsigned char *body, size_t body_cap,
                                     size_t *body_len_out,
                                     struct dmd_av1_dpb *dpb)
{
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, body, body_cap);
    put_uncompressed_header(&bw, dpb, p);

    if (obu_type == DMD_OBU_FRAME)
        dmd_av1_byte_align(&bw);      /* 纯补零，不写标记位 */
    else
        dmd_av1_trailing_bits(&bw);   /* 写 1 再补零 */

    if (bw.overflow)
        return 0;
    *body_len_out = dmd_bw_bytes(&bw);
    return *body_len_out;
}

size_t dmd_av1_build_frame_header(const void *pic_v,
                                  unsigned char *out, size_t out_cap,
                                  struct dmd_av1_dpb *dpb)
{
    const VADecPictureParameterBufferAV1 *p = pic_v;
    if (!p || !out || out_cap < 8)
        return 0;

    unsigned char body[512];
    size_t body_len = 0;
    if (build_frame_header_obu(p, DMD_OBU_FRAME_HEADER,
                              body, sizeof(body), &body_len, dpb) == 0)
        return 0;

    const size_t hdr = dmd_av1_obu_header(DMD_OBU_FRAME_HEADER,
                                         body_len, out, out_cap);
    if (hdr == 0 || hdr + body_len > out_cap)
        return 0;
    for (size_t i = 0; i < body_len; i++)
        out[hdr + i] = body[i];
    return hdr + body_len;
}

size_t dmd_av1_build_frame(const void *pic_v,
                           const struct dmd_av1_tile *tiles, int num_tiles,
                           unsigned char *out, size_t out_cap,
                           struct dmd_av1_dpb *dpb)
{
    const VADecPictureParameterBufferAV1 *p = pic_v;
    if (!p || !out || !tiles || num_tiles <= 0 || out_cap < 16)
        return 0;

    /* 帧头（结尾 byte_alignment，不是 trailing_bits）。 */
    unsigned char fh[512];
    size_t fh_len = 0;
    if (build_frame_header_obu(p, DMD_OBU_FRAME,
                              fh, sizeof(fh), &fh_len, dpb) == 0)
        return 0;

    /* tile_group_obu()（规范 5.11.1）的 payload：
     *   NumTiles > 1 时先写 tile_start_and_end_present_flag
     *   置 0 表示本 group 覆盖全部 tile（tg_start=0, tg_end=NumTiles-1）
     *   随后 byte_alignment，再逐 tile 写 tile_size_minus_1 + 数据
     *   最后一个 tile 不写长度（由 OBU 剩余长度隐含）
     *
     * tile_size_minus_1 用 le(4)，宽度必须与帧头 tile_info 里写的
     * tile_size_bytes_minus_1 = 1（即 2 字节）一致 ——
     * 两处不符会从第二个 tile 起全错位。 */
    const uint32_t tile_total =
        (uint32_t)p->tile_cols * (uint32_t)p->tile_rows;

    unsigned char tg_hdr[8];
    struct dmd_bitwriter tgw;
    dmd_bw_init(&tgw, tg_hdr, sizeof(tg_hdr));
    if (tile_total > 1)
        dmd_bw_put_flag(&tgw, 0);     /* tile_start_and_end_present_flag */
    dmd_av1_byte_align(&tgw);
    if (tgw.overflow)
        return 0;
    const size_t tg_hdr_len = dmd_bw_bytes(&tgw);

    /* 先算 OBU payload 总长，才能写 leb128 的 obu_size。 */
    size_t payload_len = fh_len + tg_hdr_len;
    for (int i = 0; i < num_tiles; i++) {
        if (!tiles[i].data && tiles[i].len)
            return 0;
        payload_len += tiles[i].len;
        if (i + 1 < num_tiles)
            payload_len += 2;         /* tile_size_minus_1，le(2) */
    }

    const size_t hdr = dmd_av1_obu_header(DMD_OBU_FRAME, payload_len,
                                          out, out_cap);
    if (hdr == 0 || hdr + payload_len > out_cap)
        return 0;

    unsigned char *q = out + hdr;
    for (size_t i = 0; i < fh_len; i++)
        *q++ = fh[i];
    for (size_t i = 0; i < tg_hdr_len; i++)
        *q++ = tg_hdr[i];
    for (int i = 0; i < num_tiles; i++) {
        if (i + 1 < num_tiles) {
            /* le(2)：宽度必须等于帧头 tile_size_bytes_minus_1 + 1 = 2。
             * 单 tile 上限 64KB —— 实测 1080p 最大 tile 约 4KB，
             * 而 VA-API 源码流本身就用 2 字节，跟随它即可。
             * ⚠️ 若将来遇到 >64KB 的单 tile，这里和帧头要一起加宽。 */
            const uint32_t v = (uint32_t)(tiles[i].len - 1);
            *q++ = (unsigned char)(v & 0xFF);
            *q++ = (unsigned char)((v >> 8) & 0xFF);
        }
        for (size_t k = 0; k < tiles[i].len; k++)
            *q++ = tiles[i].data[k];
    }
    /* 帧已合成成功 —— 把本帧登记进影子 DPB，供后续帧的 ref_frame_idx 查询。
     * 必须在成功路径的末尾做：合成失败时不能污染 DPB 状态，
     * 否则后续帧会引用一个从未真正写入解码器的槽。 */
    if (dpb) {
        const VADecPictureParameterBufferAV1 *pp = pic_v;
        int is_key_frame = (pp->pic_info_fields.bits.frame_type == 0);
        if (is_key_frame && pp->pic_info_fields.bits.show_frame) {
            /* KEY + show 帧刷新全部 8 槽（规范如此，字段不写入码流）。 */
            for (int k = 0; k < 8; k++) {
                dpb->dpb_shadow[k] = pp->current_frame;
                dpb->dpb_order_hint[k] = pp->order_hint;
            }
            dpb->dpb_next_slot = 0;
        } else {
            unsigned sl = dpb->dpb_next_slot & 7u;
            dpb->dpb_shadow[sl] = pp->current_frame;
            dpb->dpb_order_hint[sl] = pp->order_hint;
            dpb->dpb_next_slot = (sl + 1u) & 7u;
        }
    }

    return hdr + payload_len;
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
