/*
 * dmd_v4l2_session — dmd_clit.h interface V4L2 is implemented directly.
 *
 * Replace the original socket+de-deaemon link: the driver opens itself
 * /dev/video32 (msm_vidc_driver) decodes and no longer requires cross-process communication.
 *
 * Why do you do this? (Specify overturning earlier judgment): inside /dev/ video32 with
 * /dev/dma_head/systems belong to the droidspaces-gpu group, where the container user is,
 * ** The decoding is done without root**. No root decoding of AV1 inside the container is done by 10 frames,
 * All four codecs are available, and three examples are available without interference.
 *
 * The only reason why daemon exists (inter-processed interview with Android MediaCodec) disappeared.
 * Remove it: Socket, go round, go round, go round, and get a full picture of memcpy (1080p is 3MB),
 * The SHM slot is for restitution agreements, handshake and version negotiation, the daemon process and watchdog.
 *
 * The dmd_client.h interface is maintained -- decode.c without changing the line, and the & & & & & & & & ;
 * It still sees the record of the creation/send_unit/text_frame/release_frame/drain/destroy.
 * This modification is thus transparent for the upper layers and allows for cross-checking.
 *
 * Semantic differences with the daemon version (semantics specified at each function):
 * - dmd_frame.data directs dmabuf map to V4L2 and is no longer received by the socket
 *   Copy. Release_frame cannot be overwritten before release lapses immediately after.
 * - Shm_slot field is re-used as CAPTURE buffer index (> = 0), and semantically still "refundable documents".
 * - unit_seq from Ben achieved through V4L2 timestamp, the rules are consistent with the daemon version
 *   (Seq*1,000 microseconds in feed, removes when frame is collected)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "dmd_v4l2_session_private.h"
#include "video_stats.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Log switch that is consistent with dmd_client.c: only "1" is opened literally. */
static int v4l2_log_wanted(void)
{
    const char *e = getenv("DMD_VA_LOG");
    return e && e[0] == '1';
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - configuration */

void dmd_session_config_init(struct dmd_session_config *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    /* Port / lock_path / want_shm with direct V4L2 is meaningless and the field is reserved for
     * ABI and caller codes are not destroyed at dmd_client.h; this is done without them. */
    cfg->codec = DMD_CODEC_H264;
    cfg->connect_timeout_ms = 2000;
    cfg->io_timeout_ms = 5000;
}

int dmd_format_display_width(const struct dmd_format *fmt)
{
    if (!fmt || !fmt->valid) return 0;
    if (fmt->crop_right > fmt->crop_left)
        return fmt->crop_right - fmt->crop_left + 1;
    return fmt->buf_width;
}

int dmd_format_display_height(const struct dmd_format *fmt){
    if (!fmt || !fmt->valid) return 0;
    if (fmt->crop_bottom > fmt->crop_top)
        return fmt->crop_bottom - fmt->crop_top + 1;
    return fmt->buf_height;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - The life cycle of the conversation */

void dmd_session_set_ten_bit(struct dmd_session *s, int ten_bit)
{
    if (!s)
        return;
    /* Only when CAPTURE is not ready: no one will be able to negotiate again after the geometry is done.
     * That situation (the deep change in the inside of the same session) was not supported by the fact that it would be preferable to leave a log. */
    if (s->dec.cap_size && !!ten_bit != !!s->dec.ten_bit) {
        dmd_session_log(s, "ignore bit-depth change (%d to %d) after CAPTURE setup",
                 s->dec.ten_bit, ten_bit);
        return;
    }
    s->dec.ten_bit = ten_bit ? 1 : 0;
}

struct dmd_session *dmd_session_create(const struct dmd_session_config *cfg,
                                       struct dmd_error *err)
{
    if (!cfg) {
        if (err) { err->code = DMD_ERR_INVAL; snprintf(err->msg, sizeof(err->msg),
                                                "Missing session configuration"); }
        return NULL;
    }

    struct dmd_session *s = calloc(1, sizeof(*s));
    if (!s) {
        if (err) { err->code = DMD_ERR_NOMEM; snprintf(err->msg, sizeof(err->msg),
                                                "Out of memory"); }
        return NULL;
    }
    s->log = v4l2_log_wanted();
    s->codec = cfg->codec;
    s->io_timeout_ms = cfg->io_timeout_ms > 0 ? cfg->io_timeout_ms : 5000;

    /* VP8 Not achieved: Driver-driven default stream re-established.
     * Hardware support VP80 (80th round of actual count confirmed), following an earlier note that "no VP80 format" was wrong.
     * This gives clear errors, not silent failures, which allow for upward positioning. */
    if (!dmd_v4l2_probe(cfg->codec)) {
        dmd_session_set_error(s, DMD_ERR_REJECTED, "V4L2 decoder does not support this codec");
        if (err) *err = s->err;
        dmd_session_log(s, "probe failed: codec=%d is unsupported by /dev/video32", cfg->codec);
        free(s);
        return NULL;
    }

    if (dmd_v4l2_open(&s->dec, cfg->codec, cfg->width, cfg->height) < 0) {
        dmd_session_set_error(s, DMD_ERR_IO, "failed to open V4L2 decoder");
        if (err) *err = s->err;
        free(s);
        return NULL;
    }
    /* Open memset over d, the depth must fall behind it. */
    s->dec.ten_bit = cfg->ten_bit ? 1 : 0;

    dmd_session_log(s, "session opened: codec=%d %dx%d (direct V4L2)",
             cfg->codec, cfg->width, cfg->height);
    if (err) { err->code = 0; err->msg[0] = 0; }
    return s;
}

void dmd_session_set_video_stats(struct dmd_session *s, struct dmd_video_stats *stats)
{
    if (!s || s->dec.video_stats == stats) return;
    dmd_video_stats_unref(s->dec.video_stats);
    s->dec.video_stats = dmd_video_stats_ref(stats);
}

void dmd_session_destroy(struct dmd_session *s)
{
    if (!s) return;
    dmd_session_log(s, "session closed: sent=%llu units, received=%llu frames",
             (unsigned long long)s->units_sent,
             (unsigned long long)s->frames_recv);
    dmd_v4l2_close(&s->dec);
    dmd_video_stats_unref(s->dec.video_stats);
    free(s);
}

/* Determines whether an Annex-B module contains only a set of parameters (SPS/PPS and H.264).
 *
 * Why must we distinguish between relying on the serial number of the "drive registration" and "V4L2 return timestamp" in the same way?
 * No. The driver is added to each ** frame** in EndPicture (parameter set without EndPicture),
 * And if you add a number to each of the send_units, the argument set will be empty numbered and the two sides will be staggered from this.
 *
 * The results are measured (HEVC 1920x108012 frame): ffmpeg mod 12 EndPicture registrations 1.12,
 * The session layer sends out 15 cell numbers 1.15. Matching equals the values, and then systematically error frames --
 * Frame numbers are exactly correct (12/12), but the picture is a very good one: the surface called POC 0
 * The actual display order 7 frames (the average difference of 0.000 by frame).
 *
 * This bug is exposed only in dynamic images: the first half of the test material 797 is a static pattern, and the first half of the test material is exposed to the same kind of material.
 * As with the frame by bytes, only the second half of the dynamic gradient is visible. */
static int unit_is_param_set_only(int codec, const unsigned char *d, size_t len)
{
    /* The AV1:show_existing_frame module does not advance the unit number.
     *
     * This is a TD+SEF frame (of 3-4 bytes) that does not contain any tile data,
     * Hardware will re-enact a decoded frame. The drive-driven pending queue is registered only as the real frame.
     * So it** cannot ** take a unit_seq -- otherwise all the real PTSs that follow.
     * All of them are shifted and the pairs are all on the retreat path.
     *
     * This is the same as the HEVC Parameter Set: b86ab9f is the one that was submitted for repair
     * The place of error caused by "units_submitted by frame, unit_sent by send_unit"
     *
     * The decision is: length < = 8 and the first OBU is TD and the second is FRAME_HEADER, and the second is the `FRAME_HEADER'.
     * Or first, the FRAME_HEADER.
     *
     * ⁇  Scope: This repair is necessary (without fixing all the real frames after SEF)
     * PTS offset one pair to retreat path)
     * "SEF number over 20, and then crash" -- after completing the test SEF_MAX=40 is still time-consuming.
     * That ceiling is for other reasons and has not yet been established. */
    if (codec == DMD_CODEC_AV1) {
        if (len > 8 || len < 2)
            return 0;
        size_t i = 0;
        /* optional TD(type=2, obu_size=0) */
        if (((d[i] >> 3) & 0xF) == 2) {
            if ((d[i] >> 1) & 1) {
                if (i + 1 >= len || d[i + 1] != 0)
                    return 0;
                i += 2;
            } else {
                i += 1;
            }
        }
        if (i >= len)
            return 0;
        /* Then must FRAME_HEADER(type=3) with the first load show_existing_frame=1 */
        if (((d[i] >> 3) & 0xF) != 3)
            return 0;
        size_t pl = i + 1;
        if ((d[i] >> 1) & 1)
            pl = i + 2;             /* Skip lib128 obu_size (Hing 1 byte) */
        if (pl >= len)
            return 0;
        return (d[pl] >> 7) & 1;    /* show_existing_frame */
    }

    if (codec != DMD_CODEC_H264 && codec != DMD_CODEC_HEVC)
        return 0;
    size_t i = 0;
    int saw_vcl = 0, saw_any = 0;
    while (i + 4 <= len) {
        /* Positioning Starts (3 or 4 bytes) */
        size_t sc = 0;
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1)
            sc = 3;
        else if (i + 4 <= len && d[i] == 0 && d[i+1] == 0 &&
                 d[i+2] == 0 && d[i+3] == 1)
            sc = 4;
        if (!sc) { i++; continue; }
        size_t h = i + sc;
        if (h >= len) break;
        saw_any = 1;
        if (codec == DMD_CODEC_HEVC) {
            /* HEVC NAL Head 2 bytes, type = (byte0 > > 1) & 0x3F.
             * VCL NAL is 0.31; VPS=32 SPS=33 PPS=34 AUD=35 EOS/EOB/ FD/SEI=36.40. */
            unsigned t = (unsigned)((d[h] >> 1) & 0x3F);
            if (t < 32) saw_vcl = 1;
        } else {
            /* H.264 NAL Head 1 byte, type = byte0 & 0x1F.
             * VCL is 1 (non-IDR)/5 (IDR); SPS=7 PPS=8 SEI=6 AUD=9. */
            unsigned t = (unsigned)(d[h] & 0x1F);
            if (t == 1 || t == 5 || (t >= 2 && t <= 4)) saw_vcl = 1;
        }
        i = h + 1;
    }
    /* Only when nal is actually parsed and there is no VCL in it is a pure argument set cell. */
    return saw_any && !saw_vcl;
}

/* - - - - - - - - - - - - - - - - - - - - - - - Delivery */

int dmd_session_send_unit(struct dmd_session *s, const void *data, size_t len)
{
    if (!s || !data || len == 0) return DMD_ERR_INVAL;

    /* Back pressure processing: Backend returns 1 for currently free input buffer. This is not an error...
     * The buffer is recycled after the frame is collected. You must try again without dropping the unit: drop any access module
     * And it's gonna destroy the following frame of reference, and the picture's gonna be broken.
     *
     * Difference from the daemon version: that's a one-line line over there with the output and recycling.
     * So you have to start recv pushing the current line. */
    /* Pure Parameter Set Unit does not number: The driver adds one number to each frame in EndPicture.
     * The argument set does not pass EndPicture, and it must be skipped here, otherwise the two side numbers are staggered. */
    const int ps_only = unit_is_param_set_only(s->codec,
                                               (const unsigned char *)data, len);

    int spins = 0;
    for (;;) {
        /* The argument set follows the current number (not to take next number), and the VCL module is advanced. */
        int r = dmd_v4l2_send(&s->dec, data, len,
                              (uint64_t)(s->units_sent + (ps_only ? 0 : 1))
                                  * 1000ULL);
        if (r == 0) {
            if (!ps_only)
                s->units_sent++;

            /* Harvest output after every successful send, even before the
             * caller asks for it. Earlier msm_vidc tests received 6/6 or 10/10
             * units when drained promptly, but stopped at 2/6 when receive
             * ran only on input backpressure. Retain frames in FIFO order.
             *
             * Keep this opportunistic wait at 1 ms. A zero-timeout harvest
             * behaved like no harvest in those tests; 50-100 ms waits caught
             * output but serialized submission. A former 5 ms wait averaged
             * 3.9 ms per send. On the two-submission path that cost about 8 ms
             * per output frame and limited 60 fps content to 51 fps. At 1 ms,
             * the tested stream decoded at 120-190 fps with matching pixels.
             *
             * Those results justify this post-send harvest, not a background
             * barrier. Explicit waiters use next_frame. The reaper only tries
             * ready output and paces itself after releasing session I/O. */
            for (;;) {
                if (s->pend_count >= DMD_V4L2_MAX_CAP - 1) break;
                uint8_t *pd = NULL; size_t pl = 0; uint64_t pp = 0; int pi = -1;
                int pr = dmd_v4l2_recv(&s->dec, &pd, &pl, &pp, &pi, 1);
                if (pr == 1) {
                    dmd_session_publish_format_if_needed(s);
                    if (dmd_session_pending_push(s, pd, pl, pp, pi) < 0) {
                        dmd_v4l2_release(&s->dec, pi);
                        break;
                    }
                    continue;
                }
                if (pr == 2) { s->eos = 1; break; }
                break;                    /* 0 = not available, normal */
            }
            return 0;
        }
        if (r < 0) {
            dmd_session_set_error(s, DMD_ERR_IO, "V4L2 input submission failed");
            return DMD_ERR_IO;
        }

        /* r = = 1: Enter a buffer. Push a frame loop to allow the driver to recover the buffer.
         * You can't throw the frame away here -- if you really get the frame, you don't get the caller.
         * It's a question of calling the sequence. It's better to mistreat than to drop the frame. */
        uint8_t *fd = NULL; size_t fl = 0; uint64_t fp = 0; int fi = -1;
        int rr = dmd_v4l2_recv(&s->dec, &fd, &fl, &fp, &fi, 20);
        if (rr == 1) {
            /* The frame is received but the caller has not yet come to pick it up -- deposit into the to-do queue and never drop it.
             * The next frame of reference will be destroyed by the loss of any frame. */
            if (dmd_session_pending_push(s, fd, fl, fp, fi) < 0) {
                /* The queue capacity equals the CAPTURE buffer number, which is theoretically not available.
                 * It was true that the buffer count was flawed and that the frame was returned to avoid leakage. */
                dmd_v4l2_release(&s->dec, fi);
                dmd_session_set_error(s, DMD_ERR_STATE, "pending-frame queue overflow");
                return DMD_ERR_STATE;
            }
            dmd_session_publish_format_if_needed(s);
            continue;      /* The queue has released a buffer. */
        }
        if (rr == 2) {
            s->eos = 1;    /* Line up; the remaining frame will still be removed from the queue/ recv */
        }
        if (rr < 0) {
            dmd_session_set_error(s, DMD_ERR_IO, "receive failed while relieving input backpressure");
            return DMD_ERR_IO;
        }
        if (++spins > 500) {          /* 500 × 20ms = 10s */
            /* When these locks are diagnosed, the three counts are key: pend_count full.
             * The OOTPUT is all in the driver's hands, which means the consumer doesn't take the frame, the CAPTURE doesn't return, the consumer doesn't take the frame.
             * Solids are stopped. Print when DMD_VA_LOG=1. */
            int qout = 0, qcap = 0;
            for (int i = 0; i < s->dec.n_out; i++)
                if (s->dec.out[i].queued) qout++;
            for (int i = 0; i < s->dec.n_cap; i++)
                if (s->dec.cap[i].queued) qcap++;
            dmd_session_log(s, "input backpressure timed out: pending=%d/%d OUTPUT queued=%d/%d "
                        "CAPTURE queued=%d/%d",
                     s->pend_count, DMD_V4L2_MAX_CAP,
                     qout, s->dec.n_out, qcap, s->dec.n_cap);
            dmd_session_set_error(s, DMD_ERR_TIMEOUT, "input buffers remained unavailable");
            return DMD_ERR_TIMEOUT;
        }
    }
}

int dmd_session_finish_input(struct dmd_session *s)
{
    if (!s) return DMD_ERR_INVAL;
    if (s->input_finished) return 0;
    s->input_finished = 1;
    /* The notification driver did not have more data to push out the remaining frames in the stream. */
    if (dmd_v4l2_drain(&s->dec) < 0) {
        dmd_session_set_error(s, DMD_ERR_IO, "failed to request decoder drain");
        return DMD_ERR_IO;
    }
    dmd_session_log(s, "input finished, drain requested");
    return 0;
}

int dmd_session_drain(struct dmd_session *s)
{
    if (!s) return DMD_ERR_INVAL;

    /* semantic description: the upper level (SyncSurface of decode.c) is calling the function while the frame is timed out,
     * Wants to "cat out the backlog frame but the session is still available". It divides the value by return:
     *   DMD_OK — Reversed, drawn_once, continue the feed
     *   DMD_OK — Return to irreversible finish_input, end of session
     *
     * Neither of these roads can be really travelled.
     *   . STOP will push the session forward to the DRAIN substate, and the CMD_START is rejected.
     *     （dmesg: "av1D: msm_vidc_streaming_state: (CMD_START) not allowed,
     *      Sub_state (DRAIN) ** Since then, no more frames have been removed**
     *   • The V4L2 norm, which is therefore not available on this platform, is not available in the country.
     *     Actual START returns EBUSY
     *
     * The failure to return is also harmful: the upper level immediately finishes the stream.
     *
     * So here's the successful no-op -- do not move the codec so the upper level thinks "defended and can continue."
     * This is safe: Ben's end_unit gets the DQBUF collection every time the feed comes in.
     * (msm_vidc demanding timely return of CAPTURE buffer until decoded)
     * They were removed in time and no additional emptiness was required to rush.
     *
     * When it's really over, it's the end of the story that comes out of the STOP.
     *
     * But returning to DMD_OK will allow the upper layers to be drawn after ** never to trigger a real drain.
     * And the decoder does hold the tail frame -- those frames are only eOS.
     *
     * Actual (H.264,300 frames): Draw returns OK when the sunface 7 is 5,000 ms full of time,
     * ffmpeg abandoned the frame and the tail came in "filled and filled" at DestroyContext.
     * But at that time, the upstream delivery was no longer in order to harden the 298/ soften the 300 frame.
     * The decoder itself is a very large set (log: sent 300 units, received 300 frames),
     * Drop frame occurs between the driver and the ffmpeg.
     *
     * So here goes back "not available", and let the upper level go back to finish_input to send the real EOS.
     * The price is that the session is not ready to be delivered after the session - but the upper layer is only waiting for the frame to pass.
     * And the input is not yet finished and the stream is actually over. */
    (void)s;
    dmd_session_log(s, "reversible V4L2 drain unavailable; caller must finish input to receive tail frames");
    return DMD_ERR_STATE;
}
