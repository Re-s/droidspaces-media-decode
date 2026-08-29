/*
 * v4l2_dec_probe —— 直接用 V4L2 M2M 接口解一段 AV1 码流。
 *
 * 用途：完全绕开 MediaCodec / Codec2 / vendor HAL，直接对
 * /dev/video32（msm_vidc_decoder）跑标准的 V4L2 stateful 解码流程。
 * 这是判定"AV1 解不出来是硬件/内核驱动的问题，还是上层 Codec2 的问题"
 * 的关键实验。
 *
 * 背景：SM8750 上硬件确实枚举出 AV10（V4L2 ENUM_FMT 实测），但
 * c2.qti.av1.decoder 在组件创建阶段就失败（QueryrequiredInfos from
 * downstream failed），dshmon 报 vidcHw=false。若本程序能解出帧，
 * 说明硬件与内核驱动可用、故障在 Codec2 层；若同样失败，则是更底层的问题。
 *
 * 用法（需 root）:
 *     v4l2_dec_probe <file.obu> [单元数]
 *
 * 交叉编译：本机若为 aarch64 可直接 cc -O2 -static。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef V4L2_PIX_FMT_AV1
#define V4L2_PIX_FMT_AV1 v4l2_fourcc('A', 'V', '1', '0')
#endif

/* dma-heap 的 ioctl 定义（linux/dma-heap.h 在 NDK/glibc 头里可能缺失，
 * 直接内联 —— ABI 是稳定的内核接口）。 */
struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0, struct dma_heap_allocation_data)

/* msm_vidc 只接受 DMABUF 内存模型：REQBUFS 虽然报 capabilities 里有 MMAP，
 * 但 QUERYBUF 给出的 offset 无法 mmap（返回 ENODEV）。实测必须走 dma-heap。 */
static int dmabuf_alloc(size_t len)
{
    static int heap_fd = -1;
    if (heap_fd < 0) {
        heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
        if (heap_fd < 0) {
            fprintf(stderr, "  打开 /dev/dma_heap/system 失败: %s\n", strerror(errno));
            return -1;
        }
    }
    struct dma_heap_allocation_data d;
    memset(&d, 0, sizeof(d));
    d.len = len;
    d.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &d) < 0) {
        fprintf(stderr, "  dma_heap 分配 %zu 字节失败: %s\n", len, strerror(errno));
        return -1;
    }
    return (int)d.fd;
}

#define DEV       "/dev/video32"
#define NUM_OUT   6      /* 输入（码流）缓冲数 */
#define NUM_CAP   8      /* 输出（像素）缓冲数 */
#define MAX_UNITS 512

struct unit { size_t off, len; };
struct buf  { void *start; size_t length; int dbuf_fd; };

static int xioctl(int fd, unsigned long req, void *arg, const char *name)
{
    int r = ioctl(fd, req, arg);
    if (r < 0) fprintf(stderr, "  %s 失败: %s\n", name, strerror(errno));
    return r;
}

/* 按 OBU_TEMPORAL_DELIMITER(type=2) 切分裸 OBU 流。 */
static int split_tu(const uint8_t *d, size_t len, struct unit *out, int cap)
{
    int n = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t h = d[i];
        int type = (h >> 3) & 0xF, has_size = (h >> 1) & 1;
        size_t j = i + 1;
        if ((h >> 2) & 1) j++;
        size_t size = 0;
        if (has_size) {
            int shift = 0;
            while (j < len) {
                uint8_t b = d[j++];
                size |= (size_t)(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80)) break;
            }
        } else {
            size = len - j;
        }
        size_t end = j + size;
        if (end > len) break;
        if (type == 2) {
            if (n >= cap) break;
            out[n].off = i; out[n].len = end - i; n++;
        } else if (n > 0) {
            out[n - 1].len = end - out[n - 1].off;
        } else {
            if (n >= cap) break;
            out[n].off = i; out[n].len = end - i; n++;
        }
        i = end;
    }
    return n;
}

static int req_bufs(int fd, enum v4l2_buf_type type, int count)
{
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = (unsigned)count;
    rb.type = type;
    rb.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb, "REQBUFS") < 0) return -1;
    printf("  REQBUFS(%s) -> %u 个\n",
           type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? "OUTPUT" : "CAPTURE",
           rb.count);
    return (int)rb.count;
}

/* 为每个缓冲分配一个 dmabuf 并映射到用户空间（供 memcpy 填码流 / 读像素）。
 * 大小取 V4L2 报的 sizeimage —— 驱动已按 AV1 与对齐规则算好。 */
static int alloc_bufs(int fd, enum v4l2_buf_type type, int count,
                      struct buf *bufs, size_t size)
{
    for (int i = 0; i < count; i++) {
        bufs[i].dbuf_fd = dmabuf_alloc(size);
        if (bufs[i].dbuf_fd < 0) return -1;
        bufs[i].length = size;
        bufs[i].start = mmap(NULL, size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, bufs[i].dbuf_fd, 0);
        if (bufs[i].start == MAP_FAILED) {
            /* 映射失败不致命：QBUF 只需要 fd，填数据才需要映射。
             * 对 CAPTURE 侧我们只统计帧数，不读像素，可以容忍。 */
            bufs[i].start = NULL;
        }
    }
    (void)fd; (void)type;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <file.obu> [单元数]\n", argv[0]);
        return 2;
    }
    int limit = (argc > 2) ? atoi(argv[2]) : 8;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)fsz);
    if (!data || fread(data, 1, (size_t)fsz, f) != (size_t)fsz) return 1;
    fclose(f);

    struct unit units[MAX_UNITS];
    int nu = split_tu(data, (size_t)fsz, units, MAX_UNITS);
    if (limit > 0 && limit < nu) nu = limit;
    printf("码流: %ld 字节，切出 %d 个 temporal unit（首个 %zu 字节）\n",
           fsz, nu, nu ? units[0].len : 0);

    int fd = open(DEV, O_RDWR);
    if (fd < 0) { fprintf(stderr, "打开 %s: %s\n", DEV, strerror(errno)); return 1; }

    /* 1. 设置 OUTPUT 侧格式为 AV1 */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_AV1;
    fmt.fmt.pix_mp.width = 1920;
    fmt.fmt.pix_mp.height = 1080;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 4 * 1024 * 1024;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt, "S_FMT(OUTPUT/AV1)") < 0) {
        fprintf(stderr, "→ 硬件/驱动拒绝 AV1 输入格式\n"); close(fd); return 1;
    }
    printf("  S_FMT(OUTPUT) OK: %ux%u sizeimage=%u\n", fmt.fmt.pix_mp.width,
           fmt.fmt.pix_mp.height, fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
    unsigned in_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

    /* 1.5 订阅事件。V4L2 stateful 解码的标准流程要求：
     *   送入首个含序列头的单元 → 驱动解析出真实分辨率 →
     *   发 V4L2_EVENT_SOURCE_CHANGE → 应用重新 G_FMT/REQBUFS(CAPTURE) →
     *   才开始出帧。
     * 不订阅事件就直接双向 STREAMON 并灌数据，驱动无从告知分辨率协商结果，
     * 固件会按错误的假设解析 —— 实测表现为 av1DecParseFrame 报错、0 帧。 */
    {
        struct v4l2_event_subscription sub;
        memset(&sub, 0, sizeof(sub));
        sub.type = V4L2_EVENT_SOURCE_CHANGE;
        if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE(SOURCE_CHANGE)") == 0)
            printf("  已订阅 SOURCE_CHANGE\n");
        memset(&sub, 0, sizeof(sub));
        sub.type = V4L2_EVENT_EOS;
        if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE(EOS)") == 0)
            printf("  已订阅 EOS\n");
    }

    /* ---- 第一阶段：只开 OUTPUT，送首个含序列头的单元，等分辨率协商 ----
     *
     * V4L2 stateful 解码的正确次序（内核文档 dev-decoder.rst）：
     *   S_FMT(OUTPUT) → REQBUFS(OUTPUT) → STREAMON(OUTPUT)
     *   → 送含序列头的单元 → 驱动发 SOURCE_CHANGE
     *   → G_FMT(CAPTURE) 拿真实尺寸 → REQBUFS/STREAMON(CAPTURE) → 出帧
     *
     * 早先把两侧一起 STREAMON 再灌数据，跳过了协商，固件按错误假设解析，
     * 表现为 av1DecParseFrame 报 AV1 ERROR、0 帧。 */
    int n_out = req_bufs(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, NUM_OUT);
    if (n_out <= 0) { close(fd); return 1; }

    struct buf out_bufs[NUM_OUT];
    memset(out_bufs, 0, sizeof(out_bufs));
    if (alloc_bufs(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, n_out, out_bufs, in_size) < 0) {
        fprintf(stderr, "  OUTPUT dmabuf 分配失败\n"); close(fd); return 1;
    }
    printf("  OUTPUT dmabuf OK: %d x %u 字节\n", n_out, in_size);

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type, "STREAMON(OUTPUT)") < 0) { close(fd); return 1; }

    /* 送首个单元（含 OBU_SEQUENCE_HEADER） */
    int sent = 0;
    {
        if (!out_bufs[0].start) {
            fprintf(stderr, "  OUTPUT[0] 未映射，无法填码流\n"); close(fd); return 1;
        }
        memcpy(out_bufs[0].start, data + units[0].off, units[0].len);
        struct v4l2_buffer b; struct v4l2_plane p[1];
        memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
        b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        b.memory = V4L2_MEMORY_DMABUF; b.index = 0;
        b.m.planes = p; b.length = 1;
        p[0].m.fd = out_bufs[0].dbuf_fd;
        p[0].length = (unsigned)out_bufs[0].length;
        p[0].bytesused = (unsigned)units[0].len;
        if (xioctl(fd, VIDIOC_QBUF, &b, "QBUF(OUTPUT[0])") < 0) { close(fd); return 1; }
        sent = 1;
        printf("  已送首单元 %zu 字节，等 SOURCE_CHANGE…\n", units[0].len);
    }

    /* 等 SOURCE_CHANGE 事件 */
    int got_src_change = 0;
    for (int i = 0; i < 20 && !got_src_change; i++) {
        struct pollfd pfd = { .fd = fd, .events = POLLPRI | POLLOUT };
        if (poll(&pfd, 1, 500) <= 0) continue;
        if (pfd.revents & POLLPRI) {
            struct v4l2_event ev;
            memset(&ev, 0, sizeof(ev));
            if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
                printf("  事件: type=%u%s\n", ev.type,
                       ev.type == V4L2_EVENT_SOURCE_CHANGE ? " (SOURCE_CHANGE)" : "");
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE) got_src_change = 1;
            }
        }
        if (pfd.revents & POLLOUT) {
            struct v4l2_buffer b; struct v4l2_plane p[1];
            memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
            b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            b.memory = V4L2_MEMORY_DMABUF; b.m.planes = p; b.length = 1;
            ioctl(fd, VIDIOC_DQBUF, &b);
        }
    }
    if (!got_src_change)
        printf("  ⚠ 未收到 SOURCE_CHANGE，仍继续尝试（部分驱动不发该事件）\n");

    /* ---- 第二阶段：按驱动给出的真实格式配置 CAPTURE ---- */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt, "G_FMT(CAPTURE)") < 0) { close(fd); return 1; }
    printf("  G_FMT(CAPTURE): %ux%u fourcc=%c%c%c%c sizeimage=%u\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
           (char)(fmt.fmt.pix_mp.pixelformat & 0xFF),
           (char)((fmt.fmt.pix_mp.pixelformat >> 8) & 0xFF),
           (char)((fmt.fmt.pix_mp.pixelformat >> 16) & 0xFF),
           (char)((fmt.fmt.pix_mp.pixelformat >> 24) & 0xFF),
           fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

    /* 显式设为 NV12（驱动默认可能是 QCOM 压缩格式 Q08C，我们要线性 NV12） */
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt, "S_FMT(CAPTURE/NV12)") < 0) { close(fd); return 1; }
    unsigned cap_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    printf("  S_FMT(CAPTURE/NV12): sizeimage=%u\n", cap_size);

    int n_cap = req_bufs(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, NUM_CAP);
    if (n_cap <= 0) { close(fd); return 1; }
    struct buf cap_bufs[NUM_CAP];
    memset(cap_bufs, 0, sizeof(cap_bufs));
    if (alloc_bufs(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, n_cap, cap_bufs, cap_size) < 0) {
        fprintf(stderr, "  CAPTURE dmabuf 分配失败\n"); close(fd); return 1;
    }
    for (int i = 0; i < n_cap; i++) {
        struct v4l2_buffer b; struct v4l2_plane p[1];
        memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_DMABUF; b.index = (unsigned)i;
        b.m.planes = p; b.length = 1;
        p[0].m.fd = cap_bufs[i].dbuf_fd;
        p[0].length = (unsigned)cap_bufs[i].length;
        if (xioctl(fd, VIDIOC_QBUF, &b, "QBUF(CAPTURE)") < 0) { close(fd); return 1; }
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type, "STREAMON(CAPTURE)") < 0) { close(fd); return 1; }
    printf("  CAPTURE 就绪（%d 缓冲），继续送余下单元\n", n_cap);

    /* ---- 送剩余单元并收帧 ---- */
    int frames = 0, next = 1, dumped = 0;
    for (int loop = 0; loop < 60; loop++) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT | POLLPRI };
        if (poll(&pfd, 1, 500) <= 0) {
            if (next >= nu && loop > 10) break;
            continue;
        }
        if (pfd.revents & POLLIN) {
            struct v4l2_buffer b; struct v4l2_plane p[1];
            memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            b.memory = V4L2_MEMORY_DMABUF; b.m.planes = p; b.length = 1;
            if (ioctl(fd, VIDIOC_DQBUF, &b) == 0) {
                frames++;
                printf("  ★ 解出帧 %d: index=%u bytesused=%u\n",
                       frames, b.index, p[0].bytesused);
                /* 落盘首个非空帧，供离线像素校验（判断是否真图像而非全灰）。 */
                if (p[0].bytesused > 0 && !dumped && cap_bufs[b.index].start) {
                    FILE *df = fopen("/data/local/tmp/av1_frame0.nv12", "wb");
                    if (df) {
                        fwrite(cap_bufs[b.index].start, 1, p[0].bytesused, df);
                        fclose(df);
                        dumped = 1;
                        printf("    → 已落盘 %u 字节到 /data/local/tmp/av1_frame0.nv12\n",
                               p[0].bytesused);
                    }
                }
                p[0].m.fd = cap_bufs[b.index].dbuf_fd;
                p[0].length = (unsigned)cap_bufs[b.index].length;
                ioctl(fd, VIDIOC_QBUF, &b);
            }
        }
        if (pfd.revents & POLLOUT) {
            struct v4l2_buffer b; struct v4l2_plane p[1];
            memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
            b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            b.memory = V4L2_MEMORY_DMABUF; b.m.planes = p; b.length = 1;
            if (ioctl(fd, VIDIOC_DQBUF, &b) == 0 && next < nu) {
                unsigned idx = b.index;
                if (out_bufs[idx].start && units[next].len <= in_size) {
                    memcpy(out_bufs[idx].start, data + units[next].off, units[next].len);
                    memset(&b, 0, sizeof(b)); memset(p, 0, sizeof(p));
                    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
                    b.memory = V4L2_MEMORY_DMABUF; b.index = idx;
                    b.m.planes = p; b.length = 1;
                    p[0].m.fd = out_bufs[idx].dbuf_fd;
                    p[0].length = (unsigned)out_bufs[idx].length;
                    p[0].bytesused = (unsigned)units[next].len;
                    b.timestamp.tv_sec = next;
                    if (ioctl(fd, VIDIOC_QBUF, &b) == 0) { sent++; next++; }
                }
            }
        }
        if (pfd.revents & POLLPRI) {
            struct v4l2_event ev;
            memset(&ev, 0, sizeof(ev));
            if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0)
                printf("  事件: type=%u\n", ev.type);
        }
    }

    printf("结论: 送入 %d 单元，V4L2 直接解出 %d 帧\n", sent, frames);

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    close(fd);
    return frames > 0 ? 0 : 1;
}
