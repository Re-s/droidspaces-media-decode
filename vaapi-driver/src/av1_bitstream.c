/* AV1 OBU 反向合成——变长编码、对齐与 OBU 头。
 *
 * 设计说明见 av1_bitstream.h 顶部。本文件只实现最底层的比特写入原语，
 * 序列头/帧头/tile group 的语法在后续提交里加。
 */
#include "av1_bitstream.h"

/* ---------------------------------------------------------------- leb128 */

size_t dmd_av1_leb128_len(uint64_t v)
{
    size_t n = 0;
    do {
        n++;
        v >>= 7;
    } while (v && n < DMD_LEB128_MAX);
    return n;
}

size_t dmd_av1_leb128(uint64_t v, unsigned char *out, size_t out_cap)
{
    size_t n = 0;
    do {
        if (n >= out_cap || n >= DMD_LEB128_MAX)
            return 0;
        unsigned char byte = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (v)
            byte |= 0x80;          /* 还有后续字节 */
        out[n++] = byte;
    } while (v);
    return n;
}

/* ------------------------------------------------------------------ uvlc */

void dmd_av1_put_uvlc(struct dmd_bitwriter *bw, uint32_t v)
{
    /* AV1 规范 4.10.3：leadingZeros 个 0，一个 1，然后 leadingZeros 位尾数。
     * 值 v 编码为 (1 << leadingZeros) - 1 + mantissa。 */
    uint32_t leading_zeros = 0;
    uint64_t val = (uint64_t)v + 1;

    while ((val >> (leading_zeros + 1)) != 0)
        leading_zeros++;

    if (leading_zeros >= 32) {
        bw->overflow = 1;
        return;
    }

    /* leading_zeros 个 0 位；put_bits 不接受 nbits=0，所以判空。 */
    if (leading_zeros > 0)
        dmd_bw_put_bits(bw, 0, (int)leading_zeros);
    dmd_bw_put_flag(bw, 1);
    if (leading_zeros > 0)
        dmd_bw_put_bits(bw, (uint32_t)(val & ((1u << leading_zeros) - 1)),
                        (int)leading_zeros);
}

/* -------------------------------------------------------------------- le */

void dmd_av1_put_le(struct dmd_bitwriter *bw, uint64_t v, int nbytes)
{
    /* AV1 规范 4.10.4：小端字节序，要求调用时已字节对齐。 */
    if (nbytes <= 0 || nbytes > 8 || bw->bit_pos != 0) {
        bw->overflow = 1;
        return;
    }
    for (int i = 0; i < nbytes; i++)
        dmd_bw_put_bits(bw, (uint32_t)((v >> (i * 8)) & 0xff), 8);
}

/* -------------------------------------------------------------------- ns */

void dmd_av1_put_ns(struct dmd_bitwriter *bw, uint32_t v, uint32_t n)
{
    /* AV1 规范 4.10.7 ns(n)：非对称编码，小值省一位。
     *   w = FloorLog2(n) + 1
     *   m = (1 << w) - n
     * v < m 时用 w-1 位直接写；否则写 (v + m) 的 w 位。 */
    if (n == 0) {
        bw->overflow = 1;
        return;
    }
    if (n == 1)
        return;                    /* 只有一个取值，不占位 */

    uint32_t w = 0, t = n;
    while (t) { w++; t >>= 1; }    /* w = FloorLog2(n) + 1 */
    uint32_t m = (1u << w) - n;

    if (v < m) {
        dmd_bw_put_bits(bw, v, (int)(w - 1));
    } else {
        uint32_t enc = v + m;
        dmd_bw_put_bits(bw, enc >> 1, (int)(w - 1));
        dmd_bw_put_bits(bw, enc & 1, 1);
    }
}

/* -------------------------------------------------------------------- su */

void dmd_av1_put_su(struct dmd_bitwriter *bw, int32_t v, int nbits)
{
    /* AV1 规范 4.10.6 su(n)：n 位补码。写入时取低 nbits 位即可，
     * 解码侧按符号位扩展。 */
    if (nbits <= 0 || nbits > 32) {
        bw->overflow = 1;
        return;
    }
    uint32_t mask = (nbits == 32) ? 0xffffffffu : ((1u << nbits) - 1u);
    dmd_bw_put_bits(bw, (uint32_t)v & mask, nbits);
}

/* ------------------------------------------------------------------ 对齐 */

void dmd_av1_byte_align(struct dmd_bitwriter *bw)
{
    /* AV1 规范 5.3.5 byte_alignment()：纯补零，**不写 stop bit**。
     * 这是与 H.264/HEVC rbsp_trailing_bits 的关键差别。 */
    while (bw->bit_pos != 0 && !bw->overflow)
        dmd_bw_put_flag(bw, 0);
}

void dmd_av1_trailing_bits(struct dmd_bitwriter *bw)
{
    /* AV1 规范 5.3.4 trailing_bits()：一个 1，然后补零到字节边界。
     * 注意即使当前已对齐也要写这个 1 —— 它是 payload 结束标记。 */
    dmd_bw_put_flag(bw, 1);
    while (bw->bit_pos != 0 && !bw->overflow)
        dmd_bw_put_flag(bw, 0);
}

/* ---------------------------------------------------------------- OBU 头 */

size_t dmd_av1_obu_header(int obu_type, size_t payload_len,
                          unsigned char *out, size_t out_cap)
{
    if (obu_type < 0 || obu_type > 15 || out_cap < 1)
        return 0;

    /* forbidden(1)=0 | type(4) | extension(1)=0 | has_size(1)=1 | reserved(1)=0
     *
     * 即 0x00 | (type << 3) | 0x00 | 0x02 | 0x00 —— has_size 位是 bit1。
     * 举例：SEQUENCE_HEADER(1) → 0x0a，FRAME_HEADER(3) → 0x1a，
     *       TILE_GROUP(4) → 0x22，TEMPORAL_DELIMITER(2) → 0x12。
     *
     * 对照实测的非法值 0xd0 = 1101_0000：forbidden=1（必须 0）、
     * type=10（保留值）—— 一眼就能看出那不是 OBU 头而是裸载荷。 */
    out[0] = (unsigned char)(((obu_type & 0x0f) << 3) | 0x02);

    size_t n = dmd_av1_leb128((uint64_t)payload_len, out + 1, out_cap - 1);
    if (n == 0)
        return 0;
    return 1 + n;
}
