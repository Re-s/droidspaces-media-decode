/*
 * decode-daemon — Android MediaCodec 硬件解码代理服务
 *
 * 监听 TCP loopback，接收 H.264 NALU，用 MediaCodec 硬件解码，回传 NV12 帧。
 * 供同一设备上的 Linux 容器使用（容器与 Android 共享 net namespace）。
 *
 * 线路协议：
 *   客户端 → daemon:  [4B NALU 长度，大端][NALU 数据，含 Annex B start code]
 *   daemon → 客户端:  [4B 宽][4B 高][4B 帧大小][NV12 帧数据]，均为大端
 *
 * 并发结构（每个客户端一个会话，会话内两个线程）：
 *
 *   accept 线程 ──┬─→ 会话1 ─┬─ input 线程 : recv NALU → queueInputBuffer
 *                 │          └─ output线程 : dequeueOutputBuffer → send 帧
 *                 └─→ 会话2 ─┬─ ...
 *
 * 为什么必须收发分离：早期版本把 "recv → 喂解码器 → 取帧 → 发送 3.1MB"
 * 全串在一个线程里，发送期间既不收包也不取帧，硬件解码器只能空等。
 * 实测该结构下 daemon 吃满 85.6% 单核（sys 占 73%）成为整条链路的瓶颈。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <linux/memfd.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define MAX_FRAME          (8*1024*1024)
#define INPUT_TIMEOUT_US   5000000
#define OUTPUT_TIMEOUT_US  20000
#define SEND_CHUNK         262144
#define DEFAULT_PORT       20003

/* 硬件支持 16 路并发解码实例，留出余量 */
#define MAX_CLIENTS        8

/* MediaCodec buffer flags（NDK 头文件并非所有版本都导出这些常量名） */
#define FLAG_CODEC_CONFIG  2
#define FLAG_END_OF_STREAM 4

/* ------------------------------------------------------------------ 日志 */
/*
 * 0=quiet（只报错）  1=info（连接/会话统计，默认）  2=debug（逐帧）
 *
 * 逐帧日志必须是 level 2 并默认关闭：早期版本每帧都 fprintf + fflush 同步落盘，
 * 在 sys 时间占比中有可观贡献。默认级别下这些调用直接被跳过，不产生开销。
 */
static int log_level = 1;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void dlog(int lvl, const char *fmt, ...)
{
    if (lvl > log_level) return;
    pthread_mutex_lock(&log_lock);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    /* info 及更重要的即时落盘；debug 级走缓冲，避免逐帧 fflush */
    if (lvl <= 1) fflush(stderr);
    pthread_mutex_unlock(&log_lock);
}

/* -------------------------------------------------------------- 退出控制 */
static volatile sig_atomic_t running = 1;

/* self-pipe：仅靠 running 标志无法唤醒阻塞在 accept() 中的主循环，
 * 收到信号时往管道写一个字节，让 accept 前的 select 立刻返回，
 * 从而使 SIGTERM 能被及时响应（否则外部只能靠 SIGKILL 强杀）。 */
static int wakefd[2] = { -1, -1 };

static void on_signal(int s)
{
    (void)s;
    running = 0;
    if (wakefd[1] >= 0) {
        ssize_t r = write(wakefd[1], "x", 1);
        (void)r;   /* 信号处理函数内无法处理错误，显式忽略 */
    }
}

/* ------------------------------------------------------------ 握手协议 */
/*
 * 可选握手：客户端在发送第一个 NALU 之前，可以先发一个握手帧声明
 * 编解码器与分辨率，避免 daemon 用硬编码的 1920x1080 配置解码器。
 *
 *   [4B 魔数 0x444D4400 "DMD\0"][4B 版本][4B codec][4B 宽][4B 高]  共 20 字节
 *
 * daemon 回应 4 字节状态：0 = 接受，非 0 = 拒绝（随后关闭连接）。
 *
 * 握手成功后，daemon 在第一帧之前额外发送一个格式描述块（32 字节），
 * 让客户端拿到真实的 stride 与显示裁剪区域：
 *
 *   [4B 缓冲宽][4B 缓冲高][4B stride][4B slice_height]
 *   [4B crop_left][4B crop_top][4B crop_right][4B crop_bottom]
 *
 * 格式块统一由哨兵帧头引出，首次与流内变更走同一形式：
 *
 *   [4B 0][4B 0][4B 0xFFFFFFFF]  ← 哨兵帧头，随后紧跟 32 字节格式块
 *
 * 合法帧大小不可能等于 0xFFFFFFFF（远超 8MB 上限），因此不存在歧义。
 * 流中途分辨率变化时（adaptive-playback）daemon 会再次下发，
 * 客户端只需一套解析逻辑：读到哨兵就更新格式，否则按帧处理。
 *
 * 这一块解决了旧协议的缺口：帧头只有 width/height，而高通 Venus 的输出
 * 缓冲按 128/32 对齐（1080p 实际输出 1920x1088），客户端无法区分
 * "填充后的缓冲尺寸"与"真实显示区域"。宽度非 128 倍数时若按缓冲宽度
 * 逐行读取，整幅画面会斜切。
 *
 * 向后兼容：握手是可选的。NALU 帧的首 4 字节是长度，而合法 NALU 长度
 * 不可能等于魔数 0x444D4400（1145389568 远超 MAX_FRAME），因此 daemon
 * 只要窥探前 4 字节就能区分两种客户端。未握手的客户端不会收到格式描述块，
 * 行为与旧版完全一致，无需任何改动。
 */
#define HELLO_MAGIC     0x444D4400u
#define HELLO_VERSION   2      /* v2: 增加共享内存传输协商 */
#define FMTDESC_WORDS   8
/* 帧头 frame_size 取这些值时表示不是帧数据，而是控制消息 */
#define FMTDESC_SENTINEL 0xFFFFFFFFu   /* 随后 32 字节格式描述块 */
#define SHMFRAME_SENTINEL 0xFFFFFFFEu  /* 随后 8 字节：槽位号 + 数据长度 */

/*
 * 传输模式（握手时由客户端请求，daemon 可降级）
 *
 *   XFER_TCP  帧数据直接经 socket 送出。每帧两次内核拷贝。
 *   XFER_SHM  帧数据写入共享内存池，socket 只传"第几个槽位、多长"。
 *             省掉 TCP 的两次拷贝，仍保留一次 MediaCodec→shm 的 CPU 拷贝。
 *
 * SHM 模式下 daemon 在握手后立刻通过 abstract socket + SCM_RIGHTS 把
 * memfd 交给客户端；容器与 Android 共享 net namespace，abstract socket
 * 双向可见（path 形式的 Unix socket 不行，mount namespace 是隔离的）。
 */
typedef enum {
    XFER_TCP = 0,
    XFER_SHM = 1
} XferMode;

/*
 * 共享内存池：SHM_SLOTS 个槽位轮转，客户端处理完归还槽位号。
 *
 * 槽位大小按协商分辨率动态计算（4K NV12 需 12441600 字节，硬编码 8MB 不够），
 * 并留出对齐余量：宽按 128、高按 32 对齐后再乘 1.5。
 * 分辨率在流中途变大时会重建整个池。
 */
#define SHM_SLOTS      4

static size_t shm_slot_bytes(int w, int h)
{
    /* 按 adaptive-playback 声明的上限算，而不是按握手声明的实际分辨率。
     *
     * 配置解码器时用 MAX_WIDTH/MAX_HEIGHT 声明了 max(w,1920) x max(h,1088)，
     * 解码器承诺输出不超过这个尺寸。若只按当前分辨率开槽，流中途分辨率变大
     * （480p->720p 这类）就会超出槽位，SHM 会话被迫终止 ——
     * 同一码流走 TCP 能解满 120 帧、走 SHM 只有 60 帧，是实测到的功能退化。
     *
     * 代价是 720p 流也按 1080p 占池（4 x 3133440 约 12 MB），
     * 用内存换正确性；这比静默丢掉后半段帧划算。 */
    int mw = w > 1920 ? w : 1920;
    int mh = h > 1088 ? h : 1088;
    size_t aw = ((size_t)mw + 127) & ~(size_t)127;
    size_t ah = ((size_t)mh + 31)  & ~(size_t)31;
    size_t sz = aw * ah * 3 / 2;
    return sz < 64 * 1024 ? 64 * 1024 : sz;
}

typedef enum {
    CODEC_H264 = 0,
    CODEC_HEVC = 1,
    CODEC_VP9  = 2,
    CODEC_VP8  = 3,
    CODEC_MAX
} CodecId;

/* 设备硬件支持的解码器（见 doc/verified-platform-facts.md 的能力清单） */
static const char *codec_mime(int id)
{
    switch (id) {
    case CODEC_H264: return "video/avc";
    case CODEC_HEVC: return "video/hevc";
    case CODEC_VP9:  return "video/x-vnd.on2.vp9";
    case CODEC_VP8:  return "video/x-vnd.on2.vp8";
    default:         return NULL;
    }
}

/* ------------------------------------------------------------ 工具函数 */
/*
 * 取 H.264 NALU 类型。起始码可能是 3 字节(00 00 01)或 4 字节(00 00 00 01)，
 * 必须先跳过起始码再读 nal_unit_header，否则 4 字节起始码下 buf[3]==0x01，
 * 会把 SPS(7)/PPS(8) 一律误判为 type 1，导致 CSD 永远无法被识别。
 * 返回 -1 表示无法判定。
 */
static int nalu_type(const uint8_t *b, size_t len)
{
    size_t off = 0;
    if (len >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) off = 4;
    else if (len >= 3 && b[0] == 0 && b[1] == 0 && b[2] == 1) off = 3;
    if (off == 0 || off >= len) return -1;
    return b[off] & 0x1f;
}

/*
 * 该 NALU 是否为需要累积成 CSD 的参数集。
 *
 * H.264: nal_unit_type 取 header 低 5 位，SPS=7 PPS=8
 * HEVC:  nal_unit_type 取 header 高字节的 bit1-6，VPS=32 SPS=33 PPS=34
 * VP8/VP9: 无 Annex B 参数集概念，整帧直接送入
 */
static int is_param_set(int codec_id, const uint8_t *b, size_t len)
{
    if (codec_id == CODEC_H264) {
        int t = nalu_type(b, len);
        return (t == 7 || t == 8);
    }
    if (codec_id == CODEC_HEVC) {
        size_t off = 0;
        if (len >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) off = 4;
        else if (len >= 3 && b[0] == 0 && b[1] == 0 && b[2] == 1) off = 3;
        if (off == 0 || off >= len) return 0;
        int t = (b[off] >> 1) & 0x3f;
        return (t == 32 || t == 33 || t == 34);
    }
    return 0;
}

static int recv_all(int fd, void *b, size_t l)
{
    char *p = b;
    while (l > 0) {
        ssize_t n = read(fd, p, l);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; l -= n;
    }
    return 0;
}

static int send_all(int fd, const void *b, size_t l)
{
    const char *p = b;
    while (l > 0) {
        ssize_t n = write(fd, p, l);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; l -= n;
    }
    return 0;
}

/* ---------------------------------------------------------------- 会话 */
typedef struct {
    int              fd;
    AMediaCodec     *codec;
    volatile sig_atomic_t stop;        /* 任一方向出错即置位，两个线程都会退出 */
    volatile sig_atomic_t input_done;  /* input 线程已结束（已尝试送出 EOS） */
    int              w, h;             /* 仅 output 线程写 */
    long             nalu_in;
    long             frames_out;
    int              id;

    /* 握手结果 */
    int              codec_id;         /* CodecId，默认 CODEC_H264 */
    const char      *mime;
    int              negotiated;       /* 1 = 客户端完成了握手 */

    /* 输出格式详情（仅 output 线程写） */
    int              stride;
    int              slice_height;
    int              crop_l, crop_t, crop_r, crop_b;
    int              fmt_sent;         /* 当前格式的描述块已发送 */
    int              fmt_changes;      /* 格式变更次数（含首次） */

    /* 共享内存传输 */
    XferMode         xfer;
    int              shm_fd;           /* memfd，-1 表示未建立 */
    int              shm_listen;       /* abstract socket 监听 fd，-1 表示无 */
    char             shm_name[64];     /* abstract socket 名字，由 daemon 定 */
    uint8_t         *shm_base;         /* mmap 后的基址 */
    size_t           shm_slot;         /* 单槽字节数 */
    size_t           shm_total;        /* 池总字节数（含控制区） */
    int              shm_next;         /* 下一个要写的槽位 */
} Session;

/*
 * 共享内存池布局：
 *
 *   [控制区 4096 字节][槽位 0][槽位 1] ... [槽位 SHM_SLOTS-1]
 *
 * 控制区里每个槽位有一个 32 位状态字，daemon 写入后置 1（占用），
 * 客户端处理完置 0（空闲）。用 __atomic 访问，无需锁。
 * 这样归还路径不占用 socket，也不会与 NALU 上行流交织。
 */
#define SHM_CTRL_BYTES 4096

static inline volatile uint32_t *shm_slot_state(Session *s, int idx)
{
    return (volatile uint32_t *)(s->shm_base + (size_t)idx * sizeof(uint32_t));
}

static inline uint8_t *shm_slot_data(Session *s, int idx)
{
    return s->shm_base + SHM_CTRL_BYTES + (size_t)idx * s->shm_slot;
}

/* 共享内存池的建立与释放定义在后面，握手阶段先用到，此处前置声明 */
static int  shm_prepare(Session *s, int w, int h);
static int  shm_handoff(Session *s);
static void shm_teardown(Session *s);

/*
 * 握手。读前 4 字节：
 *   等于魔数    → 继续读余下 20 字节，按声明配置会话，回 8 字节响应
 *   不等于魔数  → 协议不符，拒绝连接
 * 返回 0 表示可以继续，-1 表示应当断开。
 */
static int do_handshake(Session *s)
{
    uint32_t first;
    if (recv_all(s->fd, &first, 4) < 0) return -1;   /* 连上就断，无需报错 */
    first = ntohl(first);

    if (first != HELLO_MAGIC) {
        /* 握手是必需的：客户端与 daemon 配套发布，不保留无握手的弱路径。
         * 没有握手就拿不到 mime 与初始尺寸，也无从下发 stride/crop。 */
        dlog(1, "[%d] 缺少握手（首字 0x%08x），拒绝连接", s->id, first);
        /* 与其他拒绝路径保持同一形状（12 字节）：客户端统一按
         * [status][mode][name_len] 读，若这里只回 4 字节它会阻塞在剩余 8 字节上。 */
        uint32_t reply[3] = { htonl(4), htonl(XFER_TCP), htonl(0) };
        send_all(s->fd, reply, sizeof(reply));
        return -1;
    }

    uint32_t rest[5];
    if (recv_all(s->fd, rest, sizeof(rest)) < 0) {
        dlog(1, "[%d] 握手帧不完整", s->id);
        return -1;
    }
    uint32_t ver   = ntohl(rest[0]);
    uint32_t cid   = ntohl(rest[1]);
    uint32_t w     = ntohl(rest[2]);
    uint32_t h     = ntohl(rest[3]);
    uint32_t xfer  = ntohl(rest[4]);   /* 请求的传输模式，daemon 可降级 */

    uint32_t status = 0;
    const char *mime = (cid < CODEC_MAX) ? codec_mime((int)cid) : NULL;

    if (ver != HELLO_VERSION) {
        dlog(1, "[%d] 握手版本不支持: %u", s->id, ver);
        status = 1;
    } else if (!mime) {
        dlog(1, "[%d] 未知 codec id: %u", s->id, cid);
        status = 2;
    } else if (w < 96 || h < 96 || w > 8192 || h > 4320) {
        /* 硬件限制 96x96 ~ 8192x4320，见 doc/verified-platform-facts.md */
        dlog(1, "[%d] 分辨率超出硬件支持范围: %ux%u", s->id, w, h);
        status = 3;
    }

    if (status != 0) {
        uint32_t reply[3] = { htonl(status), htonl(XFER_TCP), htonl(0) };
        send_all(s->fd, reply, sizeof(reply));
        return -1;
    }

    s->codec_id   = (int)cid;
    s->mime       = mime;
    s->w          = (int)w;
    s->h          = (int)h;
    s->negotiated = 1;

    /* 共享内存池必须在回响应之前建好：响应里要带上 abstract socket 的名字。
     * 名字由 daemon 定 —— 客户端不知道自己是第几个连接，让它猜必然出错。 */
    XferMode want = XFER_TCP;
    if (xfer == XFER_SHM) {
        if (shm_prepare(s, (int)w, (int)h) == 0) {
            want = XFER_SHM;
        } else {
            dlog(1, "[%d] 共享内存准备失败，降级为 TCP", s->id);
        }
    }

    /*
     * 响应：[4B status][4B 实际模式][4B 名字长度][名字...]
     * TCP 模式下名字长度为 0，不跟任何字节。
     */
    size_t nlen = (want == XFER_SHM) ? strlen(s->shm_name) : 0;
    uint32_t reply[3] = { htonl(0), htonl((uint32_t)want), htonl((uint32_t)nlen) };
    if (send_all(s->fd, reply, sizeof(reply)) < 0) goto reply_fail;
    if (nlen > 0 && send_all(s->fd, s->shm_name, nlen) < 0) goto reply_fail;

    if (want == XFER_SHM) {
        /* 客户端已拿到名字，现在等它来领 memfd */
        if (shm_handoff(s) == 0) {
            s->xfer = XFER_SHM;
        } else {
            /* 交接失败只能降级。客户端那边同样会超时退回 TCP。 */
            shm_teardown(s);
            s->xfer = XFER_TCP;
            dlog(1, "[%d] 共享内存交接失败，降级为 TCP", s->id);
        }
    } else {
        s->xfer = XFER_TCP;
    }

    dlog(1, "[%d] 握手成功: %s %ux%u 传输=%s",
         s->id, mime, w, h, s->xfer == XFER_SHM ? "SHM" : "TCP");
    return 0;

reply_fail:
    shm_teardown(s);
    return -1;
}

/*
 * input 线程：从 socket 读 NALU 并喂给解码器。
 * SPS/PPS 累积为 codec-specific data，用 FLAG_CODEC_CONFIG 送入，不产出帧。
 */
static void *input_thread(void *arg)
{
    Session *s = arg;
    uint8_t *buf = malloc(MAX_FRAME);
    uint8_t *csd = malloc(MAX_FRAME);
    size_t csd_len = 0;

    if (!buf || !csd) {
        dlog(0, "[%d] 输入缓冲分配失败", s->id);
        s->stop = 1;
        goto done;
    }

    while (running && !s->stop) {
        uint32_t sz;
        if (recv_all(s->fd, &sz, 4) < 0) break;      /* 客户端关闭写端，正常结束 */
        sz = ntohl(sz);
        if (sz == 0 || sz > MAX_FRAME) {
            dlog(1, "[%d] NALU 长度非法: %u", s->id, sz);
            break;
        }
        if (recv_all(s->fd, buf, sz) < 0) {
            dlog(1, "[%d] 读取 NALU 数据中断", s->id);
            break;
        }
        s->nalu_in++;

        if (is_param_set(s->codec_id, buf, sz)) {      /* 参数集，累积为 CSD */
            if (csd_len + sz <= MAX_FRAME) {
                memcpy(csd + csd_len, buf, sz);
                csd_len += sz;
            } else {
                dlog(1, "[%d] CSD 累积超出上限，丢弃", s->id);
            }
            continue;
        }

        if (csd_len > 0) {
            ssize_t ci = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_TIMEOUT_US);
            if (ci >= 0) {
                size_t cap;
                uint8_t *ib = AMediaCodec_getInputBuffer(s->codec, ci, &cap);
                if (ib && cap >= csd_len) {
                    memcpy(ib, csd, csd_len);
                    AMediaCodec_queueInputBuffer(s->codec, ci, 0, csd_len, 0,
                                                 FLAG_CODEC_CONFIG);
                    dlog(2, "[%d] CSD 已送入 (%zu 字节)", s->id, csd_len);
                } else {
                    dlog(1, "[%d] CSD 超出输入缓冲容量", s->id);
                    AMediaCodec_queueInputBuffer(s->codec, ci, 0, 0, 0, 0);
                }
            } else {
                dlog(1, "[%d] 取输入缓冲失败(CSD): %zd", s->id, ci);
            }
            csd_len = 0;
        }

        ssize_t bi = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_TIMEOUT_US);
        if (bi < 0) {
            dlog(1, "[%d] 取输入缓冲失败: %zd", s->id, bi);
            continue;
        }
        size_t cap;
        uint8_t *ib = AMediaCodec_getInputBuffer(s->codec, bi, &cap);
        if (!ib || cap < sz) {
            dlog(1, "[%d] NALU 超出输入缓冲容量 (%u > %zu)", s->id, sz, cap);
            AMediaCodec_queueInputBuffer(s->codec, bi, 0, 0, 0, 0);
            continue;
        }
        memcpy(ib, buf, sz);
        AMediaCodec_queueInputBuffer(s->codec, bi, 0, sz, 0, 0);
    }

    /* 送出 end-of-stream，让 output 线程能确定性地收完剩余帧。
     * 缓冲模式下正确做法是 queue 一个带 EOS 标志的空缓冲，
     * 而不是 AMediaCodec_signalEndOfInputStream（那是给 Surface 输入用的）。 */
    if (!s->stop) {
        ssize_t bi = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_TIMEOUT_US);
        if (bi >= 0) {
            AMediaCodec_queueInputBuffer(s->codec, bi, 0, 0, 0, FLAG_END_OF_STREAM);
            dlog(2, "[%d] 已送出 EOS", s->id);
        } else {
            dlog(1, "[%d] 无法送出 EOS: %zd", s->id, bi);
        }
    }

done:
    free(buf);
    free(csd);
    s->input_done = 1;
    return NULL;
}

/*
 * 建立共享内存池，并通过 abstract socket 把 memfd 交给客户端。
 *
 * 名字由会话 id 决定（dmd-shm-<id>），客户端在握手响应后据此监听。
 * 之所以用 abstract socket 而非路径形式：容器与 Android 共享 net namespace
 * （abstract socket 属于 net ns，双向可见），但 mount namespace 是隔离的，
 * 基于路径的 Unix socket 在对侧根本不存在。
 *
 * 返回 0 成功；失败返回 -1，调用方应降级为 TCP 模式。
 */
static int shm_prepare(Session *s, int w, int h)
{
    s->shm_slot  = shm_slot_bytes(w, h);
    s->shm_total = SHM_CTRL_BYTES + s->shm_slot * SHM_SLOTS;

    s->shm_fd = (int)syscall(SYS_memfd_create, "dmd-frames", MFD_CLOEXEC);
    if (s->shm_fd < 0) {
        dlog(1, "[%d] memfd_create 失败: %s", s->id, strerror(errno));
        return -1;
    }
    if (ftruncate(s->shm_fd, (off_t)s->shm_total) < 0) {
        dlog(1, "[%d] ftruncate 失败: %s", s->id, strerror(errno));
        goto fail;
    }
    s->shm_base = mmap(NULL, s->shm_total, PROT_READ | PROT_WRITE,
                       MAP_SHARED, s->shm_fd, 0);
    if (s->shm_base == MAP_FAILED) {
        dlog(1, "[%d] mmap 失败: %s", s->id, strerror(errno));
        s->shm_base = NULL;
        goto fail;
    }
    memset(s->shm_base, 0, SHM_CTRL_BYTES);   /* 所有槽位标记为空闲 */

    /* 由 daemon 定名并监听：客户端无从得知自己是第几个连接，
     * 让它猜名字必然串台或连不上。
     *
     * pid + 会话 id 已能保证唯一，但那是可预测的 —— 同一 net namespace 里
     * 任何进程都能抢先 bind 该名字，使 daemon 的 bind 失败并降级为 TCP
     * （不会泄漏 fd，但构成一种廉价的降级攻击）。加 32 位随机后缀消除可预测性。 */
    snprintf(s->shm_name, sizeof(s->shm_name), "dmd-shm-%d-%d-%08x",
             (int)getpid(), s->id, (unsigned)arc4random());

    s->shm_listen = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->shm_listen < 0) {
        dlog(1, "[%d] abstract socket 创建失败: %s", s->id, strerror(errno));
        goto fail;
    }
    struct sockaddr_un ua;
    memset(&ua, 0, sizeof(ua));
    ua.sun_family = AF_UNIX;
    ua.sun_path[0] = 0;                        /* abstract namespace */
    strncpy(ua.sun_path + 1, s->shm_name, sizeof(ua.sun_path) - 2);
    socklen_t ulen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                 + 1 + strlen(s->shm_name));
    if (bind(s->shm_listen, (struct sockaddr *)&ua, ulen) < 0 ||
        listen(s->shm_listen, 1) < 0) {
        dlog(1, "[%d] abstract socket bind/listen 失败: %s", s->id, strerror(errno));
        close(s->shm_listen);
        s->shm_listen = -1;
        goto fail;
    }
    return 0;

fail:
    if (s->shm_base) { munmap(s->shm_base, s->shm_total); s->shm_base = NULL; }
    if (s->shm_fd >= 0) { close(s->shm_fd); s->shm_fd = -1; }
    return -1;
}

/*
 * 等客户端连上来，把 memfd 交给它。必须在握手响应之后调用 ——
 * 客户端要先从响应里读到名字才知道往哪连。
 */
static int shm_handoff(Session *s)
{
    /* 等待有上限：客户端可能崩了或不理解 SHM 模式，不能无限阻塞会话 */
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(s->shm_listen, &rs);
    struct timeval tv = { 3, 0 };
    int r = select(s->shm_listen + 1, &rs, NULL, NULL, &tv);
    if (r <= 0) {
        dlog(1, "[%d] 等待客户端领取共享内存超时", s->id);
        return -1;
    }
    int cs = accept(s->shm_listen, NULL, NULL);
    if (cs < 0) {
        dlog(1, "[%d] accept 失败: %s", s->id, strerror(errno));
        return -1;
    }

    /* SCM_RIGHTS 传 fd，附带槽位参数供客户端 mmap */
    uint32_t meta[3] = {
        htonl((uint32_t)SHM_SLOTS),
        htonl((uint32_t)s->shm_slot),
        htonl((uint32_t)s->shm_total)
    };
    struct iovec io = { meta, sizeof(meta) };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &io; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof(cbuf);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &s->shm_fd, sizeof(int));

    if (sendmsg(cs, &mh, 0) < 0) {
        dlog(1, "[%d] 传递 memfd 失败: %s", s->id, strerror(errno));
        close(cs);
        return -1;
    }
    close(cs);

    /* 交接完成，监听端口不再需要 */
    close(s->shm_listen);
    s->shm_listen = -1;

    dlog(1, "[%d] 共享内存已交接: %d 槽 x %zu 字节 (共 %zu)",
         s->id, SHM_SLOTS, s->shm_slot, s->shm_total);
    return 0;
}

static void shm_teardown(Session *s)
{
    if (s->shm_listen >= 0) { close(s->shm_listen); s->shm_listen = -1; }
    if (s->shm_base) { munmap(s->shm_base, s->shm_total); s->shm_base = NULL; }
    if (s->shm_fd >= 0) { close(s->shm_fd); s->shm_fd = -1; }
}

/*
 * 经共享内存送出一帧：拷进空闲槽位，socket 只写 20 字节控制消息。
 *
 *   [4B 宽][4B 高][4B 0xFFFFFFFE][4B 槽位号][4B 数据长度]
 *
 * 相比 TCP 模式省掉了内核态的两次拷贝（send 时进 socket 缓冲、recv 时出）。
 * 仍保留一次 MediaCodec 输出缓冲 → 共享内存的 CPU 拷贝，
 * 要去掉它需要 dmabuf 零拷贝（见 doc/performance-and-roadmap.md）。
 *
 * 返回 0 成功，-1 失败。
 */
static int send_frame_shm(Session *s, const uint8_t *data, size_t len)
{
    if (len > s->shm_slot) {
        dlog(1, "[%d] 帧 %zu 字节超出槽位 %zu，需重建池", s->id, len, s->shm_slot);
        return -1;
    }

    /* 找一个空闲槽位。轮转起点是 shm_next，保证公平使用所有槽位。
     * 客户端处理完会把状态字置 0；这里最多等约 1 秒，超时说明客户端卡死。 */
    int slot = -1;
    for (int spin = 0; spin < 1000 && slot < 0; spin++) {
        for (int k = 0; k < SHM_SLOTS; k++) {
            int idx = (s->shm_next + k) % SHM_SLOTS;
            uint32_t st = __atomic_load_n(shm_slot_state(s, idx), __ATOMIC_ACQUIRE);
            if (st == 0) { slot = idx; break; }
        }
        if (slot < 0) {
            if (!running || s->stop) return -1;
            usleep(1000);
        }
    }
    if (slot < 0) {
        dlog(1, "[%d] 等不到空闲槽位（客户端未及时归还）", s->id);
        return -1;
    }

    memcpy(shm_slot_data(s, slot), data, len);
    /* release 序：确保数据写入对读到状态字的客户端可见 */
    __atomic_store_n(shm_slot_state(s, slot), 1u, __ATOMIC_RELEASE);
    s->shm_next = (slot + 1) % SHM_SLOTS;

    uint32_t msg[5] = {
        htonl((uint32_t)s->w), htonl((uint32_t)s->h),
        htonl(SHMFRAME_SENTINEL),
        htonl((uint32_t)slot), htonl((uint32_t)len)
    };
    if (send_all(s->fd, msg, sizeof(msg)) < 0) {
        /* 通知失败则立即释放槽位，否则池会漏 */
        __atomic_store_n(shm_slot_state(s, slot), 0u, __ATOMIC_RELEASE);
        return -1;
    }
    return 0;
}

/*
 * 下发格式描述块（哨兵帧头 + 32 字节内容）。
 * 只对握手过的客户端发送；未握手的旧客户端不理解这个块，必须跳过。
 * 返回 0 成功，-1 失败（调用方应结束会话）。
 */
static int send_format_desc(Session *s)
{
    /* 缺失值兜底：FORMAT_CHANGED 尚未到达时用当前已知尺寸 */
    if (s->stride <= 0)       s->stride = s->w;
    if (s->slice_height <= 0) s->slice_height = s->h;
    if (s->crop_r <= 0)       s->crop_r = s->w - 1;
    if (s->crop_b <= 0)       s->crop_b = s->h - 1;

    uint32_t hdr[3] = { htonl(0), htonl(0), htonl(FMTDESC_SENTINEL) };
    uint32_t body[FMTDESC_WORDS] = {
        htonl((uint32_t)s->w),      htonl((uint32_t)s->h),
        htonl((uint32_t)s->stride), htonl((uint32_t)s->slice_height),
        htonl((uint32_t)s->crop_l), htonl((uint32_t)s->crop_t),
        htonl((uint32_t)s->crop_r), htonl((uint32_t)s->crop_b)
    };
    if (send_all(s->fd, hdr, sizeof(hdr)) < 0 ||
        send_all(s->fd, body, sizeof(body)) < 0) {
        dlog(1, "[%d] 发送格式描述失败", s->id);
        s->stop = 1;
        return -1;
    }
    s->fmt_sent = 1;
    return 0;
}

/*
 * output 线程：取解码帧并写回 socket。
 * 与 input 线程完全解耦——发送大帧期间不影响 NALU 的接收与入队。
 */
static void *output_thread(void *arg)
{
    Session *s = arg;
    int idle_after_eof = 0;

    while (running && !s->stop) {
        AMediaCodecBufferInfo info;
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(s->codec, &info, OUTPUT_TIMEOUT_US);

        if (oi >= 0) {
            idle_after_eof = 0;
            int eos = (info.flags & FLAG_END_OF_STREAM) != 0;

            /* 握手过的客户端必须先收到格式描述块再收帧。
             * 正常流程里 FORMAT_CHANGED 总在首帧之前到达，但不能依赖这一点：
             * 若解码器直接给出帧，这里用当前已知值补发，避免客户端错位解析。 */
            if (!s->fmt_sent && info.size > 0) {
                if (send_format_desc(s) == 0)
                    dlog(2, "[%d] 已补发格式描述（FORMAT_CHANGED 未先到）", s->id);
            }

            if (info.size > 0 && !s->stop) {
                size_t osz;
                uint8_t *ob = AMediaCodec_getOutputBuffer(s->codec, oi, &osz);
                if (ob) {
                    int fail;
                    if (s->xfer == XFER_SHM) {
                        fail = (send_frame_shm(s, ob + info.offset,
                                               (size_t)info.size) < 0);
                    } else {
                        uint32_t hdr[3] = {
                            htonl((uint32_t)s->w),
                            htonl((uint32_t)s->h),
                            htonl((uint32_t)info.size)
                        };
                        fail = (send_all(s->fd, hdr, sizeof(hdr)) < 0);
                        size_t off = (size_t)info.offset;
                        size_t rem = (size_t)info.size;
                        while (!fail && rem > 0) {
                            size_t ch = rem > SEND_CHUNK ? SEND_CHUNK : rem;
                            if (send_all(s->fd, ob + off, ch) < 0) { fail = 1; break; }
                            off += ch; rem -= ch;
                        }
                    }
                    if (fail) {
                        dlog(1, "[%d] 发送帧失败，结束会话", s->id);
                        s->stop = 1;
                    } else {
                        s->frames_out++;
                        dlog(2, "[%d] 帧 %dx%d %d 字节", s->id, s->w, s->h, info.size);
                    }
                }
            }
            AMediaCodec_releaseOutputBuffer(s->codec, oi, 0);
            if (eos) {
                dlog(2, "[%d] 收到 EOS，输出结束", s->id);
                break;
            }
        } else if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *of = AMediaCodec_getOutputFormat(s->codec);
            if (of) {
                int w = s->w, h = s->h;
                AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_WIDTH, &w);
                AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_HEIGHT, &h);
                s->w = w; s->h = h;

                /* stride / slice_height：解码器输出缓冲的实际行距与平面高度。
                 * 高通 Venus 会把宽度对齐到 128、高度对齐到 32，
                 * 不读这两个值就无法正确定位第二平面（UV）的起始偏移。 */
                int stride = 0, slice = 0;
                if (!AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_STRIDE, &stride))
                    stride = w;
                if (!AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice))
                    slice = h;
                s->stride = stride;
                s->slice_height = slice;

                /* 显示裁剪区域：真实可见范围。1080p 解码为 1920x1088 时
                 * crop_bottom 应为 1079，多出的 8 行是对齐填充。 */
                int cl = 0, ct = 0, cr = w - 1, cb = h - 1;
                AMediaFormat_getRect(of, AMEDIAFORMAT_KEY_DISPLAY_CROP,
                                     &cl, &ct, &cr, &cb);
                s->crop_l = cl; s->crop_t = ct;
                s->crop_r = cr; s->crop_b = cb;
                AMediaFormat_delete(of);

                s->fmt_changes++;
                dlog(1, "[%d] 输出格式 %dx%d stride=%d slice=%d crop=(%d,%d)-(%d,%d)%s",
                     s->id, w, h, stride, slice, cl, ct, cr, cb,
                     s->fmt_changes > 1 ? "（流内变更）" : "");

                /* 标记需要（重新）下发：流中途分辨率变化时客户端必须拿到新的
                 * stride/crop，否则会按旧尺寸解析后续帧，数据整体错位。 */
                s->fmt_sent = 0;
                send_format_desc(s);
            }
        } else if (oi == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            /* 无需处理：NDK 下每次都用 getOutputBuffer 重新取指针 */
        } else if (oi == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            /* 输入已结束却还等不到 EOS，说明 EOS 没能送进去，避免无限空转 */
            if (s->input_done && ++idle_after_eof > 100) {
                dlog(1, "[%d] 输入结束后长时间无输出，放弃等待", s->id);
                break;
            }
        } else {
            dlog(1, "[%d] dequeueOutputBuffer 异常: %zd", s->id, oi);
            break;
        }
    }
    return NULL;
}

/* ------------------------------------------------------- 并发客户端计数 */
static int client_count = 0;
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;

static void client_release(void)
{
    pthread_mutex_lock(&count_lock);
    client_count--;
    pthread_mutex_unlock(&count_lock);
}

/*
 * 会话线程：为一个客户端建立解码器，跑 input/output 两个线程直到结束。
 */
static void *session_thread(void *arg)
{
    Session *s = arg;

    /* 握手必须在配置解码器之前完成：它决定 mime 与初始分辨率 */
    if (do_handshake(s) < 0) goto out_fd;

    AMediaFormat *fmt = AMediaFormat_new();
    if (!fmt) { dlog(0, "[%d] AMediaFormat_new 失败", s->id); goto out_fd; }
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, s->mime);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, s->w);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, s->h);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, MAX_FRAME);

    /* 启用 adaptive-playback：声明后续可能出现的最大尺寸，解码器据此
     * 预分配输出池，流中途分辨率变化时无需重建解码器。
     * 取握手声明尺寸与 1080p 的较大者——声明过大会白占内存，
     * 过小则超出后仍要重配。 */
    int max_w = s->w > 1920 ? s->w : 1920;
    int max_h = s->h > 1088 ? s->h : 1088;
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_WIDTH,  max_w);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_HEIGHT, max_h);

    s->codec = AMediaCodec_createDecoderByType(s->mime);
    if (!s->codec) { dlog(0, "[%d] 无可用解码器: %s", s->id, s->mime); goto out_fmt; }

    media_status_t st = AMediaCodec_configure(s->codec, fmt, NULL, NULL, 0);
    if (st != AMEDIA_OK) { dlog(0, "[%d] configure 失败: %d", s->id, st); goto out_codec; }

    st = AMediaCodec_start(s->codec);
    if (st != AMEDIA_OK) { dlog(0, "[%d] start 失败: %d", s->id, st); goto out_codec; }

    pthread_t tin, tout;
    int have_in = (pthread_create(&tin, NULL, input_thread, s) == 0);
    if (!have_in) { dlog(0, "[%d] 无法创建 input 线程", s->id); s->stop = 1; }
    int have_out = (pthread_create(&tout, NULL, output_thread, s) == 0);
    if (!have_out) { dlog(0, "[%d] 无法创建 output 线程", s->id); s->stop = 1; }

    if (have_in)  pthread_join(tin, NULL);
    if (have_out) pthread_join(tout, NULL);

    dlog(1, "[%d] 会话结束: 收到 %ld NALU, 回传 %ld 帧",
         s->id, s->nalu_in, s->frames_out);

    AMediaCodec_stop(s->codec);
out_codec:
    AMediaCodec_delete(s->codec);
out_fmt:
    AMediaFormat_delete(fmt);
out_fd:
    shm_teardown(s);
    close(s->fd);
    free(s);
    client_release();
    return NULL;
}

/* ---------------------------------------------------------------- main */
int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)      log_level = 2;
        else if (strcmp(argv[i], "-q") == 0) log_level = 0;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("用法: %s [端口] [-v|-q]\n"
                   "  端口   监听的 TCP 端口（默认 %d，仅绑定 127.0.0.1）\n"
                   "  -v     逐帧调试日志\n"
                   "  -q     只输出错误\n",
                   argv[0], DEFAULT_PORT);
            return 0;
        } else {
            int p = atoi(argv[i]);
            if (p > 0 && p < 65536) port = p;
            else { fprintf(stderr, "端口非法: %s\n", argv[i]); return 1; }
        }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (pipe(wakefd) < 0) { perror("pipe"); return 1; }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in a = { AF_INET, htons(port), { htonl(INADDR_LOOPBACK) }, {0} };

    /* bind/listen 必须检查：失败时若继续打印 "listening" 会让外部管理脚本
     * 误判启动成功（例如另一个实例已占用端口）。 */
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "bind %d failed: %s\n", port, strerror(errno));
        close(srv); return 1;
    }
    if (listen(srv, MAX_CLIENTS) < 0) {
        fprintf(stderr, "listen failed: %s\n", strerror(errno));
        close(srv); return 1;
    }

    /* 这行是外部脚本判断启动成功的标志，保持文本不变 */
    fprintf(stderr, "listening on %d\n", port);
    fflush(stderr);

    int next_id = 1;
    while (running) {
        /* 用 select 同时等待新连接与退出信号，使 SIGTERM 能被及时响应 */
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(srv, &rs);
        FD_SET(wakefd[0], &rs);
        int mx = srv > wakefd[0] ? srv : wakefd[0];

        int sel = select(mx + 1, &rs, NULL, NULL, NULL);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }
        if (FD_ISSET(wakefd[0], &rs)) break;          /* 收到退出信号 */
        if (!FD_ISSET(srv, &rs)) continue;

        int cli = accept(srv, NULL, NULL);
        if (cli < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            perror("accept"); break;
        }

        pthread_mutex_lock(&count_lock);
        int accepted = (client_count < MAX_CLIENTS);
        if (accepted) client_count++;
        pthread_mutex_unlock(&count_lock);

        if (!accepted) {
            dlog(1, "并发会话已达上限 %d，拒绝新连接", MAX_CLIENTS);
            close(cli);
            continue;
        }

        setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        Session *s = calloc(1, sizeof(Session));
        if (!s) {
            dlog(0, "会话分配失败");
            close(cli);
            client_release();
            continue;
        }
        s->fd       = cli;
        s->w        = 1920;   /* 握手前的占位值，真实尺寸由握手与 FORMAT_CHANGED 决定 */
        s->h        = 1080;
        s->id       = next_id++;
        s->codec_id = CODEC_H264;
        s->mime     = codec_mime(CODEC_H264);
        s->xfer     = XFER_TCP;
        s->shm_fd     = -1;   /* calloc 会置 0，而 0 是合法 fd，必须显式置 -1 */
        s->shm_listen = -1;

        pthread_t th;
        if (pthread_create(&th, NULL, session_thread, s) != 0) {
            dlog(0, "无法创建会话线程");
            close(cli);
            free(s);
            client_release();
            continue;
        }
        pthread_detach(th);
        dlog(1, "[%d] 客户端接入", s->id);
    }

    close(srv);

    /* 等待仍在运行的会话收尾，避免解码器资源被强行回收 */
    for (int i = 0; i < 50; i++) {
        pthread_mutex_lock(&count_lock);
        int n = client_count;
        pthread_mutex_unlock(&count_lock);
        if (n == 0) break;
        usleep(100000);
    }

    dlog(1, "daemon 退出");
    return 0;
}
