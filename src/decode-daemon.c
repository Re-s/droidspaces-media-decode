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
#include <sys/stat.h>
#include <sys/file.h>   /* flock：判活，见 main 里的锁逻辑 */
#include <fcntl.h>      /* open/O_* ：勿依赖 <sys/file.h> 间接传递，
                         * bionic/glibc 能过而 musl 直接编译失败（2026-08-25 实测） */
#include <sys/mman.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <linux/memfd.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define MAX_FRAME          (8*1024*1024)
/* 目录模式下的 socket 文件名。驱动侧的 DMD_DEFAULT_SOCK 必须与此一致。 */
#define DAEMON_SOCK_NAME "decode.sock"
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
#define HELLO_VERSION   3      /* v2: 增加共享内存传输协商
                                * v3: 响应可携带 endpoint dev/ino 扩展（见 do_handshake） */

/*
 * 监听端点的 (st_dev, st_ino)：unix 分支 bind+listen 成功后对最终 socket 路径
 * stat 一次，之后每个握手响应原样上报。客户端拿它和自己 stat 同一路径的结果
 * 比对 —— 不一致说明客户端侧解析到的是旧 socket（典型病因：平台把单个 socket
 * 文件而非目录做 bind mount，daemon 重启换 inode 后容器侧持死引用）。
 * 历史上这个场景两侧 stat 可能"看起来一致"（孤立 inode），人工诊断极难；
 * 让 daemon 报真值、客户端做校验，把这个诊断变成自动报错。
 * TCP 模式与抽象命名空间模式没有路径概念，保持 0，客户端据此跳过校验。
 */
static uint64_t g_ep_dev = 0;
static uint64_t g_ep_ino = 0;

/* bind/listen 成功后调用：采集真实端点标识，并应用 TEST-ONLY 覆盖。 */
static void endpoint_probe(const char *sock_path)
{
    struct stat st;
    if (sock_path && stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
        g_ep_dev = (uint64_t)st.st_dev;
        g_ep_ino = (uint64_t)st.st_ino;
    }
    /* ---- TEST-ONLY：仅供本机验证客户端的降级/不匹配路径，部署环境勿设 ---- */
    const char *fake = getenv("DMD_TEST_FAKE_INO");
    if (fake && *fake) {
        unsigned long long d = 0, i = 0;
        if (sscanf(fake, "%llu:%llu", &d, &i) == 2) {
            g_ep_dev = (uint64_t)d;
            g_ep_ino = (uint64_t)i;
            fprintf(stderr, "[TEST-ONLY] endpoint 上报被覆盖为 dev=%llu ino=%llu\n",
                    d, i);
        } else {
            fprintf(stderr, "[TEST-ONLY] DMD_TEST_FAKE_INO 格式应为 \"dev:ino\"，忽略\n");
        }
    }
}
#define FMTDESC_WORDS   8
/* 帧头 frame_size 取这些值时表示不是帧数据，而是控制消息 */
#define FMTDESC_SENTINEL 0xFFFFFFFFu   /* 随后 32 字节格式描述块 */

/* 格式描述块头部第 2 个字的能力标志（原为保留的 0）。
 * CAP_FRAME_PTS：每个帧头额外带第 4 个字段 —— 该帧对应的输入单元序号
 * （由 MediaCodec 通过 presentationTimeUs 原样回传）。
 * 驱动用它精确配对 surface，无需知道解码器按什么顺序出帧。 */
#define CAP_FRAME_PTS   0x00000001u

/* 输入单元序号 → presentationTimeUs 的放大倍数。
 *
 * ⚠️ 解码器按**毫秒**量化 PTS，直接用序号（步长 1us）会被全部压成 0 ——
 * 实测送 9 个单元回传的 PTS 全为 0，配对退化成"一个号对应多帧"
 * （表现为 unit 5 重复出现、35 次无匹配回退、70/150 帧错位）。
 * 乘 1000 后每个单元相差 1ms，量化后仍然唯一。
 * 客户端收到后除回去，还原成序号。 */
#define PTS_UNIT_SCALE  1000
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
 *
 * SHM_SLOTS 必须 >= 驱动侧的 DMD_PIPELINE_DEPTH（vaapi-driver/src/driver.h，
 * 当前 6）：驱动在 pending < 6 时会一直放行新的解码请求不取帧，若池只有
 * 4 个槽，第 5 帧就必然撞上"全忙"。这两个常量分处两个仓库目录、互不知情，
 * 是历史上的维护陷阱 —— 改动任一侧都要同步检查另一侧。
 * 8 槽在 4K 下约 95 MB（8 x 12533760），换来的是慢消费者场景不再丢帧。
 */
#define SHM_SLOTS      8

/*
 * 等空闲槽位的上限（毫秒）。必须显著大于客户端的 DMD_FRAME_TIMEOUT_MS
 * （driver.h，5000ms），否则 daemon 会比客户端先放弃并杀掉会话，
 * 把本可正常完成的解码变成丢帧。取 15 秒 = 客户端上限的 3 倍。
 */
#define SHM_SLOT_WAIT_MS 15000

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

/* ⚠️ 这些 id 是线协议的一部分，客户端按数值发送（握手第 4 个字）。
 * 只能在末尾追加，不可重排或复用已废弃的值 —— 改动会静默错配 codec。
 * 驱动侧的 DMD_CODEC_* 必须与本枚举逐值一致。 */
typedef enum {
    CODEC_H264 = 0,
    CODEC_HEVC = 1,
    CODEC_VP9  = 2,
    CODEC_VP8  = 3,
    CODEC_AV1  = 4,
    CODEC_MAX
} CodecId;

/* 设备硬件支持的解码器（见 doc/verified-platform-facts.md 的能力清单）。
 *
 * 注意本函数只做 id→mime 的静态映射，不代表当前设备真有对应硬件：
 * AV1 需要 SM8750（骁龙 8 Elite）一级的 Iris 解码器，SM8150 上没有这个单元。
 * 设备是否支持由 MediaCodec 在 configure 时决定 —— 拿不到解码器会握手失败，
 * 这是预期行为，不需要在此处按设备分支。 */
static const char *codec_mime(int id)
{
    switch (id) {
    case CODEC_H264: return "video/avc";
    case CODEC_HEVC: return "video/hevc";
    case CODEC_VP9:  return "video/x-vnd.on2.vp9";
    case CODEC_VP8:  return "video/x-vnd.on2.vp8";
    case CODEC_AV1:  return "video/av01";
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
    /* VP8 / VP9 / AV1 一律返回 0：它们不用 Annex-B start code，参数集不是
     * 独立的"NALU"。AV1 的序列头是 OBU_SEQUENCE_HEADER，与 tile 数据同在
     * 一个 temporal unit 里由驱动整体转发（见 vaapi-driver/src/decode.c
     * 的 DMD_CODEC_AV1 分支），daemon 侧无需单独识别。 */
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

/*
 * 返回 0 成功，SEND_PEER_GONE 对端已正常关闭，-1 真错误。
 *
 * 为什么要把"对端关了"单独列一档：客户端拿够帧后直接 close 是正常收尾
 * （ffmpeg -f null、播放器 seek/停止都这样），而 daemon 手上往往还有几帧
 * 在流水线里，写进已关闭的 fd 必然得到 EPIPE/ECONNRESET。实测 412 个真实
 * 解码会话里 78.6% 以此路径结束，一律记成"发送帧失败"会让绝大多数正常
 * 会话看起来像出错，把真正的故障埋掉。已用字节级证据确认此路径不丢帧：
 * 客户端落盘 933120000 字节 ÷ (1920×1080×1.5) = 精确 300.000 帧。
 */
#define SEND_PEER_GONE (-2)

static int send_all(int fd, const void *b, size_t l)
{
    const char *p = b;
    while (l > 0) {
        ssize_t n = write(fd, p, l);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            /* socket 是阻塞的且没设 SO_SNDTIMEO，所以缓冲满只会让 write
             * 阻塞，不会返回错误；真返回错误就是对端出了问题。 */
            if (errno == EPIPE || errno == ECONNRESET) {
                dlog(2, "send_all: 对端已关闭 (%s)", strerror(errno));
                return SEND_PEER_GONE;
            }
            dlog(1, "send_all: write 失败: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            dlog(2, "send_all: write 返回 0（对端已关闭）");
            return SEND_PEER_GONE;
        }
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
    /* 客户端提前 close 后未能送出的流水线尾帧数。不是丢帧：客户端已经
     * 不需要这些帧了（实测逐帧 md5 与软解参考 10/10 一致）。单独计数
     * 是为了让运维一眼分清"正常收尾"与"传输故障"。 */
    long             frames_dropped_at_exit;
    int              peer_gone;        /* 客户端正常关闭，非故障 */
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

    /* 可逆排空（长度 0 的带内请求）
     *
     * drain_req 由 input 线程自增，drain_done 由 output 线程在完成
     * EOS 处理 + AMediaCodec_flush 后自增。input 线程等两者相等才继续送料，
     * 否则新数据会被 flush 一起丢掉，表现为吞帧。 */
    volatile sig_atomic_t drain_req;
    volatile sig_atomic_t drain_done;

    /* 输入单元序号，用作 PTS 标签（1 起）。仅 input 线程写。
     * 让每个输出帧能被追溯到"第几次提交"，驱动据此精确配对 surface，
     * 无需知道解码器的输出顺序。 */
    uint64_t         vcl_in;
    /* CSD 副本：flush 清掉解码器里的参数集，排空后必须原样重送。
     * SPS+PPS 量级很小，256 字节足够（实测 1080p 为 31+9）。 */
    uint8_t          csd_keep[256];
    size_t           csd_keep_len;
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
 *   等于魔数    → 继续读余下 20 字节，按声明配置会话，回握手响应
 *   不等于魔数  → 协议不符，拒绝连接（回裸 12 字节 status=4）
 * 返回 0 表示可以继续，-1 表示应当断开。
 * 响应格式与版本协商详见函数内注释与本文件 HELLO_VERSION 处说明。
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

    /* 版本协商：daemon 支持 {2..HELLO_VERSION}。不再要求严格相等 ——
     * 严格相等意味着 daemon 与驱动必须同步升级，任何一端先更新就全体断连。
     * 客户端 version>=3 时响应携带 endpoint 扩展；v2 客户端收到与旧版
     * 完全相同的 12 字节，行为不变。 */
    if (ver < 2 || ver > HELLO_VERSION) {
        dlog(1, "[%d] 握手版本不支持: %u（支持 2..%d）", s->id, ver, HELLO_VERSION);
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
     *
     * v3 扩展（客户端请求 version>=3 时）：namelen 字段的 bit31 置 1 作为
     * 标记（真实 namelen 远小于 2^31），随后在名字字节之前追加 16 字节：
     *   [u32 dev_hi][u32 dev_lo][u32 ino_hi][u32 ino_lo]   （各自为大端）
     * 即 g_ep_dev/g_ep_ino 各按 64 位拆高低两个 u32。
     * v2 客户端的响应一个字节都不变；错误路径的响应也保持 12 字节裸格式，
     * 客户端先看 status 再决定是否解析扩展。
     *
     * TEST-ONLY：DMD_TEST_REPLY_LEGACY=1 强制按 v2 形状回包，
     * 用于验证客户端对旧 daemon 的降级路径。
     */
    size_t nlen = (want == XFER_SHM) ? strlen(s->shm_name) : 0;
    int use_ext = (ver >= 3) && getenv("DMD_TEST_REPLY_LEGACY") == NULL;
    uint32_t nlen_wire = (uint32_t)nlen;
    if (use_ext)
        nlen_wire |= 0x80000000u;
    uint32_t reply[3] = { htonl(0), htonl((uint32_t)want), htonl(nlen_wire) };
    if (send_all(s->fd, reply, sizeof(reply)) < 0) goto reply_fail;
    if (use_ext) {
        uint32_t ext[4] = {
            htonl((uint32_t)(g_ep_dev >> 32)), htonl((uint32_t)(g_ep_dev & 0xffffffffu)),
            htonl((uint32_t)(g_ep_ino >> 32)), htonl((uint32_t)(g_ep_ino & 0xffffffffu)),
        };
        if (send_all(s->fd, ext, sizeof(ext)) < 0) goto reply_fail;
    }
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

    /* 帧回传方式只有两种：SHM（memfd 零拷贝）或走控制连接内联。
     *
     * ⚠️ 这里**故意不叫 "TCP"**。老版本印的是 `传输=TCP`，但那个字段描述的是
     * "帧不走 SHM、而是内联在控制连接里回传"，与控制通道究竟是 TCP 还是
     * Unix socket 无关 —— 走 --sock 时它照样印 TCP。这个歧义害我误判过一次
     * 根因（把 SELinux domain 导致的解码失败当成 SHM 帧交付的 bug，
     * 因为日志显示 TCP 就以为 SHM 没参与）。
     * 控制通道类型请看启动时的 `listening on ...` 那行。 */
    dlog(1, "[%d] 握手成功: %s %ux%u 帧回传=%s",
         s->id, mime, w, h, s->xfer == XFER_SHM ? "SHM" : "内联");
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

        /* 长度 0 = 排空请求（可逆），不是数据单元。
         *
         * 为什么需要它：消费者只保持 3 帧在飞，而本机解码器有 B 帧时要收到
         * 第 4 个输入单元才吐首帧 —— 双方互等。原先驱动只能靠
         * shutdown(SHUT_WR) 逼出帧，但那是**不可逆**的：会话作废、必须重建，
         * 实测每帧 155 ms、播放慢 4.7 倍。
         *
         * 而 EOS + AMediaCodec_flush 在"会话不作废"这个意义上是可逆的：
         * 送 EOS 让解码器吐出在手的帧，收到 EOS 标志后 flush 复位，
         * 重送 CSD，会话继续可用，不必重建连接。
         *
         * ⚠️ 但"可逆"仅指**会话对象**可继续使用，**不指画面无损**。
         * tools/probe_drain.c 曾报告"排空 3 帧后 flush 重送 CSD 能继续解出
         * 6 帧"，那句话只数了帧数、没看画面 —— 该探针不验证像素内容，
         * 结论不足以支撑"无损"。
         *
         * 后来的黑帧调查已经确证：**flush 会摧毁参考帧链**，续解出的
         * P/B 帧要一直黑到下一个 IDR（实测浏览器 135/708 帧纯黑，
         * 修复后才归零）。所以排空是**有画面代价**的操作，
         * 驱动侧已把触发条件收紧到几乎不发生（见 decode.c 的 wait_is_futile）。
         *
         * 选 sz==0 承载：它原本被判为非法长度，老客户端不会发，
         * 因此无需协议版本协商。 */
        if (sz == 0) {
            s->drain_req++;
            dlog(2, "[%d] 收到排空请求 #%u", s->id, s->drain_req);

            ssize_t di = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_TIMEOUT_US);
            if (di < 0) {
                dlog(1, "[%d] 排空取输入缓冲失败: %zd", s->id, di);
                continue;
            }
            /* 送 EOS：解码器会把已排队但因流水线深度压着的帧全部吐出。 */
            AMediaCodec_queueInputBuffer(s->codec, di, 0, 0, 0, FLAG_END_OF_STREAM);

            /* 等 output 线程见到 EOS 并完成 flush，再继续收下一个单元。
             * 必须等：flush 会丢弃解码器里的一切，若此时已经把新数据塞进去
             * 就会被一起丢掉，表现为吞帧。
             *
             * ⚠️ 必须有上界。output 线程可能因为解码错误提前退出，
             * 那时 drain_done 永远追不上 drain_req —— 无界等待会让 input 线程
             * 永久卡住、会话泄漏、解码器不释放。实测后果是 8 个会话全部泄漏、
             * daemon 空转 200% CPU 并开始拒绝新连接（"并发会话已达上限 8"），
             * 表现却是"浏览器一帧也解不出来"，极易误判成别处的问题。 */
            int waited_us = 0;
            const int drain_wait_max_us = 2000000;   /* 2s：远大于正常排空耗时 */
            while (running && !s->stop &&
                   s->drain_done < s->drain_req &&
                   waited_us < drain_wait_max_us) {
                usleep(1000);
                waited_us += 1000;
            }
            if (s->drain_done < s->drain_req) {
                dlog(1, "[%d] 排空等待超时（%d ms），放弃会话",
                     s->id, waited_us / 1000);
                s->stop = 1;
                break;
            }

            /* flush 清掉了 CSD，必须重送，否则后续 VCL 无参考参数集。
             * 这里用累积的 csd（首次送入时保留了副本）。 */
            if (s->csd_keep_len > 0) {
                ssize_t ci = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_TIMEOUT_US);
                if (ci >= 0) {
                    size_t cap;
                    uint8_t *ib = AMediaCodec_getInputBuffer(s->codec, ci, &cap);
                    if (ib && cap >= s->csd_keep_len) {
                        memcpy(ib, s->csd_keep, s->csd_keep_len);
                        AMediaCodec_queueInputBuffer(s->codec, ci, 0,
                                                     s->csd_keep_len, 0,
                                                     FLAG_CODEC_CONFIG);
                        dlog(2, "[%d] 排空后已重送 CSD (%zu 字节)",
                             s->id, s->csd_keep_len);
                    } else {
                        AMediaCodec_queueInputBuffer(s->codec, ci, 0, 0, 0, 0);
                    }
                }
            }
            continue;
        }

        if (sz > MAX_FRAME) {
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
                    /* 留一份副本：排空后的 flush 会清掉解码器里的 CSD，
                     * 那时要原样重送，否则后续 VCL 无参数集可参考。 */
                    if (csd_len <= sizeof(s->csd_keep)) {
                        memcpy(s->csd_keep, csd, csd_len);
                        s->csd_keep_len = csd_len;
                    }
                } else {
                    dlog(1, "[%d] CSD 超出输入缓冲容量", s->id);
                    AMediaCodec_queueInputBuffer(s->codec, ci, 0, 0, 0, 0);
                }
            } else {
                dlog(1, "[%d] 取输入缓冲失败(CSD): %zd", s->id, ci);
            }
            csd_len = 0;
        }

        /* 取输入缓冲：拿不到必须**重试**，绝不能丢这个 NALU。
         *
         * ⚠️ 这里曾经只 dlog + continue —— 那等于静默丢弃一个 NALU，
         * 而丢任何一个 VCL 都会毁掉后续的参考帧链，画面必然坏掉。
         *
         * 什么时候会拿不到？输入缓冲被占满，而这只在**消费者猛灌**时发生：
         * ffmpeg 与 Firefox 只保持少量在途帧，从不触发；Chrome 会一次性
         * 投出数百个解码请求，实测 259 个单元进来后输入缓冲耗尽，
         * 于是 "取输入缓冲失败: -1" → 上层判定发送失败 → 会话直接结束，
         * Chrome 侧表现为硬解完全不可用。
         *
         * 输入缓冲耗尽是**背压**而非错误：output 线程取走帧后缓冲就会
         * 回收。所以这里要一直等，只有真正的错误码才放弃。 */
        ssize_t bi;
        int bi_tries = 0;
        for (;;) {
            bi = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_TIMEOUT_US);
            if (bi >= 0)
                break;
            if (bi != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                dlog(1, "[%d] 取输入缓冲错误: %zd", s->id, bi);
                break;
            }
            if (s->stop)
                break;
            bi_tries++;
            dlog(1, "[%d] 输入缓冲暂满，重试 #%d（等 %d ms）",
                 s->id, bi_tries, INPUT_TIMEOUT_US / 1000);
            if (bi_tries >= 12) {   /* 12 × 5s = 60s，足够任何正常背压 */
                dlog(1, "[%d] 输入缓冲持续不可用，放弃", s->id);
                break;
            }
        }
        if (bi < 0)
            goto done;   /* 拿不到就结束会话，但不静默丢帧 */
        size_t cap;
        uint8_t *ib = AMediaCodec_getInputBuffer(s->codec, bi, &cap);
        if (!ib || cap < sz) {
            dlog(1, "[%d] NALU 超出输入缓冲容量 (%u > %zu)", s->id, sz, cap);
            AMediaCodec_queueInputBuffer(s->codec, bi, 0, 0, 0, 0);
            continue;
        }
        memcpy(ib, buf, sz);
        /* PTS 用"第几个数据单元"的序号（1 起，步长 1us）。
         *
         * 这不是为了计时，而是给每个输入单元一个**可回传的身份标签**：
         * MediaCodec 会把 presentationTimeUs 原样带到对应的输出帧上
         * （tools/probe_negotiate.c 实测：值域与步长都能对上，未被改写）。
         * output 线程把它放进下行帧头，驱动就能精确知道"这一帧对应第几次提交"，
         * 从而按提交序号配对 surface —— 与解码器按什么顺序出帧完全无关。
         *
         * 这样就不需要让驱动去猜/协商输出顺序：显示序也好、跟随输入序也好，
         * 配对都正确。此前依赖输出顺序的做法很脆弱：一旦两侧假设不一致，
         * 后果是画面错位而不报错（实测 105/150 帧错位）。
         *
         * 从 1 开始：0 留作"无 PTS 信息"的哨兵，便于兼容旧 daemon。 */
        AMediaCodec_queueInputBuffer(s->codec, bi, 0, sz,
                                     (int64_t)s->vcl_in * PTS_UNIT_SCALE, 0);
        s->vcl_in++;
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
 * 经共享内存送出一帧：拷进空闲槽位，socket 只写 24 字节控制消息。
 *
 *   [4B 宽][4B 高][4B 0xFFFFFFFE][4B 槽位号][4B 数据长度][4B 输入单元序号]
 *
 * ⚠️ 是 6 个字（24 字节）而非 5 个。第 6 字段无条件发送，
 * CAP_FRAME_PTS 只是在格式描述块里告知客户端该字段存在，不是发送开关。
 * 少读这个字会让客户端把下一帧从 PTS 开始解析，整条流错位 ——
 * client/ 参考实现曾因此出错。此处注释此前写 20 字节，是多份文档抄错的源头。
 *
 * 相比 TCP 模式省掉了内核态的两次拷贝（send 时进 socket 缓冲、recv 时出）。
 * 仍保留一次 MediaCodec 输出缓冲 → 共享内存的 CPU 拷贝，
 * 要去掉它需要 dmabuf 零拷贝（见 doc/performance-and-roadmap.md）。
 *
 * 返回 0 成功，SEND_PEER_GONE 客户端已正常离开，-1 真错误。
 */
static int send_frame_shm(Session *s, const uint8_t *data, size_t len,
                          uint32_t pts)
{
    if (len > s->shm_slot) {
        dlog(1, "[%d] 帧 %zu 字节超出槽位 %zu，需重建池", s->id, len, s->shm_slot);
        return -1;
    }

    /* 找一个空闲槽位。轮转起点是 shm_next，保证公平使用所有槽位。
     * 客户端处理完会把状态字置 0。
     *
     * 等待上限必须显著大于客户端侧的 DMD_FRAME_TIMEOUT_MS（driver.h，5000ms）：
     * 客户端每条取帧入口都可以合法阻塞 5 秒，而这里原先只等 1 秒就判死并
     * 杀掉整个会话 —— 1000 < 5000 这个不等式本身就是 bug。实测 4K + 慢消费者
     * （ffmpeg 落盘）场景下 10/10 触发，真实丢掉 56~83% 的帧，客户端报
     * Conversion failed! 这是本模块唯一造成真实丢帧的路径。
     *
     * 槽位暂时全忙是**正常背压**，不是故障：只要客户端还活着就该继续等，
     * 由客户端自己的超时机制去决定放弃。参照 Wayland wl_buffer.release、
     * GstBufferPool 默认阻塞、V4L2 缓冲池的一致做法 —— 没有一个把池耗尽
     * 当致命错误。 */
    int slot = -1;
    for (int spin = 0; spin < SHM_SLOT_WAIT_MS && slot < 0; spin++) {
        for (int k = 0; k < SHM_SLOTS; k++) {
            int idx = (s->shm_next + k) % SHM_SLOTS;
            uint32_t st = __atomic_load_n(shm_slot_state(s, idx), __ATOMIC_ACQUIRE);
            if (st == 0) { slot = idx; break; }
        }
        if (slot < 0) {
            /* 客户端已走或 daemon 要退出：按"对端离开"处理，不是故障 */
            if (!running || s->stop) return SEND_PEER_GONE;
            /* 等待期偏长时留一条痕迹，便于诊断慢消费者（每 2 秒一条） */
            if (spin > 0 && spin % 2000 == 0)
                dlog(2, "[%d] 槽位全忙，已等 %d ms（客户端消费慢，正常背压）",
                     s->id, spin);
            usleep(1000);
        }
    }
    if (slot < 0) {
        /* 等满上限仍无槽位 —— 客户端确实卡死了，这才是真错误 */
        dlog(1, "[%d] 等不到空闲槽位 %d ms（客户端卡死），结束会话",
             s->id, SHM_SLOT_WAIT_MS);
        return -1;
    }

    memcpy(shm_slot_data(s, slot), data, len);
    /* release 序：确保数据写入对读到状态字的客户端可见 */
    __atomic_store_n(shm_slot_state(s, slot), 1u, __ATOMIC_RELEASE);
    s->shm_next = (slot + 1) % SHM_SLOTS;

    /* 第 6 字段 = 输入单元序号，与 TCP 路径的第 4 字段同义，
     * 让两种传输模式对驱动呈现一致的配对信息。 */
    uint32_t msg[6] = {
        htonl((uint32_t)s->w), htonl((uint32_t)s->h),
        htonl(SHMFRAME_SENTINEL),
        htonl((uint32_t)slot), htonl((uint32_t)len),
        htonl(pts)
    };
    int rc = send_all(s->fd, msg, sizeof(msg));
    if (rc < 0) {
        /* 通知失败则立即释放槽位，否则池会漏 */
        __atomic_store_n(shm_slot_state(s, slot), 0u, __ATOMIC_RELEASE);
        return rc;
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

    /* 第 2 个字原为保留的 0，现用作能力标志：置 CAP_FRAME_PTS 表示
     * 后续每个帧头都带第 4 个字段（PTS，即输入单元序号）。
     * 旧 daemon 这里恒为 0，客户端据此按 3 字段帧头解析 —— 向后兼容。 */
    uint32_t hdr[3] = { htonl(0), htonl(CAP_FRAME_PTS),
                        htonl(FMTDESC_SENTINEL) };
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
                    int rc;
                    if (s->xfer == XFER_SHM) {
                        rc = send_frame_shm(s, ob + info.offset,
                                            (size_t)info.size,
                                            (uint32_t)(info.presentationTimeUs / PTS_UNIT_SCALE));
                    } else {
                        /* 第 4 字段 = 该帧对应的输入单元序号。
                         * MediaCodec 把 queueInputBuffer 时给的
                         * presentationTimeUs 原样带到输出帧上，
                         * 于是驱动能精确知道这一帧对应第几次提交。 */
                        uint32_t hdr[4] = {
                            htonl((uint32_t)s->w),
                            htonl((uint32_t)s->h),
                            htonl((uint32_t)info.size),
                            htonl((uint32_t)(info.presentationTimeUs / PTS_UNIT_SCALE))
                        };
                        rc = send_all(s->fd, hdr, sizeof(hdr));
                        size_t off = (size_t)info.offset;
                        size_t rem = (size_t)info.size;
                        while (rc == 0 && rem > 0) {
                            size_t ch = rem > SEND_CHUNK ? SEND_CHUNK : rem;
                            rc = send_all(s->fd, ob + off, ch);
                            if (rc != 0) break;
                            off += ch; rem -= ch;
                        }
                    }
                    if (rc == SEND_PEER_GONE) {
                        /* 客户端已拿够帧并正常关闭，剩下的是流水线尾帧
                         * （实测固定 14 帧）。这是正常收尾，不是故障：
                         * 逐帧 md5 与软解参考比对 10/10 完全一致，无丢帧。
                         * 单独计数便于运维区分，日志压到 debug 级。 */
                        s->frames_dropped_at_exit++;
                        s->peer_gone = 1;
                        s->stop = 1;
                    } else if (rc < 0) {
                        dlog(1, "[%d] 发送帧失败（传输错误），结束会话", s->id);
                        s->stop = 1;
                    } else {
                        s->frames_out++;
                        dlog(2, "[%d] 帧 %dx%d %d 字节", s->id, s->w, s->h, info.size);
                    }
                }
            }
            AMediaCodec_releaseOutputBuffer(s->codec, oi, 0);
            if (eos) {
                /* EOS 有两种来源，处理完全不同：
                 *   1) 排空请求（drain_req > drain_done）—— 会话还要继续用，
                 *      flush 复位解码器后接着服务，由 input 线程重送 CSD
                 *   2) 客户端真的关了写端 —— 流结束，退出线程
                 * 区分依据就是有没有未完成的排空请求。 */
                if (s->drain_req > s->drain_done) {
                    media_status_t fs = AMediaCodec_flush(s->codec);
                    if (fs != AMEDIA_OK) {
                        dlog(1, "[%d] 排空后 flush 失败: %d", s->id, fs);
                        s->stop = 1;
                        break;
                    }
                    /* flush 会丢弃未取走的输出，格式描述也要重新下发：
                     * 客户端靠它确定 stride/crop，漏发会让后续帧解析错位。 */
                    s->fmt_sent = 0;
                    s->drain_done++;
                    dlog(2, "[%d] 排空 #%u 完成，会话继续", s->id, s->drain_done);
                    continue;
                }
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

    /* 低延迟模式：让解码器尽快吐帧，而不是攥着几帧等流水线填满。
     *
     * 为什么必须开：MediaCodec 稳态滞后 2-3 个输入单元，而浏览器里的
     * ffmpeg 稳态只保持 3 帧在飞（H.264 重排深度 has_b_frames=2 决定），
     * 送完第 3 帧就 vaSyncSurface 阻塞等第 1 帧。双方差正好一帧 →
     * 互等 → 驱动侧兜底 flush（不可逆 shutdown(SHUT_WR)）→ 会话作废 →
     * 浏览器永久回落软解。实测 Firefox 140 只硬解出 1 帧就掉回软解。
     * 命令行 ffmpeg 不受影响，因为它送料远远超前，盖住了这个滞后。
     *
     * 为什么用字面量而不用 AMEDIAFORMAT_KEY_LOW_LATENCY：
     * 该符号是 __INTRODUCED_IN(30)（NdkMediaFormat.h:321），而本 daemon
     * 按 API 29 构建。key 本身只是字符串常量，直接写字面值即可，
     * 既不用抬高构建 API，也不引入弱符号判空。
     * 低于 API 30 的设备上 MediaCodec 会忽略未知 key —— 退化为原有行为，
     * 不会失败。 */
    AMediaFormat_setInt32(fmt, "low-latency", 1);

    /* 让解码器按**解码顺序**输出，而不是攒够重排缓冲再按显示顺序吐。
     *
     * 这是根治画面黑屏闪烁的手段。默认（显示序）下有 B 帧时要收到第 4 个
     * 输入单元才吐首帧，而浏览器稳态只保持 3 帧在飞 —— 差一帧，双方死等。
     * 此前只能靠 EOS 逼出帧，但 EOS/flush/重建会话都会摧毁参考帧链，
     * 导致约 9 成帧纯黑（tools/probe_black.c：60 帧里 54 帧亮度为 16）。
     *
     * 开这个键后滞后从 4 降到 1（tools/probe_keys.c 逐键实测：low-latency、
     * max-output-reorder-frames、output-delay、vendor.qti-ext-dec-low-latency
     * 全都无效，只有这个键有用），互等消失，排空/重建/重放统统不再需要。
     *
     * 历史注意事项（已消除）：早期驱动用编译期常量 DMD_DECODE_ORDER_OUTPUT
     * 声明"解码器按什么顺序出帧"，必须与这里严格一致，否则画面错位且不报错
     * （实测 test1080 帧数对但 105/150 帧错位）。该常量已删除 —— 现在每帧
     * 回传自己的输入单元序号（CAP_FRAME_PTS），驱动按序号精确配对，
     * 与输出顺序完全解耦，改这个键不再需要驱动配合。
     *
     * 用字面量：这是高通 vendor 扩展，NDK 头文件里没有定义。
     * 非高通平台会忽略未知键，退化为原有行为，不会失败。 */
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);

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

    /* 收尾原因写进日志：实测 412 个真实会话中 78.6% 是客户端拿够帧就
     * close（正常），此前它们与真故障共用同一条 "发送帧失败" 日志，
     * 导致绝大多数正常会话看起来像出错。 */
    if (s->peer_gone)
        dlog(1, "[%d] 会话结束(客户端正常关闭): 收到 %ld NALU, 回传 %ld 帧, "
                "尾帧 %ld 未发送",
             s->id, s->nalu_in, s->frames_out, s->frames_dropped_at_exit);
    else
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
    const char *sock_path = NULL;   /* 非 NULL = 监听 Unix socket 而非 TCP */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)      log_level = 2;
        else if (strcmp(argv[i], "-q") == 0) log_level = 0;
        else if (strcmp(argv[i], "--sock") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("用法: %s [端口] [--sock 路径] [-v|-q]\n"
                   "  端口          监听的 TCP 端口（默认 %d，仅绑定 127.0.0.1）\n"
                   "  --sock 路径   改为监听该路径的 Unix socket（推荐）\n"
                   "  -v            逐帧调试日志\n"
                   "  -q            只输出错误\n"
                   "\n"
                   "两种监听方式的区别：\n"
                   "  TCP 127.0.0.1 依赖容器与 Android **共享 net namespace**。\n"
                   "  DroidSpaces 的 host 型容器满足，NAT 型容器不满足\n"
                   "  （实测 NAT 容器 netns 独立，127.0.0.1 不可达）。\n"
                   "\n"
                   "  --sock 建的是路径式 Unix socket，不属于 net namespace，\n"
                   "  由平台把它 bind mount 进容器即可用 —— 两类容器都通，\n"
                   "  且鉴权直接靠文件权限，不必把服务暴露到网络。\n"
                   "  DroidSpaces 自己的显示通道就是这个模式\n"
                   "  （/data/local/tmp/anland-*.sock → 容器 /run/display.sock）。\n"
                   "\n"
                   "⚠️ 在 Android 上建 socket 文件需要合适的 SELinux domain。\n"
                   "  用 su 直接启动会跑在 u:r:ksu:s0 下，bind 得到 EACCES。\n"
                   "  实测可行的启动方式：\n"
                   "    runcon u:r:droidspacesd:s0 %s --sock /data/local/tmp/dmd.sock\n",
                   argv[0], DEFAULT_PORT, argv[0]);
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

    int srv;
    int opt = 1;

    /* --sock 指向**目录**时，在其中建固定名 socket。
     *
     * 这是为了解决 bind mount 的 inode 语义问题：bind() 只能创建新 inode，
     * 所以 daemon 每次重启都会换 inode，而 bind mount 绑的是 inode ——
     * 挂单个 socket 文件时，daemon 一重启容器侧就变成 ECONNREFUSED，
     * 必须重新挂载。
     *
     * 改挂**目录**就没这个问题：目录 inode 稳定，里面的 socket 换 inode
     * 不影响挂载。平台只需挂一次：
     *   宿主 /data/local/tmp/dmd/  →  容器 /run/dmd/
     * 之后 daemon 随便重启，容器一直能连 /run/dmd/decode.sock。 */
    char sock_in_dir[256];
    char sock_trimmed[256];
    if (sock_path) {
        /* 先剥掉尾部斜杠，否则拼出来是 dir//decode.sock ——
         * 能用，但日志里的双斜杠会和文档里要匹配的字符串对不上，
         * 平台照抄验证清单时会以为没生效。 */
        size_t sl = strlen(sock_path);
        while (sl > 1 && sock_path[sl - 1] == '/') sl--;
        if (sl < sizeof(sock_trimmed)) {
            memcpy(sock_trimmed, sock_path, sl);
            sock_trimmed[sl] = '\0';
            sock_path = sock_trimmed;
        }

        /* 只 stat 一次并把结果记下来 —— 不要在 else 分支里重复 stat 或去读
         * errno：stat 成功时 errno 未定义，靠它判分支是脆的。 */
        struct stat dst;
        int st_ok = (stat(sock_path, &dst) == 0);

        if (st_ok && S_ISDIR(dst.st_mode)) {
            snprintf(sock_in_dir, sizeof(sock_in_dir), "%s/%s",
                     sock_path, DAEMON_SOCK_NAME);
            dlog(1, "--sock 是目录，实际监听 %s", sock_in_dir);
            sock_path = sock_in_dir;
        } else if (!st_ok) {
            /* 路径不存在。老行为是直接把它当 socket 文件名 bind ——
             * 那会**静默**造出一个单文件 socket，而单文件挂载在 daemon
             * 重启后必然失效（inode 变了）。平台拼错路径或忘了建目录时
             * 拿不到任何警告，几天后才在"容器突然 ECONNREFUSED"里暴露。
             *
             * 现在的处理：以斜杠结尾、或看起来就是目录名（不含 .sock）时，
             * 认为用户想要目录模式，直接建出来；否则保留文件模式并告警。 */
            int wants_dir = (strstr(sock_path, ".sock") == NULL);
            if (wants_dir) {
                if (mkdir(sock_path, 0755) == 0 || errno == EEXIST) {
                    snprintf(sock_in_dir, sizeof(sock_in_dir), "%s/%s",
                             sock_path, DAEMON_SOCK_NAME);
                    dlog(1, "--sock 目录不存在，已创建；实际监听 %s",
                         sock_in_dir);
                    sock_path = sock_in_dir;
                } else {
                    /* 注意：此处 srv 尚未创建（它在下面的分支里才 socket()），
                     * 所以不要 close 它 —— 早前版本在这里 close(srv) 用的是
                     * 未初始化值，编译器已警告。 */
                    fprintf(stderr,
                            "创建目录 %s 失败: %s\n"
                            "（若本意是直接指定 socket 文件，请让文件名以 .sock 结尾）\n",
                            sock_path, strerror(errno));
                    return 1;
                }
            } else {
                dlog(1, "⚠ --sock 指向单个 socket 文件 %s —— "
                        "该文件的 inode 每次重启都会变，若平台按文件做 bind mount，"
                        "重启后容器侧会 ECONNREFUSED。建议改传目录。",
                     sock_path);
            }
        }
    }

    if (sock_path) {
        /* Unix socket 监听：路径式，不属于 net namespace，
         * 由平台 bind mount 进容器 —— host 型与 NAT 型容器都能用。 */
        srv = socket(AF_UNIX, SOCK_STREAM, 0);
        if (srv < 0) { perror("socket"); return 1; }

        struct sockaddr_un ua;
        memset(&ua, 0, sizeof(ua));
        ua.sun_family = AF_UNIX;
        if (strlen(sock_path) >= sizeof(ua.sun_path)) {
            fprintf(stderr, "sock 路径过长（上限 %zu）: %s\n",
                    sizeof(ua.sun_path) - 1, sock_path);
            close(srv); return 1;
        }
        strncpy(ua.sun_path, sock_path, sizeof(ua.sun_path) - 1);

        /* 残留 socket 文件的处理 —— 这里有个**部署顺序约束**，务必看完。
         *
         * bind() 只能创建**新 inode**，无法绑到已存在的文件。而 bind mount
         * 绑定的是 inode 而非路径：一旦 unlink 重建，容器侧的挂载点就指向
         * 孤立 inode，表现为 connect 得到 ECONNREFUSED —— 宿主两侧 stat
         * 看到的 inode 甚至会一致（都是那个孤立 inode），很容易误判。
         *
         * 所以正确的部署顺序是：
         *   1. daemon 先启动、建好 socket 文件
         *   2. 平台**再** bind mount 进容器
         *   3. daemon 重启后必须**重新挂载**（inode 变了）
         *
         * 这里只在"确认没有活的监听者"时才删残留：先 connect 探一下，
         * 连得上说明另一个实例正在服务，直接报错退出而不是抢它的路径。 */
        /* 判活用 flock 而**不是** connect 探测。
         *
         * 早前版本是"connect 一下，连得上就认为有活实例"。那样有两个问题：
         *   1. 用的是阻塞 socket 且 connect 没有上界 —— 旧实例 backlog 打满时
         *      新进程会挂死在启动路径上；而如果内核返回 EAGAIN，还会被误判成
         *      "无监听者"，进而 unlink 掉**活实例正在用的** socket 文件。
         *   2. 对活实例有副作用：每次被拒的启动都让旧实例白跑一次 accept +
         *      建线程再销毁，并短暂占用一个并发配额。
         *
         * flock 没有这些问题：不碰对方进程、无需超时、且进程无论怎么死
         * （含 SIGKILL）内核都会释放锁，所以不会留下假的"已占用"状态。
         * 锁文件与 socket 同目录、独立命名，句柄故意**不关闭** ——
         * 靠它在整个进程生命周期内持有锁。 */
        char lockpath[300];
        snprintf(lockpath, sizeof(lockpath), "%s.lock", sock_path);
        int lockfd = open(lockpath, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lockfd < 0) {
            fprintf(stderr, "打开锁文件 %s 失败: %s\n",
                    lockpath, strerror(errno));
            close(srv);
            return 1;
        }
        if (flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK)
                fprintf(stderr, "%s 上已有实例在运行，拒绝启动\n", sock_path);
            else
                fprintf(stderr, "flock %s 失败: %s\n",
                        lockpath, strerror(errno));
            close(lockfd);
            close(srv);
            return 1;
        }
        /* 锁已拿到，说明没有活实例 —— lockfd 故意不关，保持持有。 */

        struct stat st;
        if (stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            /* 拿到锁还看到 socket 文件，就是上次退出留下的死文件，可以删。
             * ⚠️ 但这会换掉 inode，已有的 bind mount 会失效，需重新挂载。 */
            fprintf(stderr, "清理残留 socket 文件 %s"
                            "（注意：inode 将改变，已有的 bind mount 需重挂）\n",
                    sock_path);
            unlink(sock_path);
        }

        if (bind(srv, (struct sockaddr *)&ua, sizeof(ua)) < 0) {
            fprintf(stderr, "bind %s failed: %s\n", sock_path, strerror(errno));
            /* Android 上最常见的原因是 SELinux domain 不对：
             * su 启动会跑在 u:r:ksu:s0，bind 得到 EACCES。 */
            if (errno == EACCES)
                fprintf(stderr, "  提示: 试试 runcon u:r:droidspacesd:s0 启动\n");
            close(srv); return 1;
        }
        /* 0660 + 由平台决定属组。这里放宽到 0666 是为了先跑通；
         * 真实部署应收紧到特定 gid（参考 /dev/dri 用的 droidspaces-gpu）。 */
        if (chmod(sock_path, 0666) < 0)
            dlog(1, "chmod %s 警告: %s", sock_path, strerror(errno));

        if (listen(srv, MAX_CLIENTS) < 0) {
            fprintf(stderr, "listen failed: %s\n", strerror(errno));
            close(srv); unlink(sock_path); return 1;
        }
        /* 与 TCP 分支保持同样的"启动成功"标志格式，外部脚本可统一匹配 */
        fprintf(stderr, "listening on %s\n", sock_path);
        /* 采集端点标识供握手上报；TEST-ONLY 钩子在此一并生效。
         * 这两行是 inode 校验机制的"服务端说真话"半边 —— 客户端拿它对账。 */
        endpoint_probe(sock_path);
        fprintf(stderr, "listening endpoint: dev=%llu ino=%llu\n",
                (unsigned long long)g_ep_dev, (unsigned long long)g_ep_ino);
    } else {
        srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv < 0) { perror("socket"); return 1; }

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
    }
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
            /* accept 的错误几乎都只影响这一个连接，不该拖垮整个 daemon。
             * Linux 会把新连接上待处理的网络错误从 accept 抛出来，man 手册
             * 明确要求把它们当 EAGAIN 一样重试；EMSGSIZE 实测也出现过
             * （容器跨 netns 连宿主 TCP 时），当时 daemon 直接退出，
             * 所有会话一起断。只有 fd 耗尽这类真正的进程级故障才值得退出。 */
            switch (errno) {
            case EINTR:
            case ECONNABORTED:
            case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
            case EMSGSIZE:
            case EPROTO:
            case ENOPROTOOPT:
            case EHOSTDOWN:
            case EHOSTUNREACH:
            case ENETDOWN:
            case ENETUNREACH:
            case ENONET:
            case EOPNOTSUPP:
            case ETIMEDOUT:
                dlog(1, "accept 跳过一个连接: %s", strerror(errno));
                continue;
            default:
                /* EMFILE/ENFILE/ENOBUFS/ENOMEM 也可能是暂时的，但反复重试
                 * 会变忙等，交给上层重启更干净。 */
                perror("accept");
                goto accept_loop_done;
            }
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

        /* TCP_NODELAY 在 AF_UNIX 上返回 EOPNOTSUPP，Unix 模式下没必要调。 */
        if (!sock_path)
            setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        /* 回传缓冲必须显式放大，否则 AF_UNIX 下吞吐塌到无法实时解码。
         *
         * 实测（nabu，720p30 HEVC 5s 码流，同一二进制、同一硬件）：
         *   TCP  153 NALU → 150 帧  8.6x
         *   UNIX  52 NALU →  49 帧  0.92x，会话因"输入缓冲暂满"中断
         *
         * 根因是**默认缓冲容量差一个数量级**，而单帧远大于它：
         *   AF_UNIX  SO_SNDBUF/SO_RCVBUF = 229376 (224KB)
         *   AF_INET  SO_SNDBUF = 524288 / SO_RCVBUF = 1048576
         *   一帧 NV12：720p = 1.38MB，1080p = 3.11MB
         *
         * 于是每帧回传都要把 224KB 缓冲反复填满、阻塞等消费者取走，往返
         * 次数是 TCP 的数倍；output 线程被 send_all 堵住 → MediaCodec 输出
         * 帧不回收 → 输入槽位耗尽 → 报"输入缓冲暂满"，重试 12 次后放弃。
         *
         * 取 4MB：整帧装得下 1080p NV12 并留余量。内核会把 SO_SNDBUF 翻倍
         * 记账，且受 net.core.{w,r}mem_max 截断，设不到就退化为原状，
         * 因此失败不致命，忽略返回值。 */
        {
            int bufsz = 4 * 1024 * 1024;
            (void)setsockopt(cli, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
            (void)setsockopt(cli, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
        }

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
        s->vcl_in   = 1;      /* PTS 标签从 1 起：0 留作"无 PTS 信息"哨兵 */
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
accept_loop_done:

    close(srv);
    /* Unix socket 要删掉文件，否则下次启动 bind 会拿到 EADDRINUSE。
     * （启动时也会删一次残留，双保险 —— 被信号杀死时走不到这里。） */
    if (sock_path)
        unlink(sock_path);

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
