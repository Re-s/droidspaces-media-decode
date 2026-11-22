/*
 * v4l2_backend — V4L2 M2M is directly decoded backend is realized.
 *
 * Process and constants are all measured from the SM/v4l2_dec_probe.c (the probe is on SM8750)
 * Dismantled 13 frames, pixels match ffmpeg soft_points by 100%. What we do here is a one-time probe.
 * Collapse into a state machine that can be reused by daemon, with consistent behaviour.
 *
 * Design cut-off:
 *
 * - No zero copy. The dmabuf map on the CAPTURE side is sent to the upper layer after the user space.
 *   Because the current SM slot/ socket is all by "get a byte."
 *   Designed. The zero copy requires dmabuf fd to pass through the container, which is another subject.
 *
 * - buffer number-driven values, not demanding. REQBUFS changes count to minimum number requested for driver
 *   (on-site CAPTURE side request 8 is detected, request 24 may be downgraded) and is available.
 *
 * - All bugs are returned -1 and left to callers close, not to make a partial rollback -
 *   The life cycle of the session level resources is taken into account by dmd_v4l2_close.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "v4l2_backend.h"
#include "sandbox_heap.h"
#include "video_stats.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Vendor-private incident base site (include/uapi/linux/videodev2.h:2290).
 * glibc Videodev2.h of the packaging/host does not have these macros, which are self-defined. */
#ifndef V4L2_EVENT_MSM_VIDC_START
#define V4L2_EVENT_MSM_VIDC_START  (V4L2_EVENT_PRIVATE_START + 0x00001000)
#endif
#define DMD_EV_MSM_VIDC(n)  (V4L2_EVENT_MSM_VIDC_START + (n))

/* (videodev2.h:1991): Allowing in_reconfig to continue the conversation after the entry into force of the cf. */
#define DMD_V4L2_QCOM_CMD_SESSION_CONTINUE  5
/* Private Flush command. In_reconfig only Flush CAPTURE (Producer OMX)
 * EXECute_omx_flush) also removes the queued EDB on the flush input side. */
#define DMD_V4L2_QCOM_CMD_FLUSH             4
#define DMD_V4L2_QCOM_CMD_FLUSH_CAPTURE     (1 << 1)

#ifndef V4L2_PIX_FMT_AV1
#define V4L2_PIX_FMT_AV1 v4l2_fourcc('A', 'V', '1', '0')
#endif
#ifndef V4L2_PIX_FMT_HEVC
#define V4L2_PIX_FMT_HEVC v4l2_fourcc('H', 'E', 'V', 'C')
#endif
#ifndef V4L2_PIX_FMT_VP9
#define V4L2_PIX_FMT_VP9 v4l2_fourcc('V', 'P', '9', '0')
#endif

/* dma-heap definition of octl. linux/dma-heap.h is missing in part sysroot,
 * Direct in-line -- this is stable inner core ABI. */
struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0, struct dma_heap_allocation_data)

/* Decoding node. The video32= decoding, video33=coding (video0/1 is camera) on msm_vidc
 * It is more secure to use detection than hard encoding, but it is measured that the platform is fixed and is moved at fixed values.
 * Probe failed by passing through it. */
#define DEC_NODE_PRIMARY "/dev/video32"

/* Log: daemon's dlog is state, which is not easy to use here, instead of stderr.
 * The stderr of the daemon itself has been redirected to the log file and is set consistently. */
#define V4L2_LOG(fmt, ...) \
    fprintf(stderr, "[v4l2] " fmt "\n", ##__VA_ARGS__)

/* codec_id → V4L2 fourcc。
 *
 * Vidico_ENUM_FMT fielding of /dev/video32 confirmed that the list of OTPUT side names is as follows:
 *   [0] MPG2  [1] H264  [2] HEVC  [3] VP80  [4] VP90
 * （driver=msm_vidc_driver card=msm_vidc_vdec）
 *
 * ⁇  0.4.2: VP8 added to the map. The previous note said "VP8 is not in the drive list," but it's not working.
 *
 * ⁇ 0.4.7 (This device, kernel 6.6 new msm_vidc): the kernel's enumeration becomes
 *   H264 / HEVC / VP90 / HEIC / AV10 — MPG2 and VP80 ** Not in column**
 * (S_FMT sent in "unsupported codec" and silently returned to H.264, see dmesg),
 * And AV1 sourcecc is v42_fourcc ("A', 'V', '1', '0') **not standard**,
 * V4L2_PIX_FMT_AV1 ("AV01") is not the main line. Sending 'AV01 'is considered unsupported.
 * Encoding denied. So AV1 is selected by the device and is listed in two candidates.
 * dmd_v4l2_nick_fourhourcc), probe and open must choose the result. */
static uint32_t codec_to_fourcc(int codec_id)
{
    switch (codec_id) {
    case DMD_V4L2_CODEC_H264: return V4L2_PIX_FMT_H264;
    case DMD_V4L2_CODEC_HEVC: return V4L2_PIX_FMT_HEVC;
    case DMD_V4L2_CODEC_VP9:  return V4L2_PIX_FMT_VP9;
    case DMD_V4L2_CODEC_VP8:  return V4L2_PIX_FMT_VP8;
    case DMD_V4L2_CODEC_AV1:  return V4L2_PIX_FMT_AV1;
    case DMD_V4L2_CODEC_MPEG2: return V4L2_PIX_FMT_MPEG2;
    default:                  return 0;
    }
}

static int xioctl(int fd, unsigned long req, void *arg, const char *name)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        V4L2_LOG("%s failed: %s", name, strerror(errno));
    return r;
}

/* - - - - - - - - - - - - - - - - dmabuf distribution */

/* DMABUF Source: dma-heap priority, Ion-Back.
 *
 * Why do you need two roads (81-84 round of live measurements):
 *   Equipment with dma-heap (kernel 5.x) /dev/dma_head/system
 *   Mi-pixel 5/ nabu(kernel 4.14) ** No dma_heap subsystem**
 *                                     (/ sys/clas/ dma_heap/ does not exist)
 *                                     /dev/ion only
 *
 * ⁇  Round 81 I decided "no Ion of nabu" was not available, and that conclusion was wrong**.
 * The method was to try heap_id_mask from one to 0x80 -- and this device is a device that is a tool that can be used to create a new system.
 * The system head id is **25**(mask = 1 < 25), is not at all part of the field of inquiry.
 * The right thing is to ask the insider what heap first.
 *
 * Actual (Android side root fully matches the result of the master user in the container):
 *     Heap = 9
 *     id=27 qsecom          id=26 user_contig     id=25 system
 *     id=22 adsp            id=19 qsecom_ta       id=14 secure_carveout
 *     id=13 spss            id=10 secure_display  id=9  secure_heap
 * The id=25 distribution of 3133440 bytes (1080 p NV12 one) was successful and is mmap.
 *
 * Lessons: the difference between ENODEV and EINVAL is informative - the modern ABI returns ENODEV
 * (The kernel knows this octl, but there's no heap, and Legacy ABI returns EINVAL
 * I lost this clue when I thought both were "unsupported." */

enum { HEAP_NONE = 0, HEAP_DMA_HEAP, HEAP_ION };

/* Ion's modern ABI (4.12 unison interfaces, nabu's 4.14 with this). */
struct ion_alloc_data {
    uint64_t len;
    uint32_t heap_id_mask;
    uint32_t flags;
    uint32_t fd;
    uint32_t unused;
};
#define ION_IOC_ALLOC_MODERN _IOWR('I', 0, struct ion_alloc_data)

struct ion_heap_data {
    char     name[32];
    uint32_t type;
    uint32_t heap_id;
    uint32_t reserved0, reserved1, reserved2;
};
struct ion_heap_query_data {
    uint32_t cnt;
    uint32_t reserved0;
    uint64_t heaps;          /* User state address to the ion_heap_data array */
    uint32_t reserved1, reserved2;
};
#define ION_IOC_HEAP_QUERY _IOWR('I', 8, struct ion_heap_query_data)

/* Ask the kernel for system header mask. Failed to return 0. */
static uint32_t ion_system_mask(int fd)
{
    struct ion_heap_query_data q;
    memset(&q, 0, sizeof(q));
    if (ioctl(fd, ION_IOC_HEAP_QUERY, &q) != 0 || q.cnt == 0 || q.cnt > 64) {
        V4L2_LOG("ION HEAP_QUERY failed or returned an invalid heap count (%u): %s", q.cnt,
                 strerror(errno));
        return 0;
    }

    struct ion_heap_data hd[64];
    memset(hd, 0, sizeof(hd));
    q.heaps = (uint64_t)(uintptr_t)hd;
    if (ioctl(fd, ION_IOC_HEAP_QUERY, &q) != 0) {
        V4L2_LOG("ION HEAP_QUERY failed to fetch heap data: %s", strerror(errno));
        return 0;
    }

    /* Ion_HEAP_TYPE_SYSTEM=0. Priority is given to identifying names by type, only for the bottom-up.
     * The type is part of ABI, the name is from the manufacturer. */
    for (unsigned i = 0; i < q.cnt; i++) {
        if (hd[i].type == 0) {
            V4L2_LOG("ION system heap: id=%u name=%s", hd[i].heap_id,
                     hd[i].name);
            return 1u << (hd[i].heap_id & 31u);
        }
    }
    for (unsigned i = 0; i < q.cnt; i++) {
        if (strncmp(hd[i].name, "system", sizeof(hd[i].name)) == 0) {
            V4L2_LOG("ION system heap matched by name: id=%u", hd[i].heap_id);
            return 1u << (hd[i].heap_id & 31u);
        }
    }
    V4L2_LOG("ION reported %u heaps with no system heap", q.cnt);
    return 0;
}

static int heap_open(struct dmd_v4l2_dec *d)
{
    if (d->heap_fd >= 0) return 0;

    d->heap_fd = dmd_sandbox_system_heap_dup();
    if (d->heap_fd >= 0) {
        d->heap_kind = HEAP_DMA_HEAP;
        V4L2_LOG("buffer source: preopened /dev/dma_heap/system");
        return 0;
    }

    d->heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (d->heap_fd >= 0) {
        d->heap_kind = HEAP_DMA_HEAP;
        V4L2_LOG("buffer source: /dev/dma_heap/system");
        return 0;
    }
    const int dh_errno = errno;

    d->heap_fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
    if (d->heap_fd >= 0) {
        d->ion_mask = ion_system_mask(d->heap_fd);
        if (d->ion_mask == 0) {
            close(d->heap_fd);
            d->heap_fd = -1;
            V4L2_LOG("No usable ION system heap");
            return -1;
        }
        d->heap_kind = HEAP_ION;
        V4L2_LOG("buffer source: /dev/ion mask=0x%x (dma_heap unavailable: %s)",
                 d->ion_mask, strerror(dh_errno));
        return 0;
    }

    V4L2_LOG("No usable DMA-BUF allocator: dma_heap=%s ion=%s",
             strerror(dh_errno), strerror(errno));
    return -1;
}

static int dmabuf_alloc(struct dmd_v4l2_dec *d, size_t len)
{
    if (heap_open(d) < 0) return -1;

    if (d->heap_kind == HEAP_DMA_HEAP) {
        struct dma_heap_allocation_data a;
        memset(&a, 0, sizeof(a));
        a.len = len;
        a.fd_flags = O_RDWR | O_CLOEXEC;
        if (ioctl(d->heap_fd, DMA_HEAP_IOCTL_ALLOC, &a) < 0) {
            V4L2_LOG("dma_heap could not allocate %zu bytes: %s", len, strerror(errno));
            return -1;
        }
        return (int)a.fd;
    }

    struct ion_alloc_data a;
    memset(&a, 0, sizeof(a));
    a.len = len;
    a.heap_id_mask = d->ion_mask;
    if (ioctl(d->heap_fd, ION_IOC_ALLOC_MODERN, &a) < 0) {
        V4L2_LOG("ION could not allocate %zu bytes (mask=0x%x): %s", len, d->ion_mask,
                 strerror(errno));
        return -1;
    }
    return (int)a.fd;
}

static void bufs_free(struct dmd_v4l2_buf *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (b[i].map && b[i].map != MAP_FAILED)
            munmap(b[i].map, b[i].length);
        if (b[i].dbuf_fd >= 0)
            close(b[i].dbuf_fd);
        b[i].map = NULL;
        b[i].dbuf_fd = -1;
        b[i].length = 0;
        b[i].queued = 0;
    }
}

static int bufs_alloc(struct dmd_v4l2_dec *d, struct dmd_v4l2_buf *b, int n,
                      size_t size, int need_map)
{
    for (int i = 0; i < n; i++) {
        b[i].dbuf_fd = dmabuf_alloc(d, size);
        if (b[i].dbuf_fd < 0) return -1;
        b[i].length = size;
        b[i].queued = 0;
        b[i].map = NULL;
        if (need_map) {
            void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                           b[i].dbuf_fd, 0);
            if (m == MAP_FAILED) {
                V4L2_LOG("mmap failed for dmabuf[%d] (%zu bytes): %s", i, size,
                         strerror(errno));
                return -1;
            }
            b[i].map = m;
        }
    }
    return 0;
}

/* The buffer source for msm_vidc is dmabuf fd, the difference is only in memory mode in queue:
 * The old kernel (nabu/4.14) only accepts V4L2_MEMORY_USERPTR, but that's a nominal value --
 * The driver vb2_mem_ops.get_userptr is a stump function that returns 0xdeadbeef
 * (Msm_vidc.c: 717-720), the real source of buffers is
 *     b->m.planes[i].m.fd = b->m.planes[i].reserved[0];   (msm_vidc.c:533-536)
 * That is, dmabuf fd must be written into plain.reserved [0];
 * The strangulation of the new core (Android12+/kernel 5.x+) is a direct rejection of USERPTR by msm_vidc.
 * REQBUFS accepts only V4L2_MEMORY_DMABUF, fd walk plain.m.fd.
 * Therefore, the DMABUF is tried and rejected before returning the USERPTR (see dmd_v4l2_open). */
static int req_bufs(int fd, enum v4l2_buf_type type, int count,
                    enum v4l2_memory mem)
{
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = (unsigned)count;
    rb.type = type;
    rb.memory = mem;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb, "REQBUFS") < 0) return -1;
    return (int)rb.count;
}

/* dmabuf fd at extra_fd > = 0,
 * Extra_len is its length. The CAPTURE side is a num_planes 2 after setting the EXTRADATA control. */
static int qbuf_userptr(int fd, enum v4l2_buf_type type, int index,
                        struct dmd_v4l2_buf *b, unsigned bytesused,
                        uint64_t pts_us, int extra_fd, unsigned extra_len,
                        enum v4l2_memory mem)
{
    struct v4l2_buffer v;
    struct v4l2_plane p[VIDEO_MAX_PLANES];
    const int dmabuf = (mem == V4L2_MEMORY_DMABUF);
    memset(&v, 0, sizeof(v));
    memset(p, 0, sizeof(p));
    v.type = type;
    v.memory = mem;
    v.index = (unsigned)index;
    v.m.planes = p;
    v.length = (extra_fd >= 0) ? 2 : 1;
    if (dmabuf) {
        p[0].m.fd = (int)b->dbuf_fd;
    } else {
        /* Old inner core: USERPTR nominal + fd Go Reserved [0] */
        p[0].reserved[0] = (unsigned)b->dbuf_fd;
    }
    p[0].length = (unsigned)b->length;
    p[0].bytesused = bytesused;
    if (extra_fd >= 0) {
        if (dmabuf) {
            p[1].m.fd = extra_fd;
        } else {
            p[1].reserved[0] = (unsigned)extra_fd;
        }
        p[1].length = extra_len ? extra_len : 16384;
    }
    if (pts_us) {
        v.timestamp.tv_sec = (long)(pts_us / 1000000ULL);
        v.timestamp.tv_usec = (long)(pts_us % 1000000ULL);
    }
    if (xioctl(fd, VIDIOC_QBUF, &v, "QBUF") < 0) return -1;
    b->queued = 1;
    return 0;
}

/* ------------------------------------------------------------------ probe */

int dmd_v4l2_probe(int codec_id)
{
    uint32_t want = dmd_v4l2_pick_fourcc(codec_id);
    if (!want) return 0;

    return 1;   /* Pick_fourcc confirmed that the format is in the list */
}

/* Codec_id — device actually supported sourcecc.
 *
 * Select the following list of the figures by device ODIOTOC_ENUM_FMT on the side of the device: kernel in the main line
 * Use V4L2_PIX_FMT_AV1('AV01'), this device (kernel 6.6 new msm_vidc) is non-standard
 * The rest of the codec only has one standard candidate, equal to the direct check, but it's a flat-rate move.
 * The list can be given as defined "not supported" when the solid is not supported (e.g., this device MPG2/VP80).
 * Instead of waiting until S_FMT returns silently H.264 to remove the trash. returns 0 = not supported. */
uint32_t dmd_v4l2_pick_fourcc(int codec_id)
{
    uint32_t cands[2] = { codec_to_fourcc(codec_id), 0 };
    if (codec_id == DMD_V4L2_CODEC_AV1)
        cands[1] = v4l2_fourcc('A', 'V', '1', '0');
    if (!cands[0]) return 0;

    int fd = open(DEC_NODE_PRIMARY, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;

    uint32_t found = 0;
    for (unsigned i = 0; i < 64 && !found; i++) {
        struct v4l2_fmtdesc fd_desc;
        memset(&fd_desc, 0, sizeof(fd_desc));
        fd_desc.index = i;
        fd_desc.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fd_desc) < 0) break;
        for (int k = 0; k < 2; k++)
            if (cands[k] && fd_desc.pixelformat == cands[k])
                found = cands[k];
    }
    close(fd);
    return found;
}

static int setup_capture(struct dmd_v4l2_dec *d);
static int cap_reconfig(struct dmd_v4l2_dec *d);

/* New Qualcomm ABI reuses legacy numeric CIDs for unrelated metadata.
 * A name/type match identifies the ABI even if that control is disabled:
 * never send old EXTRADATA/SECONDARY values to known modern metadata CIDs. */
static int query_named_control(int fd, uint32_t id, const char *name,
                               uint32_t type, struct v4l2_queryctrl *query)
{
    memset(query, 0, sizeof(*query));
    query->id = id;
    return ioctl(fd, VIDIOC_QUERYCTRL, query) == 0 && query->id == id &&
           query->type == type && memchr(query->name, 0, sizeof(query->name)) &&
           !strcmp((const char *)query->name, name);
}

static int control_can_write_value(const struct v4l2_queryctrl *query, int value)
{
    const uint32_t blocked = V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY |
                             V4L2_CTRL_FLAG_INACTIVE | V4L2_CTRL_FLAG_GRABBED;
    return !(query->flags & blocked) && query->minimum <= value &&
           query->maximum >= value && query->step > 0 &&
           ((int64_t)value - query->minimum) % query->step == 0;
}

/* Return -1 only for the legacy required SECONDARY control failure. Optional
 * delay requests retain their diagnostic, nonfatal failure behavior; 0 alone
 * must not be used as evidence that the hardware outputs in decode order. */
static int configure_decode_controls(int fd)
{
    struct v4l2_queryctrl latency;
    const int modern = query_named_control(fd, 0x00992003, "LOWLATENCY_MODE",
                                           V4L2_CTRL_TYPE_INTEGER, &latency);
    if (modern) {
        /* 0x992003 is LOWLATENCY, not legacy OUTPUT_ORDER. The other legacy
         * IDs are BLUR_RESOLUTION / output-fence / OPB-MISR metadata here. */
        V4L2_LOG("Detected the modern msm_vidc control ABI. Skipping legacy EXTRADATA and SECONDARY controls");
        if (control_can_write_value(&latency, 1)) {
            struct v4l2_control ctl = { .id = 0x00992003, .value = 1 };
            if (ioctl(fd, VIDIOC_S_CTRL, &ctl) < 0)
                V4L2_LOG("S_CTRL LOWLATENCY_MODE(0x%x)=1 failed: %s",
                         ctl.id, strerror(errno));
        }
    } else {
        /* msm_vidc private control entry. The CID value is from the manufacturer uapi header
         * include/uapi/linux/v4l2-controls.h:684
         *   V4L2_CID_MPEG_MSM_VIDC_BASE = V4L2_CTRL_CLASS_MPEG|0x2000 = 0x00992000
         * In contrast to the OMX HAL living body strace (pid omx@1.0-service) item by item: */
        struct v4l2_control ctl;
        static const struct { uint32_t id; int32_t val; const char *nm; } inits[] = {
            /* OTPUT_ORDER: 0 = display order (re-altered output) 1 = de-coding order (not with different re-alterations).
             *
             * ⁇  Must be decoded (1) otherwise the browser cannot be hardened.
             *
             * Research/slowfeed.c direct to V4L2 field frame delay (1080 p H.264):
             *     OPPUT_ORDER=0 (show order)  ⁇  Full of 7 units, 8 is the first frame
             *     OTPUT_ORDER=1 (decoding sequence) — Send 4 units, the 5th is the first frame
             * The poor 3 frames are the cost of showing the sequenced reordering.
             *
             * Why does this determine whether the browser can work: Firefox, steady state only, 5 frames are flying
             * (Verified pending consistently is 5, 99.9%, while showing the sequence lags 7 > 5,
             * So it's just waiting for the fifth frame, and the decoder still owes two frames -- two frames for each other.
             * SyncSurface can only be released time-consuming, falsely reported, Firefox takes an unfilled buffer:
             * Test 847 decoding / 792 discard = 93.5%, the image is almost static.
             * It's a structural contradiction, reconciling DMD_PIPELINE_DEPTH -- 12/4/7 all three values tried.
             * Each frame is released (93.5% drop), each frame is blocked 5s timed out (fps 0.4, session re-established),
             * The frame is still released per frame (91.8% to the ground).
             *
             * The frame sequence becomes decoded without prejudice to the correctness of the frame content: pairing v4l2_buffer.timestamp
             * Bringing unit_seq with exact matching (see decode.c's dmd_pending_take_locked),
             * This is independent of output order, as shown by the five bit-exact FFmpeg regression streams,
             * Specialized B frame sequence check stream (research/perf/order.mp4,220 B frames,
             * The smooth-speed transverse squares (slips at 300 frames below the ffmpeg path) and match the soft-dismantled frame by zero.
             *
             * ⁇  But the browser path has the cost of the following (Firefox 140, order_long.mp4, 20s):
             *     ORDER=0 display order: MediaTime returns 0, but drop frame 99.79%
             *                     950 decoding/948 discard, 20s mission escape 62s, not available
             *     ORDER=1 decoding sequence: MediaTime retreat 2/465 = 0.43%, drop frame 4.17%
             * The regression is just a frame (33ms) that Firefox has occasionally changed position by going on screen himself.
             * Not pair error -- Sync only returns when the requested surface is ready.
             * The best solution under the current hardware is to trade the detectable but minimal sequence of shaking for available playbacks:
             * Show sequence lags 7 > Firefox flying 5 and waiting for each other cannot be eliminated by software (LOWLATENCY)
             * Switches have no effect on display order lag, and slowfeed measured both settings. */
            { 0x00992003, 1,  "OUTPUT_ORDER"    },  /* Base+3, 1 = decoding */
            { 0x00992011, 2,  "EXTRADATA"       },  /* Base+17, Menu, HAL four-by-four */
            { 0x00992011, 25, "EXTRADATA"       },
            { 0x00992011, 31, "EXTRADATA"       },
            { 0x00992011, 29, "EXTRADATA"       },
            { 0x00992038, 1,  "LOWLATENCY_MODE" },  /* base+56 */
        };
        for (unsigned i = 0; i < sizeof(inits) / sizeof(inits[0]); i++) {
            memset(&ctl, 0, sizeof(ctl));
            ctl.id = inits[i].id;
            ctl.value = inits[i].val;
            if (ioctl(fd, VIDIOC_S_CTRL, &ctl) < 0)
                V4L2_LOG("S_CTRL %s(0x%x)=%d failed: %s", inits[i].nm,
                         inits[i].id, inits[i].val, strerror(errno));
        }

        /* Decisive one: the SECANDRY diversion mode must be enabled.
         *   V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_MODE = base+22 = 0x00992016
         *   V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_SECONDARY = 1
         * CAPTURE under PRIMARY (default) directly returns DPB, start_streaming()
         * -EINVAL. SECONDARY Enables HAL_BUFER_OUTPUT2, DPB separate from OPB
         * (msm_vidc.c: 1214-1222 will be extra adjusted msm_comm_set_output_buffers()
         * Lets the driver assign the DPB himself, and the path is verified differently.
         * Single variable measured against: SECONDARY  ⁇  OK; PRIMARY  ⁇  EINVAL.
         * Accompanied benefit: CAPTURE can use linear NV12 without UBWC Q128. */
        memset(&ctl, 0, sizeof(ctl));
        ctl.id = 0x00992016;
        ctl.value = 1;
        if (ioctl(fd, VIDIOC_S_CTRL, &ctl) < 0) {
            V4L2_LOG("fatal: STREAM_OUTPUT_MODE=SECONDARY failed: %s", strerror(errno));
            return -1;
        }
    }

    const char *delay_env = getenv("DMD_V4L2_DISPLAY_DELAY");
    int request_delay = 0;
    if (delay_env) {
        if (!strcmp(delay_env, "0")) {
            V4L2_LOG("DMD_V4L2_DISPLAY_DELAY=0 disables DISPLAY_DELAY requests");
        } else if (!strcmp(delay_env, "1")) {
            request_delay = 1; /* Explicit diagnostic request on any ABI. */
        } else {
            V4L2_LOG("DMD_V4L2_DISPLAY_DELAY accepts only 0 or 1. Leaving DISPLAY_DELAY unchanged");
        }
    } else if (modern) {
        struct v4l2_queryctrl enable, delay;
        request_delay = control_can_write_value(&latency, 1) &&
            query_named_control(fd,
                V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE,
                "Display Delay Enable", V4L2_CTRL_TYPE_BOOLEAN, &enable) &&
            control_can_write_value(&enable, 1) &&
            query_named_control(fd, V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY,
                "Display Delay", V4L2_CTRL_TYPE_INTEGER, &delay) &&
            control_can_write_value(&delay, 0);
        if (!request_delay)
            V4L2_LOG("Could not confirm that the DISPLAY_DELAY controls are writable. Skipping automatic configuration");
    }

    /* AHMETBAYIR identified the standard display-delay controls for decode
     * order on SM8750 and the legacy control-ID collision in upstream #6:
     * https://github.com/Re-s/droidspaces-media-decode/issues/6
     *
     * Must run after S_FMT(OUTPUT), BEFORE the first REQBUFS(OUTPUT).
     * Qualcomm queue_setup adjusts DISPLAY_DELAY_ENABLE=1 + DELAY=0 into
     * internal OUTPUT_ORDER=1; STREAMON sends the derived value, not a fresh
     * calculation. Setting these after REQBUFS silently leaves output order 0.
     * Official source, commit a2883c1e49efae9c7cefe038eb1a7a9864a5e6eb:
     * driver/vidc/src/msm_vidc_vb2.c msm_vb2_adjust_properties()/queue_setup(),
     * driver/platform/common/src/msm_vidc_platform.c msm_vidc_adjust_output_order().
     * nabu/4.14 rejects these controls; only positively identified modern
     * controls are automatic. Explicit env=1 preserves legacy diagnostics. */
    if (request_delay) {
        struct v4l2_control ctl = {
            .id = V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE, .value = 1
        };
        if (ioctl(fd, VIDIOC_S_CTRL, &ctl) == 0) {
            ctl.id = V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY;
            ctl.value = 0;
            if (ioctl(fd, VIDIOC_S_CTRL, &ctl) == 0)
                V4L2_LOG("requested DISPLAY_DELAY=0 (decode-order output)");
            else
                V4L2_LOG("DISPLAY_DELAY failed. Keeping default output reordering: %s", strerror(errno));
        } else {
            V4L2_LOG("DISPLAY_DELAY_ENABLE failed. Keeping default output reordering: %s", strerror(errno));
        }
    }
    return 0;
}

/* ------------------------------------------------------------------- open */

int dmd_v4l2_open(struct dmd_v4l2_dec *d, int codec_id, int w, int h)
{
    memset(d, 0, sizeof(*d));
    d->fd = -1;
    d->heap_fd = -1;
    d->heap_kind = HEAP_NONE;
    d->ion_mask = 0;
    for (int i = 0; i < DMD_V4L2_MAX_OUT; i++) d->out[i].dbuf_fd = -1;
    for (int i = 0; i < DMD_V4L2_MAX_CAP; i++) {
        d->cap[i].dbuf_fd = -1;
        /* ⁇ extra cannot leave: the memset above stops it at 0, dmd_v4l2_close unconditionally
         * Buffs_free(d->extra, MAX_CAP), so close (0) 24 times each session.
         * Strace Actual: Success 1 (turn off host process stdin), later 23 EBADF.
         * ffmpeg only stole fd 0; Chrome 's GPU process
         * Billing, close() intercepted by chromium and checked for belonging
         * （base/files/scoped_file_linux.cc: IsFDOwned(fd) → CrashOnFdOwnership
         * Violation), which immediately exits with code 5. */
        d->extra[i].dbuf_fd = -1;
    }

    uint32_t fourcc = dmd_v4l2_pick_fourcc(codec_id);
    if (!fourcc) {
        V4L2_LOG("codec_id=%d is not in the device OUTPUT format list", codec_id);
        return -1;
    }

    /* O_NONBLOCK is mandatory: event queues are empty and the nucleician core is blocked by the Quevenet.
     * vl2_event_dequeue((as confirmed by proc/PID/ wchan), poll not re-entering. */
    d->fd = open(DEC_NODE_PRIMARY, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (d->fd < 0) {
        V4L2_LOG("Could not open %s: %s", DEC_NODE_PRIMARY, strerror(errno));
        return -1;
    }

    /* UTPUT Side: Tell the driver what the stream is feeding us. */
    struct v4l2_format f;

    /* S_FMT (OUTPUT) from msm_vidc returns early when the resolution is the same as the current value.
     * Skip inst->bufq [OUTPUT_PORT]. plain_sizes:
     *   If (fourcc is the same & & west is the same & height) {
     *       dprintk("No change in OUTPUT port params"); return 0;
     *   }                                    (msm_vdec.c:732-738)
     * The OUTPUT default is 1920x1088 after driving open, so direct target resolution hits it.
     * The size of the block always returns 0. First, make a different resolution and break the number of dummy S_FMT.
     * (b) Actual: direct 1920x1088  ⁇  sizeimage=0;
     *       First 1280x720 and then 1920x1088  ⁇  sizeimage=1658800. */
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.pixelformat = fourcc;
    f.fmt.pix_mp.width = (unsigned)((w == 1280 && h == 720) ? 640 : 1280);
    f.fmt.pix_mp.height = (unsigned)((w == 1280 && h == 720) ? 480 : 720);
    f.fmt.pix_mp.num_planes = 1;
    ioctl(d->fd, VIDIOC_S_FMT, &f);   /* Dummy, ignore the result. */

    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.pixelformat = fourcc;
    f.fmt.pix_mp.width = (unsigned)w;
    f.fmt.pix_mp.height = (unsigned)h;
    f.fmt.pix_mp.num_planes = 1;
    if (xioctl(d->fd, VIDIOC_S_FMT, &f, "S_FMT(OUTPUT)") < 0) goto fail;
    d->out_w = (int)f.fmt.pix_mp.width;
    d->out_h = (int)f.fmt.pix_mp.height;
    d->in_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    if (!d->in_size) {
        V4L2_LOG("Warning: sizeimage remains 0 after the initial S_FMT probe. Using 2 MB");
        d->in_size = 2u * 1024u * 1024u;
    }
    V4L2_LOG("S_FMT(OUTPUT) OK: %ux%u sizeimage=%u", f.fmt.pix_mp.width,
             f.fmt.pix_mp.height, d->in_size);

    /* Frame rate: msm_comm_get_mbs_per_sec() calculates loads with inst->prop.fps. */
    struct v4l2_streamparm sp;
    memset(&sp, 0, sizeof(sp));
    sp.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    sp.parm.output.timeperframe.numerator = 1;
    sp.parm.output.timeperframe.denominator = 30;
    ioctl(d->fd, VIDIOC_S_PARM, &sp);

    if (configure_decode_controls(d->fd) < 0) goto fail;

    /* Subscription event. Key: msm_vidc uses ** private vendor event**,
     * Never issue standard V4L2_EVENT_SOURCE_CHANGE.
     *   V4L2_EVENT_MSM_VIDC_START = V4L2_EVENT_PRIVATE_START + 0x1000
     *                             = 0x08001000        (videodev2.h:2290)
     *     +1 FLUSH_DONE   +2 PORT_SETTINGS_SUFFICIENT
     *     +3 PORT_SETTINGS_INSUFFICIENT   +4 BITDEPTH_CHANGED
     *     +5 SYS_ERROR    +6 RELEASE_BUFFER_REFERENCE
     *     +7 RELEASE_UNQUEUED_BUFFER
     * Subscription only to the standard SOURCE_CHANGE will lead to the poll never returning to POLLPRI. */
    struct v4l2_event_subscription sub;
    static const unsigned evs[] = {
        DMD_EV_MSM_VIDC(1), DMD_EV_MSM_VIDC(2), DMD_EV_MSM_VIDC(3),
        DMD_EV_MSM_VIDC(4), DMD_EV_MSM_VIDC(5), DMD_EV_MSM_VIDC(6),
        DMD_EV_MSM_VIDC(7), V4L2_EVENT_SOURCE_CHANGE, V4L2_EVENT_EOS,
    };
    for (unsigned i = 0; i < sizeof(evs) / sizeof(evs[0]); i++) {
        memset(&sub, 0, sizeof(sub));
        sub.type = evs[i];
        ioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);   /* Individual failure is not fatal. */
    }

    /* The OTPUT buffer + STREAMON. The CAPTURE side should wait for SOURCE_CHANGE to match. */
    /* Buffer queue memory mode: New kernels msm_vidc only accepts DMABUF, old kernel only
     * USERPTR nominal. DMABUF, REQBUFS rejected and then returned USERPTR. */
    d->buf_mem = V4L2_MEMORY_DMABUF;
    d->n_out = req_bufs(d->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                        DMD_V4L2_MAX_OUT, d->buf_mem);
    if (d->n_out <= 0) {
        V4L2_LOG("REQBUFS(DMABUF) rejected. Falling back to USERPTR");
        d->buf_mem = V4L2_MEMORY_USERPTR;
        d->n_out = req_bufs(d->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                            DMD_V4L2_MAX_OUT, d->buf_mem);
    }
    if (d->n_out <= 0) goto fail;
    if (d->n_out > DMD_V4L2_MAX_OUT) d->n_out = DMD_V4L2_MAX_OUT;
    if (bufs_alloc(d, d->out, d->n_out, d->in_size, 1) < 0) goto fail;

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

/* The timing of the CAPTURE is determined by buffer memory mode (in order still follows the manufacturer 's OX component, see
     * msm_vidc.c:1294-1302, first STREAMON skipping due to non-streaming side
     * Start_streaming(), the second is really going to do all the checks:
     * - Old inner core (USERPTR / SECONDARY mode): CAPTURE press EUTPUT before feeding
     *   (b) The negotiation value prefixing and triggering of the verification of STREAMON (OUTPUT);
     * - New inner core msm_vidc (standard stateful, marked with REQBUFS (DMABUF) accepted):
     *   The standard V4L2_EVENT_SOURCE_CHANGE, must wait until after the event.
     *   G_FMT/S_FMT(CAPTURE) buffer and STREAMON(CAPTURE); pre-packaged stops the solids
     *   Reconfig waits, no vomiting. */
    if (d->buf_mem != V4L2_MEMORY_DMABUF) {
        if (setup_capture(d) < 0) {
            V4L2_LOG("CAPTURE configuration failed");
            goto fail;
        }
    }

    if (xioctl(d->fd, VIDIOC_STREAMON, &type, "STREAMON(OUTPUT)") < 0) goto fail;
    d->out_streaming = 1;

    V4L2_LOG("Session ready: OUTPUT %d x %u bytes, CAPTURE %d x %u bytes",
             d->n_out, d->in_size, d->n_cap, d->cap_size);
    return 0;

fail:
    dmd_v4l2_close(d);
    return -1;
}

/* - - - - - - - - - - - - - - - - Phase II: CAPTURE Consultations */

/*
 * Configure the CAPTURE side after receiving SOURCE_CHANGE.
 * Key: CAPTURE may be a QCOM compression format with a linear NV12/P010,
 * Otherwise, the upper layer cannot be read directly on a half-plane layout.
 */
static int setup_capture(struct dmd_v4l2_dec *d)
{
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(d->fd, VIDIOC_G_FMT, &f, "G_FMT(CAPTURE)") < 0) return -1;

    uint32_t got = f.fmt.pix_mp.pixelformat;
    V4L2_LOG("G_FMT(CAPTURE): %ux%u fourcc=%c%c%c%c sizeimage=%u",
             f.fmt.pix_mp.width, f.fmt.pix_mp.height,
             (char)(got & 0xFF), (char)((got >> 8) & 0xFF),
             (char)((got >> 16) & 0xFF), (char)((got >> 24) & 0xFF),
             f.fmt.pix_mp.plane_fmt[0].sizeimage);

    /* Before the first SOURCE_CHANGE, G_FMT(CAPTURE) can retain the
     * default 1920x1088 even after S_FMT(OUTPUT) accepts 3840x2160.
     * Allocating for that stale size prevented the 4K stream from producing
     * frames and FFmpeg reported an invalid surface parameter.
     *
     * Use the negotiated OUTPUT dimensions during initial setup. msm_vidc
     * has no scaling support, so OUTPUT and CAPTURE geometry must agree.
     * During a midstream change, trust the new CAPTURE geometry instead. */
    if (!d->trust_gfmt && d->out_w > 0 && d->out_h > 0 &&
        ((unsigned)d->out_w != f.fmt.pix_mp.width ||
         (unsigned)d->out_h != f.fmt.pix_mp.height)) {
        V4L2_LOG("Replacing stale CAPTURE geometry %ux%u with negotiated OUTPUT geometry %dx%d",
                 f.fmt.pix_mp.width, f.fmt.pix_mp.height, d->out_w, d->out_h);
        f.fmt.pix_mp.width = (unsigned)d->out_w;
        f.fmt.pix_mp.height = (unsigned)d->out_h;
    }

    /* DMD_PROBE_10BIT=1 requests Venus QP10 CAPTURE to check whether the
     * firmware produces buffers for Main10. QP10 and Q12A are vendor
     * layouts that the normal linear P010 import and export paths cannot read. */
    uint32_t want = d->ten_bit ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
    const char *probe10 = getenv("DMD_PROBE_10BIT");
    /* DMD_PROBE_10BIT=9 probes each candidate, then restores the normal NV12/P010
     * request. A successful S_FMT can establish format acceptance. Verify
     * the final linear format and pixel values separately. */
    if (probe10 && probe10[0] == '9') {
        static const uint32_t cand[] = {
            V4L2_PIX_FMT_NV12,
            V4L2_PIX_FMT_P010,
            v4l2_fourcc('Q', 'P', '1', '0'),
            v4l2_fourcc('Q', '1', '0', 'C'),
            v4l2_fourcc('T', 'P', '1', '0'),
            v4l2_fourcc('Q', '1', '2', 'A'),
            v4l2_fourcc('Q', 'T', '1', '0'),
        };
        for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++) {
            struct v4l2_format t = f;
            t.fmt.pix_mp.width = (unsigned)d->out_w;
            t.fmt.pix_mp.height = (unsigned)d->out_h;
            t.fmt.pix_mp.pixelformat = cand[i];
            if (xioctl(d->fd, VIDIOC_S_FMT, &t, "S_FMT(CAPTURE Detection)") < 0) {
                V4L2_LOG("Probe %c%c%c%c: S_FMT failed",
                         (char)(cand[i] & 0xFF), (char)((cand[i] >> 8) & 0xFF),
                         (char)((cand[i] >> 16) & 0xFF), (char)((cand[i] >> 24) & 0xFF));
                continue;
            }
            V4L2_LOG("Probe requested %c%c%c%c, got %c%c%c%c %ux%u stride=%u size=%u nplanes=%u",
                     (char)(cand[i] & 0xFF), (char)((cand[i] >> 8) & 0xFF),
                     (char)((cand[i] >> 16) & 0xFF), (char)((cand[i] >> 24) & 0xFF),
                     (char)(t.fmt.pix_mp.pixelformat & 0xFF),
                     (char)((t.fmt.pix_mp.pixelformat >> 8) & 0xFF),
                     (char)((t.fmt.pix_mp.pixelformat >> 16) & 0xFF),
                     (char)((t.fmt.pix_mp.pixelformat >> 24) & 0xFF),
                     t.fmt.pix_mp.width, t.fmt.pix_mp.height,
                     t.fmt.pix_mp.plane_fmt[0].bytesperline,
                     t.fmt.pix_mp.plane_fmt[0].sizeimage,
                     t.fmt.pix_mp.num_planes);
        }
        probe10 = NULL;
    }
    if (probe10 && (probe10[0] == '1' || probe10[0] == '2')) {
        want = (probe10[0] == '2') ? v4l2_fourcc('Q', '1', '2', 'A')
                                   : v4l2_fourcc('Q', 'P', '1', '0');
        V4L2_LOG("DMD_PROBE_10BIT=%c: trying CAPTURE format %s for probing only. Output pixels will be incorrect",
                 probe10[0], probe10[0] == '2' ? "Q12A(TP10 UBWC)"
                                               : "QP10(P10 Venus)");
    }
    f.fmt.pix_mp.pixelformat = want;
    if (xioctl(d->fd, VIDIOC_S_FMT, &f, "S_FMT(CAPTURE)") < 0) return -1;
    if (f.fmt.pix_mp.pixelformat != want) {
        V4L2_LOG("Driver changed the CAPTURE format to %c%c%c%c",
                 (char)(f.fmt.pix_mp.pixelformat & 0xFF),
                 (char)((f.fmt.pix_mp.pixelformat >> 8) & 0xFF),
                 (char)((f.fmt.pix_mp.pixelformat >> 16) & 0xFF),
                 (char)((f.fmt.pix_mp.pixelformat >> 24) & 0xFF));
        /* A successful S_FMT can still substitute NV12 or a vendor format.
         * Neither can be read using P010's 16-bit samples/UV offset. Fail
         * before allocating/queuing CAPTURE buffers, rather than reporting a
         * successful 10-bit decode with a different pixel representation.
         * QP10/Q12A overrides deliberately retain their diagnostic behavior;
         * they never select the normal P010 request above. */
        if (want == V4L2_PIX_FMT_P010) {
            V4L2_LOG("Linear P010 was rejected. Refusing to treat the substitute format as P010");
            errno = ENOTSUP;
            return -1;
        }
    }

    d->w = (int)f.fmt.pix_mp.width;
    d->h = (int)f.fmt.pix_mp.height;
    d->cap_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    d->stride = (int)f.fmt.pix_mp.plane_fmt[0].bytesperline;
    /* S_FMT may leave stale bytesperline when the application overrides
     * width. A 4K test returned 1920 bytes per row after width became 3840.
     * NV12 requires at least width bytes per row. 10-bit output requires
     * width * 2. For 1080p Main10 in QP10, sizeimage was 6270976, close to
     * 3840 * 1088 * 3/2 = 6266880 plus padding. Using an 8-bit stride of
     * 1920 caused repeated PORT_SETTINGS(INSUFFICIENT) and no output frames. */
    const int bpp2 = (want == v4l2_fourcc('Q', 'P', '1', '0') ||
                      want == v4l2_fourcc('Q', '1', '2', 'A') ||
                      want == v4l2_fourcc('P', '0', '1', '0')) ? 2 : 1;
    if (d->stride < d->w * bpp2) d->stride = d->w * bpp2;

    /* S_FMT can also leave bytesperline too large. After a 1080p stream,
     * G_FMT(CAPTURE) reported 1920x1088. Setting width=1280 and height=720
     * left the 1920-byte stride. Each row was displaced by 640 bytes, and
     * deriving slice_height from that stride yielded 492 rows instead of 736.
     * Firefox showed green frames with occasional valid regions, since 351
     * of 356 exports used the stale geometry. Single-stream FFmpeg tests
     * skipped the resolution change on the same V4L2 file descriptor.
     *
     * msm_vidc aligns NV12 CAPTURE stride to 128 bytes (VENUS_Y_STRIDE).
     * Recompute it from the current width when the reported stride is stale. */
    if (bpp2 == 1) {
        int want_stride = (d->w + 127) & ~127;
        if (d->stride > want_stride) {
            V4L2_LOG("Corrected stale CAPTURE stride %d for width %d. Using %d",
                     d->stride, d->w, want_stride);
            d->stride = want_stride;
        }
    }
    /* In single-plane NV12, UV follows the Y plane at stride * slice_height.
     * Use the coded height and alignment. sizeimage includes extra padding,
     * so 3137536 * 2 / (1920 * 3) would give 1089 rows instead of 1088.
     * Reading the extra row caused a SIGSEGV in nv12_copy via dmd_GetImage.
     *
     * G_FMT(CAPTURE) normally returns the aligned height. If we overrode
     * stale geometry using OUTPUT, it can instead contain the unaligned
     * height we supplied. In a 4K test that was 2160, while the actual
     * slice height was 2176. The UV offset was calculated as 8294400
     * instead of 8355840. Y was bit-exact, but UV was corrupt.
     *
     * Apply the 32-row alignment and use sizeimage only as a capacity check. */
    d->slice_height = (int)f.fmt.pix_mp.height;
    if (d->slice_height < d->h) d->slice_height = d->h;
    /* msm_vidc aligns CAPTURE height to 32 rows (1080 → 1088, 720 → 736, 2160 → 2176). */
    int aligned = (d->slice_height + 31) & ~31;
    if (d->stride > 0 && d->cap_size > 0) {
        /* The capacity bounds slice_height. Round down to whole rows. */
        int fit = (int)((size_t)d->cap_size * 2u / (size_t)(d->stride * 3));
        if (aligned <= fit)
            d->slice_height = aligned;
        else if (d->slice_height > fit)
            d->slice_height = fit & ~1;
    } else {
        d->slice_height = aligned;
    }

    /* sizeimage may also be stale. After correcting stride and slice height,
     * ensure the CAPTURE allocation can hold the resulting frame. Otherwise
     * every frame can be truncated after a resolution change. */
    if (d->stride > 0 && d->slice_height > 0) {
        unsigned int need = (unsigned int)d->stride *
                            (unsigned int)d->slice_height * 3u / 2u;
        if (d->cap_size < need) {
            V4L2_LOG("Corrected CAPTURE sizeimage %u, below the required %u (stride=%d slice_h=%d)",
                     d->cap_size, need, d->stride, d->slice_height);
            d->cap_size = need;
        }
    }

    /* Effective display area: Use G_SELECTION for cropping rectangles (1080p will be returned 1080 when 1088 matches are made). */
    struct v4l2_selection sel;
    memset(&sel, 0, sizeof(sel));
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_COMPOSE;
    if (ioctl(d->fd, VIDIOC_G_SELECTION, &sel) == 0 &&
        sel.r.width > 0 && sel.r.height > 0) {
        d->crop_w = (int)sel.r.width;
        d->crop_h = (int)sel.r.height;
    } else {
        d->crop_w = d->w;
        d->crop_h = d->h;
    }

    d->n_cap = req_bufs(d->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                        DMD_V4L2_MAX_CAP, d->buf_mem);
    if (d->n_cap <= 0) return -1;
    if (d->n_cap > DMD_V4L2_MAX_CAP) d->n_cap = DMD_V4L2_MAX_CAP;
    if (bufs_alloc(d, d->cap, d->n_cap, d->cap_size, 1) < 0) return -1;

    /* After setting EXTRADATA control, CAPTURE is 2 plane, the second is extradata.
     * valid_v4l2_buffer() request b->legth=bufq[port].num_planes
     * (msm_vidc.c:452-461) The difference in plane size is direct EINVAL. */
    d->cap_planes = (int)f.fmt.pix_mp.num_planes;
    if (d->cap_planes < 1) d->cap_planes = 1;
    d->extra_size = (d->cap_planes > 1)
                  ? f.fmt.pix_mp.plane_fmt[1].sizeimage : 0;
    if (d->cap_planes > 1 && !d->extra_size) d->extra_size = 16384;
    if (d->cap_planes > 1) {
        for (int i = 0; i < d->n_cap; i++) {
            d->extra[i].dbuf_fd = dmabuf_alloc(d, d->extra_size);
            if (d->extra[i].dbuf_fd < 0) return -1;
            d->extra[i].length = d->extra_size;
        }
    }

    for (int i = 0; i < d->n_cap; i++) {
        if (qbuf_userptr(d->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i,
                        &d->cap[i], 0, 0,
                        d->cap_planes > 1 ? d->extra[i].dbuf_fd : -1,
                        (unsigned)d->extra_size, d->buf_mem) < 0)
            return -1;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(d->fd, VIDIOC_STREAMON, &type, "STREAMON(CAPTURE)") < 0) return -1;
    d->cap_streaming = 1;
    d->cap_ready = 1;

    V4L2_LOG("CAPTURE ready: %dx%d (visible %dx%d) stride=%d slice_h=%d "
             "%d buffers x %u bytes",
             d->w, d->h, d->crop_w, d->crop_h, d->stride, d->slice_height,
             d->n_cap, d->cap_size);
    return 0;
}

/* -----------------------reprogramming after INSUFFICIENT */

/*
 * Re-programming OPB according to the normal sequence of the manufacturer after receiving PORT_SETTENGS_CHANGED_INSUFFICENT.
 * Just CapTure: maintain streaming throughout the process, otherwise session is dismantled and queued
 * EDB was lost all of them (msm_vidc.c's msm_vidc_streamoff in_reconfig).
 *
 * Returns 0 success (CAPTURE has re-created STREAMON with new geometry), 1 failed.
 */
static int cap_reconfig(struct dmd_v4l2_dec *d)
{
    int ct = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    d->cap_recfg_count++;
    V4L2_LOG("INSUFFICIENT: CAPTURE reconfiguration attempt %d (current %dx%d stride=%d "
             "%d buffers x %u bytes)",
             d->cap_recfg_count, d->w, d->h, d->stride, d->n_cap, d->cap_size);

    /* 1) Only flush CAPTURE. The solid must first return all OPBs, or the back ones.
     *    STREAMON will be EINVAL (the session is live).
     *    Just keep FLUSH_CAPTURE, without touching the input side. */
    struct v4l2_decoder_cmd fc;
    memset(&fc, 0, sizeof(fc));
    fc.cmd = DMD_V4L2_QCOM_CMD_FLUSH;
    fc.flags = DMD_V4L2_QCOM_CMD_FLUSH_CAPTURE;
    if (ioctl(d->fd, VIDIOC_DECODER_CMD, &fc) < 0)
        V4L2_LOG("FLUSH_CAPTURE failed. Continuing to drain: %s", strerror(errno));

    /* 2) The CAPTURE buffer returned to the full DQ and waiting for FLUSH_Done.
     *    The device is O_Nonblock, and here's a short pol, etc., of about 500 ms. */
    int flush_done = 0;
    for (int i = 0; i < 50 && !flush_done; i++) {
        for (;;) {
            struct v4l2_buffer v;
            struct v4l2_plane p[VIDEO_MAX_PLANES];
            memset(&v, 0, sizeof(v));
            memset(p, 0, sizeof(p));
            v.type = ct;
            v.memory = (unsigned int)d->buf_mem;
            v.m.planes = p;
            v.length = (unsigned)(d->cap_planes > 0 ? d->cap_planes : 1);
            if (ioctl(d->fd, VIDIOC_DQBUF, &v) < 0) break;
            if (v.index < (unsigned)d->n_cap) d->cap[v.index].queued = 0;
        }

        struct pollfd pfd;
        pfd.fd = d->fd;
        pfd.events = POLLPRI | POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 10) <= 0) continue;
        if (!(pfd.revents & POLLPRI)) continue;

        struct v4l2_event ev;
        memset(&ev, 0, sizeof(ev));
        while (ioctl(d->fd, VIDIOC_DQEVENT, &ev) == 0) {
            if (ev.type == DMD_EV_MSM_VIDC(1)) {
                flush_done = 1;
            } else if (ev.type == DMD_EV_MSM_VIDC(5)) {
                V4L2_LOG("Received SYS_ERROR during reconfiguration");
                return -1;
            }
            memset(&ev, 0, sizeof(ev));
        }
    }
    V4L2_LOG("Resize:FLUSH_DONE %s", flush_done ? "Received" : "Not received (continue))");

    /* 3) STREAMOFF + REQBUFS(0).
     *    Dma-buf fd Go plane.reserved [0]) */
    if (d->cap_streaming) {
        if (xioctl(d->fd, VIDIOC_STREAMOFF, &ct, "STREAMOFF(CAPTURE)") < 0)
            return -1;
        d->cap_streaming = 0;
    }

    struct v4l2_requestbuffers rb0;
    memset(&rb0, 0, sizeof(rb0));
    rb0.type = ct;
    /* Memory mode must match the application. This is where the USERPTR was written and the kernel is gone.
     * DMABUF (see open d->buf_mem selection), REQBUFS(0) Direct EINVAL,
     * Equivalent to cap_reconfig never succeeded on the DMABUF path. */
    rb0.memory = (unsigned int)d->buf_mem;
    rb0.count = 0;
    if (xioctl(d->fd, VIDIOC_REQBUFS, &rb0, "REQBUFS(CAPTURE,0)") < 0)
        return -1;

    /* 4) The kernel has been removed from the reference to dma-buf so that the fd/ demap can be turned off.
     *    Release before REQBUFS(0) will allow the apogee hardware DMA to be released into the memory. */
    bufs_free(d->cap, DMD_V4L2_MAX_CAP);
    bufs_free(d->extra, DMD_V4L2_MAX_CAP);
    d->cap_ready = 0;
    d->n_cap = 0;

    /* 5) New geometry of solids and streamon -- the core is in this STreamon
     *    Automatically send_continue to restore decoding of queued EDBs. */
    if (setup_capture(d) < 0) return -1;

    /* 6) The end: if the kernel generation is not in STREAMON continue, send it back.
     *    The msm_comm_session_continue() is safe no-op for non-reconfig state. */
    struct v4l2_decoder_cmd dc;
    memset(&dc, 0, sizeof(dc));
    dc.cmd = DMD_V4L2_QCOM_CMD_SESSION_CONTINUE;
    if (ioctl(d->fd, VIDIOC_DECODER_CMD, &dc) == 0)
        V4L2_LOG("Reconfiguration complete. Sent SESSION_CONTINUE");
    else
        V4L2_LOG("Reconfiguration complete. SESSION_CONTINUE failed: %s", strerror(errno));
    /* Geometric changes. The s->fmt (stride/slick/crop) of the session layer cache was released during the first consultation.
     * The next frame will be handed over to the driver -- actual measurements.
     * After 1080p ~720p, the world still interprets 1280 lines by stride=1920/slice=1088.
     * The entire frame was moved at a different line space. */
    d->fmt_dirty = 1;
    return 0;
}

/* ------------------------------------------------------------------- send */

/* Recover all completed OTPUT buffers so they can be reused. */
static void reap_output(struct dmd_v4l2_dec *d)
{
    for (;;) {
        struct v4l2_buffer v;
        struct v4l2_plane p[1];
        memset(&v, 0, sizeof(v));
        memset(p, 0, sizeof(p));
        v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        v.memory = d->buf_mem;
        v.m.planes = p;
        v.length = 1;
        if (ioctl(d->fd, VIDIOC_DQBUF, &v) < 0) break;
        if (v.index < (unsigned)d->n_out) d->out[v.index].queued = 0;
    }
}

int dmd_v4l2_send(struct dmd_v4l2_dec *d, const uint8_t *data, size_t len,
                  uint64_t pts_us)
{
    if (d->fd < 0) return -1;
    if (len > d->in_size) {
        V4L2_LOG("Unit size %zu bytes exceeds the input buffer size %u", len, d->in_size);
        return -1;
    }

    reap_output(d);

    int idx = -1;
    for (int i = 0; i < d->n_out; i++) {
        if (!d->out[i].queued) { idx = i; break; }
    }
    if (idx < 0) return 1;                 /* No free buffers. Callers should take the frame first. */
    if (!d->out[idx].map) return -1;

    memcpy(d->out[idx].map, data, len);
    if (qbuf_userptr(d->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, idx,
                    &d->out[idx], (unsigned)len, pts_us, -1, 0,
                    d->buf_mem) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------- recv */

/* Unblocked to remove a frame CAPTURE. returns 1 = got the image, 2 = EOS frame, 0 = currently not, -1 = error.
 * The device is O_NONBLONK, which returns directly to EAGAIN without blocking when the queue is empty. */
static int try_dq_capture(struct dmd_v4l2_dec *d, uint8_t **out_data,
                          size_t *out_len, uint64_t *out_pts, int *out_index)
{
    struct v4l2_buffer v;
    struct v4l2_plane p[VIDEO_MAX_PLANES];
    memset(&v, 0, sizeof(v));
    memset(p, 0, sizeof(p));
    v.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    v.memory = d->buf_mem;
    v.m.planes = p;
    v.length = (unsigned)(d->cap_planes > 0 ? d->cap_planes : 1);

    if (ioctl(d->fd, VIDIOC_DQBUF, &v) != 0)
        return 0;                       /* EAGAIN: There's no frame. */
    if (v.index >= (unsigned)d->n_cap)
        return -1;
    d->cap[v.index].queued = 0;

    /* Bytesused = 0 with LAST tag = stream end tag frame, not image. */
    if (p[0].bytesused == 0) {
        if (v.flags & V4L2_BUF_FLAG_LAST) {
            d->saw_eos = 1;
            return 2;
        }
        /* Empty but not LEST: Go back and wait. */
        qbuf_userptr(d->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                     (int)v.index, &d->cap[v.index], 0, 0,
                     d->cap_planes > 1 ? d->extra[v.index].dbuf_fd : -1,
                     (unsigned)d->extra_size, d->buf_mem);
        return 0;
    }

    *out_data = (uint8_t *)d->cap[v.index].map;
    *out_len = p[0].bytesused;
    *out_pts = (uint64_t)v.timestamp.tv_sec * 1000000ULL +
               (uint64_t)v.timestamp.tv_usec;
    *out_index = (int)v.index;
    if (v.flags & V4L2_BUF_FLAG_LAST) d->saw_eos = 1;
    d->cap[v.index].hardware_error = (v.flags & V4L2_BUF_FLAG_ERROR) != 0;
    if (d->cap[v.index].hardware_error) dmd_video_stats_error(d->video_stats);
    else dmd_video_stats_hw(d->video_stats);
    return 1;
}

int dmd_v4l2_recv(struct dmd_v4l2_dec *d, uint8_t **out_data, size_t *out_len,
                  uint64_t *out_pts, int *out_index, int timeout_ms)
{
    if (d->fd < 0) return -1;

    /* We'll try DQBUF directly and decide if we want to play the poll.
     *
     * POLLIN of msm_vidc cannot be considered a reliable signal for "Classed": the frame is already in the CAPTURE queue
     * Now, the poll may be out of place, so it's empty and timed to return, and the frame is being dragged into a living place.
     * Timeout. Research/slowfeed.c Actual 27Mbps stream, 5 frames in flight,
     * 300 frames:
     *     Poll timeout 100ms * 250 frames after the interval is stable at 111ms (9fps)
     *     Poll timeout 20ms * Same position interval becomes 41-53ms
     * The interval follows the timeout, which is an empty fingerprint. After "go straight, not just bill":
     *     Slow frame (>33ms) 15.3% 3.4%, inhalation 32.6fps ~ 57.2fps
     * This explains the 15.8% drop frame on the side of the browser -- the same rate as the 15.3% slow frame. */
    if (d->cap_ready) {
        int r = try_dq_capture(d, out_data, out_len, out_pts, out_index);
        if (r != 0) return r;
    }

    struct pollfd pfd;
    pfd.fd = d->fd;
    pfd.events = POLLPRI | POLLOUT | (d->cap_ready ? POLLIN : 0);
    pfd.revents = 0;

    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return (errno == EINTR) ? 0 : -1;
    if (pr == 0) return 0;

    /* Organisation */
    if (pfd.revents & POLLPRI) {
        struct v4l2_event ev;
        int guard = 0;
        memset(&ev, 0, sizeof(ev));
        /* Ceiling protection: The device is O_NONBLONK, which returns EAGAIN when the queue is empty, not blocked. */
        while (guard++ < 8 && ioctl(d->fd, VIDIOC_DQEVENT, &ev) == 0) {
            unsigned rel = ev.type - V4L2_EVENT_MSM_VIDC_START;

            /* New kernel msm_vidc (DMABUF mode) standard by standard
             * V4L2_EVENT_SOURCE_CHANGE(changes=1 RESOLUTION), no private
             * Port_SETTENGS event, nor does it need ESSION_CONTINUE (the private order is in the
             * Returns EINVAL on the new kernel. The CAPTURE solids continue to appear automatically. */
            if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                const unsigned *ed = (const unsigned *)ev.u.data;
                V4L2_LOG("SOURCE_CHANGE: changes=0x%x", ed[0]);
                if (!d->cap_ready) {
                    if (setup_capture(d) < 0) return -1;
                } else {
                    /* Fluid resolution (second SOURCE_CHANGE in the same session).
                     *
                     * Actual: G_FMT reported new ** geometry after the incident
                     * (1080 p  ⁇ 720 p report 1280x736), while the OUTPUT side negotiation value is still old
                     * 1920x1088. So this must be the part in G_FMT — setup_capture
                     * "Cover G_FMT with out_w/out_h" is a change in flow to prevent first consultation of residual values
                     * The opposite direction is then applied, which puts the new resolution back into the old size.
                     *
                     * ffmpeg side only AV1 will walk this path: h264/hevc changes resolution
                     * New VA context from top, with only the first event per session; AV1 reuse
                     * Context, second time received from the same session. */
                    struct v4l2_format nf;
                    memset(&nf, 0, sizeof(nf));
                    nf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                    if (ioctl(d->fd, VIDIOC_G_FMT, &nf) != 0) {
                        V4L2_LOG("Midstream SOURCE_CHANGE: G_FMT failed: %s",
                                 strerror(errno));
                        return -1;
                    }
                    if ((int)nf.fmt.pix_mp.width == d->w &&
                        (int)nf.fmt.pix_mp.height == d->h) {
                        V4L2_LOG("Midstream SOURCE_CHANGE: geometry is still %dx%d. Keeping current CAPTURE buffers",
                                 d->w, d->h);
                    } else {
                        V4L2_LOG("Midstream SOURCE_CHANGE: %dx%d to %ux%u. Reconfiguring CAPTURE",
                                 d->w, d->h,
                                 nf.fmt.pix_mp.width, nf.fmt.pix_mp.height);
                        d->trust_gfmt = 1;
                        int r = cap_reconfig(d);
                        d->trust_gfmt = 0;
                        if (r < 0) {
                            V4L2_LOG("Midstream SOURCE_CHANGE: reconfiguration failed. Keeping previous session geometry");
                            return -1;
                        }
                        /* Sync the OTPUT consultation value: out_w/ out_h only setup_capture
                         * Overwrite logical readings, keeping old values will allow any subsequent non-trust_gfmt
                         * Re-alignment (e.g., INSUFFICIENT) puts the resolution back into the old. */
                        d->out_w = (int)d->w;
                        d->out_h = (int)d->h;
                    }
                }
            } else if (ev.type == DMD_EV_MSM_VIDC(2) ||
                       ev.type == DMD_EV_MSM_VIDC(3)) {
                const unsigned *ed = (const unsigned *)ev.u.data;
                /* Payload layout is intergenerationally incompatible (8998 with the word word sm8150),
                 * So except for the h/w, which is based on the current layout, the original is printed.
                 * ev.u.data is 64 bytes, up to 16 u32. */
                V4L2_LOG("PORT_SETTINGS%s: h=%u w=%u bitdepth=%u picstruct=%u "
                         "raw=[%08x %08x %08x %08x %08x %08x %08x %08x "
                         "%08x %08x %08x %08x]",
                         ev.type == DMD_EV_MSM_VIDC(3) ? "(INSUFFICIENT)"
                                                       : "(SUFFICIENT)",
                         ed[0], ed[1], ed[2], ed[3],
                         ed[0], ed[1], ed[2], ed[3],
                         ed[4], ed[5], ed[6], ed[7],
                         ed[8], ed[9], ed[10], ed[11]);

                if (!d->cap_ready && setup_capture(d) < 0) return -1;

                /* INSUFFIENT: Solids require re-equipment of OPBs.
                 * (omx_vdec_v4l2.cpp execute_omx_flush/ PortDisable)
                 * The sequence is:
                 *   DECODER_CMD{FLUSH, FLUSH_CAPTURE}
                 *   * Wait for FLUSH_Done, return all OPBs
                 *   → STREAMOFF(CAPTURE) → REQBUFS(CAPTURE,0)
                 *   * Resize with New Geometry *Streamon (CAPTURE)
                 * The core automatically sends message_continue in this STREAMON
                 * (msm_vidc.c: 1240-1252 authoritative note).
                 *
                 * CapTure:msm_vidc_streamoff()
                 * `if (! inst->in_reconfig) ' Guards, so the STREAMOFF will not be removed at this time.
                 * - Yes, but STREAMOFF(OUTPUT)/REQBUFS (OUTPUT, 0) will leave
                 * RELEASE_RESOURCES_DONE, LEFT EXB ALL DIED.
                 *
                 * ⁇  This session is live: 24 OPs are still in place when the FLUSH Direct STREAMOFF
                 * Solid hands, then Streamon (CAPTURE) constant EINVAL chained
                 * SYS_ERROR. The missing step is this flush. */
                if (ev.type == DMD_EV_MSM_VIDC(3) && d->cap_ready) {
                    if (cap_reconfig(d) < 0) {
                        V4L2_LOG("Could not reconfigure CAPTURE. The caller must rebuild the session");
                        return -1;
                    }
                } else {
                    /* SUFFICIENT: The geometry remains unchanged, just let the solids leave in_reconfig.
                     * Every event has to be launched -- sm 8150 generation cores no longer automatically, but the next time the kernels are in the air, the kernels are not going to be in the air.
                     * The video is available on the website of the blog "Firrefox seek/ABR" and is being triggered repeatedly on the same fd.
                     * The use of sessional locks keeps the solid permanently stuck in waiting for FBD. */
                    struct v4l2_decoder_cmd dc;
                    memset(&dc, 0, sizeof(dc));
                    dc.cmd = DMD_V4L2_QCOM_CMD_SESSION_CONTINUE;
                    if (ioctl(d->fd, VIDIOC_DECODER_CMD, &dc) == 0)
                        V4L2_LOG("Sent SESSION_CONTINUE (event seq=%u)",
                                 ev.sequence);
                    else
                        V4L2_LOG("SESSION_CONTINUE failed (event seq=%u): %s",
                                 ev.sequence, strerror(errno));
                }
            } else if (ev.type == DMD_EV_MSM_VIDC(5)) {
                V4L2_LOG("Fatal: received SYS_ERROR. The session is no longer valid");
                return -1;
            } else if (ev.type == V4L2_EVENT_EOS || ev.type == DMD_EV_MSM_VIDC(1)) {
                V4L2_LOG("received EOS/FLUSH_DONE");
                d->saw_eos = 1;
            } else if (rel <= 7) {
                V4L2_LOG("private event +%u", rel);
            }
            memset(&ev, 0, sizeof(ev));
        }
    }

    if (pfd.revents & POLLOUT)
        reap_output(d);

    /* Poll says you have a frame, you can't just believe it.
     * So, you can try and don't ask for revents with POLLIN. */
    if (d->cap_ready)
        return try_dq_capture(d, out_data, out_len, out_pts, out_index);

    return 0;
}

int dmd_v4l2_release(struct dmd_v4l2_dec *d, int index)
{
    if (d->fd < 0 || index < 0 || index >= d->n_cap) return -1;
    if (d->cap[index].queued) return 0;
    return qbuf_userptr(d->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, index,
                       &d->cap[index], 0, 0,
                       d->cap_planes > 1 ? d->extra[index].dbuf_fd : -1,
                       (unsigned)d->extra_size, d->buf_mem);
}

/* ------------------------------------------------------------------ drain */

int dmd_v4l2_drain(struct dmd_v4l2_dec *d)
{
    if (d->fd < 0) return -1;
    if (d->draining) return 0;

    struct v4l2_decoder_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = V4L2_DEC_CMD_STOP;
    if (xioctl(d->fd, VIDIOC_DECODER_CMD, &cmd, "DECODER_CMD(STOP)") < 0) {
        /* Partly driven does not support the command, decomposition to sending a buffer EOS bytesused=0. */
        reap_output(d);
        for (int i = 0; i < d->n_out; i++) {
            if (!d->out[i].queued) {
                qbuf_userptr(d->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, i,
                            &d->out[i], 0, 0, -1, 0, d->buf_mem);
                break;
            }
        }
    }
    d->draining = 1;
    return 0;
}

/* ------------------------------------------------------------------ close */

void dmd_v4l2_close(struct dmd_v4l2_dec *d)
{
    if (!d) return;

    bufs_free(d->extra, DMD_V4L2_MAX_CAP);

    if (d->fd >= 0) {
        int type;
        if (d->out_streaming) {
            type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            ioctl(d->fd, VIDIOC_STREAMOFF, &type);
            d->out_streaming = 0;
        }
        if (d->cap_streaming) {
            type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            ioctl(d->fd, VIDIOC_STREAMOFF, &type);
            d->cap_streaming = 0;
        }
    }

    bufs_free(d->out, DMD_V4L2_MAX_OUT);
    bufs_free(d->cap, DMD_V4L2_MAX_CAP);

    if (d->fd >= 0) { close(d->fd); d->fd = -1; }
    if (d->heap_fd >= 0) { close(d->heap_fd); d->heap_fd = -1; }
    d->heap_kind = HEAP_NONE;

    d->cap_ready = 0;
    d->n_out = d->n_cap = 0;
}
