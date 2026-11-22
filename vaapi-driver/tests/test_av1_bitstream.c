/* AV1 Bits in the original language.
 *
 * Why is it worth taking alone: these code rules are wrong, and the entire bar stream is completely misplaced from there.
 * And the symptoms are a vague "failed to read obu" -- not from the phenomenon.
 * Leb128 writes a byte less or n(n) bitwidth error. So before the access is really synthesized,
 * The known values in the norms are used to nail each original word first.
 *
 * Source of expected values: AV1 Regulation 4.10 (leb128/uvlc/le/su/ns) and 5.3 (obu_header).
 * The boundary value of the leb128 is also cross-checked with the protobuf definition of varint (the same code).
 *
 * Independent compile, unchained libva:
 *   gcc -o test_av1 tests/test_av1_bitstream.c src/av1_bitstream.c src/bitstream.c
 */
#include <stdio.h>
#include <stdlib.h>      /* Setenv/unsetenv: Tox DMD_AV1_NO_SHOWORCE */
#include <string.h>

#include <va/va.h>
#include <va/va_dec_av1.h>

#include "../src/av1_bitstream.h"

static int fails;

static void check_bytes(const char *what, const unsigned char *got, size_t got_n,
                        const unsigned char *want, size_t want_n)
{
    if (got_n == want_n && memcmp(got, want, want_n) == 0)
        return;
    fails++;
    printf("  ✗ %s\n    - Yeah. (%zu bytes):", what, got_n);
    for (size_t i = 0; i < got_n; i++) printf(" %02x", got[i]);
    printf("\n    Expectations (%zu bytes):", want_n);
    for (size_t i = 0; i < want_n; i++) printf(" %02x", want[i]);
    printf("\n");
}

static void check_eq(const char *what, long got, long want)
{
    if (got == want)
        return;
    fails++;
    printf("  ✗ %s: - Yeah. %ld,Expectations %ld\n", what, got, want);
}

/* ---------------------------------------------------------------- leb128 */

static void test_leb128(void)
{
    printf("leb128(Norms 4.10.5)\n");
    struct { uint64_t v; size_t n; unsigned char want[4]; } tv[] = {
        { 0,     1, { 0x00 } },
        { 1,     1, { 0x01 } },
        { 127,   1, { 0x7f } },          /* Word Top */
        { 128,   2, { 0x80, 0x01 } },    /* Borders in point */
        { 255,   2, { 0xff, 0x01 } },
        { 256,   2, { 0x80, 0x02 } },
        { 16383, 2, { 0xff, 0x7f } },    /* Double Byte Top */
        { 16384, 3, { 0x80, 0x80, 0x01 } },
    };
    for (size_t i = 0; i < sizeof(tv) / sizeof(tv[0]); i++) {
        unsigned char out[8];
        size_t n = dmd_av1_leb128(tv[i].v, out, sizeof(out));
        char msg[64];
        snprintf(msg, sizeof(msg), "leb128(%llu)", (unsigned long long)tv[i].v);
        check_bytes(msg, out, n, tv[i].want, tv[i].n);

        /* The length estimate must be consistent with actual writing -- obu_size must be calculated for length before payload. */
        snprintf(msg, sizeof(msg), "leb128_len(%llu)", (unsigned long long)tv[i].v);
        check_eq(msg, (long)dmd_av1_leb128_len(tv[i].v), (long)tv[i].n);
    }

    /* The capacity deficit must be returned to 0 and cannot be written across borders. */
    unsigned char tiny[1];
    check_eq("leb128 Unlimited capacity returns 0", (long)dmd_av1_leb128(128, tiny, 1), 0);
}

/* ------------------------------------------------------------- obu_header */

static void test_obu_header(void)
{
    printf("obu_header(Norms 5.3.1/5.3.2)\n");

    /* forbidden(1)=0 type(4) ext(1)=0 has_size(1)=1 reserved(1)=0 */
    struct { int type; unsigned char first; const char *name; } tv[] = {
        { DMD_OBU_SEQUENCE_HEADER,    0x0a, "SEQUENCE_HEADER" },
        { DMD_OBU_TEMPORAL_DELIMITER, 0x12, "TEMPORAL_DELIMITER" },
        { DMD_OBU_FRAME_HEADER,       0x1a, "FRAME_HEADER" },
        { DMD_OBU_TILE_GROUP,         0x22, "TILE_GROUP" },
        { DMD_OBU_FRAME,              0x32, "FRAME" },
    };
    for (size_t i = 0; i < sizeof(tv) / sizeof(tv[0]); i++) {
        unsigned char out[8];
        size_t n = dmd_av1_obu_header(tv[i].type, 10, out, sizeof(out));
        char msg[80];
        snprintf(msg, sizeof(msg), "%s First bytes", tv[i].name);
        check_eq(msg, n, 2);                 /* 1 Head + 1 leb 128 (10) */
        check_eq(msg, out[0], tv[i].first);
        snprintf(msg, sizeof(msg), "%s obu_size", tv[i].name);
        check_eq(msg, out[1], 10);
    }

    /* Big payload: obu_size becomes 2 bytes. */
    unsigned char out[8];
    size_t n = dmd_av1_obu_header(DMD_OBU_TILE_GROUP, 200, out, sizeof(out));
    check_eq("TILE_GROUP payload=200 Chief", (long)n, 3);
    check_eq("  First bytes", out[0], 0x22);
    check_eq("  size[0]", out[1], 0xc8);
    check_eq("  size[1]", out[2], 0x01);

    /* Inverse confirmation of illegal value: 0xd0 forbidden is 1, type is 10.
     * The effect of this assertion is to solidise the "Why Naked Loads Not OBU" into enforceable evidence. */
    unsigned char bad = 0xd0;
    check_eq("0xd0 of forbidden_bit(Illegal)", (bad >> 7) & 1, 1);
    check_eq("0xd0 of obu_type(Reserve value)",    (bad >> 3) & 0x0f, 10);
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - Alignment */

static void test_align(void)
{
    printf("byte_alignment / trailing_bits(Norms 5.3.4/5.3.5)\n");

    /* byte_align writes zero padding with no stop bit. Writing 101 and aligning produces 1010_0000. */
    unsigned char buf[4];
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_bw_put_bits(&bw, 0x5, 3);            /* 101 */
    dmd_av1_byte_align(&bw);
    check_eq("byte_align Number of the after-bytes", (long)dmd_bw_bytes(&bw), 1);
    check_eq("byte_align result",     buf[0], 0xa0);

    /* Trailing_bits: Write 1 then zero. Same as 101 § 1011_000 */
    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_bw_put_bits(&bw, 0x5, 3);
    dmd_av1_trailing_bits(&bw);
    check_eq("trailing_bits Number of the after-bytes", (long)dmd_bw_bytes(&bw), 1);
    check_eq("trailing_bits result",     buf[0], 0xb0);

    /* The one that is still written when the time is set (it is the end mark, not the fill). */
    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_bw_put_bits(&bw, 0xff, 8);
    dmd_av1_trailing_bits(&bw);
    check_eq("Marker is still aligned", (long)dmd_bw_bytes(&bw), 2);
    check_eq("  Tag Bytes",       buf[1], 0x80);
}

/* -------------------------------------------------------------------- le */

static void test_le(void)
{
    printf("le(n)(Norms 4.10.4,Hey, Taki.)\n");
    unsigned char buf[8];
    struct dmd_bitwriter bw;

    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_le(&bw, 0x1234, 2);
    check_eq("le(2) Bytes", (long)dmd_bw_bytes(&bw), 2);
    check_eq("le(2)[0] Down ahead.", buf[0], 0x34);
    check_eq("le(2)[1]",          buf[1], 0x12);

    /* The call when the bytes are not aligned is an error in usage and must be overflowed instead of writing the wrong bit. */
    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_bw_put_flag(&bw, 1);
    dmd_av1_put_le(&bw, 0xff, 1);
    check_eq("le Unsettled Time overflow", bw.overflow, 1);
}

/* ------------------------------------------------------------------ uvlc */

static void test_uvlc(void)
{
    printf("uvlc(Norms 4.10.3)\n");
    /* v=0 → "1"；v=1 → "010"；v=2 → "011"；v=3 → "00100" */
    struct { uint32_t v; const char *bits; } tv[] = {
        { 0, "1" }, { 1, "010" }, { 2, "011" }, { 3, "00100" }, { 6, "00111" },
    };
    for (size_t i = 0; i < sizeof(tv) / sizeof(tv[0]); i++) {
        unsigned char buf[8];
        struct dmd_bitwriter bw;
        dmd_bw_init(&bw, buf, sizeof(buf));
        dmd_av1_put_uvlc(&bw, tv[i].v);

        /* Read the written bit back to the string matching, avoids the bytes. */
        size_t nbits = strlen(tv[i].bits);
        char got[40] = {0};
        for (size_t b = 0; b < nbits; b++)
            got[b] = ((buf[b / 8] >> (7 - b % 8)) & 1) ? '1' : '0';

        if (strcmp(got, tv[i].bits) != 0) {
            fails++;
            printf("  ✗ uvlc(%u): - Yeah. %s,Expectations %s\n", tv[i].v, got, tv[i].bits);
        }
    }
}

/* -------------------------------------------------------------------- ns */

static void test_ns(void)
{
    printf("ns(n)(Norms 4.10.7)\n");
    /* n = 1 is not available (only value). */
    unsigned char buf[8];
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_ns(&bw, 0, 1);
    check_eq("ns(v=0,n=1) Do Not Place", (long)bw.bit_pos, 0);

    /* n=3:w=2, m=1. v=0 = 1 "0"; v= 1 = 2 "10"; v= 2 = 2 "11" */
    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_ns(&bw, 0, 3);
    check_eq("ns(0,3) Digit", (long)bw.bit_pos, 1);
    check_eq("ns(0,3) Value",   (buf[0] >> 7) & 1, 0);

    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_ns(&bw, 2, 3);
    check_eq("ns(2,3) Digit", (long)bw.bit_pos, 2);
    check_eq("ns(2,3) Value",   (buf[0] >> 6) & 3, 3);
}

/* -------------------------------------------------------------------- su */

static void test_su(void)
{
    printf("su(n)(Norms 4.10.6,Compend)\n");
    unsigned char buf[8];
    struct dmd_bitwriter bw;

    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_su(&bw, -1, 4);
    check_eq("su(-1,4) = 0b1111", (buf[0] >> 4) & 0xf, 0xf);

    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_su(&bw, 3, 4);
    check_eq("su(3,4) = 0b0011", (buf[0] >> 4) & 0xf, 0x3);

    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_su(&bw, -8, 4);
    check_eq("su(-8,4) = 0b1000", (buf[0] >> 4) & 0xf, 0x8);
}

/* - - - - - - - - - - - - - - - - - - - - - - - - Sequence head synthesis */

static void test_sequence_header(void)
{
    printf("OBU_SEQUENCE_HEADER Synthetic (normative) 5.5.1)\n");

    /* 1080p / 8bit / 4:2 / profile 0, corresponding to properties of the actual bitstream av1_1080p.obu. */
    VADecPictureParameterBufferAV1 p;
    memset(&p, 0, sizeof(p));
    p.profile                 = 0;
    p.bit_depth_idx           = 0;
    p.matrix_coefficients     = 1;              /* BT.709 */
    p.frame_width_minus1      = 1920 - 1;
    p.frame_height_minus1     = 1080 - 1;
    p.order_hint_bits_minus_1 = 6;
    p.seq_info_fields.fields.use_128x128_superblock = 1;
    p.seq_info_fields.fields.enable_order_hint      = 1;
    p.seq_info_fields.fields.enable_cdef            = 1;
    p.seq_info_fields.fields.subsampling_x          = 1;
    p.seq_info_fields.fields.subsampling_y          = 1;

    unsigned char buf[256];
    size_t n = dmd_av1_build_sequence_header(&p, buf, sizeof(buf));

    check_eq("Synthetic success (returning to non-false) 0)", n > 0, 1);
    check_eq("First bytes = 0x0a(SEQ_HDR, has_size=1)", buf[0], 0x0a);
    check_eq("obu_size As compared to actual payload Unanimously.", buf[1], (long)(n - 2));

    /* The profile is 3 bit higher in the first payload byte (f(3) is the first written field). */
    check_eq("payload Start 3 bits = seq_profile", (buf[2] >> 5) & 0x7, 0);

    /* The capacity deficit must return to 0 instead of cross-border writing. */
    unsigned char tiny[4];
    check_eq("Unlimited capacity returns 0",
             (long)dmd_av1_build_sequence_header(&p, tiny, sizeof(tiny)), 0);
    check_eq("NULL Parameter Back 0",
             (long)dmd_av1_build_sequence_header(NULL, buf, sizeof(buf)), 0);

    /* External cross-checking of records (this test does not link external tools, leaving evidence only):
     * The above parameters synthesize 16 bytes `0a 0e 00 00 00 05 57 7f 86 ff c8 81 01 00 82 `,
     * ffmpeg CBS layer resolved
     *   "obu_type: 1, payload size: 14"
     *   "Video: av1 (Main), none(tv, bt709/unknown/unknown)"
     * The three are actually being interpreted. */
}


/* - - - - - - - - - - - - - - - - - - - - - SEF Synthesis
 *
 * Why is it worth checking: the show_existing_frame unit has only 4 bit payloads, but it's
 * The byte layouts are repeated on the 65~68 round (when the code change was tested and no benchmarks were available).
 * Here, as per the norm, it is nailed to the ground by 5.9.2 + 5.3.4.
 *
 * The expectation is derived (regulation 5.3.2 obu_header + 5.9.2 frame_header + 5.3.4 trading):
 *   OBU Head = forbidden (1) = 0  ⁇  type(4) = OBU_FRAME_HEADER=3  ⁇ ext(1) = 0
 *            | has_size(1)=1 | reserved(1)=0
 *          = 0<<7 | 3<<3 | 0<<2 | 1<<1 | 0 = 0x1a
 *   obu_size = 1 (load one byte)
 *   Load = show_existing_frame(1)=1, frame_to_show_map_idx(3)=idx,
 *          Trailing: Stop bit 1 + Zero Filling to Byte Boundary
 *        = 1 idx idx idx 1 0 0 0
 * idx=0 → 0b1000_1000 = 0x88；idx=5 → 0b1101_1000 = 0xd8
 * idx=7 → 0b1111_1000 = 0xf8 */
static void test_show_existing(void)
{
    printf("show_existing_frame Synthetic (normative) 5.9.2)\n");
    unsigned char buf[8];

    static const unsigned char want0[] = { 0x1a, 0x01, 0x88 };
    size_t n = dmd_av1_build_show_existing(buf, sizeof(buf), 0);
    check_bytes("SEF map_idx=0", buf, n, want0, sizeof(want0));

    static const unsigned char want5[] = { 0x1a, 0x01, 0xd8 };
    n = dmd_av1_build_show_existing(buf, sizeof(buf), 5);
    check_bytes("SEF map_idx=5", buf, n, want5, sizeof(want5));

    static const unsigned char want7[] = { 0x1a, 0x01, 0xf8 };
    n = dmd_av1_build_show_existing(buf, sizeof(buf), 7);
    check_bytes("SEF map_idx=7", buf, n, want7, sizeof(want7));

    /* Map_idx only has 3 bits, the height must be cut and not spilled over to the head of OBU. */
    n = dmd_av1_build_show_existing(buf, sizeof(buf), 13);   /* 13 & 7 == 5 */
    check_bytes("SEF map_idx=13 Equivalent 5", buf, n, want5, sizeof(want5));

    /* The capacity deficit must return to 0 instead of writing bad memory. */
    check_eq("SEF cap=3 Reject", (long)dmd_av1_build_show_existing(buf, 3, 0), 0);
    check_eq("SEF cap=0 Reject", (long)dmd_av1_build_show_existing(buf, 0, 0), 0);
    check_eq("SEF buf=NULL Reject",
             (long)dmd_av1_build_show_existing(NULL, 8, 0), 0);
}

/* --------------- refresh_frame_flags rewrite
 *
 * Why is it worth checking: this is the only **status rewording in the AV1 path, which is a silent contamination?
 * The frame is already written. The 79th round found "Fifresh 5 is not worth" out of this chain.
 * And there was no single measurement of the ability to locate whether the difference was wrong or whether the position was wrong.
 *
 * Here only the most basic non-variable (not dependent on real stream):
 *   1. dpb is NULL or cur_pic is NULL indestructible and non-rewable
 *   2. First call (prev_valid=0) is not rewritten, but must save Map and prev_valid
 *      - That's the root of the 76th wheel of the sequence -- 8, 32,64th down.
 *   Write the specified bitpos at ref_frame_map when second call
 *
 * The last true reference is prev_frame (the frame number that was rewritten) for the shadow DPI registry only.
 * This example asserts a cipher bytes with a Map benchmark, and fax 0 means "no shadow registered". */
static void test_patch_prev_refresh(void)
{
    printf("refresh_frame_flags Rewrite As-Signature\n");
    struct dmd_av1_dpb dpb;
    VADecPictureParameterBufferAV1 pic;
    unsigned char frame[4];

    /* 1. NULL Protection */
    memset(&dpb, 0, sizeof(dpb));
    memset(&pic, 0, sizeof(pic));
    dmd_av1_patch_prev_refresh(NULL, &pic, frame, sizeof(frame), 0, 0);
    dmd_av1_patch_prev_refresh(&dpb, NULL, frame, sizeof(frame), 0, 0);
    check_eq("NULL Parameter is not set prev_valid", (long)dpb.prev_valid, 0);

    /* First call: no rewrite, but save Map and prev_valid */
    memset(&dpb, 0, sizeof(dpb));
    memset(&pic, 0, sizeof(pic));
    for (int i = 0; i < 8; i++) pic.ref_frame_map[i] = 0xff;
    memset(frame, 0, sizeof(frame));
    dmd_av1_patch_prev_refresh(&dpb, &pic, frame, sizeof(frame), 0, 0);
    check_eq("First Call prev_valid", (long)dpb.prev_valid, 1);
    check_eq("First call without rewrite stream", (long)frame[0], 0);
    check_eq("First Call Save map", (long)dpb.prev_ref_map[0], 0xff);

    /* 3. On the second call, slots 0 and 3 change. The mask is 0b00001001 = 0x09.
     *    Bitpos=0 means eight bits from the top of the frame [0]. */
    pic.ref_frame_map[0] = 0x11;   /* It's changed. */
    pic.ref_frame_map[3] = 0x22;   /* It's changed. */
    memset(frame, 0, sizeof(frame));
    dmd_av1_patch_prev_refresh(&dpb, &pic, frame, sizeof(frame), 0, 0);
    check_eq("Offset Rewrite mask Slot 0|Slot 3", (long)frame[0], 0x09);

    /* 4. Bitpos is also to be correctly aligned in non-byte: bitpos=4  ⁇  cross frame[0]/frame[1].
     *    mask 0x09 = 00000_1001, starting with 4th place:
     *      frame[0] low 4 bit = high 4 bit mask = 00000
     *      frame[1] 4 bit high = mask = 1001  ⁇  0x90 low */
    memset(&dpb, 0, sizeof(dpb));
    memset(&pic, 0, sizeof(pic));
    for (int i = 0; i < 8; i++) pic.ref_frame_map[i] = 0xff;
    dmd_av1_patch_prev_refresh(&dpb, &pic, frame, sizeof(frame), 4, 0);
    pic.ref_frame_map[0] = 0x11;
    pic.ref_frame_map[3] = 0x22;
    memset(frame, 0, sizeof(frame));
    dmd_av1_patch_prev_refresh(&dpb, &pic, frame, sizeof(frame), 4, 0);
    check_eq("bitpos=4 Low Halfbyte", (long)(frame[0] & 0x0f), 0x00);
    check_eq("bitpos=4 Half bytes taller", (long)(frame[1] & 0xf0), 0x90);
}


/* - -----------------------------------------------------
 *
 * dmd_av1_refs_resolve() is the gate "decomposition from the GOP": the hardware is quoted in the DPB
 * There's no frame stream, and Venus is neither out of frame nor wrong, and will drag the vasyncSurface to time.
 * The caller then abandons the entire bar stream (rec=251, 0 frame). So the wrong direction is at the cost:
 *   The video is a good one, but it should be released and stopped.
 *   The stop is put down and the whole bar code is gone.
 * This is the case of nailing each side. */
static void test_refs_resolvable(void)
{
    printf("Reference frame synthesizing decision (from GOP The middle exit.)\n");
    struct dmd_av1_dpb dpb;
    VADecPictureParameterBufferAV1 pic;

    /* Commitment: Shadow Slot 2 Frame number 7, Frame 7 belongs to the surface 100.
     * The surf_hist is a static interface, which is filled out directly in its semantic form in the test. */
#define SETUP()   do {                                              \
        memset(&dpb, 0, sizeof(dpb));                               \
        memset(&pic, 0, sizeof(pic));                               \
        dpb.dpb_shadow[2] = 7;                                      \
        dpb.surf_hist[0].surf = 100; dpb.surf_hist[0].frame = 7;    \
        dpb.surf_hist_n = 1;                                        \
        dpb.frame_seq = 7;                                          \
        for (int i = 0; i < 8; i++)                                 \
            pic.ref_frame_map[i] = (VASurfaceID)100;                \
        pic.ref_frame_idx[0] = 2;   /* LAST in slot 2 */               \
        pic.ref_frame_idx[3] = 2;   /* ALTREF is in the slot two. */           \
    } while (0)

    /* 1. Frames, references found */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;   /* INTER_FRAME */
    check_eq("Quote fully-referenced frames to release", (long)dmd_av1_refs_resolvable(&pic, &dpb), 1);

    /* KEY/INTRA_ONLY/SWITCH does not need to be  ⁇  released,
     *    Even if the map is full of INVALID. */
    SETUP();
    for (int ft = 0; ft <= 3; ft++) {
        if (ft == 1)
            continue;
        memset(pic.ref_frame_map, 0xff, sizeof(pic.ref_frame_map));
        pic.pic_info_fields.bits.frame_type = (unsigned)ft;
        check_eq("Non-frame frame release", (long)dmd_av1_refs_resolvable(&pic, &dpb), 1);
    }

    /* Allow_intrabc frames do not quote any frames  ⁇  Release */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;
    pic.pic_info_fields.bits.allow_intrabc = 1;
    memset(pic.ref_frame_map, 0xff, sizeof(pic.ref_frame_map));
    check_eq("intrabc Frame Release", (long)dmd_av1_refs_resolvable(&pic, &dpb), 1);

    /* LAST points to a never decoded surface (VA_INVALID_ID) stop.
     *    This is the first frame of the "draw to the middle of the GOP": ref_frame_map
     *    Those slots are 4294967295, the hardware DPB counters are empty. */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;
    pic.ref_frame_map[2] = 0xffffffffu;
    check_eq("reject missing LAST reference", (long)dmd_av1_refs_resolvable(&pic, &dpb), 0);

    /* 5. LAST is capable of dissecting, Altraf (shelter 5) is unable to decipher, and is not stopped.
     *    The composite reference and the skip_mode really use the slots that can't be cited as unsettling. */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;
    pic.ref_frame_idx[3] = 5;
    pic.ref_frame_map[5] = 0xffffffffu;    /* There's no frame in that slot. */
    check_eq("reject missing ALTREF reference", (long)dmd_av1_refs_resolvable(&pic, &dpb), 0);

    /* 6.  ⁇ Reference_select=0** ** Can't ** be used as a "no quote from this frame" release condition.
     *    Standard 5.9.2: It's 0, but it's not visible in the frame to seven slots, and the frame refers to LAST.
     *    The first edition of the "ref_frame_idx[0]" was added to the pre-release, and the stop was stopped.
     *    The log still shows "Synthetic 2957 bytes", which is the same rc=251 card dead.
     *    Both directions are nailed: + reference_select=0. */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;
    pic.mode_control_fields.bits.reference_select = 0;
    pic.ref_frame_map[2] = 0xffffffffu;
    check_eq("reference_select=0 And when the quote is bad, stop it.",
             (long)dmd_av1_refs_resolvable(&pic, &dpb), 0);
    /* +Reference_select=0  ⁇  (a lot of frames are on normal play.)
     * The PIMEC: 43 frames dropped. */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;
    pic.mode_control_fields.bits.reference_select = 0;
    check_eq("reference_select=0 and quote full-time release",
             (long)dmd_av1_refs_resolvable(&pic, &dpb), 1);

    /* 7. The searchface repeater: ffmpeg recovers the searchface after the same number belongs to the new frame.
     *    The suf_hist 100 is already owned by frame 9, and the shadow watch only contains seven  ⁇  frame nine.
     *    # Stop. This is the wrong way to "get the surface when you're an identity." */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;
    dpb.surf_hist[0].frame = 9;
    check_eq("surface Intercept when available", (long)dmd_av1_refs_resolvable(&pic, &dpb), 0);

    /* NULL protection: Disable from judgement without pic (to back tile);
     *    The "no explanation" is given without dpb. The real call is from the permanent location of the context
     *    Members, never NULL, are simply semantic, and not later. */
    SETUP();
    check_eq("pic Yes. NULL Time free", (long)dmd_av1_refs_resolvable(NULL, &dpb), 1);
    pic.pic_info_fields.bits.frame_type = 1;
    check_eq("dpb Yes. NULL Unsettled by Resolution",
             (long)dmd_av1_refs_resolvable(&pic, NULL), 0);
#undef SETUP
}


/* The load is written by bit, and the assertion is read by bit.
 * bit_at: idx (idx=0 is the highest of buff [0]), cross-border return -1. */
static long bit_at(const unsigned char *buf, size_t n, size_t idx)
{
    if (idx >= n * 8)
        return -1;
    return (long)((buf[idx / 8] >> (7 - (idx % 8))) & 1u);
}

/* Last subscript for 1; trading_bits() (regulation 5.3.4) at the end of Payload
 * Write a one and then add zero to byte boundary, so this subscript +1 is the **digit of payload**.
 * Use it to compare length to size to size -- this is the key observation of this case.
 * returns -1 at zero. */
static long last_set_bit(const unsigned char *buf, size_t n)
{
    for (size_t i = n * 8; i-- > 0; )
        if (bit_at(buf, n, i) == 1)
            return (long)i;
    return -1;
}


/* - - - - - - - - - - - - - Show_frame bits of frame_header
 *
 * Why do you take these two alone: the driver turns the show_frame ** constant 1** in the synthetic head?
 * Landed) - Hardware only throws up CAPTURE buffers for show_frame=1, while ffmpeg backends
 * It'll be pixels for every frame-up surface.
 * And regulation 5.9.2 provides that the show_frame** only appears when show_frame=0:
 *     frame_type          f(2)
 *     show_frame          f(1)
 *     if (!show_frame) showable_frame  f(1)
 * That is, rewrite show_frame** changes the position of all subsequent fields** —
 * Turn only the one who did not delete the showable_frame, and the whole frame is now a different one.
 * and refresh_frame_flags (bitpos for patch_prev_refresh)
 * That path will also stop working.
 *
 * So this example is divided into two paragraphs: first, to regulate it under the A/B Switch (DMD_AV1_NO_SHOWORCE)
 * The conditions were crucified (one more, one more right, two more values).
 * Verify that the default path does synthesize two PPBs by bytes (compulsory effect).
 *
 * ⁇  This test only verifies structural nonvariancies (digital, location-specific, location-specific)
 * Do not construct a complete frame of reference - that takes a set of VA parameters and DPB status,
 * End-to-end matching returns overlayed by the sample of verefy_driver.sh and 14. */
static void test_frame_header_show_frame(void)
{
    printf("frame_header of show_frame / showable_frame Networking (normality) 5.9.2)\n");

    /* Driver Defaults Show_frame ** Always 1** (two-fold foundation: hardware only frames for show=1
     * The showable_frame branch that this example is about to measure is not available.
     * The Syntax is a key to the process of re-establishing the standard branch of the A/B switch and then to assert the grammar.
     * And then the "default forced 1" act itself is stuck at the end. */
    setenv("DMD_AV1_NO_SHOWFORCE", "1", 1);

    unsigned char buf_show[512], buf_noshow[512];
    struct dmd_av1_dpb dpb;
    VADecPictureParameterBufferAV1 pic;

    /* Constructes a minimum available frame parameter. Only the previous layouts are concerned.
     * The following field is available at 0 -- two calls are only short of show_frame, the rest being identical. */
    memset(&pic, 0, sizeof(pic));
    pic.frame_width_minus1  = 1919;
    pic.frame_height_minus1 = 1079;
    pic.profile = 0;
    pic.order_hint = 4;
    pic.pic_info_fields.bits.frame_type = 1;        /* INTER_FRAME */
    pic.pic_info_fields.bits.showable_frame = 1;

    pic.pic_info_fields.bits.show_frame = 1;
    memset(&dpb, 0, sizeof(dpb));
    size_t n_show = dmd_av1_build_frame_header(&pic, buf_show,
                                              sizeof(buf_show), &dpb);

    pic.pic_info_fields.bits.show_frame = 0;
    memset(&dpb, 0, sizeof(dpb));
    size_t n_noshow = dmd_av1_build_frame_header(&pic, buf_noshow,
                                                 sizeof(buf_noshow), &dpb);

    if (n_show == 0 || n_noshow == 0) {
        /* The argument for synthesis is not full-time -- that's why this test is meaningless.
         * It is clearly reported, not silently adopted (passing is the worst result). */
        fails++;
        printf("  ✗ Frame Synthesis Return 0(show=%zu noshow=%zu),"
               "The test failed to cover the target\n", n_show, n_noshow);
        return;
    }

    /* ⁇  The assertion must be verifiable and cannot be used as a byte number.
     *
     * The first edition reads `n_noshow=n_show+1 ' with
     * "First difference in 1 byte." Both are based on "this time, it just crossed the byte boundary":
     * The frame ends with a trailing_bits((regulation 5.3.4:a) one to the border)
     * The need for one more byte depends on how many are already ahead of that one.
     * The results of the survey changed between 11/12 and 12/12 - claiming that it was wrong.
     * Once it is not established, a correct realization is judged to fail.
     *
     * Standard 5.9.2 The real nonvariable is: show_frame=0 more than one showable_frame,
     * So show=0 payload ** more than show=1 ** and 5th from the whole to the right.
     * Digits to see: Payload, end mark (the tracking_bits 1) is in the buffer zone
     * The last bit of 1 is left with nothing to fill. So the "Last One under" is
     * The number of places in the payload is not related to bytes. */
    const size_t PAYLOAD = 2;           /* OBU header 1 byte + obu_size 1 byte */
    const long stop_s = (long)last_set_bit(buf_show, n_show);
    const long stop_t = (long)last_set_bit(buf_noshow, n_noshow);
    check_eq("show=0 of payload It’s like... show=1 One more.",
             stop_t, stop_s + 1);

    /* Show_frame that person after the frame_type, the load is 3rd place (from the highest number).
     *
     * Bit layout (frame frame, non-KEY) of load 0 bytes (= buff [2]):
     *     show_existing_frame f(1)=0
     *     frame_type          f(2)=1  (INTER)
     *     show_frame          f(1)
     *     [! Show_frame]
     *     ...
     * So show_frame is bit 4 (mask 0x10) in byte, showable_frame is bit 3.
     * Observed: show=1 gives 0x30 = 0011_0000. show=0 gives 0x28 = 0010_1000. */
    if (n_show > PAYLOAD && n_noshow > PAYLOAD) {
        check_eq("show_frame bit (load) bit4)Yes. show=1 Time 1",
                 (long)((buf_show[PAYLOAD] >> 4) & 1u), 1);
        check_eq("show_frame bit (load) bit4)Yes. show=0 Time 0",
                 (long)((buf_noshow[PAYLOAD] >> 4) & 1u), 0);
        check_eq("showable_frame bit (load) bit3)Yes. show=0 Time 1",
                 (long)((buf_noshow[PAYLOAD] >> 3) & 1u), 1);
    }

    /* Shift right one: all places after the showable_frame, show=0 and show=1 must be staggered.
     * The one that caught the "only wrote more than one bit but the following field did not move" kind of mistake. */
    {
        size_t mism = 0, checked = 0;
        for (long i = (long)PAYLOAD * 8 + 5; i <= stop_s + 1; i++) {
            long a = bit_at(buf_noshow, n_noshow, (size_t)i);
            long b = bit_at(buf_show, n_show, (size_t)(i - 1));
            if (a < 0 || b < 0)
                break;
            checked++;
            if (a != b)
                mism++;
        }
        if (checked < 16) {
            fails++;
            printf("  ✗ Right-hand check is only over. %zu Bits, too few. The test failed to cover the target.\n",
                   checked);
        } else if (mism != 0) {
            fails++;
            printf("  ✗ show=0 No one to follow.:%zu/%zu Bits Unlike\n",
                   mism, checked);
        }
    }

    /* - - **Pick the default **in itself: constant show_frame=1 -
     * The whole section above is validated by the branch of the norm at the A/B switch; the real run is another:
     * Driver writes show_frame as 1 (hardware only for show=1 vomiting CAPTURE buffer, and then you can use the current system to create a new one.
     * Two-fold foundation. So two PPBs must produce a frame with the same bytes.
     * Whoever forced this off one day, 896 frames returned here immediately to blow up first. */
    unsetenv("DMD_AV1_NO_SHOWFORCE");

    pic.pic_info_fields.bits.show_frame = 1;
    memset(&dpb, 0, sizeof(dpb));
    size_t f_show = dmd_av1_build_frame_header(&pic, buf_show,
                                              sizeof(buf_show), &dpb);
    pic.pic_info_fields.bits.show_frame = 0;
    memset(&dpb, 0, sizeof(dpb));
    size_t f_noshow = dmd_av1_build_frame_header(&pic, buf_noshow,
                                                 sizeof(buf_noshow), &dpb);
    check_eq("Default:show=0 and show=1 Frame Head (compulsoryly) 1)",
             (long)f_noshow, (long)f_show);
    if (f_show > 0 && f_show == f_noshow) {
        check_eq("Default: Two PPB Frame by bytes same",
                 (long)memcmp(buf_show, buf_noshow, f_show), 0);
        check_eq("Default: Load bit4 Always 1",
                 (long)((buf_noshow[PAYLOAD] >> 4) & 1u), 1);
    }
}



/* - - - - - - - - - - - - - - - - - - - offset_frame_flags
 *
 * Background: AV1 Residual defect with 5 frames (page 30/60/90/120/150 of the 150 frame sample)
 * The mapping data are:
 *     70 frames last_refresh_bitpos=50, 5 frames = 58, 8-bit**
 * And that's what this bitpos is, just to write it on another field.
 *
 * This test has nailed the factors that affected the deviation and recorded the exclusions:
 *   The group of three, the first of which is the "Foster of the "F" and the second of the "Foster of the Red Cross" (Foster of the Red Cross) and the second of the "Foster of the Red Cross" (Foster of the Red Cross) (Foster of the Red Cross
 *   The maximum margin is only 3 bits (width of prism_ref_frame),** 8** not available.
 * So the eight-bit difference is not from the three -- it narrows the range of follow-up screening.
 *
 * ⁇  The absolute value here (15-19) is much smaller than the true stream of 50/58 because the smallest construction
 * Missing prefix fields from the true sequence header. The difference ** is measured instead of absolute deviation.
 * Absolute values vary as adjustments are achieved, and the margin relationship is fixed by the norm 5.9.2. */
static void test_refresh_bitpos_invariants(void)
{
    printf("refresh_frame_flags No variable for offset (norm) 5.9.2)\n");

    /* This example takes two values from the show_frame, which is asserted under the normative branch**
     * Bit-diverse. The driver will keep show_frame constant at 1 (see build_frame_header),
     * It will allow KEY (show=0) to enter the refresh_all branch of KEY+show,
     * The whole show=0 dimension collapses to show=1 -- the assertion loses meaning.
     * So turn on the A/B switch to restore the norm behavior. Default behavior is by
     * The end of the test_frame_header_show_frame is nailed separately. */
    setenv("DMD_AV1_NO_SHOWFORCE", "1", 1);

    struct dmd_av1_dpb dpb;
    VADecPictureParameterBufferAV1 pic;
    unsigned char buf[512];

    /* returns last_refresh_bitpos;size_t)-1 to the frame does not write the field. */
    size_t bp[4][2][2];
    for (int ft = 0; ft < 4; ft++)
        for (int show = 0; show < 2; show++)
            for (int er = 0; er < 2; er++) {
                memset(&dpb, 0, sizeof(dpb));
                memset(&pic, 0, sizeof(pic));
                dpb.last_refresh_bitpos = (size_t)-1;
                pic.frame_width_minus1 = 1919;
                pic.frame_height_minus1 = 1079;
                pic.order_hint = 4;
                pic.order_hint_bits_minus_1 = 6;
                pic.primary_ref_frame = 0;
                pic.pic_info_fields.bits.frame_type = (unsigned)ft;
                pic.pic_info_fields.bits.show_frame = (unsigned)show;
                pic.pic_info_fields.bits.showable_frame = 1;
                pic.pic_info_fields.bits.error_resilient_mode = (unsigned)er;
                pic.seq_info_fields.fields.enable_order_hint = 1;
                dmd_av1_build_frame_header(&pic, buf, sizeof(buf), &dpb);
                bp[ft][show][er] = dpb.last_refresh_bitpos;
            }

    /* KEY+show with SWITCH walk refresh_all path, not write the field -- must be -1.
     * This is important: if they are miswritten, the rewriting will be on a non-existent field. */
    check_eq("KEY+show No writing. refresh Fields",
             (long)(bp[0][1][0] == (size_t)-1), 1);
    check_eq("SWITCH No writing. refresh Fields",
             (long)(bp[3][1][0] == (size_t)-1), 1);
    check_eq("SWITCH(show=0) Not even a letter.",
             (long)(bp[3][0][0] == (size_t)-1), 1);

    /* The frame type that will write the field must be validly offset. */
    check_eq("INTER Write refresh Fields",
             (long)(bp[1][1][0] != (size_t)-1), 1);
    check_eq("INTRA_ONLY Write refresh Fields",
             (long)(bp[2][1][0] != (size_t)-1), 1);
    check_eq("KEY(show=0) Write refresh Fields",
             (long)(bp[0][0][0] != (size_t)-1), 1);

    /* error_resilient_mode saves prismary_ref_frame (3 places), so er=1 less than 3 places.
     * This is the norm 5.9.2 condition! Intra_only &!error_resilient_mode. */
    check_eq("INTER: er=1 Bing er=0 Less 3 bits",
             (long)(bp[1][1][0] - bp[1][1][1]), 3);

    /* Intra_only also skips the prism_ref_frame, 3 bits from INTER(er=0). */
    check_eq("INTRA_ONLY Bing INTER(er=0) Less 3 bits",
             (long)(bp[1][1][0] - bp[2][1][0]), 3);

    /* Key conclusion: The maximum margin for all the above combinations is 3,** 8 bits** which cannot be captured.
     * So the 8-bit offset of the 30/60/90/120/150 frame does not come from frame_type/
     * Show_frame / error_resilient_mode any of them.
     * This assertion has stabilized it to prevent the subsequent misperception that it is one of the three. */
    size_t mx = 0, mn = (size_t)-1;
    for (int ft = 0; ft < 4; ft++)
        for (int show = 0; show < 2; show++)
            for (int er = 0; er < 2; er++) {
                size_t v = bp[ft][show][er];
                if (v == (size_t)-1) continue;
                if (v > mx) mx = v;
                if (v < mn) mn = v;
            }
    if (mx - mn >= 8) {
        fails++;
        printf("  ✗ Three factors have been created. %zu Bit(>=8),"
               "And we’re on the spot. 8 The difference may be them. We need to recheck them.\n", mx - mn);
    }

    unsetenv("DMD_AV1_NO_SHOWFORCE");
}

/* - - - - - - - - - - - - - - - - - - - - - - - - Global campaigns */

/* Find a continuous position in the bit-by-post load, return to the starting subscript; no return -1 can be found. */
static long find_bits(const unsigned char *buf, size_t n, const char *pat)
{
    const size_t plen = strlen(pat);
    for (size_t i = 0; i + plen <= n * 8; i++) {
        size_t k = 0;
        while (k < plen && bit_at(buf, n, i + k) == (long)(pat[k] - '0'))
            k++;
        if (k == plen)
            return (long)i;
    }
    return -1;
}

static void check_bits(const char *what, const unsigned char *buf, size_t n,
                       const char *pat)
{
    if (find_bits(buf, n, pat) >= 0)
        return;
    fails++;
    printf("  ✗ %s:I can’t find it in the stream. %zu Bit Mode\n      %s\n",
           what, strlen(pat), pat);
}

static void check_no_bits(const char *what, const unsigned char *buf, size_t n,
                          const char *pat)
{
    long at = find_bits(buf, n, pat);
    if (at < 0)
        return;
    fails++;
    printf("  ✗ %s:SMS. %ld You’ve got a pattern that you shouldn’t have.\n      %s\n", what, at, pat);
}

/* A minimum available frame parameter (global motion only occurs in frame frame).
 * All 0xff, dpb, zero:my_idx[] will count meaningless slots, but
 * 1 prev = default is not even checked at all; 2 prev = non-default is filled in **8 slots
 * The same ** parameter, the slot number is the same everywhere. So this is a rule that only applies to algorithms and not to translator.
 * (Staff translation is covered by test_patch_prev_refresh/ hardware regression). */
static void gm_pic(VADecPictureParameterBufferAV1 *pic, int hp)
{
    memset(pic, 0, sizeof(*pic));
    pic->frame_width_minus1  = 63;
    pic->frame_height_minus1 = 63;
    pic->order_hint = 2;
    pic->order_hint_bits_minus_1 = 6;
    pic->pic_info_fields.bits.frame_type = 1;          /* INTER_FRAME */
    pic->pic_info_fields.bits.show_frame = 1;
    pic->pic_info_fields.bits.allow_high_precision_mv = (unsigned)hp;
    pic->seq_info_fields.fields.enable_order_hint = 1;
}

static void gm_set(VADecPictureParameterBufferAV1 *pic, int i, int type,
                   const int32_t m[6])
{
    pic->wm[i].wmtype = (VAAV1TransformationType)type;
    pic->wm[i].invalid = 0;
    for (int j = 0; j < 6; j++)
        pic->wm[i].wmmat[j] = m[j];
}

/* The source of the string is expected (three duplicates, any error will be caught in two other places):
 *   1) "111110110110..." This 109 bit** ffmpeg CBS traffic**, verbatim from source stream
 *      (v1work/cov/orig_tr.txt 158~266 rows by bit) i.e., the position of the aom encoder itself.
 *      Syntax: ref1 is ROTZOOM (1,1) + symbol 118/31/936/2873, ref2, ref3 none,
 *      Ref4 is ROTZOOM + symbol 2/512/1541, ref5~7 none.
 *   2) to remove the string with a separate reading (av1work/gbits.py)
 *      118/31/936/2873, 2/512/1541, consistent with CBS printing.
 *   3) Hardware physical: The 24-snippet output is the same as the soft resolution bytes.
 * The other three (diff/hp0/fall) are generated and written by the same Python round-trip. */
static const char GM_SRC[] =
    "11111101101101101111111111101101010001111111110011001110010011001001"
    "01111111100000000001111111101000000101000";
static const char GM_DIFF[] =
    "10100011000010011111111100000000000011111111101000000000011111111100"
    "1000000000111001000111011110111000110";
static const char GM_HP0[] = "10101100001";
static const char GM_FALLBACK[] = "010100100001";
/* The ref1 paragraph in GM_SRC (58 bits): the wm [0] of Example 4 is that it adds an LSB, which is the first LSB to the LSB.
 * It's used to assert that "not rounded to the nearest cell, but written as it is". */
static const char GM_M0[] =
    "1111110110110110111111111110110101000111111111001100111001";

static void test_global_motion(void)
{
    printf("Global global_motion_params(Norms 5.9.24 / 7.11.3.7)\n");

    static const int32_t M0[6] = { 479232, -1471488, 65654, -32, 32, 65654 };
    static const int32_t M3[6] = { 262144, -789504, 65538, -6, 6, 65538 };
    /* TRANSLATION(hp=1) < 13, (hp=0) 1<14; idx0/1 of ROTSOOM/ AFFFINE
     * Box 1 < 10, idx2.5 > < 1. */
    static const int32_t D_TR[6] = { 40960, -16384, 0, 0, 0, 65536 };
    static const int32_t D_AF[6] = { 1024, -3072, 65536, 2048, -2048, 65536 };
    static const int32_t H_TR[6] = { 49152, -16384, 0, 0, 0, 65536 };
    static const int32_t F_TR[6] = { 8192, -8192, 0, 0, 0, 65536 };
    static const int32_t PREV[6] = { 32768, 16384, 67584, -1024, 512, 65576 };

    unsigned char buf[1024];
    struct dmd_av1_dpb dpb;
    VADecPictureParameterBufferAV1 pic;
    size_t n;

    /* --1) prev = default value (primariy_ref_frame=NONE), equal to source flow --- */
    gm_pic(&pic, 1);
    pic.primary_ref_frame = 7;                    /* PRIMARY_REF_NONE */
    gm_set(&pic, 0, VAAV1TransformationRotzoom, M0);
    gm_set(&pic, 3, VAAV1TransformationRotzoom, M3);
    memset(&dpb, 0, sizeof(dpb));
    n = dmd_av1_build_frame_header(&pic, buf, sizeof(buf), &dpb);
    if (n == 0) {
        fails++;
        printf("  ✗ Frame Synthesis Return 0,Use of examples that do not cover targets\n");
        return;
    }
    check_bits("prev=The default should be written with the same source stream as the one in place gm Syntax:", buf, n, GM_SRC);

    /* --2) prev = non-default value: check table prev by slot, symbol is differential result --- */
    gm_pic(&pic, 1);
    pic.primary_ref_frame = 0;
    gm_set(&pic, 0, VAAV1TransformationTranslation, D_TR);
    gm_set(&pic, 1, VAAV1TransformationAffine, D_AF);
    memset(&dpb, 0, sizeof(dpb));
    for (int k = 0; k < 8; k++)
        for (int i = 0; i < 7; i++)
            for (int j = 0; j < 6; j++)
                dpb.gm_slot[k].p[i][j] = PREV[j];
    n = dmd_av1_build_frame_header(&pic, buf, sizeof(buf), &dpb);
    if (n == 0) {
        fails++;
        printf("  ✗ The case is synthesized using the case header. 0\n");
        return;
    }
    check_bits("prev=The difference in the number of characters should be used for non-default(ROTZOOM/AFFINE Order [2][3][0][1]/[2][3][4][5][0][1])",
               buf, n, GM_DIFF);

    /* --3) thp=0 TRANSLATION: abs_bits8/ prec_bits2, all slot errors --- */
    gm_pic(&pic, 0);
    pic.primary_ref_frame = 7;
    gm_set(&pic, 0, VAAV1TransformationTranslation, H_TR);
    memset(&dpb, 0, sizeof(dpb));
    n = dmd_av1_build_frame_header(&pic, buf, sizeof(buf), &dpb);
    check_bits("hp=0 TRANSLATION We should go. 9-1/3-1 That one.", buf, n, GM_HP0);

    /* --4) Unable to compile values must be returned to IDENTITY without half-hands--
     * wm[0] wmmat[0] for a margin 1 (479232+1 cannot be divided by 1024);
     * wm[1] is legal TRAINSLATION. Returning the correct time stream is a string of "0" + wm[1],
     * If you write half the parameter, the wm [1] string will not be close to 0 after is_global=0. */
    gm_pic(&pic, 1);
    pic.primary_ref_frame = 7;
    {
        int32_t bad[6];
        memcpy(bad, M0, sizeof(bad));
        bad[0] += 1;
        gm_set(&pic, 0, VAAV1TransformationRotzoom, bad);
    }
    gm_set(&pic, 1, VAAV1TransformationTranslation, F_TR);
    memset(&dpb, 0, sizeof(dpb));
    n = dmd_av1_build_frame_header(&pic, buf, sizeof(buf), &dpb);
    check_bits("Unencoded gm The whole frame should be returned. IDENTITY And don’t write half a cross-section of parameters.",
               buf, n, GM_FALLBACK);
    check_no_bits("Do not round the value of the crossing back and write it as usual.)",
                  buf, n, GM_M0);
}

int main(void)
{
    printf("=== AV1 Bits in the plain language. ===\n");
    test_leb128();
    test_obu_header();
    test_align();
    test_le();
    test_uvlc();
    test_ns();
    test_su();
    test_sequence_header();
    test_show_existing();
    test_patch_prev_refresh();
    test_refs_resolvable();
    test_frame_header_show_frame();
    test_refresh_bitpos_invariants();
    test_global_motion();

    if (fails == 0) {
        printf("=== All through ===\n");
        return 0;
    }
    printf("=== %d Item Failed ===\n", fails);
    return 1;
}
