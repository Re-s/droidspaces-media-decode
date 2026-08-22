/*
 * comm.c - Unix Socket 通信模块
 *
 * 通过 Unix domain socket 与 Android 侧 MediaCodec 解码 daemon 通信。
 * 协议：大端序 4 字节长度前缀 + 数据。
 */
#include "comm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/mman.h>
#include <stddef.h>

/* 握手常量，须与 daemon 的 decode-daemon.c 保持一致 */
#define HELLO_MAGIC      0x444D4400u
#define HELLO_VERSION    2      /* v2: 增加共享内存传输协商 */
#define FMTDESC_BYTES    32
/* 帧头 frame_size 取该值表示"这不是帧，而是格式描述块" */
#define FMTDESC_SENTINEL 0xFFFFFFFFu
/* 帧头 frame_size 取该值表示帧数据在共享内存里，随后 8 字节是槽位号+长度 */
#define SHMFRAME_SENTINEL 0xFFFFFFFEu
#define SHM_CTRL_BYTES    4096

/* 内部上下文 */
struct CommContext {
    int fd;              /* socket 文件描述符 */
    uint8_t *recv_buf;   /* 接收缓冲区 */
    size_t   recv_buf_size;

    int negotiated;      /* 已完成握手 */
    CommFormat fmt;      /* 格式描述块内容 */
    int annexb;          /* 是否自动补 Annex B 起始码，默认 1 */

    /* 共享内存传输 */
    CommXferMode want_xfer;   /* 请求的模式 */
    CommXferMode xfer;        /* 实际生效的模式 */
    uint8_t     *shm_base;    /* mmap 基址，NULL 表示未启用 */
    size_t       shm_slot;    /* 单槽字节数 */
    size_t       shm_total;   /* 池总字节数 */
    int          shm_slots;   /* 槽位数 */
};

/*
 * 连到 daemon 指定的 abstract socket，用 SCM_RIGHTS 领取 memfd 并 mmap。
 *
 * 名字由 daemon 决定（客户端不知道自己是第几个连接，猜名字必然串台），
 * 通过握手响应传来。用 abstract socket 是因为容器与 Android 共享
 * net namespace，而 mount namespace 隔离 —— 路径形式的 Unix socket 不可用。
 */
static int shm_attach(CommContext *ctx, const char *name)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("[comm] abstract socket 创建失败"); return -1; }

    struct sockaddr_un ua;
    memset(&ua, 0, sizeof(ua));
    ua.sun_family = AF_UNIX;
    ua.sun_path[0] = 0;                     /* abstract namespace */
    strncpy(ua.sun_path + 1, name, sizeof(ua.sun_path) - 2);
    socklen_t ulen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                 + 1 + strlen(name));
    if (connect(sock, (struct sockaddr *)&ua, ulen) < 0) {
        fprintf(stderr, "[comm] 连接 %s 失败: %s\n", name, strerror(errno));
        close(sock);
        return -1;
    }

    /* 12 字节参数 + SCM_RIGHTS 里的 memfd */
    uint32_t meta[3];
    struct iovec io = { meta, sizeof(meta) };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &io; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof(cbuf);

    ssize_t n = recvmsg(sock, &mh, 0);
    if (n != (ssize_t)sizeof(meta)) {
        fprintf(stderr, "[comm] 领取共享内存参数失败\n");
        close(sock);
        return -1;
    }
    int mfd = -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            memcpy(&mfd, CMSG_DATA(cm), sizeof(int));
            break;
        }
    }
    close(sock);
    if (mfd < 0) {
        fprintf(stderr, "[comm] 未收到 memfd\n");
        return -1;
    }

    ctx->shm_slots = (int)ntohl(meta[0]);
    ctx->shm_slot  = (size_t)ntohl(meta[1]);
    ctx->shm_total = (size_t)ntohl(meta[2]);

    ctx->shm_base = mmap(NULL, ctx->shm_total, PROT_READ | PROT_WRITE,
                         MAP_SHARED, mfd, 0);
    close(mfd);                             /* mmap 之后 fd 就不需要了 */
    if (ctx->shm_base == MAP_FAILED) {
        perror("[comm] mmap 共享内存失败");
        ctx->shm_base = NULL;
        return -1;
    }
    fprintf(stderr, "[comm] 共享内存已挂载: %d 槽 x %zu 字节\n",
            ctx->shm_slots, ctx->shm_slot);
    return 0;
}

void comm_set_annexb(CommContext *ctx, int enable)
{
    if (ctx) ctx->annexb = enable ? 1 : 0;
}

/*
 * 确保接收缓冲区足够大。
 */
static int ensure_recv_buf(CommContext *ctx, size_t needed)
{
    if (ctx->recv_buf_size >= needed) return 0;
    size_t new_size = needed * 2;
    if (new_size < 65536) new_size = 65536;
    uint8_t *new_buf = realloc(ctx->recv_buf, new_size);
    if (!new_buf) return -1;
    ctx->recv_buf = new_buf;
    ctx->recv_buf_size = new_size;
    return 0;
}

/*
 * 可靠读取：确保读取指定字节数，处理部分读取。
 */
static int reliable_read(int fd, void *buf, size_t count)
{
    /* Set read timeout to avoid indefinite blocking */
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t *p = (uint8_t *)buf;
    size_t remaining = count;
    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "[comm] read timeout\n");
                return -1;
            }
            return -1;
        }
        if (n == 0) return -1;
        p += n; remaining -= n;
    }
    return 0;
}

/*
 * 可靠写入：确保写入指定字节数，处理部分写入。
 */
static int reliable_write(int fd, const void *buf, size_t count)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = count;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        remaining -= n;
    }
    return 0;
}

CommContext *comm_connect(const char *socket_path)
{
    CommContext *ctx = calloc(1, sizeof(CommContext));
    if (!ctx) return NULL;

    ctx->fd        = -1;
    ctx->annexb    = 1; /* 默认补起始码，适配 H.264/HEVC；VP8/VP9 需显式关闭 */
    ctx->want_xfer = COMM_XFER_TCP;
    ctx->xfer      = COMM_XFER_TCP;

    /* 检测是否为 TCP 端口号（纯数字） */
    int is_port = 1;
    for (int i = 0; socket_path[i]; i++) {
        if (socket_path[i] < '0' || socket_path[i] > '9') { is_port = 0; break; }
    }

    if (is_port && strlen(socket_path) > 0) {
        /* TCP 模式：连接 127.0.0.1:port */
        int port = atoi(socket_path);
        ctx->fd = socket(AF_INET, SOCK_STREAM, 0);
        if (ctx->fd < 0) { perror("[comm] TCP socket() 失败"); goto fail; }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        if (connect(ctx->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "[comm] 无法连接到 TCP 127.0.0.1:%d: %s\n", port, strerror(errno));
            goto fail;
        }
        /* Disable Nagle for low-latency frame delivery */
        int flag = 1;
        setsockopt(ctx->fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        printf("[comm] 已连接到 TCP daemon: 127.0.0.1:%d\n", port);
    } else {
        /* Unix domain socket */
        ctx->fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (ctx->fd < 0) { perror("[comm] socket() 失败"); goto fail; }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;

        if (socket_path[0] == '@') {
            /* Abstract namespace socket */
            addr.sun_path[0] = '\0';
            strncpy(addr.sun_path + 1, socket_path + 1, sizeof(addr.sun_path) - 2);
            socklen_t addr_len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(socket_path + 1);
            if (connect(ctx->fd, (struct sockaddr *)&addr, addr_len) < 0) {
                fprintf(stderr, "[comm] 无法连接到 abstract socket %s: %s\n", socket_path, strerror(errno));
                goto fail;
            }
        } else {
            /* 文件系统 socket 路径 */
            strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
            if (connect(ctx->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                fprintf(stderr, "[comm] 无法连接到 %s: %s\n", socket_path, strerror(errno));
                goto fail;
            }
        }
        printf("[comm] 已连接到 daemon: %s\n", socket_path);
    }

    /* 初始接收缓冲区 256KB */
    ctx->recv_buf_size = 256 * 1024;
    ctx->recv_buf = malloc(ctx->recv_buf_size);
    if (!ctx->recv_buf) goto fail;

    return ctx;

fail:
    comm_close(ctx);
    return NULL;
}

int comm_handshake(CommContext *ctx, int codec_id, int width, int height)
{
    if (!ctx || ctx->fd < 0) return -1;
    if (ctx->negotiated) {
        fprintf(stderr, "[comm] 握手只能进行一次\n");
        return -1;
    }

    uint32_t hello[6] = {
        htonl(HELLO_MAGIC), htonl(HELLO_VERSION),
        htonl((uint32_t)codec_id), htonl((uint32_t)width), htonl((uint32_t)height),
        htonl((uint32_t)ctx->want_xfer)
    };
    if (reliable_write(ctx->fd, hello, sizeof(hello)) < 0) {
        perror("[comm] 发送握手失败");
        return -1;
    }

    /* 响应: [4B status][4B 实际模式][4B 名字长度][名字...] */
    uint32_t head[3];
    if (reliable_read(ctx->fd, head, sizeof(head)) != 0) {
        fprintf(stderr, "[comm] 未收到握手响应（daemon 版本不匹配？客户端与 daemon 需配套）\n");
        return -1;
    }
    uint32_t status = ntohl(head[0]);
    uint32_t mode   = ntohl(head[1]);
    uint32_t nlen   = ntohl(head[2]);

    char name[64] = { 0 };
    if (nlen > 0) {
        if (nlen >= sizeof(name)) {
            fprintf(stderr, "[comm] 共享内存名字过长: %u\n", nlen);
            return -1;
        }
        if (reliable_read(ctx->fd, name, nlen) != 0) {
            fprintf(stderr, "[comm] 读取共享内存名字失败\n");
            return -1;
        }
    }

    if (status != 0) {
        const char *why = (status == 1) ? "协议版本不支持"
                        : (status == 2) ? "编解码器不支持"
                        : (status == 3) ? "分辨率超出硬件范围"
                        : (status == 4) ? "缺少握手"
                        : "未知原因";
        fprintf(stderr, "[comm] daemon 拒绝握手: %s (status=%u)\n", why, status);
        return (int)status;
    }

    ctx->negotiated = 1;
    ctx->xfer = COMM_XFER_TCP;

    if (mode == COMM_XFER_SHM && nlen > 0) {
        if (shm_attach(ctx, name) == 0) {
            ctx->xfer = COMM_XFER_SHM;
        } else {
            /* 领取失败：daemon 那边 accept 会超时并同样退回 TCP，
             * 双方都不会卡死，只是少了这一轮的省拷贝收益。 */
            fprintf(stderr, "[comm] 共享内存领取失败，退回 TCP\n");
        }
    }
    return 0;
}

void comm_set_xfer(CommContext *ctx, CommXferMode mode)
{
    if (ctx && !ctx->negotiated) ctx->want_xfer = mode;
}

CommXferMode comm_get_xfer(CommContext *ctx)
{
    return ctx ? ctx->xfer : COMM_XFER_TCP;
}

void comm_release_frame(CommContext *ctx, DecodedFrame *frame)
{
    if (!ctx || !frame) return;
    if (ctx->xfer != COMM_XFER_SHM || frame->shm_slot < 0) return;
    if (frame->shm_slot >= ctx->shm_slots) return;

    /* release 序：确保对帧数据的读取都已完成，daemon 才能覆写这个槽位 */
    volatile uint32_t *st = (volatile uint32_t *)
        (ctx->shm_base + (size_t)frame->shm_slot * sizeof(uint32_t));
    __atomic_store_n(st, 0u, __ATOMIC_RELEASE);

    frame->shm_slot = -1;
    frame->data = NULL;
}

int comm_get_format(CommContext *ctx, CommFormat *out)
{
    if (!ctx || !out) return -1;
    /* buf_width 为 0 说明还没收到过任何格式块（第一帧到达前） */
    if (!ctx->negotiated || ctx->fmt.buf_width == 0) return -1;
    *out = ctx->fmt;
    return 0;
}

int comm_send_nalu(CommContext *ctx, const uint8_t *data, size_t size)
{
    if (!ctx || ctx->fd < 0) return -1;
    if (size == 0) return 0;
    if (size > UINT32_MAX - 4) {
        fprintf(stderr, "[comm] NALU 太大: %zu bytes\n", size);
        return -1;
    }

    /* demuxer 产出的 NALU 不含 start code（AVCC 与 Annex B 两条路径都是如此），
     * 但线路协议要求 Annex B 格式：daemon 依赖 start code 定位 nal_unit_header
     * 来识别 SPS/PPS，MediaCodec 也需要 start code 才能解析码流。
     * 缺少 start code 时解码器会静默拒绝全部输入（入队 N 个、输出 0 帧）。
     * 因此在此统一补上 4 字节 start code，demuxer 保持"裸 NALU"语义不变。 */
    static const uint8_t start_code[4] = { 0x00, 0x00, 0x00, 0x01 };
    /* VP8/VP9 关闭起始码补齐：这类码流没有 Annex B 结构，
     * 强行补 4 字节会破坏帧数据。此时 has_sc 恒为真，走"原样发送"分支。 */
    int has_sc = !ctx->annexb ||
                 (size >= 3 && data[0] == 0x00 && data[1] == 0x00 &&
                  (data[2] == 0x01 || (size >= 4 && data[2] == 0x00 && data[3] == 0x01)));

    uint32_t payload_len = has_sc ? (uint32_t)size : (uint32_t)(size + sizeof(start_code));
    uint32_t len_be = htonl(payload_len);
    if (reliable_write(ctx->fd, &len_be, 4) < 0) {
        perror("[comm] 发送 NALU 长度失败");
        return -1;
    }
    if (!has_sc && reliable_write(ctx->fd, start_code, sizeof(start_code)) < 0) {
        perror("[comm] 发送 start code 失败");
        return -1;
    }
    if (reliable_write(ctx->fd, data, size) < 0) {
        perror("[comm] 发送 NALU 数据失败");
        return -1;
    }

    return 0;
}

int comm_recv_frame(CommContext *ctx, DecodedFrame *frame)
{
    if (!ctx || !frame || ctx->fd < 0) return -1;

    /* 循环是因为帧头可能是格式描述块的哨兵，而非真正的帧。
     * 流中途分辨率变化时 daemon 会插入新的格式块，可能连续出现。 */
    for (;;) {
        /* 读取帧头: 4+4+4 = 12 bytes */
        uint8_t header[12];
        if (reliable_read(ctx->fd, header, 12) < 0) {
            return 1; /* 连接关闭或错误 */
        }

        /* 用 memcpy 而非指针强转：header 是 uint8_t 数组，没有 4 字节对齐保证，
         * 在 aarch64 上对未对齐地址做 uint32_t 解引用是未定义行为。 */
        uint32_t w_be, h_be, sz_be;
        memcpy(&w_be,  header + 0, 4);
        memcpy(&h_be,  header + 4, 4);
        memcpy(&sz_be, header + 8, 4);
        frame->width      = ntohl(w_be);
        frame->height     = ntohl(h_be);
        frame->frame_size = ntohl(sz_be);

        if (frame->frame_size == SHMFRAME_SENTINEL) {
            /* 帧数据在共享内存里：socket 只带槽位号与长度，没有帧数据拷贝 */
            uint32_t si[2];
            if (reliable_read(ctx->fd, si, sizeof(si)) < 0) return 1;
            int slot = (int)ntohl(si[0]);
            uint32_t dlen = ntohl(si[1]);
            if (!ctx->shm_base || slot < 0 || slot >= ctx->shm_slots) {
                fprintf(stderr, "[comm] 非法共享内存槽位 %d\n", slot);
                return -1;
            }
            frame->frame_size = dlen;
            frame->data       = ctx->shm_base + SHM_CTRL_BYTES
                              + (size_t)slot * ctx->shm_slot;
            frame->data_alloc = 0;        /* 非自有内存，绝不能 free */
            frame->shm_slot   = slot;
            return 0;
        }

        if (frame->frame_size != FMTDESC_SENTINEL) break;   /* 是真正的帧 */

        /* 哨兵：随后是 32 字节格式描述块。首次与流内变更走同一路径。 */
        uint32_t fd_[8];
        if (reliable_read(ctx->fd, fd_, FMTDESC_BYTES) < 0) return 1;
        int prev_w = ctx->fmt.buf_width, prev_h = ctx->fmt.buf_height;
        ctx->fmt.buf_width    = (int)ntohl(fd_[0]);
        ctx->fmt.buf_height   = (int)ntohl(fd_[1]);
        ctx->fmt.stride       = (int)ntohl(fd_[2]);
        ctx->fmt.slice_height = (int)ntohl(fd_[3]);
        ctx->fmt.crop_left    = (int)ntohl(fd_[4]);
        ctx->fmt.crop_top     = (int)ntohl(fd_[5]);
        ctx->fmt.crop_right   = (int)ntohl(fd_[6]);
        ctx->fmt.crop_bottom  = (int)ntohl(fd_[7]);
        if (prev_w && (prev_w != ctx->fmt.buf_width || prev_h != ctx->fmt.buf_height)) {
            fprintf(stderr, "[comm] 流内分辨率变更: %dx%d -> %dx%d\n",
                    prev_w, prev_h, ctx->fmt.buf_width, ctx->fmt.buf_height);
        }
    }

    /* 合理性检查 */
    if (frame->width > 16384 || frame->height > 16384 || frame->frame_size > 100 * 1024 * 1024) {
        fprintf(stderr, "[comm] 帧头异常: %ux%u, size=%u\n",
                frame->width, frame->height, frame->frame_size);
        return -1;
    }

    /* 确保缓冲区足够 */
    if (ensure_recv_buf(ctx, frame->frame_size) < 0) {
        fprintf(stderr, "[comm] 内存分配失败\n");
        return -1;
    }

    /* 读取帧数据 */
    if (frame->frame_size > 0) {
        if (reliable_read(ctx->fd, ctx->recv_buf, frame->frame_size) < 0) {
            perror("[comm] 读取帧数据失败");
            return -1;
        }
    }

    frame->data = ctx->recv_buf;
    frame->data_alloc = ctx->recv_buf_size;
    frame->shm_slot = -1;          /* TCP 模式无槽位，release 时据此跳过 */

    return 0;
}

int comm_get_fd(CommContext *ctx)
{
    return ctx ? ctx->fd : -1;
}

void comm_close_write(CommContext *ctx)
{
    if (!ctx || ctx->fd < 0) return;
    shutdown(ctx->fd, SHUT_WR);
}

void comm_close(CommContext *ctx)
{
    if (!ctx) return;
    if (ctx->fd >= 0) close(ctx->fd);
    if (ctx->recv_buf) free(ctx->recv_buf);
    if (ctx->shm_base) munmap(ctx->shm_base, ctx->shm_total);
    free(ctx);
}
