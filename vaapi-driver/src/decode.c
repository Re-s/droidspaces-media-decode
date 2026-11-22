/* Decode data path for VA-API surfaces, contexts and buffers.
 *
 * VA-API submits pictures to specific surfaces and leaves presentation order
 * to the client. Stateful V4L2 produces frames without surface identities.
 * The context's pending queue pairs CAPTURE output with submitted targets.
 *
 * For codecs with one output per submitted unit, the queue can pair frames
 * in order. H.264 and HEVC parameter sets yield no frame and do not add a
 * target. Later code handles cases such as AV1 display and repair units.
 *
 * Release the driver lock for blocking I/O. io_busy serializes I/O within a
 * context while other threads can continue creating and preparing buffers.
 */

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <sys/stat.h>

/* Surface is assigned to dbbuffer in msm_drm to export dmabuf
 * (Firefox's only way to take the frame, see export.c.) */
#include <drm/drm.h>

#include "driver.h"
#include "av1_bitstream.h"
#include "mpeg2_bitstream.h"
#include "encode.h"
#include "hevc_slice.h"

static void dumb_sync_end_write(struct dmd_surface *s);
static void encode_output_error_locked(struct dmd_driver *drv,
                                       struct dmd_context *c, VAStatus status);

/* Pre-declaration: DestroyContext needs to be emptied of flying frames, and these two auxiliary definitions are below
 * Decoding path section (placed closer to the site). */
static VASurfaceID dmd_pending_take_locked(struct dmd_context *c,
                                           uint32_t unit_seq);

/* Serial number tracking switch: DMD_TRACE_ORDER=1 open. Only affects logs and does not change behavior. */
static int dmd_trace_order(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("DMD_TRACE_ORDER");
        cached = (e && *e && *e != '0') ? 1 : 0;
    }
    return cached;
}
static void surface_store_frame_locked(struct dmd_context *c, struct dmd_surface *s,
                                       const struct dmd_frame *f);

unsigned int dmd_align_up(unsigned int v, unsigned int align)
{
    if (align == 0)
        return v;
    return (v + align - 1) / align * align;
}

/* Object lookup */

struct dmd_surface *dmd_find_surface_locked(struct dmd_driver *drv,
                                            VASurfaceID id)
{
    if (id == VA_INVALID_ID)
        return NULL;
    for (int i = 0; i < DMD_MAX_SURFACE_OBJECTS; i++) {
        if (drv->surfaces[i].in_use && drv->surfaces[i].id == id)
            return &drv->surfaces[i];
    }
    return NULL;
}

struct dmd_context *dmd_find_context_locked(struct dmd_driver *drv,
                                            VAContextID id)
{
    if (id == VA_INVALID_ID)
        return NULL;
    for (int i = 0; i < DMD_MAX_CONTEXTS; i++) {
        if (drv->contexts[i].in_use && drv->contexts[i].id == id)
            return &drv->contexts[i];
    }
    return NULL;
}

struct dmd_buffer *dmd_find_buffer_locked(struct dmd_driver *drv, VABufferID id)
{
    if (id == VA_INVALID_ID)
        return NULL;
    for (int i = 0; i < DMD_MAX_BUFFERS; i++) {
        if (drv->buffers[i].in_use && drv->buffers[i].id == id)
            return &drv->buffers[i];
    }
    return NULL;
}

void dmd_surface_reset_locked(struct dmd_surface *s)
{
    dmd_import_unref(s->imported);
    if (s->exportable) {
        /* When you want to use the dmabuf fd to synchronize, you turn off: it has a quote for the buffer,
         * Keeping it will keep the memory of DESTROY_DUMB unreleased.
         * Note that this is not the fd that was handed to the vExportSurfaceHandle caller. */
        if (s->dumb_sync_fd >= 0)
            close(s->dumb_sync_fd);
        /* Domb buffer: munmap first and then destroy handle in a non-reverse order. */
        if (s->data)
            munmap(s->data, s->dumb_size);
        if (s->dumb_handle && s->dumb_drm_fd >= 0) {
            struct drm_mode_destroy_dumb dreq;
            memset(&dreq, 0, sizeof(dreq));
            dreq.handle = s->dumb_handle;
            ioctl(s->dumb_drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        }
    } else {
        free(s->data);
    }
    memset(s, 0, sizeof(*s));
    s->id = VA_INVALID_ID;
    s->context = VA_INVALID_ID;
}

void dmd_context_reset_locked(struct dmd_context *c)
{
    dmd_video_stats_close(c->video_stats);
    dmd_video_stats_unref(c->video_stats);
    c->video_stats = NULL;
    if (c->encoder)
        dmd_encode_destroy(c->encoder);
    /* The session_destroy will close the lock. It doesn't block the screen.
     * So do it here safe.
     * ⁇  It ends with memset the whole context, and the buffer must be released before it**. */
    free(c->av1_repair_buf);
    free(c->av1_next_repair_buf);
    c->av1_repair_buf = c->av1_next_repair_buf = NULL;
    if (c->session)
        dmd_session_destroy(c->session);
    free(c->slice_data);
    memset(c, 0, sizeof(*c));
    c->av1_dpb.last_refresh_bitpos = (size_t)-1;
    c->av1_dpb.last_endupd_bitpos = (size_t)-1;
    c->id = VA_INVALID_ID;
    c->current_target = VA_INVALID_ID;
}

/* I/O ownership and session teardown
 *
 * A thread grabs the session pointer under the driver lock, drops the lock
 * for I/O, then reacquires it to store or release the frame. Keep io_busy
 * set until the last use of the pointer, including those final operations.
 *
 * DestroyContext and session rebuild use io_busy to decide whether they can
 * close the V4L2 fd and unmap CAPTURE buffers. Clearing it early let a thread
 * issue QBUF or DQBUF on a closed or recycled fd. Chrome observed an FD
 * ownership violation and its GPU process exited with code 5.
 *
 * session_retire_locked defers teardown while I/O is active. The thread
 * returning I/O ownership through io_release can complete the teardown.
 */

/* IO title to this slot: the previous user is returned with a lock and then taken over.
 * returns 0 = already obtained; 1 = waiting to be over-timed, callers** must give up this IO** and must not be hard-held.
 *
 * Why not "Io_busy=1 without waiting": that's covering the title tag that someone else is using.
 * The one who returned it will take it out of the way, so the person who broke the session sees it, close -- both lines.
 * QBUF/DQBUF for the same V4L2 fd, is ENOTTY/ FD ownership operation.
 *
 * ⁇  The lock is placed during the period of this function waiting, so ** Reconfirmation of slots is required after return**
 * (context_slot_ok) and ** After that, ** caught c->session.
 * "Hold the session pointer, wait for the old pointer to be IO."
 * The session can be torn down by others, that's use-after-free. */
static int io_acquire(struct dmd_driver *drv, int idx, const char *who)
{
    int waited = 0;
    while (drv->io_busy[idx]) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&drv->io_done, &drv->lock, &ts);
        waited += 20;
        if (waited >= DMD_FRAME_TIMEOUT_MS) {
            dmd_log("%s: timed out acquiring I/O ownership (slot %d held for %d ms). Skipping this operation\n",
                    who, idx, waited);
            return -1;
        }
    }
    drv->io_busy[idx] = 1;
    return 0;
}

/* We'll wait until we know this slot is still our context.
 * Drv->contents are a long array, the pointers don't fail, but the contents will be next.
 * CreateContext for someone -- less than a pointer, more than ID with in_use. */
static int context_slot_ok(struct dmd_context *c, VAContextID id)
{
    return c && c->in_use && c->id == id;
}

/* Hand over the IO title in this slot; if the meeting is hung up and so forth, then destroy it.
 * returns 1 = Sessions in this slot have been destroyed and caller** should not ** use the old session pointer.
 * Caller holds the lock. */
static int io_release(struct dmd_driver *drv, int idx)
{
    drv->io_busy[idx] = 0;
    pthread_cond_broadcast(&drv->io_done);

    if (drv->encode_defer[idx]) {
        struct dmd_encode *e = drv->encode_defer[idx];
        drv->encode_defer[idx] = NULL;
        dmd_encode_destroy(e);
    }

    struct dmd_session *z = drv->io_defer[idx];
    if (!z) return 0;
    drv->io_defer[idx] = NULL;
    dmd_session_destroy(z);
    return 1;
}

/* Request to destroy a session. The caller holds the lock and must have ** removed it from context and placed it
 * c->retiring, otherwise Io_busy will be recapturing this session pending demolition before returning it. */
static void session_retire_locked(struct dmd_driver *drv, int idx,
                                 struct dmd_session *s)
{
    if (!s) return;
    if (!drv->io_busy[idx]) {
        dmd_session_destroy(s);
        return;
    }
    if (drv->io_defer[idx]) {
        /* Coming here means having two unused sessions on the same slot -- just in the context of the above.
         * It shouldn't have happened. I'd rather not have a conversation with someone else. */
        dmd_log("io_defer[%d] is occupied. Keeping the old session alive to avoid destroying the wrong one\n",
                idx);
        return;
    }
    drv->io_defer[idx] = s;
}

/* Background frame receiver for Chrome
 *
 * Why must it be: Chrome never tunes vaSyncSurface (0 times in fact, Firefox is 1500 times),
 * Nor do you want to change QuerySurfaceStatus. The only chance to drive is EndPicture.
 * EndPicture is played twice at slow speed, approximately 2 seconds apart, while hardware early-dismantling frames are pressed at CAPTURE
 * Nobody in the queue takes -- the dmabuf side of Chrome is empty or the last round of pixels.
 *
 * This thread is only one thing: press the frame ready to press unit_seq pair into the corresponding surface.
 * The alignment logic is fully consistent with EndPicture/SyncSurface (dmd_pending_take_locked).
 * So it doesn't change the semantics of any "what frame belongs to which surface."
 */
static void *dmd_reaper_thread(void *arg)
{
    struct dmd_driver *drv = (struct dmd_driver *)arg;

    for (;;) {
        pthread_mutex_lock(&drv->lock);
        if (drv->reaper_stop) {
            pthread_mutex_unlock(&drv->lock);
            break;
        }

        int worked = 0;
        for (int i = 0; i < DMD_MAX_CONTEXTS; i++) {
            struct dmd_context *c = &drv->contexts[i];
            if (!c->in_use || !c->session || drv->io_busy[i] || c->retiring)
                continue;
            if (c->pending_count <= 0)
                continue;

            /* The pending list tracks input, not ready CAPTURE buffers.
             * A previous queue-only receiver missed output still in V4L2,
             * so always try the backend as well as the retained-frame queue.
             *
             * The old 20 ms receive also paced retries, but io_busy excludes
             * the input that may be needed to release reordered output.
             * Keep this receive nonblocking and pace retries outside I/O.
             * Retain the thread for output arriving after the last send. */
            VAContextID cid = c->id;
            struct dmd_session *sess = c->session;
            drv->io_busy[i] = 1;
            pthread_mutex_unlock(&drv->lock);

            struct dmd_frame f;
            memset(&f, 0, sizeof(f));
            int rc = dmd_session_try_next_frame(sess, &f);

            pthread_mutex_lock(&drv->lock);

            /* Io_busy won't be returned until the last time we touched it.
             * Domd_session_release_frame with the finger that came before the lock.
             * "There's someone else who's using this session to "no one."
             * There, DestroyContext/ Session Rebuilding goes close to its fd. */
            c = dmd_find_context_locked(drv, cid);
            if (c && rc == DMD_OK) {
                VASurfaceID id = dmd_pending_take_locked(c, f.unit_seq);
                struct dmd_surface *s = dmd_find_surface_locked(drv, id);
                if (s) {
                    surface_store_frame_locked(c, s, &f);
                    s->state = DMD_SURFACE_READY;
                    c->frames_out++;
                    worked = 1;
                    if (dmd_trace_order())
                        dmd_log("ORDER reap   surf=%u unit_seq=%u pend=%d\n",
                                (unsigned)id, f.unit_seq, c->pending_count);
                }
            }
            if (rc == DMD_OK && dmd_session_release_frame(sess, &f) != DMD_OK)
                dmd_log("background release failed: context=%u unit=%u: %s\n",
                        (unsigned)cid, f.unit_seq, dmd_session_last_error(sess));
            if (rc != DMD_OK && rc != DMD_ERR_TIMEOUT && rc != DMD_EOS)
                dmd_log("background receive failed: context=%u: %s\n",
                        (unsigned)cid, dmd_session_last_error(sess));
            io_release(drv, i);
        }
        pthread_mutex_unlock(&drv->lock);

        /* The old bounded receive supplied pacing while holding session I/O.
         * This existing no-work sleep now supplies pacing outside ownership.
         * Empty queues already skipped receive and used this sleep before.
         * Pending input without ready output can now cause more wakeups;
         * improved playback here does not establish power neutrality. */
        if (!worked)
            usleep(2000);
    }

    return NULL;
}

void dmd_reaper_start_locked(struct dmd_driver *drv)
{
    if (drv->reaper_started)
        return;
    drv->reaper_stop = 0;
    if (pthread_create(&drv->reaper, NULL, dmd_reaper_thread, drv) != 0) {
        dmd_log("Could not start the background frame receiver. Receiving frames only in EndPicture\n");
        return;
    }
    drv->reaper_started = 1;
    dmd_log("Background frame receiver started\n");
}

void dmd_reaper_stop(struct dmd_driver *drv)
{
    pthread_mutex_lock(&drv->lock);
    if (!drv->reaper_started) {
        pthread_mutex_unlock(&drv->lock);
        return;
    }
    drv->reaper_stop = 1;
    pthread_t t = drv->reaper;
    drv->reaper_started = 0;
    pthread_mutex_unlock(&drv->lock);
    pthread_join(t, NULL);
    dmd_log("Background frame receiver stopped\n");
}

/* ================================ surface ================================ */

/* Distribute a NV12 buffer for the display size.
 *
 * Geometry: Wide alignment 128, high alignment 32 (Venus' real machine behavior, 1080p  ⁇  1920x1088).
 * You can prep the alignment, so the frame that Damon came back will be loaded -- or you'll have to wait until you get it.
 * Format blocks will know how big the allocation is, and ffmpeg will need a surface before building context.
 */
/* To allocate a piece** to dmm_drm to dumb buffer, which can be exported to dmabuf**.
 *
 * Why do you need: Firefox takes the frame away from the DRM_PRIME_2?
 * Failures allow the whole stream to fall and resolve, and ** no copy of the retreat path**
 * （FFmpegVideoDecoder.cpp:1632 GetVAAPISurfaceDescriptor）。
 * So the frame must be in the memory that will export the frd.
 *
 * Note that this is not a zero copy: MediaCodec, we still don't get the memory.
 * (Container ION is not available, none /dev/dma_heap, NDK does not provide an output buffer fd).
 * Here's the file that you can export memory and let the frame of the re-transmission go straight in...
 * The number of copies is the same as the original calloc scheme, but there is more "combinable" ability.
 *
 * Failed to return -1, the caller returns to normal header (hwdownload path of ffmpeg does not need to export).
 */
static int surface_alloc_dumb(struct dmd_surface *s, int drm_fd,
                              unsigned int stride, unsigned int bh,
                              unsigned int sample_bytes, size_t size)
{
    if (drm_fd < 0)
        return -1;

    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    /* The parameter is width, but the kernel is based on the width*bpp/8 line bytes, and here bpp=8,
     * So direct bytes** is the desired number of bytes.
     * (P010 line is sample width x 2 bytes) and the entire buffer is still applied for on 8bpp single plane,
     * Y/UV is to be pushed by ourselves to cut. */
    creq.width  = stride;
    /* The number of rows = bh of Y plus bh/2 of UV. */
    creq.height = bh * 3 / 2;
    creq.bpp    = 8;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0)
        return -1;

    /* Pitch must be exactly the stride we want, otherwise the geometry doesn't match the frame of the daemon. */
    if (creq.pitch != stride || creq.size < size) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = creq.handle;
        ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        dmd_log("Dumb buffer geometry mismatch: pitch %u (expected %u), size %llu (required %zu)\n",
                creq.pitch, stride, (unsigned long long)creq.size, size);
        return -1;
    }

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) != 0) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = creq.handle;
        ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }

    void *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     drm_fd, (off_t)mreq.offset);
    if (map == MAP_FAILED) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = creq.handle;
        ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }
    /* It's starting to be legal "pure black" NV12, not a whole piece of zero.
     *
     * UV=0 in NV12 is not colorless, but maximum color bias: BT.601 turns RGB gets
     * R ⁇ 0, G ⁇ 135, B ⁇ 0 -- that's the sign of pure greenness. The neutral value is 128.
     *
     * Why does this make sense: Firefox will be able to decode the ** ** vExportSurfaceHandle
     * Take fd to build texture (actually every surface export at Y=0, UV=0), and then buffer in
     * This is what you fill out. The whole piece of zero will allow it to be sampled first into a single green frame; the fillup UV=128 is pure black.
     * It's the same as the video before it starts. It doesn't flash green.
     *
     * Actual (1280x720,230 export): 3 findings from the first export of 6 surface before the change
     * Pure green; all normal exports were followed 781 times (UV average approximately 123.6).
     *
     * 10bit (P010) cannot follow the same memset: sample is 16-bit applet, indent 0x80
     * The neutral is 512 < 6 = 0 x 8000,
     * The lower byte is 0x00x80. Y plane (black = 0) is two places in line. */
    memset(map, 0, (size_t)stride * bh);                    /* Y plane: black */
    {
        unsigned char *uv = (unsigned char *)map + (size_t)stride * bh;
        size_t uv_bytes = creq.size - (size_t)stride * bh;
        if (sample_bytes == 2) {
            /* P010: neutral chroma 512 occupies the high ten bits. */
            for (size_t k = 0; k + 1 < uv_bytes; k += 2) {
                uv[k] = 0x00;
                uv[k + 1] = 0x80;
            }
        } else {
            memset(uv, 0x80, uv_bytes);                    /* NV12: Colorless */
        }
    }

    s->dumb_handle = creq.handle;
    s->dumb_size = creq.size;
    s->dumb_drm_fd = drm_fd;
    s->data = (unsigned char *)map;
    s->data_size = creq.size;
    s->exportable = 1;

    /* Permanent dmabuf fd for DMA_BUF_IOCTL_SYNC for cache maintenance (see driver.h)
     * dumb_sync_fd comment. Failure does not affect distribution success: only GPU zero copy consumer may
     * The CPU reading path is not affected when the data are outdated. */
    s->dumb_sync_fd = -1;
    {
        struct drm_prime_handle ph;
        memset(&ph, 0, sizeof(ph));
        ph.handle = creq.handle;
        ph.flags = DRM_CLOEXEC;
        if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0)
            s->dumb_sync_fd = ph.fd;
        else
            dmd_log("Warning: could not obtain a DMA-BUF fd for surface synchronization (%s). "
                    "Zero-copy GPU consumers may read stale data\n", strerror(errno));
    }

    /* The zero above memset (map, 0, ...) is also written by CPU and has to be brushed out.
     * Otherwise, GPU sampled ** uninitiated physical memory** before the frame was removed
     * (Possibly the remnants of the last process) not the black/green image we thought was.
     * The fd just came up, just one time ED -- nothing happened before the fd.
     * The full package of START/ END is not available, but the clear semantic remains valid. */
    dumb_sync_end_write(s);
    return 0;
}

/* CPU access to dumb buffer must be coupled with DMA_BUF_IOCTL_SYNC.
 *
 * The map from msync: dumb buffer is VM_PFNMAP/ VM_IO, without page cache
 * Replayable, msync returns directly to EINVAL (substantial confirmation).
 *
 * The START must be paired with the END. The linux/dma-buf.h comment is command-based:
 * "Prior to accessing the map, the client **must** call DMA_BUF_IOCTL_SYNC
 *  with DMA_BUF_SYNC_START ... Once the access is complete, the client
 *  should call ... with DMA_BUF_SYNC_END and **the same** read/write flags."
 *
 * Once END tried to save octl (because "CPU writing, device reading, no need to inventate."
 * That's wrong: the core manages the cache state machine by pressing START/END pairs, isolated END is not guaranteed
 * Trigger clear. The measurements show a rotation of the green screen with the normal image and with local colour blocks**--
 * Some of the frames were painted out, some were missing, and it was the lack of matching that led to the time-series competition.
 * It's harder to judge than a completely different step.
 *
 * The two functions correspond to the beginning and end of the access, and the caller must wrap the entire session. */
static void dumb_sync_begin_write(struct dmd_surface *s)
{
    if (!s || s->dumb_sync_fd < 0)
        return;
    struct dma_buf_sync sync;
    memset(&sync, 0, sizeof(sync));
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
    if (ioctl(s->dumb_sync_fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        static int warned;
        if (!warned) {
            warned = 1;
            dmd_log("Warning: DMA_BUF_SYNC_START failed (%s)\n", strerror(errno));
        }
    }
}

static void dumb_sync_end_write(struct dmd_surface *s)
{
    if (!s || s->dumb_sync_fd < 0)
        return;
    struct dma_buf_sync sync;
    memset(&sync, 0, sizeof(sync));
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    if (ioctl(s->dumb_sync_fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        static int warned;
        if (!warned) {
            warned = 1;
            dmd_log("Warning: DMA_BUF_SYNC_END failed (%s). Zero-copy GPU consumers "
                    "may read stale data, producing green frames\n", strerror(errno));
        }
    }
}

static VAStatus surface_alloc_locked(struct dmd_surface *s, VASurfaceID id,
                                     unsigned int width, unsigned int height,
                                     unsigned int format, int drm_fd)
{
    /* Bytes per sample: NV12=1, P010=2.
     * 10bit is differentiated by RTFormat only (VA did not send sourcecc into v1
     * PixelFormat of v2 in new_surfaces_common
     * RTFormat) is uniform, so all of the following geometry is **bytes** in units:
     * Stride = line bytes = sample width after alignment x bpp.
     * The benefits are Y/UV cut, size=sride*slick*3/2, flat frame memcpy
     * All of them no more branches -- actual Venus for 640x384 P010.
     * stride=1280 sizeimage=737280(=1280*384*3/2)。 */
    const unsigned int bpp = (format == VA_RT_FORMAT_YUV420_10) ? 2u : 1u;
    unsigned int bw = dmd_align_up(width, DMD_WIDTH_ALIGN);
    unsigned int bh = dmd_align_up(height, DMD_HEIGHT_ALIGN);
    unsigned int stride = bw * bpp;
    /* Mono-Plus layout: Y takes the stride*slick, UV is half of it. */
    size_t size = (size_t)stride * bh * 3 / 2;

    memset(s, 0, sizeof(*s));

    /* Priority is given to exportable dumb buffer; return to normal heap if not supported. */
    if (surface_alloc_dumb(s, drm_fd, stride, bh, bpp, size) != 0) {
        unsigned char *data = calloc(1, size);
        if (!data)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        s->data = data;
        s->data_size = size;
        s->exportable = 0;
        s->dumb_handle = 0;
    }

    s->in_use = 1;
    s->id = id;
    s->width = width;
    s->height = height;
    s->visible_width = width;
    s->visible_height = height;
    s->buf_width = bw;
    s->buf_height = bh;
    s->stride = stride;
    s->slice_height = bh;
    s->format = format;
    s->state = DMD_SURFACE_IDLE;
    s->decode_status = VA_STATUS_SUCCESS;
    s->context = VA_INVALID_ID;
    return VA_STATUS_SUCCESS;
}

static VAStatus create_surfaces_common(VADriverContextP ctx,
                                       unsigned int format, unsigned int width,
                                       unsigned int height,
                                       VASurfaceID *surfaces,
                                       unsigned int num_surfaces)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !surfaces)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_surfaces == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    /* Surfaces are also standalone images: mpv tests upload/export with a
     * real 16x16 NV12/P010 surface before accepting decoder frames. The codec
     * minimum belongs to CreateContext, which still enforces DMD_MIN_*.
     * Keep the storage limit and require complete 4:2:0 chroma samples. */
    if (!width || !height || ((width | height) & 1) ||
        width > DMD_MAX_WIDTH || height > DMD_MAX_HEIGHT)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    /* 8-bit NV12 with 10-bit P010.
     * So the real threshold is in the profiles.c: ** Only AV1 profile.
     * VA_RT_FORMAT_YUV420_10** (V4L2 backend only at PPB bit_depth_idx! =0)
     * The captour is P010.
     * The codec is judged, but the double-distribution buffer is not in itself wrong -- it's not the 10bit frame. */
    if (format != VA_RT_FORMAT_YUV420 && format != VA_RT_FORMAT_YUV420_10)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    VAStatus status = VA_STATUS_SUCCESS;
    unsigned int created = 0;

    pthread_mutex_lock(&drv->lock);
    for (unsigned int n = 0; n < num_surfaces; n++) {
        struct dmd_surface *slot = NULL;
        for (int i = 0; i < DMD_MAX_SURFACE_OBJECTS; i++) {
            if (!drv->surfaces[i].in_use) {
                slot = &drv->surfaces[i];
                break;
            }
        }
        if (!slot) {
            status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            break;
        }

        VASurfaceID id = (VASurfaceID)(drv->next_surface_id++);
        status = surface_alloc_locked(slot, id, width, height, format,
                                      drv->drm_fd);
        if (status != VA_STATUS_SUCCESS)
            break;

        surfaces[n] = id;
        created++;
    }

    /* Partial failure to roll: VA-API syntax is complete or total, leaving half a array to allow
     * Consumers use uninitiated IDs as legal surface. */
    if (status != VA_STATUS_SUCCESS) {
        for (unsigned int n = 0; n < created; n++) {
            struct dmd_surface *s = dmd_find_surface_locked(drv, surfaces[n]);
            if (s)
                dmd_surface_reset_locked(s);
            surfaces[n] = VA_INVALID_ID;
        }
    }
    pthread_mutex_unlock(&drv->lock);

    if (status == VA_STATUS_SUCCESS)
        dmd_log("CreateSurfaces: %ux%u format=0x%x n=%u -> id %u..\n", width,
                height, format, num_surfaces, (unsigned)surfaces[0]);
    else
        dmd_log("CreateSurfaces: failed with status 0x%x (rolled back %u created surfaces)\n",
                status, created);

    return status;
}

VAStatus dmd_CreateSurfaces(VADriverContextP ctx, int width, int height,
                            int format, int num_surfaces, VASurfaceID *surfaces)
{
    if (width <= 0 || height <= 0 || num_surfaces <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    return create_surfaces_common(ctx, (unsigned int)format,
                                  (unsigned int)width, (unsigned int)height,
                                  surfaces, (unsigned int)num_surfaces);
}

static VAStatus create_imported_surfaces(VADriverContextP ctx, unsigned format,
                                         unsigned width, unsigned height,
                                         VASurfaceID *surfaces, unsigned count,
                                         unsigned memory_type, const void *descriptors)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv || !surfaces || !descriptors || !count || count > DMD_MAX_SURFACES)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (format != VA_RT_FORMAT_YUV420)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (width < DMD_MIN_WIDTH || height < DMD_MIN_HEIGHT ||
        width > DMD_MAX_WIDTH || height > DMD_MAX_HEIGHT || ((width | height) & 1))
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    /* PRIME v1 has one shared ExternalBuffers descriptor and a buffer array;
     * only the single-buffer form used by Chromium is supported here. */
    if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME && count != 1)
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    for (unsigned i = 0; i < count; i++) surfaces[i] = VA_INVALID_ID;
    pthread_mutex_lock(&drv->lock);
    VAStatus status = VA_STATUS_SUCCESS;
    unsigned made = 0;
    for (; made < count; made++) {
        struct dmd_surface *slot = NULL;
        for (unsigned i = 0; i < DMD_MAX_SURFACE_OBJECTS; i++)
            if (!drv->surfaces[i].in_use) { slot = &drv->surfaces[i]; break; }
        if (!slot) { status = VA_STATUS_ERROR_ALLOCATION_FAILED; break; }
        const void *desc = memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 ?
            (const VADRMPRIMESurfaceDescriptor *)descriptors + made : descriptors;
        status = dmd_import_surface_init(slot, width, height, memory_type, desc);
        if (status != VA_STATUS_SUCCESS) break;
        slot->in_use = 1;
        slot->id = drv->next_surface_id++;
        surfaces[made] = slot->id;
    }
    if (status != VA_STATUS_SUCCESS)
        for (unsigned i = 0; i < made; i++) {
            dmd_surface_reset_locked(dmd_find_surface_locked(drv, surfaces[i]));
            surfaces[i] = VA_INVALID_ID;
        }
    pthread_mutex_unlock(&drv->lock);
    dmd_log("CreateSurfaces: imported 0x%x %ux%u n=%u status=0x%x\n",
            memory_type, width, height, count, status);
    return status;
}

VAStatus dmd_CreateSurfaces2(VADriverContextP ctx, unsigned int format,
                             unsigned int width, unsigned int height,
                             VASurfaceID *surfaces, unsigned int num_surfaces,
                             VASurfaceAttrib *attrib_list,
                             unsigned int num_attribs)
{
    /* Consumers only can pass PixelFormat (NV12 or P010) on the antrib_list
     * MemoryType (to VA internal, we do not support external RAM import).
     * PixelFormat is the only source deep** reliable: format fort of v2 has many calls
     * Still pass VA_RT_FORMAT_YUV420 (Chrome/ ffmpeg as measured), so P010 hours
     * Raise format from here to YuV420_10, so that the surface_alloc_locked
     * 2 Bytes/species distribution. */
    if (num_attribs > 0 && !attrib_list)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    unsigned memory_type = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    const void *external = NULL;
    int have_memory_type = 0, have_external = 0;
    for (unsigned i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VASurfaceAttribMemoryType) {
            if (have_memory_type++ || attrib_list[i].value.type != VAGenericValueTypeInteger)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            memory_type = (unsigned)attrib_list[i].value.value.i;
        } else if (attrib_list[i].type == VASurfaceAttribExternalBufferDescriptor) {
            if (have_external++ || attrib_list[i].value.type != VAGenericValueTypePointer)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            external = attrib_list[i].value.value.p;
        }
    }
    if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME ||
        memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        for (unsigned i = 0; i < num_attribs; i++)
            if (attrib_list[i].type == VASurfaceAttribPixelFormat &&
                (attrib_list[i].value.type != VAGenericValueTypeInteger ||
                 attrib_list[i].value.value.i != VA_FOURCC_NV12))
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        return create_imported_surfaces(ctx, format, width, height, surfaces,
                                         num_surfaces, memory_type, external);
    }

    for (unsigned int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VASurfaceAttribPixelFormat) {
            if (attrib_list[i].value.type != VAGenericValueTypeInteger)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            unsigned int fcc = (unsigned int)attrib_list[i].value.value.i;
            if (fcc == VA_FOURCC_P010) {
                format = VA_RT_FORMAT_YUV420_10;
            } else if (fcc != VA_FOURCC_NV12) {
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            }
        } else if (attrib_list[i].type == VASurfaceAttribMemoryType) {
            /* Only the memory that you can drive your own allocation is supported. dmabuf/user pointers import this path
             * It cannot be moved in a container (ION is not available, no / dev/dma_head). */
            if (attrib_list[i].value.type != VAGenericValueTypeInteger)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            if (!(attrib_list[i].value.value.i &
                  VA_SURFACE_ATTRIB_MEM_TYPE_VA))
                return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        } else if (attrib_list[i].type == VASurfaceAttribExternalBufferDescriptor) {
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
        /* The remaining properties (Usage Hint, etc.) are harmless, ignored. */
    }

    return create_surfaces_common(ctx, format, width, height, surfaces,
                                  num_surfaces);
}

/* A derived image borrows this allocation. An outstanding map or external
 * handle locks pixels; even an unmapped image must retain valid storage until
 * DestroyImage. Callers hold the object lock. */
static int surface_images_locked(struct dmd_driver *drv, VASurfaceID surface,
                                  int active_only)
{
    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        const struct dmd_image *img = &drv->images[i];
        if (img->in_use && img->derived_from == surface &&
            (!active_only || img->mapped || img->handle_acquired))
            return 1;
    }
    return 0;
}

VAStatus dmd_DestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list,
                             int num_surfaces)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !surface_list || num_surfaces <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    VAStatus status = VA_STATUS_SUCCESS;
    int freed = 0;

    pthread_mutex_lock(&drv->lock);
    for (int i = 0; i < num_surfaces; i++) {
        struct dmd_surface *s = dmd_find_surface_locked(drv, surface_list[i]);
        if (!s) {
            /* Continue to destroy the remaining: those that will be leaked back on their way back.
             * But still, the wrongs are being reported, and the caller has bugs that know. */
            status = VA_STATUS_ERROR_INVALID_SURFACE;
            continue;
        }
        if (surface_images_locked(drv, s->id, 0)) {
            status = VA_STATUS_ERROR_SURFACE_BUSY;
            continue;
        }
        dmd_surface_reset_locked(s);
        freed++;
    }
    pthread_mutex_unlock(&drv->lock);

    /* The record shows that there are logs and there are no ones here, and the check-ups will be viewed as "only create non-destruction" leaks.
     * The last round was misjudged once: 45 seconds of recordSurfaces, 6 times,
     * DestroySurfaces 0 times, actually the browser has been reusing the same collection,
     * Instead of driving the destruction. Filling up the log allows both sides to reconcile directly. */
    dmd_log("DestroySurfaces: requested %d, released %d%s\n",
            num_surfaces, freed,
            status == VA_STATUS_SUCCESS ? "" : "(Invalid ID)");

    return status;
}

/* ================================ context ================================ */

/* Decoding session (open / dev/ video32 and complete two-part consultation).
 * Caller** is not locked** - opening and negotiating is blocking IO. */
static struct dmd_session *session_open(int codec, unsigned int width,
                                        unsigned int height, int ten_bit)
{
    struct dmd_session_config cfg;
    struct dmd_error err;

    dmd_session_config_init(&cfg);
    cfg.codec = codec;
    cfg.width = (int)width;
    cfg.height = (int)height;
    cfg.io_timeout_ms = DMD_FRAME_TIMEOUT_MS;
    cfg.ten_bit = ten_bit;

    /* End: Fixed/dev/video32, no options available.
     *
     * 0.3.x Here are about 130 line comment and code endpoint selection (Unix block priority, TCP spins, TCPs)
     * DMD_ENDPOINT parsing unix: /tcp: prefix, netns impact on active locket,
     * The relationship between the SM_RIGHTS and the SM's is a mistake.
     * Replace session with direct /dev/ video32, all of which failed after / dev/ video32, deleted in 0.4.0 —
     * The "Unix socket" is a "retry of returning TCP": it will only fail again.
     * And the "back to TCP" in the log will push the real cause of failure behind.
     * It takes over when checking dma_heap missing.
     *
     * Two environmental variables, DMD_ENDPOINT/DMD_WANT_SHM, are therefore no longer relevant.
     * The complete history design is recorded in paragraph 0.3.x of CHANGELOG.md. */


    memset(&err, 0, sizeof(err));
    struct dmd_session *s = dmd_session_create(&cfg, &err);

    /* There was a "Unix block" back at TCP.
     * V4L2 Directly connects to — dmd_session_crime /dev/video32,
     * It's not about the sock_path/ want_shm. Try again only fails again.
     * And the "back to TCP" in the log will take the real cause of failure.
     * Squeeze behind and misdirect the check. The 80th wheel is crossed by the current machine when it is checked. Deleted. */

    if (!s)
        dmd_log("Session creation failed: code=%d handshake=%d %s\n", err.code,
                err.handshake_status, err.msg);
    else
        dmd_log("session opened: codec=%d %ux%u device=/dev/video32\n",
                codec, width, height);
    return s;
}

/* The session that Flush has had is no longer available for data to be sent, and transparent for a new one.
 *
 * ⁇  Rebuilding will destroy the frame of reference, not "slow" but "bad image".**
 *
 * There was a tip/ probe report "from non-IDR frames to immediately out."
 * So there's no visible screen." - That conclusion is wrong.
 * It only counts frames, it doesn't look at the screen, and those frames are completely black (Y=16). The probe files are marked at the beginning.
 * It is not credible in itself and should not quote from its conclusions.
 *
 * Truth: Any action (flush or reconstruction) to discard the decoder state will destroy the frame of reference.
 * The P/ B frame must rely on it. Start from non-IDR position again, until it is blacked out to the next IDR
 * This is "Deeper Plays" when you're playing.
 * The root of the flash of the picture is -- the actual 135/708 frame is pure black.
 *
 * So reconstruction** must be avoided as much as possible, not "reliable."
 * The right approach is to re-dispose and discard the re-distributed frame from the recent IDR (see alsos/probe_replay.c).
 * The strategy now being achieved is to tighten the trigger to almost non-occurrence (see the verdict of wait_is_futile).
 *
 * Caller must hold a lock; locks are temporarily placed inside this function to block IO (conflict + handshake).
 * Returns 0 successful, 1 failed (c->session empty when failed, caller to go failed path).
 */
static int session_rebuild_locked(struct dmd_driver *drv, struct dmd_context *c,
                                  int idx)
{
    struct dmd_session *old = c->session;
    int codec = c->codec;
    unsigned int w = c->picture_width;
    unsigned int h = c->picture_height;
    int ten_bit = c->output_ten_bit;

    /* Removes the old session + putting retiring. Only the pointer can't block the frame line: it may be before the takeoff.
     * The sess is holding hands and is locking in the IO. The old code is here directly io_busy=1 (not checking present values)
     * And then destroy (old) is like one side close to the other waiting for it to be right at the octl's fd.
     * It is now handed over to the session_retrie_locked: someone hangs up and returns it. */
    const VAContextID cid = c->id;
    c->session = NULL;
    c->retiring = 1;
    session_retire_locked(drv, idx, old);

    pthread_mutex_unlock(&drv->lock);
    struct dmd_session *ns = session_open(codec, w, h, ten_bit);
    pthread_mutex_lock(&drv->lock);

    c = dmd_find_context_locked(drv, cid);
    if (!c) {
        /* Context was removed during locking.
         * You can return it to the last person who has spent it. */
        if (ns)
            dmd_session_destroy(ns);
        dmd_log("Context disappeared during session rebuild\n");
        return -1;
    }

    if (!ns) {
        dmd_log("Session rebuild failed\n");
        c->session_failed = 1;
        c->retiring = 0;
        return -1;
    }

    c->session = ns;
    dmd_session_set_video_stats(ns, c->video_stats);
    c->retiring = 0;
    c->input_finished = 0;
    /* New session's daemon side vcl_in restart from 1 where it must be synchronized to zero,
     * Otherwise, submitting serial numbers to PTSs is mispositioned and the pairs will continue to follow the " no match " retreat path.
     * The old serial numbers left in the queue must be destroyed together, otherwise the new session will be issued with a number that will crash with them:
     * Test1080 is detected twice unit 5, 35 times "no match" retreat, 70/150 frames misposition.
     * Set 0 means "This is not valid serial number" and does not match any returns to PTS,
     * Only the retroactivity inference can be assigned in order -- that's the semantic sense that is needed. */
    c->units_submitted = 0;
    c->av1_head_stall = 0;
    c->av1_dead_head = 0;
    c->av1_dead_count = 0;
    c->av1_hold_surface = VA_INVALID_ID;
    c->av1_send_surface = VA_INVALID_ID;
    c->av1_last_ready = VA_INVALID_ID;
    c->av1_dpb.last_refresh_bitpos = (size_t)-1;
    c->av1_dpb.last_endupd_bitpos = (size_t)-1;
    /* Reset session: two bytes in the chute are old and cannot be retained (to be used as cost sessions)
     * And not without the previous frame re-emerged. */
    free(c->av1_repair_buf);
    free(c->av1_next_repair_buf);
    c->av1_repair_buf = c->av1_next_repair_buf = NULL;
    c->av1_repair_len = c->av1_next_repair_len = 0;
    c->av1_repair_frame = c->av1_next_repair_frame = 0;
    c->av1_repair_bitpos = c->av1_next_repair_bitpos = (size_t)-1;
    c->av1_repair_endupd_bitpos = c->av1_next_repair_endupd_bitpos = (size_t)-1;
    c->av1_repair_endupd_val = c->av1_next_repair_endupd_val = -1;
    c->av1_sef_count = 0;
    c->av1_hold_show = 0;
    c->av1_send_show = 0;
    for (int k = 0; k < c->pending_count; k++) {
        int idx = (c->pending_head + k) % DMD_MAX_SURFACES;
        c->pending_unit[idx] = 0;
    }
    /* The argument set is the state of the old session, the new session must be re-delivered -- clear the "send" mark,
     * Next time the logic of EndPicture will be recombined and sent to the SPS/PPS. */
    c->param_sets_sent = 0;
    dmd_log("Session rebuilt (codec=%d %ux%u). Parameter sets will be resent\n", codec, w, h);
    return 0;
}

VAStatus dmd_CreateContext(VADriverContextP ctx, VAConfigID config_id,
                           int picture_width, int picture_height, int flag,
                           VASurfaceID *render_targets, int num_render_targets,
                           VAContextID *context)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !context)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_render_targets < 0 ||
        (num_render_targets > 0 && !render_targets))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_render_targets > DMD_MAX_SURFACES)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (picture_width < DMD_MIN_WIDTH || picture_height < DMD_MIN_HEIGHT ||
        picture_width > DMD_MAX_WIDTH || picture_height > DMD_MAX_HEIGHT)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;

    *context = VA_INVALID_ID;

    VAProfile profile;
    int codec;
    int slot_idx = -1;

    pthread_mutex_lock(&drv->lock);

    /* Config must be effective and derived from it. */
    struct dmd_config *cfg = NULL;
    for (int i = 0; i < DMD_MAX_CONFIGS; i++) {
        if (drv->configs[i].in_use && drv->configs[i].id == config_id) {
            cfg = &drv->configs[i];
            break;
        }
    }
    if (!cfg) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    profile = cfg->profile;
    const int vpp = cfg->entrypoint == VAEntrypointVideoProc;
    codec = dmd_profile_to_codec(profile);
    if (codec < 0 && !vpp) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    /* Render target must be all pre-existing. */
    for (int i = 0; i < num_render_targets; i++) {
        if (!dmd_find_surface_locked(drv, render_targets[i])) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }

    for (int i = 0; i < DMD_MAX_CONTEXTS; i++) {
        if (!drv->contexts[i].in_use && !drv->io_busy[i]) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("CreateContext: no free slots (limit %d)\n", DMD_MAX_CONTEXTS);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    struct dmd_context *c = &drv->contexts[slot_idx];
    memset(c, 0, sizeof(*c));
    c->in_use = 1;
    c->id = (VAContextID)(drv->next_context_id++);
    c->config_id = config_id;
    c->profile = profile;
    c->output_ten_bit = profile == VAProfileHEVCMain10;
    c->entrypoint = cfg->entrypoint;
    c->codec = codec;
    c->picture_width = (unsigned int)picture_width;
    c->picture_height = (unsigned int)picture_height;
    c->flag = flag;
    c->current_target = VA_INVALID_ID;

    if (vpp) {
        *context = c->id;
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_SUCCESS;
    }

    c->video_stats = dmd_video_stats_create(c->id, cfg->entrypoint == VAEntrypointEncSlice,
                                            codec, (int)profile, DMD_VENDOR_STRING);
    if (cfg->entrypoint == VAEntrypointEncSlice) {
        unsigned rc = VA_RC_VBR;
        for (int i = 0; i < cfg->num_attribs; i++)
            if (cfg->attribs[i].type == VAConfigAttribRateControl)
                rc = cfg->attribs[i].value;
        c->encoder = dmd_encode_create(profile, picture_width, picture_height, rc);
        if (!c->encoder) {
            dmd_context_reset_locked(c);
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        dmd_encode_set_video_stats(c->encoder, c->video_stats);
        *context = c->id;
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_SUCCESS;
    }

    VAContextID new_id = c->id;
    pthread_mutex_unlock(&drv->lock);

    /* Sessions are built on locking -- concerned+ handshake is blocked IO, locking is stuck
     * All objects of other threads are operated. */
    /* Main10 tentatively starts with P010; its first valid PPB and actual
     * target select NV12/P010 before any parameter-set unit is sent. Config
     * RTFormat and an optional mixed render-target pool do not fix that choice.
     * AV1 still supplies its 8/10-bit depth in its PPB. */
    struct dmd_session *sess =
        session_open(codec, (unsigned int)picture_width,
                     (unsigned int)picture_height, profile == VAProfileHEVCMain10);

    pthread_mutex_lock(&drv->lock);
    /* Research: No one can destroy a context that has not been handed over to the caller, in theory.
     * However, review on ID is more durable than a cache pointer. */
    c = dmd_find_context_locked(drv, new_id);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        if (sess)
            dmd_session_destroy(sess);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    c->session = sess;
    dmd_session_set_video_stats(sess, c->video_stats);
    if (!sess) dmd_video_stats_error(c->video_stats);
    /* The session cannot be built to prevent the failure of the CreativeContext: daemon may be only temporarily unavailable, but the discussion is not available.
     * The first EndPicture will try again. The failure report's error is closer to the real cause. */
    c->session_failed = sess ? 0 : 1;
    /* The only consumer of EndPicture needs a hysteria that doesn't depend on it.
     * The frame collection path, otherwise the slow time frame will remain in the V4L2 queue. */
    dmd_reaper_start_locked(drv);
    pthread_mutex_unlock(&drv->lock);

    *context = new_id;
    dmd_log("CreateContext: config=%u profile=%d %dx%d -> context=%u%s\n",
            (unsigned)config_id, (int)profile, picture_width, picture_height,
            (unsigned)new_id, sess ? "" : "(Sessions to be re-tested)");

    return VA_STATUS_SUCCESS;
}

VAStatus dmd_DestroyContext(VADriverContextP ctx, VAContextID context)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (c->codec == DMD_CODEC_AV1 && c->av1_sef_visits)
        dmd_log("SEF stats: EndPicture calls %lu (nonempty units %lu, null units %lu), "
                "slot lookups %lu (send_show=1: %lu), queued %lu, dropped %lu, appended %lu\n",
                c->av1_ep_enter, c->av1_ep_unit, c->av1_ep_null,
                c->av1_sef_visits, c->av1_sef_show1,
                c->av1_sef_enq, c->av1_sef_drop, c->av1_sef_sent),
        dmd_log("SEF stats 2: staged with show=1 %lu, set send_show=1 %lu, "
                "submitted during flush %lu\n",
                c->av1_hold_show1, c->av1_sendset1, c->av1_flushed),
        dmd_log("SEF stats 3: submitted with show=1 in EndPicture %lu, in flush %lu\n",
                c->av1_ep_show1, c->av1_flush_show1);

    /* There are IOs that can't be broken when flying: when it's over. The belt is time-barred, death-free.
     * **Screw the session to io_defer, when the line is being returned.
     * The one that's gonna fix it is the one that's got the vertl.
     * The next move we make after the process is to get rid of it. */
    int idx = (int)(c - drv->contexts);
    int waited = 0;
    while (drv->io_busy[idx] && waited < DMD_FRAME_TIMEOUT_MS) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&drv->io_done, &drv->lock, &ts);
        waited += 50;
        c = dmd_find_context_locked(drv, context);
        if (!c) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_SUCCESS;
        }
    }
    const int io_stuck = drv->io_busy[idx];
    if (io_stuck)
        dmd_log("DestroyContext: I/O ownership was not returned within %d ms. Deferring session destruction\n",
                DMD_FRAME_TIMEOUT_MS);

    /* Encoding works on owned pixel/packet snapshots while the lock is free.
     * If its bounded IO has not returned yet, the last user owns destruction. */
    if (c->encoder && io_stuck) {
        drv->encode_defer[idx] = c->encoder;
        c->encoder = NULL;
    }

    /* From this moment on, the frame-up thread stops touching Ben context sessions (it skips when it sees retiring). */
    c->retiring = 1;

    /* The only way to get all the buffer:ffmpegs that belong to Ben is to be found in Destroy.
     * But it may leak under the abnormal path, and Driver can't leak it. */
    for (int i = 0; i < DMD_MAX_BUFFERS; i++) {
        struct dmd_buffer *b = &drv->buffers[i];
        if (b->in_use && b->context == context) {
            free(b->data);
            memset(b, 0, sizeof(*b));
        }
    }

    /* We'll empty the frame in the decoder and we'll think about giving up the rest.
     *
     * Why is it necessary: ffmpeg will first build a new DestroyContext when the resolution changes in the ** flow.
     * But it's still gonna be the old frame of the vaSyncSurface** (which belongs to the previous paragraph)
     * Resolution, it must be removed. Actual switch.h264 sent in 62 units here only retrieves 44 frames
     * - If the remaining 18 surfaces are marked as failed, ffmpeg will receive
     * "1 (operation failed)" the whole strip can't go on.
     *
     * These frames are not wrong, but they're still in MediaCodec: like the end of the stream, they need to be closed.
     * So this is the same flush + frame-take loop as SyncSurface.
     * I've been waiting for it, and there's no other line to do IO on this context. */
    if (c->session && c->pending_count > 0 && !io_stuck) {
        struct dmd_session *fs = c->session;
        /* The whole block was empty.** It held the slot.
         * The frame line is perfectly enough to grab this session (it only looks at io_busy), and this function goes on.
         * It's about to be torn down -- that's where ENOTY and the FD ownership operation come from. */
        drv->io_busy[idx] = 1;
        if (!c->input_finished) {
            c->input_finished = 1;
            pthread_mutex_unlock(&drv->lock);
            dmd_session_finish_input(fs);
            pthread_mutex_lock(&drv->lock);
        }

        int drained = 0;
        int budget = DMD_FRAME_TIMEOUT_MS;
        while (c && c->pending_count > 0 && budget > 0) {
            struct dmd_frame frame;
            memset(&frame, 0, sizeof(frame));
            pthread_mutex_unlock(&drv->lock);
            int frc = dmd_session_next_frame(fs, &frame, 100);
            pthread_mutex_lock(&drv->lock);
            budget -= 100;

            c = dmd_find_context_locked(drv, context);
            if (c && frc == DMD_OK) {
                VASurfaceID id = dmd_pending_take_locked(c, frame.unit_seq);
                struct dmd_surface *ds = dmd_find_surface_locked(drv, id);
                if (ds) {
                    surface_store_frame_locked(c, ds, &frame);
                    ds->state = DMD_SURFACE_READY;
                    drained++;
                }
            }
            if (frc == DMD_OK)
                dmd_session_release_frame(fs, &frame);
            if (!c)
                break;
            if (frc == DMD_ERR_TIMEOUT)
                continue;          /* And the budget will be waiting, as it was. */
            if (frc != DMD_OK)
                break;             /* EOS or True Error: The rest is not available */
        }
        io_release(drv, idx);
        if (drained)
            dmd_log("DestroyContext: drained %d frames\n", drained);
        if (!c) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_SUCCESS;
        }
    }

    /* Unready surface state is to be reset, otherwise they will stop at Pending forever.
     * SyncSurface will wait for a never coming frame.
     * Keep the context attribute with the READY state: the ffmpeg above is empty. */
    for (int i = 0; i < DMD_MAX_SURFACE_OBJECTS; i++) {
        struct dmd_surface *s = &drv->surfaces[i];
        if (s->in_use && s->context == context &&
            s->state == DMD_SURFACE_PENDING) {
            s->state = DMD_SURFACE_IDLE;
            s->decode_status = VA_STATUS_ERROR_OPERATION_FAILED;
            s->context = VA_INVALID_ID;
        }
    }

    const unsigned long long gone_units =
        c->session ? (unsigned long long)dmd_session_units_sent(c->session) : 0ULL;
    const unsigned long long gone_frames =
        c->session ? (unsigned long long)dmd_session_frames_received(c->session)
                   : 0ULL;
    dmd_log("DestroyContext: context=%u (submitted %llu units, received %llu frames)\n",
            (unsigned)context, gone_units, gone_frames);

    /* Send session_retrie_locked to open, dmd_content_reset_locked
     * And the same time that the thread was using it when the rod was used is this bug. */
    struct dmd_session *gone = c->session;
    c->session = NULL;
    dmd_context_reset_locked(c);
    session_retire_locked(drv, idx, gone);
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}

/* ================================ buffer ================================ */

VAStatus dmd_CreateBuffer(VADriverContextP ctx, VAContextID context,
                          VABufferType type, unsigned int size,
                          unsigned int num_elements, void *data,
                          VABufferID *buf_id)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !buf_id)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (size == 0 || num_elements == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    /* Spill check: size with num_elements are unsigned att, and the multiplier may be rewinded. */
    if ((size_t)size * num_elements > DMD_MAX_FRAME_BYTES)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    *buf_id = VA_INVALID_ID;
    size_t total = (size_t)size * num_elements;

    /* Allocation outside the lock: Large block calloc may trigger mmap with page error. */
    void *mem = calloc(1, total);
    if (!mem)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (data)
        memcpy(mem, data, total);

    pthread_mutex_lock(&drv->lock);

    /* Context must be valid. VA-API allows VA_INVALID_ID to build "No Context" braffer,
     * But the ffmpeg in the decoding path always carries context, so only valid context is accepted. */
    if (!dmd_find_context_locked(drv, context)) {
        pthread_mutex_unlock(&drv->lock);
        free(mem);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    struct dmd_buffer *slot = NULL;
    for (int i = 0; i < DMD_MAX_BUFFERS; i++) {
        if (!drv->buffers[i].in_use) {
            slot = &drv->buffers[i];
            break;
        }
    }
    if (!slot) {
        pthread_mutex_unlock(&drv->lock);
        free(mem);
        dmd_log("CreateBuffer: no free slots (limit %d)\n", DMD_MAX_BUFFERS);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->id = (VABufferID)(drv->next_buffer_id++);
    slot->context = context;
    slot->type = type;
    slot->element_size = size;
    slot->num_elements = num_elements;
    slot->size = total;
    slot->data = mem;
    if (type == VAEncCodedBufferType) {
        slot->coded_segment.buf = mem;
        slot->coded_segment.status = VA_CODED_BUF_STATUS_BAD_BITSTREAM;
        slot->coded_status = VA_STATUS_ERROR_OPERATION_FAILED;
    }

    *buf_id = slot->id;
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}

VAStatus dmd_BufferSetNumElements(VADriverContextP ctx, VABufferID buf_id,
                                  unsigned int num_elements)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || num_elements == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_buffer *b = dmd_find_buffer_locked(drv, buf_id);
    if (!b) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (b->mapped || b->coded_pending) {
        /* The mapping is a way to disable the caller's hand. */
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    size_t total = (size_t)b->element_size * num_elements;
    if (total > DMD_MAX_FRAME_BYTES) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    VAStatus status = VA_STATUS_SUCCESS;
    if (total > b->size) {
        void *mem = realloc(b->data, total);
        if (!mem) {
            status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        } else {
            memset((unsigned char *)mem + b->size, 0, total - b->size);
            b->data = mem;
        }
    }
    if (status == VA_STATUS_SUCCESS) {
        b->num_elements = num_elements;
        b->size = total;
        if (b->type == VAEncCodedBufferType) {
            b->coded_segment.buf = b->data;
            b->coded_segment.size = 0;
            b->coded_segment.status = VA_CODED_BUF_STATUS_BAD_BITSTREAM;
            b->coded_status = VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    pthread_mutex_unlock(&drv->lock);

    return status;
}

static VAStatus map_buffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf,
                            unsigned flags)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !pbuf)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    *pbuf = NULL;
    if (flags & ~(VA_MAPBUFFER_FLAG_READ | VA_MAPBUFFER_FLAG_WRITE))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!flags)
        flags = VA_MAPBUFFER_FLAG_READ | VA_MAPBUFFER_FLAG_WRITE;

    pthread_mutex_lock(&drv->lock);
    struct dmd_buffer *b = dmd_find_buffer_locked(drv, buf_id);
    if (b) {
        if (b->type == VAEncCodedBufferType) {
            if (b->coded_pending) {
                pthread_mutex_unlock(&drv->lock);
                return VA_STATUS_ERROR_HW_BUSY;
            }
            b->mapped++;
            *pbuf = &b->coded_segment;
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_SUCCESS;
        }
        b->mapped++;
        *pbuf = b->data;
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_SUCCESS;
    }

    /* VAImage’s reserve storage is also accessed through vamapBuffer (image. buff is a buffer ID).
     * The ffmpeg readback path is exactly the way to take pixels. */
    struct dmd_image *img = NULL;
    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        if (drv->images[i].in_use && drv->images[i].buf_id == buf_id) {
            img = &drv->images[i];
            break;
        }
    }
    if (!img) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (img->handle_acquired) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (img->derived_from != VA_INVALID_ID) {
        struct dmd_surface *parent = dmd_find_surface_locked(drv, img->derived_from);
        if (!parent || img->data != parent->data ||
            img->data_size > parent->data_size ||
            img->image.pitches[0] != parent->stride ||
            img->image.offsets[1] != (uint64_t)parent->stride * parent->slice_height ||
            img->image.width != parent->width || img->image.height != parent->height) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (parent->state == DMD_SURFACE_PENDING ||
            surface_images_locked(drv, parent->id, 1)) {
            /* Multiple maps of this same image retain the existing API's
             * reference count; other active derived views cannot overlap. */
            int others = 0;
            for (int i = 0; i < DMD_MAX_IMAGES; i++) {
                struct dmd_image *other = &drv->images[i];
                if (other != img && other->in_use &&
                    other->derived_from == parent->id &&
                    (other->mapped || other->handle_acquired))
                    others = 1;
            }
            if (parent->state == DMD_SURFACE_PENDING || others) {
                pthread_mutex_unlock(&drv->lock);
                return VA_STATUS_ERROR_SURFACE_BUSY;
            }
        }
        if (!img->mapped && parent->exportable && parent->dumb_sync_fd >= 0) {
            /* A legacy MapBuffer may read and write. Use one matching RW
             * cache transaction for all nested maps; flags still determine
             * whether the final unmap commits an uploaded image. */
            struct dma_buf_sync sync = {
                .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE
            };
            if (ioctl(parent->dumb_sync_fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
                pthread_mutex_unlock(&drv->lock);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            img->map_sync_started = 1;
        }
    }
    img->map_flags |= flags;
    img->mapped++;
    *pbuf = img->data;
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}

VAStatus dmd_MapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf)
{
    return map_buffer(ctx, buf_id, pbuf, VA_MAPBUFFER_FLAG_DEFAULT);
}

/* vMapBuffer2 = vMapBuffer+Lite of intention to read and write.
 *
 * ⁇  Must be achieved, not leaving a stake: libva only returns to vamapBuffer when ** slot is NULL**
 * (va.c) And we filled all the slots to pass CHECK_VTABLE,
 * So the ffmpeg vaap_map_frame (hwcontext_vaapi.c:928) got UNIMPLEMENTED
 * And it just failed and never went to the vamapBuffer branch.
 * It's a real conflict between "fill vtable" and "libva by NULL judgement."
 *
 * Direct surface uploads commit only after a successful WRITE unmap. */
VAStatus dmd_SyncBuffer(VADriverContextP ctx, VABufferID buf_id, uint64_t timeout_ns)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    /* Match the driver's bounded hardware waits, including VA_TIMEOUT_INFINITE. */
    const uint64_t limit = (uint64_t)DMD_FRAME_TIMEOUT_MS * 1000000ULL;
    if (timeout_ns > limit)
        timeout_ns = limit;
    deadline.tv_sec += (time_t)(timeout_ns / 1000000000ULL);
    deadline.tv_nsec += (long)(timeout_ns % 1000000000ULL);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&drv->lock);
    VAStatus status;
    for (;;) {
        struct dmd_buffer *b = dmd_find_buffer_locked(drv, buf_id);
        if (!b || b->type != VAEncCodedBufferType) {
            status = VA_STATUS_ERROR_INVALID_BUFFER;
            break;
        }
        if (!b->coded_pending) {
            status = b->coded_status;
            break;
        }
        if (timeout_ns == 0 ||
            pthread_cond_timedwait(&drv->io_done, &drv->lock, &deadline) == ETIMEDOUT) {
            status = VA_STATUS_ERROR_TIMEDOUT;
            break;
        }
    }
    pthread_mutex_unlock(&drv->lock);
    return status;
}

VAStatus dmd_MapBuffer2(VADriverContextP ctx, VABufferID buf_id, void **pbuf,
                        uint32_t flags)
{
    return map_buffer(ctx, buf_id, pbuf, flags);
}

VAStatus dmd_UnmapBuffer(VADriverContextP ctx, VABufferID buf_id)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_buffer *b = dmd_find_buffer_locked(drv, buf_id);
    if (b) {
        if (b->mapped > 0)
            b->mapped--;
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_SUCCESS;
    }

    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        struct dmd_image *img = &drv->images[i];
        if (img->in_use && img->buf_id == buf_id) {
            if (img->handle_acquired) {
                pthread_mutex_unlock(&drv->lock);
                return VA_STATUS_ERROR_SURFACE_BUSY;
            }
            VAStatus status = VA_STATUS_SUCCESS;
            if (img->mapped == 1 && img->derived_from != VA_INVALID_ID) {
                struct dmd_surface *parent = dmd_find_surface_locked(drv, img->derived_from);
                if (!parent || parent->data != img->data) {
                    status = VA_STATUS_ERROR_INVALID_BUFFER;
                } else {
                    if (img->map_sync_started) {
                        struct dma_buf_sync sync = {
                            .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE
                        };
                        if (parent->dumb_sync_fd < 0 ||
                            ioctl(parent->dumb_sync_fd, DMA_BUF_IOCTL_SYNC, &sync) != 0)
                            status = VA_STATUS_ERROR_OPERATION_FAILED;
                    }
                    if (img->map_flags & VA_MAPBUFFER_FLAG_WRITE) {
                        /* CPU upload supplies valid pixels, not a completed
                         * hardware decode. Do not update hardware telemetry. */
                        parent->decode_status = status;
                        parent->state = status == VA_STATUS_SUCCESS ?
                            DMD_SURFACE_READY : DMD_SURFACE_IDLE;
                        parent->context = VA_INVALID_ID;
                        parent->visible_x = parent->visible_y = 0;
                        parent->visible_width = parent->width;
                        parent->visible_height = parent->height;
                    }
                }
            }
            if (img->mapped > 0 && !--img->mapped) {
                img->map_flags = 0;
                img->map_sync_started = 0;
            }
            pthread_mutex_unlock(&drv->lock);
            return status;
        }
    }
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_ERROR_INVALID_BUFFER;
}

VAStatus dmd_DestroyBuffer(VADriverContextP ctx, VABufferID buffer_id)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_buffer *b = dmd_find_buffer_locked(drv, buffer_id);
    if (!b) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    void *mem = b->data;
    memset(b, 0, sizeof(*b));
    pthread_mutex_unlock(&drv->lock);

    free(mem);
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_BufferInfo(VADriverContextP ctx, VABufferID buf_id,
                        VABufferType *type, unsigned int *size,
                        unsigned int *num_elements)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !type || !size || !num_elements)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_buffer *b = dmd_find_buffer_locked(drv, buf_id);
    if (b) {
        *type = b->type;
        *size = b->element_size;
        *num_elements = b->num_elements;
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_SUCCESS;
    }

    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        struct dmd_image *img = &drv->images[i];
        if (img->in_use && img->buf_id == buf_id) {
            *type = VAImageBufferType;
            *size = (unsigned int)img->data_size;
            *num_elements = 1;
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_ERROR_INVALID_BUFFER;
}

/* Frame submission and bitstream assembly */

/* Adds data to the frame stream. The caller holds the lock. */
static int slice_append_locked(struct dmd_context *c, const void *data,
                               size_t len)
{
    if (len == 0)
        return 0;
    if (c->slice_len + len > DMD_MAX_UNIT_BYTES)
        return -1; /* Over the 8MB cap for daemon, the delivery was only convicted of illegal length */

    if (c->slice_len + len > c->slice_cap) {
        size_t cap = c->slice_cap ? c->slice_cap * 2 : 65536;
        while (cap < c->slice_len + len)
            cap *= 2;
        unsigned char *mem = realloc(c->slice_data, cap);
        if (!mem)
            return -1;
        c->slice_data = mem;
        c->slice_cap = cap;
    }
    memcpy(c->slice_data + c->slice_len, data, len);
    c->slice_len += len;

    return 0;
}

static int surface_is_external_encode_input(const struct dmd_surface *s)
{
    return s->state == DMD_SURFACE_IDLE && s->external_write_exported &&
           s->exportable && s->dumb_sync_fd >= 0 && !s->imported &&
           s->decode_status == VA_STATUS_SUCCESS &&
           s->format == VA_RT_FORMAT_YUV420;
}

VAStatus dmd_BeginPicture(VADriverContextP ctx, VAContextID context,
                          VASurfaceID render_target)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    struct dmd_surface *s = dmd_find_surface_locked(drv, render_target);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (surface_images_locked(drv, render_target, 1)) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }

    if (c->encoder) {
        if (drv->io_busy[c - drv->contexts]) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_HW_BUSY;
        }
        VAStatus status;
        if (!surface_is_external_encode_input(s)) {
            pthread_mutex_unlock(&drv->lock);
            status = dmd_surface_wait(drv, render_target);
            if (status != VA_STATUS_SUCCESS)
                return status;
            pthread_mutex_lock(&drv->lock);
            c = dmd_find_context_locked(drv, context);
            s = dmd_find_surface_locked(drv, render_target);
            if (!c || !c->encoder || !s) {
                pthread_mutex_unlock(&drv->lock);
                return !c ? VA_STATUS_ERROR_INVALID_CONTEXT : VA_STATUS_ERROR_INVALID_SURFACE;
            }
            if (drv->io_busy[c - drv->contexts]) {
                pthread_mutex_unlock(&drv->lock);
                return VA_STATUS_ERROR_HW_BUSY;
            }
            if (surface_images_locked(drv, render_target, 1)) {
                pthread_mutex_unlock(&drv->lock);
                return VA_STATUS_ERROR_SURFACE_BUSY;
            }
            /* Waiting releases the lock: a new submission or failed decode
             * may have changed the same surface before we reacquired it. */
            if (s->decode_status != VA_STATUS_SUCCESS) {
                status = s->decode_status;
                pthread_mutex_unlock(&drv->lock);
                return status;
            }
            if (s->state != DMD_SURFACE_READY &&
                !surface_is_external_encode_input(s)) {
                status = s->state == DMD_SURFACE_PENDING ?
                    VA_STATUS_ERROR_SURFACE_BUSY : VA_STATUS_ERROR_INVALID_SURFACE;
                pthread_mutex_unlock(&drv->lock);
                return status;
            }
        }
        /* WRITE export lets an application supply a fresh NV12 texture without
         * a decode/PutImage submission. The application finishes GPU commands;
         * EndPicture still brackets its CPU snapshot with DMA_BUF_SYNC_READ. */
        if (s->format != VA_RT_FORMAT_YUV420) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        }
        status = dmd_encode_begin(c->encoder, render_target);
        if (status == VA_STATUS_SUCCESS)
            c->current_target = render_target;
        pthread_mutex_unlock(&drv->lock);
        return status;
    }

    if (s->imported) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_UNIMPLEMENTED; /* imported surfaces are read-only inputs */
    }

    if (c->entrypoint == VAEntrypointVideoProc) {
        c->current_target = render_target;
        c->vpp_have_params = 0;
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_SUCCESS;
    }

    /* The last frame was not EndPicture, and Begin: Dispose the remaining data, otherwise it would be embedded in the new frame. */
    if (c->current_target != VA_INVALID_ID)
        dmd_log("BeginPicture: previous frame had no EndPicture call. Discarding %zu leftover bytes\n",
                c->slice_len);

    /* ⁇ 0.4.5 Is not proven: do not refuse to reuse READY 's searchface here.
     *
     * The motive is right (Chrhome may be reused before being sampled)
     * The refusal would destroy ffmpeg/Firefox: they go DeriveImage/MapBuffer reading pixels, but they would be a bit more likely to be a bit more difficult to read.
     * The path is not read back to IDLE, and the second round is denied.
     * (b) The md5 returns to H.264/HEVC/VP9 all FAIL, ffmpeg I/O EROR.
     *
     * The VA-API contract is that the time of reuse of the surface is the responsibility of the caller.
     * Drivers should not be limited in turn. Whoever returns too soon will be responsible for managing his resources. */
    c->current_target = render_target;
    c->slice_len = 0;
    c->av1_tile_count = 0;   /* Tile border reset with frame, with slick_len life cycle */
    c->have_vp8_slice_param = 0;
    c->have_vp8_pic_param = 0;
    c->hevc_slice_count = c->hevc_slices_bound = 0;
    c->hevc_slice_error = 0;
    /* MPEG-2's pic param / IQ matrix is sent down frame, with slice_len's life cycle.
     * mpeg2_seq_sent / mpeg2_temporal_ref is ** cross-frame** state and cannot be cleared here. */
    c->have_mpeg2_pic_param = 0;
    c->have_mpeg2_iq_matrix = 0;

    s->state = DMD_SURFACE_PENDING;
    s->decode_status = VA_STATUS_SUCCESS;
    s->context = context;

    pthread_mutex_unlock(&drv->lock);
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_RenderPicture(VADriverContextP ctx, VAContextID context,
                           VABufferID *buffers, int num_buffers)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || (num_buffers > 0 && !buffers))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_buffers < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (c->current_target == VA_INVALID_ID) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT; /* No, no, no, no. */
    }

    if (c->entrypoint == VAEntrypointVideoProc) {
        pthread_mutex_unlock(&drv->lock);
        return dmd_vpp_render(ctx, context, buffers, num_buffers);
    }

    if (c->encoder) {
        if (drv->io_busy[c - drv->contexts]) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_HW_BUSY;
        }
        VAStatus status = VA_STATUS_SUCCESS;
        for (int i = 0; i < num_buffers; i++) {
            struct dmd_buffer *b = dmd_find_buffer_locked(drv, buffers[i]);
            if (!b || b->context != context) {
                status = VA_STATUS_ERROR_INVALID_BUFFER;
                encode_output_error_locked(drv, c, status);
                dmd_encode_abort(c->encoder);
                c->current_target = VA_INVALID_ID;
                break;
            }
            status = dmd_encode_render(c->encoder, b->type, b->data,
                                       b->element_size, b->num_elements);
            if (status != VA_STATUS_SUCCESS) {
                dmd_log("Encode RenderPicture: type=%d size=%u status=0x%x\n",
                        b->type, b->element_size, status);
                encode_output_error_locked(drv, c, status);
                break;
            }
        }
        pthread_mutex_unlock(&drv->lock);
        return status;
    }

    VAStatus status = VA_STATUS_SUCCESS;

    for (int i = 0; i < num_buffers; i++) {
        struct dmd_buffer *b = dmd_find_buffer_locked(drv, buffers[i]);
        if (!b) {
            status = VA_STATUS_ERROR_INVALID_BUFFER;
            break;
        }

        switch (b->type) {
        case VASliceDataBufferType: {
            /* Here's the real stream.
             * VP9: Whole frame (including unpressed header, starting with frame_marker), as forwarded
             * VP8: Partition 0, missing RFC 6386 § 9.1 uncompressed chunk
             * H.264/HEVC: ** load** of slick NALU**, without starting number */
            size_t base = c->slice_len;
            if (slice_append_locked(c, b->data, b->size) != 0) {
                status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            } else if (c->codec == DMD_CODEC_HEVC) {
                if (c->hevc_slices_bound == c->hevc_slice_count)
                    c->hevc_slice_error = 1; /* long-format boundaries missing */
                for (unsigned j = c->hevc_slices_bound; j < c->hevc_slice_count; ++j) {
                    const VASliceParameterBufferHEVC *sp = &c->hevc_slice_param[j];
                    if (sp->slice_data_offset > b->size ||
                        sp->slice_data_size > b->size - sp->slice_data_offset)
                        c->hevc_slice_error = 1;
                    else c->hevc_slice_offset[j] = base + sp->slice_data_offset;
                }
                c->hevc_slices_bound = c->hevc_slice_count;
            }
            break;
        }

        case VASliceParameterBufferType:
            if (c->codec == DMD_CODEC_HEVC) {
                if (b->element_size < sizeof(VASliceParameterBufferHEVC) ||
                    b->num_elements > 128 - c->hevc_slice_count) {
                    c->hevc_slice_error = 1;
                    break;
                }
                for (unsigned j = 0; j < b->num_elements; ++j) {
                    memcpy(&c->hevc_slice_param[c->hevc_slice_count++],
                           (const uint8_t *)b->data + (size_t)j * b->element_size,
                           sizeof(VASliceParameterBufferHEVC));
                }
                break;
            }
            /* AV1: one slick param per tile (va_dec_av1.h:635),
             * It's the only reliable source of the Tile boundary -- slice data may be wrapped in a frame.
             * A buffer, the number of additions will get 1 instead of the actual number of tile.
             * A buffer can carry several elements (num_elements) to be taken all. */
            if (c->codec == DMD_CODEC_AV1 &&
                b->element_size >= sizeof(VASliceParameterBufferAV1)) {
                const int cap = (int)(sizeof(c->av1_tile_param) /
                                      sizeof(c->av1_tile_param[0]));
                for (unsigned int k = 0; k < b->num_elements; k++) {
                    if (c->av1_tile_count >= cap)
                        break;
                    const unsigned char *src =
                        (const unsigned char *)b->data +
                        (size_t)k * b->element_size;
                    memcpy(&c->av1_tile_param[c->av1_tile_count++], src,
                           sizeof(VASliceParameterBufferAV1));
                }
                break;
            }
            /* VP8 takes participation_size[0] and macroblock_offset to deduce
             * First_part_size, the remaining codec currently does not require a slick parameter. */
            if (c->codec == DMD_CODEC_VP8 &&
                b->size >= sizeof(VASliceParameterBufferVP8)) {
                memcpy(&c->vp8_slice_param, b->data,
                       sizeof(VASliceParameterBufferVP8));
                c->have_vp8_slice_param = 1;
            } else if (c->codec == DMD_CODEC_H264 &&
                       b->size >= sizeof(VASliceParameterBufferH264)) {
                /* Keeps the backup. Note that PPS's num_ref_idx_default cannot be taken from here:
                 * The set of parameters is to be sent before the first VCL, when only the IDR's slick param,
                 * I slick is 0 for the num_ref_idx (for details, h264_bitstream.c). */
                memcpy(&c->h264_slice_param, b->data,
                       sizeof(VASliceParameterBufferH264));
                c->have_h264_slice_param = 1;
            }
            break;

        case VAPictureParameterBufferType:
            /* AV1: All the fields of the sequence header and frame are here and are the only sources of synthesis of OBU. */
            if (c->codec == DMD_CODEC_AV1 &&
                b->size >= sizeof(VADecPictureParameterBufferAV1)) {
                memcpy(&c->av1_pic_param, b->data,
                       sizeof(VADecPictureParameterBufferAV1));
                c->have_av1_pic_param = 1;
                /* Diagnostic: Identify each ** synthetic input from the consumer.
                 * The correctness of the synthetic flow can only be taken by "show with the source stream field by field," but first, by the way.
                 * Know what the VA side gives -- the consumer didn't fill it out, or we misread.
                 * DMD_AV1_PCDBG=1 */
                if (getenv("DMD_AV1_PICDBG")) {
                    static int pdbg;
                    const VADecPictureParameterBufferAV1 *q = &c->av1_pic_param;
                    const __typeof__(q->seq_info_fields) *sq = &q->seq_info_fields;
                    if (++pdbg <= 6) {
                        const __typeof__(q->pic_info_fields) *pf =
                            &q->pic_info_fields;
                        fprintf(stderr,
                            "[picdbg] #%d ft=%u show=%u showable=%u oh=%u "
                            "cur=%u scm=%u int_mv=%u intrabc=%u hiprec=%u "
                            "switchable=%u refmvs=%u err_res=%u cdf_upd=%u "
                            "endupd=%u unif=%u warp=%u lgt=%u tx=%u "
                            "tiles=%ux%u ctx_upd=%u prof=%u dbi=%u ohb=%u\n",
                            pdbg, (unsigned)pf->bits.frame_type,
                            (unsigned)pf->bits.show_frame,
                            (unsigned)pf->bits.showable_frame,
                            (unsigned)q->order_hint,
                            (unsigned)q->current_frame,
                            (unsigned)pf->bits.allow_screen_content_tools,
                            (unsigned)pf->bits.force_integer_mv,
                            (unsigned)pf->bits.allow_intrabc,
                            (unsigned)pf->bits.allow_high_precision_mv,
                            (unsigned)pf->bits.is_motion_mode_switchable,
                            (unsigned)pf->bits.use_ref_frame_mvs,
                            (unsigned)pf->bits.error_resilient_mode,
                            (unsigned)pf->bits.disable_cdf_update,
                            (unsigned)pf->bits.disable_frame_end_update_cdf,
                            (unsigned)pf->bits.uniform_tile_spacing_flag,
                            (unsigned)pf->bits.allow_warped_motion,
                            (unsigned)pf->bits.large_scale_tile,
                            (unsigned)0,
                            (unsigned)q->tile_cols, (unsigned)q->tile_rows,
                            (unsigned)q->context_update_tile_id,
                            (unsigned)q->profile, (unsigned)q->bit_depth_idx,
                            (unsigned)q->order_hint_bits_minus_1);
                        fprintf(stderr,
                            "[seqdbg] #%d still=%u sbs128=%u f_intra=%u "
                            "edge=%u iic=%u mc=%u dual=%u oh_en=%u jnt=%u "
                            "cdef=%u mono=%u range=%u subs=%u/%u fg=%u\n",
                            pdbg, (unsigned)sq->fields.still_picture,
                            (unsigned)sq->fields.use_128x128_superblock,
                            (unsigned)sq->fields.enable_filter_intra,
                            (unsigned)sq->fields.enable_intra_edge_filter,
                            (unsigned)sq->fields.enable_interintra_compound,
                            (unsigned)sq->fields.enable_masked_compound,
                            (unsigned)sq->fields.enable_dual_filter,
                            (unsigned)sq->fields.enable_order_hint,
                            (unsigned)sq->fields.enable_jnt_comp,
                            (unsigned)sq->fields.enable_cdef,
                            (unsigned)sq->fields.mono_chrome,
                            (unsigned)sq->fields.color_range,
                            (unsigned)sq->fields.subsampling_x,
                            (unsigned)sq->fields.subsampling_y,
                            (unsigned)sq->fields.film_grain_params_present);
                    }
                }
                /* The order of display of AV1 is the order of order_hint (regulation 6.8.2),
                 * The same POC as the H.264/HEVC: the decoder spits out the frame in the display order,
                 * And ffmpeg is submitted in decoding order, matching the retreat path needs it.
                 *
                 * Previously AV1 was completely unregistered POC,pending_poc is INT32_MAX
                 * (actual log "POC 214743647" ), once unit_seq is precisely paired
                 * Failure can only be inferred blindly and sequentially, and the frame is matched to the wrong surface.
                 *
                 * The KEY frame will reset the order_hint sequence, and the new sequence will be judged with a frame_type:
                 * frame_type0=KEY, synonym with H.264. */
                /* Global motion parameters are matched: only when there is a non-permanent shift, one line by reference.
                 * To confirm whether the VA side wmmat[] is gm_params[] in the source stream
                 * (Call 5.9.24), and then reverses the subext symbol. */
                if (getenv("DMD_AV1_WMDBG")) {
                    const VADecPictureParameterBufferAV1 *q = &c->av1_pic_param;
                    int any = 0;
                    for (int i = 0; i < 7; i++)
                        if (q->wm[i].wmtype != VAAV1TransformationIdentity ||
                            q->wm[i].invalid) any = 1;
                    if (any) {
                        fprintf(stderr, "[wm] oh=%u hp=%u prim=%u warp=%u\n",
                                q->order_hint,
                                (unsigned)q->pic_info_fields.bits
                                        .allow_high_precision_mv,
                                (unsigned)q->primary_ref_frame,
                                (unsigned)q->pic_info_fields.bits
                                        .allow_warped_motion);
                        for (int i = 0; i < 7; i++)
                            fprintf(stderr, "[wm]  i=%d type=%d inv=%u "
                                    "m=%d,%d,%d,%d,%d,%d\n",
                                    i, (int)q->wm[i].wmtype,
                                    (unsigned)q->wm[i].invalid,
                                    q->wm[i].wmmat[0], q->wm[i].wmmat[1],
                                    q->wm[i].wmmat[2], q->wm[i].wmmat[3],
                                    q->wm[i].wmmat[4], q->wm[i].wmmat[5]);
                    }
                }
                if (getenv("DMD_AV1_SEFPROBE")) {
                    const VADecPictureParameterBufferAV1 *q = &c->av1_pic_param;
                    fprintf(stderr, "[sef] oh=%u ft=%u show=%u showable=%u "
                            "target=%u anchor=%u tiles=%ux%u\n",
                            q->order_hint,
                            q->pic_info_fields.bits.frame_type,
                            q->pic_info_fields.bits.show_frame,
                            q->pic_info_fields.bits.showable_frame,
                            (unsigned)c->current_target,
                            (unsigned)q->current_frame,
                            q->tile_cols, q->tile_rows);
                }
                c->current_poc =
                    (int32_t)c->av1_pic_param.order_hint;
                c->current_frame_num =
                    (c->av1_pic_param.pic_info_fields.bits.frame_type == 0)
                        ? 0 : 1;
                c->have_current_poc = 1;
                break;
            }
            /* The key_frame / version / show_frame is here. */
            if (c->codec == DMD_CODEC_VP8 &&
                b->size >= sizeof(VAPictureParameterBufferVP8)) {
                memcpy(&c->vp8_pic_param, b->data,
                       sizeof(VAPictureParameterBufferVP8));
                c->have_vp8_pic_param = 1;
            } else if (c->codec == DMD_CODEC_MPEG2 &&
                       b->size >= sizeof(VAPictureParameterBufferMPEG2)) {
                /* MPEG-2: Everything is in the document
                 * Here (size, picture_code_type, f_code, extension markers). */
                memcpy(&c->mpeg2_pic_param, b->data,
                       sizeof(VAPictureParameterBufferMPEG2));
                c->have_mpeg2_pic_param = 1;
            } else if (c->codec == DMD_CODEC_HEVC &&
                       b->size >= sizeof(VAPictureParameterBufferHEVC)) {
                /* Use the same as H.264: Synthetic VPS/SPS/PPS and POC for matching back-up paths.
                 * CurrPic.pic_order_cnt is the complete package unpackaged POC, which is directly larger than the size. */
                const VAPictureParameterBufferHEVC *pp = (const VAPictureParameterBufferHEVC *)b->data;
                const unsigned minus8 = pp->bit_depth_luma_minus8;
                const int ten_bit = minus8 == 2;
                struct dmd_surface *target = dmd_find_surface_locked(drv, c->current_target);
                /* Main10 genuinely accepts 8-bit NV12 and 10-bit P010. Lock
                 * depth on the first valid PPB, before CAPTURE negotiation;
                 * retain that lock across subsequent frames/session rebuilds.
                 * Main remains 8-bit, and no sample-size conversion is done. */
                if ((minus8 != 0 && minus8 != 2) ||
                    pp->bit_depth_chroma_minus8 != minus8 ||
                    (c->profile != VAProfileHEVCMain10 && ten_bit) ||
                    (c->hevc_pic_param_valid && ten_bit != c->output_ten_bit) ||
                    pp->pic_fields.bits.chroma_format_idc != 1 ||
                    pp->pic_fields.bits.separate_colour_plane_flag ||
                    !target || target->format != (ten_bit ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420)) {
                    dmd_log("HEVC: unsupported depth/chroma or target format "
                            "profile=%d luma=%u chroma=%u surface=%d\n",
                            (int)c->profile, (unsigned)pp->bit_depth_luma_minus8,
                            (unsigned)pp->bit_depth_chroma_minus8,
                            target ? (target->format == VA_RT_FORMAT_YUV420_10) : -1);
                    dmd_video_stats_error(c->video_stats);
                    if (target) {
                        target->state = DMD_SURFACE_IDLE;
                        target->decode_status = VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
                    }
                    c->current_target = VA_INVALID_ID;
                    pthread_mutex_unlock(&drv->lock);
                    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
                }
                if (!c->hevc_pic_param_valid) {
                    c->output_ten_bit = ten_bit;
                    dmd_session_set_ten_bit(c->session, ten_bit);
                }
                memcpy(&c->hevc_pic_param, pp, sizeof(*pp));
                c->hevc_pic_param_valid = 1;
                {
                    static int logged;
                    if (!logged) {
                        logged = 1;
                        dmd_log("HEVC pic_param: DPB=%u NoPicReordering=%u "
                                "NoBiPred=%u POC=%d\n",
                                (unsigned)c->hevc_pic_param.sps_max_dec_pic_buffering_minus1,
                                (unsigned)c->hevc_pic_param.pic_fields.bits.NoPicReorderingFlag,
                                (unsigned)c->hevc_pic_param.pic_fields.bits.NoBiPredFlag,
                                (int)c->hevc_pic_param.CurrPic.pic_order_cnt);
                    }
                }
                c->current_poc = c->hevc_pic_param.CurrPic.pic_order_cnt;
                /* HEVC does not have a frame_num; new sequences are determined with IRAP:
                 * IDR will reset POC and match with the seq. */
                c->current_frame_num =
                    c->hevc_pic_param.slice_parsing_fields.bits.IdrPicFlag ? 0 : 1;
                c->have_current_poc = 1;
            } else if (c->codec == DMD_CODEC_H264 &&
                       b->size >= sizeof(VAPictureParameterBufferH264)) {
                /* Two uses:
                 * 1) Reverse Synthesis SPS/PPS - ffmpeg consumed the parameter set
                 *    (h264dec.c: 699:717) Driver can only be rebuilt from these fields
                 * 2) Take the frame POC (Show Order) for matching - MediaCodec by display order
                 *    The frame is ffmpeg submitted in decoding order, only POC can pair the two.
                 *    ffmpeg filled out unpackaged field_poc [0] (vaapi_h264.c:74),
                 *    Not 6 bit poc_lsb that will be rounded in the stream, which is directly comparable to size. */
                memcpy(&c->h264_pic_param, b->data,
                       sizeof(VAPictureParameterBufferH264));
                c->have_h264_pic_param = 1;
                c->current_poc = c->h264_pic_param.CurrPic.TopFieldOrderCnt;
                c->current_frame_num = c->h264_pic_param.frame_num;
                c->have_current_poc = 1;
            }
            break;

        case VAIQMatrixBufferType:
            /* ⁇  MPEG-2 is the exception: its quantitative matrix** is not in the **Slice Data.
             * VA-API takes table_head *quatiser_matrix with matrix
             * The contents were moved from the level of the slides only.
             * Start. When synthesizes the tablet, you must write it back, or default it.
             * Matrix resolution is not the default matrix stream * The debris is counter-quantifiable in error, the image is deconstructable but not correct.
             * The rest of the matrix of codec (H.264/HEVC) is still in a cipher bytes, which are ignored as usual. */
            if (c->codec == DMD_CODEC_MPEG2 &&
                b->size >= sizeof(VAIQMatrixBufferMPEG2)) {
                memcpy(&c->mpeg2_iq_matrix, b->data,
                       sizeof(VAIQMatrixBufferMPEG2));
                c->have_mpeg2_iq_matrix = 1;
            }
            break;

        case VAProbabilityBufferType:
        case VAHuffmanTableBufferType:
            /* V4L2 side is a complete hardware codec, and these tables are in the bitstream.
             * We don't need to forward. Accept but ignore. */
            break;

        default:
            /* Unknown type is not wrong: some consumers will be accompanied by buffer that we don't care about.
             * Rejection would have failed the whole frame. */
            break;
        }
    }
    pthread_mutex_unlock(&drv->lock);

    return status;
}

/* - VP8: Synthetic RFC 6386 § 9.1 uncompressed data chunk
 *
 * The first 3 bytes missing from the list data of VA-API starting with part 0 (compressed frame)
 * Fame tag (start code + dimensions of seven bytes added). MediaCodec wants
 * Complete frame, so must be filled back.
 *
 * frametag (3 bytes, sub 19 bit + dimensions):
 *   bit0      frame_type（0=key）
 *   bit1-3    version
 *   bit4      show_frame
 *   bit5-23   first_part_size
 * First_part_size (key): Partition_size[0] of VA-API is "after application of the resolution."
 * The remaining control partition bytes" plus the decomposed head bytes (macroblock_offset+7)/8
 * The first_part_size of the RFC definition.
 * Show_frame has no field in VA-API, but only assumes that 1 - VP8 rarely uses an invisible frame.
 */
static size_t vp8_build_frame(struct dmd_context *c, unsigned char *out,
                              size_t out_cap)
{
    if (!c->have_vp8_slice_param || !c->have_vp8_pic_param)
        return 0;

    const VASliceParameterBufferVP8 *sp = &c->vp8_slice_param;
    const VAPictureParameterBufferVP8 *pp = &c->vp8_pic_param;

    unsigned int key_frame = pp->pic_fields.bits.key_frame ? 0 : 1;
    /* VA-API Comment: "0 means a key frame", synonym of the RFC 's frame_type
     * (RFC: 0=key frame. Here the key_frame variable means "the key frame." */
    key_frame = pp->pic_fields.bits.key_frame == 0 ? 1 : 0;
    unsigned int version = pp->pic_fields.bits.version & 0x7;
    unsigned int header_bytes = (sp->macroblock_offset + 7) / 8;
    unsigned int first_part_size = sp->partition_size[0] + header_bytes;
    if (first_part_size > 0x7FFFF)
        return 0; /* 19 is not loaded. The code is out of order. */

    size_t tag_len = key_frame ? 10 : 3;
    if (out_cap < tag_len + c->slice_len)
        return 0;

    /* frame tag:3 bytes sub-bit */
    uint32_t tag = (key_frame ? 0u : 1u) | (version << 1) |
                   (1u << 4) /* Show_frame: VA-API does not have this field, assuming visible */
                   | (first_part_size << 5);
    out[0] = (unsigned char)(tag & 0xFF);
    out[1] = (unsigned char)((tag >> 8) & 0xFF);
    out[2] = (unsigned char)((tag >> 16) & 0xFF);

    if (key_frame) {
        /* Key frame extra seven bytes: 3 bytes start code 9D 01 2A + 2+2 bytes.
         * 2 bits tall is scaling, we don't zoom in, fill zero. */
        out[3] = 0x9D;
        out[4] = 0x01;
        out[5] = 0x2A;
        unsigned int w = pp->frame_width & 0x3FFF;
        unsigned int h = pp->frame_height & 0x3FFF;
        out[6] = (unsigned char)(w & 0xFF);
        out[7] = (unsigned char)((w >> 8) & 0x3F);
        out[8] = (unsigned char)(h & 0xFF);
        out[9] = (unsigned char)((h >> 8) & 0x3F);
    }

    memcpy(out + tag_len, c->slice_data, c->slice_len);
    return tag_len + c->slice_len;
}

/* The data unit that is finally to be sent to the datamon is assembled.
 * Returns the buffer (possibly c->slice_data itself) and the length to be sent; failed to return NULL.
 * Scrack is provided by the caller and is responsible for release. */
/* AV1 Synthesis results fallboard (DMD_AV1_DUMP/DMD_AV1_DUMP_ALL).
 *
 * ⁇  Must be rewritten ** and delivered when it really came to it.
 * Earlier in the synthesis, the bytes before the rewrite, which led to the trace_headers see
 * Refresh is a round of turns (1) and not counter-counted true values, and a white-check round.
 *
 * This is the primary means of authentication of this module: synthetic correctness cannot be directly judged on the device
 * The bytes have to be returned to the house.
 * Authority tools:
 *   ffmpeg-f obu-i <documents > > f null- (can you untie the frame)
 *   ffmpeg-bsf: v trade_headers... (field-by-field)
 * The auto-synthesis device is unreliable -- it was misjudged "synchronous" by its own deviation and the achievement of a set-off of errors.
 *
 * Default to set only the frame;DMD_AV1_DUMP_ALL=1 add the entire stream to verify multi-frame sequence
 * "The first frame is correct but the next frame stops the decoder." */
static void av1_dump_sent(const unsigned char *buf, size_t n)
{
    const char *dp = getenv("DMD_AV1_DUMP");
    if (!dp || !buf || n == 0)
        return;
    const char *all = getenv("DMD_AV1_DUMP_ALL");
    int dump_all = all && all[0] == '1';
    static int dumped = 0;
    if (dump_all) {
        FILE *df = fopen(dp, dumped ? "ab" : "wb");
        if (df) { fwrite(buf, 1, n, df); fclose(df); dumped++; }
    } else if (!dumped) {
        FILE *df = fopen(dp, "wb");
        if (df) { fwrite(buf, 1, n, df); fclose(df); dumped = 1;
                  dmd_log("Wrote %zu bytes to %s", n, dp); }
    }
}

/* Each unit sent into hardware is left on a single disk (DMD_AV1_DUMP2= < prefix>).
 *
 * The difference between av1_dump_sent: the one that sets the frame in the order of "normal path" into a stream,
 * Can not distinguish between two trips of the same frame. Here are the tab+ fractions:
 *     Prefix. Tag. n. obu.
 * Tag: n = normal delivery / 1 = Flush first / 2 = repair/ k =KEY.
 * Use: Use the bytes of the repair to match the source stream to the frame, to determine whether the "content error" is in
 * Synthetic/rewrite (bytes are different from sources) or hardware is a double-trip state (bytes are the same but pixels are different). */
static void av1_dump_tagged(const char *tag, const unsigned char *buf, size_t n)
{
    const char *pre = getenv("DMD_AV1_DUMP2");
    if (!pre || !pre[0] || !buf || n == 0)
        return;
    static unsigned gseq;
    char path[512];
    /* The filename is numbered ** Global Send **, and the dictionaries are separated by the actual receipt order of the hardware. */
    snprintf(path, sizeof path, "%s.%04d.%s.obu", pre, ++gseq, tag);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, n, f); fclose(f); }
}

/* VA-API Frame-by-Face Parameter Falling (DMD_AV1_RECORD= < Path>).
 *
 * Uses: real value of refresh_frame_flags = difference between next frame Map and this frame map,
 * So just write down the whole sequence (map, ref_frame_idx, current_frame, show, oh, ft)
 * You can re-calculate the frame-to-frame values and evaluate the "delay-free" slot allocation strategies in the offline environment.
 * - There is no need to re-engineer and rerun hardware every change.
 * Only recorded and not changed any behaviour. */
static void av1_record_pic(const VADecPictureParameterBufferAV1 *pp)
{
    const char *rp = getenv("DMD_AV1_RECORD");
    if (!rp || !rp[0])
        return;
    static FILE *rf;
    static int rec_n;
    if (!rf) {
        rf = fopen(rp, "w");
        if (!rf)
            return;
    }
    if (++rec_n == 1)
        fprintf(rf, "# n ft show oh cur map[8] idx[7]\n");
    fprintf(rf, "%d %u %u %u %u", rec_n,
            pp->pic_info_fields.bits.frame_type,
            pp->pic_info_fields.bits.show_frame, pp->order_hint,
            (unsigned)pp->current_frame);
    for (int i = 0; i < 8; i++)
        fprintf(rf, " %u", (unsigned)pp->ref_frame_map[i]);
    for (int i = 0; i < 7; i++)
        fprintf(rf, " %u", pp->ref_frame_idx[i]);
    fprintf(rf, "\n");
    fflush(rf);
}

static void av1_bit_set(unsigned char *buf, size_t len, size_t bitpos, int val)
{
    size_t byte = bitpos >> 3;
    if (!buf || byte >= len)
        return;
    unsigned char m = (unsigned char)(1u << (7 - (bitpos & 7)));
    if (val) buf[byte] |= m;
    else     buf[byte] &= (unsigned char)~m;
}

static int av1_bit_get(const unsigned char *buf, size_t len, size_t bitpos)
{
    size_t byte = bitpos >> 3;
    if (!buf || byte >= len)
        return -1;
    return (buf[byte] >> (7 - (bitpos & 7))) & 1;
}

static const unsigned char *build_unit(struct dmd_context *c,
                                       unsigned char **scratch, size_t *out_len)
{
    *scratch = NULL;

    if (c->slice_len == 0)
        return NULL;

    switch (c->codec) {
    case DMD_CODEC_VP9:
        /* Zero header reconstruction: VASLiceDataBufferType is complete VP9 frame
         * (va_dec_vp9.h: 274-284 normative comment: slice data contains the whole frame,
         * The blog is a blog of the Global Voices community, which has a special focus on the issue of the Internet, and the Internet. */
        *out_len = c->slice_len;
        return c->slice_data;

    case DMD_CODEC_AV1: {
        /* Reconstruct AV1 OBUs from VA-API buffers. VASliceDataBufferType
         * contains tile payloads without OBU framing (va_dec_av1.h:643–645).
         * VADecPictureParameterBufferAV1 provides the sequence and frame
         * header fields. VP9 slice data already carries complete frames.
         *
         * Forwarding the AV1 payload unmodified started with 0xd0, which
         * sets forbidden_bit and selects reserved OBU type 10. ffprobe
         * rejected the input and MediaCodec produced no frames.
         *
         * Assemble a temporal delimiter, a sequence header on key frames,
         * and an OBU_FRAME with the frame header, alignment and tile group.
         * dav1d rejected the tested split FRAME_HEADER + TILE_GROUP form.
         * The libaom reference stream used OBU_FRAME. */
        if (!c->have_av1_pic_param || c->av1_tile_count <= 0) {
            dmd_log("EndPicture: AV1 is missing %s and cannot synthesize an OBU",
                    c->have_av1_pic_param ? "tile data" : "pic param");
            return NULL;
        }

        const VADecPictureParameterBufferAV1 *pp = &c->av1_pic_param;
        const uint32_t want_tiles =
            (uint32_t)pp->tile_cols * (uint32_t)pp->tile_rows;

        /* Report the bit depth once known. It is not available in vaCreateContext.
         * Set it before the first unit triggers SOURCE_CHANGE and CAPTURE negotiation. */
        if (!c->av1_bd_reported) {
            c->av1_bd_reported = pp->bit_depth_idx != 0 ? 2 : 1;
            c->output_ten_bit = c->av1_bd_reported == 2;
            dmd_session_set_ten_bit(c->session, c->output_ten_bit);
        }

        av1_record_pic(pp);

        /* Catch must be performed before **the frame is synthesized: it registers the slots of the previous frame into the shadow
         * DPB, whose reference translation (put_unpressed_header) is used for this registration.
         * Used to be after build_frame -- the remap of this frame so you can't see the previous frame.
         * Place, reference to old content (33 error frames shown in fact). */
        /* Patch is for the bytes of the past ** in the fixation buffer (realized from the distribution of the difference)
         * Refresh writes back the stream that has already been delivered.
         * ⁇  Must call unconditionally: patch when buffer is empty (KEY and first frame)
         * And keep the map snapshots, push the differential benchmark, skip the whole thing.
         * The sequence is wrong (actually prev_valid card 0, all refresh is not counter-productive). */
        dmd_av1_patch_prev_refresh(&c->av1_dpb, pp,
                                   c->av1_repair_buf, c->av1_repair_len,
                                   c->av1_repair_bitpos,
                                   c->av1_repair_frame);

        /* The reference frame is not in the shadow DPB -- this hardware is unlikely to be solved. See.
         * Note to dmd_av1_refs_resolve() (Venus does not have a frame for citing empty slots)
         * "The wrong thing is, it'll drag the entire bar stream to death."
         *
         * ⁇  The judgement must be placed after the watch_prev_refresh**. Before it is placed, the calibration is detected and lost:
         * The registration of shadow slots occurs in the patch.
         * Map difference is counter-counted and registered) and skipping past past is like never entering a frame
         * DPB, so each frame behind it is "quoted impregnable".
         * Sensitization 37 frames in 43 frames only for KEY with 1 frame out; past
         * The judgement  ⁇  only dropped the seven frames before the KEY.
         *
         * The missing frame is not synthesized, hardware is not sent, but its Map is still recorded as the differential benchmark
         * - That's right: the next frame paste, the difference in the score is "This lost frame is in the source stream."
         * Kicked Who out, the shoal selection for this frame; its own refresh value is for repairing buffers
         * Empty (build returned early, without writing av1_text_repair_buf) was abandoned naturally. */
        if (!dmd_av1_refs_resolvable(pp, &c->av1_dpb)) {
            c->av1_ref_drop++;
            dmd_log("EndPicture: reference frame for AV1 surface %u is missing from the DPB "
                    "(possibly a seek into a GOP). Skipping submission and leaving an empty surface\n",
                    (unsigned)c->current_target);
            c->av1_ref_drop_now = 1;
            return NULL;
        }

        if (want_tiles == 0 || (uint32_t)c->av1_tile_count != want_tiles) {
            /* The number of tiles does not match the frame statement, which will cause the decoder to be misplaced from the second tile.
             * Instead of sending out MediaCodec to remove the flower screen, it would be better to get back clean and soft. */
            dmd_log("EndPicture: AV1 tile count mismatch (received %d, frame header declares %u). Skipping hardware decode",
                    c->av1_tile_count, want_tiles);
            return NULL;
        }

        /* Tile Description Table: Use each Tile's own slick_data_offset/ slice_data_size
         * Positioning in cumulative buffers (va_dec_av1.h:649-658) cannot be based on "additional times" --
         * ffmpeg on the live machine wraps the whole frame into a slick data buffer. */
        struct dmd_av1_tile tiles[512];
        for (int i = 0; i < c->av1_tile_count; i++) {
            const VASliceParameterBufferAV1 *tp = &c->av1_tile_param[i];
            const size_t off = tp->slice_data_offset;
            const size_t len = tp->slice_data_size;
            if (len == 0 || off > c->slice_len || off + len > c->slice_len) {
                dmd_log("EndPicture: AV1 tile[%d] is out of bounds (off=%zu len=%zu, "
                        "buffer size %zu). Skipping hardware decode", i, off, len, c->slice_len);
                return NULL;
            }
            tiles[i].data = c->slice_data + off;
            tiles[i].len  = len;
        }

        /* Output buffer: total actual length of the tile load + 4 bytes per tile tile_size + head balance.
         * Use the sum of length of the tile instead of slick_len - the two may differ (in buffer allowed)
         * No gap was quoted in the Tile. */
        size_t tile_bytes = 0;
        for (int i = 0; i < c->av1_tile_count; i++)
            tile_bytes += tiles[i].len;
        const size_t cap = tile_bytes + (size_t)c->av1_tile_count * 4 + 1024;
        unsigned char *buf = malloc(cap);
        if (!buf)
            return NULL;

        size_t n = 0;
        n += dmd_av1_obu_header(DMD_OBU_TEMPORAL_DELIMITER, 0, buf + n, cap - n);

        /* Sequence headings are sent only in front of key frames: AV1 allows repeats, but each is wasted;
         * The sequence-level parameters (size, profile, capability) are not changed in a session, but they are not.
         * Change the driver will rebuild session. */
        const uint32_t ft = pp->pic_info_fields.bits.frame_type;
        /* The KEY frame is to preset an additional sequence header OBU.
         *
         * ⁇  The line in which the KEY frame is judged in this document ** twice ** (the text is identical):
         * This one only decides if the head of the sequence is written, and the other one only decides.
         * whether to submit immediately or stage the frame. The conditions look identical.
         * The 63rd round was to put the switch here, but thought it was a feed route.
         * The line numbers and context must be confirmed before changes are made. */
        if (ft == 0 /* KEY_FRAME */) {
            const size_t sn = dmd_av1_build_sequence_header(pp, buf + n, cap - n);
            if (sn == 0) {
                dmd_log("EndPicture: could not synthesize the AV1 sequence header");
                free(buf);
                return NULL;
            }
            n += sn;
        }

        const size_t frame_off = n;   /* OBU_FRAME Start offset in buff */
        const size_t fn = dmd_av1_build_frame(pp, tiles, c->av1_tile_count,
                                              buf + n, cap - n, &c->av1_dpb);
        if (fn == 0) {
            dmd_log("EndPicture: could not synthesize AV1 OBU_FRAME (%d tiles, %zu payload bytes)",
                    c->av1_tile_count, tile_bytes);
            free(buf);
            return NULL;
        }
        n += fn;

        dmd_log("EndPicture: synthesized %zu AV1 bytes (%s%d tiles, %zu payload bytes)",
                n, ft == 0 ? "Include sequence head," : "", c->av1_tile_count, tile_bytes);

        /* DSD_AV1_DUMP=Trail: The first frame is synthesized and dropped for offline verification.
         *
         * This is the primary means of verification of this module and is worth keeping for the long term: synthetic correctness cannot be used on the device
         * Direct judgment.
         * The bytes are returned to the authority.
         *   ffmpeg-f obu-i < Falling Disk File> -f null - (Can you untie the frame)
         *   ffmpeg-bsf:v trade_headers... (field-by-field shift)
         * The latter is the only viable means of locating individual errors in bits - the self-precision is unreliable.
         * The error was "synchronized" by the fact that the error was made to offset the error.
         *
         * Defaults only to the frame: the frame field is the most complete (including the serial header) and avoids the bursting disk.
         * Replace DMD_AV1_DUMP_AL=1 with the addition** whole synthetic stream** - used to verify multi-frame sequences, and the number of frames used to determine the number of frames.
         * Because the problem of the "first frame is correct but the next frame is the decoder is stopped" is only revealed by the flow:
         * Hand the entire stream to ffmpeg so that the frame number is matched to the source stream. */
        /* Deriving AV1 refresh flags from the next frame
         *
         * The correct value for refresh_frame_flags can only be counter-calculated from ref_frame_map in ** next frame:
         *     N frame refresh = difference between N+1 frame Map and N frame Map
         * Actual 4 frames hit the real source stream (1/8/32/64).
         *
         * The five "only use this frame field" deductions that have been validated -- do not try again:
         *  1. Search for current_frame in this frame ref_frame_map: this array is pre-decoding**
         *     The current frame is not written, it's zero.
         *  2. The score difference between this frame Map and **frame Map: Wrong direction, got the previous frame refresh
         *  Stirring 1 or 1 <slot rotation: structurally unable to express refresh=0 (no slot frame),
         *     Real sequences 1, 8, 32, 64, 0, 0, 64, 0, 32, 64, 0, 0, 32, 64, 0, that's one third of the frame.
         *  4. Map "The value is the minimum slot where the initial value is still and is not quoted in ref_frame_idx":
         *     First frame only (actual guess 1/2/2/2/2 for true value 1/8/32/64)
         *  5. max (filled slot)+2: 0 ⁇ 3 ⁇ 5 ⁇ 6 ⁇ , not established
         * The real writing of the slot sequence 0,3,5,5,6 is the pyramid level decision of the encoder.
         * This information is not included in this frame field.
         *
         * A frame must therefore be delayed. Key premises (extraction from previous round, resulting in miscalculation as structural locks):
         *  • The refresh_frame_flags** do not write to the bitstream** (the norm is supposed to be a full brush),
         *    Without any counter-measures, it must be delivered immediately -- otherwise ffmpeg will stop sending the material without the first frame pixel.
         *    The measurements are "inbound 0 units, copy 0 frames."
         *  . ffmpeg allows multiple frames to fly (six frames sent by DMD_PIPELINE_DEPTH=12),
         *    So the frame is delayed and the lock is not locked.
         *
         * So: KEY frames are sent directly; the frame frames are first entered av1_hold, when the next frame comes
         * Rewrite that eight bits and send it out. */

        /* SEF patch: Show_frame=0 does not output hardware, takes down the DPB slot,
         * The next frame shows_frame=1 sent out with an extra SEF head to bring it back.
         * Slot number before writing dpb_next_slot - is allocated in build_frame.
         * DMD_AV1_NO_SEF=1 Close, easy to compare with old behaviour. */
        /* ⁇  Default closure: SEF plug-in pollutes pixels (68th round of live measurements).
         * 30 frames, same binary:
         *     SEF  ⁇  16 different images (55th round baseline 17, normal shaking)
         *     Turn on SEF & Diff 8
         * Four SEFs are up, so we'll have different images numbering the beams, which will show the frames.
         * Overwrites the contents of other surface -- drivers are not registered for sending,
         * ffmpeg has been taken by its own map, and the wrong surface has been retrieved.
         * The number of frames and the correct rate of synthesis do not detect this degradation (both 150/145).
         * So it's been hidden at the 65-67th round.
         * DMD_AV1_SEF=1 visible opened to continue debug. */
        if (getenv("DMD_AV1_SEF") && ft != 0 &&
            !pp->pic_info_fields.bits.show_frame &&
            pp->pic_info_fields.bits.showable_frame) {
            /* The line is full of marks: the last round is silent.
             * I lost 58 times but there was no sign of it, and I guessed the time series. */
            if (c->av1_sef_count < 64) {
                int t = (c->av1_sef_head + c->av1_sef_count) & 63;
                c->av1_sef_slot[t] = c->av1_dpb.dpb_next_slot & 7u;
                c->av1_sef_count++;
                c->av1_sef_enq++;
            } else {
                c->av1_sef_drop++;
            }
        }

        /* - The 63rd round "unexplainable contradiction" has been identified: my tool was misused -
         * I wanted to get the frame down and put a switch on "ft = 0."
         * But put on the top ** the same name as if for the head of the sequence** (the two lines are identical).
         * So the switch hit 80 times without touching the feed path at all...
         * The three groups are not contradictory in their counting of a single number:** the correct result**.
         * Lessons: Line anchoring before replacing duplicate text, not "one-size-fits-all" instincts.
         *
         * The change itself has not yet been validated and this is the place to be reworked. */
        /* ⁇  Verifyed and not proved at **the right location: let the show_frame=1 frame also be sent.
         *
         * The 63rd wheel was wrongly placed on the same name as "write the head" on the top of the "sequence head," if
         * So the statistics don't change. I mistook them for "unexplainable contradictions."
         * The switch that this round adds after this (real branch of the feed) is actually in effect.
         * Three sets of statistics have changed completely - but the session has collapsed:
         *     EndPicture into 9 (normal 150), Flush sent 2 (normal 75)
         *     ffmpeg output 6 frames (normal 150)
         * Cause: the frame of the show=1 is not sent in av1_hold, but it is not sent directly.
         * The next frame is not "the last frame" to be counterproductive.
         * The chain of delay in a single mechanism was broken and the follow-up module was quickly rejected by hardware.
         *
         * Conclusion: The frame of the show=1 must continue to remain in reserve —
         * Delaying a frame is not optional optimization, but a prerequisite for refresh counter-calculation.
         * So the SEF has to be added to the flush path.
         * Round 62 has proved that the units inserted there are likely to be broken up (150 0011).
         * Both roads were blocked and other mechanisms were needed, not the replacement of hooks. */
        /* - Resume delay of a frame (old)+flush fixe queue -
         *
         * Frame first in av1_hold, paste when next frame arrives, calculates the true source stream
         * Refresh (shadow DBB slots match source by frame, citing both transliteration/translation)
         * Send it again. The consumer will be able to use the sync frame immediately after EndPicture.
         * Surface, temporary frame flushed out early (flush): press refresh=0 to send --
         * Do not take any slots, do not destroy the DPB -- while bytes in the repair queue, next frame
         * Rewrite to real refresh and re-emerge (output sentinding by sentry).
         * So that the frame is immediately available in pixels, and the DPB is refilled in the next frame. */
        if (ft == 0 /* KEY_FRAME */) {
            /* KEY frame without delay: refresh extrapolated by norm (full brush), nowhere to be counted,
             * And ffmpeg will not deliver any more if it does not get the first frame pixel. */
            c->av1_send_surface = c->current_target;
            c->av1_send_show = 1;
            av1_dump_sent(buf, n);
            av1_dump_tagged("k", buf, n);
            *scratch = buf;
            *out_len = n;
            return buf;
        }

        /* Immediate two-pass AV1 submission
         *
         * The problem with the old model is not misconceived, but rather wrong.
         * The consumer will revert to the surface. Actual (RA structure of SVT-AV1), 640x360:
         * Frame 7 is the same frame that was used for the 6 surface = 6, so the same surface is registered.
         * Twice, twice, another flush overlays the bytes of the repair bar 6 --
         * The real refresh 6 is permanently lost, leaving one pending that is never worthy of it.
         * Send 8 to 7 and die 5 seconds after the drop.
         * Directly sending the probe has proved that the 8 units of hardware we actually sent out 8 in and 8 out, and the number of the pieces we sent were the same.
         * The defects are on the side of the account.
         *
         * The new model puts both trips on EndPicture and ** Never-Back-Back-Back-Back-Back-Back-Back-Back-Back-Back-Back-Back-Back-Back-Back
         *   Pixel travel (the frame is sent immediately): refresh writes 0 - no DPB slots are taken, and no DPB slots are used to help people.
         *     Therefore, whatever the source flow value is, it is not possible to destroy the existing slots;
         *     Force simultaneously Disable_frame_end_update_cdf=1, inhibitor of this trip
         *     The frame context returns. Otherwise SetFrameContext will be executed twice.
         *     The second reading has been rewritten for the first time, and it will solve the problem of error of content but bring it with you.
         *     Correct frame refresh, put in DPB and contaminated all subsequent references.
         *   DPB (at the beginning of the next frame, see av1_repair_buf):
         *     Calculate real refresh re-remuneration with "The next map score from the map"
         *     Same frame and put endupd back into real, and finish the only rewriting.
         *     The two starting points are the same  ⁇  pixels are the same one by one.
         *
         * The frame refresh=0 (which is not allowed to take the slot) score 0, the DPB jumps across the board, and the number of the users is not the same.
         * The average cost is therefore less than double. */
        const size_t rep_bitpos =
            c->av1_dpb.last_refresh_bitpos == (size_t)-1
            ? (size_t)-1
            : c->av1_dpb.last_refresh_bitpos + frame_off * 8;
        const size_t rep_endupd =
            c->av1_dpb.last_endupd_bitpos == (size_t)-1
            ? (size_t)-1
            : c->av1_dpb.last_endupd_bitpos + frame_off * 8;

        for (int i = 0; i < 8; i++)
            av1_bit_set(buf, n, rep_bitpos + (size_t)i, 0);

        const int endupd_orig = rep_endupd == (size_t)-1
                              ? -1 : av1_bit_get(buf, n, rep_endupd);
        if (endupd_orig >= 0)
            av1_bit_set(buf, n, rep_endupd, 1);

        /* The bytes of this frame are reserved for the DPB re-emerged from the next **, but the buff itself is this one.
         * EndPicture is sending the main unit free (see free(tx) below),
         * So here it is necessary to** copy** a copy of which does not allow for the transfer of ownership.
         * Do not write directly to av1_repair_buf either. build_unit runs before the DPB pass,
         * Covering it will allow the DPB to re-remix the frame and free (rbuf) to release it.
         * returns the main unit (actually double free). So put the handover slot on. */
        unsigned char *rep = malloc(n);
        free(c->av1_next_repair_buf);
        c->av1_next_repair_buf = rep;
        if (rep) {
            memcpy(rep, buf, n);
            c->av1_next_repair_len = n;
            c->av1_next_repair_bitpos = rep_bitpos;
            c->av1_next_repair_frame = c->av1_dpb.frame_seq;
            c->av1_next_repair_endupd_bitpos = rep_endupd;
            c->av1_next_repair_endupd_val = endupd_orig;
        } else {
            c->av1_next_repair_len = 0;
            c->av1_next_repair_bitpos = (size_t)-1;
            c->av1_next_repair_frame = 0;
            c->av1_next_repair_endupd_bitpos = (size_t)-1;
            c->av1_next_repair_endupd_val = -1;
        }

        c->av1_send_surface = c->current_target;
        c->av1_send_show = 1;
        av1_dump_sent(buf, n);
        av1_dump_tagged("n", buf, n);
        *scratch = buf;
        *out_len = n;
        return buf;
    }

    case DMD_CODEC_VP8: {
        size_t cap = c->slice_len + 16;
        unsigned char *buf = malloc(cap);
        if (!buf)
            return NULL;
        size_t n = vp8_build_frame(c, buf, cap);
        if (n == 0) {
            free(buf);
            return NULL;
        }
        *scratch = buf;
        *out_len = n;
        return buf;
    }

    case DMD_CODEC_MPEG2: {
        /* The assembly order (levels of § 6.1.1 of 13818-2):
         *   [Sequence_header + branch_extension]  ⁇
         *   Picure_header + picture_code_extension
         *   slick byte (slick data from VA, including slick_start_code)
         *
         * ⁇ Slice data with start code: VA-API norms not mandatory, ffmpeg is measured
         * vaapi_mpeg2.c started with slick_start_code (000001.01.AF)
         * Complete slick. So, no start-up, just spell it. If a consumer doesn't have it,
         * The sign is that the decoder can't find slick — no output, not bad images. */
        if (!c->have_mpeg2_pic_param)
            return NULL;
        if (!dmd_mpeg2_can_build(&c->mpeg2_pic_param))
            return NULL;

        /* Head ceiling: maximum size_header 12 bytes + 4 group 64 bytes matrix
         * +Segment_extension 10 bytes + picture head ~16 bytes, take 320 sqt. */
        unsigned char hdr[320];
        size_t hn = 0;

        const unsigned int w = c->mpeg2_pic_param.horizontal_size;
        const unsigned int h = c->mpeg2_pic_param.vertical_size;
        if (!c->mpeg2_seq_sent || c->sent_mbs_wide != w ||
            c->sent_mbs_high != h) {
            size_t sn = dmd_mpeg2_build_sequence(
                &c->mpeg2_pic_param,
                c->have_mpeg2_iq_matrix ? &c->mpeg2_iq_matrix : NULL,
                hdr, sizeof(hdr));
            if (sn == 0)
                return NULL;
            hn = sn;
            c->mpeg2_seq_sent = 1;
            c->sent_mbs_wide = w;
            c->sent_mbs_high = h;
        }

        size_t pn = dmd_mpeg2_build_picture(&c->mpeg2_pic_param,
                                            c->mpeg2_temporal_ref,
                                            hdr + hn, sizeof(hdr) - hn);
        if (pn == 0)
            return NULL;
        hn += pn;
        c->mpeg2_temporal_ref = (c->mpeg2_temporal_ref + 1) & 0x3FF;

        unsigned char *buf = malloc(hn + c->slice_len);
        if (!buf)
            return NULL;
        memcpy(buf, hdr, hn);
        memcpy(buf + hn, c->slice_data, c->slice_len);

        /* DMD_MPEG2_DUMP=Trail: Adds the results to the drop-down in order of delivery.
         * The synthetic correctness cannot be judged directly on the device (the frame is not solved without telling you which is wrong).
         * The only reliable validation is to feed the falling disc file to the reference decoder (ffmpeg/mpeg2dec)
         * See if it can solve the image that is consistent with the soft resolution -- with the ADD_AV1_DUMP. */
        const char *mp2dump = getenv("DMD_MPEG2_DUMP");
        if (mp2dump && mp2dump[0]) {
            FILE *df = fopen(mp2dump, "ab");
            if (df) {
                fwrite(buf, 1, hn + c->slice_len, df);
                fclose(df);
            }
        }

        *scratch = buf;
        *out_len = hn + c->slice_len;
        return buf;
    }

    case DMD_CODEC_H264: {
        /* The slick data is the **full original NAL header ** (including NAL header and slice header),
         * No starting code is available. Verified: h264dec.c: 674 by nal->raw_data/raw_size,
         * h2645_parse.c: 92/145 raw_data points to the starting code and retains the transption bytes,
         * h2645_parse.c: 448-456 reading nal header from gb
         * vaapi_h264.c: 386-388 as originally slice data buffer.
         * So here only four bytes are filled, no slick header reconstruction...
         * Reorder with MMCO commands which are originally in the original byte, MediaCodec parsing itself. */
        size_t cap = c->slice_len + 4;
        unsigned char *buf = malloc(cap);
        if (!buf)
            return NULL;
        buf[0] = 0x00;
        buf[1] = 0x00;
        buf[2] = 0x00;
        buf[3] = 0x01;
        memcpy(buf + 4, c->slice_data, c->slice_len);
        *scratch = buf;
        *out_len = cap;
        return buf;
    }

    /* On-device validation of the direct V4L2 HEVC path
     * Equipment SM8750/msm_vidc/dev/video32, without root, 2026-08 in containers.
     * Material 1920x1080 25fps HEVC Main yuv420p(tv) 12 frames (14281 bytes).
     *
     * Frame: Hardly 12/ Soft 12, all 12/12 repeats three times.
     * Feed: Incoming 15 units received 12 frames - 3 extra units VPS/SPS/ PPS
     *       Parameter set nal, which does not produce images per se, is normal.
     * Logs can be found SOURCE_CHANGE and S_FMT(OUTPUT) 1920x1088
     *       (V4L2 at 16 alignment, 1080 — 1088), consultations are normal.
     *
     * ⁇  First frame pixel** not verified ** point-by-point for:
     *   Y-Pill 797 lines are the same as soft by-bits, with differences in lines 797-1079 (283 lines),
     *   The max. 40 (example: 900 hw=0x70 sw=0x52) and UV plane also varies.
     *   Excluded: tv ⁇ full colour range conversion (by (v-16) *2 55/219 forecast does not match,
     *   and the margin is positive and negative, the back inverted, horizontal/vertical spatial shifts (best match falls)
     *   Search for boundary ~40 is a false match from single gradients rather than a real shift).
     *   - wwoccle_output_format vaapi,
     *   Otherwise, output 0 bytes -- this is a ffmpeg problem, not a driver defect.
     *   To be checked: CapTure surface NV12 stride/UV is being treated at a non-16-fold height.
     *   And if the COMPOSE selection was missing at 1088 pegged back to 1080.
     */
    case DMD_CODEC_HEVC: {
        if (c->hevc_pic_param_valid && c->hevc_pic_param.num_short_term_ref_pic_sets) {
            if (!dmd_hevc_can_build(&c->hevc_pic_param) || c->hevc_slice_error ||
                !c->hevc_slice_count || c->hevc_slices_bound != c->hevc_slice_count ||
                c->slice_len > (SIZE_MAX - (size_t)c->hevc_slice_count * 2048) / 2) {
                dmd_log("HEVC RPS: unsupported picture or missing long slice boundaries\n");
                return NULL;
            }
            size_t cap = c->slice_len * 2 + (size_t)c->hevc_slice_count * 2048;
            uint8_t *buf = malloc(cap);
            if (!buf) return NULL;
            size_t total = 0;
            if (dmd_hevc_rewrite_slices(&c->hevc_pic_param, c->hevc_slice_param,
                    c->hevc_slice_offset, c->hevc_slice_count, c->slice_data,
                    c->slice_len, buf, cap, &total) < 0) {
                dmd_log("HEVC RPS: slice rewrite rejected: %s\n", strerror(errno));
                free(buf);
                return NULL;
            }
            *scratch = buf;
            *out_len = total;
            return buf;
        }
        /* No SPS-resident ST sets: keep the existing inline-RPS forwarding
         * path. The original slice header and CABAC bytes are already present. */
        size_t cap = 4 + c->slice_len;
        unsigned char *buf = malloc(cap);
        if (!buf)
            return NULL;
        buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 1;
        memcpy(buf + 4, c->slice_data, c->slice_len);
        *scratch = buf;
        *out_len = cap;
        return buf;
    }

    default:
        return NULL;
    }
}

/* HEVC: Send synthetic VPS/SPS/PPS to Damon before the first slice.
 *
 * The difference with H.264 is that there are three argument sets instead of two and must be in the order of VPS ⁇ PS ⁇ PPS —
 * id of the SPS reference to VPS, id of the PPS reference to SPS.
 * Each is sent as a stand-alone data unit (daemon is recognized and accumulated as CSD by nal_unit_header).
 *
 * Caller should not hold the lock (where it is blocked). Returns 0 successful. */
static int hevc_send_param_sets(struct dmd_session *sess,
                                const VAPictureParameterBufferHEVC *pp,
                                VAProfile profile)
{
    unsigned char nalu[600];

    size_t n = dmd_hevc_build_vps_nalu(pp, profile, nalu, sizeof(nalu));
    if (n == 0) {
        dmd_log("HEVC: could not synthesize the VPS\n");
        return -1;
    }
    if (dmd_session_send_unit(sess, nalu, n) != DMD_OK) {
        dmd_log("HEVC: could not submit the VPS: %s\n", dmd_session_last_error(sess));
        return -1;
    }
    dmd_log("HEVC: sent VPS %zu bytes\n", n);

    n = dmd_hevc_build_sps_nalu(pp, profile, nalu, sizeof(nalu));
    if (n == 0) {
        dmd_log("HEVC: could not synthesize the SPS\n");
        return -1;
    }
    if (dmd_session_send_unit(sess, nalu, n) != DMD_OK) {
        dmd_log("HEVC: could not submit the SPS: %s\n", dmd_session_last_error(sess));
        return -1;
    }
    dmd_log("HEVC: sent SPS %zu bytes\n", n);
    /* Prints the original bytes of the synthetic SPS at DMD_CSD_DUMP=1.
     * It is needed because different extraction values are often the same number under the ue(v) encoding (e. g. 2 and 7 are 5 bits), and the same number is required because the number of values is usually the same (e. g. 2 and 7 are 5 bits).
     * Seeing the length of the NALU does not make it clear that the changes actually entered the bits stream -- which is what it was measured to have been.
     * "The switch is not effective" is misread as "the value is taken without effect". */
    if (getenv("DMD_CSD_DUMP")) {
        char hex[3 * 64 + 1];
        size_t m = n < 64 ? n : 64, k;
        for (k = 0; k < m; k++)
            snprintf(hex + k * 3, 4, "%02x ", nalu[k]);
        hex[m ? m * 3 - 1 : 0] = '\0';
        dmd_log("HEVC SPS bytes: %s\n", hex);
    }

    n = dmd_hevc_build_pps_nalu(pp, nalu, sizeof(nalu));
    if (n == 0) {
        dmd_log("HEVC: could not synthesize the PPS\n");
        return -1;
    }
    if (dmd_session_send_unit(sess, nalu, n) != DMD_OK) {
        dmd_log("HEVC: could not submit the PPS: %s\n", dmd_session_last_error(sess));
        return -1;
    }
    dmd_log("HEVC: sent PPS %zu bytes\n", n);

    return 0;
}

/* H.264: Send synthetic SPS/PPS to Damon before the first slice.
 *
 * daemon locates nal_unit_header recognition parameters by starting code and accumulates into code config
 * (input thread for decode-daemon.c), encountered with the first non-parameter set
 * FLAG_CODEC_CONFIG sent. So SPS and PPS must have ** each as a separate data module**
 * Send first, and must be before the first VCL NALU.
 *
 * Caller should not hold the lock (where it is blocked). Returns 0 successful. */
static int h264_send_param_sets(struct dmd_session *sess,
                                const VAPictureParameterBufferH264 *pp,
                                const VAIQMatrixBufferH264 *iq, int have_iq,
                                const VASliceParameterBufferH264 *sp,
                                unsigned int def_l0, unsigned int def_l1,
                                VAProfile profile, unsigned int disp_width,
                                unsigned int disp_height)
{
    unsigned char nalu[512];

    size_t n = dmd_h264_build_sps_nalu(pp, profile, disp_width, disp_height,
                                       nalu, sizeof(nalu));
    if (n == 0) {
        dmd_log("H.264: could not synthesize the SPS\n");
        return -1;
    }
    if (dmd_session_send_unit(sess, nalu, n) != DMD_OK) {
        dmd_log("H.264: could not submit the SPS: %s\n", dmd_session_last_error(sess));
        return -1;
    }
    dmd_log("H.264: sent SPS %zu bytes\n", n);

    n = dmd_h264_build_pps_nalu(pp, iq, have_iq, sp, def_l0, def_l1,
                                nalu, sizeof(nalu));
    if (n == 0) {
        dmd_log("H.264: could not synthesize the PPS\n");
        return -1;
    }
    if (dmd_session_send_unit(sess, nalu, n) != DMD_OK) {
        dmd_log("H.264: could not submit the PPS: %s\n", dmd_session_last_error(sess));
        return -1;
    }
    dmd_log("H.264: sent PPS %zu bytes\n", n);

    return 0;
}

/* Is there a "export dmabuf" surface in the code to be decoded queue of the context?
 *
 * Chrome surfaces the entire pool one by one before any decoding,
 * So, as long as the sequence shows a record of export, it means that the consumer belongs to the "self sample dmabuf" category.
 * EndPicture must be synchronized with pixels into the surface before returning.
 * ffmpeg never exports (it goes GetImage/DeriveImage with dmd_surface_wait)
 * Here, there were zero hits, so save the memcpy that halved the spitting. */
static int dmd_pending_has_exported_locked(struct dmd_driver *drv,
                                           struct dmd_context *c)
{
    for (int k = 0; k < c->pending_count; k++) {
        int kp = (c->pending_head + k) % DMD_MAX_SURFACES;
        struct dmd_surface *s = dmd_find_surface_locked(drv, c->pending[kp]);
        if (s && s->exported)
            return 1;
    }
    return 0;
}

/* Never let late pixels overwrite an exported target already recycled by the
 * consumer. The old guard waited for those pixels before submitting new input,
 * which can starve a stateful decoder that needs later input to release them.
 * Keep the old unit identity so that output is consumed and returned once.
 *
 * A blanket busy response is not a replacement: earlier FFmpeg/Firefox tests
 * failed on reuse because readback did not reset surface state. Only retire
 * older exported H.264/HEVC targets; preserve non-exported synchronous reads. */
static void retire_reused_target_locked(struct dmd_driver *drv,
                                        struct dmd_context *context,
                                        VASurfaceID target)
{
    if (context->codec != DMD_CODEC_HEVC && context->codec != DMD_CODEC_H264)
        return;
    struct dmd_surface *surface = dmd_find_surface_locked(drv, target);
    if (!surface || !surface->exported) return;
    for (int offset = 0; offset < context->pending_count; offset++) {
        int index = (context->pending_head + offset) % DMD_MAX_SURFACES;
        if (context->pending[index] != target) continue;
        dmd_log("recycle surface=%u: retain old unit=%llu for discard\n",
                (unsigned)target,
                (unsigned long long)context->pending_unit[index]);
        context->pending[index] = DMD_DISCARD_SURFACE;
    }
}

/* Called with drv->lock held. Encoding may wait for firmware, so it uses owned
 * snapshots outside the object-table lock. Neither DestroyBuffer nor context
 * teardown can invalidate memory used by the in-flight ioctl path. */
static void encode_output_error_locked(struct dmd_driver *drv,
                                       struct dmd_context *c, VAStatus status)
{
    struct dmd_buffer *b = dmd_find_buffer_locked(drv,
                                 dmd_encode_coded_buffer(c->encoder));
    if (b && b->context == c->id && b->type == VAEncCodedBufferType &&
        !b->mapped && !b->coded_pending) {
        memset(&b->coded_segment, 0, sizeof(b->coded_segment));
        b->coded_segment.buf = b->data;
        b->coded_segment.status = VA_CODED_BUF_STATUS_BAD_BITSTREAM;
        b->coded_status = status;
    }
}

static VAStatus encode_abort_locked(struct dmd_driver *drv, struct dmd_context *c,
                                     VAStatus status)
{
    encode_output_error_locked(drv, c, status);
    dmd_encode_abort(c->encoder);
    c->current_target = VA_INVALID_ID;
    return status;
}

static VAStatus encode_end_locked(struct dmd_driver *drv, struct dmd_context *c)
{
    const VAContextID context = c->id;
    const int idx = (int)(c - drv->contexts);
    if (drv->io_busy[idx])
        return VA_STATUS_ERROR_HW_BUSY;
    struct dmd_surface *source = dmd_find_surface_locked(drv, c->current_target);
    VABufferID bid = dmd_encode_coded_buffer(c->encoder);
    struct dmd_buffer *coded = dmd_find_buffer_locked(drv, bid);
    if (!source)
        return encode_abort_locked(drv, c, VA_STATUS_ERROR_INVALID_SURFACE);
    if (surface_images_locked(drv, source->id, 1))
        return encode_abort_locked(drv, c, VA_STATUS_ERROR_SURFACE_BUSY);
    if (bid == VA_INVALID_ID) {
        VAStatus status = dmd_encode_end(c->encoder, NULL, NULL);
        c->current_target = VA_INVALID_ID;
        return status;
    }
    if (!coded || coded->context != context || coded->type != VAEncCodedBufferType)
        return encode_abort_locked(drv, c, VA_STATUS_ERROR_INVALID_BUFFER);
    if (coded->mapped || coded->coded_pending)
        return encode_abort_locked(drv, c, VA_STATUS_ERROR_HW_BUSY);
    if (source->state == DMD_SURFACE_PENDING)
        return encode_abort_locked(drv, c, VA_STATUS_ERROR_HW_BUSY);

    struct dmd_surface snapshot = *source;
    if (source->visible_x || source->visible_y)
        return encode_abort_locked(drv, c, VA_STATUS_ERROR_UNIMPLEMENTED);
    if (source->visible_width)
        snapshot.width = source->visible_width;
    if (source->visible_height)
        snapshot.height = source->visible_height;
    snapshot.data = malloc(source->data_size);
    struct dmd_buffer result = *coded;
    result.data = malloc(coded->size);
    if (!snapshot.data || !result.data) {
        free(snapshot.data);
        free(result.data);
        return encode_abort_locked(drv, c, VA_STATUS_ERROR_ALLOCATION_FAILED);
    }
    const int needs_sync = source->exportable && source->dumb_sync_fd >= 0;
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    if (needs_sync && ioctl(source->dumb_sync_fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        free(snapshot.data);
        free(result.data);
        return encode_abort_locked(drv, c, VA_STATUS_ERROR_OPERATION_FAILED);
    }
    if (!source->imported)
        memcpy(snapshot.data, source->data, source->data_size);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    int sync_error = needs_sync &&
        ioctl(source->dumb_sync_fd, DMA_BUF_IOCTL_SYNC, &sync) != 0;
    snapshot.exportable = 0;
    snapshot.dumb_sync_fd = -1;
    if (sync_error) {
        free(snapshot.data);
        free(result.data);
        return encode_abort_locked(drv, c, VA_STATUS_ERROR_OPERATION_FAILED);
    }
    memset(&result.coded_segment, 0, sizeof(result.coded_segment));
    result.coded_segment.buf = result.data;
    struct dmd_encode *encoder = c->encoder;
    c->current_target = VA_INVALID_ID;
    coded->coded_pending = 1;
    coded->coded_status = VA_STATUS_ERROR_HW_BUSY;
    memset(&coded->coded_segment, 0, sizeof(coded->coded_segment));
    coded->coded_segment.buf = coded->data;
    drv->io_busy[idx] = 1;
    dmd_import_ref(snapshot.imported);

    pthread_mutex_unlock(&drv->lock);
    VAStatus status = snapshot.imported ?
        dmd_import_copy(&snapshot, snapshot.data, snapshot.data_size) : VA_STATUS_SUCCESS;
    dmd_import_unref(snapshot.imported);
    snapshot.imported = NULL;
    if (status == VA_STATUS_SUCCESS) {
        status = dmd_encode_end(encoder, &snapshot, &result);
    } else {
        dmd_encode_abort(encoder);
        result.coded_segment.status = VA_CODED_BUF_STATUS_BAD_BITSTREAM;
    }
    free(snapshot.data);
    pthread_mutex_lock(&drv->lock);

    c = dmd_find_context_locked(drv, context);
    coded = dmd_find_buffer_locked(drv, bid);
    if (c && coded && coded->context == context) {
        if (status == VA_STATUS_SUCCESS && result.coded_segment.size <= coded->size) {
            memcpy(coded->data, result.data, result.coded_segment.size);
            coded->coded_segment = result.coded_segment;
            coded->coded_segment.buf = coded->data;
            coded->coded_segment.next = NULL;
        } else {
            if (status == VA_STATUS_SUCCESS)
                status = VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
            coded->coded_segment.size = 0;
            coded->coded_segment.status = result.coded_segment.status;
        }
        coded->coded_status = status;
        coded->coded_pending = 0;
        if (status == VA_STATUS_SUCCESS)
            dmd_video_stats_output(c->video_stats, 0);
    } else {
        status = c ? VA_STATUS_ERROR_INVALID_BUFFER : VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    free(result.data);
    io_release(drv, idx);
    return status;
}

static VAStatus end_picture(VADriverContextP ctx, VAContextID context)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (c->current_target == VA_INVALID_ID) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (c->entrypoint == VAEntrypointVideoProc) {
        pthread_mutex_unlock(&drv->lock);
        return dmd_vpp_end(ctx, context);
    }

    if (c->encoder) {
        VAStatus status = encode_end_locked(drv, c);
        pthread_mutex_unlock(&drv->lock);
        return status;
    }

    VASurfaceID target = c->current_target;
    int idx = (int)(c - drv->contexts);

    /* Assembly stream (locked, pure memory). */
    unsigned char *scratch = NULL;
    size_t unit_len = 0;
    if (c->codec == DMD_CODEC_AV1) c->av1_ep_enter++;
    const unsigned char *unit = build_unit(c, &scratch, &unit_len);
    if (c->codec == DMD_CODEC_AV1) {
        if (unit) {
            c->av1_ep_unit++;
            if (c->av1_send_show) c->av1_ep_show1++;
        } else {
            c->av1_ep_null++;
        }
    }

    /* ================================================================
     * Show_frame=0 The pit has been filled: show_frame constant 1 in the synthetic frame
     * ================================================================
     * The symptoms were left behind, and no more to-do: msm_vidc only vomits on the frame show_frame=1
     * Buffer (right action), while the VA-API backend for ffmpeg will ** in part ** show_frame=0
     * So, the surface mode Sync read pixels and drive this kind of surface permanently to PENDING, so...
     * "Failed to read image from surface 0x5:internal"
     * Decoding error, hardened in frame 2.
     * The actual data map (bytes) of the world's environment:
     *   surface1 19258B oh= 0 show=1   surface2  2688B oh=16 show=0
     *   surface3   649B oh= 8 show=0   surface4   245B oh= 4 show=0
     *   surface5   183B oh= 2 show=0   surface6    87B oh= 1 show=1
     * ffmpeg only Sync has 1, 6, 5, 3: the first two pairs were successful, and the surface 5 (show=0) was not available
     * Pixels. Registration is fully consistent with the dispatch itself, with no double registration (there has been a suspicion that this is the proof of the fact of the fact).
     *
     * Now every submission unit produces an output, so every Sync surface is equipped with a frame.
     * Review (GOP=300 1080p Real 300 Frame, Cut300.obu): 300 frames delivered, 300 frames delivered, 300 frames delivered, 300 frames delivered, 300 frames
     * 149 Sync, 0 overtime, 0 "citation improbable", rc=0.
     * The 149th flow show_frame=1 frame equals the size of the frame --ffmpeg is only for the frame to be displayed
     * Pixels, this part is expected behavior.
     * Another entry in the log is "Surface_wait:Surface1 Never submitted Decoding, refused to read" in position
     * After the CreateSurfaces, any decoding before it is a ffmpeg Detectory Reading, not a defect.
     * ================================================================ */

    /* - AV1 Second: DPB (remuneration of previous frame)
     *
     * Previous frame pixels at refresh=0,disable_frame_end_update_cdf=1 out
     * (no slots, no frame context) at this point in the EndPicture
     * Catch_prev_refresh has already set the " next map score to this frame" -- i.e. source stream.
     * Real refresh -- write to the repair copy, send it again here, let the hardware DPB fill it out.
     * The correct slot and complete the only frame context.
     * Re-issued output by sentry pending (surface=0xFF#FE, id never exists)
     * Discarded during pairings does not affect the alignment of the frame. */
    if (c->codec == DMD_CODEC_AV1 && c->av1_repair_buf && c->session) {
        /* The entire slot of the previous frame is removed and the frame's handover slot is moved in unconditionally.
         * Unconditional move -- no one can give up the delivery without a frame bytes.
         * Stay where you are and be sent out again. */
        unsigned char *rbuf = c->av1_repair_buf;
        const size_t rlen = c->av1_repair_len;
        const size_t rbp  = c->av1_repair_bitpos;
        const size_t rend_bp = c->av1_repair_endupd_bitpos;
        const int    rend_v  = c->av1_repair_endupd_val;
        c->av1_repair_buf = NULL;
        c->av1_repair_len = 0;
        c->av1_repair_bitpos = (size_t)-1;
        c->av1_repair_frame = 0;
        c->av1_repair_endupd_bitpos = (size_t)-1;
        c->av1_repair_endupd_val = -1;
        c->av1_repair_buf = c->av1_next_repair_buf;
        c->av1_repair_len = c->av1_next_repair_len;
        c->av1_repair_bitpos = c->av1_next_repair_bitpos;
        c->av1_repair_frame = c->av1_next_repair_frame;
        c->av1_repair_endupd_bitpos = c->av1_next_repair_endupd_bitpos;
        c->av1_repair_endupd_val = c->av1_next_repair_endupd_val;
        c->av1_next_repair_buf = NULL;
        c->av1_next_repair_len = 0;
        c->av1_next_repair_bitpos = (size_t)-1;
        c->av1_next_repair_frame = 0;
        c->av1_next_repair_endupd_bitpos = (size_t)-1;
        c->av1_next_repair_endupd_val = -1;

        /* The value is exactly the same as the one that was sent.
         * The endupd should have been one.
         * These frames are a significant proportion of the RA structure and save half the decoding. */
        int need = 0;
        if (rbp != (size_t)-1) {
            for (int i = 0; i < 8; i++)
                if (av1_bit_get(rbuf, rlen, rbp + (size_t)i) == 1) { need = 1; break; }
        }
        if (rend_bp != (size_t)-1 && rend_v == 0)
            need = 1;

        /* Revertable real value: pixels are forced to 1 to
         * Reprinting of the frame frame context, the only one to be completed. */
        if (rend_bp != (size_t)-1 && rend_v >= 0)
            av1_bit_set(rbuf, rlen, rend_bp, rend_v);

        /* DMD_AV1_NO_REPAIR=1: Skip DPB and send only pixels.
         * The pure diagnostic switch is used to determine whether "hardware swallows a unit" is decoded by the same frame twice.
         * And the price is that the DPB slots will not be fixed and pixels will deteriorate over time. */
        static int no_repair = -1;
        if (no_repair < 0) {
            const char *e = getenv("DMD_AV1_NO_REPAIR");
            no_repair = (e && *e && *e != '0') ? 1 : 0;
        }
        if (need && !no_repair && c->pending_count < DMD_MAX_SURFACES) {
            const int ridx = (int)(c - drv->contexts);
            /* Ordered requirement: IO ownership first, ** then sentry registration, **session capture
             * Pointer. The reverse is the case (registration, blinding io_busy, locking), waiting and unlocking.
             * The window is open to people who can take the session out or plug into their own frame. */
            int can_send = 0;
            if (io_acquire(drv, ridx, "EndPicture/AV1 Fix Remix") == 0) {
                can_send = context_slot_ok(c, context) && c->session &&
                           c->pending_count < DMD_MAX_SURFACES;
                if (!can_send)
                    io_release(drv, ridx);
            }
            if (can_send) {
                struct dmd_session *rsess = c->session;
                int rq = (c->pending_head + c->pending_count) % DMD_MAX_SURFACES;
                c->pending[rq] = DMD_DISCARD_SURFACE;      /* Consume without writing a target. */
                c->pending_seq[rq] = c->last_seq;
                c->pending_poc[rq] = INT32_MAX;
                c->pending_unit[rq] = ++c->units_submitted;
                c->pending_count++;
                av1_dump_tagged("2", rbuf, rlen);
                dmd_log("EndPicture: resubmitting %zu AV1 repair bytes (sentinel unit %llu)",
                        rlen, (unsigned long long)c->pending_unit[rq]);
                pthread_mutex_unlock(&drv->lock);
                (void)dmd_session_send_unit(rsess, rbuf, rlen);
                pthread_mutex_lock(&drv->lock);
                io_release(drv, ridx);
                c = dmd_find_context_locked(drv, context);
                free(rbuf);
                if (!c) {
                    pthread_mutex_unlock(&drv->lock);
                    free(scratch);
                    return VA_STATUS_ERROR_INVALID_CONTEXT;
                }
            } else {
                free(rbuf);
                dmd_log("EndPicture: skipping the DPB repair pass "
                        "(I/O ownership unavailable, slot reused, or queue full)\n");
            }
        } else {
            if (need && getenv("DMD_AV1_LOG"))
                dmd_log("EndPicture: skipping the AV1 DPB pass because the queue is full (%d)\n",
                        c->pending_count);
            free(rbuf);
        }
    } else if (c->codec == DMD_CODEC_AV1 && c->av1_next_repair_buf) {
        /* The convection slot is also advanced when there is no session or the buffer is always stacked up. */
        free(c->av1_repair_buf);
        c->av1_repair_buf = c->av1_next_repair_buf;
        c->av1_repair_len = c->av1_next_repair_len;
        c->av1_repair_bitpos = c->av1_next_repair_bitpos;
        c->av1_repair_frame = c->av1_next_repair_frame;
        c->av1_repair_endupd_bitpos = c->av1_next_repair_endupd_bitpos;
        c->av1_repair_endupd_val = c->av1_next_repair_endupd_val;
        c->av1_next_repair_buf = NULL;
        c->av1_next_repair_len = 0;
        c->av1_next_repair_bitpos = (size_t)-1;
        c->av1_next_repair_frame = 0;
        c->av1_next_repair_endupd_bitpos = (size_t)-1;
        c->av1_next_repair_endupd_val = -1;
    }

    if (!unit) {
        struct dmd_surface *s = dmd_find_surface_locked(drv, target);
        if (c->av1_ref_drop_now) {
            /* The missing frame of reference is missing: it can't be returned like "not supporting the flow."
             * UNIMPLEMENTED - vaEndPicture misreporting will allow the caller to determine that the whole stream has failed
             * (ffmpeg actual rc=251, 0 frame). This is the same one as show_frame=0.
             * Semantics: The surface mark is a record of the last vestige of this surface.
             * The process continues, the next KEY in the stream returns to normal as soon as it arrives. */
            c->av1_ref_drop_now = 0;
            if (s) {
                s->state = DMD_SURFACE_READY;
                s->decode_status = VA_STATUS_SUCCESS;
                dmd_video_stats_reuse(c->video_stats);
            }
            c->current_target = VA_INVALID_ID;
            c->slice_len = 0;
            c->av1_tile_count = 0;
            pthread_mutex_unlock(&drv->lock);
            free(scratch);
            return VA_STATUS_SUCCESS;
        }
        if (s) {
            s->state = DMD_SURFACE_IDLE;
            s->decode_status = VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        dmd_video_stats_error(c->video_stats);
        c->current_target = VA_INVALID_ID;
        c->slice_len = 0;
        c->av1_tile_count = 0;
        int codec = c->codec;
        pthread_mutex_unlock(&drv->lock);
        free(scratch);
        dmd_log("EndPicture: bitstream reconstruction is unsupported for codec=%d. Dropping the frame\n", codec);
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }

    /* Sessions are closed (flush over), but the feed is sent upstream - transparent for a new session.
     *
     * This path covers **seek**: daemon without connection reset, Flush is irreversible
     * SHUT_WR, so the player jumps and continues to decode only to recreate the session.
     * The blog also has a blog that says that the "shows can be made immediately from the IDR framework, but the IDR framework is not the only one that can be used to create a new one."
     * There will be no visible screens." -- That conclusion** was wrong.** (The probes are not looking at the screens, but the numbers are not.)
     * The frames are black and Y=16, and the probe begins with self-declared.
     * The re-establishment actually destroyed the reference chain and then continued to blacken the next IDR.
     * The cost is generally acceptable in the context of the already-screened video (the player would have expected the image to be updated) and the cost of the video would have been more than enough to be used in the video.
     * But don't ** treat it as a basis for "rebuilding harmless."
     *
     * Note: Firefox's normal play ** won't get here ** -- once it's in place, it's not going to get here **
     * The vaSyncSurface is stuck directly in the DestroyContext, and no longer in EndPicture.
     * The lock was avoided from the roots by the DMD_FLUSH_AFTER_MS + DMD_PIPELINE_DEPTH,
     * This path is only the bottom of the seek/outside scenario and has not been authenticated by the real seek end. */
    if (c->session && c->input_finished) {
        dmd_log("EndPicture: new input after the session write end closed. Rebuilding the session\n");
        if (session_rebuild_locked(drv, c, idx) < 0) {
            c = dmd_find_context_locked(drv, context);
            if (!c) {
                pthread_mutex_unlock(&drv->lock);
                free(scratch);
                return VA_STATUS_ERROR_INVALID_CONTEXT;
            }
        } else {
            /* The locks were not lifted during the reconstruction and the targets may have been destroyed and repositioned. */
            c = dmd_find_context_locked(drv, context);
            if (!c) {
                pthread_mutex_unlock(&drv->lock);
                free(scratch);
                return VA_STATUS_ERROR_INVALID_CONTEXT;
            }
        }
    }

    /* Retrying session: The dateContext may not be up yet. */
    if (!c->session && !c->session_failed) {
        /* Impossible: session_filed and session complement each other. Conservative treatment. */
        c->session_failed = 1;
    }

    struct dmd_session *sess = c->session;
    int codec = c->codec;
    unsigned int pw = c->picture_width;
    unsigned int ph = c->picture_height;

    if (!sess) {
        /* Lock the re-build session to avoid locking to do contract.
         * The IO ownership is also required before the building session: if someone takes this slot, we blindly take it.
         * Io_busy return it, it's like taking the other side's ownership.
         * Besides, ten_bit must read before lock** - it turns out it's read after locking.
         * `c->output_ten_bit ' , that c may have changed. */
        const int retry_ten_bit = c->output_ten_bit;
        if (io_acquire(drv, idx, "EndPicture/Build Session") < 0) {
            struct dmd_surface *s = dmd_find_surface_locked(drv, target);
            if (s) {
                s->state = DMD_SURFACE_IDLE;
                s->decode_status = VA_STATUS_ERROR_OPERATION_FAILED;
            }
            c->current_target = VA_INVALID_ID;
            c->slice_len = 0;
            c->av1_tile_count = 0;
            pthread_mutex_unlock(&drv->lock);
            free(scratch);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (!context_slot_ok(c, context)) {
            io_release(drv, idx);
            pthread_mutex_unlock(&drv->lock);
            free(scratch);
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
        pthread_mutex_unlock(&drv->lock);
        struct dmd_session *retry = session_open(codec, pw, ph, retry_ten_bit);
        pthread_mutex_lock(&drv->lock);
        io_release(drv, idx);
        c = dmd_find_context_locked(drv, context);
        if (!c) {
            pthread_mutex_unlock(&drv->lock);
            if (retry)
                dmd_session_destroy(retry);
            free(scratch);
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
        if (!c->session) {
            c->session = retry;
            dmd_session_set_video_stats(retry, c->video_stats);
            c->session_failed = retry ? 0 : 1;
        } else if (retry) {
            dmd_session_destroy(retry); /* The other threads are set up first. */
        }
        sess = c->session;
        if (!sess) {
            struct dmd_surface *s = dmd_find_surface_locked(drv, target);
            if (s) {
                s->state = DMD_SURFACE_IDLE;
                s->decode_status = VA_STATUS_ERROR_OPERATION_FAILED;
            }
            c->current_target = VA_INVALID_ID;
            c->slice_len = 0;
            c->av1_tile_count = 0;
            pthread_mutex_unlock(&drv->lock);
            free(scratch);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    /* * Before the locking **, the queue is strictly equal to the submission order.
     * If the send fails, then the roll back. */
    /* The line is full ** a frame** frees up the slots, not a direct failure.
     *
     * ⁇  There was a direct return to OPERATION_FAILED, which is the root of the non-useable hard-tope of chrome:
     *
     * Chrome doesn't tune vSyncSurface to frame sync like ffmpeg/ Firebox, but...
     * The queue is filled up quickly.
     * EndPicture Wronged  ⁇  Chrome Received "operation failed"  ⁇  Ruling that hard resolution is not available
     * ⁇  Disconnect (Broken Pipe/ Conaction reset is the result seen on the side of daemon).
     *
     * The time line is confirmed: Chrome reported immediately to the vaEndPicture family at the full time of the queue.
     * And all 50 pairs occurred after ** failed in DestroyContext **-
     * That means the frame was never consumed in time during the play, forming a dead lock:
     * Chrome waits for the surface ready to be ready to be in a collection frame and the frame is blocked in full queue.
     *
     * The queue is ** back pressure** is not a mistake: the frame is still on the way, and there is room for it to be withdrawn.
     *
     * One frame, enough. Once tried to recycle it here, it would be a complete frame for the consumer.
     * Take away (the actual match is 1651 times, and Chrome only exports 24 frames, and the frame rate drops to 1 fps).
     * It's been tried to zoom the queue into 256, but it's just pushing the symptoms back (in 67  ⁇  259) without solving it. */
    /* ⁇  0.4.2: The trigger conditions cannot be seen solely in the driver side queue.
     *
     * The number of to-do queues for session is the number of CAPTOURE buffers (DMD_V4L2_MAX_CAP=24), which is the number of the selected queues.
     * A DMD_MAX_SURFACES=64, much smaller than here. Each frame in the queue is attached to an unrecovered one.
     * CAPTURE buffer, which is 24 hours a day while msm_vidc is without a frame, and the input buffer stops recycling.
     * Send_unit is permanently under pressure -- that is, `pending_count > = 64 `
     * ** Never wait** Protection is a void.
     *
     * effmpeg `-hwaccle_output_format vaapi-f null- `, consumers never know
     * vaSyncSurface / vaExportSurfaceHandle: 36 submissions, 0 frames, log brushes
     * "end=24/24 UPPUT dead after driving=8/8 CAPTURE at driver=0/24."
     * 40s exited after time. The same flow `-f Rawvideo 's 150/150 downboard is normal — that one.
     * The path immediately went to vSyncSurface, and there was no backlog, so it was never triggered.
     *
     * So we're going to look at the session level: three-quarters of the capacity starts to empty. */
    struct dmd_session *bp_sess0 = c->session;
    const int sess_cap = bp_sess0 ? dmd_session_pending_capacity(bp_sess0) : 0;
    const int sess_hi  = sess_cap > 0 ? (sess_cap * 3) / 4 : 0;
    const int sess_backlogged =
        bp_sess0 && sess_hi > 0 &&
        dmd_session_frames_pending(bp_sess0) >= sess_hi;

    if (c->pending_count >= DMD_MAX_SURFACES || sess_backlogged) {
        struct dmd_session *bp_sess = c->session;
        int bp_idx = (int)(c - drv->contexts);

        if (bp_sess && !drv->io_busy[bp_idx]) {
            drv->io_busy[bp_idx] = 1;

            /* The number of frames is the key trade-off here. Both directions are stepped on:
             *
             * Only one frame (0.4.1 practice): session line is full and cannot be saved...
             * Every time EndPicture takes a frame, it presses back a frame, the water level does not drop, the lock is kept.
             *
             * A drain loop would consume the frames before the client can retrieve them.
             * "The driver matched 1651 frames, but Chrome exported only 24 and dropped to 1 fps".
             *
             * Reach: Half of the water received (session_cap/2) stops and half of the consumer is left behind
             * Line depth. So you break the lock and you don't take the frame you need from the consumer. */
            const int drain_to = sess_cap / 2;
            int drained = 0;

            for (;;) {
                if (dmd_session_frames_pending(bp_sess) <= drain_to &&
                    c->pending_count < DMD_MAX_SURFACES)
                    break;
                /* The capping: a single time EndPicture has a maximum set of sess_cap frames,
                 * Avoiding the unusual turns here for long periods of time. */
                if (drained >= sess_cap)
                    break;

                pthread_mutex_unlock(&drv->lock);

                struct dmd_frame bp_frame;
                memset(&bp_frame, 0, sizeof(bp_frame));
                int bp_rc = dmd_session_next_frame(bp_sess, &bp_frame,
                                                   DMD_BACKPRESSURE_MS);

                pthread_mutex_lock(&drv->lock);

                c = dmd_find_context_locked(drv, context);
                if (!c) {
                    if (bp_rc == DMD_OK)
                        dmd_session_release_frame(bp_sess, &bp_frame);
                    io_release(drv, bp_idx);
                    pthread_mutex_unlock(&drv->lock);
                    free(scratch);
                    return VA_STATUS_ERROR_INVALID_CONTEXT;
                }

                if (bp_rc != DMD_OK)
                    break;      /* No more frames. No more empty-handed. */

                VASurfaceID bp_head = dmd_pending_take_locked(c,
                                                             bp_frame.unit_seq);
                struct dmd_surface *bp_hs = dmd_find_surface_locked(drv,
                                                                    bp_head);
                if (bp_hs) {
                    surface_store_frame_locked(c, bp_hs, &bp_frame);
                    bp_hs->state = DMD_SURFACE_READY;
                } else {
                    dmd_log("EndPicture drain: pending surface %u was destroyed. "
                            "Dropping the frame\n", (unsigned)bp_head);
                }
                dmd_session_release_frame(bp_sess, &bp_frame);
                drained++;
            }

            /* Read the session status to print before handing over ownership: io_release may do well
             * Remove the session that was delayed and read bp_sessions are reading the memory released. */
            const int bp_left = dmd_session_frames_pending(bp_sess);

            io_release(drv, bp_idx);

            if (drained > 0)
                dmd_log("EndPicture: drained %d frames early (session pending %d/%d, "
                        "awaiting a surface match %d)\n", drained,
                        bp_left, sess_cap,
                        c->pending_count);
        }

        if (c->pending_count >= DMD_MAX_SURFACES) {
            pthread_mutex_unlock(&drv->lock);
            free(scratch);
            dmd_log("EndPicture: decode queue is full (%d) even after receiving frames\n",
                    DMD_MAX_SURFACES);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    /* AV1 show_frame=0 does not produce output: Decoder actuals only vomit on show_frame=1
     * CAPTURE buffer (set 2 for sending 6 units, consistent with dav1d baseline).
     * Registering such frames for pairs leaves entries that are never worthy of them...
     * The exact matching of the frame follows, which lapses, backs to the sequence extrapolation and miscalculates the surface.
     *
     * ⁇  However, the data must be presented**: These frames are the reference for the frame that follows and the leaks destroy the reference chain.
     * So just skip the team, not here.
     * (dmd_session_send_unit below, early departure is tantamount to non-delivery -- once.)
     *
     * ffmpeg still adjusts part of the summerface Sync (as measured, show=0 oh=2),
     * So give the wind up to the windface, or it'll stop at Pending forever until the time is over.
     * Marked as READY without pixel data: Sync returned immediately.
     *
     * This step only solves frame numbers.** It does not solve pixels**:
     *   Harden 150 frame = dav1d soften 150 frame {1}
     *   "Failed to read image from security 0x8:
     *                          "internal decoding error" is only partially exported.
     * Because READY but s->data is empty, GetImage has nothing to give.
     *
     * The right direction is for these surfaces to get ** real pixels ** not empty shells:
     * The AV1 frame content of show_frame=0 does exist (for follow-up frame reference), and the text is available in the AV1 frame.
     * Just not in the display queue. ffmpeg read it probably for the film grin
     * Current_frame / Current_display_picture
     * (See note to vaapi_av1.c: VAAPI produces 2 outputs per frame.
     * - One road excluded: V4L2 DISPLAY_DELAY Control -
     * msm_vidc /dev/video32 does expose standard controls
     *   V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE (0x00990b8e)
     *   V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY        (0x00990b8d)
     * Both defaults are 0, and S_CTRL settings return OK (in v4l2_backend.c),
     * Started by DMD_V4L2_DISPLAY_DELAY=1).
     * But the measurements** are invalid**: Pixels export is still 6 frames, and the error is still reported
     * "Failed to read image from surface 0x8"。
     * This control affects only the output timing/rehearsal depth, and does not change the syntax of AV1's show_frame--
     * The undiscovered frame still remains the CapTure buffer. This road is not working.
     *
     * - Two other attempts that have been proved (this round has been stepped on, do not repeat)
     * Gives sync flush input_finished door  ⁇  Frame number down from 150 to 1:15
     * In normal processes input_finished until late in the day, so the temporary frame never gets out.
     * Reformulate the same door to refresh  ⁇  Pixels dropped from 6 to 3. Both are withdrawn.
     *
     * Direction to be examined: Drivers to copy the contents of the cited frames to these surfaces,
     * Or let CAPTURE buffers be shared among multiple surfaces. */
    const int av1_no_output = (c->codec == DMD_CODEC_AV1 &&
                               !c->av1_send_show &&
                               c->av1_send_surface != VA_INVALID_ID);
    if (av1_no_output) {
        struct dmd_surface *ns = dmd_find_surface_locked(drv,
                                                        c->av1_send_surface);
        /* The shell is empty only when the surface** data** does not exist for pixels.
         *
         * ffmpeg will revert to the search. Actual: surface 6 in sync flush
         * The blog is a blog of the Global Voices community, which has been working to get the real frame data and complete the match.
         * 20 lines are marked as a shell by this branch, and the real data is lost.
         * The frames in the exported nv12 are completely zero (Y sample = 1).
         * If you have data, don't touch it: that means this surface already has another frame. */
        /* ⁇  Try "if (ns & .ns->data) -- degraded to two frames:
         * Surface will be able to log back into the PENDING, ffmpeg Report.
         * "Failed to sync surface 0x5: internal decoding error"。
         * The shell marking is necessary for process propulsion and cannot be skipped for fear of overlay.
         *
         * But direct label READY will allow the exported nv12 to appear in a completely zero frame.
         * (Y sample unique value = 1) because s->data is empty.
         * The number of frames and processes is still prioritized after weighing the READY.
         * The problem with the blank frame is left to "fill these real pixels with the surface." */
        if (ns) {
            /* These surfaces do not get hardware pixels: show_frame=0 frame AV1 synonym
             * The V4L2 side is not supposed to be out of the output (actually sent 39 units back to 20 frames only, and the V4L2 side is not vomiting)
             * 20 + 19 sub-hull marking = 39, hardware behavior conforms to the norm.
             * But ffmpeg still plays the word "GetImage" on every sturface.
             * Data is empty and exported with a zero frame (actually 9 in 30 frames).
             *
             * The tried compromise (repeating the recently ready pixels of the surface) has been deleted and is below
             * "Pixels carry the entire route as it is not proven."
             *
             * - acceptance proved invalid (current round)
             * Switches are 30 frames / 17 different images on both sides, with md5 different.
             * (ac44da7 vs 3faf63ad, softline baseline e8b97d00).
             * No sources of contamination of SEF were accepted, nor any additional information was added.
             *
             * - Real structure (md5 match-up by frame)
             * Hardline 30 Frame-to-File Frame-to-Face:
             *   Odd position 1, 3, 5, 27 **All ** is precise equal to softened position frame
             *   Even position 2, 4, 6, all zero or wrong frames.
             * The odd-couples are completely separated, without exception -- systematic misallocation, not data damage.
             *
             * Reason: The display of event sequences in the stream is sEF and show_frame=1
             *   [F1, F0,F0,F0,F0, F1, SEF, F1, SEF, F0, F1, SEF, ...]
             * SEF just landed in even output bits, and it quoted a frame that shows_frame=0 --
             * That's what these empty shells are. ffmpeg read empty shells/other people's pixels when resurfaced.
             *
             * So the revision is not "filling the shell with something," but it's letting show_frame=0.
             * Surface loads itself with its own frame of true pixels.
             * But the hardware doesn't throw up these frames (150 for 80, which corresponds to show_frame=1 quantity).
             * Need to find another way to get them -- that's the next problem. */
            /* - The current round of revision: take the source by the DPB slot instead of "the recently ready one"
             *
             * The analysis is already in place: SEF refers to show_frame=0.
             * And the "reinherent to the latest frame" is not valid.
             * (Switches are 17 different images on both sides) because it's the wrong source**.
             *
             * But the driver actually knows the correct source: the frame of the show_frame=0 is in the build_unit
             * is charged to av1_dpb.dpb_shadow[slot], slot is the DPB slot it occupies.
             * The sturface that's in the slot right now is "what the frame should look like."
             * The only thing that can be approximated -- if the slot was previously updated by a frame with pixels, is that the sink is not a pixmixial.
             * Cuff it more than "recently ready" is the real picture.
             *
             * ⁇  This round of measurements:** This road is not a good one, not a bad strategy.**
             *
             * After adding the diagnostic counter (pre-source 256 bytes zero) it was measured:
             *     DPB cumulative 19,** of which source itself was none.
             * That is, the frame show_frame=0 is cross-referenced and most sources** are also empty shells**.
             *
             * The results of the three strategies are therefore fully consistent (30 frame samples):
             *     Press DPB slot to extract 17 different images
             *     Recent frame readiness 16
             *     Zero total 17
             * The source change has not worked -- ** The information to be tortured did not exist from the beginning**.
             *
             * Conclusion: No "from other surface copy" solution can solve this problem.
             * The only way out is to get the frame show_frame=0 to get the hardware decoding results.
             * The number of the results of the survey is 80, which is exactly the same as the number of shows_frame=1.
             * Description msm_vidc strictly does not export them according to the AV1 semantic criteria.
             *
             * Two directions for the next:
             *   A. Let the show_frame that drives the rewrite of these frames be 1 and cheat the hardware out,
             *      You decide not to add them to the output sequence. Risk: rewrite uncompressed
             *      Header will affect the offset of refresh_frame_flags.** Not yet verified.**
             *
             *   B.  ⁇  Documented (80th round of live surveys)**: Check if msm_vidc
             *      The V4L2 control entry "out of all frames contains no frames to display".
             *      Use VIDIOC_QUERYCTRL+V4L2_CTRL_FLAG_NEXT_CTRL
             *      /dev/video 32, resulting in **0 ** - this driver is not exposed to a control item.
             *      DEC_DISPLAY_DELAY/DEC_DIPLAY_DELAY_ENABLE/
             *      AV1_PROFILE/AV1_LEVEL is also all EINVAL.
             *      (Node ID verified: msm_vidc_driver card=msm_vidc_vdec,
             *        ENM_FMT lists 5 formats, so 0 is the real result
             *        This is not an enumeration failure or a probe of the wrong node.)
             *      See toos/probe_v4l2_ctrls.c.
             *      Conclusion: No control item retweet, direction B does not exist.
             * Conclusion: No control item retweet, direction B does not exist. */
            /* - whether the entire route of the surface memcpy is proven, Round 70 -
             * Three sources were tried: the DPB slot occupied by this frame, the recently ready frame, and the zero.
             * The results were 17/ 16/ 17 different images - no different from no acceptance.
             * Decisive evidence: Counts show that 19 of the **13 sub-sources are themselves zero**.
             * The reason is that the frame of show_frame=0 is cross-referenced, and the quoted pixels are on the hardware side.
             * It never came out, and information does not exist at the driver layer.
             * So any copy of the surface is dead, not a question of penetration.
             * Related switches (DMD_AV1_CARRY / DSD_AV1_NO_CARRY) and counters
             * (v1_carry_dpb/av1_carry_blunk) Deleted with this block. */
            ns->state = DMD_SURFACE_READY;
            ns->decode_status = VA_STATUS_SUCCESS;
            dmd_video_stats_reuse(c->video_stats);
        }
        dmd_log("EndPicture: AV1 surface=%u show_frame=0. Submitting without queuing the surface\n",
                (unsigned)c->av1_send_surface);
    }

    /* Consume late output by its old unit identity without overwriting the
     * recycled export. Non-exported reads keep their synchronous behavior. */
    retire_reused_target_locked(drv, c, target);
    for (int k = 0; k < c->pending_count; k++) {
        int kp = (c->pending_head + k) % DMD_MAX_SURFACES;
        if (c->pending[kp] != target)
            continue;
        /* Target surface is still in the queue: take the frame first and avoid being overwritten by this decoding. */
        dmd_log("EndPicture: surface %u is awaiting unit %llu. Draining first\n",
                (unsigned)target, (unsigned long long)c->pending_unit[kp]);
        struct dmd_session *ds = c->session;
        int di = (int)(c - drv->contexts);
        if (!ds || drv->io_busy[di])
            break;
        drv->io_busy[di] = 1;
        for (int guard = 0; guard < DMD_MAX_SURFACES; guard++) {
            /* Are the targets still in line? If not, we'll have enough. */
            int still = 0;
            for (int m = 0; m < c->pending_count; m++) {
                int mp = (c->pending_head + m) % DMD_MAX_SURFACES;
                if (c->pending[mp] == target) { still = 1; break; }
            }
            if (!still)
                break;

            pthread_mutex_unlock(&drv->lock);
            struct dmd_frame rf;
            memset(&rf, 0, sizeof(rf));
            int rrc = dmd_session_next_frame(ds, &rf, DMD_FRAME_TIMEOUT_MS);
            pthread_mutex_lock(&drv->lock);

            c = dmd_find_context_locked(drv, context);
            if (!c) {
                if (rrc == DMD_OK)
                    dmd_session_release_frame(ds, &rf);
                /* Must hand over the ownership right before leaving: former direct return, io_busy of this slot
                 * Permanently parked on 1, next context that takes the same slot will wait five seconds.
                 * The frame line is always skipping it. */
                io_release(drv, di);
                pthread_mutex_unlock(&drv->lock);
                free(scratch);
                return VA_STATUS_ERROR_INVALID_CONTEXT;
            }
            if (rrc != DMD_OK)
                break;

            VASurfaceID got = dmd_pending_take_locked(c, rf.unit_seq);
            struct dmd_surface *gs = dmd_find_surface_locked(drv, got);
            if (gs) {
                surface_store_frame_locked(c, gs, &rf);
                gs->state = DMD_SURFACE_READY;
            }
            dmd_session_release_frame(ds, &rf);
        }
        io_release(drv, di);
        break;
    }

    int qpos = (c->pending_head + c->pending_count) % DMD_MAX_SURFACES;
    /* AV1 Delays a frame: This time the data from the previous frame is sent to register its surface.
     * The rest of the codec send is the current frame, directly with the target. */
    const VASurfaceID reg_surf = (c->codec == DMD_CODEC_AV1 &&
                                  c->av1_send_surface != VA_INVALID_ID)
                               ? c->av1_send_surface : target;

    /* The same surface cannot appear twice in the to-do pairings.
     *
     * The real root of the wheel is irrelevant to the last "empty shell":
     * ffmpeg will revert to the surface.
     * ffmpeg then takes the same surface as the target of the new frame, and it's registered for the second time...
     * Logs appear simultaneously with "end frame surface=8 unit 8" and "unit 8"
     * "Registration: target=8  ⁇ pending [10]=8", and ** There was no frame ration for the surface 8**,
     * ffmpeg "Failed to read image from surface 0x8".
     * Watch out for the failure of 8 surface, which is marked show_frame=0 by 3/4/5...
     * The last round mixed the two in the wrong direction.
     *
     * Repeat the entry, the exact pair of the wrong one.
     * The other one never fits, the further the queue.
     * Clean the queue here and no longer register in the queue. */
    int dup_pending = 0;
    for (int k = 0; k < c->pending_count; k++) {
        int kp = (c->pending_head + k) % DMD_MAX_SURFACES;
        if (c->pending[kp] == reg_surf) { dup_pending = 1; break; }
    }
    if (dup_pending)
        dmd_log("EndPicture: surface=%u is already pending. Queuing the new unit number\n",
                (unsigned)reg_surf);

    /* ⁇ jump only into the queue,** data distribution**. Subscribe below dmd_session_send_unit,
     * Here, the early-return is the same as not sending -- this session has been stepped on twice, and never again.
     *
     * ⁇ 0.4.5 Both practices that have been proven and not proven, do not repeat them:
     *
     * 1. Hit only the warning not to weigh (0.4.4 and earlier actual behaviour, which is inconsistent with the above note):
     *    Once the duplicates are in the queue, the exact matching of the unit is still correct** (for each entry).
     *    It's not fatal.
     *
     * 2. Make repeats skip the team (0.4.5 first attempt): ** Makes a much worse mistake**. Hardware will do the same.
     *    This time, a unit was returned, and there was no corresponding registration in the queue, so from that moment on,
     *    Each frame is "unit_seq=N does not match " in order of reasoning. Chrame is measured.
     *    98.6% (1292/1310) pairing failed and the image continued to leap forward and forward.
     *    Log fingerprint: submit unit=16  ⁇  two unit=0  ⁇  submit unit=19,
     *    And then unit_seq=17 without any matches.
     *
     * Conclusion:** queue registration must correspond to submission and cannot create a vacuum**.
     * The same searchface allows multiple entries -- their unit numbers are different,
     * The exact pair is separated by unit, dmd_pending_take_locked
     * Unit match item, do not look whether the surface repeats. */
    c->pending[qpos] = reg_surf;
    /* ⁇  Here** do not print c->av1_pic_param show_frame/order_hint:
     * That's the parameter of the current frame, and the registered surface is the previous frame (delaying a frame delivery).
     * I read it wrongly, and I came to the wrong conclusion of "send=5 corresponding to show=1". */
    if (c->codec == DMD_CODEC_AV1 && getenv("DMD_VA_LOG"))
        dmd_log("Queued target=%u in pending[%d]=%u for the submitted frame\n",
                (unsigned)target, qpos, (unsigned)c->pending[qpos]);
    /* Write down this frame POC for matching to display orders and with "sequence number".
     *
     * POC only has one code code secret ** single, every IDR will take it
     * Reset the starting point. Check the second IDR location POC to jump back from 65562 - if only POC
     * The smallest global take, the new sequence will be taken before the old sequence is not matched, leading to the same surface
     * The image was confused from the IDR twice by pairing.
     * So press seq and then type POC: POC size is greater than within the same sequence. */
    if (c->have_current_poc) {
        /* Determines whether to enter the new sequence.
         *
         * The cylindrical is not to be judged by "poC is smaller than the previous frame" -- the order of submission is ** decoding**, which is not to be determined by the fact that the  ⁇ Corporation system is not to be used.
         * The same GOP POC is inherently up and down (as measured by 65536, 65544, 65540, 65538).
         * That would have misconstrued every B frame into a new sequence.
         *
         * Use frame_num: it is zero per IDR (H.264 Reg. 7.4.3),
         * And the GOP is incremental, so "frame_num zero and not the first frame" or IDR.
         * VA-API provides it at VAPictureParameter Buffer H264.frame_num (va.h:3618). */
        if (c->have_last_poc && c->current_frame_num == 0 &&
            c->last_frame_num != 0)
            c->last_seq++;
        c->last_poc = c->current_poc;
        c->last_frame_num = c->current_frame_num;
        c->have_last_poc = 1;
    }
    c->pending_seq[qpos] = c->last_seq;
    c->pending_poc[qpos] = c->have_current_poc ? c->current_poc : INT32_MAX;
    /* Sending serial number: As timestamp sent out with the OUTPUT buffer, the hardware is returned to the original and the exact matching is done accordingly.
     * Numbering begins with 1 and each data unit plus 1.
     * Note that only the submission of the true data release module is counted and the parameter collection is not. (
     * The unit is_param_set to walk the CSD path without increasing vcl_in. */
    /* ⁇  Documented: Change the number to the session same source.
     * The number of returns has not decreased further (still 64), but pixels have deteriorated significantly:
     *     17 ff 5 different images (exact hits 21 ff 24, just by chance)
     * Because AV1 frames may be stored pending at the time of registration of this function,
     * The session count is not advanced, but the same source is wrong.
     * The one that was different: the data was sent right after, +1 was correct.
     * The lesson is that different images reflect more true quality than accurate hits. */
    c->pending_unit[qpos] = ++c->units_submitted;
    if (!av1_no_output)
        c->pending_count++;

    c->have_current_poc = 0;
    c->current_target = VA_INVALID_ID;

    /* Serial number tracking (DMD_TRACE_ORDER=1 only): Three columns CSV allow for external numeric misallocation.
     * The aim is to accurately detect the deviation between the consumer's request and the driver of the frame, rather than to rely on guessing. */
    if (dmd_trace_order())
        dmd_log("ORDER submit surf=%u unit=%llu pend=%d\n",
                (unsigned)target,
                (unsigned long long)c->pending_unit[qpos], c->pending_count);

    /* A copy of the code flows to a local buffer: c->slick_data may be reused by another thread after locking. */
    unsigned char *tx = scratch;
    scratch = NULL; /* The title is transferred to tx; subsequent withdrawal is only possible to release tx and the same pointer is no longer released. */
    if (!tx) {
        tx = malloc(unit_len);
        if (!tx) {
            c->pending_count--;
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        memcpy(tx, unit, unit_len);
    }
    c->slice_len = 0;
    c->av1_tile_count = 0;

    /* Serializes the IO of context.
     *
     * Order requires:** to acquire ownership and then to capture the message pointer**.
     * So the opposite is true -- `session' caught it in the first half of the function, and here we wait for io_busy,
     * And wait for the lock. The DestroyContext/ session re-establishment in the lock window can take this session apart.
     * (old code even does not even vary ownership, direct io_busy=1, two threads moving the same V4L2 fd),
     * IO = use-after-free, and then give it back with zero ownership.
     * = The next person who opens the session thinks nobody's using it. The actual signature is QBUF/DMA_BUF_SYNC.
     * ENOTTY, Chrome side "Crazing due to FD ownership situation". */
    if (io_acquire(drv, idx, "EndPicture/Send Unit") < 0) {
        /* The locks are placed during the waiting period: the former context may have been destroyed and the new slots cannot be changed. */
        if (!context_slot_ok(c, context)) {
            pthread_mutex_unlock(&drv->lock);
            free(tx);
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
        /* No title is given: this frame is not delivered. The registered queues are to be taken back.
         * Otherwise, there is a hole that never fits, and every frame is then in the order of the inference. */
        if (c->pending_count > 0)
            c->pending_count--;
        struct dmd_surface *s = dmd_find_surface_locked(drv, target);
        if (s) {
            s->state = DMD_SURFACE_IDLE;
            s->decode_status = VA_STATUS_ERROR_OPERATION_FAILED;
        }
        c->current_target = VA_INVALID_ID;
        pthread_mutex_unlock(&drv->lock);
        free(tx);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!context_slot_ok(c, context)) {
        /* Slots changed: ** This cannot ** move any fields c-> (those queues, counters)
         * Now it's someone else's context. The original context goes with its queue.
         * It doesn't matter if we don't have this frame. */
        io_release(drv, idx);
        pthread_mutex_unlock(&drv->lock);
        free(tx);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    sess = c->session;
    if (!sess) {
        if (c->pending_count > 0)
            c->pending_count--;
        struct dmd_surface *s = dmd_find_surface_locked(drv, target);
        if (s) {
            s->state = DMD_SURFACE_IDLE;
            s->decode_status = VA_STATUS_ERROR_OPERATION_FAILED;
        }
        c->current_target = VA_INVALID_ID;
        /* Returning latent sessions with possible destruction; all context/service clean-up must be completed first. */
        io_release(drv, idx);
        pthread_mutex_unlock(&drv->lock);
        free(tx);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* H.264: The argument set must be sent before the first VCL. First, when the data is needed for the snapshot when the lock is held,
     * Lock it and then send it. */
    int need_param_sets = 0;
    VAPictureParameterBufferH264 pp_snap;
    VAIQMatrixBufferH264 iq_snap;
    VASliceParameterBufferH264 sp_snap;
    int have_iq_snap = 0;
    int have_sp_snap = 0;
    VAProfile profile_snap = c->profile;
    unsigned int mbs_wide = 0, mbs_high = 0;
    VAPictureParameterBufferHEVC hevc_snap;
    int need_hevc_params = 0;
    /* Locked snapshot: Unlocked at the time of sending, cannot read c->pending_l0/l1. */
    unsigned int l0_snap = 0, l1_snap = 0;

    if (codec == DMD_CODEC_H264 && c->have_h264_pic_param) {
        mbs_wide = (unsigned int)c->h264_pic_param.picture_width_in_mbs_minus1 + 1;
        mbs_high = (unsigned int)c->h264_pic_param.picture_height_in_mbs_minus1 + 1;
        /* Reissued condition: the first time, geometric change in the stream, or the change in the num_ref_idx default value of the PPP.
         *
         * The last one is necessary: the PPS default values must be copied from the entry value of the current frame**
         * (Details are given in the note kh264_bitstream.c) and the entry values of the different frames vary.
         * MediaCodec accepts the recurring PPS in the stream, with only a few bytes per time. */
        /* Use the largest l0 and smallest l1 values seen so far.
         *
         * VA-API gives the effective value for each slick with override_flag
         * The entry value is not related to the PPS default - the default value is written directly by the default value.
         * Unlock the screen by other non-override slice (for details, see h264_bitstream.c)
         * , which produces 5 variants).
         *
         * One can be larger, one can be bigger, one can be larger.
         * Max and smallest, the only correct PPS in a few frames. */
        /* The num_ref_idx_l0/l1_active_minus1 is always zero, but the number of people who are not in the process of being killed is zero.
         * That ** is not the true value of the **PPS default value -- I slice does not have this syntax element at all.
         * Blindly, it will make the first PPP** session ** write 1 0 = 0.
         *
         * Actual (h264_slow.mp4, authentication with bit level resolution PPS is 1 0=2 l1=0):
         *   Frame 1 is IDR  ⁇  Sending PPS 9 bytes (I0=0l1=0, error)
         *   Frame 3 is B slick § Sends PPS 10 bytes (l0=2l1=0, right)
         * So the unit 1, 2 frames were decoded by the wrong l0; the 3rd frame is restored.
         * This is what the user says: "The first two jumps, the first flow from frame 3"
         * The web stream is also consistent with local files (not related to MSE/ABR).
         *
         * ⁇  This bug is not visible on the ffmpeg path**, do not return to verify it with md5.
         * The old version drives the same material before 8 frames match the soft-blank bytes, md5 PASS, and then the next page is the same.
         * Add DMD_NO_MAP_WAIT also PASS. Cause of submission: ffmpeg pours 6 frames at once
         * Up (Journalend to 6) and drive off the wrong PPS, unit 3 in unit 1
         * Yeah, the right piece of hardware actually started decoding. Chrome submitted it by frame.
         * The first frame decodes only the wrong one.
         *
         * Authentication must be based on browser + pixels, see
         * These are the three rounds of A-B measurements:
         *   Old version (first PPS l0=0): The image is correct 5/4/5, and cannot be identified 13/13/13 (total 20)
         *   New edition (first PPS l0 = 2: correct image 20/20/20, 0
         *
         * I slice (or not yet slick param) to use num_ref_frames-1 when l0.
         * The number of hours is larger (the single variable is correct).
         * Num_ref_frames-1 must be > = the true default, so it is both safe and overlaying the true value.
         * Il still takes 0:1 and not one person is bigger, and 0 is the minimum.
         *
         * And to watch this is two things -- "Pps, one piece of it."
         * (long 3000.h264 only solves 15/1323 frames and then dies. This is the first to be used.
         * The re-transmission behaviour of P/B slicice remains intact and the oscillation over the true value remains intact. */
        int slice_is_i = c->have_h264_slice_param &&
                         (c->h264_slice_param.slice_type % 5) == 2;
        unsigned int l0_now, l1_now;
        if (!c->have_h264_slice_param || slice_is_i) {
            unsigned int nref = c->h264_pic_param.num_ref_frames;
            l0_now = nref > 0 ? nref - 1 : 0;
            l1_now = 0;
        } else {
            l0_now = c->h264_slice_param.num_ref_idx_l0_active_minus1;
            l1_now = c->h264_slice_param.num_ref_idx_l1_active_minus1;
        }
        /* Don't do "just increase and no loss" single-tone here for the LT. Testimony has been detected.
         *
         * Motive seems reasonable: PPS sequences after the first frame are 10, 9, 10, 10... the 9 bytes.
         * (l0 = 0, from the first P slice after IDR) seems to still contaminate the frame.
         * So we tried `if (param_sets_sent & & 0_now < send_l0) 0_now = sent_l0; `
         *
         * The result is a clear regression (h264_slow.mp440 frames, frame-by-frame softened):
         *   Old version driver md5 PASS
         *   Single-modified frames 0, 1 consistent, frame 2 all different (5-12% difference in Y plane)
         *   PPS Reload 12  ⁇  3 times, all amplified to 1 0 = 2
         * Which means that the admission itself is a cause of the disease, and the h264_bitstream.c
         * Long 3000.h264 returns (15/1323, stuck behind) are the same pit.
         *
         * Reason: The number of retransmissions is taking on the error. Different slicks require different PPS default values,
         * The oscillation allows the decoder to always receive the one matching the current slice; once the harvest is in one single piece,
         * Those slicks that don't match are never getting the right code.
         * To really eliminate ostracism, the true default values must be determined before the first slice.
         * And VA-API only gives the entry value for each slick -- there is no reliable way to deduce it at this time.
         *
         * So here's only the first PPS.
         * The retransmission behaviour of P/B slice remains the same. */
        /* l0/l1 Directly uses the valid value reported by slick param.
         *
         * ⁇  tried to "retard PPS ostracism": switch to SPS 's num_ref_frames-1 to guide the 0,
         * A minimum observation value of 1 can be used to allow only 1 PPS per session (685 times _ 1 time).
         * It was a return**: long 3000.h264, only to unblock 15/1323, which was stuck to the frame and died while waiting for it to pass.
         *
         * The reason is that the number of retransmissions is itself a bug-fixing function -- the normal version sends 4 variants to the current.
         * A total of 2735, with a real value of 68ebe3cb22c0; the harvest results are sent out in one piece.
         * 68e938f2c8b0, the codecer never gets the right piece.
         *
         * In other words: the ostracism looks ugly, but it covers the true value; it is a stake in a single one.
         * To really eliminate the ostracism, we need to be able to determine the real default values before the first **slice **slice **si
         * VA-API only gives the effective value of each slick (override is not related to the default value),
         * There is no reliable way to follow. Keep the status quo. */
        unsigned int l0 = l0_now;
        unsigned int l1 = l1_now;
        if (!c->param_sets_sent || c->sent_mbs_wide != mbs_wide ||
            c->sent_mbs_high != mbs_high || c->sent_l0 != l0 ||
            c->sent_l1 != l1) {
            c->pending_l0 = l0;
            c->pending_l1 = l1;
            l0_snap = l0;
            l1_snap = l1;
            need_param_sets = 1;
            pp_snap = c->h264_pic_param;
            iq_snap = c->h264_iq_matrix;
            have_iq_snap = c->have_h264_iq_matrix;
            sp_snap = c->h264_slice_param;
            have_sp_snap = c->have_h264_slice_param;
        }
    }

    if (codec == DMD_CODEC_HEVC && c->hevc_pic_param_valid) {
        /* Simpler re-enactment than H.264: the HEVC PPS does not contain framework-changed fields
         * (num_ref_idx default is the true default in PPS, and slick header will overwrite),
         * So only re-emerged when the first resolution changed. */
        unsigned int w = c->hevc_pic_param.pic_width_in_luma_samples;
        unsigned int h = c->hevc_pic_param.pic_height_in_luma_samples;
        /* VA-API lacks the information needed to reproduce certain streams (see dmd_hevc_can_build).
         * This must fail to allow the top down to soften -- synthesize an unmatched SPS
         * It's a lot worse than a direct failure. */
        if (!dmd_hevc_can_build(&c->hevc_pic_param)) {
            dmd_log("HEVC: cannot synthesize the parameter sets for this stream "
                    "(num_short_term_ref_pic_sets=%u). Rejecting the frame\n",
                    c->hevc_pic_param.num_short_term_ref_pic_sets);
            if (c->pending_count > 0)
                c->pending_count--;
            struct dmd_surface *s = dmd_find_surface_locked(drv, target);
            if (s) {
                s->state = DMD_SURFACE_IDLE;
                s->decode_status = VA_STATUS_ERROR_UNIMPLEMENTED;
            }
            dmd_video_stats_error(c->video_stats);
            c->current_target = VA_INVALID_ID;
            c->slice_len = 0;
            c->av1_tile_count = 0;
            /* This function has acquired IO ownership. The refusal must also be returned, otherwise the next frame
             * Always wait for it; return a possible destruction delay session and then no further access to c/sess. */
            io_release(drv, idx);
            pthread_mutex_unlock(&drv->lock);
            free(tx);
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        if (!c->param_sets_sent || c->sent_mbs_wide != w ||
            c->sent_mbs_high != h) {
            need_param_sets = 1;
            need_hevc_params = 1;
            hevc_snap = c->hevc_pic_param;
            /* Use these two fields to record resolution (HEVC no macro-blank concept, in pixels). */
            mbs_wide = w;
            mbs_high = h;
        }
    }

    /* Take a to-record slot: SEF is only filled when the frame is shown itself.
     *
     * ⁇  Not completed. Unconditional counter measurements (150 frames full):
     *     Sluice execution 75 (of which send_show=1 only 5)
     *     Get in line 68, drop it 2, add 4
     * Two things don't work out:
     *  (a) Only 75 slots were taken and 154 units, units and units were actually delivered
     *      Build_unit was switched 150 times -- there is a path around here, location unpositioned.
     *      (Av1_no_output branch has been excluded, it is not early-return.
     *  (b) Send_show=1 only 5 times, with 80 times a pair.
     * Do not change the trigger until these two numbers are explained.
     * The last rotation was av1_hold_show, and the delivery was struck from 154 to 8. */
    int sef_send_slot = -1;
    if (c->codec == DMD_CODEC_AV1) {
        c->av1_sef_visits++;
        if (c->av1_send_show) c->av1_sef_show1++;
    }
    /* ================================================================
     * Why is SEF adding only 4?
     * ================================================================
     * Full 150 frames:
     *   EndPicture Enter 150 (build_unit non-empty 75, returns NULL 75)
     *   Take slot execution 75 (of which send_show=1 only 5)
     *   Get in line 68, drop it 2, add 4
     *   Current frame at suspense show = 1 total 35
     *   Av1_send_show assigned 1 — **0 **
     *   sync flash sent 75 times
     *
     * Both conflicts are solved:
     * (a) 75 lift points and dispatch 154 units:
     *     The project is a long-term project that has delayed the release of only half the frames under a frame of mechanisms.
     *     The other half (75) is sent by sync flush, which is not available on the Flush path.
     *     154 = 75 (EndPicture) + 75 (flush) + 4 (SEF), reconciliation of accounts.
     * (b) Send_show=1 only 5 times:
     *     The decisive data is "av1_send_show was assigned 1 0 times in total".
     *     All five times from the KEY framework branch (decode.c: 1606 direct).
     *     Hold-swap branch (1635 lines) reads av1_hold_show,
     *     It is not updated to the current frame until 1639...
     *     The reading is always the same as the previous show, and the first time in is the initial value of zero.
     *     So, "Show =1 on deposit has 35 times" information is lost.
     *
     * But this is not a simple change of field.
     * Sending from 154 to 8: Flush sent the frame out of the house without passing through here.
     * What really needs is "is the frame that actually came out of that unit shown?"
     * And this information is available on both EndPicture and Flush paths.
     * ================================================================ */

    /* The trigger conditions are "whether this sent frame is shown" instead of the current frame.
     * The fact that the frame is being sent - semanticly correct - is reflected in the delay in the frame.
     * But it's only five times in 150 frames, one time in 70 times in 0:30.
     * The vast majority of frames sent are the show = 0 staggers.
     * And SEF must follow closely "the frame that will show," which is not the same.
     * Replace av1_hold_show -- it's a show value that's just been stored in the current frame.
     * Consistent with "if this ffmpeg submission is shown." */
    /* ⁇  has tried to change av1_hold_show (the current frame shows) -- severe degradation:
     * Sending from 154 units down to 8 and the collection frame down from 80 to 2.
     * The reason is not identified, but the direction is not proven: no simple replacement of a show sign.
     * Av1_send_show. */
    /* SEF door: The DPB slot that it quotes must have been out of pixels.
     *
     * Why do you need a door: "Stand in line or empty" ffmpeg output dropped into 64 frames...
     * SEF was sent out before the decoding of the referenced frame, citing a slot that was not filled.
     * Why don't you use av1_send_show as a door: it only has 5 times to 1 in EndPicture path
     * (Film frames almost always go free) and only four SEFs are made up each run.
     *
     * Source flow measurements: SEF is the smallest 2-mode 5-mode, the largest 23-year-old event in the world, and the most important event in the world is the SEF.
     * The Quest is a very important tool for the community, which is that the frame quoted has necessarily been decoded -- judging by whether the search is READY, which is the most important part of the story.
     * Much more accurate than "is the current frame shown?" And both feed paths can be used.
     * DMD_AV1_SEF_GATE=show can be cross-referenced to the old send_show door. */
    /* ⁇  No proof: "Recited surface is no READY" as the door.
     * The result is like "No Door": ffmpeg output 150  ⁇ 64.
     * This condition is almost always true (the reference frame has already finished decoding,
     * consistent with the source trace showing at least two events between SEF and its reference frame),
     * The price is the no door -- so the real reason for degradation is not "citing an unprepared slot."
     *
     * And then, inversely: 65th Round, 64th Frame is wrong because of the "SEF grab".
     * The real reason is more likely that the number of SEF units themselves -- to 60 to 70 time-strikes -- is to be added to the current situation.
     * Four hours is normal. The direction should be turned to "How to Disrupt CAPTURE buffers from the extra units."
     * "and pinging" instead of continuing to look for stricter doors. */
    /* - SEF quantity cap mapping conclusions (68th Round, DMD_AV1_SEF_MAX Switch deleted)
     * Actual (150 frame sample, ffmpeg output frame):
     *     Maximum 8 * 150 frames *
     *     Maximum 20 * 150 frames *
     *     Upper limit 40  ⁇  Timeout (>180s, normal app. 40s)
     *     No cap (~68) ~64 frame
     * The crash threshold is between 20 and 40 and ** has nothing to do with the door** — 20 can run without setting the door at all.
     * Does this prove the cause of 65th round "SEF grab before the frame is decoded?"
     * If it's a robbery, then 20 more should be in trouble. The real reason is the number itself.
     * Unsolved puzzle: Why does it crash between 20 and 40? The direction is "How the extra units run out of CapTure."
     * Buffer or bursting pending queue ", which requires equipment to be observed online, frame by frame.
     * Current act: Follow the send_show door (conservative, only about four), without a cap switch. */
    if (c->codec == DMD_CODEC_AV1 && c->av1_sef_count > 0 &&
        c->av1_send_show != 0) {
        sef_send_slot = (int)c->av1_sef_slot[c->av1_sef_head];
        c->av1_sef_head = (c->av1_sef_head + 1) & 63;
        c->av1_sef_count--;
    }

    /* The title is on top of io_acquire, and here's only the lock. */
    pthread_mutex_unlock(&drv->lock);

    int rc = DMD_OK;
    int sef_sent_now = 0;   /* Not write c-> during locking, see how it works down. */
    if (need_hevc_params) {
        if (hevc_send_param_sets(sess, &hevc_snap, profile_snap) != 0)
            rc = DMD_ERR_PROTOCOL;
    } else if (need_param_sets) {
        if (h264_send_param_sets(sess, &pp_snap, &iq_snap, have_iq_snap,
                                 have_sp_snap ? &sp_snap : NULL,
                                 l0_snap, l1_snap, profile_snap,
                                 pw, ph) != 0)
            rc = DMD_ERR_PROTOCOL;
    }
    /* SEF patch: Sended as a stand-alone module** before frame data.
     *
     * TU structure for the 65th wheel to determine the source stream (by TEMPORAL_DELIMITER )
     *     70 TUs with only one SEF, no frame data
     *     45 with 1 frame, 15 with 2 frames, 10 with 3 frames, 5 with 4 frames, 5 with 5 frames
     *     Total 150 TU, 150 OBU_FRAME, 70 SEF
     * So SEF ** is a complete temperal unit**,
     * And not attached to the frame -- I was wrong on all three previous rounds.
     * Set3 sent 150 units to collect 150 frames also match:
     * 80 frames TU out of 80 + 70 pure SEF TU out of 70 = 150.
     *
     * So this is a separate delivery and must pre-empt TD to make it legal TU.
     * The difference from the 62nd round "add in Flush": the time when the units were inserted into the path of the equivalent pixels,
     * The rhythms were broken (150 00111), which was delivered before the normal delivery point.
     * Don't touch Flush. */
    if (rc == DMD_OK && sef_send_slot >= 0) {
        unsigned char sefbuf[8];
        size_t sn = 0;
        sefbuf[sn++] = (unsigned char)((DMD_OBU_TEMPORAL_DELIMITER << 3) | 0x02);
        sefbuf[sn++] = 0;                     /* +0 for TD */
        size_t hn = dmd_av1_build_show_existing(sefbuf + sn, sizeof(sefbuf) - sn,
                                                (unsigned)sef_send_slot);
        if (hn > 0) {
            sn += hn;
            /* ⁇  Not write c->: the pointer is not valid for the object or for us ** during locking**
             * (Stacks may have been reset by DestroyContext, and used by the next CreativeContext).
             * The accounts are maintained after the locking has been re-established after the local load has been exhausted. */
            if (dmd_session_send_unit(sess, sefbuf, sn) == DMD_OK)
                sef_sent_now++;
        }
    }

    if (rc == DMD_OK)
        rc = dmd_session_send_unit(sess, tx, unit_len);
    free(tx);


    pthread_mutex_lock(&drv->lock);

    /* ** The error was copied before the title was returned: dmd_session_last_error returned the session
     * Internal buffer zones, and Io_release may have been able to take down the late-destruction session. */
    char sess_err[160];
    sess_err[0] = 0;
    if (rc != DMD_OK) {
        const char *m = dmd_session_last_error(sess);
        if (m)
            snprintf(sess_err, sizeof(sess_err), "%s", m);
    }

    io_release(drv, idx);

    c = dmd_find_context_locked(drv, context);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    c->av1_sef_sent += sef_sent_now;

    if (rc != DMD_OK) {
        /* Back to the team: this frame never has a corresponding output. Take it off the tail. */
        if (c->pending_count > 0)
            c->pending_count--;
        struct dmd_surface *s = dmd_find_surface_locked(drv, target);
        if (s) {
            s->state = DMD_SURFACE_IDLE;
            s->decode_status = VA_STATUS_ERROR_DECODING_ERROR;
        }
        pthread_mutex_unlock(&drv->lock);
        dmd_log("EndPicture: unit submission failed (rc=%d): %s\n", rc, sess_err);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }

    /* ⁇  Certificate: Registering the feed for the SEF (the surface that it refers to).
     *
     * The motive is right: SEF is a stand-alone unit, and hardware will throw a frame at it, and drive the previous increment counters.
     * Not in the queue -- that frame is then placed on the line that belongs to someone else (the root of the 68th wheel of pixel degradation).
     *
     * But the "registry of the surface" that it refers to is not a good idea. Actual (30 frame samples, SEF registration 8 times):
     *     Different images 17 ffmpeg ffpeg
     *     Failed to read image from surface 0xc: 23 (internal decoding error)
     * Log matching gives direct evidence:
     *     SEEF registered surface: 3 4 5 10 11 12
     *     Normal pairs: 1 3 4 5 6 7 11
     * Two groups** overlap** (3/4/51/1) the same surface was registered twice,
     * It was removed by ffmpeg and reused at the time of the second return  ⁇  Internal decoding error.
     *
     * Focal point: VA-API 's surface life cycle is controlled by consumers, one surface and only one.
     * An incomplete decoding result. A re-enactment requires a different version of a surface with the same content.
     * And the driver does not have the right to create a ffmpeg -- ffmpeg only comes to get it himself
     * Those were designated as ttagets.
     *
     * ⁇  Whether or not to give evidence of a plausible inference.
     * I used to assume that "recovering the responsibility of the upper level, driving the SEF is superfluous," and the verdict was that
     * "If ffmpeg only switched 80 times EndPicture, it's dealing with 70 frames itself."
     * Actual (AV1=1 build, no SEF, 150 frame stream):
     *     EndPicture Enter **150**second, ffmpeg output **150** Frame
     * So ffmpeg**does not **process the re-record itself -- it's all 150 frames (including 70 SEFs)
     * Complete VA-API processes, each frame has its own target mode.
     *
     * This explains why "registration references to surface" is inherently conflicting:
     * ffmpeg prepared a new ** target for the SEF frame, not to repeat the quoted one.
     * The right direction is to cuff the re-reflection** into the ffmpeg for the frame's target**, which is the most important part of the story.
     * This neither asks hardware to emit an extra frame nor registers the old surface again.
     *
     * Known data support: build_unit to this 150 calls back non-empty 75, NULL 75--
     * 75 times NULL is probably the same as the SEF frame that didn't produce after the drive.
     * The stream unit. If you can directly use the dpb_shadow [slot] pixels on that NULL path
     * Copying to this target does not require hardware to be involved in re-emerge.
     * Next check: 75 NULL matches 70 SEF (less 5 to check). */

    if (need_param_sets) {
        c->param_sets_sent = 1;
        c->sent_mbs_wide = mbs_wide;
        c->sent_mbs_high = mbs_high;
        c->sent_l0 = c->pending_l0;
        c->sent_l1 = c->pending_l1;
    }

    /* * This is a confused Chrome ** real fixation point** (used A-B experimental positioning).
     *
     * The vaSyncSurface is not adapted to the vaSyncSurface (0 actual, Firefox co-situation 1,500).
     * And no DeviveImage/GetImage, so drive the "map-time" trails.
     * It's not leaving any of them. The send_unit has been pushed forward by 5ms recv to the session to the queue, and the Queueue of the Queue, which is the first time that the Queuesca has been able to get a job.
     * But if we don't write it here, Chrome will be back soon.
     * SurfaceReady / GPU sampled with an empty buffer or a previous round of pixels.
     *
     * - - A-B positioning experiments (order_slow.mp4 HEVC, browser pixels, 2 rounds each) -
     * Three separate changes were tested separately, based on "the actual frame in the picture":
     *
     *   Old version 9f00b1a6 (no code for this paragraph, write 0)
     *       The image is correct 3/20 with 4/20, cannot be identified 13 with 11
     *       Log "Return to Write Before" 0 times
     *   Variant (with this paragraph code, re-rewind 0)
     *       The picture is correct. Two rounds; "Return before writing" 21 times; backstage line reap=0
     *   Current version (with this paragraph code, write DPB cap)
     *       The picture is correct. Two rounds; "Return to write" 34 times.
     *
     * Conclusions
     * The code is working. The variant changes the reorder back 0 and still is all right.
     *   The value of the recorder that proves the synthesis of the SPS is irrelevant (consistency with md5 single variable).
     * - Nor is it a backstage line: the variant reap=0, without a single frame, it's all right.
     *   The backstage thread was just a little bit late for the long time when Chrome failed to adjust EndPicture.
     *
     * ⁇  This bug is undetectable with ffmpeg md5 (old version drives the md5 PASS),
     * Because ffmpeg goes DeriveImage/GetImage, where dmd_surface_wait is going to go.
     * Returns must be based on pixels from the browsers/ browser/.
     *
     * Only the frame already in the queue (next_frame against pend_pop immediately) returns,
     * Do not block hardware like hardware, avoid reintroducing the type of Firefox that is a dead-end lock.
     *
     * ⁇  (dmd_pending_has_exported_locked)
     * This time, memcpy is really expensive: it's going to cut the spitting by half -- real.
     * Jinjie_265.mp4,300 frames 76.8 fps * 34.8 fps. 25fps online stream is only 1.4 times
     * The balance is measured by the browser from 6 frames (dropped 6/34, old version 0/28).
     * ffmpeg/Firefox go Derive Image/GetImage, where dmd_surface_wait will go,
     * No need to synchronize writing here. */
    /* Only the surface collection (default open) for exporting dmabuf.
     * DMD_HARVEST_EXPORTELD_ONLY=0 may be closed and returned to the unconditional collection.
     *
     * Motive: Unconditional collection of frame to halve vomiting -- memcpy into dumb buffer one more time per frame.
     * The 2fps material is not budgeted for 500ms per frame, 25fps is available for 40ms, and 6 frames are measured.
     * And ffmpeg/Firefox go DeriveImage/GetImage, where dmd_surface_wait
     * The bottom-down frame does not need to be written in EndPicture; only Chrome is this
     * The consumer of "decoding pre-export dmabuf, then only EndPicture" must.
     *
     * Actual (jinjie_265.mp4,300 frames, quickest three times):
     *   Close 9.09s § 33.0 fps
     *   Open 5.35s ~ 56.1 fps
     * Chrome Pixels Correct (order_slow.mp4 HEVC tricycle + h264_slow.mp4):
     *   20/20, 20/20, 20/20, H.264 20/20, both non-conform and unidentifiable;
     *   The log confirms that the ExportSurfaceHandle hit 19 times, "Return before writing" 28 - 32 times, and then the "Return before writing" is a "Charlie" in the first place.
     *   The door handle did identify Chrome's surface and write it as usual.
     * ffmpeg side "Return to write before" is reduced to 0, with two-contracts returned to 9 full green. */
    static int exported_only = -1;
    if (exported_only < 0) {
        const char *e = getenv("DMD_HARVEST_EXPORTED_ONLY");
        exported_only = (e && e[0] == '0') ? 0 : 1;   /* Default start */
    }
    if (c->session && !drv->io_busy[idx] &&
        (!exported_only || dmd_pending_has_exported_locked(drv, c)) &&
        dmd_session_frames_pending(c->session) > 0) {
        struct dmd_session *hsess = c->session;
        drv->io_busy[idx] = 1;
        int harvested = 0;
        while (dmd_session_frames_pending(hsess) > 0) {
            pthread_mutex_unlock(&drv->lock);
            struct dmd_frame hf;
            memset(&hf, 0, sizeof(hf));
            int hrc = dmd_session_next_frame(hsess, &hf, 1);
            pthread_mutex_lock(&drv->lock);
            c = dmd_find_context_locked(drv, context);
            if (!c) {
                if (hrc == DMD_OK)
                    dmd_session_release_frame(hsess, &hf);
                io_release(drv, idx);
                pthread_mutex_unlock(&drv->lock);
                return VA_STATUS_ERROR_INVALID_CONTEXT;
            }
            if (hrc != DMD_OK)
                break;
            VASurfaceID hid = dmd_pending_take_locked(c, hf.unit_seq);
            struct dmd_surface *hs = dmd_find_surface_locked(drv, hid);
            if (hs) {
                surface_store_frame_locked(c, hs, &hf);
                hs->state = DMD_SURFACE_READY;
                harvested++;
            }
            dmd_session_release_frame(hsess, &hf);
        }
        /* Before the title is returned, I read that Io_release may have taken down the session that was later destroyed.
         * And c->session may have changed by then, and the log count is not this session. */
        const int left = dmd_session_frames_pending(hsess);
        io_release(drv, idx);
        if (harvested)
            dmd_log("EndPicture: wrote before returning %d frame(s) (session pending %d)\n",
                    harvested, left);
    }

    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}

VAStatus dmd_EndPicture(VADriverContextP ctx, VAContextID context)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    struct dmd_video_stats *stats = NULL;
    if (drv) {
        pthread_mutex_lock(&drv->lock);
        struct dmd_context *c = dmd_find_context_locked(drv, context);
        if (c) stats = dmd_video_stats_ref(c->video_stats);
        pthread_mutex_unlock(&drv->lock);
    }
    VAStatus status = end_picture(ctx, context);
    if (status != VA_STATUS_SUCCESS) dmd_video_stats_error(stats);
    dmd_video_stats_unref(stats);
    return status;
}

/* Frame retrieval in SyncSurface */

/* Cuff a frame back for the daemon into the surface buffer. The caller holds the lock.
 *
 * Geometry: frame of daemon press fmt.stride / fmt. slick_height layout, UV starting point =
 * The stride*slick_height.
 * Press a copy of the line when the discrepancy is inconsistent (the decoder has given different alignments) and never the whole metcpy -- that would be a misposition.
 */
/* Remove the "Next Should be Filled" surface from the code to be decoded queue and remove it.
 * Queues to return VA_INVALID_ID. The caller holds the lock.
 *
 * Who should fill in first by codec decides:
 * - VP9/VP8: No frame reorder, submission sequence = frame order, first queue
 * - H.264/HEVC: ffmpeg in decoding order, MediaCodec in display order out of frame,
 *   Must take (seq, POC) the smallest, otherwise all the places from frame 2 are misplaced
 *
 * Scannning with linear rather than stacking: up to 64 items in queue and measuring actual reorder depth only 2-3,
 * The cost of a frame-to-frame scan is negligible.
 */
static void av1_dead_push(struct dmd_context *c, uint64_t unit)
{
    if (c->av1_dead_count < 8) {
        int t = (c->av1_dead_head + c->av1_dead_count) % 8;
        c->av1_dead_unit[t] = unit;
        c->av1_dead_count++;
        return;
    }
    /* The old one is lost: the list is for the "late frame," and then it's gone.
     * It is unlikely that it will ever happen again and recovery of the oldest item will not affect the determination. */
    c->av1_dead_head = (c->av1_dead_head + 1) % 8;
    c->av1_dead_unit[(c->av1_dead_head + 7) % 8] = unit;
}

/* Unit in the list of waste? Take it and return it (each number is blocked by a single return). */
static int av1_dead_take(struct dmd_context *c, uint32_t unit)
{
    for (int k = 0; k < c->av1_dead_count; k++) {
        int i = (c->av1_dead_head + k) % 8;
        if (c->av1_dead_unit[i] != (uint64_t)unit)
            continue;
        for (; k + 1 < c->av1_dead_count; k++) {
            int cur = (c->av1_dead_head + k) % 8;
            int nxt = (c->av1_dead_head + k + 1) % 8;
            c->av1_dead_unit[cur] = c->av1_dead_unit[nxt];
        }
        c->av1_dead_count--;
        return 1;
    }
    return 0;
}

static VASurfaceID dmd_pending_take_locked(struct dmd_context *c,
                                           uint32_t unit_seq)
{
    if (c->pending_count <= 0)
        return VA_INVALID_ID;

    /* The hardware drop frame: this frame was already abolished in Sync and removed from the queue.
     * Now it's late -- it's thrown out, and it's never going to fall back into the assumption.
     * (The consequence of creating a hole is that every frame is later misplaced, see this function length comment. */
    if (unit_seq != 0 && av1_dead_take(c, unit_seq)) {
        dmd_log("match: dropping late output for unit %u after marking it as missing\n", unit_seq);
        return VA_INVALID_ID;
    }

    int pick = c->pending_head;

    /* ⁇  Recourse to the same source based on two side numbering has failed and changes must be read out first.
     *
     * The driver is a number for each ** frame** in EndPicture (the argument set is not passed through EndPicture), which is a very important tool for the development of a new system of social media.
     * The session layer originally added the number of nars to each send_unit -- the HEVC frame.
     * The argument set has a number in white, and two sets of numbers are staggered from this.
     *
     * Actual Effects (1920x1080 HEVC 12 Frame): ffmpeg mod 12 EndPicture Registration
     * 1.12 The session level sends out 15 units with 1.15 numbers. Matches match the values,
     * So the systematic error frame -- ** the frame number is exactly correct (12/12), but the picture is full of pictures.
     * The soface labeled POC 0 actually contains the display order 7 frame (the frame-by-frame match average difference of 0.000).
     *
     * Modified: unit_is_param_set_only() with pure argument set
     * Not numbering. The 12 frames are fixed by by bytes with the ffmpeg softened (md5,237fed06).
     *
     * V4L2 timestamp was misjudged during the screening -- backend self-checked to prove it's teleported correctly.
     * (Repeal values are not repeated and are validly arranged for the sending number) but only for the frame order.
     * The real problem is never in timestamp, but it's not the same as it is in the source.
     *
     * ⁇  AV1 Another similar failure** which has not been repaired: delay in the operation of a frame to disrupt the numbering rhythm.
     * Actual (synthetic stream 6/6 is correct by bytes):
     *   Match: Frame - > Surface 1 (unit 1 seq 0 POC 0, queue 0)
     *   Match: unit_seq=6 No matching, retreat order extrapolation (queue 4)
     *   Match: Frame - > framework 3 (unit 2 seq 0 POC 8, queue 3)
     * The second frame echoes unit_seq=6 but the corresponding entry is not found, and backsliding to the order extrapolation surface 3;
     * ffmpeg is waiting for the end of the session of the pixels of the summer 6  ⁇  2000ms  ⁇ flush  ⁇ ,
     * The entire section flowed out of only 1 frame.
     *
     * Reason: AV1 frames have to wait for the next frame to be counterproductive.
     * EndPicture does not deliver immediately (frame entry av1_hold).
     * The next step is to create a new social media platform that will be used to help people to learn how to use their own language.
     * The rhythm is staggered by the staggered mechanism; the third path of the sync flush is sent out of the frame.
     * The same problem (different source for both sides) as the above HEVC parameter number is accounted for.
     *
     * The AV1 pending_unit is refilled when it is really sent_unit.
     * or separate the numbering of the temporary frame when it is actually issued.
     *
     * Preference: Match precisely by submission serial number.
     *
     * The serial number of each input module is sent as v4l2_buffer.timestamp, and the hardware is returned to
     * The corresponding output frame (toools/ probe_negotiate.c measurements not rewritten) and so this frame
     * The facts as to which submissions were ** are known and do not require inference.
     *
     * So the pairing is completely decoupled from the frame sequence of the decoder: display order, follow input sequence, follow the sequence.
     * Even a new platform will not affect the correctness.
     * The implicit assumption is that "output is display order," and if it doesn't work, the image is mispositioned and it's not misreported.
     * (Accurates 105/150 frames misposition).
     *
     * unit_seq = 0 means that the capability (old version) is not supported and returns to the following assumption. */
    if (unit_seq != 0) {
        /* And once you see it, remember: Damon supports the exact pairing. That means it's open.
         * Follow the input sequence output (both introduced in the same version in Damon), laging to 1,
         * So there's no need to empty -- wait_is_futile to judge with this sign. */
        c->daemon_has_unit_seq = 1;
        for (int k = 0; k < c->pending_count; k++) {
            int idx = (c->pending_head + k) % DMD_MAX_SURFACES;
            /* Serial number 0 = disabled (resumed before re-establishment), not participating in exact matching */
            if (c->pending_unit[idx] != 0 &&
                c->pending_unit[idx] == (uint64_t)unit_seq) {
                pick = idx;
                goto found;
            }
        }
        /* The #Daemon side numbering is mostly a result of emptiness/reconstruction, which is a result of which the numbering of the daemon side numbers is often not matched.
         * You can also return the sequence to the inference that the frame is not dropped. */
        dmd_log("match: no pending entry for unit_seq=%u (queue size %d). Falling back to output order\n",
                unit_seq, c->pending_count);
    }
    /* *Decoding ** decoding **
     * Output, ffmpeg is submitted in decoding order - So submission = frame = frame & order,
     * All codecs are degraded to take the lead, and no POC reordering is required.
     *
     * The purpose of this is to root out a black frame: the default decoder is received when the B frame is available.
     * The 4th input unit is the first frame, the browser is only steady in flying 3 frames, the two are the same.
     * EOS is the only way to push out the frame -- which destroys the frame frame, leaving about nine squarely black.
     * (tools/probe_black.c). Decoding the output drops to one, disappears, disappears.
     * The emptiness/reconstruction/re-location is no longer required (tools/probe_keys.c). */
    /* Return path: When daemon does not provide unit_seq (old version), only extrapolation (seq, POC) is possible.
     *
     * This road relies on the assumption that "output is the order of the order," and the assumption is based on the daemon.
     * Decoder configuration -- the driver cannot know by itself. This is why the unit_seq is used to match precisely:
     * It's not necessary to guess. No frame reordering, no frame reordering, no frame = frame = frame = frame & line & just take the head. */
    if (c->codec == DMD_CODEC_H264 || c->codec == DMD_CODEC_HEVC) {
        unsigned int bseq = c->pending_seq[pick];
        int32_t best = c->pending_poc[pick];
        for (int k = 1; k < c->pending_count; k++) {
            int idx2 = (c->pending_head + k) % DMD_MAX_SURFACES;
            /* First than the serial number: the old sequence must be finished before the new sequence (IDR) is reset
             * The old sequence is not yet matched. */
            if (c->pending_seq[idx2] < bseq ||
                (c->pending_seq[idx2] == bseq && c->pending_poc[idx2] < best)) {
                bseq = c->pending_seq[idx2];
                best = c->pending_poc[idx2];
                pick = idx2;
            }
        }
    }

found:
    /* The head of the team crossed the count (see driver.h av1_head_stop): the first line is to go outside the first line of the team.
     * It means that one older unit has a newer one, but not a frame. */
    if (c->codec == DMD_CODEC_AV1)
        c->av1_head_stall = (pick == c->pending_head) ? 0 : c->av1_head_stall + 1;

    VASurfaceID head = c->pending[pick];
    /* Write down the last shell that has the true pixels for the shell to carry. */
    c->av1_last_ready = head;
    dmd_log("match: frame -> surface %u (unit %llu seq %u POC %d, %d pending)\n",
            (unsigned)head, (unsigned long long)c->pending_unit[pick],
            c->pending_seq[pick], (int)c->pending_poc[pick],
            c->pending_count - 1);

    /* Remove pick: move the whole of the items before it to one single point and then shrink the head.
     *
     * The Queen cannot use the "Put the first to the empty seat" method -- which would move the head elements into the middle of the queue.
     * ** Disruption of the matching of the remaining items with the order of submission**, and the smallest POC will be wrong when looking for the smallest POC.
     * (Specific evidence is partial frame correct, partial frame error). */
    for (int k = pick; k != c->pending_head;) {
        int prev = (k - 1 + DMD_MAX_SURFACES) % DMD_MAX_SURFACES;
        c->pending[k] = c->pending[prev];
        c->pending_poc[k] = c->pending_poc[prev];
        c->pending_seq[k] = c->pending_seq[prev];
        /* Pending_unit must be moved together. The missing number will cause the serial number to be misallocated with the surface,
         * The same number is duplicated -- the unit 5 and the unit 9 appear twice each.
         * Serial numbers 2 and 6 Disappeared by air, 70/150 frames mispositioned. */
        c->pending_unit[k] = c->pending_unit[prev];
        k = prev;
    }
    c->pending_head = (c->pending_head + 1) % DMD_MAX_SURFACES;
    c->pending_count--;
    return head;
}

/* Diagnostic: The pixel drop-down disc (DMD_SURF_DUMP2= < prefix>) that actually delivers** to consumers.
 *
 * File name with a drop-down plate number, target search, unit number, so you can judge without relying on display
 * "Driving into something that is surfaced right." Use: The same bar stream uses ffmpeg and browsers separately.
 * Run again, frame by frame to md5 -- this step cuts the bugs to the same byte.
 * "Decoding/shotch status" or "Export and show path". */
static void surf_dump_store(struct dmd_surface *s, unsigned unit)
{
    const char *pre = getenv("DMD_SURF_DUMP2");
    if (!pre || !pre[0] || !s->data || s->data_size == 0)
        return;
    static unsigned n;
    char path[512];
    snprintf(path, sizeof path, "%s.%04u.s%u.u%u.yuv", pre, ++n,
             (unsigned)s->id, unit);
    FILE *fp = fopen(path, "wb");
    if (fp) { fwrite(s->data, 1, s->data_size, fp); fclose(fp); }
}

/* When the frame is smaller: line by line can fit the top left corner, the rest is legal black.
 * Only walk here when the domb buffer is not loaded. */
static void store_clipped_locked(struct dmd_surface *s, const struct dmd_frame *f,
                                 unsigned int src_stride, unsigned int src_slice)
{
    if (!f->data || !s->data)
        return;

    const size_t plane = (size_t)src_stride * src_slice;   /* Source Y square bytes */
    if (f->size < plane + plane / 2)
        return;                      /* It's not even a whole frame. I'd rather not write it. */

    unsigned int cols = src_stride < s->stride ? src_stride : s->stride;
    unsigned int rows = src_slice < s->slice_height ? src_slice : s->slice_height;
    cols &= ~1u;                     /* Color size 2x2, odd numbers will be unbundled U/V */
    rows &= ~1u;

    dumb_sync_begin_write(s);

    for (unsigned int r = 0; r < rows; r++)
        memcpy(s->data + (size_t)r * s->stride,
               f->data + (size_t)r * src_stride, cols);
    for (unsigned int r = rows; r < s->slice_height; r++)
        memset(s->data + (size_t)r * s->stride, 0, s->stride);

    unsigned char *duv = s->data + (size_t)s->stride * s->slice_height;
    const unsigned char *suv = f->data + plane;
    for (unsigned int r = 0; r < rows / 2; r++)
        memcpy(duv + (size_t)r * s->stride, suv + (size_t)r * src_stride, cols);
    for (unsigned int r = rows / 2; r < s->slice_height / 2; r++) {
        unsigned char *line = duv + (size_t)r * s->stride;
        if (s->format == VA_RT_FORMAT_YUV420_10) {
            for (unsigned int k = 0; k + 1 < s->stride; k += 2) {
                line[k] = 0;
                line[k + 1] = 0x80;
            }
        } else {
            memset(line, 128, s->stride);
        }
    }

    dumb_sync_end_write(s);
}

static void surface_store_frame_data_locked(struct dmd_surface *s,
                                       const struct dmd_frame *f)
{
    s->visible_x = s->visible_y = 0;
    s->visible_width = s->width;
    s->visible_height = s->height;
    if (f->crop_left >= 0 && f->crop_top >= 0 &&
        f->crop_right >= f->crop_left && f->crop_bottom >= f->crop_top &&
        (unsigned)f->crop_right < f->width &&
        (unsigned)f->crop_bottom < f->height) {
        unsigned w = (unsigned)(f->crop_right - f->crop_left + 1);
        unsigned h = (unsigned)(f->crop_bottom - f->crop_top + 1);
        s->visible_x = (unsigned)f->crop_left;
        s->visible_y = (unsigned)f->crop_top;
        s->visible_width = w < s->width ? w : s->width;
        s->visible_height = h < s->height ? h : s->height;
    }
    /* Diagnosis: Is the frame from Damon itself black?
     * Matching detection with the same name in export.c. to distinguish
     * "The decoder has produced a black frame" and "cuffed into the darkface before it gets black."
     * DMD_VA_LUMA=1 is only available. */
    if (getenv("DMD_VA_LUMA") && f->data && f->size > 100000) {
        double sum = 0; int cnt = 0;
        for (size_t k = 0; k < f->size && k < 2000000; k += 1499) {
            sum += f->data[k]; cnt++;
        }
        double m = cnt ? sum / cnt : 0;
        dmd_log("Input luma: unit=%u mean %.1f%s\n",
                f->unit_seq, m, m < 20.0 ? "  ← daemon It’s black.!" : "");
    }

    unsigned int src_stride = f->stride > 0 ? (unsigned int)f->stride
                                            : (unsigned int)f->width;
    unsigned int src_slice = f->slice_height > 0
                                 ? (unsigned int)f->slice_height
                                 : (unsigned int)f->height;

    /* Update offsets [1] of surface:VAIMAGE according to the geometry given by the decoder
     * Use slick_height (1088) instead of displaying height (1080), otherwise the colour level is misplaced.
     * The value must wait for confirmation of "the frame is filled" -- see the branch below need > data_size. */
    size_t need = (size_t)src_stride * src_slice * 3 / 2;
    if (need > s->data_size) {
        /* The decoder gives a buffer larger than the pre-allocated (reverse resolution within the stream).
         *
         * The buffer from the calloc only allows realloc.
         * S->data is a mmap address for DRM dumb buffer (see surface_alloc_dumb).
         * Realloc must SIGS-glibc will take the mmp address forward
         * Malloc chunk header, reading pixel data.
         * Test the crash pad (round 8, ffmpeg-hwackel vaapi):
         *   __GI___libc_realloc (oldmem=0x7fafda3000, bytes=3136320)
         *   → surface_store_frame_locked → sync_surface_locked
         *   → dmd_SyncSurface2 → vaSyncSurface → av_hwframe_transfer_data
         *
         * The original approach when the dombbuffer was not loaded is "remember the new geometry and only the parts that can be tortured."
         * It's wrong: stride/slick is written in 1920/1088 and the map is still 1280x736.
         * Any subsequent reading of the colour level by stride*slick is crossed.
         * SIGSEGV takes it at nv12_copy
         * UV = src + 1920*1088.
         *
         * The real root is on the client: the surface pool is distributed according to the size of the pool when it is built, AV1 is replaced by a larger one.
         * The ffmpeg at resolution does not recreate the pool (the results are still at 1280x720 vaGetImage).
         * The whole frame is not going to be handed over. What drives is that frame to fail, not to drag it down.
         * The whole process -- sourface keeps old geometry and old content, and the person reading gets the frame. */
        if (s->exportable) {
            /* Domb Buffer can't grow a man.
             * "Remember New Geometry + Intercept" -- which would allow stride/ slick (1920/1088) to be actual
             * Map (1280 x 736 size) does not match, and any subsequent positioning colour & & & & & & & & & color; level;
             * Read all over the border. Check the grid for example (720p building pool ~ 1080p stream)
             * SIGSEGV at nv12_copy fetching UV = src + 1920*1088.
             *
             * Nor can you judge the frame for failure: ffmpeg received
             * VA_STATUS_ERROR_OPERATION_FAILED
             * Frame, rc=-5), the real scene is the same as the player stops as soon as it is clear.
             *
             * So, by VA-API, "surface less than image" general semantic treatment: ** to
             * The upper left corner of the surface can be installed. ** Geometry remains the same. The client has access to the serviceable.
             * The image (magnification) is not wrong.
             *
             * Root at client: the surface pool is distributed according to the size of the pool when built, AV1 is replaced by a larger resolution
             * The ffmpeg does not rebuild the pool (the results are still at 1280x720 vaGetImage). */
            dmd_log("surface %u: frame %ux%u exceeds the pool size %ux%u. Cropping to fit "
                    "(client kept the existing pool after a resolution change)\n",
                    (unsigned)s->id, src_stride, src_slice,
                    s->stride, s->slice_height);
            store_clipped_locked(s, f, src_stride, src_slice);
            s->decode_status = VA_STATUS_SUCCESS;
            return;
        }
        unsigned char *mem = realloc(s->data, need);
        if (!mem) {
            s->decode_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            return;
        }
        s->data = mem;
        s->data_size = need;
    }

    s->stride = src_stride;
    s->slice_height = src_slice;
    s->buf_width = (unsigned int)f->width;
    s->buf_height = (unsigned int)f->height;

    /* ⁇  CPU for dumb buffer must be written by START/ END of DMA_BUF_IOCTL_SYNC
     * Wrap, otherwise data may be parked in D-cache, GPU through exported dmabuf sampling
     * Read the old content. Actual symptoms: Firefox green screens alternate with normal images, with local colour blocks
     * (only END at the time) or constant green -- the latter because
     * And the buffer is completely zero with memset, and the NV12 is completely zero.
     * The CPU reading path (DeriveImage of ffmpeg) is the same cache, but it is not a good idea.
     * The md5 verification did not detect this defect. */
    dumb_sync_begin_write(s);

    size_t copy = f->size < need ? f->size : need;
    memcpy(s->data, f->data, copy);
    if (copy < need)
        memset(s->data + copy, 0, need - copy);

    dumb_sync_end_write(s);

    surf_dump_store(s, (unsigned)f->unit_seq);
    s->decode_status = VA_STATUS_SUCCESS;
}

static void surface_store_frame_locked(struct dmd_context *c, struct dmd_surface *s,
                                       const struct dmd_frame *f)
{
    surface_store_frame_data_locked(s, f);
    if (s->decode_status == VA_STATUS_SUCCESS) {
        dmd_video_stats_decoded(c->video_stats, s->visible_width, s->visible_height,
                                 f->hardware_error);
    } else {
        dmd_video_stats_error(c->video_stats);
    }
}

/* Take frame from daemon until target is ready or timed out.
 *
 * This is the matching point between VA-API and MediaCodec fluids. Targets surface usually
 * Not in the "should be filled" position -- so keep it in, fill in the first one until the target is on the line.
 * Who should fill in first by Codec decides: VP9/VP8 is the head of the team and H.264/HEVC is the smallest (seq, POC).
 */
static VAStatus sync_surface_locked(struct dmd_driver *drv, VAContextID context,
                                    VASurfaceID target, int timeout_ms)
{
    int idx;
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    idx = (int)(c - drv->contexts);

    /* Flush the buffered AV1 frame before waiting for output
     *
     * Delaying a frame will lock the latest synthetic frame in c->av1_hold, when the next frame arrives.
     * It's only after the last frame is sent out.
     * There's no more "the next frame" to squeeze it out.
     *
     * Actual Effects: A pyramid B, GOP with 6 frames (show_frame in order)
     * 1,0,0,0,0,1, EndPicture 6 sent out only 5 units,
     * Frame 6 --the only frame that shows_frame=1 except the first frame, which produces the output.
     * Always in the temporary memory. ffmpeg waits for pixels for surface=6, 5,000ms after timeout
     * The paper "Failed to read image from surface 0x6", the entire session is available only on a frame.
     *
     * So just waiting for the frame means that the upstream round is no longer ready and that the temporary frame must be washed out.
     *
     * ⁇  used to write here that "it's refresh is not counterproductive, so just keep the place value given by the rotation policy...
     * The last frame is not quoted in any subsequent frame, and the value does not affect the decoding outcome."** This assumption is wrong**,
     * Direct evidence:
     *   Source current terminal frames refresh_frame_flags=0x00,
     *   The rotation value is 0x20 -- the drop stream is matched by the source by by bytes.
     *   6 The top 5 frames in the frame sample are all identical, but the only last frame is different;
     *   "Error working frame: Input/output error"
     *   Aborted. The entire segment stream only exits from a frame.
     *
     * Reason: Refresh_frame_flags states "what reference slots do you want to use when this frame is decoded?"
     * The writing of the non-zero will allow the decoder to plug this frame into the reference slot and to take the original content off.
     * Breaking the frame state inside the decoder -- even if there is no frame explicit reference to it.
     *
     * Correct approach: Refresh to 0 when running out of the stagger.
     * Semanticly established: this is the point of not sending the upstream round, and the frame is not required to be in the reference slot.
     * If a new frame comes later, it will carry the correct refresh values without any impact. */
    /* ⁇  Expires the staves only after the input is actually completed.
     *
     * Tested: ffmpeg repeated rounding with shorter timeout Sync, triggered seven times
     * (a 10-frame stream) each time, the temporary frame was sent out as the "end frame".
     * The consequences are twofold:
     *   1. Refresh of the temporary memory frame has been recast to 0, and the frame is then quoted  ⁇  reference chain break
     *   Av1_hold becomes empty after delivery, next EndPicture takes the first frame branch.
     *      Only temporary storage of unsent data, the match between queue and surface is broken
     * The result of the cipher is that some sort of surface never gets the decoding results.
     * The ffmpeg newspaper "Failed to read image from surface 0x8")
     *
     * Input_finished by the upper finish_input path, that's the signal of the end of the flow. */
    /* ================================================================
     * Flush counter-valued ending (quantitative, not undisclosed)
     * ================================================================
     * 60 Frame sample comparison (DMD_AV1_NO_FLUSH switch, by bytes-by-count comparison):
     *   Open Flush (Status) Synthesis 61 Frame, 56/61 Bytes Correct
     *   Turn off frush synthesized 5 frames, 5/5 bytes correct, but -f null only 1 frame
     *
     * So: Flush is the source of all the remaining synthetic defects, ** and ** the remaining deficiencies are the cause of the problem.
     * But turning it off would keep the frame permanently locked - ffmpeg would stop delivering the material without the pixels.
     * Sessions are parked at "Into Unit 5, Received 1 Frame". Not on either side.
     *
     * Reason: The correct value for refresh_frame_flags is based on "ref_frame_map next frame"
     * The difference with this frame is "averse" (dmd_av1_watch_prev_refresh, the mechanism itself has been verified).
     * Flush submits the staged frame before the next frame arrives.
     * The moment was no difference, only the placeholder, and it was never changed after it had been issued.
     *
     * Why is the status quo still going on? 56/61: Actual source stream 39 frames, 18
     * Refresh is already 0, Flush writes 0, just hit in almost half the case.
     * It's a coincidence, not a correct -- 60 frames flash triggered 30 times.
     *
     * Two changes have been attempted and not validated:
     *   Input_finished door total dropped from 56 to 28 (worse, withdrawn)
     *   Not at all. Flush  ⁇  5/5 but the session is suspended (see above).
     *
     * Possible path (ext round of validation): defer the flash to the next bag of BeginPicture
     * Ref_frame_map before it's ready, so you can even get the right one.
     * This also prevents frames from being held indefinitely.
     *
     * Diagnostic switch: DMD_AV1_NO_FLUSH=1 does not run out of the stagger at all.
     * ================================================================ */
    /* The thugs only run out of it when "it's just the staggered frame."
     *
     * The only necessary scene for frush is that ffmpeg and so on is held in a temporary memory.
     * It's never gonna wait until it's out. If it's another surface, Flush, it's just redundant...
     * And it's expensive: the frame sent out has no chance of being counterproductive, only with place values.
     *
     * 60 frames sampled (bytes-by-te) measured:
     *   Unconditional Flush (30 times)  ⁇  56/61 correct
     *   Not at all Flush  ⁇  5/5 Correct but Stop Session (Show 5 units only)
     * By narrowing the trage, the goal is both to retain the necessary Flush and to reduce the number of errors. */
    const int hold_is_target = (c->av1_hold_surface == target);
    if (c->codec == DMD_CODEC_AV1 && c->av1_hold && c->session &&
        hold_is_target && !getenv("DMD_AV1_NO_FLUSH")) {
        unsigned char *held = c->av1_hold;
        const int held_show = c->av1_hold_show;
        size_t held_len = c->av1_hold_len;
        size_t held_bitpos = c->av1_hold_bitpos;
        size_t held_endupd = c->av1_hold_endupd_bitpos;
        const VASurfaceID held_surf = c->av1_hold_surface;
        c->av1_hold_surface = VA_INVALID_ID;
        c->av1_hold = NULL;
        c->av1_hold_len = 0;
        c->av1_hold_bitpos = (size_t)-1;
        /* Rewrite the refresh_frame_flags to 0 (8 bits, MSB in front).
         *
         * Refresh=0 means "This frame does not take any DPB slot" -- regardless of the real value of the source stream.
         * What, this pen doesn't destroy the existing slots.
         * "Replace 0 may erase the frame that was also cited from the slot," and "no shoving" is the opposite:
         * Nothing. Real refresh is counting when the next frame is EndPicture.
         * The last frame of the stream is not the next frame, but the last one is the last one.
         * And there's no frame to quote it, refresh=0 is equally safe. */
        /* ⁇  A path that has been proved: Flush uses the current pic_param difference to countervalue.
         *
         * The idea is: to come here c->av1_pic_param stored the last bag of BeginPicture
         * Parameters, if newer than the temporary frame, can be used as a base for the difference, and it is true refresh instead of writing 0.
         * Actual (60 frames, DMD_AV1_FLUSH_DERIVE):
         *   The differential path was executed 30 times, fully consistent with the results of 0 ** 56/61,
         *   There is a good number of fault frames (18/26/30/48/60).
         * Reason: dpb_shadow / prev_valid already in the same frame build_unit
         * It's updated by this pic_param, and the difference is the empty set.
         * The switch and code have been removed and only this record has been kept, so that it will not be tried again. */
        /* - 150 frame samples complete mapping of this path (for this round)
         * 75 triggers: 70 bitpos = 50, 5 bitpos = 58.
         * And all refresh class error frames are bitpos=58 the 5 frames
         * (Box 30/60/90/120/150), source refresh is all 0x10,
         * dpb_text_slot=5 (rotation 0x20).
         *
         * Three writing values that have been tried and not proved (Appropriation: Byte-to-Pari passu, Baseline 140/150):
         *   Write 0 (reservations) 140/150
         *   Retention of rotational placeholder (KEEP_ROT) 70/150
         *   Rewrite All 1<(slot-1) (SLOT1) 85/150
         *   Only bitpos = 58 with 1<(slot-1) 140/150 (no change)
         * Last no change: rewrite the timing of the occurrence and the dump log
         * dpb_text_slot is not at the same time, or the value is now advanced...
         * Can't rely on the slot counter-provocative target at this point.
         * Conclusion: 0 is still the best; the 5 frames are fixed by unwinding the frush.
         *
         * - Round 88: three candidates excluded from the 8-digit deviation
         * The last of the 5 frames_refresh_bitpos is 58, the remaining 70 frames are 50, 8 bits.
         * Fame_type x show_framex error_resilient_mode
         * All 16 combinations (tests/ test_av1_bitstream.c)
         * est_resh_bitpos_invariants)
         * (= width of prism_ref_frame, regulating 5.9.2 in
         *   The blog is a part of the blog of the ‘Assisting the Human Rights Watch’, which is a part of the blog’s work.
         * 3 Can't get 8 so the 8-bit difference** does not come from any of the three**.
         *
         * Also confirm that the frame_size_override_flag is not the reason: VA-API is not at all.
         * This field, which drives constant 0 and has a valid reason (size changes will restore the session).
         *
         * Remaining candidate: real stream bitpos before tile_info/ Quantification
         * A variable length field of the type requires a real bitstream of frame dum bitpos to locate.
         * That's for equipment online, and for remote recovery. */
        unsigned patch_val = 0;
        if (held_bitpos != (size_t)-1) {
            int patched = 1;
            for (int i = 0; i < 8; i++) {
                size_t bp = held_bitpos + (size_t)i;
                size_t byte = bp >> 3;
                if (byte >= held_len) { patched = 0; break; }
                unsigned bit = (patch_val >> (7 - i)) & 1u;
                if (bit)
                    held[byte] |= (unsigned char)(1u << (7 - (bp & 7)));
                else
                    held[byte] &= (unsigned char)~(1u << (7 - (bp & 7)));
            }
            dmd_log("sync: setting refresh to 0 for the final frame (bitpos=%zu %s slot=%u "
                    "rotating mask=0x%02x)",
                    held_bitpos, patched ? "succeeded" : "Skipping Crosses",
                    c->av1_dpb.dpb_next_slot & 7u,
                    1u << (c->av1_dpb.dpb_next_slot & 7u));
        }
        /* The frame context is not allowed to be written back.
         *
         * Double decoding (refresh=0 for pixels only) and then real when the next frame arrives
         * Reload DPB to set 7.19 SetFrameContext to execute
         * Two: copy this frame's CDF/delta-q/filtration terminal** at the end of the first trip
         * The context of the reference slot for the prism, the starting point for the second reading is no longer original.
         * The context -- so the second time a frame of error was solved with the correct frame refresh,
         * The reference is all gone after you put it in the DPB. This is what you used to do. "Repairing the re-emerged is 20/24.
         * (c) Mechanisms.
         * The first time I forced the death_frame_end_update_cdf=1 (no rewrite)
         * real value in av1_repair_endupd_val, written back and completed only once by restoration
         * Reverse. Two trips start at the same point  ⁇  the second pixel is the same as the first one. */
        {
            size_t eb = held_endupd;
            int orig = (eb == (size_t)-1) ? -1 : av1_bit_get(held, held_len, eb);
            if (eb != (size_t)-1 && orig >= 0) {
                av1_bit_set(held, held_len, eb, 1);
                c->av1_repair_endupd_bitpos = eb;
                c->av1_repair_endupd_val = orig;
            } else {
                c->av1_repair_endupd_bitpos = (size_t)-1;
                c->av1_repair_endupd_val = -1;
            }
        }
        /* Whether the last frame is in the queue -- both approaches have been tried, neither ideal, and remain in the queue here:
         *   Enter * The summer face will be re-registered when ffmpeg returns
         *            (Log "surface=8 already in pair queue" ), but pixels can be used to get out of 12 frames
         *   No, no, no, no, no, no, no, no, no, no, no, no, no, no, no, no, no, no, no, no, no, no, no, no, no.
         * The explanation that the duplicate item is not the main cause, but the real missing is "a surface carrying two frames."
         * This is not possible in the current pairing model. Leave the reset key (re-use)
         * Solface+unit is resolved when combining rather than just surface. */
        if (held_surf != VA_INVALID_ID &&
            c->pending_count < DMD_MAX_SURFACES) {
            int hq = (c->pending_head + c->pending_count) % DMD_MAX_SURFACES;
            c->pending[hq] = held_surf;
            c->pending_seq[hq] = c->last_seq;
            c->pending_poc[hq] = INT32_MAX;
            /* ⁇  Not proven: change message id (units_sent+1).
             *
             * Motive: echoed unt_seq from session unts_sent,
             * The blog is a blog of the blog ++units_submitted (based on EndPicture), which is a blog of the Global Voices community.
             * The physical registrations were 6,7,9,10,13,14 and the returns were 1,2,3,4,6,7,
             * 150 frames retreat 79 times -- numbers do differ from source.
             * Turn back 79 765, hit 21 .26/30, looks like it's improving.
             *
             * But pixel mass has deteriorated: 17 ff 4.
             * (b) The retreat path is inferted sequentially, instead giving a more reasonable pair;
             * "Accurate Match" matches the wrong frame.
             * Description of the numbering of different sources is just a face, the real difference is elsewhere...
             * Do not force alignment with the same source number until it is clear.
             *
             * Lessons learned: Accurate hits are misleading, and different images reflect real quality. */
            c->pending_unit[hq] = ++c->units_submitted;
            c->pending_count++;
            dmd_log("sync: queued final surface=%u (unit %llu)",
                    (unsigned)held_surf,
                    (unsigned long long)c->pending_unit[hq]);
        }
        dmd_log("sync: submitting the buffered AV1 frame (%zu bytes, refresh=0, added to the repair queue)",
                held_len);
        av1_dump_sent(held, held_len);
        av1_dump_tagged("1", held, held_len);
        (void)dmd_session_send_unit(c->session, held, held_len);
        c->av1_flushed++;
        if (held_show) c->av1_flush_show1++;
        /* Bytes in the fix queue: the next frame diff can be calculated for real as EndPicture
         * Refresh, rewrite and re-remit DPB (outload by sentry pending).
         * Send_unit has already copied the bytes into the V4L2 buffer, and the finger here is still ours. */
        free(c->av1_repair_buf);
        c->av1_repair_buf = held;
        c->av1_repair_len = held_len;
        c->av1_repair_bitpos = held_bitpos;
        c->av1_repair_frame = c->av1_hold_frame;

        /* Testing ruled out appending SEF in the flush path.
         *
         * Motive: 150 frames.
         *   EndPicture Sends 75 Frames, of which show=1 has only 5
         *   Flush sent 75 frames, of which show=1 has **75**
         * The frames are almost all gone, so it looks like the main battleground.
         *
         * However, the results of the survey were degraded: ffmpeg output fell from 150 frames to 111 frames, and the output fell from 150 frames to 111 frames.
         * And it's clearly slow down (up from about 40s to over 180s).
         * The cause theory (unsubstantiated): Flush is used on the path of the pixels.
         * Additional units here can disrupt the ongoing pairing and buffer return rhythm.
         * Conclusion: SEF cannot add to the Flush path. */
        /* Ownership handed over to the restoration queue (av1_repair_buf), no free--
         * Double release is measured munmap_chunk() abort. */
    }

    int spent = 0;
    /* Whether this wait has been emptied once. Reversed emptiness does not change the depth of the queue.
     * Without this gate, there will be repeated empties (frustration loops) in the same waiting period. */
    int drained_once = 0;
    /* A single waiting period is used for smaller pieces, which allow for periodic return to lock-in check-ups.
     *
     * The original value of 100ms is the daemon era, and it's the V4L2 that passes straight through.
     * The browser drops from 15.8% to 34.6% when measured to 10ms, extra wake-up grabs CPU
     * It is harmful (for more details see the slice log in dmd_v4l2_session.c). */
    const int slice_ms = 50;
    /* After so long before the frame was in place, the decoder was thought to be in the tail frame, active flush.
     *
     * ⁇  Can't take evidence that timeout_ms/2: the caller's timeout is not "end of the stream".
     * Firefox with a short timeout.
     * The time is now too late for the first time, and the time is now too late for the first time.
     * Execute irreversible shutdown (SHUT_WR), session is dead and browser permanently downs and softenes.
     * The command line ffmpeg used a big timeout, so it never exposed the problem.
     *
     * Flush is what's supposed to be done "upstream no more" and not about waiting for patience.
     * Therefore, fixed thresholds are used here and must be significantly greater than the time required for the decoder to fill the flow line. */
    const int flush_after_ms = DMD_FLUSH_AFTER_MS;

    for (;;) {
        struct dmd_surface *s = dmd_find_surface_locked(drv, target);
        if (!s)
            return VA_STATUS_ERROR_INVALID_SURFACE;
        if (s->state != DMD_SURFACE_PENDING)
            return s->decode_status;

        c = dmd_find_context_locked(drv, context);
        if (!c)
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        if (!c->session)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        if (c->pending_count <= 0) {
            /* Pending but queued: submission failed. Can't wait, never wait. */
            s->state = DMD_SURFACE_IDLE;
            return VA_STATUS_ERROR_DECODING_ERROR;
        }

        /* Long wait for frame failure: The decoder may be holding on to more input or EOS.
         *
         * MediaCodec Delayed 2-3 units (1/2/3 VCL after measured)
         * No frame comes out, no fourth, so the last frames at the end of the stream have to be closed to remove them.
         * And ffmpeg is blocking the vSyncSurface with no new data coming out -- both sides
         * We're just gonna have to start with the flush.
         *
         * ⁇ finish_input is irreversible shutdown (SHUT_WR): Once closed, this session
         * No more data can be sent. So only when "sustained enough time to actually jam" is done, and only once.
         *
         * The session will then be a one-time resource, but ** does not amount to confets **:
         * See below session_rebuild_locked -- next EndPicture will be re-established transparently
         * The session is also retransmitted to the argument set, which is not detected at the upper level. This is necessary for browsers:
         * ffmpeg steady state in browser only 3 frames are flying while MediaCodec has B frames
         * Time lag 4 frames (projected probe_lag: B 4 without B 1)
         * The flash is triggered here. If the flash is out of service, the browser gets I/ O error
         * The result is a permanent softening of the solution (the proof is that Firefox 140 hardwired only one frame is returned softly). */
        /* When should the flush: The verdict is "there can be no frame" and not "for long enough."
         *
         * When the depth of consumer feed is less than the depth required for the decoder to come out, the two sides necessarily have to be equal:
         * Consumers have to get the frame before they can deliver the next unit, and the decoder has to take another unit before they can get the frame out.
         * It's futile to wait -- the line depth doesn't change itself.
         *
         * The circle of Firefox is exactly like this: "Send 3 frames and wait 1 frames," and MediaCodec
         * The first frame is given in the fourth unit when B is available (tools/ probe_lag.c measured; daemon side)
         * The low-latency is not going down, that's the depth of the flow line inherent in the decoder.
         * If you equal 2000ms by the patient threshold, then Flush, 2 seconds per frame, and the play is dead.
         * The first time the flash is out of the frame.
         *
         * ⁇  It was written here that the price of the flush has been eliminated by the transparent reconstruction, and the upper levels can't detect it.
         * "------ that's wrong. That probe only counts in frame.
         * Without looking at the picture (all frames are black Y=16), it itself has been identified as untrustworthy at the beginning.
         *
         * The real cost of the flush is ** the reference frame was destroyed** and the transmission is followed by blacking to the next IDR.
         * The top level doesn't just notice. The user will see the "show-speech" right in the eye.
         * (actually 135/708 frames are pure black. So Flush is not a cheap operation.
         *
         * The line is deep enough to be based on patient thresholds -- in which case the frame is on the way.
         * The advance flash would interrupt a normal session in vain.
         *
         * ⁇  Reversed empties must be "only once a time." It's not like the finish_input will put it away.
         * Input_finished to block himself - no change in the depth of the back queue, condition still holds,
         * Without a gate, it becomes a busy cycle (1.13 million lively triggers, 1 frame only). */
        /* Only when "no more than impossible" is empty -- the drain will destroy the frame chain, a black frame root.
         *
         * When you support unit_seq, you also follow input sequence output, only 1 is behind.
         * The frame came almost immediately, and never judged futile (or it would have been zero ms, empty).
         * The error triggered 383 times. The old daemon only lags 4 and the consumer flies 3 frames together.
         *
         * ⁇ daemon_has_unit_seq' is ** a run-time observation of the first frame to be set**,
         * So when the session is just created and the first frame is not back, it's definitely zero. If only we look at it now, we can see it.
         * If you can't find a way to get out of the system, you can find no use in the 0ms and empty it immediately - and empty it = EOS + Flush, and then empty it immediately.
         * The reference chain was destroyed and the P/B frame was then completely blacked to the next IDR.
         *
         * Actual (browse loops 5 wheels): 1 ms per session triggers 1 octration per session,
         * 5 times corresponding to 5 black bars; 25-27 frames per black section (unit 4.30),
         * It's just the first IDR after it goes to the next before it. It's a total of 135/708 frames.
         * The log is "Oms 0, no frame, re-alertable (Queue 3)" -- the queue is clearly full.
         * It's on the way, it's on the way. It's not empty.
         *
         * So, a condition was added: ** At least one frame** was received before allowing the determination to be futile.
         * When a frame is not received, it's impossible to distinguish between "one another" and "the first frame is still on the road."
         * This time, patience should wait until the Flush_after_ms threshold, rather than immediately destroying the session state. */
        int wait_is_futile = dmd_session_frames_received(c->session) > 0 &&
                             !c->daemon_has_unit_seq &&
                             (c->pending_count < DMD_PIPELINE_DEPTH);

        if (!c->input_finished && !drained_once &&
            (wait_is_futile || spent >= flush_after_ms)) {
            dmd_log("flush triggered: futile=%d(recv=%llu has_seq=%d pend=%d) "
                    "spent=%d/%d\n",
                    wait_is_futile,
                    (unsigned long long)dmd_session_frames_received(c->session),
                    c->daemon_has_unit_seq, c->pending_count,
                    spent, flush_after_ms);
            /* The emptying also requires IO ownership ** and then ** the meeting pointer.
             * So this is blind Io_busy=1: the frame line may be holding up the session to do it, but IO is not going to be able to do it.
             * We're drawing it while we get it DQBUF, and we're turning it back and we're erasing it. */
            if (io_acquire(drv, idx, "SyncSurface/Empty") < 0)
                return VA_STATUS_ERROR_TIMEDOUT;
            if (!context_slot_ok(c, context)) {
                io_release(drv, idx);
                return VA_STATUS_ERROR_INVALID_CONTEXT;
            }
            struct dmd_session *fs = c->session;
            if (!fs) {
                io_release(drv, idx);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            /* Priority is given to reversible empty: daemon sends EOS to reset the frame and reload CSD, and then the user is able to reset the frame.
             * Sessions are still available -- so no reconstruction is necessary to save the connect+handhake+configure.
             * Only if the drain fails (old daemon does not recognize length 0) returns to the irreversible finish_input. */
            pthread_mutex_unlock(&drv->lock);
            int frc = dmd_session_drain(fs);
            int reversible = (frc == DMD_OK);
            if (!reversible)
                frc = dmd_session_finish_input(fs);
            pthread_mutex_lock(&drv->lock);
            io_release(drv, idx);
            c = dmd_find_context_locked(drv, context);
            if (!c) {
                pthread_mutex_unlock(&drv->lock);
                return VA_STATUS_ERROR_INVALID_CONTEXT;
            }
            if (reversible)
                drained_once = 1;
            else
                c->input_finished = 1;
            dmd_log("SyncSurface: no frame after %d ms, %s (rc=%d, "
                    "threshold %d ms, queue size %d)\n",
                    spent, reversible ? "Reversed discharge" : "flush Enter",
                    frc, flush_after_ms, c->pending_count);
            /* Re-search: The object may be destroyed during the locking. */
            continue;
        }

        if (spent >= timeout_ms) {
            dmd_log("SyncSurface: timed out after %d ms waiting for surface=%u (queue size %d)\n",
                    timeout_ms, (unsigned)target, c->pending_count);
            return VA_STATUS_ERROR_TIMEDOUT;
        }

        /* Serial IO, then lock the collection frame.
         *
         * The session pointer must be taken after the title has been acquired.
         * "Sess = c->session, wait for io_busy" and wait for the lock to be placed...
         * The meeting can be broken down by re-establishing the session.
         * So next_frame used a free session. */
        while (drv->io_busy[idx]) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&drv->io_done, &drv->lock, &ts);
            spent += 20;
            if (spent >= timeout_ms)
                return VA_STATUS_ERROR_TIMEDOUT;
            /* Other threads may have filled out our frame. */
            s = dmd_find_surface_locked(drv, target);
            if (!s)
                return VA_STATUS_ERROR_INVALID_SURFACE;
            if (s->state != DMD_SURFACE_PENDING)
                return s->decode_status;
        }
        /* The circular exit is "unoccupied", and the disposal slot is the Atomic takeover (which remains locked) and not the hard-jacket. */
        drv->io_busy[idx] = 1;
        if (!context_slot_ok(c, context)) {
            io_release(drv, idx);
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
        struct dmd_session *sess = c->session;
        if (!sess) {
            io_release(drv, idx);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        pthread_mutex_unlock(&drv->lock);

        struct dmd_frame frame;
        memset(&frame, 0, sizeof(frame));
        int rc = dmd_session_next_frame(sess, &frame, slice_ms);

        pthread_mutex_lock(&drv->lock);
        spent += slice_ms;

        /* I'm sure I'll be in line for the last time after the last touch. (Release_frame below).
         * First you get the sign, then the sign gets to the list.
         * Rebuild the session and we can take it apart -- release using the buffer that has been destroyed. */
        c = dmd_find_context_locked(drv, context);
        if (rc == DMD_OK) {
            if (c) {
                VASurfaceID head = dmd_pending_take_locked(c, frame.unit_seq);
                if (dmd_trace_order())
                    dmd_log("ORDER out    surf=%u unit_seq=%u waiting=%u pend=%d\n",
                            (unsigned)head, frame.unit_seq, (unsigned)target,
                            c->pending_count);
                c->frames_out++;  /* Diagnostic count: number of frames delivered for this session */
                struct dmd_surface *hs = dmd_find_surface_locked(drv, head);
                if (hs) {
                    surface_store_frame_locked(c, hs, &frame);
                    hs->state = DMD_SURFACE_READY;
                } else {
                    dmd_log("SyncSurface: pending surface %u was destroyed. Dropping the frame\n",
                            (unsigned)head);
                }
            }
            /* Release does not block IO (TCP mode only marks buffers can be reused). */
            dmd_session_release_frame(sess, &frame);
        }
        /* The wrong string is copied before the title is returned**.
         * "The meeting that was later destroyed was then removed."
         * And even if it returns to 0, another thread (Chrome is in DestroyContext) can be found.
         * The next moment, we'll take the same session apart. */
        char serr[160];
        serr[0] = 0;
        if (rc != DMD_OK && rc != DMD_ERR_TIMEOUT) {
            const char *m = dmd_session_last_error(sess);
            if (m)
                snprintf(serr, sizeof(serr), "%s", m);
        }
        const int sess_dead = io_release(drv, idx);

        if (!c)
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        if (sess_dead)
            break;               /* The session was postponed and torn apart. */
        if (rc == DMD_OK)
            continue;

        if (rc == DMD_ERR_TIMEOUT) {
            /* The frame is not out yet. Damon doesn't open the low-latency, the decoder will have a few frames.
             * It's a normal phenomenon -- keep waiting until the time is always over.
             *
             * But one thing keeps waiting is that it never lasts: the hardware is swallowed.
             * Some unit. Actual (1080p60 frames 1669, order_hint return)
             * Venus eats unit 3333/3324 into the UTPUT queue and does not return to the frame or return to the frame.
             * Back to error mark, then the unit goes out of frame as usual -- CAPTURE side 24/24 is all driven
             * In hand, the OTPUT side 0/8, the hardware level confirmed it was lost, not the one we didn't take.
             * The result is that the frame is too much to lose: the caller is full of 5,000 ms on vSyncSurface.
             * Discards the entire bar stream (rc=251) and equals "a drop frame = play aborted".
             *
             * The decision (used only on AV1 to avoid byte returns affecting the other four streams):
             * The frame follows the order, so "the unit that's been updated is out of the frame, the first team never returns."
             * indicates the decoder dropped the head entry. Require three later frames and at least 150 ms
             * Give room to the really late frame.
             *
             * Disposition: Deleting the head of the team (no holes left - entry as a waste)
             * If the actual frame is taken, it's the caller.
             * The one waiting for the surface is delivered and returned successfully with it** current content**.
             * So the player sees the same pattern that was found on the screen, and at most one image is wrong.
             * And the whole part is not interrupted. */
            if (c->codec == DMD_CODEC_AV1 && c->av1_head_stall >= 3 &&
                spent >= 150 && c->pending_count > 0) {
                int hp = c->pending_head;
                VASurfaceID dead = c->pending[hp];
                uint64_t du = c->pending_unit[hp];
                av1_dead_push(c, du);
                c->pending_head = (hp + 1) % DMD_MAX_SURFACES;
                c->pending_count--;
                c->av1_head_stall = 0;
                dmd_log("SyncSurface: treating unit %llu (surface %u) as dropped by hardware "
                        "after matching three later frames. Removing the pending entry\n",
                        (unsigned long long)du, (unsigned)dead);
                if (dead == target) {
                    struct dmd_surface *ls =
                        dmd_find_surface_locked(drv, target);
                    if (ls && ls->state == DMD_SURFACE_PENDING) {
                        ls->state = DMD_SURFACE_READY;
                        ls->decode_status = VA_STATUS_SUCCESS;
                    }
                }
            }
            continue;
        }

        if (rc == DMD_EOS) {
            /* The flow is over, but we are still waiting: the number of units sent does not match the number of output frames. */
            struct dmd_surface *s2 = dmd_find_surface_locked(drv, target);
            if (s2 && s2->state == DMD_SURFACE_PENDING) {
                s2->state = DMD_SURFACE_IDLE;
                s2->decode_status = VA_STATUS_ERROR_DECODING_ERROR;
            }
            dmd_log("SyncSurface: stream ended with %d frames still awaiting a surface match\n",
                    c->pending_count);
            return VA_STATUS_ERROR_DECODING_ERROR;
        }

        /* That's a mistake. */
        dmd_log("SyncSurface: could not receive a frame (rc=%d): %s\n", rc, serr);
        struct dmd_surface *s3 = dmd_find_surface_locked(drv, target);
        if (s3 && s3->state == DMD_SURFACE_PENDING) {
            s3->state = DMD_SURFACE_IDLE;
            s3->decode_status = VA_STATUS_ERROR_DECODING_ERROR;
        }
        return VA_STATUS_ERROR_DECODING_ERROR;
    }

    /* Only the session_dead will come here: the session was delayed for destruction during the locking frame of the current thread.
     * The slot is handed over to the next user, and no more sess. The one on the frame is later than
     * Terminate/reconstruction, the report is less relevant than the continuation of an honest conversation that does not exist. */
    dmd_log("SyncSurface: session destroyed while waiting (context rebuilt or terminated)\n");
    return VA_STATUS_ERROR_INVALID_CONTEXT;
}

/* Finds the context of the surface. */
static VAContextID surface_context_locked(struct dmd_driver *drv,
                                          VASurfaceID id)
{
    struct dmd_surface *s = dmd_find_surface_locked(drv, id);
    return s ? s->context : VA_INVALID_ID;
}

/* For intuitive synchronisation: The search was held before the release of the "GetImage".
 * Caller holds a lock; the function will temporarily lock the IO, and the lock will remain on return.
 * The caller must therefore re-search the object pointer that it caches. */
VAStatus dmd_surface_sync_locked(struct dmd_driver *drv, VASurfaceID surface,
                                 int timeout_ms)
{
    struct dmd_surface *s = dmd_find_surface_locked(drv, surface);
    if (!s)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (s->state != DMD_SURFACE_PENDING)
        return s->decode_status;

    VAContextID context = surface_context_locked(drv, surface);
    if (context == VA_INVALID_ID)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    return sync_surface_locked(drv, context, surface, timeout_ms);
}

VAStatus dmd_SyncSurface(VADriverContextP ctx, VASurfaceID render_target)
{
    return dmd_SyncSurface2(ctx, render_target,
                            (uint64_t)DMD_FRAME_TIMEOUT_MS * 1000000ULL);
}

static VAStatus sync_surface2(VADriverContextP ctx, VASurfaceID surface,
                          uint64_t timeout_ns)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* VA_TIMEOT_INFINITE also has a cap: the death of a triver will drag the host process. */
    int timeout_ms;
    if (timeout_ns == VA_TIMEOUT_INFINITE ||
        timeout_ns / 1000000ULL > (uint64_t)DMD_FRAME_TIMEOUT_MS)
        timeout_ms = DMD_FRAME_TIMEOUT_MS;
    else
        timeout_ms = (int)(timeout_ns / 1000000ULL);
    if (timeout_ms <= 0)
        timeout_ms = 1;

    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, surface);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (s->state != DMD_SURFACE_PENDING) {
        VAStatus st = s->decode_status;
        pthread_mutex_unlock(&drv->lock);
        return st;
    }
    VAContextID context = surface_context_locked(drv, surface);
    if (context == VA_INVALID_ID) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    struct dmd_context *sc = dmd_find_context_locked(drv, context);
    int c_pending = sc ? sc->pending_count : 0;

    if (dmd_trace_order())
        dmd_log("ORDER req    surf=%u pend=%d state=%d timeout=%dms\n",
                (unsigned)surface, c_pending, s->state, timeout_ms);

    /* There is a limit to the number of releases: only when "the number of frames is not enough to feed the decoder's waterline".
     *
     * Unconditional release will replace the lock with a lineless growth -- measured target rising all the way up to the top.
     * DMD_MAX_SURFACES overflow, ffmpeg "Failed to end picture decode"
     * Because the consumer is "send": without it, it keeps sending,
     * And taking pixels is only done when it has enough frame.
     *
     * So the amount of time that is taken is behind us.
     * Release when the number of frames is below it (filling the stream line is necessary to break the lock);
     * And then you block the frame, and then you block the frame -- and the decoder is already unexpected, and then you get it.
     * The queue was then queued and formed a stable back pressure. */
    /* The decision cannot be viewed solely as the sum of the pending_count.
     *
     * The total volume threshold was tested 12/7/5/4/3 and the browser drop rate was not met (93.5%,
     * 91.8%, 2-19%, volatile, fps 0.4, dead, 26.4%.
     * The real question is, "Is this the one I'm waiting for? Is the frame on the road?"
     *
     * The real verdict is in the queue: pending_unit[] remembers the serial number of each unit that has been submitted to the surface.
     * If the requested surface is in the queue, it says that its stream has been sent into the codec and the frame will be back.
     * It's time to wait honestly -- for consumers who take samples from dmabuf (Firefox/Chrome)
     * The only way to get out of this is to get rid of the "map" and the "map" and "get the "end of the road"" and then go.
     * It has been given an unfilled buffer, which is reflected in the frame throwing and the image jumping back.
     * If not in the queue, it's not submitted by EndPicture, and when it's meaningless...
     * At this point, release allows upstream supplies to continue, which is a necessary condition for breaking reciprocity. */
    int in_queue = 0;
    if (sc) {
        for (int k = 0; k < sc->pending_count; k++) {
            int qi = (sc->pending_head + k) % DMD_MAX_SURFACES;
            if (sc->pending[qi] == surface) { in_queue = 1; break; }
        }
    }

    dmd_log("Sync: surface=%u pending=%d timeout=%dms %s\n",
            (unsigned)surface, c_pending, timeout_ms,
            in_queue ? "Block the frame.(In line.)"
                     : (c_pending >= DMD_PIPELINE_DEPTH ? "Block the frame.(Line full.)"
                                                        : "Release(Not in the team.)"));

    if (in_queue || c_pending >= DMD_PIPELINE_DEPTH) {
        /* The delivery delay histogram (DMD_TRACE_ORDER=1 only).
         * The frame interval for 30fps is 33ms: Sync can't get the consumer if he drags through the value.
         * On time, it's a frame drop -- this is the key to the long end. */
        struct timespec ts0, ts1;
        int trace = dmd_trace_order();
        if (trace) clock_gettime(CLOCK_MONOTONIC, &ts0);

        VAStatus st = sync_surface_locked(drv, context, surface, timeout_ms);

        if (trace) {
            clock_gettime(CLOCK_MONOTONIC, &ts1);
            long us = (ts1.tv_sec - ts0.tv_sec) * 1000000L +
                      (ts1.tv_nsec - ts0.tv_nsec) / 1000L;
            static unsigned long n_call, n_over33, n_over66, max_us, sum_us;
            n_call++;
            sum_us += (unsigned long)us;
            if ((unsigned long)us > max_us) max_us = (unsigned long)us;
            if (us > 33000) n_over33++;
            if (us > 66000) n_over66++;
            dmd_log("LAT %ld us surf=%u st=%d\n", us, (unsigned)surface, st);
            if (n_call % 100 == 0)
                dmd_log("LAT summary: %lu calls, mean %lu us, peak %lu us, "
                        ">33ms %lu (%.1f%%), >66ms %lu\n",
                        n_call, sum_us / n_call, max_us,
                        n_over33, 100.0 * n_over33 / n_call, n_over66);
        }
        pthread_mutex_unlock(&drv->lock);
        return st;
    }

    /* ⁇  Here** can't wait until the frame is really in hand, otherwise the water line with MediaCodec is locked in depth.
     *
     * The measured time-series contradictions (probe_lag straight to daemon measured, unrelated to driver):
     *   MediaCodec has B frames that are behind 4 input units before throwing up the frame (no B frames that are behind 1)
     *   The ffmpeg steady state in the browser is only kept in 3 frames (H.264 rehearsing depth)
     *   When delivered, frame 3 is blocked at the vaSyncSurface and so on.
     * The two sides are on the right side: nobody moves.
     * The session is aborted and the browser gets I/O extract and then permanently returns softened.
     * Actual Firefox 140 so only 1 frame is harded out, EndPicture only calls 3 times
     * Just Destroy Context.
     *
     * Two roads (both measured) were rejected:
     *  • Duemon open low-latency: the lag is still 4, invalid (changes retained, interactively valuable)
     *  • Synthetic SPS writing max_num_reorder_frames: The real SPS is already with VUI,
     *    The old 4 -- which means it's the depth of the flow line inherent in the high-speed decoder, signal is not lost.
     *
     * So the blocking point must be moved: VA-API allows pixels to be really ready when the map is made
     * (Consumptions take the pixels to DeriveImage + MapBuffer, as is observed in Firefox/ffmpeg).
     * It's a "spiritual attempt" and it's a ffmpeg that's gone -- it's submitted to frame four.
     * The water line of MediaCodec was pushed through the threshold and the frame was released.
     * The real wait is in MapBuffer/DeriveImage (see dmd_service_wait. */
    const int probe_ms = 30;
    VAStatus status = sync_surface_locked(drv, context, surface,
                                         timeout_ms < probe_ms ? timeout_ms
                                                               : probe_ms);
    if (status == VA_STATUS_ERROR_TIMEDOUT) {
        /* The frame is still lined up in the decoder. It's not a mistake. The report is successful in getting the upstream feed.
         * Pixels are ready when Map. */
        status = VA_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&drv->lock);

    return status;
}

VAStatus dmd_SyncSurface2(VADriverContextP ctx, VASurfaceID surface, uint64_t timeout_ns)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    struct dmd_video_stats *stats = NULL;
    if (drv) {
        pthread_mutex_lock(&drv->lock);
        struct dmd_surface *s = dmd_find_surface_locked(drv, surface);
        struct dmd_context *c = s ? dmd_find_context_locked(drv, s->context) : NULL;
        if (c) stats = dmd_video_stats_ref(c->video_stats);
        pthread_mutex_unlock(&drv->lock);
    }
    VAStatus status = sync_surface2(ctx, surface, timeout_ns);
    if (status != VA_STATUS_SUCCESS) dmd_video_stats_error(stats);
    dmd_video_stats_unref(stats);
    return status;
}

/* Wait until the pixels of the surface are really ready.
 *
 * This is the real block point after SyncSurface leaves the road: the consumer has already sent the follow-up frame in at this point.
 * MediaCodec's current line is no longer unexpected, so wait here to get it. */
VAStatus dmd_surface_wait(struct dmd_driver *drv, VASurfaceID surface)
{
    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, surface);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    /* DMD_NO_MAP_WAIT=1: Not in Map Time Equivalent Frame, reporting only current status.
     *
     * This is the key switch** for the validation of the Crome Contract**, not the optimisation item.
     *
     * The problem is that Chrome never talks about vSyncSurface / DeriveImage / GetImage,
     * It's exported dmabuf after the CreateSurfaces, and then only EndPicture, and then it's a little bit more than a little bit more than a little bit of a little bit of a tack.
     * The DPB decides when to hand a surface to a synth sample.
     * This means that:** Pixels must be written in the searchface** when EndPicture returns.
     *
     * And ffmpeg goes DeriveImage/GetImage, the two entrances to the bottom-down frame of the function
     * (image.c: 169/:283). Then md5 returns to prove only "the logic of pairing," even if it's green.
     * The time-series premise that Chrome relies on -- the second and third rounds I misjudged on it twice.
     *
     * When this switch is open, ffmpeg still sends the full set of parameters buffers (decoding is guaranteed).
     * Pixels must be in place at the moment of the Map, otherwise reading an empty buffer ~ md5 is immediately inconsistent.
     * It turns "Chrome" into "Churme" for unfinished surface.
     * The regression test is no longer needed to look at the browser.
     *
     * Interpretation:
     *   Switch open + md5 consistent  ⁇  EndPicture returns pixel ready, Chhomey contract satisfied
     *   Switch opens + md5 inconsistent  ⁇  Sample window exists, Chrome will see the jump frame
     * The control group is always "open shut down + md5" (that's the ffmpeg/ Firefox contract). */
    {
        static int no_wait = -1;
        if (no_wait < 0) {
            const char *e = getenv("DMD_NO_MAP_WAIT");
            no_wait = (e && e[0] == '1') ? 1 : 0;
        }
        if (no_wait) {
            int state = s->state;
            VAStatus st = (state == DMD_SURFACE_READY)
                              ? s->decode_status
                              : VA_STATUS_ERROR_SURFACE_BUSY;
            pthread_mutex_unlock(&drv->lock);
            /* Only Pending is the real "sampling window": decoding has been submitted, pixels have not yet arrived.
             * IDLE is a capability survey that ffmpeg does before any submission (actually every operation)
             * Just one, surface 1) that had nothing to do with Chrome's jumping.
             * It's not statistically possible to misread "detection" as "competing". */
            if (state == DMD_SURFACE_PENDING)
                dmd_log("NO_MAP_WAIT: surface %u submitted but pixels are not ready"
                        ". Chrome may sample a partially written buffer\n",
                        (unsigned)surface);
            else if (st != VA_STATUS_SUCCESS)
                dmd_log("NO_MAP_WAIT: surface %u state=%d (not PENDING, "
                        "usually a capability probe before decoding)\n",
                        (unsigned)surface, state);
            return st;
        }
    }
    /* ⁇ IDLE = no decoding has been submitted, buffer is not initialized (or remnants from the frame),
     * The image must not be handed over.
     *
     * Actual: ffmpeg prior to submitting any EndPicture****
     * The DREIVIMAMAMAMAMA Detectability (DMD_VA_LOG actual state=0).
     * It gets a strange memory and is the first frame output.
     *
     * This bug is extremely disguised: 1920x1080 testsrc top half 797 lines are static,
     * As with the real frame by bytes, only the second half of 283 lines of dynamic gradients are exposed; and each time running,
     * The contents obtained differ (two times exactly match the soft frame 7 with the 5 frame, the average difference is 0.00).
     * If the material is static, it's misreported as "pass."
     *
     * IDLE reporting error is the correct act: reading a surface that has never been decoded is a time sequence error on the caller. */
    if (s->state == DMD_SURFACE_IDLE) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("surface_wait: refusing to read surface %u before it has been submitted for decoding\n",
                (unsigned)surface);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (s->state != DMD_SURFACE_PENDING) {
        VAStatus st = s->decode_status;
        pthread_mutex_unlock(&drv->lock);
        return st;
    }
    VAContextID context = surface_context_locked(drv, surface);
    if (context == VA_INVALID_ID) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    VAStatus status = sync_surface_locked(drv, context, surface,
                                         DMD_FRAME_TIMEOUT_MS);

    /* DMD_VA_TOLERTE_MISSING=1: Successful when frame is not available (pixels are preset).
     *
     * Pure diagnostic switch, default shutdown. Use: AV1 flow is only correct by "synthesis the whole stream."
     * "To the authority's softening" but if the headline is blocked, ffmpeg will stop sending the information.
     * We can only take six frames -- not enough to expose the problems of the frame.
     * Open it to get ffmpeg to run the full stream and to synthesize the results.
     * DMD_AV1_DUMP + DMD_AV1_DUMP_ALL）。
     *
     * ⁇  The image is necessarily wrong when it is opened and must not be used for functional authentication but only for sampling. */
    if (status == VA_STATUS_ERROR_TIMEDOUT) {
        const char *tol = getenv("DMD_VA_TOLERATE_MISSING");
        if (tol && tol[0] == '1')
            status = VA_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&drv->lock);
    return status;
}

VAStatus dmd_QuerySurfaceStatus(VADriverContextP ctx,
                                VASurfaceID render_target,
                                VASurfaceStatus *status)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !status)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, render_target);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    /* The semantic of not doing IO: QuerySurfaceStatus is "return to the current state immediately."
     * Blocking the frame is SyncSurface's job. */
    *status = (s->state == DMD_SURFACE_PENDING) ? VASurfaceRendering
                                                : VASurfaceReady;
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}
