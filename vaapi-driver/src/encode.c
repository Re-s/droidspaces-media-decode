/* SPDX-License-Identifier: Apache-2.0
 * VA-API encode parameters -> in-process V4L2 stateful encoder.
 *
 * Written against the public libva and V4L2 interfaces.  There is no Android
 * service, IPC transport, MediaCodec dependency, or copied termux-va code.
 */
#include <errno.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <va/va.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_enc_vp9.h>

#include "driver.h"
#include "encode.h"
#include "v4l2_encoder.h"

struct dmd_encode {
    VAProfile profile;
    unsigned rate_control;
    unsigned context_width, context_height;
    struct dmd_enc_config config, opened_config;
    struct dmd_v4l2_encoder *backend;
    struct dmd_video_stats *video_stats;
    VASurfaceID source, reconstructed, last_reconstructed;
    VABufferID coded;
    int begun, failed, have_sequence, have_picture, have_slice, have_rate_control;
    int keyframe, have_previous;
    VAStatus picture_error;
    unsigned ctu_size;
    union {
        VAEncSequenceParameterBufferH264 h264;
        VAEncSequenceParameterBufferHEVC hevc;
        VAEncSequenceParameterBufferVP9 vp9;
    } sequence;
    union {
        VAEncPictureParameterBufferH264 h264;
        VAEncPictureParameterBufferHEVC hevc;
        VAEncPictureParameterBufferVP9 vp9;
    } picture;
    union {
        VAEncSliceParameterBufferH264 h264;
        VAEncSliceParameterBufferHEVC hevc;
    } slice;
};

static int is_h264(const struct dmd_encode *e)
{
    return e->profile == VAProfileH264ConstrainedBaseline ||
           e->profile == VAProfileH264Main || e->profile == VAProfileH264High;
}

static int same_config(const struct dmd_enc_config *a,
                       const struct dmd_enc_config *b)
{
    return a->profile == b->profile && a->width == b->width &&
           a->height == b->height && a->bitrate == b->bitrate &&
           a->peak_bitrate == b->peak_bitrate &&
           a->fps_num == b->fps_num && a->fps_den == b->fps_den &&
           a->gop_size == b->gop_size && a->cbr == b->cbr &&
           a->min_qp == b->min_qp && a->max_qp == b->max_qp &&
           a->fixed_qp == b->fixed_qp &&
           a->h264_deblock_mode == b->h264_deblock_mode &&
           a->h264_deblock_alpha == b->h264_deblock_alpha &&
           a->h264_deblock_beta == b->h264_deblock_beta &&
           a->level == b->level && a->vbv_size_bits == b->vbv_size_bits &&
           a->vbv_initial_bits == b->vbv_initial_bits;
}

static unsigned gcd_u(unsigned a, unsigned b)
{
    while (b) { unsigned t = a % b; a = b; b = t; }
    return a;
}

static VAStatus set_fps(struct dmd_enc_config *cfg, unsigned num, unsigned den)
{
    if (!num || !den || (uint64_t)num > (uint64_t)den * 1000u)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned divisor = gcd_u(num, den);
    cfg->fps_num = num / divisor;
    cfg->fps_den = den / divisor;
    return VA_STATUS_SUCCESS;
}

struct dmd_encode *dmd_encode_create(VAProfile profile, unsigned width,
                                      unsigned height, unsigned rate_control)
{
    enum dmd_enc_profile backend_profile;
    switch (profile) {
    case VAProfileH264ConstrainedBaseline: backend_profile = DMD_ENC_H264_BASELINE; break;
    case VAProfileH264Main: backend_profile = DMD_ENC_H264_MAIN; break;
    case VAProfileH264High: backend_profile = DMD_ENC_H264_HIGH; break;
    case VAProfileHEVCMain: backend_profile = DMD_ENC_HEVC_MAIN; break;
    case VAProfileVP9Profile0: backend_profile = DMD_ENC_VP9_0; break;
    default: errno = ENOTSUP; return NULL;
    }
    if (!width || !height || width > DMD_MAX_WIDTH || height > DMD_MAX_HEIGHT ||
        (width & 1) || (height & 1) ||
        (rate_control != VA_RC_CBR && rate_control != VA_RC_VBR &&
         (rate_control != VA_RC_CQP || profile == VAProfileVP9Profile0))) {
        errno = EINVAL;
        return NULL;
    }
    struct dmd_encode *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->profile = profile;
    e->rate_control = rate_control;
    e->context_width = width;
    e->context_height = height;
    e->config.profile = backend_profile;
    e->config.width = width;
    e->config.height = height;
    e->config.fps_num = 30;
    e->config.fps_den = 1;
    e->config.cbr = rate_control == VA_RC_CBR;
    if (rate_control == VA_RC_CQP) e->config.fixed_qp = 26;
    e->source = e->reconstructed = e->last_reconstructed = VA_INVALID_ID;
    e->coded = VA_INVALID_ID;
    return e;
}

void dmd_encode_destroy(struct dmd_encode *e)
{
    if (!e) return;
    dmd_v4l2_enc_close(e->backend);
    dmd_video_stats_unref(e->video_stats);
    free(e);
}

void dmd_encode_set_video_stats(struct dmd_encode *e, struct dmd_video_stats *stats)
{
    if (!e || e->video_stats == stats) return;
    dmd_video_stats_unref(e->video_stats);
    e->video_stats = dmd_video_stats_ref(stats);
}

void dmd_encode_abort(struct dmd_encode *e)
{
    if (!e) return;
    e->begun = e->have_picture = e->have_slice = e->keyframe = 0;
    e->source = e->coded = e->reconstructed = VA_INVALID_ID;
    e->picture_error = VA_STATUS_SUCCESS;
    memset(&e->picture, 0, sizeof(e->picture));
    memset(&e->slice, 0, sizeof(e->slice));
}

VAStatus dmd_encode_begin(struct dmd_encode *e, VASurfaceID source)
{
    if (!e || source == VA_INVALID_ID)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (e->failed) return VA_STATUS_ERROR_ENCODING_ERROR;
    if (e->begun) return VA_STATUS_ERROR_OPERATION_FAILED;
    e->begun = 1;
    e->source = source;
    e->coded = e->reconstructed = VA_INVALID_ID;
    e->have_picture = e->have_slice = e->keyframe = 0;
    e->picture_error = VA_STATUS_SUCCESS;
    memset(&e->picture, 0, sizeof(e->picture));
    memset(&e->slice, 0, sizeof(e->slice));
    return VA_STATUS_SUCCESS;
}

/* intra_period=0 disables periodic non-IDR I pictures; it does not disable
 * the separate IDR cadence. Chromium's normal IP stream uses 0/2048/1.
 * Independent nonzero I and IDR periods still need two hardware controls,
 * whereas this backend exposes one closed-GOP period. */
static VAStatus sequence_gop(struct dmd_enc_config *cfg, unsigned intra,
                             unsigned idr, unsigned ip)
{
    unsigned period = intra ? intra : idr;
    if (!period || period > INT_MAX || (intra && idr && idr != intra) ||
        (ip != 1 && !(period == 1 && ip == 0)))
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    cfg->gop_size = period;
    return VA_STATUS_SUCCESS;
}

static VAStatus render_sequence(struct dmd_encode *e, const void *data,
                                 unsigned size)
{
    struct dmd_enc_config cfg = e->config;
    VAStatus status;
    if (is_h264(e)) {
        VAEncSequenceParameterBufferH264 p;
        if (size < sizeof(p)) return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&p, data, sizeof(p));
        if (p.seq_fields.bits.chroma_format_idc != 1 ||
            !p.seq_fields.bits.frame_mbs_only_flag ||
            p.seq_fields.bits.mb_adaptive_frame_field_flag ||
            p.seq_fields.bits.seq_scaling_matrix_present_flag ||
            p.bit_depth_luma_minus8 || p.bit_depth_chroma_minus8 ||
            p.max_num_ref_frames > 1)
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        if (!p.picture_width_in_mbs || !p.picture_height_in_mbs ||
            p.picture_width_in_mbs != (e->context_width + 15) / 16 ||
            p.picture_height_in_mbs != (e->context_height + 15) / 16)
            return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
        unsigned coded_width = (unsigned)p.picture_width_in_mbs * 16u;
        unsigned coded_height = (unsigned)p.picture_height_in_mbs * 16u;
        if (p.frame_cropping_flag &&
            (p.frame_crop_left_offset || p.frame_crop_top_offset ||
             p.frame_crop_right_offset > 7 || p.frame_crop_bottom_offset > 7))
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        /* FFmpeg passes macroblock-aligned dimensions to CreateContext, but
         * the uploaded source retains its visible dimensions.  The SPS crop
         * is the authoritative visible rectangle for H.264. */
        cfg.width = p.frame_cropping_flag ? coded_width - 2 * p.frame_crop_right_offset :
                    (coded_width < e->context_width ? coded_width : e->context_width);
        cfg.height = p.frame_cropping_flag ? coded_height - 2 * p.frame_crop_bottom_offset :
                     (coded_height < e->context_height ? coded_height : e->context_height);
        if (cfg.width > e->context_width || cfg.height > e->context_height)
            return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
        status = sequence_gop(&cfg, p.intra_period, p.intra_idr_period, p.ip_period);
        if (status != VA_STATUS_SUCCESS) return status;
        if (!e->have_rate_control) cfg.bitrate = p.bits_per_second;
        cfg.level = p.level_idc;
        if (p.vui_parameters_present_flag) {
            if (p.vui_fields.bits.aspect_ratio_info_present_flag &&
                p.aspect_ratio_idc != 0 && p.aspect_ratio_idc != 1 &&
                !(p.aspect_ratio_idc == 255 && p.sar_width && p.sar_width == p.sar_height))
                return VA_STATUS_ERROR_UNIMPLEMENTED;
            if (p.vui_fields.bits.timing_info_present_flag) {
                if (!p.num_units_in_tick || p.num_units_in_tick > UINT_MAX / 2)
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                status = set_fps(&cfg, p.time_scale, 2 * p.num_units_in_tick);
                if (status != VA_STATUS_SUCCESS) return status;
            }
        }
        e->sequence.h264 = p;
    } else if (e->profile == VAProfileHEVCMain) {
        VAEncSequenceParameterBufferHEVC p;
        if (size < sizeof(p)) return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&p, data, sizeof(p));
        if (p.general_profile_idc != 1 || p.general_tier_flag ||
            p.seq_fields.bits.chroma_format_idc != 1 ||
            p.seq_fields.bits.separate_colour_plane_flag ||
            p.seq_fields.bits.bit_depth_luma_minus8 ||
            p.seq_fields.bits.bit_depth_chroma_minus8 ||
            p.seq_fields.bits.pcm_enabled_flag ||
            p.seq_fields.bits.hierachical_flag ||
            p.seq_fields.bits.scaling_list_enabled_flag || p.scc_fields.value)
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        if (!p.pic_width_in_luma_samples || !p.pic_height_in_luma_samples ||
            p.pic_width_in_luma_samples > e->context_width ||
            p.pic_height_in_luma_samples > e->context_height)
            return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
        cfg.width = p.pic_width_in_luma_samples;
        cfg.height = p.pic_height_in_luma_samples;
        unsigned ctu_log2 = 3u + p.log2_min_luma_coding_block_size_minus3 +
                            p.log2_diff_max_min_luma_coding_block_size;
        if (ctu_log2 < 4 || ctu_log2 > 6)
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        status = sequence_gop(&cfg, p.intra_period, p.intra_idr_period, p.ip_period);
        if (status != VA_STATUS_SUCCESS) return status;
        if (!e->have_rate_control) cfg.bitrate = p.bits_per_second;
        cfg.level = p.general_level_idc;
        if (p.vui_parameters_present_flag) {
            if (p.vui_fields.bits.field_seq_flag ||
                (p.vui_fields.bits.aspect_ratio_info_present_flag &&
                 p.aspect_ratio_idc != 0 && p.aspect_ratio_idc != 1 &&
                 !(p.aspect_ratio_idc == 255 && p.sar_width && p.sar_width == p.sar_height)))
                return VA_STATUS_ERROR_UNIMPLEMENTED;
            if (p.vui_fields.bits.vui_timing_info_present_flag) {
                status = set_fps(&cfg, p.vui_time_scale, p.vui_num_units_in_tick);
                if (status != VA_STATUS_SUCCESS) return status;
            }
        }
        e->ctu_size = 1u << ctu_log2;
        e->sequence.hevc = p;
    } else {
        VAEncSequenceParameterBufferVP9 p;
        if (size < sizeof(p)) return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&p, data, sizeof(p));
        if (!p.max_frame_width || !p.max_frame_height ||
            p.max_frame_width > e->context_width || p.max_frame_height > e->context_height ||
            (p.max_frame_width & 1) || (p.max_frame_height & 1))
            return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
        cfg.width = p.max_frame_width;
        cfg.height = p.max_frame_height;
        if (p.kf_auto || p.kf_min_dist ||
            (p.kf_max_dist && p.kf_max_dist != p.intra_period))
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        status = sequence_gop(&cfg, p.intra_period, p.intra_period, 1);
        if (status != VA_STATUS_SUCCESS) return status;
        if (!e->have_rate_control) cfg.bitrate = p.bits_per_second;
        e->sequence.vp9 = p;
    }
    e->config = cfg;
    e->have_sequence = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus render_picture(struct dmd_encode *e, const void *data,
                                unsigned size)
{
    if (e->have_picture) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (is_h264(e)) {
        VAEncPictureParameterBufferH264 p;
        if (size < sizeof(p)) return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&p, data, sizeof(p));
        if (p.pic_init_qp > 51 || p.num_ref_idx_l0_active_minus1 ||
            p.num_ref_idx_l1_active_minus1 || p.chroma_qp_index_offset ||
            p.second_chroma_qp_index_offset ||
            p.pic_fields.bits.weighted_pred_flag || p.pic_fields.bits.weighted_bipred_idc ||
            p.pic_fields.bits.pic_scaling_matrix_present_flag ||
            (p.CurrPic.flags & (VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD |
                                VA_PICTURE_H264_LONG_TERM_REFERENCE)))
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        e->picture.h264 = p;
        e->coded = p.coded_buf;
        e->reconstructed = p.CurrPic.picture_id;
        e->keyframe = p.pic_fields.bits.idr_pic_flag;
    } else if (e->profile == VAProfileHEVCMain) {
        VAEncPictureParameterBufferHEVC p;
        if (size < sizeof(p)) return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&p, data, sizeof(p));
        if (p.pic_init_qp > 51 || p.pic_fields.bits.coding_type < 1 ||
            p.pic_fields.bits.coding_type > 2 ||
            (p.pic_fields.bits.idr_pic_flag && p.pic_fields.bits.coding_type != 1) ||
            p.num_ref_idx_l0_default_active_minus1 ||
            p.num_ref_idx_l1_default_active_minus1 ||
            p.pps_cb_qp_offset || p.pps_cr_qp_offset ||
            p.pic_fields.bits.dependent_slice_segments_enabled_flag ||
            p.pic_fields.bits.weighted_pred_flag || p.pic_fields.bits.weighted_bipred_flag ||
            p.pic_fields.bits.transquant_bypass_enabled_flag ||
            p.pic_fields.bits.tiles_enabled_flag || p.num_tile_columns_minus1 ||
            p.num_tile_rows_minus1 || p.pic_fields.bits.scaling_list_data_present_flag ||
            p.pic_fields.bits.screen_content_flag ||
            p.pic_fields.bits.enable_gpu_weighted_prediction ||
            p.hierarchical_level_plus1 > 1 || p.scc_fields.value ||
            (p.decoded_curr_pic.flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE))
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        e->picture.hevc = p;
        e->coded = p.coded_buf;
        e->reconstructed = p.decoded_curr_pic.picture_id;
        e->keyframe = p.pic_fields.bits.idr_pic_flag || p.pic_fields.bits.coding_type == 1;
    } else {
        VAEncPictureParameterBufferVP9 p;
        if (size < sizeof(p)) return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&p, data, sizeof(p));
        if (p.frame_width_src != e->config.width || p.frame_height_src != e->config.height ||
            p.frame_width_dst != e->config.width || p.frame_height_dst != e->config.height)
            return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
        if (!p.pic_flags.bits.show_frame || p.pic_flags.bits.intra_only ||
            p.pic_flags.bits.segmentation_enabled ||
            p.pic_flags.bits.segmentation_temporal_update || p.pic_flags.bits.segmentation_update_map ||
            p.pic_flags.bits.lossless_mode || p.pic_flags.bits.comp_prediction_mode ||
            p.pic_flags.bits.super_frame_flag || p.ref_flags.bits.temporal_id ||
            p.ref_flags.bits.ref_frame_ctrl_l1 ||
            p.log2_tile_rows || p.log2_tile_columns || p.skip_frame_flag ||
            p.number_skip_frames || p.skip_frames_size || p.luma_dc_qindex_delta ||
            p.chroma_ac_qindex_delta || p.chroma_dc_qindex_delta ||
            p.bit_offset_ref_lf_delta || p.bit_offset_mode_lf_delta || p.bit_offset_lf_level ||
            p.bit_offset_qindex || p.bit_offset_first_partition_size ||
            p.bit_offset_segmentation || p.bit_size_segmentation)
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        e->picture.vp9 = p;
        e->coded = p.coded_buf;
        e->reconstructed = p.reconstructed_frame;
        e->keyframe = p.ref_flags.bits.force_kf || !p.pic_flags.bits.frame_type;
    }
    if (e->coded == VA_INVALID_ID) return VA_STATUS_ERROR_INVALID_BUFFER;
    if (e->reconstructed == VA_INVALID_ID) return VA_STATUS_ERROR_INVALID_SURFACE;
    e->have_picture = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus render_slice(struct dmd_encode *e, const void *data, unsigned size)
{
    if (e->have_slice) return VA_STATUS_ERROR_UNIMPLEMENTED;
    if (is_h264(e)) {
        VAEncSliceParameterBufferH264 p;
        if (size < sizeof(p)) return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&p, data, sizeof(p));
        unsigned type = p.slice_type % 5;
        if (p.slice_type > 9 || (type != 0 && type != 2) ||
            p.macroblock_address ||
            p.num_macroblocks != ((e->config.width + 15) / 16) * ((e->config.height + 15) / 16) ||
            p.macroblock_info != VA_INVALID_ID || p.slice_qp_delta ||
            p.num_ref_idx_l0_active_minus1 || p.num_ref_idx_l1_active_minus1 ||
            p.luma_weight_l0_flag || p.chroma_weight_l0_flag ||
            p.luma_weight_l1_flag || p.chroma_weight_l1_flag)
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        if (p.disable_deblocking_filter_idc > 2 ||
            p.slice_alpha_c0_offset_div2 < -6 || p.slice_alpha_c0_offset_div2 > 6 ||
            p.slice_beta_offset_div2 < -6 || p.slice_beta_offset_div2 > 6)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        e->slice.h264 = p;
    } else if (e->profile == VAProfileHEVCMain) {
        VAEncSliceParameterBufferHEVC p;
        if (size < sizeof(p)) return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&p, data, sizeof(p));
        if (!e->have_sequence) return VA_STATUS_ERROR_INVALID_PARAMETER;
        unsigned ctu = e->ctu_size;
        if ((p.slice_type != 1 && p.slice_type != 2) || p.slice_segment_address ||
            p.num_ctu_in_slice != ((e->config.width + ctu - 1) / ctu) *
                                  ((e->config.height + ctu - 1) / ctu) ||
            !p.slice_fields.bits.last_slice_of_pic_flag ||
            p.slice_fields.bits.dependent_slice_segment_flag || p.slice_fields.bits.colour_plane_id ||
            p.num_ref_idx_l0_active_minus1 || p.num_ref_idx_l1_active_minus1 ||
            p.slice_qp_delta || p.slice_cb_qp_offset || p.slice_cr_qp_offset ||
            p.slice_beta_offset_div2 || p.slice_tc_offset_div2 ||
            p.pred_weight_table_bit_offset || p.pred_weight_table_bit_length)
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        e->slice.hevc = p;
    } else return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    e->have_slice = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus render_misc(struct dmd_encode *e, const void *data, unsigned size)
{
    const size_t offset = offsetof(VAEncMiscParameterBuffer, data);
    if (size < offset) return VA_STATUS_ERROR_INVALID_BUFFER;
    VAEncMiscParameterType type;
    memcpy(&type, data, sizeof(type));
    const uint8_t *body = (const uint8_t *)data + offset;
    size_t len = size - offset;
    switch (type) {
    case VAEncMiscParameterTypeFrameRate: {
        VAEncMiscParameterFrameRate p;
        if (len < sizeof(p)) return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&p, body, sizeof(p));
        if (p.framerate_flags.value) return VA_STATUS_ERROR_UNIMPLEMENTED;
        unsigned den = p.framerate >> 16;
        return set_fps(&e->config, p.framerate & 0xffffu, den ? den : 1);
    }
    case VAEncMiscParameterTypeRateControl: {
        VAEncMiscParameterRateControl p;
        if (len < sizeof(p)) return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&p, body, sizeof(p));
        if (e->rate_control == VA_RC_CQP) {
            unsigned qp = p.initial_qp ? p.initial_qp
                        : p.quality_factor ? p.quality_factor : p.ICQ_quality_factor;
            if (qp > 51) return VA_STATUS_ERROR_INVALID_PARAMETER;
            if (qp) e->config.fixed_qp = qp;
            return VA_STATUS_SUCCESS;
        }
        /* initial_qp is an RC starting suggestion, not a fixed-QP request.
         * H.264 min/max QP are hard constraints: pass them to the backend,
         * which must apply and verify the controls before accepting frames.
         * Other codecs have no QP-limit implementation and must reject them.
         * ICQ/quality fields are inactive in CBR/VBR (FFmpeg fills ICQ=1 even
         * in CBR), and mb_rate_control=2 is its normal non-MB-RC request. */
        if (!p.bits_per_second || p.bits_per_second > INT_MAX || p.target_percentage > 100 ||
            p.initial_qp > (e->profile == VAProfileVP9Profile0 ? 255u : 51u))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (p.min_qp || p.max_qp) {
            if (!is_h264(e)) return VA_STATUS_ERROR_UNIMPLEMENTED;
            if (p.min_qp > 51 || p.max_qp > 51 ||
                (p.min_qp && p.max_qp && p.min_qp > p.max_qp))
                return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (p.basic_unit_size || p.target_frame_size ||
            (p.rc_flags.bits.mb_rate_control != 0 && p.rc_flags.bits.mb_rate_control != 2) ||
            p.rc_flags.bits.temporal_id ||
            p.rc_flags.bits.cfs_I_frames || p.rc_flags.bits.enable_parallel_brc ||
            p.rc_flags.bits.enable_dynamic_scaling || p.rc_flags.bits.frame_tolerance_mode ||
            p.rc_flags.bits.reserved ||
            (e->config.cbr && p.rc_flags.bits.disable_bit_stuffing))
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        /* No frame dropping is ever used by this synchronous backend, so
         * disable_frame_skip=1 is already satisfied.  A requested RC reset
         * needs a new context; reopening mid-GOP would break references. */
        if (p.rc_flags.bits.reset && e->backend)
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        unsigned percentage = p.target_percentage ? p.target_percentage : 100;
        uint64_t target = e->config.cbr ? p.bits_per_second :
                          (uint64_t)p.bits_per_second * percentage / 100u;
        if (!target || target > INT_MAX) return VA_STATUS_ERROR_INVALID_PARAMETER;
        e->config.bitrate = (unsigned)target;
        e->config.peak_bitrate = e->config.cbr ? 0 : p.bits_per_second;
        e->config.min_qp = p.min_qp;
        e->config.max_qp = p.max_qp;
        e->have_rate_control = 1;
        if (p.window_size) {
            uint64_t vbv = (uint64_t)p.bits_per_second * p.window_size / 1000u;
            if (!vbv || vbv > UINT_MAX) return VA_STATUS_ERROR_INVALID_PARAMETER;
            e->config.vbv_size_bits = (unsigned)vbv;
        }
        return VA_STATUS_SUCCESS;
    }
    case VAEncMiscParameterTypeHRD: {
        VAEncMiscParameterHRD p;
        if (len < sizeof(p)) return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&p, body, sizeof(p));
        if (p.initial_buffer_fullness > p.buffer_size)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        e->config.vbv_size_bits = p.buffer_size;
        e->config.vbv_initial_bits = p.initial_buffer_fullness;
        return VA_STATUS_SUCCESS;
    }
    case VAEncMiscParameterTypeQualityLevel: {
        VAEncMiscParameterBufferQualityLevel p;
        if (len < sizeof(p)) return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&p, body, sizeof(p));
        return p.quality_level ? VA_STATUS_ERROR_UNIMPLEMENTED : VA_STATUS_SUCCESS;
    }
    default: return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
}

VAStatus dmd_encode_render(struct dmd_encode *e, VABufferType type,
                            const void *data, unsigned size, unsigned num_elements)
{
    if (!e || !e->begun || !data || !size || !num_elements)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (e->picture_error != VA_STATUS_SUCCESS) return e->picture_error;
    VAStatus result;
    if (num_elements != 1) result = VA_STATUS_ERROR_UNIMPLEMENTED;
    else switch (type) {
    case VAEncSequenceParameterBufferType: result = render_sequence(e, data, size); break;
    case VAEncPictureParameterBufferType: result = render_picture(e, data, size); break;
    case VAEncSliceParameterBufferType: result = render_slice(e, data, size); break;
    case VAEncMiscParameterBufferType: result = render_misc(e, data, size); break;
    case VAQMatrixBufferType: {
        /* VP9 requires this buffer even without segmentation.  Only an all
         * zero map is supported; no app-selected per-segment QP or reference. */
        result = VA_STATUS_ERROR_UNIMPLEMENTED;
        if (e->profile == VAProfileVP9Profile0 &&
            size >= sizeof(VAEncMiscParameterTypeVP9PerSegmantParam)) {
            const uint8_t *bytes = data;
            result = VA_STATUS_SUCCESS;
            for (size_t i = 0; i < sizeof(VAEncMiscParameterTypeVP9PerSegmantParam); i++)
                if (bytes[i]) { result = VA_STATUS_ERROR_UNIMPLEMENTED; break; }
        }
        break;
    }
    default: result = VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE; break;
    }
    if (result != VA_STATUS_SUCCESS) e->picture_error = result;
    return result;
}

VABufferID dmd_encode_coded_buffer(const struct dmd_encode *e)
{
    return e && e->begun && e->have_picture ? e->coded : VA_INVALID_ID;
}

static VAStatus validate_references(struct dmd_encode *e)
{
    if (e->keyframe) return VA_STATUS_SUCCESS;
    if (!e->have_previous) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (is_h264(e)) {
        const VAPictureH264 *refs = e->picture.h264.ReferenceFrames;
        for (unsigned i = 0; i < 16; i++) {
            if (refs[i].picture_id == VA_INVALID_ID || (refs[i].flags & VA_PICTURE_H264_INVALID))
                continue;
            if (refs[i].picture_id != e->last_reconstructed ||
                (refs[i].flags & (VA_PICTURE_H264_LONG_TERM_REFERENCE |
                                  VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD)))
                return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        if (e->slice.h264.RefPicList0[0].picture_id != e->last_reconstructed)
            return VA_STATUS_ERROR_UNIMPLEMENTED;
    } else if (e->profile == VAProfileHEVCMain) {
        const VAPictureHEVC *refs = e->picture.hevc.reference_frames;
        for (unsigned i = 0; i < 15; i++) {
            if (refs[i].picture_id == VA_INVALID_ID || (refs[i].flags & VA_PICTURE_HEVC_INVALID))
                continue;
            if (refs[i].picture_id != e->last_reconstructed ||
                (refs[i].flags & (VA_PICTURE_HEVC_LONG_TERM_REFERENCE | VA_PICTURE_HEVC_RPS_ST_CURR_AFTER)))
                return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        if (e->slice.hevc.ref_pic_list0[0].picture_id != e->last_reconstructed)
            return VA_STATUS_ERROR_UNIMPLEMENTED;
    } else {
        const VAEncPictureParameterBufferVP9 *p = &e->picture.vp9;
        unsigned ctrl = p->ref_flags.bits.ref_frame_ctrl_l0;
        if (ctrl != 1 || p->reference_frames[p->ref_flags.bits.ref_last_idx] != e->last_reconstructed)
            return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus encode_failure(struct dmd_encode *e, int error)
{
    e->failed = 1;
    dmd_v4l2_enc_close(e->backend);
    e->backend = NULL;
    return error == ENOSPC ? VA_STATUS_ERROR_NOT_ENOUGH_BUFFER : VA_STATUS_ERROR_ENCODING_ERROR;
}

VAStatus dmd_encode_end(struct dmd_encode *e, const struct dmd_surface *source,
                         struct dmd_buffer *coded)
{
    if (!e || !e->begun) return VA_STATUS_ERROR_INVALID_PARAMETER;
    e->begun = 0;
    if (coded) {
        memset(&coded->coded_segment, 0, sizeof(coded->coded_segment));
        coded->coded_segment.buf = coded->data;
        coded->coded_segment.status = VA_CODED_BUF_STATUS_BAD_BITSTREAM;
    }
    if (e->failed) return VA_STATUS_ERROR_ENCODING_ERROR;
    if (e->picture_error != VA_STATUS_SUCCESS) return e->picture_error;
    if (!e->have_sequence || !e->have_picture || (is_h264(e) && !e->have_slice) ||
        (e->profile == VAProfileHEVCMain && !e->have_slice))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!coded || coded->id != e->coded || coded->type != VAEncCodedBufferType ||
        !coded->data || !coded->size || coded->mapped)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (!source || source->id != e->source || !source->data ||
        source->format != VA_RT_FORMAT_YUV420)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    /* VA's HEVC sequence structure omits conformance-window offsets.
     * FFmpeg creates a padded reconstruction context; its input surface is
     * the remaining source of visible geometry.  Use it for that padding,
     * while never allowing an application-requested downscale. */
    if (e->profile == VAProfileHEVCMain) {
        if (source->width < e->config.width) {
            if (e->config.width - source->width >= 16)
                return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
            e->config.width = source->width;
        }
        if (source->height < e->config.height) {
            if (e->config.height - source->height >= 16)
                return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
            e->config.height = source->height;
        }
    }
    if (source->width < e->config.width || source->height < e->config.height ||
        source->stride < source->width || source->slice_height < source->height ||
        (source->stride & 1) || (source->slice_height & 1))
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    if (source->stride > SIZE_MAX / source->slice_height)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    size_t plane_size = (size_t)source->stride * source->slice_height;
    if (plane_size > SIZE_MAX - plane_size / 2)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    size_t need = plane_size + plane_size / 2;
    if (need > source->data_size)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (e->rate_control == VA_RC_CQP) {
        unsigned qp = is_h264(e) ? e->picture.h264.pic_init_qp
                                 : e->picture.hevc.pic_init_qp;
        if (qp) e->config.fixed_qp = qp;
        e->config.bitrate = 2000000;
        e->config.peak_bitrate = e->config.bitrate;
    }
    if (!e->config.bitrate || e->config.bitrate > INT_MAX)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (is_h264(e)) {
        unsigned type = e->slice.h264.slice_type % 5;
        if (type == 2) e->keyframe = 1;
        else if (e->keyframe) return VA_STATUS_ERROR_INVALID_PARAMETER;
        /* Resolve the effective syntax after both picture and slice buffers
         * have arrived; VA clients need not submit these buffer types in order.
         * H.264 infers enabled/0/0 if PPS omits deblocking controls. Offsets
         * have no effect while the loop filter is disabled. */
        unsigned mode = e->picture.h264.pic_fields.bits.deblocking_filter_control_present_flag
                      ? e->slice.h264.disable_deblocking_filter_idc : 0;
        e->config.h264_deblock_mode = mode;
        e->config.h264_deblock_alpha =
            e->picture.h264.pic_fields.bits.deblocking_filter_control_present_flag && mode != 1
            ? e->slice.h264.slice_alpha_c0_offset_div2 : 0;
        e->config.h264_deblock_beta =
            e->picture.h264.pic_fields.bits.deblocking_filter_control_present_flag && mode != 1
            ? e->slice.h264.slice_beta_offset_div2 : 0;
    } else if (e->profile == VAProfileHEVCMain) {
        unsigned type = e->slice.hevc.slice_type;
        if ((type == 2) != (e->picture.hevc.pic_fields.bits.coding_type == 1))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    VAStatus status = validate_references(e);
    if (status != VA_STATUS_SUCCESS) return status;
    if (e->backend && !same_config(&e->config, &e->opened_config))
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    dmd_video_stats_config(e->video_stats, e->config.width, e->config.height,
                            e->config.fps_num, e->config.fps_den);
    if (!e->backend) {
        errno = 0;
        int rc = dmd_v4l2_enc_open(&e->backend, &e->config);
        if (rc < 0) return encode_failure(e, rc < -1 ? -rc : errno);
        e->opened_config = e->config;
    }
    /* External callers may hand us a CPU-mapped DRM surface.  Read access must
     * be bracketed even if the driver previously wrote that surface itself.
     * Failure is observable: encoding stale GPU data is never called success. */
    int sync_fd = source->exportable ? source->dumb_sync_fd : -1;
    if (sync_fd >= 0) {
        struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
        if (ioctl(sync_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    size_t length = 0;
    uint32_t flags = 0;
    errno = 0;
    int rc = dmd_v4l2_enc_frame(e->backend, source->data, need,
                                source->stride, source->slice_height,
                                e->keyframe, coded->data, coded->size,
                                &length, &flags);
    int error = rc < -1 ? -rc : errno;
    if (rc == 0 && length && length <= coded->size)
        dmd_video_stats_hw(e->video_stats);
    if (sync_fd >= 0) {
        struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
        if (ioctl(sync_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0 && rc >= 0) {
            rc = -1;
            error = EIO;
        }
    }
    if (rc < 0) {
        if (error == ENOSPC) coded->coded_segment.status = VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK;
        return encode_failure(e, error);
    }
    if (!length || length > coded->size || length > UINT32_MAX ||
        (e->keyframe && !(flags & DMD_ENC_PACKET_KEYFRAME)))
        return encode_failure(e, length > coded->size ? ENOSPC : EIO);
    coded->coded_segment.size = (uint32_t)length;
    coded->coded_segment.status = 0;
    e->last_reconstructed = e->reconstructed;
    e->have_previous = 1;
    return VA_STATUS_SUCCESS;
}
