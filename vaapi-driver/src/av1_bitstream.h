/* AV1 OBU 反向合成。
 *
 * 为什么需要这个文件：VA-API 把 AV1 的解码参数以**结构化字段**交给驱动
 * （VADecPictureParameterBufferAV1），码流侧只给**裸 tile 载荷**——
 * 不含任何 OBU 封装。而下游 MediaCodec 要的是完整 AV1 OBU 流。
 *
 * libva 自己把这件事说清楚了（/usr/include/va/va_dec_av1.h:643-645）：
 *   "host decoder is responsible to parse out the per tile information.
 *    And the bit stream in sent to driver in per tile granularity."
 * 同文件 :637-638 还注明 VASliceParameterBufferAV1 "actually means
 * VATileParameterBufferAV1"。
 *
 * ⚠️ 这与 VP9 的情形完全不同，别照搬。VP9 的 slice data 本身就是完整帧
 * （va_dec_vp9.h:274-284），所以那条路零重建成立；AV1 走同样的路会送出
 * 非法码流——实测首字节 0xd0（forbidden_bit=1、OBU 类型 10 是保留值），
 * 解码器报 "No sequence header available"，一帧也解不出。
 *
 * 位写入原语复用 bitstream.h：AV1 的 f(n) 与 H.264/HEVC 的 u(n) 编码方式
 * 一致（都是 MSB 优先的固定位宽，AV1 规范 4.10.2 对 f(n) 的定义），
 * 已核对 dmd_bw_put_bits 的实现确认位序相同。AV1 特有的变长编码
 * （leb128 / uvlc / le）在本文件补齐。
 */
#ifndef DMD_AV1_BITSTREAM_H
#define DMD_AV1_BITSTREAM_H

#include <stddef.h>
#include <stdint.h>

#include "bitstream.h"

/* OBU 类型（AV1 规范 6.2.2 表 obu_type）。 */
enum {
    DMD_OBU_SEQUENCE_HEADER        = 1,
    DMD_OBU_TEMPORAL_DELIMITER     = 2,
    DMD_OBU_FRAME_HEADER           = 3,
    DMD_OBU_TILE_GROUP             = 4,
    DMD_OBU_METADATA               = 5,
    DMD_OBU_FRAME                  = 6,
    DMD_OBU_REDUNDANT_FRAME_HEADER = 7,
    DMD_OBU_TILE_LIST              = 8,
    DMD_OBU_PADDING                = 15,
};

/* leb128 最大字节数（AV1 规范 4.10.5：最多 8 字节）。 */
#define DMD_LEB128_MAX 8

/* ---------------------------------------------------------------- 变长编码 */

/* leb128(v)：小端 7 位分组，每字节高位是"还有后续"标志。
 * 写入 out，返回字节数；out_cap 不足返回 0。 */
size_t dmd_av1_leb128(uint64_t v, unsigned char *out, size_t out_cap);

/* leb128 的编码长度，不实际写入——用于先算 obu_size 占几字节。 */
size_t dmd_av1_leb128_len(uint64_t v);

/* uvlc()：AV1 规范 4.10.3。前导零计数 + 尾数，用于 frame_header 里
 * 少数字段（如 timing_info 的 num_units_in_display_tick 不用它，
 * 但 delta_frame_id 之类要）。 */
void dmd_av1_put_uvlc(struct dmd_bitwriter *bw, uint32_t v);

/* le(n)：AV1 规范 4.10.4。n 字节小端整数，**必须字节对齐时调用**。 */
void dmd_av1_put_le(struct dmd_bitwriter *bw, uint64_t v, int nbytes);

/* ns(n)：AV1 规范 4.10.7 的非对称二值编码。tile_info 里
 * context_update_tile_id 与 tile 尺寸推导会用到。 */
void dmd_av1_put_ns(struct dmd_bitwriter *bw, uint32_t v, uint32_t n);

/* su(n)：AV1 规范 4.10.6 的有符号定长。global_motion 等处用。 */
void dmd_av1_put_su(struct dmd_bitwriter *bw, int32_t v, int nbits);

/* ------------------------------------------------------------------ 对齐 */

/* byte_alignment()：AV1 规范 5.3.5。补零至字节边界。
 * ⚠️ 与 H.264 的 rbsp_trailing_bits 不同——AV1 **不写** stop bit，
 * 纯补零。用错会让解码器把填充位当语法元素读。 */
void dmd_av1_byte_align(struct dmd_bitwriter *bw);

/* trailing_bits()：AV1 规范 5.3.4。写一个 1 再补零至字节边界。
 * 用于 OBU **payload 结尾**（sequence_header_obu 与
 * frame_header_obu 结尾要，tile_group_obu 结尾不要）。 */
void dmd_av1_trailing_bits(struct dmd_bitwriter *bw);

/* ---------------------------------------------------------------- OBU 头 */

/* ------------------------------------------------------- 序列头合成（2/4） */

/* 从 VADecPictureParameterBufferAV1 合成一个完整的 OBU_SEQUENCE_HEADER
 * （含 OBU 头 + payload + trailing_bits）。
 *
 * pic 是 const void* 而非具体类型：本头文件刻意不 include va_dec_av1.h，
 * 免得把 libva 依赖扩散到只需要比特原语的调用方。实现里转型。
 *
 * 写入 out，返回总字节数；容量不足或参数非法返回 0。 */
size_t dmd_av1_build_sequence_header(const void *pic,
                                     unsigned char *out, size_t out_cap);

/* obu_header() + obu_size（AV1 规范 5.3.1/5.3.2）。
 *
 * 位布局（共 1 字节，无 extension 时）：
 *   obu_forbidden_bit    f(1)  必须 0
 *   obu_type             f(4)
 *   obu_extension_flag   f(1)  本驱动恒 0（不用可扩展层）
 *   obu_has_size_field   f(1)  本驱动恒 1（low-overhead 格式需要）
 *   obu_reserved_1bit    f(1)  必须 0
 *
 * 写入 out（头 1 字节 + leb128 编码的 payload_len），返回总字节数；
 * out_cap 不足返回 0。调用方随后把 payload 追加在返回位置之后。 */
size_t dmd_av1_obu_header(int obu_type, size_t payload_len,
                          unsigned char *out, size_t out_cap);

#endif
