/* MPEG-2 头合成（实现与设计说明见 mpeg2_bitstream.c 的文件头注释）。 */
#ifndef DMD_MPEG2_BITSTREAM_H
#define DMD_MPEG2_BITSTREAM_H

#include <stddef.h>
#include <va/va.h>

/* sequence_header + sequence_extension。返回写入字节数，0 = 失败/容量不足。
 * iq 可为 NULL（表示不加载量化矩阵，解码器用默认矩阵）。 */
size_t dmd_mpeg2_build_sequence(const VAPictureParameterBufferMPEG2 *pp,
                                const VAIQMatrixBufferMPEG2 *iq,
                                unsigned char *out, size_t out_cap);

/* picture_header + picture_coding_extension。返回写入字节数，0 = 失败。
 * temporal_ref 由调用方按显示序维护（模 1024）。 */
size_t dmd_mpeg2_build_picture(const VAPictureParameterBufferMPEG2 *pp,
                               unsigned int temporal_ref,
                               unsigned char *out, size_t out_cap);

/* 本驱动能否为该帧合成合法码流。0 = 不能（调用方应返回 UNIMPLEMENTED
 * 让上层回落软解，而不是送出会解出坏画面的码流）。 */
int dmd_mpeg2_can_build(const VAPictureParameterBufferMPEG2 *pp);

#endif /* DMD_MPEG2_BITSTREAM_H */
