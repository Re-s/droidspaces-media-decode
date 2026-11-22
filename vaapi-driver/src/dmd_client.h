/*
 * dmd_clit.h - decode-daemon client library (for internal use by VA-API triver)
 *
 * This library sealed the "android side client for Android decode-daemon" thing into one.
 * The handle of the opacity session. It's eventually chained to msm_drm_drv_video.so by libva.
 * & & & & & & & & & & ;
 *
 *   - Never extra() / abort() / asert(): Any error is reported by return code
 *   - Never write stdout; log leaves stderr, and by default silent, DMD_VA_LOG=1
 *   - No global/static variable state: all states are in the sruct dmd_session,
 *     Multiple sessions can be organized in parallel with the process (one TCP connection per session, one MediaCodec example)
 *   - Timeout for all blockages: internal Poll + non-barrier fd, time-out return to DMD_ERR_TIMEOUT
 *   - Send MSG_NOSIGNAL: SIGPIPE is not cast for host process when the end is disconnected
 *   - All fd with CLOEXEC: host may fork/exec, do not leak fd
 *   - Reliminating only libc + pthread (actually not used pthread, but already in target link)
 *
 * Transfer mode is transparent to the user: dmd_frame.data points to shared memory pools in the SHM mode.
 * TCP mode points to the internal buffer of the session. Both cases must be called,
 * The data lapses after that. This is the primary value of this library -- the caller does not have to divide two paths.
 */
#ifndef DMD_CLIENT_H
#define DMD_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* - - - - - - - - - - - - - - - - - - - - - - - - - - Return to code */
/*
 * Commitment 0 Success, positive number is a non-correct flow state and negative number is a mistake.
 * The reason why humans can read (including errno description) can be found after an error.
 */
enum {
    DMD_OK            =  0,
    DMD_EOS           =  1,   /* The counter is closed and there is no frame. */

    DMD_ERR_INVAL     = -1,   /* Parameter Invalid */
    DMD_ERR_NOMEM     = -2,   /* Memory distribution failed */
    DMD_ERR_CONNECT    = -3,  /* Failed or timeout for connect */
    DMD_ERR_REJECTED  = -4,   /* Daemon refuses to shake hands, see dmd_error.handshake_status */
    DMD_ERR_IO        = -5,   /* V4L2 octl or read-write failed */
    DMD_ERR_TIMEOUT   = -6,   /* Waiting for timeout */
    DMD_ERR_PROTOCOL  = -7,   /* Bytes received that do not conform to the protocol */
    DMD_ERR_STATE     = -8,   /* Session state does not allow the operation (if already finished_input) */
    DMD_ERR_TOOBIG    = -9    /* Data module/framework exceeded the ceiling (no generation point at present, reserved for border check) */
    /* ⁇  Used to be DMD_ERR_ENDPOT_MISMATCH =-10 for the socket architecture
     * "Customer stat to endpoint inconsistent with the report of the daemon."
     * /dev/video32, no endpoint and no daemon,
     * The error code zero-point was deleted. */
};

/* codec id. The pre-line is part of the line agreement (with deleted decode-daemon.c).
 * CodecID is a value-by-value-consistency (CodecID), which is now used only internally to select V4L2 pixelformat.
 * ⁇  Still only add at the end, not re-allocated: these values are spread in the ability table for profiles.c
 * In the format map of dmd_v4l2_session.c, re-allocation will not be misreported but will be silently decoded. */
enum {
    DMD_CODEC_H264  = 0,
    DMD_CODEC_HEVC  = 1,
    DMD_CODEC_VP9   = 2,
    DMD_CODEC_VP8   = 3,
    DMD_CODEC_AV1   = 4,
    DMD_CODEC_MPEG2 = 5   /* 0.4.2 Append: Hardware ENM_FMT Listing MPG2 */
};

/* Frame Data Transfer Mode */
enum {
    DMD_XFER_TCP = 0,
    DMD_XFER_SHM = 1
};

/* Upline Data Unit cap (same as MAX_FRAME in daemon, beyond the length that will be convicted illegal by daemon) */
#define DMD_MAX_UNIT_BYTES (8u * 1024u * 1024u)
/* The frame below is a reasonable ceiling. Note that it must be much larger than the DMD_MAX_UNIT_BYTES:
 * 4K NV12 single frame is 12441600 bytes, 8MB of daemon only binds the input side. */
#define DMD_MAX_FRAME_BYTES (64u * 1024u * 1024u)

/* - - - - - - - - - - - - - - - - - - - - - - configuration */
/* V4L2 straight up just needs these five.
 * 0.3.x There are also three fields of port / lock_path / want_shm (TCP ports, ports, ports, ports, and
 * Unix matcht path, request SHM transfer) - change session to direct /dev/ video32
 * They are no longer read by any code, have been deleted at 0.4.0. */
struct dmd_session_config {
    int      codec;             /* DMD_CODEC_* */
    int      width;             /* 96..8192 */
    int      height;            /* 96..4320 */
    int      connect_timeout_ms;/* < =0 for default 2000 */
    int      io_timeout_ms;     /* Default timeout for single incoming and outgoing, <=0 for 5,000 */
    /* The CapTure side should be P010 instead of NV12 for > 8bit depth.
     * The session was usually created without knowing it. So leave one.
     * Postset port dmd_session_set_ten_bit(), obtained at the first EndPicture
     * Write it down by frame parameter -- CAPTURE consultations take place later in the SOURCE_CHANGE. */
    int      ten_bit;
};

/* Fills cfg with default values (H.264, 2s/5s timeout). cfg is NULL without action. */
void dmd_session_config_init(struct dmd_session_config *cfg);

/* - - - - - - - - - - - - - - - - - - - - - - - Error details */
struct dmd_error {
    int  code;              /* DMD_ERR_* */
    int  handshake_status;   /* code=status for daemon at DMD_ERR_REJECTED
                              * 1 = Version does not support 2 = codec Not supported 3 = Resolution Exceeding
                              * 4 = Lack of handshake; other cases 0 */
    char msg[192];          /* Human readable reasons always end with '\\0 ' */
};

/* Format */
/*
 * The geometric information for the decoder output buffer is derived from the formatting descriptor block of the daemon.
 *
 * Buff_width/buf_night is ** buffer size after filling (Venus aligns the width to 128, the height to 32, and the size of the buffer to be used to create a new environment for the benefit of the population).
 * 1080p output 1920x1088; real display area is given by Crop and the corp is **closed room**:
 *   Show width = crop_right - crop_ left + 1
 *   Show height = crop_bottom - crop_top +1
 * Positioning UV plane must use the stride/ slick_height: UV starting point = stride*slice_height.
 */
struct dmd_format {
    int buf_width;
    int buf_height;
    int stride;
    int slice_height;
    int crop_left;
    int crop_top;
    int crop_right;
    int crop_bottom;
    int valid;          /* 0 = No format block received */
    int changes;        /* Number of times format blocks are received >1 which indicates a change in the resolution inside the stream */
};

/* Show size-a-clip (crop closed), returns 0 when fmt is invalid */
int dmd_format_display_width(const struct dmd_format *fmt);
int dmd_format_display_height(const struct dmd_format *fmt);

/* - - - - - - - - - - - - - - - - - - Frameworks */
/*
 * A frame NV12 data. Data ownership is always in the library:
 *   SM mode  ⁇  Point to shared memory slots to return status 0 to daemon when release
 *   TCP Mode Point to the internal reception of buffers, release just to mark it as reusable
 * Both cases release and the data lapse, either the caller runs out or the caller uses it,
 * Or you can either torture it yourself. Forget about the decision in the SHM mode.
 * Clients are stuck and end the session, so release is mandatory.
 */
struct dmd_frame {
    uint8_t *data;
    size_t   size;

    /* Frame size (daemon with buffer size, same as fmt.buf_*) */
    uint32_t width;
    uint32_t height;

    /* Internal V4L2 metadata for telemetry. Existing pixel/VA return behavior
     * is unchanged, but a known bad CAPTURE is not a successful output. */
    int hardware_error;

    /* The frame** corresponds to the unit serial number** (1 event), 0 = Damon not provided.
     *
     * By daemon when queueInputBuffer writes pre-existing Times,
     * MediaCodec is brought to the output frame as it is. The use is for a precise pairing: the caller will know about it accordingly.
     * This frame was submitted several times,** and it is not necessary to know how the decoder came out in the order in which it was submitted**.
     * Old daemon is 0 when it does not have this field, and the caller returns to the inference in order. */
    uint32_t unit_seq;

    /* Geometric information that is valid for the frame is copied from the last formatted description block and is self-directed */
    int stride;
    int slice_height;
    int crop_left;
    int crop_top;
    int crop_right;
    int crop_bottom;

    int      shm_slot;   /* > = 0 = SHM slot number; 1 = TCP internal buffer */
    uint64_t seq;        /* Session internal frame number, starting with 0 */
};

/* - - - - - - - - - - - - - - - - - - Sessions */
struct dmd_session;
struct dmd_video_stats;
/* Retains telemetry independently of the VA context's/deferred IO lifetime. */
void dmd_session_set_video_stats(struct dmd_session *session,
                                 struct dmd_video_stats *stats);

/* Notify depth after session is created: the vaCreateContext stage is not ready for AV1
 * Bit_depth_idx in the first PPB) while CAPTURE consultation waits for the first module to be sent
 * SOURCE_CHANGE, SO WE HAVE TO MOVE HERE BEFORE THAT.
 * P010. CAPTURE is ready to call as invalid (with logs). */
void dmd_session_set_ten_bit(struct dmd_session *s, int ten_bit);

/*
 * Creates a session: connects to daemon, completes handshake, receives (if requested) a shared memory pool.
 * Returns NULL for failure;err is not NULL for failure (including the refusal code for daemon).
 * There will be no fd or map leaks when failure occurs.
 */
struct dmd_session *dmd_session_create(const struct dmd_session_config *cfg,
                                       struct dmd_error *err);

/* Release session: Close connection, demap, release buffer. S is NULL without action. */
void dmd_session_destroy(struct dmd_session *s);

/*
 * Sends a data module.
 *   H.264/HEVC: an NALU,** must have annex B starting code** (3 or 4 bytes) -
 *               Daemon locates nal_unit_header recognition by starting code
 *   VP8/VP9: A complete frame** without a starting code**
 * The library does not replace the caller's initials: the error is then quietly destroyed and placed in the top-level overt.
 * H.264/HEVC returns to DMD_ERR_PROTOCOL when the starting code is missing.
 * returns DSD_OK/DMD_ERR_*.
 */
int dmd_session_send_unit(struct dmd_session *s, const void *data, size_t len);

/*
 * Closes the write (SHUT_WR) triggers the daemon send EOS and Flush codec.
 * Number of data units sent! = number of frames returned (queuing, reordering, parameter set not producing),
 * This function must therefore be called to get all the remaining frames. You can repeat the call (sing, etc.).
 */
int dmd_session_finish_input(struct dmd_session *s);

/*
 * Reversed empty: drop the frame from the V4L2_DEC_CMD_STOP to the frame in hand, and reset when it is collected
 * The difference between CSD and finish_input is ** that is still available after the session**.
 *
 * Purpose: Consumers keep only a small number of frames flying (the browser is 3 frames), while decoder is received when the B frames are available
 * The 4th input unit is used to throw up the frame, each waiting for the other. This function is called to get the frame and not to use the session.
 * The only way to break each other is to use the finish_input at the cost of rebuilding the session.
 * (Specify 155 ms per frame, slow 4.7 times).
 *
 * The hardware is re-negotiated after the drain, and the nett_frame is automatically consumed internally and is unsatisfied.
 * returns DSD_OK/DMD_ERR_*.
 */
int dmd_session_drain(struct dmd_session *s);

/*
 * Take the frame. Timeeout_ms<0 means the io_timeout_ms in the configuration.
 * returns DMD_OK to frame, DSD_EOS end, DMD_ERR_TIMEOT temporarily without frame (the session is still available),
 * Other negative values are errors. The internal autoconsuming formatte profile block is updated and dmd_session_format(s).
 *
 * Note: The remaining bytes after the frame arrives by io_timeout_ms, not because the timeout_ms are small.
 * Half a frame; the time spent on the border "no one byte has yet come."
 */
int dmd_session_next_frame(struct dmd_session *s, struct dmd_frame *out,
                           int timeout_ms);

/* Receive currently available output without waiting for future frames.
 * Return DMD_ERR_TIMEOUT if no frame is ready. Ordering, format updates, other
 * results and release_frame ownership match next_frame. Serialize with all
 * other I/O on this session. */
int dmd_session_try_next_frame(struct dmd_session *s, struct dmd_frame *out);

/*
 * The frame is returned. The SHM mode is returned to slots, the TCP mode is released from internal buffers.
 * Both models must be called and the unconditional call is safe (f NULL or when returned inoperable).
 */
int dmd_session_release_frame(struct dmd_session *s, struct dmd_frame *f);

/* The contents of the last formatted description block. The returned pointer is valid during the session lifetime. */
const struct dmd_format *dmd_session_format(const struct dmd_session *s);

/* The last error is human readable; returns empty string without error, never returns NULL. */
const char *dmd_session_last_error(const struct dmd_session *s);

/* Last error code (DMD_ERR_*), 0 when no error */
int dmd_session_last_error_code(const struct dmd_session *s);

/* Transfer mode in effect: DMD_XFER_TCP/DMD_XFER_SHM.
 * The request may also be downgraded silently by the failure of the handover, using it to verify. */
int dmd_session_xfer_mode(const struct dmd_session *s);

/* Bottom block fd, caller's popup */
int dmd_session_fd(const struct dmd_session *s);

/* Statistics: Number of data units sent / frame taken */
uint64_t dmd_session_units_sent(const struct dmd_session *s);
uint64_t dmd_session_frames_received(const struct dmd_session *s);

/* Number of frames that have been solved and are being collected by the callers in the session to-do queue, and the capacity of the queue.
 *
 * Why it needs to be exposed: the to-do queue of the session (volume = CAPTURE buffer 24) relative to the driver
 * Side-to-side pairing (DMD_MAX_SURFACES=64) is much smaller. Each frame in the queue is held by one.
 * Unrefunded CAPTURE buffer, msm_vidc unwritten, not even input buffers
 * Recover, send_unit so permanently back down.
 *
 * The result is the back-up protection of the driver `pending_count > = DMD_MAX_SURFACES ' ** forever
 * The trigger cannot be activated **64 > 24, session locked long before it.
 * `-hwaccel_output_format vaapi-f null- ' , consumers are not at all vaSyncSurface/
 * vExportSurfaceHandle: 36 times EndPicture submission, 0 times frame extraction, log brush
 * "end=24/24 UPPUT Hanged after driver=8/8 CAPTURE =0/24".
 *
 * The driver then changes to the water slot to the session line to be empty in advance, instead of waiting for its own queue to be full. */
int dmd_session_frames_pending(const struct dmd_session *s);
int dmd_session_pending_capacity(const struct dmd_session *s);

#ifdef __cplusplus
}
#endif

#endif /* DMD_CLIENT_H */
