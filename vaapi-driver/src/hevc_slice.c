/* SPDX-License-Identifier: Apache-2.0 */
/* Grammar and VA semantics cross-checked against FFmpeg n8.0:
 * libavcodec/hevc/hevcdec.c hls_slice_header(), hevc/refs.c ff_hevc_frame_rps(),
 * and vaapi_hevc.c fill_vaapi_reference_frames()/vaapi_hevc_decode_slice().
 * No CABAC decoding/re-encoding takes place here. */
#include "hevc_slice.h"
#include "bitstream.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct reader { const uint8_t *data; size_t bits, pos; int error; };

static uint32_t get(struct reader *r, unsigned bits)
{
    if (bits > 32 || r->pos > r->bits || bits > r->bits - r->pos) {
        r->error = 1;
        return 0;
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < bits; ++i, ++r->pos)
        value = (value << 1) | ((r->data[r->pos / 8] >> (7 - r->pos % 8)) & 1);
    return value;
}

static uint32_t ue(struct reader *r)
{
    unsigned zeros = 0;
    while (!r->error && !get(r, 1)) {
        if (++zeros > 30) { r->error = 1; return 0; }
    }
    return ((1u << zeros) - 1) + get(r, zeros);
}

static unsigned ceil_log2(uint32_t value)
{
    unsigned bits = 0;
    for (uint32_t v = value > 0 ? value - 1 : 0; v; v >>= 1) ++bits;
    return bits;
}

static void copy_bits(struct dmd_bitwriter *w, const uint8_t *data,
                       size_t start, size_t end)
{
    for (size_t i = start; i < end; ++i)
        dmd_bw_put_flag(w, (data[i / 8] >> (7 - i % 8)) & 1);
}

static int unescape(const uint8_t *input, size_t size, uint8_t *out, size_t *length)
{
    unsigned zeros = 0;
    size_t n = 0;
    for (size_t i = 0; i < size; ++i) {
        uint8_t c = input[i];
        if (zeros == 2 && c == 3) {
            if (i + 1 >= size || input[i + 1] > 3) { errno = EBADMSG; return -1; }
            zeros = 0;
            continue;
        }
        out[n++] = c;
        zeros = c == 0 ? zeros + 1 : 0;
        if (zeros > 2) zeros = 2;
    }
    *length = n;
    return 0;
}

struct reference { int32_t delta; int used; };

static int build_rps(const VAPictureParameterBufferHEVC *p,
                      struct reference refs[15], unsigned *negative, unsigned *positive)
{
    unsigned count = 0;
    for (unsigned i = 0; i < 15; ++i) {
        const VAPictureHEVC *ref = &p->ReferenceFrames[i];
        if ((ref->flags & VA_PICTURE_HEVC_INVALID) || ref->picture_id == VA_INVALID_ID)
            continue;
        unsigned used_flags = ref->flags & (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
            VA_PICTURE_HEVC_RPS_ST_CURR_AFTER | VA_PICTURE_HEVC_RPS_LT_CURR);
        if ((ref->flags & (VA_PICTURE_HEVC_FIELD_PIC | VA_PICTURE_HEVC_BOTTOM_FIELD)) ||
            (used_flags && (used_flags & (used_flags - 1)))) goto unsupported;
        int is_long = (ref->flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE) != 0;
        if (is_long) {
            if (used_flags & (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
                              VA_PICTURE_HEVC_RPS_ST_CURR_AFTER)) goto unsupported;
            continue; /* The source LT syntax is copied verbatim below. */
        }
        if (used_flags & VA_PICTURE_HEVC_RPS_LT_CURR) goto unsupported;
        int64_t delta = (int64_t)ref->pic_order_cnt - p->CurrPic.pic_order_cnt;
        if (!delta || delta <= -INT32_MAX || delta >= INT32_MAX) goto unsupported;
        if ((used_flags == VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE && delta >= 0) ||
            (used_flags == VA_PICTURE_HEVC_RPS_ST_CURR_AFTER && delta <= 0)) goto unsupported;
        for (unsigned j = 0; j < count; ++j) if (refs[j].delta == delta) goto unsupported;
        refs[count++] = (struct reference){ (int32_t)delta, used_flags != 0 };
    }
    /* S0 is nearest negative POC first; S1 is nearest positive POC first.
     * Valid non-LT DPB entries without a CURR flag are ST_FOLL, used=0. */
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = i + 1; j < count; ++j) {
            int swap = (refs[i].delta > 0 && refs[j].delta < 0) ||
                (refs[i].delta < 0 && refs[j].delta < 0 && refs[i].delta < refs[j].delta) ||
                (refs[i].delta > 0 && refs[j].delta > 0 && refs[i].delta > refs[j].delta);
            if (swap) { struct reference tmp = refs[i]; refs[i] = refs[j]; refs[j] = tmp; }
        }
    *negative = 0;
    while (*negative < count && refs[*negative].delta < 0) ++*negative;
    *positive = count - *negative;
    return 0;
unsupported:
    errno = ENOTSUP;
    return -1;
}

static void put_rps(struct dmd_bitwriter *w, const struct reference refs[15],
                     unsigned negative, unsigned positive)
{
    /* stRpsIdx=0: inter_ref_pic_set_prediction_flag is absent. */
    dmd_bw_put_ue(w, negative);
    dmd_bw_put_ue(w, positive);
    int32_t previous = 0;
    for (unsigned i = 0; i < negative; ++i) {
        dmd_bw_put_ue(w, (uint32_t)((int64_t)previous - refs[i].delta - 1));
        dmd_bw_put_flag(w, refs[i].used);
        previous = refs[i].delta;
    }
    previous = 0;
    for (unsigned i = negative; i < negative + positive; ++i) {
        dmd_bw_put_ue(w, (uint32_t)((int64_t)refs[i].delta - previous - 1));
        dmd_bw_put_flag(w, refs[i].used);
        previous = refs[i].delta;
    }
}

int dmd_hevc_rewrite_slice(const VAPictureParameterBufferHEVC *p,
                            const VASliceParameterBufferHEVC *s,
                            const uint8_t *input, size_t size,
                            uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!p || !s || !input || !out || !written || size < 3 || capacity < 6 ||
        size > SIZE_MAX / 8 || size > SIZE_MAX - 1024 || s->slice_data_size != size ||
        s->slice_data_flag != VA_SLICE_DATA_FLAG_ALL) { errno = EINVAL; return -1; }
    if (!p->num_short_term_ref_pic_sets || p->num_short_term_ref_pic_sets > 64 ||
        p->num_long_term_ref_pic_sps || p->pic_fields.bits.chroma_format_idc != 1 ||
        p->pic_fields.bits.separate_colour_plane_flag ||
        p->bit_depth_luma_minus8 != p->bit_depth_chroma_minus8 ||
        (p->bit_depth_luma_minus8 != 0 && p->bit_depth_luma_minus8 != 2) ||
        p->pic_fields.bits.scaling_list_enabled_flag ||
        (p->CurrPic.flags & (VA_PICTURE_HEVC_FIELD_PIC | VA_PICTURE_HEVC_BOTTOM_FIELD)) ||
        p->log2_max_pic_order_cnt_lsb_minus4 > 12) { errno = ENOTSUP; return -1; }
    unsigned type = (input[0] >> 1) & 63;
    if ((input[0] & 0x80) || (input[0] & 1) || (input[1] >> 3) ||
        !(input[1] & 7) || (type > 9 && type < 16) || type > 21) {
        errno = ENOTSUP; return -1;
    }
    uint8_t *rbsp = malloc(size), *rebuilt = malloc(size + 1024);
    if (!rbsp || !rebuilt) { free(rbsp); free(rebuilt); errno = ENOMEM; return -1; }
    int result = -1;
    size_t length = 0;
    if (unescape(input, size, rbsp, &length) < 0) goto done;
    size_t cabac = s->slice_data_byte_offset;
    if (cabac < 3 || cabac >= length || !rbsp[cabac - 1]) { errno = EBADMSG; goto done; }
    /* VA's offset counts RBSP bytes including the NAL header. The last header
     * byte's least significant one is alignment_bit_equal_to_one. Everything
     * after it is zero padding; all preceding header bits are retained. */
    unsigned zeros = 0;
    for (uint8_t byte = rbsp[cabac - 1]; !(byte & 1); byte >>= 1) ++zeros;
    size_t header_bits = cabac * 8 - zeros - 1;
    struct reader r = { rbsp, header_bits, 16, 0 };
    unsigned first = get(&r, 1);
    if (type >= 16 && type <= 23) get(&r, 1);
    size_t pps_start = r.pos;
    if (ue(&r) > 63) { errno = EBADMSG; goto done; }
    size_t pps_end = r.pos;
    unsigned dependent = 0;
    if (!first) {
        if (p->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag)
            dependent = get(&r, 1);
        unsigned log2_ctb = p->log2_min_luma_coding_block_size_minus3 + 3u +
                            p->log2_diff_max_min_luma_coding_block_size;
        if (log2_ctb < 3 || log2_ctb > 6) { errno = ENOTSUP; goto done; }
        uint32_t ctb = 1u << log2_ctb;
        uint32_t count = ((p->pic_width_in_luma_samples + ctb - 1) / ctb) *
                         ((p->pic_height_in_luma_samples + ctb - 1) / ctb);
        uint32_t address = get(&r, ceil_log2(count));
        if (!count || address >= count || address != s->slice_segment_address) {
            errno = EBADMSG; goto done;
        }
    } else if (s->slice_segment_address) { errno = EBADMSG; goto done; }
    if (dependent != s->LongSliceFlags.fields.dependent_slice_segment_flag) {
        errno = EBADMSG; goto done;
    }
    /* Main and Main10 use the same syntax up through the ST-RPS. Bit-depth
     * dependent weights/QP and all CABAC data lie in the unchanged suffix;
     * never reinterpret or reduce those fields to an 8-bit representation. */
    size_t begin = 0, end = 0;
    struct reference refs[15];
    unsigned negative = 0, positive = 0;
    int rewrite = !dependent && type != 19 && type != 20;
    if (!dependent) {
        get(&r, p->num_extra_slice_header_bits);
        unsigned slice_type = ue(&r);
        if (slice_type > 2 || slice_type != s->LongSliceFlags.fields.slice_type) {
            errno = EBADMSG; goto done;
        }
        if (p->slice_parsing_fields.bits.output_flag_present_flag) get(&r, 1);
        if (rewrite) {
            get(&r, p->log2_max_pic_order_cnt_lsb_minus4 + 4u);
            begin = r.pos;
            unsigned from_sps = get(&r, 1);
            if (from_sps) {
                uint32_t index = get(&r, ceil_log2(p->num_short_term_ref_pic_sets));
                if (index >= p->num_short_term_ref_pic_sets) { errno = EBADMSG; goto done; }
            } else {
                if (!p->st_rps_bits || p->st_rps_bits > r.bits - r.pos) {
                    errno = ENOTSUP; goto done;
                }
                r.pos += p->st_rps_bits;
            }
            end = r.pos;
            if (build_rps(p, refs, &negative, &positive) < 0) goto done;
        }
    }
    if (r.error || pps_end > header_bits || (rewrite && end > header_bits)) {
        errno = EBADMSG; goto done;
    }
    struct dmd_bitwriter w;
    dmd_bw_init(&w, rebuilt, size + 1024);
    copy_bits(&w, rbsp, 0, pps_start);
    dmd_bw_put_ue(&w, 0); /* generated PPS uses id 0 */
    if (rewrite) {
        copy_bits(&w, rbsp, pps_end, begin);
        dmd_bw_put_flag(&w, 0); /* ST set is now inline, not an SPS index */
        put_rps(&w, refs, negative, positive);
        copy_bits(&w, rbsp, end, header_bits);
    } else copy_bits(&w, rbsp, pps_end, header_bits);
    dmd_bw_rbsp_trailing(&w); /* slice byte_alignment has the same 1,zero* form */
    if (w.overflow || dmd_bw_bytes(&w) > size + 1024 - (length - cabac)) {
        errno = ENOSPC; goto done;
    }
    size_t header_size = dmd_bw_bytes(&w);
    memcpy(rebuilt + header_size, rbsp + cabac, length - cabac);
    out[0] = out[1] = out[2] = 0; out[3] = 1;
    size_t escaped = dmd_rbsp_escape(rebuilt, header_size + length - cabac, out + 4, capacity - 4);
    if (!escaped) { errno = ENOSPC; goto done; }
    *written = escaped + 4;
    result = 0;
done:
    free(rbsp);
    free(rebuilt);
    return result;
}

int dmd_hevc_rewrite_slices(const VAPictureParameterBufferHEVC *p,
                            const VASliceParameterBufferHEVC *slices,
                            const size_t *offsets, unsigned count,
                            const uint8_t *input, size_t size,
                            uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!p || !slices || !offsets || !count || count > 128 ||
        !input || !out || !written) { errno = EINVAL; return -1; }
    size_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        size_t offset = offsets[i], n = 0;
        if (offset > size || slices[i].slice_data_size > size - offset) {
            errno = EINVAL;
            return -1;
        }
        if (dmd_hevc_rewrite_slice(p, &slices[i], input + offset,
                slices[i].slice_data_size, out + total, capacity - total, &n) < 0)
            return -1;
        total += n;
    }
    *written = total;
    return 0;
}
