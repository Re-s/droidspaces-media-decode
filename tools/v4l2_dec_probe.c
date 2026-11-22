/*
 * v4l2_dec_probe - Dissect a AV1 stream directly from the V4L2 M2M interface.
 *
 * Use: Completely bypassed MediaCodec / Codec2 / vendor HAL, direct
 * /dev/video32(msm_vidc_decode) running standard V4L2 stateful decoding process.
 * This is a determination of whether "AV1 is a hardware/kernel-driven problem or a problem with the upper Codec2 problem."
 * Key experiments.
 *
 * Background: SM8750 hardware identified AV10 (V4L2 ENUM_FMT) but
 * c2.qti.av1.decoder failed during the component creation phase (QueryrequiredInfos from
 * If you can unblock the program, you can use the software to help you.
 * Description of hardware and kernel drive available and malfunctioning in the Codec2 layer; if the same fails, it is a lower problem.
 *
 * Usage (need root):
 *     v42_dec_probe<file.obu> [units]
 *
 * Cross-compilation: This machine can be directly cc-O2-static if it is aarch64.
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

/* The octl definition of dma-heap (linux/dma-heap.h may be missing in the NDK/glibc header, but the NDK/ glibc can be missing.
 * Direct in-line - ABI is a stable kernel interface. */
struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0, struct dma_heap_allocation_data)

/* The MSMP is a model that is not available in the RTBP, but is used to keep the memory of the MMAP, which is a model that is not available in the RMFP.
 * But the QUERYBUF gives outfset cannot map (back to ENODEV). The actual measurements must go dma-heap. */
static int dmabuf_alloc(size_t len)
{
    static int heap_fd = -1;
    if (heap_fd < 0) {
        heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
        if (heap_fd < 0) {
            fprintf(stderr, "  open /dev/dma_heap/system failed: %s\n", strerror(errno));
            return -1;
        }
    }
    struct dma_heap_allocation_data d;
    memset(&d, 0, sizeof(d));
    d.len = len;
    d.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &d) < 0) {
        fprintf(stderr, "  dma_heap allocate %zu Bytes failed: %s\n", len, strerror(errno));
        return -1;
    }
    return (int)d.fd;
}

#define DEV       "/dev/video32"
#define NUM_OUT   6      /* Enter (bitstream) buffers */
#define NUM_CAP   8      /* Output (pixels) buffers */
#define MAX_UNITS 512

struct unit { size_t off, len; };
struct buf  { void *start; size_t length; int dbuf_fd; };

static int xioctl(int fd, unsigned long req, void *arg, const char *name)
{
    int r = ioctl(fd, req, arg);
    if (r < 0) fprintf(stderr, "  %s failed: %s\n", name, strerror(errno));
    return r;
}

/* Cut the naked OBU stream by the OBU_TEMPORAL_DELIMITER(type=2) */
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
    printf("  REQBUFS(%s) -> %u \n",
           type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? "OUTPUT" : "CAPTURE",
           rb.count);
    return (int)rb.count;
}

/* Assigns one dmabuf to each buffer and maps it to the user space (for memcpy fill stream/ read pixels).
 * Size of sizeimage for V4L2 -- driver is calculated by the AV1 alignment rule. */
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
            /* The map failed is not fatal: QBUF only needs fd to fill in the data to map.
             * We only count frames, don't read pixels, and we can tolerate them. */
            bufs[i].start = NULL;
        }
    }
    (void)fd; (void)type;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.obu> [Number of units]\n", argv[0]);
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
    printf("bitstream: %ld Bytes, cut out %d  temporal unit(First %zu bytes)\n",
           fsz, nu, nu ? units[0].len : 0);

    int fd = open(DEV, O_RDWR);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", DEV, strerror(errno)); return 1; }

    /* 1. Setup UPPUT side format to AV1 */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_AV1;
    fmt.fmt.pix_mp.width = 1920;
    fmt.fmt.pix_mp.height = 1080;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 4 * 1024 * 1024;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt, "S_FMT(OUTPUT/AV1)") < 0) {
        fprintf(stderr, "→ hardware/Driver Reject AV1 Enter Format\n"); close(fd); return 1;
    }
    printf("  S_FMT(OUTPUT) OK: %ux%u sizeimage=%u\n", fmt.fmt.pix_mp.width,
           fmt.fmt.pix_mp.height, fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
    unsigned in_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

    /* 1.5 Subscription events. Standard process requirements for V4L2 status:
     *   Send the first unit with the serial header * Driver to decipher the true resolution *
     *   V4L2_EVENT_SOURCE_CHANGE — Applying the G_FMT/REQBUFS (CAPTURE)  ⁇
     *   Only then does the decoder start producing frames.
     * The non-subscription event is directly a two-way STREAMON and then the data is injected, driving the failure to communicate the resolution to the outcome of the consultation.
     * Solids are parsed according to an incorrect hypothesis - the actual measurements are av1DecParseFrame, 0 frames. */
    {
        struct v4l2_event_subscription sub;
        memset(&sub, 0, sizeof(sub));
        sub.type = V4L2_EVENT_SOURCE_CHANGE;
        if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE(SOURCE_CHANGE)") == 0)
            printf("  Subscriptions SOURCE_CHANGE\n");
        memset(&sub, 0, sizeof(sub));
        sub.type = V4L2_EVENT_EOS;
        if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE(EOS)") == 0)
            printf("  Subscriptions EOS\n");
    }

    /* - Phase I: only OUTPUT, sending the first module with the serial head, etc. resolution consultation
     *
     * The correct order of decoding of V4L2 status (kernel document dev-decoder.rst):
     *   S_FMT(OUTPUT) → REQBUFS(OUTPUT) → STREAMON(OUTPUT)
     *   ⁇  Unit with sequence head  ⁇  Driver SOURCE_CHANGE
     *   *G_FMT(CAPTURE) takes real dimensions * REQBUFS/ STREAMON(CAPTURE) * Frame out
     *
     * The earlier streamon refilled the data, jumped over the negotiations, the solids were misconstrued.
     * The video is available in the AV1 ERROR, 0 frames. */
    int n_out = req_bufs(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, NUM_OUT);
    if (n_out <= 0) { close(fd); return 1; }

    struct buf out_bufs[NUM_OUT];
    memset(out_bufs, 0, sizeof(out_bufs));
    if (alloc_bufs(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, n_out, out_bufs, in_size) < 0) {
        fprintf(stderr, "  OUTPUT dmabuf Distribution Failed\n"); close(fd); return 1;
    }
    printf("  OUTPUT dmabuf OK: %d x %u bytes\n", n_out, in_size);

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type, "STREAMON(OUTPUT)") < 0) { close(fd); return 1; }

    /* Send the first module (including OBU_SEQUENCE_HEADER) */
    int sent = 0;
    {
        if (!out_bufs[0].start) {
            fprintf(stderr, "  OUTPUT[0] Unmaped, Unable to fill the stream\n"); close(fd); return 1;
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
        printf("  Sended First Module %zu Bytes, wait SOURCE_CHANGE…\n", units[0].len);
    }

    /* Wait for SOURCE_CHANGE events */
    int got_src_change = 0;
    for (int i = 0; i < 20 && !got_src_change; i++) {
        struct pollfd pfd = { .fd = fd, .events = POLLPRI | POLLOUT };
        if (poll(&pfd, 1, 500) <= 0) continue;
        if (pfd.revents & POLLPRI) {
            struct v4l2_event ev;
            memset(&ev, 0, sizeof(ev));
            if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
                printf("  Events: type=%u%s\n", ev.type,
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
        printf("  ⚠ Not received SOURCE_CHANGE,Continue trying (partially driven not to send the event))\n");

    /* - Phase 2: Configure CAPTURE in real format given by driver - */
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

    /* Show set NV12 (Driver default may be QCOM compression format Q08C, we want linear NV12) */
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt, "S_FMT(CAPTURE/NV12)") < 0) { close(fd); return 1; }
    unsigned cap_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    printf("  S_FMT(CAPTURE/NV12): sizeimage=%u\n", cap_size);

    int n_cap = req_bufs(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, NUM_CAP);
    if (n_cap <= 0) { close(fd); return 1; }
    struct buf cap_bufs[NUM_CAP];
    memset(cap_bufs, 0, sizeof(cap_bufs));
    if (alloc_bufs(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, n_cap, cap_bufs, cap_size) < 0) {
        fprintf(stderr, "  CAPTURE dmabuf Distribution Failed\n"); close(fd); return 1;
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
    printf("  CAPTURE ready(%d Continue with the remaining units\n", n_cap);

    /* - Send the remaining units and take the frame */
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
                printf("  ★ Undo Frame %d: index=%u bytesused=%u\n",
                       frames, b.index, p[0].bytesused);
                /* The first non-empty frame on the fall plate is used for offline pixel verification (judgment of whether the image is real rather than full ash). */
                if (p[0].bytesused > 0 && !dumped && cap_bufs[b.index].start) {
                    FILE *df = fopen("/data/local/tmp/av1_frame0.nv12", "wb");
                    if (df) {
                        fwrite(cap_bufs[b.index].start, 1, p[0].bytesused, df);
                        fclose(df);
                        dumped = 1;
                        printf("    → Disk %u Bytes to /data/local/tmp/av1_frame0.nv12\n",
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
                printf("  Events: type=%u\n", ev.type);
        }
    }

    printf("Conclusions: Send In %d Module,V4L2 Just clear it out. %d frame\n", sent, frames);

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    close(fd);
    return frames > 0 ? 0 : 1;
}
