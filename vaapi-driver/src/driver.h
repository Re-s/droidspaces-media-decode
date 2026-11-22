/* DroidSpaces MediaCodec VA-API triver -- Internal Data Structure and Public Statements
 *
 * This driver is loaded into the consumer process (vainfo/ ffmpeg/ Firefox) by libva in dlopen, and is used to use the & & & & & & & & & & & & & & & & & & & & & & & ; ;
 * Therefore, the plugins are followed: no exit/about, no stdout, log default silence, all entrance threads are secure.
 */

#ifndef DMD_DRIVER_H
#define DMD_DRIVER_H

#include <pthread.h>
#include <stdint.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_vp8.h>
#include <va/va_dec_vp9.h>
/* The rule drm_state is defined here, vá_backend.h only makes forward declarations. */
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <va/va_backend_vpp.h>

/* Keep vaMapBuffer's internal access flags available with older libva headers. */
#ifndef VA_MAPBUFFER_FLAG_DEFAULT
#define VA_MAPBUFFER_FLAG_DEFAULT 0
#define VA_MAPBUFFER_FLAG_READ    1
#define VA_MAPBUFFER_FLAG_WRITE   2
#endif

#include "dmd_client.h"
#include "video_stats.h"
#include "stubs.h"

#include "av1_bitstream.h"

struct dmd_encode;

/* Driver version, seen by consumers with vendor.
 * Note that ffmpeg matches the list of vaapi_driver_quirks by applying the vendor string; we are not on the list, but we are not on the list.
 * Stand down before we get a semantic standard. */
/* ⁇ Sync distribution must be synchronized: this string is displayed in vainfo by DSD_VENDOR_STRING
 * In "Driver Version" is the user-observable message. It's been parked on 0.3.3 for a long time and the project has arrived.
 * 0.3.7 (Regular 4), the user points to the wrong version when reporting bugs on vainfo.
 * Synchronization List: This document, CHANGELOG.md.
 * (ksu-module/module. prop previously in the list, 0.4.0 of KSU modules were deleted as a whole. */
/* 0.4.0: Restructure to drive V4L2 direct (no longer unix block side with Android)
 * Decode-daemon / MediaCodec. vendor 's "MediaCodec" is removed from the string...
 * It now describes a path that no longer exists. */
/* 0.4.1: Nabu (Snapdragon 860 / kennel 4.14) decode path connected.
 * Four private agreement requirements (SECONDARY diversion mode / Private event subscription / SESSION_CONTINUE/
 * O_ONBLONK), buffer transfer to USERPTR + plain.reserved [0].
 * Five-yard flow MD5 corresponds to the ffmpeg solution by bytes. See CHANGELOG for details. */
/* 0.4.2: New VP8 hardwire (90/90 frame md5 consistent with soft resolution by bytes); fix consumer not to go by frame
 * CapTure back-lock when the frame is removed (`-f null ' scene from 40s to 3s normal exit).
 * 10bit and MPEG-2 were measured as being limited by solids, and were not declared after compilation switches.
 * See for more information at CHANGELOG. */
/* 0.4.3: Fix the browser green screen. Root cause is the CAPTURE residual geometry - G_FMT(CAPTURE) returns total
 * The default for msm_vidc is 1920x1088, which is not filled in after the width of the coverage bytesperline/size drive, and the default is not filled out.
 * The original protection only checked the lower limit, so 720p followed the 1080p stride=1920:a misposition of each line
 * 640 bytes, and slick_height is reversed to 492 (real 736), each frame cut.
 * Another frame is pure green (UV = 0 for NV12 is the largest color deviation, RGB is exactly pure green and should be filled in 128).
 * Newly increased resolution and resolution switchback -- all old returns are 1080 p, which is exactly the default geometry.
 * The branch was never triggered, and the defect was thus long undetected. */
/* 0.4.7 (pre-published rc1: AV1 hardwired is available. AV1 sourcecc of this kernel msm_vidc is not standard
 * AV10; hardware only for frame pixels showing_frame=1, while VA-API does not transfer refresh_frame_flags,
 * So double-twice "pixel + DPB fix" to rewrite slots and frame context; Shadow DPB is driven by this
 * Frames record the consumer 's surface reuse. Real flow (B station 1080p601800 frames) and soft resolution
 * Bytes, bytes, toss 120~190fps.
 * Previously falsely declared profile. See for more details at CHANGELOG. */
#define DMD_DRIVER_VERSION "0.5.0-dev"
/* Makefile injects every time a building is built: git short hash, with -dirty when no changes are submitted in the workspace.
 * Example: 0.4.3 + 60534cf2 / 0.4.3 + 60534cf2-dirty. This way vainfo can report clearly.
 * Firefox actually dlopen is the version.so, the search browser problem is no longer based on file time guess. */
#ifndef DMD_BUILD_ID
#define DMD_BUILD_ID "manual"
#endif
#define DMD_VENDOR_STRING "DroidSpaces V4L2 VA-API driver " \
                          DMD_DRIVER_VERSION "+" DMD_BUILD_ID

/* Device hardware decoding capability from /vendor/etc/media_codes.xml (real machine taking evidence).
 * H.264/HEVC Codec specification: 96x96 ~ 8192x4320, with example 16, supports adaptive-playback. */
#define DMD_MIN_WIDTH 96
#define DMD_MIN_HEIGHT 96
#define DMD_MAX_WIDTH 8192
#define DMD_MAX_HEIGHT 4320

/* Libva requests that all fields be > 0 (CHECK_MAXIMUM for va/ va.c).
 * And these values are the basis for consumer distribution of query arrays, which must > = the amount we actually return. */
#define DMD_MAX_PROFILES 16
#define DMD_MAX_ENTRYPOINTS 4
#define DMD_MAX_CONFIG_ATTRIBUTES 32
#define DMD_MAX_IMAGE_FORMATS 4
#define DMD_MAX_SUBPIC_FORMATS 4
#define DMD_MAX_DISPLAY_ATTRIBUTES 4

/* The ceiling for the config object that also survives. VA-API's config is very light.
 * Consumers usually only build one or two; the full amount is enough. */
#define DMD_MAX_CONFIGS 32

/* Per-context queue depth is separate from the display-wide object table.
 * Multiple decoder pools share a VADisplay, not one context's slot budget. */
#define DMD_MAX_SURFACES 64
#define DMD_MAX_SURFACE_OBJECTS (DMD_MAX_SURFACES * DMD_MAX_CONTEXTS)
/* Retain the unit identity for consumption without writing a reused target. */
#define DMD_DISCARD_SURFACE ((VASurfaceID)0xFFFFFFFEu)

/* The output depth of MediaCodec is lagging (real values, see probe_lag in vaapi-driver/tools):
 * The H.264 stream with B frames is sent into the 4th input unit to throw out the 1st frame, without B frames.
 *
 * SyncSurface uses it as the "freeze" level: it does not block consumers when the number of frames is below this value.
 * It continues to deliver materials to fill the stream lines; it reaches the frame that really blocks them.
 * Take 6 (delay 4 and leave 2 left behind) -- too small to return the lock, too small to allow the queue to grow,
 * This adds latency and wastes surfaces.
 *
 * ⁇  Cross directory coupling (historical, socket-structured period): SHM_SLOTS on the side of daemon must
 * > = Current value. Historical accident: SHM_SLOTS=4 < value 6, frame 5 must hit pool full, daemon in
 * 1 second to die and kill session - 4K + Slow Consumer scene 10/10 Drop frame (238,300).
 * V4L2 The coupling disappeared under the straight frame (no SHM pool), but the restraints were replaced.
 * The number of CAPTURE buffers for V4L2 (DMD_V4L2_MAX_CAP, actual driver to 24).
 *
 * Basis for taking value 12 (V4L2 direct-live): msm_vidc output lags further than MediaCodec.
 * Original value 6 is based on the actual time lag of MediaCodec (B frame 4) for V4L2
 * The ffmpeg sent 6 units, i.e. stopped the frame, and the decoder threw up only 2 frames.
 * SyncSurface et al. 2000ms timed out  ⁇  triggering emptiness  ⁇  4 frames to match  ⁇
 * The ffmpeg newspaper internal decoding error.
 * And the direct backend takes 10 units to stabilize the 10 frames, so take 12 (10 left 2 more),
 * And 12 < 24 will not hit CAPTURE buffer cap.
 *
 * ⁇  0.4.1 Amendment: 12 is the value of the MediaCodec daemon era, and the real lag in V4L2 direct traffic is 7.
 *
 * Research/slowfeed.c Directly hit V4L2 (bypassing VA-API and this drive)
 *     [Deliver 1.7] 0  ⁇  Full 7 units undone
 *     [Send 8] 1  ⁇  collected 8th to throw up the top frame
 *     [Send 9..] Received 2,3,4... after 1:1 steady follow
 * The fast (consequent, simulate ffmpeg) is consistent with the slow (simulating Firefox, etc.) rhythm:
 * The lag is seven and the OUTPUT buffer never falls to zero -- slow delivery is not in itself a problem.
 *
 * Retrieve OTPUT_ORDER=1 (decoding sequence, lag to 4) after taking 5:
 * The decoding sequence is used to stabilize the feed at 6 > 4 and the decoder is not unexpected and can be safely blocked.
 * The threshold is set at 7 and the threshold is not enough, with 1,057 / 15 blocking and loss of frame 15.5%.
 *
 * The reason for the historical value 7 (sequence times shown): the pending < 7-hour decoder is really unanticipated, and Sync must release it to the consumer for further delivery.
 * Otherwise, the two sides will wait. The test is to change it to four, and it's this way: Firefox, every frame block, 5s, time out.
 * Triggers flash, re-recreated session(six times "re-established session" in the log), fps drops to 0.4.
 * Coming > = 7 hours is full, the next frame is due, so the frame is blocked and so on - for dmabuf
 * Consumers of zero copies sampled (DeriveImage, see export.c)
 * The only way is right, they don't have the "map time" to wait for that trail.
 * The consequences of leaving 12: putting steady at 5 is never enough for 12, so every frame is released and falsely declared.
 * Firefox takes an unfilled buffer - 847 decoding / 792 discard = 93.5%, the image remains intact. */
#define DMD_PIPELINE_DEPTH 5

/* The line is full with a frame for empty spaces.
 *
 * Shorter: This path is in EndPicture, and it will be submitted too late.
 * Longer: too short to be empty and to return to the failed position.
 * 200ms is enough for MediaCodec to throw up a frame under normal load (actual 1080p p 95 about 10ms). */
#define DMD_BACKPRESSURE_MS 200

/* After so long (ms) the frame was not taken, the upstream feed was determined to be non-reversible.
 *
 * It must be fixed and cannot be extra-duration to the caller: the caller's patience is not evidence of "end of current".
 * Firefox uses a very short time-over query on vSyncSurface, which is proportionally calculated to get very small thresholds, and is used to create a very small number of people who are not allowed to spend time on the Internet.
 * The frame 3 is the end of the misdirection  ⁇  Flush  ⁇  session is dead  ⁇  permanent downs and softs. */
#define DMD_FLUSH_AFTER_MS 2000
/* Number of simultaneous decoded examples declared by context cap alignment media_codes.xml (16). */
#define DMD_MAX_CONTEXTS 16
/* Buffer cap: about 4 per frame (pic param / IQ matrix / slick param / slice data).
 * ffmpeg is only after EndPicture Destroy, and survival is associated with the depth of the DPB. */
#define DMD_MAX_BUFFERS 256
/* Image cap: ffmpeg builds one, unmap, destruction at a time with very little survival. */
#define DMD_MAX_IMAGES 64

/* Bump geometric alignment: Venus decoder output wide alignment 128, high alignment 32
 * (Photo, the machine takes evidence)
 * The frame that ensures that the daemon returns must be filled and not be distributed until the form block is received. */
#define DMD_WIDTH_ALIGN 128
#define DMD_HEIGHT_ALIGN 32

/* When a frame returns from the daemon, the ceiling. Driver runs in the host process and is never allowed to wait indefinitely:
 * It is better to take the time to get the wrong back and let the consumer decide to try again or give up than to hang the whole process.
 *
 * ⁇  Cross-catalogue coupling: SHM_SLOT_WAIT_MS (src/decode-daemon.c) on the side of daemon
 * This value is the maximum available slot for free slots like daemon, which the client wants to wait for.
 * If it is shorter, the bottom level will give up and suspend the conversation when the caller is willing to wait.
 * Historical accident: daemon waits only for 1000 ms < value 5000 ms, decodes that could have been done properly
 * To drop frame. SHM_SLOT_WAIT_MS must be checked simultaneously when resetting the large value. */
#define DMD_FRAME_TIMEOUT_MS 5000

/* A surface: VA-API decode target, holding a NV12 buffer.
 *
 * Data stream: EndPicture send slick data to daemon and hang the surface into conft
 * To decode; SyncSurface takes frames from daemon and fills the first surface buffer in the FIFO order.
 */
struct dmd_import;
struct dmd_surface {
    int in_use;
    VASurfaceID id;
    /* The requested size (sizes shown, ffmpeg fax 1920x1080). VAImage refers to this. */
    unsigned int width;
    unsigned int height;
    /* Decoded display rectangle may be smaller than the application's pool
     * dimensions (e.g. an FFmpeg 1088-row pool carrying 1080 visible rows). */
    unsigned int visible_width, visible_height, visible_x, visible_y;
    /* Batch geometry: Wide alignment 128, values after high alignment 32, data distributed as this. */
    unsigned int buf_width;
    unsigned int buf_height;
    unsigned int stride;
    unsigned int slice_height;
    unsigned int format; /* VA_RT_FORMAT_* */
    unsigned char *data;
    size_t data_size;
    /* Read-only, externally owned NV12 input. The import owns duplicated
     * descriptors/mappings; an in-flight snapshot retains it independently. */
    struct dmd_import *imported;
    /* Dombbuffer reserve storage: exportable not 0 means that data is exported from mmap
     * Memory, exported dmabuf fd via DRM_IOCTL_PRIME_HANDLE_TO_FD.
     * Firefox takes the frame away from the vExportSurfaceHandle, without which the whole stream is softened;
     * Falls normal header (exportable=0) when allocating fails, and hwdownload of ffmpeg is still available. */
    uint32_t dumb_handle;
    size_t dumb_size;
    int exportable;
    /* Release the dombbuffer requires the original drm fd assigned to it. There is a surface instead of a let-off.
     * Release function takes one argument -- this makes it impossible to pass on the wrong fd. */
    int dumb_drm_fd;
    /* dmabuf fd, used only for DMA_BUF_IOCTL_SYNC cache maintenance.
     *
     * Why stay: data may still be available after CPU memcpy writes dumb buffer 's mmap
     * In D-cache; GPU reads old content (in fact, it is a pure green screen) through dmabuf sampling.
     * The map of dumb buffer is VM_PFNMap, msync returns directly to EINVAL, so it must be used
     * DMA_BUF_IOCTL_SYNC -- and it needs a dmabuf fd.
     * Separated from the fd that vExportSurfaceHandle handed to the caller: that life cycle
     * Recaller (which is responsible for close) cannot be used for internal synchronisation. 1 = not available. */
    int dumb_sync_fd;
    /* He was exported by the vaExportSurfaceHandle.
     *
     * Use: Distinguishing consumer contracts to determine whether EndPicture must immediately write pixels into the surface.
     * Chrome will export all dmabuf before decoding, and then only EndPicture, and never Sync/GetImage, but the only way to get a new one is to get a new one.
     * So it must be written before EndPicture returns;
     * ffmpeg/Firefox go DeriveImage/GetImage, where dmd_surface_wait
     * No need to sync memcpy in EndPicture -- and that memcpy would cut the spitting by half.
     * (Destination of jinjie 300 frames: 76.8 § 34.8 fps). */
    int exported;
    /* A successful WRITE_ONLY/READ_WRITE PRIME export permits the application
     * to supply encoder input directly through this local NV12 allocation.
     * This grants no decode readiness: the producer must complete GPU writes
     * before BeginPicture, and ordinary pixel-read APIs retain IDLE checks. */
    int external_write_exported;
    /* Decoding status: 0 = freetime (VASURfaceReady), 1 = submitted pending decoding (VASURACERendering),
     * 2 = frame ready (VASURfaceReady and data valid). */
    int state;
    /* The last decoding result of the surface returns it. */
    VAStatus decode_status;
    VAContextID context; /* Context at the time of submission, not submitted as VA_INVALID_ID */
};

#define DMD_SURFACE_IDLE 0
#define DMD_SURFACE_PENDING 1
#define DMD_SURFACE_READY 2

/* A buffer: VA-API parameter/code flow buffer. */
struct dmd_buffer {
    int in_use;
    VABufferID id;
    VAContextID context;
    VABufferType type;
    unsigned int element_size;
    unsigned int num_elements;
    size_t size; /* element_size * num_elements */
    void *data;
    int mapped;
    /* EncCodedBuffer maps to this segment, not directly to the byte storage. */
    VACodedBufferSegment coded_segment;
    VAStatus coded_status;
    int coded_pending;
};

/* An image: VAImage reserve storage. Data is a normal heap memory
 * (ION/dma_head in containers is not available and the dmabuf zero copy has been dropped). */
struct dmd_image {
    int in_use;
    VAImageID id;
    VABufferID buf_id; /* Fake id for vamapBuffer */
    VAImage image;
    unsigned char *data;
    size_t data_size;
    int mapped;
    /* Devereve immersion tied to a surface; CreateImage is VA_INVALID_ID */
    VASurfaceID derived_from;
    int acquired_fd;
    int handle_acquired;
    unsigned int map_flags;
    int map_sync_started;
};

/* A context:config + reder trage + a V4L2 decoding session.
 *
 * The to decode queue pending[] is the bridge between the VA-API semantic and the MediaCodec fluid syntax.
 *
 * ⁇  The pairing rule is divided into two categories by codec, as the order of the two sides may not be the same:
 *
 * - VP9/VP8 (no frame rehearsing): ffmpeg's submission order = frame order = daemon,
 *   Direct FIFO pairing (take the first team). Strict 1 in and 1 out.
 *
 * - H.264/HEVC (with B reorder): ffmpeg by ** decoding ** EndPicture,
 *   And MediaCodec shows the **-- the **-- the two are different!
 *   PB B... (P at 2 bit, because B is to refer to it),
 *   Display order I B B B P... The second submission (P-surface) if FIFO pairs
 *   is filled in in the second output (B content), which is wrong from frame 2.
 *   So you must pair with POC (sequence of display): the first k frame of daemon to POC the second k surface.
 *   The POC is taken from VAPictureParameter Buffer H264.CurrPic. TopFieldOrderCnt.
 *   ffmpeg is a disassembled field_poc [0] (vaapi_h264.c:74), which is a very important part of the story.
 *   Not 6 bits in the stream will return to poc_lsb, so you can directly compare size.
 */
struct dmd_context {
    int in_use;
    VAContextID id;
    VAConfigID config_id;
    VAProfile profile;
    VAEntrypoint entrypoint;
    VASurfaceID vpp_source;
    VARectangle vpp_source_region;
    VARectangle vpp_output_region;
    uint32_t vpp_rotation;
    int vpp_has_source_region;
    int vpp_has_output_region;
    int vpp_have_params;
    struct dmd_encode *encoder;
    struct dmd_video_stats *video_stats;
    int codec; /* DMD_CODEC_* */
    unsigned int picture_width;
    unsigned int picture_height;
    int flag;

    /* V4L2 decoding session. Inert creation: Create Context when built, leaving NULL by
     * EndPicture Try Again - When creating context, daemon cannot be used to let the whole initialization fail. */
    struct dmd_session *session;
    int session_failed; /* The building session failed, avoiding the delay of the failure path in each frame */
    /* Ben context is being removed (destruction is already or is about to be hung).
     * New IO for this slot -- without this sign, return io_busy to the window between "dismant."
     * The frame-trace will re-take the session that was just about to be destroyed. */
    int retiring;

    /* Pending decoding Queue: Surface from EndPicture.
     * VP9/VP8 Take the head of the team; H.264/HEVC Take the smallest ending_poc (see structure head comment). */
    VASurfaceID pending[DMD_MAX_SURFACES];
    int32_t pending_poc[DMD_MAX_SURFACES];
    /* Corded video sequence number: every IDR will be reset over the POC
     * Size doesn't make sense, so sort it out first over seq and then over POC. */
    unsigned int pending_seq[DMD_MAX_SURFACES];
    /* This surface corresponds to the submission serial number** (1 for the first data module sent to the daemon).
     * The Daemon returns it as a PTS back to the frame (dmd_frame.unit_seq), which is a very important tool for the development of the new system.
     * The pairing then matches precisely without the need to assume the frame order of the decoder. */
    uint64_t pending_unit[DMD_MAX_SURFACES];
    int pending_head;
    int pending_count;
    /* Test for silent drop of frame by hardware (AV1 only).
     *
     * The frame follows the order, so the units that were submitted later than the first team have already been framed and the first team has been set up.
     * "Delaying" is evidence of the hardware that swallowed the head of the team -- a frame plus one for each one that passes through the head.
     * The header is normally set to go with zero. Actual trigger point: 1080p60 1665 (order_hint return)
     * ) unit 3333/3324 never returned, the caller is on vSyncSurface
     * The entire bar stream stops after five full seconds of dry-scrambling -- a frameless frame should not destroy the whole segment. */
    int av1_head_stall;
    /* The number of the unit is determined to be disabled. If the hardware is not used to spit out the frame, the frame path is dropped;
     * If you don't remember, you're going to be in the queue "no matches" and then you're going back to the sequence -- that's going to pair the next one.
     * Incorrect pairing affected 98.6% of Chrome frames in 0.4.5. See dmd_pending_take_locked. */
    uint64_t av1_dead_unit[8];
    int av1_dead_head;
    int av1_dead_count;
    /* Number of data units submitted, signaled to sending_unit. */
    uint64_t units_submitted;
    /* The text has been delivered to consumers in number and is purely diagnostic.
     *
     * The first time I was using it as a "start preheating window": the former N frame Sync was only detecting the presence of the team and the presence of the virus.
     * To eliminate the loss of the start-up phase. A/B alternated to test whether the change was confirmed.
     * (research/perf/ab.sh, 4-5 rounds each):
     *     No preheat 3.50% median (range 1.82-39.84%)
     *     240 frames medium 5.97% (range 4.25-45.20%)
     * The numbers are kept and the delivery of the session is easy. */
    uint64_t frames_out;
    /* Whether daemon returns unit_seq (observation from frame at running time, non-compilation period assumptions).
     * With this, you can match the exact pair and indicate that the daemon has followed the input sequence to output (delay 1). */
    int daemon_has_unit_seq;
    /* The POC, RenderPicture from the pic param, EndPicture with the surface.
     * H.264/HEVC only makes sense. */
    int32_t current_poc;
    int have_current_poc;
    /* Fame_num to zero in every IDR (regulation 7.4.3), using it to detect new sequences -
     * PoC cannot be compared because POC is in the decoding sequence and is inflexing. */
    unsigned int current_frame_num;
    int32_t last_poc;
    unsigned int last_frame_num;
    int have_last_poc;
    unsigned int last_seq;

    /* Current BeginPictures selection */
    VASurfaceID current_target;
    /* The flow data accumulated in this frame (RenderPicture may be divided into several different times to slick data) */
    unsigned char *slice_data;
    size_t slice_len;
    size_t slice_cap;
    /* VP8 slice parameter for this frame, synthesized uncompressed chunk required */
    int have_vp8_slice_param;
    VASliceParameterBufferVP8 vp8_slice_param;
    int have_vp8_pic_param;
    VAPictureParameterBufferVP8 vp8_pic_param;

    /* MPEG-2: Security header only exists in the pic param and IQ matrix,
     * slick data only slices bytes, which must be inverted (see mpeg2_bitstream.c). */
    int have_mpeg2_pic_param;
    VAPictureParameterBufferMPEG2 mpeg2_pic_param;
    int have_mpeg2_iq_matrix;
    VAIQMatrixBufferMPEG2 mpeg2_iq_matrix;
    /* Temporal_reference increases itself in the submission order (model 1024). This field for MPEG-2 is only used
     * Displays sequenced identification, which is not used for frame management, so self-increment is sufficient. */
    unsigned int mpeg2_temporal_ref;
    /* The first frame and resolution changes only, as param_sets_sent. */
    int mpeg2_seq_sent;

    /* AV1: Synthetic OBU requires two things --
     *   1) Pic param 's Structured Fields: Sequence Header and Frame** only exists here**,
     *      VASLiceDataBuffer only contains a Tile load, without any OBU envelope
     *      (va_dec_av1.h: 643-645 with clear code currents per-tile particle level)
     *   2) Offset and length of each tile in the code current buffer: tile_group must write by tile
     *      The tile_size_minus_1, and slice_data is a cumulative one.
     *
     * ⁇  Boundary source must be VASLiceParameter BufferAV1,** not **
     * Additional times for VASLiceDataBuffer. Actual: ffmpeg adjusted only once per frame
     * vaRenderPicture sent slicice data, but declared 8 tile...
     * That is, the whole frame tile is packed in a buffer, and only one is obtained by adding.
     * The correct source is a slick param per tile (va_dec_av1.h:635 "should be)
     * The slick_data_size/ slice_data_offset”)
     * The byte number of tile is offset by buffer (649-658, note makes clear slick_data_size)
     * "actually means tile_data_size"）。
     *
     * 512 sufficient to cover 8K/ multiple tile scenes (regulation MAX_TILE_COLSx_MAX_TILE_ROWS)
     * 64 x 64 in nominal terms, which is much less bound by MAX_TILE_AREA). */
    int have_av1_pic_param;
    /* Has the session been overtaken by a long time: 0 = not yet reported, 1 = 8bit, 2 = 10bit reported.
     * CAPTURE consultation occurred in SOURCE_CHANGE from the first module, and vCreateContext
     * The stage is not ready for further processing, so only the first time EndPicture sees the PPP can be filled. */
    /* Effective output sample size, also preserved when HEVC sessions reopen. */
    int output_ten_bit;
    int av1_bd_reported;
    VADecPictureParameterBufferAV1 av1_pic_param;
    VASliceParameterBufferAV1 av1_tile_param[512];
    int    av1_tile_count;

    /* The AV1 self-directed DPB: writes_frame_flags and ref_frame_idx are all written by us into the stream.
     * VA-API does not provide the former (that's the GP decision of the encoder, discarded with the source stream).
     * So, it maintains a slot allocation that guarantees that the two writing streams will be self-serving.
     * See the explanation for av1_bitstream.hstruct dmd_av1_dpb for details. */
    struct dmd_av1_dpb av1_dpb;

    /* AV1 Delays the storage of a frame for the delivery.
     *
     * Why delay: the correct value for refresh_frame_flags is equal to "the next frame"
     * The & & & & & & & & & & & & & & & & & & & & & & & & & & & & & & & & & & & & &
     * So this frame can't be sent out immediately after the completion of the synthesis -- until the next frame arrives, the correct value is calculated.
     * Rewrite that eight bits just to be delivered.
     *
     * v1_hold holds the last bytes that have been synthesized but not yet sent (this module Malloc, the first time the message is written in the first place.)
     * Free after sending out. av1_hold_len is its length. */
    unsigned char *av1_hold;
    size_t         av1_hold_len;
    /* Refresh_frame_flags offset.
     * The dpb.last_refresh_bitpos -- that field will be ** the frame**
     * . Checked: log shows "bitpos=19 len=655",
     * And len=655 is the length of the new synthetic frame, the temporary frame 2686, and you know the wrong frame at first glance. */
    size_t         av1_hold_bitpos;
    /* Disable_frame_end_update_cdf offset from the temporary frame (ibid., cannot be reused)
     * The same name segment in dpb will be covered by this frame. */
    size_t         av1_hold_endupd_bitpos;
    /* AV1 Delays a frame delivery, EndPicture sends ** data from previous frame**,
     * So the to pairing queue must register the frame of the framework instead of the current pattern.
     * The consequences of not doing so are measured: echoes unit_seq not found in queues
     * ("unit_seq=6 no match, rebuttal inference,"), frame is set to the wrong surface,
     * ffmpeg cannot wait for the one it wants to get.
     *   av1_hold_surface: currently presses the frame of av1_hold
     *   Av1_send_surface: build_unit The frame that was actually delivered */
    VASurfaceID    av1_hold_surface;
    VASurfaceID    av1_send_surface;
    /* The frame number is in the shadow of the DPB (frame_seq, see av1_bitstream.h).
     * Refresh's shadow registration occurred in the next frame of the past (that's when we knew the source stream was the frame)
     * - What slot is it? - This is the number for the registration. */
    int            av1_hold_frame;
    /* -flush fix queue -
     *
     * When sync flushes the sync pre-emption, refresh
     * Uncounted, press 0 to send (no slot, no DPB) while leaving the bytes in
     * Fix Queue. The next frame diff can already calculate the true source stream
     * Refresh, rewrite the fix buffers to the ground** re-reissued**: Hardware decoded the frame
     * (Expires are still in order, sourface is again written into the same pixels, no harm), DPB therefore
     * Fills the correct slots. Reissued output is discarded by sentry pending (surface=0).
     * No matchmaking. */
    unsigned char *av1_repair_buf;
    size_t         av1_repair_len;
    size_t         av1_repair_bitpos;
    int            av1_repair_frame;
    /* Restored session_frame_end_update_cdf: First trip (flush delivery)
     * Force 1 to inhibit frame reset, real value is stored here, rewriting the real value is completed
     * The only answer is given. See dmd_av1_dpb::last_endupd_bitpos. */
    size_t         av1_repair_endupd_bitpos;
    int            av1_repair_endupd_val;
    /* Double feed delivery slot: EndPicture builder_unit first, it runs the pixels **Figure**
     * Buffer here; wait for the DPB above to deliver av1_repair_buf (upset) and move in as a whole.
     * av1_repair_buf. The frame itself is re-reissued without separation -- measured: frame 2 synthesis 4422 bytes
     * It was sent twice as "repair 4422 byte," followed by a double free. */
    unsigned char *av1_next_repair_buf;
    size_t         av1_next_repair_len;
    size_t         av1_next_repair_bitpos;
    int            av1_next_repair_frame;
    size_t         av1_next_repair_endupd_bitpos;
    int            av1_next_repair_endupd_val;
    /* The last shell that gets real pixels is a surface for show_frame=0 to carry pixels. */
    VASurfaceID    av1_last_ready;

    /* - Show_existing_frame plug-in
     * Hardware does not export show_frame=0 frames (compliant), source flow is 70 SEF heads
     * Let them reappear once and fill 150 frames. Only 80 frames are collected when SEF headouts are missing.
     *
     * Location pattern (150 frame sample, 100% established):
     *   All 70 SEFs are closely behind a show_frame=1
     *   80 show = 1 with 70 SEFs
     * Reference Law: SEF's map_idx point slot, just some show_frame=0
     *   Finally refresh that slot.
     *
     * The following is a video of the video that shows the number of people who are using the system:
     * After the frame is sent out of show_frame=1, a SEF head is taken from the first team. */
    /* The DPB slot to be re-recovered. Capacity extraction 64: 8 is too small --
     * The frame of the pyramid B structure shows_frame=0 will continue to arrive.
     * 150 frames were long-suffering, and 58 times were discarded.
     * (12 times only in the team, and 70 times in show=0 frame). */
    unsigned       av1_sef_slot[64];
    int            av1_sef_head;
    int            av1_sef_count;
    /* Unconditional counter. Do not extrapolate the number of times executed by log lines --
     * The session was misjudged twice (round 54 calls 220 lines for 220 calls, and then the video is available on the page).
     * In round 59, 75 logs are counted as 75 times executed). */
    unsigned long  av1_sef_visits;   /* Number of slots executed */
    unsigned long  av1_sef_show1;    /* Of which send_show is 1 */
    unsigned long  av1_sef_enq;      /* Number of entries */
    unsigned long  av1_sef_drop;     /* Number of times they've been thrown out of line */
    unsigned long  av1_sef_sent;     /* Number of additional SEF units actually added */
    unsigned long  av1_ep_enter;     /* EndPicture Entry */
    unsigned long  av1_ep_unit;      /* Number of times build_unit returns non-empty */
    unsigned long  av1_ep_null;      /* NULL (install) */
    unsigned long  av1_hold_show1;   /* Number of current frames show=1 on suspense */
    unsigned long  av1_sendset1;     /* av1_send_show was assigned 1 times */
    unsigned long  av1_flushed;      /* sync flash sent to suspense */
    unsigned long  av1_flush_show1;  /* Show =1 times from the frame sent by Flush */
    unsigned long  av1_ep_show1;     /* Number of times that EndPicture sent show=1 */
    /* The frame of reference is not the frame of the shadow DPB that was thrown out (to drag to the middle of the GOP, the stream cut).
     * av1_ref_drop is cumulative, av1_ref_drop_now only in **EndPicture
     * Valid: Build_unit uses it to tell EndPicture "This time NULL is missing frame, not unsupported
     * The cipher flow is reconstructed" and the end of the two is different (the former is delivered by shell and returned to success). */
    unsigned long  av1_ref_drop;
    int            av1_ref_drop_now;
    /* The frame with the accompanying log show_frame: show_frame=0 does not produce output (decoder only for actual measurements).
     * Show_frame=1 frame vomiting CAPTURE buffer) cannot be registered for the pairing,
     * Otherwise, there are more entries that never fit. */
    int            av1_hold_show;
    int            av1_send_show;

    /* H.264: VA-API never transmits the set of parameters (the original bit stream is lost after the SPS/PPS has been deconstructed into fields).
     * So you want to synthesize backwards from the pic param and send it to the OUTPUT queue before the first VCL. */
    int have_h264_pic_param;
    VAPictureParameterBufferH264 h264_pic_param;
    /* HEVC image parameter: Synthetic VPS/SPS/PPS. Use with h264. */
    /* PPS's num_ref_idx default deduction: zero takes the maximum, l1 takes the smallest seen.
     * See the description of decode.c - A direct copy of each frame entry value will write the wrong PPS default value. */
    unsigned int refidx_l0_max;
    unsigned int refidx_l1_min;
    int refidx_seen;

    VAPictureParameterBufferHEVC hevc_pic_param;
    int hevc_pic_param_valid;
    /* Long-format slice boundaries. Parameters describe the next data buffer;
     * offsets below become absolute in slice_data when that buffer arrives. */
    VASliceParameterBufferHEVC hevc_slice_param[128];
    size_t hevc_slice_offset[128];
    unsigned hevc_slice_count, hevc_slices_bound;
    int hevc_slice_error;
    int have_h264_slice_param;
    VASliceParameterBufferH264 h264_slice_param;
    int have_h264_iq_matrix;
    VAIQMatrixBufferH264 h264_iq_matrix;
    int param_sets_sent;
    unsigned int sent_mbs_wide;
    unsigned int sent_mbs_high;
    /* Last time a num_ref_idx default was sent with a PPS. It must follow the current frame entry value,
     * Reissued PPS - l1 overselect ref_idx_l1 entropy decoding long. */
    unsigned int sent_l0;
    unsigned int sent_l1;
    unsigned int pending_l0;
    unsigned int pending_l1;
    /* SHUT_WR: no longer send, no need to repeat Flush */
    int input_finished;
};

/* A VA-API config object: profile + entry + property collection */
struct dmd_config {
    int in_use;
    VAConfigID id;
    VAProfile profile;
    VAEntrypoint entrypoint;
    VAConfigAttrib attribs[DMD_MAX_CONFIG_ATTRIBUTES];
    int num_attribs;
};

/* Driver private data, hang on ctx->pDriverData
 *
 * Lock policy: Keep all object sheets on lock. This is not a performance bottle -- the table is an O (table length) operation
 * ** But blocking IOs must not be locked **: with the receipt and distribution of daemon after the locking, the IO must be locked.
 * The collage of the same context on the IO by the context bary sign.
 */
struct dmd_driver {
    pthread_mutex_t lock;
    /* IO broadcast when finished, waiting for the confext busy tag */
    pthread_cond_t io_done;

    struct dmd_config configs[DMD_MAX_CONFIGS];
    struct dmd_surface surfaces[DMD_MAX_SURFACE_OBJECTS];
    struct dmd_context contexts[DMD_MAX_CONTEXTS];
    struct dmd_buffer buffers[DMD_MAX_BUFFERS];
    struct dmd_image images[DMD_MAX_IMAGES];

    unsigned int next_config_id;
    unsigned int next_surface_id;
    unsigned int next_context_id;
    unsigned int next_buffer_id;
    unsigned int next_image_id;

    /* Some context is doing daemon IO (the peer-over sign during locking).
     * Index the array directly instead of looking up the ID.
     * The semantic is "the lined quote from the session outside the lock (including the frame buffer it has not yet release), which is the most important part of the story.
     * ** So it has to be done only after the last use**; clearing the session is just the fun of taking out the fd that is being used. */
    int io_busy[DMD_MAX_CONTEXTS];

    /* I've got a line for the slot session when I return io_busy.
     * By the last person to be used** that is actually destroyed (see io_release of decode.c).
     * Not directly: the frame line might have been holding the same session at the octl, close to dropping its fd
     * Then the fd is recycled by the host process and the move is chrome's own frd. */
    struct dmd_session *io_defer[DMD_MAX_CONTEXTS];
    struct dmd_encode *encode_defer[DMD_MAX_CONTEXTS];

    /* Backstage frame closing thread: Chrome only epticure, never vSyncSurface.
     *
     * Slow playback actual 2fps number stream x 0.25 times the speed of  ⁇  2 seconds per frame: hardware is already solved, and the hardware is now available.
     * But there's no VA call between EndPicture and the frame is always on V4L2.
     * CAPTURE queue -- driver does not have any executive opportunity to write pixels into the surface,
     * Chrome sampled either an empty buffer or a previous round of content, with a forward and forward frame leap.
     *
     * So there's a frame path that doesn't depend on the call-side rhythm. Linear inertia starts.
     * "CreateContext" (when you're on)
     * Shares io_busy stringing with EndPicture/ SyncSurface without changing the matchmaking logic. */
    pthread_t reaper;
    int reaper_started;
    int reaper_stop;

    int drm_fd; /* From ctx->drm_state, record only, not currently octl */
    uint32_t encode_caps; /* DMD_ENC_CAP(profile), probed once per VA display. */
};

/* Starts the backstage collection (inertity, repeated calls without side effects). */
void dmd_reaper_start_locked(struct dmd_driver *drv);
/* Stops and recovers the backstage collection. Callers** must not ** hold the lock. */
void dmd_reaper_stop(struct dmd_driver *drv);

/* Take private data from VADRIVCONT; ctx or pDriverData returns NULL when empty. */
struct dmd_driver *dmd_get_driver(VADriverContextP ctx);

/* Log: Default silence, with DMD_VA_LOG=1 output to stderr.
 * Never write stdout -- that's gonna pollute the output of the host process. */
void dmd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* - a realized vtable entry (the rest in auto-generated stubs.c) */

VAStatus dmd_Terminate(VADriverContextP ctx);

VAStatus dmd_QueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list,
                                 int *num_profiles);

VAStatus dmd_QueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile,
                                    VAEntrypoint *entrypoint_list,
                                    int *num_entrypoints);

VAStatus dmd_GetConfigAttributes(VADriverContextP ctx, VAProfile profile,
                                 VAEntrypoint entrypoint,
                                 VAConfigAttrib *attrib_list, int num_attribs);

VAStatus dmd_CreateConfig(VADriverContextP ctx, VAProfile profile,
                          VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
                          int num_attribs, VAConfigID *config_id);

VAStatus dmd_DestroyConfig(VADriverContextP ctx, VAConfigID config_id);

VAStatus dmd_QueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id,
                                   VAProfile *profile, VAEntrypoint *entrypoint,
                                   VAConfigAttrib *attrib_list,
                                   int *num_attribs);

VAStatus dmd_QuerySurfaceAttributes(VADriverContextP ctx, VAConfigID config,
                                    VASurfaceAttrib *attrib_list,
                                    unsigned int *num_attribs);

VAStatus dmd_QueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list,
                               int *num_formats);

VAStatus dmd_QuerySubpictureFormats(VADriverContextP ctx,
                                    VAImageFormat *format_list,
                                    unsigned int *flags,
                                    unsigned int *num_formats);

VAStatus dmd_QueryDisplayAttributes(VADriverContextP ctx,
                                    VADisplayAttribute *attr_list,
                                    int *num_attributes);

VAStatus dmd_GetDisplayAttributes(VADriverContextP ctx,
                                  VADisplayAttribute *attr_list,
                                  int num_attributes);

VAStatus dmd_SetDisplayAttributes(VADriverContextP ctx,
                                  VADisplayAttribute *attr_list,
                                  int num_attributes);

/* - decode.c: decoding data path - */

VAStatus dmd_CreateSurfaces(VADriverContextP ctx, int width, int height,
                            int format, int num_surfaces,
                            VASurfaceID *surfaces);

VAStatus dmd_CreateSurfaces2(VADriverContextP ctx, unsigned int format,
                             unsigned int width, unsigned int height,
                             VASurfaceID *surfaces, unsigned int num_surfaces,
                             VASurfaceAttrib *attrib_list,
                             unsigned int num_attribs);

VAStatus dmd_DestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list,
                             int num_surfaces);

VAStatus dmd_CreateContext(VADriverContextP ctx, VAConfigID config_id,
                           int picture_width, int picture_height, int flag,
                           VASurfaceID *render_targets, int num_render_targets,
                           VAContextID *context);

VAStatus dmd_DestroyContext(VADriverContextP ctx, VAContextID context);

VAStatus dmd_CreateBuffer(VADriverContextP ctx, VAContextID context,
                          VABufferType type, unsigned int size,
                          unsigned int num_elements, void *data,
                          VABufferID *buf_id);

VAStatus dmd_BufferSetNumElements(VADriverContextP ctx, VABufferID buf_id,
                                  unsigned int num_elements);

VAStatus dmd_MapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf);

VAStatus dmd_MapBuffer2(VADriverContextP ctx, VABufferID buf_id, void **pbuf,
                        uint32_t flags);

VAStatus dmd_UnmapBuffer(VADriverContextP ctx, VABufferID buf_id);

VAStatus dmd_DestroyBuffer(VADriverContextP ctx, VABufferID buffer_id);

VAStatus dmd_BufferInfo(VADriverContextP ctx, VABufferID buf_id,
                        VABufferType *type, unsigned int *size,
                        unsigned int *num_elements);

VAStatus dmd_BeginPicture(VADriverContextP ctx, VAContextID context,
                          VASurfaceID render_target);

VAStatus dmd_RenderPicture(VADriverContextP ctx, VAContextID context,
                           VABufferID *buffers, int num_buffers);

VAStatus dmd_EndPicture(VADriverContextP ctx, VAContextID context);

VAStatus dmd_vpp_end(VADriverContextP ctx, VAContextID context);
VAStatus dmd_vpp_render(VADriverContextP ctx, VAContextID context,
                        VABufferID *buffers, int num_buffers);
VAStatus dmd_vpp_query_filters(VADriverContextP ctx, VAContextID context,
                               VAProcFilterType *filters,
                               unsigned int *num_filters);
VAStatus dmd_vpp_query_filter_caps(VADriverContextP ctx, VAContextID context,
                                   VAProcFilterType type, void *filter_caps,
                                   unsigned int *num_filter_caps);
VAStatus dmd_vpp_query_pipeline_caps(VADriverContextP ctx,
                                     VAContextID context,
                                     VABufferID *filters,
                                     unsigned int num_filters,
                                     VAProcPipelineCaps *pipeline_caps);

VAStatus dmd_SyncSurface(VADriverContextP ctx, VASurfaceID render_target);

VAStatus dmd_SyncSurface2(VADriverContextP ctx, VASurfaceID surface,
                          uint64_t timeout_ns);

VAStatus dmd_SyncBuffer(VADriverContextP ctx, VABufferID buf_id,
                        uint64_t timeout_ns);

VAStatus dmd_PutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image,
                      int src_x, int src_y, unsigned int src_width,
                      unsigned int src_height, int dest_x, int dest_y,
                      unsigned int dest_width, unsigned int dest_height);

/* - export.c: dmabuf export - */

/* Export the surface to dmabuf (DRM_PRIME_2).
 *
 * Firefox takes the frame only this way: CreateImageVAAPI cannot get DRM_PRIE_2
 * returns DECODE_ERR and the player then returns softly and has no copy of the retreat path.
 * The ffmpeg command line does not need it (hwdownload walk vaDeriveImage + vamapBuffer). */
VAStatus dmd_ExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id,
                                 uint32_t mem_type, uint32_t flags,
                                 void *descriptor);

/* Wait until the surface pixel is really ready (internal self-lock, caller is not allowed to hold the lock).
 *
 * SyncSurface, to avoid locking up the water line with MediaCodec, will "had submitted but not ready"
 * , the searchface is now released. The real wait is postponed until the pixels are taken.
 * DeriveImage / GetImage must call this function before reading the geometry or copying of the surface. */
VAStatus dmd_surface_wait(struct dmd_driver *drv, VASurfaceID surface);

VAStatus dmd_QuerySurfaceStatus(VADriverContextP ctx,
                                VASurfaceID render_target,
                                VASurfaceStatus *status);

/* - image.c: VAImage Export - */

VAStatus dmd_CreateImage(VADriverContextP ctx, VAImageFormat *format,
                         int width, int height, VAImage *image);

VAStatus dmd_DeriveImage(VADriverContextP ctx, VASurfaceID surface,
                         VAImage *image);

VAStatus dmd_DestroyImage(VADriverContextP ctx, VAImageID image);

VAStatus dmd_GetImage(VADriverContextP ctx, VASurfaceID surface, int x, int y,
                      unsigned int width, unsigned int height, VAImageID image);

/* - Cross-file internal support (all callers are required to hold drv->lock) */

struct dmd_surface *dmd_find_surface_locked(struct dmd_driver *drv,
                                            VASurfaceID id);
struct dmd_context *dmd_find_context_locked(struct dmd_driver *drv,
                                            VAContextID id);
struct dmd_buffer *dmd_find_buffer_locked(struct dmd_driver *drv,
                                          VABufferID id);
struct dmd_image *dmd_find_image_locked(struct dmd_driver *drv, VAImageID id);

/* Invisible sync a surface (GetImage in surface when not ready).
 * Requires the caller to be locked; the internal lock will be used temporarily for IO, and the lock will remain on return --
 * The caller must re-search all cached object points. */
VAStatus dmd_surface_sync_locked(struct dmd_driver *drv, VASurfaceID surface,
                                 int timeout_ms);

/* Release a resource (unlocked) held in a surface slot. */
void dmd_surface_reset_locked(struct dmd_surface *s);
/* Releases a resource held in a context slot containing V4L2 sessions (unlocked; closes the device fd). */
void dmd_context_reset_locked(struct dmd_context *c);

/* Enter a VAImage description by the surface geometry (excluding image_id/buf).
 * This is the only real source of 1088-vs-1080 geometry that the direve shares with the new world.
 * The stride fax** bytes;ten_bit decision is sourcecc (P010 bytes per sample). */
void dmd_fill_image_geometry(VAImage *img, unsigned int disp_width,
                             unsigned int disp_height, unsigned int stride,
                             unsigned int slice_height, int ten_bit);

/* Alignment Auxiliary */
unsigned int dmd_align_up(unsigned int v, unsigned int align);

/* - PROFES.c INTERNAL ADDITIONAL - */

/* Whether this profile is supported by this driver (i. e. msm_vidc has a corresponding hardware decoder). */
int dmd_profile_supported(VAProfile profile);

/* The profile  ⁇  protocol codec id (DMD_CODEC_*). Unsupported profile returns -1. */
int dmd_profile_to_codec(VAProfile profile);

/* ---- h264_bitstream.c ---- */

/* SPS/ PPS NALU from the Pic Param inverted synthesis of 4 bytes from VA-API.
 * VA-API never transmits the original bits stream of the argument set, and daemon recognizes them with the starting code nar type
 * It's a codec-specific data, so you have to write it yourself.
 * Returns the byte number of write out, 0 for failure (including out_cap insufficient). */
/* The HEVC parameters are assembled (hevc_bitstream.c).
 *
 * dmd_hevc_can_build returns 0 to the stream that cannot be synthesized (VA-API lacks the necessary information).
 * The caller should allow the top down to be softened rather than produce bad images. */
int    dmd_hevc_can_build(const VAPictureParameterBufferHEVC *pp);
size_t dmd_hevc_build_vps_nalu(const VAPictureParameterBufferHEVC *pp,
                               VAProfile profile,
                               unsigned char *out, size_t cap);
size_t dmd_hevc_build_sps_nalu(const VAPictureParameterBufferHEVC *pp,
                               VAProfile profile,
                               unsigned char *out, size_t cap);
size_t dmd_hevc_build_pps_nalu(const VAPictureParameterBufferHEVC *pp,
                               unsigned char *out, size_t cap);

size_t dmd_h264_build_sps_nalu(const VAPictureParameterBufferH264 *pp,
                               VAProfile profile, unsigned int disp_width,
                               unsigned int disp_height, unsigned char *out,
                               size_t out_cap);
size_t dmd_h264_build_pps_nalu(const VAPictureParameterBufferH264 *pp,
                               const VAIQMatrixBufferH264 *iq, int have_iq,
                               const VASliceParameterBufferH264 *sp,
                               unsigned int def_l0_minus1,
                               unsigned int def_l1_minus1,
                               unsigned char *out, size_t out_cap);

/* External linear NV12 surfaces are encoder inputs. No caller FD is adopted. */
VAStatus dmd_import_surface_init(struct dmd_surface *surface, unsigned width,
                                  unsigned height, unsigned memory_type,
                                  const void *descriptor);
void dmd_import_ref(struct dmd_import *imported);
void dmd_import_unref(struct dmd_import *imported);
VAStatus dmd_import_copy(const struct dmd_surface *surface, unsigned char *data,
                          size_t capacity);

#endif /* DMD_DRIVER_H */
