/*
 * probe_av1_feed —— 把 AV1 OBU temporal unit 直送 /dev/video32，绕过 VA-API 驱动
 *
 * 用途：判定"AV1 出帧不对"是驱动的码流合成有问题，还是硬件/内核侧的问题。
 * 这是第 68 轮定位 AV1 缺陷的关键手段 —— 当年用的是 tools/av1_probe.py
 * （走 daemon 线协议）。daemon 在 0.4.0 已删除，故重写为 V4L2 版。
 *
 * 判据：
 *   直送源码流能吐满帧   → 硬件没问题，缺陷在驱动的 OBU 合成
 *   直送源码流也吐不满   → 硬件/内核侧的问题，与合成无关
 *
 * 历史结论（第 68 轮，用 daemon 版探针测得，判据同上）：
 *   源码流 av1_1080p.obu 直送：送 150 单元、收 **150** 帧
 *   驱动合成流：           送 150 单元、收  **80** 帧
 * 两条流的 OBU 构成差异只有一项：源码流含 70 个 show_existing_frame
 * FRAME_HEADER，合成流一个都没有。所以硬件会为每个 SEF 头复显一次，
 * 缺的正是这 70 个头 —— 这条结论当年推翻了"SEF 复显由 ffmpeg 自行完成"。
 *
 * 用法：
 *   cc -O2 -o probe_av1_feed probe_av1_feed.c
 *   ./probe_av1_feed <file.obu> [单元数上限]
 *
 * ⚠️ burst 语义：一次性灌入全部单元再统一收帧。AV1 解码器有 output delay，
 * 逐单元等帧会死锁（它要收满 N 个输入才吐第一帧，而我们在等第一帧）。
 * 这个坑当年在 daemon 版上踩过，这里沿用 burst。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <linux/videodev2.h>

#define OBU_TEMPORAL_DELIMITER 2

static unsigned char *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n);
    if (!b || fread(b, 1, *n, f) != *n) { free(b); fclose(f); return NULL; }
    fclose(f);
    return b;
}

/* 读 leb128，返回值，*len 写入占用字节数。 */
static uint64_t leb128(const unsigned char *d, size_t cap, size_t *len)
{
    uint64_t v = 0; size_t i = 0;
    for (; i < 8 && i < cap; i++) {
        v |= (uint64_t)(d[i] & 0x7f) << (i * 7);
        if (!(d[i] & 0x80)) { i++; break; }
    }
    *len = i;
    return v;
}

/* 从 pos 起找下一个 temporal delimiter 的偏移（即下一个 TU 的开始）。 */
static size_t next_td(const unsigned char *d, size_t n, size_t pos)
{
    size_t p = pos;
    while (p < n) {
        unsigned type = (d[p] >> 3) & 0x0f;
        int has_size = (d[p] >> 1) & 1;
        size_t hdr = 1;
        if ((d[p] >> 2) & 1) hdr++;            /* extension */
        if (!has_size) return n;               /* 无 size 字段，无法继续 */
        size_t szlen = 0;
        uint64_t sz = leb128(d + p + hdr, n - p - hdr, &szlen);
        size_t total = hdr + szlen + (size_t)sz;
        if (p != pos && type == OBU_TEMPORAL_DELIMITER) return p;
        if (total == 0 || p + total > n) return n;
        p += total;
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <file.obu> [单元数上限]\n", argv[0]);
        return 2;
    }
    const int cap_units = (argc > 2) ? atoi(argv[2]) : 0;

    size_t flen = 0;
    unsigned char *data = slurp(argv[1], &flen);
    if (!data) { fprintf(stderr, "读不到 %s\n", argv[1]); return 1; }
    printf("  输入 %s，%zu 字节\n", argv[1], flen);

    int fd = open("/dev/video32", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("  open /dev/video32"); free(data); return 1; }

    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.width = 1920;
    f.fmt.pix_mp.height = 1080;
    f.fmt.pix_mp.pixelformat = v4l2_fourcc('A', 'V', '1', '0');
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = 8 << 20;
    if (ioctl(fd, VIDIOC_S_FMT, &f) < 0) {
        printf("  S_FMT(AV10) 失败: %s\n", strerror(errno));
        printf("  → 该设备的 msm_vidc 可能不支持 AV1；用 v4l2_enum_fmt 确认\n");
        close(fd); free(data); return 1;
    }
    const size_t isz = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    printf("  S_FMT(OUTPUT/AV10) ok，sizeimage=%zu\n", isz);

    struct v4l2_requestbuffers rb;
    const unsigned mems[] = { V4L2_MEMORY_DMABUF, V4L2_MEMORY_USERPTR,
                              V4L2_MEMORY_MMAP };
    const char *mnm[] = { "DMABUF", "USERPTR", "MMAP" };
    unsigned mem = 0;
    for (int i = 0; i < 3; i++) {
        memset(&rb, 0, sizeof(rb));
        rb.count = 8; rb.type = f.type; rb.memory = mems[i];
        if (ioctl(fd, VIDIOC_REQBUFS, &rb) == 0) {
            mem = mems[i];
            printf("  REQBUFS(%s) ok，count=%u\n", mnm[i], rb.count);
            break;
        }
    }
    if (!mem) {
        printf("  三种内存类型的 REQBUFS 全失败: %s\n", strerror(errno));
        close(fd); free(data); return 1;
    }
    if (mem != V4L2_MEMORY_USERPTR) {
        printf("  ⚠️ 本探针只实现了 USERPTR 喂料；当前设备给的是 %s\n",
               mem == V4L2_MEMORY_DMABUF ? "DMABUF" : "MMAP");
        printf("  → 需要 DMABUF 时请参考 v4l2_backend.c 的 dmabuf_alloc\n");
        close(fd); free(data); return 1;
    }

    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);

    unsigned t = f.type;
    if (ioctl(fd, VIDIOC_STREAMON, &t) < 0) {
        printf("  STREAMON(OUTPUT) 失败: %s\n", strerror(errno));
        close(fd); free(data); return 1;
    }

    /* 切分并 burst 灌入。 */
    void *bufs[8];
    for (int i = 0; i < 8; i++) bufs[i] = aligned_alloc(4096, isz);

    size_t pos = 0, sent = 0;
    int got_sc = 0;
    while (pos < flen) {
        if (cap_units && (int)sent >= cap_units) break;
        size_t end = next_td(data, flen, pos);
        size_t len = end - pos;
        if (!len || len > isz) break;

        int idx = (int)(sent % 8);
        memcpy(bufs[idx], data + pos, len);

        struct v4l2_buffer b; struct v4l2_plane pl[1];
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = f.type; b.memory = V4L2_MEMORY_USERPTR; b.index = idx;
        b.m.planes = pl; b.length = 1;
        b.timestamp.tv_usec = (long)(sent + 1);
        pl[0].m.userptr = (unsigned long)bufs[idx];
        pl[0].length = isz; pl[0].bytesused = len;

        if (ioctl(fd, VIDIOC_QBUF, &b) < 0) {
            /* 队列满是正常的，回收一个再试。 */
            struct pollfd pf = { .fd = fd, .events = POLLOUT | POLLPRI };
            poll(&pf, 1, 500);
            if (pf.revents & POLLPRI) {
                struct v4l2_event ev; memset(&ev, 0, sizeof(ev));
                if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 &&
                    ev.type == V4L2_EVENT_SOURCE_CHANGE && !got_sc) {
                    got_sc = 1;
                    printf("  ✓ SOURCE_CHANGE 在第 %zu 个单元后到达\n", sent);
                }
            }
            struct v4l2_buffer d; struct v4l2_plane dp[1];
            memset(&d, 0, sizeof(d)); memset(dp, 0, sizeof(dp));
            d.type = f.type; d.memory = V4L2_MEMORY_USERPTR;
            d.m.planes = dp; d.length = 1;
            if (ioctl(fd, VIDIOC_DQBUF, &d) < 0) {
                printf("  第 %zu 个单元 QBUF 失败且无缓冲可回收: %s\n",
                       sent, strerror(errno));
                break;
            }
            if (ioctl(fd, VIDIOC_QBUF, &b) < 0) {
                printf("  第 %zu 个单元重试 QBUF 仍失败: %s\n",
                       sent, strerror(errno));
                break;
            }
        }
        sent++;
        pos = end;
    }
    printf("  送入 %zu 个 temporal unit\n", sent);

    if (!got_sc) {
        struct pollfd pf = { .fd = fd, .events = POLLPRI };
        if (poll(&pf, 1, 2000) > 0 && (pf.revents & POLLPRI)) {
            struct v4l2_event ev; memset(&ev, 0, sizeof(ev));
            if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
                got_sc = 1;
                printf("  ✓ SOURCE_CHANGE 在灌完后到达\n");
            }
        }
    }
    if (!got_sc) {
        printf("  ✗ 始终没有 SOURCE_CHANGE —— 固件没解析码流\n");
        printf("  → 这与码流内容无关，是设备的解码会话起不来；\n");
        printf("    先跑 probe_device_support 确认该设备是否可用\n");
        close(fd); free(data); return 1;
    }

    /* CAPTURE 侧协商并收帧。 */
    struct v4l2_format cf;
    memset(&cf, 0, sizeof(cf));
    cf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd, VIDIOC_G_FMT, &cf);
    printf("  G_FMT(CAPTURE): %ux%u fourcc=%.4s\n",
           cf.fmt.pix_mp.width, cf.fmt.pix_mp.height,
           (char *)&cf.fmt.pix_mp.pixelformat);
    cf.fmt.pix_mp.pixelformat = v4l2_fourcc('N', 'V', '1', '2');
    ioctl(fd, VIDIOC_S_FMT, &cf);
    const size_t csz = cf.fmt.pix_mp.plane_fmt[0].sizeimage;

    struct v4l2_requestbuffers crb;
    memset(&crb, 0, sizeof(crb));
    crb.count = 12; crb.type = cf.type; crb.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(fd, VIDIOC_REQBUFS, &crb) < 0) {
        printf("  REQBUFS(CAPTURE) 失败: %s\n", strerror(errno));
        close(fd); free(data); return 1;
    }
    void *cb[12];
    for (unsigned i = 0; i < crb.count && i < 12; i++) {
        cb[i] = aligned_alloc(4096, csz);
        struct v4l2_buffer b; struct v4l2_plane pl[1];
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = cf.type; b.memory = V4L2_MEMORY_USERPTR; b.index = i;
        b.m.planes = pl; b.length = 1;
        pl[0].m.userptr = (unsigned long)cb[i]; pl[0].length = csz;
        ioctl(fd, VIDIOC_QBUF, &b);
    }
    unsigned ct = cf.type;
    ioctl(fd, VIDIOC_STREAMON, &ct);

    size_t frames = 0;
    for (;;) {
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        if (poll(&pf, 1, 3000) <= 0) break;
        if (!(pf.revents & POLLIN)) break;
        struct v4l2_buffer d; struct v4l2_plane dp[1];
        memset(&d, 0, sizeof(d)); memset(dp, 0, sizeof(dp));
        d.type = cf.type; d.memory = V4L2_MEMORY_USERPTR;
        d.m.planes = dp; d.length = 1;
        if (ioctl(fd, VIDIOC_DQBUF, &d) < 0) break;
        frames++;
        ioctl(fd, VIDIOC_QBUF, &d);          /* 立刻还回去继续收 */
    }

    printf("\n  ===== 结果 =====\n");
    printf("  送入 %zu 单元，收到 %zu 帧\n", sent, frames);
    if (frames >= sent)
        printf("  → 硬件能吐满：AV1 缺陷在驱动的 OBU 合成侧\n");
    else
        printf("  → 硬件只吐 %zu/%zu：先排查硬件/内核侧，再看合成\n",
               frames, sent);

    close(fd); free(data);
    return 0;
}
