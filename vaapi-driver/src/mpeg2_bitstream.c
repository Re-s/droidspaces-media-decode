/* MPG-2 (ISO/IEC 13818-2) head synthesis.
 *
 * Why do you need: V4L2 stateful decoder for the full MPEG-2 video base stream bytes?
 * VA-API only gives **slick layer** data in VASLiceDataBufferType
 * (Slick_start_code, see VASLiceParameterBufferPEG2: va.h)
 * It's got a slick_horizontal_position/Quantiser_scale_code_slick header
 * field, which indicates that the upper layer has been deconstructed, but the bytes are still in the buffer.
 * sequence_header / sequence_extension / picture_header /
 * This ** frame level and no sequence header **-
 * Their contents were removed into VAPictureParameter BufferPEG2 and
 * VAIQMatrixBufferMPEG2. So you have to write it back in the reverse direction.
 *
 * Difference to h264/hevc: MPEG-2 separated by start code (000000 01 x),
 * Syntax: All elements are long-spaced, no ue(v)/se(v) or ** environment reservation**
 * (Rocking is a unique constraint by start code, encoder guarantees that no false starting code is created),
 * So there is no need for dmd_rbsp_escape.
 *
 * ⁇  Known information gaps (which determine the scope of application of this realization):
 *   - non-provision of VA-API.
 *     These only affect time sequence and buffer models, do not affect pixel reconstruction, and fill in the legal values allowed by the specifications.
 *   - mpc_ratio_information: Not available, fill 1 (square screens).
 *   - Profile_and_level_indication: Not available, fill in Main@Main.
 *   - chroma_format: VA-API without field. 1 (4:2:0) - this driver NV12
 *     The return path is only 4:2:0 supported, and the output format of other colour formatting is not valid.
 * These filling values are not involved in the reconstruction of the residue/projection, so pixels are still correct by bytes;
 * If you encounter a 4:2:2 stream in the future, you should reject it, not guess it.
 */
#include "mpeg2_bitstream.h"

#include <string.h>

#include "bitstream.h"

/* MPEG-2 start code. The prefix is 0000 01. */
#define MP2_SC_PICTURE   0x00
#define MP2_SC_SEQUENCE  0xB3
#define MP2_SC_EXTENSION 0xB5
#define MP2_SC_GOP       0xB8

/* Extension_start_code_identifier (Table 6-2 13818-2) */
#define MP2_EXT_SEQUENCE          0x1
#define MP2_EXT_PICTURE_CODING    0x8

static void put_start_code(struct dmd_bitwriter *bw, unsigned char code)
{
    /* The starting code must be byte aligned. Call points are aligned, and here the numeric patches are made to prevent misuse. */
    while (bw->bit_pos != 0)
        dmd_bw_put_flag(bw, 0);
    dmd_bw_put_bits(bw, 0x000001, 24);
    dmd_bw_put_bits(bw, code, 8);
}

/* sequence_header()（13818-2 §6.2.2.1）+ sequence_extension（§6.2.2.3）。
 * The two must appear in pairs, otherwise the decoder will interpret subsequent syntax by MPEG-1. */
static void build_sequence(struct dmd_bitwriter *bw,
                           const VAPictureParameterBufferMPEG2 *pp,
                           const VAIQMatrixBufferMPEG2 *iq)
{
    const unsigned w = pp->horizontal_size;
    const unsigned h = pp->vertical_size;

    put_start_code(bw, MP2_SC_SEQUENCE);
    /* Horizontal/ vertical_size_value only 12 bits, 2 bits high
     * *Size_extension. */
    dmd_bw_put_bits(bw, w & 0xFFF, 12);
    dmd_bw_put_bits(bw, h & 0xFFF, 12);
    /* * See table 6-3 for the table.
     * VA-API does not provide the field. By size, because it only affects the vertical ratio,
     * And 16:9 material fill square will allow solids to be verified with inconsistent display parameters.
     * The primary stream (1920x1080) is measured at 3. */
    unsigned aspect = 1;
    if (h > 0) {
        /* Avoid floating points by integer ratio: 16/9  ⁇  1.777, 4/3  ⁇  1.333 */
        const unsigned r1000 = (w * 1000u) / h;
        if (r1000 >= 1700 && r1000 <= 1850) aspect = 3;       /* 16:9 */
        else if (r1000 >= 1300 && r1000 <= 1400) aspect = 2;  /* 4:3 */
    }
    dmd_bw_put_bits(bw, aspect, 4);
    dmd_bw_put_bits(bw, 5, 4);          /* frame_rate_code: 30fps (not provided by VA)*/
    dmd_bw_put_bits(bw, 0x3FFFF, 18);   /* bit_rate_value: Full 1 = Not specified */
    dmd_bw_put_flag(bw, 1);             /* marker_bit */
    /* vbv_buffer_size_value: VA-API not provided.
     * The  ⁇  cannot be a big one. The SYS_ERROR, which was previously filled in 112, is a Venus solid.
     * The same number flow is filled in 3. Solids will set a code flow buffer against this value and perform a consistency check.
     * Filled too big is a requirement that it prepare a buffer that does not exist. Take 3 to match the common encoder. */
    dmd_bw_put_bits(bw, 3, 10);
    dmd_bw_put_flag(bw, 0);             /* constrained_parameters_flag */

    /* Quantified matrix (zig-zag sequence, in the order required for the stream).
     *
     * The  ⁇  can't directly be trusted with the Va-API load_*_quatiser_matrix sign.
     * ffmpeg's vaapi_mpeg2.c**Puts two marks in 1** with current entry
     * Matrix (even if the bitstream does not exist and uses the standard default matrix).
     *   Original stream sequence_header 12 bytes (load_intra=0, no matrix)
     *   Synthetic stream sequence_head 140 bytes (load_intra=1, 128 byte matrix)
     * Although ffmpeg can untangle the stream itself, Venus solids are received directly by SYS_ERROR.
     *
     * So, compared to the normative default matrix (Table 13818-2 7-3/7-4): "Samely write 0 without the matrix,"
     * Real non-default writing. The head of a common stream is written in the same bytes as the original stream. */
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

    /* The standard default matrix for non-intra is full 16. */
    int nondefault_ni = 0;
    if (iq && iq->load_non_intra_quantiser_matrix) {
        for (int i = 0; i < 64; i++)
            if (iq->non_intra_quantiser_matrix[i] != 16) { nondefault_ni = 1; break; }
    }
    dmd_bw_put_flag(bw, nondefault_ni ? 1 : 0);
    if (nondefault_ni)
        for (int i = 0; i < 64; i++)
            dmd_bw_put_bits(bw, iq->non_intra_quantiser_matrix[i], 8);

    /* The extension syntax for MPEG-2 is enabled. */
    put_start_code(bw, MP2_SC_EXTENSION);
    dmd_bw_put_bits(bw, MP2_EXT_SEQUENCE, 4);
    /* (c) Profile_and_level_indication (13818-2 § 6.3.5, table 8-4/8-5):
     * bit7 = escape(0)，bit6-4 = profile，bit3-0 = level。
     * Main profile = 0b100，Main level = 0b1000 → 0x48。
     * ⁇  but ffmpeg for 1080p Main is **0x44** (bytes-by-tet against original flow)
     * Confirmed) corresponding to Main profile + High-1440 level. 1920x1080 More than Main
     * The limit of 720x576 for the level is the limit, so the encoder must report higher level.
     * SYS_ERROR - it press the level cap
     * Verify resolution, 1080p does not match Main level.
     * Here is the choice of the level: >720x576 by resolution with High-1440(4), otherwise Main (8). */
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
     * GOP header is ** optional**, but Venus solids require it:
     * The results are consistent with the original stream by bytes.
     * If the GOP header is missing, the solid is still reported as SYS_ERROR, and the frame is not vomiting;
     * The source stream order is seq → ext → GOP → picture → ext → slice.
     * VA-API does not provide time_code/close_gop/broken_link,
     * These affect only random access to semantics, do not participate in pixel reconstruction, and are sufficient to fill in legal values. */
    while (bw->bit_pos != 0)
        dmd_bw_put_flag(bw, 0);
    put_start_code(bw, MP2_SC_GOP);
    /* Time_code: 25 bits (drop_frame (1) hour (5) minute (6) marker (1)
     * Second (6) picture (6). Full 0 plus marker=1 i.e. 00:00:00:00.00. */
    dmd_bw_put_flag(bw, 0);             /* drop_frame_flag */
    dmd_bw_put_bits(bw, 0, 5);          /* time_code_hours */
    dmd_bw_put_bits(bw, 0, 6);          /* time_code_minutes */
    dmd_bw_put_flag(bw, 1);             /* marker_bit */
    dmd_bw_put_bits(bw, 0, 6);          /* time_code_seconds */
    dmd_bw_put_bits(bw, 0, 6);          /* time_code_pictures */
    /* Closed_gop=1: claims that this GOP does not rely on the previous GOP. Each frame is brought under the synthetic path.
     * Full head, and the frame is managed by the decoder itself by pressing picture_coding_type,
     * Fill 1 safer (fill 0 will make solids expect forward reference across GOP). */
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
    /* vbv_delay:0x12 indicates that no specification is specified and the norms allow. */
    dmd_bw_put_bits(bw, 0xFFFF, 16);
    if (ct == 2 || ct == 3) {
        /* Full_pel_forward_vector and forward_f_code are MPEG-1 legacy fields.
         * MPEG-2 must write 0/ 7, the real f_code in picture_country_country_extension. */
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
    /* f_code: VA-API wraps four four-digit fields into an int32.
     * "Pack all four lines into this"
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
    /* Bytes alignment is required before starting; session_extension ends with 0. */
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
    /* Only I/P/B (4 = D picture, MPEG-1 exclusive, not supported). */
    if (pp->picture_coding_type < 1 || pp->picture_coding_type > 3)
        return 0;
    /* Field encoding (picture_structure 1=top field, 2=bottom field) needs
     * The field-by-field pairing is_first_field state machine, the driven surface model is frame-by-frame,
     * Do not do semi-finished support -- returns 0 to allow the upper level to fall softly. 3 = frame picture. */
    if (pp->picture_coding_extension.bits.picture_structure != 3)
        return 0;
    if (pp->horizontal_size == 0 || pp->vertical_size == 0)
        return 0;
    return 1;
}
