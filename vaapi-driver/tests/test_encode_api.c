/* VA-API encode contract tests.  Link the real driver with the fake encoder
 * below, instead of v4l2_encoder.c.  No video device or Android daemon is used.
 * These check upload geometry, VA parameter translation, coded-buffer ABI and
 * failure recovery; they deliberately do not claim to validate hardware output.
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <drm/drm.h>
#include <drm_fourcc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver.h"

#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_enc_vp9.h>

#include "v4l2_encoder.h"
#include "v4l2_backend.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)
#define OK(expr) CHECK((expr) == VA_STATUS_SUCCESS)
#define BAD(expr) CHECK((expr) != VA_STATUS_SUCCESS)

extern VAStatus __vaDriverInit_1_0(VADriverContextP ctx);

struct fixture {
    struct VADriverContext va;
    struct VADriverVTable vt;
    VAConfigID config;
    VAContextID context;
    VASurfaceID surface;
    unsigned width, height;
};

static unsigned fake_open_calls, fake_close_calls, fake_frame_calls;
static unsigned fake_live;
static uint32_t fake_caps = (1u << (DMD_ENC_VP9_0 + 1)) - 1;
static int fake_frame_error;
static int fake_open_error;
static struct dmd_enc_config fake_config;
static uint8_t fake_pixels[1024 * 1024];
static size_t fake_pixels_size;
static unsigned fake_stride, fake_slice_height;
static int fake_force_key;
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_cond = PTHREAD_COND_INITIALIZER;
static int fake_block_frame, fake_frame_entered, fake_release_frame;
static int fake_sync_mode, fake_block_sync;
static unsigned fake_sync_starts, fake_sync_ends;
static int fake_size_mode;
static unsigned fake_size_stat_calls, fake_size_end_calls, fake_size_set_calls;
static int fake_export_fd = -1, fake_export_error;
static unsigned fake_export_flags;
static int fake_capture_count;
static unsigned fake_capture_bytes, fake_capture_flags;
static struct fixture *fake_export_destroy;
static const uint8_t h264_packet[] = { 0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21 };
static const uint8_t hevc_packet[] = { 0, 0, 0, 1, 0x26, 1, 0x91, 0x80 };
static const uint8_t vp9_packet[] = { 0x82, 0x49, 0x83, 0x42, 0x00, 0x00 };

off_t lseek(int fd, off_t offset, int whence)
{
    if (fake_size_mode) {
        if (whence == SEEK_END) {
            fake_size_end_calls++;
            if (fake_size_mode == 2 || fake_size_mode == 3) {
                errno = ESPIPE;
                return -1;
            }
            if (fake_size_mode == 4) return 64; /* a valid but undersized object */
        }
        if (whence == SEEK_SET) fake_size_set_calls++;
        if (fake_size_mode == 1 && whence == SEEK_CUR) {
            errno = EINVAL; /* legal DMA-BUF ABI: END/SET only */
            return -1;
        }
    }
    return (off_t)syscall(SYS_lseek, fd, offset, whence);
}

int fstat(int fd, struct stat *st)
{
    int result = (int)syscall(SYS_fstat, fd, st);
    if (fake_size_mode) {
        fake_size_stat_calls++;
        if (result == 0 && (fake_size_mode == 1 || fake_size_mode == 3))
            st->st_size = 0;
    }
    return result;
}

/* A memfd supplies deterministic shared storage. Only explicitly enabled
 * DMA_BUF_IOCTL_SYNC calls are mocked; the strict-mode case reaches the real
 * syscall and must report ENOTTY instead of treating a regular file as DMA. */
int ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (request == VIDIOC_DQBUF && fake_capture_count) {
        struct v4l2_buffer *buffer = arg;
        CHECK(buffer && buffer->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        buffer->index = 0;
        buffer->flags = fake_capture_flags;
        buffer->m.planes[0].bytesused = fake_capture_bytes;
        buffer->timestamp.tv_sec = 1;
        buffer->timestamp.tv_usec = 0;
        fake_capture_count--;
        return 0;
    }
    if (request == DRM_IOCTL_PRIME_HANDLE_TO_FD && fake_export_fd >= 0) {
        struct drm_prime_handle *prime = arg;
        CHECK(prime != NULL);
        fake_export_flags = prime->flags;
        if (fake_export_error) {
            errno = fake_export_error;
            return -1;
        }
        prime->fd = fcntl(fake_export_fd, F_DUPFD_CLOEXEC, 0);
        CHECK(prime->fd >= 0);
        if (fake_export_destroy) {
            /* Export must have released the lock for this ioctl. Reentrant
             * destruction deterministically exercises its post-ioctl lookup. */
            struct fixture *f = fake_export_destroy;
            fake_export_destroy = NULL;
            OK(f->vt.vaDestroySurfaces(&f->va, &f->surface, 1));
            f->surface = VA_INVALID_SURFACE;
        }
        return 0;
    }
    if (request == DMA_BUF_IOCTL_SYNC && fake_sync_mode) {
        struct dma_buf_sync *sync = arg;
        CHECK(sync && (fake_sync_mode == 4 ?
              (sync->flags & DMA_BUF_SYNC_RW) == DMA_BUF_SYNC_WRITE :
              (sync->flags & DMA_BUF_SYNC_READ) != 0));
        pthread_mutex_lock(&fake_lock);
        if (sync->flags & DMA_BUF_SYNC_END) fake_sync_ends++;
        else fake_sync_starts++;
        if (fake_block_sync && !(sync->flags & DMA_BUF_SYNC_END)) {
            fake_frame_entered = 1;
            pthread_cond_broadcast(&fake_cond);
            while (!fake_release_frame) pthread_cond_wait(&fake_cond, &fake_lock);
        }
        pthread_mutex_unlock(&fake_lock);
        if (fake_sync_mode == 2 ||
            (fake_sync_mode == 3 && (sync->flags & DMA_BUF_SYNC_END))) {
            errno = EIO;
            return -1;
        }
        return 0;
    }
    return (int)syscall(SYS_ioctl, fd, request, arg);
}

struct dmd_v4l2_encoder {
    struct dmd_enc_config config;
};

uint32_t dmd_v4l2_enc_probe(void)
{
    return fake_caps;
}

int dmd_v4l2_enc_get_limits(enum dmd_enc_profile profile, unsigned *min_width,
                            unsigned *min_height, unsigned *max_width,
                            unsigned *max_height)
{
    if (!min_width || !min_height || !max_width || !max_height ||
        (unsigned)profile > DMD_ENC_VP9_0 || !(fake_caps & DMD_ENC_CAP(profile))) {
        errno = EINVAL;
        return -1;
    }
    *min_width = *min_height = 96;
    *max_width = 8192;
    *max_height = 4320;
    return 0;
}

int dmd_v4l2_enc_open(struct dmd_v4l2_encoder **out, const struct dmd_enc_config *cfg)
{
    CHECK(out && cfg);
    *out = NULL;
    if (fake_open_error) {
        errno = fake_open_error;
        return -1;
    }
    struct dmd_v4l2_encoder *enc = calloc(1, sizeof(*enc));
    CHECK(enc != NULL);
    enc->config = fake_config = *cfg;
    *out = enc;
    fake_open_calls++;
    fake_live++;
    return 0;
}

int dmd_v4l2_enc_frame(struct dmd_v4l2_encoder *enc, const uint8_t *data,
                      size_t size, unsigned stride, unsigned slice_height,
                      int force_keyframe, uint8_t *output, size_t capacity,
                      size_t *len, uint32_t *flags)
{
    CHECK(enc && data && output && len && flags);
    pthread_mutex_lock(&fake_lock);
    if (fake_block_frame) {
        fake_frame_entered = 1;
        pthread_cond_broadcast(&fake_cond);
        while (!fake_release_frame)
            pthread_cond_wait(&fake_cond, &fake_lock);
    }
    pthread_mutex_unlock(&fake_lock);
    CHECK(size <= sizeof(fake_pixels));
    memcpy(fake_pixels, data, size);
    fake_pixels_size = size;
    fake_stride = stride;
    fake_slice_height = slice_height;
    fake_force_key = force_keyframe;
    fake_frame_calls++;
    *len = 0;
    *flags = 0;
    if (fake_frame_error) {
        errno = fake_frame_error;
        return -1;
    }
    const uint8_t *packet = h264_packet;
    size_t packet_size = sizeof(h264_packet);
    if (enc->config.profile == DMD_ENC_HEVC_MAIN) {
        packet = hevc_packet;
        packet_size = sizeof(hevc_packet);
    } else if (enc->config.profile == DMD_ENC_VP9_0) {
        packet = vp9_packet;
        packet_size = sizeof(vp9_packet);
    }
    if (packet_size > capacity) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(output, packet, packet_size);
    *len = packet_size;
    *flags = force_keyframe ? DMD_ENC_PACKET_KEYFRAME : 0;
    return 0;
}

void dmd_v4l2_enc_close(struct dmd_v4l2_encoder *enc)
{
    if (!enc)
        return;
    CHECK(fake_live > 0);
    fake_live--;
    fake_close_calls++;
    free(enc);
}

static void init_driver(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->va.vtable = &f->vt;
    f->config = f->context = f->surface = VA_INVALID_ID;
    OK(__vaDriverInit_1_0(&f->va));
    CHECK(f->va.pDriverData != NULL);
    CHECK(f->va.max_profiles > 0);
    CHECK(f->vt.vaBeginPicture && f->vt.vaRenderPicture && f->vt.vaEndPicture);
    CHECK(f->vt.vaPutImage && f->vt.vaSyncBuffer && f->vt.vaMapBuffer);
}

static void finish_driver(struct fixture *f)
{
    OK(f->vt.vaTerminate(&f->va));
    CHECK(f->va.pDriverData == NULL);
    CHECK(fake_live == 0);
    /* libva can terminate again after a partially failed initialization. */
    OK(f->vt.vaTerminate(&f->va));
}

static void create_encode(struct fixture *f, VAProfile profile,
                          unsigned width, unsigned height, unsigned rc)
{
    f->width = width;
    f->height = height;
    VAConfigAttrib attrs[] = {
        { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420 },
        { VAConfigAttribRateControl, rc },
    };
    OK(f->vt.vaCreateConfig(&f->va, profile, VAEntrypointEncSlice,
                           attrs, 2, &f->config));
    VAProfile actual_profile;
    VAEntrypoint actual_entrypoint;
    VAConfigAttrib actual[DMD_MAX_CONFIG_ATTRIBUTES];
    int actual_count = 0;
    OK(f->vt.vaQueryConfigAttributes(&f->va, f->config, &actual_profile,
                                     &actual_entrypoint, actual, &actual_count));
    CHECK(actual_profile == profile && actual_entrypoint == VAEntrypointEncSlice);
    int have_rt = 0, have_rc = 0;
    for (int i = 0; i < actual_count; i++) {
        if (actual[i].type == VAConfigAttribRTFormat) {
            CHECK(actual[i].value == VA_RT_FORMAT_YUV420);
            have_rt = 1;
        }
        if (actual[i].type == VAConfigAttribRateControl) {
            CHECK(actual[i].value == rc);
            have_rc = 1;
        }
    }
    CHECK(have_rt && have_rc);
    OK(f->vt.vaCreateSurfaces2(&f->va, VA_RT_FORMAT_YUV420, width, height,
                              &f->surface, 1, NULL, 0));
    OK(f->vt.vaCreateContext(&f->va, f->config, (int)width, (int)height,
                            VA_PROGRESSIVE, &f->surface, 1, &f->context));
}

static uint8_t y_sample(unsigned x, unsigned y)
{
    return (uint8_t)(16 + (x * 3 + y * 7) % 220);
}

static uint8_t uv_sample(unsigned x, unsigned y)
{
    return (uint8_t)(64 + (x * 11 + y * 13) % 128);
}

static VAImage upload_pattern(struct fixture *f)
{
    VAImageFormat fmt = { .fourcc = VA_FOURCC_NV12, .byte_order = VA_LSB_FIRST,
                          .bits_per_pixel = 12 };
    VAImage image;
    OK(f->vt.vaCreateImage(&f->va, &fmt, (int)f->width, (int)f->height, &image));
    uint8_t *p = NULL;
    OK(f->vt.vaMapBuffer(&f->va, image.buf, (void **)&p));
    CHECK(p && image.num_planes == 2);
    /* Padding has an explicit poison value, so flattening the source image
     * instead of copying its visible rows cannot pass the pixel checks. */
    memset(p, 0xa5, image.data_size);
    for (unsigned y = 0; y < f->height; y++)
        for (unsigned x = 0; x < f->width; x++)
            p[image.offsets[0] + y * image.pitches[0] + x] = y_sample(x, y);
    for (unsigned y = 0; y < f->height / 2; y++)
        for (unsigned x = 0; x < f->width; x++)
            p[image.offsets[1] + y * image.pitches[1] + x] = uv_sample(x, y);
    OK(f->vt.vaUnmapBuffer(&f->va, image.buf));
    OK(f->vt.vaPutImage(&f->va, f->surface, image.image_id, 0, 0,
                       f->width, f->height, 0, 0, f->width, f->height));
    return image;
}

static VABufferID make_buffer(struct fixture *f, VABufferType type,
                              size_t size, const void *data)
{
    VABufferID id = VA_INVALID_ID;
    CHECK(size <= UINT32_MAX);
    OK(f->vt.vaCreateBuffer(&f->va, f->context, type, (unsigned)size, 1,
                           (void *)data, &id));
    CHECK(id != VA_INVALID_ID);
    return id;
}

static void render_data(struct fixture *f, VABufferType type,
                        const void *data, size_t size)
{
    VABufferID id = make_buffer(f, type, size, data);
    OK(f->vt.vaRenderPicture(&f->va, f->context, &id, 1));
    /* Parameters are copied by RenderPicture, as VA-API requires. */
    OK(f->vt.vaDestroyBuffer(&f->va, id));
}

static void render_fps(struct fixture *f, unsigned num, unsigned den)
{
    union {
        VAEncMiscParameterBuffer alignment;
        uint8_t bytes[sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterFrameRate)];
    } data;
    memset(&data, 0, sizeof(data));
    VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer *)data.bytes;
    misc->type = VAEncMiscParameterTypeFrameRate;
    VAEncMiscParameterFrameRate rate = { .framerate = (den << 16) | num };
    memcpy(misc->data, &rate, sizeof(rate));
    render_data(f, VAEncMiscParameterBufferType, data.bytes, sizeof(data.bytes));
}

static void render_h264(struct fixture *f, VABufferID coded, int key,
                        int ten_bit, int b_frame)
{
    VAEncSequenceParameterBufferH264 seq = { 0 };
    seq.level_idc = 40;
    seq.intra_period = seq.intra_idr_period = 60;
    seq.ip_period = 1;
    seq.bits_per_second = 2000000;
    seq.max_num_ref_frames = 1;
    seq.picture_width_in_mbs = (uint16_t)((f->width + 15) / 16);
    seq.picture_height_in_mbs = (uint16_t)((f->height + 15) / 16);
    seq.seq_fields.bits.chroma_format_idc = 1;
    seq.seq_fields.bits.frame_mbs_only_flag = 1;
    seq.bit_depth_luma_minus8 = seq.bit_depth_chroma_minus8 = ten_bit ? 2 : 0;
    VAEncPictureParameterBufferH264 pic = { 0 };
    pic.CurrPic.picture_id = f->surface;
    pic.coded_buf = coded;
    pic.pic_init_qp = 26;
    pic.pic_fields.bits.idr_pic_flag = key;
    pic.pic_fields.bits.reference_pic_flag = 1;
    for (unsigned i = 0; i < 16; i++) {
        pic.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    VAEncSliceParameterBufferH264 slice = { 0 };
    slice.num_macroblocks = seq.picture_width_in_mbs * seq.picture_height_in_mbs;
    slice.macroblock_info = VA_INVALID_ID;
    slice.slice_type = b_frame ? 1 : (key ? 2 : 0);
    if (!key) {
        pic.ReferenceFrames[0].picture_id = f->surface;
        pic.ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        slice.RefPicList0[0] = pic.ReferenceFrames[0];
    }
    render_data(f, VAEncSequenceParameterBufferType, &seq, sizeof(seq));
    render_data(f, VAEncPictureParameterBufferType, &pic, sizeof(pic));
    render_data(f, VAEncSliceParameterBufferType, &slice, sizeof(slice));
}

static void render_hevc(struct fixture *f, VABufferID coded, int key)
{
    VAEncSequenceParameterBufferHEVC seq = { 0 };
    seq.general_profile_idc = 1;
    seq.general_level_idc = 120;
    seq.intra_period = seq.intra_idr_period = 60;
    seq.ip_period = 1;
    seq.bits_per_second = 2000000;
    seq.pic_width_in_luma_samples = (uint16_t)f->width;
    seq.pic_height_in_luma_samples = (uint16_t)f->height;
    seq.seq_fields.bits.chroma_format_idc = 1;
    seq.log2_diff_max_min_luma_coding_block_size = 3;
    VAEncPictureParameterBufferHEVC pic = { 0 };
    pic.decoded_curr_pic.picture_id = f->surface;
    pic.coded_buf = coded;
    pic.pic_init_qp = 26;
    pic.pic_fields.bits.idr_pic_flag = key;
    pic.pic_fields.bits.coding_type = key ? 1 : 2;
    for (unsigned i = 0; i < 15; i++) {
        pic.reference_frames[i].picture_id = VA_INVALID_SURFACE;
        pic.reference_frames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    VAEncSliceParameterBufferHEVC slice = { 0 };
    slice.num_ctu_in_slice = ((f->width + 63) / 64) * ((f->height + 63) / 64);
    slice.slice_type = key ? 2 : 1;
    slice.slice_fields.bits.last_slice_of_pic_flag = 1;
    if (!key) {
        pic.reference_frames[0].picture_id = f->surface;
        pic.reference_frames[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
        slice.ref_pic_list0[0] = pic.reference_frames[0];
    }
    render_data(f, VAEncSequenceParameterBufferType, &seq, sizeof(seq));
    render_data(f, VAEncPictureParameterBufferType, &pic, sizeof(pic));
    render_data(f, VAEncSliceParameterBufferType, &slice, sizeof(slice));
}

static void render_vp9(struct fixture *f, VABufferID coded, int key)
{
    VAEncSequenceParameterBufferVP9 seq = { 0 };
    seq.max_frame_width = f->width;
    seq.max_frame_height = f->height;
    seq.bits_per_second = 2000000;
    seq.intra_period = 60;
    VAEncPictureParameterBufferVP9 pic = { 0 };
    pic.frame_width_src = pic.frame_width_dst = f->width;
    pic.frame_height_src = pic.frame_height_dst = f->height;
    pic.coded_buf = coded;
    pic.reconstructed_frame = f->surface;
    pic.ref_flags.bits.force_kf = key;
    pic.pic_flags.bits.frame_type = !key;
    pic.pic_flags.bits.show_frame = 1;
    pic.luma_ac_qindex = 100;
    for (unsigned i = 0; i < 8; i++)
        pic.reference_frames[i] = VA_INVALID_SURFACE;
    if (!key) {
        pic.reference_frames[0] = f->surface;
        pic.ref_flags.bits.ref_frame_ctrl_l0 = 1;
        pic.ref_flags.bits.ref_last_sign_bias = 1;
    }
    render_data(f, VAEncSequenceParameterBufferType, &seq, sizeof(seq));
    render_data(f, VAEncPictureParameterBufferType, &pic, sizeof(pic));
}

static void render_frame(struct fixture *f, VAProfile profile, VABufferID coded,
                         int key)
{
    if (profile == VAProfileHEVCMain)
        render_hevc(f, coded, key);
    else if (profile == VAProfileVP9Profile0)
        render_vp9(f, coded, key);
    else
        render_h264(f, coded, key, 0, 0);
    render_fps(f, 30000, 1001);
}

static void check_pixels(unsigned width, unsigned height)
{
    CHECK(fake_stride >= width && fake_slice_height >= height);
    CHECK(fake_pixels_size >= (size_t)fake_stride * fake_slice_height * 3 / 2);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++)
            CHECK(fake_pixels[y * fake_stride + x] == y_sample(x, y));
    const uint8_t *uv = fake_pixels + (size_t)fake_stride * fake_slice_height;
    for (unsigned y = 0; y < height / 2; y++)
        for (unsigned x = 0; x < width; x++)
            CHECK(uv[y * fake_stride + x] == uv_sample(x, y));
}

static void check_packet(struct fixture *f, VABufferID coded, VAProfile profile)
{
    OK(f->vt.vaSyncBuffer(&f->va, coded, 0));
    VACodedBufferSegment *segment = NULL;
    OK(f->vt.vaMapBuffer(&f->va, coded, (void **)&segment));
    CHECK(segment && segment->buf && segment->next == NULL);
    CHECK(segment->bit_offset == 0);
    CHECK(!(segment->status & (VA_CODED_BUF_STATUS_BAD_BITSTREAM |
                              VA_CODED_BUF_STATUS_FRAME_SIZE_OVERFLOW)));
    const uint8_t *expected = h264_packet;
    size_t size = sizeof(h264_packet);
    if (profile == VAProfileHEVCMain) {
        expected = hevc_packet;
        size = sizeof(hevc_packet);
    } else if (profile == VAProfileVP9Profile0) {
        expected = vp9_packet;
        size = sizeof(vp9_packet);
    }
    CHECK(segment->size == size);
    CHECK(memcmp(segment->buf, expected, size) == 0);
    OK(f->vt.vaUnmapBuffer(&f->va, coded));
}

static uint64_t stats_number(const char *json, const char *field)
{
    char key[128];
    snprintf(key, sizeof(key), "\"%s\":", field);
    const char *p = strstr(json, key);
    CHECK(p);
    return strtoull(p + strlen(key), NULL, 10);
}

static int stats_snapshot(VAContextID context, char json[4096])
{
    DIR *directory = opendir("/proc/self/fd");
    CHECK(directory);
    struct dirent *entry;
    int found = -1;
    while ((entry = readdir(directory))) {
        char path[128], target[256];
        snprintf(path, sizeof(path), "/proc/self/fd/%.40s", entry->d_name);
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n < 0) continue;
        target[n] = 0;
        if (!strstr(target, "/memfd:" DMD_VIDEO_STATS_NAME)) continue;
        int fd = atoi(entry->d_name);
        unsigned char header[32];
        CHECK(pread(fd, header, 32, 0) == 32);
        CHECK(!memcmp(header, "DMDVID1", 8));
        uint64_t before, after;
        uint32_t length;
        memcpy(&before, header + 16, 8);
        memcpy(&length, header + 24, 4);
        CHECK(!(before & 1) && length < 4096 - 32);
        CHECK(pread(fd, json, length, 32) == length);
        CHECK(pread(fd, &after, 8, 16) == 8 && before == after);
        json[length] = 0;
        if (stats_number(json, "context") == context) { found = fd; break; }
    }
    CHECK(closedir(directory) == 0);
    return found;
}

static void test_discovery(void)
{
    struct fixture f;
    init_driver(&f);
    VAProfile profiles[DMD_MAX_PROFILES];
    int count = 0;
    OK(f.vt.vaQueryConfigProfiles(&f.va, profiles, &count));
    CHECK(count > 0 && count <= f.va.max_profiles);
    const VAProfile expected[] = { VAProfileH264ConstrainedBaseline,
        VAProfileH264Main, VAProfileH264High, VAProfileHEVCMain, VAProfileVP9Profile0 };
    for (unsigned p = 0; p < sizeof(expected) / sizeof(expected[0]); p++) {
        int found = 0;
        for (int i = 0; i < count; i++)
            found |= profiles[i] == expected[p];
        CHECK(found);
        VAEntrypoint entries[DMD_MAX_ENTRYPOINTS];
        int n = 0;
        OK(f.vt.vaQueryConfigEntrypoints(&f.va, expected[p], entries, &n));
        found = 0;
        for (int i = 0; i < n; i++)
            found |= entries[i] == VAEntrypointEncSlice;
        CHECK(found);
        VAConfigAttrib attrs[] = { { .type = VAConfigAttribRTFormat },
            { .type = VAConfigAttribRateControl },
            { .type = VAConfigAttribEncPackedHeaders },
            { .type = VAConfigAttribEncMaxRefFrames } };
        OK(f.vt.vaGetConfigAttributes(&f.va, expected[p], VAEntrypointEncSlice,
                                      attrs, 4));
        CHECK(attrs[0].value == VA_RT_FORMAT_YUV420);
        CHECK(attrs[1].value == (VA_RC_CBR | VA_RC_VBR |
              (expected[p] == VAProfileVP9Profile0 ? 0 : VA_RC_CQP)));
        CHECK(attrs[2].value == 0);
        CHECK((attrs[3].value >> 16) == 0); /* no backward references/B frames */
    }
    VAConfigID id = VA_INVALID_ID;
    BAD(f.vt.vaCreateConfig(&f.va, VAProfileHEVCMain10, VAEntrypointEncSlice,
                            NULL, 0, &id));
    VAConfigAttrib cqp = { VAConfigAttribRateControl, VA_RC_CQP };
    OK(f.vt.vaCreateConfig(&f.va, VAProfileH264Main, VAEntrypointEncSlice,
                           &cqp, 1, &id));
    OK(f.vt.vaDestroyConfig(&f.va, id));
    BAD(f.vt.vaCreateConfig(&f.va, VAProfileVP9Profile0, VAEntrypointEncSlice,
                            &cqp, 1, &id));
    VAConfigAttrib unsupported;
    unsupported.type = VAConfigAttribRTFormat;
    unsupported.value = VA_RT_FORMAT_YUV420_10;
    BAD(f.vt.vaCreateConfig(&f.va, VAProfileH264Main, VAEntrypointEncSlice,
                            &unsupported, 1, &id));
    finish_driver(&f);

    /* Missing kernel encoder capabilities must leave decoding usable while
     * suppressing encode entrypoints, rather than advertising a dead encoder. */
    fake_caps = 0;
    init_driver(&f);
    VAEntrypoint entries[DMD_MAX_ENTRYPOINTS];
    int n = 0;
    OK(f.vt.vaQueryConfigEntrypoints(&f.va, VAProfileH264Main, entries, &n));
    int decode = 0;
    for (int i = 0; i < n; i++) {
        CHECK(entries[i] != VAEntrypointEncSlice);
        decode |= entries[i] == VAEntrypointVLD;
    }
    CHECK(decode);
    BAD(f.vt.vaCreateConfig(&f.va, VAProfileH264Main, VAEntrypointEncSlice,
                            NULL, 0, &id));
    finish_driver(&f);
    fake_caps = (1u << (DMD_ENC_VP9_0 + 1)) - 1;
}

static void test_encode_profiles(void)
{
    const VAProfile profiles[] = { VAProfileH264ConstrainedBaseline,
        VAProfileH264Main, VAProfileH264High, VAProfileHEVCMain, VAProfileVP9Profile0 };
    for (unsigned p = 0; p < sizeof(profiles) / sizeof(profiles[0]); p++) {
        struct fixture f;
        init_driver(&f);
        create_encode(&f, profiles[p], 128, 96, p & 1 ? VA_RC_VBR : VA_RC_CBR);
        char stats[4096];
        int stats_descriptor = stats_snapshot(f.context, stats);
        CHECK(stats_descriptor >= 0 && stats_number(stats, "hw_completed") == 0);
        CHECK(strstr(stats, "\"state\":\"created\""));
        VAImage image = upload_pattern(&f);
        unsigned before_open = fake_open_calls, before_frame = fake_frame_calls;
        for (int key = 1; key >= 0; key--) {
            VABufferID coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
            OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
            render_frame(&f, profiles[p], coded, key);
            OK(f.vt.vaEndPicture(&f.va, f.context));
            CHECK(stats_snapshot(f.context, stats) >= 0);
            CHECK(stats_number(stats, "hw_completed") == (unsigned)(2 - key));
            CHECK(stats_number(stats, "output_frames") == (unsigned)(2 - key));
            CHECK(stats_number(stats, "width") == 128 && stats_number(stats, "height") == 96);
            CHECK(strstr(stats, "\"target_fps\":29.970029") && stats_number(stats, "errors") == 0);
            check_packet(&f, coded, profiles[p]);
            OK(f.vt.vaSyncSurface(&f.va, f.surface));
            check_pixels(f.width, f.height);
            CHECK(fake_force_key == key);
            OK(f.vt.vaDestroyBuffer(&f.va, coded));
        }
        CHECK(fake_open_calls == before_open + 1); /* a continuous stream */
        CHECK(fake_frame_calls == before_frame + 2);
        CHECK(fake_config.width == f.width && fake_config.height == f.height);
        CHECK(fake_config.bitrate == 2000000 && fake_config.gop_size == 60);
        CHECK(fake_config.fps_num == 30000 && fake_config.fps_den == 1001);
        CHECK(fake_config.cbr == !(p & 1));
        CHECK((unsigned)fake_config.profile == p);
        OK(f.vt.vaDestroyImage(&f.va, image.image_id));
        unsigned before_close = fake_close_calls;
        OK(f.vt.vaDestroyContext(&f.va, f.context));
        CHECK(fcntl(stats_descriptor, F_GETFD) < 0 && errno == EBADF);
        CHECK(fake_close_calls == before_close + 1);
        OK(f.vt.vaDestroySurfaces(&f.va, &f.surface, 1));
        OK(f.vt.vaDestroyConfig(&f.va, f.config));
        finish_driver(&f);
    }
}

static void test_upload_geometry(void)
{
    struct fixture f;
    init_driver(&f);
    create_encode(&f, VAProfileH264Main, 130, 98, VA_RC_CBR);
    VAImage image = upload_pattern(&f);
    /* Check pixels through the public image API, including both planes. */
    VAImage derived;
    OK(f.vt.vaDeriveImage(&f.va, f.surface, &derived));
    uint8_t *pixels = NULL;
    OK(f.vt.vaMapBuffer(&f.va, derived.buf, (void **)&pixels));
    CHECK(derived.pitches[0] >= 130 && derived.offsets[1] >= derived.pitches[0] * 98);
    for (unsigned y = 0; y < 98; y++)
        for (unsigned x = 0; x < 130; x++)
            CHECK(pixels[derived.offsets[0] + y * derived.pitches[0] + x] == y_sample(x, y));
    for (unsigned y = 0; y < 49; y++)
        for (unsigned x = 0; x < 130; x++)
            CHECK(pixels[derived.offsets[1] + y * derived.pitches[1] + x] == uv_sample(x, y));
    OK(f.vt.vaUnmapBuffer(&f.va, derived.buf));
    BAD(f.vt.vaPutImage(&f.va, f.surface, image.image_id, 0, 0, 130, 98,
                         0, 0, 128, 96)); /* scaling is not advertised */
    BAD(f.vt.vaPutImage(&f.va, f.surface, image.image_id, 1, 0, 128, 98,
                         0, 0, 128, 98)); /* NV12 chroma requires even origins */
    OK(f.vt.vaDestroyImage(&f.va, derived.image_id));
    OK(f.vt.vaDestroyImage(&f.va, image.image_id));
    finish_driver(&f);
}

static void test_backend_failures(void)
{
    const int errors[] = { ENOSPC, ETIMEDOUT, EIO };
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        struct fixture f;
        init_driver(&f);
        create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
        VAImage image = upload_pattern(&f);
        VABufferID coded = make_buffer(&f, VAEncCodedBufferType, i == 0 ? 4 : 4096, NULL);
        OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
        render_frame(&f, VAProfileH264Main, coded, 1);
        fake_frame_error = errors[i];
        VAStatus status = f.vt.vaEndPicture(&f.va, f.context);
        char stats[4096];
        CHECK(stats_snapshot(f.context, stats) >= 0);
        CHECK(stats_number(stats, "hw_completed") == 0 && stats_number(stats, "output_frames") == 0);
        CHECK(stats_number(stats, "errors") > 0);
        fake_frame_error = 0;
        CHECK(status == (i == 0 ? VA_STATUS_ERROR_NOT_ENOUGH_BUFFER :
                                  VA_STATUS_ERROR_ENCODING_ERROR));
        /* A failed frame must never expose a stale success packet. */
        VACodedBufferSegment *segment = NULL;
        OK(f.vt.vaMapBuffer(&f.va, coded, (void **)&segment));
        CHECK(segment && segment->size == 0);
        CHECK(segment->status & (i == 0 ? (VA_CODED_BUF_STATUS_FRAME_SIZE_OVERFLOW |
                                          VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK) :
                                           VA_CODED_BUF_STATUS_BAD_BITSTREAM));
        OK(f.vt.vaUnmapBuffer(&f.va, coded));
        BAD(f.vt.vaSyncBuffer(&f.va, coded, UINT64_MAX));
        OK(f.vt.vaDestroyBuffer(&f.va, coded));
        /* A failed stateful stream stays failed.  A new VA context must be
         * usable immediately, without leaking or retaining its predecessor. */
        BAD(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
        OK(f.vt.vaDestroyContext(&f.va, f.context));
        OK(f.vt.vaCreateContext(&f.va, f.config, (int)f.width, (int)f.height,
                                VA_PROGRESSIVE, &f.surface, 1, &f.context));
        coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
        OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
        render_frame(&f, VAProfileH264Main, coded, 1);
        OK(f.vt.vaEndPicture(&f.va, f.context));
        check_packet(&f, coded, VAProfileH264Main);
        OK(f.vt.vaDestroyBuffer(&f.va, coded));
        OK(f.vt.vaDestroyImage(&f.va, image.image_id));
        finish_driver(&f);
    }
    struct fixture f;
    init_driver(&f);
    create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
    VAImage image = upload_pattern(&f);
    OK(f.vt.vaDestroyImage(&f.va, image.image_id));
    VABufferID coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
    OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
    render_frame(&f, VAProfileH264Main, coded, 1);
    fake_open_error = ENODEV;
    BAD(f.vt.vaEndPicture(&f.va, f.context));
    fake_open_error = 0;
    finish_driver(&f);
}

static void test_rejected_parameters(void)
{
    for (int mode = 0; mode < 3; mode++) {
        struct fixture f;
        init_driver(&f);
        create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
        VAImage image = upload_pattern(&f);
        OK(f.vt.vaDestroyImage(&f.va, image.image_id));
        VABufferID invalid = VA_INVALID_ID;
        CHECK(f.vt.vaCreateBuffer(&f.va, VA_INVALID_ID, VAEncCodedBufferType,
                                  4096, 1, NULL, &invalid) ==
              VA_STATUS_ERROR_INVALID_CONTEXT);
        BAD(f.vt.vaSyncBuffer(&f.va, VA_INVALID_ID, 0));
        BAD(f.vt.vaBeginPicture(&f.va, VA_INVALID_ID, f.surface));
        BAD(f.vt.vaBeginPicture(&f.va, f.context, VA_INVALID_SURFACE));
        VABufferID coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
        VAContextID second = VA_INVALID_ID;
        if (mode == 2) {
            OK(f.vt.vaDestroyBuffer(&f.va, coded));
            OK(f.vt.vaCreateContext(&f.va, f.config, 128, 96, VA_PROGRESSIVE,
                                    &f.surface, 1, &second));
            VAContextID saved = f.context;
            f.context = second;
            coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
            f.context = saved;
        }
        VAEncSequenceParameterBufferH264 seq = { 0 };
        seq.level_idc = 40;
        seq.intra_period = seq.intra_idr_period = 60;
        seq.ip_period = 1;
        seq.bits_per_second = 2000000;
        seq.max_num_ref_frames = 1;
        seq.picture_width_in_mbs = 8;
        seq.picture_height_in_mbs = 6;
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.seq_fields.bits.frame_mbs_only_flag = 1;
        if (mode == 0)
            seq.bit_depth_luma_minus8 = seq.bit_depth_chroma_minus8 = 2;
        VAEncPictureParameterBufferH264 pic = { 0 };
        pic.coded_buf = coded;
        pic.pic_init_qp = 26;
        pic.pic_fields.bits.idr_pic_flag = mode != 1;
        VAEncSliceParameterBufferH264 slice = { 0 };
        slice.num_macroblocks = 48;
        slice.macroblock_info = VA_INVALID_ID;
        slice.slice_type = mode == 1 ? 1 : 2;
        VABufferID parameters[] = {
            make_buffer(&f, VAEncSequenceParameterBufferType, sizeof(seq), &seq),
            make_buffer(&f, VAEncPictureParameterBufferType, sizeof(pic), &pic),
            make_buffer(&f, VAEncSliceParameterBufferType, sizeof(slice), &slice),
        };
        unsigned before = fake_frame_calls;
        OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
        VAStatus render = f.vt.vaRenderPicture(&f.va, f.context, parameters, 3);
        VAStatus end = f.vt.vaEndPicture(&f.va, f.context);
        CHECK(render != VA_STATUS_SUCCESS || end != VA_STATUS_SUCCESS);
        CHECK(fake_frame_calls == before);
        for (unsigned b = 0; b < 3; b++)
            OK(f.vt.vaDestroyBuffer(&f.va, parameters[b]));
        OK(f.vt.vaDestroyBuffer(&f.va, coded));
        if (second != VA_INVALID_ID)
            OK(f.vt.vaDestroyContext(&f.va, second));
        /* Error paths must release the active picture and its buffer links. */
        coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
        OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
        render_frame(&f, VAProfileH264Main, coded, 1);
        OK(f.vt.vaEndPicture(&f.va, f.context));
        check_packet(&f, coded, VAProfileH264Main);
        finish_driver(&f);
    }
}

static void test_decoded_surface_visible_geometry(void)
{
    struct fixture f;
    init_driver(&f);
    /* Decode pools and the encoder reconstruction context can both expose
     * an aligned height.  The decoded frame's crop still describes the
     * actual image, as with a 1088-row pool holding a 1080-row frame. */
    create_encode(&f, VAProfileHEVCMain, 128, 112, VA_RC_CBR);
    VAImage image = upload_pattern(&f);
    OK(f.vt.vaDestroyImage(&f.va, image.image_id));
    struct dmd_driver *drv = dmd_get_driver(&f.va);
    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *source = dmd_find_surface_locked(drv, f.surface);
    CHECK(source && source->width == 128 && source->height == 112);
    source->visible_width = 128;
    source->visible_height = 104;
    source->visible_x = source->visible_y = 0;
    const unsigned original_stride = source->stride;
    const unsigned original_slice_height = source->slice_height;
    pthread_mutex_unlock(&drv->lock);
    unsigned before_open = fake_open_calls;
    for (int key = 1; key >= 0; key--) {
        VABufferID coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
        OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
        /* VA sequence and context retain the padded pool dimensions. */
        render_frame(&f, VAProfileHEVCMain, coded, key);
        OK(f.vt.vaEndPicture(&f.va, f.context));
        check_packet(&f, coded, VAProfileHEVCMain);
        CHECK(fake_config.width == 128 && fake_config.height == 104);
        char stats[4096];
        CHECK(stats_snapshot(f.context, stats) >= 0);
        CHECK(stats_number(stats, "height") == 104);
        CHECK(fake_stride == original_stride);
        CHECK(fake_slice_height == original_slice_height);
        check_pixels(128, 104);
        OK(f.vt.vaDestroyBuffer(&f.va, coded));
    }
    CHECK(fake_open_calls == before_open + 1);
    pthread_mutex_lock(&drv->lock);
    source = dmd_find_surface_locked(drv, f.surface);
    CHECK(source && source->width == 128 && source->height == 112);
    CHECK(source->stride == original_stride && source->slice_height == original_slice_height);
    CHECK(source->visible_width == 128 && source->visible_height == 104);
    pthread_mutex_unlock(&drv->lock);
    finish_driver(&f);
}

struct concurrent_call {
    struct fixture *f;
    VAContextID other_context;
    VABufferID coded;
    int destroy_surface;
    int operation_done;
    VAStatus end_status, independent_status, destroy_status;
};

static void *end_picture_thread(void *opaque)
{
    struct concurrent_call *call = opaque;
    call->end_status = call->f->vt.vaEndPicture(&call->f->va, call->f->context);
    return NULL;
}

static void *destroy_during_encode_thread(void *opaque)
{
    struct concurrent_call *call = opaque;
    struct fixture *f = call->f;
    VABufferID independent = VA_INVALID_ID;
    call->independent_status = f->vt.vaCreateBuffer(&f->va, call->other_context,
        VAEncCodedBufferType, 4096, 1, NULL, &independent);
    if (call->independent_status == VA_STATUS_SUCCESS)
        call->independent_status = f->vt.vaDestroyBuffer(&f->va, independent);
    if (call->destroy_surface)
        call->destroy_status = f->vt.vaDestroySurfaces(&f->va, &f->surface, 1);
    else
        call->destroy_status = f->vt.vaDestroyBuffer(&f->va, call->coded);
    pthread_mutex_lock(&fake_lock);
    call->operation_done = 1;
    pthread_cond_broadcast(&fake_cond);
    pthread_mutex_unlock(&fake_lock);
    return NULL;
}

/* Timed test handshakes ensure a regression which holds drv->lock during
 * firmware IO reports failure instead of deadlocking the test runner. */
static int wait_test_flag(int *flag)
{
    struct timespec deadline;
    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 2;
    pthread_mutex_lock(&fake_lock);
    int result = 0;
    while (!*flag) {
        int rc = pthread_cond_timedwait(&fake_cond, &fake_lock, &deadline);
        if (rc == ETIMEDOUT) break;
        CHECK(rc == 0);
    }
    result = *flag;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static void test_concurrent_destruction(void)
{
    for (int destroy_surface = 0; destroy_surface <= 1; destroy_surface++) {
        struct fixture f;
        init_driver(&f);
        create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
        VAImage image = upload_pattern(&f);
        OK(f.vt.vaDestroyImage(&f.va, image.image_id));
        struct concurrent_call call = { .f = &f, .destroy_surface = destroy_surface };
        OK(f.vt.vaCreateContext(&f.va, f.config, (int)f.width, (int)f.height,
                                VA_PROGRESSIVE, &f.surface, 1, &call.other_context));
        call.coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
        OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
        render_frame(&f, VAProfileH264Main, call.coded, 1);
        pthread_mutex_lock(&fake_lock);
        fake_block_frame = 1;
        fake_frame_entered = fake_release_frame = 0;
        pthread_mutex_unlock(&fake_lock);
        pthread_t encoding, destroying;
        CHECK(pthread_create(&encoding, NULL, end_picture_thread, &call) == 0);
        int entered = wait_test_flag(&fake_frame_entered);
        CHECK(pthread_create(&destroying, NULL, destroy_during_encode_thread, &call) == 0);
        int operation_completed_while_blocked = wait_test_flag(&call.operation_done);
        pthread_mutex_lock(&fake_lock);
        fake_release_frame = 1;
        pthread_cond_broadcast(&fake_cond);
        pthread_mutex_unlock(&fake_lock);
        CHECK(pthread_join(encoding, NULL) == 0);
        CHECK(pthread_join(destroying, NULL) == 0);
        pthread_mutex_lock(&fake_lock);
        fake_block_frame = 0;
        pthread_mutex_unlock(&fake_lock);
        CHECK(entered && operation_completed_while_blocked);
        CHECK(call.independent_status == VA_STATUS_SUCCESS);
        CHECK(call.destroy_status == VA_STATUS_SUCCESS);
        char stats[4096];
        CHECK(stats_snapshot(f.context, stats) >= 0);
        CHECK(stats_number(stats, "hw_completed") == 1);
        CHECK(stats_number(stats, "output_frames") == (unsigned)destroy_surface);
        CHECK(stats_number(stats, "errors") == (unsigned)!destroy_surface);
        check_pixels(f.width, f.height);
        if (destroy_surface) {
            /* The source snapshot belongs to EndPicture, so deleting the
             * surface after snapshotting cannot invalidate the input. */
            CHECK(call.end_status == VA_STATUS_SUCCESS);
            check_packet(&f, call.coded, VAProfileH264Main);
            OK(f.vt.vaDestroyBuffer(&f.va, call.coded));
        } else {
            /* The completed packet has nowhere to go after its VA buffer
             * disappears, and must not be copied into freed/reused storage. */
            CHECK(call.end_status == VA_STATUS_ERROR_INVALID_BUFFER);
            BAD(f.vt.vaSyncBuffer(&f.va, call.coded, 0));
        }
        OK(f.vt.vaDestroyContext(&f.va, call.other_context));
        finish_driver(&f);
    }
}

struct external_input {
    VADRMPRIMESurfaceDescriptor prime;
    VASurfaceAttribExternalBuffers legacy;
    uintptr_t legacy_fd;
    int fds[2];
    uint8_t *maps[2];
    size_t sizes[2];
    unsigned objects;
};

static unsigned fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    CHECK(directory);
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (entry->d_name[0] != '.') count++;
    CHECK(closedir(directory) == 0);
    return count;
}

static void external_input_init(struct external_input *input, int separate)
{
    memset(input, 0, sizeof(*input));
    input->fds[0] = input->fds[1] = -1;
    input->objects = separate ? 2 : 1;
    input->prime.fourcc = VA_FOURCC_NV12;
    input->prime.width = 128;
    input->prime.height = 96;
    input->prime.num_objects = input->objects;
    input->prime.num_layers = separate ? 2 : 1;
    input->sizes[0] = separate ? 160 * 112 + 64 : 160 * 112 + 192 * 48 + 128;
    input->sizes[1] = 192 * 48 + 128;
    for (unsigned i = 0; i < input->objects; i++) {
        input->fds[i] = memfd_create("dmd-nv12-import-test", MFD_CLOEXEC);
        CHECK(input->fds[i] >= 0);
        CHECK(ftruncate(input->fds[i], (off_t)input->sizes[i]) == 0);
        input->maps[i] = mmap(NULL, input->sizes[i], PROT_READ | PROT_WRITE,
                               MAP_SHARED, input->fds[i], 0);
        CHECK(input->maps[i] != MAP_FAILED);
        memset(input->maps[i], 0xa5, input->sizes[i]);
        input->prime.objects[i].fd = input->fds[i];
        input->prime.objects[i].size = (uint32_t)input->sizes[i];
        input->prime.objects[i].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    }
    input->prime.layers[0].drm_format = separate ? DRM_FORMAT_R8 : DRM_FORMAT_NV12;
    input->prime.layers[0].num_planes = separate ? 1 : 2;
    input->prime.layers[0].offset[0] = 32;
    input->prime.layers[0].pitch[0] = 160;
    unsigned uv_offset = separate ? 64 : 160 * 112;
    if (separate) {
        input->prime.layers[1].drm_format = DRM_FORMAT_GR88;
        input->prime.layers[1].num_planes = 1;
        input->prime.layers[1].object_index[0] = 1;
        input->prime.layers[1].offset[0] = uv_offset;
        input->prime.layers[1].pitch[0] = 192;
    } else {
        input->prime.layers[0].offset[1] = uv_offset;
        input->prime.layers[0].pitch[1] = 192;
    }
    for (unsigned y = 0; y < 96; y++)
        for (unsigned x = 0; x < 128; x++)
            input->maps[0][32 + y * 160 + x] = y_sample(x, y);
    for (unsigned y = 0; y < 48; y++)
        for (unsigned x = 0; x < 128; x++)
            input->maps[separate ? 1 : 0][uv_offset + y * 192 + x] = uv_sample(x, y);
    input->legacy_fd = (uintptr_t)input->fds[0];
    input->legacy.pixel_format = VA_FOURCC_NV12;
    input->legacy.width = 128;
    input->legacy.height = 96;
    input->legacy.data_size = (uint32_t)input->sizes[0];
    input->legacy.num_planes = 2;
    input->legacy.pitches[0] = 160;
    input->legacy.pitches[1] = 192;
    input->legacy.offsets[0] = 32;
    input->legacy.offsets[1] = uv_offset;
    input->legacy.buffers = &input->legacy_fd;
    input->legacy.num_buffers = 1;
}

static VASurfaceID import_input(struct fixture *f, unsigned memory_type, const void *descriptor)
{
    VASurfaceAttrib attrs[2] = {
        { .type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE,
          .value = { .type = VAGenericValueTypeInteger, .value.i = (int)memory_type } },
        { .type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE,
          .value = { .type = VAGenericValueTypePointer, .value.p = (void *)descriptor } },
    };
    VASurfaceID surface = VA_INVALID_ID;
    OK(f->vt.vaCreateSurfaces2(&f->va, VA_RT_FORMAT_YUV420, 128, 96,
                               &surface, 1, attrs, 2));
    CHECK(surface != VA_INVALID_ID);
    return surface;
}

static void external_close_fds(struct external_input *input)
{
    for (unsigned i = 0; i < input->objects; i++)
        if (input->fds[i] >= 0) {
            CHECK(close(input->fds[i]) == 0);
            input->fds[i] = -1;
        }
}

static void external_input_finish(struct external_input *input)
{
    external_close_fds(input);
    for (unsigned i = 0; i < input->objects; i++)
        CHECK(munmap(input->maps[i], input->sizes[i]) == 0);
}

static void test_prime_import(void)
{
    for (int kind = 0; kind < 3; kind++) {
        struct fixture f;
        init_driver(&f);
        create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
        OK(f.vt.vaDestroySurfaces(&f.va, &f.surface, 1));
        struct external_input input;
        external_input_init(&input, kind == 2);
        unsigned baseline_fds = fd_count();
        f.surface = import_input(&f, kind ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 :
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME,
                                  kind ? (void *)&input.prime : (void *)&input.legacy);
        CHECK(fd_count() == baseline_fds + input.objects);
        for (unsigned i = 0; i < input.objects; i++) CHECK(fcntl(input.fds[i], F_GETFD) >= 0);
        fake_sync_mode = 1;
        unsigned before_sync = fake_sync_starts;
        for (int n = 0; n < 3; n++) {
            if (n) input.maps[0][32] ^= 1;
            if (n == 2) external_close_fds(&input);
            VABufferID coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
            OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
            render_frame(&f, VAProfileH264Main, coded, n == 0);
            OK(f.vt.vaEndPicture(&f.va, f.context));
            check_packet(&f, coded, VAProfileH264Main);
            if (!n) check_pixels(128, 96);
            CHECK(fake_pixels[0] == (uint8_t)(y_sample(0, 0) ^ (n == 1)));
            OK(f.vt.vaDestroyBuffer(&f.va, coded));
        }
        CHECK(fake_sync_starts == before_sync + 3 * input.objects);
        CHECK(fake_sync_starts == fake_sync_ends);
        OK(f.vt.vaDestroySurfaces(&f.va, &f.surface, 1));
        CHECK(fd_count() == baseline_fds - input.objects);
        fake_sync_mode = 0;
        external_input_finish(&input);
        finish_driver(&f);
    }
}

static void test_prime_import_rejects(void)
{
    struct fixture f;
    init_driver(&f);
    create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
    struct external_input input;
    external_input_init(&input, 0);
    unsigned baseline_fds = fd_count();
    VASurfaceAttrib attrs[2] = {
        { .type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE,
          .value = { .type = VAGenericValueTypeInteger,
                     .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 } },
        { .type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE,
          .value = { .type = VAGenericValueTypePointer, .value.p = &input.prime } },
    };
    for (unsigned mode = 0; mode < 7; mode++) {
        VADRMPRIMESurfaceDescriptor desc = input.prime;
        if (mode == 0) desc.layers[0].object_index[1] = 4;
        if (mode == 1) desc.layers[0].offset[1] = UINT32_MAX - 3;
        if (mode == 2) desc.layers[0].pitch[0] = 126;
        if (mode == 3) desc.objects[0].drm_format_modifier = 1; /* not linear */
        if (mode == 4) desc.objects[0].size = (uint32_t)input.sizes[0] + 4096;
        if (mode == 5) desc.layers[0].offset[1] = 64; /* overlaps Y */
        if (mode == 6) desc.objects[0].fd = -1;
        attrs[1].value.value.p = &desc;
        VASurfaceID invalid = VA_INVALID_ID;
        BAD(f.vt.vaCreateSurfaces2(&f.va, VA_RT_FORMAT_YUV420, 128, 96,
                                    &invalid, 1, attrs, 2));
        CHECK(fd_count() == baseline_fds);
        CHECK(fcntl(input.fds[0], F_GETFD) >= 0);
    }
    VADRMPRIMESurfaceDescriptor batch[] = { input.prime, input.prime };
    batch[1].layers[0].object_index[1] = 4;
    attrs[1].value.value.p = batch;
    VASurfaceID rolled_back[2] = { VA_INVALID_ID, VA_INVALID_ID };
    BAD(f.vt.vaCreateSurfaces2(&f.va, VA_RT_FORMAT_YUV420, 128, 96,
                                rolled_back, 2, attrs, 2));
    CHECK(rolled_back[0] == VA_INVALID_ID && rolled_back[1] == VA_INVALID_ID);
    CHECK(fd_count() == baseline_fds);
    external_input_finish(&input);
    finish_driver(&f);
}

static void test_prime_size_discovery(void)
{
    struct fixture f;
    init_driver(&f);
    create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
    struct external_input input;
    external_input_init(&input, 0);
    unsigned baseline_fds = fd_count();
    for (int mode = 0; mode <= 4; mode++) {
        /* Producer file offsets must survive ordinary-file probing. A DMA
         * descriptor that rejects CUR can only request the ABI SET(0). */
        CHECK(syscall(SYS_lseek, input.fds[0], 173, SEEK_SET) == 173);
        fake_size_mode = mode;
        fake_size_stat_calls = fake_size_end_calls = fake_size_set_calls = 0;
        VASurfaceAttrib attrs[2] = {
            { .type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE,
              .value = { .type = VAGenericValueTypeInteger,
                         .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 } },
            { .type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE,
              .value = { .type = VAGenericValueTypePointer, .value.p = &input.prime } },
        };
        VASurfaceID imported = VA_INVALID_ID;
        VAStatus status = f.vt.vaCreateSurfaces2(&f.va, VA_RT_FORMAT_YUV420, 128, 96,
                                                 &imported, 1, attrs, 2);
        if (mode <= 2) {
            OK(status);
            CHECK(imported != VA_INVALID_ID && fd_count() == baseline_fds + 1);
            OK(f.vt.vaDestroySurfaces(&f.va, &imported, 1));
        } else {
            BAD(status);
            CHECK(imported == VA_INVALID_ID);
        }
        CHECK(fd_count() == baseline_fds);
        CHECK(fcntl(input.fds[0], F_GETFD) >= 0);
        CHECK(syscall(SYS_lseek, input.fds[0], 0, SEEK_CUR) == (mode == 1 ? 0 : 173));
        if (mode) {
            CHECK(fake_size_end_calls == 1 && fake_size_set_calls == 1);
            CHECK(fake_size_stat_calls == (unsigned)(mode == 2 || mode == 3));
        }
        fake_size_mode = 0;
    }
    external_input_finish(&input);
    finish_driver(&f);
}

static void test_concurrent_import_destruction(void)
{
    struct fixture f;
    init_driver(&f);
    create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
    OK(f.vt.vaDestroySurfaces(&f.va, &f.surface, 1));
    struct external_input input;
    external_input_init(&input, 1);
    unsigned baseline_fds = fd_count();
    f.surface = import_input(&f, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, &input.prime);
    struct concurrent_call call = { .f = &f, .destroy_surface = 1 };
    OK(f.vt.vaCreateContext(&f.va, f.config, 128, 96, VA_PROGRESSIVE,
                            &f.surface, 1, &call.other_context));
    /* The independent context may own its own telemetry descriptor. Keep
     * the import FD lifetime assertions independent of that optional FD. */
    baseline_fds = fd_count() - input.objects;
    call.coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
    OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
    render_frame(&f, VAProfileH264Main, call.coded, 1);
    pthread_mutex_lock(&fake_lock);
    fake_sync_mode = fake_block_sync = 1;
    fake_frame_entered = fake_release_frame = 0;
    pthread_mutex_unlock(&fake_lock);
    pthread_t encoding, destroying;
    CHECK(pthread_create(&encoding, NULL, end_picture_thread, &call) == 0);
    int entered = wait_test_flag(&fake_frame_entered);
    CHECK(pthread_create(&destroying, NULL, destroy_during_encode_thread, &call) == 0);
    int operation_done = wait_test_flag(&call.operation_done);
    /* DestroySurface has released its reference, but the blocked read owns
     * another reference and must still keep both mappings and FDs alive. */
    CHECK(fd_count() == baseline_fds + 2);
    pthread_mutex_lock(&fake_lock);
    fake_release_frame = 1;
    pthread_cond_broadcast(&fake_cond);
    pthread_mutex_unlock(&fake_lock);
    CHECK(pthread_join(encoding, NULL) == 0);
    CHECK(pthread_join(destroying, NULL) == 0);
    fake_block_sync = fake_sync_mode = 0;
    CHECK(entered && operation_done);
    CHECK(call.independent_status == VA_STATUS_SUCCESS);
    CHECK(call.destroy_status == VA_STATUS_SUCCESS && call.end_status == VA_STATUS_SUCCESS);
    check_pixels(128, 96);
    check_packet(&f, call.coded, VAProfileH264Main);
    CHECK(fd_count() == baseline_fds);
    external_input_finish(&input);
    finish_driver(&f);
}

static void test_prime_sync_failure(void)
{
    struct fixture f;
    init_driver(&f);
    create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
    OK(f.vt.vaDestroySurfaces(&f.va, &f.surface, 1));
    struct external_input input;
    external_input_init(&input, 0);
    f.surface = import_input(&f, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, &input.prime);
    for (int mode = 0; mode <= 3; mode++) {
        if (mode == 1) continue;
        fake_sync_mode = mode;
        unsigned before = fake_frame_calls;
        VABufferID coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
        OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
        render_frame(&f, VAProfileH264Main, coded, 1);
        /* mode 0 is real memfd ENOTTY; mode 2/3 inject START/END EIO. */
        CHECK(f.vt.vaEndPicture(&f.va, f.context) == VA_STATUS_ERROR_OPERATION_FAILED);
        CHECK(fake_frame_calls == before);
        BAD(f.vt.vaSyncBuffer(&f.va, coded, 0));
        CHECK(fcntl(input.fds[0], F_GETFD) >= 0);
        OK(f.vt.vaDestroyBuffer(&f.va, coded));
    }
    fake_sync_mode = 1;
    VABufferID coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
    OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
    render_frame(&f, VAProfileH264Main, coded, 1);
    OK(f.vt.vaEndPicture(&f.va, f.context));
    check_packet(&f, coded, VAProfileH264Main);
    fake_sync_mode = 0;
    external_input_finish(&input);
    finish_driver(&f);
}

/* Replace only the allocation with shared test storage. DRM export itself is
 * mocked above; the real export API, descriptor layout, application FD writes,
 * Begin/Render/End and the encoder's DMA read snapshot remain under test. */
static int make_exportable_input(struct fixture *f)
{
    struct dmd_driver *drv = dmd_get_driver(&f->va);
    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, f->surface);
    CHECK(s && !s->exportable && !s->imported && s->state == DMD_SURFACE_IDLE);
    int fd = memfd_create("dmd-external-write-test", MFD_CLOEXEC);
    CHECK(fd >= 0 && ftruncate(fd, (off_t)s->data_size) == 0);
    void *map = mmap(NULL, s->data_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED);
    memcpy(map, s->data, s->data_size);
    free(s->data);
    s->data = map;
    s->exportable = 1;
    s->dumb_handle = 17;
    s->dumb_size = s->data_size;
    s->dumb_drm_fd = fd;
    s->dumb_sync_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    CHECK(s->dumb_sync_fd >= 0);
    pthread_mutex_unlock(&drv->lock);
    return fd;
}

/* mpv's format negotiation uploads a real 16x16 image even when it intends to
 * pass through full-size VA decoder frames. This is an image-storage contract,
 * not a claim that the video hardware can encode/decode at 16x16. */
static void test_image_only_surface_dimensions(void)
{
    const unsigned invalid[][2] = {
        { 0, 16 }, { 16, 0 }, { 1, 16 }, { 16, 1 },
        { 15, 16 }, { 16, 15 }, { 17, 16 }, { 16, 17 },
        { DMD_MAX_WIDTH + 2, 16 }, { 16, DMD_MAX_HEIGHT + 2 },
    };
    for (unsigned ten_bit = 0; ten_bit < 2; ten_bit++) {
        const unsigned initial_fds = fd_count();
        struct fixture f;
        init_driver(&f);
        const unsigned rt = ten_bit ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
        VASurfaceAttrib attribute = {
            .type = VASurfaceAttribPixelFormat,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value.type = VAGenericValueTypeInteger,
            .value.value.i = ten_bit ? VA_FOURCC_P010 : VA_FOURCC_NV12,
        };
        for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            VASurfaceID surface = VA_INVALID_ID;
            CHECK(f.vt.vaCreateSurfaces2(&f.va, rt, invalid[i][0], invalid[i][1],
                                          &surface, 1, &attribute, 1) ==
                  VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
            CHECK(surface == VA_INVALID_ID);
        }
        f.width = f.height = 16;
        OK(f.vt.vaCreateSurfaces2(&f.va, rt, 16, 16, &f.surface, 1, &attribute, 1));
        int storage = make_exportable_input(&f);
        fake_export_fd = storage;
        fake_sync_mode = 4; /* image upload must bracket CPU writes */
        VAImageFormat format = { .fourcc = (uint32_t)attribute.value.value.i,
                                 .byte_order = VA_LSB_FIRST };
        VAImage image;
        OK(f.vt.vaCreateImage(&f.va, &format, 16, 16, &image));
        uint8_t *pixels;
        OK(f.vt.vaMapBuffer(&f.va, image.buf, (void **)&pixels));
        const unsigned bytes = ten_bit ? 2 : 1;
        memset(pixels, 0xa5, image.data_size);
        for (unsigned plane = 0; plane < 2; plane++) {
            unsigned rows = plane ? 8 : 16;
            for (unsigned y = 0; y < rows; y++) {
                for (unsigned x = 0; x < 16; x++) {
                    uint8_t *sample = pixels + image.offsets[plane] +
                                       (size_t)y * image.pitches[plane] + x * bytes;
                    uint8_t value = plane ? uv_sample(x, y) : y_sample(x, y);
                    sample[0] = ten_bit ? 0 : value;
                    if (ten_bit) sample[1] = value; /* legal MSB-aligned P010 */
                }
            }
        }
        OK(f.vt.vaUnmapBuffer(&f.va, image.buf));
        unsigned starts = fake_sync_starts, ends = fake_sync_ends;
        OK(f.vt.vaPutImage(&f.va, f.surface, image.image_id, 0, 0, 16, 16,
                            0, 0, 16, 16));
        CHECK(fake_sync_starts == starts + 1 && fake_sync_ends == ends + 1);
        VADRMPRIMESurfaceDescriptor desc;
        OK(f.vt.vaExportSurfaceHandle(&f.va, f.surface,
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc));
        CHECK(desc.width == 16 && desc.height == 16 && desc.num_layers == 2);
        CHECK(desc.num_objects == 1 && !(fake_export_flags & DRM_RDWR));
        CHECK(desc.fourcc == (ten_bit ? VA_FOURCC_P010 : VA_FOURCC_NV12));
        CHECK(desc.layers[0].drm_format == (ten_bit ? DRM_FORMAT_R16 : DRM_FORMAT_R8));
        CHECK(desc.layers[1].drm_format == (ten_bit ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88));
        CHECK(fcntl(desc.objects[0].fd, F_GETFD) & FD_CLOEXEC);
        const uint8_t *external = mmap(NULL, desc.objects[0].size, PROT_READ,
                                       MAP_SHARED, desc.objects[0].fd, 0);
        CHECK(external != MAP_FAILED);
        for (unsigned plane = 0; plane < 2; plane++) {
            CHECK(desc.layers[plane].num_planes == 1 &&
                  desc.layers[plane].object_index[0] == 0);
            CHECK(desc.layers[plane].pitch[0] == 128 * bytes);
            CHECK(desc.layers[plane].offset[0] == (plane ? 128 * bytes * 32 : 0));
            unsigned rows = plane ? 8 : 16;
            for (unsigned y = 0; y < rows; y++) {
                for (unsigned x = 0; x < 16; x++) {
                    const uint8_t *sample = external + desc.layers[plane].offset[0] +
                        (size_t)y * desc.layers[plane].pitch[0] + x * bytes;
                    uint8_t expected = plane ? uv_sample(x, y) : y_sample(x, y);
                    CHECK(sample[0] == (ten_bit ? 0 : expected));
                    if (ten_bit) CHECK(sample[1] == expected);
                }
            }
        }
        CHECK(munmap((void *)external, desc.objects[0].size) == 0);
        CHECK(close(desc.objects[0].fd) == 0);
        OK(f.vt.vaDestroyImage(&f.va, image.image_id));
        OK(f.vt.vaDestroySurfaces(&f.va, &f.surface, 1));
        fake_sync_mode = 0;
        fake_export_fd = -1;
        CHECK(close(storage) == 0);
        finish_driver(&f);
        CHECK(fd_count() == initial_fds);
    }
}

static void test_codec_context_minimums(void)
{
    static const struct {
        VAProfile profile;
        VAEntrypoint entrypoint;
        unsigned rt;
    } configs[] = {
        { VAProfileH264Main, VAEntrypointVLD, VA_RT_FORMAT_YUV420 },
        { VAProfileHEVCMain, VAEntrypointVLD, VA_RT_FORMAT_YUV420 },
        { VAProfileH264Main, VAEntrypointEncSlice, VA_RT_FORMAT_YUV420 },
#ifdef DMD_ENABLE_HEVC_MAIN10
        { VAProfileHEVCMain10, VAEntrypointVLD, VA_RT_FORMAT_YUV420_10 },
#endif
    };
    static const unsigned small[][2] = {
        { 16, 16 }, { 16, 96 }, { 96, 16 }, { 94, 96 }, { 96, 94 },
    };
    for (unsigned n = 0; n < sizeof(configs) / sizeof(configs[0]); n++) {
        struct fixture f;
        init_driver(&f);
        VAConfigAttrib attribute = { VAConfigAttribRTFormat, configs[n].rt };
        OK(f.vt.vaCreateConfig(&f.va, configs[n].profile, configs[n].entrypoint,
                               &attribute, 1, &f.config));
        OK(f.vt.vaCreateSurfaces2(&f.va, configs[n].rt, 16, 16,
                                  &f.surface, 1, NULL, 0));
        VASurfaceAttrib attributes[8];
        unsigned count = sizeof(attributes) / sizeof(attributes[0]);
        OK(f.vt.vaQuerySurfaceAttributes(&f.va, f.config, attributes, &count));
        unsigned minimums = 0;
        for (unsigned a = 0; a < count; a++) {
            if (attributes[a].type == VASurfaceAttribMinWidth) {
                CHECK(attributes[a].value.value.i == DMD_MIN_WIDTH);
                minimums++;
            }
            if (attributes[a].type == VASurfaceAttribMinHeight) {
                CHECK(attributes[a].value.value.i == DMD_MIN_HEIGHT);
                minimums++;
            }
        }
        CHECK(minimums == 2);
        unsigned opened = fake_open_calls;
        for (unsigned s = 0; s < sizeof(small) / sizeof(small[0]); s++) {
            VAContextID context = VA_INVALID_ID;
            CHECK(f.vt.vaCreateContext(&f.va, f.config, small[s][0], small[s][1],
                                        VA_PROGRESSIVE, &f.surface, 1, &context) ==
                  VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
            CHECK(context == VA_INVALID_ID && fake_open_calls == opened);
        }
        struct dmd_driver *driver = dmd_get_driver(&f.va);
        for (unsigned c = 0; c < DMD_MAX_CONTEXTS; c++)
            CHECK(!driver->contexts[c].in_use);
        OK(f.vt.vaDestroySurfaces(&f.va, &f.surface, 1));
        OK(f.vt.vaDestroyConfig(&f.va, f.config));
        finish_driver(&f);
    }
}

static void test_write_export_encoder_input(void)
{
    unsigned initial_fds = fd_count();
    struct fixture f;
    init_driver(&f);
    create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
    BAD(f.vt.vaBeginPicture(&f.va, f.context, f.surface)); /* ordinary IDLE */
    int storage = make_exportable_input(&f);
    fake_export_fd = storage;
    struct dmd_driver *drv = dmd_get_driver(&f.va);
    VADRMPRIMESurfaceDescriptor desc;
    OK(f.vt.vaExportSurfaceHandle(&f.va, f.surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc));
    CHECK(!(fake_export_flags & DRM_RDWR));
    CHECK(close(desc.objects[0].fd) == 0);
    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, f.surface);
    CHECK(s && s->exported && !s->external_write_exported && s->state == DMD_SURFACE_IDLE);
    pthread_mutex_unlock(&drv->lock);
    BAD(f.vt.vaBeginPicture(&f.va, f.context, f.surface)); /* READ_ONLY grants no producer contract */

    fake_export_error = EIO;
    BAD(f.vt.vaExportSurfaceHandle(&f.va, f.surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc));
    fake_export_error = 0;
    BAD(f.vt.vaBeginPicture(&f.va, f.context, f.surface)); /* failed export grants nothing */

    struct external_input imported_storage;
    external_input_init(&imported_storage, 0);
    VASurfaceID imported_id = import_input(&f, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                          &imported_storage.prime);
    for (unsigned pass = 0; pass < 2; pass++) {
        unsigned access = pass ? VA_EXPORT_SURFACE_READ_WRITE : VA_EXPORT_SURFACE_WRITE_ONLY;
        OK(f.vt.vaExportSurfaceHandle(&f.va, f.surface,
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            access | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc));
        CHECK(fake_export_flags & DRM_RDWR);
        CHECK(desc.fourcc == VA_FOURCC_NV12 && desc.num_layers == 2 && desc.num_objects == 1);
        pthread_mutex_lock(&drv->lock);
        s = dmd_find_surface_locked(drv, f.surface);
        CHECK(s && s->external_write_exported && s->state == DMD_SURFACE_IDLE);
        pthread_mutex_unlock(&drv->lock);
        VAImage image;
        /* VLC probes a fresh pool through DeriveImage. Accessing allocated
         * storage must not claim that a decode has completed. */
        OK(f.vt.vaDeriveImage(&f.va, f.surface, &image));
        pthread_mutex_lock(&drv->lock);
        CHECK(dmd_find_surface_locked(drv, f.surface)->state == DMD_SURFACE_IDLE);
        pthread_mutex_unlock(&drv->lock);
        OK(f.vt.vaDestroyImage(&f.va, image.image_id));

        if (!pass) {
            /* A WRITE export must not erase any of the existing readiness,
             * format or ownership restrictions. A pending decode still takes
             * the normal wait path (invalid owner makes this test immediate). */
            for (unsigned invalid = 0; invalid < 6; invalid++) {
                pthread_mutex_lock(&drv->lock);
                s = dmd_find_surface_locked(drv, f.surface);
                struct dmd_surface saved = *s;
                if (invalid == 0) {
                    s->state = DMD_SURFACE_PENDING;
                    s->context = VA_INVALID_ID;
                } else if (invalid == 1) s->decode_status = VA_STATUS_ERROR_DECODING_ERROR;
                else if (invalid == 2) s->format = VA_RT_FORMAT_YUV420_10;
                else if (invalid == 3) s->exportable = 0;
                else if (invalid == 4) s->dumb_sync_fd = -1;
                else s->imported = dmd_find_surface_locked(drv, imported_id)->imported;
                pthread_mutex_unlock(&drv->lock);
                BAD(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
                pthread_mutex_lock(&drv->lock);
                *dmd_find_surface_locked(drv, f.surface) = saved;
                pthread_mutex_unlock(&drv->lock);
            }
        }

        uint8_t *pixels = mmap(NULL, desc.objects[0].size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, desc.objects[0].fd, 0);
        CHECK(pixels != MAP_FAILED);
        for (unsigned y = 0; y < f.height; y++)
            for (unsigned x = 0; x < f.width; x++)
                pixels[desc.layers[0].offset[0] + y * desc.layers[0].pitch[0] + x] = y_sample(x, y);
        for (unsigned y = 0; y < f.height / 2; y++)
            for (unsigned x = 0; x < f.width; x++)
                pixels[desc.layers[1].offset[0] + y * desc.layers[1].pitch[0] + x] = uv_sample(x, y);
        CHECK(munmap(pixels, desc.objects[0].size) == 0);
        CHECK(close(desc.objects[0].fd) == 0);

        VABufferID coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
        unsigned starts = fake_sync_starts, ends = fake_sync_ends;
        fake_sync_mode = 1;
        OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
        render_frame(&f, VAProfileH264Main, coded, 1);
        OK(f.vt.vaEndPicture(&f.va, f.context));
        fake_sync_mode = 0;
        CHECK(fake_sync_starts == starts + 1 && fake_sync_ends == ends + 1);
        check_packet(&f, coded, VAProfileH264Main);
        check_pixels(f.width, f.height);
        OK(f.vt.vaDestroyBuffer(&f.va, coded));
        pthread_mutex_lock(&drv->lock);
        s = dmd_find_surface_locked(drv, f.surface);
        CHECK(s && s->state == DMD_SURFACE_IDLE);
        s->external_write_exported = 0; /* separately verify READ_WRITE grants the contract */
        pthread_mutex_unlock(&drv->lock);
    }
    OK(f.vt.vaDestroySurfaces(&f.va, &imported_id, 1));
    external_input_finish(&imported_storage);
    fake_export_fd = -1;
    finish_driver(&f);
    CHECK(close(storage) == 0);
    CHECK(fd_count() == initial_fds);
}

static void test_legacy_image_prime_interop(void)
{
    const unsigned initial_fds = fd_count();
    struct fixture f;
    init_driver(&f);
    create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
    int storage = make_exportable_input(&f);
    fake_export_fd = storage;
    VAImage image;
    OK(f.vt.vaDeriveImage(&f.va, f.surface, &image));
    struct dmd_driver *drv = dmd_get_driver(&f.va);
    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, f.surface);
    CHECK(s && s->state == DMD_SURFACE_IDLE && s->decode_status == VA_STATUS_SUCCESS);
    pthread_mutex_unlock(&drv->lock);
    CHECK(f.vt.vaDestroySurfaces(&f.va, &f.surface, 1) == VA_STATUS_ERROR_SURFACE_BUSY);

    VABufferInfo info = { .mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_USER_PTR };
    CHECK(f.vt.vaAcquireBufferHandle(&f.va, image.buf, &info) ==
          VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    fake_export_error = EIO;
    info = (VABufferInfo){ .mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME };
    unsigned before = fd_count();
    BAD(f.vt.vaAcquireBufferHandle(&f.va, image.buf, &info));
    CHECK(fd_count() == before);
    fake_export_error = 0;
    info = (VABufferInfo){ .mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME };
    OK(f.vt.vaAcquireBufferHandle(&f.va, image.buf, &info));
    CHECK(info.mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME &&
          info.type == VAImageBufferType && info.mem_size == image.data_size);
    int exported_fd = (int)info.handle;
    CHECK(exported_fd >= 0 && fcntl(exported_fd, F_GETFD) >= 0);
    VABufferInfo second = { .mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME };
    OK(f.vt.vaAcquireBufferHandle(&f.va, image.buf, &second));
    CHECK(second.handle == info.handle && fd_count() == before + 1);
    void *pixels = (void *)1;
    BAD(f.vt.vaMapBuffer(&f.va, image.buf, &pixels));
    CHECK(pixels == NULL);
    BAD(f.vt.vaUnmapBuffer(&f.va, image.buf));
    CHECK(f.vt.vaDestroyImage(&f.va, image.image_id) == VA_STATUS_ERROR_SURFACE_BUSY);
    CHECK(f.vt.vaDestroySurfaces(&f.va, &f.surface, 1) == VA_STATUS_ERROR_SURFACE_BUSY);
    CHECK(f.vt.vaBeginPicture(&f.va, f.context, f.surface) == VA_STATUS_ERROR_SURFACE_BUSY);
    OK(f.vt.vaReleaseBufferHandle(&f.va, image.buf));
    CHECK(fcntl(exported_fd, F_GETFD) >= 0);
    OK(f.vt.vaReleaseBufferHandle(&f.va, image.buf));
    errno = 0;
    CHECK(fcntl(exported_fd, F_GETFD) < 0 && errno == EBADF);
    OK(f.vt.vaReleaseBufferHandle(&f.va, image.buf));
    fake_sync_mode = 1;
    OK(f.vt.vaMapBuffer(&f.va, image.buf, &pixels));
    CHECK(pixels != NULL);
    CHECK(f.vt.vaDestroyImage(&f.va, image.image_id) == VA_STATUS_ERROR_SURFACE_BUSY);
    BAD(f.vt.vaAcquireBufferHandle(&f.va, image.buf, &info));
    CHECK(f.vt.vaBeginPicture(&f.va, f.context, f.surface) == VA_STATUS_ERROR_SURFACE_BUSY);
    OK(f.vt.vaUnmapBuffer(&f.va, image.buf));
    fake_sync_mode = 0;
    OK(f.vt.vaDestroyImage(&f.va, image.image_id));

    /* A client which omits Release/Destroy must not leak the PRIME handle at
     * Terminate, nor free a borrowed surface allocation twice. */
    OK(f.vt.vaDeriveImage(&f.va, f.surface, &image));
    info = (VABufferInfo){ 0 };
    OK(f.vt.vaAcquireBufferHandle(&f.va, image.buf, &info));
    exported_fd = (int)info.handle;
    finish_driver(&f);
    errno = 0;
    CHECK(fcntl(exported_fd, F_GETFD) < 0 && errno == EBADF);
    fake_export_fd = -1;
    CHECK(close(storage) == 0);
    CHECK(fd_count() == initial_fds);
}

static void test_direct_derived_upload(void)
{
    struct fixture f;
    init_driver(&f);
    create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
    struct dmd_driver *drv = dmd_get_driver(&f.va);
    VAImage image;
    OK(f.vt.vaDeriveImage(&f.va, f.surface, &image));
    uint8_t *pixels;
#if VA_CHECK_VERSION(1, 22, 0)
    __typeof__(&dmd_MapBuffer2) map_buffer2 = f.vt.vaMapBuffer2;
#else
    __typeof__(&dmd_MapBuffer2) map_buffer2 = dmd_MapBuffer2;
#endif
    OK(map_buffer2(&f.va, image.buf, (void **)&pixels, VA_MAPBUFFER_FLAG_READ));
    OK(f.vt.vaUnmapBuffer(&f.va, image.buf));
    pthread_mutex_lock(&drv->lock);
    CHECK(dmd_find_surface_locked(drv, f.surface)->state == DMD_SURFACE_IDLE);
    pthread_mutex_unlock(&drv->lock);
    OK(map_buffer2(&f.va, image.buf, (void **)&pixels, VA_MAPBUFFER_FLAG_WRITE));
    for (unsigned y = 0; y < f.height; y++)
        for (unsigned x = 0; x < f.width; x++)
            pixels[image.offsets[0] + y * image.pitches[0] + x] = y_sample(x, y);
    for (unsigned y = 0; y < f.height / 2; y++)
        for (unsigned x = 0; x < f.width; x++)
            pixels[image.offsets[1] + y * image.pitches[1] + x] = uv_sample(x, y);
    CHECK(f.vt.vaBeginPicture(&f.va, f.context, f.surface) == VA_STATUS_ERROR_SURFACE_BUSY);
    /* Nested read does not erase a pending write or commit it prematurely. */
    void *second;
    OK(map_buffer2(&f.va, image.buf, &second, VA_MAPBUFFER_FLAG_READ));
    CHECK(second == pixels);
    OK(f.vt.vaUnmapBuffer(&f.va, image.buf));
    pthread_mutex_lock(&drv->lock);
    CHECK(dmd_find_surface_locked(drv, f.surface)->state == DMD_SURFACE_IDLE);
    pthread_mutex_unlock(&drv->lock);
    OK(f.vt.vaUnmapBuffer(&f.va, image.buf));
    OK(f.vt.vaDestroyImage(&f.va, image.image_id));
    pthread_mutex_lock(&drv->lock);
    CHECK(dmd_find_surface_locked(drv, f.surface)->state == DMD_SURFACE_READY);
    pthread_mutex_unlock(&drv->lock);
    VABufferID coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
    OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
    render_frame(&f, VAProfileH264Main, coded, 1);
    OK(f.vt.vaEndPicture(&f.va, f.context));
    check_pixels(f.width, f.height);
    check_packet(&f, coded, VAProfileH264Main);
    finish_driver(&f);
}

static void test_export_destroy_race(void)
{
    struct fixture f;
    init_driver(&f);
    create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
    int storage = make_exportable_input(&f);
    fake_export_fd = storage;
    fake_export_destroy = &f;
    unsigned before = fd_count();
    VADRMPRIMESurfaceDescriptor desc;
    CHECK(f.vt.vaExportSurfaceHandle(&f.va, f.surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &desc) == VA_STATUS_ERROR_INVALID_SURFACE);
    /* The surface's sync descriptor was closed by destruction. The temporary
     * exported descriptor must also be closed after the failed lookup. */
    CHECK(fd_count() == before - 1);
    fake_export_fd = -1;
    finish_driver(&f);
    CHECK(close(storage) == 0);
}

static void test_decode_hardware_telemetry(void)
{
    struct dmd_video_stats *stats = dmd_video_stats_create(99, 0, DMD_CODEC_AV1,
                                                          VAProfileAV1Profile0, "decode mock");
    CHECK(stats);
    dmd_video_stats_config(stats, 640, 360, 0, 0);
    struct dmd_v4l2_dec dec = { .video_stats = stats, .fd = STDIN_FILENO,
        .cap_ready = 1, .cap_planes = 1, .n_cap = 1 };
    dec.cap[0].map = fake_pixels;
    uint8_t *pixels;
    size_t length;
    uint64_t pts;
    int index;
    char json[4096];
    for (unsigned frame = 1; frame <= 2; ++frame) {
        fake_capture_count = 1;
        fake_capture_bytes = 64;
        fake_capture_flags = 0;
        CHECK(dmd_v4l2_recv(&dec, &pixels, &length, &pts, &index, 0) == 1);
        CHECK(pixels == fake_pixels && length == 64 && index == 0);
        CHECK(!dec.cap[index].hardware_error);
        CHECK(stats_snapshot(99, json) >= 0);
        /* A second AV1 DPB repair output has no surface. Hardware completions
         * must not be mistaken for two successful VA/presentation frames. */
        CHECK(stats_number(json, "hw_completed") == frame);
        CHECK(stats_number(json, "output_frames") == 0);
    }
    fake_capture_count = 1;
    fake_capture_bytes = 0;
    fake_capture_flags = V4L2_BUF_FLAG_LAST;
    CHECK(dmd_v4l2_recv(&dec, &pixels, &length, &pts, &index, 0) == 2);
    CHECK(stats_snapshot(99, json) >= 0 && stats_number(json, "hw_completed") == 2);
    fake_capture_count = 1;
    fake_capture_bytes = 64;
    fake_capture_flags = V4L2_BUF_FLAG_ERROR;
    CHECK(dmd_v4l2_recv(&dec, &pixels, &length, &pts, &index, 0) == 1);
    CHECK(dec.cap[index].hardware_error);
    /* The session carries this slot flag into dmd_frame even if the frame
     * first waited in send_unit's pending queue. Surface delivery policy must
     * not count it as success or clear the error state. */
    dmd_video_stats_decoded(stats, 640, 360, dec.cap[index].hardware_error);
    CHECK(stats_snapshot(99, json) >= 0);
    CHECK(stats_number(json, "hw_completed") == 2 && stats_number(json, "errors") == 1);
    CHECK(stats_number(json, "output_frames") == 0 && strstr(json, "\"state\":\"error\""));
    dmd_video_stats_reuse(stats);
    CHECK(stats_snapshot(99, json) >= 0 && stats_number(json, "reused_frames") == 1);
    CHECK(stats_number(json, "output_frames") == 0 && stats_number(json, "hw_completed") == 2);
    dmd_video_stats_close(stats);
    dmd_video_stats_unref(stats);
}

static void test_telemetry_disabled(void)
{
    CHECK(setenv("DMD_VIDEO_STATS", "0", 1) == 0);
    struct fixture f;
    init_driver(&f);
    create_encode(&f, VAProfileH264Main, 128, 96, VA_RC_CBR);
    VAImage image = upload_pattern(&f);
    VABufferID coded = make_buffer(&f, VAEncCodedBufferType, 4096, NULL);
    char stats[4096];
    CHECK(stats_snapshot(f.context, stats) < 0);
    OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
    render_frame(&f, VAProfileH264Main, coded, 1);
    OK(f.vt.vaEndPicture(&f.va, f.context));
    check_packet(&f, coded, VAProfileH264Main);
    OK(f.vt.vaDestroyImage(&f.va, image.image_id));
    finish_driver(&f);
    CHECK(unsetenv("DMD_VIDEO_STATS") == 0);
}

/* Exercise the real decode frontend before any compressed payload is sent.
 * A profile gate alone must never allow mixed NV12/P010 bytes or 9/12-bit
 * syntax to enter a negotiated fixed-depth context. No HW frame is claimed. */
static void check_hevc_picture_format(VAProfile profile, unsigned surface_format,
                                      unsigned luma, unsigned chroma,
                                      unsigned chroma_format, unsigned separate,
                                      VAStatus expected)
{
    struct fixture f;
    init_driver(&f);
    const int ten_bit = profile == VAProfileHEVCMain10;
    /* Chrome's Main10 capability/config request is RT8 even when the actual
     * ten-bit picture targets P010. It must not fix the frame's bit depth. */
    VAConfigAttrib attribute = { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420 };
    OK(f.vt.vaCreateConfig(&f.va, profile, VAEntrypointVLD, &attribute, 1, &f.config));
    OK(f.vt.vaCreateSurfaces2(&f.va, surface_format, 128, 96,
                              &f.surface, 1, NULL, 0));
    OK(f.vt.vaCreateContext(&f.va, f.config, 128, 96, VA_PROGRESSIVE,
                            &f.surface, 1, &f.context));
    struct dmd_driver *driver = dmd_get_driver(&f.va);
    pthread_mutex_lock(&driver->lock);
    struct dmd_context *context = dmd_find_context_locked(driver, f.context);
    CHECK(context && context->output_ten_bit == ten_bit && !context->hevc_pic_param_valid);
    pthread_mutex_unlock(&driver->lock);
    OK(f.vt.vaBeginPicture(&f.va, f.context, f.surface));
    VAPictureParameterBufferHEVC picture = { 0 };
    picture.pic_width_in_luma_samples = 128;
    picture.pic_height_in_luma_samples = 96;
    picture.bit_depth_luma_minus8 = luma;
    picture.bit_depth_chroma_minus8 = chroma;
    picture.pic_fields.bits.chroma_format_idc = chroma_format;
    picture.pic_fields.bits.separate_colour_plane_flag = separate;
    VABufferID buffer = make_buffer(&f, VAPictureParameterBufferType, sizeof(picture), &picture);
    CHECK(f.vt.vaRenderPicture(&f.va, f.context, &buffer, 1) == expected);
    pthread_mutex_lock(&driver->lock);
    context = dmd_find_context_locked(driver, f.context);
    CHECK(context);
    if (expected == VA_STATUS_SUCCESS) {
        CHECK(context->hevc_pic_param_valid && context->output_ten_bit == (luma == 2));
        CHECK(context->hevc_pic_param.bit_depth_luma_minus8 == luma);
    } else {
        CHECK(!context->hevc_pic_param_valid && context->output_ten_bit == ten_bit);
    }
    pthread_mutex_unlock(&driver->lock);
    OK(f.vt.vaDestroyBuffer(&f.va, buffer));
    OK(f.vt.vaDestroyContext(&f.va, f.context));
    OK(f.vt.vaDestroySurfaces(&f.va, &f.surface, 1));
    OK(f.vt.vaDestroyConfig(&f.va, f.config));
    finish_driver(&f);
}

static void test_hevc_picture_depth_guards(void)
{
    check_hevc_picture_format(VAProfileHEVCMain, VA_RT_FORMAT_YUV420,
                              0, 0, 1, 0, VA_STATUS_SUCCESS);
    check_hevc_picture_format(VAProfileHEVCMain, VA_RT_FORMAT_YUV420,
                              2, 2, 1, 0, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    check_hevc_picture_format(VAProfileHEVCMain, VA_RT_FORMAT_YUV420_10,
                              0, 0, 1, 0, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
#ifdef DMD_ENABLE_HEVC_MAIN10
    check_hevc_picture_format(VAProfileHEVCMain10, VA_RT_FORMAT_YUV420,
                              0, 0, 1, 0, VA_STATUS_SUCCESS);
    check_hevc_picture_format(VAProfileHEVCMain10, VA_RT_FORMAT_YUV420_10,
                              2, 2, 1, 0, VA_STATUS_SUCCESS);
    check_hevc_picture_format(VAProfileHEVCMain10, VA_RT_FORMAT_YUV420,
                              2, 2, 1, 0, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    check_hevc_picture_format(VAProfileHEVCMain10, VA_RT_FORMAT_YUV420_10,
                              0, 0, 1, 0, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    check_hevc_picture_format(VAProfileHEVCMain10, VA_RT_FORMAT_YUV420_10,
                              2, 0, 1, 0, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    check_hevc_picture_format(VAProfileHEVCMain10, VA_RT_FORMAT_YUV420_10,
                              1, 1, 1, 0, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    check_hevc_picture_format(VAProfileHEVCMain10, VA_RT_FORMAT_YUV420_10,
                              4, 4, 1, 0, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    check_hevc_picture_format(VAProfileHEVCMain10, VA_RT_FORMAT_YUV420_10,
                              2, 2, 2, 0, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    check_hevc_picture_format(VAProfileHEVCMain10, VA_RT_FORMAT_YUV420_10,
                              2, 2, 1, 1, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
#endif
}

#ifdef DMD_ENABLE_HEVC_MAIN10
static void test_hevc_main10_depth_is_fixed(void)
{
    for (unsigned first_ten = 0; first_ten < 2; first_ten++) {
        struct fixture f;
        init_driver(&f);
        VAConfigAttrib attribute = { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420 };
        OK(f.vt.vaCreateConfig(&f.va, VAProfileHEVCMain10, VAEntrypointVLD,
                               &attribute, 1, &f.config));
        VASurfaceID surfaces[2];
        OK(f.vt.vaCreateSurfaces2(&f.va, VA_RT_FORMAT_YUV420, 128, 96,
                                  &surfaces[0], 1, NULL, 0));
        OK(f.vt.vaCreateSurfaces2(&f.va, VA_RT_FORMAT_YUV420_10, 128, 96,
                                  &surfaces[1], 1, NULL, 0));
        /* A render-target list is an allocation pool, not a depth selection. */
        OK(f.vt.vaCreateContext(&f.va, f.config, 128, 96, VA_PROGRESSIVE,
                                surfaces, 2, &f.context));
        VAPictureParameterBufferHEVC picture = { 0 };
        picture.pic_width_in_luma_samples = 128;
        picture.pic_height_in_luma_samples = 96;
        picture.pic_fields.bits.chroma_format_idc = 1;
        for (unsigned step = 0; step < 3; step++) {
            unsigned depth = step == 1 ? !first_ten : first_ten;
            OK(f.vt.vaBeginPicture(&f.va, f.context, surfaces[depth]));
            picture.bit_depth_luma_minus8 = picture.bit_depth_chroma_minus8 = depth ? 2 : 0;
            VABufferID buffer = make_buffer(&f, VAPictureParameterBufferType,
                                            sizeof(picture), &picture);
            VAStatus expected = step == 1 ? VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT : VA_STATUS_SUCCESS;
            CHECK(f.vt.vaRenderPicture(&f.va, f.context, &buffer, 1) == expected);
            OK(f.vt.vaDestroyBuffer(&f.va, buffer));
            struct dmd_driver *driver = dmd_get_driver(&f.va);
            pthread_mutex_lock(&driver->lock);
            struct dmd_context *context = dmd_find_context_locked(driver, f.context);
            CHECK(context && context->hevc_pic_param_valid &&
                  context->output_ten_bit == (int)first_ten && !context->units_submitted);
            CHECK(context->hevc_pic_param.bit_depth_luma_minus8 == (first_ten ? 2u : 0u));
            pthread_mutex_unlock(&driver->lock);
            /* No compressed payload: finish the test transaction without
             * claiming that a hardware frame was produced. The depth lock must
             * survive errors and the next BeginPicture. */
            CHECK(f.vt.vaEndPicture(&f.va, f.context) ==
                  (step == 1 ? VA_STATUS_ERROR_INVALID_CONTEXT : VA_STATUS_ERROR_UNIMPLEMENTED));
        }
        OK(f.vt.vaDestroyContext(&f.va, f.context));
        OK(f.vt.vaDestroySurfaces(&f.va, surfaces, 2));
        OK(f.vt.vaDestroyConfig(&f.va, f.config));
        finish_driver(&f);
    }
}
#endif

int main(void)
{
    CHECK(unsetenv("DMD_VIDEO_STATS") == 0);
    test_discovery();
    test_encode_profiles();
    test_upload_geometry();
    test_backend_failures();
    test_rejected_parameters();
    test_decoded_surface_visible_geometry();
    test_concurrent_destruction();
    test_prime_import();
    test_prime_import_rejects();
    test_prime_size_discovery();
    test_concurrent_import_destruction();
    test_prime_sync_failure();
    test_image_only_surface_dimensions();
    test_codec_context_minimums();
    test_write_export_encoder_input();
    test_legacy_image_prime_interop();
    test_direct_derived_upload();
    test_export_destroy_race();
    test_decode_hardware_telemetry();
    test_hevc_picture_depth_guards();
#ifdef DMD_ENABLE_HEVC_MAIN10
    test_hevc_main10_depth_is_fixed();
#endif
    test_telemetry_disabled();
    CHECK(fake_live == 0);
    CHECK(fake_open_calls == fake_close_calls);
    puts("encode VA-API contract tests passed");
    return 0;
}
