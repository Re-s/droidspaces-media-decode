/*
 * dmd_client.c - decode-daemon 客户端库实现
 *
 * 设计要点见 dmd_client.h 的头注释。这里只记录实现层面的决策：
 *
 * 1) 全部 fd 非阻塞 + poll：宿主是浏览器，任何无上限阻塞都等于挂死整个进程。
 *    connect 也走非阻塞 + POLLOUT + SO_ERROR 取结果。
 * 2) recv/send 一律循环到收满/发满，处理 EINTR 与短读短写；send 带 MSG_NOSIGNAL。
 * 3) SHM 只是意向：daemon 在 memfd 交接**之前**就发出握手响应
 *    （decode-daemon.c:404 发响应，:409 才 shm_handoff），交接超时 3 秒后
 *    daemon 静默退回 TCP 帧格式。因此这里领取失败不算错误，只降级，
 *    继续按 TCP 帧格式解析 —— 这是必须自带的 fallback。
 * 4) 错误传递：所有失败路径都经 set_err() 记录 code + 可读原因（含 strerror），
 *    调用方用 dmd_session_last_error() 取。永不 exit/abort/assert。
 * 5) 无全局可变状态：日志开关也存在会话里（每个会话各自读一次环境变量）。
 *    唯一的全局是 const 字符串字面量。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE     /* MSG_CMSG_CLOEXEC / SOCK_CLOEXEC；Makefile 已传，此处兜底 */
#endif
#include "dmd_client.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ---- 协议常量，必须与 src/decode-daemon.c 保持一致 ---- */
#define DMD_HELLO_MAGIC       0x444D4400u
#define DMD_HELLO_VERSION     2u
#define DMD_FMTDESC_SENTINEL  0xFFFFFFFFu
/* 格式描述块头部第 2 个字的能力标志（旧 daemon 恒为 0）。
 * CAP_FRAME_PTS：帧头带额外的 PTS 字段 = 该帧对应的输入单元序号，
 * 驱动用它精确配对 surface，无需知道解码器的出帧顺序。 */
#define DMD_CAP_FRAME_PTS     0x00000001u
#define DMD_SHMFRAME_SENTINEL 0xFFFFFFFEu
#define DMD_FMTDESC_WORDS     8
#define DMD_SHM_CTRL_BYTES    4096
#define DMD_DEFAULT_PORT      20003

#define DMD_DEF_CONNECT_MS    2000
#define DMD_DEF_IO_MS         5000
/* 领取 memfd 的窗口。daemon 那边 select 等 3 秒（decode-daemon.c:606），
 * 这里取 1.5 秒：要在 daemon 放弃之前就完成或放弃，避免两边认知不一致。 */
#define DMD_SHM_ATTACH_MS     1500

struct dmd_session {
    int fd;                 /* daemon TCP 连接，非阻塞 */
    int log;                /* 日志开关，来自 DMD_VA_LOG */

    int codec;
    int io_timeout_ms;

    int xfer;               /* 实际生效的传输模式 */
    int input_finished;     /* 已 shutdown(SHUT_WR) */
    int eos;                /* 读到对端关闭 */
    int tx_broken;          /* 上行流已损坏（发送中途超时，写出了半个单元） */

    struct dmd_format fmt;
    struct dmd_error  err;

    /* TCP 模式的接收缓冲（复用，按需增长） */
    uint8_t *rbuf;
    size_t   rbuf_size;
    int      rbuf_busy;     /* 1 = 有一帧未 release，正指向 rbuf */

    /* SHM 池 */
    uint8_t *shm_base;
    size_t   shm_slot_bytes;
    size_t   shm_total;
    int      shm_slots;
    int      shm_held;      /* 当前未归还的槽位数，用于诊断 */

    uint64_t units_sent;
    uint64_t frames_recv;
    uint64_t drains_sent;   /* 发出的可逆排空请求数（诊断用） */
    uint32_t caps;          /* daemon 能力位（来自格式描述块，0 = 旧 daemon） */
    uint32_t last_pts;      /* 最近一帧的输入单元序号，0 = 不可用 */
};

/* ------------------------------------------------------------ 日志 */
/*
 * 默认静默，DMD_VA_LOG=1 打开，只写 stderr。
 * driver 里有同名机制，但这里刻意自带一份：库不依赖 driver 的符号，
 * 才能被测试程序单独链接。
 */
static void dmd_c_log(const struct dmd_session *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void dmd_c_log(const struct dmd_session *s, const char *fmt, ...)
{
    if (!s || !s->log)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[dmd-client] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int dmd_log_wanted(void)
{
    const char *env = getenv("DMD_VA_LOG");
    return (env && env[0] == '1') ? 1 : 0;
}

/* ------------------------------------------------------------ 错误记录 */
static void err_set(struct dmd_error *e, int code, int hs_status,
                    const char *what, int use_errno)
{
    if (!e)
        return;
    e->code = code;
    e->handshake_status = hs_status;
    if (use_errno) {
        int sv = errno;
        snprintf(e->msg, sizeof(e->msg), "%s: %s", what, strerror(sv));
        errno = sv;
    } else {
        snprintf(e->msg, sizeof(e->msg), "%s", what);
    }
}

/* 记录到会话，并把同一份内容抄给可选的外部 err 出参 */
static int sess_err(struct dmd_session *s, int code, const char *what,
                    int use_errno)
{
    err_set(&s->err, code, 0, what, use_errno);
    if (s->log)
        dmd_c_log(s, "错误(%d): %s", code, s->err.msg);
    return code;
}

/* ------------------------------------------------------------ fd 工具 */
/* 所有 socket 都用 SOCK_CLOEXEC | SOCK_NONBLOCK 一次创建好，
 * 不留 fcntl 补设的窗口（fork 竞态会泄漏 fd 给宿主进程的子进程）。 */

/* 等 fd 可读/可写。返回 1 就绪，0 超时，-1 出错（errno 有效）。
 * timeout_ms < 0 视为 0（不等待）以避免无限阻塞 —— 库里没有无限等待。 */
static int wait_fd(int fd, short events, int timeout_ms)
{
    if (timeout_ms < 0)
        timeout_ms = 0;
    struct pollfd p;
    p.fd = fd;
    p.events = events;
    for (;;) {
        p.revents = 0;
        int r = poll(&p, 1, timeout_ms);
        if (r < 0) {
            if (errno == EINTR)
                continue;   /* 简化：EINTR 时按原超时重试，最坏情况延长等待 */
            return -1;
        }
        if (r == 0)
            return 0;
        return 1;
    }
}

/*
 * 循环收满 len 字节。
 *   first_timeout_ms：等第一个字节的超时（可以很短，用于"有没有帧"的探测）
 *   rest_timeout_ms： 收到首字节之后每次等待的超时（不能太短，否则留半帧）
 * 返回 DMD_OK / DMD_EOS（一个字节都没读到就对端关闭）/ DMD_ERR_*。
 * 帧中途对端关闭算 DMD_ERR_PROTOCOL：那是被截断的消息，不是干净的流结束。
 */
static int recv_exact(struct dmd_session *s, void *buf, size_t len,
                      int first_timeout_ms, int rest_timeout_ms)
{
    uint8_t *p = buf;
    size_t got = 0;

    while (got < len) {
        int to = (got == 0) ? first_timeout_ms : rest_timeout_ms;
        int r = wait_fd(s->fd, POLLIN, to);
        if (r < 0)
            return sess_err(s, DMD_ERR_IO, "poll 等待可读失败", 1);
        if (r == 0) {
            if (got == 0)
                return DMD_ERR_TIMEOUT;   /* 干净的"暂时没数据"，不污染 last_error */
            return sess_err(s, DMD_ERR_TIMEOUT, "消息中途接收超时", 0);
        }

        ssize_t n = recv(s->fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return sess_err(s, DMD_ERR_IO, "recv 失败", 1);
        }
        if (n == 0) {
            if (got == 0) {
                s->eos = 1;
                return DMD_EOS;
            }
            return sess_err(s, DMD_ERR_PROTOCOL,
                            "对端在消息中途关闭连接（消息被截断）", 0);
        }
        got += (size_t)n;
    }
    return DMD_OK;
}

/* 循环发满 len 字节，MSG_NOSIGNAL 防 SIGPIPE 杀宿主进程 */
static int send_exact(struct dmd_session *s, const void *buf, size_t len,
                      int timeout_ms)
{
    const uint8_t *p = buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(s->fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int r = wait_fd(s->fd, POLLOUT, timeout_ms);
                if (r < 0)
                    return sess_err(s, DMD_ERR_IO, "poll 等待可写失败", 1);
                if (r == 0)
                    return sess_err(s, DMD_ERR_TIMEOUT, "发送超时", 0);
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET)
                return sess_err(s, DMD_ERR_IO, "连接已被 daemon 关闭", 1);
            return sess_err(s, DMD_ERR_IO, "send 失败", 1);
        }
        sent += (size_t)n;
    }
    return DMD_OK;
}

/* ------------------------------------------------------------ 共享内存 */
static volatile uint32_t *shm_state_word(struct dmd_session *s, int idx)
{
    return (volatile uint32_t *)(s->shm_base + (size_t)idx * sizeof(uint32_t));
}

/*
 * 连 daemon 指定的 abstract socket，用 SCM_RIGHTS 领 memfd 并 mmap。
 * 失败返回 -1（调用方降级为 TCP，不是致命错误）。
 *
 * abstract socket 地址构造：sun_path[0] = 0，名字从 sun_path+1 起，
 * addrlen = offsetof(sun_path) + 1 + strlen(name)（不含结尾 NUL）——
 * 与 daemon 的 bind 完全对称（decode-daemon.c:574-580）。
 */
static int shm_attach(struct dmd_session *s, const char *name)
{
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sock < 0) {
        dmd_c_log(s, "abstract socket 创建失败: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un ua;
    memset(&ua, 0, sizeof(ua));
    ua.sun_family = AF_UNIX;
    size_t nl = strlen(name);
    if (nl > sizeof(ua.sun_path) - 2) {
        dmd_c_log(s, "共享内存名字过长: %zu", nl);
        close(sock);
        return -1;
    }
    ua.sun_path[0] = 0;
    memcpy(ua.sun_path + 1, name, nl);
    socklen_t ulen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl);

    if (connect(sock, (struct sockaddr *)&ua, ulen) < 0) {
        if (errno != EINPROGRESS) {
            dmd_c_log(s, "连接 @%s 失败: %s", name, strerror(errno));
            close(sock);
            return -1;
        }
        if (wait_fd(sock, POLLOUT, DMD_SHM_ATTACH_MS) != 1) {
            dmd_c_log(s, "连接 @%s 超时", name);
            close(sock);
            return -1;
        }
        int se = 0;
        socklen_t sl = sizeof(se);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &se, &sl) < 0 || se != 0) {
            dmd_c_log(s, "连接 @%s 失败: %s", name, strerror(se ? se : errno));
            close(sock);
            return -1;
        }
    }

    /* 12 字节 [槽位数][单槽字节数][池总字节数] + SCM_RIGHTS 里的 memfd。
     * daemon 一次 sendmsg 发出，辅助数据不会跨消息拆分，
     * 但仍要等它可读（非阻塞 fd 下 recvmsg 可能先 EAGAIN）。 */
    uint32_t meta[3];
    struct iovec io;
    io.iov_base = meta;
    io.iov_len = sizeof(meta);
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh;
    ssize_t n;

    for (;;) {
        if (wait_fd(sock, POLLIN, DMD_SHM_ATTACH_MS) != 1) {
            dmd_c_log(s, "等待 memfd 超时");
            close(sock);
            return -1;
        }
        memset(cbuf, 0, sizeof(cbuf));
        memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &io;
        mh.msg_iovlen = 1;
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        n = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        break;
    }

    if (n != (ssize_t)sizeof(meta)) {
        dmd_c_log(s, "领取共享内存参数失败: n=%zd (%s)", n,
                  n < 0 ? strerror(errno) : "长度不符");
        close(sock);
        return -1;
    }

    int mfd = -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS &&
            cm->cmsg_len == CMSG_LEN(sizeof(int))) {
            memcpy(&mfd, CMSG_DATA(cm), sizeof(int));
            break;
        }
    }
    close(sock);
    if (mfd < 0) {
        dmd_c_log(s, "响应里没有 memfd");
        return -1;
    }

    int slots      = (int)ntohl(meta[0]);
    size_t slot_sz = (size_t)ntohl(meta[1]);
    size_t total   = (size_t)ntohl(meta[2]);

    /* 自校验：布局必须自洽，否则宁可降级也不要按错误偏移读共享内存 */
    if (slots <= 0 || slots > 64 || slot_sz == 0 ||
        total < DMD_SHM_CTRL_BYTES + slot_sz * (size_t)slots ||
        (size_t)slots * sizeof(uint32_t) > DMD_SHM_CTRL_BYTES) {
        dmd_c_log(s, "共享内存参数不自洽: slots=%d slot=%zu total=%zu",
                  slots, slot_sz, total);
        close(mfd);
        return -1;
    }

    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    close(mfd);                     /* mmap 之后 fd 不再需要，不泄漏给宿主 */
    if (base == MAP_FAILED) {
        dmd_c_log(s, "mmap 共享内存失败: %s", strerror(errno));
        return -1;
    }

    s->shm_base       = base;
    s->shm_slots      = slots;
    s->shm_slot_bytes = slot_sz;
    s->shm_total      = total;
    dmd_c_log(s, "共享内存已挂载: %d 槽 x %zu 字节 (共 %zu)", slots, slot_sz, total);
    return 0;
}

/* ------------------------------------------------------------ 连接与握手 */
static int tcp_connect(struct dmd_session *s, uint16_t port, int timeout_ms,
                       struct dmd_error *err)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        err_set(err, DMD_ERR_CONNECT, 0, "创建 TCP socket 失败", 1);
        return -1;
    }

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);

    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        if (errno != EINPROGRESS) {
            err_set(err, DMD_ERR_CONNECT, 0, "connect 127.0.0.1 失败", 1);
            close(fd);
            return -1;
        }
        int r = wait_fd(fd, POLLOUT, timeout_ms);
        if (r < 0) {
            err_set(err, DMD_ERR_CONNECT, 0, "poll 等待 connect 失败", 1);
            close(fd);
            return -1;
        }
        if (r == 0) {
            err_set(err, DMD_ERR_TIMEOUT, 0, "connect 超时", 0);
            close(fd);
            return -1;
        }
        int se = 0;
        socklen_t sl = sizeof(se);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &se, &sl) < 0) {
            err_set(err, DMD_ERR_CONNECT, 0, "getsockopt(SO_ERROR) 失败", 1);
            close(fd);
            return -1;
        }
        if (se != 0) {
            errno = se;
            err_set(err, DMD_ERR_CONNECT, 0, "connect 被拒绝", 1);
            close(fd);
            return -1;
        }
    }

    /* 帧交付要低延迟；失败不致命，忽略返回值 */
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    s->fd = fd;
    return 0;
}

/*
 * 握手。请求固定 24 字节：
 *   [4B 魔数][4B 版本][4B codec][4B 宽][4B 高][4B 传输模式]
 * 全部大端。daemon 先读 4 字节魔数（decode-daemon.c:335），
 * 再一次读余下 20 字节（:349-350），所以必须一次写满 24 字节。
 *
 * 响应至少 12 字节：[4B status][4B 实际模式][4B 名字长度 n]，之后 n 字节名字。
 * 拒绝路径同样回 12 字节（:344、:376），所以这里对 status!=0 也照读三个字。
 */
static int do_handshake(struct dmd_session *s,
                        const struct dmd_session_config *cfg,
                        struct dmd_error *err)
{
    uint32_t hello[6];
    hello[0] = htonl(DMD_HELLO_MAGIC);
    hello[1] = htonl(DMD_HELLO_VERSION);
    hello[2] = htonl((uint32_t)cfg->codec);
    hello[3] = htonl((uint32_t)cfg->width);
    hello[4] = htonl((uint32_t)cfg->height);
    hello[5] = htonl(cfg->want_shm ? (uint32_t)DMD_XFER_SHM
                                   : (uint32_t)DMD_XFER_TCP);

    if (send_exact(s, hello, sizeof(hello), s->io_timeout_ms) != DMD_OK) {
        if (err)
            *err = s->err;
        return -1;
    }

    uint32_t head[3];
    int r = recv_exact(s, head, sizeof(head), s->io_timeout_ms, s->io_timeout_ms);
    if (r != DMD_OK) {
        if (r == DMD_EOS)
            sess_err(s, DMD_ERR_PROTOCOL,
                     "daemon 未回握手响应就关闭了连接（版本不匹配？）", 0);
        else if (r == DMD_ERR_TIMEOUT)
            sess_err(s, DMD_ERR_TIMEOUT, "等待握手响应超时", 0);
        if (err)
            *err = s->err;
        return -1;
    }

    uint32_t status = ntohl(head[0]);
    uint32_t mode   = ntohl(head[1]);
    uint32_t nlen   = ntohl(head[2]);

    char name[64];
    memset(name, 0, sizeof(name));
    if (nlen > 0) {
        if (nlen >= sizeof(name)) {
            sess_err(s, DMD_ERR_PROTOCOL, "握手响应里的名字长度非法", 0);
            if (err)
                *err = s->err;
            return -1;
        }
        if (recv_exact(s, name, nlen, s->io_timeout_ms, s->io_timeout_ms) != DMD_OK) {
            sess_err(s, DMD_ERR_PROTOCOL, "读取共享内存名字失败", 0);
            if (err)
                *err = s->err;
            return -1;
        }
    }

    if (status != 0) {
        const char *why = (status == 1) ? "daemon 拒绝握手: 协议版本不支持"
                        : (status == 2) ? "daemon 拒绝握手: codec 不支持"
                        : (status == 3) ? "daemon 拒绝握手: 分辨率超出 96x96~8192x4320"
                        : (status == 4) ? "daemon 拒绝握手: 缺少握手"
                        : "daemon 拒绝握手: 未知 status";
        err_set(&s->err, DMD_ERR_REJECTED, (int)status, why, 0);
        if (err)
            *err = s->err;
        return -1;
    }

    /* mode==SHM 只是 daemon 的意向：响应在 memfd 交接之前发出
     * （decode-daemon.c:404 vs :409），交接失败它会静默退回 TCP 帧格式。
     * 所以领取失败绝不能当致命错误 —— 直接按 TCP 继续。 */
    s->xfer = DMD_XFER_TCP;
    if (mode == (uint32_t)DMD_XFER_SHM && nlen > 0) {
        if (shm_attach(s, name) == 0)
            s->xfer = DMD_XFER_SHM;
        else
            dmd_c_log(s, "共享内存领取失败，退回 TCP 帧格式");
    }
    return 0;
}

/* ------------------------------------------------------------ 公开接口 */
void dmd_session_config_init(struct dmd_session_config *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = DMD_DEFAULT_PORT;
    cfg->codec = DMD_CODEC_H264;
    cfg->connect_timeout_ms = DMD_DEF_CONNECT_MS;
    cfg->io_timeout_ms = DMD_DEF_IO_MS;
}

int dmd_format_display_width(const struct dmd_format *fmt)
{
    if (!fmt || !fmt->valid)
        return 0;
    int w = fmt->crop_right - fmt->crop_left + 1;
    return w > 0 ? w : 0;
}

int dmd_format_display_height(const struct dmd_format *fmt)
{
    if (!fmt || !fmt->valid)
        return 0;
    int h = fmt->crop_bottom - fmt->crop_top + 1;
    return h > 0 ? h : 0;
}

struct dmd_session *dmd_session_create(const struct dmd_session_config *cfg,
                                       struct dmd_error *err)
{
    if (err) {
        memset(err, 0, sizeof(*err));
    }
    if (!cfg) {
        err_set(err, DMD_ERR_INVAL, 0, "cfg 为 NULL", 0);
        return NULL;
    }
    if (cfg->codec < DMD_CODEC_H264 || cfg->codec > DMD_CODEC_VP8) {
        err_set(err, DMD_ERR_INVAL, 0, "codec 取值非法（应为 0..3）", 0);
        return NULL;
    }
    if (cfg->width < 96 || cfg->height < 96 ||
        cfg->width > 8192 || cfg->height > 4320) {
        err_set(err, DMD_ERR_INVAL, 0,
                "分辨率超出 daemon 接受范围 96x96~8192x4320", 0);
        return NULL;
    }

    struct dmd_session *s = calloc(1, sizeof(*s));
    if (!s) {
        err_set(err, DMD_ERR_NOMEM, 0, "会话分配失败", 0);
        return NULL;
    }
    s->fd = -1;
    s->log = dmd_log_wanted();
    s->codec = cfg->codec;
    s->io_timeout_ms = cfg->io_timeout_ms > 0 ? cfg->io_timeout_ms : DMD_DEF_IO_MS;
    s->xfer = DMD_XFER_TCP;

    uint16_t port = cfg->port ? cfg->port : DMD_DEFAULT_PORT;
    int cto = cfg->connect_timeout_ms > 0 ? cfg->connect_timeout_ms
                                          : DMD_DEF_CONNECT_MS;

    if (tcp_connect(s, port, cto, err) < 0) {
        if (err)
            s->err = *err;
        dmd_session_destroy(s);
        return NULL;
    }
    if (do_handshake(s, cfg, err) < 0) {
        dmd_session_destroy(s);
        return NULL;
    }

    dmd_c_log(s, "会话建立: port=%u codec=%d %dx%d 传输=%s",
              (unsigned)port, cfg->codec, cfg->width, cfg->height,
              s->xfer == DMD_XFER_SHM ? "SHM" : "TCP");
    return s;
}

void dmd_session_destroy(struct dmd_session *s)
{
    if (!s)
        return;
    if (s->shm_base) {
        /* 归还所有仍被持有的槽位：会话结束时 daemon 可能还在等，
         * 留着占用会让它多等 1 秒才判定异常。 */
        for (int i = 0; i < s->shm_slots; i++)
            __atomic_store_n(shm_state_word(s, i), 0u, __ATOMIC_RELEASE);
        munmap(s->shm_base, s->shm_total);
        s->shm_base = NULL;
    }
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    free(s->rbuf);
    s->rbuf = NULL;
    free(s);
}

int dmd_session_send_unit(struct dmd_session *s, const void *data, size_t len)
{
    if (!s)
        return DMD_ERR_INVAL;
    if (!data || len == 0)
        return sess_err(s, DMD_ERR_INVAL, "数据单元为空", 0);
    if (len > DMD_MAX_UNIT_BYTES)
        return sess_err(s, DMD_ERR_TOOBIG,
                        "数据单元超过 daemon 的 8MB 上限", 0);
    if (s->input_finished)
        return sess_err(s, DMD_ERR_STATE, "已 finish_input，不能再发送", 0);
    if (s->fd < 0)
        return sess_err(s, DMD_ERR_STATE, "会话已无有效连接", 0);
    /* 上一次发送在长度前缀之后失败过：上行字节流已经错位，
     * 继续发只会让 daemon 把数据当长度解析。直接拒绝，逼调用方重建会话。 */
    if (s->tx_broken)
        return sess_err(s, DMD_ERR_STATE,
                        "上行流已损坏（此前发送中断），需重建会话", 0);

    /* H.264/HEVC 必须带 Annex B 起始码：daemon 靠它定位 nal_unit_header
     * 识别 SPS/PPS/VPS（decode-daemon.c:209-240）。缺失时解码器会静默
     * 吞掉全部输入、一帧不产 —— 与其静默失败，不如在这里直接报错。
     * VP8/VP9 相反：补起始码会破坏帧数据，所以这里也绝不代补。 */
    if (s->codec == DMD_CODEC_H264 || s->codec == DMD_CODEC_HEVC) {
        const uint8_t *b = data;
        int sc3 = (len >= 3 && b[0] == 0 && b[1] == 0 && b[2] == 1);
        int sc4 = (len >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1);
        if (!sc3 && !sc4)
            return sess_err(s, DMD_ERR_PROTOCOL,
                            "H.264/HEVC 数据单元缺少 Annex B 起始码", 0);
    }

    uint32_t be = htonl((uint32_t)len);
    int r = send_exact(s, &be, 4, s->io_timeout_ms);
    if (r != DMD_OK) {
        /* 长度前缀本身可能已部分写出，同样算流损坏 */
        s->tx_broken = 1;
        return r;
    }
    r = send_exact(s, data, len, s->io_timeout_ms);
    if (r != DMD_OK) {
        s->tx_broken = 1;
        return r;
    }

    s->units_sent++;
    return DMD_OK;
}

int dmd_session_drain(struct dmd_session *s)
{
    if (!s)
        return DMD_ERR_INVAL;
    if (s->input_finished)
        return sess_err(s, DMD_ERR_STATE, "已 finish_input，无需排空", 0);
    if (s->fd < 0)
        return sess_err(s, DMD_ERR_STATE, "会话已无有效连接", 0);
    if (s->tx_broken)
        return sess_err(s, DMD_ERR_STATE,
                        "上行流已损坏（此前发送中断），需重建会话", 0);

    /* 长度 0 = 排空请求。daemon 会送 EOS 逼解码器吐出在手的帧，
     * 收齐后 flush 复位并重送 CSD —— 与 finish_input 不同，**会话仍可用**。
     *
     * 为什么需要：消费者只保持 3 帧在飞，而解码器有 B 帧时要第 4 个单元
     * 才吐首帧，双方互等。原先只能用 finish_input 打破，但那不可逆，
     * 每次都要重建会话（实测每帧 155 ms、播放慢 4.7 倍）。 */
    uint32_t be = htonl(0);
    int r = send_exact(s, &be, 4, s->io_timeout_ms);
    if (r != DMD_OK) {
        s->tx_broken = 1;
        return r;
    }
    s->drains_sent++;
    return DMD_OK;
}

int dmd_session_finish_input(struct dmd_session *s)
{
    if (!s)
        return DMD_ERR_INVAL;
    if (s->input_finished)
        return DMD_OK;              /* 幂等 */
    if (s->fd < 0)
        return sess_err(s, DMD_ERR_STATE, "会话已无有效连接", 0);
    /* shutdown 让 daemon 的 input_thread 读到 EOF，进而 queue 一个带
     * FLAG_END_OF_STREAM 的空缓冲触发 flush（decode-daemon.c:510-518）。
     * 不做这一步就取不到解码器里排队的尾部帧。 */
    if (shutdown(s->fd, SHUT_WR) < 0 && errno != ENOTCONN)
        return sess_err(s, DMD_ERR_IO, "shutdown(SHUT_WR) 失败", 1);
    s->input_finished = 1;
    dmd_c_log(s, "已关闭写端，等待 flush 出的剩余帧");
    return DMD_OK;
}

static int ensure_rbuf(struct dmd_session *s, size_t need)
{
    if (s->rbuf_size >= need)
        return DMD_OK;
    size_t ns = need + need / 2;
    if (ns < 256 * 1024)
        ns = 256 * 1024;
    uint8_t *nb = realloc(s->rbuf, ns);
    if (!nb)
        return sess_err(s, DMD_ERR_NOMEM, "接收缓冲扩容失败", 0);
    s->rbuf = nb;
    s->rbuf_size = ns;
    return DMD_OK;
}

/* 把当前格式快照写进帧，让每帧自带自洽的几何信息 */
static void frame_apply_format(const struct dmd_session *s, struct dmd_frame *f)
{
    f->unit_seq     = s->last_pts;
    f->stride       = s->fmt.stride;
    f->slice_height = s->fmt.slice_height;
    f->crop_left    = s->fmt.crop_left;
    f->crop_top     = s->fmt.crop_top;
    f->crop_right   = s->fmt.crop_right;
    f->crop_bottom  = s->fmt.crop_bottom;
}

int dmd_session_next_frame(struct dmd_session *s, struct dmd_frame *out,
                           int timeout_ms)
{
    if (!s)
        return DMD_ERR_INVAL;
    if (!out)
        return sess_err(s, DMD_ERR_INVAL, "out 为 NULL", 0);
    if (s->fd < 0)
        return sess_err(s, DMD_ERR_STATE, "会话已无有效连接", 0);
    if (s->eos)
        return DMD_EOS;
    if (s->rbuf_busy)
        return sess_err(s, DMD_ERR_STATE,
                        "上一帧尚未 release，TCP 缓冲仍被占用", 0);

    int first_to = (timeout_ms < 0) ? s->io_timeout_ms : timeout_ms;
    int rest_to  = s->io_timeout_ms;

    /* 循环：帧头可能是格式描述块的哨兵，要消费掉再继续等真正的帧 */
    for (;;) {
        uint8_t hdr[12];
        int r = recv_exact(s, hdr, sizeof(hdr), first_to, rest_to);
        if (r != DMD_OK)
            return r;                /* DMD_EOS / TIMEOUT / 错误原样上报 */

        /* 逐字节 memcpy 再 ntohl：hdr 是 uint8_t[]，aarch64 上直接强转
         * uint32_t* 解引用可能未对齐，是未定义行为 */
        uint32_t w_be, h_be, sz_be;
        memcpy(&w_be, hdr + 0, 4);
        memcpy(&h_be, hdr + 4, 4);
        memcpy(&sz_be, hdr + 8, 4);
        uint32_t w = ntohl(w_be), h = ntohl(h_be), sz = ntohl(sz_be);

        if (sz == DMD_FMTDESC_SENTINEL) {
            /* 第 2 个字是能力标志（旧 daemon 为 0） */
            s->caps = h;
            /* [0][0][0xFFFFFFFF] 之后是 8 个 32 位字的格式描述块 */
            uint32_t fw[DMD_FMTDESC_WORDS];
            r = recv_exact(s, fw, sizeof(fw), rest_to, rest_to);
            if (r == DMD_EOS)
                return sess_err(s, DMD_ERR_PROTOCOL,
                                "格式描述块被截断", 0);
            if (r != DMD_OK)
                return r;
            s->fmt.buf_width    = (int)ntohl(fw[0]);
            s->fmt.buf_height   = (int)ntohl(fw[1]);
            s->fmt.stride       = (int)ntohl(fw[2]);
            s->fmt.slice_height = (int)ntohl(fw[3]);
            s->fmt.crop_left    = (int)ntohl(fw[4]);
            s->fmt.crop_top     = (int)ntohl(fw[5]);
            s->fmt.crop_right   = (int)ntohl(fw[6]);
            s->fmt.crop_bottom  = (int)ntohl(fw[7]);
            s->fmt.valid = 1;
            s->fmt.changes++;
            dmd_c_log(s, "格式块#%d: 缓冲 %dx%d stride=%d slice=%d 显示 %dx%d",
                      s->fmt.changes, s->fmt.buf_width, s->fmt.buf_height,
                      s->fmt.stride, s->fmt.slice_height,
                      dmd_format_display_width(&s->fmt),
                      dmd_format_display_height(&s->fmt));
            /* 已经等到了字节，后续继续用 rest_to，避免探测式短超时误判 */
            first_to = rest_to;
            continue;
        }

        if (sz == DMD_SHMFRAME_SENTINEL) {
            /* SHM 控制消息：帧头之后再跟 [4B 槽位号][4B 数据长度]，共 20 字节 */
            uint32_t si[2];
            r = recv_exact(s, si, sizeof(si), rest_to, rest_to);
            if (r == DMD_EOS)
                return sess_err(s, DMD_ERR_PROTOCOL, "SHM 控制消息被截断", 0);
            if (r != DMD_OK)
                return r;
            int slot = (int)ntohl(si[0]);
            uint32_t dlen = ntohl(si[1]);

            /* 支持 PTS 的 daemon 在槽位/长度之后再跟一个字：输入单元序号。
             * 与 TCP 路径的帧头第 4 字段同义，保证两种传输模式一致。 */
            if (s->caps & DMD_CAP_FRAME_PTS) {
                uint32_t p_be;
                r = recv_exact(s, &p_be, 4, rest_to, rest_to);
                if (r == DMD_EOS)
                    return sess_err(s, DMD_ERR_PROTOCOL,
                                    "SHM 消息 PTS 字段被截断", 0);
                if (r != DMD_OK)
                    return r;
                s->last_pts = ntohl(p_be);
            } else {
                s->last_pts = 0;
            }

            if (!s->shm_base)
                return sess_err(s, DMD_ERR_PROTOCOL,
                                "收到 SHM 帧但共享内存未挂载", 0);
            if (slot < 0 || slot >= s->shm_slots)
                return sess_err(s, DMD_ERR_PROTOCOL, "SHM 槽位号越界", 0);
            if ((size_t)dlen > s->shm_slot_bytes)
                return sess_err(s, DMD_ERR_PROTOCOL, "SHM 帧长度超出槽位", 0);

            memset(out, 0, sizeof(*out));
            out->data = s->shm_base + DMD_SHM_CTRL_BYTES
                      + (size_t)slot * s->shm_slot_bytes;
            out->size = dlen;
            out->width = w;
            out->height = h;
            out->unit_seq = s->last_pts;
            out->shm_slot = slot;
            out->seq = s->frames_recv;
            frame_apply_format(s, out);
            /* SHM 路径也必须累加 frames_recv —— 它不只是统计。
             *
             * ⚠️ 这里原先只读不写，于是 SHM 模式下该计数恒为 0，
             * 造成两个后果：
             *
             * 1. dmd_session_frames_received() 恒 0，而 decode.c 的排空判据
             *    用 `frames_received() > 0` 当护栏（"至少收到过一帧才允许
             *    判定等待徒劳"，那是黑帧根因的修复）。恒 0 会让整个
             *    wait_is_futile 恒假 —— 方向上偏保守（只靠超时、不会误排空），
             *    但护栏语义已经失真，一旦将来有人改动那个表达式就会踩坑。
             * 2. out->seq 恒为 0，帧序号信息丢失。
             *
             * TCP 路径在下面 s->frames_recv++ 处累加，两条路径必须一致。 */
            s->frames_recv++;
                s->shm_held++;
            return DMD_OK;
        }

        /* 普通 TCP 帧。
         * 支持 CAP_FRAME_PTS 的 daemon 在 12 字节帧头后再跟一个字：
         * 该帧对应的输入单元序号。必须在读帧体**之前**取走，
         * 否则字节流错位（实测表现为六条流全部差异，含无重排的 VP8/VP9）。 */
        if (s->caps & DMD_CAP_FRAME_PTS) {
            uint32_t p_be;
            r = recv_exact(s, &p_be, 4, first_to, rest_to);
            if (r == DMD_EOS)
                return sess_err(s, DMD_ERR_PROTOCOL, "帧头 PTS 字段被截断", 0);
            if (r != DMD_OK)
                return r;
            s->last_pts = ntohl(p_be);
        } else {
            s->last_pts = 0;   /* 0 = 无 PTS 信息（旧 daemon） */
        }

        if (sz == 0)
            return sess_err(s, DMD_ERR_PROTOCOL, "帧长度为 0", 0);
        if (sz > DMD_MAX_FRAME_BYTES || w > 16384 || h > 16384)
            return sess_err(s, DMD_ERR_PROTOCOL, "帧头数值不合理", 0);

        r = ensure_rbuf(s, sz);
        if (r != DMD_OK)
            return r;
        r = recv_exact(s, s->rbuf, sz, rest_to, rest_to);
        if (r == DMD_EOS)
            return sess_err(s, DMD_ERR_PROTOCOL, "帧数据被截断", 0);
        if (r != DMD_OK)
            return r;

        memset(out, 0, sizeof(*out));
        out->data = s->rbuf;
        out->size = sz;
        out->width = w;
        out->height = h;
        out->shm_slot = -1;
        out->seq = s->frames_recv;
        frame_apply_format(s, out);
        s->frames_recv++;
        s->rbuf_busy = 1;
        return DMD_OK;
    }
}

int dmd_session_release_frame(struct dmd_session *s, struct dmd_frame *f)
{
    if (!s)
        return DMD_ERR_INVAL;
    if (!f || !f->data)
        return DMD_OK;              /* 无条件调用是安全的 */

    if (f->shm_slot >= 0) {
        if (!s->shm_base || f->shm_slot >= s->shm_slots)
            return sess_err(s, DMD_ERR_INVAL, "release 的槽位号无效", 0);
        /* release 序：确保本进程对帧数据的读取都已完成，
         * daemon 看到状态字变 0 之后才会覆写这个槽位 */
        __atomic_store_n(shm_state_word(s, f->shm_slot), 0u, __ATOMIC_RELEASE);
        if (s->shm_held > 0)
            s->shm_held--;
    } else {
        s->rbuf_busy = 0;
    }

    f->data = NULL;
    f->size = 0;
    f->shm_slot = -1;
    return DMD_OK;
}

const struct dmd_format *dmd_session_format(const struct dmd_session *s)
{
    return s ? &s->fmt : NULL;
}

const char *dmd_session_last_error(const struct dmd_session *s)
{
    if (!s)
        return "会话为 NULL";
    return s->err.msg;
}

int dmd_session_last_error_code(const struct dmd_session *s)
{
    return s ? s->err.code : DMD_ERR_INVAL;
}

int dmd_session_xfer_mode(const struct dmd_session *s)
{
    return s ? s->xfer : DMD_XFER_TCP;
}

int dmd_session_fd(const struct dmd_session *s)
{
    return s ? s->fd : -1;
}

uint64_t dmd_session_units_sent(const struct dmd_session *s)
{
    return s ? s->units_sent : 0;
}

uint64_t dmd_session_frames_received(const struct dmd_session *s)
{
    return s ? s->frames_recv : 0;
}
