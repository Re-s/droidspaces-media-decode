/*
 * comm.h - 与解码 daemon 的通信模块
 *
 * 支持两种传输方式：
 *   - TCP loopback（当前 daemon 使用）：地址参数传纯数字端口号
 *   - Unix domain socket：传路径；以 '@' 开头则为 abstract namespace
 *
 * 协议格式：
 *   发送: [4 bytes NALU size big-endian] [NALU data，含 Annex B start code]
 *   接收: [4 bytes width] [4 bytes height] [4 bytes frame_size] [NV12 frame data]
 *
 * 两种帧数据传输模式（握手时协商，daemon 可降级）：
 *   COMM_XFER_TCP  帧数据经 socket 传回，每帧两次内核拷贝
 *   COMM_XFER_SHM  帧数据放在共享内存里，socket 只传槽位号；
 *                  DecodedFrame.data 直接指向共享内存，没有额外拷贝。
 *                  用完必须调 comm_release_frame 归还槽位，否则 daemon 会卡住。
 *
 * 注意：必须交错收发。daemon 单线程串行处理，每收一个 NALU 就尝试写回一帧；
 * 若客户端先发完全部输入再开始接收，daemon 的写会阻塞在满的 socket 缓冲上，
 * 双方僵死并丢失全部解码帧。
 */
#ifndef COMM_H
#define COMM_H

#include <stdint.h>
#include <stddef.h>

/* 帧数据传输模式 */
typedef enum {
    COMM_XFER_TCP = 0,
    COMM_XFER_SHM = 1
} CommXferMode;

/* 解码后的帧数据 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t frame_size;
    uint8_t *data;       /* NV12 格式的帧数据。SHM 模式下直接指向共享内存 */
    size_t   data_alloc; /* data 缓冲区分配大小；SHM 模式下为 0（非自有内存）*/
    int      shm_slot;   /* SHM 模式下的槽位号；TCP 模式为 -1 */
} DecodedFrame;

/* 通信上下文（前向声明） */
typedef struct CommContext CommContext;

/*
 * 连接到 Unix socket。
 * socket_path: socket 文件路径（如 /tmp/anland/display_daemon.sock）。
 * 返回通信上下文，NULL 表示失败。
 */
CommContext *comm_connect(const char *socket_path);

/* daemon 支持的编解码器 id（与 daemon 的 CodecId 保持一致） */
typedef enum {
    COMM_CODEC_H264 = 0,
    COMM_CODEC_HEVC = 1,
    COMM_CODEC_VP9  = 2,
    COMM_CODEC_VP8  = 3
} CommCodecId;

/*
 * 解码器输出格式详情，握手成功后由 daemon 回传。
 *
 * 关键区别：buf_width/buf_height 是**填充后的缓冲尺寸**
 * （高通 Venus 把宽对齐到 128、高对齐到 32，1080p 输出 1920x1088），
 * 而 crop_* 描述的才是**真实显示区域**。渲染或保存图像时应当按 crop 裁剪，
 * 定位 UV 平面时则必须用 stride 与 slice_height。
 */
typedef struct {
    int buf_width;
    int buf_height;
    int stride;         /* Y 平面行距，可能大于 buf_width */
    int slice_height;   /* Y 平面高度，UV 平面从 stride*slice_height 开始 */
    int crop_left;
    int crop_top;
    int crop_right;     /* 闭区间，显示宽 = crop_right - crop_left + 1 */
    int crop_bottom;    /* 闭区间，显示高 = crop_bottom - crop_top + 1 */
} CommFormat;

/*
 * 取回 daemon 回传的格式详情。
 * 返回 0 成功；-1 表示还没收到（第一帧到达后才可用）。
 * 流中途分辨率变化时内容会自动更新，每帧后重新取即可。
 */
int comm_get_format(CommContext *ctx, CommFormat *out);

/*
 * 与 daemon 握手，声明编解码器与分辨率。
 *
 * 必须在任何 comm_send_nalu 之前调用，且只能调用一次。
 * 握手是**必需**的：daemon 靠它确定 mime 与初始尺寸，
 * 缺少握手会被直接拒绝连接（客户端与 daemon 配套发布，无兼容模式）。
 *
 * 返回 0 成功；>0 为 daemon 拒绝的状态码
 * （1=版本不支持 2=未知 codec 3=分辨率超范围 4=缺少握手）；-1 为通信失败。
 */
int comm_handshake(CommContext *ctx, int codec_id, int width, int height);

/*
 * 请求帧数据传输模式。必须在 comm_handshake 之前调用。
 * 默认 COMM_XFER_TCP。请求 SHM 后仍可能被 daemon 降级，
 * 握手后用 comm_get_xfer 查看实际生效的模式。
 */
void comm_set_xfer(CommContext *ctx, CommXferMode mode);

/* 取实际生效的传输模式（握手之后才有意义） */
CommXferMode comm_get_xfer(CommContext *ctx);

/*
 * 归还共享内存槽位。SHM 模式下每处理完一帧都必须调用，
 * 否则池会耗尽，daemon 等约 1 秒后判定客户端卡死并结束会话。
 * TCP 模式下是空操作，无条件调用是安全的。
 */
void comm_release_frame(CommContext *ctx, DecodedFrame *frame);

/*
 * 设置是否自动补 Annex B 起始码。
 *
 * H.264/HEVC 需要（默认开启）：demuxer 产出的 NALU 不含起始码，
 * 而 daemon 依赖起始码定位 nal_unit_header 来识别参数集。
 * VP8/VP9 必须关闭：这类码流没有 Annex B 结构，
 * 补上 4 字节起始码会破坏帧数据，解码器直接拒绝。
 */
void comm_set_annexb(CommContext *ctx, int enable);

/*
 * 发送一个访问单元到 daemon（H.264/HEVC 为一个 NALU，VP8/VP9 为一整帧）。
 * data/size: 数据和大小（不含长度前缀，本函数自动添加）。
 * 返回 0 成功，-1 失败。
 */
int comm_send_nalu(CommContext *ctx, const uint8_t *data, size_t size);

/*
 * 接收一帧解码后的数据。
 * 自动处理帧头（width/height/frame_size）和帧数据。
 * 返回 0 成功，-1 失败，1 表示连接关闭。
 * 调用者不需要释放 DecodedFrame.data，内部复用缓冲区。
 */
int comm_recv_frame(CommContext *ctx, DecodedFrame *frame);

/*
 * 关闭写端（通知 daemon 输入结束），读端仍可接收数据。
 */
void comm_close_write(CommContext *ctx);

/*
 * 关闭连接并释放资源。
 */
void comm_close(CommContext *ctx);

/*
 * 获取底层 socket fd（用于 poll/select 多路复用）。
 */
int comm_get_fd(CommContext *ctx);

#endif /* COMM_H */
