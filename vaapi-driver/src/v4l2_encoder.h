/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DMD_V4L2_ENCODER_H
#define DMD_V4L2_ENCODER_H

#include <stddef.h>
#include <stdint.h>

enum dmd_enc_profile {
    DMD_ENC_H264_BASELINE = 0,
    DMD_ENC_H264_MAIN,
    DMD_ENC_H264_HIGH,
    DMD_ENC_HEVC_MAIN,
    DMD_ENC_VP9_0,
};
#define DMD_ENC_CAP(p) (1u << (p))
#define DMD_ENC_PACKET_KEYFRAME 1u

struct dmd_enc_config {
    enum dmd_enc_profile profile;
    unsigned width, height, bitrate, fps_num, fps_den, gop_size;
    int cbr;
    /* Bitstream level_idc (H.264 10..62; HEVC 30..186), zero = default. */
    unsigned level;
    /* Optional rate-control hints, in bits. Unsupported hints retain the
     * hardware's rate-control defaults and are not fatal. */
    unsigned vbv_size_bits, vbv_initial_bits;
    /* Optional VBR peak hint in bits/second; no hard ceiling is promised. */
    unsigned peak_bitrate;
    /* H.264 rate-control QP constraints, 1..51; zero leaves that bound
     * unspecified. A requested bound must be set and read back exactly. */
    unsigned min_qp, max_qp;
    unsigned fixed_qp; /* CQP over VBR, 1..51; zero selects ordinary rate control. */
    /* Effective H.264 slice deblocking syntax: 0=enabled, 1=disabled,
     * 2=disabled across slice boundaries; alpha/beta are offset_div2 (-6..6),
     * not doubled filter-table offsets. Fixed for the encoder context.
     * Zero defaults retain compatibility with devices lacking these controls;
     * nondefault requests require verified kernel controls. */
    unsigned h264_deblock_mode;
    int h264_deblock_alpha, h264_deblock_beta;
};

struct dmd_v4l2_encoder;

/* Probe only the selected device (DMD_VA_ENCODE_DEVICE, default video33).
 * The result describes kernel formats/profiles, not a successful frame test. */
uint32_t dmd_v4l2_enc_probe(void);
/* Optional kernel-advertised coded-size bounds; -1 if unavailable. */
int dmd_v4l2_enc_get_limits(enum dmd_enc_profile profile,
                            unsigned *min_width, unsigned *min_height,
                            unsigned *max_width, unsigned *max_height);

/* All operations return 0 on success, -1 and errno on failure.  An error
 * while encoding poisons the session: close and reopen before submitting
 * another frame.  This backend does not use a daemon, MediaCodec or sockets. */
int dmd_v4l2_enc_open(struct dmd_v4l2_encoder **out,
                      const struct dmd_enc_config *cfg);
int dmd_v4l2_enc_frame(struct dmd_v4l2_encoder *enc,
                       const uint8_t *data, size_t size,
                       unsigned stride, unsigned slice_height,
                       int force_keyframe, uint8_t *output, size_t capacity,
                       size_t *len, uint32_t *flags);
void dmd_v4l2_enc_close(struct dmd_v4l2_encoder *enc);

#endif
