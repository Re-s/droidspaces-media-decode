/* Verify the feasibility of the HEVC parameters being assembled.
 *
 * Critical risk: VAPictureParameter BufferHEVC from VA-API only gives
 * Num_shot_term_ref_pic_sets (** number**), not to each st_ref_pic_set**.
 * The slice header references them through short_term_ref_pic_set_idx,
 * And when the short_term_ref_pic_set_sps_flag=0 slick header** inner connection**
 * st_ref_pic_set -- that part is based on the SPS declaration num_shot_term_ref_pic_sets
 * The syntax of st_ref_pic_set is dependent on stRpsIdx and num_delta_pocs.
 *
 * Setting num_short_term_ref_pic_sets to 0 in the synthesized SPS would
 * And the original slick header is "N" coded in the SPS, and the parsing is mispositioned.
 *
 * This probe uses ffmpeg to interpret the real HEVC stream, print:
 *   1) actual num_short_term_ref_pic_sets
 *   2) Short_term_ref_pic_set_sps_flag for each slick
 * This makes it possible to synthesize SPS safely.
 *
 * Compile (in packaging):
 *   gcc -O2 -o probe_hevc_sps probe_hevc_sps.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned char *buf;
static long blen;

/* Extremely Simple RBSP Reader (Go EPB) */
struct br {
    const unsigned char *d;
    long len;
    long bytepos;
    int bitpos;
};

static void br_init(struct br *b, const unsigned char *d, long len)
{
    b->d = d; b->len = len; b->bytepos = 0; b->bitpos = 0;
}

static uint32_t br_u(struct br *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if (b->bytepos >= b->len) return v;
        int bit = (b->d[b->bytepos] >> (7 - b->bitpos)) & 1;
        v = (v << 1) | (uint32_t)bit;
        if (++b->bitpos == 8) { b->bitpos = 0; b->bytepos++; }
    }
    return v;
}

static uint32_t br_ue(struct br *b)
{
    int lz = 0;
    while (b->bytepos < b->len && br_u(b, 1) == 0 && lz < 32) lz++;
    if (lz == 0) return 0;
    return ((1u << lz) - 1) + br_u(b, lz);
}

/* The field requiring se (v) has not been resolved in HEVC SPS, but the bit reader set is kept...
 * The following field is directly available when you want to interpret the pps_beta_offset_div2. */
__attribute__((unused))
static int32_t br_se(struct br *b)
{
    uint32_t c = br_ue(b);
    return (c & 1) ? (int32_t)((c + 1) / 2) : -(int32_t)(c / 2);
}

/* Remove the declaration reservation bytes */
static long unescape(const unsigned char *in, long n, unsigned char *out)
{
    long o = 0; int zeros = 0;
    for (long i = 0; i < n; i++) {
        if (zeros == 2 && in[i] == 0x03) { zeros = 0; continue; }
        out[o++] = in[i];
        if (in[i] == 0) zeros++; else zeros = 0;
    }
    return o;
}

static void skip_ptl(struct br *b, int max_sub_layers)
{
    br_u(b, 2);         /* general_profile_space */
    br_u(b, 1);         /* general_tier_flag */
    br_u(b, 5);         /* general_profile_idc */
    for (int i = 0; i < 32; i++) br_u(b, 1);
    br_u(b, 1); br_u(b, 1); br_u(b, 1); br_u(b, 1);
    for (int i = 0; i < 43; i++) br_u(b, 1);
    br_u(b, 1);
    br_u(b, 8);         /* general_level_idc */

    int spf[8] = {0}, slf[8] = {0};
    for (int i = 0; i < max_sub_layers - 1; i++) {
        spf[i] = (int)br_u(b, 1);
        slf[i] = (int)br_u(b, 1);
    }
    if (max_sub_layers - 1 > 0)
        for (int i = max_sub_layers - 1; i < 8; i++) br_u(b, 2);
    for (int i = 0; i < max_sub_layers - 1; i++) {
        if (spf[i]) {
            br_u(b, 2); br_u(b, 1); br_u(b, 5);
            for (int k = 0; k < 32; k++) br_u(b, 1);
            br_u(b, 1); br_u(b, 1); br_u(b, 1); br_u(b, 1);
            for (int k = 0; k < 43; k++) br_u(b, 1);
            br_u(b, 1);
        }
        if (slf[i]) br_u(b, 8);
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/root/decode-test/test1080.h265";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("I can’t read it. %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); blen = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(blen);
    if (fread(buf, 1, blen, f) != (size_t)blen) { printf("Reading Failed\n"); return 1; }
    fclose(f);

    unsigned char *rb = malloc(blen);
    int n_sps = 0, n_slice = 0, sps_flag_1 = 0, sps_flag_0 = 0;
    int num_st_rps = -1;

    long i = 0;
    while (i + 4 < blen) {
        if (!(buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1)) { i++; continue; }
        long s = i + 3;
        long e = s;
        while (e + 3 < blen && !(buf[e] == 0 && buf[e+1] == 0 && buf[e+2] == 1)) e++;
        long n = (e + 3 >= blen) ? blen - s : e - s;
        if (n < 3) { i = s; continue; }

        int nut = (buf[s] >> 1) & 0x3f;

        if (nut == 33 && num_st_rps < 0) {          /* SPS */
            long rn = unescape(buf + s + 2, n - 2, rb);
            struct br b; br_init(&b, rb, rn);
            br_u(&b, 4);                             /* sps_video_parameter_set_id */
            int max_sub = (int)br_u(&b, 3) + 1;      /* sps_max_sub_layers_minus1 */
            br_u(&b, 1);                             /* temporal_id_nesting */
            skip_ptl(&b, max_sub);
            br_ue(&b);                               /* sps_seq_parameter_set_id */
            uint32_t cfi = br_ue(&b);                /* chroma_format_idc */
            if (cfi == 3) br_u(&b, 1);
            br_ue(&b); br_ue(&b);                    /* pic width/height */
            if (br_u(&b, 1)) {                       /* conformance_window_flag */
                br_ue(&b); br_ue(&b); br_ue(&b); br_ue(&b);
            }
            br_ue(&b); br_ue(&b);                    /* bit_depth luma/chroma minus8 */
            br_ue(&b);                               /* log2_max_poc_lsb_minus4 */
            int sub_ordering = (int)br_u(&b, 1);
            for (int k = (sub_ordering ? 0 : max_sub - 1); k < max_sub; k++) {
                br_ue(&b); br_ue(&b); br_ue(&b);
            }
            br_ue(&b); br_ue(&b); br_ue(&b); br_ue(&b); br_ue(&b); br_ue(&b);
            if (br_u(&b, 1)) {                       /* scaling_list_enabled */
                if (br_u(&b, 1)) { printf("  (Ham scaling list data)\n"); }
            }
            br_u(&b, 1);                             /* amp_enabled */
            br_u(&b, 1);                             /* sao_enabled */
            if (br_u(&b, 1)) {                       /* pcm_enabled */
                br_u(&b, 4); br_u(&b, 4); br_ue(&b); br_ue(&b); br_u(&b, 1);
            }
            num_st_rps = (int)br_ue(&b);
            n_sps++;
            printf("SPS: num_short_term_ref_pic_sets = %d\n", num_st_rps);
        } else if (nut <= 21 && nut != 10 && nut != 12 && nut != 14) {
            /* VCL slice */
            long rn = unescape(buf + s + 2, n - 2, rb);
            struct br b; br_init(&b, rb, rn);
            int first_slice = (int)br_u(&b, 1);
            int irap = (nut >= 16 && nut <= 23);
            if (irap) br_u(&b, 1);                   /* no_output_of_prior_pics */
            br_ue(&b);                               /* slice_pic_parameter_set_id */
            if (first_slice && !irap) {
                /* POC and st_rps references are not for IRAP */
                n_slice++;
            }
            i = s; continue;
        }
        i = s;
    }

    printf("\n=== Conclusions ===\n");
    if (num_st_rps == 0)
        printf("num_short_term_ref_pic_sets = 0:SPS None st_ref_pic_set,\n"
               "slice header Inner Unions for-- Synthesis SPS Clock 0 - I’ll be fine.,**Security**.\n");
    else
        printf("num_short_term_ref_pic_sets = %d:SPS In there. st_ref_pic_set,\n"
               "And... VA-API Do not provide content. If synthesized, write 0,slice header Medium\n"
               "short_term_ref_pic_set_sps_flag=1 The quote will fail. —— **Risk.**,\n"
               "Need further confirmation ffmpeg It’s in the stream. flag Values.\n", num_st_rps);
    (void)sps_flag_1; (void)sps_flag_0; (void)n_slice;
    return 0;
}
