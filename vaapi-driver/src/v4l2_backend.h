/*
 * v42_backend - Decode V4L2 M2M interface directly to msm_vidc and bypass MediaCodec/Codec2.
 *
 * Reason for Existence: AV1 support for SM8750 for Codec2 -- c2.qti.av1. decoder created in component
 * The stage failed, logcat visible
 *     QC2VppFilterCaps: Unknown codectype=c2.qti.av1.decoder
 *     QueryrequiredInfos from downstream failed!
 * VPP filter downstream does not know AV1 and the component cannot rise. But hardware and kernel drive are fully available:
 * The UOTPUT_MPLANE item of /dev/video32 lists AV10, which is a direct transmission to the V4L2 that can decode the frame, and the following:
 * And pixels are fully consistent with the ffmpeg soft resolution point (testing conclusion for tools/v4l2_dec_probe.c).
 *
 * The above section is a 0.3.x location, which is outdated: 0.4.0 ** all** codec is on the back end,
 * No MediaCodec that way.
 *
 * Key constraints (all measured and modified to be maintained):
 *
 * This paragraph is rewritten in 0.4.1 as a whole. The previously recorded "video32 decoding path of nabu" does not start."
 *    This conclusion** has been reversed ** — nabu can now be deconstructed with ffmpeg sobrete
 *    1080 p Frame (300/300 frame MD5 is the same and two different streams are identical).
 *    The lack of buffer type, Ion head, no privileges, no solidity, was not a problem.
 *    The four private agreements are required by article 4/6/7/8 below.
 *    Source: MiCode/Xiaomi_Kernel_OpenSource branch nabu-r-oss.
 *
 * 1. Type of buffer: msm_vidc** only V4L2_MEMORY_USERPTR**,
 *    Both MMAP and DMABUF REQBUFS return EINVAL
 *    （q->io_modes = VB2_MMAP | VB2_USERPTR，msm_vidc.c:1548）。
 *
 *    But USERPTR is a nominal value -- a driver vb2_mem_ops.get_userptr is a stubble function:
 *        static void *vidc_get_userptr(...) { return (void *)0xdeadbeef; }
 *                                              (msm_vidc.c:717-720)
 *    The real source of buffer is
 *        b->m.planes[i].m.fd        = b->m.planes[i].reserved[0];
 *        b->m.planes[i].data_offset = b->m.planes[i].reserved[1];
 *                                              (msm_vidc.c:533-536)
 *    That is, **dmabuf fd must be written into plane.reserved [0]**, m.userptr completely ignored.
 *    Maps occur at QBUF (msm_vidc_common.c: 6693-6708 msm_smem_map_dma_buf).
 *    This explains the "USERPTR process ended without getting out of the frame" trap.
 *
 *    Buffer source: nabu no dma_heap, only /dev/ion, system head id=**25**
 *    (Use ION_IOC_HEAP_QUERY to ask the kernel, do not test mask on location).
 *
 * S_FMT (OUTPUT) must first be called by a dumimmy with a ** different resolution**.
 *    Drives to return early when the resolution is equal to the current value, skips the plain_sizes grant:
 *        If (fourcc is the same & & west is the same & height) {
 *            dprintk("No change in OUTPUT port params"); return 0;
 *        }                                     (msm_vdec.c:732-738)
 *    After the target open, the OUTPUT defaults to 1920x1088, which is directly targeted and will be hit.
 *    sizeimage was 0 when configured directly, and 16588800 after an initial 1280x720 S_FMT.
 *
 * 3. The output resolution for input must be strictly equal and this device does not support scaling:
 *        if (!scale_x.min || !scale_x.max || !scale_y.min || !scale_y.max) {
 *            if (input_w * input_h != output_w * output_h) return -ENOTSUPP;
 *        }                                     (msm_vidc_common.c:5613-5625)
 *    So 1080p uses 1920x**1088** (capture values for CAPTURE), not 1080.
 *    STREAMON 524 (ENOTSUPP) instead of EINVAL - which can be distinguished when size does not match.
 *
 * 4. ** Must activate SECANDRY diversion mode** (this is the decisive element missing from the year):
 *        V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_MODE = base + 22 = 0x00992016
 *        V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_SECONDARY = 1
 *                                     (Producer v4l2-controls.h: 865-869)
 *    CAPTURE under PRIMARY (default) directly returns DPB, start_streaming()
 *    -EINVAL. SECONDARY Enables HAL_BUFER_OUTPUT2, DPB separate from OPB,
 *    msm_vidc.c: 1214-1222 will be extra adjusted msm_comm_set_output_buffers()
 *    Let the driver assign the DPB himself, the path is completely different.
 *    Single variable is measured by: SECONDARY  ⁇  STREAMON (OUT) OK; PRIMARY  ⁇  EINVAL.
 *    Accompanied benefit: CAPTURE has linear NV12, without having to solve UBWC's Q128.
 *
 * 5. Private control project site (plant v422-controls.h:684);
 *        V4L2_CID_MPEG_MSM_VIDC_BASE = V4L2_CTRL_CLASS_MPEG|0x2000 = 0x00992000
 *    Initializations against OMX HAL live object:
 *        0x00992003 base+3  VIDEO_OUTPUT_ORDER    = 0
 *        0x00992011 base+17 VIDEO_EXTRADATA = 2, 25, 31, 29 (four consecutive)
 *        0x00992038 base+56 VIDEO_LOWLATENCY_MODE = 1
 *    Note that base+15 SYNC_FRAME_DECADE 1 will press MIN_BUFFERS out=1 cap=1,
 *    Not applicable to normal decoding.
 *
 *    ⁇  previously recorded "QUERYCTRL 0, S_CTRL EINVAL, no control item at this node"
 *    ** Wrong**: QUERYCTRL does not list the private CID (they are not in the standard control framework).
 *    But directly S_CTRL these ids are **successful**. Use QUERYCTRL results to extrapolate control items
 *    The opposite conclusion is reached if there is no one. (G_CTRL does not verify id, take 0xDEADBEEF and returns success.)
 *    This is still valid, and it is not used to verify that the control item exists.
 *
 * 6. The true semantic of STREAMON:** The first was false passage and the second was fully certified**.
 *        Case OPPUT_MPLANE: if (CAPTURE already streaming) rc = start_streaming();
 *        Case CAPTURE_MPLANE: if (OUTPUT already streaming) rc = start_streaming();
 *                                              (msm_vidc.c:1294-1302)
 *    The first STREAMON ** skipped**start_streaming () due to the fact that the side was not streaming.
 *    So "Capture first streamon succeeds" is a false image, failure is just postponed to the second.
 *    This also overturns the old note 2 "must be two stages, not two-way together" Streamon:
 *    The SECANDRY mode doesn't need to wait for an event to match CAPTURE, and the two sides are set before the second.
 *    The Streamon trigger verification is sufficient.
 *
 * ** Must subscribe to private vendor events**, driven by never issuing criteria V4L2_EVENT_SOURCE_CHANGE:
 *        V4L2_EVENT_MSM_VIDC_START = V4L2_EVENT_PRIVATE_START + 0x1000
 *                                  = 0x0800000 (planter videodev2.h: 2290-2305)
 *          +1 FLUSH_DONE   +2 PORT_SETTINGS_CHANGED_SUFFICIENT
 *          +3 PORT_SETTINGS_CHANGED_INSUFFICIENT  +4 BITDEPTH_CHANGED
 *          +5 SYS_ERROR    +6 RELEASE_BUFFER_REFERENCE
 *          +7 RELEASE_UNQUEUED_BUFFER
 *    Subscription only to the standard SOURCE_CHANGE will lead to poll never returning to POLLPRI...
 *    That's why "Wait for Source_Change never arrives," not that the solids didn't respond.
 *
 * 8. Every PORT_SETTENGS event is to leave the solid in_reconfig, and both
 *    Processing ** Different **. Driven inst->in_reconfig = true
 *    (msm_vidc_common.c:1761) The solids were then stopped in reconfig waiting state.
 *    There are only two calls:
 *      a. Start_streaming() in_reconfig
 *                                              (msm_vidc.c:1240-1252)
 *      b. branch of V4L2_QCOM_CMSION_CONTINUE ()
 *                                              (msm_vidc_common.c:4155)
 *
 *    +2 SUFFICIENT (alternate)  ⁇  (b): VIDIOC_DECODER_CMD+cmd=5
 *      (videodev2.h:1991), do not move in queue status.
 *      ⁇  msm8998 from kernel autocontinue, but sm8150 generation to
 *      **userpace must send it himself** otherwise it's permanently stuck waiting for FBD.
 *    +3 IFFIENT (requires re-equipment OPB)  ⁇  Must go (a), sequence as per Article 12.
 *
 *    The platinum cannot be made into a session-level single lock: Firefox seek/ ABR will repeat on the same fd
 *    In the case of a breach, the drop of the solid stopped in_reconfig until the upper layer was over-timed.
 *
 * ** Device must be opened with O_NONBLONK**.
 *    The kernel v4l2_event_dequeue(), the poll cycle will never return.
 *    Positioning method: /proc/PID/wchan displays v4l2_event_dequeue,
 *    /proc/PID/syscall shows 29 (ioctl) + 0x80885659 (VIDOC_DQEVENT).
 *
 * Two geometric pits for CAPTURE:
 *     . slick_height does not reverse the sizeimage. 3137536*2/(1920*3) = 1089.16
 *       ~ 1089, the real value is 1088, and the extra line will allow the upper level to cross. nv12_copy
 *       SIGSEGV. Use f.fmt.pix_mp.night.
 *     The default value of the residual value of 1920x1088 for the possible return of the open value of G_FMT(CAPTURE),
 *       (a) Not associated with S_FMT (OUTPUT) (4K-code flow) and must be covered with the OUTPUT side consultation value;
 *       Overwrite the width after the drive does not fill the bytesperline, stride has to take the bottom of the wheel.
 *
 * 11. Output 1088 line contains 8 line alignment filling, valid area to be conditioned to 1080.
 *     ⁇  do not let the reference end output 1088 lines - fill data participation matches
 *     All the lines are misallocated, and the false conclusion is that "PSNR only 25.9 dB" (this session has been stepped on).
 *
 * **PORT_SETTENGS_INSUFFICIENT re-sequence sequence (0.4.4; see cap_reconfig).**
 *     Browser seek/ ABR clarity switch will trigger it repeatedly on the same fd. Formal sequence
 *     (Eppecute_omx_flush + PortDisabled by OMX omx_vdec_v4l2.cpp):
 *
 *       DECODER_CMD{cmd=4 FLUSH, flags=FLUSH_CAPTURE}
 *       ~ The CAPTURE buffer returned with the solids is all DQ off, waiting for FLUSH_DonE(+1)
 *       → STREAMOFF(CAPTURE)
 *       REQBUFS (CAPTURE, 0) must remain USERPTR
 *       ⁇  Release dma-buf fd/ demap  ⁇  only after REQBUFS(0)
 *       *Ration by New Geometry + QBUF + STREAMON (CAPTURE)
 *       * The core automatically sends message_continue in this STREAMON
 *
 *     The first step of the flush is inevitable. The kernel comment (msm_vidc.c:1240-1252) reads:
 *     "all output Buffers have been flushed and laid"
 *     Precondition. 24 OPs were still in the hands of the solid when it jumped directly from STREAMOFF.
 *     STREAMON (CAPTURE) EINVAL with a chain SYS_ERROR --
 *     0.4.3 The earlier misconstrued as "msm_vidc does not allow CAPTURE to restart", is wrong.
 *
 *     The full process can only be moved by CAPTURE.
 *     Guards, so the STREAMOFF (CAPTURE) during reconfig will not open session; but
 *     STREAMOFF(OUTPUT)/ REQBUFS(OUTPUT, 0) will go to RELEASE_RESOURCES_DONE,
 *     The ETBs in line are all lost.
 *
 *     Actual (Firefox, H.264/HEVC, including a switch between seek and 856x480 ⁇ 1920x1080):
 *     Nine times INSUFFICENT was fully reset, SYS_ERROR 0, 2s flush, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
 *     5s SyncSurface 0, session reconstruction 0,2977 frames pairing,
 *     DestroyContext sends/takes back fully balanced (e.g. 1080/1080).
 *
 * Validation state (0.4.1, nabu / Snapdragon 860 / kennel 4.14):
 *     t.h264 1080p 300 Frames MD5 is consistent with ffmpeg solution
 *     Real1080p 1080p Surge MD5 is softened with ffmpeg
 *     s_3840x2160 4K can be framed but not complete (166.18), content not in line, to be investigated
  */
#ifndef DMD_V4L2_BACKEND_H
#define DMD_V4L2_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* CordecID corresponding to decode-deaem.c (wire id, supplement, not re-allocated).
 *
 * ⁇  0.4.2 Corrigendum: VP8 is reactivated, number 3 is its original number (not the new number).
 * The two previous negative reasons were not established:
 *   - " Hardware does not have VP80 format" - Wrong, ENM_FMT field list
 *     MPG2/H264/HEVC/VP80/VP90, in column VP80;
 *   - "Driving side default stream reconstruction" -- also non-convinced, vp8_build_frame()
 *     Uncompressed data § 9.1 of RFC 6386 is achieved.
 * All that is missing is the statement, the map, and the VP80 item of codec_to_sourcecc. */
enum {
    DMD_V4L2_CODEC_H264 = 0,
    DMD_V4L2_CODEC_HEVC = 1,
    DMD_V4L2_CODEC_VP9  = 2,
    DMD_V4L2_CODEC_VP8  = 3,
    DMD_V4L2_CODEC_AV1  = 4,
    DMD_V4L2_CODEC_MPEG2 = 5,
};

#define DMD_V4L2_MAX_OUT 8    /* Enter (bitstream) buffers */
#define DMD_V4L2_MAX_CAP 24   /* Output (pixel) buffers, if > = minimum value required for driver */

struct dmd_v4l2_buf {
    int      dbuf_fd;         /* dma-heap assigned dmabuf, 1 = undistributed */
    void    *map;             /* mmap to userspace address, NULL means unmap */
    size_t   length;
    int      queued;          /* 1 = Already in drive queue */
    int      hardware_error;  /* Last nonempty CAPTURE had V4L2_BUF_FLAG_ERROR. */
};

struct dmd_video_stats;
struct dmd_v4l2_dec {
    /* Borrowed from the owning session, which retains it across deferred IO. */
    struct dmd_video_stats *video_stats;
    int      fd;                              /* /dev/videoN */
    int      heap_fd;                         /* fd of dma_heap or /dev/ion */
    int      heap_kind;                       /* See HEAP_* of v42_backend.c */
    unsigned ion_mask;                        /* Ion system head, mask. */

    int      out_w, out_h;                    /* Align sizes from the OPTPUT side */
    /* Set before CAPTURE negotiation when the bitstream exceeds 8 bits.
     * Request P010 to keep the full sample values. Forcing NV12 can produce
     * correctly structured output with truncated sample values.
     * The a_10b probe returned v10 & 0xFC for each sample in that case. */
    int      ten_bit;
    int      w, h;                            /* Align sizes for consultation */
    int      crop_w, crop_h;                  /* Show range effectively */
    unsigned in_size;                         /* OUTPUT single buffer bytes */
    unsigned cap_size;                        /* CASTOURE Single Buffet Bytes */
    int      stride;                          /* Line spacing for CAPTURE */
    int      slice_height;                    /* plane height of CAPTURE */

    int      n_out, n_cap;
    struct dmd_v4l2_buf out[DMD_V4L2_MAX_OUT];
    struct dmd_v4l2_buf cap[DMD_V4L2_MAX_CAP];

    int      cap_planes;                      /* CAPTURE plane (including extradata) */
    size_t   extra_size;                      /* square bytes of extradata */
    struct dmd_v4l2_buf extra[DMD_V4L2_MAX_CAP];  /* extradata dmabuf */
    int      reconfig_done;                   /* Reservation: Used for one-time CONTINUE lock, no longer in place */
    int      cap_recfg_count;                 /* CAPTURE Reassemble (defiguring) */
    int      cap_ready;                       /* 1 = Resolution consultation completed and STREAMON */
    /* 1 = This setup_capture is triggered by a resolution change in flow, geometrically based on the new value returned by G_FMT,
     * The OUTPUT consultation value should no longer be used to cover (it was the first time that the negotiations were for residual protection, in the opposite direction). */
    int      trust_gfmt;
    int      fmt_dirty;                       /* 1 = Geometry changed after reset, session layer has to be re-mixed */
    int      out_streaming, cap_streaming;
    int      buf_mem;                         /* Queue Memory Mode V4L2_MEMORY_* */
    int      draining;                        /* Sended EOS, waiting for the remaining frame */
    int      saw_eos;
};

/*
 * Opens the decoder and completes the OUTPUT side configuration (phase 1).
 * The only support currently available is MDD_V4L2_CODEC_AV1.
 * w/h is the size declared by the client for the initial S_FMT; real size is after SOURCE_CHANGE
 * G_FMT (CAPTURE) gives different results (e.g. 1080  ⁇  1088 alignment).
 * returns 0 successful, 1 failed (cleaned at failure, no further close-up).
 */
int dmd_v4l2_open(struct dmd_v4l2_dec *d, int codec_id, int w, int h);

/*
 * Sends a access module. Data/len is the full temporal unit.
 * pts_us as-sale pass-driven timestamp, back-to-back when the frame is out, to match the submission serial number.
 * returns 0 to be successfully joined,1 indicating that there is currently no free input buffer (the caller should take the frame before retrying).
 * -1 means there's been an error.
 */
int dmd_v4l2_send(struct dmd_v4l2_dec *d, const uint8_t *data, size_t len,
                  uint64_t pts_us);

/*
 * Receive one frame, waiting at most timeout_ms.
 * On success, out_data points into the backend DMA-BUF mapping. Use it
 * before dmd_v4l2_release. out_len is the byte count and out_pts the timestamp.
 * Returns 1 for a frame, 0 for a timeout, -1 on failure, and 2 for EOS.
 *
 * The first receive waits for SOURCE_CHANGE and sets up CAPTURE. Start
 * receiving after submitting the first unit so negotiation can proceed.
 */
int dmd_v4l2_recv(struct dmd_v4l2_dec *d, uint8_t **out_data, size_t *out_len,
                  uint64_t *out_pts, int *out_index, int timeout_ms);

/* Returns the frame buffers obtained by recv to the driver. Index uses the value of the recv output. */
int dmd_v4l2_release(struct dmd_v4l2_dec *d, int index);

/* Send EOS to drive the remaining frame in the stream line. Continue recv until return 2.
 * This is **reversible**: no more material should be delivered. For the end of the session. */
int dmd_v4l2_drain(struct dmd_v4l2_dec *d);


/* Closes and releases all resources. Recalls are available. */
void dmd_v4l2_close(struct dmd_v4l2_dec *d);

/*
 * Detection: Whether the V4L2 decoding node on this machine supports the codec.
 * Do not open the session, just do ENUM_FMT, at a very low cost. Use the daemon to decide backend at startup.
 * returns 1 support, 0 not supported.
 */
int dmd_v4l2_probe(int codec_id);

/* Codec_id — Devices:
 * The two candidates are selected by the number of names, the rest of the codec is given by standard values. returns 0 = the codec
 * Not supported by this device solid. */
uint32_t dmd_v4l2_pick_fourcc(int codec_id);

#endif /* DMD_V4L2_BACKEND_H */
