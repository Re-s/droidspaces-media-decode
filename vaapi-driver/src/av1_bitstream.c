/* AV1 OBU Inverted Synthesis - Longer Encoding, Alignment with OBU Head.
 *
 * Design description at av1_bitstream.h. This document only allows for the bottom bit to be written in the original language, and then only the bottom bit to be written.
 * The syntax of the sequence header/frame/tile group is submitted in Riga.
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
            byte |= 0x80;          /* And the following bytes. */
        out[n++] = byte;
    } while (v);
    return n;
}

/* ------------------------------------------------------------------ uvlc */

void dmd_av1_put_uvlc(struct dmd_bitwriter *bw, uint32_t v)
{
    /* AV1 Standard 4.10.3: Leave Zeros 0, one, then leave Zeros bitile.
     * Value v Encoded as (1 < leaving Zeros) - 1 + mantissa. */
    uint32_t leading_zeros = 0;
    uint64_t val = (uint64_t)v + 1;

    while ((val >> (leading_zeros + 1)) != 0)
        leading_zeros++;

    if (leading_zeros >= 32) {
        bw->overflow = 1;
        return;
    }

    /* Left_zeros 0 bits; put_bits not accepted nbits=0, so empty. */
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
    /* AV1 Regulation 4.10.4: A small byte order requiring byte alignment at the time of the call. */
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
    /* AV1 Code 4.10.7 ns (n): Asymmetric code, with one small value.
     *   w = FloorLog2(n) + 1
     *   m = (1 << w) - n
     * v < m written directly with w-1 bit; otherwise the w bit written (v+m). */
    if (n == 0) {
        bw->overflow = 1;
        return;
    }
    if (n == 1)
        return;                    /* Only one value, not in place */

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
    /* AV1 Regulation 4.10.6 su(n): n bit supplementation. Take a lower nbits bit when writing.
     * Decoding side expands by sign bit. */
    if (nbits <= 0 || nbits > 32) {
        bw->overflow = 1;
        return;
    }
    uint32_t mask = (nbits == 32) ? 0xffffffffu : ((1u << nbits) - 1u);
    dmd_bw_put_bits(bw, (uint32_t)v & mask, nbits);
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - Alignment */

void dmd_av1_byte_align(struct dmd_bitwriter *bw)
{
    /* AV1 Regulation 5.3.5 byte_alignment(): Nothing but scratch, ** not to write stop bit**.
     * This is the key difference with H.264/HEVC rbsp_trailing_bits. */
    while (bw->bit_pos != 0 && !bw->overflow)
        dmd_bw_put_flag(bw, 0);
}

void dmd_av1_trailing_bits(struct dmd_bitwriter *bw)
{
    /* AV1 Regulation 5.3.4 trailing_bits(): one and then complete zero to byte boundary.
     * Note that this 1 -- it's Payload end mark -- even if it is now aligned. */
    dmd_bw_put_flag(bw, 1);
    while (bw->bit_pos != 0 && !bw->overflow)
        dmd_bw_put_flag(bw, 0);
}

/* - - - - - - - - - - - - - - - - - - - Sequence head synthesis (2/4) */

/* Bit-wide calculation: AV1 is used to use the name "expression x takes several" in FloorLog2(x)+1. */
static int bits_for(uint32_t v)
{
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n ? n : 1;
}

/* Coll._config(), AV1 Reg. 5.5.2. */
static void put_color_config(struct dmd_bitwriter *bw,
                             const VADecPictureParameterBufferAV1 *p)
{
    const uint32_t depth_idx  = p->bit_depth_idx;
    const uint32_t mono       = p->seq_info_fields.fields.mono_chrome;
    const uint32_t sub_x      = p->seq_info_fields.fields.subsampling_x;
    const uint32_t sub_y      = p->seq_info_fields.fields.subsampling_y;

    /* high_bitdepth / twelve_bit: bit_depth_idx 0/1/2 represents 8/10/12 bits.
     * (va_dec_av1.h:255-260). Only the profile 2 can reach 12 places. */
    const int high_bitdepth = (depth_idx != 0);
    dmd_bw_put_flag(bw, high_bitdepth);
    if (p->profile == 2 && high_bitdepth)
        dmd_bw_put_flag(bw, depth_idx == 2);       /* twelve_bit */

    if (p->profile != 1)
        dmd_bw_put_flag(bw, (int)mono);            /* mono_chrome */

    /* color_description_present_flag = 0。
     *
     * ⁇  used 1 and clearly wrote 3 descriptors (to save VA-API from)
     * The results of the video are not consistent with the real stream: libaom 0, which is not a good idea.
     * So all three take the UNSPECIFED(2). More writing makes the head of the sequence three bytes long.
     *
     * The cost is acceptable: the choice of color conversion matrices is influenced only by the cost of the matrix_coefficients.
     * Does not affect the ability to defox the frame; this driver produces NV12 for the upper layer, where colour interpretation is the responsibility. */
    dmd_bw_put_flag(bw, 0);

    if (mono) {
        dmd_bw_put_flag(bw, (int)p->seq_info_fields.fields.color_range);
        return;                                    /* Monochrome branch is over. */
    }

    dmd_bw_put_flag(bw, (int)p->seq_info_fields.fields.color_range);

    /* subsampling only writes when profile 2 and 12 bits; profile 0 is 4:2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
     * Profile 1 persistent 4:4 with no code currents (regulation 5.5.2). */
    if (p->profile == 2 && depth_idx == 2) {
        dmd_bw_put_flag(bw, (int)sub_x);
        if (sub_x)
            dmd_bw_put_flag(bw, (int)sub_y);
    }

    /* The same name section of VA-API appears only at 4:2 :00.
     * va_deprecaed(:285) and therefore wrote UNKNOWN(0): It only affects color-plug-in phase scenarios, but it is not a good idea.
     * Without prejudice to the unwinding of the frame. */
    /* The real code flow 1;
     * The same name section of VA-API is marked va_deprecated (285).
     * This value affects only the phase scenario of the colour plug value and does not affect the frame being untangled. */
    if (sub_x && sub_y)
        dmd_bw_put_bits(bw, 1, 2);

    /* **1** (not the value of the source stream, but we can only state once:
     * The head of the sequence is sent to the KEY frame once, and "Is the frame U/ V's delta different" is frame-by-frame.
     * Declaration 1 After each frame is expressed as a true expression: when V in source inherits U
     * Write 0 (the decoder that takes V delta into U, which corresponds to the source inference),
     * V writes 1 when different from U (rav1e usually) and takes two deltas with V.
     * Declares that V delta will be thrown out -- actual rav1e samples luma by byte,
     * 50% color size bytes wrong, that field was eaten. */
    dmd_bw_put_flag(bw, 1);                        /* separate_uv_delta_q */
}

/* Tile_info(), AV1 Regulator 5.9.15.
 *
 * ⁇  tile_size_bytes_minus_1 by max(tile_len) dynamic (written dead, see below),
 *    Step 4
 * Tile_group must write the same width
 * The decoder reads the length of tile in error width, all mispositions from the second tile. */
/* Tile_info(), AV1 Regulator 5.9.15.
 *
 * ⁇  The boundary value of this function was missed twice, and eventually line-by-line proofing based on the ffmpeg CBS achievement
 * (tile_info(c) of libavcodec/cbs_av1_syntax_template.c) two pits:
 *
 *   1) The shift is sb_size = sb_shift + 2, **not **sb_shift. CBS Original:
 *        sb_size = sb_shift + 2;
 *        max_tile_width_sb = AV1_MAX_TILE_WIDTH >> sb_size;
 *      Using sb_shift makes max_tile_width_sb four times too large, affecting min_log2_tile_cols.
 *      Small, one-yard starting point is wrong.
 *
 *   2) Min_log2_tiles to take max from min_log2_tile_cols. CBS original text:
 *        min_log2_tiles = FFMAX(min_log2_tile_cols,
 *                    cbs_av1_tile_log2(max_tile_area_sb, sb_rows*sb_cols));
 *      The omission will leave the min_log2_tile_rows small enough to write several more 1 in the direction of the line.
 *
 * Code rules for increment(v, min, max) (cbs_av1_write_increment):
 *   If v == max, write (max-min) one bits with no stop bit.
 *   Otherwise, write (v-min) one bits followed by a zero.
 *
 * ⁇ tile_size_bytes_minus_1 Dynamic calculation, step 4
 * The tile_size_minus_1 must be the same width -- two inconsistencies will be completely misplaced from the second tile. */
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

    /* VA-API gives tile_cols/tile_rows(number), then reverses log2. */
    uint32_t cols_log2 = 0;
    while ((1u << cols_log2) < p->tile_cols)
        cols_log2++;
    uint32_t rows_log2 = 0;
    while ((1u << rows_log2) < p->tile_rows)
        rows_log2++;


    dmd_bw_put_flag(bw, (int)p->pic_info_fields.bits.uniform_tile_spacing_flag);

    if (p->pic_info_fields.bits.uniform_tile_spacing_flag) {
        /* increment(tile_cols_log2, min_log2_tile_cols, max_log2_tile_cols)
         * First, we'll get to the legal area: CBS's side of writing direct denial of cross-border reporting. */
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
        /* Uneven: write width_insbs_minus_1 / head_insbs_minus_1,
         * Encoding with ns(n). The upper limit is the smaller "suspendable sb" with max_tile_*sb. */
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

    /* TileCols*TileRows > 1 writes context_update_tile_id and tile_size_bytes.
     * Bit wide** log2 values for the modified ** after the clip above can no longer be recalculated by tile_cols. */
    if (cols_log2 + rows_log2 > 0) {
        dmd_bw_put_bits(bw, p->context_update_tile_id,
                        (int)(cols_log2 + rows_log2));
        /* tile_size_bytes_minus_1: ** actual length of tile** ** cannot be written dead.
         *
         * ⁇  wrote death by 1 (2 bytes) on the grounds that "VA-API gave tile offset just as well.
         * The inference is wrong -- track_headers here are 0
         * (i. e. 1 byte). Writing dead 2 byte will give each tile a gap of 1 byte,
         * 7 gaps with 6-7 bytes, which are shifted from this overall,
         * The decoder then reads all the fields in a misallocation (expressed as Zero_bit out of range).
         *
         * Correct approach: take the minimum byte number required for max(tile_len).
         * The same width must be used for the time of the tile_size_minus_1, so the caller is counted and then passed on. */
        dmd_bw_put_bits(bw, (unsigned)(tile_size_bytes - 1), 2);
    }
}

/* Quantification_params(), AV1 Regulation 5.9.12. */
static void put_quantization_params(struct dmd_bitwriter *bw,
                                    const VADecPictureParameterBufferAV1 *p)
{
    const uint32_t mono = p->seq_info_fields.fields.mono_chrome;

    dmd_bw_put_bits(bw, p->base_qindex, 8);

    /* delta_q: 1 bit delta_coded sign + 7 bit** binary supplement** has a symbol value.
     * Su(n)="n-1 bit value +1 bit sign" has been used to check the writing here, as is the norm.
     * Original bittology from rav1e samples bat/ rav_720: Real stream is a supplement
     * (y=10/u_dc=12/u_ac=2/v_dc=-9/v_ac=15 by adding to the five sets of values + diff_uv_delta)
     * The table is capable of being hit by the 25th position of the payload, and by the value + sign is not hit at any point.
     * The CBS read_signed is also a purely coding reading that is consistent on both sides. */
    #define PUT_DELTA_Q(v) do {                        \
        if ((v) != 0) { dmd_bw_put_flag(bw, 1);        \
                        dmd_av1_put_su(bw, (v), 7); }  \
        else            dmd_bw_put_flag(bw, 0);        \
    } while (0)

    PUT_DELTA_Q(p->y_dc_delta_q);

    if (!mono) {
        /* Diff_uv_delta: stay close to delta_q_y_dc**, U delta**
         * Before (CBS Quantification_params in original order y_dc  ⁇  diff_uv_delta  ⁇
         * ⁇ dc ⁇ u_ac ⁇  (v_dc, v_ac)  ⁇  wrote it after U, the sequence header
         * This position was not exposed at the time of the separate_uv_delta_q=0; changed to constant1 immediately
         * The overall error of 1 bits, the subsequent field is all messed up.
         * VA-API v_dc/v_ac is CBS ** entry value** (diff_uv_delta=0 in source)
         * It has been deduced as a U), so "V and U are different" just to restore the original source flow value. */
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

/* Regulation_params(), AV1 Reg. 5.9.14. */
static void put_segmentation_params(struct dmd_bitwriter *bw,
                                    const VADecPictureParameterBufferAV1 *p,
                                    int primary_ref_none)
{
    const uint32_t enabled = p->seg_info.segment_info_fields.bits.enabled;
    dmd_bw_put_flag(bw, (int)enabled);
    if (!enabled)
        return;

    /* Update_map/update_data is constant 1, without writing the bitstream when prism_ref_frame is NONE. */
    if (!primary_ref_none) {
        dmd_bw_put_flag(bw, (int)p->seg_info.segment_info_fields.bits.update_map);
        if (p->seg_info.segment_info_fields.bits.update_map)
            dmd_bw_put_flag(bw,
                (int)p->seg_info.segment_info_fields.bits.temporal_update);
        dmd_bw_put_flag(bw, (int)p->seg_info.segment_info_fields.bits.update_data);
    }

    if (primary_ref_none || p->seg_info.segment_info_fields.bits.update_data) {
        /* Para-specific. The field matching SEG_LVL_* feature is enabled,
         * The value is the value. The bit width is the standard table with the symbols.
         * Decision on Security_Feature_Bits/Signed. */
        static const int seg_bits[8]   = { 8, 6, 6, 6, 6, 3, 0, 0 };
        static const int seg_signed[8] = { 1, 1, 1, 1, 1, 0, 0, 0 };
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                const int on = (p->seg_info.feature_mask[i] >> j) & 1;
                dmd_bw_put_flag(bw, on);
                if (!on)
                    continue;
                if (seg_bits[j] == 0)
                    continue;            /* SEG_LVL_RF_FRAME Equivalent feature */
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

/* Loop_filter_params(), AV1 Regulation 5.9.11. */
static void put_loop_filter_params(struct dmd_bitwriter *bw,
                                   const VADecPictureParameterBufferAV1 *p,
                                   int coded_lossless, int allow_intrabc)
{
    /* The userless or allow_intrabc time is left blank (the default is returned directly from the norm). */
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
                /* Write update sign + su(7) by item. This is an update for each non-zero value.
                 * Zero-value writing sign 0 - VA-API does not distinguish between "unupdated" and "updated to 0",
                 * The non-zero is conservatively seen as requiring updating. */
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
 * = = = = = = AV1 composite validity baseline (actual, not modified by memory) = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
 *
 * The TU:
 *   150 TUs, each containing frame numbers [1,5,0,1,2,0,1...] — TU2 contains 5 frames.
 * So any cut in "pre-N" will destroy TU's integrity, and the source stream is actually original bytes.
 * When the frame is cut to 5 frames, dav1d only removes 1 frame. The contrast must be **TU border** cut.
 *
 * Hardware V4L2 baseline (one module per frame):
 *   Previous 2 TU(6 frames) Send 6 to collect 2
 *   Previous 6 TU(9) Send 9 to collect 6
 *   Top 16 TU(17) Send 17, charge 16
 * The decoder has an inherent delay of about 1 frame, the longer the sequence, the closer it is to N-1.
 *
 * dav1d side-side same baseline: 2/6/16 TU separate 2/6/16 frames.
 *
 * The "Send 6" for the baseline -- that's the result of six TUs of nine frames, and the result of the "Send 6" for the base line.
 *    The mass of the synthesis is not of the same magnitude as the "Fix Frame", and the resulting synthetic quality will lead to the wrong conclusion.
 *
 * - The only known defect at present: 1 byte less between frames (substantially narrowed)
 *
 * Latest actual measurements (6 frame samples, all check according to "Independent stream + trade_headers field series diff"):
 *   Frame 2 Source 2683B. Synthetic 2683B.
 *   Frame 3 Source 644B Synthesis 643B Less 1
 *   Frame 4 Source 240B Synthesis 240B is identical
 *   Frame 5 source 178B synthetic 178B identically
 *   Frame 6 Source 83B Synthesis 82B Less 1
 * That is, the 6 frames 4 are by bytes correct, with only 3 frames and 6 frames 1 bytes less each.
 *
 * Field sequence matching results for frame 3 (f3s.obu vs f3y.obu, each with KEY frames + the frame):
 *   267 fields** all the same**, the only difference being obu_size 644 vs 643.
 * Byte Level: Offset 19 with 000 00 0a, synthesized with 00 00 0a - one more 0x00,
 * The following elements are identical (a staggered bytes).
 *
 * Layout accounting (frame 3): 8 tile data 619B + 7 tile_size (1B) = 626B
 *   Source obu_size 644 + tg header = 18B
 *   Synthetic obu_size 643 + tg header = 17B
 *
 * Excluded candidates:
 *   · Tile data is identical to the tile_size sequence - the same on both sides ([1,11,11,11,11,10,236,91] +248B)
 *   . tile_size_bytes width -- 1 byte on both sides, dynamic calculations correct
 *   . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .
 *     Tile_start_and_end_present_flag did write, byte_align
 *   • byte_align does not replace when aligned - code correct (code 5.3.5 for clean completion)
 *
 * The 1 byte is therefore not available inside the unpressed_header and** does not change the field sequence**.
 *
 * Three more have been excluded from this round:
 *   • Field wide - 267 item-by-item Length($3), **n difference**
 *   . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .
 *   . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .
 *     Neither side, Trace. The two newspapers are identical.
 *
 * Precision (injection is more reliable than any extrapolation):
 *   Inserts a 0x00 anywhere in the synthetic frame offset 17/18/19, with the result identical to the source** by bytes**.
 *   Source [10:26] = 00000a179f000000a89774458bb
 *   Synthetic [10:26] = 00000a1799f000000a89774458bb44
 *   This is the source here at 000000 (three zeroes) and the synthesis at 0000 (two zeros).
 * Inverse frame length: Source 17B (136 bit), synthetic 16B (128 bit), minus 8 bit.
 *
 * Note that AV1 ** does not use the ** EBSP translator (which is the mechanism of H.264/HEVC)
 * So the zero is not a anti-competitive by-by-blank, but the true content of a given field.
 * By this deflection, the suspect is located in the area of delta_q/ distribution.
 *
 * Byte pair (substantiated, more reliable than any deviation):
 *   Source      [10:26] = 00 00 0a 17 9f 00 03 00 | 00 00 0a 89 77 44 58 bb
 *   Synthesized [10:26] = 00 00 0a 17 9f 00 03    | 00 00 0a 89 77 44 58 bb
 * Known tile layer is identical (first Tile_size=0x00 for size=1, followed by 1 byte)
 * The tile data 0x00, followed by 0x0a starting with the second item.
 *   Source Frame = [0.17], last byte [17] = 0x00
 *   Synthetic Frame = [0.16], last bytes [16] = 0x03
 * and **[0.16] Seventeen bytes are identical**.
 *
 * * Conclusion: Each one is right, but one to eight bits is missing at the end of the frame**
 *   This leads to a less than one byte_align byte. The field you are looking for meets three conditions:
 *     (a) Value 0 (b) length 1-8 bit (c) at the end of uncompressed_header
 *
 * The correct branch has been verified and confirmed on a line-by-line basis in the current round (no further need to look):
 *   * put_tile_info full paragraph with context_update_tile_id and tile_size_bytes_minus_1
 *   * Put_quantisation_params — separate_uv_delta_q=0 (actual),
 *     Do not write diff_uv_delta, U/V share a set of delta, code processed correctly
 *   • delta_q_params / delta_lf_params (1104-1120) — consistent with normative regulation 5.9.17/5.9.18,
 *     Actual Delta_q_present=1, delta_q_res=0
 *   • tx_mode (inclement code), reference_select, reduced_tx_set_used
 *
 * - The conclusion of the post supplemental experiment (DMD_AV1_PAD_BITS, decisive) -
 *
 * Add N byte_align to the frame-to-source ratio (6 frames sample):
 *       pad=0  pad=1  pad=2  pad=3
 *   Frame 2...
 *   Frame 3 . . . . . . . . . . . . . . . .
 *   Frame 4   ✓      ✓      ✓      ✓
 *   Frame 5   ✓      ✓      ✓      ✓
 *   Frame 6...
 * ( ⁇  Same as source** by bytes**)
 *
 * Three points confirm:
 * Frame 3 is correct by by bytes if you fill 1 bit -- last round "Lite 1-8 bytes at the end of the frame."
 *    The assumption is that the number of digits is not sensitive because the byte_align will absorb the extra zeros.
 * 2. Frames 4, 5 are correct at any Pad value, which indicates that they are in fact located on the byte boundary,
 *    Nothing is going to matter -- so this is not a frame-specific, but ** all frames should be filled**.
 * Frame 2, frame 6 is not a rescue: they are poor** non-zero ** and they are another kind of defect.
 *
 * - Residue defects of frame 2/ frame 6 (one byte each, all remaining)
 *   Frame 2 (2683B) Common prefix 18B, only [18]:
 *       Source 0x70 = 01110000
 *       XOR = 0x18 - Two different modes in the adjacent (source 11 0/ synthetic 101)
 *   Frame 6 (83B) Common prefix 2B, only [2] Different:
 *       Source 0x00 = 000000000
 *       XOR = 0x04 - Synthetic** 1 bit more
 * The opposite direction (frame 2 and more) is not the same constant deviation.
 * Frame 6 offset 2 landed along the recfresh_frame_flags (the field was fixed in this session);
 * Frame 2 is the first interframe (order_hint=16), deflecting 18 close to the end of the frame.
 *
 * Note: Frame 2 is the first interframe on the chain, and it is all gone by mistake.
 * This explains why the replacement of three frames is correct, but the number of hardware decoding frames is still 1.
 * The order of restoration should be frame 2  ⁇  frame 6 and not the number of frames to be pursued first.
 *
 * - Frame 2 defect is located: skip_mode_present (new DMD_AV1_BITS switch detected)
 *
 * DMD_AV1_BITS=1 Print frame start-up for the sections, frame 2 (first interframe)
 *   tile_info@55 quant@66 seg@78 lf@83 cdef@111 lr@139
 *   txmode@145 refsel@146 skipmode@147 warp@148 redtx@149 header_end@157
 * And the byte ratio gives the difference in the range **147** -- that is, skip_mode_present.
 *
 * VA-API extraction values vs flow from each frame are actually written (inverse source bytes by position above):
 *   Frame 2 VA-API skip_mode_present=0 Source=1 ⁇  Unconforms
 *   Frame 3 VA-API 1 Source = 1
 *   Frame 4 VA-API 1 Source = 1
 *   Frame 5 VA-API 1 Source = 1
 *   Frame 6 VA-API 1 Source = 1
 * The source stream is 1 for all 5 inter frames, except for the VA-API value of 2 frames which does not match the source.
 *
 * Namely: the mode_control_fields. bits.skip_mode_present of VA-API
 * ** Not directly retransmitted**. The skipModeAllowed of 5.9.22 compares the frame of reference to the frame of reference
 * (The two most recent references need to be identified forward/backward), the conclusions given by VA-API do not match libaom in this frame.
 * - Test findings: skip_mode_present and allow_warped_motion**
 *
 * Add a combination of DMD_AV1_SKIP/ DSD_AV1_WarP overScopeless Switches, matching the source-to-source:
 *   skip=1 warp=0: frame 2 ✓, frame 3 ✓, frame 4 ✓, frame 5 ✓, frame 6 ✗
 *   skip=1 warp=-: frame 2 ✗, frame 3 ✓, frame 4 ✓, frame 5 ✓, frame 6 ✗
 *   skip=-warp=0 = Frame 2 = Frame 3 = Frame 4 = Frame 5 = Frame 6
 * ( ⁇  exactly the same as source bytes;-= original VA-API)
 * One is not enough, two must be changed simultaneously, and frame 2 is correct by by bytes.
 *
 * Predictable (frame 2 shifting 18,144.152 bit):
 *   Source 1 in 147. Synthetic 0skip_mode_present
 *   148 bits source 0 composite1 allow_warped_motion
 * The total number of places on both sides is the same (both to header_end@157), so** not to move**, but to both values.
 *
 * The situation of the frame of reference on the side of VA-API (DMD_AV1_BITS) explains the skip_mode_present:
 *   Frame 2 oh = 16 idx = [2,2,2,2,2,2] ref_oh = [0,0,0,0,0,0,0,0,0,0,0,0]
 * All seven references point to the same slot, only one reference available -- by standard 5.9.22
 * The skipModeAllowed needs to go forward and back two different references, so the VA-API 0 is "justifiably."
 * But libaom code is written in 1. So this is not a copy of VA-API conclusions.
 *
 * ⁇  Confirmed**is not** field read error: va_dec_av1.h: 545-576
 * The skip_mode_present is a separate field in the mod_control_fields domain statement.
 * Read correctly (suspected and excluded).
 *
 * To be resolved: What are the correct values for these two fields? Only the current flow is known.
 * All 5 inter frames are source values skip=1, while the warp is judged by frame
 * (Framework 2 Source 0). Not write the death constant -- that's just a bar flow.
 *
 * The next step is either to deduce the skipMode Alllowed by standard or to find a correct interpretation of the value of VA-API.
 *
 * Side correction: 1154 line reference_select
 * The prefix for skipModeAllowed includes reference_select), which was characterized as a suspect by error in the previous round.
 *
 * - Frame 6 defect is located: the last frame refresh_frame_flags never rewrited -
 *
 * The first 21 landed in refresh_frame_flags (actually @19, 19.26 bit).
 * The current is a frame-by-frame comparison between the falling stream and the source stream:
 *   Frame 2 0x01 Synthetic 0x01
 *   Frame 4: source 0x20, synthesized 0x20 ✓. Frame 5: source 0x40, synthesized 0x40 ✓.
 *   Frame 6: source 0x00, synthesized 0x20 ✗.
 * The first four frames are perfectly correct -- "Delaying a frame-on-the-spot" mechanism itself works properly.
 * (DMD_AV1_LOG results in refresh=0x01/0x08/0x20/0x40).
 *
 * Frame 6 is the last frame in the sample**, without the "Next frame" triggering the rewrite,
 * The place value of the `1 < < slot ' is 0x20, while the frame value of the source stream is 0x00.
 * This is the sister defect of the "silence frame never sent" that this session has fixed:
 * The missing item was the data sent and the missing item was the numerical rewrite.
 *
 * The law is modified by the fact that when the frush is stored in the sync_search_locked, except for the sending of data, the data is not available.
 * And give it a refresh value. The last frame is comparable, but available.
 * "No frame will ever quote from this frame." Launch 0 --and this is a universal rule.
 * Or is it just a local stream (the last frame value of a stream cannot be taken as a constant).
 *
 * Note that the refresh_mask print from DMD_AV1_BITS is ** placeholder value before rewrite**
 * (simple rotation of 0x010x02,0x04,0x08,0x10) is not the value that is ultimately written into the stream.
 * The correctness of the refresh must be judged by looking at the stream (DMD_AV1_DUMP) and not at this probe.
 *
 * - Frame 6 Original Record: Bit 21 -
 * Frame 6 Differences are offset 2 (bit 16.23), synthesized in 21 overwritten 1 bit.
 * This position is the opening paragraph of uncompressed_header before tile_info (@55) --
 * The #saves #saves #saves #saves #saves #saves #saves #saves #saves #saves #saves #saves #saves #saves #s
 * Not related to kip_mode_present, separate positioning is required.
 *
 * Remaining suspicion (by location):
 *   • skip_mode_present**: code used `reference_select` when door (1154 lines),
 *     And the skipModeAllowed descriptor 5.9.22 does not contain this.
 *     The framework is still written, but the conditions themselves need to be reviewed in accordance with the norms.
 *   . that is_motion_mode_switchable
 *   • Whether there are any standard requirements after which there are completely unwritten fields
 *
 * ⁇  (Trace_headers output is at ** accumulated position within stream**, and then the result is a change in the system.
 * Not internal; and there is no obu_forbidden_bit line in the output**,
 * So "Stick the N OBU by obu_forbidden_bit" is invalid...
 * The groups of "the same side" made on this basis are all scrapped.
 * There are only two reliable findings: bytes of insertion and field sequences of single-frame independent streams.
 *
 * The current cycle is another time of "cross-frames versus right": the flow of multi-frames will take the frames.
 * The field was mixed up for output, and I came up with a "168" for tile_start_and_end_present_flag."
 * The line actually came from another frame (the same position in this frame is cdef_y_pri_stream[5]).
 * The lessons reiterate that the framework to be measured must be isolated into an independent stream and then trace.
 *

 * Take frame 3 (order_hint=4) for example, "read the length of tile according to its stated width, see if the balance is reasonable."
 * _Other Organiser
 *   Source stream: 18-byte frame header, tiles=[1,11,11,11,10,236,91], 248 bytes remaining.
 *   Synthesized: 17-byte frame header, tiles=[1,11,11,11,10,236,91], 248 bytes remaining.
 * Tile data are bytes equal, style length sequences are identical (and max=236 from VA-API)
 * The difference is only 1 byte less than the frame itself.
 * Specifically which field is missing/ several are not yet located - the next field-by-field check box 3
 * Unpressed_header, not re-dependent on the conversion of bits.
 *
 * ⁇ tile_size_bytes_minus1 is ** frame-by-frame **, not flow-level constant:
 *   Source stream first frame = 1 (2 bytes, max_tile=4123)
 *   Source flow frame 3 = 0 (1 byte, max_tile=236)
 * The first frame value was considered a flat-rate value, which resulted in 2 bytes of death, resulting in a gap of each tile
 * 1 byte. The measurements are consistent with the frame-by-frame of the source stream, based on the dynamic of max(tile_len)-1
 * （4123→2, 747→2, 236→1, 79→1, 38→1, 19→1）。
 *
 * - Pyramid B frame structure: most frames show_frame=0, no output produced -
 * VA-API provides show_frame (va_dec_av1.h:425), but not show_existing_frame.
 * Pre-detection 6 frames sent by ffmpeg:
 *   ft=0 show=1 oh=0 ⁇ KEY, display
 *   ft=1 show=0 oh=16  ⁇  Reference frame, not displayed
 *   ft=1 show=0 oh=8
 *   ft=1 show=0 oh=4
 *   ft=1 show=0 oh=2
 *   ft=1 show=1 oh=1 display
 * Only the first and last frames show_frame=1. The source stream track_headers' show_frame sequence
 * (1  ⁇ 0,0,0,0,1) Exactly consistent with VA-API, show_existing_frame0,
 * And five OBU_FRAMEs in TU2 are all full frames -- so the synthetic structure itself is correct.
 *
 * The result is to overturn the conclusion that "synthetic currents are less than source flow" and then to reverse it.
 *   Source Flow 1+Fram 2  ⁇  dav1d 1 Frame 2 Hardware Delivery 1
 *   Synthetic stream 1+frame 2  ⁇  dav1d 1 frame, hardware 2 sent 1
 * Exactly the same. The show_frame=0 of frame 2 does not produce output.
 * Take "Send N take N-1" to ask it to come out of frame is wrong.
 *
 * ⁇ trace_headers is not a multi-framework decision: it also interprets the source stream only by parsing the first frame.
 *    (-c copyy under bsf encounters follow-up OBU stops), field sequence of actual source and synthesis
 *    The tail is identical. Multi-frame correctness is judged by the number of frames that dav1d untangles.
 *
 * - - Each of the two TU clusters triggers a different reporting error (actually measured, used to distinguish the source of the symptoms) -
 * Feed the five frames of the synthesis to dav1d in different ways:
 *   One TU per frame (drilling original output)
 *     → "zero_bit out of range: 1, but must be in [0,0]"
 *   Grouping by source stream structure (TU1 = 1 frame, TU2 = 4 remaining frame)
 *     → "Invalid repeated frame header OBU"
 * Both are located in frame 2. The latter states that the presence of multiple complete frames in one TU is illegal.
 * The source stream can do this because most of the frames show_existing_frame or are not shown.
 * So the zero_bit is not a stand-alone defect, but a by-product of the grouping approach...
 * The chase will run out of time (this round will be the one to check the CEDF area).
 *
 * In addition: The first difference between the synthetic and the source stream is constant in frame load offset 18 (by 144, CDEF area).
 * Source 0x70/ Synthesis 0x68. Revert the byte back to 0x70 is invalid (dav1d still 1 frame),
 * It is demonstrated that it is the result of a different field take-off from the upstream area ** which results in an overall deviation of the flow** rather than the cause.
 */

/* cdef_params(), AV1 Reg. 5.9.19. */
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
        /* VA-API packs Y/UV strength as (pri << 2) | sec.
         * The layout of the four-bit pr + 2-bit sec in the stream is identical and can be broken directly. */
        dmd_bw_put_bits(bw, p->cdef_y_strengths[i] >> 2, 4);
        dmd_bw_put_bits(bw, p->cdef_y_strengths[i] & 0x3, 2);
        if (!p->seq_info_fields.fields.mono_chrome) {
            dmd_bw_put_bits(bw, p->cdef_uv_strengths[i] >> 2, 4);
            dmd_bw_put_bits(bw, p->cdef_uv_strengths[i] & 0x3, 2);
        }
    }
}

/* lr_params(), AV1 Regulator 5.9.20. */
static void put_lr_params(struct dmd_bitwriter *bw,
                          const VADecPictureParameterBufferAV1 *p,
                          int all_lossless, int allow_intrabc)
{
    if (all_lossless || allow_intrabc)
        return;

    const uint32_t ry = p->loop_restoration_fields.bits.yframe_restoration_type;
    const uint32_t rcb = p->loop_restoration_fields.bits.cbframe_restoration_type;
    const uint32_t rcr = p->loop_restoration_fields.bits.crframe_restoration_type;

    /* Sequence header persistently writes 1, so this paragraph** always writes**--
     * Even three lr_types are all zero.
     * Earlier, the "All Zeros" was used to return early, which did not match the head of the sequence, resulting in a short frame of six bits. */

    /* lr_type with f(2), value order is the same as VA-API list of reference_type
     * （0=NONE, 1=WIENER, 2=SGRPROJ, 3=SWITCHABLE）。 */
    if (getenv("DMD_AV1_BITS"))
        fprintf(stderr, "[lr] oh=%u ry=%u rcb=%u rcr=%u shift=%u uv=%u\n",
                p->order_hint, ry, rcb, rcr,
                p->loop_restoration_fields.bits.lr_unit_shift,
                p->loop_restoration_fields.bits.lr_uv_shift);
    /* ⁇ lr_type coding of the stream with the VA-API emits** is not the same set of extracts and must be mapd.
     *
     * Standard 5.9.20 Use Remap_Lr_Type[4] = NONE, SWITCHABLE, WIENER, SCRPROJ}
     * (b) Map f(2) in the stream as FrameRestorationType;
     * VA-API directs to FrameRestorationType
     * （0=NONE, 1=WIENER, 2=SGRPROJ, 3=SWITCHABLE）。
     * Write stream to map backwards **
     *   NONE(0)→0   WIENER(1)→2   SGRPROJ(2)→3   SWITCHABLE(3)→1
     *
     * The evidence is measured (frame 26, oh = 28): VA-API to ry=1 (WIENER),
     * Source stream 2 - 1 direct transliteration, 115/116.
     * The earlier "put_lr_params have verified" conclusions were from a six-frame sample, which was found to be a very good one.
     * The frames just happened to be ry=rcb=rcr=0, and the map was all 0, covering up the flaw. */
    static const unsigned char lr_to_bits[4] = { 0, 2, 3, 1 };
    dmd_bw_put_bits(bw, lr_to_bits[ry & 3], 2);
    if (!p->seq_info_fields.fields.mono_chrome) {
        dmd_bw_put_bits(bw, lr_to_bits[rcb & 3], 2);
        dmd_bw_put_bits(bw, lr_to_bits[rcr & 3], 2);
    }

    if (ry || rcb || rcr) {
        /* VA-API lr_unit_shift is the total shift (0..2),
         * The stream is divided into two 1-bit fields (regulation 5.9.20):
         *   Use_128x128_subperblock=1 lr_unit_ship
         *   Otherwise: lr_unit_ship (1) + if 1 is written lr_unit_extra_ship (1)
         *         Total Offset = lr_unit_ship + lr_unit_extra_ship
         * So the shift=2 is to write as one and write again as one.
         *
         * The earlier code says `lr_unit_ship ` low 1 bit, exttra constant 0:
         * Shift=2 written as 0 without extra -- realistic frame 26 (Shift=2)
         * The difference is 121/122 here.
         * 6 in the frame sample ry=rcb=rcr=0, the whole if block was skipped and the defects were masked. */
        const unsigned sh = p->loop_restoration_fields.bits.lr_unit_shift;
        if (p->seq_info_fields.fields.use_128x128_superblock) {
            /* ⁇  sbs128=1 time current is ** Effective Offset 1** (Regular 5.9.20:
             * "When use_128x128_superblock is equal to 1, lr_unit_shift is
             * Set effective to 1 + lr_unit_ship"), only valid values of 1 or 2.
             * VA-API gives a valid value, so it must be written sh-1, not sh non-zero.
             *
             * (tesrc composite stream av1_probe.mp4, frame oh=16, lr_type[2]=SCRPROJ,
             * Source stream 0  ⁇  valid shift 1: old writing 0 to 1  ⁇  valid shift 2  ⁇
             * Retention unit from 64 to 128  ⁇  ** Every single single single piece of the stream is wrong**
             * (framemd 5,128/128 does not match at all, and the browser is full of screens.
             * The reason B Station 1800 is really a byte-by-the-temperate frame is its framework of response
             * And it's all just a valid shift -- it's a flaw that's covered up. */
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

    /* Payload writes a temporary buffer: obu_size with a leb128 encoding, you must know about payload
     * The length is enough to write the head. The sequence head is small (live 20 bytes up and down) and there's enough on the pad. */
    unsigned char body[128];
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, body, sizeof(body));

    const uint32_t enable_order_hint =
        p->seq_info_fields.fields.enable_order_hint;

    /* - procedure_header_obu(), AV1 Regulation 5.5.1 - */

    dmd_bw_put_bits(&bw, p->profile, 3);                    /* seq_profile   */
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.still_picture);
    dmd_bw_put_flag(&bw, 0);   /* reduced_still_picture_header
                                * Hen-0: set 1 will cut most of the back fields off and frame heads
                                * Parsing the power bits that rely on it. */

    dmd_bw_put_flag(&bw, 0);   /* timing_info_present_flag
                                * VA-API does not provide text_info. Time sequence does not affect decoding,
                                * Shows the rhythm is controlled by the upper layers. */
    dmd_bw_put_flag(&bw, 0);   /* initial_display_delay_present_flag */
    dmd_bw_put_bits(&bw, 0, 5);/* operating_points_cnt_minus_1
                                * Single operation point: This driver does not make scalable layers,
                                * Consistency with OBU head extension_flag constant 0. */
    dmd_bw_put_bits(&bw, 0, 12);          /* operating_point_idc[0] */
    /* seq_level_idx[0] = 8（level 4.0）+ seq_tier[0] = 0。
     *
     * ⁇  wrote 0 (level 2.0) which does not match the real stream: libaom for 1080p output 8.
     * The key is not just the number — seq_vel_idx > 7 ** must follow seq_tier**(f(1)),
     * Writing 0 will be less than this, leading to a 1-bit shift in all fields following the sequence header.
     * VA-API does not provide level, pick 8 with real stream; MediaCodec by actual resolution
     * Allocate resources, without this field verification. */
    dmd_bw_put_bits(&bw, 8, 5);           /* seq_level_idx[0] */
    dmd_bw_put_flag(&bw, 0);              /* seq_tier [0] (because idx > 7) */

    /* frame_width_bits takes the required bit wide without writing to death 16 bits.
     * frame_width_minus1 press v_dec_av1.h:332-334 is ** the resolution after sampling**,
     * It's the semantic meaning of max_frame_width_minus_1. */
    const uint32_t w_m1  = p->frame_width_minus1;
    const uint32_t h_m1  = p->frame_height_minus1;
    const int      wbits = bits_for(w_m1);
    const int      hbits = bits_for(h_m1);

    dmd_bw_put_bits(&bw, (uint32_t)(wbits - 1), 4);
    dmd_bw_put_bits(&bw, (uint32_t)(hbits - 1), 4);
    dmd_bw_put_bits(&bw, w_m1, wbits);
    dmd_bw_put_bits(&bw, h_m1, hbits);

    dmd_bw_put_flag(&bw, 0);   /* frame_id_numbers_present_flag
                                * Set 0: VA-API does not provide delta_frame_id, frame
                                * The corresponding field is not written -- the two places must be identical. */

    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.use_128x128_superblock);
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_filter_intra);
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_intra_edge_filter);
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_interintra_compound);
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_masked_compound);

    /* ⁇ enable_warped_motion sequence field VA-API** not available (full file grep)
     * Take one: a security side: it's a capability level, allow_warped_motion (:439)
     * The frame is used. If you have 0 here and the frame is 1 and the decoder rejects the frame;
     * Set 1 with 0 without any side effects. */
    dmd_bw_put_flag(&bw, 1);

    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_dual_filter);
    dmd_bw_put_flag(&bw, (int)enable_order_hint);
    if (enable_order_hint) {
        dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_jnt_comp);
        dmd_bw_put_flag(&bw, 1);   /* The blog is also available on the Internet, and the Internet is available on the Internet.
                                    * Same security side 1 (frame level)
                                    * In: 435. */
    }

    /* The next time you're in the game, you're gonna have to be a good girl.
     * SelECT_Streen_CONTEN_TOOLS ** does not occupy a bitstream** semantic is "each frame is in the frame"
     * Declare yourself." This is what it takes: VA-API only gives frame levels
     * (429) A fixed value is assumed at the sequence level that may conflict with the frame.
     * Seq_choose_integer_mv (framework level force_integer_mv in:430). */
    dmd_bw_put_flag(&bw, 1);       /* seq_choose_screen_content_tools */
    dmd_bw_put_flag(&bw, 1);       /* seq_choose_integer_mv           */

    if (enable_order_hint)
        dmd_bw_put_bits(&bw, p->order_hint_bits_minus_1, 3);

    /* Eenable_suppreres: VA-API does not provide sequence field, using frame_subpres to reverse.
     * Eenable_restoration: VA-API also does not provide, constant writing 1 and lr_params
     * It says lr_type=0 -- self-serving and semantic equivalent to "no response".
     * Cost: Frames 6 bits more than source stream of Eenable_restoration=0, but 6 bits
     * In our own bitstream, the tile_group is self-configured and does not affect the correct decoding.
     * ⁇  Real use of the log flow (lr_type ⁇ 0) is not supported —
     * VA-API does not provide LR parameters at all, and pixels are missing a filtering link. */
    dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.use_superres);
    dmd_bw_put_flag(&bw, (int)p->seq_info_fields.fields.enable_cdef);
    dmd_bw_put_flag(&bw, 1);

    put_color_config(&bw, p);

    dmd_bw_put_flag(&bw,
        (int)p->seq_info_fields.fields.film_grain_params_present);

    dmd_av1_trailing_bits(&bw);

    if (bw.overflow)
        return 0;

    /* assembly: OBU head (including the payload length of the leb128) + payload. */
    const size_t body_len = dmd_bw_bytes(&bw);
    const size_t hdr = dmd_av1_obu_header(DMD_OBU_SEQUENCE_HEADER,
                                         body_len, out, out_cap);
    if (hdr == 0 || hdr + body_len > out_cap)
        return 0;
    for (size_t i = 0; i < body_len; i++)
        out[hdr + i] = body[i];
    return hdr + body_len;
}

/* -Surface-> Frame Number's "Recent Owner" Table--
 *
 * The ffmpeg surface pool will recover the surface id: once a frame is displayed and no longer quoted, the ffmpeg will be used to restore the surface id.
 * Its surface will be removed by a new decoding frame (live 160 frames of the sample, oh1 and oh7
 * So, it's not a frame identity; but "recently owned."
 * The frame of the surface is always the only one alive. All the identities from the surface are always the ones that are left.
 * It's all through here. */
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
        /* Table full: Cover the first item (the oldest relationship is the last to be searched again). */
        for (int i = 1; i < dpb->surf_hist_n; i++)
            dpb->surf_hist[i - 1] = dpb->surf_hist[i];
        dpb->surf_hist[dpb->surf_hist_n - 1].surf = surf;
        dpb->surf_hist[dpb->surf_hist_n - 1].frame = frame;
    }
}

/* - - - - - - - - - - - - - - - - - - - - - - Global campaigns (5.9.24) */

/* The bit-wide constant (libavcodec/av1.h) which is regulated/referenced. */
#define DMD_GM_ABS_ALPHA_BITS      12   /* Alpha (idx 2.5 and slashed entries of various types)*/
#define DMD_GM_ALPHA_PREC_BITS     15
#define DMD_GM_ABS_TRANS_ONLY_BITS  9   /* idx 0.1 for TRAINSLATION only*/
#define DMD_GM_TRANS_ONLY_PREC_BITS 3
#define DMD_GM_ABS_TRANS_BITS      12   /* idx 0/1 of ROTZOOM/ AFFENE */
#define DMD_GM_TRANS_PREC_BITS      6
#define DMD_WARPEDMODEL_PREC_BITS  16

enum dmd_gm_type { GM_IDENTITY = 0, GM_TRANSLATION = 1,
                   GM_ROTZOOM = 2, GM_AFFINE = 3 };

/* v_log2: floor (log2(v)), v=0 is 0. */
static int dmd_gm_ilog2(uint32_t v)
{
    int n = 0;
    while (v > 1) { v >>= 1; n++; }
    return n;
}

/* Arithmetic right shift, equivalent to floor(v / 2^n). The decoder uses
 * `prev > > prec_diff ' , negative is **down-diff**; integer C is divided by zero,
 * The direct difference between the square and the coder is not consistent. */
static int32_t dmd_gm_fdiv(int32_t v, int32_t d)
{
    int32_t q = v / d;
    if (v % d && ((v < 0) != (d < 0))) q--;
    return q;
}

/* Increment(, regulation 4.10.5): Write v, range value [0, max].
 * The string is "v of 1", and an additional 0 is added to the end of v+1 bits when v<max;
 * v = 0 (total max bit) when max is not added - the top value does not terminate, read side by count to max bit.
 * ⁇  This used to be "v+1 1 plus 0", so every subext symbol has one more 1:
 *   The actual source symbol 118 is read 237 (= 2v+1) and each symbol is scattered and re-set
 *   Alignment (the termination is still in place), so only the value of gm is all wrong and the rest of the frame is as usual. */
static void dmd_gm_put_increment(struct dmd_bitwriter *bw, uint32_t v,
                                 uint32_t max_len)
{
    for (uint32_t i = 0; i < v; i++)
        dmd_bw_put_flag(bw, 1);
    if (v != max_len)
        dmd_bw_put_flag(bw, 0);
}

/* Sub_exp() (regulation 4.10.8), biographically aligned libavcodec/cbs_av1 write_subext. */
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
        if (len > max_len) {          /* Merge top with next, reusing ns() */
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

/* Recentering inverts. Decoding side (regulation 7.11.3.6 / invers_recenter) is
 *   if ((r<<1) <= mx) x = inverse_recenter(r, sub)
 *   else              x = mx-1-inverse_recenter(mx-1-r, sub)
 * Known x with r, counter-reconstruct sub: first click the cross-border r image to the first half (with x)
 * Inversion_recenter's three-point definition is counterproductive. */
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

/* gm_params default value in the norm for "re-reconstruction" (initialization before 7.11.3.6):
 * idx%3=2 < 1<16 (scaling point 1.0), remaining 0. */
static void dmd_gm_default(int32_t p[7][6])
{
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 6; j++)
            p[i][j] = (j % 3 == 2) ? (1 << DMD_WARPEDMODEL_PREC_BITS) : 0;
}

/* Calculates a gm_params symbol (integer value of sub_exp in the stream). Returns 0 successful;
 * -1 indicates that the value given by VA cannot be expressed in this syntax, and the caller must return the reference frame to IDENTITY.
 *
 * Based on: decoded side cur = (decoded < prec_diff > round, therefore)
 * The > prec_diff must be eliminated; r is calculated by the prev method,
 * Must be the ** decoder at the time** prev (see dmd_av1_dpb.gm_slot). */
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

    if (delta % scale) return -1;              /* It's not on the reconstruction. */
    const int32_t d = delta / scale;
    if (d < -mx || d > mx) return -1;

    int32_t r = dmd_gm_fdiv(prev, scale) - sub;
    if (r < -mx) r = -mx;
    if (r >  mx) r =  mx;

    /* Recentering on an unsigned domain: x, r moving mx on a par, mx on a 2*mx+1 value. */
    const uint32_t mxu = (uint32_t)mx * 2u + 1u;
    const uint32_t x  = (uint32_t)(d + mx);
    uint32_t ru = (uint32_t)(r + mx);
    if (ru >= mxu) ru = mxu - 1;
    *sym = dmd_gm_recenter_encode(x, ru, mxu);
    *range_max = mxu;
    return 0;
}

/* Write uncompressed_header() in bw, without endings of traiding_bits/
 * Byte_alignment - The caller determines in sealed form:
 *   OBU_FRAME_HEADER(3) with traiding_bits (regulation 5.9.1)
 *   OBU_FRAME(6) byte_alignment (regulation 5.10.1)
 * The difference is measured by the fact that using error will deflect the tile_group from the beginning.
 * The dav1d newspaper "Failed to read unit". */
/* The slot number for the VA 's first reference flip cost synthesis stream. returns 0 to mean **is unable to resolve**:
 * The consumer gives ref_frame_map to VA_INVALID_ID (the slot does not have a frame), or the frame it points to
 * It has already been evicted from the shadow DPB. slot_out still passes through the source slot as a usable slot number,
 * To keep the caller on the "send" path. */
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
    /* KEY/ INTRA_ONLY/ SWITCH does not read the frame contents (SWITCH full brush), but the text is not available.
     * Allow_intrabc's frame intra (a direct skipping of the frame forecast in the norm) is not read.
     * ⁇  Do not take reference_select as release conditions: Standard 5.9.2 in which it is 0, just
     * "No visible presentation of 7 slots", with frame quotes from LAST (ref_frame_idx[0]). */
    if (ft != 1 /* INTER_FRAME */ || p->pic_info_fields.bits.allow_intrabc)
        return 1;
    /* The slot that will really be taken from the decoder: LAST for long-term use; composite reference/skip_mode for ALTREF
     * (ref_alternate_frame = ref_frame_map [ref_frame_idx[3], regulating 5.9.2).
     * The PPB of VA-API does not have reference_mode, so here
     * And conservatively, count three into "use reference." */
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
    /* Struck dmd_bitwriter is a pure value structure: buff points to the buffer of the caller, the rest of the members
     * It's all counting. So "Cuffing & Write & Cuff Back" is safe and the function is preserved.
     * The `&bw ' formula, aligned with the real stream by field, does not require line-by-line changes to introduce a clerical error. */
    struct dmd_bitwriter bw = *bwp;

    /* - show_frame forced 1 (landfall of Direction A, see long comment for this document)
     *
     * Hardware only for frame vomiting of show_frame=1 CAPTURE buffer, while ffmpeg VA-API backend
     * The frame frame that will be used to use the frame (show_frame=0) for **s**shows
     * The first time these frames were not able to get hardware pixels, they were used again.
     * Any "from other surface pixels" is verified.
     *
     * Reverse: Write show_frame in the synthetic head as 1 to allow hardware to frame each submission frame
     * (OUTPUT_ORDER=1 output in decoding order, in the order of registration with pending),
     * Every surface gets a real pixel ** of its own. SEF repeats ffmpeg
     * Reuse the surface to complete the drive without the need to synthesize any SEF header.
     *
     * Show_frame only controls "Is output" without prejudice to DPB/Semantics
     * (Staff refresher decided by refresh_frame_flags, unrelated to this position).
     * All the syntax branches in the head that rely on it (showable bits, error_resilient inference,
     * The KEY frame refresh inference, the file_grain conditions are present) is used to apply this entry value uniformly, but the value is not the same as the value.
     * The guarantee numbers flow into the system. Shadow (decode.c) still keeps the accounts at the original values.
     *
     * DMD_AV1_NO_SHOWORCE=1 Restoration of old behaviour for A/B comparison. */
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

    /* CodedLossless Decision (Regulation 7.12.1): qindex with four delta_q for all paragraphs
     * All zero. VA-API does not give this amount directly, as defined -- it decides to drop_filter/
     * Cdef/lr Whether the three segments appear in the stream is miscalculated to cause the frame structure to be mispositioned. */
    const int coded_lossless =
        (p->base_qindex == 0 && p->y_dc_delta_q == 0 &&
         p->u_dc_delta_q == 0 && p->u_ac_delta_q == 0 &&
         p->v_dc_delta_q == 0 && p->v_ac_delta_q == 0);
    /* AllLossless also requires no sampling on the superres (regulation 7.121). */
    const int all_lossless =
        coded_lossless && !p->pic_info_fields.bits.use_superres;

    /* -- uncompressed_header(), AV1 Regulation 5.9.2 -- */

    /* Show_existing_frame: This driver forwards the real frame by frame, without using decoded frames, constant 0.
     *
     * - This is the root cause of the difference in the number of pixel frames (this round)
     * OBU composition of the source stream av1_1080p.obu:
     *     TD 150, OBU_FRAME 150, independent FRAME_HEADER 70, SEQ_HDR 5
     *     Only 80 out of 150 of the 150 OBU_FRAME show_frame=1
     *     70 independent Frame_HEADER** all ** is show_existing_frame=1
     *     80 Real display + 70 regress = 150 = number of soft output frames
     *
     * Hardware side: number of units sent into 150, 80 frames received - and show_frame=1
     * Accurate match, perfect hardware behavior, a lot of frames.
     * The missing 70 frame is the SEF frame, which has never been synthesized.
     *
     * But the SEF frame cannot be synthesized here: it's measured that the baginpicture is just 150 times adjusted
     * (= OBU_FRAME), SEF frame** unexacted ** VA-API decoding path
     * (The probe shows 150 calls made all in real frames, ancchor = target).
     * Decoded VA-API no show_existing_frame fields
     * (only va_enc_av1.h mentions it, which is code side).
     * Conclusion: SEF repeats that it was done by ffmpeg on VA-API itself, and that the ffmpeg is not a good idea.
     * Drivers only ensure that the quoted surface contains the correct pixels.
     * So the reason why md5 is not so soft is in the direction of "surface content."
     * Not "Less Synthesis 70 SEF Frames."
     *
     * - The above conclusion is half wrong, and this round is overridden by isolation tests.
     * Feed the source stream to hardware (/tmp/sis3 bypassing the entire drive):
     *     Session API: Send 150 units, take 150 frames
     * Hardware can fill 150 frames! And the synthetic stream can only take 80 frames.
     * The two streams of OBU make up only one difference:
     *     Source stream SEQ_HDR 5 TD 150 FRAME 150 FRAME_HDR 70 (all SEF)
     *     Synthetic stream SEQ_HDR 5 TD 150 FRAME 150 FRAME_HDR 0
     * So "hardware doesn't vomit show_frame=0" is right.
     * But "so you can't get the 70 frames" is wrong -- hardware will be for every SEF head.
     * Once again, if there is that head in the stream. The missing 70 SEF heads.
     *
     * - Whether the driver can export SEF -
     * The driver never receives an SEF (all 150 BeginPicture calls are real frames),
     * But it can be deduced that the frame of the show_frame=0 & & showable=1 will be quoted later by the SEF, which will be used to create a new one.
     * Inserts the time determined by the order_hint (show order). The two fields are measured as ready:
     *     oh=16 show=0 showable=1   (displayed later by SEF)
     *     oh=1  show=1 showable=1   (displayed immediately)
     * Next: Insert SEF FRAME_HEADER in the synthetic stream in the display order.
     *
     * (fame_id_numbers_present=0; only follow-up non-readable frame_id. */
    dmd_bw_put_flag(&bw, 0);

    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "frame_type", bw.byte_pos*8+bw.bit_pos);
    dmd_bw_put_bits(&bw, frame_type, 2);
    dmd_bw_put_flag(&bw, show_frame_eff);
    if (!show_frame_eff)
        dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.showable_frame);

    /* The ERROOR_resilient_mode: KEY_FRAME and show_frame1 non-writing. */
    /* ERROOR_resilient_mode (CBS original):
     *   frame_type == SWITCH || (frame_type == KEY && show_frame)
     *     ~Infer 1 (No writing stream)
     *   Otherwise, the flag (error_resilient_mode) */
    const int er_inferred = (frame_type == 3) ||
                            (is_key && show_frame_eff);
    if (!er_inferred) {
        if (getenv("DMD_AV1_BITS"))
            fprintf(stderr, "[bits] err_res @ %zu\n",
                    bw.byte_pos * 8 + bw.bit_pos);
        dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.error_resilient_mode);
    }

    dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.disable_cdf_update);

    /* Allow_screw_content_tools: seq_choose_screen_content_tools=1
     * * Seq_force = SELECT_SCREEN_CONTENT_TOOLS (2), so write f(1) here. */
    dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.allow_screen_content_tools);
    if (p->pic_info_fields.bits.allow_screen_content_tools) {
        /* See  ⁇  seq_choose_integer_mv=1  ⁇  seq_force_integer_mv = SELECT, writing f(1). */
        dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.force_integer_mv);
    }

    /* The frame_id_numbers_present=0 (sequence header) skips the current_frame_id. */

    /* frame_size_override_flag: persistent 0, which means that frame size is equal to that in the head of the sequence
     * Max_frame_size -- we put max on the cost frame size, which is the same.
     * If the frame outside KEY_FRAME changes size, the driver will recreate the session, so the constant is established. */
    if (frame_type != 3 /* SWITCH_FRAME should be constant1 */)
        dmd_bw_put_flag(&bw, 0);

    /* order_hint (norm 5.9.2): provided that the means_order_hint and** not**
     * "frame_is_intra and refresh_frame_flags == allFrames".
     *
     * The pits were stepped on twice, and finally the real bitstream was solved:
     * 1080p KEY_FRAME (show_frame=1)**the order_hint
     * (value 0, after frame_size_override_flag). So not simplified to
     * "No frame is written" -- that would lower the order_hint_bits bit.
     *
     * Reason: The refresh_frame_flags field of the KEY+show frame does not appear in the bitstream, but the norm
     * 7.20 Says that at this point, RefreshFrame Flags takes all Flames only ** decoder side decoder decomposing values**,
     * The decision in 5.9.2 is directed at the **grammatical variable**. The actual behaviour of libaom indicates that the decision was based on the evidence.
     * Not here, that's the order of the order. */
    if (enable_order_hint) {
        if (getenv("DMD_AV1_BITS"))
            fprintf(stderr, "[bits] order_hint @ %zu\n",
                    bw.byte_pos * 8 + bw.bit_pos);
        dmd_bw_put_bits(&bw, p->order_hint, order_hint_bits);
    }

    /* Primariy_ref_frame: Do not write when frame or error_resilient. */
    /* The err_res is ** Valid Value**: Always Infer Inver Place 1, otherwise VA-API gives.
     * Follow-up Primariy_ref_frame/use_ref_frame_mvs/ allow_warped_motion
     * The condition is to use this entry value, which is wrong in the original field. */
    const int err_res = er_inferred
                      ? 1 : (int)p->pic_info_fields.bits.error_resilient_mode;
    const int primary_ref_none = intra_only || err_res ||
        (p->primary_ref_frame == 7 /* PRIMARY_REF_NONE */);
    if (!intra_only && !err_res)
        dmd_bw_put_bits(&bw, p->primary_ref_frame, 3);

    /* Refresh_frame_flags: KEY_FRAME + show_frame with constant 0xFF, without writing the bitstream.
     *
     * VA-API** does not provide the field** (full file grep confirms: only va_dec_av1.h:421).
     * The notes are mentioned, not in the structure. It's a bit mask for "what slots this frame is to refresh."
     *
     * ⁇  History error (V4L2 exposed): this is the original case of 0xFF, which is the reason why
     * "MediaCodec runs the frame life cycle itself, without relying on this mask in the stream."
     * The hypothesis was created for MediaCodec, v4L2 ** not created ** — msm_vidc by stream
     * Manage the frame, 0xFF means that each frame has been erased from all eight slots and the reference chain immediately collapses.
     * (b) Actual effects: synthetic flow flow dav1d softened, 2nd frame startup Invalid Data, delineating only 1 frame;
     * The hardware is shown as sending 6 units back to 2 frames and then the decoder stops.
     * The field_headers field-by-field matching displays refresh_frame_flags source stream of 1 and synthesizes to 255.
     *
     * Correct approach: From DPB snapshot ref_frame_map[8]. This frame decodes and uses some slots,
     * VA-API gives frame ref_frame_map -- the surface id stored in every slot
     * The same as this frame's crrent_frame, which is the target of the frame to be refreshed.
     *
     * returns 0 when no match slot is found (no slot is refreshed): that's show_existing_frame
     * The normal situation of the frame that is not the reference is much safer than 0xFF - less declaration to refresh,
     * And don't lie about refreshing all the frames that the decoder has dropped. */
    const int refresh_all = (frame_type == 3) ||
                            (is_key && show_frame_eff);

    /* Maintain a consistent shadow DPB for reference frames
     *
     * Question: refresh_frame_flags (8 bit mask for which reference slots to write after this frame decoding)
     * VA-API is not provided at all -- it is discarded by ffmpeg as it is in the source stream
     * GOP decision for encoder. Source flow real-time sequence 1,8,32,64,0,0,0,64,0,0,32,64,0
     * (one third is 0) cannot be restored from single frame information.
     *
     * Key insight:** I don't need to restore it.**
     * Ref_frame_idx[] also writes the stream by this function. The decoder only requires these two.
     * ** **: I said this frame is in k, and then I'll take k when I quote it.
     * As to which slot the original encoder placed in, it has nothing to do with the decoding result.
     *
     * So you're taking care of a shadow yourself.
     *   • Each non-KEY frame occupies one slot, 8 slot rotation (refresh = 1 < slot)
     *   • Recording shadow[lot] = the archive id stored in the slot
     *   • When writing ref_frame_idx, give the "swart" to VA-API by its ref_frame_map
     *     Turn it into a surface id and find my ** slot in the shadow of the DPB.
     *
     * The simulation of the live data has been verified: all references to the 5 consecutive frames can be interpreted in the shadow DPB.
     *
     * Historical error (cause of this round of restoration): original constant 0xFF, on the grounds that "MediaCodec is self-dependant
     * Manages the frame of reference, without looking at this mask in the stream." This hypothesis is for MediaCodec, V4L2.
     * Not created - msm_vidc manages the frame by stream, 0xFF allows each frame to brush out all 8 slots,
     * The reference chain immediately collapses: synthetic flow flow dav1d softly delineate 2nd frame Invalid data.
     *
     * The three rejected "reverted encoder decision" (all failed):
     *  Find the current_frame -- the array is pre-coded** in ref_frame_map[]
     *     The snapshot, the current is not written yet. It's not the same.
     *  The two adjacent frames ref_frame_map difference - given as a new slot,
     *     And cannot express "This frame is not updated" (value 0)
     *  The first empty slot that was not quoted/is still primary - actual 5 frames only match the first
     * ==================================================================== */
    if (getenv("DMD_AV1_LOG"))
        fprintf(stderr, "[av1] ft=%u oh=%u refresh_all=%d err_res(Entry into force)=%d "
                        "er_inferred=%d Original er=%u\n",
                frame_type, p->order_hint, refresh_all, err_res, er_inferred,
                p->pic_info_fields.bits.error_resilient_mode);
    unsigned refresh_mask = 0;
    int my_slot = -1;
    if (!refresh_all) {
        /* Places are occupied. The real value is set to the next map score.
         * (dmd_av1_watch_prev_refresh) and then rewrite the eight places in place;
         * If frame sync is flushed out early, rewrite to 0 and enter the repair queue.
         * Shadow registration is not here either -- it's only registered when the real value comes out. */
        my_slot = dpb ? (int)(dpb->dpb_next_slot & 7u) : 0;
        refresh_mask = 1u << my_slot;
        if (dpb) {
            /* The frame number and the surface belong to the registration at this point (the quotes from the frame follow) and the other part of the frame are used to register the situation.
             * The slots are placed until the patch is calculated for the real refresh. */
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
        /* KEY/ SWITCH frames are fully brushed: refresh does not write the stream (regulation 7.20 extrapolation),
         * The shadow sheet is immediately registered - the frame is sent straight, without delay, and the frame is the final value. */
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

    /* ref_order_hint[i] (norm 5.9.2):
     *   !refresh_all && error_resilient_mode && enable_order_hint
     *
     * The HID does not trigger (DMD_AV1_LOG pick):
     *   & ft=0 refresh_all=1 err_res=1  ⁇ KEY frames, exclude
     *   ft=1 refresh_all=0 err_res=0  ⁇  framework, err_res 0
     * The KEY frame is blocked by refresh_all, the frame frame err_res is fake, and no bytes are written.
     *
     * So this is not happening here. If you encounter a error_resilient inter-framework stream, you need to fill it up.
     * Note two points: (1) must be brought with: < http_all > , otherwise write more to KEY frames
     * 8*order_hint_bits will cause the entire frame to be mispositioned (actual dav1d reads 8 tile)
     * 2x17 after reporting zero_bit out of range; (2) VA-API does not provide for slots or for
     * Hint, to be taken from dpb_order_hint[] of DPB. */

    /* Reference Frame Index: Only in the frame. */
    unsigned my_idx[7] = { 0, 0, 0, 0, 0, 0, 0 };  /* Enter a slot number for the stream after translation */
    if (!intra_only) {
        /* frame_refs_shot_signaling requires enable_order_hint; state 0
         * A display of all seven ref_frame_idx (the array is provided by VA-API). */
        if (enable_order_hint)
            dmd_bw_put_flag(&bw, 0);
        /* Translating VA-API slot numbers by Shadow DPB into slot numbers ** for this synthetic stream**.
         *
         * The DPB slot allocation for this stream is not related to the source stream (KEY/All-Filter 8 slots).
         * The rest of the frame is on dpb_next_slot and is registered at the end of the build_frame)
         * And VA-API's ref_frame_idx[i] is the slot number that ffmpeg maintains by source stream.
         * Translation rules: s  ⁇  ref_frame_map[s] (source id, "What frame do you want?")
         * "Content" is the shadow of which slot the shadow has now taken.
         *
         * Why must it be translated instead of overflowing: tectonics only distribute slots and source streams in synthetic streams
         * It's built when the frame is consistent. It requires refresh_frame_flags to be set to the frame equals the source value.
         * - And the true value is to be calculated by waiting for the next map score, which means that each frame must be delayed.
         * The ffmpeg will be in place soon after EndPicture's Sync leaf frame.
         * The temporary frame is sent out early with the wrong refresh, the reference chain is broken
         * (Survey 160 frame samples 147 frame pixel error, KEY frames all right, frames all wrong).
         * The translation is done and the flow is self-directed, and each frame is ready to be sent out immediately, with the whole delay/caution/flush
         * The mechanisms are no longer needed.
         *
         * CDF succession is also right: the decoder from ref_frame_idx [primariy_ref_frame]
         * Load the pointed slots with the CDF, the translation guarantees that it is still the same frame, so the CDF evolves.
         * Chains are the same as the source stream through the frame, and the source stream tile data can be decoded as it is.
         *
         * The same shadow mode in the DPB may take over multiple slots (KEY/ Full-Frusher 8 slots), but the same system can be used to create a new system of filters.
         * Take the same price for any item (the same content as the CDF), take the first one that you have scanned. */
        for (int i = 0; i < 7; i++) {
            unsigned my = 0;
            const int ok = resolve_ref(dpb, p, i, &my);
            if (getenv("DMD_AV1_DBG"))
                fprintf(stderr, "[dbg] ref[%d]: va_slot=%u -> my_slot=%u %s\n",
                        i, (unsigned)p->ref_frame_idx[i], my,
                        ok ? "" : "Not parsed");
            if (getenv("DMD_AV1_REFDBG"))
                fprintf(stderr, "[refdbg] f=%d t=%u ref[%d]: surf=%u va_slot=%u"
                                " -> my=%u%s\n",
                                dpb ? dpb->frame_seq : -1, frame_type, i,
                                (unsigned)(p->ref_frame_idx[i] < 8
                                               ? p->ref_frame_map[p->ref_frame_idx[i]]
                                               : 0),
                                (unsigned)p->ref_frame_idx[i], my,
                                ok ? "" : "  <-- Not parsed");
            my_idx[i] = my;
            dmd_bw_put_bits(&bw, my, 3);
        }
        /* The frame_id_numbers_present=0, not delta_frame_id. */
    }

    /* frame_size() / render_size(): frame_size_override=0 by sequence head
     * Here only the supres and render_size (regulation 5.9.5/5.9.6. 5.9.8). */
    /* Superres_params((regulation 5.9.8):** Only when the head of the sequence is enable_supres=1**
     * The stream has only one of these.
     *
     * The first post was written here by a new user: "The first time I wrote this post is that I wrote the following:
     * The decoder came to read the sign, and the realization was written unconditionally -- the frame was misplaced by 1 bit from here.
     * Dav1d reports "Trailing_one_bit out of range:0". Lessons: Van's head is somewhere.
     * There is no place for this field in the frame, and both conditions must be written in the same expression. */
    if (p->pic_info_fields.bits.use_superres) {
        dmd_bw_put_flag(&bw, 1);
        /* @dord_denom: SUPERRES_DENOM_MIN=9, in SUPERRES_DENOM_BITS=3 places. */
        const uint32_t denom = p->superres_scale_denominator;
        dmd_bw_put_bits(&bw, (denom >= 9 ? denom - 9 : 0), 3);
    }
    /* Render_and_frame_size_different = 0: display size equals frame size.
     * VA-API does not provide render_size and it only affects displaying cropping and not decoding. */
    dmd_bw_put_flag(&bw, 0);

    /* Allow_intrabc (regulation 5.9.2): condition of allow_screen_content_tools & & &
     * UPCALEDWIdth = FlameWidth. By ffmpeg CBS:
     *   if (allow_screen_content_tools && upscaled_width == frame_width)
     *       flag(allow_intrabc);
     *   else
     *       infer(allow_intrabc, 0);
     *
     * Upscaled_width = frame_width equal to "no sample on superres" --
     * The frame_width button is narrowed only when the user_papers are used.
     *
     * ⁇  This condition was missed twice: first leaked the evidence, then took it.
     * Allow_screen_content_tools completely removed (cause of a misread of the real stream somewhere).
     * Both conditions**, with one missing, write more than one bit less. */
    /* ⁇  Here the CBS source code does not match the actual output of libaom, whichever is ** the real stream**.
     *
     * CBS (cbs_av1_syntax_temlate.c) writes about readable conditions:
     *   if (allow_screen_content_tools && upscaled_width == frame_width)
     *       flag(allow_intrabc); else infer 0
     *
     * But field-by-field reconciliation of libaom generated by bits parser 1080p KEY_FRAME:
     * Allow_screen_content_tools=0. If you skip this position under CBS conditions, the entire frame is set to be a bit more than a few minutes away.
     * Trailing_one_bit=0 at the end; read this one unconditionally to get
     * Trailing_one_bit=1 (precision closing).
     *
     * Conclusion: The actual encoder wrote the position unconditionally. The verdict is "can the real code close?"
     * And not how the source code says -- we're feeding the real codec, not the CBS. */
    /* Allow_intrabc: Depends on the ffmpeg trade_headers' in-situ output of the real stream.
     * 1080p KEY_FRAME (allow_screen_content_tools=0) from the actual libaom:
     *   The first of the 46 renders and frame_size_disferent
     *   47 places_frame_end_update_cdf ⁇  allow_intrabc** does not exist**
     *   48 places uniform_tile_spacing_flag
     * That is, CBS conditions are read in: this is not written into the stream when asct=0.
     *
     * The  ⁇  was assumed to be unconditional based on the self-written parser "digit closing," which is wrong...
     * The parser itself is biased here, and two errors offset each other to appear closed.
     * Lessons: Validation tools must first be calibrated with an authoritative output like trade_headers, which is a very important tool for the development of the new technology.
     * Uncalibrated home-made solver cannot be used as a baseline. */
    if (intra_only && p->pic_info_fields.bits.allow_screen_content_tools &&
        !p->pic_info_fields.bits.use_superres)
        dmd_bw_put_flag(&bw, (int)allow_intrabc);

    if (!intra_only) {
        dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.allow_high_precision_mv);
        /* read_interpolation_filter()：is_filter_switchable f(1)，
         * Write 2 bits of interp_filter for 0 hours. The interp_filter for VA-API
         * 4 = SWITCHABLE。 */
        if (p->interp_filter == 4) {
            dmd_bw_put_flag(&bw, 1);
        } else {
            dmd_bw_put_flag(&bw, 0);
            dmd_bw_put_bits(&bw, p->interp_filter, 2);
        }
        dmd_bw_put_flag(&bw,
            (int)p->pic_info_fields.bits.is_motion_mode_switchable);
        /* Use_ref_frame_mvs to apply_ref_frame_mvs (Sequence header 1),
         * Not error_resilient and available_order_hint. */
        if (!err_res && enable_order_hint)
            dmd_bw_put_flag(&bw, (int)p->pic_info_fields.bits.use_ref_frame_mvs);
    }

    /* Disable_frame_end_update_cdf:reduced_still_picture_header=0 and
     * appears when Disable_cdf_update=0. */
    if (dpb)
        dpb->last_endupd_bitpos = (size_t)-1;
    if (!p->pic_info_fields.bits.disable_cdf_update) {
        /* Write down the offset: Double decoding is done by turning off the first rewrite. See.
         * dmd_av1_dpb::last_endupd_bitpos */
        if (dpb)
            dpb->last_endupd_bitpos = bw.byte_pos * 8 + (size_t)bw.bit_pos;
        dmd_bw_put_flag(&bw,
            (int)p->pic_info_fields.bits.disable_frame_end_update_cdf);
    }

    /* MiCols/MiRows: image size in 4x4 units (guiding by 5.9.5).
     * 2*(width+7)>3 equals up to 8 pixels equal to half a point. */
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

    /* Delta_q_params(5.9.17): delta_q_present at base_qindex > 0. */
    if (p->base_qindex > 0)
        dmd_bw_put_flag(&bw, (int)p->mode_control_fields.bits.delta_q_present_flag);
    if (p->mode_control_fields.bits.delta_q_present_flag) {
        dmd_bw_put_bits(&bw, p->mode_control_fields.bits.log2_delta_q_res, 2);

        /* Delta_lf_params(5.9.18): only occurs when delta_q_present(s). */
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

    /* Read_tx_mode((5.9.21): tx_mode_only_04x4, tx_mod_mod_mode_losses
     * Do not write; otherwise write tx_mode_secondf(1). VA-API tx_mode
     * 2 = TX_MODE_LARGEST, 3 = TX_MODE_SELECT。 */
    /* Read_tx_mode (original CBS):
     *   ⁇  infer (tx_mode, ONLY_4X4), do not write
     *   Otherwise (tx_mode, TX_MODE_LARGEST=1, TX_MODE_SELECT=2)
     *
     * ⁇  used as f(1) sign `tx_mode==3 ' , both wrong:
     *   - Encoding is not a single signpost, but rather an incretion (range [1])
     *   - The tx_mode domain of VA-API is [0.2] (va_dec_av1.h:560/563),
     *     Directly the norm enumerator, does not exist.
     * Inclement writes (max-min)=1 with no stop slot, and then you can't stop.
     * Write only one 0 when the range_min is in. */
    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "txmode", bw.byte_pos*8+bw.bit_pos);
    if (!coded_lossless) {
        const uint32_t tm = p->mode_control_fields.bits.tx_mode;
        if (tm >= 2)
            dmd_bw_put_flag(&bw, 1);   /* TX_MODE_SELECT: Write 1, no stop slot */
        else
            dmd_bw_put_flag(&bw, 0);   /* TX_MODE_LARGEST: Stop bit */
    }

    /* (5.9.23): frame frame frame writing */
    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "refsel", bw.byte_pos*8+bw.bit_pos);
    if (!intra_only)
        dmd_bw_put_flag(&bw, (int)p->mode_control_fields.bits.reference_select);

    /* skip_mode_params((5.9.22)): skipModeAllowed must follow the status of ** this synthetic stream**
     * The decoder is the same. If you do not agree, you write more or less one bit.
     * The frame is misallocated from this point on.
     * Skip bit, and we write zero, shift the whole frame back to one bit, CBS reads our warp bit as
     * The blog is a part of the blog, which is a part of the blog of the Global Voices community.
     *
     * The decision (CBS unpressed_header):
     *   The frame is full of  ⁇  [speaks Spanish]!
     *   Otherwise, search forwards and backwards in seven references.
     *   Most remote; backward  ⁇ 1; no backward but longer present than forward  ⁇ 1;
     *   ⁇ .dist = get_relative_dist (ref_hint, this frame hit).
     *
     * The frame order hit takes the shadow DPB (our own slot allocation), not the number of the slots in the source stream,
     * Because the decoder saw exactly what we wrote into the stream of my_idx. */
    int skip_allowed = 0;
    if (!intra_only && p->mode_control_fields.bits.reference_select &&
        enable_order_hint && order_hint_bits > 0 && dpb) {
        const unsigned ohm = 1u << order_hint_bits;
        /* Standard get_relative_dist( 5.9.x): v = (a-b) & (ohm-1); should v ** be greater than**
         * ohm/2 only reduces the ohm. ties(v = ohm/2) keep the positive value.
         * previously written `v - (v & ohm) ' equals the bit order_hint_bits: only margin
         * Only negative = 3*ohm/4 is negative and rounded back (e. g. Hint=0 reference Hint=96, ohm=128)
         * It should have been -32 but calculated to be +96, skipModeAllowed 1 bit less, and the overall frame was 1 bit different. */
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
        /* Mode_control_fields of the value VA-API_mode_present:ffmpeg
         * CBS deciphers from the source stream, faithful to the source stream. */
        dmd_bw_put_flag(&bw,
            (int)p->mode_control_fields.bits.skip_mode_present);
    }

    /* Allow_warped_motion (5.9.22, immediately after kip_mode_params):
     * For authority conditions, see CBS cbs_av1_syntax_templace.c, uncompressed_header:
     *   if (frame_is_intra || error_resilient_mode || !seq->enable_warped_motion)
     *       infer (allow_warped_motion, 0); / / do not take the code current
     *   else
     *       flag(allow_warped_motion);
     * Not related to the fact that the field only determines the size of the block
     * Whether the motion mode needs to be interpreted is used long before the read_mv_mode is found above this function.
     * The old realization added it to the guard, so the mms=0 frame was less than 1 bit, then
     * The whole of the group is now in the front line of the #MultiBlackBlackBlackBlackBlackBlackBlack.
     * Softly reporting Invalid Data, hard-disposal garbage.
     * Sequence level is achievable_warped_motion We write consistently 1 (see: 902), so there is no need to sentence here. */
    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu va_warp=%u va_redtx=%u va_mms=%u cf=%u oh=%u\n", "warp", bw.byte_pos*8+bw.bit_pos,
            (unsigned)p->pic_info_fields.bits.allow_warped_motion,
            (unsigned)p->mode_control_fields.bits.reduced_tx_set_used,
            (unsigned)p->pic_info_fields.bits.is_motion_mode_switchable,
            (unsigned)p->current_frame, (unsigned)p->order_hint);
    if (!intra_only && !err_res)
        /* Allow_warped_motion for direct transliteration of VA-API (see ibid.:
         * The ffmpeg CBS parsing value is faithful to the source stream, the old rule "uniform_ref set 0" is
         * The switch flow is wrong. */
        dmd_bw_put_flag(&bw,
            (int)p->pic_info_fields.bits.allow_warped_motion);

    if (getenv("DMD_AV1_BITS")) fprintf(stderr,"[bits] %s @ %zu\n", "redtx", bw.byte_pos*8+bw.bit_pos);
    dmd_bw_put_flag(&bw, (int)p->mode_control_fields.bits.reduced_tx_set_used);

    /* Global_motion_params((NOR 5.9.24): Frame-to-frame frame
     *   ⁇  is_global[i] is_rot_zoom[i]  ⁇  is_translation[i]  ⁇  arguments
     *
     * Wm[i] of VA-API for ** after reconstruction (ffmpeg libavcodec/vaapi_av1.c)
     * AV1Frame.gm_params) to restore the symbol in the original stream must be recentering
     * , for the counter-counting +subtext bit, see dmd_gm_put_param above.
     *
     * The base for the difference must be ** the decoder at the time**: in the norm it was taken from
     *   We use my_idx[, translated]
     *   The slot number of this synthetic stream is checked for backup gm_slot[] in the slot, which is read to the same value as hardware.
     *   Primariy_ref_frame to NONE (the frame/err_res/source stream does not quote any frame)
     *   , using the default value, is consistent with the setup_past_indexence that regulates 7.11.3.6.
     *
     * Order: ROTZOOM in the stream is [2][3] [0][1], AFFFINE is [2][3] [4] [5][0][1],
     * TRANSLATION only [0][1] - not below the ascending order, misorders the entire frame.
     *
     * Any parameter that doesn't work out is worth anything to rebuild the grid or get out of the class.
     * [-mx, mx] Return to IDENTITY**: Count it first, never half.
     * The default parameters are the defaults that are stored in the back frame, and the following prev returns the defaults together.
     * The synthetic stream remains self-serving, but only the global movement compensation (the same as the old behaviour) that has been lost from the frame. */
    struct dmd_av1_gm cur_gm;
    dmd_gm_default(cur_gm.p);
    if (!intra_only) {
        /* The order of the lower mark in the stream (not ascending!): see the order of call in 5.9.24. */
        static const int gm_order[4][6] = {
            { -1, -1, -1, -1, -1, -1 },   /* IDENTITY: No parameter written */
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

            /* Arguments for depositing this frame into the slot = the decoder that is reconstructed. VA wmmat[0.5] is exactly
             * AV1Frame.gm_params [0.5] (ffmpeg) item by item, including untransmitted subscripts
             * The default value is 4/5 extrapolated by ROTZOOM, so just six values are taken. */
            for (int j = 0; j < 6; j++) cur_gm.p[i][j] = cm[j];
        }
    }
    if (dpb) {
        dpb->gm_pending = cur_gm;
        dpb->gm_pending_frame = dpb->frame_seq;
        if (refresh_all) {
            /* KEY/SWITCH Full brush: 8 slots are this frame (registration with dpb_shadow above)
             * Same place, same condition. The gm of the frame is always the default. */
            for (int k = 0; k < 8; k++) dpb->gm_slot[k] = cur_gm;
        }
    }

    /* film_grain_params() (5.9.30): file_grain_params_present
     * The entire time is 0. We rewrite the sign, so the same conditions apply here. */
    if (p->seq_info_fields.fields.film_grain_params_present &&
        (show_frame_eff || p->pic_info_fields.bits.showable_frame))
        dmd_bw_put_flag(&bw, 0);         /* apply_grain = 0 */

    /* Do not end with trailing_bits / byte_alignment - to the caller in sealed form. */
    *bwp = bw;
}

/* Load the frame payload into the specified type of OBU. OBU_type determines which alignment to follow at the end:
 *   DMD_OBU_FRAME_HEADER  ⁇  trailing_bits (Regular 5.9.1)
 *   DMD_OBU_FRAME  ⁇  byte_alignment (regulation 5.10), followed by Tile_group
 * Returns the bytes (including the header of the OBU) that write out, failed to return 0.
 * Tail_out returns the payload bit length to continue the spell tile_group for OBU_FRAME. */
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

    /* There was an "unidentified replacement" here. It does not exist in
     * In the norm, and on the frame of the frame, the true length of the frame is 0 (mod 8) to be held up byte_alignment
     * A whole 0x00 bytes --tile data body moves 1 bytes, entropy decoding from first byte
     * It is wrong (actually 640x360 7th frame dav1d direct report Invalid Data).
     * The frame field is now aligned to the source stream in step, and no replacement is required. */
    if (obu_type == DMD_OBU_FRAME)
        dmd_av1_byte_align(&bw);      /* Zero clean, no tags. */
    else
        dmd_av1_trailing_bits(&bw);   /* Write 1 and make zero. */

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
    /* Independent Frame_HEADER OBU without tile data, with a minimum width of 1. */
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

    /* Tile_size_bytes: the minimum bytes required to take the maximum length of tile.
     * Tile_size_bytes_minus_1 and tile_group
     * The width must be consistent, so we can calculate the frame first and then synthesize it.
     * Note that this is written at len-1 (tile_size_minus_1), so press max_len-1 for width. */
    size_t max_tile = 0;
    for (int i = 0; i + 1 < num_tiles; i++)
        if (tiles[i].len > max_tile) max_tile = tiles[i].len;
    int tile_size_bytes = 1;
    if (max_tile > 0) {
        size_t v = max_tile - 1;
        while (v >> (tile_size_bytes * 8)) tile_size_bytes++;
    }
    if (tile_size_bytes > 4) tile_size_bytes = 4;

    /* Frame (end byte_alignment, not trading_bits). */
    unsigned char fh[512];
    size_t fh_len = 0;
    if (build_frame_header_obu(tile_size_bytes, p, DMD_OBU_FRAME,
                              fh, sizeof(fh), &fh_len, dpb) == 0)
        return 0;

    /* The payload of tile_group_obu((regulation 5.111):
     *   NumTiles > 1 writes tile_start_and_end_present_flag
     *   Make 0 for this group to overwrite all tile (tg_start=0, tg_end=NumTiles-1)
     *   Next_alignment, write tile_size_minus_1+ data byte_alignment
     *   Last Tile No Length (insured by the remaining length of OBU)
     *
     * Tile_size_minus_1 with le(4), width must be written in the frame tile_info
     * Tile_size_bytes_minus_1=1 (i. e. 2 bytes) is consistent --
     * The two discrepancies will be completely misallocated from the second Tile. */
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

    /* Count OBU payload first to write the obu_size of the leb128. */
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

    /* ⁇  Convert the last_refresh_bitpos from "deficit in frame" to "intra-OBU offset".
     *
     * The bitwriter in put_uncompressed_header writes to a local body buffer,
     * The frame is inverse (actual 19). But the upper layer is changed to contain
     * The full buffer of the head of the OBU will be missed by direct hit of hdr*8 bit.
     * The first byte of OBU is 0x32 and is changed to 0x20.
     * The frame 6 became 4(TILE_GROUP) and has_size was cleared, and the #MultiDanananDownDownSives are not allowed to use the #Murvey4 — the frame — but the #Murvey4 — is not a good idea.
     * The whole stream collapsed from the structure of the place — dav1d recognized only five OBUs and reported invalid Data.
     *
     * Caller (decode.c) adds the length of the more outer prefix, such as TD. */
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
            /* le (tile_size_bytes): width must be written on the frame
             * Tile_size_bytes_minus_1+1 is consistent, otherwise the second Tile is completely misplaced.
             * The width is calculated by pressing max(tile_len) above and writing no more deaths - 2 bytes written dead,
             * The source stream actually uses one byte, with one byte more than one byte per gap, leading to an overall offset of the flow. */
            const uint32_t v = (uint32_t)(tiles[i].len - 1);
            for (int b = 0; b < tile_size_bytes; b++)
                *q++ = (unsigned char)((v >> (8 * b)) & 0xFF);
        }
        for (size_t k = 0; k < tiles[i].len; k++)
            *q++ = tiles[i].data[k];
    }
    /* Frame successfully synthesized - register this frame into Shadow DPB for ref_frame_idx queries for the follow frame.
     * Must be done at the end of the successful path: do not pollute the DPB state when the synthesis fails,
     * Otherwise, the frame will quote a slot that has never really been written into the decoder. */
    /* Shadow DPB registered moved into refresh in put_unpressed_head:
     * The whole frame is empty there. The 8th slot is empty. The rest is taken there.
     * The victim slot "Determination confirmed". This is no longer registered, and double-entry is avoided. */

    return hdr + payload_len;
}

size_t dmd_av1_build_show_existing(unsigned char *buf, size_t cap,
                                   unsigned map_idx)
{
    if (!buf || cap < 4)
        return 0;

    /* Load: show_existing_frame(1)=1, frame_to_show_map_idx(3),
     * Then the trailing_bits (regulation 5.3.4: write a 1 first and then fill the byte boundary with zero).
     * 4-bit valid data + stop bit 1 + 3 0 = just 1 byte. */
    struct dmd_bitwriter bw;
    unsigned char payload[2];
    dmd_bw_init(&bw, payload, sizeof(payload));
    dmd_bw_put_flag(&bw, 1);                 /* show_existing_frame */
    dmd_bw_put_bits(&bw, map_idx & 7u, 3);   /* frame_to_show_map_idx */
    dmd_av1_trailing_bits(&bw);
    if (bw.overflow)
        return 0;

    const size_t plen = bw.byte_pos + (bw.bit_pos ? 1 : 0);

    /* OBU Head: Forbidden (1) = 0 type (4) = 3 interpretation (1) = 0 has_size (1) = 1
     * Reserved (1) = 0  ⁇  (3 < 3) ⁇ 0x02 = 0x1a, consistent with the source flow. */
    size_t n = 0;
    buf[n++] = (unsigned char)((DMD_OBU_FRAME_HEADER << 3) | 0x02);
    /* The bou_size of the leb128; the square is consistently 1 and the word bytes are sufficient. */
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
    /* The pav_frame_bytes cannot return early because they are empty.
     *
     * So that KEY and the first frame (there is no temporary frame) will all jump through the end.
     * Map snapshot, make prev_valid late. Actual consequence: calculated sequence
     * - 8, 32, 64 and real value 1, 8, 32, 64 - First value 1 lost, remaining overall error
     * The previous frame was given, and all the rewrites fell on the wrong frame.
     * Whether or not there is a frame to rewrite this time, it is important to keep the map as the benchmark for the next score difference. */
    if (!dpb || !cur_pic) return;
    const VADecPictureParameterBufferAV1 *p = cur_pic;
    if (getenv("DMD_AV1_DBG"))
        fprintf(stderr, "[dbg] patch: cur_oh=%u prev_valid=%d hold=%s bitpos=%zd\n",
                p->order_hint, dpb->prev_valid,
                prev_frame_bytes ? "yes" : "NO",
                prev_bitpos == (size_t)-1 ? -1 : (ssize_t)prev_bitpos);

    /* - Calculate Source Frame E from previous DPB frame (for slot allocation of this frame)
     *
     * The difference between the map of this frame and the previous map = changes to the DPB when the previous frame is decoded:
     * The old slot is the frame (E) that the source encoder kicked. The source encoder guarantees that the kick is made.
     * The frame is no longer quoted by any successor frame -- it's the only "death-sure" frame at this moment.
     * Let this frame take its slots never cover living references (a later one phaseout).
     * Disparities 0 (previous frame refresh=0, no DBB) or multi-hull (all KEY)
     * ivic_valid=0, selected by put_unpressed_header. */
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

    /* ⁇  prev_valid must be called in position at the KEY frame.
     *
     * The difference takes "the last frame" and the KEY frame does not write refresh (no rewrite),
     * But its map is the baseline for the first time that the frame is missing. If only the rewrite is actually implemented,
     * The whole sequence will be shot later: the actual number is 1, 8, 32, 64, and the real value is 1, 8, 32, 64.
     * The first value 1 was lost and the first one was misplaced to the previous frame.
     * So, whether or not this function is rewritten, saves the Map at the end and prev_valid. */
    if (dpb->prev_valid && prev_frame_bytes &&
        prev_bitpos != (size_t)-1 && prev_len > 0) {
        /* The difference between this frame Map and the upper frame Map bit = the slot actually written in the previous frame.
         * Actual 4 frames hit the real source stream (1/8/32/64).
         *
         * ⁇  But this frame is KEY/INTRA_ONLY not able to do this difference when the whole frame is painted** (No. 94 live positioning).
         * Regulation 7.20: RefreshFrame Flags for allFrames is ** on the side of the decoder
         * The index **, without writing the stream. The brush will replace the ref_frame_map eight slots once and for all.
         * The difference is 0xff -- and this 0xff is assigned to the last frame**, which is the first time that the  ⁇  is used to create a new structure.
         * Cover it for the real refresh value.
         *
         * Real-time symptoms (150 frame samples, oh=30 at KEY frames):
         *     Counter-sequence 0x00 0xff 0xff 0x01 0x08...
         *     Source current real value 0x00 0x08 0x01 0x08 0x20...
         * The first 28 items were identical, and the first 28 items were inserted into two 0xffs, which resulted in the entire misposition of one.
         * 49 out of the full sequence 104 do not match. Refresh is misplaced by the full reference frame.
         * It is expressed as "frame pair, screen count, but pixels are inconsistent from frame 1. "
         *
         * Correct approach: The whole frame does not involve differentials, jumps straight to the end to update the baseline map.
         * Refresh of the previous frame keeps the rotation value when it synthesizes itself -- that value.
         * Not necessarily equal to the source true, but at least not contaminated by 0xff, nor did it cause a sequence error.
         *
         * The whole brush is not enough in itself (this is half of the first version of the repair).
         * After the whole map has been replaced, ** the frame that followed it ** takes this new benchmark.
         * The difference will be calculated as near-full mask. The first version of the index will reduce the inconsistency from 49 to 49.
         * 29, but item 28 is still 0xff -- that is the frame that is the back of the whole frame.
         * So, in addition to skipping the score, the whole frame must be disabled by the baseline (prev_valid=0), which is a very good thing.
         * Let the back frame also skip once and then re-establish the differential benchmark. */
        /* The evidence must be identical to the refresh_all in dmd_av1_build_frame
         * (Code 7.20/ 5.9.2): SWITCH frame, or KEY frame show_frame=1.
         *
         * I wrote the first version of `frame_type=0 ⁇   ⁇  frame_type=2 ' -- both wrong:
         *   • Missing show_frame: The KEY frame is only painted when show_frame=1
         *   ..don't worry. INTRA_ONLY(2): Not all of it, as usual.
         * Unit testing for spot-on-the-spot capture (for example, memset construction of pic, frame_type
         * The two claims were zero).
         * Lessons: The same semantic approach is always one way or another, and here we can just copy the refresh_all formula. */
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
                fprintf(stderr, "[av1] This is a whole painting.(ft=%u show=%u),Skipping score"
                                "(Otherwise, we’ll figure it out. 0x%02x Contamination past the frame.)\n",
                        cur_ft, p->pic_info_fields.bits.show_frame, mask);
            mask = 0;
            /* Saves a new baseline, but marked as "not usable for difference" -- one skipping from the frame.
             * By its own Map re-created the baseline. */
            for (int i = 0; i < 8; i++) dpb->prev_ref_map[i] = p->ref_frame_map[i];
            dpb->prev_valid = 0;
            return;
        }

        /* Rewrite the eight places in the top of the top.
         * Not necessarily bytes, then, in the first place, MSB. */
        if (getenv("DMD_AV1_LOG"))
            fprintf(stderr, "[av1] It’s a frame. refresh=0x%02x (bitpos=%zu len=%zu)\n",
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
        /* Shadow registration: The source stream puts the frame into those slots in the mask, our slots.
         * Consistent with source (the synthetic stream refresh has been recast to source true), shadowed.
         * The subsequent citation translation is done accordingly. */
        if (prev_frame > 0) {
            for (int k = 0; k < 8; k++)
                if (mask >> k & 1u) {
                    dpb->dpb_shadow[k] = prev_frame;
                    dpb->dpb_order_hint[k] = dpb->last_oh;
                    /* Global motion backups must be synchronized with shadow tables: next frame counts
                     * The difference sign is the parameter of this frame that is registered here.
                     * gm_pending is the parameter for "Recent Synthetic Frame," only it does belong to it.
                     * Prev_frame only available (otherwise default values - with codecs
                     * If you don't agree, you can throw up your motion compensation at most once, and you don't write bad-ass flows. */
                    if (dpb->gm_pending_frame == prev_frame)
                        dpb->gm_slot[k] = dpb->gm_pending;
                }
        }
    }

    for (int i = 0; i < 8; i++) dpb->prev_ref_map[i] = p->ref_frame_map[i];
    dpb->prev_valid = 1;
}


/* - - - - - - - - - - - - - - - - - - - - Head of the OBU */

size_t dmd_av1_obu_header(int obu_type, size_t payload_len,
                          unsigned char *out, size_t out_cap)
{
    if (obu_type < 0 || obu_type > 15 || out_cap < 1)
        return 0;

    /* forbidden(1)=0 | type(4) | extension(1)=0 | has_size(1)=1 | reserved(1)=0
     *
     * This means 0x00  ⁇ (type< < 3) ⁇ 0x00 ⁇ 0x00  ⁇  has_size is bit1.
     * Examples: SEQUENCE_HEADER(1)  ⁇ Ox0a, FRAME_HEADER(3)  ⁇ x1a,
     *       TILE_GROUP(4) → 0x22，TEMPORAL_DELIMITER(2) → 0x12。
     *
     * The actual illegal value is 0xd0 = 1101_000:forbidden=1 (must 0),
     * Type = 10 -- one can see that it's not an OBU head but a naked load. */
    out[0] = (unsigned char)(((obu_type & 0x0f) << 3) | 0x02);

    size_t n = dmd_av1_leb128((uint64_t)payload_len, out + 1, out_cap - 1);
    if (n == 0)
        return 0;
    return 1 + n;
}
