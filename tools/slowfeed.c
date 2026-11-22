/* Slowfeed - by going through VA-API direct /dev/video 32, quantifying the delivery properties of msm_vidc.
 *
 * Why it is needed: the frame drop rate on the browser is not available on this device to judge the drive changes --
 * The same profile, the same driver, the same material's middle self-check 3.73% vs. 4.10%,
 * The range spans from 2.17 to 22.8.2 per cent to 3.16 to 29.24 per cent, which is 5 times the median for the configuration of the batch.
 * Noise comes from the Android side of the shared core and is invisible and uncontrollable in the container.
 * The program is fully consistent with and is available for adjudication through five rounds of 23.7fps without browsers and synthesizers.
 *
 * It was originally written to verify the hypothesis that "OUTPUT is driving 0/8" to describe the unit that was sent in.
 * Not driven to catch. The hypothesis is wrong -- dmd_v4l2_recv will be used in POLLOUT
 * Read_output() log is a snapshot of recovery, 0/8 is normal. Leave this record to avoid re-exiting.
 *
 * Feed rhythm (second parameter):
 *   Fast consecutive feed, maximum for stowage detection (simulating ffmpeg)
 *   Slow send one frame and one frame, first frame time lag (mage-by-frame sync for Firefox)
 *   DeepthN maintains N units flying, scans "in depth" for "steaking."
 *
 * With it, the key conclusion is that:
 *
 * 1) Solid Header lag = 4 input units, not related to B
 *    The B-bound, B-free stream is all five units that are sent to the first frame.
 *    Shows the sequence (OUTPUT_ORDER=0) which is later turned into 7. The LoWLATENCY switch does not affect this.
 *
 * 2) The threshold is exactly the amount of the lag at the depth of the flight
 *      Demoth 1-3:23.5 fps, slow frame (>33ms) 22.7%
 *      Dapth 4: 45.8 fps, slow frame 0.0%  ⁇  more than 1 frame
 *      Dipth 5: 58.9 fps, slow frame 0.0%
 *    Firefox is always at the end 2-3 (18 surfaces still unchanged) and falls forever below the crack.
 *
 * 3) POLLIN will miss out on the tab. (The frame path for v4l2_backend.c is fixed accordingly)
 *    Frame is already in the CAPTURE queue and the poll is empty, empty to return after timeout,
 *    Frame interval is dragged into the timeout itself: 100ms — interval 111ms, time 20ms — 41ms.
 *    Replace "DQBUF without block, no pop" with "DQBUF without pick" and then slow frame 15.3% * 3.4%.
 *
 * 4) Driver delivery rhythm is healthy (microsimeter histogram, Dept. 3)
 *      Intermediate 31984us (ideal 33333us, 4.0%) p90 3424us
 *      Cumulatively backward 1.41 frame length / 197 frame
 *    Keep up with 30fps, long-term no-delayed.
 *
 * Build: gcc -O2 -o slowfeed slowfeed.c
 * Usage: ./slowfeed <file> <fast|slow|depthN> [frames] [order] [hevc] [lowlat] [pad]
 *   order 0 = display order 1 = decoded (default 0)
 *   Hevc 1 = HEVC stream (default 0 = H.264)
 *   ⁇  Based on individual nal feed, HEVC contains multiple nar, HEVC results only for reference
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

/* - Private Agreement Constant (see research/FINDING-msm-vidc-protocol.md) - */
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
    /* Set CID_EXTRADATA after CAPTURE is 2 plane, the plane does not match the direct EINVAL
     * (valid_v4l2_buffer request b->legth=num_planes, msm_vidc.c:452-461)*/
    v.length = (b->extra_fd >= 0) ? 2 : 1;
    p[0].reserved[0] = (unsigned)b->fd;   /* fd Go Reserved [0] */
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

/* Unblocked recovery of UTPUT, return number of recoverys */
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

/* Take a frame CAPTURE, returns 1 with frame / 0 none / -1 error; external index */
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

/* Find the next AnnexB NAL starting code. */
static size_t next_start(const uint8_t *d, size_t n, size_t from)
{
    for (size_t i = from; i + 3 < n; i++)
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) return i;
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <file.h264> <fast|slow> [Frames]\n", argv[0]);
        return 2;
    }
    int slow = strcmp(argv[2], "slow") == 0;
    /* DepthN: Maintains N units flying. N=1 down, N is high fast.
     * This directly quantifys the relationship between "in the frame number--- > spitting" to determine how deep the browser needs to be in the queue.*/
    int depth = 0;
    if (strncmp(argv[2], "depth", 5) == 0) depth = atoi(argv[2] + 5);
    int want = (argc > 3) ? atoi(argv[3]) : 40;
    /* OTPUT_ORDER: 0 = display order (default) 1 = decoding sequence. Decoding sequences vary and should be smaller. */
    int order = (argc > 4) ? atoi(argv[4]) : 0;
    int is_hevc = (argc > 5) ? atoi(argv[5]) : 0;
    int lowlat  = (argc > 6) ? atoi(argv[6]) : 1;
    /* Pad>0: Fill up a "fill" unit to drive the stream when the pad is short.
     * Fill with SPS (non-image unit) starting with the stream to verify whether the solid is accepted,
     * The blog is a good example of how the country is going to be able to make a difference. */
    int pad     = (argc > 7) ? atoi(argv[7]) : 0;

    /* Read Source */
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

    /* --- Protocol sequence --- */
    struct v4l2_format fmt;
    /* 1) dummy S_FMT (OUTPUT) at different resolution */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = 640; fmt.fmt.pix_mp.height = 480;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix_mp.num_planes = 1;
    ioctl(fd, VIDIOC_S_FMT, &fmt);

    /* 2) Target resolution */
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
           order ? "Decoding" : "Show Order");
    set_ctrl(fd, CID_EXTRADATA, 2);
    set_ctrl(fd, CID_LOWLATENCY, lowlat);
    printf("LOWLATENCY=%d\n", lowlat);
    if (set_ctrl(fd, CID_OUTPUT_MODE, 1) < 0) {   /* SECANDARY Yes */
        perror("S_CTRL OUTPUT_MODE=SECONDARY"); return 1;
    }
    for (unsigned i = 0; i <= 7; i++) sub_event(fd, EV_MSM_VIDC_START + i);

    /* CAPTURE Format */
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

    /* Buffer */
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
    printf("buffer: OUTPUT %d x %u, CAPTURE %d x %u\n",
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
    printf("STREAMON Double OK,Mode=%s\n", slow ? "slow(I’ll go through the frame.)" : "fast(Send)");

    /* --- Material cycle --- */
    size_t pos = next_start(bs, fsz, 0);
    int sent = 0, got = 0, zero_out_events = 0;
    int64_t t0 = now_ms();
    int64_t last_frame_ms = 0;      /* The previous frame arrives at the moment to calculate the interval */
    int slow_frames = 0;            /* Number of frames for > 33ms between */
    /* Interval Histogram (us accuracy): The frame drop rate on the browser side is overridden by the system noise (as measured by configuration)
     * 2%-29%) cannot be used to judge the effect of change. And the driving speed of delivery is highly stable.
     * (dept3 five rounds of 23.7fps completely consistent), so we switched to it as evidence. */
    static long gap_hist[4096]; int gap_n = 0;

    while (sent < want && pos < (size_t)fsz) {
        /* Take one of the NALs (to the next start) */
        size_t end = next_start(bs, fsz, pos + 3);
        size_t len = end - pos;
        if (len > in_size) { pos = end; continue; }

        reap_out(fd, out);
        int idx = -1;
        for (int i = 0; i < n_out; i++) if (!out[i].queued) { idx = i; break; }
        if (idx < 0) {
            /* No time for buffers: try again later. */
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

        /* Critical observations: QBUF, followed by several UUTPUTs in hand */
        int q = count_queued(out, n_out);
        if (q == 0) zero_out_events++;
        if (sent <= 12 || q == 0)
            printf("[Send %2d] OUTPUT In Driver %d/%d  Received %d%s\n",
                   sent, q, n_out, got, q == 0 ? "   ← It fell. 0!" : "");

        /* Filling module: A SPS (non-image unit) is added when the plane is short.
         *
         * The purpose of the project is to verify the idea that Firefox only lets two to three frames fly, while solids lag behind four frames, and that the two to three frames are only flying.
         * The poor 1 frame can be filled with "no harm to non-image unit" and thus drive the flow line.
         * Key to be verified: Is solid accepted? Can you throw up one more frame?*/
        if (pad > 0 && sent - got < pad) {
            size_t sp = next_start(bs, fsz, 0);
            size_t se = next_start(bs, fsz, sp + 4);
            if (se > sp && se - sp < 256) {     /* SPS is small, anti-crossing. */
                reap_out(fd, out);
                int oi = -1;
                for (int k = 0; k < n_out; k++)
                    if (!out[k].queued) { oi = k; break; }
                if (oi >= 0) {
                    memcpy(out[oi].map, bs + sp, se - sp);
                    /* PTS Fills the unit with a 0-point mark to enable it to identify whether it produces an output frame */
                    if (qbuf(fd, ot, oi, &out[oi], se - sp, 0) == 0)
                        printf("    [Casualties] %zu bytes SPS, Fly! %d -> %d\n",
                               se - sp, sent - got, sent - got + 1);
                }
            }
        }

        if (depth > 0) {
            /* Maintained depth flying: continued at a depth, only one frame was reached.
             * The incident still needs to be addressed in a timely manner, otherwise the PORT_SETTENGS will not be issued SESSION_CONTINUE. */
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
                /* A non-stop DQBUF is first tried: if the frame is ready, you do not have to enter the poll.
                 * After 250, POLLIN was often delayed, the poll was empty and timed out.
                 * The frame interval is dragged into the poll overtime (111ms 100m, 41ms 20ms).*/
                int ci0 = -1; uint64_t pts0 = 0;
                if (deq_cap(fd, cap, &ci0, &pts0) == 1) {
                    got++;
                    int64_t nm = now_ms();
                    if (last_frame_ms) {
                        int64_t g = nm - last_frame_ms;
                        if (g > 33) { slow_frames++;
                          if (slow_frames <= 12)
                            printf("    [frame %d] Interval %lld ms (Straight)\n", got, (long long)g); }
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
                                printf("    [frame %d] Interval %lld ms (>33)\n",
                                       got, (long long)gap);
                        }
                    }
                    last_frame_ms = nowm;
                    qbuf(fd, ct, ci, &cap[ci], 0, 0);
                } else break;
            }
        } else if (slow) {
            /* Simulate Firefox: Send one frame and wait for one frame, at most 200ms */
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
                    qbuf(fd, ct, ci, &cap[ci], 0, 0);   /* Give it back now. */
                    break;
                }
                if (now_ms() > deadline) break;
            }
        } else {
            /* Fast: Just event processing, not frame */
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

    /* End of story: A little more time. */
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
    printf("\n=== result(%s)===\n", slow ? "slow" : "fast");
    printf("Send In %d Module, received %d frame, Time-consuming %lld ms (%.1f fps)\n",
           sent, got, (long long)el, el > 0 ? got * 1000.0 / el : 0);
    printf("OUTPUT It fell. 0 Number: %d\n", zero_out_events);
    printf("Frame interval >33ms: %d / %d (%.1f%%)\n", slow_frames, got,
           got > 0 ? 100.0 * slow_frames / got : 0.0);
    if (gap_n > 10) {
        /* The bubble is too slow. Use qsort. */
        int cmp(const void*a,const void*b){long x=*(long*)a,y=*(long*)b;return x<y?-1:x>y;}
        qsort(gap_hist, gap_n, sizeof(long), cmp);
        long sum=0; for(int i=0;i<gap_n;i++) sum+=gap_hist[i];
        printf("Interval(us): Centre %ld  Mean %ld  p10 %ld  p90 %ld  Minimum %ld\n",
               gap_hist[gap_n/2], sum/gap_n, gap_hist[gap_n/10],
               gap_hist[gap_n*9/10], gap_hist[0]);
        printf("  Ideals. 33333us,Medium Difference %+ld us (%.1f%%)\n",
               gap_hist[gap_n/2]-33333, 100.0*(gap_hist[gap_n/2]-33333)/33333);
        printf("  p95 %ld  p99 %ld  Max %ld us\n",
               gap_hist[gap_n*95/100], gap_hist[gap_n*99/100], gap_hist[gap_n-1]);
        /* Cumulative time-over: the sum of all parts above the desired value equals the total lag time.
         * That's the amount that determines "the browser determines the lag and jumps the key frame." */
        long over=0; int n_over=0;
        for(int i=0;i<gap_n;i++) if(gap_hist[i]>33333){over+=gap_hist[i]-33333;n_over++;}
        printf("  Cumulative backwardness %ld us (%.2f Frame duration), From %d frame\n",
               over, (double)over/33333, n_over);
    }

    ioctl(fd, VIDIOC_STREAMOFF, &ot);
    ioctl(fd, VIDIOC_STREAMOFF, &ct);
    close(fd);
    close(ionfd);
    return got > 0 ? 0 : 1;
}
