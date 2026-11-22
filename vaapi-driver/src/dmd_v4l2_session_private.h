/* Private session state, protected by the caller's per-session I/O ownership. */
#ifndef DMD_V4L2_SESSION_PRIVATE_H
#define DMD_V4L2_SESSION_PRIVATE_H

#include "dmd_client.h"
#include "v4l2_backend.h"

struct dmd_session {
    struct dmd_v4l2_dec dec;
    int    log;
    int    codec;
    int    io_timeout_ms;

    struct dmd_format fmt;
    struct dmd_error  err;

    uint64_t units_sent;
    uint64_t frames_recv;
    uint64_t frame_seq;

    int  input_finished;
    int  eos;

    /* send_unit must retain output received while relieving input backpressure.
     * Each entry borrows a CAPTURE buffer until next_frame hands it to the
     * caller. Capacity matches the maximum number of CAPTURE buffers. */
    struct {
        uint8_t *data;
        size_t   len;
        uint64_t pts;
        int      index;
    } pend[DMD_V4L2_MAX_CAP];
    int  pend_head, pend_tail, pend_count;

    /* Buffers borrowed by callers. Several frames can be outstanding at once,
     * so ownership cannot be represented by a single current index. */
    unsigned char held[DMD_V4L2_MAX_CAP];
};

void dmd_session_publish_format_if_needed(struct dmd_session *s);
int dmd_session_pending_push(struct dmd_session *s, uint8_t *data, size_t len,
                             uint64_t pts, int index);
int dmd_session_pending_pop(struct dmd_session *s, uint8_t **data, size_t *len,
                            uint64_t *pts, int *index);
void dmd_session_log(struct dmd_session *s, const char *fmt, ...);
void dmd_session_set_error(struct dmd_session *s, int code, const char *msg);

#endif
