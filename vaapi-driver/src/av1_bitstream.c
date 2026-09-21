/* AV1 OBU 反向合成——变长编码、对齐与 OBU 头。
 *
 * 设计说明见 av1_bitstream.h 顶部。本文件只实现最底层的比特写入原语，
 * 序列头/帧头/tile group 的语法在后续提交里加。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    /* separate_uv_delta_q 写 **1**（不是源码流的值，但我们只能声明一次：
     * 序列头随 KEY 帧发一次，而"本帧 U/V 的 delta 是否不同"是逐帧的。
     * 声明 1 之后每帧用 diff_uv_delta 这一位如实表达：源里 V 继承 U 时
     * 就写 0（解码器据此把 V 的 delta 取成 U 的，与源推断结果一致），
     * V 与 U 不同（rav1e 常见）时写 1 并带上 V 的两个 delta。
     * 声明 0 会把 V 的 delta 丢掉 —— 实测 rav1e 样本 luma 逐字节全对、
     * 色度 50% 字节错，就是这个字段被吃掉了。 */
    dmd_bw_put_flag(bw, 1);                        /* separate_uv_delta_q */
}

/* tile_info()，AV1 规范 5.9.15。
 *
 * ⚠️ tile_size_bytes_minus_1 按 max(tile_len) 动态计算（曾写死，见下），
 *    第 4 步的
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
 * ⚠️ tile_size_bytes_minus_1 动态计算，第 4 步 tile_group 里写
 * tile_size_minus_1 必须用同样宽度 —— 两处不一致会从第二个 tile 起全错位。 */
static void put_tile_info(struct dmd_bitwriter *bw, int tile_size_bytes,
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
        /* tile_size_bytes_minus_1：按各 tile 的**实际长度**决定，不能写死。
         *
         * ⚠️ 曾写死 1（2 字节），理由是"VA-API 给的 tile offset 之间恰好
         * 各差 2 字节"。该推断是错的 —— 实测源码流 trace_headers 此处为 0
         * （即 1 字节）。写死 2 字节会让每个 tile 间隙多 1 字节，
         * 7 个间隙共多 6~7 字节，位流从此整体偏移，
         * 解码器随后读到的字段全部错位（表现为 zero_bit out of range）。
         *
         * 正确做法：取 max(tile_len) 所需的最小字节数。tile_group 里写
         * tile_size_minus_1 时必须用同一宽度，故由调用方算好后传入。 */
        dmd_bw_put_bits(bw, (unsigned)(tile_size_bytes - 1), 2);
    }
}

/* quantization_params()，AV1 规范 5.9.12。 */
static void put_quantization_params(struct dmd_bitwriter *bw,
                                    const VADecPictureParameterBufferAV1 *p)
{
    const uint32_t mono = p->seq_info_fields.fields.mono_chrome;

    dmd_bw_put_bits(bw, p->base_qindex, 8);

    /* delta_q：1 位 delta_coded 标志 + 7 位**二进制补码**有符号值。
     * 曾按规范 4.10.6 的 su(n)="n-1 位幅值 + 1 位符号" 怀疑过这里的写法，
     * 用 rav1e 样本 bat/rav_720 的原始位流实测定论：真实码流就是补码
     * （y=10/u_dc=12/u_ac=2/v_dc=-9/v_ac=-15 五组值按补码+diff_uv_delta
     * 排列能在 payload 第 25 位起逐位命中，按幅值+符号则一处都不命中）。
     * CBS 的 read_signed 也是纯补码读法，两边一致。 */
    #define PUT_DELTA_Q(v) do {                        \
        if ((v) != 0) { dmd_bw_put_flag(bw, 1);        \
                        dmd_av1_put_su(bw, (v), 7); }  \
        else            dmd_bw_put_flag(bw, 0);        \
    } while (0)

    PUT_DELTA_Q(p->y_dc_delta_q);

    if (!mono) {
        /* diff_uv_delta 的位置：紧跟 delta_q_y_dc **之后**、U 的两个 delta
         * 之前（CBS quantization_params 原文顺序是 y_dc → diff_uv_delta →
         * u_dc → u_ac → (v_dc, v_ac)）。曾把它写在 U 之后，序列头
         * separate_uv_delta_q=0 时该位不存在所以没暴露；改成恒 1 后立刻
         * 整体错位 1 位起，后续字段全乱。
         * VA-API 的 v_dc/v_ac 是 CBS 的**生效值**（源里 diff_uv_delta=0 时
         * 它已被推断成 U 的值），于是"V 与 U 是否不同"恰好还原源码流真值。 */
        const int diff_uv = p->v_dc_delta_q != p->u_dc_delta_q ||
                            p->v_ac_delta_q != p->u_ac_delta_q;
        dmd_bw_put_flag(bw, diff_uv);
        PUT_DELTA_Q(p->u_dc_delta_q);
        PUT_DELTA_Q(p->u_ac_delta_q);
        if (diff_uv) {
            PUT_DELTA_Q(p->v_dc_delta_q);
            PUT_DELTA_Q(p->v_ac_delta_q);
        }
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

/*
 * ============ AV1 合成正确性的验证基线（实测，勿凭记忆改动） ============
 *
 * 源码流 av1_1080p.obu 的 temporal unit 结构并非一帧一 TU：
 *   150 个 TU，各含帧数 [1,5,0,1,0,2,0,1...] —— TU2 就含 5 帧。
 * 因此任何"取前 N 帧"的截断都会破坏 TU 完整性，实测源码流原始字节
 * 按帧截断到 5 帧后 dav1d 也只解出 1 帧。对照必须按 **TU 边界** 截断。
 *
 * 硬件 V4L2 基线（每帧一单元送料）：
 *   前 2 TU（ 6 帧）→ 送 6 收 2
 *   前 6 TU（ 9 帧）→ 送 9 收 6
 *   前16 TU（17 帧）→ 送17 收16
 * 解码器有约 1 帧固有延迟，序列越长越接近 N-1。
 *
 * dav1d 侧同一基线：2/6/16 TU 分别出 2/6/16 帧。
 *
 * ⚠️ 曾长期误用"送 6 收 6"作基线 —— 那是 6 个 TU 共 9 帧的结果，
 *    与"6 帧"不是同一量级，据此判断合成质量会得出错误结论。
 *
 * ---- 当前唯一已知缺陷：帧间帧帧头少 1 字节（已大幅收窄）----
 *
 * 最新实测（6 帧样本，全部按"独立流 + trace_headers 字段序列 diff"核对）：
 *   帧2  源2683B 合成2683B  完全一致
 *   帧3  源 644B 合成 643B  少 1
 *   帧4  源 240B 合成 240B  完全一致
 *   帧5  源 178B 合成 178B  完全一致
 *   帧6  源  83B 合成  82B  少 1
 * 即 6 帧里 4 帧已逐字节正确，只有帧3、帧6 各少 1 字节。
 *
 * 帧3 的字段序列比对结果（f3s.obu vs f3y.obu，各含 KEY 帧 + 该帧）：
 *   267 个字段**全部相同**，唯一差异是 obu_size 644 vs 643。
 * 字节层面：偏移 19 处源为 00 00 00 0a、合成为 00 00 0a —— 源多一个 0x00，
 * 其后内容完全相同（整体错开一字节）。
 *
 * 布局核算（帧3）：8 个 tile 数据 619B + 7 个 tile_size(1B) = 626B
 *   源   obu_size 644 → 帧头 + tg头 = 18B
 *   合成 obu_size 643 → 帧头 + tg头 = 17B
 *
 * 已排除的候选：
 *   · tile 数据与 tile_size 序列 —— 两侧完全相同（[1,11,11,11,10,236,91] + 248B）
 *   · tile_size_bytes 宽度 —— 两侧都是 1 字节，动态计算已正确
 *   · tile_group 头 —— tile_total=8>1（实测 tile_cols=4 tile_rows=2），
 *     tile_start_and_end_present_flag 确实写了，byte_align 后正好 1 字节
 *   · byte_align 在已对齐时不补 —— 代码正确（规范 5.3.5 纯补零）
 *
 * 所以差的 1 字节在 uncompressed_header 内部，且**不改变字段序列**。
 *
 * 本轮又排除了三项：
 *   · 字段位宽 —— 267 项逐项比对 length($3)，**零差异**
 *   · leb128 编码 —— 两侧 obu_size 都用 2 字节，644 vs 643 是结果非原因
 *   · film_grain / apply_grain —— 序列头 film_grain_params_present=0，
 *     两侧都不写该位，trace 各报 2 处一致
 *
 * 精确定位（穷举插入法，比任何推断都可靠）：
 *   在合成帧的偏移 17/18/19 任一处插入一个 0x00，结果与源**逐字节完全相同**。
 *   源   [10:26] = 00000a179f00030000000a89774458bb
 *   合成 [10:26] = 00000a179f000300000a89774458bb44
 *   即源此处是 00 00 00（三个零），合成只有 00 00（两个零）。
 * 反推帧头长度：源 17B（136 位），合成 16B（128 位），差整 8 位。
 *
 * 注意 AV1 **不使用** EBSP 转义（那是 H.264/HEVC 的机制），
 * 所以这三个零不是防竞争字节，而是某个字段的真实内容。
 * 按该偏移位置，嫌疑落在 delta_q / segmentation 一带。
 *
 * 字节级对位（已确证，比任何位偏移推断可靠）：
 *   源   [10:26] = 00 00 0a 17 9f 00 03 00 | 00 00 0a 89 77 44 58 bb
 *   合成 [10:26] = 00 00 0a 17 9f 00 03    | 00 00 0a 89 77 44 58 bb
 * 已知 tile 层完全相同（第一个 tile_size=0x00 表示 size=1，其后 1 字节
 * tile 数据 0x00，再往后 0x0a 起是第二个 tile）。据此对齐可得：
 *   源   帧头 = [0..17]，末字节 [17]=0x00
 *   合成 帧头 = [0..16]，末字节 [16]=0x03
 * 且 **[0..16] 十七个字节两侧逐字节相同**。
 *
 * ★ 结论：合成写的每一位都是对的，只是在帧头**最末**少写了 1~8 位零，
 *   导致 byte_align 少补一整字节。要找的字段满足三个条件：
 *     (a) 值为 0   (b) 长度 1~8 位   (c) 位于 uncompressed_header 末尾
 *
 * 本轮已逐行核对并确认正确的分支（不必再查）：
 *   · put_tile_info 全段，含 context_update_tile_id 与 tile_size_bytes_minus_1
 *   · put_quantization_params —— separate_uv_delta_q=0（实测），
 *     故不写 diff_uv_delta、U/V 共用一组 delta，代码处理正确
 *   · delta_q_params / delta_lf_params（1104-1120）—— 与规范 5.9.17/5.9.18 一致，
 *     实测 delta_q_present=1、delta_q_res=0
 *   · tx_mode（increment 编码）、reference_select、reduced_tx_set_used
 *
 * ---- 补位实验结论（DMD_AV1_PAD_BITS，决定性）----
 *
 * 在 byte_align 之前额外补 N 位零，逐帧与源比对（6 帧样本）：
 *       pad=0  pad=1  pad=2  pad=3
 *   帧2   ·      ·      ·      ·
 *   帧3   ·      ✓      ✓      ✓
 *   帧4   ✓      ✓      ✓      ✓
 *   帧5   ✓      ✓      ✓      ✓
 *   帧6   ·      ·      ·      ·
 * （✓ = 与源**逐字节完全相同**）
 *
 * 三点确证：
 * 1. 帧3 只要补 ≥1 位零就逐字节正确 —— 上一轮"帧头末尾少写 1~8 位零"
 *    的假设成立。位数不敏感是因为 byte_align 会吸收多余零位。
 * 2. 帧4、帧5 在任何 pad 值下都正确，说明它们本来就落在字节边界上，
 *    补零不影响 —— 所以这个补位不是"某帧特有"，而是**所有帧都该补**。
 * 3. 帧2、帧6 补零救不了：它们差的是**非零位**，属另一类缺陷。
 *
 * ---- 帧2 / 帧6 的残余缺陷（各只差 1 个字节，其余全同）----
 *   帧2 (2683B) 共同前缀 18B，仅 [18] 不同：
 *       源 0x70 = 01110000   合成 0x68 = 01101000
 *       XOR = 0x18 —— 相邻两位模式不同（源 11 0 / 合成 10 1）
 *   帧6 (83B) 共同前缀 2B，仅 [2] 不同：
 *       源 0x00 = 00000000   合成 0x04 = 00000100
 *       XOR = 0x04 —— 合成**多写**了 1 位
 * 方向相反（帧2 少、帧6 多），故不是同一个常量偏差。
 * 帧6 偏移 2 落在 refresh_frame_flags 一带（本会话修过该字段）；
 * 帧2 是第一个 inter 帧（order_hint=16），偏移 18 接近帧头末尾。
 *
 * 注意：帧2 是链条上第一个 inter 帧，它错则后续全废 ——
 * 这解释了为什么补位让 3 帧变正确、硬件解码帧数却仍是 1。
 * 修复顺序应当是 帧2 → 帧6，而非先追帧数。
 *
 * ---- 帧2 缺陷已定位：skip_mode_present（新增 DMD_AV1_BITS 开关测得）----
 *
 * DMD_AV1_BITS=1 打印帧头各段起始位位置，帧2（第一个 inter 帧）实测：
 *   tile_info@55 quant@66 seg@78 lf@83 cdef@111 lr@139
 *   txmode@145 refsel@146 skipmode@147 warp@148 redtx@149 header_end@157
 * 而字节比对给出的差异位正是 **147** —— 即 skip_mode_present。
 *
 * 各帧的 VA-API 取值 vs 源码流实际写入（按上述位位置逐位反解源字节）：
 *   帧2  VA-API skip_mode_present=0   源=1   ← 不一致
 *   帧3  VA-API 1                     源=1
 *   帧4  VA-API 1                     源=1
 *   帧5  VA-API 1                     源=1
 *   帧6  VA-API 1                     源=1
 * 源码流全部 5 个 inter 帧都是 1，只有帧2 的 VA-API 值与源不符。
 *
 * 也就是说：VA-API 的 mode_control_fields.bits.skip_mode_present 在本帧
 * **不可直接转写**。规范 5.9.22 的 skipModeAllowed 要比较参考帧 order hint
 * （需找出前向/后向最近的两个参考），VA-API 给的结论在此帧与 libaom 不符。
 * ---- 实测结论：skip_mode_present 与 allow_warped_motion **两个都要改** ----
 *
 * 加 DMD_AV1_SKIP / DMD_AV1_WARP 覆盖开关穷举组合，与源逐帧比对：
 *   skip=1 warp=0 → 帧2 ✓ 帧3 ✓ 帧4 ✓ 帧5 ✓ 帧6 ✗
 *   skip=1 warp=- → 帧2 ✗ 帧3 ✓ 帧4 ✓ 帧5 ✓ 帧6 ✗
 *   skip=- warp=0 → 帧2 ✗ 帧3 ✓ 帧4 ✓ 帧5 ✓ 帧6 ✗
 * （✓ = 与源逐字节完全相同；- = 用 VA-API 原值）
 * 单改任一个都不够，两个必须同时改，帧2 才逐字节正确。
 *
 * 逐位实测（帧2 偏移18，144..152 位）：
 *   位147 源1 合成0  skip_mode_present
 *   位148 源0 合成1  allow_warped_motion
 * 两侧总位数相同（都到 header_end@157），所以**不是移位**，是两个值都反。
 *
 * VA-API 侧的参考帧状况（DMD_AV1_BITS 实测）解释了 skip_mode_present：
 *   帧2 oh=16 idx=[2,2,2,2,2,2,2] ref_oh=[0,0,0,0,0,0,0]
 * 七个参考全指向同一 slot，只有一个可用参考 —— 按规范 5.9.22
 * skipModeAllowed 需要前向+后向两个不同参考，故 VA-API 报 0 是"讲道理"的，
 * 但 libaom 编码时写的是 1。所以此处不能照抄 VA-API 结论。
 *
 * ⚠️ 已确认**不是**位域读取错位：va_dec_av1.h:545-576 的
 * mode_control_fields 位域声明里 skip_mode_present 是独立字段，
 * 读取无误（曾怀疑此项，已排除）。
 *
 * 待解决：这两个字段的正确取值规则是什么。当前只知道本码流
 * 全部 5 个 inter 帧的源值都是 skip=1，而 warp 需按帧判断
 * （帧2 源为 0）。不能直接写死常量 —— 那只是拟合这一条码流。
 *
 * 下一步要么按规范自行推导 skipModeAllowed，要么找出 VA-API 该值的正确解释。
 *
 * 附带纠正：1154 行用 reference_select 当门是对的（规范 5.9.22 里
 * skipModeAllowed 的前置就含 reference_select），上一轮把它列为嫌疑属误判。
 *
 * ---- 帧6 缺陷已定位：末帧的 refresh_frame_flags 从未被改写 ----
 *
 * 位 21 落在 refresh_frame_flags（实测 @19，占 19..26 位）内。
 * 落盘流与源码流逐帧对比：
 *   帧2 源0x01 合成0x01 ✓   帧3 源0x08 合成0x08 ✓
 *   帧4 源0x20 合成0x20 ✓   帧5 源0x40 合成0x40 ✓
 *   帧6 源0x00 合成0x20 ✗
 * 前四帧完全正确 —— "延迟一帧就地改写"机制本身工作正常
 * （DMD_AV1_LOG 实测依次给出 refresh=0x01/0x08/0x20/0x40）。
 *
 * 帧6 是样本里的**最后一帧**，没有"下一帧"来触发改写，
 * 于是停在 `1 << slot` 的占位值 0x20，而源码流该帧真值是 0x00。
 * 这是本会话修过的"暂存末帧从不送出"的姊妹缺陷：
 * 那次漏的是数据发送，这次漏的是数值改写。
 *
 * 修法方向：sync_surface_locked 里 flush 暂存帧时，除了送出数据，
 * 还要给它一个 refresh 值。末帧无后继可比，但可用
 * "本帧之后没有任何帧会引用它" 推出 0 —— 需确认这是通用规则
 * 还是仅本码流如此（不能拿一条码流的末帧值当常量）。
 *
 * ⚠️ 注意 DMD_AV1_BITS 打印的 refresh_mask 是**改写前**的占位值
 * （0x01,0x02,0x04,0x08,0x10 的简单轮转），不是最终写入码流的值。
 * 判断 refresh 正确性必须看落盘流（DMD_AV1_DUMP），不要看这个探针。
 *
 * ---- 帧6 原始记录：位 21 ----
 * 帧6 差异在偏移 2（位 16..23），合成在位 21 多写 1 位。
 * 该位置在 tile_info(@55) 之前，属 uncompressed_header 开头段 ——
 * frame_type / show_frame / refresh_frame_flags / order_hint 一带，
 * 与 skip_mode_present 无关，需单独定位。
 *
 * 剩余嫌疑（按位置从后往前）：
 *   · skip_mode_present 的**门条件**：代码用 `reference_select` 当门（1154 行），
 *     而规范 5.9.22 的 skipModeAllowed 推导并不含该项。实测
 *     reference_select=1 故本帧仍写了，但门条件本身仍需按规范复核。
 *   · allow_warped_motion 的 is_motion_mode_switchable 门
 *   · reduced_tx_set_used 之后是否还有规范要求、代码完全未写的字段
 *
 * ⚠️ 方法教训（本轮再次踩到）：trace_headers 输出的位置是**流内累计位置**，
 * 不是帧内偏移；且输出里**没有** obu_forbidden_bit 行，
 * 所以"按 obu_forbidden_bit 分段取第 N 个 OBU"这种做法无效 ——
 * 本轮据此做的几组"两侧相同"比较全部作废。
 * 可靠判据只有两条：字节级穷举插入、以及单帧独立流的字段序列 diff。
 *
 * ⚠️ 本轮一度又犯"跨帧比对"的老错：trace_headers 对含多帧的流会把各帧
 * 字段混在一起输出，我据此得出"位 168 是 tile_start_and_end_present_flag"，
 * 实际那行来自另一帧（同位置在本帧是 cdef_y_pri_strength[5]）。
 * 教训重申：必须把待测帧单独包成独立流再 trace。
 *

 * 以帧 3（order_hint=4）为例，用"按声明宽度解 tile 长度、看余量是否合理"
 * 的方式反推帧头长度：
 *   源码流   帧头 18B → tiles=[1,11,11,11,10,236,91] 余 248B
 *   合成流   帧头 17B → tiles=[1,11,11,11,10,236,91] 余 248B
 * tile 数据逐字节相同、tile 长度序列相同（且 max=236 与 VA-API 给的
 * max_tile 吻合），差别只在帧头本身少 1 字节。
 * 具体是哪个字段少写/少了几位尚未定位 —— 下一步应逐字段核对帧 3 的
 * uncompressed_header，而非再依赖位偏移换算。
 *
 * ⚠️ tile_size_bytes_minus1 是**逐帧变化**的，不是流级常量：
 *   源码流首帧 = 1（2 字节，max_tile=4123）
 *   源码流帧 3 = 0（1 字节，max_tile=236）
 * 曾把首帧的值当成全流统一值，据此写死 2 字节，导致每个 tile 间隙多
 * 1 字节。现按 max(tile_len)-1 动态计算，实测与源码流逐帧一致
 * （4123→2, 747→2, 236→1, 79→1, 38→1, 19→1）。
 *
 * ---- 金字塔 B 帧结构：多数帧 show_frame=0，不产生输出 ----
 * VA-API 提供 show_frame（va_dec_av1.h:425），但不提供 show_existing_frame。
 * 实测 ffmpeg 送来的前 6 帧：
 *   ft=0 show=1 oh=0    ← KEY，显示
 *   ft=1 show=0 oh=16   ← 参考帧，不显示
 *   ft=1 show=0 oh=8
 *   ft=1 show=0 oh=4
 *   ft=1 show=0 oh=2
 *   ft=1 show=1 oh=1    ← 显示
 * 只有首帧与末帧 show_frame=1。源码流 trace_headers 的 show_frame 序列
 * (1 | 0,0,0,0,1) 与 VA-API 给的完全一致，show_existing_frame 恒 0，
 * 且 TU2 内 5 个 OBU_FRAME 都带完整帧头 —— 故合成结构本身正确。
 *
 * ⚠️ 由此推翻"合成流比源码流少出帧"的结论。决定性对照：
 *   源码流 帧1+帧2 → dav1d 1 帧、硬件 送 2 收 1
 *   合成流 帧1+帧2 → dav1d 1 帧、硬件 送 2 收 1
 * 完全相同。帧 2 的 show_frame=0，本就不产生输出，
 * 拿"送 N 收 N-1"去要求它出帧是错的。
 *
 * ⚠️ trace_headers 不是多帧判据：它对源码流同样只解析首帧
 *    （-c copy 下 bsf 遇到后续 OBU 会停），实测源与合成的字段序列
 *    尾部完全相同。多帧正确性只能靠 dav1d 解出的帧数判断。
 *
 * ---- 两种 TU 分组各自触发不同报错（实测，用于区分症状来源）----
 * 把合成的 5 帧按不同方式分组喂给 dav1d：
 *   每帧独立一个 TU（驱动原本的输出方式）
 *     → "zero_bit out of range: 1, but must be in [0,0]"
 *   按源码流结构分组（TU1=1帧, TU2=其余4帧）
 *     → "Invalid repeated frame header OBU"
 * 两者都停在第 2 帧。后一条说明：一个 TU 内出现多个完整帧头是非法的，
 * 源码流能这么做是因为其中多数帧带 show_existing_frame 或不显示。
 * 因此 zero_bit 那条并非独立缺陷，而是分组方式的副产物 ——
 * 追它会跑偏（本轮一度按它去查 CDEF 区）。
 *
 * 另：合成流与源码流的首个差异恒在第 2 帧载荷偏移 18（位 144，CDEF 区），
 * 源 0x70 / 合成 0x68。单独把该字节改回 0x70 无效（dav1d 仍 1 帧），
 * 证明它是上游某字段取值不同导致位流整体偏移的**结果**而非原因。
 */

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
    if (getenv("DMD_AV1_BITS"))
        fprintf(stderr, "[lr] oh=%u ry=%u rcb=%u rcr=%u shift=%u uv=%u\n",
                p->order_hint, ry, rcb, rcr,
                p->loop_restoration_fields.bits.lr_unit_shift,
                p->loop_restoration_fields.bits.lr_uv_shift);
    /* ⚠️ lr_type 的码流编码与 VA-API 枚举**不是**同一套取值，必须映射。
     *
     * 规范 5.9.20 用 Remap_Lr_Type[4] = {NONE, SWITCHABLE, WIENER, SGRPROJ}
     * 把码流里的 f(2) 映射成 FrameRestorationType；
     * VA-API 直接给 FrameRestorationType 枚举
     * （0=NONE, 1=WIENER, 2=SGRPROJ, 3=SWITCHABLE）。
     * 写码流要做**反向**映射：
     *   NONE(0)→0   WIENER(1)→2   SGRPROJ(2)→3   SWITCHABLE(3)→1
     *
     * 实测证据（帧26，oh=28）：VA-API 给 ry=1(WIENER)，
     * 源码流该位写 2 —— 直接转写得到 1，正是差异位 115/116。
     * 早先"put_lr_params 已验证正确"的结论来自 6 帧样本，
     * 那些帧恰好 ry=rcb=rcr=0，映射前后都是 0，掩盖了这个缺陷。 */
    static const unsigned char lr_to_bits[4] = { 0, 2, 3, 1 };
    dmd_bw_put_bits(bw, lr_to_bits[ry & 3], 2);
    if (!p->seq_info_fields.fields.mono_chrome) {
        dmd_bw_put_bits(bw, lr_to_bits[rcb & 3], 2);
        dmd_bw_put_bits(bw, lr_to_bits[rcr & 3], 2);
    }

    if (ry || rcb || rcr) {
        /* ⚠️ VA-API 的 lr_unit_shift 是**总位移量**（0..2），
         * 而码流里分成两个各 1 位的字段（规范 5.9.20）：
         *   use_128x128_superblock=1: lr_unit_shift 一位即表示 0 或 1
         *   否则: lr_unit_shift(1) + 若为 1 再写 lr_unit_extra_shift(1)
         *         总位移 = lr_unit_shift + lr_unit_extra_shift
         * 所以 shift=2 要写成 1 再写 1。
         *
         * 早先代码写 `lr_unit_shift` 的低 1 位、extra 恒填 0：
         * shift=2 被写成 0，且不写 extra —— 实测帧26（shift=2）
         * 差异位 121/122 正是此处。
         * 6 帧样本里 ry=rcb=rcr=0，整个 if 块被跳过，掩盖了缺陷。 */
        const unsigned sh = p->loop_restoration_fields.bits.lr_unit_shift;
        if (p->seq_info_fields.fields.use_128x128_superblock) {
            /* ⚠️ sbs128=1 时码流位是**有效位移减一**（规范 5.9.20：
             * "When use_128x128_superblock is equal to 1, lr_unit_shift is
             * set equal to 1 + lr_unit_shift"），有效值只可能是 1 或 2。
             * VA-API 给的是有效值，所以这里必须写 sh-1，而不是 sh 的非零判定。
             *
             * 实测（testsrc 合成流 av1_probe.mp4，帧 oh=16，lr_type[2]=SGRPROJ，
             * 源码流该位 0 ⇒ 有效位移 1）：旧写法把 0 写成 1 ⇒ 有效位移 2 ⇒
             * restoration 单元从 64 变成 128 ⇒ **整条流每一帧都错**
             * （framemd5 128/128 全不匹配，浏览器里表现为满屏坏帧）。
             * 之所以此前 B 站 1800 帧真流逐字节一致，是它的 restoration 帧
             * 恰好全是有效位移 2（写 1 蒙对）—— 这个缺陷被样本掩盖了。 */
            dmd_bw_put_bits(bw, sh > 1 ? 1 : 0, 1);
        } else {
            dmd_bw_put_bits(bw, sh ? 1 : 0, 1);
            if (sh)
                dmd_bw_put_bits(bw, sh > 1 ? 1 : 0, 1);
        }
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

    /* enable_superres：VA-API 不提供序列级字段，用帧级 use_superres 反推。
     * enable_restoration：VA-API 同样不提供，恒写 1 并在每帧 lr_params
     * 里写 lr_type=0 —— 自洽且语义等价于"不用 restoration"。
     * ⚠️ 代价：帧头比 enable_restoration=0 的源码流多 6 位，但这 6 位
     * 写在我们自己的码流里，tile_group 位置自洽，不影响解码正确性。
     * ⚠️ 真正使用 loop restoration（lr_type≠0）的码流不受支持 ——
     * VA-API 完全不提供 LR 参数，像素会差一个滤波环节。 */
    dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.use_superres);
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_cdef);
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

/* ---- surface -> 帧号 的"最近拥有者"表 ----
 *
 * ffmpeg 的 surface 池会回收复用 surface id：一帧显示完且不再被引用后，
 * 它的 surface 会被新的解码帧拿走（实测 160 帧样本里 oh1 与 oh7 先后
 * 同用 surface 6）。因此 surface 不能直接当帧身份用；但"最近拥有某
 * surface 的帧"总是唯一且还活着的。所有从 surface 出发的身份判断
 * 都经过这里。 */
static int dmd_av1_frame_of(struct dmd_av1_dpb *dpb, VASurfaceID surf)
{
    if (!dpb || surf == VA_INVALID_ID || surf == 0)
        return -1;
    for (int i = dpb->surf_hist_n - 1; i >= 0; i--)
        if (dpb->surf_hist[i].surf == surf)
            return dpb->surf_hist[i].frame;
    return -1;
}

static void dmd_av1_remember_surface(struct dmd_av1_dpb *dpb,
                                     VASurfaceID surf, int frame)
{
    if (!dpb || surf == VA_INVALID_ID || surf == 0)
        return;
    for (int i = dpb->surf_hist_n - 1; i >= 0; i--) {
        if (dpb->surf_hist[i].surf == surf) {
            dpb->surf_hist[i].frame = frame;
            return;
        }
    }
    if (dpb->surf_hist_n < (int)(sizeof(dpb->surf_hist) /
                                 sizeof(dpb->surf_hist[0]))) {
        dpb->surf_hist[dpb->surf_hist_n].surf = surf;
        dpb->surf_hist[dpb->surf_hist_n].frame = frame;
        dpb->surf_hist_n++;
    } else {
        /* 表满：覆盖最早一项（最老的拥有关系最不可能再被查询）。 */
        for (int i = 1; i < dpb->surf_hist_n; i++)
            dpb->surf_hist[i - 1] = dpb->surf_hist[i];
        dpb->surf_hist[dpb->surf_hist_n - 1].surf = surf;
        dpb->surf_hist[dpb->surf_hist_n - 1].frame = frame;
    }
}

/* ------------------------------------------------- 全局运动（5.9.24） */

/* 规范/参考实现的位宽常量（libavcodec/av1.h）。 */
#define DMD_GM_ABS_ALPHA_BITS      12   /* alpha（idx 2..5 及各类型的斜切项）*/
#define DMD_GM_ALPHA_PREC_BITS     15
#define DMD_GM_ABS_TRANS_ONLY_BITS  9   /* 仅平移（TRANSLATION 的 idx 0/1）*/
#define DMD_GM_TRANS_ONLY_PREC_BITS 3
#define DMD_GM_ABS_TRANS_BITS      12   /* ROTZOOM/AFFINE 的 idx 0/1 */
#define DMD_GM_TRANS_PREC_BITS      6
#define DMD_WARPEDMODEL_PREC_BITS  16

enum dmd_gm_type { GM_IDENTITY = 0, GM_TRANSLATION = 1,
                   GM_ROTZOOM = 2, GM_AFFINE = 3 };

/* av_log2()：floor(log2(v))，v=0 时为 0。 */
static int dmd_gm_ilog2(uint32_t v)
{
    int n = 0;
    while (v > 1) { v >>= 1; n++; }
    return n;
}

/* 算术右移（等价 floor(v / 2^n)）。这里必须自己写：解码器侧用的是
 * `prev >> prec_diff`，对负数是**向下取整**；C 的整数除法是向零取整，
 * 直接除会差一格，算出的符号与解码器重建的就不一致。 */
static int32_t dmd_gm_fdiv(int32_t v, int32_t d)
{
    int32_t q = v / d;
    if (v % d && ((v < 0) != (d < 0))) q--;
    return q;
}

/* increment()（规范 4.10.5）：写 v，取值范围 [0,max]。
 * 位串是"v 个 1"，v<max 时再补一个 0 作终止符（共 v+1 位）；
 * v==max 时不补 0（共 max 位）—— 顶格值没有终止符，读侧靠数到 max 位收敛。
 * ⚠️ 这里曾写成"v+1 个 1 再补 0"，于是每个 subexp 符号前都多出一个 1：
 *   实测源符号 118 被读成 237（= 2v+1），且每个符号都各自错位又各自重新
 *   对齐（终止符仍在），所以只有 gm 的数值全错、帧头其余部分照常。 */
static void dmd_gm_put_increment(struct dmd_bitwriter *bw, uint32_t v,
                                 uint32_t max_len)
{
    for (uint32_t i = 0; i < v; i++)
        dmd_bw_put_flag(bw, 1);
    if (v != max_len)
        dmd_bw_put_flag(bw, 0);
}

/* sub_exp()（规范 4.10.8）：逐位对齐 libavcodec/cbs_av1 的 write_subexp。 */
static void dmd_gm_put_subexp(struct dmd_bitwriter *bw, uint32_t value,
                              uint32_t range_max)
{
    const uint32_t max_len = (uint32_t)dmd_gm_ilog2(range_max - 1) - 3;
    uint32_t len, range_bits, range_offset;

    if (value < 8) {
        range_bits = 3; range_offset = 0; len = 0;
    } else {
        range_bits = (uint32_t)dmd_gm_ilog2(value);
        len = range_bits - 2;
        if (len > max_len) {          /* 顶格与下一档合并，改用 ns() */
            range_bits--;
            len = max_len;
        }
        range_offset = 1u << range_bits;
    }
    dmd_gm_put_increment(bw, len, max_len);
    if (len < max_len)
        dmd_bw_put_bits(bw, value - range_offset, (int)range_bits);
    else
        dmd_av1_put_ns(bw, value - range_offset, range_max - range_offset);
}

/* recentering 的逆运算。解码侧（规范 7.11.3.6 / inverse_recenter）是
 *   if ((r<<1) <= mx) x = inverse_recenter(r, sub)
 *   else              x = mx-1-inverse_recenter(mx-1-r, sub)
 * 已知 x 与 r，反解 sub：先把越界的 r 镜像到前半区（连同 x），再按
 * inverse_recenter 的三段定义逐个求逆。 */
static uint32_t dmd_gm_recenter_encode(uint32_t x, uint32_t r, uint32_t mx)
{
    if ((int64_t)r * 2 > (int64_t)mx) {
        x = mx - 1 - x;
        r = mx - 1 - r;
    }
    if ((int64_t)x > (int64_t)r * 2) return x;
    if (x < r) return 2 * (r - x) - 1;
    return 2 * (x - r);
}

/* 规范里"重建后"的 gm_params 默认值（7.11.3.6 之前的初始化）：
 * idx%3==2 → 1<<16（恒等缩放的定点 1.0），其余 0。 */
static void dmd_gm_default(int32_t p[7][6])
{
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 6; j++)
            p[i][j] = (j % 3 == 2) ? (1 << DMD_WARPEDMODEL_PREC_BITS) : 0;
}

/* 算出一个 gm_params 符号（码流里 sub_exp 的整数值）。返回 0 成功；
 * -1 表示 VA 给的值无法由本语法表达，调用方须把该参考帧退回 IDENTITY。
 *
 * 依据：解码侧 cur = (decoded << prec_diff) + round，故
 * decoded = (cur - round) >> prec_diff 必须整除；r 由 prev 同法算出，
 * 必须是**解码器当时那份** prev（见 dmd_av1_dpb.gm_slot）。 */
static int dmd_gm_encode_param(int type, int idx, int32_t cur, int32_t prev,
                               int hp, uint32_t *sym, uint32_t *range_max)
{
    uint32_t abs_bits, prec_bits;

    if (idx < 2) {
        if (type == GM_TRANSLATION) {
            abs_bits  = DMD_GM_ABS_TRANS_ONLY_BITS - !hp;
            prec_bits = DMD_GM_TRANS_ONLY_PREC_BITS - !hp;
        } else {
            abs_bits  = DMD_GM_ABS_TRANS_BITS;
            prec_bits = DMD_GM_TRANS_PREC_BITS;
        }
    } else {
        abs_bits  = DMD_GM_ABS_ALPHA_BITS;
        prec_bits = DMD_GM_ALPHA_PREC_BITS;
    }

    const int32_t round     = (idx % 3 == 2) ? (1 << DMD_WARPEDMODEL_PREC_BITS) : 0;
    const int     prec_diff = (int)DMD_WARPEDMODEL_PREC_BITS - (int)prec_bits;
    const int32_t sub       = (idx % 3 == 2) ? (int32_t)(1u << prec_bits) : 0;
    const int32_t mx        = (int32_t)(1u << abs_bits);
    const int32_t scale     = 1 << prec_diff;
    const int32_t delta     = cur - round;

    if (delta % scale) return -1;              /* 不落在重建格点上 */
    const int32_t d = delta / scale;
    if (d < -mx || d > mx) return -1;

    int32_t r = dmd_gm_fdiv(prev, scale) - sub;
    if (r < -mx) r = -mx;
    if (r >  mx) r =  mx;

    /* 无符号域上的 recentering：x、r 各自平移 mx，mx 变成 2*mx+1 个取值。 */
    const uint32_t mxu = (uint32_t)mx * 2u + 1u;
    const uint32_t x  = (uint32_t)(d + mx);
    uint32_t ru = (uint32_t)(r + mx);
    if (ru >= mxu) ru = mxu - 1;
    *sym = dmd_gm_recenter_encode(x, ru, mxu);
    *range_max = mxu;
    return 0;
}

/* 把 uncompressed_header() 写进 bw，不含结尾的 trailing_bits /
 * byte_alignment —— 由调用方按封装形式决定：
 *   OBU_FRAME_HEADER(3) 用 trailing_bits（规范 5.9.1）
 *   OBU_FRAME(6)        用 byte_alignment（规范 5.10.1）
 * 这个区别是实测踩出来的：用错会让 tile_group 起始位置偏移，
 * dav1d 报 "Failed to read unit"。 */
/* 把 VA 的第 i 个参考翻成本合成流的槽号。返回 0 表示**解析不出来**：
 * 消费者给的 ref_frame_map 项是 VA_INVALID_ID（该槽没有帧），或它指向的帧
 * 早已被踢出影子 DPB。slot_out 仍会给一个可用的槽号（透传源槽号），
 * 便于调用方在"照样要送"的路径上保持原有行为。 */
static int resolve_ref(struct dmd_av1_dpb *dpb,
                       const VADecPictureParameterBufferAV1 *p,
                       int i, unsigned *slot_out)
{
    const unsigned va_slot = p->ref_frame_idx[i];
    *slot_out = va_slot < 8 ? va_slot : 0;
    if (!dpb || va_slot >= 8)
        return 0;
    const int want_frame = dmd_av1_frame_of(dpb, p->ref_frame_map[va_slot]);
    if (want_frame <= 0)
        return 0;
    for (int k = 0; k < 8; k++) {
        if (dpb->dpb_shadow[k] == want_frame) {
            *slot_out = (unsigned)k;
            return 1;
        }
    }
    return 0;
}

int dmd_av1_refs_resolvable(const void *pic_v, struct dmd_av1_dpb *dpb)
{
    const VADecPictureParameterBufferAV1 *p = pic_v;
    if (!p)
        return 1;
    const uint32_t ft = p->pic_info_fields.bits.frame_type;
    /* KEY / INTRA_ONLY / SWITCH 都不读参考帧内容（SWITCH 全刷），
     * allow_intrabc 的帧整帧 intra（规范里直接跳过帧间预测），也不读。
     * ⚠️ 不要拿 reference_select 当放行条件：规范 5.9.2 里它为 0 只是
     * "不显式给出 7 个槽号"，帧照样引用 LAST（ref_frame_idx[0]）。 */
    if (ft != 1 /* INTER_FRAME */ || p->pic_info_fields.bits.allow_intrabc)
        return 1;
    /* 真正会被解码器取用的槽位：LAST 恒用；复合参考/skip_mode 再用 ALTREF
     * （规范 5.9.2 的 ref_alternate_frame = ref_frame_map[ref_frame_idx[3]]）。
     * VA-API 的 PPB 里没有 reference_mode（它在 slice 参数里），所以这里
     * 保守地把 3 也算进"用到的参考"。 */
    unsigned unused_slot;
    if (!resolve_ref(dpb, p, 0, &unused_slot))
        return 0;
    if (!resolve_ref(dpb, p, 3, &unused_slot))
        return 0;
    return 1;
}

static void put_uncompressed_header(struct dmd_bitwriter *bwp,
                                    struct dmd_av1_dpb *dpb,
                                    const VADecPictureParameterBufferAV1 *p, int tile_size_bytes)
{
    /* struct dmd_bitwriter 是纯值结构：buf 指向调用方的缓冲，其余成员
     * 都是计数器。所以"拷入 → 写 → 拷回"是安全的，函数体内得以保留
     * 已与真实码流逐字段对齐过的 `&bw` 写法，不必逐行改动引入笔误。 */
    struct dmd_bitwriter bw = *bwp;

    /* ---- show_frame 强制置 1（方向 A 的落地，见本文件 SEF 长注释）----
     *
     * 硬件只对 show_frame=1 的帧吐 CAPTURE 缓冲，而 ffmpeg 的 VA-API 后端
     * 会为**每个**提交帧的 surface 要像素（show_frame=0 的帧日后靠 SEF
     * 复显，复显就是重用同一个 surface）。之前这些帧拿不到硬件像素，
     * 任何"从别的 surface 拷像素"的承接都被实测否证（信息根本不存在）。
     *
     * 修法：合成头里把 show_frame 写成 1，让硬件为每个提交帧都出一帧
     * （OUTPUT_ORDER=1 下按解码序输出，与 pending 登记顺序一致），
     * 每张 surface 拿到**自己那一帧**的真实像素。SEF 复显由 ffmpeg
     * 重用 surface 完成，驱动无需合成任何 SEF 头。
     *
     * show_frame 只控制"是否输出"，不影响 DPB/参考语义
     * （槽位刷新由 refresh_frame_flags 决定，与本位无关）。
     * 头内所有依赖它的语法分支（showable 位、error_resilient 推断、
     * KEY 帧的 refresh 推断、film_grain 出现条件）都统一用这个生效值，
     * 保证码流自身自洽。影子 DPB（decode.c）仍按原始值记账。
     *
     * DMD_AV1_NO_SHOWFORCE=1 恢复旧行为，供 A/B 对照。 */
    const int no_showforce = getenv("DMD_AV1_NO_SHOWFORCE")
                             && getenv("DMD_AV1_NO_SHOWFORCE")[0] == '1';
    const int show_frame_eff = no_showforce
        ? (int)p->pic_info_fields.bits.show_frame : 1;

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
     *
     * ---- 这正是像素帧数差距的根源（本轮测定）----
     * 源码流 av1_1080p.obu 的 OBU 构成：
     *     TD 150、OBU_FRAME 150、独立 FRAME_HEADER 70、SEQ_HDR 5
     *     150 个 OBU_FRAME 里 show_frame=1 的只有 80 个
     *     70 个独立 FRAME_HEADER **全部**是 show_existing_frame=1
     *     80 真实显示 + 70 复显 = 150 = 软解输出帧数
     *
     * 而硬件侧：送入 150 单元、收到 80 帧 —— 与 show_frame=1 的数量
     * 精确吻合，硬件行为完全正确，一帧不少。
     * 缺的 70 帧是 SEF 复显帧，本驱动从未合成。
     *
     * 但 SEF 帧无法在此合成：实测 BeginPicture 恰好被调 150 次
     * （= OBU_FRAME 数），SEF 帧**不经过** VA-API 解码路径
     * （探针显示 150 次调用全是真实帧，anchor == target）。
     * 解码侧 VA-API 也没有 show_existing_frame 字段
     * （只有 va_enc_av1.h 提到它，属编码侧）。
     * 结论：SEF 复显由 ffmpeg 在 VA-API 之上自行完成，
     * 驱动只需保证被引用的 surface 里有正确像素即可。
     * 所以 md5 对不上软解的原因要往"surface 内容"方向找，
     * 不是"少合成了 70 个 SEF 帧"。
     *
     * ---- 上面这段结论错了一半，本轮用隔离测试推翻 ----
     * 把源码流原封不动喂硬件（/tmp/sess3 绕过整个驱动）：
     *     session API: 送 150 单元, 收 150 帧
     * 硬件能吐满 150 帧！而驱动合成流只能收 80 帧。
     * 两条流的 OBU 构成差异只有一项：
     *     源码流   SEQ_HDR 5  TD 150  FRAME 150  FRAME_HDR 70（全是 SEF）
     *     合成流   SEQ_HDR 5  TD 150  FRAME 150  FRAME_HDR  0
     * 所以"硬件不吐 show_frame=0 的帧"是对的，
     * 但"因此拿不到那 70 帧"是错的 —— 硬件会为每个 SEF 头
     * 复显一次，只要码流里有那个头。缺的正是这 70 个 SEF 头。
     *
     * ---- 驱动能否推导出 SEF ----
     * 驱动收不到 SEF（150 次 BeginPicture 全是真实帧），
     * 但可以推导：show_frame=0 && showable=1 的帧日后必被 SEF 引用，
     * 插入时机由 order_hint（显示序）决定。实测这两个字段齐备：
     *     oh=16 show=0 showable=1   ← 将被 SEF 复显
     *     oh=1  show=1 showable=1   ← 自身即显示
     * 下一步：在合成流里按显示序补插 SEF FRAME_HEADER。
     *
     * （frame_id_numbers_present=0 时该字段仍存在，只是后续不读 frame_id。） */
    dmd_bw_put_flag(&bw, 0);

    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "frame_type", bw.byte_pos*8+bw.bit_pos);
    dmd_bw_put_bits(&bw, frame_type, 2);
    dmd_bw_put_flag(&bw, show_frame_eff);
    if (!show_frame_eff)
        dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.showable_frame);

    /* error_resilient_mode：KEY_FRAME 且 show_frame 时恒 1、不写入。 */
    /* error_resilient_mode（CBS 原文）：
     *   frame_type == SWITCH || (frame_type == KEY && show_frame)
     *     → infer 1（不写入码流）
     *   否则 flag(error_resilient_mode) */
    const int er_inferred = (frame_type == 3) ||
                            (is_key && show_frame_eff);
    if (!er_inferred) {
        if (getenv("DMD_AV1_BITS"))
            fprintf(stderr, "[bits] err_res @ %zu\n",
                    bw.byte_pos * 8 + bw.bit_pos);
        dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.error_resilient_mode);
    }

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
    if (enable_order_hint) {
        if (getenv("DMD_AV1_BITS"))
            fprintf(stderr, "[bits] order_hint @ %zu\n",
                    bw.byte_pos * 8 + bw.bit_pos);
        dmd_bw_put_bits(&bw, p->order_hint, order_hint_bits);
    }

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
                            (is_key && show_frame_eff);

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
    if (getenv("DMD_AV1_LOG"))
        fprintf(stderr, "[av1] ft=%u oh=%u refresh_all=%d err_res(生效)=%d "
                        "er_inferred=%d 原始er=%u\n",
                frame_type, p->order_hint, refresh_all, err_res, er_inferred,
                p->pic_info_fields.bits.error_resilient_mode);
    unsigned refresh_mask = 0;
    int my_slot = -1;
    if (!refresh_all) {
        /* 占位 refresh：轮转槽。真实值要等下一帧的 map 差分反算
         * （dmd_av1_patch_prev_refresh），届时就地改写这 8 位；
         * 若帧被 sync 提前冲出（flush），改写为 0 并进修复队列。
         * 影子登记也不在这里 —— 等真实值出来才登记（见 patch）。 */
        my_slot = dpb ? (int)(dpb->dpb_next_slot & 7u) : 0;
        refresh_mask = 1u << my_slot;
        if (dpb) {
            /* 帧号与 surface 归属此刻登记（后续帧的引用翻译要用），
             * 槽位放置则等 patch 反算出真实 refresh 后再做。 */
            const int me = ++dpb->frame_seq;
            dmd_av1_remember_surface(dpb, p->current_frame, me);
        }
        dpb->last_refresh_bitpos = bw.byte_pos * 8 + (size_t)bw.bit_pos;
        dpb->last_oh = p->order_hint;
        if (getenv("DMD_AV1_BITS"))
            fprintf(stderr, "[bits] refresh_mask=0x%02x @ %zu\n",
                    refresh_mask, bw.byte_pos * 8 + bw.bit_pos);
        dmd_bw_put_bits(&bw, refresh_mask, 8);
    } else if (dpb) {
        /* KEY / SWITCH 帧全刷：refresh 不写入码流（规范 7.20 推断），
         * 影子表立即登记 —— KEY 帧直送、无延迟，本帧 placement 即最终值。 */
        const int me = ++dpb->frame_seq;
        dmd_av1_remember_surface(dpb, p->current_frame, me);
        for (int k2 = 0; k2 < 8; k2++) {
            dpb->dpb_shadow[k2] = me;
            dpb->dpb_order_hint[k2] = p->order_hint;
        }
        dpb->dpb_next_slot = 0;
    }
    if (dpb && getenv("DMD_AV1_DBG")) {
        fprintf(stderr, "[dbg] shadow(cur=oh%u cur_surf=%u):",
                p->order_hint, (unsigned)p->current_frame);
        for (int k2 = 0; k2 < 8; k2++)
            fprintf(stderr, " %u", (unsigned)dpb->dpb_shadow[k2]);
        fprintf(stderr, "\n");
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
    unsigned my_idx[7] = { 0, 0, 0, 0, 0, 0, 0 };  /* 翻译后写入码流的槽号 */
    if (!intra_only) {
        /* frame_refs_short_signaling 需要 enable_order_hint；置 0 表示
         * 显式给出全部 7 个 ref_frame_idx（VA-API 提供的正是这个数组）。 */
        if (enable_order_hint)
            dmd_bw_put_flag(&bw, 0);
        /* 把 VA-API 的槽号经影子 DPB 翻译成**本合成流**的槽号。
         *
         * 本合成流的 DPB 槽位分配与源码流无关（KEY/全刷帧占全部 8 槽，
         * 其余帧按 dpb_next_slot 轮转，见 build_frame 末尾的影子登记），
         * 而 VA-API 的 ref_frame_idx[i] 是 ffmpeg 按源码流维护的槽号。
         * 翻译规则：源槽 s → ref_frame_map[s]（surface id，即"想要哪一帧
         * 的内容"）→ 在影子 DPB 里查该 surface 现在占本流的哪个槽。
         *
         * 为什么必须翻译而不是透传：透传只在合成流的槽位分配与源码流
         * 逐帧一致时才成立，那要求 refresh_frame_flags 也逐帧等于源真值
         * —— 而真值要等下一帧的 map 差分才能算出，导致每帧必须延迟一帧
         * 送料；ffmpeg 又会在 EndPicture 后立刻 Sync 叶帧的 surface，
         * 暂存帧被 flush 提前送出、带着错误的 refresh，参考链全断
         * （实测 160 帧样本 147 帧像素错误，KEY 帧全对、帧间帧全错）。
         * 翻译之后合成流自身自洽，每帧可立即送出，整个延迟/反算/flush
         * 机制都不再需要。
         *
         * CDF 继承也跟着对：解码器从 ref_frame_idx[primary_ref_frame]
         * 指向的槽加载 CDF，翻译保证那仍是"同一帧内容"，于是 CDF 演化
         * 链与源码流逐帧相同，源码流的 tile 数据可原样解码。
         *
         * 影子 DPB 里同一 surface 可能占多个槽（KEY/全刷帧刷新 8 槽），
         * 取哪个都等价（内容与 CDF 相同），取扫描到的第一个。 */
        for (int i = 0; i < 7; i++) {
            unsigned my = 0;
            const int ok = resolve_ref(dpb, p, i, &my);
            if (getenv("DMD_AV1_DBG"))
                fprintf(stderr, "[dbg] ref[%d]: va_slot=%u -> my_slot=%u %s\n",
                        i, (unsigned)p->ref_frame_idx[i], my,
                        ok ? "" : "未解析");
            if (getenv("DMD_AV1_REFDBG"))
                fprintf(stderr, "[refdbg] f=%d t=%u ref[%d]: surf=%u va_slot=%u"
                                " -> my=%u%s\n",
                                dpb ? dpb->frame_seq : -1, frame_type, i,
                                (unsigned)(p->ref_frame_idx[i] < 8
                                               ? p->ref_frame_map[p->ref_frame_idx[i]]
                                               : 0),
                                (unsigned)p->ref_frame_idx[i], my,
                                ok ? "" : "  <-- 未解析");
            my_idx[i] = my;
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
    if (dpb)
        dpb->last_endupd_bitpos = (size_t)-1;
    if (!p->pic_info_fields.bits.disable_cdf_update) {
        /* 记下位偏移：双趟解码要靠它把第一趟的回写关掉，见
         * dmd_av1_dpb::last_endupd_bitpos 的说明。 */
        if (dpb)
            dpb->last_endupd_bitpos = bw.byte_pos * 8 + (size_t)bw.bit_pos;
        dmd_bw_put_flag(&bw,
            (int)p->pic_info_fields.bits.disable_frame_end_update_cdf);
    }

    /* MiCols/MiRows：以 4x4 单元计的画面尺寸（规范 5.9.5 的推导）。
     * 2*((width+7)>>3) 等价于向上取整到 8 像素再折半。 */
    const uint32_t width  = (uint32_t)p->frame_width_minus1 + 1;
    const uint32_t height = (uint32_t)p->frame_height_minus1 + 1;
    const uint32_t mi_cols = 2 * ((width  + 7) >> 3);
    const uint32_t mi_rows = 2 * ((height + 7) >> 3);

    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "tile_info", bw.byte_pos*8+bw.bit_pos);
    put_tile_info(&bw, tile_size_bytes, p, mi_cols, mi_rows);
    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "quant", bw.byte_pos*8+bw.bit_pos);
    put_quantization_params(&bw, p);
    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "seg", bw.byte_pos*8+bw.bit_pos);
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

    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "lf", bw.byte_pos*8+bw.bit_pos);
    put_loop_filter_params(&bw, p, coded_lossless, (int)allow_intrabc);
    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "cdef", bw.byte_pos*8+bw.bit_pos);
    put_cdef_params(&bw, p, coded_lossless, (int)allow_intrabc);
    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "lr", bw.byte_pos*8+bw.bit_pos);
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
    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "txmode", bw.byte_pos*8+bw.bit_pos);
    if (!coded_lossless) {
        const uint32_t tm = p->mode_control_fields.bits.tx_mode;
        if (tm >= 2)
            dmd_bw_put_flag(&bw, 1);   /* TX_MODE_SELECT：写 1，无停止位 */
        else
            dmd_bw_put_flag(&bw, 0);   /* TX_MODE_LARGEST：停止位 */
    }

    /* frame_reference_mode()（5.9.23）：帧间帧写 reference_select。 */
    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "refsel", bw.byte_pos*8+bw.bit_pos);
    if (!intra_only)
        dmd_bw_put_flag(&bw, (int)p->mode_control_fields.bits.reference_select);

    /* skip_mode_params()（5.9.22）：skipModeAllowed 必须按**本合成流**的状态
     * 推导 —— 解码器也是这么算的。两边结论不一致就会多写或少写 1 位，
     * 帧头从此错位（实测：源码流 oh=16 那帧 skipModeAllowed=0、码流里没有
     * skip 位，而我们无条件写 0，整帧后移 1 位，CBS 把我们的 warp 位读成
     * reduced_tx_set，dav1d 直接 Invalid data）。
     *
     * 判据（CBS uncompressed_header 原文）：
     *   帧内 || !reference_select || !enable_order_hint → allowed = 0
     *   否则在 7 个参考里找前向（dist<0 里最接近当前的）与后向（dist>0 里
     *   最远离的）；有后向 → 1；无后向但存在比 forward 更早的"第二前向" → 1；
     *   否则 → 0。dist = get_relative_dist(ref_hint, 本帧 hint)。
     *
     * 参考帧的 order hint 取影子 DPB（我们自己的槽位分配），不是源码流的槽号，
     * 因为解码器看到的正是我们写进码流的 my_idx。 */
    int skip_allowed = 0;
    if (!intra_only && p->mode_control_fields.bits.reference_select &&
        enable_order_hint && order_hint_bits > 0 && dpb) {
        const unsigned ohm = 1u << order_hint_bits;
        /* 规范 get_relative_dist（5.9.x）：v = (a-b) & (ohm-1)；仅当 v **大于**
         * ohm/2 才减 ohm。ties（v == ohm/2）保留正值。
         * 之前写成 `v - (v & ohm)`，等于用 bit order_hint_bits 判符号：只有差值
         * 恰好 = 3*ohm/4 时才为负，回绕处（如 hint=0 参考 hint=96，ohm=128）
         * 本应 -32 却算成 +96，skipModeAllowed 少 1 位，帧头整体错位 1 位。 */
#define RELD(x, y) ((((x) - (y)) & (ohm - 1u)) > (ohm >> 1)                         \
                    ? (int)((((x) - (y)) & (ohm - 1u)) - ohm)                       \
                    : (int)(((x) - (y)) & (ohm - 1u)))
        int fwd = -1, bwd = -1;
        unsigned fwd_h = 0, bwd_h = 0;
        for (int i = 0; i < 7; i++) {
            const unsigned h = dpb->dpb_order_hint[my_idx[i] & 7u];
            const int d = RELD(h, p->order_hint);
            if (d < 0) {
                if (fwd < 0 || RELD(h, fwd_h) > 0) { fwd = i; fwd_h = h; }
            } else if (d > 0) {
                if (bwd < 0 || RELD(h, bwd_h) < 0) { bwd = i; bwd_h = h; }
            }
        }
        if (fwd >= 0) {
            skip_allowed = bwd >= 0;
            for (int i = 0; !skip_allowed && i < 7; i++)
                if (RELD(dpb->dpb_order_hint[my_idx[i] & 7u], fwd_h) < 0)
                    skip_allowed = 1;
        }
#undef RELD
        if (getenv("DMD_AV1_BITS"))
            fprintf(stderr, "[skip] oh=%u va_skip=%u refsel=%u allowed=%d"
                            " fwd=%d fwd_oh=%u bwd=%d my_idx=[%u,%u,%u,%u,%u,%u,%u]"
                            " ref_oh=[%u,%u,%u,%u,%u,%u,%u]\n",
                    p->order_hint,
                    p->mode_control_fields.bits.skip_mode_present,
                    p->mode_control_fields.bits.reference_select,
                    skip_allowed, fwd, fwd_h, bwd,
                    my_idx[0], my_idx[1], my_idx[2], my_idx[3],
                    my_idx[4], my_idx[5], my_idx[6],
                    dpb->dpb_order_hint[my_idx[0] & 7u],
                    dpb->dpb_order_hint[my_idx[1] & 7u],
                    dpb->dpb_order_hint[my_idx[2] & 7u],
                    dpb->dpb_order_hint[my_idx[3] & 7u],
                    dpb->dpb_order_hint[my_idx[4] & 7u],
                    dpb->dpb_order_hint[my_idx[5] & 7u],
                    dpb->dpb_order_hint[my_idx[6] & 7u]);
    }
    if (skip_allowed) {
        /* 值取 VA-API 的 skip_mode_present：ffmpeg 的 mode_control_fields
         * 是 CBS 从源码流解析出来的，忠实于源码流。 */
        dmd_bw_put_flag(&bw,
            (int)p->mode_control_fields.bits.skip_mode_present);
    }

    /* allow_warped_motion（5.9.22，紧接 skip_mode_params 之后）：
     * 权威条件见 CBS cbs_av1_syntax_template.c 的 uncompressed_header：
     *   if (frame_is_intra || error_resilient_mode || !seq->enable_warped_motion)
     *       infer(allow_warped_motion, 0);      // 不占码流位
     *   else
     *       flag(allow_warped_motion);
     * 与 is_motion_mode_switchable **无关** —— 那个字段只决定块级的
     * motion mode 是否需要解析，早在本函数上方 read_mv_mode 处就用掉了。
     * 旧实现把它加进守卫，于是 mms=0 的帧少写 1 位，其后
     * reduced_tx_set / is_global[7] / film_grain / tile_group 全体前移，
     * 软解报 Invalid data、硬解吐垃圾。
     * 序列级 enable_warped_motion 我们恒写 1（见 :902），故此处不必再判。 */
    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu va_warp=%u va_redtx=%u va_mms=%u cf=%u oh=%u\n", "warp", bw.byte_pos*8+bw.bit_pos,
            (unsigned)p->pic_info_fields.bits.allow_warped_motion,
            (unsigned)p->mode_control_fields.bits.reduced_tx_set_used,
            (unsigned)p->pic_info_fields.bits.is_motion_mode_switchable,
            (unsigned)p->current_frame, (unsigned)p->order_hint);
    if (!intra_only && !err_res)
        /* 直接转写 VA-API 的 allow_warped_motion（理由同上：
         * ffmpeg CBS 解析值忠实于源码流，旧"uniform_ref 置 0"规则是
         * 对抗位错位时拟合出来的，换码流即错）。 */
        dmd_bw_put_flag(&bw,
            (int)p->pic_info_fields.bits.allow_warped_motion);

    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "redtx", bw.byte_pos*8+bw.bit_pos);
    dmd_bw_put_flag(&bw, (int)p->mode_control_fields.bits.reduced_tx_set_used);

    /* global_motion_params()（规范 5.9.24）：帧间帧逐参考帧写
     *   is_global[i] | is_rot_zoom[i] | is_translation[i] | 参数们
     *
     * VA-API 的 wm[i] 给的是**重建后**的参数（ffmpeg libavcodec/vaapi_av1.c
     * 直接抄 AV1Frame.gm_params），要还原码流里的符号必须做 recentering
     * 的逆运算 + subexp 写位，见上面 dmd_gm_put_param。
     *
     * ⚠️ 差分的基准 prev 必须是**解码器当时那份**：规范里它取自
     *   ref_frame_idx[primary_ref_frame] 那个槽。我们用 my_idx[]（已翻译成
     *   本合成流的槽号）去查按槽备份 gm_slot[]，与硬件读到同一个值。
     *   primary_ref_frame 为 NONE（帧内帧/err_res/源码流本就不引用任何帧）
     *   时用默认值，与规范 7.11.3.6 的 setup_past_independence 一致。
     *
     * 顺序：ROTZOOM 在码流里是 [2][3][0][1]，AFFINE 是 [2][3][4][5][0][1]，
     * TRANSLATION 只有 [0][1] —— 不是下标升序，写错顺序整帧头就错位。
     *
     * 任一个参数编不出来（VA 给的值不落在重建格点上、或越出该档的
     * [-mx,mx]）就**整参考帧退回 IDENTITY**：先算后写，绝不写半截。
     * 退回后本帧存的就是默认参数，后续帧的 prev 跟着一起退回默认，
     * 合成流仍然自洽，只是丢掉该参考帧的全局运动补偿（与旧行为相同）。 */
    struct dmd_av1_gm cur_gm;
    dmd_gm_default(cur_gm.p);
    if (!intra_only) {
        /* 码流里的下标顺序（不是升序！）：见 5.9.24 的调用次序。 */
        static const int gm_order[4][6] = {
            { -1, -1, -1, -1, -1, -1 },   /* IDENTITY：不写参数 */
            {  0,  1, -1, -1, -1, -1 },   /* TRANSLATION */
            {  2,  3,  0,  1, -1, -1 },   /* ROTZOOM */
            {  2,  3,  4,  5,  0,  1 },   /* AFFINE */
        };
        static const int gm_n[4] = { 0, 2, 4, 6 };
        const int hp = (int)p->pic_info_fields.bits.allow_high_precision_mv;
        int32_t prev[7][6];

        if (primary_ref_none || !dpb)
            dmd_gm_default(prev);
        else
            memcpy(prev, dpb->gm_slot[my_idx[p->primary_ref_frame & 7u] & 7u].p,
                   sizeof(prev));

        for (int i = 0; i < 7; i++) {
            int type = (int)p->wm[i].wmtype;
            if (type < GM_IDENTITY || type > GM_AFFINE) type = GM_IDENTITY;
            const int32_t *cm = p->wm[i].wmmat;
            const int n = gm_n[type];
            uint32_t sym[6] = { 0 }, rmax[6] = { 0 };
            int ok = type != GM_IDENTITY;

            for (int k = 0; ok && k < n; k++)
                if (dmd_gm_encode_param(type, gm_order[type][k],
                                        cm[gm_order[type][k]],
                                        prev[i][gm_order[type][k]],
                                        hp, &sym[k], &rmax[k]) < 0)
                    ok = 0;

            if (!ok) {
                dmd_bw_put_flag(&bw, 0);      /* is_global=0 → IDENTITY */
                continue;
            }
            dmd_bw_put_flag(&bw, 1);                        /* is_global */
            if (type == GM_ROTZOOM) {
                dmd_bw_put_flag(&bw, 1);                    /* is_rot_zoom */
            } else {
                dmd_bw_put_flag(&bw, 0);
                dmd_bw_put_flag(&bw, type == GM_TRANSLATION);  /* is_translation */
            }
            for (int k = 0; k < n; k++)
                dmd_gm_put_subexp(&bw, sym[k], rmax[k]);
            if (getenv("DMD_AV1_BITS"))
                fprintf(stderr, "[bits] gm i=%d type=%d sym=%u,%u,%u,%u,%u,%u "
                                "m=%d,%d,%d,%d,%d,%d\n",
                        i, type, sym[0], sym[1], sym[2], sym[3], sym[4], sym[5],
                        cm[0], cm[1], cm[2], cm[3], cm[4], cm[5]);

            /* 本帧存进槽的参数 = 解码器重建的那份。VA 的 wmmat[0..5] 正是
             * AV1Frame.gm_params[0..5]（ffmpeg 逐项照抄，含未传输下标的
             * 默认值与 ROTZOOM 的 4/5 推导），所以直接取六个值即可。 */
            for (int j = 0; j < 6; j++) cur_gm.p[i][j] = cm[j];
        }
    }
    if (dpb) {
        dpb->gm_pending = cur_gm;
        dpb->gm_pending_frame = dpb->frame_seq;
        if (refresh_all) {
            /* KEY/SWITCH 全刷：8 个槽都是本帧（与上面 dpb_shadow 的登记
             * 同一处、同一条件）。帧内帧的 gm 恒为默认值。 */
            for (int k = 0; k < 8; k++) dpb->gm_slot[k] = cur_gm;
        }
    }

    /* film_grain_params()（5.9.30）：序列头里 film_grain_params_present
     * 为 0 时整段不出现。我们如实转写该标志，故此处同样条件。 */
    if (p->seq_info_fields.fields.film_grain_params_present &&
        (show_frame_eff || p->pic_info_fields.bits.showable_frame))
        dmd_bw_put_flag(&bw, 0);         /* apply_grain = 0 */

    /* 结尾不写 trailing_bits / byte_alignment —— 交给调用方按封装形式决定。 */
    *bwp = bw;
}

/* 把帧头 payload 装进指定类型的 OBU。obu_type 决定结尾用哪种对齐：
 *   DMD_OBU_FRAME_HEADER → trailing_bits（规范 5.9.1）
 *   DMD_OBU_FRAME        → byte_alignment（规范 5.10.1），之后紧跟 tile_group
 * 返回写入 out 的字节数（含 OBU 头），失败返回 0。
 * tail_out 回传 payload 的位长度，供 OBU_FRAME 继续拼 tile_group。 */
static size_t build_frame_header_obu(int tile_size_bytes,
                                    const VADecPictureParameterBufferAV1 *p,
                                     int obu_type,
                                     unsigned char *body, size_t body_cap,
                                     size_t *body_len_out,
                                     struct dmd_av1_dpb *dpb)
{
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, body, body_cap);
    put_uncompressed_header(&bw, dpb, p, tile_size_bytes);


    if (getenv("DMD_AV1_BITS"))
        fprintf(stderr, "[bits] header_end @ %zu\n",
                bw.byte_pos * 8 + bw.bit_pos);

    /* 此处曾有一个"身份不明的补位"（无条件写 1 个 0  bit）。它不存在于
     * 规范里，且在帧头真实长度 ≡ 0 (mod 8) 的帧上会把 byte_alignment 撑成
     * 一整个 0x00 字节 —— tile 数据整体后移 1 字节，熵解码从第一 byte 起
     * 就是错的（实测 640x360 第 7 帧起 dav1d 直接报 Invalid data）。
     * 帧头字段现已与源码流逐位对齐，不需要任何补位。 */
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
    /* 独立 FRAME_HEADER OBU 不带 tile 数据，宽度取最小值 1。 */
    if (build_frame_header_obu(1, p, DMD_OBU_FRAME_HEADER,
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

    /* tile_size_bytes：取最大 tile 长度所需的最小字节数。
     * 帧头的 tile_size_bytes_minus_1 与 tile_group 里 tile_size_minus_1
     * 的宽度必须一致，故先算出来再合成帧头。
     * 注意写入的是 len-1（tile_size_minus_1），故按 max_len-1 取位宽。 */
    size_t max_tile = 0;
    for (int i = 0; i + 1 < num_tiles; i++)
        if (tiles[i].len > max_tile) max_tile = tiles[i].len;
    int tile_size_bytes = 1;
    if (max_tile > 0) {
        size_t v = max_tile - 1;
        while (v >> (tile_size_bytes * 8)) tile_size_bytes++;
    }
    if (tile_size_bytes > 4) tile_size_bytes = 4;

    /* 帧头（结尾 byte_alignment，不是 trailing_bits）。 */
    unsigned char fh[512];
    size_t fh_len = 0;
    if (build_frame_header_obu(tile_size_bytes, p, DMD_OBU_FRAME,
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
            payload_len += (size_t)tile_size_bytes;  /* tile_size_minus_1 */
    }

    const size_t hdr = dmd_av1_obu_header(DMD_OBU_FRAME, payload_len,
                                          out, out_cap);
    if (hdr == 0 || hdr + payload_len > out_cap)
        return 0;

    /* ⚠️ 把 last_refresh_bitpos 由"帧头内偏移"换算为"本 OBU 内偏移"。
     *
     * put_uncompressed_header 里的 bitwriter 基于局部 body 缓冲，
     * 记下的是帧头内的位偏移（实测 19）。但上层改写时拿到的是含
     * OBU 头的完整缓冲，直接用会打偏 hdr*8 位。
     * 实测后果：OBU 头首字节由 0x32 被改成 0x20，
     * type 6(FRAME) 变成 4(TILE_GROUP) 且 has_size 被清零，
     * 整条流从该处结构崩塌 —— dav1d 只认出 5 个 OBU 就报 Invalid data。
     *
     * 调用方（decode.c）再叠加 TD 等更外层前缀的长度。 */
    if (dpb && dpb->last_refresh_bitpos != (size_t)-1)
        dpb->last_refresh_bitpos += hdr * 8;
    if (dpb && dpb->last_endupd_bitpos != (size_t)-1)
        dpb->last_endupd_bitpos += hdr * 8;

    unsigned char *q = out + hdr;
    for (size_t i = 0; i < fh_len; i++)
        *q++ = fh[i];
    for (size_t i = 0; i < tg_hdr_len; i++)
        *q++ = tg_hdr[i];
    for (int i = 0; i < num_tiles; i++) {
        if (i + 1 < num_tiles) {
            /* le(tile_size_bytes)：宽度必须与帧头写的
             * tile_size_bytes_minus_1 + 1 一致，否则从第二个 tile 起全错位。
             * 该宽度由上面按 max(tile_len) 算出，不再写死 —— 曾写死 2 字节，
             * 而源码流实际用 1 字节，每个间隙多 1 字节导致位流整体偏移。 */
            const uint32_t v = (uint32_t)(tiles[i].len - 1);
            for (int b = 0; b < tile_size_bytes; b++)
                *q++ = (unsigned char)((v >> (8 * b)) & 0xFF);
        }
        for (size_t k = 0; k < tiles[i].len; k++)
            *q++ = tiles[i].data[k];
    }
    /* 帧已合成成功 —— 把本帧登记进影子 DPB，供后续帧的 ref_frame_idx 查询。
     * 必须在成功路径的末尾做：合成失败时不能污染 DPB 状态，
     * 否则后续帧会引用一个从未真正写入解码器的槽。 */
    /* 影子 DPB 登记已移入 put_uncompressed_header 的 refresh 写入处：
     * 全刷帧（KEY/SWITCH）在那里清空 8 槽，其余帧在那里占一个
     * "确定已死"的受害者槽。此处不再登记，避免同帧双重写入。 */

    return hdr + payload_len;
}

size_t dmd_av1_build_show_existing(unsigned char *buf, size_t cap,
                                   unsigned map_idx)
{
    if (!buf || cap < 4)
        return 0;

    /* 载荷：show_existing_frame(1)=1、frame_to_show_map_idx(3)，
     * 随后 trailing_bits（规范 5.3.4：先写一个 1，再零填充到字节边界）。
     * 4 位有效数据 + 停止位 1 + 3 个 0 = 恰好 1 字节。 */
    struct dmd_bitwriter bw;
    unsigned char payload[2];
    dmd_bw_init(&bw, payload, sizeof(payload));
    dmd_bw_put_flag(&bw, 1);                 /* show_existing_frame */
    dmd_bw_put_bits(&bw, map_idx & 7u, 3);   /* frame_to_show_map_idx */
    dmd_av1_trailing_bits(&bw);
    if (bw.overflow)
        return 0;

    const size_t plen = bw.byte_pos + (bw.bit_pos ? 1 : 0);

    /* OBU 头：forbidden(1)=0 type(4)=3 extension(1)=0 has_size(1)=1
     * reserved(1)=0 → (3 << 3) | 0x02 = 0x1a，与源码流实测一致。 */
    size_t n = 0;
    buf[n++] = (unsigned char)((DMD_OBU_FRAME_HEADER << 3) | 0x02);
    /* leb128 的 obu_size；plen 恒为 1，单字节足够。 */
    if (plen >= 0x80)
        return 0;
    buf[n++] = (unsigned char)plen;
    for (size_t i = 0; i < plen; i++)
        buf[n++] = payload[i];
    return n;
}

void dmd_av1_patch_prev_refresh(struct dmd_av1_dpb *dpb,
                                const void *cur_pic,
                                unsigned char *prev_frame_bytes,
                                size_t prev_len,
                                size_t prev_bitpos,
                                int prev_frame)
{
    /* ⚠️ 不能因 prev_frame_bytes 为空就提前返回。
     *
     * 那样 KEY 帧与第一个帧间帧（此时还没有暂存帧）都会跳过末尾的
     * map 快照，使 prev_valid 迟一拍置位。实测后果：算出的序列是
     * —,8,32,64 而真值为 1,8,32,64 —— 首个值 1 丢失，其余整体错位
     * 赋给了前一帧，改写因此全部落在错误的帧上。
     * 无论本次是否有帧可改写，都必须保存 map 作为下一次差分的基准。 */
    if (!dpb || !cur_pic) return;
    const VADecPictureParameterBufferAV1 *p = cur_pic;
    if (getenv("DMD_AV1_DBG"))
        fprintf(stderr, "[dbg] patch: cur_oh=%u prev_valid=%d hold=%s bitpos=%zd\n",
                p->order_hint, dpb->prev_valid,
                prev_frame_bytes ? "yes" : "NO",
                prev_bitpos == (size_t)-1 ? -1 : (ssize_t)prev_bitpos);

    /* ---- 计算源 DPB 上一帧踢出的帧 E（供本帧的槽位分配）----
     *
     * 本帧的 map 与上一帧的 map 的差异 = 上一帧解码时对 DPB 的改动：
     * 被改的槽里，旧内容就是源编码器踢出的帧（E）。源编码器保证被踢出的
     * 帧不再被任何后续帧引用 —— 它是当前时刻唯一"确定已死"的帧，
     * 让本帧占它的槽永远不会覆盖活引用（滞后一帧淘汰）。
     * 差分为 0（上一帧 refresh=0，没动 DPB）或多槽（KEY 全刷）时置
     * evict_valid=0，由 put_uncompressed_header 的兜底规则选槽。 */
    dpb->evict_valid = 0;
    for (int i = 0; i < 8; i++) {
        if (p->ref_frame_map[i] != dpb->prev_ref_map[i]) {
            const int fr = dmd_av1_frame_of(dpb, dpb->prev_ref_map[i]);
            if (fr > 0) {
                dpb->evict_frame = fr;
                dpb->evict_valid = 1;
            }
            break;
        }
    }
    if (getenv("DMD_AV1_DBG"))
        fprintf(stderr, "[dbg] patch: evict_valid=%d frame=%d\n",
                dpb->evict_valid, dpb->evict_frame);

    /* ⚠️ prev_valid 必须在 KEY 帧那次调用就置位。
     *
     * 差分需要"上一帧的 map"，而 KEY 帧本身不写 refresh（无需改写），
     * 但它的 map 是后续第一个帧间帧做差分的基准。若只在真正执行改写时
     * 才置位，整个序列会晚一拍：实测算出 —,8,32,64 而真值是 1,8,32,64，
     * 首个值 1 丢失，且 8/32/64 被错位赋给了前一帧。
     * 所以本函数无论是否改写，末尾都保存 map 并置 prev_valid。 */
    if (dpb->prev_valid && prev_frame_bytes &&
        prev_bitpos != (size_t)-1 && prev_len > 0) {
        /* 本帧 map 与上帧 map 的差异位 = 上一帧实际写入的槽。
         * 实测 4 帧全部命中源码流真实值（1/8/32/64）。
         *
         * ⚠️ 但**本帧是 KEY/INTRA_ONLY 全刷帧时不能做这个差分**（第 94 轮实机定位）。
         * 规范 7.20：帧内帧的 RefreshFrameFlags 取 allFrames 是**解码器侧的
         * 推导值**，不写入码流。全刷会把 ref_frame_map 八个槽一次性换掉，
         * 于是差分算出 0xff —— 而这个 0xff 会被赋给**上一个帧间帧**，
         * 覆盖它真正的 refresh 值。
         *
         * 实机症状（150 帧样本，oh=30 处是 KEY 帧）：
         *     反算序列 ... 0x00 0xff 0xff 0x01 0x08 ...
         *     源码流真值 ... 0x00 0x08 0x01 0x08 0x20 ...
         * 前 28 项完全一致，第 28 项起插入两个 0xff，导致其后整体错位一项，
         * 全序列 104 项里 49 项不符。而 refresh 错位会让参考帧全盘错，
         * 表现为"帧数对、画面数对，但从第 1 帧起像素就不一致"。
         *
         * 正确做法：全刷帧不参与差分，直接跳到末尾更新基准 map。
         * 上一个帧间帧的 refresh 保持它自己合成时的轮转占位值 —— 那个值
         * 未必等于源真值，但至少不会被 0xff 污染，也不会造成序列错位。
         *
         * ⚠️ 只挡住全刷帧本身还不够（这是第一版修复漏掉的一半）。
         * 全刷帧把基准 map 整体换掉之后，**紧跟它的那一帧**拿这份新基准
         * 做差分，照样会算出接近满位的 mask。实测第一版把不一致从 49 降到
         * 29，但第 28 项仍是 0xff —— 正是全刷帧的后继帧。
         * 所以全刷帧除了跳过差分，还必须让基准失效一拍（prev_valid=0），
         * 让后继帧也跳过一次，从它之后重新建立差分基准。 */
        /* ⚠️ 判据必须与 dmd_av1_build_frame 里的 refresh_all 完全一致
         * （规范 7.20 / 5.9.2）：SWITCH 帧，或 show_frame=1 的 KEY 帧。
         *
         * 第一版我写成 `frame_type==0 || frame_type==2` —— 两处都错：
         *   · 漏了 show_frame：KEY 帧只在 show_frame=1 时才全刷
         *   · 多算了 INTRA_ONLY(2)：它不全刷，照常写 refresh_frame_flags
         * 单元测试当场抓住（用例以 memset 构造 pic，frame_type 落在 0，
         * 被误判成全刷而跳过差分，两项断言归零）。
         * 教训：同一个语义在两处实现就一定会走偏，这里直接抄 refresh_all 的式子。 */
        const unsigned cur_ft = p->pic_info_fields.bits.frame_type;
        int cur_is_intra = (cur_ft == 3) ||
                           (cur_ft == 0 && p->pic_info_fields.bits.show_frame);
        unsigned mask = 0;
        for (int i = 0; i < 8; i++) {
            if (p->ref_frame_map[i] != dpb->prev_ref_map[i])
                mask |= (1u << i);
        }
        if (cur_is_intra) {
            if (getenv("DMD_AV1_LOG"))
                fprintf(stderr, "[av1] 本帧是全刷帧(ft=%u show=%u)，跳过差分"
                                "（否则会算出 0x%02x 污染上一帧）\n",
                        cur_ft, p->pic_info_fields.bits.show_frame, mask);
            mask = 0;
            /* 保存新基准，但标记为"不可用于差分" —— 后继帧跳过一次，
             * 由它自己的 map 重新建立基准。 */
            for (int i = 0; i < 8; i++) dpb->prev_ref_map[i] = p->ref_frame_map[i];
            dpb->prev_valid = 0;
            return;
        }

        /* 就地改写上一帧帧头里那 8 位。
         * refresh_frame_flags 未必字节对齐，须按位写，MSB first。 */
        if (getenv("DMD_AV1_LOG"))
            fprintf(stderr, "[av1] 反算上帧 refresh=0x%02x (bitpos=%zu len=%zu)\n",
                    mask, prev_bitpos, prev_len);
        if (getenv("DMD_AV1_DBG"))
            fprintf(stderr, "[dbg] patch: cur_oh=%u mask=0x%02x %s\n",
                    p->order_hint, mask, mask ? "WRITTEN" : "ZERO");
        size_t bp = prev_bitpos;
        if ((bp + 8 + 7) / 8 <= prev_len) {
            for (int k = 0; k < 8; k++) {
                size_t bit = bp + (size_t)k;
                size_t byi = bit >> 3;
                int    bii = 7 - (int)(bit & 7);
                int    v   = (mask >> (7 - k)) & 1;
                if (v) prev_frame_bytes[byi] |=  (unsigned char)(1 << bii);
                else   prev_frame_bytes[byi] &= (unsigned char)~(1u << bii);
            }
        }
        /* 影子登记：源码流把上一帧放进了 mask 的那些槽，我们的槽位
         * 与源一致（合成流的 refresh 已改写为源真值），影子表照抄。
         * 之后的引用翻译据此进行。 */
        if (prev_frame > 0) {
            for (int k = 0; k < 8; k++)
                if (mask >> k & 1u) {
                    dpb->dpb_shadow[k] = prev_frame;
                    dpb->dpb_order_hint[k] = dpb->last_oh;
                    /* 全局运动的按槽备份必须与影子表同步：下一帧算
                     * 差分符号时查的就是这里登记的这一帧的参数。
                     * gm_pending 是"最近合成帧"的参数，只有它确实属于
                     * prev_frame 时才可用（否则宁可用默认值 —— 与解码器
                     * 那份不一致时最多丢一次运动补偿，不会写坏码流）。 */
                    if (dpb->gm_pending_frame == prev_frame)
                        dpb->gm_slot[k] = dpb->gm_pending;
                }
        }
    }

    for (int i = 0; i < 8; i++) dpb->prev_ref_map[i] = p->ref_frame_map[i];
    dpb->prev_valid = 1;
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
