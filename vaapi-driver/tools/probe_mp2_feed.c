/*
 * probe_mp2_feed —— 把 MPEG-2 基本流按不同粒度直送 /dev/video32，绕过 VA-API 驱动
 *
 * 用途：定位 MPEG-2 "固件 SYS_ERROR" 是送料粒度问题还是固件能力缺失。
 * 背景见 profiles.c 的 MPEG-2 注释与 mpeg2_bitstream.c：
 * 合成码流已与 ffmpeg 原始流逐字节一致，但送入后固件在第 2 个单元报
 * SYS_ERROR、一帧不吐。本探针用**原始 ES**（不经合成）对拍不同送料方式，
 * 若某种粒度能出帧，就把驱动的送料改成同样的粒度。
 *
 * 内存模式：本机（内核 6.6 新 msm_vidc）REQBUFS 只接受 MMAP/DMABUF，
 * 探针用 MMAP（REQBUFS + QUERYBUF + mmap），OUTPUT/CAPTURE 都是。
 *
 * 模式（argv[2]）：
 *   pic      每个单元 = 一幅图像（GOP 头并入其后的图像单元），
 *            首单元前置文件开头的 sequence 头区（驱动当前行为）
 *   seqevery 同 pic，但每个图像单元都前置 sequence 头区
 *   gop      每个单元 = 一个 GOP（sequence 头区并入首单元）
 *   whole    整个文件作为一个单元（要求放得进 OUTPUT sizeimage）
 *
 * 用法：
 *   cc -O2 -o probe_mp2_feed probe_mp2_feed.c
 *   ./probe_mp2_feed <file.mpg> [pic|seqevery|gop|whole] [单元数上限]
 *   DMP_DUMP=1 时把前几帧 CAPTURE 内容落盘到 /tmp/dmdwork/mp2_cap_*.yuv
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
#include <sys/mman.h>
#include <poll.h>
#include <linux/videodev2.h>

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

/* 在 [pos,n) 内找下一个会改变送料边界的起始码（0x00 图像 / 0xB8 GOP）。 */
static size_t next_boundary(const unsigned char *d, size_t n, size_t pos)
{
    for (size_t p = pos; p + 3 <= n; p++)
        if (d[p] == 0 && d[p+1] == 0 && d[p+2] == 1 &&
            (d[p+3] == 0x00 || d[p+3] == 0xB8))
            return p;
    return n;
}

/* QBUF 一个 OUTPUT 缓冲（MMAP）。队列满则回收一个再试；失败返回 -1。 */
static int qbuf_out(int fd, unsigned type, int idx, size_t bytesused)
{
    struct v4l2_buffer b; struct v4l2_plane pl[1];
    memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
    b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = idx;
    b.m.planes = pl; b.length = 1;
    b.timestamp.tv_usec = (long)(bytesused & 0xFFFF);
    pl[0].bytesused = bytesused;
    if (ioctl(fd, VIDIOC_QBUF, &b) == 0) return 0;
    struct pollfd pf = { .fd = fd, .events = POLLOUT };
    poll(&pf, 1, 500);
    struct v4l2_buffer d; struct v4l2_plane dp[1];
    memset(&d, 0, sizeof(d)); memset(dp, 0, sizeof(dp));
    d.type = type; d.memory = V4L2_MEMORY_MMAP;
    d.m.planes = dp; d.length = 1;
    if (ioctl(fd, VIDIOC_DQBUF, &d) < 0) return -1;
    return ioctl(fd, VIDIOC_QBUF, &b);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <file.mpg> [pic|seqevery|gop|whole] [单元数上限]\n",
                argv[0]);
        return 2;
    }
    const char *mode = (argc > 2) ? argv[2] : "pic";
    const int cap_units = (argc > 3) ? atoi(argv[3]) : 0;
    const int whole = !strcmp(mode, "whole");
    const int seqevery = !strcmp(mode, "seqevery");

    size_t flen = 0;
    unsigned char *data = slurp(argv[1], &flen);
    if (!data) { fprintf(stderr, "读不到 %s\n", argv[1]); return 1; }

    size_t first_pic = next_boundary(data, flen, 0);
    printf("  输入 %s，%zu 字节，首个图像/GOP 边界在 %zu，模式 %s\n",
           argv[1], flen, first_pic, mode);

    int fd = open("/dev/video32", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("  open /dev/video32"); free(data); return 1; }

    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.width = 716;      /* matrixbench；内核会按宏块对齐改写 */
    f.fmt.pix_mp.height = 236;
    f.fmt.pix_mp.pixelformat = v4l2_fourcc('M', 'P', 'G', '2');
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = 16 << 20;
    if (ioctl(fd, VIDIOC_S_FMT, &f) < 0) {
        printf("  S_FMT(MPG2) 失败: %s\n", strerror(errno));
        close(fd); free(data); return 1;
    }
    const size_t isz = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    printf("  S_FMT(OUTPUT/MPG2) ok，sizeimage=%zu，核准确认 %ux%u\n",
           isz, f.fmt.pix_mp.width, f.fmt.pix_mp.height);

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = 8; rb.type = f.type; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) {
        printf("  REQBUFS(OUTPUT/MMAP) 失败: %s\n", strerror(errno));
        close(fd); free(data); return 1;
    }
    void *bufs[8];
    size_t bufsz[8];
    for (int i = 0; i < 8; i++) {
        struct v4l2_buffer b; struct v4l2_plane pl[1];
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = f.type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.m.planes = pl; b.length = 1;
        if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
            printf("  QUERYBUF(OUTPUT %d) 失败: %s\n", i, strerror(errno));
            close(fd); free(data); return 1;
        }
        bufsz[i] = pl[0].length;
        bufs[i] = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, pl[0].m.mem_offset);
        if (bufs[i] == MAP_FAILED) {
            printf("  mmap(OUTPUT %d) 失败: %s\n", i, strerror(errno));
            close(fd); free(data); return 1;
        }
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

    /* 构造单元列表：按图像/GOP 边界切。units[0] 若是纯 sequence 头区
     * （off=0、len=first_pic），按模式并入下一单元（pic/gop）或剔除
     * （seqevery，喂料时统一前置）。 */
    struct { size_t off, len; } units[8192];
    size_t nunits = 0;
    if (whole) {
        units[0].off = 0; units[0].len = flen; nunits = 1;
        if (flen > isz) {
            printf("  整文件 %zu 字节放不进 sizeimage %zu\n", flen, isz);
            close(fd); free(data); return 1;
        }
    } else {
        size_t pos = 0;
        while (pos < flen && nunits < 8192) {
            size_t end = next_boundary(data, flen, pos);
            if (end == pos) break;
            units[nunits].off = pos; units[nunits].len = end - pos;
            nunits++;
            pos = end;
        }
        if (nunits >= 1 && units[0].off == 0 && units[0].len == first_pic) {
            if (seqevery) {
                memmove(units, units + 1, (nunits - 1) * sizeof(units[0]));
                nunits--;
            } else if (nunits >= 2) {
                units[1].off = 0;
                units[1].len += units[0].len;
                memmove(units, units + 1, (nunits - 1) * sizeof(units[0]));
                nunits--;
            }
        }
    }
    printf("  切出 %zu 个单元（首单元 len=%zu）\n", nunits,
           nunits ? units[0].len : 0);

    size_t sent = 0;
    int got_sc = 0;
    for (size_t u = 0; u < nunits; u++) {
        if (cap_units && (int)sent >= cap_units) break;
        size_t off = units[u].off, len = units[u].len;
        int idx = (int)(sent % 8);

        if (seqevery && off >= first_pic && first_pic > 0) {
            /* 图像单元前置 seq 头区，借用当前轮转槽。 */
            if (first_pic + len > isz) { printf("  单元超 sizeimage\n"); break; }
            memcpy(bufs[idx], data, first_pic);
            memcpy((char*)bufs[idx] + first_pic, data + off, len);
            len += first_pic;
        } else {
            if (len > isz) { printf("  单元 %zu 长 %zu 超 sizeimage\n",
                                    u, len); break; }
            memcpy(bufs[idx], data + off, len);
        }
        if (qbuf_out(fd, f.type, idx, len) < 0) {
            printf("  第 %zu 单元 QBUF 失败: %s\n", sent, strerror(errno));
            break;
        }
        sent++;
        /* 每 4 个单元看一眼 SOURCE_CHANGE。 */
        struct pollfd pf = { .fd = fd, .events = POLLPRI };
        if (poll(&pf, 1, 0) > 0 && (pf.revents & POLLPRI)) {
            struct v4l2_event ev; memset(&ev, 0, sizeof(ev));
            if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 &&
                ev.type == V4L2_EVENT_SOURCE_CHANGE && !got_sc) {
                got_sc = 1;
                printf("  ✓ SOURCE_CHANGE 在第 %zu 单元后到达\n", sent);
            }
        }
    }
    printf("  送入 %zu 个单元\n", sent);

    if (!got_sc) {
        struct pollfd pf = { .fd = fd, .events = POLLPRI };
        if (poll(&pf, 1, 2000) > 0 && (pf.revents & POLLPRI)) {
            struct v4l2_event ev; memset(&ev, 0, sizeof(ev));
            if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 &&
                ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                got_sc = 1;
                printf("  ✓ SOURCE_CHANGE 在灌完后到达\n");
            }
        }
    }
    if (!got_sc)
        printf("  ✗ 始终没有 SOURCE_CHANGE —— 固件没解析出流参数（看 dmesg）\n");

    /* CAPTURE 侧协商并收帧。 */
    struct v4l2_format cf;
    memset(&cf, 0, sizeof(cf));
    cf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd, VIDIOC_G_FMT, &cf);
    printf("  G_FMT(CAPTURE): %ux%u fourcc=%.4s\n",
           cf.fmt.pix_mp.width, cf.fmt.pix_mp.height,
           (char *)&cf.fmt.pix_mp.pixelformat);
    if (!got_sc) { close(fd); free(data); return 1; }
    cf.fmt.pix_mp.pixelformat = v4l2_fourcc('N', 'V', '1', '2');
    if (ioctl(fd, VIDIOC_S_FMT, &cf) < 0) {
        printf("  S_FMT(CAPTURE/NV12) 失败: %s\n", strerror(errno));
        close(fd); free(data); return 1;
    }
    const size_t csz = cf.fmt.pix_mp.plane_fmt[0].sizeimage;

    struct v4l2_requestbuffers crb;
    memset(&crb, 0, sizeof(crb));
    crb.count = 16; crb.type = cf.type; crb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &crb) < 0) {
        printf("  REQBUFS(CAPTURE/MMAP) 失败: %s\n", strerror(errno));
        close(fd); free(data); return 1;
    }
    void *cb[16];
    for (unsigned i = 0; i < crb.count && i < 16; i++) {
        struct v4l2_buffer b; struct v4l2_plane pl[1];
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = cf.type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.m.planes = pl; b.length = 1;
        if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) break;
        cb[i] = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, pl[0].m.mem_offset);
        b.m.planes = pl; b.length = 1;
        pl[0].bytesused = 0;
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
        d.type = cf.type; d.memory = V4L2_MEMORY_MMAP;
        d.m.planes = dp; d.length = 1;
        if (ioctl(fd, VIDIOC_DQBUF, &d) < 0) break;
        frames++;
        if ((frames <= 2 || getenv("DMP_DUMP")) && frames <= 999) {
            char nm[64];
            snprintf(nm, sizeof(nm), "/tmp/dmdwork/mp2_cap_%03zu.yuv", frames);
            FILE *df = fopen(nm, "wb");
            if (df) { fwrite(cb[d.index], 1, csz, df); fclose(df); }
        }
        ioctl(fd, VIDIOC_QBUF, &d);
    }

    printf("\n  ===== 结果（模式 %s）=====\n", mode);
    printf("  送入 %zu 单元，收到 %zu 帧\n", sent, frames);
    close(fd); free(data);
    return 0;
}
