/*
 * Probe_mp2_feed - Send MPEG-2 base flow to /dev/ video32 at different particle levels, bypassing VA-API drive
 *
 * Use: Positioning MPEG-2 "Sys_ERROR" is a matter of particle size or lack of solid capacity.
 * See MPEG-2 notes and mpeg2_bitstream.c for background:
 * Synthetic stream is aligned with the ffmpeg original stream bytes, but send solids in 2 units
 * SYS_ERROR, no vomiting. This probe uses ** original ES** (without synthesis) to shoot different delivery methods.
 * If a particle size can be drawn from a frame, the driven delivery is changed to the same particle size.
 *
 * Memory mode: This machine (kernel 6.6 new msm_vidc) REQBUFS only accepts MMAP/DMABUF,
 * The probe MMAP (REQBUFS + QUEYBUF + mmap), both of which are available in the OTPUT/ CAPTURE.
 *
 * Mode (argv [2]):
 *   pic Each unit = one image (GOP header incorporated into the image unit that follows),
 *            First module for the first file header (drivers current behaviour)
 *   Seqery with pic, but each image unit is pre-set for the first section
 *   Gop Each unit = one GOP (sequience header merged with first unit)
 *   whole file as a unit (request to put in OPPUT sizeimage)
 *
 * Usage:
 *   cc -O2 -o probe_mp2_feed probe_mp2_feed.c
 *   ./probe_mp2_feed<file.mpg> [pic ⁇ seqevery ⁇ gop ⁇ whole] [unit ceiling]
 *   Drop previous CAPTURE contents on /tmp/dmdwork/mp2_cap_*.youv
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

/* Find the next starting number (0x00 image) within [pos,n) that changes the boundary of the feed. / 0xB8 GOP. */
static size_t next_boundary(const unsigned char *d, size_t n, size_t pos)
{
    for (size_t p = pos; p + 3 <= n; p++)
        if (d[p] == 0 && d[p+1] == 0 && d[p+2] == 1 &&
            (d[p+3] == 0x00 || d[p+3] == 0xB8))
            return p;
    return n;
}

/* QBUF A UTPUT buffer (MMAP). The full queues recover one retry; failed to return -1. */
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
        fprintf(stderr, "Usage: %s <file.mpg> [pic|seqevery|gop|whole] [Unit ceiling]\n",
                argv[0]);
        return 2;
    }
    const char *mode = (argc > 2) ? argv[2] : "pic";
    const int cap_units = (argc > 3) ? atoi(argv[3]) : 0;
    const int whole = !strcmp(mode, "whole");
    const int seqevery = !strcmp(mode, "seqevery");

    size_t flen = 0;
    unsigned char *data = slurp(argv[1], &flen);
    if (!data) { fprintf(stderr, "Can’t read. %s\n", argv[1]); return 1; }

    size_t first_pic = next_boundary(data, flen, 0);
    printf("  Enter %s,%zu Bytes, first image/GOP The border is on. %zu,Mode %s\n",
           argv[1], flen, first_pic, mode);

    int fd = open("/dev/video32", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("  open /dev/video32"); free(data); return 1; }

    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.width = 716;      /* matrixbench;kernels are rewrited to macros */
    f.fmt.pix_mp.height = 236;
    f.fmt.pix_mp.pixelformat = v4l2_fourcc('M', 'P', 'G', '2');
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = 16 << 20;
    if (ioctl(fd, VIDIOC_S_FMT, &f) < 0) {
        printf("  S_FMT(MPG2) failed: %s\n", strerror(errno));
        close(fd); free(data); return 1;
    }
    const size_t isz = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    printf("  S_FMT(OUTPUT/MPG2) ok,sizeimage=%zu,Confirmed confirmation %ux%u\n",
           isz, f.fmt.pix_mp.width, f.fmt.pix_mp.height);

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = 8; rb.type = f.type; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) {
        printf("  REQBUFS(OUTPUT/MMAP) failed: %s\n", strerror(errno));
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
            printf("  QUERYBUF(OUTPUT %d) failed: %s\n", i, strerror(errno));
            close(fd); free(data); return 1;
        }
        bufsz[i] = pl[0].length;
        bufs[i] = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, pl[0].m.mem_offset);
        if (bufs[i] == MAP_FAILED) {
            printf("  mmap(OUTPUT %d) failed: %s\n", i, strerror(errno));
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

    /* Construct a list of units: cut by image/GOP boundary. Units [0] if it is a plain section header
     * (off=0, len=first_pic) incorporated in mode into the next module (pic/gop) or removed
     * (seqevery, uniform feed prefix). */
    struct { size_t off, len; } units[8192];
    size_t nunits = 0;
    if (whole) {
        units[0].off = 0; units[0].len = flen; nunits = 1;
        if (flen > isz) {
            printf("  Whole %zu Bytes can’t fit. sizeimage %zu\n", flen, isz);
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
    printf("  Cut %zu Modules (first module) len=%zu)\n", nunits,
           nunits ? units[0].len : 0);

    size_t sent = 0;
    int got_sc = 0;
    for (size_t u = 0; u < nunits; u++) {
        if (cap_units && (int)sent >= cap_units) break;
        size_t off = units[u].off, len = units[u].len;
        int idx = (int)(sent % 8);

        if (seqevery && off >= first_pic && first_pic > 0) {
            /* Image unit prefixes the seq header by borrowing the current rotor. */
            if (first_pic + len > isz) { printf("  Unit Super sizeimage\n"); break; }
            memcpy(bufs[idx], data, first_pic);
            memcpy((char*)bufs[idx] + first_pic, data + off, len);
            len += first_pic;
        } else {
            if (len > isz) { printf("  Module %zu Long %zu Super sizeimage\n",
                                    u, len); break; }
            memcpy(bufs[idx], data + off, len);
        }
        if (qbuf_out(fd, f.type, idx, len) < 0) {
            printf("  number %zu Module QBUF failed: %s\n", sent, strerror(errno));
            break;
        }
        sent++;
        /* A glance at SOURCE_CHANGE per 4 units. */
        struct pollfd pf = { .fd = fd, .events = POLLPRI };
        if (poll(&pf, 1, 0) > 0 && (pf.revents & POLLPRI)) {
            struct v4l2_event ev; memset(&ev, 0, sizeof(ev));
            if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 &&
                ev.type == V4L2_EVENT_SOURCE_CHANGE && !got_sc) {
                got_sc = 1;
                printf("  ✓ SOURCE_CHANGE At %zu Unit is arriving.\n", sent);
            }
        }
    }
    printf("  Send In %zu Units\n", sent);

    if (!got_sc) {
        struct pollfd pf = { .fd = fd, .events = POLLPRI };
        if (poll(&pf, 1, 2000) > 0 && (pf.revents & POLLPRI)) {
            struct v4l2_event ev; memset(&ev, 0, sizeof(ev));
            if (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 &&
                ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                got_sc = 1;
                printf("  ✓ SOURCE_CHANGE We’re here after the irrigation.\n");
            }
        }
    }
    if (!got_sc)
        printf("  ✗ Never. SOURCE_CHANGE —— Solid does not parse outflow parameters (see dmesg)\n");

    /* CAPTURE side-view consultation and collection. */
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
        printf("  S_FMT(CAPTURE/NV12) failed: %s\n", strerror(errno));
        close(fd); free(data); return 1;
    }
    const size_t csz = cf.fmt.pix_mp.plane_fmt[0].sizeimage;

    struct v4l2_requestbuffers crb;
    memset(&crb, 0, sizeof(crb));
    crb.count = 16; crb.type = cf.type; crb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &crb) < 0) {
        printf("  REQBUFS(CAPTURE/MMAP) failed: %s\n", strerror(errno));
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

    printf("\n  ===== Outcome (mode) %s)=====\n", mode);
    printf("  Send In %zu Unit, copy. %zu frame\n", sent, frames);
    close(fd); free(data);
    return 0;
}
