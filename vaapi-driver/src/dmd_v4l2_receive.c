/* One queue-first receive path for nonblocking attempts and explicit waits. */
#include "dmd_v4l2_session_private.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Publish geometry and the borrowed buffer token together with each frame. */
static void fill_frame(struct dmd_session *s, struct dmd_frame *out,
                       uint8_t *data, size_t len, uint64_t pts, int index)
{
    memset(out, 0, sizeof(*out));
    out->data   = data;
    out->size   = len;
    out->width  = (uint32_t)s->dec.w;
    out->height = (uint32_t)s->dec.h;
    /* Keep the input-unit identity carried through V4L2 timestamps.
     * Output order may differ from submission order. */
    out->unit_seq = (uint32_t)(pts / 1000ULL);
    out->stride       = s->fmt.stride;
    out->slice_height = s->fmt.slice_height;
    out->crop_left    = s->fmt.crop_left;
    out->crop_top     = s->fmt.crop_top;
    out->crop_right   = s->fmt.crop_right;
    out->crop_bottom  = s->fmt.crop_bottom;
    /* Preserve shm_slot as the CAPTURE buffer return token. */
    out->shm_slot = index;
    out->seq = s->frame_seq++;

    if (index >= 0 && index < DMD_V4L2_MAX_CAP) s->held[index] = 1;
    s->frames_recv++;
}

static int receive_once(struct dmd_session *s, struct dmd_frame *out,
                         int wait_ms)
{
    uint8_t *pixels = NULL;
    size_t length = 0;
    uint64_t timestamp = 0;
    int index = -1;

    /* Frames retained during send_unit must precede newer backend output. */
    if (dmd_session_pending_pop(s, &pixels, &length, &timestamp, &index)) {
        fill_frame(s, out, pixels, length, timestamp, index);
        return DMD_OK;
    }
    int result = dmd_v4l2_recv(&s->dec, &pixels, &length, &timestamp,
                                &index, wait_ms);
    int receive_errno = errno;
    dmd_session_publish_format_if_needed(s);
    if (result < 0) {
        char message[192];
        snprintf(message, sizeof(message),
                 "V4L2 receive failed: fd=%d, wait_ms=%d, errno=%d (%s)",
                 s->dec.fd, wait_ms, receive_errno, strerror(receive_errno));
        dmd_session_set_error(s, DMD_ERR_IO, message);
        return DMD_ERR_IO;
    }
    if (result == 2) {
        s->eos = 1;
        dmd_session_log(s, "received EOS");
        return DMD_EOS;
    }
    if (result == 0) return DMD_ERR_TIMEOUT;
    fill_frame(s, out, pixels, length, timestamp, index);
    return DMD_OK;
}

int dmd_session_try_next_frame(struct dmd_session *s, struct dmd_frame *out)
{
    if (!s || !out) return DMD_ERR_INVAL;
    /* next_frame(0) uses the configured timeout, so it cannot express a try. */
    return receive_once(s, out, 0);
}

static void log_receive_timeout(struct dmd_session *s, int budget)
{
    int capture_queued = 0, output_queued = 0, held_count = 0;
    for (int index = 0; index < s->dec.n_cap; index++)
        if (s->dec.cap[index].queued) capture_queued++;
    for (int index = 0; index < s->dec.n_out; index++)
        if (s->dec.out[index].queued) output_queued++;
    for (int index = 0; index < DMD_V4L2_MAX_CAP; index++)
        if (s->held[index]) held_count++;
    dmd_session_log(s, "next_frame timed out after %d ms (pending=%d, sent=%llu, received=%llu, "
                       "CAPTURE queued=%d/%d, caller-held=%d)",
                    budget, s->pend_count, (unsigned long long)s->units_sent,
                    (unsigned long long)s->frames_recv, capture_queued,
                    s->dec.n_cap, held_count);
    dmd_session_log(s, "  OUTPUT queued=%d/%d", output_queued, s->dec.n_out);
}

int dmd_session_next_frame(struct dmd_session *s, struct dmd_frame *out,
                           int timeout_ms)
{
    if (!s || !out) return DMD_ERR_INVAL;
    int budget = timeout_ms > 0 ? timeout_ms : s->io_timeout_ms;
    /* Preserve the explicit-wait slices and default timeout. A historical
     * Firefox 140/1080p30 experiment with 5 ms slices left mean SyncSurface
     * latency near 29 ms and raised drops from 15.8% to 34.6%. The original
     * implementation selected 20 ms to balance event handling and wakeups.
     * That evidence still matters for explicit waits. The reaper uses a try
     * because it must not exclude input while waiting for future output. */
    const int slice = 20;
    int waited = 0;
    for (;;) {
        int result = receive_once(s, out, slice);
        if (result != DMD_ERR_TIMEOUT) return result;
        waited += slice;
        if (waited < budget) continue;
        log_receive_timeout(s, budget);
        return DMD_ERR_TIMEOUT;
    }
}

int dmd_session_release_frame(struct dmd_session *s, struct dmd_frame *f)
{
    if (!s || !f) return DMD_ERR_INVAL;
    int idx = f->shm_slot;
    if (idx < 0) return 0;

    if (dmd_v4l2_release(&s->dec, idx) < 0) {
        dmd_session_set_error(s, DMD_ERR_IO, "failed to release CAPTURE buffer");
        return DMD_ERR_IO;
    }
    if (idx < DMD_V4L2_MAX_CAP) s->held[idx] = 0;
    /* Returning the dmabuf invalidates this borrow immediately. Clear the
     * token so a repeated release cannot return the same buffer twice. */
    f->data = NULL;
    f->size = 0;
    f->shm_slot = -1;
    return 0;
}
