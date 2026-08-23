/* H.264/HEVC 共用的比特流写入原语实现。见 bitstream.h。 */

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

/* ue(v)：Exp-Golomb 无符号。写法是 (leadingZeros) 1 (info)，
 * 其中 codeNum+1 的二进制长度决定前导零个数。 */
void dmd_bw_put_ue(struct dmd_bitwriter *bw, uint32_t v)
{
    if (v == 0xFFFFFFFFu) { /* v+1 会回绕 */
        bw->overflow = 1;
        return;
    }
    uint32_t val = v + 1;
    int nbits = 0;
    while ((val >> nbits) != 0)
        nbits++;
    /* nbits-1 个前导零，然后 val 的 nbits 位（最高位那个 1 就是分隔符）。
     * v==0 时 nbits==1，前导零个数为 0 —— 不能调 dmd_bw_put_bits(.,.,0)，
     * 它会把 nbits<=0 判成溢出。 */
    if (nbits > 1)
        dmd_bw_put_bits(bw, 0, nbits - 1);
    dmd_bw_put_bits(bw, val, nbits);
}

/* se(v)：Exp-Golomb 有符号。映射 0,1,-1,2,-2,... → 0,1,2,3,4,... */
void dmd_bw_put_se(struct dmd_bitwriter *bw, int32_t v)
{
    uint32_t code;
    if (v <= 0)
        code = (uint32_t)(-2 * (int64_t)v);
    else
        code = (uint32_t)(2 * (int64_t)v - 1);
    dmd_bw_put_ue(bw, code);
}

/* rbsp_trailing_bits：一个 1 位，然后补零到字节边界。 */
void dmd_bw_rbsp_trailing(struct dmd_bitwriter *bw)
{
    dmd_bw_put_flag(bw, 1);
    while (bw->bit_pos != 0)
        dmd_bw_put_flag(bw, 0);
}

/* 已写出的字节数（必须在 rbsp_trailing 之后调用，此时已对齐）。 */
size_t dmd_bw_bytes(const struct dmd_bitwriter *bw)
{
    return bw->bit_pos == 0 ? bw->byte_pos : bw->byte_pos + 1;
}

/* ------------------------------------------------- emulation prevention 转义 */

/* 把 RBSP 转成 SODB/EBSP：每遇到 00 00 0x（x <= 3）就插入一个 03。
 * MediaCodec 收到的是完整 NALU，必须是转义后的形式，否则解析器会把
 * 数据里偶然出现的 00 00 01 当成起始码。 */
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
