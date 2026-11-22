/* SPDX-License-Identifier: Apache-2.0 */
/* Pure CAPTURE negotiation and control initialization simulator. No hardware.
 * cc -O2 -g -std=c11 -Wall -Wextra -Werror tests/test_v4l2_capture.c -o test-cap
 * Includes production setup_capture()/dmd_v4l2_open(), not a duplicate policy. */
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
#include <unistd.h>

static int mock_open(const char *, int, ...);
static int mock_close(int);
static int mock_ioctl(int, unsigned long, ...);
static void *mock_mmap(void *, size_t, int, int, int, off_t);
static int mock_munmap(void *, size_t);
#define open mock_open
#define close mock_close
#define ioctl mock_ioctl
#define mmap mock_mmap
#define munmap mock_munmap
#include "../src/v4l2_backend.c"
#undef open
#undef close
#undef ioctl
#undef mmap
#undef munmap

void dmd_video_stats_error(struct dmd_video_stats *s) { (void)s; }
void dmd_video_stats_hw(struct dmd_video_stats *s) { (void)s; }

static uint32_t actual_format, requested_formats[16];
static unsigned width, height, pitch, capacity, format_count;
static int set_error, requested_buffers, allocations, mappings, streamons, queues;
static int live_fds;
static int next_fd = 100;
static int open_mode, legacy_memory;
static unsigned ioctl_step, output_format_step, first_output_req_step;
static struct v4l2_queryctrl controls[4];
static unsigned control_count;
static struct { uint32_t id; int value; unsigned step; } sets[32];
static unsigned set_count;
static uint32_t fail_control;

static int mock_open(const char *path, int flags, ...)
{
    (void)flags;
    assert(!strcmp(path, "/dev/dma_heap/system") ||
           (open_mode && !strcmp(path, "/dev/video32")));
    ++live_fds;
    return next_fd++;
}

static int mock_close(int fd)
{
    assert(fd >= 100);
    assert(live_fds > 0);
    --live_fds;
    return 0;
}

static void *mock_mmap(void *address, size_t size, int protection, int flags,
                       int fd, off_t offset)
{
    (void)address; (void)protection; (void)flags; (void)fd; (void)offset;
    void *memory = calloc(1, size);
    assert(memory);
    ++mappings;
    return memory;
}

static int mock_munmap(void *memory, size_t size)
{
    (void)size;
    free(memory);
    assert(mappings > 0);
    --mappings;
    return 0;
}

static int mock_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    ++ioctl_step;
    if (open_mode && request == VIDIOC_ENUM_FMT) {
        struct v4l2_fmtdesc *f = arg;
        assert(f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        if (f->index) { errno = EINVAL; return -1; }
        f->pixelformat = V4L2_PIX_FMT_HEVC;
        return 0;
    }
    if (open_mode && request == VIDIOC_QUERYCTRL) {
        struct v4l2_queryctrl *q = arg;
        for (unsigned i = 0; i < control_count; ++i)
            if (q->id == controls[i].id) { *q = controls[i]; return 0; }
        errno = EINVAL;
        return -1;
    }
    if (open_mode && request == VIDIOC_S_CTRL) {
        struct v4l2_control *c = arg;
        assert(set_count < sizeof(sets) / sizeof(sets[0]));
        sets[set_count].id = c->id;
        sets[set_count].value = c->value;
        sets[set_count++].step = ioctl_step;
        if (c->id == fail_control) { errno = EIO; return -1; }
        return 0;
    }
    if (open_mode && request == VIDIOC_S_PARM) return 0;
    if (open_mode && request == VIDIOC_SUBSCRIBE_EVENT) return 0;
    if (request == VIDIOC_G_FMT || request == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg;
        if (open_mode && f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            assert(request == VIDIOC_S_FMT);
            assert(f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_HEVC);
            f->fmt.pix_mp.plane_fmt[0].sizeimage = 1024*1024;
            output_format_step = ioctl_step;
            return 0;
        }
        assert(f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        if (request == VIDIOC_S_FMT) {
            assert(format_count < sizeof(requested_formats) / sizeof(requested_formats[0]));
            requested_formats[format_count++] = f->fmt.pix_mp.pixelformat;
            if (set_error) { errno = set_error; return -1; }
        }
        f->fmt.pix_mp.pixelformat = actual_format;
        f->fmt.pix_mp.width = width;
        f->fmt.pix_mp.height = height;
        f->fmt.pix_mp.num_planes = 1;
        f->fmt.pix_mp.plane_fmt[0].bytesperline = pitch;
        f->fmt.pix_mp.plane_fmt[0].sizeimage = capacity;
        return 0;
    }
    if (request == VIDIOC_G_SELECTION) {
        errno = EINVAL;
        return -1;
    }
    if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = arg;
        if (open_mode && r->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (r->count && !first_output_req_step) first_output_req_step = ioctl_step;
            if (r->count && legacy_memory && r->memory == V4L2_MEMORY_DMABUF) {
                errno = EINVAL;
                return -1;
            }
            if (r->count) r->count = 2;
            return 0;
        }
        assert(r->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        assert(r->memory == (open_mode && legacy_memory ?
                             V4L2_MEMORY_USERPTR : V4L2_MEMORY_DMABUF));
        ++requested_buffers;
        if (r->count) r->count = 2;
        return 0;
    }
    if (request == DMA_HEAP_IOCTL_ALLOC) {
        struct dma_heap_allocation_data *a = arg;
        assert(a->len >= capacity);
        a->fd = (uint32_t)next_fd++;
        ++allocations;
        ++live_fds;
        return 0;
    }
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        assert(b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        assert(b->memory == (open_mode && legacy_memory ?
                             V4L2_MEMORY_USERPTR : V4L2_MEMORY_DMABUF));
        assert(b->length == 1);
        assert(b->m.planes[0].length >= capacity);
        ++queues;
        return 0;
    }
    if (request == VIDIOC_STREAMON || request == VIDIOC_STREAMOFF) {
        assert(*(int *)arg == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
               (open_mode && *(int *)arg == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE));
        if (request == VIDIOC_STREAMON) ++streamons;
        return 0;
    }
    assert(!"unexpected ioctl");
    errno = ENOTTY;
    return -1;
}

static void initialize(struct dmd_v4l2_dec *d, unsigned w, unsigned h,
                       unsigned stride, unsigned slice, uint32_t actual,
                       int ten_bit)
{
    assert(!mappings && !live_fds);
    assert(unsetenv("DMD_PROBE_10BIT") == 0);
    assert(unsetenv("DMD_V4L2_DISPLAY_DELAY") == 0);
    width = w; height = h; pitch = stride;
    capacity = stride * slice * 3u / 2u + 4096u;
    actual_format = actual;
    format_count = 0;
    set_error = requested_buffers = allocations = streamons = queues = 0;
    open_mode = legacy_memory = 0;
    next_fd = 100;
    ioctl_step = output_format_step = first_output_req_step = 0;
    control_count = set_count = 0;
    fail_control = 0;
    memset(d, 0, sizeof(*d));
    d->fd = -1;
    d->heap_fd = -1;
    d->out_w = (int)w;
    d->out_h = (int)h;
    d->ten_bit = ten_bit;
    d->buf_mem = V4L2_MEMORY_DMABUF;
    for (unsigned i = 0; i < DMD_V4L2_MAX_OUT; ++i) d->out[i].dbuf_fd = -1;
    for (unsigned i = 0; i < DMD_V4L2_MAX_CAP; ++i) {
        d->cap[i].dbuf_fd = -1;
        d->extra[i].dbuf_fd = -1;
    }
}

static void finish(struct dmd_v4l2_dec *d)
{
    dmd_v4l2_close(d);
    assert(!mappings && !live_fds);
}

static void test_reject_substituted_p010(void)
{
    const uint32_t alternatives[] = {
        V4L2_PIX_FMT_NV12, v4l2_fourcc('Q','P','1','0'),
        v4l2_fourcc('Q','1','2','A'), v4l2_fourcc('T','P','1','0')
    };
    for (unsigned i = 0; i < sizeof(alternatives) / sizeof(alternatives[0]); ++i) {
        struct dmd_v4l2_dec d;
        initialize(&d, 640, 360, 1280, 384, alternatives[i], 1);
        errno = 0;
        assert(setup_capture(&d) == -1);
        assert(errno == ENOTSUP);
        assert(format_count == 1 && requested_formats[0] == V4L2_PIX_FMT_P010);
        assert(!d.cap_ready && !requested_buffers && !allocations && !streamons);
        finish(&d);
    }
}

static void test_linear_layouts(void)
{
    /* Normal P010 keeps byte strides reported by V4L2, including padding and
     * non-128-aligned visible widths; NV12's 128-byte clamp must not run. */
    const unsigned cases[][4] = {
        {640, 360, 1280, 384}, {644, 360, 1536, 384},
        {1920, 1080, 3840, 1088}, {640, 360, 1408, 384}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct dmd_v4l2_dec d;
        initialize(&d, cases[i][0], cases[i][1], cases[i][2], cases[i][3],
                   V4L2_PIX_FMT_P010, 1);
        assert(setup_capture(&d) == 0);
        assert(d.w == (int)cases[i][0] && d.h == (int)cases[i][1]);
        assert(d.stride == (int)cases[i][2] && d.slice_height == (int)cases[i][3]);
        assert(d.cap_size == capacity && d.cap_ready && streamons == 1 && queues == 2);
        finish(&d);
    }
    struct dmd_v4l2_dec d;
    initialize(&d, 640, 360, 640, 384, V4L2_PIX_FMT_NV12, 0);
    assert(setup_capture(&d) == 0 && d.stride == 640 && d.slice_height == 384);
    assert(requested_formats[0] == V4L2_PIX_FMT_NV12);
    finish(&d);
}

static void test_diagnostic_overrides(void)
{
    for (unsigned mode = 1; mode <= 2; ++mode) {
        struct dmd_v4l2_dec d;
        uint32_t qformat = mode == 1 ? v4l2_fourcc('Q','P','1','0')
                                     : v4l2_fourcc('Q','1','2','A');
        initialize(&d, 640, 360, 1280, 384, qformat, 1);
        assert(setenv("DMD_PROBE_10BIT", mode == 1 ? "1" : "2", 1) == 0);
        assert(setup_capture(&d) == 0);
        assert(format_count == 1 && requested_formats[0] == qformat);
        assert(requested_formats[0] != V4L2_PIX_FMT_P010);
        finish(&d);
    }
    struct dmd_v4l2_dec d;
    initialize(&d, 640, 360, 1280, 384, V4L2_PIX_FMT_P010, 1);
    assert(setenv("DMD_PROBE_10BIT", "9", 1) == 0);
    assert(setup_capture(&d) == 0);
    assert(format_count == 8 && requested_formats[7] == V4L2_PIX_FMT_P010);
    finish(&d);
    /* Probing other formats cannot disable validation of the final request. */
    initialize(&d, 640, 360, 1280, 384, V4L2_PIX_FMT_NV12, 1);
    assert(setenv("DMD_PROBE_10BIT", "9", 1) == 0);
    errno = 0;
    assert(setup_capture(&d) == -1 && errno == ENOTSUP);
    assert(format_count == 8 && !requested_buffers && !allocations);
    finish(&d);
    assert(unsetenv("DMD_PROBE_10BIT") == 0);
}

static void test_set_error(void)
{
    struct dmd_v4l2_dec d;
    initialize(&d, 640, 360, 1280, 384, V4L2_PIX_FMT_P010, 1);
    set_error = EIO;
    errno = 0;
    assert(setup_capture(&d) == -1 && errno == EIO);
    assert(!requested_buffers && !allocations && !streamons);
    finish(&d);
}

static void add_control(uint32_t id, uint32_t type, const char *name,
                        int minimum, int maximum)
{
    assert(control_count < sizeof(controls) / sizeof(controls[0]));
    struct v4l2_queryctrl *q = &controls[control_count++];
    memset(q,0,sizeof(*q));
    q->id = id; q->type = type;
    snprintf((char *)q->name,sizeof(q->name),"%s",name);
    q->minimum = minimum; q->maximum = maximum; q->step = 1;
}

static void open_fixture(struct dmd_v4l2_dec *d, int modern)
{
    initialize(d,640,360,640,384,V4L2_PIX_FMT_NV12,0);
    open_mode = 1;
    legacy_memory = !modern;
    if (modern) {
        /* Device descriptions, independently supplied to real QUERYCTRL. The
         * test never calls or duplicates production ABI classification. */
        add_control(0x00992003,V4L2_CTRL_TYPE_INTEGER,"LOWLATENCY_MODE",0,1);
        add_control(V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE,
                    V4L2_CTRL_TYPE_BOOLEAN,"Display Delay Enable",0,1);
        add_control(V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY,
                    V4L2_CTRL_TYPE_INTEGER,"Display Delay",0,1);
    }
}

static unsigned count_set(uint32_t id)
{
    unsigned n = 0;
    for (unsigned i = 0; i < set_count; ++i) n += sets[i].id == id;
    return n;
}

static void check_dd(unsigned expected)
{
    unsigned n = 0;
    assert(first_output_req_step > output_format_step && output_format_step);
    for (unsigned i = 0; i < set_count; ++i) {
        if (sets[i].id != V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE &&
            sets[i].id != V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY) continue;
        /* This catches the real hardware regression: successful controls sent
         * after REQBUFS cannot affect queue_setup's derived OUTPUT_ORDER. */
        assert(sets[i].step > output_format_step);
        assert(sets[i].step < first_output_req_step);
        assert(sets[i].id == (n == 0 ? V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE :
                                      V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY));
        assert(sets[i].value == (n == 0 ? 1 : 0));
        ++n;
    }
    assert(n == expected);
}

static void check_modern(void)
{
    /* On modern msm_vidc these old private IDs denote metadata, not the old
     * EXTRADATA, LOWLATENCY and SECONDARY controls. Never write them. */
    assert(!count_set(0x00992011));
    assert(!count_set(0x00992038));
    assert(!count_set(0x00992016));
}

static void check_legacy(void)
{
    static const struct {uint32_t id; int value;} expected[] = {
        {0x00992003,1}, {0x00992011,2}, {0x00992011,25}, {0x00992011,31},
        {0x00992011,29}, {0x00992038,1}, {0x00992016,1}
    };
    unsigned n = 0;
    for (unsigned i = 0; i < set_count; ++i) {
        if (sets[i].id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE ||
            sets[i].id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY) continue;
        assert(n < sizeof(expected) / sizeof(expected[0]));
        assert(sets[i].id == expected[n].id && sets[i].value == expected[n].value);
        assert(sets[i].step < first_output_req_step);
        ++n;
    }
    assert(n == sizeof(expected) / sizeof(expected[0]));
}

static void test_control_order_and_abis(void)
{
    struct dmd_v4l2_dec d;
    open_fixture(&d,1);
    assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
    check_modern(); check_dd(2);
    assert(set_count == 3 && sets[0].id == 0x00992003 && sets[0].value == 1);
    assert(d.buf_mem == V4L2_MEMORY_DMABUF && d.cap_ready == 0);
    finish(&d);

    /* Legacy private QUERYCTRL is unavailable; keep its real initialization
     * sequence and USERPTR fallback, without automatically issuing DD. */
    open_fixture(&d,0);
    assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
    check_legacy(); check_dd(0);
    assert(d.buf_mem == V4L2_MEMORY_USERPTR && d.cap_ready);
    finish(&d);

    /* Name and type must both match to reinterpret base+3 as modern lowlat. */
    for (unsigned kind = 0; kind < 2; ++kind) {
        open_fixture(&d,1);
        if (kind == 0) snprintf((char *)controls[0].name,sizeof(controls[0].name),"UNKNOWN_MODE");
        else controls[0].type = V4L2_CTRL_TYPE_BOOLEAN;
        assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
        check_legacy(); check_dd(0);
        finish(&d);
    }
}

static void test_control_permissions_and_descriptions(void)
{
    struct dmd_v4l2_dec d;
    const unsigned bad_flags[] = {
        V4L2_CTRL_FLAG_DISABLED,V4L2_CTRL_FLAG_READ_ONLY,
        V4L2_CTRL_FLAG_INACTIVE,V4L2_CTRL_FLAG_GRABBED
    };
    /* Readonly/inactive lowlat still identifies the modern ABI: it must not
     * cause metadata collisions by falling back to legacy initialization. */
    for (unsigned i = 0; i < sizeof(bad_flags)/sizeof(bad_flags[0]); ++i) {
        open_fixture(&d,1); controls[0].flags = bad_flags[i];
        assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
        check_modern(); check_dd(0);
        assert(!count_set(0x00992003));
        finish(&d);
    }
    for (unsigned control = 1; control <= 2; ++control)
        for (unsigned i = 0; i < sizeof(bad_flags)/sizeof(bad_flags[0]); ++i) {
            open_fixture(&d,1); controls[control].flags = bad_flags[i];
            assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
            check_modern(); check_dd(0);
            finish(&d);
        }
    for (unsigned bad = 0; bad < 9; ++bad) {
        open_fixture(&d,1);
        switch (bad) {
        case 0: snprintf((char *)controls[1].name,sizeof(controls[1].name),"Unknown Enable"); break;
        case 1: snprintf((char *)controls[2].name,sizeof(controls[2].name),"Unknown Delay"); break;
        case 2: controls[1].type = V4L2_CTRL_TYPE_INTEGER; break;
        case 3: controls[2].type = V4L2_CTRL_TYPE_BOOLEAN; break;
        case 4: controls[1].minimum = 2; controls[1].maximum = 4; break;
        case 5: controls[2].minimum = 1; controls[2].maximum = 4; break;
        case 6: controls[2].minimum = -1; controls[2].step = 2; break; /* 0 off-step */
        case 7: controls[2].step = 0; break;
        case 8: control_count = 1; break; /* no standard DD controls */
        }
        assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
        check_modern(); check_dd(0);
        finish(&d);
    }
    open_fixture(&d,1); controls[0].maximum = 0;
    assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
    check_modern(); check_dd(0); assert(!count_set(0x00992003)); finish(&d);
}

static void test_control_environment_overrides(void)
{
    struct dmd_v4l2_dec d;
    const char *disable[] = {"0","garbage","10",""};
    for (unsigned i = 0; i < sizeof(disable)/sizeof(disable[0]); ++i) {
        open_fixture(&d,1);
        assert(setenv("DMD_V4L2_DISPLAY_DELAY",disable[i],1) == 0);
        assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
        check_modern(); check_dd(0); finish(&d);
    }
    /* Explicit1 permits a diagnostic attempt even without QUERYCTRL support;
     * initialization order still must be before the first OUTPUT REQBUFS. */
    open_fixture(&d,0);
    assert(setenv("DMD_V4L2_DISPLAY_DELAY","1",1) == 0);
    assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
    check_legacy(); check_dd(2); finish(&d);

    open_fixture(&d,0);
    assert(setenv("DMD_V4L2_DISPLAY_DELAY","0",1) == 0);
    assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
    check_legacy(); check_dd(0); finish(&d);

    open_fixture(&d,1); controls[0].flags = V4L2_CTRL_FLAG_READ_ONLY;
    controls[1].flags = V4L2_CTRL_FLAG_READ_ONLY;
    assert(setenv("DMD_V4L2_DISPLAY_DELAY","1",1) == 0);
    assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
    check_modern(); check_dd(2); assert(!count_set(0x00992003)); finish(&d);
    assert(unsetenv("DMD_V4L2_DISPLAY_DELAY") == 0);
}

static void test_control_failures(void)
{
    struct dmd_v4l2_dec d;
    open_fixture(&d,1);
    fail_control = V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE;
    assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
    check_modern(); check_dd(1); /* optional failure never sets DELAY after failed enable */
    finish(&d);

    open_fixture(&d,1);
    fail_control = V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY;
    assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == 0);
    check_modern(); check_dd(2); finish(&d);

    open_fixture(&d,0);
    fail_control = 0x00992016; /* legacy SECONDARY remains mandatory */
    assert(dmd_v4l2_open(&d,DMD_V4L2_CODEC_HEVC,640,360) == -1);
    assert(!first_output_req_step && !allocations && !mappings && !live_fds);
    assert(set_count == 7 && sets[6].id == fail_control);
    finish(&d);
}

int main(void)
{
    test_reject_substituted_p010();
    test_linear_layouts();
    test_diagnostic_overrides();
    test_set_error();
    test_control_order_and_abis();
    test_control_permissions_and_descriptions();
    test_control_environment_overrides();
    test_control_failures();
    puts("V4L2 CAPTURE P010 negotiation and initialization order tests passed");
    return 0;
}
