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

/* --------------------------------------------- OBU_FRAME 组装（3/4 + 4/4） */

/*
 * 自洽 DPB 状态（影子参考帧管理）。
 *
 * 为什么需要它：refresh_frame_flags 与 ref_frame_idx 都由本模块写入码流，
 * 而 VA-API 不提供 refresh_frame_flags（那是编码器的 GOP 决策，随源码流
 * 被解析后丢弃）。解决办法不是去还原编码器的选择，而是自己维护一套
 * **自洽**的槽位分配：我说本帧存进槽 k，之后引用它时就报槽 k。
 * 解码器只要求两者一致，不要求与原编码器相同。
 *
 * 由调用方（每个解码 context）持有一份，跨帧保持。
 * 新建 context 时整体清零即为初始状态。
 */
struct dmd_av1_dpb {
    /* ⚠️ 影子表存**本驱动的帧号**（frame_seq，从 1 起），不是 surface id。
     * ffmpeg 会回收复用 surface：同一个 VASurfaceID 先后属于不同帧
     * （实测 160 帧样本里 oh1 与 oh7 同用 surface 6），surface 做身份
     * 会把死帧和复用它的活帧混为一谈。0 表示空槽。 */
    int         dpb_shadow[8];     /* 槽 -> 该槽当前存的帧号 */
    unsigned    dpb_order_hint[8]; /* 槽 -> 该槽帧的 order hint。
                                    * error_resilient 帧的帧头要逐槽写出
                                    * （规范 5.9.2 的 ref_order_hint[i]）。 */
    unsigned    dpb_next_slot;     /* 下一个要写入的槽，8 槽轮转 */
    int         frame_seq;         /* 已合成帧计数，本帧号 = ++frame_seq */

    /* surface -> 最近拥有它的帧号。引用翻译与 E 识别都要经过它：
     * ref_frame_map 里给的是 surface，而同一 surface 可能属于多个
     * 历史帧，"最近拥有者"正是还活着的那个（死帧的 surface 被回收后
     * 归新帧所有）。表满时线性替换最旧一项。 */
    struct { VASurfaceID surf; int frame; } surf_hist[64];
    int         surf_hist_n;

    /* 上一帧 refresh_frame_flags 字段在其帧头内的**位**偏移。
     * 正确值要等下一帧的 ref_frame_map 才能算出，届时用它就地改写。
     * SIZE_MAX 表示上一帧没有该字段（KEY+show 帧不写入）。 */
    size_t      last_refresh_bitpos;

    /* 上一帧（即 last_refresh_bitpos 所属那一帧）的 order hint。
     * patch 反算出真实 refresh 后要把该帧登记进影子槽，槽的 order hint
     * 是解码器推导 skipModeAllowed（5.9.22）的输入，必须一起记。 */
    unsigned    last_oh;

    /* 同理记录 disable_frame_end_update_cdf 的位偏移。
     *
     * 为什么需要：双趟解码（第一趟 refresh=0 只取像素，第二趟带真实
     * refresh 补 DPB）会让 AV1 的**帧上下文回写**发生两次。规范 7.19
     * 里帧解码结束时执行
     *     if ( !disable_frame_end_update_cdf )
     *         SetFrameContext( ref_frame_idx[ primary_ref_frame ] )
     * 把本帧的 CDF/delta-q/环路滤波等终态**拷贝**进 primary 参考帧所在
     * 槽的上下文。第一趟已经把它改成本帧终态，第二趟再读就是错的起点
     * —— 于是第二趟解出一帧垃圾并放进 DPB，后续引用全废。
     *
     * 修法：第一趟把这一位强制写 1（不回写），第二趟还原成源码流真值
     * （由它完成唯一一次回写）。两趟的起点上下文相同 → 第二趟像素与
     * 第一趟逐位相同，DPB 与 CDF 同时正确。
     * SIZE_MAX 表示码流里没有这个字段（disable_cdf_update=1 时推断为 1，
     * 本来就不回写，无需处理）。 */
    size_t      last_endupd_bitpos;

    /* 上一帧的 ref_frame_map 快照，用于与本帧的 map 做差分。 */
    VASurfaceID prev_ref_map[8];
    int         prev_valid;

    /* 源 DPB 在上一帧解码时踢出的帧（E_{k-1} = map_{k-1} \ map_k），
     * 以帧号表示。源编码器保证被踢出的帧不会再被任何后续帧引用
     * （死了），本驱动让当前帧占它的影子槽，就永远不会覆盖活引用。
     * 单槽差分算不出（如 KEY 全刷后的多槽变化）时置 valid=0。 */
    int         evict_frame;
    int         evict_valid;
};

/*
 * 用本帧的 ref_frame_map 反算**上一帧**的 refresh_frame_flags，并就地改写
 * 上一帧已合成的字节。
 *
 * 原理（实测验证，4 帧全部命中源码流真实值 1/8/32/64）：
 *   第 N 帧的 refresh_frame_flags = 第 N+1 帧 map 与第 N 帧 map 的差异位
 * 因为"本帧写入了哪些槽"正是在下一帧的 DPB 快照里显现出来的。
 *
 * 这是唯一可行的推导方式。已否决的四种（细节见 .c 内注释）：
 *   1. 在本帧 ref_frame_map 里找 current_frame —— 该数组是解码**前**快照
 *   2. 本帧 map 与**上**帧 map 差分 —— 方向错，给出的是上帧的 refresh
 *   3. 恒 1 或 8 槽轮转 1<<slot —— 无法表达 refresh=0（不占槽的帧），
 *      而真实序列里这类帧占三分之一
 *   4. 取第一个空槽 / 未被引用的槽 —— 实测仅首帧碰对
 *
 * prev_frame_bytes 指向上一帧帧头所在的缓冲（调用方保存），
 * 该缓冲必须在本次调用时仍然有效且尚未送入解码器。
 */
/* 合成一个 show_existing_frame 帧头 OBU（规范 5.9.2）。
 *
 * 硬件不输出 show_frame=0 的帧（符合规范），但会为每个 SEF 头复显一次。
 * 源码流正是这么做的：150 个 OBU_FRAME 里只有 80 个 show_frame=1，
 * 另外 70 帧靠 70 个 SEF 头复显，合计 150 帧输出。
 * 实测源码流直喂硬件是"送 150 收 150"，而缺 SEF 头的合成流只收 80。
 *
 * 结构极简（实测源码流的 SEF 全是 2 字节）：
 *     OBU 头 0x1a（type=3 FRAME_HEADER, has_size=1）
 *     leb128 size = 1
 *     载荷 1 字节：show_existing_frame(1)=1 + frame_to_show_map_idx(3)
 *                  + trailing_bits
 * 例：map_idx=6 → 载荷 0xe8；map_idx=5 → 0xd8；map_idx=3 → 0xb8。
 *
 * buf 至少 4 字节。返回写入长度，失败返回 0。 */
size_t dmd_av1_build_show_existing(unsigned char *buf, size_t cap,
                                   unsigned map_idx);

void dmd_av1_patch_prev_refresh(struct dmd_av1_dpb *dpb,
                                const void *cur_pic,
                                unsigned char *prev_frame_bytes,
                                size_t prev_len,
                                size_t prev_bitpos,
                                int prev_frame);

/* 一个 tile 的位置与长度描述，供 dmd_av1_build_frame() 组装 tile_group。 */
struct dmd_av1_tile {
    const unsigned char *data;
    size_t               len;
};

/* 合成一个完整的 OBU_FRAME(6)：帧头 + byte_alignment + tile_group。
 *
 * ⚠️ 为什么用 OBU_FRAME 而不是分离的 FRAME_HEADER(3) + TILE_GROUP(4)：
 * 实测 dav1d 对分离形式报 "Failed to read unit 0 (type 3)"，而合并形式
 * 直接通过。libaom 生成的真实码流用的也是 OBU_FRAME。
 *
 * ⚠️ 关键差别：OBU_FRAME 内的帧头结尾用 **byte_alignment（纯补零）**，
 * 而不是 trailing_bits（写 1 再补零）—— 规范 5.10.1 frame_obu() 里
 * frame_header_obu() 之后紧跟 byte_alignment()。用错会让 tile_group
 * 的起始位置偏移，dav1d 就是这么报错的。
 *
 * tiles 按 tile_row-major 顺序给出，数量必须等于 tile_cols*tile_rows。
 * 写入 out，返回总字节数；容量不足或参数非法返回 0。 */
size_t dmd_av1_build_frame(const void *pic,
                           const struct dmd_av1_tile *tiles, int num_tiles,
                           unsigned char *out, size_t out_cap,
                           struct dmd_av1_dpb *dpb);

/* ------------------------------------------------------- 帧头合成（3/4） */

/* 从 VADecPictureParameterBufferAV1 合成一个完整的 OBU_FRAME_HEADER
 * （含 OBU 头 + payload + trailing_bits）。
 *
 * tile_cols/tile_rows 取自 pic 参数；tile_size_bytes 固定 4 字节，
 * 与第 4 步 tile_group 里写 tile_size_minus_1 的宽度必须一致。
 *
 * 写入 out，返回总字节数；容量不足或参数非法返回 0。 */
size_t dmd_av1_build_frame_header(const void *pic,
                                  unsigned char *out, size_t out_cap,
                                  struct dmd_av1_dpb *dpb);

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
