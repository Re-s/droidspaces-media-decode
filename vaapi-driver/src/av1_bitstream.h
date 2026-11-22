/* AV1 OBU inverted.
 *
 * Why this file is needed: VA-API delivers the AV1 decoding parameters to the driver **structured fields**
 * (VADecPictureParameter BufferAV1) The code side is given only to **naked Tile load**-
 * No OBU envelopes are available. Downstream MediaCodec wants the full AV1 OBU stream.
 *
 * Libva herself made this clear (/usr/include/va/va_dec_av1.h: 643-645):
 *   "host decoder is responsible to parse out the per tile information.
 *    And the bit stream in sent to driver in per tile granularity."
 * Same file: 637-638 and also indicates VASLiceParameter BufferAV1 "actually means
 * VATileParameterBufferAV1"。
 *
 * This is completely different from the VP9 situation, not the same. The Slick Data of VP9 is the complete frame.
 * So that road was re-established; AV1 would be sent out on the same path.
 * Illegal stream - actual first byte 0xd0 (forbidden_bit=1, OBU type 10 is a reserved value)
 * The decoder is "No security header available", and no frame can be solved.
 *
 * bits to repeat bitstream.h: f(n) of AV1 and u(n) of H.264/HEVC
 * Consistency (all MSB preferred fixed bit width, AV1 norm 4.10.2 definition of f(n)),
 * The de-dentation sequence for dmd_bw_put_bits is the same. AV1 unique long code
 * (leb128/ uvlc/le) is completed in this document.
 */
#ifndef DMD_AV1_BITSTREAM_H
#define DMD_AV1_BITSTREAM_H

#include <stddef.h>
#include <stdint.h>

#include "bitstream.h"

/* OBU type (AV1 Regulation 6.2.2 Table obu_type). */
enum {
    DMD_OBU_SEQUENCE_HEADER        = 1,
    DMD_OBU_TEMPORAL_DELIMITER     = 2,
    DMD_OBU_FRAME_HEADER           = 3,
    DMD_OBU_TILE_GROUP             = 4,
    DMD_OBU_METADATA               = 5,
    DMD_OBU_FRAME                  = 6,
    DMD_OBU_REDUNDANT_FRAME_HEADER = 7,
    DMD_OBU_TILE_LIST              = 8,
    DMD_OBU_PADDING                = 15,
};

/* The maximum bytes of the eb128 (AV1 norm 4.10.5: maximum 8 bytes). */
#define DMD_LEB128_MAX 8

/* - - - - - - - - - - - - - - - - - - - - - - - Longer codes */

/* LEB128(v): A small 7-bit group with a byte height marked "and follow."
 * Write out, returns bytes;out_cap is not enough to return 0. */
size_t dmd_av1_leb128(uint64_t v, unsigned char *out, size_t out_cap);

/* The encoded length of the leb128 is not actually written - bytes used to calculate the obu_size first. */
size_t dmd_av1_leb128_len(uint64_t v);

/* uvlc(): AV1 Regulation 4.10.3. The leading zero count + the tail number used for the frame_header
 * The number of small numbers (e.g. number_info's num_units_in_display_tick) does not need it.
 * The blog is a blog of the blog Elaa, which is a blog of the blog Elaaa, which is a blog of the blog Elaa. */
void dmd_av1_put_uvlc(struct dmd_bitwriter *bw, uint32_t v);

/* le (n): AV1 Regulation 4.10.4.n Byte Integer,** which must be called when the byte is aligned**. */
void dmd_av1_put_le(struct dmd_bitwriter *bw, uint64_t v, int nbytes);

/* ns(n): AV1 specification 4.10.7 asymmetrical double-value encoding.
 * Context_update_tile_id and tile sizes are used. */
void dmd_av1_put_ns(struct dmd_bitwriter *bw, uint32_t v, uint32_t n);

/* su(n): AV1 regulates the sign length of 4.10.6. Global_motion etc. */
void dmd_av1_put_su(struct dmd_bitwriter *bw, int32_t v, int nbits);

/* - - - - - - - - - - - - - - - - - - - - - - - - - - Alignment */

/* Byte_alignment(): AV1 Reg. 5.3.5.
 * Unlike the ribsp_trailing_bits of H.264 -- AV1 ** stop bit,
 * Pure zero. Errors allow the decoder to read the fill as a grammar element. */
void dmd_av1_byte_align(struct dmd_bitwriter *bw);

/* Trailing_bits(): AV1 Regulator 5.3.4. Write a 1 then add zero to bytes boundary.
 * For OBU **payload ending** (segment_header_obu and
 * The end of the page is not the end of the sentence. */
void dmd_av1_trailing_bits(struct dmd_bitwriter *bw);

/* - - - - - - - - - - - - - - - - - - - - Head of the OBU */

/* - - - - - - - - - - - - - - - - - - - Sequence head synthesis (2/4) */

/* Combine a complete OBU_SEQUENCE_HEADER from VADecPictureParameter BufferAV1
 * (includes OBU headers + payload + trading_bits).
 *
 * Pic is the const void* not the specific type: this file is deliberately not intended to be include vá_dec_av1.h, but is not intended to be a part of the current document.
 * So that libva can be spread to the user that only needs bits of original language.
 *
 * Write out, returns the total bytes; the capacity is insufficient or the parameter is returned illegally. */
size_t dmd_av1_build_sequence_header(const void *pic,
                                     unsigned char *out, size_t out_cap);

/* - - - - - - - - - - - - - - OBU_FRAME assembly (3/4 + 4/4) */

/*
 * Self-regulation DPB status (shadow frame management).
 *
 * Why it is needed: both refresh_frame_flags and ref_frame_idx are written by this module into the stream, and the user is not allowed to write about it.
 * VA-API does not provide refresh_frame_flags (that's the encoder's GOP decision, with the source stream)
 * Disassembleded. The solution is not to restore the encoder, but to maintain one.
 * ** Schlot allocation**: I said this frame is stored in k, and then I reported k when it was quoted.
 * The decoder only requires the sameness of the two, not the same as the original encoder.
 *
 * Holds one copy of each decoding context, held across frames.
 * New confext is the initial state when the whole package is zero.
 */
struct dmd_av1_dpb {
    /* ⁇  Shadowwatch** This is a driven frame number** (frame_seq, from 1), not a surface id.
     * ffmpeg recovers the recovery search: the same VASURfaceID is in different frames
     * "Ox1 with oh7" "Surface 6" is "surface"
     * It's a mix of dead frames and reused living frames. */
    int         dpb_shadow[8];     /* Slot - - The current frame number of the slot */
    unsigned    dpb_order_hint[8]; /* The slot-- the frame of the slot is over.
                                    * The frame of the error_resilient frame is to be written by slot.
                                    * (Ref_order_hint[i] for 5.9.2). */
    unsigned    dpb_next_slot;     /* Next slot to write, 8 slots turn */
    int         frame_seq;         /* Synthetic frame count, this frame = ++frame_seq */

    /* The reference translation and E recognition are all through it:
     * ref_frame_map gives a surface, and the same surface may belong to more than one.
     * The historical frame, the recently owned, is the one still alive.
     * New frame. Linear replacement of the oldest item on full time. */
    struct { VASurfaceID surf; int frame; } surf_hist[64];
    int         surf_hist_n;

    /* The previous frame refresh_frame_flags field ** bits within its frame ** offset.
     * The correct value is to be calculated by waiting for the next frame ref_frame_map, which is then rewrited on the ground.
     * SIZE_MAX indicates that the previous frame does not contain the field (KEY+show frame does not write). */
    size_t      last_refresh_bitpos;

    /* The last frame (the last_refresh_bitpos) is the order hit.
     * Catch to rewrite the real refresh and then register the frame into the shadow slot, order hit
     * It is the input of the decoder to skipModeAllowed (5.9.22), which must be recorded together. */
    unsigned    last_oh;

    /* Distortion of the same record.
     *
     * Why? Double-checking. First refresh=0 for pixels only. Second one for real.
     * Refresh refill DPB) will allow the ** frame reset of AV1 to occur twice.
     * Execute when the frame decodes
     *     if ( !disable_frame_end_update_cdf )
     *         SetFrameContext( ref_frame_idx[ primary_ref_frame ] )
     * Copy the frame's CDF/delta-q/ ring filtering terminal** into the Primariy reference frame
     * The slot context. The first time it was changed to the cost frame, the second time it was read again was the wrong starting point.
     * So the second time a piece of garbage was removed and placed in the DPB, and the subsequent quotes were all gone.
     *
     * Reverse: First time write this mandatory 1 (no reply) and second time restore to source stream value
     * (It performs the only rewriting). The starting context is the same for two trips — the second pixel and the second pixel.
     * The first time was the same, the DPB was right with the CDF.
     * SIZE_MAX indicates that this field is not in the stream (disable_cdf_update=1 extrapolated to 1, the number of the people who are not in the stream is 1 , the number of the people who are in the stream is 1 , the number of the people who are in the stream is 1 , the number of people who are in the stream
     * It's not written back, it's not done. */
    size_t      last_endupd_bitpos;

    /* The previous frame ref_frame_map snapshot, used to score the difference with the frame ' s map. */
    VASurfaceID prev_ref_map[8];
    int         prev_valid;

    /* - Replenishment of global motion parameters (regulation 5.9.24 / 7.11.3.6) -
     *
     * gm_params are ** differential **: the symbols of this frame are derived from parameters relative to the prism frame,
     * The prev used by the decoder is in its own DPB slot. If we use another button when we synthesize the flow,
     * Value encoding, the parameters of the hardware reconstructed differ from the source stream, and the motion compensation (and therefore the pixel) is wrong.
     * So you have to mirror one of the slots and update it in sync with dpb_shadow: which frame does the slot k store,
     * gm_slot[k] is the 7x6 parameter for that frame (** value after reconstruction**, i.e. wmmat for VA).
     *
     * Both tables are updated only when the frame really goes down:
     *   - Normal frame: pixel trip refresh=0 no slot, waiting for DPB trip (watch_prev_refresh)
     *     (b) Rewrite the register only after the real value has been calculated) and therefore add gm_pending suspense frame parameters;
     *   - KEY/All-out brush: Standard deductions are full, eight slots are written in build_frame.
     * Time sequence is consistent with hardware: EndPicture(k+1) paste (this frame chute) then synthesizes k+1,
     * Hardware is also collected first repair (k) and then pixel (k+1). */
    struct dmd_av1_gm { int32_t p[7][6]; } gm_slot[8];
    struct dmd_av1_gm gm_pending;      /* Arguments for the most recent synthetic frame */
    int               gm_pending_frame;/* Which frame does it belong to? 0 = none. */

    /* Source DPB kicked the frame (E_k- ⁇ map_k) when the previous frame was decoded,
     * In frame number. The source encoder guarantees that the kicked frame will not be cited in any subsequent frame.
     * (Death) This driver lets the current frame take its shadow slot, and it never covers live references.
     * The single slot difference is not counted (e.g., multiple slot changes after KEY has been fully brushed) and the value = 0 is not counted. */
    int         evict_frame;
    int         evict_valid;
};

/*
 * Refresh_frame_map-composition from the frame**
 * The bytes of the previous frame that have been synthesized.
 *
 * Rationale (as measured, 4 frames have all the source flow real values 1/8/32/64):
 *   Frame N refresh_frame_flags = variance between frame N+1 Map and frame N
 * Because "what slots are written in this frame" is just the next DPB snapshot.
 *
 * This is the only possible way to deduce. Four of the rejected (see note in .c for details):
 *   Find Current_frame in this frame -- this array is decoded**
 *   This frame map is split between the **frame map -- the wrong direction is given as the top frame refresh
 *   A constant 1 or 8 sink rotation 1 < <slot > - cannot express refresh=0 (not a slot frame),
 *      And this kind of frame is one third of the real sequence.
 *   Taking the first empty slot / unquoted slot - only first frame is checked
 *
 * The prev_frame_bytes point to the buffer where the upper bow is (caller saver)
 * The buffer must remain in effect at the time of this call and has not been sent to the decoder.
 */
/* Synthesizes a show_existing_frame frame OBU (regulation 5.9.2).
 *
 * Hardware does not export a frame (comply with the norm) of show_frame=0, but it is resurfaced for each SEF.
 * This is how the source stream is: out of 150 OBU_FRAME, only 80 show_frame=1, and the number of users is 80.
 * Another 70 frames are resurfaced by 70 SEFs, with 150 frames in total.
 * The actual source flow feed is " one hundred fifty for 150" for hardware, while the synthetic stream without SEF head is only 80 for.
 *
 * The structure is extremely simple (all SEFs in the actual source stream are 2 bytes):
 *     OBU Head 0x1a (type=3 FRAME_HEADER, has_size=1)
 *     leb128 size = 1
 *     Load 1 bytes: show_existing_frame(1)=1 + frame_to_show_map_idx(3)
 *                  + trailing_bits
 * Example: Map_idx=6  ⁇  load 0xe8; Map_idx=5 ⁇ 0xd8; Map_idx=3 ⁇ 0xb8.
 *
 * Buff at least 4 bytes. Returns writing length, failed to return 0. */
size_t dmd_av1_build_show_existing(unsigned char *buf, size_t cap,
                                   unsigned map_idx);

void dmd_av1_patch_prev_refresh(struct dmd_av1_dpb *dpb,
                                const void *cur_pic,
                                unsigned char *prev_frame_bytes,
                                size_t prev_len,
                                size_t prev_bitpos,
                                int prev_frame);

/* Whether this frame of reference is found in the shadow DPB (i.e. whether hardware can solve this frame).
 *
 * Why do you have to ask? The consumer has reached the "no frame" watch.
 * VA_INVALID_ID (this is the way to drag the progress bar to the GOP middle or when the stream is cut off)
 * Synthetic flows can only pass through the source slot, and hardware is then required to quote a DPB slot that has never been filled**.
 * The msm_vidc behavior of this frame is neither vomiting nor responding to CAPTURE buffers - actual measurements
 * The first frame is the inter stream: submit a module, retrieve 0 frames, SyncSurface etc. is 5 ss overtime,
 * The caller (ffmpeg) thus abandoned the entire bar stream (rc=251). Soft resolution dav1d's behavior was to throw away these.
 * Frame, next KEY from the stream continues to exit the frame. So the caller should return 0 when this function returns
 * ** No hardware submitted**, directs the surface to the shell, and the process is not broken.
 *
 * Only INTER and really read reference frames need to be judged: KEY/INTRA_ONLY/SWITCH does not read reference, but the only thing that is not available is that it is not possible to read the frame.
 * Allow_intrabc and reference_second=0 do not read, returns 1.
 * The decision only covers the slots actually used by the decoder (LAST and ALTREF, regulation 5.9.2),
 * The remaining slots are filled with values and point to empty slots without coding.
 *
 * Actually measured safe boundary: Health stream (1,800 frames before 300 frames, 2100 references) returns 0
 * The number of times is **0**, so there is no risk of any injury to the normal broadcast. */
int dmd_av1_refs_resolvable(const void *pic, struct dmd_av1_dpb *dpb);

/* A text description of the position and length of a Tile to assemble tile_group. */
struct dmd_av1_tile {
    const unsigned char *data;
    size_t               len;
};

/* Synthesizes a complete OBU_FRAME(6): frame + byte_alignment + tile_group.
 *
 * Why do you use OBU_FRAME instead of the separated FAME_HEADER(3) + TILLE_GRUP(4):
 * "Failed to read unit 0 (type 3)", and merged.
 * Directly through. The real bitstream generated by libaom is also used by OBU_FRAME.
 *
 * ⁇  Key difference: at the end of the frame in OBU_FRAME, **byte_alignment**,
 * Instead of traying_bits (writing 1 and then zeroing) - Regulation 5.10.1 frame_obu()
 * The error allows the tile_group to follow the text of the video.
 * That's how you mistook the initial position.
 *
 * tiles in the order of tile_row-major must be equal to tile_cols*tile_rows.
 * Write out, returns the total bytes; the capacity is insufficient or the parameter is returned illegally. */
size_t dmd_av1_build_frame(const void *pic,
                           const struct dmd_av1_tile *tiles, int num_tiles,
                           unsigned char *out, size_t out_cap,
                           struct dmd_av1_dpb *dpb);

/* - - - - - - - - - - - - - - - - - - Frame synthesis (3/4) */

/* Combine a complete OBU_FRAME_HEADER from VADecPictureParameter BufferAV1
 * (includes OBU headers + payload + trading_bits).
 *
 * Tile_cols/ tile_rows from pic parameters; tile_size_bytes fix 4 bytes,
 * The width of the tile_size_minus_1 written in step 4 tile_group must be consistent.
 *
 * Write out, returns the total bytes; the capacity is insufficient or the parameter is returned illegally. */
size_t dmd_av1_build_frame_header(const void *pic,
                                  unsigned char *out, size_t out_cap,
                                  struct dmd_av1_dpb *dpb);

/* obu_header() + obu_size (AV1 Reg. 5.3.1/5.3.2).
 *
 * Bit layout (1 bytes without extension):
 *   obu_forbidden_bit f(1) must 0
 *   obu_type             f(4)
 *   obu_extension_flag f(1) this driver is constant 0 (no scalable layer)
 *   obu_has_size_field f(1) this driver is constant1 (low-overhead format needs)
 *   obu_reserved_1bit f(1) must 0
 *
 * Write out (first 1 byte + payload_len) coded to re-enter total byte numbers;
 * Out_cap is not enough to return 0. The caller then adds the payload to the returned position. */
size_t dmd_av1_obu_header(int obu_type, size_t payload_len,
                          unsigned char *out, size_t out_cap);

#endif
