/*
 * Probe_av1_feed -- direct AV1 OBU temporal unit /dev/ video32, bypassing VA-API driver
 *
 * Uses: whether the "AV1" frame is determined to be a driver-driven coding stream synthesis or a hardware/kernel side issue.
 * This is the key to 68th wheel positioning AV1 defects -- the sames/ av1_probe. py used in the year.
 * (go daemon line protocol). Daemon was deleted at 0.4.0, so rewrite to V4L2.
 *
 * The verdict is:
 *   Direct source stream can fill the frame  ⁇  Hardware is fine, bug is driving OBU synthesis
 *   • Hardware/kernel side problem, not synthesis.
 *
 * Historical conclusions (round 68, measured with daemon probes, as per the following:
 *   Source stream av1_1080p.obu direct: transport 150 units, take **150** frame
 *   Driver of synthetic streams: Send 150 units, take **80** frame
 * Two streams of OBU make up only one difference: 70 source streams show_existing_frame
 * FRAME_HEADER, None of the synthetic streams. So hardware will resurface for every SEEF.
 * The missing is this 70 head -- this conclusion was reversed in the year that "SEF rediscovers that ffmpeg did it himself."
 *
 * Usage:
 *   cc -O2 -o probe_av1_feed probe_av1_feed.c
 *   .../probe_av1_feed<file.obu> [cell ceiling]
 *
 * ⁇ burg semantic: AV1 decoder with output delay,
 * The lock is locked on a unit-by-unit basis (it has to fill up with N input before throwing up the first frame, and we're waiting for the first frame).
 * This pit was stepped on the daemon page, and it's the only place we're gonna be.
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
#include <sys/mman.h>
#include <linux/videodev2.h>

/* dma-heap allocation (the core of which is OTPUT/CAPTURE only receives DMABUF). */
struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0, struct dma_heap_allocation_data)

static int heap_fd = -1;
static int dmabuf_alloc(size_t len)
{
    if (heap_fd < 0) {
        heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
        if (heap_fd < 0) return -1;
    }
    struct dma_heap_allocation_data a;
    memset(&a, 0, sizeof(a));
    a.len = len;
    a.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &a) < 0) return -1;
    return (int)a.fd;
}

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

/* Read the lib128, return the value, *len writes the byte count. */
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

/* Find the next deviation from pos (the next TU). */
static size_t next_td(const unsigned char *d, size_t n, size_t pos)
{
    size_t p = pos;
    while (p < n) {
        unsigned type = (d[p] >> 3) & 0x0f;
        int has_size = (d[p] >> 1) & 1;
        size_t hdr = 1;
        if ((d[p] >> 2) & 1) hdr++;            /* extension */
        if (!has_size) return n;               /* No size field, cannot continue */
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
        fprintf(stderr, "Usage: %s <file.obu> [Unit ceiling]\n", argv[0]);
        return 2;
    }
    const int cap_units = (argc > 2) ? atoi(argv[2]) : 0;

    size_t flen = 0;
    unsigned char *data = slurp(argv[1], &flen);
    if (!data) { fprintf(stderr, "Can’t read. %s\n", argv[1]); return 1; }
    printf("  Enter %s,%zu bytes\n", argv[1], flen);

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
        printf("  S_FMT(AV10) failed: %s\n", strerror(errno));
        printf("  → The device msm_vidc Maybe not. AV1;Use v4l2_enum_fmt Confirm\n");
        close(fd); free(data); return 1;
    }
    const size_t isz = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    printf("  S_FMT(OUTPUT/AV10) ok，sizeimage=%zu\n", isz);

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = 64; rb.type = f.type; rb.memory = V4L2_MEMORY_DMABUF;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) {
        printf("  REQBUFS(OUTPUT/DMABUF) failed: %s\n", strerror(errno));
        close(fd); free(data); return 1;
    }
    printf("  REQBUFS(DMABUF) ok，count=%u\n", rb.count);
    /* 8 OTPUT dmabuf (the core of this UTPUT only receives DMABUF and does not support MMAP). */
    void *bufs[64];
    int ofd[64];
    for (int i = 0; i < 64; i++) {
        ofd[i] = dmabuf_alloc(isz);
        if (ofd[i] < 0) {
            printf("  dma_heap allocate OUTPUT %d failed: %s\n", i, strerror(errno));
            close(fd); free(data); return 1;
        }
        bufs[i] = mmap(NULL, isz, PROT_READ | PROT_WRITE, MAP_SHARED, ofd[i], 0);
        if (bufs[i] == MAP_FAILED) {
            printf("  mmap(OUTPUT dmabuf %d) failed: %s\n", i, strerror(errno));
            close(fd); free(data); return 1;
        }
    }

    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);

    unsigned t = f.type;
    if (ioctl(fd, VIDIOC_STREAMON, &t) < 0) {
        printf("  STREAMON(OUTPUT) failed: %s\n", strerror(errno));
        close(fd); free(data); return 1;
    }

    /* Cut and burst into (buffer map). */

    size_t pos = 0, sent = 0;
    int got_sc = 0;
    while (pos < flen) {
        if (cap_units && (int)sent >= cap_units) break;
        size_t end = next_td(data, flen, pos);
        size_t len = end - pos;
        if (!len || len > isz) break;

        int idx = (int)(sent % 64);
        memcpy(bufs[idx], data + pos, len);

        struct v4l2_buffer b; struct v4l2_plane pl[1];
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = f.type; b.memory = V4L2_MEMORY_DMABUF; b.index = idx;
        b.m.planes = pl; b.length = 1;
        b.timestamp.tv_usec = (long)(sent + 1);
        pl[0].m.fd = ofd[idx];
        pl[0].length = (unsigned)isz; pl[0].bytesused = len;

        if (ioctl(fd, VIDIOC_QBUF, &b) < 0) {
            /* The queue is full and normal, and one is recycled again. */
            struct pollfd pf = { .fd = fd, .events = POLLOUT | POLLPRI };
            poll(&pf, 1, 500);
            if (pf.revents & POLLPRI) {
                struct v4l2_event ev; memset(&ev, 0, sizeof(ev));
                if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 &&
                    ev.type == V4L2_EVENT_SOURCE_CHANGE && !got_sc) {
                    got_sc = 1;
                    printf("  ✓ SOURCE_CHANGE At %zu Unit is arriving.\n", sent);
                }
            }
            struct v4l2_buffer d; struct v4l2_plane dp[1];
            memset(&d, 0, sizeof(d)); memset(dp, 0, sizeof(dp));
            d.type = f.type; d.memory = V4L2_MEMORY_DMABUF;
            d.m.planes = dp; d.length = 1;
            if (ioctl(fd, VIDIOC_DQBUF, &d) < 0) {
                printf("  number %zu Units QBUF Failed without buffer to recover: %s\n",
                       sent, strerror(errno));
                break;
            }
            if (ioctl(fd, VIDIOC_QBUF, &b) < 0) {
                printf("  number %zu Retry Unit QBUF Still failed: %s\n",
                       sent, strerror(errno));
                break;
            }
        }
        sent++;
        pos = end;
    }
    printf("  Send In %zu  temporal unit\n", sent);

    if (!got_sc) {
        struct pollfd pf = { .fd = fd, .events = POLLPRI };
        if (poll(&pf, 1, 2000) > 0 && (pf.revents & POLLPRI)) {
            struct v4l2_event ev; memset(&ev, 0, sizeof(ev));
            if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
                got_sc = 1;
                printf("  ✓ SOURCE_CHANGE We’re here after the irrigation.\n");
            }
        }
    }
    if (!got_sc) {
        printf("  ✗ Never. SOURCE_CHANGE —— Solid does not parse code stream\n");
        printf("  → It’s not about the flow. It’s about the decoding session.;\n");
        printf("    Run first. probe_device_support Confirm whether the equipment is available\n");
        close(fd); free(data); return 1;
    }

    /* CAPTURE side-view consultation and collection. */
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
    printf("  S_FMT(CAPTURE): %ux%u stride=%u sizeimage=%zu\n",
           cf.fmt.pix_mp.width, cf.fmt.pix_mp.height,
           cf.fmt.pix_mp.plane_fmt[0].bytesperline, csz);

    struct v4l2_requestbuffers crb;
    memset(&crb, 0, sizeof(crb));
    crb.count = 64; crb.type = cf.type; crb.memory = V4L2_MEMORY_DMABUF;
    if (ioctl(fd, VIDIOC_REQBUFS, &crb) < 0) {
        printf("  REQBUFS(CAPTURE) failed: %s\n", strerror(errno));
        close(fd); free(data); return 1;
    }
    void *cb[64];
    int cfd[64];
    for (unsigned i = 0; i < crb.count && i < 64; i++) {
        cfd[i] = dmabuf_alloc(csz);
        if (cfd[i] < 0) break;
        cb[i] = mmap(NULL, csz, PROT_READ | PROT_WRITE, MAP_SHARED, cfd[i], 0);
        if (cb[i] == MAP_FAILED) break;
        struct v4l2_buffer b; struct v4l2_plane pl[1];
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = cf.type; b.memory = V4L2_MEMORY_DMABUF; b.index = i;
        b.m.planes = pl; b.length = 1;
        pl[0].m.fd = cfd[i];
        pl[0].length = (unsigned)csz;
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
        d.type = cf.type; d.memory = V4L2_MEMORY_DMABUF;
        d.m.planes = dp; d.length = 1;
        if (ioctl(fd, VIDIOC_DQBUF, &d) < 0) break;
        frames++;
        if (getenv("DMP_DUMP") && frames <= 999) {
            char nm[64];
            snprintf(nm, sizeof(nm), "/tmp/dmdwork/av1_cap_%03zu.yuv", frames);
            FILE *df = fopen(nm, "wb");
            if (df) { fwrite(cb[d.index], 1, csz, df); fclose(df); }
        }
        ioctl(fd, VIDIOC_QBUF, &d);          /* Return it now and keep it. */
    }

    printf("\n  ===== result =====\n");
    printf("  Send In %zu Unit, copy. %zu frame\n", sent, frames);
    if (frames >= sent)
        printf("  → Hardware can be filled with it.:AV1 The bug is driving. OBU Synthetic Sides\n");
    else
        printf("  → Hardware only vomits. %zu/%zu:Check the hardware first./kernel side, look at synthesis.\n",
               frames, sent);

    close(fd); free(data);
    return 0;
}
