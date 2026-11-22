/* SPDX-License-Identifier: Apache-2.0
 * Hardware-free tests of VA parameter validation and translation.  Link this
 * file with encode.c, not v4l2_encoder.c: the fake below models the synchronous
 * backend boundary and never opens a device.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_vp9.h>

#include "driver.h"
#include "encode.h"
#include "v4l2_encoder.h"

struct dmd_v4l2_encoder { int live; };
static struct dmd_enc_config captured;
static unsigned opens, frames, closes;
static int open_errno, frame_errno, omit_keyframe, empty_packet;

int dmd_v4l2_enc_open(struct dmd_v4l2_encoder **out,
                      const struct dmd_enc_config *config)
{
    captured = *config;
    if (open_errno) { errno = open_errno; return -1; }
    *out = calloc(1, sizeof(**out));
    assert(*out);
    (*out)->live = 1;
    opens++;
    return 0;
}

void dmd_v4l2_enc_close(struct dmd_v4l2_encoder *enc)
{
    if (enc) { assert(enc->live); closes++; free(enc); }
}

int dmd_v4l2_enc_frame(struct dmd_v4l2_encoder *enc,
                       const uint8_t *data, size_t size,
                       unsigned stride, unsigned slice_height,
                       int force_keyframe, uint8_t *output, size_t capacity,
                       size_t *len, uint32_t *flags)
{
    assert(enc && enc->live && data && stride == 128 && slice_height == 96);
    assert(size == 128 * 96 * 3 / 2);
    frames++;
    if (frame_errno) { errno = frame_errno; return -1; }
    assert(capacity >= 4);
    memcpy(output, "CODE", 4);
    *len = empty_packet ? 0 : 4;
    *flags = force_keyframe && !omit_keyframe ? DMD_ENC_PACKET_KEYFRAME : 0;
    return 0;
}

static struct dmd_encode *new_encoder(void)
{
    struct dmd_encode *enc = dmd_encode_create(VAProfileH264High, 128, 96, VA_RC_CBR);
    assert(enc);
    return enc;
}

static VAStatus misc(struct dmd_encode *enc, VAEncMiscParameterType type,
                     const void *body, size_t size)
{
    uint8_t storage[512] = {0};
    const size_t offset = offsetof(VAEncMiscParameterBuffer, data);
    assert(offset + size <= sizeof(storage));
    memcpy(storage, &type, sizeof(type));
    memcpy(storage + offset, body, size);
    return dmd_encode_render(enc, VAEncMiscParameterBufferType, storage,
                              (unsigned)(offset + size), 1);
}

static VAEncSequenceParameterBufferH264 sequence(void)
{
    VAEncSequenceParameterBufferH264 p = {0};
    p.level_idc = 40;
    p.intra_period = p.intra_idr_period = 60;
    p.ip_period = 1;
    p.bits_per_second = 2000000;
    p.max_num_ref_frames = 1;
    p.picture_width_in_mbs = 8;
    p.picture_height_in_mbs = 6;
    p.seq_fields.bits.chroma_format_idc = 1;
    p.seq_fields.bits.frame_mbs_only_flag = 1;
    return p;
}

static void picture(struct dmd_encode *enc)
{
    VAEncPictureParameterBufferH264 p = {0};
    VAEncSliceParameterBufferH264 s = {0};
    p.coded_buf = 7;
    p.CurrPic.picture_id = 3;
    p.pic_fields.bits.idr_pic_flag = 1;
    p.pic_init_qp = 26;
    s.num_macroblocks = 48;
    s.macroblock_info = VA_INVALID_ID;
    s.slice_type = 2;
    assert(dmd_encode_render(enc, VAEncPictureParameterBufferType, &p, sizeof(p), 1) == VA_STATUS_SUCCESS);
    assert(dmd_encode_render(enc, VAEncSliceParameterBufferType, &s, sizeof(s), 1) == VA_STATUS_SUCCESS);
    assert(dmd_encode_coded_buffer(enc) == 7);
}

/* Chromium 154.0.8037.97's public VA H.264 parameter contract. Its normal
 * Linux path lets firmware generate headers and perform rate control. */
static void chrome_h264_frame(struct dmd_encode *enc, int key)
{
    VAEncSequenceParameterBufferH264 seq = sequence();
    seq.intra_period = 0;
    seq.intra_idr_period = 2048;
    seq.seq_fields.bits.log2_max_frame_num_minus4 = 7;
    seq.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 8;
    seq.vui_parameters_present_flag = 1;
    seq.vui_fields.bits.timing_info_present_flag = 1;
    seq.num_units_in_tick = 1;
    seq.time_scale = 60;
    assert(dmd_encode_render(enc, VAEncSequenceParameterBufferType,
                              &seq, sizeof(seq), 1) == VA_STATUS_SUCCESS);
    VAEncPictureParameterBufferH264 p = {0};
    VAEncSliceParameterBufferH264 s = {0};
    p.coded_buf = 7;
    p.CurrPic.picture_id = 3;
    p.pic_init_qp = 26;
    p.pic_fields.bits.idr_pic_flag = key;
    p.pic_fields.bits.reference_pic_flag = 1;
    p.pic_fields.bits.entropy_coding_mode_flag = 1;
    p.pic_fields.bits.transform_8x8_mode_flag = 1;
    p.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    s.num_macroblocks = 48;
    s.macroblock_info = VA_INVALID_ID;
    s.slice_type = key ? 2 : 0;
    s.num_ref_idx_active_override_flag = 1;
    for (unsigned i = 0; i < 16; i++) {
        p.ReferenceFrames[i].picture_id = VA_INVALID_ID;
        p.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    for (unsigned i = 0; i < 32; i++) {
        s.RefPicList0[i].picture_id = s.RefPicList1[i].picture_id = VA_INVALID_ID;
        s.RefPicList0[i].flags = s.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
    }
    if (!key) {
        p.ReferenceFrames[0].picture_id = 3;
        p.ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        s.RefPicList0[0] = p.ReferenceFrames[0];
    }
    assert(dmd_encode_render(enc, VAEncPictureParameterBufferType,
                              &p, sizeof(p), 1) == VA_STATUS_SUCCESS);
    assert(dmd_encode_render(enc, VAEncSliceParameterBufferType,
                              &s, sizeof(s), 1) == VA_STATUS_SUCCESS);
    VAEncMiscParameterRateControl rc = { .bits_per_second = 2000000,
        .target_percentage = 100, .window_size = 1500, .initial_qp = 26,
        .min_qp = 1, .max_qp = 42 };
    rc.rc_flags.bits.disable_frame_skip = 1;
    assert(misc(enc, VAEncMiscParameterTypeRateControl, &rc, sizeof(rc)) == VA_STATUS_SUCCESS);
    VAEncMiscParameterFrameRate fps = { .framerate = 30 };
    assert(misc(enc, VAEncMiscParameterTypeFrameRate, &fps, sizeof(fps)) == VA_STATUS_SUCCESS);
    VAEncMiscParameterHRD hrd = { .buffer_size = 3000000, .initial_buffer_fullness = 1500000 };
    assert(misc(enc, VAEncMiscParameterTypeHRD, &hrd, sizeof(hrd)) == VA_STATUS_SUCCESS);
}

static VAStatus deblock_picture(struct dmd_encode *enc, unsigned present,
                                unsigned mode, int alpha, int beta, int reverse)
{
    VAEncPictureParameterBufferH264 p = {0};
    VAEncSliceParameterBufferH264 s = {0};
    p.coded_buf = 7;
    p.CurrPic.picture_id = 3;
    p.pic_init_qp = 26;
    p.pic_fields.bits.idr_pic_flag = 1;
    p.pic_fields.bits.deblocking_filter_control_present_flag = present;
    s.num_macroblocks = 48;
    s.macroblock_info = VA_INVALID_ID;
    s.slice_type = 2;
    s.disable_deblocking_filter_idc = mode;
    s.slice_alpha_c0_offset_div2 = alpha;
    s.slice_beta_offset_div2 = beta;
    VAStatus status;
    if (reverse) {
        status = dmd_encode_render(enc, VAEncSliceParameterBufferType, &s, sizeof(s), 1);
        if (status != VA_STATUS_SUCCESS) return status;
    }
    status = dmd_encode_render(enc, VAEncPictureParameterBufferType, &p, sizeof(p), 1);
    if (status != VA_STATUS_SUCCESS || reverse) return status;
    return dmd_encode_render(enc, VAEncSliceParameterBufferType, &s, sizeof(s), 1);
}

static void test_deblocking(struct dmd_surface *source, struct dmd_buffer *coded)
{
    /* GStreamer 1.28.2 sends enabled/2/2. The boundary values remain signed
     * offset_div2 values, not already doubled table indices. */
    const int cases[][6] = {
        {1, 0, 2, 2, 2, 2}, {1, 2, -6, 6, -6, 6},
        {1, 1, 2, 2, 0, 0}, /* Disabled filtering has no effective offsets. */
        {0, 2, 2, 2, 0, 0}, /* PPS absent: H.264 infers enabled/0/0. */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct dmd_encode *enc = new_encoder();
        unsigned initial_opens = opens;
        for (unsigned frame = 0; frame < 2; frame++) {
            assert(dmd_encode_begin(enc, source->id) == VA_STATUS_SUCCESS);
            VAEncSequenceParameterBufferH264 seq = sequence();
            assert(dmd_encode_render(enc, VAEncSequenceParameterBufferType,
                                      &seq, sizeof(seq), 1) == VA_STATUS_SUCCESS);
            assert(deblock_picture(enc, cases[i][0], cases[i][1], cases[i][2],
                                     cases[i][3], frame == 1) == VA_STATUS_SUCCESS);
            assert(dmd_encode_end(enc, source, coded) == VA_STATUS_SUCCESS);
            assert(captured.h264_deblock_mode == (unsigned)(cases[i][0] ? cases[i][1] : 0));
            assert(captured.h264_deblock_alpha == cases[i][4]);
            assert(captured.h264_deblock_beta == cases[i][5]);
        }
        assert(opens == initial_opens + 1);
        dmd_encode_destroy(enc);
    }
    /* Fixed context controls must never silently change on the next frame.
     * A rejected transaction can be followed by the original configuration. */
    struct dmd_encode *enc = new_encoder();
    assert(dmd_encode_begin(enc, source->id) == VA_STATUS_SUCCESS);
    VAEncSequenceParameterBufferH264 seq = sequence();
    assert(dmd_encode_render(enc, VAEncSequenceParameterBufferType,
                              &seq, sizeof(seq), 1) == VA_STATUS_SUCCESS);
    assert(deblock_picture(enc, 1, 0, 2, 2, 0) == VA_STATUS_SUCCESS);
    assert(dmd_encode_end(enc, source, coded) == VA_STATUS_SUCCESS);
    unsigned fixed_opens = opens;
    const int changed[][3] = {{2, 2, 2}, {0, 1, 2}, {0, 2, -1}};
    for (unsigned i = 0; i < sizeof(changed) / sizeof(changed[0]); i++) {
        unsigned old_frames = frames;
        assert(dmd_encode_begin(enc, source->id) == VA_STATUS_SUCCESS);
        assert(deblock_picture(enc, 1, changed[i][0], changed[i][1], changed[i][2], 0) == VA_STATUS_SUCCESS);
        assert(dmd_encode_end(enc, source, coded) == VA_STATUS_ERROR_UNIMPLEMENTED);
        assert(frames == old_frames && !coded->coded_segment.size);
        assert(dmd_encode_begin(enc, source->id) == VA_STATUS_SUCCESS);
        assert(deblock_picture(enc, 1, 0, 2, 2, 0) == VA_STATUS_SUCCESS);
        assert(dmd_encode_end(enc, source, coded) == VA_STATUS_SUCCESS);
        assert(opens == fixed_opens);
    }
    dmd_encode_destroy(enc);
    const int invalid[][3] = {{3, 0, 0}, {0, -7, 0}, {0, 7, 0}, {0, 0, -7}, {0, 0, 7}};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        enc = new_encoder();
        unsigned old_frames = frames;
        assert(dmd_encode_begin(enc, source->id) == VA_STATUS_SUCCESS);
        assert(deblock_picture(enc, 1, invalid[i][0], invalid[i][1], invalid[i][2], 0) == VA_STATUS_ERROR_INVALID_PARAMETER);
        assert(dmd_encode_end(enc, source, coded) == VA_STATUS_ERROR_INVALID_PARAMETER);
        assert(frames == old_frames && !coded->coded_segment.size);
        dmd_encode_destroy(enc);
    }
    enc = new_encoder();
    assert(dmd_encode_begin(enc, source->id) == VA_STATUS_SUCCESS);
    assert(deblock_picture(enc, 1, 0, 2, 2, 0) == VA_STATUS_SUCCESS);
    VAEncSliceParameterBufferH264 extra = {0};
    assert(dmd_encode_render(enc, VAEncSliceParameterBufferType, &extra, sizeof(extra), 1) == VA_STATUS_ERROR_UNIMPLEMENTED);
    assert(dmd_encode_end(enc, source, coded) == VA_STATUS_ERROR_UNIMPLEMENTED);
    dmd_encode_destroy(enc);
    /* Missing requested controls and backend S_CTRL failures remain errors. */
    const int errors[] = {ENOTSUP, EIO};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        enc = new_encoder();
        unsigned old_frames = frames;
        assert(dmd_encode_begin(enc, source->id) == VA_STATUS_SUCCESS);
        assert(dmd_encode_render(enc, VAEncSequenceParameterBufferType,
                                  &seq, sizeof(seq), 1) == VA_STATUS_SUCCESS);
        assert(deblock_picture(enc, 1, 0, 2, 2, 0) == VA_STATUS_SUCCESS);
        open_errno = errors[i];
        assert(dmd_encode_end(enc, source, coded) == VA_STATUS_ERROR_ENCODING_ERROR);
        assert(frames == old_frames && !coded->coded_segment.size);
        assert(dmd_encode_begin(enc, source->id) == VA_STATUS_ERROR_ENCODING_ERROR);
        open_errno = 0;
        dmd_encode_destroy(enc);
    }
}

int main(void)
{
    uint8_t pixels[128 * 96 * 3 / 2] = {0}, bits[4096];
    struct dmd_surface source = { .id = 4, .width = 128, .height = 96,
        .stride = 128, .slice_height = 96, .format = VA_RT_FORMAT_YUV420,
        .data = pixels, .data_size = sizeof(pixels), .dumb_sync_fd = -1 };
    struct dmd_buffer coded = { .id = 7, .type = VAEncCodedBufferType,
        .data = bits, .size = sizeof(bits) };
    VAEncSequenceParameterBufferH264 seq = sequence();
    struct dmd_encode *enc = new_encoder();
    assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
    assert(dmd_encode_render(enc, VAEncSequenceParameterBufferType, &seq, sizeof(seq), 1) == VA_STATUS_SUCCESS);
    VAEncMiscParameterFrameRate fps = { .framerate = (1001u << 16) | 30000u };
    assert(misc(enc, VAEncMiscParameterTypeFrameRate, &fps, sizeof(fps)) == VA_STATUS_SUCCESS);
    VAEncMiscParameterRateControl rc = { .bits_per_second = 2000000,
        .target_percentage = 100, .window_size = 2000, .ICQ_quality_factor = 1 };
    rc.rc_flags.bits.mb_rate_control = 2; /* FFmpeg's CBR/VBR default */
    assert(misc(enc, VAEncMiscParameterTypeRateControl, &rc, sizeof(rc)) == VA_STATUS_SUCCESS);
    VAEncMiscParameterHRD hrd = { .buffer_size = 4000000, .initial_buffer_fullness = 2000000 };
    assert(misc(enc, VAEncMiscParameterTypeHRD, &hrd, sizeof(hrd)) == VA_STATUS_SUCCESS);
    picture(enc);
    assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_SUCCESS);
    assert(captured.profile == DMD_ENC_H264_HIGH && captured.level == 40);
    assert(captured.fps_num == 30000 && captured.fps_den == 1001);
    assert(captured.gop_size == 60 && captured.bitrate == 2000000 && captured.cbr);
    assert(captured.min_qp == 0 && captured.max_qp == 0);
    assert(!captured.h264_deblock_mode && !captured.h264_deblock_alpha && !captured.h264_deblock_beta);
    assert(captured.vbv_size_bits == 4000000 && captured.vbv_initial_bits == 2000000);
    assert(coded.coded_segment.size == 4 && !coded.coded_segment.status);
    assert(coded.coded_segment.buf == bits && !coded.coded_segment.next);
    dmd_encode_destroy(enc);

    test_deblocking(&source, &coded);

    enc = new_encoder();
    unsigned chrome_opens = opens;
    for (unsigned frame = 0; frame < 2; frame++) {
        assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
        chrome_h264_frame(enc, frame == 0);
        assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_SUCCESS);
        assert(captured.gop_size == 2048 && captured.fps_num == 30 && captured.fps_den == 1);
        assert(captured.min_qp == 1 && captured.max_qp == 42);
        assert(captured.vbv_size_bits == 3000000 && captured.vbv_initial_bits == 1500000);
    }
    assert(opens == chrome_opens + 1); /* No hidden session reset between I/P. */
    /* A changed hard QP bound must reach a reconfiguration implementation or
     * fail explicitly; this backend cannot silently keep its old bound. */
    assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
    chrome_h264_frame(enc, 0);
    VAEncMiscParameterRateControl changed_qp = { .bits_per_second = 2000000,
        .target_percentage = 100, .min_qp = 1, .max_qp = 41 };
    assert(misc(enc, VAEncMiscParameterTypeRateControl, &changed_qp, sizeof(changed_qp)) == VA_STATUS_SUCCESS);
    unsigned before_reconfigure = frames;
    assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_ERROR_UNIMPLEMENTED);
    assert(frames == before_reconfigure && !coded.coded_segment.size);
    dmd_encode_destroy(enc);

    /* Independent I/IDR cadences, an infinite GOP and B-picture periods are
     * still unsupported; supporting Chromium must not erase these checks. */
    const unsigned bad_periods[][3] = { {0, 0, 1}, {30, 60, 1}, {0, 2048, 2} };
    for (unsigned i = 0; i < sizeof(bad_periods) / sizeof(bad_periods[0]); i++) {
        enc = new_encoder();
        assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
        VAEncSequenceParameterBufferH264 bad_seq = sequence();
        bad_seq.intra_period = bad_periods[i][0];
        bad_seq.intra_idr_period = bad_periods[i][1];
        bad_seq.ip_period = bad_periods[i][2];
        assert(dmd_encode_render(enc, VAEncSequenceParameterBufferType,
                                  &bad_seq, sizeof(bad_seq), 1) == VA_STATUS_ERROR_UNIMPLEMENTED);
        assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_ERROR_UNIMPLEMENTED);
        dmd_encode_destroy(enc);
    }
    const unsigned bad_qp[][2] = { {52, 0}, {1, 52}, {43, 42} };
    for (unsigned i = 0; i < sizeof(bad_qp) / sizeof(bad_qp[0]); i++) {
        enc = new_encoder();
        assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
        VAEncMiscParameterRateControl bad_rc = { .bits_per_second = 2000000,
            .target_percentage = 100, .min_qp = bad_qp[i][0], .max_qp = bad_qp[i][1] };
        assert(misc(enc, VAEncMiscParameterTypeRateControl, &bad_rc, sizeof(bad_rc)) == VA_STATUS_ERROR_INVALID_PARAMETER);
        assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_ERROR_INVALID_PARAMETER);
        dmd_encode_destroy(enc);
    }
    const VAProfile other_profiles[] = { VAProfileHEVCMain, VAProfileVP9Profile0 };
    for (unsigned i = 0; i < sizeof(other_profiles) / sizeof(other_profiles[0]); i++) {
        enc = dmd_encode_create(other_profiles[i], 128, 96, VA_RC_CBR);
        assert(enc && dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
        VAEncMiscParameterRateControl unsupported_qp = { .bits_per_second = 2000000,
            .target_percentage = 100, .min_qp = 1, .max_qp = 42 };
        assert(misc(enc, VAEncMiscParameterTypeRateControl, &unsupported_qp,
                      sizeof(unsupported_qp)) == VA_STATUS_ERROR_UNIMPLEMENTED);
        assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_ERROR_UNIMPLEMENTED);
        dmd_encode_destroy(enc);
    }

    /* Real FFmpeg supplies a 64-aligned VP9 reconstruction context while the
     * source and VP9 sequence still describe the visible 128x96 picture. */
    enc = dmd_encode_create(VAProfileVP9Profile0, 128, 128, VA_RC_VBR);
    assert(enc);
    assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
    VAEncSequenceParameterBufferVP9 vp9_seq = { .max_frame_width = 128,
        .max_frame_height = 96, .intra_period = 60, .bits_per_second = 2000000 };
    VAEncPictureParameterBufferVP9 vp9_pic = { .frame_width_src = 128,
        .frame_height_src = 96, .frame_width_dst = 128, .frame_height_dst = 96,
        .reconstructed_frame = 3, .coded_buf = 7 };
    vp9_pic.pic_flags.bits.show_frame = 1;
    vp9_pic.ref_flags.bits.force_kf = 1;
    assert(dmd_encode_render(enc, VAEncSequenceParameterBufferType,
                              &vp9_seq, sizeof(vp9_seq), 1) == VA_STATUS_SUCCESS);
    VAEncMiscParameterRateControl vbr = { .bits_per_second = 4000000,
        .target_percentage = 50, .window_size = 2000, .ICQ_quality_factor = 1 };
    vbr.rc_flags.bits.mb_rate_control = 2;
    vbr.rc_flags.bits.disable_bit_stuffing = 1;
    assert(misc(enc, VAEncMiscParameterTypeRateControl, &vbr, sizeof(vbr)) == VA_STATUS_SUCCESS);
    assert(dmd_encode_render(enc, VAEncPictureParameterBufferType,
                              &vp9_pic, sizeof(vp9_pic), 1) == VA_STATUS_SUCCESS);
    assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_SUCCESS);
    assert(captured.width == 128 && captured.height == 96 && !captured.cbr);
    assert(captured.bitrate == 2000000 && captured.peak_bitrate == 4000000);
    assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
    vp9_pic.pic_flags.bits.frame_type = 1;
    vp9_pic.ref_flags.bits.force_kf = 0;
    vp9_pic.ref_flags.bits.ref_frame_ctrl_l0 = 1;
    vp9_pic.ref_flags.bits.ref_last_sign_bias = 1; /* FFmpeg's normal P syntax */
    vp9_pic.reference_frames[0] = 3;
    /* A repeated sequence supplies the initial bitrate again. The persistent
     * misc RC override must continue to win at subsequent GOP boundaries. */
    assert(dmd_encode_render(enc, VAEncSequenceParameterBufferType,
                              &vp9_seq, sizeof(vp9_seq), 1) == VA_STATUS_SUCCESS);
    assert(dmd_encode_render(enc, VAEncPictureParameterBufferType,
                              &vp9_pic, sizeof(vp9_pic), 1) == VA_STATUS_SUCCESS);
    assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_SUCCESS);
    dmd_encode_destroy(enc);

    /* Missing sequence and malformed NV12 geometry cannot submit hardware IO. */
    unsigned old_frames = frames;
    enc = new_encoder();
    assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
    picture(enc);
    assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_ERROR_INVALID_PARAMETER);
    assert(!coded.coded_segment.size && (coded.coded_segment.status & VA_CODED_BUF_STATUS_BAD_BITSTREAM));
    assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
    assert(dmd_encode_render(enc, VAEncSequenceParameterBufferType, &seq, sizeof(seq), 1) == VA_STATUS_SUCCESS);
    picture(enc);
    source.data_size--;
    assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_ERROR_INVALID_SURFACE);
    source.data_size++;
    assert(frames == old_frames);
    dmd_encode_destroy(enc);

    /* A rejected B-frame/10-bit/packed-header transaction cannot accidentally
     * finish successfully if its caller overlooks RenderPicture's error. */
    enc = new_encoder();
    assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
    seq.ip_period = 3;
    assert(dmd_encode_render(enc, VAEncSequenceParameterBufferType, &seq, sizeof(seq), 1) == VA_STATUS_ERROR_UNIMPLEMENTED);
    assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_ERROR_UNIMPLEMENTED);
    seq = sequence();
    assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
    seq.bit_depth_luma_minus8 = 2;
    assert(dmd_encode_render(enc, VAEncSequenceParameterBufferType, &seq, sizeof(seq), 1) == VA_STATUS_ERROR_UNIMPLEMENTED);
    assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_ERROR_UNIMPLEMENTED);
    assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
    uint32_t packed = 0;
    assert(dmd_encode_render(enc, VAEncPackedHeaderDataBufferType, &packed, sizeof(packed), 1) == VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
    assert(dmd_encode_end(enc, &source, &coded) == VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
    dmd_encode_destroy(enc);

    /* An output overflow or empty/mislabelled key packet is a failure, and
     * the damaged GOP must not silently resume on the next BeginPicture. */
    seq = sequence();
    for (unsigned failure = 0; failure < 3; failure++) {
        enc = new_encoder();
        assert(dmd_encode_begin(enc, source.id) == VA_STATUS_SUCCESS);
        assert(dmd_encode_render(enc, VAEncSequenceParameterBufferType, &seq, sizeof(seq), 1) == VA_STATUS_SUCCESS);
        picture(enc);
        frame_errno = failure == 0 ? ENOSPC : 0;
        omit_keyframe = failure == 1;
        empty_packet = failure == 2;
        VAStatus want = failure == 0 ? VA_STATUS_ERROR_NOT_ENOUGH_BUFFER : VA_STATUS_ERROR_ENCODING_ERROR;
        assert(dmd_encode_end(enc, &source, &coded) == want);
        assert(!coded.coded_segment.size && coded.coded_segment.status);
        assert(dmd_encode_begin(enc, source.id) == VA_STATUS_ERROR_ENCODING_ERROR);
        dmd_encode_destroy(enc);
    }
    assert(opens == closes);
    puts("encode frontend: parameter translation, strict rejection and failure state PASS");
    return 0;
}
