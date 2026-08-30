/*
 * dmd_client.h - decode-daemon 客户端库（供 VA-API driver 内部使用）
 *
 * 这个库把「容器侧作为 Android decode-daemon 的客户端」这件事封装成一个
 * 不透明会话句柄。它最终会被链进 msm_drm_drv_video.so，由 libva 在
 * Firefox / ffmpeg 进程里 dlopen，因此遵守宿主插件的硬约束：
 *
 *   - 绝不 exit() / abort() / assert()：任何错误都通过返回码上报
 *   - 绝不写 stdout；日志走 stderr，且默认静默，DMD_VA_LOG=1 才开
 *   - 无全局/静态可变状态：所有状态在 struct dmd_session 内，
 *     同进程内可并发开多个会话（每个会话一条 TCP 连接、一个 MediaCodec 实例）
 *   - 所有阻塞操作带超时：内部一律 poll + 非阻塞 fd，超时返回 DMD_ERR_TIMEOUT
 *   - 发送用 MSG_NOSIGNAL：对端断开不会给宿主进程投 SIGPIPE
 *   - 所有 fd 带 CLOEXEC：宿主可能 fork/exec，不泄漏 fd
 *   - 只依赖 libc + pthread（实际未用 pthread 原语，但目标链接里已有）
 *
 * 传输模式对调用方是透明的：SHM 模式下 dmd_frame.data 指向共享内存池，
 * TCP 模式下指向会话内部缓冲。两种情况都必须调用 dmd_session_release_frame，
 * 之后 data 失效。这是本库的主要价值 —— 调用方不必分两条路径。
 */
#ifndef DMD_CLIENT_H
#define DMD_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ 返回码 */
/*
 * 约定：0 成功，正数是非错误的流状态，负数是错误。
 * 出错后可用 dmd_session_last_error() 取人类可读的原因（含 errno 描述）。
 */
enum {
    DMD_OK            =  0,
    DMD_EOS           =  1,   /* 对端已关闭写端，且不会再有帧 */

    DMD_ERR_INVAL     = -1,   /* 参数非法 */
    DMD_ERR_NOMEM     = -2,   /* 内存分配失败 */
    DMD_ERR_CONNECT    = -3,  /* connect 失败或超时 */
    DMD_ERR_REJECTED  = -4,   /* daemon 拒绝握手，见 dmd_error.handshake_status */
    DMD_ERR_IO        = -5,   /* V4L2 ioctl 或读写失败 */
    DMD_ERR_TIMEOUT   = -6,   /* 等待超时 */
    DMD_ERR_PROTOCOL  = -7,   /* 收到不符合协议的字节 */
    DMD_ERR_STATE     = -8,   /* 会话状态不允许该操作（如已 finish_input 后再发） */
    DMD_ERR_TOOBIG    = -9    /* 数据单元/帧超出上限（当前无产生点，留作边界检查） */
    /* ⚠️ 曾有 DMD_ERR_ENDPOINT_MISMATCH = -10，用于 socket 架构下
     * "客户端 stat 到的 endpoint 与 daemon 上报的不一致"。
     * 0.4.0 起驱动直接打开 /dev/video32，没有 endpoint 也没有 daemon，
     * 该错误码零产生点，已删除。 */
};

/* codec id。0.4.0 前是线协议的一部分（与已删除的 decode-daemon.c 的
 * CodecId 逐值一致），现在只在驱动内部用于选择 V4L2 pixelformat。
 * ⚠️ 仍然只能在末尾追加、不可重排：这些值散布在 profiles.c 的能力表与
 * dmd_v4l2_session.c 的格式映射里，重排不会报错但会静默解错码。 */
enum {
    DMD_CODEC_H264 = 0,
    DMD_CODEC_HEVC = 1,
    DMD_CODEC_VP9  = 2,
    DMD_CODEC_VP8  = 3,
    DMD_CODEC_AV1  = 4
};

/* 帧数据传输模式 */
enum {
    DMD_XFER_TCP = 0,
    DMD_XFER_SHM = 1
};

/* 上行数据单元上限（与 daemon 的 MAX_FRAME 一致，超出会被 daemon 判为非法长度） */
#define DMD_MAX_UNIT_BYTES (8u * 1024u * 1024u)
/* 下行帧的合理性上限。注意它必须远大于 DMD_MAX_UNIT_BYTES：
 * 4K NV12 单帧就是 12441600 字节，daemon 的 8MB 只约束输入侧。 */
#define DMD_MAX_FRAME_BYTES (64u * 1024u * 1024u)

/* ------------------------------------------------------------ 配置 */
/* V4L2 直通只需要这五项。
 * 0.3.x 这里还有 port / sock_path / want_shm 三个字段（daemon 的 TCP 端口、
 * Unix socket 路径、是否请求 SHM 传输）—— 会话改为直接打开 /dev/video32 后
 * 它们不再被任何代码读取，已在 0.4.0 删除。 */
struct dmd_session_config {
    int      codec;             /* DMD_CODEC_* */
    int      width;             /* 96..8192 */
    int      height;            /* 96..4320 */
    int      connect_timeout_ms;/* <=0 表示用默认 2000 */
    int      io_timeout_ms;     /* 单次收发的默认超时，<=0 表示用默认 5000 */
};

/* 用默认值填充 cfg（H.264、2s/5s 超时）。cfg 为 NULL 时无操作。 */
void dmd_session_config_init(struct dmd_session_config *cfg);

/* ------------------------------------------------------------ 错误详情 */
struct dmd_error {
    int  code;              /* DMD_ERR_* */
    int  handshake_status;   /* code==DMD_ERR_REJECTED 时为 daemon 的 status
                              * 1=版本不支持 2=codec 不支持 3=分辨率超范围
                              * 4=缺少握手；其他情况为 0 */
    char msg[192];          /* 人类可读原因，总是以 '\0' 结尾 */
};

/* ------------------------------------------------------------ 格式 */
/*
 * 解码器输出缓冲的几何信息，来自 daemon 的格式描述块。
 *
 * buf_width/buf_height 是**填充后的**缓冲尺寸（Venus 把宽对齐 128、高对齐 32，
 * 1080p 输出 1920x1088）；真实显示区由 crop 给出，且 crop 是**闭区间**：
 *   显示宽 = crop_right - crop_left + 1
 *   显示高 = crop_bottom - crop_top + 1
 * 定位 UV 平面必须用 stride/slice_height：UV 起点 = stride * slice_height。
 */
struct dmd_format {
    int buf_width;
    int buf_height;
    int stride;
    int slice_height;
    int crop_left;
    int crop_top;
    int crop_right;
    int crop_bottom;
    int valid;          /* 0 = 还没收到过格式块 */
    int changes;        /* 收到格式块的次数，>1 说明流内分辨率变过 */
};

/* 显示尺寸便捷取值（crop 闭区间换算），fmt 无效时返回 0 */
int dmd_format_display_width(const struct dmd_format *fmt);
int dmd_format_display_height(const struct dmd_format *fmt);

/* ------------------------------------------------------------ 帧 */
/*
 * 一帧 NV12 数据。data 的所有权始终属于库：
 *   SHM 模式 → 指向共享内存槽位，release 时把状态字置 0 归还给 daemon
 *   TCP 模式 → 指向会话内部接收缓冲，release 只是把它标记为可复用
 * 两种情况下 release 之后 data 都失效，调用方要么用完再 release，
 * 要么自己先拷走。忘记 release 在 SHM 模式下会让 daemon 约 1 秒后判定
 * 客户端卡死并结束会话，因此 release 是强制的。
 */
struct dmd_frame {
    uint8_t *data;
    size_t   size;

    /* 帧头声明的尺寸（daemon 用的是缓冲尺寸，与 fmt.buf_* 一致） */
    uint32_t width;
    uint32_t height;

    /* 该帧对应的**输入单元序号**（1 起），0 = daemon 未提供。
     *
     * 由 daemon 在 queueInputBuffer 时写入 presentationTimeUs，
     * MediaCodec 原样带到输出帧上。用途是精确配对：调用方据此就知道
     * 这一帧属于第几次提交，**无需知道解码器按什么顺序出帧**。
     * 旧 daemon 不带此字段时为 0，调用方需回退到按顺序推断。 */
    uint32_t unit_seq;

    /* 该帧生效的几何信息，从最近一次格式描述块复制而来，随帧自洽 */
    int stride;
    int slice_height;
    int crop_left;
    int crop_top;
    int crop_right;
    int crop_bottom;

    int      shm_slot;   /* >=0 = SHM 槽位号；-1 = TCP 内部缓冲 */
    uint64_t seq;        /* 会话内帧序号，从 0 起 */
};

/* ------------------------------------------------------------ 会话 */
struct dmd_session;

/*
 * 建立会话：连接 daemon、完成握手、（若请求）领取共享内存池。
 * 返回 NULL 表示失败；err 非 NULL 时写入失败原因（含 daemon 的拒绝码）。
 * 失败时不会有任何 fd 或映射泄漏。
 */
struct dmd_session *dmd_session_create(const struct dmd_session_config *cfg,
                                       struct dmd_error *err);

/* 释放会话：关连接、解映射、释放缓冲。s 为 NULL 时无操作。 */
void dmd_session_destroy(struct dmd_session *s);

/*
 * 送一个数据单元。
 *   H.264/HEVC：一个 NALU，**必须带 Annex B 起始码**（3 或 4 字节）——
 *               daemon 靠起始码定位 nal_unit_header 识别 SPS/PPS/VPS
 *   VP8/VP9：   一个完整帧，**不得带起始码**
 * 本库不替调用方补起始码：补错会静默破坏码流，交由上层显式负责。
 * H.264/HEVC 缺起始码时直接返回 DMD_ERR_PROTOCOL，早失败早发现。
 * 返回 DMD_OK / DMD_ERR_*。
 */
int dmd_session_send_unit(struct dmd_session *s, const void *data, size_t len);

/*
 * 关闭写端（shutdown(SHUT_WR)），触发 daemon 送 EOS 并 flush 解码器。
 * 送入的数据单元数 != 返回的帧数（排队、重排序、参数集不产帧），
 * 因此必须调用本函数才能取到全部剩余帧。可重复调用（幂等）。
 */
int dmd_session_finish_input(struct dmd_session *s);

/*
 * 可逆排空：送 V4L2_DEC_CMD_STOP 逼解码器吐出在手的帧，收齐后复位
 * 并重送 CSD —— 与 finish_input 的区别是**会话之后仍然可用**。
 *
 * 用途：消费者只保持少量帧在飞（浏览器是 3 帧），而解码器有 B 帧时要收到
 * 第 4 个输入单元才吐首帧，双方互等。此时调用本函数即可拿到帧且不作废会话。
 * 原先只能用 finish_input 打破互等，代价是每次都要重建会话
 * （实测每帧 155 ms，播放慢 4.7 倍）。
 *
 * 排空后硬件会重新协商格式，next_frame 内部自动消费，调用方无感。
 * 返回 DMD_OK / DMD_ERR_*。
 */
int dmd_session_drain(struct dmd_session *s);

/*
 * 取下一帧。timeout_ms <0 表示用配置里的 io_timeout_ms。
 * 返回 DMD_OK 拿到帧、DMD_EOS 流结束、DMD_ERR_TIMEOUT 暂时无帧（会话仍可用）、
 * 其他负值为错误。内部自动消费格式描述块并更新 dmd_session_format()。
 *
 * 注意：帧头到达后剩余字节按 io_timeout_ms 收，不会因为 timeout_ms 很小而
 * 留下半帧；超时只发生在「一个字节都还没来」的边界上。
 */
int dmd_session_next_frame(struct dmd_session *s, struct dmd_frame *out,
                           int timeout_ms);

/*
 * 归还帧。SHM 模式归还槽位，TCP 模式解除内部缓冲占用。
 * 两种模式都必须调用，无条件调用是安全的（f 为 NULL 或已归还时无操作）。
 */
int dmd_session_release_frame(struct dmd_session *s, struct dmd_frame *f);

/* 最近一次格式描述块的内容。返回的指针在会话生命期内有效。 */
const struct dmd_format *dmd_session_format(const struct dmd_session *s);

/* 最近一次错误的人类可读原因；无错误时返回空字符串，绝不返回 NULL。 */
const char *dmd_session_last_error(const struct dmd_session *s);

/* 最近一次错误码（DMD_ERR_*），无错误时 0 */
int dmd_session_last_error_code(const struct dmd_session *s);

/* 实际生效的传输模式：DMD_XFER_TCP / DMD_XFER_SHM。
 * 请求 SHM 也可能因交接失败被静默降级，用它核实。 */
int dmd_session_xfer_mode(const struct dmd_session *s);

/* 底层 socket fd，供调用方自己 poll（不要 close 它） */
int dmd_session_fd(const struct dmd_session *s);

/* 统计：送入的数据单元数 / 取出的帧数 */
uint64_t dmd_session_units_sent(const struct dmd_session *s);
uint64_t dmd_session_frames_received(const struct dmd_session *s);

#ifdef __cplusplus
}
#endif

#endif /* DMD_CLIENT_H */
