/* H.264/HEVC shares bitstream writing in the original language. See bitstream.h. */

#include <string.h>

#include "bitstream.h"


void dmd_bw_init(struct dmd_bitwriter *bw, unsigned char *buf, size_t cap)
{
    bw->buf = buf;
    bw->cap = cap;
    bw->byte_pos = 0;
    bw->bit_pos = 0;
    bw->overflow = 0;
    if (cap > 0)
        buf[0] = 0;
}

void dmd_bw_put_bits(struct dmd_bitwriter *bw, uint32_t value, int nbits)
{
    if (nbits <= 0 || nbits > 32) {
        bw->overflow = 1;
        return;
    }
    for (int i = nbits - 1; i >= 0; i--) {
        if (bw->byte_pos >= bw->cap) {
            bw->overflow = 1;
            return;
        }
        unsigned int bit = (value >> i) & 1u;
        bw->buf[bw->byte_pos] |= (unsigned char)(bit << (7 - bw->bit_pos));
        bw->bit_pos++;
        if (bw->bit_pos == 8) {
            bw->bit_pos = 0;
            bw->byte_pos++;
            if (bw->byte_pos < bw->cap)
                bw->buf[bw->byte_pos] = 0;
        }
    }
}

void dmd_bw_put_flag(struct dmd_bitwriter *bw, int v)
{
    dmd_bw_put_bits(bw, v ? 1u : 0u, 1);
}

/* : Exp-Golom unsigned. Write is (LaidingZeros) 1 (info),
 * Of which the binary length of codeNum+1 determines the number of zeros to lead. */
void dmd_bw_put_ue(struct dmd_bitwriter *bw, uint32_t v)
{
    if (v == 0xFFFFFFFFu) { /* v+1 will be back */
        bw->overflow = 1;
        return;
    }
    uint32_t val = v + 1;
    int nbits = 0;
    while ((val >> nbits) != 0)
        nbits++;
    /* nbits-1 zero for the lead, then val's nbits bit (the top one is the separator).
     * v = nbits=1 at 0 - cannot be adjusted dmd_bw_put_bits(...,.,0),
     * It'll overflow the nbits < =0. */
    if (nbits > 1)
        dmd_bw_put_bits(bw, 0, nbits - 1);
    dmd_bw_put_bits(bw, val, nbits);
}

/* s(v): Exp-Golumb has symbols. Map 0, 1, 1, 2, 2, 2, 2, 0, 1, 2, 3, 4,... */
void dmd_bw_put_se(struct dmd_bitwriter *bw, int32_t v)
{
    uint32_t code;
    if (v <= 0)
        code = (uint32_t)(-2 * (int64_t)v);
    else
        code = (uint32_t)(2 * (int64_t)v - 1);
    dmd_bw_put_ue(bw, code);
}

/* rbsp_trailing_bits: one bit, then zero to byte boundary. */
void dmd_bw_rbsp_trailing(struct dmd_bitwriter *bw)
{
    dmd_bw_put_flag(bw, 1);
    while (bw->bit_pos != 0)
        dmd_bw_put_flag(bw, 0);
}

/* The number of bytes written (must be called after rbsp_trailing, is aligned at this time). */
size_t dmd_bw_bytes(const struct dmd_bitwriter *bw)
{
    return bw->bit_pos == 0 ? bw->byte_pos : bw->byte_pos + 1;
}

/* - - - - - - - - - - - - - - - Conversion to */

/* Convert RBPSP into SODB/EBSP: Insert a 03 every 0000 0x (x < = 3) encountered.
 * MediaCodec received the complete NALU, which must be in a transliterated form, otherwise the solver would have taken it.
 * The random occurrence of 000001 in the data is used as a starting code. */
size_t dmd_rbsp_escape(const unsigned char *rbsp, size_t len,
                          unsigned char *out, size_t out_cap)
{
    size_t o = 0;
    int zeros = 0;

    for (size_t i = 0; i < len; i++) {
        if (zeros >= 2 && rbsp[i] <= 0x03) {
            if (o >= out_cap)
                return 0;
            out[o++] = 0x03;
            zeros = 0;
        }
        if (o >= out_cap)
            return 0;
        out[o++] = rbsp[i];
        if (rbsp[i] == 0x00)
            zeros++;
        else
            zeros = 0;
    }
    return o;
}
