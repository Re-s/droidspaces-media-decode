/* Bump-back synthesis from VA-API parameters H.264 SPS/PPS bit current
 *
 * ─ Why this -- — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — — —
 * The daemon side is MediaCodec, which is a standard Annex B stream, location by starting code.
 * nal_unit_header identifies the SPS/PPS as codec config (decode-daemon.c: 461-489).
 * But Hwaccel of ffmpeg does not hand over the SPS/ PPS to Driver -- it deciphers itself.
 * (h264dec.c: 699 / 717 case H264_NAL_SPS/PPS) only VCL NALU
 * Passed down by decode_slick. So the argument set must be from us.
 * VAPicture Parameter BufferH264 inverted synthesis.
 *
 * ─ Good news: slick does not need to rebuild -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
 * Main Session Verification (research/M-1-forwarding-spec.md) and reviewed in this module:
 *   h264dec.c:674        FF_HW_CALL(avctx, decode_slice, nal->raw_data, nal->raw_size)
 *   h2645_parse.c: 92/145 nal->raw_data = src, src point to start code
 *   h2645_parse.h: 102-107 raw_data reserve emulation reservation byte (not transferred)
 *   vaapi_h264.c: 386-388
 *   vaapi_h264.c:349-350 slice_data_offset=0、slice_data_flag=VA_SLICE_DATA_FLAG_ALL
 * The original NALU is the original, the original NALU and the slice header**.
 * The only missing starting code is the vaapi_h264.c:351 conclusive circumstantial evidence.
 * - It's a slick header.
 * **The position of the finished bit, the presence of this field indicates the presence of the slick header in the buffer.
 *
 * So we don't need to rebuild the slick header, and we're going to skip the reference list and reorder it with the MMCO command.
 * Inverse (that is the problem of default: VAPictureH264 arrays give the final list rather than the order to reorder).
 * Forwards slick only by four bytes before 0000 00:00 01.
 *
 * ─ The fields still to be guessed -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
 * The profile_idc / level_idc / VUI does not exist in VA-API. See the following notes for this treatment:
 * Profile_idc is mapd by VAProfile, and the number_idc is just enough to push one by resolution with the DPB checklist.
 */

#include <string.h>

#include "driver.h"
#include "bitstream.h"

/* Bit writing in the original language and EPB conversions have been extracted to bitstream.c, shared with HEVC. */

/* ------------------------------------------------------ */

/* Profile_idc: VA-API does not have this field, only from the VAProfile map.
 *
 * Key constraints: pfile_idc > = 100 = only the SPS bit flow appears when chroma_format_idc /
 * bit_depth_* / seq_scaling_matrix_present_flag. So the field is profile_idc
 * Whether or not we write this field must be done on our own, otherwise the separators are mispositioned and the bitstream is out of order.
 *
 * Policy: If the troma/bit_depth in the pic param is not the default value, the High(100) is required
 * To express them; otherwise, they are mapd using VAProfile. H264Hig of ffmpeg is the most common case.
 */
static unsigned int derive_profile_idc(VAProfile profile,
                                       const VAPictureParameterBufferH264 *pp)
{
    unsigned int chroma = pp->seq_fields.bits.chroma_format_idc;
    unsigned int bd_luma = pp->bit_depth_luma_minus8;
    unsigned int bd_chroma = pp->bit_depth_chroma_minus8;

    /* Non-4:2 / non-8-bit can only be expressed in the High series. */
    if (chroma != 1 || bd_luma != 0 || bd_chroma != 0)
        return 100;
    /* Transform_8x8 is a unique tool for High profile. */
    if (pp->pic_fields.bits.transform_8x8_mode_flag)
        return 100;

    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        return 66;
    case VAProfileH264Main:
        return 77;
    case VAProfileH264High:
    default:
        return 100;
    }
}

/* Quevel_idc: VA-API not at all. The risk of overfilling is that MediaCodec is set at level too high
 * Bush even refuses configuration; the risk of being too low is that the decoder considers the code flow to be too high.
 *
 * Press the Max MBPS/MaxFS Table of Annex A to push a level ** just enough to be used **, which is more stable than the fixed values.
 * Only frame sizes (the frame rate is unknown) are used, so take MaxFS for each level (frame size cap, in macros).
 */
static unsigned int derive_level_idc(unsigned int mbs_wide,
                                     unsigned int mbs_high)
{
    unsigned int fs = mbs_wide * mbs_high; /* Frame size, macros */

    /* MaxFS column for Annex A-1. The value is idc (levelx10). */
    static const struct {
        unsigned int max_fs;
        unsigned int idc;
    } tbl[] = {
        { 99, 10 },     /* 1.0  QCIF */
        { 396, 11 },    /* 1.1 */
        { 396, 12 },    /* 1.2 */
        { 396, 20 },    /* 2.0  CIF */
        { 792, 21 },    /* 2.1 */
        { 1620, 22 },   /* 2.2  SD */
        { 1620, 30 },   /* 3.0 */
        { 3600, 31 },   /* 3.1  720p */
        { 5120, 32 },   /* 3.2 */
        { 8192, 40 },   /* 4.0  1080p */
        { 8192, 41 },   /* 4.1 */
        { 8704, 42 },   /* 4.2 */
        { 22080, 50 },  /* 5.0 */
        { 36864, 51 },  /* 5.1  4K */
        { 36864, 52 },  /* 5.2 */
        { 139264, 60 }, /* 6.0  8K */
    };

    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (fs <= tbl[i].max_fs)
            return tbl[i].idc;
    }
    return 62; /* Out of the table, tops. */
}

/* Sequencing of SPSs */

/* Write seq_parameter_set_rbsp in the syntax order of ISO/IEC 14496-10 § 7.3.2.1.1.
 * Each field indicates the source: A = VA-API has, B = deduceable, C = only the default values. */
static size_t build_sps_rbsp(const VAPictureParameterBufferH264 *pp,
                             VAProfile profile, unsigned int disp_width,
                             unsigned int disp_height, unsigned char *out,
                             size_t out_cap)
{
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, out, out_cap);

    unsigned int mbs_wide = (unsigned int)pp->picture_width_in_mbs_minus1 + 1;
    unsigned int mbs_high = (unsigned int)pp->picture_height_in_mbs_minus1 + 1;
    unsigned int profile_idc = derive_profile_idc(profile, pp);
    unsigned int frame_mbs_only = pp->seq_fields.bits.frame_mbs_only_flag;

    /* C: profile_idc by VAProfile */
    dmd_bw_put_bits(&bw, profile_idc, 8);
    /* C:constraint_set0.5 + 2 bit reserved.
     *
     * ⁇  The 8 places cannot be filled out: h264_ps.c: 677-689 more_rbsp_data_in_pps()
     * `Profile_idc ⁇ 66,77,88} and (constraint_set_flags & 7)! = 0 '
     * returns 0, **PPS parsing over tail** transform_8x8_mode_flag/
     * pic_scaling_matrix_present_flag / second_chroma_qp_index_offset
     * (Presspoints h264_ps.c:795).
     *
     * So only set0  ⁇ set1 (which is its definition) is placed on Constrained Basicline.
     * The rest of the same 0 -- we need PPS tails that can be read. */
    unsigned int constraints = 0;
    if (profile_idc == 66 && profile == VAProfileH264ConstrainedBaseline) {
        /* Set0 (bit7) + set1 (bit6): baseline and constraining */
        constraints = 0xC0;
    }
    dmd_bw_put_bits(&bw, constraints, 8);
    /* C: level_idc by frame size */
    dmd_bw_put_bits(&bw, derive_level_idc(mbs_wide, mbs_high), 8);
    /* C: seq_parameter_set_id is always 0 (the slice header references it indirectly through the PPS,
     * And we wrote in the PPS that we can just go on our own. */
    dmd_bw_put_ue(&bw, 0);

    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 || profile_idc == 128) {
        /* A: This group only appears in the High Series and the value must be self-serving with the profile_idc selection */
        dmd_bw_put_ue(&bw, pp->seq_fields.bits.chroma_format_idc);
        if (pp->seq_fields.bits.chroma_format_idc == 3) {
            /* A: resocial_colour_transform_flag of VA-API
             * separate_colour_plane_flag */
            dmd_bw_put_flag(&bw, pp->seq_fields.bits.residual_colour_transform_flag);
        }
        dmd_bw_put_ue(&bw, pp->bit_depth_luma_minus8);   /* A */
        dmd_bw_put_ue(&bw, pp->bit_depth_chroma_minus8); /* A */
        /* C:qpprime_y_zero_transform_bypass_flag, fill 0 (no loss encoding only) */
        dmd_bw_put_flag(&bw, 0);
        /* B: No SPS scale matrix. VAIQMatrix BufferH264 gives **final ** matrix, ** matrix.
         * The reverse push of delta_scale sequences requires a distinction between the fallback rules from the SPS or PPS, and the "F" rules from the PPS.
         * Unable to determine. Put the matrix in the PPP (see build_pps_rbsp), where semantic clarity is expressed. */
        dmd_bw_put_flag(&bw, 0);
    }

    dmd_bw_put_ue(&bw, pp->seq_fields.bits.log2_max_frame_num_minus4); /* A */
    dmd_bw_put_ue(&bw, pp->seq_fields.bits.pic_order_cnt_type);        /* A */

    if (pp->seq_fields.bits.pic_order_cnt_type == 0) {
        /* A */
        dmd_bw_put_ue(&bw, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
    } else if (pp->seq_fields.bits.pic_order_cnt_type == 1) {
        /* A */
        dmd_bw_put_flag(&bw, pp->seq_fields.bits.delta_pic_order_always_zero_flag);
        /* C：offset_for_non_ref_pic / offset_for_top_to_bottom_field /
         * Nom_ref_frames_in_pic_order_cnt_cyclé and exfset_for_ref_frame[]
         * Not at all in VA-API. Pic_order_cnt_type=1 rarely generated (ffmpeg)
         * The stream is almost all type 0 or 2), the simplest form is filled out here: cycle length 0.
         * If you actually encounter a type 1 stream, POC will miscalculate the frame sequence. */
        dmd_bw_put_se(&bw, 0);
        dmd_bw_put_se(&bw, 0);
        dmd_bw_put_ue(&bw, 0);
    }

    dmd_bw_put_ue(&bw, pp->num_ref_frames); /* A */
    /* A */
    dmd_bw_put_flag(&bw, pp->seq_fields.bits.gaps_in_frame_num_value_allowed_flag);
    dmd_bw_put_ue(&bw, pp->picture_width_in_mbs_minus1); /* A */
    /* B:  ⁇  VA-API for ** frame height** macros (vaapi_h264.c:251 filled out)
     * h->mb_width/mb_night-1, while h264_ps.c:457 was analyzed
     * The field in the Bits stream is
     * Map units, half the height of the frame at the staggered flow, must be converted back.
     * Direct original writing doubles the height of the staggered stream. */
    dmd_bw_put_ue(&bw, mbs_high / (frame_mbs_only ? 1u : 2u) - 1);
    dmd_bw_put_flag(&bw, frame_mbs_only); /* A */
    if (!frame_mbs_only) {
        /* A */
        dmd_bw_put_flag(&bw, pp->seq_fields.bits.mb_adaptive_frame_field_flag);
    }
    dmd_bw_put_flag(&bw, pp->seq_fields.bits.direct_8x8_inference_flag); /* A */

    /* B: frame_croping is rolled out by the difference between the size of the macro and the actual size of the display.
     * This is the 1088-vs-1080 counterpart on the side of the bitstream: 1080 is not a multiple of 16, but the number of the people who are in the system is not the same as the number of people who are in the system.
     * So the code height in the SPS is 1088, cutting off 8 lines by Crop.
     * Unit: Crop offset is "Current Sample", which is divided by two vertically at 4:2:0,
     * And the vertical unit multiplied by two when the frame_mbs_only=0. */
    unsigned int coded_w = mbs_wide * 16;
    unsigned int coded_h = mbs_high * 16 * (frame_mbs_only ? 1 : 2);
    unsigned int crop_right = 0, crop_bottom = 0;
    unsigned int chroma = pp->seq_fields.bits.chroma_format_idc;
    /* SubWidthC/SubHeightC：4:2:0 → 2/2，4:2:2 → 2/1，4:4:4 → 1/1 */
    unsigned int sub_w = (chroma == 3) ? 1 : 2;
    unsigned int sub_h = (chroma == 1) ? 2 : 1;
    unsigned int unit_x = sub_w;
    unsigned int unit_y = sub_h * (frame_mbs_only ? 1 : 2);

    if (disp_width > 0 && disp_width < coded_w)
        crop_right = (coded_w - disp_width) / unit_x;
    if (disp_height > 0 && disp_height < coded_h)
        crop_bottom = (coded_h - disp_height) / unit_y;

    if (crop_right || crop_bottom) {
        dmd_bw_put_flag(&bw, 1);
        dmd_bw_put_ue(&bw, 0); /* crop_left */
        dmd_bw_put_ue(&bw, crop_right);
        dmd_bw_put_ue(&bw, 0); /* crop_top */
        dmd_bw_put_ue(&bw, crop_bottom);
    } else {
        dmd_bw_put_flag(&bw, 0);
    }

    /* C: VUI only writes bitstream_restriction paragraph, the rest (colour space, SAR, frame rate)
     * Still missing -- VA-API does not provide them, the decoder uses default values: SAR 1:1,
     * Colour space unspecied. No geometry or pixel values are affected for NV12 output
     * (Show area is handled by us by clicking on the corp.)
     *
     * Why must you write bitstream_restriction:
     * When this is not available, the default value for max_num_reorder_frames is equal to the DPB cap (§E.2.1),
     * MediaCodec can only throw up the frame by worst-case scenario -- actual time lag of 4 input units
     * (No B-frame flow lags 1, proving that the lynchium is indeed derived from the re-allocation assumption.
     * The ffmpeg steady state in the browser is only kept in 3 frames (H.264 rehearsing depth)
     * When you deliver the third frame, you block the first frame, the vaSyncSurface, etc.
     * Drives the bottom flush — invalid session — permanent collapse of the browser softened.
     * The fieldbox 140 is measured so only 1 frame is harded out.
     *
     * ⁇  Do not write max_num_reorder_frames 0 (ve tried, rejected):
     * That's the reordering requirement for the false stream, which undermines semantic -- soft side measurements 150 frames fall into the 115 frame.
     * The top line is the num_ref_frames: it's the frame limit of the SPS' own frame.
     * Rehearsal depth cannot exceed it, so it is a constraint** real and non-magnification**. */
    dmd_bw_put_flag(&bw, 1); /* vui_parameters_present_flag */

    dmd_bw_put_flag(&bw, 0); /* aspect_ratio_info_present_flag */
    dmd_bw_put_flag(&bw, 0); /* overscan_info_present_flag */
    dmd_bw_put_flag(&bw, 0); /* video_signal_type_present_flag */
    dmd_bw_put_flag(&bw, 0); /* chroma_loc_info_present_flag */
    dmd_bw_put_flag(&bw, 0); /* timing_info_present_flag */
    dmd_bw_put_flag(&bw, 0); /* nal_hrd_parameters_present_flag */
    dmd_bw_put_flag(&bw, 0); /* vcl_hrd_parameters_present_flag */
    dmd_bw_put_flag(&bw, 0); /* pic_struct_present_flag */

    dmd_bw_put_flag(&bw, 1); /* bitstream_restriction_flag */
    dmd_bw_put_flag(&bw, 1); /* motion_vectors_over_pic_boundaries_flag：
                          * Default is 1, Write 1 does not change semantic */
    dmd_bw_put_ue(&bw, 0);   /* Max_bytes_per_pic_denom: 0 = Unlimited */
    dmd_bw_put_ue(&bw, 0);   /* max_bits_per_mb_denom: 0 = unlimited */
    /* The blog  ⁇ mx_mv_length_horizontal/vertical: The writing code allows maximum value 15, which is not allowed in the first place.
     * The equivalent is not subject to additional restrictions. */
    dmd_bw_put_ue(&bw, 15);
    dmd_bw_put_ue(&bw, 15);
    dmd_bw_put_ue(&bw, pp->num_ref_frames); /* max_num_reorder_frames */
    dmd_bw_put_ue(&bw, pp->num_ref_frames); /* max_dec_frame_buffering */

    dmd_bw_rbsp_trailing(&bw);

    return bw.overflow ? 0 : dmd_bw_bytes(&bw);
}

/* - - - - - - - - - - - - - - - - - - - - - - PPS Serialisation */

/* Press § 7.3.2.2 to write pic_parameter_set_rbsp. */
static size_t build_pps_rbsp(const VAPictureParameterBufferH264 *pp,
                             const VAIQMatrixBufferH264 *iq, int have_iq,
                             int have_sp, unsigned int sp_l0_minus1,
                             unsigned int sp_l1_minus1, unsigned char *out,
                             size_t out_cap)
{
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, out, out_cap);

    dmd_bw_put_ue(&bw, 0); /* C:pic_parameter_set_id, consistent with the slick header quote */
    dmd_bw_put_ue(&bw, 0); /* C: seq_parameter_set_id, consistent with what is written in SPS */

    dmd_bw_put_flag(&bw, pp->pic_fields.bits.entropy_coding_mode_flag); /* A */
    /* A: pic_order_present_flag of VA-API
     * bottom_field_pic_order_in_frame_present_flag */
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.pic_order_present_flag);
    /* B: FMO does not support (VA-API header "FMO is not supported" ),
     * So the number of slick groupes is always 1. */
    dmd_bw_put_ue(&bw, 0);

    /* B：num_ref_idx_l0/l1_default_active_minus1。
     *
     * **The current frame  ⁇ um_ref_idx_l0/l1_active_minus1 **
     * And whenever these two values change, the PPS is reissued (see h264_send_param_sets for decode.c).
     *
     * Why is this correct: VA-API gives "the effective value of this slice."
     * - For slicks without num_ref_idx_active_override_flag, the effective value** equals**
     *   PPS Default Value * Copying is real
     * - The default PPS does not work for the slick with override.
     * Both cases are correct and there is no need to distinguish between override_flag (VA-API does not reveal it).
     *
     * Two ways of indefensible error have been tested for perjury:
     * 1) The first slick paraam ** IDR** -- I slice does not have this syntax element, constant 0,
     *    So B slice, which is not override, uses only one frame. This is the old bug.
     * 2) Use num_ref_frames-1 when "safe above" -- **l1 Not one person is larger**.
     *    Single variable actual (the other value remains true at l0=2/l1=0):
     *      0_m1 = 0,1  ⁇  error; 2,3,4,15  ⁇  (algesic)
     *      1_m1 = 0  ⁇  Yes; 1,2,3  ⁇  All wrong (l1 is not big)
     *    Because I've decided not to be a self-default_active one of the most important things in the "override B slick"
     *    ** Entropy decoding long**, changed to a syntax decoupling layer, not a quality prediction problem.
     *
     * - -----------------------------------------------------------------------------------
     * The reasoning "Clump the current frame" leaves out a situation: ** slick** with override.
     * For them VA-API gives value after override, regardless of PPS default,
     * So we'll write a wrong default, and the **other **not override slick will use it.
     *
     * Actual evidence: test1080.h264 real PPS constant at 1 = 2 l1 = 0 (only one original stream).
     * PPS, which is driven by 5 variants: zero 0/1/2/3, 11 0/1.
     * Only 1 0 = 2 1 = 0 is right. The opening session is exactly the same as the 0 = 0.
     * So the screens are blacked out from the fourth frame to the next IDR--
     * Every round black 25 frames around, is what users see as "every other black screen."
     *
     * Method of amendment: ** Maximum number of times taken, minimum number of times.
     * -Twenty can be larger (actually: 1-m1=2, 3, 4, 15), so the maximum is safe.
     *   and overwrites the true default value (the true value must be < = the effective value of a slice).
     * -1 No one is bigger, so minimum is required - not the override slick
     *   Real default, while override slice only returns more.
     * The only PPS that has been collected after these multiple rounds has been removed by the random repeat. */
    unsigned int def_l0 = have_sp ? sp_l0_minus1 : 0;
    unsigned int def_l1 = have_sp ? sp_l1_minus1 : 0;
    (void)pp;
    if (def_l0 > 31)
        def_l0 = 31;
    if (def_l1 > 31)
        def_l1 = 31;
    dmd_bw_put_ue(&bw, def_l0);
    dmd_bw_put_ue(&bw, def_l1);

    dmd_bw_put_flag(&bw, pp->pic_fields.bits.weighted_pred_flag);  /* A */
    dmd_bw_put_bits(&bw, pp->pic_fields.bits.weighted_bipred_idc, 2); /* A */
    dmd_bw_put_se(&bw, pp->pic_init_qp_minus26);                   /* A */
    dmd_bw_put_se(&bw, pp->pic_init_qs_minus26);                   /* A */
    dmd_bw_put_se(&bw, pp->chroma_qp_index_offset);                /* A */
    /* A */
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.deblocking_filter_control_present_flag);
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.constrained_intra_pred_flag); /* A */
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.redundant_pic_cnt_present_flag); /* A */

    /* Optional end of PPS: only if you want to express transform_8x8 or zoom in the matrix.
     * Note that these three fields are either "all or nothing" (more_rbsp_data).
     *
     * Precondition: The profile_idc / constraining_set combination on the SPS side must allow the decoder to read to
     * This tail (see instructions for the background_set in build_sps_rbsp). We're just...
     * Constrained Basin has been raised to the constraint position, and Constrained Basin is not allowed
     * Transform_8x8, so there's no conflict between need_tail and "end readable." */
    int need_tail = pp->pic_fields.bits.transform_8x8_mode_flag || have_iq ||
                    pp->second_chroma_qp_index_offset !=
                        pp->chroma_qp_index_offset;

    if (need_tail) {
        dmd_bw_put_flag(&bw, pp->pic_fields.bits.transform_8x8_mode_flag); /* A */

        /* B: Scale matrix. VAIQMatrix BufferH264 gives the final absolute matrix,
         * The bitstream is a difference in the value relative to the fallback (delta_scale).
         *
         * Here choose not to write** (pic_scaling_matrix_present_flag=0):
         * Inverted delta_scale needs to know the fallback benchmark (rule A uses default matrix, default matrix, default matrix, default matrix, default matrix, default matrix, default matrix, default matrix, default checker
         * Rule B, with the previous matrix, and "Is this matrix from the SPS or PPS?"
         * Unable to determine at VA-API level -- guessing will deflect the entire matrix.
         *
         * Consequences: If the bitstream uses a non-default scaling matrix, the counter-quantification factor is not consistent with the coding end.
         * ** Minor distortion rather than flower screen** (moderate only influences the scaling coefficient and does not affect entropy decoding)
         * . The ffmpeg default encoding does not enable custom matrices,
         * This path is not triggered by the common stream. */
        dmd_bw_put_flag(&bw, 0);

        dmd_bw_put_se(&bw, pp->second_chroma_qp_index_offset); /* A */
    }

    dmd_bw_rbsp_trailing(&bw);

    (void)iq;
    return bw.overflow ? 0 : dmd_bw_bytes(&bw);
}

/* - - - - - - - - - - - - - - - - - - - - - - External interface */

/* To assemble a NAMU:0000 01 + nal_header + RBP with a starting code.
 * nal_ref_idc fixes the parameter set (highest priority, standard recommendation). */
static size_t emit_nalu(unsigned int nal_ref_idc, unsigned int nal_unit_type,
                        const unsigned char *rbsp, size_t rbsp_len,
                        unsigned char *out, size_t out_cap)
{
    if (out_cap < 5)
        return 0;

    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x01;
    out[4] = (unsigned char)(((nal_ref_idc & 0x3) << 5) |
                             (nal_unit_type & 0x1F));

    size_t n = dmd_rbsp_escape(rbsp, rbsp_len, out + 5, out_cap - 5);
    if (n == 0 && rbsp_len > 0)
        return 0;

    return 5 + n;
}

size_t dmd_h264_build_sps_nalu(const VAPictureParameterBufferH264 *pp,
                               VAProfile profile, unsigned int disp_width,
                               unsigned int disp_height, unsigned char *out,
                               size_t out_cap)
{
    unsigned char rbsp[256];
    size_t n = build_sps_rbsp(pp, profile, disp_width, disp_height, rbsp,
                              sizeof(rbsp));
    if (n == 0)
        return 0;
    return emit_nalu(3, 7 /* SPS */, rbsp, n, out, out_cap);
}

size_t dmd_h264_build_pps_nalu(const VAPictureParameterBufferH264 *pp,
                               const VAIQMatrixBufferH264 *iq, int have_iq,
                               const VASliceParameterBufferH264 *sp,
                               unsigned int def_l0_minus1,
                               unsigned int def_l1_minus1,
                               unsigned char *out, size_t out_cap)
{
    unsigned char rbsp[256];
    /* ⁇ / l1 is passed on by the caller, ** cannot ** directly take the entry value from the sp--
     * The entry value of the slick with override_flag is not related to the PPS default.
     * See the decode.c for the deference note. */
    size_t n = build_pps_rbsp(pp, iq, have_iq, sp != NULL,
                              def_l0_minus1, def_l1_minus1, rbsp,
                              sizeof(rbsp));
    if (n == 0)
        return 0;
    return emit_nalu(3, 8 /* PPS */, rbsp, n, out, out_cap);
}
