/* SPDX-License-Identifier: Apache-2.0 */
/* Standalone syscall simulator; does not need libva, Android or video nodes.
 * Build: cc -std=c11 -Wall -Wextra -Werror tests/test_v4l2_encoder.c -o test-enc
 * Run: ./test-enc
 * All syscall overrides are confined to this translation unit. The production
 * backend uses the real system calls and exposes no test hooks in its ABI. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
static int mock_open(const char *, int, ...);
static int mock_close(int);
static int mock_ioctl(int, unsigned long, ...);
static void *mock_mmap(void *, size_t, int, int, int, off_t);
static int mock_munmap(void *, size_t);
static int mock_poll(struct pollfd *, nfds_t, int);
static int mock_clock_gettime(clockid_t, struct timespec *);
#define open mock_open
#define close mock_close
#define ioctl mock_ioctl
#define mmap mock_mmap
#define munmap mock_munmap
#define poll mock_poll
#define clock_gettime mock_clock_gettime
#include "../src/v4l2_encoder.c"
#undef open
#undef close
#undef ioctl
#undef mmap
#undef munmap
#undef poll
#undef clock_gettime

static struct dmd_v4l2_encoder *active;
static struct v4l2_format input_fmt, coded_fmt;
static uint64_t timestamp;
static int cap_phase, input_pending, scenario, legacy, allocs, maps, closes;
static int next_dma = 200, reserved_seen;
static int advertise_sizes;
static unsigned advertised_max = 8192;
static int peak_test, peak_seen;
static int qp_mode, qp_reads, qp_sets;
static int qp_values[6];
static int deblock_test, deblock_queries, deblock_sets, deblock_reads;
static int deblock_values[3];
static int deblock_index(uint32_t id) {
    switch (id) {
    case V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE: return 0;
    case V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA: return 1;
    case V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA: return 2;
    default: return -1;
    }
}
static const uint32_t qp_ids[] = {
    V4L2_CID_MPEG_VIDEO_H264_MIN_QP,         V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
    V4L2_CID_MPEG_VIDEO_H264_I_FRAME_MIN_QP, V4L2_CID_MPEG_VIDEO_H264_I_FRAME_MAX_QP,
    V4L2_CID_MPEG_VIDEO_H264_P_FRAME_MIN_QP, V4L2_CID_MPEG_VIDEO_H264_P_FRAME_MAX_QP,
};
static int qp_index(uint32_t id) {
    for (unsigned i = 0; i < sizeof(qp_ids) / sizeof(qp_ids[0]); ++i)
        if (qp_ids[i] == id)
            return (int)i;
    return -1;
}
static int mock_open(const char *p, int flags, ...) {
    (void)flags;
    return !strcmp(p, "/dev/dma_heap/system") ? 101 : 100;
}
static int mock_close(int fd) {
    assert(fd >= 100);
    closes++;
    return 0;
}
static void *mock_mmap(void *a, size_t n, int p, int f, int fd, off_t off) {
    (void)a;
    (void)p;
    (void)f;
    (void)fd;
    (void)off;
    maps++;
    return calloc(1, n);
}
static int mock_munmap(void *p, size_t n) {
    (void)n;
    free(p);
    maps--;
    return 0;
}
static int mock_clock_gettime(clockid_t c, struct timespec *t) {
    (void)c;
    static long ns;
    ns += 1000000;
    t->tv_sec = ns / 1000000000;
    t->tv_nsec = ns % 1000000000;
    return 0;
}
static int mock_poll(struct pollfd *p, nfds_t n, int ms) {
    (void)n;
    (void)ms;
    if (scenario == 1)
        return 0;
    p->revents = POLLIN;
    return 1;
}
static int mock_ioctl(int fd, unsigned long req, ...) {
    (void)fd;
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    switch (req) {
    case VIDIOC_ENUM_FRAMESIZES: {
        if (!advertise_sizes) {
            errno = ENOTTY;
            return -1;
        }
        struct v4l2_frmsizeenum *v = arg;
        if (v->index) {
            errno = EINVAL;
            return -1;
        }
        v->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
        v->stepwise.min_width = v->stepwise.min_height = 128;
        v->stepwise.max_width = v->stepwise.max_height = advertised_max;
        return 0;
    }
    case VIDIOC_QUERYCAP: {
        struct v4l2_capability *v = arg;
        v->capabilities = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M_MPLANE;
        strcpy((char *)v->driver, legacy ? "msm_vidc" : "mock_encoder");
        return 0;
    }
    case VIDIOC_ENUM_FMT: {
        struct v4l2_fmtdesc *v = arg;
        if (v->index) {
            errno = EINVAL;
            return -1;
        }
        v->pixelformat =
            v->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_H264;
        return 0;
    }
    case VIDIOC_S_FMT: {
        struct v4l2_format *v = arg;
        v->fmt.pix_mp.num_planes = 1;
        v->fmt.pix_mp.plane_fmt[0].bytesperline =
            v->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? (legacy ? 0 : 320) : 0;
        v->fmt.pix_mp.plane_fmt[0].sizeimage =
            v->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? (legacy ? 147456 : 115200) : 4096;
        if (v->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
            input_fmt = *v;
        else
            coded_fmt = *v;
        return 0;
    }
    case VIDIOC_G_FMT: {
        struct v4l2_format *v = arg;
        *v = v->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? input_fmt : coded_fmt;
        return 0;
    }
    case VIDIOC_S_SELECTION:
    case VIDIOC_S_PARM:
    case VIDIOC_SUBSCRIBE_EVENT:
        return 0;
    case VIDIOC_S_CTRL: {
        struct v4l2_control *v = arg;
        int d = deblock_index(v->id);
        if (d >= 0) {
            deblock_sets++;
            if (d == 1 && (deblock_test == 3 || deblock_test == 8)) {
                errno = deblock_test == 3 ? EIO : EINVAL;
                return -1;
            }
            deblock_values[d] = v->value;
            return 0;
        }
        int q = qp_index(v->id);
        if (qp_mode && q >= 0) {
            qp_sets++;
            if ((qp_mode == 2 && q < 2) || qp_mode == 5 || qp_mode == 6) {
                errno = qp_mode == 6 ? EIO : EINVAL;
                return -1;
            }
            qp_values[q] = v->value;
            return 0;
        }
        if (peak_test && v->id == V4L2_CID_MPEG_VIDEO_BITRATE_PEAK) {
            assert(v->value == 1000000);
            peak_seen++;
            errno = EINVAL;
            return -1;
        }
        if (legacy &&
            (v->id == V4L2_CID_MPEG_VIDEO_B_FRAMES || v->id == V4L2_CID_MPEG_VIDEO_GOP_SIZE)) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    case VIDIOC_G_CTRL: {
        struct v4l2_control *v = arg;
        int d = deblock_index(v->id);
        if (d >= 0) {
            deblock_reads++;
            if (d == 1 && deblock_test == 7) { errno = EIO; return -1; }
            v->value = d == 1 && deblock_test == 4 ? 0 : deblock_values[d];
            return 0;
        }
        int q = qp_index(v->id);
        if (!qp_mode || q < 0) {
            errno = EINVAL;
            return -1;
        }
        qp_reads++;
        if (qp_mode == 7) {
            errno = EIO;
            return -1;
        }
        v->value = qp_mode == 4 ? 0 : qp_values[q] + (qp_mode == 3 ? 1 : 0);
        return 0;
    }
    case VIDIOC_QUERYCTRL: {
        struct v4l2_queryctrl *v = arg;
        int d = deblock_index(v->id);
        if (d >= 0) {
            deblock_queries++;
            if (!deblock_test || (d == 1 && deblock_test == 2)) {
                errno = EINVAL;
                return -1;
            }
            if (d == 1 && deblock_test == 5) { errno = EIO; return -1; }
            v->type = d ? V4L2_CTRL_TYPE_INTEGER : V4L2_CTRL_TYPE_MENU;
            v->minimum = d ? -6 : 0;
            v->maximum = d ? (deblock_test == 6 ? 1 : 6) : 2;
            v->step = 1;
            v->flags = d == 1 && deblock_test == 9 ? V4L2_CTRL_FLAG_DISABLED : 0;
            return 0;
        }
        errno = EINVAL;
        return -1;
    }
    case VIDIOC_REQBUFS: {
        struct v4l2_requestbuffers *v = arg;
        if (v->count && legacy && v->memory != V4L2_MEMORY_USERPTR) {
            errno = EINVAL;
            return -1;
        }
        allocs += v->count ? 1 : -1;
        return 0;
    }
    case VIDIOC_QUERYBUF: {
        struct v4l2_buffer *v = arg;
        v->m.planes[0].length = v->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? 115200 : 4096;
        v->m.planes[0].m.mem_offset = v->index * 4096;
        return 0;
    }
    case VIDIOC_STREAMON:
    case VIDIOC_STREAMOFF:
        return 0;
    case VIDIOC_QBUF: {
        struct v4l2_buffer *v = arg;
        if (legacy) {
            assert(v->m.planes[0].reserved[0] >= 200);
            reserved_seen++;
        }
        if (v->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            assert(!input_pending);
            input_pending = 1;
            cap_phase = 0;
            timestamp = (uint64_t)v->timestamp.tv_sec * 1000000 + v->timestamp.tv_usec;
        }
        return 0;
    }
    case VIDIOC_DQBUF: {
        struct v4l2_buffer *v = arg;
        if (!input_pending || scenario == 1) {
            errno = EAGAIN;
            return -1;
        }
        v->index = 0;
        v->timestamp.tv_sec = timestamp / 1000000;
        v->timestamp.tv_usec = timestamp % 1000000;
        if (v->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (cap_phase < 2) {
                errno = EAGAIN;
                return -1;
            }
            input_pending = 0;
            return 0;
        }
        if (cap_phase >= 2) {
            errno = EAGAIN;
            return -1;
        }
        static const uint8_t hdr[] = {0, 0, 0, 1, 0x67, 0x64, 0, 0x1f, 0, 0, 0, 1, 0x68, 0xeb};
        static const uint8_t frame[] = {0, 0, 0, 1, 0x65, 0x88, 0x84};
        const uint8_t *src = cap_phase ? frame : hdr;
        size_t count = cap_phase ? sizeof(frame) : sizeof(hdr);
        memcpy(active->coded.buffer[0].plane[0].map, src, count);
        v->m.planes[0].bytesused = count;
        if (!cap_phase)
            v->timestamp.tv_sec = v->timestamp.tv_usec = 0;
        if (scenario == 2 && cap_phase)
            v->timestamp.tv_usec++;
        if (scenario == 3)
            v->flags = V4L2_BUF_FLAG_ERROR;
        cap_phase++;
        return 0;
    }
    case ENC_HEAP_ALLOC: {
        struct enc_heap_alloc *v = arg;
        v->fd = next_dma++;
        return 0;
    }
    case ENC_DMA_SYNC:
        return 0;
    default:
        errno = ENOTTY;
        return -1;
    }
}
static void run(int mode, int is_legacy) {
    scenario = mode;
    legacy = is_legacy;
    cap_phase = input_pending = 0;
    allocs = maps = closes = reserved_seen = 0;
    struct dmd_enc_config cfg = {.profile = DMD_ENC_H264_HIGH,
                                 .width = 320,
                                 .height = 240,
                                 .bitrate = 500000,
                                 .fps_num = 30000,
                                 .fps_den = 1001,
                                 .gop_size = 30,
                                 .cbr = 1};
    if (peak_test) {
        cfg.cbr = 0;
        cfg.peak_bitrate = 1000000;
    }
    assert(dmd_v4l2_enc_open(&active, &cfg) == 0);
    assert(active);
    uint8_t *raw = malloc(115200);
    memset(raw, 128, 115200);
    uint8_t out[1024];
    size_t len;
    uint32_t flags;
    int rc = dmd_v4l2_enc_frame(active, raw, 115200, 320, 240, 1, out, mode == 4 ? 1 : sizeof(out),
                                &len, &flags);
    if (!mode) {
        assert(rc == 0);
        assert(len == 21);
        assert(flags == DMD_ENC_PACKET_KEYFRAME);
        assert(!active->input.buffer[0].queued);
        assert(!input_pending);
        assert(dmd_v4l2_enc_frame(active, raw, 115200, 320, 240, 0, out, sizeof(out), &len,
                                  &flags) == 0);
    } else {
        assert(rc == -1);
        assert(errno == (mode == 1 ? ETIMEDOUT : mode == 2 ? EPROTO : mode == 3 ? EIO : ENOSPC));
        assert(dmd_v4l2_enc_frame(active, raw, 115200, 320, 240, 0, out, sizeof(out), &len,
                                  &flags) == -1 &&
               errno == EIO);
    }
    if (legacy) {
        assert(reserved_seen);
    }
    dmd_v4l2_enc_close(active);
    active = NULL;
    assert(!maps);
    assert(!allocs);
    assert(closes >= 1);
    free(raw);
}
static void run_qp(int mode, int is_legacy) {
    qp_mode = mode;
    legacy = is_legacy;
    cap_phase = input_pending = 0;
    allocs = maps = closes = qp_sets = qp_reads = 0;
    memset(qp_values, 0, sizeof(qp_values));
    struct dmd_enc_config cfg = {.profile = DMD_ENC_H264_HIGH,
                                 .width = 320,
                                 .height = 240,
                                 .bitrate = 500000,
                                 .fps_num = 30,
                                 .fps_den = 1,
                                 .gop_size = 30,
                                 .cbr = 1,
                                 .min_qp = 1,
                                 .max_qp = 42};
    int rc = dmd_v4l2_enc_open(&active, &cfg);
    if (mode == 1 || mode == 2) {
        assert(rc == 0 && active);
        assert(qp_reads == (mode == 1 ? 2 : 4));
        assert(qp_values[mode == 1 ? 0 : 2] == 1);
        assert(qp_values[mode == 1 ? 1 : 3] == 42);
        if (mode == 2)
            assert(qp_values[4] == 1 && qp_values[5] == 42);
        dmd_v4l2_enc_close(active);
        active = NULL;
    } else {
        assert(rc == -1 && !active);
        assert(errno == (mode >= 6 ? EIO : mode == 5 ? EINVAL : ENOTSUP));
        if (mode == 6 || mode == 7)
            assert(qp_sets <= 2); /* No fallback hides I/O failure. */
    }
    assert(!maps && !allocs && closes >= 1);
    qp_mode = 0;
}

static void run_deblock(int test, unsigned mode, int alpha, int beta,
                         int want_error, int is_legacy, unsigned min_qp) {
    deblock_test = test;
    legacy = is_legacy;
    qp_mode = min_qp ? 1 : 0;
    cap_phase = input_pending = 0;
    allocs = maps = closes = 0;
    qp_sets = qp_reads = 0;
    deblock_queries = deblock_sets = deblock_reads = 0;
    memset(qp_values, 0, sizeof(qp_values));
    memset(deblock_values, 0, sizeof(deblock_values));
    struct dmd_enc_config cfg = {.profile = DMD_ENC_H264_HIGH,
        .width = 320, .height = 240, .bitrate = 500000,
        .fps_num = 30, .fps_den = 1, .gop_size = 30, .cbr = 1,
        .min_qp = min_qp,
        .h264_deblock_mode = mode, .h264_deblock_alpha = alpha,
        .h264_deblock_beta = beta};
    int rc = dmd_v4l2_enc_open(&active, &cfg);
    if (!want_error) {
        assert(rc == 0 && active);
        if (test) {
            assert(deblock_queries == 3 && deblock_sets == 3 && deblock_reads == 3);
            /* Kernel values are offset_div2: +2 must stay +2, not become +4. */
            assert(deblock_values[0] == (int)mode && deblock_values[1] == alpha &&
                   deblock_values[2] == beta);
        } else {
            assert(deblock_queries == 3 && !deblock_sets && !deblock_reads);
        }
        dmd_v4l2_enc_close(active);
        active = NULL;
    } else {
        assert(rc == -1 && !active && errno == want_error);
        if (is_legacy && mode != 1 && (alpha || beta) && min_qp < 24)
            assert(!deblock_queries && !deblock_sets && !deblock_reads);
    }
    assert(!maps && !allocs && closes >= 1);
    deblock_test = 0;
    qp_mode = 0;
}

int main(void) {
    run(0, 0);
    run(0, 1);
    run(1, 0);
    run(2, 0);
    run(3, 0);
    run(4, 0);
    peak_test = 1;
    run(0, 0); /* An unsupported VBR peak hint must not disable encoding. */
    assert(peak_seen == 1);
    peak_test = 0;
    run_qp(1, 0); /* Standard global limits. */
    run_qp(1, 1); /* msm_vidc limits must also be read back. */
    run_qp(2, 0); /* Global control absent, verified standard per-I/P limits. */
    run_qp(3, 0); /* A silently clamped readback is not success. */
    run_qp(4, 1); /* Old vendor G_CTRL stubs returning zero are not confirmation. */
    run_qp(5, 0); /* Unsupported requested constraints fail. */
    run_qp(6, 0); /* S_CTRL I/O error remains fatal. */
    run_qp(7, 0); /* G_CTRL I/O error remains fatal. */
    run_deblock(0, 0, 0, 0, 0, 0, 0); /* Preserve absent-control zero defaults. */
    run_deblock(1, 0, 0, 0, 0, 0, 0); /* Verify zeros if controls are advertised. */
    run_deblock(1, 0, 2, 2, 0, 0, 0); /* Non-msm keeps the control path. */
    run_deblock(1, 0, 2, 2, 0, 0, 1); /* Non-msm permits an explicit low bound. */
    run_deblock(1, 0, 2, 2, ENOTSUP, 1, 0); /* Unspecified msm QP is unsafe. */
    run_deblock(1, 0, 2, 2, ENOTSUP, 1, 1); /* Gst's default low minimum. */
    run_deblock(1, 0, 2, 0, ENOTSUP, 1, 23); /* Alpha alone needs the bound. */
    run_deblock(1, 0, 0, -2, ENOTSUP, 1, 23); /* Beta alone needs it too. */
    run_deblock(1, 0, 2, 2, 0, 1, 24); /* Verified msm +2/+2 preset. */
    run_deblock(1, 0, 2, 2, 0, 1, 25); /* A stricter caller bound is valid. */
    run_deblock(1, 2, -6, 6, ENOTSUP, 1, 0); /* Applies to mode 2 as well. */
    run_deblock(1, 2, -6, 6, 0, 0, 0); /* Exact boundary offsets and mode mapping. */
    run_deblock(1, 1, 0, 0, 0, 0, 0); /* Filter disabled. */
    run_deblock(1, 1, 0, 0, 0, 1, 0); /* Disabled msm needs no QP restriction. */
    run_deblock(1, 0, 0, 0, 0, 1, 0); /* Zero msm offsets keep old behavior. */
    run_deblock(0, 1, 0, 0, ENOTSUP, 0, 0); /* Missing required mode. */
    run_deblock(0, 0, 0, 2, ENOTSUP, 0, 0); /* Missing required beta. */
    run_deblock(2, 0, 2, 2, ENOTSUP, 0, 0); /* Missing required alpha. */
    run_deblock(3, 0, 2, 2, EIO, 0, 0); /* S_CTRL I/O error propagates. */
    run_deblock(4, 0, 2, 2, ENOTSUP, 1, 24); /* Stub/clamped readback is not success. */
    run_deblock(5, 0, 2, 2, EIO, 0, 0); /* QUERYCTRL I/O error propagates. */
    run_deblock(6, 0, 2, 2, ENOTSUP, 0, 0); /* Driver range excludes request. */
    run_deblock(7, 0, 2, 2, EIO, 0, 0); /* G_CTRL I/O error propagates. */
    run_deblock(8, 0, 2, 2, ENOTSUP, 0, 0); /* Advertised but S_CTRL rejected. */
    run_deblock(9, 0, 2, 2, ENOTSUP, 0, 0); /* Disabled control is unavailable. */
    advertise_sizes = 1;
    unsigned minw, minh, maxw, maxh;
    assert(dmd_v4l2_enc_get_limits(DMD_ENC_H264_MAIN, &minw, &minh, &maxw, &maxh) == 0);
    assert(minw == 128 && minh == 128 && maxw == 8192 && maxh == 8192);
    advertised_max = 640;
    assert(dmd_v4l2_enc_probe() ==
           (DMD_ENC_CAP(DMD_ENC_H264_BASELINE) | DMD_ENC_CAP(DMD_ENC_H264_MAIN) |
            DMD_ENC_CAP(DMD_ENC_H264_HIGH)));
    advertised_max = 8192;
    /* Generic vendor ranges for an unenumerated codec must be ignored. */
    assert(dmd_v4l2_enc_get_limits(DMD_ENC_VP9_0, &minw, &minh, &maxw, &maxh) == -1);
    assert(errno == ENOTSUP);
    struct dmd_enc_config small = {.profile = DMD_ENC_H264_MAIN,
                                   .width = 128,
                                   .height = 96,
                                   .bitrate = 500000,
                                   .fps_num = 30,
                                   .fps_den = 1,
                                   .gop_size = 30};
    assert(dmd_v4l2_enc_open(&active, &small) == -1 && errno == ENOTSUP && !active);
    /* Failure before opening the device must not close descriptor zero. */
    small.height = 128;
    assert(setenv("DMD_VA_ENCODE_TIMEOUT_MS", "invalid", 1) == 0);
    closes = 0;
    assert(dmd_v4l2_enc_open(&active, &small) == -1 && errno == EINVAL && !active);
    assert(closes == 0);
    unsetenv("DMD_VA_ENCODE_TIMEOUT_MS");
    small.peak_bitrate = small.bitrate - 1;
    assert(dmd_v4l2_enc_open(&active, &small) == -1 && errno == EINVAL && !active);
    small.peak_bitrate = UINT_MAX;
    assert(dmd_v4l2_enc_open(&active, &small) == -1 && errno == EINVAL && !active);
    small.peak_bitrate = 0;
    small.min_qp = 43;
    small.max_qp = 42;
    assert(dmd_v4l2_enc_open(&active, &small) == -1 && errno == EINVAL && !active);
    small.min_qp = 0;
    small.max_qp = 52;
    assert(dmd_v4l2_enc_open(&active, &small) == -1 && errno == EINVAL && !active);
    small.profile = DMD_ENC_HEVC_MAIN;
    small.max_qp = 42;
    assert(dmd_v4l2_enc_open(&active, &small) == -1 && errno == ENOTSUP && !active);
    small.max_qp = 0;
    small.h264_deblock_alpha = 2;
    assert(dmd_v4l2_enc_open(&active, &small) == -1 && errno == ENOTSUP && !active);
    small.profile = DMD_ENC_H264_HIGH;
    small.h264_deblock_alpha = 7;
    assert(dmd_v4l2_enc_open(&active, &small) == -1 && errno == EINVAL && !active);
    small.h264_deblock_alpha = -7;
    assert(dmd_v4l2_enc_open(&active, &small) == -1 && errno == EINVAL && !active);
    small.h264_deblock_alpha = 0;
    small.h264_deblock_beta = 7;
    assert(dmd_v4l2_enc_open(&active, &small) == -1 && errno == EINVAL && !active);
    small.h264_deblock_beta = 0;
    small.h264_deblock_mode = 3;
    assert(dmd_v4l2_enc_open(&active, &small) == -1 && errno == EINVAL && !active);
    puts("encoder backend mock tests passed: MMAP, vendor USERPTR, split headers, delayed input, "
         "timestamps, timeout, error, ENOSPC, cleanup, device limits, VBR hints, verified QP "
         "bounds, exact H264 deblocking controls, msm low-QP offset guard");
    return 0;
}
