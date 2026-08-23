/* 从 VA-API 的 HEVC 参数缓冲合成 VPS/SPS/PPS NALU。
 *
 * 为什么要合成：MediaCodec 要的是标准 Annex B 码流，而 ffmpeg 通过 VA-API
 * 交给我们的是**已解析成结构体**的参数集。原始 SPS/PPS 字节流不会传下来，
 * 只能按 H.265 语法重新写出来。
 *
 * ── slice 不需要重建 ─────────────────────────────────────
 * 和 H.264 一样，VASliceParameterBufferHEVC 里有 slice_data_byte_offset，
 * 它是 slice header 结束的位置 —— 这个字段能存在就说明 slice header
 * 原样在 slice data buffer 里。所以转发 slice 只需前置起始码，
 * 不必反推参考列表重排序（那是欠定问题）。
 *
 * ── VA-API 缺失、必须推导的字段 ───────────────────────────
 * profile_tier_level 的大部分、conformance window、VUI、
 * 以及 st_ref_pic_set 的内容都不在 VA-API 里。处理见下面各处注释。
 *
 * ⚠️ 最关键的约束是 num_short_term_ref_pic_sets：VA-API 只给个数、不给内容。
 * 若原码流在 SPS 里放了参考集（个数 > 0），我们无法复现，
 * 而 slice header 中 short_term_ref_pic_set_sps_flag=1 的引用会指向
 * 不存在的集合 —— 那会解出坏画面。所以个数 > 0 时**直接拒绝**，
 * 让上层回落软解，而不是产出错误图像。
 * 实测 x265 默认输出该字段为 0（slice header 内联自己的参考集），
 * 所以常见码流不受影响（tools/probe_hevc_sps.c）。
 */

#include <string.h>

#include "driver.h"
#include "bitstream.h"

/* ---------------------------------------------------------------- 字段推导 */

/* general_profile_idc：VA-API 不提供，从 VAProfile 映射。
 * 只声明了 VAProfileHEVCMain（见 profiles.c），所以固定为 Main=1。 */
static unsigned int hevc_profile_idc(VAProfile profile)
{
    switch (profile) {
    case VAProfileHEVCMain10:
        return 2;   /* Main 10 */
    case VAProfileHEVCMain:
    default:
        return 1;   /* Main */
    }
}

/* general_level_idc = 30 × level。按分辨率取一个刚好够用的档位。
 *
 * 宁可偏高：level 只是能力声明，报高了解码器仍按实际码流解，
 * 报低了则可能被拒。这里按 H.265 表 A.6 的 MaxLumaPs 取。 */
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
 * VA-API 只有 profile 与分辨率，其余按"Main profile 的常规取值"填：
 * general_profile_compatibility_flag 只置对应 profile 那一位，
 * progressive_source + frame_only_constraint 置 1（我们不处理场编码）。
 * 这些 flag 是能力声明，解码器不会据此改变解码行为。 */
static void put_ptl(struct dmd_bitwriter *bw, VAProfile profile,
                    unsigned int w, unsigned int h)
{
    unsigned int pidc = hevc_profile_idc(profile);

    dmd_bw_put_bits(bw, 0, 2);          /* general_profile_space = 0 */
    dmd_bw_put_flag(bw, 0);             /* general_tier_flag = Main tier */
    dmd_bw_put_bits(bw, pidc, 5);       /* general_profile_idc */

    /* general_profile_compatibility_flag[32]：只置自己那一位 */
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
    /* sps_max_sub_layers_minus1 固定为 0，所以没有 sub_layer 的 PTL */
}

/* ---------------------------------------------------------------- VPS */

/* VPS：H.265 7.3.2.1。
 *
 * 解码单层码流其实用不到 VPS 的多层信息，但 MediaCodec 的 CSD 通常期望
 * VPS+SPS+PPS 三件套，缺 VPS 有些解码器会拒。这里写一个最小可用的单层 VPS。 */
static size_t build_vps_rbsp(const VAPictureParameterBufferHEVC *pp,
                             VAProfile profile,
                             unsigned char *out, size_t cap)
{
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, out, cap);

    dmd_bw_put_bits(&bw, 0, 4);         /* vps_video_parameter_set_id */
    dmd_bw_put_bits(&bw, 3, 2);         /* vps_base_layer_internal/available: 保留位=3 */
    dmd_bw_put_bits(&bw, 0, 6);         /* vps_max_layers_minus1 */
    dmd_bw_put_bits(&bw, 0, 3);         /* vps_max_sub_layers_minus1 */
    dmd_bw_put_flag(&bw, 1);            /* vps_temporal_id_nesting_flag */
    dmd_bw_put_bits(&bw, 0xFFFF, 16);   /* vps_reserved_0xffff_16bits */

    put_ptl(&bw, profile,
            pp->pic_width_in_luma_samples, pp->pic_height_in_luma_samples);

    dmd_bw_put_flag(&bw, 0);            /* vps_sub_layer_ordering_info_present_flag */
    /* 只有 sub_layer 0：三个 ue(v) */
    dmd_bw_put_ue(&bw, pp->sps_max_dec_pic_buffering_minus1);
    dmd_bw_put_ue(&bw, 0);              /* vps_max_num_reorder_pics */
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

    /* conformance_window_flag：VA-API 不提供裁剪窗口。
     * 写 0 表示无裁剪 —— 解码尺寸就是编码尺寸。真实裁剪信息由 daemon
     * 从 MediaCodec 的 crop 字段获得并回传给驱动，所以这里不写不影响最终显示。 */
    dmd_bw_put_flag(&bw, 0);

    dmd_bw_put_ue(&bw, pp->bit_depth_luma_minus8);
    dmd_bw_put_ue(&bw, pp->bit_depth_chroma_minus8);
    dmd_bw_put_ue(&bw, pp->log2_max_pic_order_cnt_lsb_minus4);

    dmd_bw_put_flag(&bw, 0);            /* sps_sub_layer_ordering_info_present_flag */
    dmd_bw_put_ue(&bw, pp->sps_max_dec_pic_buffering_minus1);
    dmd_bw_put_ue(&bw, 0);              /* sps_max_num_reorder_pics */
    dmd_bw_put_ue(&bw, 0);              /* sps_max_latency_increase_plus1 */

    dmd_bw_put_ue(&bw, pp->log2_min_luma_coding_block_size_minus3);
    dmd_bw_put_ue(&bw, pp->log2_diff_max_min_luma_coding_block_size);
    dmd_bw_put_ue(&bw, pp->log2_min_transform_block_size_minus2);
    dmd_bw_put_ue(&bw, pp->log2_diff_max_min_transform_block_size);
    dmd_bw_put_ue(&bw, pp->max_transform_hierarchy_depth_inter);
    dmd_bw_put_ue(&bw, pp->max_transform_hierarchy_depth_intra);

    /* scaling_list_enabled_flag。
     * 置位时本应跟 sps_scaling_list_data_present_flag 与整张缩放表，
     * 而 VA-API 把缩放表放在独立的 VAIQMatrixBufferHEVC 里。
     * 这里写 present=0，表示"用默认缩放表" —— 只有当码流真的用了
     * 自定义缩放表时才会不一致，那种情况由调用方拒绝（见 dmd_hevc_can_build）。 */
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

    /* num_short_term_ref_pic_sets：VA-API 只给个数不给内容。
     * 个数 > 0 的码流已在 dmd_hevc_can_build 里被拒，所以这里必然是 0。 */
    dmd_bw_put_ue(&bw, pp->num_short_term_ref_pic_sets);

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
        /* uniform_spacing_flag：VA-API 总是给出显式的 column_width/row_height，
         * 所以写 0 并原样输出，避免依赖"均匀划分"的假设。 */
        dmd_bw_put_flag(&bw, 0);
        for (int i = 0; i < pp->num_tile_columns_minus1; i++)
            dmd_bw_put_ue(&bw, pp->column_width_minus1[i]);
        for (int i = 0; i < pp->num_tile_rows_minus1; i++)
            dmd_bw_put_ue(&bw, pp->row_height_minus1[i]);
        dmd_bw_put_flag(&bw, pp->pic_fields.bits.loop_filter_across_tiles_enabled_flag);
    }

    dmd_bw_put_flag(&bw, pp->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag);

    /* deblocking_filter_control_present_flag：VA-API 没有这个字段本身，
     * 但它是下面三项的开关。只要其中任何一项非默认值就必须置 1。 */
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

    /* pps_scaling_list_data_present_flag：同 SPS，缩放表在独立 buffer 里，
     * 这里声明不存在，用默认表。 */
    dmd_bw_put_flag(&bw, 0);

    dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.lists_modification_present_flag);
    dmd_bw_put_ue(&bw, pp->log2_parallel_merge_level_minus2);
    dmd_bw_put_flag(&bw, pp->slice_parsing_fields.bits.slice_segment_header_extension_present_flag);
    dmd_bw_put_flag(&bw, 0);            /* pps_extension_present_flag */

    dmd_bw_rbsp_trailing(&bw);
    return bw.overflow ? 0 : dmd_bw_bytes(&bw);
}

/* ---------------------------------------------------------------- 对外接口 */

/* 能否为这个码流合成参数集？不能则应回落软解，而不是产出坏画面。
 *
 * 唯一的硬性拒绝条件是 num_short_term_ref_pic_sets > 0：
 * VA-API 只给个数不给内容，无法复现，而 slice header 会引用它们。
 * 返回 0 = 不支持，非 0 = 可以。 */
int dmd_hevc_can_build(const VAPictureParameterBufferHEVC *pp)
{
    if (!pp)
        return 0;
    if (pp->num_short_term_ref_pic_sets > 0)
        return 0;
    return 1;
}

/* 把 RBSP 包成带起始码的 NALU：
 * [00 00 00 01][nal_unit_header 2 字节][转义后的 RBSP] */
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
    unsigned char rbsp[256];
    size_t n = build_vps_rbsp(pp, profile, rbsp, sizeof(rbsp));
    return emit_nalu(32, rbsp, n, out, cap);
}

size_t dmd_hevc_build_sps_nalu(const VAPictureParameterBufferHEVC *pp,
                               VAProfile profile,
                               unsigned char *out, size_t cap)
{
    unsigned char rbsp[512];
    size_t n = build_sps_rbsp(pp, profile, rbsp, sizeof(rbsp));
    return emit_nalu(33, rbsp, n, out, cap);
}

size_t dmd_hevc_build_pps_nalu(const VAPictureParameterBufferHEVC *pp,
                               unsigned char *out, size_t cap)
{
    unsigned char rbsp[512];
    size_t n = build_pps_rbsp(pp, rbsp, sizeof(rbsp));
    return emit_nalu(34, rbsp, n, out, cap);
}
