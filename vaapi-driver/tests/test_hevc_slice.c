/* Pure HEVC ST-RPS rewrite tests: Main/Main10 syntax, generated parameter
 * sets, semantic reference ordering and byte-for-byte CABAC preservation.
 * No hardware or software decoder. */
#include "driver.h"
#include "hevc_slice.h"
#include "bitstream.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s errno=%d\n", __FILE__, __LINE__, #e, errno); exit(1); } } while (0)
struct bits { const uint8_t *p; size_t pos, size; };
static unsigned u(struct bits *b, unsigned n)
{
    CHECK(n <= 32 && b->pos + n <= b->size * 8);
    unsigned v = 0;
    for (unsigned i = 0; i < n; ++i, ++b->pos) v = (v << 1) | ((b->p[b->pos / 8] >> (7 - b->pos % 8)) & 1);
    return v;
}
static unsigned golomb(struct bits *b)
{
    unsigned n = 0;
    while (!u(b, 1)) CHECK(++n < 31);
    return ((1u << n) - 1) + u(b, n);
}
static int signed_golomb(struct bits *b)
{
    unsigned code = golomb(b);
    return code & 1 ? (int)((code + 1) / 2) : -(int)(code / 2);
}
static size_t deescape(const uint8_t *src, size_t size, uint8_t *out)
{
    size_t n = 0;
    unsigned zeros = 0;
    for (size_t i = 0; i < size; ++i) {
        if (zeros == 2 && src[i] == 3) { zeros = 0; continue; }
        out[n++] = src[i];
        zeros = src[i] ? 0 : zeros + 1;
        if (zeros > 2) zeros = 2;
    }
    return n;
}

static const uint8_t payload[] = { 0x11, 0, 0, 1, 0, 0, 2, 0, 0, 3, 0xaa, 0x80, 0, 0 };
struct sample {
    VAPictureParameterBufferHEVC p;
    VASliceParameterBufferHEVC s;
    uint8_t input[4096], rbsp[4096];
    size_t input_size, tail_start, tail_end;
    unsigned type, first, dependent, from_sps;
};

static void picture(VAPictureParameterBufferHEVC *p)
{
    memset(p, 0, sizeof(*p));
    p->CurrPic.picture_id = 1;
    p->CurrPic.pic_order_cnt = 10;
    p->pic_width_in_luma_samples = 128;
    p->pic_height_in_luma_samples = 96;
    p->pic_fields.bits.chroma_format_idc = 1;
    p->log2_diff_max_min_luma_coding_block_size = 3; /* CTU 64 */
    p->log2_max_pic_order_cnt_lsb_minus4 = 4;
    p->num_short_term_ref_pic_sets = 4;
    p->slice_parsing_fields.bits.long_term_ref_pics_present_flag = 1;
    p->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag = 1;
    for (unsigned i = 0; i < 15; ++i) {
        p->ReferenceFrames[i].picture_id = VA_INVALID_ID;
        p->ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    p->ReferenceFrames[0] = (VAPictureHEVC){ .picture_id = 2, .pic_order_cnt = 5,
        .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE };
    p->ReferenceFrames[1] = (VAPictureHEVC){ .picture_id = 3, .pic_order_cnt = 15, .flags = 0 };
    p->ReferenceFrames[2] = (VAPictureHEVC){ .picture_id = 4, .pic_order_cnt = 9,
        .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE };
    p->ReferenceFrames[3] = (VAPictureHEVC){ .picture_id = 5, .pic_order_cnt = 8, .flags = 0 };
    p->ReferenceFrames[4] = (VAPictureHEVC){ .picture_id = 6, .pic_order_cnt = 12,
        .flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER };
    p->ReferenceFrames[5] = (VAPictureHEVC){ .picture_id = 7, .pic_order_cnt = 0,
        .flags = VA_PICTURE_HEVC_LONG_TERM_REFERENCE | VA_PICTURE_HEVC_RPS_LT_CURR };
}

static void make_sample(struct sample *a, unsigned type, int first, int dependent, int from_sps)
{
    memset(a, 0, sizeof(*a));
    picture(&a->p);
    a->type = type; a->first = first; a->dependent = dependent; a->from_sps = from_sps;
    a->s.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    a->s.slice_segment_address = first ? 0 : 3;
    a->s.LongSliceFlags.fields.dependent_slice_segment_flag = dependent;
    a->s.LongSliceFlags.fields.slice_type = type >= 16 ? 2 : 0;
    struct dmd_bitwriter w;
    dmd_bw_init(&w, a->rbsp, sizeof(a->rbsp));
    dmd_bw_put_bits(&w, type << 9 | 1, 16);
    dmd_bw_put_flag(&w, first);
    if (type >= 16 && type <= 23) dmd_bw_put_flag(&w, 0);
    dmd_bw_put_ue(&w, 5); /* source PPS id is deliberately nonzero */
    if (!first) {
        dmd_bw_put_flag(&w, dependent);
        dmd_bw_put_bits(&w, 3, 2); /* CTB address, ceil_log2(2*2) */
    }
    if (!dependent) {
        dmd_bw_put_ue(&w, a->s.LongSliceFlags.fields.slice_type);
        if (type != 19 && type != 20) {
            dmd_bw_put_bits(&w, 10, 8); /* POC LSB */
            dmd_bw_put_flag(&w, from_sps);
            if (from_sps) dmd_bw_put_bits(&w, 2, 2);
            else {
                size_t start = w.byte_pos * 8 + w.bit_pos;
                dmd_bw_put_flag(&w, 0); /* old inline RPS stRpsIdx=4, not predicted */
                dmd_bw_put_ue(&w, 1); dmd_bw_put_ue(&w, 0);
                dmd_bw_put_ue(&w, 0); dmd_bw_put_flag(&w, 1);
                a->p.st_rps_bits = (uint32_t)(w.byte_pos * 8 + w.bit_pos - start);
            }
        }
    }
    a->tail_start = w.byte_pos * 8 + w.bit_pos;
    /* Include an LT entry despite num_long_term_ref_pic_sps=0, plus a dense
     * opaque suffix standing for temporal MVP/list/weights/entry-point fields. */
    if (!dependent && type != 19 && type != 20) {
        dmd_bw_put_ue(&w, 1);
        dmd_bw_put_bits(&w, 0, 8);
        dmd_bw_put_flag(&w, 1);
        dmd_bw_put_flag(&w, 0);
    }
    dmd_bw_put_bits(&w, 0xa5213, 20);
    a->tail_end = w.byte_pos * 8 + w.bit_pos;
    dmd_bw_rbsp_trailing(&w);
    a->s.slice_data_byte_offset = (uint32_t)dmd_bw_bytes(&w);
    memcpy(a->rbsp + dmd_bw_bytes(&w), payload, sizeof(payload));
    size_t n = dmd_bw_bytes(&w) + sizeof(payload);
    a->input_size = dmd_rbsp_escape(a->rbsp, n, a->input, sizeof(a->input));
    CHECK(a->input_size && !w.overflow);
    a->s.slice_data_size = (uint32_t)a->input_size;
}

static void check_rewrite(struct sample *a)
{
    uint8_t output[8192], rbsp[8192];
    size_t length;
    CHECK(dmd_hevc_rewrite_slice(&a->p, &a->s, a->input, a->input_size,
                                 output, sizeof(output), &length) == 0);
    CHECK(length > 4 && !memcmp(output, "\0\0\0\1", 4));
    size_t n = deescape(output + 4, length - 4, rbsp);
    struct bits b = { rbsp, 0, n };
    CHECK(u(&b, 16) == (a->type << 9 | 1));
    CHECK(u(&b, 1) == a->first);
    if (a->type >= 16 && a->type <= 23) CHECK(u(&b, 1) == 0);
    CHECK(golomb(&b) == 0); /* matches synthesized PPS */
    if (!a->first) { CHECK(u(&b, 1) == a->dependent); CHECK(u(&b, 2) == 3); }
    if (!a->dependent) {
        CHECK(golomb(&b) == a->s.LongSliceFlags.fields.slice_type);
        if (a->type != 19 && a->type != 20) {
            CHECK(u(&b, 8) == 10 && u(&b, 1) == 0);
            CHECK(golomb(&b) == 3 && golomb(&b) == 2);
            const unsigned expected_delta_minus1[] = { 0, 0, 2, 1, 2 };
            const unsigned expected_used[] = { 1, 0, 1, 1, 0 };
            for (unsigned i = 0; i < 5; ++i) {
                CHECK(golomb(&b) == expected_delta_minus1[i]);
                CHECK(u(&b, 1) == expected_used[i]);
            }
        }
    }
    struct bits old = { a->rbsp, a->tail_start, sizeof(a->rbsp) };
    for (size_t bit = a->tail_start; bit < a->tail_end; ++bit) CHECK(u(&b, 1) == u(&old, 1));
    CHECK(u(&b, 1) == 1);
    while (b.pos % 8) CHECK(u(&b, 1) == 0);
    CHECK(n - b.pos / 8 == sizeof(payload));
    CHECK(!memcmp(rbsp + b.pos / 8, payload, sizeof(payload)));
    uint8_t escaped_payload[64];
    size_t payload_size = dmd_rbsp_escape(payload, sizeof(payload), escaped_payload, sizeof(escaped_payload));
    CHECK(payload_size && length >= payload_size &&
           !memcmp(output + length - payload_size, escaped_payload, payload_size));
}

static void test_rejects(void)
{
    struct sample a;
    make_sample(&a, 1, 1, 0, 1);
    uint8_t output[8192]; size_t length;
    VAPictureParameterBufferHEVC p = a.p;
    p.num_long_term_ref_pic_sps = 1;
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    p = a.p; p.ReferenceFrames[0].pic_order_cnt = 9; /* duplicate */
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    p = a.p; p.ReferenceFrames[0].pic_order_cnt = 11; /* BEFORE with positive delta */
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    p = a.p; p.ReferenceFrames[0].flags |= VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    p = a.p; p.ReferenceFrames[0].flags |= VA_PICTURE_HEVC_LONG_TERM_REFERENCE;
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    p = a.p; p.ReferenceFrames[0].flags = VA_PICTURE_HEVC_RPS_LT_CURR;
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    p = a.p; p.ReferenceFrames[0].flags |= VA_PICTURE_HEVC_FIELD_PIC;
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    p = a.p; p.CurrPic.flags = VA_PICTURE_HEVC_FIELD_PIC;
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    p = a.p; p.ReferenceFrames[0].pic_order_cnt = p.CurrPic.pic_order_cnt;
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    p = a.p; p.ReferenceFrames[0].pic_order_cnt = INT32_MIN;
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    p = a.p; p.bit_depth_luma_minus8 = 2;
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    p = a.p; p.num_short_term_ref_pic_sets = 65;
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    {
        p = a.p; p.num_short_term_ref_pic_sets = 3;
        uint8_t invalid_rbsp[4096], invalid_ebsp[8192];
        memcpy(invalid_rbsp, a.rbsp, sizeof(invalid_rbsp));
        struct bits b = { invalid_rbsp, 16, sizeof(invalid_rbsp) };
        CHECK(u(&b, 1) == 1); CHECK(golomb(&b) == 5);
        CHECK(golomb(&b) == 0); CHECK(u(&b, 8) == 10); CHECK(u(&b, 1) == 1);
        ++b.pos; /* change source SPS index from binary 10 to 11 */
        invalid_rbsp[b.pos / 8] |= (uint8_t)(1u << (7 - b.pos % 8));
        size_t invalid_size = dmd_rbsp_escape(invalid_rbsp,
            a.s.slice_data_byte_offset + sizeof(payload), invalid_ebsp, sizeof(invalid_ebsp));
        VASliceParameterBufferHEVC bad_s = a.s; bad_s.slice_data_size = (uint32_t)invalid_size;
        CHECK(dmd_hevc_rewrite_slice(&p, &bad_s, invalid_ebsp, invalid_size,
                                     output, sizeof(output), &length) < 0);
    }
    VASliceParameterBufferHEVC s = a.s; s.slice_data_byte_offset = 0;
    CHECK(dmd_hevc_rewrite_slice(&a.p, &s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    s = a.s; s.slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN;
    CHECK(dmd_hevc_rewrite_slice(&a.p, &s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    s = a.s; s.slice_segment_address = 1; /* first slice must start at CTB zero */
    CHECK(dmd_hevc_rewrite_slice(&a.p, &s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    s = a.s; s.LongSliceFlags.fields.dependent_slice_segment_flag = 1;
    CHECK(dmd_hevc_rewrite_slice(&a.p, &s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    s = a.s; s.slice_data_byte_offset = UINT32_MAX;
    CHECK(dmd_hevc_rewrite_slice(&a.p, &s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    CHECK(dmd_hevc_rewrite_slice(&a.p, &a.s, a.input, a.input_size, output, 6, &length) < 0);
    for (size_t n = 0; n < a.input_size; ++n) {
        s = a.s; s.slice_data_size = (uint32_t)n;
        CHECK(dmd_hevc_rewrite_slice(&a.p, &s, a.input, n, output, sizeof(output), &length) < 0 ||
              n > a.s.slice_data_byte_offset); /* truncated headers always reject */
    }
    make_sample(&a, 1, 1, 0, 0); a.p.st_rps_bits = 0;
    CHECK(dmd_hevc_rewrite_slice(&a.p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    a.p.st_rps_bits = UINT32_MAX;
    CHECK(dmd_hevc_rewrite_slice(&a.p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    make_sample(&a, 1, 0, 0, 1);
    s = a.s; s.slice_segment_address = 2;
    CHECK(dmd_hevc_rewrite_slice(&a.p, &s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    p = a.p; p.log2_diff_max_min_luma_coding_block_size = 7;
    CHECK(dmd_hevc_rewrite_slice(&p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    make_sample(&a, 1, 1, 0, 1);
    a.input[a.input_size++] = 0; a.input[a.input_size++] = 0; a.input[a.input_size++] = 3;
    a.s.slice_data_size = (uint32_t)a.input_size; /* truncated EPB */
    CHECK(dmd_hevc_rewrite_slice(&a.p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    a.input[a.input_size++] = 4; a.s.slice_data_size = (uint32_t)a.input_size;
    CHECK(dmd_hevc_rewrite_slice(&a.p, &a.s, a.input, a.input_size, output, sizeof(output), &length) < 0);
    picture(&p); CHECK(dmd_hevc_can_build(&p));
    p.num_long_term_ref_pic_sps = 1; CHECK(!dmd_hevc_can_build(&p));
    p = a.p; p.pic_fields.bits.scaling_list_enabled_flag = 1; CHECK(!dmd_hevc_can_build(&p));
}

static void test_multiple_slices(void)
{
    struct sample a, b;
    uint8_t input[9000], output[18000], expected[18000];
    size_t offsets[2], length, n0, n1;
    make_sample(&a, 1, 1, 0, 1);
    for (unsigned dependent = 0; dependent < 2; ++dependent) {
        make_sample(&b, 1, 0, dependent, 1);
        memset(input, 0x5a, sizeof(input));
        offsets[0] = 13; offsets[1] = offsets[0] + a.input_size + 7;
        size_t size = offsets[1] + b.input_size + 11;
        memcpy(input + offsets[0], a.input, a.input_size);
        memcpy(input + offsets[1], b.input, b.input_size);
        VASliceParameterBufferHEVC slices[2] = { a.s, b.s };
        CHECK(dmd_hevc_rewrite_slice(&a.p, &a.s, a.input, a.input_size,
                                     expected, sizeof(expected), &n0) == 0);
        CHECK(dmd_hevc_rewrite_slice(&a.p, &b.s, b.input, b.input_size,
                                     expected + n0, sizeof(expected) - n0, &n1) == 0);
        CHECK(dmd_hevc_rewrite_slices(&a.p, slices, offsets, 2, input, size,
                                     output, sizeof(output), &length) == 0);
        CHECK(length == n0 + n1 && !memcmp(expected, output, length));
        CHECK(!memcmp(output + n0, "\0\0\0\1", 4));
        /* Invalid second-slice metadata must not commit a partial first NAL. */
        size_t saved = offsets[1]; offsets[1] = SIZE_MAX;
        length = 99;
        CHECK(dmd_hevc_rewrite_slices(&a.p, slices, offsets, 2, input, size,
                                     output, sizeof(output), &length) < 0 && !length);
        offsets[1] = saved;
        CHECK(dmd_hevc_rewrite_slices(&a.p, slices, offsets, 2, input,
                                     saved + b.input_size - 1, output, sizeof(output), &length) < 0 && !length);
        CHECK(dmd_hevc_rewrite_slices(&a.p, slices, offsets, 2, input, size,
                                     output, n0, &length) < 0 && !length);
        slices[1].slice_data_flag = VA_SLICE_DATA_FLAG_END;
        CHECK(dmd_hevc_rewrite_slices(&a.p, slices, offsets, 2, input, size,
                                     output, sizeof(output), &length) < 0 && !length);
        CHECK(dmd_hevc_rewrite_slices(&a.p, slices, offsets, 0, input, size,
                                     output, sizeof(output), &length) < 0 && !length);
        CHECK(dmd_hevc_rewrite_slices(&a.p, slices, offsets, 129, input, size,
                                     output, sizeof(output), &length) < 0 && !length);
    }
}

static void test_invalid_entries_ignored(void)
{
    struct sample a;
    make_sample(&a, 1, 1, 0, 1);
    /* Both invalid indicators independently make a DPB slot unused, even if
     * stale POC/RPS bits remain there. Neither becomes a fabricated ST_FOLL. */
    a.p.ReferenceFrames[6] = (VAPictureHEVC){ .picture_id = VA_INVALID_ID,
        .pic_order_cnt = 10, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE };
    a.p.ReferenceFrames[7] = (VAPictureHEVC){ .picture_id = 8,
        .pic_order_cnt = 9, .flags = VA_PICTURE_HEVC_INVALID };
    check_rewrite(&a);
}

static void test_main10_rewrite(void)
{
    /* The edited prefix is independent of bit depth. Exercise both original
     * RPS encodings, non-first/dependent segments, both IDR forms and CRA.
     * The suffix includes LT fields and EPB-sensitive opaque CABAC bytes;
     * check_rewrite verifies every one of those bits/bytes is retained. */
    static const unsigned cases[][4] = {
        { 1, 1, 0, 1 }, { 1, 1, 0, 0 }, { 1, 0, 0, 1 },
        { 1, 0, 1, 1 }, { 19, 1, 0, 0 }, { 20, 1, 0, 0 }, { 21, 1, 0, 1 }
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct sample a;
        make_sample(&a, cases[i][0], cases[i][1], cases[i][2], cases[i][3]);
        a.p.bit_depth_luma_minus8 = a.p.bit_depth_chroma_minus8 = 2;
        a.p.init_qp_minus26 = -30; /* legal only with the 10-bit QpBdOffset */
        CHECK(dmd_hevc_can_build(&a.p));
        check_rewrite(&a);
    }
    /* Main10 must retain the same explicit exclusions as the 8-bit rewrite. */
    struct sample a;
    make_sample(&a, 1, 1, 0, 1);
    a.p.bit_depth_luma_minus8 = a.p.bit_depth_chroma_minus8 = 2;
    VAPictureParameterBufferHEVC invalid[5];
    for (unsigned i = 0; i < 5; ++i) invalid[i] = a.p;
    invalid[0].pic_fields.bits.scaling_list_enabled_flag = 1;
    invalid[1].num_long_term_ref_pic_sps = 1;
    invalid[2].pic_fields.bits.chroma_format_idc = 2;
    invalid[3].CurrPic.flags = VA_PICTURE_HEVC_FIELD_PIC;
    invalid[4].bit_depth_chroma_minus8 = 0;
    for (unsigned i = 0; i < 5; ++i) {
        uint8_t output[8192]; size_t length = 99;
        CHECK(!dmd_hevc_can_build(&invalid[i]));
        CHECK(dmd_hevc_rewrite_slice(&invalid[i], &a.s, a.input, a.input_size,
                                     output, sizeof(output), &length) < 0);
        CHECK(errno == ENOTSUP && length == 0);
    }
}

static void check_ptl(struct bits *b, unsigned profile, unsigned level)
{
    CHECK(u(b, 2) == 0 && u(b, 1) == 0 && u(b, 5) == profile);
    for (unsigned i = 0; i < 32; ++i) CHECK(u(b, 1) == (i == profile));
    CHECK(u(b, 4) == 9); /* progressive + frame_only */
    /* For Main10 these zeros also express one_picture_only_constraint=0,
     * rather than accidentally advertising an intra/still-picture stream. */
    CHECK(u(b, 22) == 0 && u(b, 21) == 0 && u(b, 1) == 0);
    CHECK(u(b, 8) == level);
}

static void test_parameter_sets(void)
{
    uint8_t nalu[1024], rbsp[1024];
    static const struct { VAProfile profile; unsigned depth, profile_idc; } cases[] = {
        { VAProfileHEVCMain, 0, 1 },
        { VAProfileHEVCMain10, 0, 2 },
        { VAProfileHEVCMain10, 2, 2 }
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c)
        for (unsigned pcm = 0; pcm < 2; ++pcm) {
            unsigned depth = cases[c].depth;
            VAPictureParameterBufferHEVC p;
            picture(&p);
            p.bit_depth_luma_minus8 = p.bit_depth_chroma_minus8 = depth;
            p.sps_max_dec_pic_buffering_minus1 = 5;
            p.init_qp_minus26 = depth ? -30 : -26;
            p.pic_fields.bits.pcm_enabled_flag = pcm;
            p.pcm_sample_bit_depth_luma_minus1 = p.pcm_sample_bit_depth_chroma_minus1 = 7 + depth;
            VAProfile profile = cases[c].profile;
            unsigned profile_idc = cases[c].profile_idc;
            CHECK(dmd_hevc_can_build(&p));

            size_t n = dmd_hevc_build_vps_nalu(&p, profile, nalu, sizeof(nalu));
            CHECK(n > 6 && !memcmp(nalu, "\0\0\0\1\x40\x01", 6));
            size_t length = deescape(nalu + 6, n - 6, rbsp);
            struct bits b = { rbsp, 0, length };
            CHECK(u(&b, 4) == 0 && u(&b, 2) == 3 && u(&b, 6) == 0);
            CHECK(u(&b, 3) == 0 && u(&b, 1) == 1 && u(&b, 16) == 65535);
            check_ptl(&b, profile_idc, 30);

            n = dmd_hevc_build_sps_nalu(&p, profile, nalu, sizeof(nalu));
            CHECK(n > 6 && !memcmp(nalu, "\0\0\0\1\x42\x01", 6));
            length = deescape(nalu + 6, n - 6, rbsp);
            b = (struct bits){ rbsp, 0, length };
            CHECK(u(&b, 4) == 0 && u(&b, 3) == 0 && u(&b, 1) == 1);
            check_ptl(&b, profile_idc, 30);
            CHECK(golomb(&b) == 0 && golomb(&b) == 1);
            CHECK(golomb(&b) == 128 && golomb(&b) == 96 && u(&b, 1) == 0);
            CHECK(golomb(&b) == depth && golomb(&b) == depth);
            CHECK(golomb(&b) == 4 && u(&b, 1) == 0);
            CHECK(golomb(&b) == 5 && golomb(&b) == 5 && golomb(&b) == 0);
            CHECK(golomb(&b) == 0 && golomb(&b) == 3);
            CHECK(golomb(&b) == 0 && golomb(&b) == 0);
            CHECK(golomb(&b) == 0 && golomb(&b) == 0);
            CHECK(u(&b, 1) == 0 && u(&b, 1) == 0 && u(&b, 1) == 0);
            CHECK(u(&b, 1) == pcm);
            if (pcm) {
                CHECK(u(&b, 4) == 7 + depth && u(&b, 4) == 7 + depth);
                CHECK(golomb(&b) == 0 && golomb(&b) == 0 && u(&b, 1) == 0);
            }
            CHECK(golomb(&b) == 0); /* ST-RPS table removed, slice is rewritten */
            CHECK(u(&b, 1) == 1 && golomb(&b) == 0); /* no fabricated LT entries */
            CHECK(u(&b, 1) == 0 && u(&b, 1) == 0); /* temporal MVP + smoothing */
            CHECK(u(&b, 1) == 0 && u(&b, 1) == 0); /* VUI + extensions */
            CHECK(u(&b, 1) == 1);
            while (b.pos < length * 8) CHECK(u(&b, 1) == 0);

            n = dmd_hevc_build_pps_nalu(&p, nalu, sizeof(nalu));
            CHECK(n > 6 && !memcmp(nalu, "\0\0\0\1\x44\x01", 6));
            length = deescape(nalu + 6, n - 6, rbsp);
            b = (struct bits){ rbsp, 0, length };
            CHECK(golomb(&b) == 0 && golomb(&b) == 0);
            CHECK(u(&b, 1) == 1 && u(&b, 1) == 0 && u(&b, 3) == 0);
            CHECK(u(&b, 1) == 0 && u(&b, 1) == 0);
            CHECK(golomb(&b) == 0 && golomb(&b) == 0);
            CHECK(signed_golomb(&b) == p.init_qp_minus26);
        }

    VAPictureParameterBufferHEVC p;
    picture(&p);
    /* Invalid/foreign VA profiles must not fall through to Main and falsely
     * advertise a different format. The full parse above checks that a
     * Main10 PTL really keeps an 8-bit SPS, including its PCM/QP values. */
    p.bit_depth_luma_minus8 = p.bit_depth_chroma_minus8 = 2;
    CHECK(dmd_hevc_build_sps_nalu(&p, VAProfileHEVCMain, nalu, sizeof(nalu)) == 0);
    CHECK(dmd_hevc_build_vps_nalu(&p, VAProfileHEVCMain, nalu, sizeof(nalu)) == 0);
    CHECK(dmd_hevc_build_sps_nalu(&p, VAProfileHEVCMain12, nalu, sizeof(nalu)) == 0);
    CHECK(dmd_hevc_build_vps_nalu(&p, VAProfileNone, nalu, sizeof(nalu)) == 0);
    CHECK(dmd_hevc_build_pps_nalu(NULL, nalu, sizeof(nalu)) == 0);

    for (unsigned rps = 0; rps <= 4; rps += 4)
        for (unsigned luma = 0; luma <= 4; ++luma)
            for (unsigned chroma = 0; chroma <= 4; ++chroma) {
                picture(&p);
                p.num_short_term_ref_pic_sets = rps;
                p.bit_depth_luma_minus8 = luma;
                p.bit_depth_chroma_minus8 = chroma;
                int supported = luma == chroma && (luma == 0 || luma == 2);
                CHECK(dmd_hevc_can_build(&p) == supported);
                CHECK((dmd_hevc_build_sps_nalu(&p, VAProfileHEVCMain10,
                                               nalu, sizeof(nalu)) > 0) == supported);
                CHECK((dmd_hevc_build_vps_nalu(&p, VAProfileHEVCMain10,
                                               nalu, sizeof(nalu)) > 0) == supported);
                CHECK((dmd_hevc_build_sps_nalu(&p, VAProfileHEVCMain,
                                               nalu, sizeof(nalu)) > 0) == (supported && luma == 0));
                CHECK((dmd_hevc_build_vps_nalu(&p, VAProfileHEVCMain,
                                               nalu, sizeof(nalu)) > 0) == (supported && luma == 0));
            }
    picture(&p); p.bit_depth_luma_minus8 = p.bit_depth_chroma_minus8 = 2;
    p.pic_fields.bits.pcm_enabled_flag = 1;
    p.pcm_sample_bit_depth_luma_minus1 = 10; /* 11-bit PCM in a 10-bit SPS */
    CHECK(!dmd_hevc_can_build(&p));
    picture(&p); p.pic_fields.bits.chroma_format_idc = 2;
    CHECK(!dmd_hevc_can_build(&p));
}

int main(void)
{
    struct sample a;
    make_sample(&a, 1, 1, 0, 1); check_rewrite(&a);
    make_sample(&a, 1, 1, 0, 0); check_rewrite(&a);
    make_sample(&a, 1, 0, 0, 1); check_rewrite(&a);
    make_sample(&a, 1, 0, 1, 1); check_rewrite(&a);
    make_sample(&a, 19, 1, 0, 0); check_rewrite(&a);
    make_sample(&a, 20, 1, 0, 0); check_rewrite(&a);
    make_sample(&a, 21, 1, 0, 1); check_rewrite(&a);
    test_rejects();
    test_multiple_slices();
    test_invalid_entries_ignored();
    test_main10_rewrite();
    test_parameter_sets();
    puts("HEVC Main/Main10: parameter sets/profile/depth, RPS, ST_FOLL/LT, IDR/CRA, dependent slices, EPB/CABAC and rejection PASS");
    return 0;
}
