/* SPDX-License-Identifier: Apache-2.0
 *
 * Stateful V4L2 encoder client.  Independently implemented against the Linux
 * userspace ABI; no code from termux-va, MediaCodec, Mesa or a GPL encoder is
 * included here.  The only vendor definitions below are ioctl ABI numbers.
 *
 * References for protocol/ABI facts (not implementation code):
 * https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-encoder.html
 * MiCode/Xiaomi_Kernel_OpenSource, nabu-r-oss:
 *   include/uapi/linux/{v4l2-controls.h,videodev2.h}
 *   drivers/media/platform/msm/vidc/{msm_venc.c,msm_vidc.c}
 * qualcomm-linux/video-driver, video.qclinux.main:
 *   driver/vidc/src/msm_venc.c; driver/platform/sun/src/sun.c
 *
 * The 4.14 driver imports dma-buf descriptors from USERPTR plane.reserved[0].
 * New msm_vidc uses ordinary DMABUF; ordinary stateful drivers may use MMAP.
 * Selection is per encoder instance and never alters the decoder backend.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "v4l2_encoder.h"
#include "sandbox_heap.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ENC_MAX_BUFFERS 32u
#define ENC_MAX_ALLOCATION (256u * 1024u * 1024u)
#define ENC_DEFAULT_TIMEOUT 15000
#define ENC_MSM_BASE (V4L2_CTRL_CLASS_MPEG | 0x2000)
#define ENC_MSM_IDR_PERIOD (ENC_MSM_BASE + 5)
#define ENC_MSM_P_FRAMES (ENC_MSM_BASE + 6)
#define ENC_MSM_B_FRAMES (ENC_MSM_BASE + 7)
#define ENC_MSM_IFRAME (ENC_MSM_BASE + 8)
#define ENC_MSM_HEVC_PROFILE (ENC_MSM_BASE + 40)
#define ENC_MSM_HEVC_LEVEL (ENC_MSM_BASE + 41)
#define ENC_MSM_SLICE_DELIVERY (ENC_MSM_BASE + 18)
#define ENC_MSM_EVENT_ERROR (V4L2_EVENT_PRIVATE_START + 0x1000 + 5)
#define ENC_MSM_CODECCONFIG 0x00020000u
#define ENC_MSM_SUBFRAME 0x00000080u
#define ENC_MSM_IDRFRAME 0x80000000u

#ifndef V4L2_PIX_FMT_HEVC
#define V4L2_PIX_FMT_HEVC v4l2_fourcc('H', 'E', 'V', 'C')
#endif
#ifndef V4L2_PIX_FMT_VP9
#define V4L2_PIX_FMT_VP9 v4l2_fourcc('V', 'P', '9', '0')
#endif
#ifndef V4L2_PIX_FMT_NV12M
#define V4L2_PIX_FMT_NV12M v4l2_fourcc('N', 'M', '1', '2')
#endif
#ifndef V4L2_CID_MPEG_VIDEO_HEVC_PROFILE
#define V4L2_CID_MPEG_VIDEO_HEVC_PROFILE (V4L2_CID_MPEG_BASE + 615)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_HEVC_LEVEL
#define V4L2_CID_MPEG_VIDEO_HEVC_LEVEL (V4L2_CID_MPEG_BASE + 616)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_VP9_PROFILE
#define V4L2_CID_MPEG_VIDEO_VP9_PROFILE (V4L2_CID_MPEG_BASE + 512)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER
#define V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER (V4L2_CID_MPEG_BASE + 226)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_H264_I_FRAME_MIN_QP
#define V4L2_CID_MPEG_VIDEO_H264_I_FRAME_MIN_QP (V4L2_CID_MPEG_BASE + 385)
#define V4L2_CID_MPEG_VIDEO_H264_I_FRAME_MAX_QP (V4L2_CID_MPEG_BASE + 386)
#define V4L2_CID_MPEG_VIDEO_H264_P_FRAME_MIN_QP (V4L2_CID_MPEG_BASE + 387)
#define V4L2_CID_MPEG_VIDEO_H264_P_FRAME_MAX_QP (V4L2_CID_MPEG_BASE + 388)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA
#define V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA (V4L2_CID_MPEG_BASE + 360)
#define V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA (V4L2_CID_MPEG_BASE + 361)
#define V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE (V4L2_CID_MPEG_BASE + 362)
#endif

/* Stable allocation/cache-sync UAPIs, declared locally for older sysroots. */
struct enc_heap_alloc {
    uint64_t len;
    uint32_t fd, fd_flags;
    uint64_t heap_flags;
};
#define ENC_HEAP_ALLOC _IOWR('H', 0, struct enc_heap_alloc)
struct enc_ion_alloc {
    uint64_t len;
    uint32_t heap_id_mask, flags, fd, unused;
};
struct enc_ion_heap {
    char name[32];
    uint32_t type, id, reserved[3];
};
struct enc_ion_query {
    uint32_t count, reserved0;
    uint64_t heaps;
    uint32_t reserved[2];
};
#define ENC_ION_ALLOC _IOWR('I', 0, struct enc_ion_alloc)
#define ENC_ION_QUERY _IOWR('I', 8, struct enc_ion_query)
struct enc_dma_sync { uint64_t flags; };
#define ENC_DMA_SYNC _IOW('b', 0, struct enc_dma_sync)
#define ENC_DMA_READ 1u
#define ENC_DMA_WRITE 2u
#define ENC_DMA_END 4u

struct enc_plane { int fd; void *map; size_t size; };
struct enc_buffer { struct enc_plane plane[VIDEO_MAX_PLANES]; int queued; };
struct enc_queue {
    enum v4l2_buf_type type;
    enum v4l2_memory memory;
    unsigned count, planes;
    size_t size[VIDEO_MAX_PLANES];
    int requested, streaming;
    struct enc_buffer buffer[ENC_MAX_BUFFERS];
};
struct dmd_v4l2_encoder {
    int fd, heap_fd, heap_ion, msm, legacy, failed;
    uint32_t ion_mask;
    struct dmd_enc_config cfg;
    struct enc_queue input, coded;
    unsigned stride, slice_height, uv_stride;
    int nv12m, timeout_ms;
    uint64_t sequence;
};

static int enc_ioctl(int fd, unsigned long request, void *arg)
{
    int rc;
    do { rc = ioctl(fd, request, arg); } while (rc < 0 && errno == EINTR);
    if (rc < 0 && getenv("DMD_VA_LOG") && errno != EAGAIN) {
        int saved = errno;
        fprintf(stderr, "[v4l2-enc] ioctl 0x%lx: %s\n", request, strerror(saved));
        errno = saved;
    }
    return rc;
}

static int unsupported(int error)
{
    return error == EINVAL || error == ENOTTY || error == EOPNOTSUPP;
}

static const char *device_path(void)
{
    const char *value = getenv("DMD_VA_ENCODE_DEVICE");
    return value && *value ? value : "/dev/video33";
}

static uint32_t coded_fourcc(enum dmd_enc_profile profile)
{
    switch (profile) {
    case DMD_ENC_H264_BASELINE:
    case DMD_ENC_H264_MAIN:
    case DMD_ENC_H264_HIGH: return V4L2_PIX_FMT_H264;
    case DMD_ENC_HEVC_MAIN: return V4L2_PIX_FMT_HEVC;
    case DMD_ENC_VP9_0: return V4L2_PIX_FMT_VP9;
    default: return 0;
    }
}

static int has_format(int fd, enum v4l2_buf_type type, uint32_t fourcc)
{
    for (unsigned i = 0; i < 256; ++i) {
        struct v4l2_fmtdesc fmt = {0};
        fmt.index = i;
        fmt.type = type;
        if (enc_ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
            if (errno == EINVAL) return 0;
            return -1;
        }
        if (fmt.pixelformat == fourcc) return 1;
    }
    errno = EOVERFLOW;
    return -1;
}

static int check_device(int fd, int *msm)
{
    struct v4l2_capability cap = {0};
    if (enc_ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) return -1;
    uint32_t bits = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                  ? cap.device_caps : cap.capabilities;
    if (!(bits & V4L2_CAP_STREAMING) ||
        (!(bits & V4L2_CAP_VIDEO_M2M_MPLANE) &&
         (bits & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE)) !=
                 (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE))) {
        errno = ENOTSUP;
        return -1;
    }
    *msm = memmem(cap.driver, sizeof(cap.driver), "msm_vidc", 8) != NULL ||
           memmem(cap.card, sizeof(cap.card), "msm_vidc", 8) != NULL;
    return 0;
}

static int set_control(int fd, uint32_t id, int value)
{
    struct v4l2_control ctrl = { .id = id, .value = value };
    int rc = enc_ioctl(fd, VIDIOC_S_CTRL, &ctrl);
    if (rc < 0 && getenv("DMD_VA_LOG")) {
        int saved = errno;
        fprintf(stderr, "[v4l2-enc] control 0x%x=%d: %s\n", id, value, strerror(saved));
        errno = saved;
    }
    return rc;
}

/* QUERYCTRL/G_CTRL on old msm_vidc are unreliable. A successful S_CTRL is
 * the supported operation there; do not reject it just because QUERY fails.
 * On regular drivers, verify that S_CTRL did not silently clamp the value. */
static int exact_control(int fd, uint32_t id, int value, int msm)
{
    if (set_control(fd, id, value) < 0) return -1;
    if (!msm) {
        struct v4l2_queryctrl query = { .id = id };
        if (enc_ioctl(fd, VIDIOC_QUERYCTRL, &query) == 0 &&
            query.type != V4L2_CTRL_TYPE_BUTTON) {
            struct v4l2_control got = { .id = id };
            if (enc_ioctl(fd, VIDIOC_G_CTRL, &got) < 0) return -1;
            if (got.value != value) { errno = ENOTSUP; return -1; }
        }
    }
    return 0;
}

static int profile_control(int fd, enum dmd_enc_profile profile, int msm)
{
    uint32_t id;
    int value;
    switch (profile) {
    case DMD_ENC_H264_BASELINE:
        id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
        value = V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
        break;
    case DMD_ENC_H264_MAIN:
        id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
        value = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
        break;
    case DMD_ENC_H264_HIGH:
        id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
        value = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
        break;
    case DMD_ENC_HEVC_MAIN: id = V4L2_CID_MPEG_VIDEO_HEVC_PROFILE; value = 0; break;
    case DMD_ENC_VP9_0: id = V4L2_CID_MPEG_VIDEO_VP9_PROFILE; value = 0; break;
    default: errno = EINVAL; return -1;
    }
    if (exact_control(fd, id, value, msm) == 0) return 0;
    if (profile == DMD_ENC_HEVC_MAIN && msm && unsupported(errno))
        return exact_control(fd, ENC_MSM_HEVC_PROFILE, 0, 1);
    return -1;
}

static int check_frame_size(int fd, uint32_t fourcc, unsigned width, unsigned height)
{
    int have_sizes = 0;
    for (unsigned index = 0; index < 256; ++index) {
        struct v4l2_frmsizeenum sizes = {0};
        sizes.index = index;
        sizes.pixel_format = fourcc;
        if (enc_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &sizes) < 0) {
            if (!unsupported(errno)) return -1;
            if (!have_sizes) return 0; /* Optional ioctl is not implemented. */
            errno = ENOTSUP;
            return -1;
        }
        have_sizes = 1;
        if (sizes.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            if (width == sizes.discrete.width && height == sizes.discrete.height)
                return 0;
            continue;
        }
        if (sizes.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
            sizes.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
            const struct v4l2_frmsize_stepwise *s = &sizes.stepwise;
            if (width < s->min_width || width > s->max_width ||
                height < s->min_height || height > s->max_height) break;
            if (sizes.type == V4L2_FRMSIZE_TYPE_STEPWISE &&
                ((!s->step_width || !s->step_height) ||
                 (width - s->min_width) % s->step_width ||
                 (height - s->min_height) % s->step_height)) break;
            return 0;
        }
        errno = EPROTO;
        return -1;
    }
    errno = ENOTSUP;
    return -1;
}

static int choose_coded_format(int fd, enum dmd_enc_profile profile,
                               unsigned width, unsigned height)
{
    uint32_t fourcc = coded_fourcc(profile);
    int supported = has_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, fourcc);
    if (supported <= 0) { if (!supported) errno = ENOTSUP; return -1; }
    if (check_frame_size(fd, fourcc, width, height) < 0) return -1;
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.pixelformat = fourcc;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (enc_ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) return -1;
    if (fmt.fmt.pix_mp.pixelformat != fourcc) { errno = ENOTSUP; return -1; }
    return 0;
}

static int probe_coded_format(int fd, enum dmd_enc_profile profile)
{
    uint32_t fourcc = coded_fourcc(profile);
    int exists = has_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, fourcc);
    if (exists <= 0) { if (!exists) errno = ENOTSUP; return -1; }
    unsigned width = 1920, height = 1080;
    struct v4l2_frmsizeenum sizes = { .pixel_format = fourcc };
    if (enc_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &sizes) == 0) {
        if (sizes.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            width = sizes.discrete.width;
            height = sizes.discrete.height;
        } else if (sizes.type == V4L2_FRMSIZE_TYPE_CONTINUOUS ||
                   sizes.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
            width = sizes.stepwise.min_width;
            height = sizes.stepwise.min_height;
        } else { errno = EPROTO; return -1; }
        if (!width || !height) { errno = EPROTO; return -1; }
    } else if (!unsupported(errno)) {
        return -1;
    }
    /* Prefer an advertised legal geometry. A small encoder must not lose
     * all profiles merely because it cannot configure our old 1080p probe. */
    return choose_coded_format(fd, profile, width, height);
}

uint32_t dmd_v4l2_enc_probe(void)
{
    uint32_t mask = 0;
    int saved = ENOTSUP;
    /* Separate open instances keep profile/format probing independent. No
     * REQBUFS, allocation or STREAMON is needed for capability enumeration. */
    for (int p = DMD_ENC_H264_BASELINE; p <= DMD_ENC_VP9_0; ++p) {
        int fd = open(device_path(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { saved = errno; break; }
        int msm = 0;
        if (check_device(fd, &msm) == 0 &&
            probe_coded_format(fd, (enum dmd_enc_profile)p) == 0 &&
            profile_control(fd, (enum dmd_enc_profile)p, msm) == 0) {
            int nv12 = has_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                  V4L2_PIX_FMT_NV12);
            if (!nv12) nv12 = has_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                         V4L2_PIX_FMT_NV12M);
            if (nv12 > 0) mask |= DMD_ENC_CAP(p);
        }
        saved = errno;
        close(fd);
    }
    errno = mask ? 0 : saved;
    return mask;
}

int dmd_v4l2_enc_get_limits(enum dmd_enc_profile profile,
                            unsigned *min_width, unsigned *min_height,
                            unsigned *max_width, unsigned *max_height)
{
    uint32_t fourcc = coded_fourcc(profile);
    if (!fourcc || !min_width || !min_height || !max_width || !max_height) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(device_path(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    int msm, rc = -1;
    if (check_device(fd, &msm) < 0) goto done;
    int exists = has_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, fourcc);
    if (exists <= 0) { if (!exists) errno = ENOTSUP; goto done; }
    /* Some vendor kernels return generic limits even for unsupported codec
     * fourcc values. ENUM_FMT above is mandatory before trusting this ioctl. */
    struct v4l2_frmsizeenum sizes = { .pixel_format = fourcc };
    if (enc_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &sizes) < 0) goto done;
    if (sizes.type != V4L2_FRMSIZE_TYPE_CONTINUOUS &&
        sizes.type != V4L2_FRMSIZE_TYPE_STEPWISE) { errno = ENOTSUP; goto done; }
    if (!sizes.stepwise.min_width || !sizes.stepwise.min_height ||
        sizes.stepwise.min_width > sizes.stepwise.max_width ||
        sizes.stepwise.min_height > sizes.stepwise.max_height) {
        errno = EPROTO;
        goto done;
    }
    *min_width = sizes.stepwise.min_width;
    *min_height = sizes.stepwise.min_height;
    *max_width = sizes.stepwise.max_width;
    *max_height = sizes.stepwise.max_height;
    rc = 0;
done:
    {
        int saved = errno;
        close(fd);
        errno = saved;
    }
    return rc;
}

static void queue_init(struct enc_queue *queue, enum v4l2_buf_type type)
{
    queue->type = type;
    for (unsigned i = 0; i < ENC_MAX_BUFFERS; ++i)
        for (unsigned p = 0; p < VIDEO_MAX_PLANES; ++p)
            queue->buffer[i].plane[p].fd = -1;
}

static int queue_format(struct dmd_v4l2_encoder *enc, struct enc_queue *queue,
                        struct v4l2_format *fmt)
{
    memset(fmt, 0, sizeof(*fmt));
    fmt->type = queue->type;
    if (enc_ioctl(enc->fd, VIDIOC_G_FMT, fmt) < 0) return -1;
    unsigned planes = fmt->fmt.pix_mp.num_planes;
    if (!planes || planes > VIDEO_MAX_PLANES) { errno = EPROTO; return -1; }
    queue->planes = planes;
    for (unsigned p = 0; p < planes; ++p) {
        queue->size[p] = fmt->fmt.pix_mp.plane_fmt[p].sizeimage;
        if (!queue->size[p] || queue->size[p] > ENC_MAX_ALLOCATION) {
            errno = EOVERFLOW;
            return -1;
        }
    }
    return 0;
}

static int configure_level(struct dmd_v4l2_encoder *enc)
{
    if (!enc->cfg.level) return 0;
    static const unsigned h264[] = {
        10, 9, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42,
        50, 51, 52, 60, 61, 62
    };
    static const unsigned hevc[] = {30,60,63,90,93,120,123,150,153,156,180,183,186};
    const unsigned *levels = enc->cfg.profile == DMD_ENC_HEVC_MAIN ? hevc : h264;
    size_t count = enc->cfg.profile == DMD_ENC_HEVC_MAIN
                 ? sizeof(hevc) / sizeof(hevc[0]) : sizeof(h264) / sizeof(h264[0]);
    if (enc->cfg.profile == DMD_ENC_VP9_0) { errno = ENOTSUP; return -1; }
    for (size_t i = 0; i < count; ++i) {
        if (levels[i] != enc->cfg.level) continue;
        uint32_t id = enc->cfg.profile == DMD_ENC_HEVC_MAIN
                    ? V4L2_CID_MPEG_VIDEO_HEVC_LEVEL : V4L2_CID_MPEG_VIDEO_H264_LEVEL;
        if (exact_control(enc->fd, id, (int)i, enc->msm) == 0) return 0;
        if (enc->cfg.profile == DMD_ENC_HEVC_MAIN && enc->msm && unsupported(errno))
            return exact_control(enc->fd, ENC_MSM_HEVC_LEVEL, (int)i * 2, 1);
        return -1;
    }
    errno = EINVAL;
    return -1;
}

static int optional_control(struct dmd_v4l2_encoder *enc, uint32_t id, int value)
{
    if (exact_control(enc->fd, id, value, enc->msm) == 0) return 0;
    return unsupported(errno) ? 0 : -1;
}

static void rate_control_hints(struct dmd_v4l2_encoder *enc)
{
    if (!enc->cfg.cbr && enc->cfg.peak_bitrate &&
        exact_control(enc->fd, V4L2_CID_MPEG_VIDEO_BITRATE_PEAK,
                      (int)enc->cfg.peak_bitrate, enc->msm) < 0 && getenv("DMD_VA_LOG"))
        fprintf(stderr, "[v4l2-enc] VBR peak-rate hint unavailable; using hardware default\n");
    /* Linux ext-ctrls-codec.rst defines H264_CPB_SIZE in kilobytes and
     * VBV_DELAY in milliseconds. VA HRD values are bits. These are hints:
     * rejecting FFmpeg's routine HRD hints would make ordinary CBR/VBR fail.
     * VBV_SIZE is MPEG1/2/4-specific, so do not send it to HEVC or VP9. */
    if (enc->cfg.vbv_size_bits && enc->cfg.profile <= DMD_ENC_H264_HIGH) {
        unsigned kb = (unsigned)(((uint64_t)enc->cfg.vbv_size_bits + 8191u) / 8192u);
        if (exact_control(enc->fd, V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE,
                          (int)kb, enc->msm) < 0 && getenv("DMD_VA_LOG"))
            fprintf(stderr, "[v4l2-enc] CPB size hint unavailable; using hardware default\n");
    } else if (enc->cfg.vbv_size_bits && getenv("DMD_VA_LOG")) {
        fprintf(stderr, "[v4l2-enc] codec has no standard CPB size control; using hardware default\n");
    }
    if (enc->cfg.vbv_initial_bits) {
        uint64_t delay = (uint64_t)enc->cfg.vbv_initial_bits * 1000u / enc->cfg.bitrate;
        if (delay <= INT_MAX &&
            exact_control(enc->fd, V4L2_CID_MPEG_VIDEO_VBV_DELAY,
                          (int)delay, enc->msm) == 0)
            return;
        if (getenv("DMD_VA_LOG"))
            fprintf(stderr, "[v4l2-enc] VBV initial-delay hint unavailable; using hardware default\n");
    }
}

static int qp_controls(struct dmd_v4l2_encoder *enc, const uint32_t *minimum,
                         const uint32_t *maximum, unsigned count)
{
    /* Bounds are constraints, unlike VBV/peak hints. Always read them back,
     * including on msm_vidc where exact_control() normally trusts S_CTRL.
     * Verify after every set has completed: related controls may clamp one
     * another. An old vendor G_CTRL stub returning zero is not confirmation
     * that a nonzero requested QP bound was actually accepted. */
    for (unsigned i = 0; i < count; ++i) {
        if (enc->cfg.max_qp && set_control(enc->fd, maximum[i], (int)enc->cfg.max_qp) < 0)
            return -1;
        if (enc->cfg.min_qp && set_control(enc->fd, minimum[i], (int)enc->cfg.min_qp) < 0)
            return -1;
    }
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t ids[] = { minimum[i], maximum[i] };
        const unsigned requested[] = { enc->cfg.min_qp, enc->cfg.max_qp };
        for (unsigned bound = 0; bound < 2; ++bound) {
            if (!requested[bound]) continue;
            struct v4l2_control got = { .id = ids[bound] };
            if (enc_ioctl(enc->fd, VIDIOC_G_CTRL, &got) < 0) return -1;
            if (got.value != (int)requested[bound]) { errno = ENOTSUP; return -1; }
        }
    }
    return 0;
}

static int configure_qp_bounds(struct dmd_v4l2_encoder *enc)
{
    if (!enc->cfg.min_qp && !enc->cfg.max_qp) return 0;
    const uint32_t minimum[] = { V4L2_CID_MPEG_VIDEO_H264_MIN_QP };
    const uint32_t maximum[] = { V4L2_CID_MPEG_VIDEO_H264_MAX_QP };
    int per_frame = 0;
    if (qp_controls(enc, minimum, maximum, 1) < 0) {
        if (!unsupported(errno)) return -1;
        /* Qualcomm's sun capability table exposes both the global standard
         * controls (HFI_PROP_{MIN,MAX}_QP_PACKED) and standard per-I/P limits:
         * qualcomm-linux/video-driver, driver/platform/sun/src/sun.c.
         * This backend disables B pictures, so confirmed I/P bounds cover
         * all encoded pictures. Do not substitute fixed I_FRAME_QP/P_FRAME_QP
         * controls, which change rate-control semantics instead of bounds. */
        const uint32_t minimum_ip[] = { V4L2_CID_MPEG_VIDEO_H264_I_FRAME_MIN_QP,
                                        V4L2_CID_MPEG_VIDEO_H264_P_FRAME_MIN_QP };
        const uint32_t maximum_ip[] = { V4L2_CID_MPEG_VIDEO_H264_I_FRAME_MAX_QP,
                                        V4L2_CID_MPEG_VIDEO_H264_P_FRAME_MAX_QP };
        if (qp_controls(enc, minimum_ip, maximum_ip, 2) < 0) return -1;
        per_frame = 1;
    }
    if (getenv("DMD_VA_LOG"))
        fprintf(stderr, "[v4l2-enc] H264 QP bounds verified (%s): min=%u max=%u "
                        "(0=unspecified)\n", per_frame ? "I/P" : "global",
                        enc->cfg.min_qp, enc->cfg.max_qp);
    return 0;
}

static int deblock_control(struct dmd_v4l2_encoder *enc, uint32_t id, int value)
{
    struct v4l2_queryctrl query = { .id = id };
    if (enc_ioctl(enc->fd, VIDIOC_QUERYCTRL, &query) < 0) {
        if (!unsupported(errno)) return -1;
        /* Older encoders default to enabled/0/0 without exposing loop-filter
         * controls. Keep that existing path, but never infer support for a
         * nondefault request from msm_vidc's sometimes-stubbed G_CTRL. */
        if (!value) return 0;
        errno = ENOTSUP;
        return -1;
    }
    if (query.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_INACTIVE)) {
        if (!value) return 0;
        errno = ENOTSUP;
        return -1;
    }
    if ((query.type != V4L2_CTRL_TYPE_INTEGER && query.type != V4L2_CTRL_TYPE_MENU) ||
        value < query.minimum || value > query.maximum ||
        (query.step > 1 && ((int64_t)value - query.minimum) % query.step)) {
        errno = ENOTSUP;
        return -1;
    }
    if (!(query.flags & V4L2_CTRL_FLAG_READ_ONLY) &&
        set_control(enc->fd, id, value) < 0) {
        if (unsupported(errno)) errno = ENOTSUP;
        return -1;
    }
    /* Read back on msm too: these affect the in-loop reconstruction, and
     * silently clamped values must not be passed off as the requested syntax. */
    struct v4l2_control got = { .id = id };
    if (enc_ioctl(enc->fd, VIDIOC_G_CTRL, &got) < 0) return -1;
    if (got.value != value) { errno = ENOTSUP; return -1; }
    return 0;
}

static int configure_deblocking(struct dmd_v4l2_encoder *enc)
{
    if (enc->cfg.profile > DMD_ENC_H264_HIGH) return 0;
    /* msm firmware can rewrite nonzero offsets to -6 at low QP even after
     * S_CTRL/G_CTRL agree with the request. Native 60-frame verification
     * preserves +2/+2 with an explicit minimum QP of 24; do not silently
     * raise the caller's QP bound or rewrite the reconstructed bitstream. */
    if (enc->msm && enc->cfg.h264_deblock_mode != 1 &&
        (enc->cfg.h264_deblock_alpha || enc->cfg.h264_deblock_beta) &&
        enc->cfg.min_qp < 24) {
        if (getenv("DMD_VA_LOG"))
            fprintf(stderr, "[v4l2-enc] msm firmware rewrites nonzero H264 "
                            "deblocking offsets at low QP; explicit min_qp>=24 "
                            "required (requested %u)\n", enc->cfg.min_qp);
        errno = ENOTSUP;
        return -1;
    }
    if (deblock_control(enc, V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE,
                         (int)enc->cfg.h264_deblock_mode) < 0 ||
        deblock_control(enc, V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA,
                         enc->cfg.h264_deblock_alpha) < 0 ||
        deblock_control(enc, V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA,
                         enc->cfg.h264_deblock_beta) < 0)
        return -1;
    if (getenv("DMD_VA_LOG"))
        fprintf(stderr, "[v4l2-enc] H264 deblocking: mode=%u alpha=%d beta=%d\n",
                enc->cfg.h264_deblock_mode, enc->cfg.h264_deblock_alpha,
                enc->cfg.h264_deblock_beta);
    return 0;
}

static int configure_fixed_qp(struct dmd_v4l2_encoder *enc)
{
    int qp = (int)enc->cfg.fixed_qp;
    if (!qp) return 0;

    int hevc = enc->cfg.profile == DMD_ENC_HEVC_MAIN;
    const uint32_t controls[] = {
        hevc ? V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP : V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP,
        hevc ? V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP : V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP,
        hevc ? V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP : V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
        hevc ? V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP : V4L2_CID_MPEG_VIDEO_H264_MIN_QP,
    };
    if (hevc) {
        for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
            struct v4l2_queryctrl query = { .id = controls[i] };
            if (enc_ioctl(enc->fd, VIDIOC_QUERYCTRL, &query) < 0) {
                if (!unsupported(errno)) return -1;
                goto hevc_vbr;
            }
            if (query.flags & V4L2_CTRL_FLAG_DISABLED) goto hevc_vbr;
        }
    }
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++)
        if (set_control(enc->fd, controls[i], qp) < 0) return -1;
    return 0;

hevc_vbr:
    if (getenv("DMD_VA_LOG"))
        fprintf(stderr, "[v4l2-enc] HEVC QP controls unavailable; using VBR fallback\n");
    return 0;
}

static int configure_controls(struct dmd_v4l2_encoder *enc)
{
    if (profile_control(enc->fd, enc->cfg.profile, enc->msm) < 0 ||
        configure_level(enc) < 0 ||
        exact_control(enc->fd, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
                      enc->cfg.cbr ? V4L2_MPEG_VIDEO_BITRATE_MODE_CBR :
                                     V4L2_MPEG_VIDEO_BITRATE_MODE_VBR, enc->msm) < 0 ||
        exact_control(enc->fd, V4L2_CID_MPEG_VIDEO_BITRATE,
                      (int)enc->cfg.bitrate, enc->msm) < 0)
        return -1;

    if (enc->cfg.profile != DMD_ENC_VP9_0) {
        if (exact_control(enc->fd, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, enc->msm) < 0) {
            if (!enc->msm || !unsupported(errno) ||
                exact_control(enc->fd, ENC_MSM_B_FRAMES, 0, 1) < 0)
                return -1;
            enc->legacy = 1;
        }
    }
    /* VP9 has no B-picture syntax; its encoder does not need the unrelated
     * AVC/HEVC B_FRAMES control in order to run a single-reference IP path. */
    if (exact_control(enc->fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE,
                      (int)enc->cfg.gop_size, enc->msm) < 0) {
        if (!enc->msm || !unsupported(errno) ||
            exact_control(enc->fd, ENC_MSM_P_FRAMES,
                          (int)enc->cfg.gop_size - 1, 1) < 0)
            return -1;
        enc->legacy = 1;
        /* Old msm_venc counts IDR periods in intra frames, not P frames. */
        if (enc->cfg.profile != DMD_ENC_VP9_0 &&
            exact_control(enc->fd, ENC_MSM_IDR_PERIOD, 1, 1) < 0)
            return -1;
    }
    if (optional_control(enc, V4L2_CID_MPEG_VIDEO_HEADER_MODE,
                          V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME) < 0 ||
        optional_control(enc, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1) < 0)
        return -1;
    if (enc->cfg.profile != DMD_ENC_VP9_0) {
        if (optional_control(enc, V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE,
                              V4L2_MPEG_VIDEO_MULTI_SLICE_MODE_SINGLE) < 0)
            return -1;
        /* nabu-r-oss v4l2-controls.h: slice delivery is base + 18.  Its
         * default is disabled; do not enable partial-frame delivery. */
        if (enc->legacy && optional_control(enc, ENC_MSM_SLICE_DELIVERY, 0) < 0)
            return -1;
    }
    if (configure_fixed_qp(enc) < 0 || configure_qp_bounds(enc) < 0 ||
        configure_deblocking(enc) < 0) return -1;
    rate_control_hints(enc);
    return 0;
}

static int configure_formats(struct dmd_v4l2_encoder *enc)
{
    if (choose_coded_format(enc->fd, enc->cfg.profile,
                            enc->cfg.width, enc->cfg.height) < 0)
        return -1;
    int available = has_format(enc->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                               V4L2_PIX_FMT_NV12);
    if (available < 0) return -1;
    if (!available) {
        available = has_format(enc->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                V4L2_PIX_FMT_NV12M);
        if (available <= 0) { if (!available) errno = ENOTSUP; return -1; }
        enc->nv12m = 1;
    }
    uint32_t fourcc = enc->nv12m ? V4L2_PIX_FMT_NV12M : V4L2_PIX_FMT_NV12;
    struct v4l2_format fmt = {0};
    fmt.type = enc->input.type;
    fmt.fmt.pix_mp.pixelformat = fourcc;
    fmt.fmt.pix_mp.width = enc->cfg.width;
    fmt.fmt.pix_mp.height = enc->cfg.height;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (enc_ioctl(enc->fd, VIDIOC_S_FMT, &fmt) < 0) return -1;
    if (fmt.fmt.pix_mp.pixelformat != fourcc ||
        fmt.fmt.pix_mp.width < enc->cfg.width ||
        fmt.fmt.pix_mp.height < enc->cfg.height) { errno = ENOTSUP; return -1; }
    /* The kernel can align storage geometry, but encoded visible geometry
     * must remain exactly the resolution requested by the VA context. */
    struct v4l2_selection selection = {0};
    selection.type = enc->input.type;
    selection.target = V4L2_SEL_TGT_CROP;
    selection.r.width = enc->cfg.width;
    selection.r.height = enc->cfg.height;
    if (enc_ioctl(enc->fd, VIDIOC_S_SELECTION, &selection) == 0) {
        if (selection.r.left || selection.r.top ||
            selection.r.width != enc->cfg.width ||
            selection.r.height != enc->cfg.height) { errno = ENOTSUP; return -1; }
    } else if (!unsupported(errno) || fmt.fmt.pix_mp.width != enc->cfg.width ||
               fmt.fmt.pix_mp.height != enc->cfg.height) {
        return -1;
    }
    struct v4l2_streamparm parm = {0};
    parm.type = enc->input.type;
    parm.parm.output.timeperframe.numerator = enc->cfg.fps_den;
    parm.parm.output.timeperframe.denominator = enc->cfg.fps_num;
    if (enc_ioctl(enc->fd, VIDIOC_S_PARM, &parm) < 0) return -1;
    uint64_t lhs = (uint64_t)parm.parm.output.timeperframe.numerator * enc->cfg.fps_num;
    uint64_t rhs = (uint64_t)parm.parm.output.timeperframe.denominator * enc->cfg.fps_den;
    /* msm_vidc stores a fixed-point frame rate; it may return a reduced
     * equivalent rational, which is accepted, but never round 29.97 to 30. */
    if (!lhs || lhs != rhs) { errno = ENOTSUP; return -1; }
    if (configure_controls(enc) < 0) return -1;

    if (queue_format(enc, &enc->input, &fmt) < 0) return -1;
    if (fmt.fmt.pix_mp.pixelformat != fourcc ||
        fmt.fmt.pix_mp.width < enc->cfg.width ||
        fmt.fmt.pix_mp.height < enc->cfg.height ||
        (enc->nv12m && enc->input.planes < 2)) { errno = EPROTO; return -1; }
    enc->stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    enc->slice_height = fmt.fmt.pix_mp.height;
    if (!enc->stride) {
        /* 4.14 msm_venc does not report bytesperline or padded scanlines.
         * Linear Venus NV12: Y stride aligns to 128, scanlines to 32.  This
         * is an ABI/layout fact; never derive scanlines from sizeimage,
         * which includes UV alignment, extradata and allocation padding. */
        enc->stride = enc->msm ? (enc->cfg.width + 127u) & ~127u
                               : fmt.fmt.pix_mp.width;
        if (enc->msm) enc->slice_height = (enc->cfg.height + 31u) & ~31u;
    }
    enc->uv_stride = enc->nv12m ? fmt.fmt.pix_mp.plane_fmt[1].bytesperline : enc->stride;
    if (!enc->uv_stride) enc->uv_stride = enc->stride;
    size_t y_size = (size_t)enc->stride * enc->slice_height;
    size_t uv_size = (size_t)enc->uv_stride * ((enc->slice_height + 1u) / 2u);
    if (enc->stride < enc->cfg.width || enc->uv_stride < enc->cfg.width ||
        y_size > enc->input.size[0] ||
        (enc->nv12m ? uv_size > enc->input.size[1]
                    : uv_size > enc->input.size[0] - y_size)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (queue_format(enc, &enc->coded, &fmt) < 0) return -1;
    if (fmt.fmt.pix_mp.pixelformat != coded_fourcc(enc->cfg.profile)) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

static int heap_open(struct dmd_v4l2_encoder *enc)
{
    if (enc->heap_fd >= 0) return 0;
    enc->heap_fd = dmd_sandbox_system_heap_dup();
    if (enc->heap_fd >= 0) return 0;
    enc->heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (enc->heap_fd >= 0) return 0;
    enc->heap_fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
    if (enc->heap_fd < 0) return -1;
    struct enc_ion_query query = {0};
    struct enc_ion_heap heaps[64] = {0};
    if (enc_ioctl(enc->heap_fd, ENC_ION_QUERY, &query) < 0) return -1;
    if (!query.count || query.count > 64) { errno = EOVERFLOW; return -1; }
    query.heaps = (uint64_t)(uintptr_t)heaps;
    if (enc_ioctl(enc->heap_fd, ENC_ION_QUERY, &query) < 0) return -1;
    if (query.count > 64) { errno = EOVERFLOW; return -1; }
    for (unsigned i = 0; i < query.count; ++i) {
        if (heaps[i].id < 32 &&
            (!heaps[i].type || !strncmp(heaps[i].name, "system", sizeof(heaps[i].name)))) {
            enc->ion_mask = 1u << heaps[i].id;
            enc->heap_ion = 1;
            return 0;
        }
    }
    errno = ENODEV;
    return -1;
}

static int plane_allocate(struct dmd_v4l2_encoder *enc, struct enc_plane *plane,
                           size_t size)
{
    if (heap_open(enc) < 0) return -1;
    if (enc->heap_ion) {
        struct enc_ion_alloc allocation = {0};
        allocation.len = size;
        allocation.heap_id_mask = enc->ion_mask;
        if (enc_ioctl(enc->heap_fd, ENC_ION_ALLOC, &allocation) < 0) return -1;
        plane->fd = (int)allocation.fd;
    } else {
        struct enc_heap_alloc allocation = {0};
        allocation.len = size;
        allocation.fd_flags = O_RDWR | O_CLOEXEC;
        if (enc_ioctl(enc->heap_fd, ENC_HEAP_ALLOC, &allocation) < 0) return -1;
        plane->fd = (int)allocation.fd;
    }
    plane->size = size;
    plane->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, plane->fd, 0);
    if (plane->map == MAP_FAILED) { plane->map = NULL; return -1; }
    return 0;
}

static int request_queue(struct dmd_v4l2_encoder *enc, struct enc_queue *queue,
                          enum v4l2_memory memory, unsigned count)
{
    struct v4l2_requestbuffers request = {0};
    request.type = queue->type;
    request.memory = memory;
    request.count = count;
    if (enc_ioctl(enc->fd, VIDIOC_REQBUFS, &request) < 0) return -1;
    queue->memory = memory;
    queue->requested = request.count != 0;
    if (!request.count || request.count > ENC_MAX_BUFFERS) {
        errno = EOVERFLOW;
        return -1;
    }
    queue->count = request.count;
    return 0;
}

static int release_request(struct dmd_v4l2_encoder *enc, struct enc_queue *queue)
{
    if (!queue->requested) return 0;
    struct v4l2_requestbuffers request = {0};
    request.type = queue->type;
    request.memory = queue->memory;
    if (enc_ioctl(enc->fd, VIDIOC_REQBUFS, &request) < 0) return -1;
    queue->requested = 0;
    queue->count = 0;
    return 0;
}

static int allocate_queues(struct dmd_v4l2_encoder *enc)
{
    /* A single memory ABI must work for both queues. An unsupported trial
     * is rolled back before another ABI is attempted; no buffers are mapped
     * until both REQBUFS operations have succeeded. */
    enum v4l2_memory candidates[] = { V4L2_MEMORY_MMAP, V4L2_MEMORY_DMABUF,
                                      V4L2_MEMORY_USERPTR };
    /* msm_vidc may accept REQBUFS(MMAP) yet have no mmap file operation.
     * Its documented import API is DMA-BUF; regular drivers prefer MMAP. */
    if (enc->msm) {
        candidates[0] = V4L2_MEMORY_DMABUF;
        candidates[1] = V4L2_MEMORY_USERPTR;
        candidates[2] = V4L2_MEMORY_MMAP;
    }
    int allocated = 0;
    for (unsigned m = 0; m < sizeof(candidates) / sizeof(candidates[0]); ++m) {
        if (candidates[m] == V4L2_MEMORY_USERPTR && !enc->msm) continue;
        int rc = request_queue(enc, &enc->input, candidates[m], 4);
        if (rc == 0) rc = request_queue(enc, &enc->coded, candidates[m], 4);
        if (rc == 0) { allocated = 1; break; }
        int saved = errno;
        if (release_request(enc, &enc->input) < 0 ||
            release_request(enc, &enc->coded) < 0) return -1;
        if (!unsupported(saved)) { errno = saved; return -1; }
    }
    if (!allocated) { errno = ENOTSUP; return -1; }
    if (enc->input.memory == V4L2_MEMORY_USERPTR) enc->legacy = 1;
    struct enc_queue *queues[] = { &enc->input, &enc->coded };
    for (unsigned q = 0; q < 2; ++q) {
        struct enc_queue *queue = queues[q];
        for (unsigned i = 0; i < queue->count; ++i) {
            if (queue->memory == V4L2_MEMORY_MMAP) {
                struct v4l2_buffer buffer = {0};
                struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
                buffer.type = queue->type;
                buffer.memory = queue->memory;
                buffer.index = i;
                buffer.m.planes = planes;
                buffer.length = queue->planes;
                if (enc_ioctl(enc->fd, VIDIOC_QUERYBUF, &buffer) < 0) return -1;
                if (buffer.length != queue->planes) { errno = EPROTO; return -1; }
                for (unsigned p = 0; p < queue->planes; ++p) {
                    struct enc_plane *plane = &queue->buffer[i].plane[p];
                    if (planes[p].length < queue->size[p] ||
                        planes[p].length > ENC_MAX_ALLOCATION) { errno = EOVERFLOW; return -1; }
                    plane->size = planes[p].length;
                    plane->map = mmap(NULL, plane->size, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, enc->fd, planes[p].m.mem_offset);
                    if (plane->map == MAP_FAILED) { plane->map = NULL; return -1; }
                }
            } else {
                for (unsigned p = 0; p < queue->planes; ++p)
                    if (plane_allocate(enc, &queue->buffer[i].plane[p], queue->size[p]) < 0)
                        return -1;
            }
        }
    }
    return 0;
}

static int sync_plane(struct enc_plane *plane, uint64_t flags)
{
    if (plane->fd < 0) return 0; /* Driver-managed MMAP cache maintenance. */
    struct enc_dma_sync sync = { .flags = flags };
    if (enc_ioctl(plane->fd, ENC_DMA_SYNC, &sync) == 0) return 0;
    /* Old uncached ION dma-bufs may not implement CPU access ioctls. */
    return unsupported(errno) ? 0 : -1;
}

static int queue_buffer(struct dmd_v4l2_encoder *enc, struct enc_queue *queue,
                         unsigned index, uint64_t stamp)
{
    struct v4l2_buffer buffer = {0};
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
    buffer.type = queue->type;
    buffer.memory = queue->memory;
    buffer.index = index;
    buffer.m.planes = planes;
    buffer.length = queue->planes;
    buffer.timestamp.tv_sec = (time_t)(stamp / 1000000u);
    buffer.timestamp.tv_usec = (suseconds_t)(stamp % 1000000u);
    for (unsigned p = 0; p < queue->planes; ++p) {
        struct enc_plane *plane = &queue->buffer[index].plane[p];
        planes[p].length = (uint32_t)plane->size;
        if (queue->memory == V4L2_MEMORY_DMABUF) planes[p].m.fd = plane->fd;
        if (queue->memory == V4L2_MEMORY_USERPTR) {
            planes[p].m.userptr = (unsigned long)plane->map;
            planes[p].reserved[0] = (uint32_t)plane->fd;
            planes[p].reserved[1] = 0;
        }
        if (queue == &enc->input && (!p || (enc->nv12m && p == 1))) {
            size_t bytes = p ? (size_t)enc->uv_stride * ((enc->slice_height + 1u) / 2u)
                             : (size_t)enc->stride * enc->slice_height;
            if (!enc->nv12m) bytes += (size_t)enc->uv_stride * ((enc->slice_height + 1u) / 2u);
            planes[p].bytesused = (uint32_t)bytes;
        }
    }
    if (enc_ioctl(enc->fd, VIDIOC_QBUF, &buffer) < 0) return -1;
    queue->buffer[index].queued = 1;
    return 0;
}

static int stream_queue(struct dmd_v4l2_encoder *enc, struct enc_queue *queue)
{
    enum v4l2_buf_type type = queue->type;
    if (enc_ioctl(enc->fd, VIDIOC_STREAMON, &type) < 0) return -1;
    queue->streaming = 1;
    return 0;
}

void dmd_v4l2_enc_close(struct dmd_v4l2_encoder *enc)
{
    if (!enc) return;
    int saved = errno;
    struct enc_queue *queues[] = { &enc->input, &enc->coded };
    /* Stop DMA before any mappings/descriptors can disappear. Closing the
     * video fd is the final barrier if a failed driver refuses STREAMOFF or
     * REQBUFS(0); memory remains alive until after close in either case. */
    if (enc->fd >= 0) {
        for (unsigned q = 0; q < 2; ++q) {
            if (queues[q]->streaming) {
                enum v4l2_buf_type type = queues[q]->type;
                enc_ioctl(enc->fd, VIDIOC_STREAMOFF, &type);
            }
            release_request(enc, queues[q]);
        }
        close(enc->fd);
    }
    for (unsigned q = 0; q < 2; ++q)
        for (unsigned i = 0; i < ENC_MAX_BUFFERS; ++i)
            for (unsigned p = 0; p < VIDEO_MAX_PLANES; ++p) {
                struct enc_plane *plane = &queues[q]->buffer[i].plane[p];
                if (plane->map) munmap(plane->map, plane->size);
                if (plane->fd >= 0) close(plane->fd);
            }
    if (enc->heap_fd >= 0) close(enc->heap_fd);
    free(enc);
    errno = saved;
}

int dmd_v4l2_enc_open(struct dmd_v4l2_encoder **out,
                      const struct dmd_enc_config *cfg)
{
    if (!out) { errno = EINVAL; return -1; }
    *out = NULL;
    if (!cfg || !coded_fourcc(cfg->profile) || !cfg->width || !cfg->height ||
        ((cfg->width | cfg->height) & 1u) || cfg->width > 16384 || cfg->height > 16384 ||
        !cfg->bitrate || cfg->bitrate > INT_MAX || cfg->peak_bitrate > INT_MAX ||
        (cfg->peak_bitrate && cfg->peak_bitrate < cfg->bitrate) ||
        cfg->min_qp > 51 || cfg->max_qp > 51 || cfg->fixed_qp > 51 ||
        (cfg->fixed_qp && (cfg->cbr || cfg->min_qp || cfg->max_qp ||
                           cfg->profile == DMD_ENC_VP9_0)) ||
        cfg->h264_deblock_mode > 2 ||
        cfg->h264_deblock_alpha < -6 || cfg->h264_deblock_alpha > 6 ||
        cfg->h264_deblock_beta < -6 || cfg->h264_deblock_beta > 6 ||
        (cfg->min_qp && cfg->max_qp && cfg->min_qp > cfg->max_qp) ||
        !cfg->fps_num || !cfg->fps_den ||
        !cfg->gop_size || cfg->gop_size > INT_MAX ||
        (uint64_t)cfg->fps_num > (uint64_t)cfg->fps_den * 1000u) {
        errno = EINVAL;
        return -1;
    }
    if ((cfg->min_qp || cfg->max_qp || cfg->h264_deblock_mode ||
         cfg->h264_deblock_alpha || cfg->h264_deblock_beta) &&
        cfg->profile > DMD_ENC_H264_HIGH) {
        errno = ENOTSUP;
        return -1;
    }
    struct dmd_v4l2_encoder *enc = calloc(1, sizeof(*enc));
    if (!enc) return -1;
    enc->fd = enc->heap_fd = -1;
    queue_init(&enc->input, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    queue_init(&enc->coded, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    enc->cfg = *cfg;
    enc->timeout_ms = ENC_DEFAULT_TIMEOUT;
    const char *timeout = getenv("DMD_VA_ENCODE_TIMEOUT_MS");
    if (timeout && *timeout) {
        char *end;
        errno = 0;
        long value = strtol(timeout, &end, 10);
        if (errno || *end || value < 100 || value > 600000) {
            errno = EINVAL;
            goto fail;
        }
        enc->timeout_ms = (int)value;
    }
    enc->fd = open(device_path(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (enc->fd < 0 || check_device(enc->fd, &enc->msm) < 0 ||
        configure_formats(enc) < 0 || allocate_queues(enc) < 0)
        goto fail;
    if (enc->msm) {
        struct v4l2_event_subscription sub = { .type = ENC_MSM_EVENT_ERROR };
        if (enc_ioctl(enc->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0 && !unsupported(errno))
            goto fail;
    }
    for (unsigned i = 0; i < enc->coded.count; ++i)
        if (queue_buffer(enc, &enc->coded, i, 0) < 0) goto fail;
    if (stream_queue(enc, &enc->coded) < 0 || stream_queue(enc, &enc->input) < 0)
        goto fail;
    *out = enc;
    return 0;
fail:
    dmd_v4l2_enc_close(enc);
    return -1;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int copy_input(struct dmd_v4l2_encoder *enc, unsigned index,
                       const uint8_t *data, size_t size,
                       unsigned stride, unsigned slice_height)
{
    if (!data || stride < enc->cfg.width || slice_height < enc->cfg.height ||
        (slice_height & 1u)) { errno = EINVAL; return -1; }
    size_t y_offset = (size_t)stride * slice_height;
    size_t uv_end = y_offset + (size_t)stride * (enc->cfg.height / 2u - 1u) + enc->cfg.width;
    if (y_offset / stride != slice_height || uv_end < y_offset || uv_end > size) {
        errno = EINVAL;
        return -1;
    }
    struct enc_plane *y = &enc->input.buffer[index].plane[0];
    struct enc_plane *uv = enc->nv12m ? &enc->input.buffer[index].plane[1] : y;
    if (sync_plane(y, ENC_DMA_WRITE) < 0) return -1;
    if (uv != y && sync_plane(uv, ENC_DMA_WRITE) < 0) {
        sync_plane(y, ENC_DMA_WRITE | ENC_DMA_END);
        return -1;
    }
    memset(y->map, 0, y->size);
    uint8_t *uv_data = enc->nv12m ? uv->map : (uint8_t *)y->map + (size_t)enc->stride * enc->slice_height;
    size_t uv_size = enc->nv12m ? uv->size : y->size - (size_t)enc->stride * enc->slice_height;
    memset(uv_data, 128, uv_size);
    for (unsigned row = 0; row < enc->cfg.height; ++row)
        memcpy((uint8_t *)y->map + (size_t)row * enc->stride,
               data + (size_t)row * stride, enc->cfg.width);
    for (unsigned row = 0; row < enc->cfg.height / 2u; ++row)
        memcpy(uv_data + (size_t)row * enc->uv_stride,
               data + y_offset + (size_t)row * stride, enc->cfg.width);
    int rc = sync_plane(y, ENC_DMA_WRITE | ENC_DMA_END);
    if (uv != y && sync_plane(uv, ENC_DMA_WRITE | ENC_DMA_END) < 0) rc = -1;
    return rc;
}

/* Inspect assembled Annex-B output, including separate sequence headers.
 * We do not fabricate codec headers. Stateful encoders generate them. */
static void packet_info(enum dmd_enc_profile profile, const uint8_t *data,
                          size_t size, int *picture, int *key)
{
    *picture = *key = 0;
    if (profile == DMD_ENC_VP9_0) {
        /* VP9 uncompressed header: marker=2, profile=0, show_existing=0,
         * then frame_type (0=key). Error-resilient/predicted frames still
         * produce one complete coded frame per capture buffer. */
        if (size) { *picture = 1; *key = (data[0] & 0xfcu) == 0x80u; }
        return;
    }
    for (size_t i = 0; i + 3 < size; ++i) {
        size_t header = 0;
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) header = i + 3;
            else if (i + 4 < size && data[i + 2] == 0 && data[i + 3] == 1) header = i + 4;
        }
        if (!header) continue;
        unsigned type = profile == DMD_ENC_HEVC_MAIN ? (data[header] >> 1) & 63u
                                                     : data[header] & 31u;
        if (profile == DMD_ENC_HEVC_MAIN) {
            if (type <= 31) *picture = 1;
            if (type >= 16 && type <= 21) *key = 1;
        } else {
            if (type >= 1 && type <= 5) *picture = 1;
            if (type == 5) *key = 1;
        }
        i = header;
    }
}

static int dequeue_input(struct dmd_v4l2_encoder *enc, uint64_t stamp, int *returned)
{
    for (;;) {
        struct v4l2_buffer buffer = {0};
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
        buffer.type = enc->input.type;
        buffer.memory = enc->input.memory;
        buffer.m.planes = planes;
        buffer.length = enc->input.planes;
        if (enc_ioctl(enc->fd, VIDIOC_DQBUF, &buffer) < 0)
            return errno == EAGAIN ? 0 : -1;
        if (buffer.index >= enc->input.count ||
            !enc->input.buffer[buffer.index].queued ||
            (buffer.flags & V4L2_BUF_FLAG_ERROR)) { errno = EIO; return -1; }
        uint64_t got = (uint64_t)buffer.timestamp.tv_sec * 1000000u +
                       (uint64_t)buffer.timestamp.tv_usec;
        if (got != stamp) { errno = EPROTO; return -1; }
        enc->input.buffer[buffer.index].queued = 0;
        *returned = 1;
    }
}

static int dequeue_coded(struct dmd_v4l2_encoder *enc, uint64_t stamp,
                          uint8_t *output, size_t capacity, size_t *length,
                          int *complete, uint32_t *flags)
{
    /* Keep the outer deadline effective even for a malfunctioning driver
     * that continuously returns empty buffers as soon as they are requeued. */
    for (unsigned processed = 0; processed < ENC_MAX_BUFFERS * 2u; ++processed) {
        struct v4l2_buffer buffer = {0};
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
        buffer.type = enc->coded.type;
        buffer.memory = enc->coded.memory;
        buffer.m.planes = planes;
        buffer.length = enc->coded.planes;
        if (enc_ioctl(enc->fd, VIDIOC_DQBUF, &buffer) < 0)
            return errno == EAGAIN ? 0 : -1;
        if (buffer.index >= enc->coded.count || buffer.length != enc->coded.planes ||
            !enc->coded.buffer[buffer.index].queued) { errno = EPROTO; return -1; }
        enc->coded.buffer[buffer.index].queued = 0;
        if (buffer.flags & (V4L2_BUF_FLAG_ERROR | V4L2_BUF_FLAG_LAST)) { errno = EIO; return -1; }
        struct enc_plane *plane = &enc->coded.buffer[buffer.index].plane[0];
        size_t offset = planes[0].data_offset, bytes = planes[0].bytesused;
        if (offset > bytes || bytes > plane->size) { errno = EOVERFLOW; return -1; }
        size_t amount = bytes - offset;
        if (*length > capacity || amount > capacity - *length) { errno = ENOSPC; return -1; }
        if (sync_plane(plane, ENC_DMA_READ) < 0) return -1;
        if (amount) memcpy(output + *length, (uint8_t *)plane->map + offset, amount);
        if (sync_plane(plane, ENC_DMA_READ | ENC_DMA_END) < 0) return -1;
        *length += amount;
        int picture, key;
        packet_info(enc->cfg.profile, output, *length, &picture, &key);
        int config = enc->legacy && (buffer.flags & ENC_MSM_CODECCONFIG);
        uint64_t got = (uint64_t)buffer.timestamp.tv_sec * 1000000u +
                       (uint64_t)buffer.timestamp.tv_usec;
        if (amount && picture && !config) {
            if (got != stamp) { errno = EPROTO; return -1; }
            *complete = !(enc->legacy && (buffer.flags & ENC_MSM_SUBFRAME));
        }
        if (key || (buffer.flags & V4L2_BUF_FLAG_KEYFRAME) ||
            (enc->legacy && (buffer.flags & ENC_MSM_IDRFRAME)))
            *flags |= DMD_ENC_PACKET_KEYFRAME;
        if (queue_buffer(enc, &enc->coded, buffer.index, 0) < 0) return -1;
    }
    return 0;
}

int dmd_v4l2_enc_frame(struct dmd_v4l2_encoder *enc,
                       const uint8_t *data, size_t size,
                       unsigned stride, unsigned slice_height,
                       int force_keyframe, uint8_t *output, size_t capacity,
                       size_t *len, uint32_t *flags)
{
    if (len) *len = 0;
    if (flags) *flags = 0;
    if (!enc || !output || !capacity || !len || !flags) { errno = EINVAL; return -1; }
    if (enc->failed) { errno = EIO; return -1; }
    unsigned index;
    for (index = 0; index < enc->input.count; ++index)
        if (!enc->input.buffer[index].queued) break;
    if (index == enc->input.count) { errno = EBUSY; goto fail; }
    if (copy_input(enc, index, data, size, stride, slice_height) < 0) goto fail;
    if (force_keyframe) {
        if (set_control(enc->fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0) < 0) {
            if (!enc->msm || !unsupported(errno) ||
                set_control(enc->fd, ENC_MSM_IFRAME, 1) < 0) goto fail;
        }
    }
    /* Real input frame intervals also matter to rate control. Start at one
     * second to avoid the sequence-header timestamp 0; retain fractional fps
     * instead of pretending every input frame is one millisecond apart. */
    __uint128_t pts = (__uint128_t)enc->sequence * enc->cfg.fps_den * 1000000u /
                       enc->cfg.fps_num + 1000000u;
    if (pts > INT64_MAX || enc->sequence == UINT64_MAX) { errno = EOVERFLOW; goto fail; }
    uint64_t stamp = (uint64_t)pts;
    ++enc->sequence;
    if (queue_buffer(enc, &enc->input, index, stamp) < 0) goto fail;
    int64_t start = monotonic_ms();
    if (start < 0) goto fail;
    int complete = 0, returned = 0;
    size_t length = 0;
    uint32_t packet_flags = 0;
    for (;;) {
        if (dequeue_input(enc, stamp, &returned) < 0 ||
            dequeue_coded(enc, stamp, output, capacity, &length, &complete, &packet_flags) < 0)
            goto fail;
        if (complete && returned) {
            if (force_keyframe && !(packet_flags & DMD_ENC_PACKET_KEYFRAME)) {
                errno = EPROTO;
                goto fail;
            }
            *len = length;
            *flags = packet_flags;
            return 0;
        }
        int64_t now = monotonic_ms();
        if (now < 0) goto fail;
        int64_t remaining = enc->timeout_ms - (now - start);
        if (remaining <= 0) { errno = ETIMEDOUT; goto fail; }
        /* POLLOUT can stay asserted merely because spare raw input buffers
         * exist. Waiting for it before a coded packet arrives would spin. */
        struct pollfd wait = { .fd = enc->fd, .events = POLLPRI |
            (!complete ? POLLIN : 0) | (complete && !returned ? POLLOUT : 0) };
        int rc = poll(&wait, 1, (int)remaining);
        if (rc < 0) { if (errno == EINTR) continue; goto fail; }
        if (!rc) { errno = ETIMEDOUT; goto fail; }
        if (wait.revents & POLLPRI) {
            struct v4l2_event event;
            while (enc_ioctl(enc->fd, VIDIOC_DQEVENT, &event) == 0)
                if (event.type == ENC_MSM_EVENT_ERROR || event.type == V4L2_EVENT_EOS) {
                    errno = EIO;
                    goto fail;
                }
            if (errno != EAGAIN && !unsupported(errno)) goto fail;
        }
        if (wait.revents & (POLLHUP | POLLNVAL)) { errno = EIO; goto fail; }
        /* Some M2M drivers assert POLLERR while one queue has no ready
         * buffer. Try DQBUF first; its errno gives the actual driver error. */
    }
fail:
    enc->failed = 1;
    return -1;
}
