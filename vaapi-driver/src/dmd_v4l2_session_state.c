/* Session queue, format and status helpers. The caller serializes access. */
#include "dmd_v4l2_session_private.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Preserve the negotiated buffer geometry and visible crop. */
static void publish_format(struct dmd_session *s)
{
    struct dmd_v4l2_dec *d = &s->dec;
    int cw = d->crop_w > 0 ? d->crop_w : d->w;
    int ch = d->crop_h > 0 ? d->crop_h : d->h;

    s->fmt.buf_width    = d->w;
    s->fmt.buf_height   = d->h;
    s->fmt.stride       = d->stride;
    s->fmt.slice_height = d->slice_height;
    s->fmt.crop_left    = 0;
    s->fmt.crop_top     = 0;
    s->fmt.crop_right   = cw - 1;
    s->fmt.crop_bottom  = ch - 1;
    if (!s->fmt.valid) s->fmt.changes++;
    s->fmt.valid = 1;

    dmd_session_log(s, "format: buffer=%dx%d visible=%dx%d stride=%d slice=%d",
             d->w, d->h, cw, ch, d->stride, d->slice_height);
}

/* CAPTURE reconfiguration unmaps the buffers referenced by queued frames.
 * Invalidate those borrowed pointers before publishing the new geometry. */
void dmd_session_publish_format_if_needed(struct dmd_session *s)
{
    if (s->dec.fmt_dirty) {
        s->dec.fmt_dirty = 0;
        if (s->pend_count > 0)
            dmd_session_log(s, "discard %d stale, unmapped frames after CAPTURE reconfiguration", s->pend_count);
        s->pend_head = s->pend_tail = s->pend_count = 0;
        memset(s->held, 0, sizeof(s->held));
        publish_format(s);
        return;
    }
    if (!s->fmt.valid && s->dec.cap_ready) publish_format(s);
}

/* Return zero on enqueue, or -1 if every CAPTURE slot is already queued. */
int dmd_session_pending_push(struct dmd_session *s, uint8_t *data, size_t len,
                     uint64_t pts, int index)
{
    if (s->pend_count >= DMD_V4L2_MAX_CAP) return -1;
    s->pend[s->pend_tail].data  = data;
    s->pend[s->pend_tail].len   = len;
    s->pend[s->pend_tail].pts   = pts;
    s->pend[s->pend_tail].index = index;
    s->pend_tail = (s->pend_tail + 1) % DMD_V4L2_MAX_CAP;
    s->pend_count++;
    return 0;
}

/* Return the oldest queued frame before receiving any later output. */
int dmd_session_pending_pop(struct dmd_session *s, uint8_t **data, size_t *len,
                    uint64_t *pts, int *index)
{
    if (s->pend_count <= 0) return 0;
    *data  = s->pend[s->pend_head].data;
    *len   = s->pend[s->pend_head].len;
    *pts   = s->pend[s->pend_head].pts;
    *index = s->pend[s->pend_head].index;
    s->pend_head = (s->pend_head + 1) % DMD_V4L2_MAX_CAP;
    s->pend_count--;
    return 1;
}

void dmd_session_log(struct dmd_session *s, const char *fmt, ...)
{
    if (!s || !s->log) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[dmd-v4l2] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void dmd_session_set_error(struct dmd_session *s, int code, const char *msg)
{
    if (!s) return;
    s->err.code = code;
    snprintf(s->err.msg, sizeof(s->err.msg), "%s", msg ? msg : "");
}

/* Status queries. */

const struct dmd_format *dmd_session_format(const struct dmd_session *s)
{
    return s ? &s->fmt : NULL;
}

const char *dmd_session_last_error(const struct dmd_session *s)
{
    return (s && s->err.msg[0]) ? s->err.msg : "";
}

int dmd_session_last_error_code(const struct dmd_session *s)
{
    return s ? s->err.code : DMD_ERR_INVAL;
}

int dmd_session_xfer_mode(const struct dmd_session *s)
{
    (void)s;
    /* Keep the existing shared-buffer transfer contract: pixels are already
     * mapped in this process and remain borrowed until release_frame. */
    return DMD_XFER_SHM;
}

int dmd_session_fd(const struct dmd_session *s)
{
    return s ? s->dec.fd : -1;
}

uint64_t dmd_session_units_sent(const struct dmd_session *s)
{
    return s ? s->units_sent : 0;
}

uint64_t dmd_session_frames_received(const struct dmd_session *s)
{
    return s ? s->frames_recv : 0;
}

int dmd_session_frames_pending(const struct dmd_session *s)
{
    return s ? s->pend_count : 0;
}

int dmd_session_pending_capacity(const struct dmd_session *s)
{
    (void)s;
    return DMD_V4L2_MAX_CAP;
}
