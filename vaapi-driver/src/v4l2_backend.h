/*
 * v4l2_backend —— 直接对 msm_vidc 的 V4L2 M2M 接口解码，绕开 MediaCodec/Codec2。
 *
 * 存在理由：SM8750 的 Codec2 层缺 AV1 支持 —— c2.qti.av1.decoder 在组件创建
 * 阶段就失败，logcat 可见
 *     QC2VppFilterCaps: Unknown codectype=c2.qti.av1.decoder
 *     QueryrequiredInfos from downstream failed!
 * 下游 VPP filter 不认识 AV1，组件起不来。但硬件与内核驱动完全可用：
 * /dev/video32 的 OUTPUT_MPLANE 枚举出 AV10，实测 V4L2 直通能解出帧，
 * 且像素与 ffmpeg 软解逐点完全一致（tools/v4l2_dec_probe.c 的验证结论）。
 *
 * 因此本后端只服务于 MediaCodec 走不通的 codec（当前是 AV1）。
 * H264/HEVC/VP8/VP9 继续走 MediaCodec —— 那条路已经稳定，不引入变数。
 *
 * 关键约束（都是实测踩出来的，改动时务必保持）：
 *
 * 1. 只接受 DMABUF。REQBUFS 报 capabilities 里含 MMAP，但 QUERYBUF 给出的
 *    offset 无法 mmap（ENODEV）。必须从 /dev/dma_heap/system 分配后以
 *    V4L2_MEMORY_DMABUF 传 fd。
 *
 * 2. 必须两阶段 stateful 流程，不能双向一起 STREAMON：
 *      S_FMT(OUTPUT) → REQBUFS/STREAMON(OUTPUT) → 送含序列头的单元
 *      → 等 V4L2_EVENT_SOURCE_CHANGE → G_FMT(CAPTURE)
 *      → S_FMT(CAPTURE/NV12) → REQBUFS/STREAMON(CAPTURE) → 出帧
 *    跳过协商会让固件按错误假设解析，dmesg 里是
 *    "av1DecParseFrame: AV1 ERROR code 8c000060"，表现为 0 帧。
 *
 * 3. CAPTURE 默认格式是 QCOM 压缩的 Q08C，必须显式 S_FMT 成 NV12 才得到
 *    线性帧（1080p 下 sizeimage 从 3219456 变为 3133440）。
 */
#ifndef DMD_V4L2_BACKEND_H
#define DMD_V4L2_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* 与 decode-daemon.c 的 CodecId 对应（wire id，追加式，不可重排）。
 *
 * VP8 不在此列且不再支持：msm_vidc 的 V4L2 层没有 VP80 格式，
 * Android 上它一直是靠 c2.android.vp8.decoder 软解。既然整体切到 V4L2，
 * VP8 就没有硬件路径可走，明确放弃。 */
enum {
    DMD_V4L2_CODEC_H264 = 0,
    DMD_V4L2_CODEC_HEVC = 1,
    DMD_V4L2_CODEC_VP9  = 2,
    /* 3 = VP8，已废弃，保留编号不复用 */
    DMD_V4L2_CODEC_AV1  = 4,
};

#define DMD_V4L2_MAX_OUT 8    /* 输入（码流）缓冲数 */
#define DMD_V4L2_MAX_CAP 24   /* 输出（像素）缓冲数，须 >= 驱动要求的最小值 */

struct dmd_v4l2_buf {
    int      dbuf_fd;         /* dma-heap 分配的 dmabuf，-1 表示未分配 */
    void    *map;             /* mmap 到用户空间的地址，NULL 表示未映射 */
    size_t   length;
    int      queued;          /* 1 = 已在驱动队列里 */
};

struct dmd_v4l2_dec {
    int      fd;                              /* /dev/videoN */
    int      heap_fd;                         /* /dev/dma_heap/system */

    int      w, h;                            /* 协商后的对齐尺寸 */
    int      crop_w, crop_h;                  /* 有效显示区域 */
    unsigned in_size;                         /* OUTPUT 单缓冲字节数 */
    unsigned cap_size;                        /* CAPTURE 单缓冲字节数 */
    int      stride;                          /* CAPTURE 的行距 */
    int      slice_height;                    /* CAPTURE 的平面高度 */

    int      n_out, n_cap;
    struct dmd_v4l2_buf out[DMD_V4L2_MAX_OUT];
    struct dmd_v4l2_buf cap[DMD_V4L2_MAX_CAP];

    int      cap_ready;                       /* 1 = 已完成分辨率协商并 STREAMON */
    int      out_streaming, cap_streaming;
    int      draining;                        /* 已送 EOS，等剩余帧 */
    int      saw_eos;
};

/*
 * 打开解码器并完成 OUTPUT 侧配置（第一阶段）。
 * codec_id 目前只支持 DMD_V4L2_CODEC_AV1。
 * w/h 是客户端声明的尺寸，用于初始 S_FMT；真实尺寸由 SOURCE_CHANGE 后
 * G_FMT(CAPTURE) 给出，可能不同（如 1080 → 1088 对齐）。
 * 返回 0 成功，-1 失败（失败时已清理，无需再调 close）。
 */
int dmd_v4l2_open(struct dmd_v4l2_dec *d, int codec_id, int w, int h);

/*
 * 送一个访问单元。data/len 是完整的 temporal unit（AV1）。
 * pts_us 原样透传给驱动的 timestamp，出帧时回传，用于与提交序号配对。
 * 返回 0 成功入队，1 表示当前无空闲输入缓冲（调用方应先收帧再重试），
 * -1 表示出错。
 */
int dmd_v4l2_send(struct dmd_v4l2_dec *d, const uint8_t *data, size_t len,
                  uint64_t pts_us);

/*
 * 取一帧。阻塞至多 timeout_ms。
 * 成功时 *out_data 指向 NV12 数据（属于后端的 dmabuf 映射，调用方须在
 * dmd_v4l2_release 之前用完），*out_len 为字节数，*out_pts 为回传的时间戳。
 * 返回 1 拿到帧，0 超时（无帧），-1 出错，2 表示收到 EOS（流结束）。
 *
 * 首次调用会顺带完成第二阶段（等 SOURCE_CHANGE → 配置 CAPTURE），
 * 所以调用方在 send 首个单元之后必须开始 recv，否则协商不会推进。
 */
int dmd_v4l2_recv(struct dmd_v4l2_dec *d, uint8_t **out_data, size_t *out_len,
                  uint64_t *out_pts, int *out_index, int timeout_ms);

/* 把 recv 拿到的帧缓冲还给驱动。index 用 recv 输出的值。 */
int dmd_v4l2_release(struct dmd_v4l2_dec *d, int index);

/* 送 EOS，让驱动吐出流水线里剩余的帧。之后继续 recv 直到返回 2。
 * 这是**不可逆**的：之后不应再送料。用于会话收尾。 */
int dmd_v4l2_drain(struct dmd_v4l2_dec *d);

/*
 * 可逆排空：催出流水线里已积压的帧，但保持会话可继续送料。
 *
 * 为什么需要它：VA-API 上层（decode.c 的 SyncSurface）在等帧超时时会调
 * 排空来打破"解码器攒够输入才吐帧、调用方等帧才继续送料"的互等。它假设
 * 排空是可逆的 —— 在 MediaCodec 下确实如此（EOS + flush 后会话仍可用）。
 *
 * V4L2 下用 DECODER_CMD(STOP) → 收完带 LAST 标记的帧 → DECODER_CMD(START)
 * 表达同一语义。STOP 让解码器吐完已有帧，START 让它回到可接收状态。
 * 这是 V4L2 规范定义的 drain-and-resume 序列（dev-decoder.rst）。
 *
 * 直接用不可逆的 drain 会终结整条流：实测表现为 ffmpeg 送 6 单元只回 2 帧、
 * 4 帧待配对、报 internal decoding error。
 *
 * 返回 0 成功，-1 失败（此时调用方应退回不可逆路径）。
 */
int dmd_v4l2_drain_reversible(struct dmd_v4l2_dec *d);

/* 关闭并释放全部资源。可重复调用。 */
void dmd_v4l2_close(struct dmd_v4l2_dec *d);

/*
 * 探测：本机的 V4L2 解码节点是否支持该 codec。
 * 不打开会话，只做 ENUM_FMT，代价很低。用于 daemon 启动时决定后端。
 * 返回 1 支持，0 不支持。
 */
int dmd_v4l2_probe(int codec_id);

#endif /* DMD_V4L2_BACKEND_H */
