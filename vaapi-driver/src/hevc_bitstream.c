/* Composes the HEVC parameter from VA-API to a buffer VPS/SPS/PPS NALU.
 *
 * Why synthesis: V4L2 with state decoding is for standard Annex B stream, and ffmpeg through VA-API
 * The original SPS/PPS byte will not pass down, but will be the most important part of the story.
 * It can only be rewritten in H.265.
 *
 * Original slick header and CABAC load are still in slick data buffer. Source SPS does not
 * Maintain the original forwarding path when short reference set; when table is available, the POC from VA DPB
 * Recreate the equivalency set with the RPS tag, replace only the corresponding header bits and PPS id, keep other
 * Header field and CABAC load with re-do byte alignment, emulsion preparation.
 *
 * ─--------------------------------------------------------------------------------------------------------
 * Most of the profile_tier_level, Conformance window, VUI,
 * And the contents of st_ref_pic_set are not in VA-API. See the notes below for this treatment.
 *
 * VA-API only gives SPS short reference series number, not table content, so the new SPS table is empty.
 * Could not copy the SPS index directly from the old slick. The new path requires a full long-format slice parameter;
 * The missing long-term SPS tables, custom scaling matrices, etc. are still categorically rejected without extrapolating the necessary parameters.
 */

#include <string.h>
#include <stdlib.h>

#include "driver.h"
#include "bitstream.h"

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - field reasoning */

/* General_profile_idc: VA-API does not provide, from VAProfile map.
 * The entrance will check the profile/ bit depth and the unknown profile cannot be silently declared as Main. */
static unsigned int hevc_profile_idc(VAProfile profile)
{
    switch (profile) {
    case VAProfileHEVCMain10:
        return 2;   /* Main 10 */
    case VAProfileHEVCMain:
        return 1;   /* Main */
    default:
        return 0;
    }
}

/* General_level_idc = 30 x level. Take a slate by resolution.
 *
 * It's better than being tall: the power statement is just that the reporting understanding code still runs with actual code.
 * If you understate it, you may be refused. Here you can use Max LumaPs in Table H.265 A.6. */
static unsigned int hevc_level_idc(unsigned int w, unsigned int h)
{
    unsigned int luma = w * h;
    if (luma <=   36864) return 30;   /* 1.0  176x144 */
    if (luma <=  122880) return 60;   /* 2.0  352x288 */
    if (luma <=  245760) return 63;   /* 2.1  640x360 */
    if (luma <=  552960) return 90;   /* 3.0  960x540 */
    if (luma <=  983040) return 93;   /* 3.1  1280x720 */
    if (luma <= 2228224) return 120;  /* 4.0  2048x1080 */
    if (luma <= 8912896) return 150;  /* 5.0  4096x2160 */
    return 180;                       /* 6.0  8192x4320 */
}

/* profile_tier_level()：H.265 7.3.3。
 *
 * VA-API Only profile with resolution, the rest is filled with "General take-out for Main profile":
 * Set only the bit for the selected profile in general_profile_compatibility_flag,
 * The program is now being used to create a new system for the use of the Internet.
 * These flags are power statements, and the decoder will not change the decoding behavior. */
static void put_ptl(struct dmd_bitwriter *bw, VAProfile profile,
                    unsigned int w, unsigned int h)
{
    unsigned int pidc = hevc_profile_idc(profile);

    dmd_bw_put_bits(bw, 0, 2);          /* general_profile_space = 0 */
    dmd_bw_put_flag(bw, 0);             /* general_tier_flag = Main tier */
    dmd_bw_put_bits(bw, pidc, 5);       /* general_profile_idc */

    /* general_profile_compatibility_flag[32]: set only the bit for this profile. */
    for (int i = 0; i < 32; i++)
        dmd_bw_put_flag(bw, i == (int)pidc);

    dmd_bw_put_flag(bw, 1);             /* general_progressive_source_flag */
    dmd_bw_put_flag(bw, 0);             /* general_interlaced_source_flag */
    dmd_bw_put_flag(bw, 0);             /* general_non_packed_constraint_flag */
    dmd_bw_put_flag(bw, 1);             /* general_frame_only_constraint_flag */

    /* general_reserved_zero_43bits */
    dmd_bw_put_bits(bw, 0, 22);
    dmd_bw_put_bits(bw, 0, 21);
    /* general_inbld_flag / reserved_zero_bit */
    dmd_bw_put_flag(bw, 0);

    dmd_bw_put_bits(bw, hevc_level_idc(w, h), 8);   /* general_level_idc */
    /* sps_max_sub_layers_minus1 fixed to 0, so there is no PTL for sub_layer */
}

/* ---------------------------------------------------------------- VPS */

/* VPS：H.265 7.3.2.1。
 *
 * Decoding the single layer stream is not functional enough to provide VPS multilayer information, but MediaCodec's CSD usually expects
 * VPS+SPS+PPS sets, VPS missing, some decoding devices are rejected. Write a minimum available single layer of VPS here. */
static size_t build_vps_rbsp(const VAPictureParameterBufferHEVC *pp,
                             VAProfile profile,
                             unsigned char *out, size_t cap)
{
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, out, cap);

    dmd_bw_put_bits(&bw, 0, 4);         /* vps_video_parameter_set_id */
    dmd_bw_put_bits(&bw, 3, 2);         /* vps_base_layer_international/aviable: reserved = 3 */
    dmd_bw_put_bits(&bw, 0, 6);         /* vps_max_layers_minus1 */
    dmd_bw_put_bits(&bw, 0, 3);         /* vps_max_sub_layers_minus1 */
    dmd_bw_put_flag(&bw, 1);            /* vps_temporal_id_nesting_flag */
    dmd_bw_put_bits(&bw, 0xFFFF, 16);   /* vps_reserved_0xffff_16bits */

    put_ptl(&bw, profile,
            pp->pic_width_in_luma_samples, pp->pic_height_in_luma_samples);

    dmd_bw_put_flag(&bw, 0);            /* vps_sub_layer_ordering_info_present_flag */
    /* Sub_layer 0: three ue (v) */
    dmd_bw_put_ue(&bw, pp->sps_max_dec_pic_buffering_minus1);
    /* ⁇ : Chrome H265Decoder by sps_max_num_reorder_pics
     * When OutputPicture. Write 0. It'll get out every frame and it'll be a picture.
     * Decoding sequence 0:04:2 §1. VA-API does not have the absolute value of this field, just for
     * NoPicReplacedingFlag;exceeding the DPB cap without a scaling
     * (Same strategy as H.264 for synthetic SPS with num_ref_frames). */
    dmd_bw_put_ue(&bw, pp->pic_fields.bits.NoPicReorderingFlag
                          ? 0u : (unsigned)pp->sps_max_dec_pic_buffering_minus1);
    dmd_bw_put_ue(&bw, 0);              /* vps_max_latency_increase_plus1 */

    dmd_bw_put_bits(&bw, 0, 6);         /* vps_max_layer_id */
    dmd_bw_put_ue(&bw, 0);              /* vps_num_layer_sets_minus1 */
    dmd_bw_put_flag(&bw, 0);            /* vps_timing_info_present_flag */
    dmd_bw_put_flag(&bw, 0);            /* vps_extension_flag */

    dmd_bw_rbsp_trailing(&bw);
    return bw.overflow ? 0 : dmd_bw_bytes(&bw);
}

/* ---------------------------------------------------------------- SPS */

/* SPS：H.265 7.3.2.2。 */
static size_t build_sps_rbsp(const VAPictureParameterBufferHEVC *pp,
                             VAProfile profile,
                             unsigned char *out, size_t cap)
{
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, out, cap);

    dmd_bw_put_bits(&bw, 0, 4);         /* sps_video_parameter_set_id */
    dmd_bw_put_bits(&bw, 0, 3);         /* sps_max_sub_layers_minus1 */
    dmd_bw_put_flag(&bw, 1);            /* sps_temporal_id_nesting_flag */

    put_ptl(&bw, profile,
            pp->pic_width_in_luma_samples, pp->pic_height_in_luma_samples);

    dmd_bw_put_ue(&bw, 0);              /* sps_seq_parameter_set_id */
    dmd_bw_put_ue(&bw, pp->pic_fields.bits.chroma_format_idc);
    if (pp->pic_fields.bits.chroma_format_idc == 3)
        dmd_bw_put_flag(&bw, pp->pic_fields.bits.separate_colour_plane_flag);

    dmd_bw_put_ue(&bw, pp->pic_width_in_luma_samples);
    dmd_bw_put_ue(&bw, pp->pic_height_in_luma_samples);

    /* The crop window is not available for the user.
     * Write 0 for no crop -- decoding size is code size. Real crop information is by daemon
     * Gets and returns the drive from the mediaCodec field. Therefore, there is no effect on the final display. */
    dmd_bw_put_flag(&bw, 0);

    dmd_bw_put_ue(&bw, pp->bit_depth_luma_minus8);
    dmd_bw_put_ue(&bw, pp->bit_depth_chroma_minus8);
    dmd_bw_put_ue(&bw, pp->log2_max_pic_order_cnt_lsb_minus4);

    dmd_bw_put_flag(&bw, 0);            /* sps_sub_layer_ordering_info_present_flag */
    dmd_bw_put_ue(&bw, pp->sps_max_dec_pic_buffering_minus1);
    /* sps_max_num_reorder_pics / sps_max_latency_increase_plus1。
     *
     * ─ Test findings: these two values have no effect on the decoding results driven by this — — — — — — — — — — — — — — — — — — — — — — — — —
     * Single variable actual (jinjie_265.mp460 frames, real SPS for reorder=2 latency=5,
     * Confirm bytes with DMD_CSD_DUMP that the overlay is actually written into the bits stream:
     *   Reorder = 0,1,2,4,7 * all md5 PASS
     *   Latency = 0, 5, 9 * all md5 PASS
     * Reason: Hardware takes the OTPUT_ORDER=1 (decoding sequence output), reordered to the consumer's DPB,
     * MediaCodec side does not determine the timing of the vomiting based on these two values in the CSD.
     *
     * So don't think of this place as a fixation point for the "framework problem" -- there was a round of speculation.
     * "O will make the decoding machine slate, the image becomes 0  ⁇ 4  ⁇ 2" and then write the DPB cap.
     * That change is not actually valid (md5 same as PASS).
     * Keep writing the DPB cap just because it's closer to the real value than writing 0, not because it fixes anything.
     *
     * VA-API does not have the values for these two fields, only for NopicReording Flag,
     * So the original value cannot be restored; the DPB ceiling is used as a non-magnification upper limit when not set. */
    dmd_bw_put_ue(&bw, pp->pic_fields.bits.NoPicReorderingFlag
                          ? 0u : (unsigned)pp->sps_max_dec_pic_buffering_minus1);
    dmd_bw_put_ue(&bw, 0);              /* sps_max_latency_increase_plus1 */

    dmd_bw_put_ue(&bw, pp->log2_min_luma_coding_block_size_minus3);
    dmd_bw_put_ue(&bw, pp->log2_diff_max_min_luma_coding_block_size);
    dmd_bw_put_ue(&bw, pp->log2_min_transform_block_size_minus2);
    dmd_bw_put_ue(&bw, pp->log2_diff_max_min_transform_block_size);
    dmd_bw_put_ue(&bw, pp->max_transform_hierarchy_depth_inter);
    dmd_bw_put_ue(&bw, pp->max_transform_hierarchy_depth_intra);

    /* scaling_list_enabled_flag。
     * The position should be with sps_scaling_list_data_present_flag and the entire scalator.
     * And VA-API puts the scale table in a separate VAIQMatrix BufferHeVC.
     * Here is the present=0 for "Default Scale table" -- only if the stream really works.
     * The list is not consistent when defined, and that is rejected by the caller (see dmd_hevc_can_build). */
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.scaling_list_enabled_flag);
    if (pp->pic_fields.bits.scaling_list_enabled_flag)
        dmd_bw_put_flag(&bw, 0);        /* sps_scaling_list_data_present_flag */

    dmd_bw_put_flag(&bw, pp->pic_fields.bits.amp_enabled_flag);
    dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag);

    dmd_bw_put_flag(&bw, pp->pic_fields.bits.pcm_enabled_flag);
    if (pp->pic_fields.bits.pcm_enabled_flag) {
        dmd_bw_put_bits(&bw, pp->pcm_sample_bit_depth_luma_minus1, 4);
        dmd_bw_put_bits(&bw, pp->pcm_sample_bit_depth_chroma_minus1, 4);
        dmd_bw_put_ue(&bw, pp->log2_min_pcm_luma_coding_block_size_minus3);
        dmd_bw_put_ue(&bw, pp->log2_diff_max_min_pcm_luma_coding_block_size);
        dmd_bw_put_flag(&bw, pp->pic_fields.bits.pcm_loop_filter_disabled_flag);
    }

    /* VA-API does not provide original SPS reference set definition; hevc_slice.c will be supported
     * Main/ Main10 8/10 bit stream has been recast as a reference set in the visible slice, so here you can declare 0 SPS pools.
     * Keeping old pool numbers without providing definitions undermines subsequent grammar interpretation. */
    dmd_bw_put_ue(&bw, 0);

    dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag);
    if (pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag)
        dmd_bw_put_ue(&bw, pp->num_long_term_ref_pic_sps);

    dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag);
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.strong_intra_smoothing_enabled_flag);

    dmd_bw_put_flag(&bw, 0);            /* vui_parameters_present_flag */
    dmd_bw_put_flag(&bw, 0);            /* sps_extension_present_flag */

    dmd_bw_rbsp_trailing(&bw);
    return bw.overflow ? 0 : dmd_bw_bytes(&bw);
}

/* ---------------------------------------------------------------- PPS */

/* PPS：H.265 7.3.2.3。 */
static size_t build_pps_rbsp(const VAPictureParameterBufferHEVC *pp,
                             unsigned char *out, size_t cap)
{
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, out, cap);

    dmd_bw_put_ue(&bw, 0);              /* pps_pic_parameter_set_id */
    dmd_bw_put_ue(&bw, 0);              /* pps_seq_parameter_set_id */

    dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag);
    dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.output_flag_present_flag);
    dmd_bw_put_bits(&bw, pp->num_extra_slice_header_bits, 3);
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.sign_data_hiding_enabled_flag);
    dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.cabac_init_present_flag);

    dmd_bw_put_ue(&bw, pp->num_ref_idx_l0_default_active_minus1);
    dmd_bw_put_ue(&bw, pp->num_ref_idx_l1_default_active_minus1);
    dmd_bw_put_se(&bw, pp->init_qp_minus26);

    dmd_bw_put_flag(&bw, pp->pic_fields.bits.constrained_intra_pred_flag);
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.transform_skip_enabled_flag);

    dmd_bw_put_flag(&bw, pp->pic_fields.bits.cu_qp_delta_enabled_flag);
    if (pp->pic_fields.bits.cu_qp_delta_enabled_flag)
        dmd_bw_put_ue(&bw, pp->diff_cu_qp_delta_depth);

    dmd_bw_put_se(&bw, pp->pps_cb_qp_offset);
    dmd_bw_put_se(&bw, pp->pps_cr_qp_offset);

    dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag);
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.weighted_pred_flag);
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.weighted_bipred_flag);
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.transquant_bypass_enabled_flag);
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.tiles_enabled_flag);
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.entropy_coding_sync_enabled_flag);

    if (pp->pic_fields.bits.tiles_enabled_flag) {
        dmd_bw_put_ue(&bw, pp->num_tile_columns_minus1);
        dmd_bw_put_ue(&bw, pp->num_tile_rows_minus1);
        /* Unform_spacing_flag: VA-API always gives a clear column_width/row_right, and then the word "save" is used to be used to make a clear-cut message.
         * So write 0 and produce as it is, avoiding relying on the assumption of "altitude." */
        dmd_bw_put_flag(&bw, 0);
        for (int i = 0; i < pp->num_tile_columns_minus1; i++)
            dmd_bw_put_ue(&bw, pp->column_width_minus1[i]);
        for (int i = 0; i < pp->num_tile_rows_minus1; i++)
            dmd_bw_put_ue(&bw, pp->row_height_minus1[i]);
        dmd_bw_put_flag(&bw, pp->pic_fields.bits.loop_filter_across_tiles_enabled_flag);
    }

    dmd_bw_put_flag(&bw, pp->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag);

    /* VA-API has no deblocking_filter_control_present_flag field,
     * But it is a switch for the following three. Any one of these is not the default value. */
    int dbf_ctrl = pp->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag ||
                   pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag ||
                   pp->pps_beta_offset_div2 || pp->pps_tc_offset_div2;
    dmd_bw_put_flag(&bw, dbf_ctrl);
    if (dbf_ctrl) {
        dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag);
        dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag);
        if (!pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag) {
            dmd_bw_put_se(&bw, pp->pps_beta_offset_div2);
            dmd_bw_put_se(&bw, pp->pps_tc_offset_div2);
        }
    }

    /* The blog is a blog of the blog "The World of Youth" (Standing), which is a blog of the Global Voices community.
     * Here you can state that it does not exist. Use the default table. */
    dmd_bw_put_flag(&bw, 0);

    dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.lists_modification_present_flag);
    dmd_bw_put_ue(&bw, pp->log2_parallel_merge_level_minus2);
    dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.slice_segment_header_extension_present_flag);
    dmd_bw_put_flag(&bw, 0);            /* pps_extension_present_flag */

    dmd_bw_rbsp_trailing(&bw);
    return bw.overflow ? 0 : dmd_bw_bytes(&bw);
}

/* - - - - - - - - - - - - - - - - - - - - - - - External interfaces */

/* Support check at the argument set level; slick rewrite will be independently validated when the source SPS short reference table is available
 * Long-format arguments with DPB. No field can be shown to be unsuspecting. returns 0 = not supported. */
int dmd_hevc_can_build(const VAPictureParameterBufferHEVC *pp)
{
    if (!pp)
        return 0;
    /* The pixel path represents only 4:2:0 with matched 8- or 10-bit planes.
     * In particular, a Main10 profile does not make 9/12-bit or mixed-depth
     * syntax safe. PCM samples, when present, cannot exceed the SPS depth.
     * Syntax checked against FFmpeg n8.0 hevc/ps.c ff_hevc_parse_sps(). */
    if (pp->pic_fields.bits.chroma_format_idc != 1 ||
        pp->pic_fields.bits.separate_colour_plane_flag ||
        pp->bit_depth_luma_minus8 != pp->bit_depth_chroma_minus8 ||
        (pp->bit_depth_luma_minus8 != 0 && pp->bit_depth_luma_minus8 != 2) ||
        (pp->pic_fields.bits.pcm_enabled_flag &&
         (pp->pcm_sample_bit_depth_luma_minus1 >= 8u + pp->bit_depth_luma_minus8 ||
          pp->pcm_sample_bit_depth_chroma_minus1 >= 8u + pp->bit_depth_chroma_minus8)))
        return 0;
    if (pp->num_short_term_ref_pic_sets > 0) {
        /* No original LT SPS table or custom scaling-list bytes are exposed
         * here. The new path handles matched 8/10-bit full long-format slices and
         * LT syntax whose source SPS table is empty; do not invent the rest. */
        if (pp->num_short_term_ref_pic_sets > 64 || pp->num_long_term_ref_pic_sps ||
            pp->pic_fields.bits.chroma_format_idc != 1 ||
            pp->pic_fields.bits.separate_colour_plane_flag ||
            pp->pic_fields.bits.scaling_list_enabled_flag ||
            (pp->CurrPic.flags & (VA_PICTURE_HEVC_FIELD_PIC | VA_PICTURE_HEVC_BOTTOM_FIELD)))
            return 0;
    }
    return 1;
}

static int hevc_profile_matches_picture(const VAPictureParameterBufferHEVC *pp,
                                        VAProfile profile)
{
    if (!dmd_hevc_can_build(pp))
        return 0;
    /* Main10 PTL also permits 8-bit pictures. Keep the SPS sample depth from
     * the matched luma/chroma parameters rather than deriving it from PTL;
     * the decode frontend separately selects NV12 or P010 for that depth. */
    switch (profile) {
    case VAProfileHEVCMain:
        return pp->bit_depth_luma_minus8 == 0;
    case VAProfileHEVCMain10:
        return pp->bit_depth_luma_minus8 == 0 || pp->bit_depth_luma_minus8 == 2;
    default:
        return 0;
    }
}

/* Wrap RBPP into a NALU with a starting code:
 * [00:00 01] [nal_unit_head 2 bytes] [RBSP as transposition] */
static size_t emit_nalu(unsigned int nal_unit_type,
                        const unsigned char *rbsp, size_t rbsp_len,
                        unsigned char *out, size_t cap)
{
    if (rbsp_len == 0 || cap < 6)
        return 0;

    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1;
    /* forbidden_zero(1) type(6) layer_id(6) temporal_id_plus1(3) */
    out[4] = (unsigned char)((nal_unit_type & 0x3f) << 1);
    out[5] = 1;

    size_t n = dmd_rbsp_escape(rbsp, rbsp_len, out + 6, cap - 6);
    if (n == 0)
        return 0;
    return 6 + n;
}

size_t dmd_hevc_build_vps_nalu(const VAPictureParameterBufferHEVC *pp,
                               VAProfile profile,
                               unsigned char *out, size_t cap)
{
    if (!hevc_profile_matches_picture(pp, profile))
        return 0;
    unsigned char rbsp[256];
    size_t n = build_vps_rbsp(pp, profile, rbsp, sizeof(rbsp));
    return emit_nalu(32, rbsp, n, out, cap);
}

size_t dmd_hevc_build_sps_nalu(const VAPictureParameterBufferHEVC *pp,
                               VAProfile profile,
                               unsigned char *out, size_t cap)
{
    if (!hevc_profile_matches_picture(pp, profile))
        return 0;
    unsigned char rbsp[512];
    size_t n = build_sps_rbsp(pp, profile, rbsp, sizeof(rbsp));
    return emit_nalu(33, rbsp, n, out, cap);
}

size_t dmd_hevc_build_pps_nalu(const VAPictureParameterBufferHEVC *pp,
                               unsigned char *out, size_t cap)
{
    if (!dmd_hevc_can_build(pp))
        return 0;
    unsigned char rbsp[512];
    size_t n = build_pps_rbsp(pp, rbsp, sizeof(rbsp));
    return emit_nalu(34, rbsp, n, out, cap);
}
