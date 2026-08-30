/* slowfeed —— 绕过 VA-API 直打 /dev/video32，量化 msm_vidc 的交付特性。
 *
 * 为什么需要它：浏览器侧的丢帧率在本设备上无法用于判断驱动改动 ——
 * 同一 profile、同一驱动、同一素材的自我对照中位 3.73% vs 4.10%，
 * 范围却分别横跨 2.17-22.82% 与 3.16-29.24%，同配置跨批次中位差 5 倍。
 * 噪声来自共享内核的 Android 侧，容器内看不见也控制不了。
 * 本程序不经过浏览器与合成器，实测五轮 23.7fps 完全一致，可作判据。
 *
 * 最初写它是为了验证一个假设："OUTPUT 在驱动 0/8" 说明送进去的单元
 * 没被驱动接住。那个假设是错的 —— dmd_v4l2_recv 在 POLLOUT 时会
 * reap_output()，日志是回收后的快照，0/8 属正常。留此记录避免重走。
 *
 * 送料节奏（第二个参数）：
 *   fast     连续送料，测吞吐上限（模拟 ffmpeg）
 *   slow     送一帧等一帧，测首帧滞后（模拟 Firefox 的逐帧 sync）
 *   depthN   维持 N 个单元在飞，扫描"在飞深度 → 吞吐"关系
 *
 * 用它得到的关键结论：
 *
 * 1) 固件首帧滞后 = 4 个输入单元，且与 B 帧无关
 *    有 B 帧、无 B 帧的码流都是送满 5 个单元才收到第 1 帧。
 *    显示序（OUTPUT_ORDER=0）下滞后变成 7。LOWLATENCY 开关对此无影响。
 *
 * 2) 在飞深度存在断崖，阈值恰好等于滞后值
 *      depth 1-3: 23.5 fps, 慢帧(>33ms) 22.7%
 *      depth 4:   45.8 fps, 慢帧  0.0%     ← 只多 1 帧
 *      depth 5:   58.9 fps, 慢帧  0.0%
 *    Firefox 恒定在 depth 2-3（18 个 surface 也不改变），永远落在断崖下沿。
 *
 * 3) POLLIN 会漏报（据此修好了 v4l2_backend.c 的取帧路径）
 *    帧已在 CAPTURE 队列里而 poll 不置位，空转到超时才返回，
 *    帧间隔被拖成超时值本身：超时 100ms → 间隔 111ms，超时 20ms → 41ms。
 *    改成"先非阻塞 DQBUF、取不到才 poll"后慢帧 15.3% → 3.4%。
 *
 * 4) 驱动交付节奏是健康的（微秒级直方图，depth3）
 *      间隔中位 31984us（理想 33333us，快 4.0%）  p90 34249us
 *      累积落后 1.41 帧时长 / 197 帧
 *    跟得上 30fps，长期不累积落后。
 *
 * 编译: gcc -O2 -o slowfeed slowfeed.c
 * 用法: ./slowfeed <文件> <fast|slow|depthN> [帧数] [order] [hevc] [lowlat] [pad]
 *   order  0=显示序 1=解码序（默认 0）
 *   hevc   1=HEVC 码流（默认 0=H.264）
 *   ⚠️ 按单个 NAL 送料，HEVC 一帧含多个 NAL，HEVC 结果仅供参考
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define DEV "/dev/video32"
#define N_OUT 8
#define N_CAP 24
#define ION_DEV "/dev/ion"

/* --- 私有协议常量（见 research/FINDING-msm-vidc-protocol.md） --- */
#define CID_BASE            0x00992000
#define CID_OUTPUT_ORDER    (CID_BASE + 3)
#define CID_EXTRADATA       (CID_BASE + 0x11)
#define CID_OUTPUT_MODE     (CID_BASE + 22)   /* SECONDARY = 1 */
#define CID_LOWLATENCY      (CID_BASE + 0x38)
#define EV_MSM_VIDC_START   (V4L2_EVENT_PRIVATE_START + 0x00001000)
#define CMD_SESSION_CONTINUE 5

struct ion_allocation_data {
    uint64_t len; uint32_t heap_id_mask; uint32_t flags; uint32_t fd; uint32_t unused;
};
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)

struct buf { int fd; unsigned len; void *map; int queued;
             int extra_fd; unsigned extra_len; };

static int ionfd = -1;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int ion_alloc(size_t len)
{
    struct ion_allocation_data a;
    memset(&a, 0, sizeof(a));
    a.len = len;
    a.heap_id_mask = 1u << 25;          /* system heap id=25 */
    if (ioctl(ionfd, ION_IOC_ALLOC, &a) < 0) return -1;
    return (int)a.fd;
}

static int set_ctrl(int fd, unsigned id, int val)
{
    struct v4l2_control c = { .id = id, .value = val };
    return ioctl(fd, VIDIOC_S_CTRL, &c);
}

static int sub_event(int fd, unsigned type)
{
    struct v4l2_event_subscription s;
    memset(&s, 0, sizeof(s));
    s.type = type;
    return ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &s);
}

static int qbuf(int fd, enum v4l2_buf_type type, int idx, struct buf *b,
                unsigned bytesused, uint64_t pts_us)
{
    struct v4l2_buffer v;
    struct v4l2_plane p[2];
    memset(&v, 0, sizeof(v)); memset(p, 0, sizeof(p));
    v.type = type; v.memory = V4L2_MEMORY_USERPTR;
    v.index = (unsigned)idx; v.m.planes = p;
    /* 设了 CID_EXTRADATA 后 CAPTURE 是 2 平面，平面数不符直接 EINVAL
     * （valid_v4l2_buffer 要求 b->length == num_planes，msm_vidc.c:452-461）*/
    v.length = (b->extra_fd >= 0) ? 2 : 1;
    p[0].reserved[0] = (unsigned)b->fd;   /* fd 走 reserved[0] */
    p[0].length = b->len;
    p[0].bytesused = bytesused;
    if (b->extra_fd >= 0) {
        p[1].reserved[0] = (unsigned)b->extra_fd;
        p[1].length = b->extra_len;
    }
    if (pts_us) {
        v.timestamp.tv_sec = (long)(pts_us / 1000000ULL);
        v.timestamp.tv_usec = (long)(pts_us % 1000000ULL);
    }
    if (ioctl(fd, VIDIOC_QBUF, &v) < 0) return -1;
    b->queued = 1;
    return 0;
}

/* 非阻塞回收 OUTPUT，返回回收个数 */
static int reap_out(int fd, struct buf *out)
{
    int n = 0;
    for (;;) {
        struct v4l2_buffer v; struct v4l2_plane p[1];
        memset(&v, 0, sizeof(v)); memset(p, 0, sizeof(p));
        v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        v.memory = V4L2_MEMORY_USERPTR; v.m.planes = p; v.length = 1;
        if (ioctl(fd, VIDIOC_DQBUF, &v) < 0) break;
        if (v.index < N_OUT) out[v.index].queued = 0;
        n++;
    }
    return n;
}

static int count_queued(struct buf *b, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) if (b[i].queued) c++;
    return c;
}

/* 取一帧 CAPTURE，返回 1 有帧 / 0 无 / -1 错误；出参 index */
static int deq_cap(int fd, struct buf *cap, int *index, uint64_t *pts)
{
    struct v4l2_buffer v; struct v4l2_plane p[2];
    memset(&v, 0, sizeof(v)); memset(p, 0, sizeof(p));
    v.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    v.memory = V4L2_MEMORY_USERPTR; v.m.planes = p;
    v.length = (cap[0].extra_fd >= 0) ? 2 : 1;
    if (ioctl(fd, VIDIOC_DQBUF, &v) < 0) {
        if (errno == EAGAIN) return 0;
        return -1;
    }
    if (v.index < N_CAP) cap[v.index].queued = 0;
    *index = (int)v.index;
    *pts = (uint64_t)v.timestamp.tv_sec * 1000000ULL + v.timestamp.tv_usec;
    return 1;
}

/* 找下一个 AnnexB NAL 起始码 */
static size_t next_start(const uint8_t *d, size_t n, size_t from)
{
    for (size_t i = from; i + 3 < n; i++)
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) return i;
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <file.h264> <fast|slow> [帧数]\n", argv[0]);
        return 2;
    }
    int slow = strcmp(argv[2], "slow") == 0;
    /* depthN：维持 N 个单元在飞。N=1 等价 slow，N 大等价 fast。
     * 这直接量化"在飞帧数 -> 吞吐"的关系，用来判断浏览器需要多深的队列。*/
    int depth = 0;
    if (strncmp(argv[2], "depth", 5) == 0) depth = atoi(argv[2] + 5);
    int want = (argc > 3) ? atoi(argv[3]) : 40;
    /* OUTPUT_ORDER: 0=显示序(默认) 1=解码序。解码序不等重排，滞后应更小。 */
    int order = (argc > 4) ? atoi(argv[4]) : 0;
    int is_hevc = (argc > 5) ? atoi(argv[5]) : 0;
    int lowlat  = (argc > 6) ? atoi(argv[6]) : 1;
    /* pad>0: 在飞不足 pad 时补送一个"填充"单元推动流水线。
     * 用码流开头的 SPS（非画面单元）做填充，验证固件是否接受、
     * 且不产生额外输出帧。 */
    int pad     = (argc > 7) ? atoi(argv[7]) : 0;

    /* 读码流 */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open stream"); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *bs = malloc(fsz);
    if (fread(bs, 1, fsz, f) != (size_t)fsz) { perror("read"); return 1; }
    fclose(f);

    ionfd = open(ION_DEV, O_RDONLY | O_CLOEXEC);
    if (ionfd < 0) { perror("open /dev/ion"); return 1; }

    int fd = open(DEV, O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open " DEV); return 1; }

    /* --- 协议序列 --- */
    struct v4l2_format fmt;
    /* 1) dummy S_FMT(OUTPUT) 于不同分辨率 */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = 640; fmt.fmt.pix_mp.height = 480;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix_mp.num_planes = 1;
    ioctl(fd, VIDIOC_S_FMT, &fmt);

    /* 2) 目标分辨率 */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = 1920; fmt.fmt.pix_mp.height = 1088;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT OUTPUT"); return 1; }
    unsigned in_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    printf("S_FMT(OUTPUT) sizeimage=%u\n", in_size);

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = 30;
    ioctl(fd, VIDIOC_S_PARM, &parm);

    set_ctrl(fd, CID_OUTPUT_ORDER, order);
    printf("OUTPUT_ORDER=%d (%s)\n", order,
           order ? "解码序" : "显示序");
    set_ctrl(fd, CID_EXTRADATA, 2);
    set_ctrl(fd, CID_LOWLATENCY, lowlat);
    printf("LOWLATENCY=%d\n", lowlat);
    if (set_ctrl(fd, CID_OUTPUT_MODE, 1) < 0) {   /* SECONDARY 必需 */
        perror("S_CTRL OUTPUT_MODE=SECONDARY"); return 1;
    }
    for (unsigned i = 0; i <= 7; i++) sub_event(fd, EV_MSM_VIDC_START + i);

    /* CAPTURE 格式 */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = 1920; fmt.fmt.pix_mp.height = 1088;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.num_planes = 1;
    ioctl(fd, VIDIOC_S_FMT, &fmt);
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd, VIDIOC_G_FMT, &fmt);
    unsigned cap_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    printf("G_FMT(CAPTURE) %ux%u sizeimage=%u\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, cap_size);

    /* 缓冲 */
    static struct buf out[N_OUT], cap[N_CAP];
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = N_OUT; rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    rb.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) { perror("REQBUFS OUT"); return 1; }
    int n_out = (int)rb.count;
    for (int i = 0; i < n_out; i++) {
        out[i].fd = ion_alloc(in_size); out[i].len = in_size;
        out[i].extra_fd = -1;
        if (out[i].fd < 0) { perror("ion out"); return 1; }
        out[i].map = mmap(NULL, in_size, PROT_READ|PROT_WRITE, MAP_SHARED,
                          out[i].fd, 0);
        if (out[i].map == MAP_FAILED) { perror("mmap out"); return 1; }
    }
    memset(&rb, 0, sizeof(rb));
    rb.count = N_CAP; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    rb.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) { perror("REQBUFS CAP"); return 1; }
    int n_cap = (int)rb.count;
    unsigned n_planes = fmt.fmt.pix_mp.num_planes;
    unsigned extra_sz = (n_planes > 1)
                      ? fmt.fmt.pix_mp.plane_fmt[1].sizeimage : 0;
    if (n_planes > 1 && !extra_sz) extra_sz = 16384;
    printf("CAPTURE num_planes=%u extradata=%u\n", n_planes, extra_sz);
    for (int i = 0; i < n_cap; i++) {
        cap[i].fd = ion_alloc(cap_size); cap[i].len = cap_size;
        if (cap[i].fd < 0) { perror("ion cap"); return 1; }
        if (n_planes > 1) {
            cap[i].extra_fd = ion_alloc(extra_sz);
            if (cap[i].extra_fd < 0) { perror("ion extra"); return 1; }
            cap[i].extra_len = extra_sz;
        } else {
            cap[i].extra_fd = -1;
        }
    }
    printf("缓冲: OUTPUT %d x %u, CAPTURE %d x %u\n",
           n_out, in_size, n_cap, cap_size);

    for (int i = 0; i < n_cap; i++)
        if (qbuf(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i, &cap[i], 0, 0) < 0) {
            perror("QBUF CAP"); return 1;
        }

    int on = 1;
    enum v4l2_buf_type ct = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    enum v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMON, &ct) < 0) { perror("STREAMON CAP"); return 1; }
    if (ioctl(fd, VIDIOC_STREAMON, &ot) < 0) { perror("STREAMON OUT"); return 1; }
    (void)on;
    printf("STREAMON 双侧 OK，模式=%s\n", slow ? "slow(逐帧等)" : "fast(连送)");

    /* --- 送料循环 --- */
    size_t pos = next_start(bs, fsz, 0);
    int sent = 0, got = 0, zero_out_events = 0;
    int64_t t0 = now_ms();
    int64_t last_frame_ms = 0;      /* 上一帧到达时刻，用于算间隔 */
    int slow_frames = 0;            /* 间隔 > 33ms 的帧数 */
    /* 间隔直方图（us 精度）：浏览器侧丢帧率被系统噪声压倒（同配置实测
     * 2%-29%），无法用来判断改动效果。而驱动交付节奏高度稳定
     * （depth3 五轮 23.7fps 完全一致），所以改用它作判据。 */
    static long gap_hist[4096]; int gap_n = 0;

    while (sent < want && pos < (size_t)fsz) {
        /* 取一个 NAL（到下一个起始码） */
        size_t end = next_start(bs, fsz, pos + 3);
        size_t len = end - pos;
        if (len > in_size) { pos = end; continue; }

        reap_out(fd, out);
        int idx = -1;
        for (int i = 0; i < n_out; i++) if (!out[i].queued) { idx = i; break; }
        if (idx < 0) {
            /* 没空闲缓冲：等一下再试 */
            struct pollfd pfd = { .fd = fd, .events = POLLOUT | POLLIN };
            poll(&pfd, 1, 50);
            continue;
        }

        memcpy(out[idx].map, bs + pos, len);
        if (qbuf(fd, ot, idx, &out[idx], (unsigned)len,
                 (uint64_t)(sent + 1) * 1000) < 0) {
            perror("QBUF OUT"); break;
        }
        sent++;
        pos = end;

        /* 关键观测：QBUF 之后驱动手里有几个 OUTPUT */
        int q = count_queued(out, n_out);
        if (q == 0) zero_out_events++;
        if (sent <= 12 || q == 0)
            printf("[送 %2d] OUTPUT 在驱动 %d/%d  已收 %d%s\n",
                   sent, q, n_out, got, q == 0 ? "   ← 掉到 0!" : "");

        /* 补送填充单元：在飞不足 pad 时补一个 SPS（非画面单元）。
         *
         * 目的是验证一个设想：Firefox 只让 2-3 帧在飞，而固件滞后 4 帧，
         * 差的 1 帧能否用"无害的非画面单元"补上，从而推动流水线。
         * 关键待验证：固件接受吗？会不会多吐一帧（那会破坏 unit_seq 配对）？*/
        if (pad > 0 && sent - got < pad) {
            size_t sp = next_start(bs, fsz, 0);
            size_t se = next_start(bs, fsz, sp + 4);
            if (se > sp && se - sp < 256) {     /* SPS 很小，防越界 */
                reap_out(fd, out);
                int oi = -1;
                for (int k = 0; k < n_out; k++)
                    if (!out[k].queued) { oi = k; break; }
                if (oi >= 0) {
                    memcpy(out[oi].map, bs + sp, se - sp);
                    /* PTS 用 0 标记填充单元，便于识别它是否产生输出帧 */
                    if (qbuf(fd, ot, oi, &out[oi], se - sp, 0) == 0)
                        printf("    [补送] %zu 字节 SPS, 在飞 %d -> %d\n",
                               se - sp, sent - got, sent - got + 1);
                }
            }
        }

        if (depth > 0) {
            /* 维持 depth 个在飞：未达深度就继续送，达到了才收一帧。
             * 事件仍需及时处理，否则 PORT_SETTINGS 不发 SESSION_CONTINUE。 */
            struct pollfd pf = { .fd = fd, .events = POLLPRI };
            if (poll(&pf, 1, 0) > 0 && (pf.revents & POLLPRI)) {
                struct v4l2_event ev;
                memset(&ev, 0, sizeof(ev));
                if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 &&
                    (ev.type == EV_MSM_VIDC_START + 2 ||
                     ev.type == EV_MSM_VIDC_START + 3)) {
                    struct v4l2_decoder_cmd dc;
                    memset(&dc, 0, sizeof(dc));
                    dc.cmd = CMD_SESSION_CONTINUE;
                    ioctl(fd, VIDIOC_DECODER_CMD, &dc);
                }
            }
            while (sent - got >= depth) {
                /* 先直接试一次非阻塞 DQBUF：若帧已就绪就不必进 poll。
                 * 实测帧 250 后 POLLIN 常不及时置位，poll 空转到超时，
                 * 帧间隔被拖成 poll 超时值（100ms 时 111ms、20ms 时 41ms）。*/
                int ci0 = -1; uint64_t pts0 = 0;
                if (deq_cap(fd, cap, &ci0, &pts0) == 1) {
                    got++;
                    int64_t nm = now_ms();
                    if (last_frame_ms) {
                        int64_t g = nm - last_frame_ms;
                        if (g > 33) { slow_frames++;
                          if (slow_frames <= 12)
                            printf("    [帧 %d] 间隔 %lld ms (直取)\n", got, (long long)g); }
                    }
                    last_frame_ms = nm;
                    qbuf(fd, ct, ci0, &cap[ci0], 0, 0);
                    continue;
                }
                struct pollfd p2 = { .fd = fd, .events = POLLIN };
                poll(&p2, 1, 20);
                int ci = -1; uint64_t pts = 0;
                if (deq_cap(fd, cap, &ci, &pts) == 1) {
                    got++;
                    int64_t nowm = now_ms();
                    {   struct timespec _t; clock_gettime(CLOCK_MONOTONIC,&_t);
                        static long long _prev_us = 0;
                        long long _us = _t.tv_sec*1000000LL + _t.tv_nsec/1000;
                        if (_prev_us && gap_n < 4096) gap_hist[gap_n++] = (long)(_us-_prev_us);
                        _prev_us = _us; }
                    if (last_frame_ms) {
                        int64_t gap = nowm - last_frame_ms;
                        if (gap > 33) {
                            slow_frames++;
                            if (slow_frames <= 12)
                                printf("    [帧 %d] 间隔 %lld ms (>33)\n",
                                       got, (long long)gap);
                        }
                    }
                    last_frame_ms = nowm;
                    qbuf(fd, ct, ci, &cap[ci], 0, 0);
                } else break;
            }
        } else if (slow) {
            /* 模拟 Firefox：送一帧就等一帧，最多等 200ms */
            int64_t deadline = now_ms() + 200;
            for (;;) {
                struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLPRI };
                int pr = poll(&pfd, 1, 20);
                if (pr > 0 && (pfd.revents & POLLPRI)) {
                    struct v4l2_event ev;
                    memset(&ev, 0, sizeof(ev));
                    if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
                        if (ev.type == EV_MSM_VIDC_START + 2 ||
                            ev.type == EV_MSM_VIDC_START + 3) {
                            struct v4l2_decoder_cmd dc;
                            memset(&dc, 0, sizeof(dc));
                            dc.cmd = CMD_SESSION_CONTINUE;
                            ioctl(fd, VIDIOC_DECODER_CMD, &dc);
                            printf("    PORT_SETTINGS -> SESSION_CONTINUE\n");
                        }
                    }
                }
                int ci = -1; uint64_t pts = 0;
                int r = deq_cap(fd, cap, &ci, &pts);
                if (r == 1) {
                    got++;
                    qbuf(fd, ct, ci, &cap[ci], 0, 0);   /* 立刻还回去 */
                    break;
                }
                if (now_ms() > deadline) break;
            }
        } else {
            /* fast：只做事件处理，不等帧 */
            struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLPRI };
            if (poll(&pfd, 1, 0) > 0) {
                if (pfd.revents & POLLPRI) {
                    struct v4l2_event ev;
                    memset(&ev, 0, sizeof(ev));
                    if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 &&
                        (ev.type == EV_MSM_VIDC_START + 2 ||
                         ev.type == EV_MSM_VIDC_START + 3)) {
                        struct v4l2_decoder_cmd dc;
                        memset(&dc, 0, sizeof(dc));
                        dc.cmd = CMD_SESSION_CONTINUE;
                        ioctl(fd, VIDIOC_DECODER_CMD, &dc);
                        printf("    PORT_SETTINGS -> SESSION_CONTINUE\n");
                    }
                }
                int ci = -1; uint64_t pts = 0;
                while (deq_cap(fd, cap, &ci, &pts) == 1) {
                    got++;
                    qbuf(fd, ct, ci, &cap[ci], 0, 0);
                }
            }
        }
    }

    /* 收尾：再多收一会儿 */
    int64_t drain_until = now_ms() + 2000;
    while (now_ms() < drain_until && got < sent) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLPRI };
        poll(&pfd, 1, 50);
        int ci = -1; uint64_t pts = 0;
        int r = deq_cap(fd, cap, &ci, &pts);
        if (r == 1) { got++; qbuf(fd, ct, ci, &cap[ci], 0, 0); }
        reap_out(fd, out);
    }

    int64_t el = now_ms() - t0;
    printf("\n=== 结果（%s）===\n", slow ? "slow" : "fast");
    printf("送入 %d 单元, 收到 %d 帧, 耗时 %lld ms (%.1f fps)\n",
           sent, got, (long long)el, el > 0 ? got * 1000.0 / el : 0);
    printf("OUTPUT 掉到 0 的次数: %d\n", zero_out_events);
    printf("帧间隔 >33ms: %d / %d (%.1f%%)\n", slow_frames, got,
           got > 0 ? 100.0 * slow_frames / got : 0.0);
    if (gap_n > 10) {
        /* 冒泡太慢，用 qsort */
        int cmp(const void*a,const void*b){long x=*(long*)a,y=*(long*)b;return x<y?-1:x>y;}
        qsort(gap_hist, gap_n, sizeof(long), cmp);
        long sum=0; for(int i=0;i<gap_n;i++) sum+=gap_hist[i];
        printf("间隔(us): 中位 %ld  均值 %ld  p10 %ld  p90 %ld  最小 %ld\n",
               gap_hist[gap_n/2], sum/gap_n, gap_hist[gap_n/10],
               gap_hist[gap_n*9/10], gap_hist[0]);
        printf("  理想 33333us，中位差 %+ld us (%.1f%%)\n",
               gap_hist[gap_n/2]-33333, 100.0*(gap_hist[gap_n/2]-33333)/33333);
        printf("  p95 %ld  p99 %ld  最大 %ld us\n",
               gap_hist[gap_n*95/100], gap_hist[gap_n*99/100], gap_hist[gap_n-1]);
        /* 累积超时量：所有超过理想值的部分之和，等于总落后时间。
         * 这才是决定"浏览器判定落后并跳关键帧"的量。 */
        long over=0; int n_over=0;
        for(int i=0;i<gap_n;i++) if(gap_hist[i]>33333){over+=gap_hist[i]-33333;n_over++;}
        printf("  累积落后 %ld us (%.2f 帧时长), 来自 %d 帧\n",
               over, (double)over/33333, n_over);
    }

    ioctl(fd, VIDIOC_STREAMOFF, &ot);
    ioctl(fd, VIDIOC_STREAMOFF, &ct);
    close(fd);
    close(ionfd);
    return got > 0 ? 0 : 1;
}
