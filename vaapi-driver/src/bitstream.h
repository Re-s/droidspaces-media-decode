/* H.264/HEVC 共用的比特流写入原语。
 *
 * 两种编码的语法元素编码方式完全一致（u(n)/ue(v)/se(v)/rbsp_trailing_bits
 * 与 emulation prevention），所以从 h264_bitstream.c 里提出来共享，
 * 避免 HEVC 再抄一份。
 */
#ifndef DMD_BITSTREAM_H
#define DMD_BITSTREAM_H

#include <stddef.h>
#include <stdint.h>

struct dmd_bitwriter {
    unsigned char *buf;
    size_t cap;
    size_t byte_pos;
    int bit_pos;       /* 0..7，当前字节内已写的位数 */
    int overflow;      /* 置位后所有写入都是空操作，调用方检查这个字段 */
};

void   dmd_bw_init(struct dmd_bitwriter *bw, unsigned char *buf, size_t cap);
void   dmd_bw_put_bits(struct dmd_bitwriter *bw, uint32_t value, int nbits);
void   dmd_bw_put_flag(struct dmd_bitwriter *bw, int v);
void   dmd_bw_put_ue(struct dmd_bitwriter *bw, uint32_t v);
void   dmd_bw_put_se(struct dmd_bitwriter *bw, int32_t v);
void   dmd_bw_rbsp_trailing(struct dmd_bitwriter *bw);
size_t dmd_bw_bytes(const struct dmd_bitwriter *bw);

/* RBSP → EBSP：每遇 00 00 0x（x<=3）插入一个 03。
 * MediaCodec 收到的必须是转义后的形式。返回写入 out 的字节数，0 = 容量不足。 */
size_t dmd_rbsp_escape(const unsigned char *rbsp, size_t len,
                       unsigned char *out, size_t out_cap);

#endif
