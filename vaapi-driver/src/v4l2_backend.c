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
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* 厂商私有事件基址（厂商内核 include/uapi/linux/videodev2.h:2290）。
 * 容器/主机的 glibc videodev2.h 没有这些宏，自行定义。 */
#ifndef V4L2_EVENT_MSM_VIDC_START
#define V4L2_EVENT_MSM_VIDC_START  (V4L2_EVENT_PRIVATE_START + 0x00001000)
#endif
#define DMD_EV_MSM_VIDC(n)  (V4L2_EVENT_MSM_VIDC_START + (n))

/* msm_vidc 私有命令（videodev2.h:1991）：让 in_reconfig 生效后继续会话。 */
#define DMD_V4L2_QCOM_CMD_SESSION_CONTINUE  5

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

/* codec_id → V4L2 fourcc。
 *
 * 取值经本机 /dev/video32 的 VIDIOC_ENUM_FMT 实测确认，OUTPUT 侧枚举为：
 *   [0] MPG2  [1] H264  [2] HEVC  [3] VP80  [4] VP90
 * （driver=msm_vidc_driver card=msm_vidc_vdec）
 *
 * ⚠️ 0.4.2：VP8 加入映射。此前注释说"VP8 不在驱动的枚举里"，实测不成立。 */
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
        V4L2_LOG("%s 失败: %s", name, strerror(errno));
    return r;
}

/* ------------------------------------------------------------ dmabuf 分配 */

/* DMABUF 来源：dma-heap 优先，ION 兜底。
 *
 * 为什么需要两条路（第 81~84 轮实测）：
 *   有 dma-heap 的设备（内核 5.x）  → /dev/dma_heap/system
 *   小米平板 5 / nabu（内核 4.14）  → **没有 dma_heap 子系统**
 *                                     （/sys/class/dma_heap/ 都不存在）
 *                                     只有 /dev/ion
 *
 * ⚠️ 第 81 轮我判定"nabu 的 ION 也不可用"，那个结论是**错的**。
 * 当时的做法是把 heap_id_mask 从 1 逐位试到 0x80 —— 而这台设备的
 * system heap id 是 **25**（mask = 1<<25），压根不在试探范围里。
 * 正确做法是先用 ION_IOC_HEAP_QUERY 问内核有哪些 heap。
 *
 * 实测（Android 侧 root 与容器内 master 用户结果完全一致）：
 *     heap 数量 = 9
 *     id=27 qsecom          id=26 user_contig     id=25 system
 *     id=22 adsp            id=19 qsecom_ta       id=14 secure_carveout
 *     id=13 spss            id=10 secure_display  id=9  secure_heap
 * 用 id=25 分配 3133440 字节（1080p NV12 一帧）成功，且可 mmap。
 *
 * 教训：ENODEV 与 EINVAL 的区别是有信息的 —— modern ABI 返回 ENODEV
 * （内核认识这个 ioctl，只是没有匹配的 heap），legacy ABI 返回 EINVAL
 * （ABI 布局不对）。我当时把两者都当成"不支持"，丢掉了这条线索。 */

enum { HEAP_NONE = 0, HEAP_DMA_HEAP, HEAP_ION };

/* ION 的 modern ABI（4.12 起的统一接口，nabu 的 4.14 用的就是这个）。 */
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
    uint64_t heaps;          /* 指向 ion_heap_data 数组的用户态地址 */
    uint32_t reserved1, reserved2;
};
#define ION_IOC_HEAP_QUERY _IOWR('I', 8, struct ion_heap_query_data)

/* 问内核要 system heap 的 mask。失败返回 0。 */
static uint32_t ion_system_mask(int fd)
{
    struct ion_heap_query_data q;
    memset(&q, 0, sizeof(q));
    if (ioctl(fd, ION_IOC_HEAP_QUERY, &q) != 0 || q.cnt == 0 || q.cnt > 64) {
        V4L2_LOG("ION HEAP_QUERY 失败或数量异常(%u): %s", q.cnt,
                 strerror(errno));
        return 0;
    }

    struct ion_heap_data hd[64];
    memset(hd, 0, sizeof(hd));
    q.heaps = (uint64_t)(uintptr_t)hd;
    if (ioctl(fd, ION_IOC_HEAP_QUERY, &q) != 0) {
        V4L2_LOG("ION HEAP_QUERY 取数据失败: %s", strerror(errno));
        return 0;
    }

    /* ION_HEAP_TYPE_SYSTEM == 0。优先按类型认，名字只作兜底 ——
     * 类型是 ABI 的一部分，名字是厂商起的。 */
    for (unsigned i = 0; i < q.cnt; i++) {
        if (hd[i].type == 0) {
            V4L2_LOG("ION system heap: id=%u name=%s", hd[i].heap_id,
                     hd[i].name);
            return 1u << (hd[i].heap_id & 31u);
        }
    }
    for (unsigned i = 0; i < q.cnt; i++) {
        if (strncmp(hd[i].name, "system", sizeof(hd[i].name)) == 0) {
            V4L2_LOG("ION system heap(按名字): id=%u", hd[i].heap_id);
            return 1u << (hd[i].heap_id & 31u);
        }
    }
    V4L2_LOG("ION 有 %u 个 heap，但没有 system 类型的", q.cnt);
    return 0;
}

static int heap_open(struct dmd_v4l2_dec *d)
{
    if (d->heap_fd >= 0) return 0;

    d->heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (d->heap_fd >= 0) {
        d->heap_kind = HEAP_DMA_HEAP;
        V4L2_LOG("缓冲来源: /dev/dma_heap/system");
        return 0;
    }
    const int dh_errno = errno;

    d->heap_fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
    if (d->heap_fd >= 0) {
        d->ion_mask = ion_system_mask(d->heap_fd);
        if (d->ion_mask == 0) {
            close(d->heap_fd);
            d->heap_fd = -1;
            V4L2_LOG("ION 无可用 system heap");
            return -1;
        }
        d->heap_kind = HEAP_ION;
        V4L2_LOG("缓冲来源: /dev/ion mask=0x%x（无 dma_heap: %s）",
                 d->ion_mask, strerror(dh_errno));
        return 0;
    }

    V4L2_LOG("无可用 DMABUF 来源：dma_heap=%s ion=%s",
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
            V4L2_LOG("dma_heap 分配 %zu 字节失败: %s", len, strerror(errno));
            return -1;
        }
        return (int)a.fd;
    }

    struct ion_alloc_data a;
    memset(&a, 0, sizeof(a));
    a.len = len;
    a.heap_id_mask = d->ion_mask;
    if (ioctl(d->heap_fd, ION_IOC_ALLOC_MODERN, &a) < 0) {
        V4L2_LOG("ION 分配 %zu 字节失败(mask=0x%x): %s", len, d->ion_mask,
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
                V4L2_LOG("mmap dmabuf[%d] %zu 字节失败: %s", i, size,
                         strerror(errno));
                return -1;
            }
            b[i].map = m;
        }
    }
    return 0;
}

/* msm_vidc 只接受 V4L2_MEMORY_USERPTR，但那是个名义值：
 * 驱动的 vb2_mem_ops.get_userptr 是返回 0xdeadbeef 的桩函数
 * （厂商内核 msm_vidc.c:717-720），真正的缓冲来源是
 *     b->m.planes[i].m.fd = b->m.planes[i].reserved[0];   (msm_vidc.c:533-536)
 * 也就是 dmabuf fd 必须写进 plane.reserved[0]。 */
static int req_bufs(int fd, enum v4l2_buf_type type, int count)
{
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = (unsigned)count;
    rb.type = type;
    rb.memory = V4L2_MEMORY_USERPTR;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb, "REQBUFS") < 0) return -1;
    return (int)rb.count;
}

/* extra_fd >= 0 时作为第二平面（extradata）的 dmabuf fd，
 * extra_len 为其长度。CAPTURE 侧设了 EXTRADATA 控制项后 num_planes 为 2。 */
static int qbuf_userptr(int fd, enum v4l2_buf_type type, int index,
                        struct dmd_v4l2_buf *b, unsigned bytesused,
                        uint64_t pts_us, int extra_fd, unsigned extra_len)
{
    struct v4l2_buffer v;
    struct v4l2_plane p[VIDEO_MAX_PLANES];
    memset(&v, 0, sizeof(v));
    memset(p, 0, sizeof(p));
    v.type = type;
    v.memory = V4L2_MEMORY_USERPTR;
    v.index = (unsigned)index;
    v.m.planes = p;
    v.length = (extra_fd >= 0) ? 2 : 1;
    /* 关键：fd 走 reserved[0]，不是 m.fd 也不是 m.userptr */
    p[0].reserved[0] = (unsigned)b->dbuf_fd;
    p[0].length = (unsigned)b->length;
    p[0].bytesused = bytesused;
    if (extra_fd >= 0) {
        p[1].reserved[0] = (unsigned)extra_fd;
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

static int setup_capture(struct dmd_v4l2_dec *d);

/* ------------------------------------------------------------------- open */

int dmd_v4l2_open(struct dmd_v4l2_dec *d, int codec_id, int w, int h)
{
    memset(d, 0, sizeof(*d));
    d->fd = -1;
    d->heap_fd = -1;
    d->heap_kind = HEAP_NONE;
    d->ion_mask = 0;
    for (int i = 0; i < DMD_V4L2_MAX_OUT; i++) d->out[i].dbuf_fd = -1;
    for (int i = 0; i < DMD_V4L2_MAX_CAP; i++) d->cap[i].dbuf_fd = -1;

    uint32_t fourcc = codec_to_fourcc(codec_id);
    if (!fourcc) {
        V4L2_LOG("不支持的 codec_id=%d", codec_id);
        return -1;
    }

    /* O_NONBLOCK 是必须的：事件队列空时 VIDIOC_DQEVENT 会阻塞在内核
     * v4l2_event_dequeue()（实测 /proc/PID/wchan 确认），poll 循环回不来。 */
    d->fd = open(DEC_NODE_PRIMARY, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (d->fd < 0) {
        V4L2_LOG("打开 %s 失败: %s", DEC_NODE_PRIMARY, strerror(errno));
        return -1;
    }

    /* OUTPUT 侧：告诉驱动我们要喂什么码流。 */
    struct v4l2_format f;

    /* msm_vidc 的 S_FMT(OUTPUT) 在分辨率与当前值相同时会提前返回，
     * 跳过 inst->bufq[OUTPUT_PORT].plane_sizes 的赋值：
     *   if (fourcc 相同 && width 相同 && height 相同) {
     *       dprintk("No change in OUTPUT port params"); return 0;
     *   }                                    (msm_vdec.c:732-738)
     * 驱动 open 后 OUTPUT 默认就是 1920x1088，所以直接设目标分辨率会命中它，
     * sizeimage 永远回填 0。先用一个不同分辨率做 dummy S_FMT 破开。
     * 实测：直接设 1920x1088 → sizeimage=0；
     *       先 1280x720 再 1920x1088 → sizeimage=16588800。 */
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.pixelformat = fourcc;
    f.fmt.pix_mp.width = (unsigned)((w == 1280 && h == 720) ? 640 : 1280);
    f.fmt.pix_mp.height = (unsigned)((w == 1280 && h == 720) ? 480 : 720);
    f.fmt.pix_mp.num_planes = 1;
    ioctl(d->fd, VIDIOC_S_FMT, &f);   /* dummy，忽略结果 */

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
        V4L2_LOG("警告: sizeimage 仍为 0，dummy S_FMT 未生效，退回 2MB");
        d->in_size = 2u * 1024u * 1024u;
    }
    V4L2_LOG("S_FMT(OUTPUT) OK: %ux%u sizeimage=%u", f.fmt.pix_mp.width,
             f.fmt.pix_mp.height, d->in_size);

    /* 帧率：msm_comm_get_mbs_per_sec() 用 inst->prop.fps 算负载。 */
    struct v4l2_streamparm sp;
    memset(&sp, 0, sizeof(sp));
    sp.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    sp.parm.output.timeperframe.numerator = 1;
    sp.parm.output.timeperframe.denominator = 30;
    ioctl(d->fd, VIDIOC_S_PARM, &sp);

    /* msm_vidc 私有控制项。CID 数值来自厂商 uapi 头
     * include/uapi/linux/v4l2-controls.h:684
     *   V4L2_CID_MPEG_MSM_VIDC_BASE = V4L2_CTRL_CLASS_MPEG|0x2000 = 0x00992000
     * 与 OMX HAL 活体 strace（pid omx@1.0-service）逐项对照得出： */
    struct v4l2_control ctl;
    static const struct { uint32_t id; int32_t val; const char *nm; } inits[] = {
        /* OUTPUT_ORDER: 0=显示序（重排后输出）1=解码序（不等重排）。
         *
         * ⚠️ 必须用解码序(1)，否则浏览器无法硬解。
         *
         * research/slowfeed.c 直接打 V4L2 实测首帧滞后（1080p H.264）：
         *     OUTPUT_ORDER=0（显示序）→ 送满 7 个单元，第 8 个才出首帧
         *     OUTPUT_ORDER=1（解码序）→ 送 4 个单元，第 5 个就出首帧
         * 差的 3 帧就是显示序重排的代价。
         *
         * 为什么这决定浏览器能否工作：Firefox 稳态只保持 5 帧在飞
         * （实测 pending 恒为 5，占 99.9%），而显示序滞后 7 > 5，
         * 于是它送完第 5 帧就等，解码器却还欠 2 帧料 —— 双方互等。
         * SyncSurface 只能超时放行、谎报就绪，Firefox 采到未填充的缓冲：
         * 实测 847 解码 / 792 丢弃 = 93.5%，画面几乎不动。
         * 这是结构性矛盾，调 DMD_PIPELINE_DEPTH 无解 —— 12/4/7 三个值都试过，
         * 分别是每帧放行(93.5% 丢帧)、每帧阻塞 5s 超时(fps 0.4、会话反复重建)、
         * 仍然每帧放行(91.8% 丢帧)。
         *
         * 出帧顺序变成解码序不影响帧内容的正确性：配对按 v4l2_buffer.timestamp
         * 携带的 unit_seq 精确匹配（见 decode.c 的 dmd_pending_take_locked），
         * 与出帧顺序完全解耦 —— ffmpeg 五码流逐字节回归 5/5 即为此提供证据，
         * 专门造的 B 帧顺序检验流（research/perf/order.mp4，220 个 B 帧、
         * 匀速横移方块）在 ffmpeg 路径下 300 帧位置零回跳、与软解逐帧一致。
         *
         * ⚠️ 但浏览器路径有代价，取舍如下（Firefox 140，order_long.mp4，20s）：
         *     ORDER=0 显示序: mediaTime 回退 0 处，但丢帧 99.79%
         *                     （950 解码/948 丢弃，20s 任务跑了 62s，不可用）
         *     ORDER=1 解码序: mediaTime 回退 2/465 = 0.43%，丢帧 4.17%
         * 回退量恰为一帧（33ms），是 Firefox 自己上屏调度偶尔换位，
         * 不是配对错误 —— Sync 只在请求的那张 surface 就绪时才返回。
         * 用可察觉但极少的顺序抖动换回可用的播放，这是当前硬件下的最优解：
         * 显示序滞后 7 > Firefox 在飞 5，互等无法靠软件消除（LOWLATENCY
         * 开关对显示序滞后无影响，slowfeed 实测两种设置都是 7）。 */
        { 0x00992003, 1,  "OUTPUT_ORDER"    },  /* base+3，1=解码序 */
        { 0x00992011, 2,  "EXTRADATA"       },  /* base+17，MENU，HAL 连设四种 */
        { 0x00992011, 25, "EXTRADATA"       },
        { 0x00992011, 31, "EXTRADATA"       },
        { 0x00992011, 29, "EXTRADATA"       },
        { 0x00992038, 1,  "LOWLATENCY_MODE" },  /* base+56 */
    };
    for (unsigned i = 0; i < sizeof(inits) / sizeof(inits[0]); i++) {
        memset(&ctl, 0, sizeof(ctl));
        ctl.id = inits[i].id;
        ctl.value = inits[i].val;
        if (ioctl(d->fd, VIDIOC_S_CTRL, &ctl) < 0)
            V4L2_LOG("S_CTRL %s(0x%x)=%d 失败: %s", inits[i].nm,
                     inits[i].id, inits[i].val, strerror(errno));
    }

    /* 决定性一项：必须启用 SECONDARY 分流模式。
     *   V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_MODE = base+22 = 0x00992016
     *   V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_SECONDARY = 1
     * PRIMARY（默认）下 CAPTURE 直接充当 DPB，start_streaming() 恒返回
     * -EINVAL。SECONDARY 启用 HAL_BUFFER_OUTPUT2，DPB 与 OPB 分离
     * （msm_vidc.c:1214-1222 会额外调 msm_comm_set_output_buffers()
     * 让驱动自行分配 DPB），校验路径不同，两侧 STREAMON 才能通过。
     * 单一变量对照实测：SECONDARY → OK；PRIMARY → EINVAL。
     * 附带好处：CAPTURE 可用线性 NV12，不必走 UBWC 的 Q128。 */
    memset(&ctl, 0, sizeof(ctl));
    ctl.id = 0x00992016;
    ctl.value = 1;
    if (ioctl(d->fd, VIDIOC_S_CTRL, &ctl) < 0) {
        V4L2_LOG("致命: STREAM_OUTPUT_MODE=SECONDARY 失败: %s", strerror(errno));
        goto fail;
    }

    /* 订阅事件。关键：msm_vidc 用的是**厂商私有事件**，
     * 从不发标准 V4L2_EVENT_SOURCE_CHANGE。
     *   V4L2_EVENT_MSM_VIDC_START = V4L2_EVENT_PRIVATE_START + 0x1000
     *                             = 0x08001000        (videodev2.h:2290)
     *     +1 FLUSH_DONE   +2 PORT_SETTINGS_SUFFICIENT
     *     +3 PORT_SETTINGS_INSUFFICIENT   +4 BITDEPTH_CHANGED
     *     +5 SYS_ERROR    +6 RELEASE_BUFFER_REFERENCE
     *     +7 RELEASE_UNQUEUED_BUFFER
     * 只订阅标准 SOURCE_CHANGE 会导致 poll 永不返回 POLLPRI。 */
    struct v4l2_event_subscription sub;
    static const unsigned evs[] = {
        DMD_EV_MSM_VIDC(1), DMD_EV_MSM_VIDC(2), DMD_EV_MSM_VIDC(3),
        DMD_EV_MSM_VIDC(4), DMD_EV_MSM_VIDC(5), DMD_EV_MSM_VIDC(6),
        DMD_EV_MSM_VIDC(7), V4L2_EVENT_SOURCE_CHANGE, V4L2_EVENT_EOS,
    };
    for (unsigned i = 0; i < sizeof(evs) / sizeof(evs[0]); i++) {
        memset(&sub, 0, sizeof(sub));
        sub.type = evs[i];
        ioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);   /* 单项失败非致命 */
    }

    /* OUTPUT 缓冲 + STREAMON。CAPTURE 侧要等 SOURCE_CHANGE 才能配。 */
    d->n_out = req_bufs(d->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                        DMD_V4L2_MAX_OUT);
    if (d->n_out <= 0) goto fail;
    if (d->n_out > DMD_V4L2_MAX_OUT) d->n_out = DMD_V4L2_MAX_OUT;
    if (bufs_alloc(d, d->out, d->n_out, d->in_size, 1) < 0) goto fail;

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    /* AV1 金字塔 B 结构下，解码器默认只对 show_frame=1 的帧吐 CAPTURE
     * 缓冲（正确行为），但 ffmpeg 的 VA-API 后端会对部分 show_frame=0 的
     * surface 也读像素。V4L2 标准控制 DISPLAY_DELAY(_ENABLE) 可要求解码器
     * 不做重排、逐帧立即输出，从而让每个 surface 都拿到内容。
     *
     * ⚠️ 更正（第 81 轮本机实测）：此前这里写"实测这两项 S_CTRL 均返回 OK"。
     * 在 nabu 的 msm_vidc 上实测 **S_CTRL 返回 EINVAL**，两项都设不进去。
     *
     * 当时那个"返回 OK"的结论很可能来自 G_CTRL —— 而这个驱动的 G_CTRL
     * **完全不校验 id**：拿 0xDEADBEEF 去读也返回成功、值为 0。
     * 所以 G_CTRL 成功不能作为"控制项存在"的证据，只有 S_CTRL 能。
     * 配套证据：VIDIOC_QUERYCTRL 遍历这个节点得到 0 项
     * （见 tools/probe_v4l2_ctrls.c）。
     *
     * 结论：DISPLAY_DELAY 这条路在 msm_vidc 上不存在。保留代码是因为它
     * 在别的 V4L2 stateful 解码器上是标准做法，且失败不致命；
     * 但**不要**再把它当成 AV1 缺陷的可行解法。
     * 用 DMD_V4L2_DISPLAY_DELAY=1 开启（在 msm_vidc 上只会打印设置失败）。 */
    if (getenv("DMD_V4L2_DISPLAY_DELAY")) {
        struct v4l2_control ctl;
        memset(&ctl, 0, sizeof(ctl));
        ctl.id = V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE;
        ctl.value = 1;
        if (ioctl(d->fd, VIDIOC_S_CTRL, &ctl) == 0) {
            memset(&ctl, 0, sizeof(ctl));
            ctl.id = V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY;
            ctl.value = 0;
            if (ioctl(d->fd, VIDIOC_S_CTRL, &ctl) == 0)
                V4L2_LOG("已启用 DISPLAY_DELAY=0（逐帧输出，不重排）");
            else
                V4L2_LOG("DISPLAY_DELAY 设置失败，沿用默认重排输出");
        } else {
            V4L2_LOG("DISPLAY_DELAY_ENABLE 设置失败，沿用默认重排输出");
        }
    }

    /* 顺序很关键：msm_vidc.c:1294-1302 里第一个 STREAMON 因对侧未 streaming
     * 而跳过 start_streaming()（假通过），第二个才真正执行全部校验。
     * SECONDARY 模式下不需要等 SOURCE_CHANGE 才配 CAPTURE ——
     * G_FMT(CAPTURE) 在 session_init 后就能给出正确的 1920x1088，
     * 所以两侧都先配好，再让 STREAMON(OUTPUT) 触发校验。 */
    if (setup_capture(d) < 0) {
        V4L2_LOG("CAPTURE 配置失败");
        goto fail;
    }

    if (xioctl(d->fd, VIDIOC_STREAMON, &type, "STREAMON(OUTPUT)") < 0) goto fail;
    d->out_streaming = 1;

    V4L2_LOG("会话就绪: OUTPUT %d x %u 字节, CAPTURE %d x %u 字节",
             d->n_out, d->in_size, d->n_cap, d->cap_size);
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

    /* G_FMT(CAPTURE) 返回的可能是驱动 open 时的残留默认值 1920x1088，
     * 不随 S_FMT(OUTPUT) 的分辨率联动。实测 4K 码流：
     *   S_FMT(OUTPUT) 3840x2160 之后 G_FMT(CAPTURE) 仍是 1920x1088，
     *   CAPTURE 按 1080p 配好，固件随后报 PORT_SETTINGS h=2160 w=3840，
     *   几何不符 → 一帧都取不到（ffmpeg 侧是
     *   "Failed to read image from surface: 18 invalid parameter"）。
     * 所以要用 OUTPUT 侧协商出的尺寸覆盖它。
     * msm_vidc 不支持缩放（capability.scale_x/y 为 0，
     * msm_vidc_common.c:5613-5625 要求两侧像素数严格相等），
     * 两侧本来就必须一致。 */
    if (d->out_w > 0 && d->out_h > 0 &&
        ((unsigned)d->out_w != f.fmt.pix_mp.width ||
         (unsigned)d->out_h != f.fmt.pix_mp.height)) {
        V4L2_LOG("CAPTURE 残留 %ux%u，改用 OUTPUT 协商值 %dx%d",
                 f.fmt.pix_mp.width, f.fmt.pix_mp.height, d->out_w, d->out_h);
        f.fmt.pix_mp.width = (unsigned)d->out_w;
        f.fmt.pix_mp.height = (unsigned)d->out_h;
    }

    /* DMD_PROBE_10BIT=1：把 CAPTURE 设成 QP10（P10 Venus，10bit 半平面）
     * 而不是 NV12，用来判定"10bit 解不出来"是硬件/固件的限制还是驱动没实现。
     *
     * ⚠️ 这是**探测开关，不是功能开关**。上层（surface 分配、VAImage、
     * export）全都假设 8bit NV12：每像素字节数、UV 平面偏移、
     * DRM_FORMAT_NV12 都是按 8bit 写死的，所以开了它像素必然错乱。
     * 它只回答一个问题：固件在 Main10 码流下会不会吐 CAPTURE 缓冲。 */
    uint32_t want = V4L2_PIX_FMT_NV12;
    const char *probe10 = getenv("DMD_PROBE_10BIT");
    if (probe10 && (probe10[0] == '1' || probe10[0] == '2')) {
        want = (probe10[0] == '2') ? v4l2_fourcc('Q', '1', '2', 'A')
                                   : v4l2_fourcc('Q', 'P', '1', '0');
        V4L2_LOG("⚠️ DMD_PROBE_10BIT=%c：CAPTURE 试设 %s（仅探测，像素会错）",
                 probe10[0], probe10[0] == '2' ? "Q12A(TP10 UBWC)"
                                               : "QP10(P10 Venus)");
    }
    f.fmt.pix_mp.pixelformat = want;
    if (xioctl(d->fd, VIDIOC_S_FMT, &f, "S_FMT(CAPTURE)") < 0) return -1;
    if (f.fmt.pix_mp.pixelformat != want)
        V4L2_LOG("CAPTURE 格式被驱动改写为 %c%c%c%c",
                 (char)(f.fmt.pix_mp.pixelformat & 0xFF),
                 (char)((f.fmt.pix_mp.pixelformat >> 8) & 0xFF),
                 (char)((f.fmt.pix_mp.pixelformat >> 16) & 0xFF),
                 (char)((f.fmt.pix_mp.pixelformat >> 24) & 0xFF));

    d->w = (int)f.fmt.pix_mp.width;
    d->h = (int)f.fmt.pix_mp.height;
    d->cap_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    d->stride = (int)f.fmt.pix_mp.plane_fmt[0].bytesperline;
    /* 覆盖过 width 时驱动可能不回填 bytesperline（实测 4K：width 改成 3840
     * 后 bytesperline 仍是 1920），stride 小于宽度会让上层按错误行距取像素。
     * NV12 的 stride 至少等于宽度。
     *
     * ⚠️ 10bit 格式每像素 2 字节，下限是 width*2 而不是 width。
     * 实测（Main10 1080p，CAPTURE=QP10）：固件给的 sizeimage 是 6270976，
     * 而 3840*1088*3/2 = 6266880（差值是对齐余量），也就是说它期望
     * stride=3840；按 8bit 钳成 1920 后几何不符，固件持续报
     * PORT_SETTINGS(INSUFFICIENT) 且一帧不吐。 */
    const int bpp2 = (want == v4l2_fourcc('Q', 'P', '1', '0') ||
                      want == v4l2_fourcc('Q', '1', '2', 'A') ||
                      want == v4l2_fourcc('P', '0', '1', '0')) ? 2 : 1;
    if (d->stride < d->w * bpp2) d->stride = d->w * bpp2;
    /* NV12 单平面布局：Y 平面高度即 slice_height，UV 紧随其后。
     *
     * ⚠️ 不要用 sizeimage 反推。msm_vidc 的 sizeimage 含额外对齐余量，
     * 整数除法会得到偏大的值：
     *   3137536 * 2 / (1920 * 3) = 1089.16 → 1089
     * 而真实的 Y 平面高度是 1088。多出的这一行会让上层 nv12_copy 按
     * 1089 行读源缓冲、越过末尾 SIGSEGV（实测栈：__memcpy_generic
     * → nv12_copy(src_slice=1089, dst_slice=1088) → dmd_GetImage）。
     *
     * 正常路径下 f.fmt.pix_mp.height 就是驱动对齐后的 slice_height
     * （1080p 是 1088），直接用它。
     *
     * ⚠️ 但走了上面那段"用 OUTPUT 协商值覆盖 CAPTURE 残留"之后，height 是
     * 我们自己写进去的**未对齐**值，驱动不会替我们对齐。4K 实测：
     *   写入 height=2160 → slice_height 也成了 2160，
     *   而驱动的 sizeimage=12537856 对应的真实 slice_height 是 **2176**
     *   （3840*2176*3/2 = 12533760，再加 4096 对齐余量）。
     * 后果是 UV 平面起点算成 3840*2160=8294400 而实际在 3840*2176=8355840，
     * 表现为 **Y 平面全对、UV 平面从 Y 末尾起全错**（逐帧实测：
     * Y 采样差异 0，UV 采样差异 281-301）。
     *
     * 所以要按 msm_vidc 的 32 行对齐规则自己补齐，再用 sizeimage 校验。 */
    d->slice_height = (int)f.fmt.pix_mp.height;
    if (d->slice_height < d->h) d->slice_height = d->h;
    if (d->stride > 0 && d->cap_size > 0) {
        /* sizeimage 能容纳的最大 slice_height（含对齐余量，故向下取）。 */
        int fit = (int)((size_t)d->cap_size * 2u / (size_t)(d->stride * 3));
        /* msm_vidc 的 CAPTURE 高度按 32 对齐（1080→1088，2160→2176）。 */
        int aligned = (d->slice_height + 31) & ~31;
        if (aligned <= fit)
            d->slice_height = aligned;
        else if (d->slice_height > fit)
            d->slice_height = fit & ~1;
    }

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

    /* 设了 EXTRADATA 控制项后 CAPTURE 是 2 平面，第二个是 extradata。
     * valid_v4l2_buffer() 要求 b->length == bufq[port].num_planes
     * （msm_vidc.c:452-461），平面数不符会直接 EINVAL。 */
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
                        (unsigned)d->extra_size) < 0)
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
        v.memory = V4L2_MEMORY_USERPTR;
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
    if (qbuf_userptr(d->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, idx,
                    &d->out[idx], (unsigned)len, pts_us, -1, 0) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------- recv */

/* 非阻塞取一帧 CAPTURE。返回 1=拿到画面, 2=EOS 标记帧, 0=当前没有, -1=错误。
 * 设备是 O_NONBLOCK，队列空时 DQBUF 直接返回 EAGAIN，不会阻塞。 */
static int try_dq_capture(struct dmd_v4l2_dec *d, uint8_t **out_data,
                          size_t *out_len, uint64_t *out_pts, int *out_index)
{
    struct v4l2_buffer v;
    struct v4l2_plane p[VIDEO_MAX_PLANES];
    memset(&v, 0, sizeof(v));
    memset(p, 0, sizeof(p));
    v.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    v.memory = V4L2_MEMORY_USERPTR;
    v.m.planes = p;
    v.length = (unsigned)(d->cap_planes > 0 ? d->cap_planes : 1);

    if (ioctl(d->fd, VIDIOC_DQBUF, &v) != 0)
        return 0;                       /* EAGAIN：现在没帧 */
    if (v.index >= (unsigned)d->n_cap)
        return -1;
    d->cap[v.index].queued = 0;

    /* bytesused==0 且带 LAST 标记 = 流结束标记帧，不是画面。 */
    if (p[0].bytesused == 0) {
        if (v.flags & V4L2_BUF_FLAG_LAST) {
            d->saw_eos = 1;
            return 2;
        }
        /* 空帧但非 LAST：还回去继续等。 */
        qbuf_userptr(d->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                     (int)v.index, &d->cap[v.index], 0, 0,
                     d->cap_planes > 1 ? d->extra[v.index].dbuf_fd : -1,
                     (unsigned)d->extra_size);
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

int dmd_v4l2_recv(struct dmd_v4l2_dec *d, uint8_t **out_data, size_t *out_len,
                  uint64_t *out_pts, int *out_index, int timeout_ms)
{
    if (d->fd < 0) return -1;

    /* ⚠️ 先直接试一次非阻塞 DQBUF，再决定要不要 poll。
     *
     * msm_vidc 的 POLLIN 不能当作"有帧"的可靠信号：帧已经在 CAPTURE 队列里
     * 了，poll 却可能不置位，于是空转到超时才返回，帧间隔被硬生生拖成
     * 超时值。research/slowfeed.c 实测（真实 27Mbps 码流，维持 5 帧在飞，
     * 300 帧）：
     *     poll 超时 100ms → 第 250 帧后间隔稳定在 111ms（9fps）
     *     poll 超时  20ms → 同一位置间隔变成 41~53ms
     * 间隔跟着超时值走，正是空转的指纹。改成"先直取、取不到才 poll"后：
     *     慢帧（>33ms）占比 15.3% → 3.4%，吞吐 32.6fps → 57.2fps
     * 这也解释了浏览器侧 15.8% 的丢帧 —— 与 15.3% 的慢帧率吻合。 */
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

    /* 事件处理。msm_vidc 发的是私有事件（见 open 里的订阅列表）。 */
    if (pfd.revents & POLLPRI) {
        struct v4l2_event ev;
        int guard = 0;
        memset(&ev, 0, sizeof(ev));
        /* 上限保护：设备是 O_NONBLOCK，队列空时 DQEVENT 返回 EAGAIN 而非阻塞。 */
        while (guard++ < 8 && ioctl(d->fd, VIDIOC_DQEVENT, &ev) == 0) {
            unsigned rel = ev.type - V4L2_EVENT_MSM_VIDC_START;

            if (ev.type == DMD_EV_MSM_VIDC(2) || ev.type == DMD_EV_MSM_VIDC(3) ||
                ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                const unsigned *ed = (const unsigned *)ev.u.data;
                V4L2_LOG("PORT_SETTINGS%s: h=%u w=%u bitdepth=%u picstruct=%u",
                         ev.type == DMD_EV_MSM_VIDC(3) ? "(INSUFFICIENT)"
                                                       : "(SUFFICIENT)",
                         ed[0], ed[1], ed[2], ed[3]);

                if (!d->cap_ready && setup_capture(d) < 0) return -1;

                /* INSUFFICIENT 表示已配好的 CAPIURE 缓冲不满足固件要求
                 * （典型场景：10bit 码流下每像素 2 字节，按 8bit 算出的
                 * sizeimage 只有一半）。此时只发 SESSION_CONTINUE 是不够的
                 * —— 缓冲几何没变，固件继续停在 reconfig 等待态，一帧不吐。
                 *
                 * 实测（Main10 1080p，CAPTURE 设 QP10 或 Q12A）：
                 * 送入 8 单元、收到 0 帧，日志只有 INSUFFICIENT，
                 * 从未出现 SUFFICIENT。
                 *
                 * DMD_PROBE_10BIT_RECFG=1 时在这里尝试真正的重协商。
                 * ⚠️ 仓库既有注释记录：STREAMOFF 会把 state 打回
                 * MSM_VIDC_START_DONE 以下并触发 SYS_ERROR。所以这条路径
                 * 是**探测**用的，用来验证那条结论在 CAPTURE 侧是否也成立。 */
                if (ev.type == DMD_EV_MSM_VIDC(3) && d->cap_ready) {
                    const char *rec = getenv("DMD_PROBE_10BIT_RECFG");
                    if (rec && rec[0] == '1' && !d->cap_recfg_tried) {
                        d->cap_recfg_tried = 1;
                        V4L2_LOG("⚠️ INSUFFICIENT：尝试重协商 CAPTURE"
                                 "（探测路径）");
                        int ct = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                        if (d->cap_streaming) {
                            if (xioctl(d->fd, VIDIOC_STREAMOFF, &ct,
                                       "STREAMOFF(CAPTURE)") == 0)
                                d->cap_streaming = 0;
                        }
                        /* 释放旧缓冲后重新按固件报的几何配一遍。 */
                        struct v4l2_requestbuffers rb0;
                        memset(&rb0, 0, sizeof(rb0));
                        rb0.type = ct;
                        rb0.memory = V4L2_MEMORY_DMABUF;
                        rb0.count = 0;
                        xioctl(d->fd, VIDIOC_REQBUFS, &rb0, "REQBUFS(CAPTURE,0)");
                        d->cap_ready = 0;
                        d->n_cap = 0;
                        if (setup_capture(d) < 0) {
                            V4L2_LOG("重协商 CAPTURE 失败");
                            return -1;
                        }
                        V4L2_LOG("重协商完成: %ux%u stride=%d slice=%d "
                                 "%d 缓冲 x %u 字节",
                                 (unsigned)d->w, (unsigned)d->h, d->stride,
                                 d->slice_height, d->n_cap, d->cap_size);
                    }
                }

                /* 驱动在事件处理里无条件置 inst->in_reconfig = true
                 * （msm_vidc_common.c:1761），固件随后停在 reconfig 等待态。
                 * msm_comm_session_continue() 只有两个调用点：
                 *   1. start_streaming() 内 (msm_vidc.c:1244)
                 *   2. msm_vidc_comm_cmd() 的 V4L2_QCOM_CMD_SESSION_CONTINUE
                 *      分支 (msm_vidc_common.c:4155)
                 * 走 (1) 需要重跑 STREAMON，实测必然触发 SYS_ERROR
                 * （STREAMOFF 把 state 打回 MSM_VIDC_START_DONE 以下）。
                 * 走 (2) 才是正解：不动队列状态。 */
                if (!d->reconfig_done) {
                    struct v4l2_decoder_cmd dc;
                    memset(&dc, 0, sizeof(dc));
                    dc.cmd = DMD_V4L2_QCOM_CMD_SESSION_CONTINUE;
                    if (ioctl(d->fd, VIDIOC_DECODER_CMD, &dc) == 0) {
                        d->reconfig_done = 1;
                        V4L2_LOG("已发 SESSION_CONTINUE");
                    } else {
                        V4L2_LOG("SESSION_CONTINUE 失败: %s", strerror(errno));
                    }
                }
            } else if (ev.type == DMD_EV_MSM_VIDC(5)) {
                V4L2_LOG("致命: 收到 SYS_ERROR，会话已失效");
                return -1;
            } else if (ev.type == V4L2_EVENT_EOS || ev.type == DMD_EV_MSM_VIDC(1)) {
                V4L2_LOG("收到 EOS/FLUSH_DONE");
                d->saw_eos = 1;
            } else if (rel <= 7) {
                V4L2_LOG("私有事件 +%u", rel);
            }
            memset(&ev, 0, sizeof(ev));
        }
    }

    if (pfd.revents & POLLOUT)
        reap_output(d);

    /* poll 说有帧就再取一次。注意不能只信 POLLIN：上面开头那次直取已经
     * 说明它会漏报，所以这里 cap_ready 就试，不要求 revents 带 POLLIN。 */
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
                       (unsigned)d->extra_size);
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
                qbuf_userptr(d->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, i,
                            &d->out[i], 0, 0, -1, 0);
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
    bufs_free(d->extra, DMD_V4L2_MAX_CAP);
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
    d->heap_kind = HEAP_NONE;

    d->cap_ready = 0;
    d->n_out = d->n_cap = 0;
}
