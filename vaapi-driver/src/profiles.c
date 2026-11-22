/* Profile / enterpoint / config query and config object management
 *
 * The declared capability is strictly matched by the codec that can be provided by msm_vidc on /dev/video32:
 * Protocol codec extraction 0=H.264 1=HEVC 2=VP9 3=VP8 (derelicted) 4=AV1.
 * V4L2 Verified from end-to-end with direct access H.264 300/300, HEVC 12/12, VP9 P0 50/50
 * (all bytes equal to soft resolution). AV1 frames are identical but not adopted in pixels, as detailed in doc/av1-v4l2-status.md.
 * VP8 Not achieved: The driver is missing the cipher flow re-establishment of VP8 (RFC 6386 uncompressed chank synthesis).
 * Note that the hardware is not not supported - the OUTPUT format of /dev/ video32 does contain VP80, but the U.S.B.
 * See the explanation of corrections to the statement below this document.
 * HEVC Main10/P010 has been validated in pixels by SM8750P and is independently enabled with HEVC10=1;
 * The default remains closed pending further platform/application validation. VP9 Profile2, H.264 High10 is not in this range.
 */

#include <string.h>

#include "dmd_client.h"
#include "v4l2_backend.h"
#include "driver.h"
#include "v4l2_encoder.h"

static int encode_profile_index(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline: return DMD_ENC_H264_BASELINE;
    case VAProfileH264Main: return DMD_ENC_H264_MAIN;
    case VAProfileH264High: return DMD_ENC_H264_HIGH;
    case VAProfileHEVCMain: return DMD_ENC_HEVC_MAIN;
    case VAProfileVP9Profile0: return DMD_ENC_VP9_0;
    default: return -1;
    }
}

static int encode_profile_supported(VADriverContextP ctx, VAProfile profile)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);
    int p = encode_profile_index(profile);
    return drv && p >= 0 && (drv->encode_caps & DMD_ENC_CAP(p));
}

static void encode_dimensions(VAProfile profile, unsigned *min_w, unsigned *min_h,
                               unsigned *max_w, unsigned *max_h)
{
    *min_w = DMD_MIN_WIDTH;
    *min_h = DMD_MIN_HEIGHT;
    *max_w = DMD_MAX_WIDTH;
    *max_h = DMD_MAX_HEIGHT;
    unsigned a, b, c, d;
    int p = encode_profile_index(profile);
    if (p >= 0 && dmd_v4l2_enc_get_limits(p, &a, &b, &c, &d) == 0) {
        if (a > *min_w) *min_w = a;
        if (b > *min_h) *min_h = b;
        if (c < *max_w) *max_w = c;
        if (d < *max_h) *max_h = d;
    }
}

/* This driver supports the profile collection. The order is the output order of the vainfo.
 *
 * ⁇  Only list ** decoding path really works ** profile. Declarations are worse than non-declared –
 * The consumer will select us and then fail, and it could have been softened.
 *
 * The HEVC (VAProfileHeVCMain) was removed for a while because of "bitstream forwarding not realized" --
 * It was declared false, and the Vapitest probe of Firefox read into CODEC_HW_HEVC
 * (GfxInfo.h:63 of 1 <8 < > > ) hevc_bitstream.c has now achieved VPS/SPS/PPS synthesis
 * The video is available in the blog of the blog "The Acquis" (http://www.flickr.com/) and the real machine (1080p/4K/720p by byte, long stream 1500 frames, Seek all).
 * So re-stated.
 * "The probe output of 368 = H264  ⁇ VP8 ⁇ VP9 ⁇ HEVC" is outdated:
 *   VP8 declared removed and the probe bitmap no longer contains CODEC_HW_VP8.
 *
 * The SPS reference set for HEVC can be found in the supported Main 8-bit stream based on VA DPB/POC
 * Recreates as a reference set in a visible slice. SPS long-term reference table, custom scale table and unknown slice
 * (c) The border is still subject to restricted checks by dmd_hevc_can_build/hevc_slice.c;
 * Main10 to reuse these limitations and to check the 10-bit parameter against P010 output with additional rigour;
 * Restricted support cannot be interpreted as being verified as being arbitrary.
 */
static const VAProfile dmd_profiles[] = {
    VAProfileH264ConstrainedBaseline,
    VAProfileH264Main,
    VAProfileH264High,
    VAProfileHEVCMain,
    VAProfileVP9Profile0,
#if defined(DMD_ENABLE_HEVC_MAIN10) || defined(DMD_PROBE_10BIT_PROFILES)
    /* HEVC10=1: independently gated Main10/P010 decode validation. This does
     * not enable 10-bit encoding, VP9 Profile2, or browser plane imports. */
    VAProfileHEVCMain10,
#endif
#ifdef DMD_PROBE_10BIT_PROFILES
    /* Specialized for detection (-DMDD_PROBE_10BIT_PROFILES): declaration 10bit profile
     * ffmpeg willing to hand over the Main10/ VP9 Profile2 code flow to match the running time
     * DMD_PROBE_10BIT=1 Determines whether solids are really solventable.
     * This old diagnostic switch also declares VP9 Profile2 not yet verified; Main10 please use standalone
     * HEVC10=1. Private detection layouts such as QP10/Q12A do not qualify for P010 support. */
    VAProfileVP9Profile2,
#endif
    /* VP8:0.4.2 Restatement.
     *
     * The reason I didn't say before was "Driver unwritten stream" but that's already been...
     * Unestablished — vp8_build_frame() to achieve RFC 6386 § 9.1
     * Start code for uncompressed data chuck synthesis (frame tag + key frame)
     * & Size, parameter collection (VASLiceParameter BufferVP8/
     * DMD_CODEC_VP8 branch for VAPictureParameter BufferVP8) and build_unit
     * All that is missing is the declaration and the mapping of two gates in dmd_profile_to_codec.
     *
     * Hardware side is equally valid: VIDIOC_ENUM_FMT on / dev/ video32 side of OTPUT
     * Lists MPG2/H264/HEVC/VP80/VP90, column VP80. */
    VAProfileVP8Version0_3,
#ifdef DMD_ENABLE_MPEG2
    /* MPEG-2:  ⁇  Not yet ** (to be activated only with -DDDDD_ENABLE_MPEG2).
     *
     * Achieved as complete: mpeg2_bitstream.c synthesized stream with ffmpeg original stream
     * ** Bytes identical** (actually 1 frame 82620 bytes, 2 frame 129038 bytes,
     * There were zero byte differences, and the seq → ext → GOP → picture → ext → slice order matched.)
     * The synthesis correctness can therefore be considered proven.
     *
     * But after sending /dev/video32, the Venus solid was in the 2nd Unit newspaper SYS_ERROR, which is a very good source of information about the situation in the country.
     * A frame doesn't vomit. Since there is no by-product difference between the stream and the original stream that the solid itself can solve,
     * The problem is not synthesis, but the SM8150 feed path for MPG2 in this solid device...
     * For further investigation, OUTPUT sizeimage is only 1958400 (16588800 for H.264/HEVC),
     * And the single frame actually 129038 bytes can be loaded, but the solid may be MPEG-2 required
     * The border with the GOP/S sequence is not a frame-by-frame delivery.
     *
     * Declares that it is false: ffmpeg's-hwackel vaapi encounters
     * The declared profile will not fall soft, will simply fail to exit. */
    VAProfileMPEG2Main,
#endif
#ifdef DMD_ENABLE_AV1
    /* Development/debug special: -DDDD_ENABLE_AV1 is the only AV1 declaration.
     * The release is not stated (for reasons, see comment below), but the device still needs to be able to directly monitor the hardware chain.
     * So use the compiler switch instead of deleting the code -- so that every debug can be manually changed. */
    VAProfileAV1Profile0,
#endif
    /* ⁇  AV1 is not declared (0.4.0 release).
     *
     * AV1 hardline is available on this device, but the synthesizing of the driver is not yet fully correct:
     * The number of frames is the same as dav1d (150), the synthesizing 145/150 by bytes,
     * And there are only 17/30 different images in pixels (soft resolution 30) --
     * The missing are 70 show_existing_frames, which are detailed in doc/av1-v4l2-status.md.
     *
     * Declares AV1 is a false statement before the pixel verification is passed: Firefox/ ffmpeg will follow
     * Hand over the AV1 decoding task, and get double frames and flower screens, which are worse than a downscaling.
     * Map (dmd_profile_to_code)
     * It is not in itself a good thing, but it can be added to the statement form after the pixels are passed.
     * The project is a pilot project to develop a version of the pilot - DMDD_ENABLE_AV1 (see #ifdef above) and a new version of the project is available.
     * No manual changes to source files.
     *
     * ⁇  One confirmed difference in behaviour: under the release (not declaring AV1) the number of people who are not allowed to live in the country is not the same as the number of people who live in the country.
     * `-hwackel vaapi ' of ffmpeg meets an undeclared profile ** not to return silently **, but to be able to do so.
     * Instead, it exits immediately with status 69:
     *     No support for codec av1 profile 0.
     *     Your platform doesn't support hardware accelerated AV1 decoding.
     * This is the ffmpeg's established semantic (-hwaccel is a mandatory requirement, not "do what you can" ),
     * Not a bug in this drive -- a simple soft resolution of 30 frames when the same stream does not contain -hwaccle.
     * Use `-hwacel auto ' or not specified -hwacel for auto-dropping.
     * Firefox is a capability-detector path that is not attempted without a declaration.
     *
     * VP8 has removed the declaration (V4L2 direct to part of the adaptation).
     *
     * ⁇  (80th round of live measurements): No V4L2 floor from "msm_vidc"
     * VP80 format ", ** that's wrong**. Use this on /dev/ video32
     * INDIOC_ENUM_FMT emulation OF UTPUT side format, actual output:
     *     MPG2  H264  HEVC  VP80  VP90
     * VP80 is clearly in line, hardware paths exist.
     * (Node ID verified: msm_vidc_driver card=msm_vidc_vdec,
     *   Not an enumeration failure or detection of incorrect nodes.
     *
     * The real reason why VP8 is not stated is that the driver did not recreate its stream:
     * VA-API only gives data for the partion, missing RFC 6386 § 9.1 uncompressed chiunk,
     * A synth like h264/hevc/av1 is required and is not written at this time.
     * The codec map of vP8 returns 0 is the result of this decision.
     * It's not a reflection of hardware capacity -- it was treated as a hardware fact, and it was a cause-effect reversal.
     *
     * So don't say it until the synthesizer is in place.
     * The probe will report the mission to CODEC_HW_VP8 and then fail.
     * But that is** to be achieved** and not** impossible**.
     *
     * Protocol codec 3 Retention not repeated (see dmd_cliet.h:63,
     * v42_backend.h:46) to avoid collision with the old daemon's line protocol.
     * The map in dmd_profile_to_codec is also removed:
     * Keep it will allow vCreateConfig to return to success on VP8. */
};

#define DMD_NUM_PROFILES ((int)(sizeof(dmd_profiles) / sizeof(dmd_profiles[0])))

int dmd_profile_supported(VAProfile profile)
{
    /* Align with dmd_QueryConfigProfiles: Solid not supported
     * Even in the static form, codec is not supported. */
    if (profile == VAProfileMPEG2Main && !dmd_v4l2_probe(DMD_CODEC_MPEG2))
        return 0;
    if (profile == VAProfileVP8Version0_3 && !dmd_v4l2_probe(DMD_CODEC_VP8))
        return 0;
    if (profile == VAProfileAV1Profile0 && !dmd_v4l2_probe(DMD_CODEC_AV1))
        return 0;
    for (int i = 0; i < DMD_NUM_PROFILES; i++)
    {
        if (dmd_profiles[i] == profile)
            return 1;
    }
    return 0;
}

/* The protocol.
 * High depth profile is not mapd without authentication.
 * HEVCMAin reserves the map but does not ** state in dmd_profiles that: The map itself is harmless, but the map is not a good idea.
 * Once the HEVC stream is implemented, just add the profile back to the statement form. */
int dmd_profile_to_codec(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        return DMD_CODEC_H264;
    case VAProfileHEVCMain:
        return DMD_CODEC_HEVC;
    case VAProfileVP9Profile0:
        return DMD_CODEC_VP9;
    /* PP8:0.4.2 Restore map, stream reconstruction see vp8_build_frame(). */
    case VAProfileVP8Version0_3:
        return DMD_CODEC_VP8;
    /* MPEG-2: Simple takes the same V4L2 path as Main (MPG2 sourcecc).
     * Simple simply limits B frames and color formatting, and the syntax is the true collection of Main. */
    case VAProfileMPEG2Simple:
    case VAProfileMPEG2Main:
        return DMD_CODEC_MPEG2;
#if defined(DMD_ENABLE_HEVC_MAIN10) || defined(DMD_PROBE_10BIT_PROFILES)
    case VAProfileHEVCMain10:
        return DMD_CODEC_HEVC;
#endif
#ifdef DMD_PROBE_10BIT_PROFILES
    /* Legacy standalone VP9 Profile2 probe; HEVC10 does not enable this. */
    case VAProfileVP9Profile2:
        return DMD_CODEC_VP9;
#endif
    /* AV1 Profile0 = Main (8/10bit 4:2-0), Profile1 = High (including 4:4:4).
     * Video/ av01 of VideoCodec only map profile0: Undifferentiated profile by bitstream
     * Sequence headers self-explanatory, but the output format of 4:4:4 is not NV12 and this driver's frameback hypothesis is not valid. */
    case VAProfileAV1Profile0:
        return DMD_CODEC_AV1;
    default:
        return -1;
    }
}

VAStatus dmd_QueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list,
                                 int *num_profiles)
{
    if (!ctx || !profile_list || !num_profiles)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* Run capability filtering: The static statement is a statement that "drives re-establishment of the stream", and eventually can it be?
     * Check the machine solid. New msm_vidc MPG2/VP80 from the kernel is already available from EUTPUT
     * The list of names disappeared (S_FMT silently returned to H.264, dmesg newspaper "unsupported codec" ), which is a very interesting story.
     * VP8s are kept in static form, and they're going to let the consumer turn over the task and fail.
     * Worse. Probe with ENUM_FMT at full cost. */
    static int probed = 0;
    static unsigned probed_mask = 0;   /* bit: value of DMD_CODEC_* */
    if (!probed) {
        static const struct { VAProfile p; int codec; } map[] = {
            { VAProfileMPEG2Main, DMD_CODEC_MPEG2 },
            { VAProfileVP8Version0_3, DMD_CODEC_VP8 },
            { VAProfileAV1Profile0, DMD_CODEC_AV1 },
        };
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (dmd_v4l2_probe(map[i].codec))
                probed_mask |= 1u << map[i].codec;
        probed = 1;
    }

    int n = 0;
    for (int i = 0; i < DMD_NUM_PROFILES; i++) {
        VAProfile p = dmd_profiles[i];
        if (p == VAProfileMPEG2Main && !(probed_mask & (1u << DMD_CODEC_MPEG2)))
            continue;
        if (p == VAProfileVP8Version0_3 && !(probed_mask & (1u << DMD_CODEC_VP8)))
            continue;
        if (p == VAProfileAV1Profile0 && !(probed_mask & (1u << DMD_CODEC_AV1)))
            continue;
        profile_list[n++] = p;
    }
    *num_profiles = n;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_QueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile,
                                    VAEntrypoint *entrypoint_list,
                                    int *num_entrypoints)
{
    if (!ctx || !entrypoint_list || !num_entrypoints)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (profile == VAProfileNone) {
        entrypoint_list[0] = VAEntrypointVideoProc;
        *num_entrypoints = 1;
        return VA_STATUS_SUCCESS;
    }

    int decode = dmd_profile_supported(profile);
    int encode = encode_profile_supported(ctx, profile);
    if (!decode && !encode) {
        *num_entrypoints = 0;
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    int n = 0;
    if (decode)
        entrypoint_list[n++] = VAEntrypointVLD;
    if (encode)
        entrypoint_list[n++] = VAEntrypointEncSlice;
    *num_entrypoints = n;
    return VA_STATUS_SUCCESS;
}

/* The profile allows RTFormat.
 *
 * HEVC Main10 and AV1 Profile0 can carry 8- or 10-bit samples. The frame's
 * syntax and target surface select NV12/P010; this does not imply conversion
 * between bit depths. Chrome also requires YUV420 in Main10 capability/config
 * probes even when its actual ten-bit frames use P010 surfaces. */
static uint32_t rt_formats_for_profile(VAProfile profile)
{
    if (profile == VAProfileHEVCMain10)
        return VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10;
    if (profile == VAProfileAV1Profile0)
        return VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10;
    return VA_RT_FORMAT_YUV420;
}

static uint32_t encode_rate_controls(VAProfile profile)
{
    return VA_RC_CBR | VA_RC_VBR |
           (profile == VAProfileVP9Profile0 ? 0 : VA_RC_CQP);
}

VAStatus dmd_GetConfigAttributes(VADriverContextP ctx, VAProfile profile,
                                 VAEntrypoint entrypoint,
                                 VAConfigAttrib *attrib_list, int num_attribs)
{
    if (!ctx || (num_attribs > 0 && !attrib_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_attribs < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    const int vpp = profile == VAProfileNone && entrypoint == VAEntrypointVideoProc;
    if (!vpp && !dmd_profile_supported(profile) && !encode_profile_supported(ctx, profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    const int encoding = entrypoint == VAEntrypointEncSlice;
    if ((encoding && !encode_profile_supported(ctx, profile)) ||
        (!encoding && !vpp && (entrypoint != VAEntrypointVLD || !dmd_profile_supported(profile))))
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

    unsigned min_w, min_h, max_w = DMD_MAX_WIDTH, max_h = DMD_MAX_HEIGHT;
    if (encoding)
        encode_dimensions(profile, &min_w, &min_h, &max_w, &max_h);

    /* VA-API protocol: caller to enter attachment type, value;
     * Unsupported entries must be filled in VA_ATTRIB_NOT_SUPPORTED instead of misreporting. */
    for (int i = 0; i < num_attribs; i++) {
        if (encoding) {
            switch (attrib_list[i].type) {
            case VAConfigAttribRTFormat: attrib_list[i].value = VA_RT_FORMAT_YUV420; break;
            case VAConfigAttribRateControl: attrib_list[i].value = encode_rate_controls(profile); break;
            case VAConfigAttribEncMaxRefFrames: attrib_list[i].value = 1; break;
            case VAConfigAttribEncMaxSlices: attrib_list[i].value = 1; break;
            case VAConfigAttribEncPackedHeaders:
            case VAConfigAttribEncInterlaced:
            case VAConfigAttribEncSliceStructure: attrib_list[i].value = 0; break;
            case VAConfigAttribPredictionDirection:
                attrib_list[i].value = VA_PREDICTION_DIRECTION_PREVIOUS; break;
            case VAConfigAttribMaxPictureWidth: attrib_list[i].value = max_w; break;
            case VAConfigAttribMaxPictureHeight: attrib_list[i].value = max_h; break;
            default: attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED; break;
            }
            continue;
        }
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            attrib_list[i].value = rt_formats_for_profile(profile);
            break;
        case VAConfigAttribMaxPictureWidth:
            attrib_list[i].value = DMD_MAX_WIDTH;
            break;
        case VAConfigAttribMaxPictureHeight:
            attrib_list[i].value = DMD_MAX_HEIGHT;
            break;
        case VAConfigAttribDecSliceMode:
            /* Damon receives the full AnnetB data module corresponding to the nonmal slice mode. */
            attrib_list[i].value = vpp ? VA_ATTRIB_NOT_SUPPORTED : VA_DEC_SLICE_MODE_NORMAL;
            break;
        default:
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }

    return VA_STATUS_SUCCESS;
}

VAStatus dmd_CreateConfig(VADriverContextP ctx, VAProfile profile,
                          VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
                          int num_attribs, VAConfigID *config_id)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !config_id)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_attribs < 0 || (num_attribs > 0 && !attrib_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_attribs > DMD_MAX_CONFIG_ATTRIBUTES)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    const int vpp = profile == VAProfileNone && entrypoint == VAEntrypointVideoProc;
    if (!vpp && !dmd_profile_supported(profile) && !encode_profile_supported(ctx, profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    const int encoding = entrypoint == VAEntrypointEncSlice;
    if ((encoding && !encode_profile_supported(ctx, profile)) ||
        (!encoding && !vpp && (entrypoint != VAEntrypointVLD || !dmd_profile_supported(profile))))
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

    if (encoding) {
        for (int i = 0; i < num_attribs; i++) {
            uint32_t value = attrib_list[i].value;
            switch (attrib_list[i].type) {
            case VAConfigAttribRTFormat:
                if (value != VA_RT_FORMAT_YUV420)
                    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
                break;
            case VAConfigAttribRateControl:
                if (!value || (value & (value - 1)) || !(value & encode_rate_controls(profile)))
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                break;
            case VAConfigAttribEncMaxRefFrames:
                if (value > 1) return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                break;
            case VAConfigAttribEncMaxSlices:
                if (value != 1) return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                break;
            case VAConfigAttribEncPackedHeaders:
            case VAConfigAttribEncInterlaced:
            case VAConfigAttribEncSliceStructure:
                if (value) return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                break;
            default:
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
        }
    }

    if (!encoding && profile == VAProfileHEVCMain10) {
        for (int i = 0; i < num_attribs; i++)
            if (attrib_list[i].type == VAConfigAttribRTFormat &&
                (!attrib_list[i].value ||
                 (attrib_list[i].value & ~rt_formats_for_profile(profile))))
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    VAStatus status = VA_STATUS_ERROR_ALLOCATION_FAILED;

    pthread_mutex_lock(&drv->lock);
    for (int i = 0; i < DMD_MAX_CONFIGS; i++) {
        struct dmd_config *cfg = &drv->configs[i];
        if (cfg->in_use)
            continue;

        memset(cfg, 0, sizeof(*cfg));
        cfg->in_use = 1;
        /* ID from 1:0 left for "not initialized" sentries outside VA_INVALID_ID,
         * Avoids calling parties mistaking 0 as legal config. */
        cfg->id = (VAConfigID)(drv->next_config_id++);
        cfg->profile = profile;
        cfg->entrypoint = entrypoint;
        cfg->num_attribs = num_attribs;
        if (num_attribs > 0)
            memcpy(cfg->attribs, attrib_list,
                   (size_t)num_attribs * sizeof(VAConfigAttrib));

        *config_id = cfg->id;
        status = VA_STATUS_SUCCESS;
        break;
    }
    pthread_mutex_unlock(&drv->lock);

    if (status == VA_STATUS_SUCCESS)
        dmd_log("CreateConfig: profile=%d entrypoint=%d -> config=%u\n",
                (int)profile, (int)entrypoint, (unsigned)*config_id);
    else
        dmd_log("CreateConfig: Depletion of slots (ceiling) %d)\n", DMD_MAX_CONFIGS);

    return status;
}

/* Press ID to find config subject subject to locking. */
static struct dmd_config *find_config_locked(struct dmd_driver *drv,
                                             VAConfigID id)
{
    for (int i = 0; i < DMD_MAX_CONFIGS; i++) {
        if (drv->configs[i].in_use && drv->configs[i].id == id)
            return &drv->configs[i];
    }
    return NULL;
}

VAStatus dmd_DestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    VAStatus status;

    pthread_mutex_lock(&drv->lock);
    struct dmd_config *cfg = find_config_locked(drv, config_id);
    if (cfg) {
        cfg->in_use = 0;
        status = VA_STATUS_SUCCESS;
    } else {
        status = VA_STATUS_ERROR_INVALID_CONFIG;
    }
    pthread_mutex_unlock(&drv->lock);

    return status;
}

VAStatus dmd_QueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id,
                                   VAProfile *profile, VAEntrypoint *entrypoint,
                                   VAConfigAttrib *attrib_list,
                                   int *num_attribs)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !profile || !entrypoint || !attrib_list || !num_attribs)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    VAStatus status;

    pthread_mutex_lock(&drv->lock);
    struct dmd_config *cfg = find_config_locked(drv, config_id);
    if (cfg) {
        *profile = cfg->profile;
        *entrypoint = cfg->entrypoint;

        /* Reward**Driver supports **, not echo CreateConfig entry.
         *
         * The ffmpeg and Firefox are valid for the ffmpeg and the ffrefox, which are the only ones that have been copied back here.
         * Because they're sending RTFormat when they build config, and the recollection is exactly the same as the real one.
         *
         * But chrome** does not pass any attributes** construct config and then call function query driver
         * What you're supporting. In echo mode, it gets num_attribs=0, it doesn't read RTFormat,
         * So FillProfileInfo_Locked determined that the profile was not available...
         * The six profiles failed and hard-copy is totally unavailable.
         *
         * VA-API semantic is "drive the ability to declare," and it's wrong to say it now, either.
         * CreateConfig has something that RTFormat must be in the back episode. */
        int n = 0;
        int have_rtformat = 0;
        const uint32_t rt = rt_formats_for_profile(cfg->profile);

        for (int i = 0; i < cfg->num_attribs && n < DMD_MAX_CONFIG_ATTRIBUTES; i++) {
            if (cfg->attribs[i].type == VAConfigAttribRTFormat) {
                /* Use the actual supported value to overwrite the caller's input */
                attrib_list[n].type = VAConfigAttribRTFormat;
                attrib_list[n].value = rt;
                have_rtformat = 1;
            } else {
                attrib_list[n] = cfg->attribs[i];
            }
            n++;
        }

        if (!have_rtformat && n < DMD_MAX_CONFIG_ATTRIBUTES) {
            attrib_list[n].type = VAConfigAttribRTFormat;
            attrib_list[n].value = rt;
            n++;
        }

        *num_attribs = n;
        status = VA_STATUS_SUCCESS;
    } else {
        status = VA_STATUS_ERROR_INVALID_CONFIG;
    }
    pthread_mutex_unlock(&drv->lock);

    return status;
}

VAStatus dmd_QuerySurfaceAttributes(VADriverContextP ctx, VAConfigID config,
                                    VASurfaceAttrib *attrib_list,
                                    unsigned int *num_attribs)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !num_attribs)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* Config must be effective -- consumers judge the capacity of the configuration 's surface. */
    pthread_mutex_lock(&drv->lock);
    struct dmd_config *cfg = find_config_locked(drv, config);
    VAProfile cfg_profile = cfg ? cfg->profile : VAProfileNone;
    int encoding = cfg && cfg->entrypoint == VAEntrypointEncSlice;
    pthread_mutex_unlock(&drv->lock);
    if (!cfg)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    unsigned min_w = DMD_MIN_WIDTH, min_h = DMD_MIN_HEIGHT;
    unsigned max_w = DMD_MAX_WIDTH, max_h = DMD_MAX_HEIGHT;
    if (encoding)
        encode_dimensions(cfg_profile, &min_w, &min_h, &max_w, &max_h);

    /* Main10/AV1 expose NV12 and P010 for matching 8/10-bit frame syntax;
     * HEVC Main and all encoders remain NV12-only. */
    const uint32_t rt = encoding ? VA_RT_FORMAT_YUV420 : rt_formats_for_profile(cfg_profile);
    const int eight_bit = (rt & VA_RT_FORMAT_YUV420) != 0;
    const int ten_bit = (rt & VA_RT_FORMAT_YUV420_10) != 0;

    /* The two-part query agreed upon by VA-API: only the amount of return when the attrib_list is NULL. */
    const unsigned int needed = 4u + eight_bit + ten_bit + (encoding ? 1u : 0u);
    if (!attrib_list) {
        *num_attribs = needed;
        return VA_STATUS_SUCCESS;
    }
    if (*num_attribs < needed) {
        *num_attribs = needed;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    unsigned int n = 0;

    if (eight_bit) {
        attrib_list[n].type = VASurfaceAttribPixelFormat;
        attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
        attrib_list[n].value.type = VAGenericValueTypeInteger;
        attrib_list[n].value.value.i = VA_FOURCC_NV12;
        n++;
    }

    if (ten_bit) {
        attrib_list[n].type = VASurfaceAttribPixelFormat;
        attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE |
                               VA_SURFACE_ATTRIB_SETTABLE;
        attrib_list[n].value.type = VAGenericValueTypeInteger;
        attrib_list[n].value.value.i = VA_FOURCC_P010;
        n++;
    }

    attrib_list[n].type = VASurfaceAttribMinWidth;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = min_w;
    n++;

    attrib_list[n].type = VASurfaceAttribMinHeight;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = min_h;
    n++;

    attrib_list[n].type = VASurfaceAttribMaxWidth;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = max_w;
    n++;

    attrib_list[n].type = VASurfaceAttribMaxHeight;
    attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[n].value.type = VAGenericValueTypeInteger;
    attrib_list[n].value.value.i = max_h;
    n++;

    if (encoding) {
        attrib_list[n].type = VASurfaceAttribMemoryType;
        attrib_list[n].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
        attrib_list[n].value.type = VAGenericValueTypeInteger;
        attrib_list[n].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
        n++;
    }

    *num_attribs = n;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_QueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list,
                               int *num_formats)
{
    if (!ctx || !format_list || !num_formats)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* Linear half-plane: NV12 (8bit) and P010 (10bit).
     *
     * *num_formats is ** the array capacity when entering ** (libva in/out of the agreement),
     * Write the second one only when the capacity is enough -- otherwise step on the caller's pad. */
    const int cap = *num_formats;

    memset(&format_list[0], 0, sizeof(format_list[0]));
    format_list[0].fourcc = VA_FOURCC_NV12;
    format_list[0].byte_order = VA_LSB_FIRST;
    format_list[0].bits_per_pixel = 12; /* plane YUV420 per pixel 12 bit */

    *num_formats = 1;
    if (cap < 2)
        return VA_STATUS_SUCCESS;

    memset(&format_list[1], 0, sizeof(format_list[1]));
    format_list[1].fourcc = VA_FOURCC_P010;
    format_list[1].byte_order = VA_LSB_FIRST;
    format_list[1].bits_per_pixel = 12; /* nominal;effective 10bit expressed by sourcecc */

    *num_formats = 2;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_QuerySubpictureFormats(VADriverContextP ctx,
                                    VAImageFormat *format_list,
                                    unsigned int *flags,
                                    unsigned int *num_formats)
{
    (void)format_list;
    (void)flags;

    if (!ctx || !num_formats)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* Not supported subpicture (subtitles, etc.). returns 0 format instead of misreporting:
     * Consumers skip the subcepture path accordingly. */
    *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_QueryDisplayAttributes(VADriverContextP ctx,
                                    VADisplayAttribute *attr_list,
                                    int *num_attributes)
{
    (void)attr_list;

    if (!ctx || !num_attributes)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* No-tiff properties (blogness/comparison, etc., are handled by consumers themselves). */
    *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_GetDisplayAttributes(VADriverContextP ctx,
                                  VADisplayAttribute *attr_list,
                                  int num_attributes)
{
    if (!ctx || (num_attributes > 0 && !attr_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* We do not declare any display properties, so any queries are marked as unsupported. */
    for (int i = 0; i < num_attributes; i++)
        attr_list[i].flags = VA_DISPLAY_ATTRIB_NOT_SUPPORTED;

    return VA_STATUS_SUCCESS;
}

VAStatus dmd_SetDisplayAttributes(VADriverContextP ctx,
                                  VADisplayAttribute *attr_list,
                                  int num_attributes)
{
    (void)attr_list;
    (void)num_attributes;

    if (!ctx)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
}
