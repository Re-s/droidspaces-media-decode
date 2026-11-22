/* H.264/HEVC common bit flow to the original language.
 *
 * The syntax elements of the two codes are fully identical (u(n)/ue(v)/se(v)/rbsp_trailing_bits)
 * The blog is a blog of the blog "The World of Human Rights" (Advanced by the blog "The World of Human Rights" (Advanced by the blog "The World of Human Rights"), which is used to share the information from the H264_bitstream.c.
 * Avoid the HVC copying again.
 */
#ifndef DMD_BITSTREAM_H
#define DMD_BITSTREAM_H

#include <stddef.h>
#include <stdint.h>

struct dmd_bitwriter {
    unsigned char *buf;
    size_t cap;
    size_t byte_pos;
    int bit_pos;       /* 0.7, the number of places written in the current byte */
    int overflow;      /* All writing is empty after position. Caller check this field. */
};

void   dmd_bw_init(struct dmd_bitwriter *bw, unsigned char *buf, size_t cap);
void   dmd_bw_put_bits(struct dmd_bitwriter *bw, uint32_t value, int nbits);
void   dmd_bw_put_flag(struct dmd_bitwriter *bw, int v);
void   dmd_bw_put_ue(struct dmd_bitwriter *bw, uint32_t v);
void   dmd_bw_put_se(struct dmd_bitwriter *bw, int32_t v);
void   dmd_bw_rbsp_trailing(struct dmd_bitwriter *bw);
size_t dmd_bw_bytes(const struct dmd_bitwriter *bw);

/* RBSP  ⁇  EBSP: Insert a 03 at 0000 0x (x< = 3) per encounter.
 * MediaCodec must receive it in a transliterated form. Returns the bytes written out, 0 = insufficient capacity. */
size_t dmd_rbsp_escape(const unsigned char *rbsp, size_t len,
                       unsigned char *out, size_t out_cap);

#endif
