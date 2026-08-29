/*
 * v4l2_backend —— V4L2 M2M 直通解码后端实现。
 *
 * 流程与常量全部来自 tools/v4l2_dec_probe.c 的实测验证（该探针在 SM8750 上
 * 解出 13 帧，像素与 ffmpeg 软解逐点 100% 一致）。这里做的是把一次性探针
 * 整理成可被 daemon 复用的状态机，行为保持一致。
 *
 * 设计取舍：
 *
 * - 不做零拷贝。CAPTURE 侧的 dmabuf 映射到用户空间后 memcpy 给上层，
 *   因为 daemon 现有的传输层（SHM 槽位 / socket）都是按"拿到一段字节"
 *   设计的。零拷贝需要把 dmabuf fd 一路透传到容器里，那是另一个课题。
 *
 * - 缓冲数量取驱动给的值，不强求。REQBUFS 会把 count 改成驱动要求的最小值
 *   （实测 CAPTURE 侧请求 8 得到 8、请求 24 也可能被下调），照用即可。
 *
 * - 错误处理一律返回 -1 并留给调用方 close，不在中途做部分回滚 ——
 *   会话级资源的生命周期由 dmd_v4l2_close 统一负责。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "v4l2_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef V4L2_PIX_FMT_AV1
#define V4L2_PIX_FMT_AV1 v4l2_fourcc('A', 'V', '1', '0')
#endif
#ifndef V4L2_PIX_FMT_HEVC
#define V4L2_PIX_FMT_HEVC v4l2_fourcc('H', 'E', 'V', 'C')
#endif
#ifndef V4L2_PIX_FMT_VP9
#define V4L2_PIX_FMT_VP9 v4l2_fourcc('V', 'P', '9', '0')
#endif

/* dma-heap 的 ioctl 定义。linux/dma-heap.h 在部分 sysroot 里缺失，
 * 直接内联 —— 这是稳定的内核 ABI。 */
struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0, struct dma_heap_allocation_data)

/* 解码节点。msm_vidc 上 video32=解码、video33=编码（video0/1 是相机）。
 * 用探测而非硬编码更稳妥，但实测该平台固定如此，先按固定值走，
 * probe 失败时再遍历。 */
#define DEC_NODE_PRIMARY "/dev/video32"

/* 日志：daemon 的 dlog 是 static，这里不便直接用，改为写 stderr。
 * daemon 本身的 stderr 已被重定向到日志文件，落点一致。 */
#define V4L2_LOG(fmt, ...) \
    fprintf(stderr, "[v4l2] " fmt "\n", ##__VA_ARGS__)

/* codec_id → V4L2 fourcc。取值经 /dev/video32 的 ENUM_FMT 实测确认：
 *   [0] H264  [1] HEVC  [2] VP90  [3] HEIC  [4] AV10
 * VP8 不在驱动的枚举里，故无映射 —— 传入会返回 0，调用方据此拒绝。 */
static uint32_t codec_to_fourcc(int codec_id)
{
    switch (codec_id) {
    case DMD_V4L2_CODEC_H264: return V4L2_PIX_FMT_H264;
    case DMD_V4L2_CODEC_HEVC: return V4L2_PIX_FMT_HEVC;
    case DMD_V4L2_CODEC_VP9:  return V4L2_PIX_FMT_VP9;
    case DMD_V4L2_CODEC_AV1:  return V4L2_PIX_FMT_AV1;
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
        V4L2_LOG("%s 失败: %s", name, strerror(errno));
    return r;
}

/* ------------------------------------------------------------ dmabuf 分配 */

static int heap_open(struct dmd_v4l2_dec *d)
{
    if (d->heap_fd >= 0) return 0;
    d->heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (d->heap_fd < 0) {
        V4L2_LOG("打开 /dev/dma_heap/system 失败: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int dmabuf_alloc(struct dmd_v4l2_dec *d, size_t len)
{
    if (heap_open(d) < 0) return -1;
    struct dma_heap_allocation_data a;
    memset(&a, 0, sizeof(a));
    a.len = len;
    a.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(d->heap_fd, DMA_HEAP_IOCTL_ALLOC, &a) < 0) {
        V4L2_LOG("dma_heap 分配 %zu 字节失败: %s", len, strerror(errno));
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
                V4L2_LOG("mmap dmabuf[%d] %zu 字节失败: %s", i, size,
                         strerror(errno));
                return -1;
            }
            b[i].map = m;
        }
    }
    return 0;
}

static int req_bufs(int fd, enum v4l2_buf_type type, int count)
{
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = (unsigned)count;
    rb.type = type;
    rb.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb, "REQBUFS") < 0) return -1;
    return (int)rb.count;
}

static int qbuf_dmabuf(int fd, enum v4l2_buf_type type, int index,
                       struct dmd_v4l2_buf *b, unsigned bytesused,
                       uint64_t pts_us)
{
    struct v4l2_buffer v;
    struct v4l2_plane p[1];
    memset(&v, 0, sizeof(v));
    memset(p, 0, sizeof(p));
    v.type = type;
    v.memory = V4L2_MEMORY_DMABUF;
    v.index = (unsigned)index;
    v.m.planes = p;
    v.length = 1;
    p[0].m.fd = b->dbuf_fd;
    p[0].length = (unsigned)b->length;
    p[0].bytesused = bytesused;
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
    uint32_t want = codec_to_fourcc(codec_id);
    if (!want) return 0;

    int fd = open(DEC_NODE_PRIMARY, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;

    int found = 0;
    for (unsigned i = 0; i < 64 && !found; i++) {
        struct v4l2_fmtdesc fd_desc;
        memset(&fd_desc, 0, sizeof(fd_desc));
        fd_desc.index = i;
        fd_desc.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fd_desc) < 0) break;
        if (fd_desc.pixelformat == want) found = 1;
    }
    close(fd);
    return found;
}

/* ------------------------------------------------------------------- open */

int dmd_v4l2_open(struct dmd_v4l2_dec *d, int codec_id, int w, int h)
{
    memset(d, 0, sizeof(*d));
    d->fd = -1;
    d->heap_fd = -1;
    for (int i = 0; i < DMD_V4L2_MAX_OUT; i++) d->out[i].dbuf_fd = -1;
    for (int i = 0; i < DMD_V4L2_MAX_CAP; i++) d->cap[i].dbuf_fd = -1;

    uint32_t fourcc = codec_to_fourcc(codec_id);
    if (!fourcc) {
        V4L2_LOG("不支持的 codec_id=%d", codec_id);
        return -1;
    }

    d->fd = open(DEC_NODE_PRIMARY, O_RDWR | O_CLOEXEC);
    if (d->fd < 0) {
        V4L2_LOG("打开 %s 失败: %s", DEC_NODE_PRIMARY, strerror(errno));
        return -1;
    }

    /* OUTPUT 侧：告诉驱动我们要喂什么码流。 */
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.pixelformat = fourcc;
    f.fmt.pix_mp.width = (unsigned)w;
    f.fmt.pix_mp.height = (unsigned)h;
    f.fmt.pix_mp.num_planes = 1;
    /* 给一个足够大的初值；驱动会按自己的规则改写 sizeimage。 */
    f.fmt.pix_mp.plane_fmt[0].sizeimage = 4u * 1024u * 1024u;
    if (xioctl(d->fd, VIDIOC_S_FMT, &f, "S_FMT(OUTPUT)") < 0) goto fail;
    d->in_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    V4L2_LOG("S_FMT(OUTPUT) OK: %ux%u sizeimage=%u", f.fmt.pix_mp.width,
             f.fmt.pix_mp.height, d->in_size);

    /* 订阅事件。SOURCE_CHANGE 是分辨率协商的信号，EOS 用于排空收尾。
     * 不订阅就无从知道何时可以配置 CAPTURE，固件会按错误假设解析。 */
    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    if (xioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE(SOURCE_CHANGE)") < 0)
        goto fail;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_EOS;
    xioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE(EOS)");  /* 非致命 */

    /* OUTPUT 缓冲 + STREAMON。CAPTURE 侧要等 SOURCE_CHANGE 才能配。 */
    d->n_out = req_bufs(d->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                        DMD_V4L2_MAX_OUT);
    if (d->n_out <= 0) goto fail;
    if (d->n_out > DMD_V4L2_MAX_OUT) d->n_out = DMD_V4L2_MAX_OUT;
    if (bufs_alloc(d, d->out, d->n_out, d->in_size, 1) < 0) goto fail;

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(d->fd, VIDIOC_STREAMON, &type, "STREAMON(OUTPUT)") < 0) goto fail;
    d->out_streaming = 1;

    V4L2_LOG("OUTPUT 就绪: %d 缓冲 x %u 字节", d->n_out, d->in_size);
    return 0;

fail:
    dmd_v4l2_close(d);
    return -1;
}

/* ------------------------------------------------- 第二阶段：CAPTURE 协商 */

/*
 * 收到 SOURCE_CHANGE 后配置 CAPTURE 侧。
 * 关键：CAPTURE 默认是 QCOM 压缩格式 Q08C，必须显式改成 NV12，
 * 否则拿到的不是线性帧，上层无法直接当 NV12 用。
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

    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    if (xioctl(d->fd, VIDIOC_S_FMT, &f, "S_FMT(CAPTURE/NV12)") < 0) return -1;

    d->w = (int)f.fmt.pix_mp.width;
    d->h = (int)f.fmt.pix_mp.height;
    d->cap_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    d->stride = (int)f.fmt.pix_mp.plane_fmt[0].bytesperline;
    if (d->stride <= 0) d->stride = d->w;
    /* NV12 单平面布局：Y 平面高度即 slice_height，UV 紧随其后。
     * 用 sizeimage 反推比信任驱动的 plane 描述更稳。 */
    d->slice_height = (d->stride > 0)
                    ? (int)((d->cap_size * 2u) / (unsigned)(d->stride * 3))
                    : d->h;

    /* 有效显示区域：用 G_SELECTION 拿裁剪矩形（1080p 会是 1088 对齐后裁回 1080）。 */
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
                        DMD_V4L2_MAX_CAP);
    if (d->n_cap <= 0) return -1;
    if (d->n_cap > DMD_V4L2_MAX_CAP) d->n_cap = DMD_V4L2_MAX_CAP;
    if (bufs_alloc(d, d->cap, d->n_cap, d->cap_size, 1) < 0) return -1;

    for (int i = 0; i < d->n_cap; i++) {
        if (qbuf_dmabuf(d->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i,
                        &d->cap[i], 0, 0) < 0)
            return -1;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(d->fd, VIDIOC_STREAMON, &type, "STREAMON(CAPTURE)") < 0) return -1;
    d->cap_streaming = 1;
    d->cap_ready = 1;

    V4L2_LOG("CAPTURE 就绪: %dx%d（有效 %dx%d）stride=%d slice_h=%d "
             "%d 缓冲 x %u 字节",
             d->w, d->h, d->crop_w, d->crop_h, d->stride, d->slice_height,
             d->n_cap, d->cap_size);
    return 0;
}

/* ------------------------------------------------------------------- send */

/* 回收所有已完成的 OUTPUT 缓冲，让它们可被复用。 */
static void reap_output(struct dmd_v4l2_dec *d)
{
    for (;;) {
        struct v4l2_buffer v;
        struct v4l2_plane p[1];
        memset(&v, 0, sizeof(v));
        memset(p, 0, sizeof(p));
        v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        v.memory = V4L2_MEMORY_DMABUF;
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
        V4L2_LOG("单元 %zu 字节超过输入缓冲 %u", len, d->in_size);
        return -1;
    }

    reap_output(d);

    int idx = -1;
    for (int i = 0; i < d->n_out; i++) {
        if (!d->out[i].queued) { idx = i; break; }
    }
    if (idx < 0) return 1;                 /* 无空闲缓冲，调用方应先收帧 */
    if (!d->out[idx].map) return -1;

    memcpy(d->out[idx].map, data, len);
    if (qbuf_dmabuf(d->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, idx,
                    &d->out[idx], (unsigned)len, pts_us) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------- recv */

int dmd_v4l2_recv(struct dmd_v4l2_dec *d, uint8_t **out_data, size_t *out_len,
                  uint64_t *out_pts, int *out_index, int timeout_ms)
{
    if (d->fd < 0) return -1;

    struct pollfd pfd;
    pfd.fd = d->fd;
    pfd.events = POLLPRI | POLLOUT | (d->cap_ready ? POLLIN : 0);
    pfd.revents = 0;

    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return (errno == EINTR) ? 0 : -1;
    if (pr == 0) return 0;

    /* 事件优先：SOURCE_CHANGE 决定能否进入出帧阶段。 */
    if (pfd.revents & POLLPRI) {
        struct v4l2_event ev;
        memset(&ev, 0, sizeof(ev));
        if (ioctl(d->fd, VIDIOC_DQEVENT, &ev) == 0) {
            if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                V4L2_LOG("收到 SOURCE_CHANGE");
                if (!d->cap_ready && setup_capture(d) < 0) return -1;
            } else if (ev.type == V4L2_EVENT_EOS) {
                V4L2_LOG("收到 EOS 事件");
                d->saw_eos = 1;
            }
        }
    }

    if (pfd.revents & POLLOUT)
        reap_output(d);

    if ((pfd.revents & POLLIN) && d->cap_ready) {
        struct v4l2_buffer v;
        struct v4l2_plane p[1];
        memset(&v, 0, sizeof(v));
        memset(p, 0, sizeof(p));
        v.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        v.memory = V4L2_MEMORY_DMABUF;
        v.m.planes = p;
        v.length = 1;
        if (ioctl(d->fd, VIDIOC_DQBUF, &v) == 0) {
            if (v.index >= (unsigned)d->n_cap) return -1;
            d->cap[v.index].queued = 0;

            /* bytesused==0 且带 LAST 标记 = 流结束标记帧，不是画面。 */
            if (p[0].bytesused == 0) {
                if (v.flags & V4L2_BUF_FLAG_LAST) {
                    d->saw_eos = 1;
                    return 2;
                }
                /* 空帧但非 LAST：还回去继续等。 */
                qbuf_dmabuf(d->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                            (int)v.index, &d->cap[v.index], 0, 0);
                return 0;
            }

            *out_data = (uint8_t *)d->cap[v.index].map;
            *out_len = p[0].bytesused;
            *out_pts = (uint64_t)v.timestamp.tv_sec * 1000000ULL +
                       (uint64_t)v.timestamp.tv_usec;
            *out_index = (int)v.index;

            if (v.flags & V4L2_BUF_FLAG_LAST) d->saw_eos = 1;
            return 1;
        }
    }

    return 0;
}

int dmd_v4l2_release(struct dmd_v4l2_dec *d, int index)
{
    if (d->fd < 0 || index < 0 || index >= d->n_cap) return -1;
    if (d->cap[index].queued) return 0;
    return qbuf_dmabuf(d->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, index,
                       &d->cap[index], 0, 0);
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
        /* 部分驱动不支持该命令，退化为送一个 bytesused=0 的缓冲当 EOS。 */
        reap_output(d);
        for (int i = 0; i < d->n_out; i++) {
            if (!d->out[i].queued) {
                qbuf_dmabuf(d->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, i,
                            &d->out[i], 0, 0);
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

    d->cap_ready = 0;
    d->n_out = d->n_cap = 0;
}
