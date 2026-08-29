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

/* tile_info()，AV1 规范 5.9.15。
 *
 * ⚠️ tile_size_bytes_minus_1 在这里写死 3（即 4 字节），第 4 步的
 * tile_group 必须用同样宽度写 tile_size_minus_1 —— 两处不一致会让
 * 解码器按错误宽度读 tile 长度，从第二个 tile 起全部错位。 */
static void put_tile_info(struct dmd_bitwriter *bw,
                          const VADecPictureParameterBufferAV1 *p,
                          uint32_t mi_cols, uint32_t mi_rows)
{
    const int sb_shift = p->seq_info_fields.fields.use_128x128_superblock ? 5 : 4;
    const int sb_size  = 1 << sb_shift;

    /* sbCols/sbRows：以超级块为单位的画面尺寸（规范 5.9.15 的推导）。 */
    const uint32_t sb_cols = (mi_cols + sb_size - 1) >> sb_shift;
    const uint32_t sb_rows = (mi_rows + sb_size - 1) >> sb_shift;

    dmd_bw_put_flag(bw, (int)p->pic_info_fields.bits.uniform_tile_spacing_flag);

    if (p->pic_info_fields.bits.uniform_tile_spacing_flag) {
        /* 均匀间隔：写 increment_tile_cols_log2 的一元码。
         * VA-API 给的是 tile_cols 而非 log2，先反推。
         * va_dec_av1.h:385-387 说明此模式下 width_in_sbs_minus_1[] 应忽略，
         * 由驱动依 tile_cols/tile_rows 自行生成 —— 正是这里做的事。 */
        uint32_t cols_log2 = 0;
        while ((1u << cols_log2) < p->tile_cols)
            cols_log2++;
        uint32_t rows_log2 = 0;
        while ((1u << rows_log2) < p->tile_rows)
            rows_log2++;

        /* 一元码的停止位条件必须用规范的 maxLog2 上界，不能写死常数 ——
         * 写死 6 会让解码器在 cols_log2 已达上界时仍去读一位停止位，
         * 从而把后面的字段读偏（实测表现为 tile 布局解析成 3x2 而非 2x4）。
         *
         * 规范 5.9.15 的推导（tile_log2(blkSize, target) = 最小的 k
         * 使 blkSize << k >= target）：
         *   maxLog2TileCols = tile_log2(1, min(sbCols, MAX_TILE_COLS))
         *   maxLog2TileRows = tile_log2(1, min(sbRows, MAX_TILE_ROWS))
         *   minLog2TileCols = tile_log2(MAX_TILE_WIDTH_SB, sbCols)
         *   minLog2TileRows = max(minLog2Tiles - cols_log2, 0)
         * 其中 minLog2Tiles = tile_log2(MAX_TILE_AREA_SB, sbRows*sbCols)。 */
        const uint32_t MAX_TILE_COLS = 64, MAX_TILE_ROWS = 64;
        const uint32_t max_tile_width_sb = 4096 >> sb_shift;
        const uint32_t max_tile_area_sb  = (4096 * 2304) >> (2 * sb_shift);

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
        uint32_t min_log2_tiles = 0;
        while ((max_tile_area_sb << min_log2_tiles) < sb_rows * sb_cols)
            min_log2_tiles++;

        for (uint32_t i = min_log2_tile_cols; i < cols_log2; i++)
            dmd_bw_put_flag(bw, 1);       /* increment_tile_cols_log2 */
        if (cols_log2 < max_log2_tile_cols)
            dmd_bw_put_flag(bw, 0);       /* 停止位 */

        const uint32_t min_log2_tile_rows =
            (min_log2_tiles > cols_log2) ? (min_log2_tiles - cols_log2) : 0;
        for (uint32_t i = min_log2_tile_rows; i < rows_log2; i++)
            dmd_bw_put_flag(bw, 1);       /* increment_tile_rows_log2 */
        if (rows_log2 < max_log2_tile_rows)
            dmd_bw_put_flag(bw, 0);
    } else {
        /* 非均匀：逐 tile 写 width_in_sbs_minus_1 / height_in_sbs_minus_1，
         * 用 ns(n) 编码（规范 5.9.15）。 */
        uint32_t start_sb = 0;
        for (int i = 0; i < p->tile_cols && start_sb < sb_cols; i++) {
            uint32_t max_w = sb_cols - start_sb;
            dmd_av1_put_ns(bw, p->width_in_sbs_minus_1[i], max_w);
            start_sb += p->width_in_sbs_minus_1[i] + 1;
        }
        start_sb = 0;
        for (int i = 0; i < p->tile_rows && start_sb < sb_rows; i++) {
            uint32_t max_h = sb_rows - start_sb;
            dmd_av1_put_ns(bw, p->height_in_sbs_minus_1[i], max_h);
            start_sb += p->height_in_sbs_minus_1[i] + 1;
        }
    }

    /* TileCols/TileRows > 1 时才写 context_update_tile_id 与 tile_size_bytes。 */
    const uint32_t tile_total = (uint32_t)p->tile_cols * (uint32_t)p->tile_rows;
    if (tile_total > 1) {
        uint32_t cols_log2 = 0, rows_log2 = 0;
        while ((1u << cols_log2) < p->tile_cols) cols_log2++;
        while ((1u << rows_log2) < p->tile_rows) rows_log2++;
        dmd_bw_put_bits(bw, p->context_update_tile_id,
                        (int)(cols_log2 + rows_log2));
        dmd_bw_put_bits(bw, 3, 2);   /* tile_size_bytes_minus_1 = 3 → 4 字节 */
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

    /* 序列头里 enable_restoration 是按"三者任一非零"写的，两处必须一致：
     * 若全为 0，序列头写了 enable_restoration=0，解码器就不会来读这段。 */
    if (!ry && !rcb && !rcr)
        return;

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

size_t dmd_av1_build_frame_header(const void *pic_v,
                                  unsigned char *out, size_t out_cap)
{
    const VADecPictureParameterBufferAV1 *p = pic_v;
    if (!p || !out || out_cap < 8)
        return 0;

    unsigned char body[512];
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, body, sizeof(body));

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
    if (!(is_key && p->pic_info_fields.bits.show_frame))
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
    const int primary_ref_none = intra_only ||
        p->pic_info_fields.bits.error_resilient_mode ||
        (p->primary_ref_frame == 7 /* PRIMARY_REF_NONE */);
    if (!intra_only && !p->pic_info_fields.bits.error_resilient_mode)
        dmd_bw_put_bits(&bw, p->primary_ref_frame, 3);

    /* refresh_frame_flags：KEY_FRAME + show_frame 时恒 0xFF、不写入码流。
     *
     * ⚠️ VA-API **不提供**这个字段（全文件 grep 确认：只在 :421 的注释里
     * 被提及，结构体里没有）。它是"本帧要刷新哪些参考槽位"的位掩码。
     *
     * 取 0xFF（刷新全部 8 槽）：MediaCodec 内部自行管理参考帧生命周期，
     * 不依赖码流里的这个掩码来分配缓冲；对它而言只需语法合法。
     * 而 0xFF 是最保守的选择 —— 声称刷新全部，不会出现"解码器以为某槽
     * 还有效、实际已被覆盖"的悬空引用。代价是参考帧管理不够精细，
     * 但本驱动逐帧转发、由 MediaCodec 负责重排序，不受影响。 */
    const int refresh_all = (is_key && p->pic_info_fields.bits.show_frame);
    if (!refresh_all)
        dmd_bw_put_bits(&bw, 0xFF, 8);

    /* 参考帧索引：帧间帧才有。 */
    if (!intra_only) {
        /* frame_refs_short_signaling 需要 enable_order_hint；置 0 表示
         * 显式给出全部 7 个 ref_frame_idx（VA-API 提供的正是这个数组）。 */
        if (enable_order_hint)
            dmd_bw_put_flag(&bw, 0);
        for (int i = 0; i < 7; i++)
            dmd_bw_put_bits(&bw, p->ref_frame_idx[i], 3);
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
     * UpscaledWidth == FrameWidth（即无 superres 上采样），**与
     * allow_screen_content_tools 的取值无关地出现在 intra 分支里**。
     *
     * 与真实码流逐位对照确认：libaom 生成的 1080p KEY_FRAME 在第 8 位
     * 就是 allow_intrabc（此时 allow_screen_content_tools=0 却仍写了该位）。
     * 早先误加 allow_screen_content_tools 作为前置条件，导致少写 1 位、
     * dav1d 报 trailing_one_bit 错位。 */
    if (intra_only && !p->pic_info_fields.bits.use_superres)
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
        if (!p->pic_info_fields.bits.error_resilient_mode && enable_order_hint)
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
    if (!coded_lossless)
        dmd_bw_put_flag(&bw, p->mode_control_fields.bits.tx_mode == 3);

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
        p->pic_info_fields.bits.is_motion_mode_switchable &&
        !p->pic_info_fields.bits.error_resilient_mode)
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

    dmd_av1_trailing_bits(&bw);

    if (bw.overflow)
        return 0;

    const size_t body_len = dmd_bw_bytes(&bw);
    const size_t hdr = dmd_av1_obu_header(DMD_OBU_FRAME_HEADER,
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
