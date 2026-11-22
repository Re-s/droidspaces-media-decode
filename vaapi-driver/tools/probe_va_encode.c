/* SPDX-License-Identifier: Apache-2.0
 * Exercise the real VA driver and V4L2 encoder without linking libva.
 * Usage: probe_va_encode PREFIX [WIDTH HEIGHT FRAMES [cbr|vbr]]
 * Writes PREFIX.h264, PREFIX.hevc and, when advertised, PREFIX.ivf.
 * DMD_PROBE_CHROME_STYLE=1 submits Chromium 154-style H.264 parameters:
 * no intermediate I period, IDR=2048, 30 fps, QP bounds 1..42, 1500 ms HRD.
 * HEVC/VP9 and the positional command-line interface remain unchanged.
 * This is a device probe, not a replacement for decoding and checking output.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver.h"
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_enc_vp9.h>

extern VAStatus __vaDriverInit_1_0(VADriverContextP ctx);

struct probe {
    struct VADriverContext va;
    struct VADriverVTable vt;
    VAContextID context;
    unsigned width, height;
    VASurfaceID source;
    unsigned completed;
    int chrome_style;
};

static void require(struct probe *p, VAStatus status, const char *stage)
{
    if (status == VA_STATUS_SUCCESS) return;
    fprintf(stderr, "%s: VAStatus=0x%x after %u frames\n", stage, status, p->completed);
    if (p->vt.vaTerminate) p->vt.vaTerminate(&p->va);
    exit(1);
}
#define VA(p, expr) require((p), (expr), #expr)

static VABufferID buffer(struct probe *p, VABufferType type, size_t size, const void *data)
{
    VABufferID id = VA_INVALID_ID;
    VA(p, p->vt.vaCreateBuffer(&p->va, p->context, type, (unsigned)size, 1,
                               (void *)data, &id));
    return id;
}

static void render(struct probe *p, VABufferType type, const void *data, size_t size)
{
    VABufferID id = buffer(p, type, size, data);
    VA(p, p->vt.vaRenderPicture(&p->va, p->context, &id, 1));
    VA(p, p->vt.vaDestroyBuffer(&p->va, id));
}

static void chrome_h264_rate_control(struct probe *p)
{
    /* Chromium 154 h264_vaapi_video_encoder_delegate.cc:
     * CreateVAEncRateControlParams(), kCPBWindowSizeMs, kMinQP/kMaxQP.
     * RC/HRD sizes are bits; window_size is milliseconds. Submit every
     * picture as Chrome does so unchanged constraints also exercise reuse. */
    union {
        VAEncMiscParameterBuffer alignment;
        uint8_t bytes[sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl)];
    } misc = {0};
    VAEncMiscParameterBuffer *m = (void *)misc.bytes;
    m->type = VAEncMiscParameterTypeRateControl;
    VAEncMiscParameterRateControl rc = {0};
    rc.bits_per_second = 2000000;
    rc.target_percentage = 100;
    rc.window_size = 1500;
    rc.initial_qp = 26;
    rc.min_qp = 1;
    rc.max_qp = 42;
    rc.rc_flags.bits.disable_frame_skip = 1;
    memcpy(m->data, &rc, sizeof(rc));
    render(p, VAEncMiscParameterBufferType, misc.bytes, sizeof(misc.bytes));

    union {
        VAEncMiscParameterBuffer alignment;
        uint8_t bytes[sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterHRD)];
    } hrd_misc = {0};
    m = (void *)hrd_misc.bytes;
    m->type = VAEncMiscParameterTypeHRD;
    VAEncMiscParameterHRD hrd = { .buffer_size = 3000000,
                                .initial_buffer_fullness = 1500000 };
    memcpy(m->data, &hrd, sizeof(hrd));
    render(p, VAEncMiscParameterBufferType, hrd_misc.bytes, sizeof(hrd_misc.bytes));
}

static void parameters(struct probe *p, VAProfile profile, VABufferID coded, unsigned n)
{
    const int key = n == 0;
    const int chrome_h264 = p->chrome_style && profile == VAProfileH264Main;
    if (profile == VAProfileH264Main) {
        VAEncSequenceParameterBufferH264 seq = { 0 };
        seq.level_idc = 40;
        seq.intra_period = chrome_h264 ? 0 : 60;
        seq.intra_idr_period = chrome_h264 ? 2048 : 60;
        seq.ip_period = 1;
        seq.bits_per_second = 2000000;
        seq.max_num_ref_frames = 1;
        seq.picture_width_in_mbs = (p->width + 15) / 16;
        seq.picture_height_in_mbs = (p->height + 15) / 16;
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.seq_fields.bits.frame_mbs_only_flag = 1;
        if (chrome_h264) {
            seq.vui_parameters_present_flag = 1;
            seq.vui_fields.bits.timing_info_present_flag = 1;
            seq.num_units_in_tick = 1;
            seq.time_scale = 60;
        }
        VAEncPictureParameterBufferH264 pic = { 0 };
        pic.CurrPic.picture_id = p->source;
        pic.coded_buf = coded;
        pic.pic_init_qp = 26;
        pic.pic_fields.bits.idr_pic_flag = key;
        pic.pic_fields.bits.reference_pic_flag = 1;
        for (unsigned i = 0; i < 16; i++) {
            pic.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
            pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
        }
        VAEncSliceParameterBufferH264 slice = { 0 };
        slice.macroblock_info = VA_INVALID_ID;
        slice.num_macroblocks = seq.picture_width_in_mbs * seq.picture_height_in_mbs;
        slice.slice_type = key ? 2 : 0;
        if (!key) {
            pic.ReferenceFrames[0].picture_id = p->source;
            pic.ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
            slice.RefPicList0[0] = pic.ReferenceFrames[0];
        }
        if (key || chrome_h264)
            render(p, VAEncSequenceParameterBufferType, &seq, sizeof(seq));
        render(p, VAEncPictureParameterBufferType, &pic, sizeof(pic));
        render(p, VAEncSliceParameterBufferType, &slice, sizeof(slice));
    } else if (profile == VAProfileHEVCMain) {
        VAEncSequenceParameterBufferHEVC seq = { 0 };
        seq.general_profile_idc = 1;
        seq.general_level_idc = 120;
        seq.intra_period = seq.intra_idr_period = 60;
        seq.ip_period = 1;
        seq.bits_per_second = 2000000;
        seq.pic_width_in_luma_samples = p->width;
        seq.pic_height_in_luma_samples = p->height;
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.log2_diff_max_min_luma_coding_block_size = 3;
        VAEncPictureParameterBufferHEVC pic = { 0 };
        pic.decoded_curr_pic.picture_id = p->source;
        pic.coded_buf = coded;
        pic.pic_init_qp = 26;
        pic.pic_fields.bits.idr_pic_flag = key;
        pic.pic_fields.bits.coding_type = key ? 1 : 2;
        for (unsigned i = 0; i < 15; i++) {
            pic.reference_frames[i].picture_id = VA_INVALID_SURFACE;
            pic.reference_frames[i].flags = VA_PICTURE_HEVC_INVALID;
        }
        VAEncSliceParameterBufferHEVC slice = { 0 };
        slice.num_ctu_in_slice = ((p->width + 63) / 64) * ((p->height + 63) / 64);
        slice.slice_type = key ? 2 : 1;
        slice.slice_fields.bits.last_slice_of_pic_flag = 1;
        if (!key) {
            pic.reference_frames[0].picture_id = p->source;
            pic.reference_frames[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
            slice.ref_pic_list0[0] = pic.reference_frames[0];
        }
        if (key) render(p, VAEncSequenceParameterBufferType, &seq, sizeof(seq));
        render(p, VAEncPictureParameterBufferType, &pic, sizeof(pic));
        render(p, VAEncSliceParameterBufferType, &slice, sizeof(slice));
    } else {
        VAEncSequenceParameterBufferVP9 seq = { 0 };
        seq.max_frame_width = p->width;
        seq.max_frame_height = p->height;
        seq.bits_per_second = 2000000;
        seq.intra_period = 60;
        VAEncPictureParameterBufferVP9 pic = { 0 };
        pic.frame_width_src = pic.frame_width_dst = p->width;
        pic.frame_height_src = pic.frame_height_dst = p->height;
        pic.reconstructed_frame = p->source;
        pic.coded_buf = coded;
        pic.ref_flags.bits.force_kf = key;
        pic.pic_flags.bits.frame_type = !key;
        pic.pic_flags.bits.show_frame = 1;
        pic.luma_ac_qindex = 100;
        for (unsigned i = 0; i < 8; i++) pic.reference_frames[i] = VA_INVALID_SURFACE;
        if (!key) {
            pic.reference_frames[0] = p->source;
            pic.ref_flags.bits.ref_frame_ctrl_l0 = 1;
        }
        if (key) render(p, VAEncSequenceParameterBufferType, &seq, sizeof(seq));
        render(p, VAEncPictureParameterBufferType, &pic, sizeof(pic));
    }
    if (key || chrome_h264) {
        union {
            VAEncMiscParameterBuffer alignment;
            uint8_t bytes[sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterFrameRate)];
        } misc = { 0 };
        VAEncMiscParameterBuffer *m = (void *)misc.bytes;
        m->type = VAEncMiscParameterTypeFrameRate;
        VAEncMiscParameterFrameRate fps = {
            .framerate = chrome_h264 ? 30u : (1001u << 16) | 30000u };
        memcpy(m->data, &fps, sizeof(fps));
        render(p, VAEncMiscParameterBufferType, misc.bytes, sizeof(misc.bytes));
    }
    if (chrome_h264) chrome_h264_rate_control(p);
}

static void little(uint8_t *dst, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++) dst[i] = (uint8_t)(value >> (i * 8));
}

static void write_bytes(FILE *file, const void *data, size_t size)
{
    if (fwrite(data, 1, size, file) == size) return;
    perror("write encoded stream");
    exit(1);
}

static void run(struct probe *p, VAProfile profile, const char *path,
                 unsigned frames, unsigned rc)
{
    VAEntrypoint entries[DMD_MAX_ENTRYPOINTS];
    int count = 0, supported = 0;
    VA(p, p->vt.vaQueryConfigEntrypoints(&p->va, profile, entries, &count));
    for (int i = 0; i < count; i++) supported |= entries[i] == VAEntrypointEncSlice;
    if (!supported) {
        fprintf(stderr, "%s: SKIP (encoder capability absent)\n", path);
        return;
    }
    VAConfigAttrib attrs[] = { { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420 },
                              { VAConfigAttribRateControl, rc } };
    VAConfigID config;
    p->completed = 0;
    VA(p, p->vt.vaCreateConfig(&p->va, profile, VAEntrypointEncSlice, attrs, 2, &config));
    VA(p, p->vt.vaCreateSurfaces2(&p->va, VA_RT_FORMAT_YUV420, p->width, p->height,
                                 &p->source, 1, NULL, 0));
    VA(p, p->vt.vaCreateContext(&p->va, config, p->width, p->height, VA_PROGRESSIVE,
                               &p->source, 1, &p->context));
    VAImageFormat fmt = { .fourcc = VA_FOURCC_NV12, .byte_order = VA_LSB_FIRST,
                          .bits_per_pixel = 12 };
    VAImage image;
    VA(p, p->vt.vaCreateImage(&p->va, &fmt, p->width, p->height, &image));
    FILE *file = fopen(path, "wb");
    if (!file) { perror(path); exit(1); }
    const int vp9 = profile == VAProfileVP9Profile0;
    if (vp9) {
        uint8_t h[32] = { 'D', 'K', 'I', 'F', 0, 0, 32, 0, 'V', 'P', '9', '0' };
        little(h + 12, p->width, 2); little(h + 14, p->height, 2);
        little(h + 16, 30000, 4); little(h + 20, 1001, 4); little(h + 24, frames, 4);
        write_bytes(file, h, sizeof(h));
    }
    const int chrome_h264 = p->chrome_style && profile == VAProfileH264Main;
    fprintf(stderr, "%s: encoding %ux%u %u frames, %s, %s fps%s\n",
            path, p->width, p->height, frames, rc == VA_RC_CBR ? "CBR" : "VBR",
            chrome_h264 ? "30/1" : "30000/1001",
            chrome_h264 ? ", Chrome-style IDR2048/QP1..42/HRD1500ms" : "");
    size_t bytes = 0;
    for (unsigned n = 0; n < frames; n++) {
        uint8_t *pixels = NULL;
        VA(p, p->vt.vaMapBuffer(&p->va, image.buf, (void **)&pixels));
        memset(pixels, 128, image.data_size);
        for (unsigned y = 0; y < p->height; y++)
            for (unsigned x = 0; x < p->width; x++)
                pixels[image.offsets[0] + y * image.pitches[0] + x] =
                    (uint8_t)(16 + ((x / 8 + y / 8 + n * 3) % 220));
        VA(p, p->vt.vaUnmapBuffer(&p->va, image.buf));
        VA(p, p->vt.vaPutImage(&p->va, p->source, image.image_id, 0, 0,
                               p->width, p->height, 0, 0, p->width, p->height));
        VABufferID coded = buffer(p, VAEncCodedBufferType,
                                   (size_t)p->width * p->height * 3 / 2 + 65536, NULL);
        VA(p, p->vt.vaBeginPicture(&p->va, p->context, p->source));
        parameters(p, profile, coded, n);
        VA(p, p->vt.vaEndPicture(&p->va, p->context));
        VA(p, p->vt.vaSyncBuffer(&p->va, coded, UINT64_MAX));
        VACodedBufferSegment *segment = NULL;
        VA(p, p->vt.vaMapBuffer(&p->va, coded, (void **)&segment));
        if (!segment || !segment->buf || !segment->size || segment->next ||
            (segment->status & (VA_CODED_BUF_STATUS_BAD_BITSTREAM |
                                VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK))) {
            fprintf(stderr, "%s: invalid coded segment at frame %u\n", path, n);
            exit(1);
        }
        if (vp9) {
            uint8_t h[12];
            little(h, segment->size, 4); little(h + 4, n, 8);
            write_bytes(file, h, sizeof(h));
        }
        write_bytes(file, segment->buf, segment->size);
        bytes += segment->size;
        VA(p, p->vt.vaUnmapBuffer(&p->va, coded));
        VA(p, p->vt.vaDestroyBuffer(&p->va, coded));
        p->completed++;
    }
    if (fclose(file)) { perror("close encoded stream"); exit(1); }
    VA(p, p->vt.vaDestroyImage(&p->va, image.image_id));
    VA(p, p->vt.vaDestroyContext(&p->va, p->context));
    VA(p, p->vt.vaDestroySurfaces(&p->va, &p->source, 1));
    VA(p, p->vt.vaDestroyConfig(&p->va, config));
    fprintf(stderr, "%s: PASS %u frames, %zu coded bytes\n", path, p->completed, bytes);
}

static unsigned number(const char *text, unsigned max)
{
    char *end = NULL;
    errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    if (!errno && end && !*end && n && n <= max) return (unsigned)n;
    fprintf(stderr, "Invalid numeric argument: %s\n", text);
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 5 && argc != 6) {
        fprintf(stderr, "Usage: %s PREFIX [WIDTH HEIGHT FRAMES [cbr|vbr]]\n", argv[0]);
        return 2;
    }
    struct probe p = { .width = 320, .height = 240 };
    const char *chrome = getenv("DMD_PROBE_CHROME_STYLE");
    p.chrome_style = chrome && !strcmp(chrome, "1");
    unsigned frames = 30, rc = VA_RC_CBR;
    if (argc >= 5) {
        p.width = number(argv[2], DMD_MAX_WIDTH);
        p.height = number(argv[3], DMD_MAX_HEIGHT);
        frames = number(argv[4], 1000000);
    }
    if (argc == 6) {
        if (!strcmp(argv[5], "vbr")) rc = VA_RC_VBR;
        else if (strcmp(argv[5], "cbr")) { fprintf(stderr, "Expected cbr or vbr\n"); return 2; }
    }
    if ((p.width | p.height) & 1) { fprintf(stderr, "NV12 dimensions must be even\n"); return 2; }
    p.va.vtable = &p.vt;
    VA(&p, __vaDriverInit_1_0(&p.va));
    char path[4096];
    const VAProfile profiles[] = { VAProfileH264Main, VAProfileHEVCMain, VAProfileVP9Profile0 };
    const char *extensions[] = { "h264", "hevc", "ivf" };
    for (unsigned i = 0; i < 3; i++) {
        if (snprintf(path, sizeof(path), "%s.%s", argv[1], extensions[i]) >= (int)sizeof(path)) {
            fprintf(stderr, "Output prefix is too long\n"); return 2;
        }
        run(&p, profiles[i], path, frames, rc);
    }
    VA(&p, p.vt.vaTerminate(&p.va));
    return 0;
}
