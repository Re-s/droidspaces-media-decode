/* Could the mid-way re-transmission of the argument set destroy the frame of reference? Directly, without a driver, MediaCodec.
 *
 * Background: Viewer 725 frames have 135 frames of pure black (Y=16), while ffmpeg paths have zero black frames.
 * The only significant difference is that the driver will reload the PPS (643 times) with the B/P frame type.
 * So I suspect "retransmission of the Arguments Set Relocation Encoder."
 *
 * The two changes (which were not heavy and were used only for the first time) did not reduce a black frame.
 * This probe directly detects four scenarios:
 *
 *   A) Send CSD only at the beginning
 *   B) Retransmit arguments set in front of each frame with FLAG_CODEC_CONFIG
 *   C) Retransmit arguments set in front of each frame, in-band (flags=0)
 *   D) PPS (mage Num_ref_idx changes) for reloading ** in front of frame ** modified**
 *
 * Each case is marked with black frame numbers.
 *  - If A has a black frame, it's not about re-delivery, it's another reason (e.g. output order/ pairing)
 *  - If only B had a black frame, FLAG_CODEC_CONFIG would be the killer.
 *  - If B/C/D has one, no halfway parameter set.
 *  - If not, a black frame is not at all on the side of the daemon, the problem is the driver or browser.
 *
 * Build:
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang \
 *     -O2 -o probe_csd_reset probe_csd_reset.c -lmediandk -llog
 */
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define FLAG_CODEC_CONFIG 2
#define FLAG_EOS          4
#define STREAM "/data/local/tmp/test1080.h264"

static uint8_t *g_buf;
static long g_len;

struct nalu { long off; long len; int type; };
static struct nalu g_nalus[8192];
static int g_n;

static void scan(void)
{
    long i = 0;
    while (i + 3 < g_len) {
        if (g_buf[i] == 0 && g_buf[i+1] == 0 && g_buf[i+2] == 1) {
            long s = i + 3;
            long e = s;
            while (e + 3 < g_len &&
                   !(g_buf[e] == 0 && g_buf[e+1] == 0 && g_buf[e+2] == 1))
                e++;
            long n = (e + 3 >= g_len) ? g_len - s : e - s;
            if (n > 0 && g_n < 8192) {
                g_nalus[g_n].off = s;
                g_nalus[g_n].len = n;
                g_nalus[g_n].type = g_buf[s] & 0x1f;
                g_n++;
            }
            i = s;
        } else {
            i++;
        }
    }
}

/* Sample is the brightness mean */
static double luma(const uint8_t *p, size_t n)
{
    double sum = 0; int cnt = 0;
    size_t lim = n > 2000000 ? 2000000 : n;
    for (size_t k = 0; k < lim; k += 1499) { sum += p[k]; cnt++; }
    return cnt ? sum / cnt : 0;
}

/* Mode: 0 = only start 1 = FLAG_CODEC_CONFIG 2 = frame in-band 3 = frame change */
static void run(int mode, const char *name)
{
    /* Collecting SPS/PPS */
    uint8_t csd[512]; size_t csd_len = 0;
    int sps_i = -1, pps_i = -1;
    for (int k = 0; k < g_n; k++) {
        if (g_nalus[k].type == 7 && sps_i < 0) sps_i = k;
        if (g_nalus[k].type == 8 && pps_i < 0) pps_i = k;
    }
    if (sps_i < 0 || pps_i < 0) { printf("  Not found SPS/PPS\n"); return; }

    csd[0]=0;csd[1]=0;csd[2]=0;csd[3]=1;
    memcpy(csd+4, g_buf+g_nalus[sps_i].off, g_nalus[sps_i].len);
    csd_len = 4 + g_nalus[sps_i].len;
    csd[csd_len]=0;csd[csd_len+1]=0;csd[csd_len+2]=0;csd[csd_len+3]=1;
    memcpy(csd+csd_len+4, g_buf+g_nalus[pps_i].off, g_nalus[pps_i].len);
    csd_len += 4 + g_nalus[pps_i].len;

    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    AMediaFormat_setInt32(fmt, "low-latency", 1);
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);
    AMediaFormat_setBuffer(fmt, "csd-0", csd, csd_len);

    AMediaCodec *c = AMediaCodec_createDecoderByType("video/avc");
    if (!c) { printf("  Failed to create decoder\n"); AMediaFormat_delete(fmt); return; }
    if (AMediaCodec_configure(c, fmt, NULL, NULL, 0) != AMEDIA_OK) {
        printf("  configure failed\n");
        AMediaCodec_delete(c); AMediaFormat_delete(fmt); return;
    }
    AMediaCodec_start(c);
    AMediaFormat_delete(fmt);

    int sent = 0, got = 0, black = 0, first_black = -1;
    int csd_once_done = 0;

    for (int k = 0; k < g_n && sent < 150; k++) {
        int t = g_nalus[k].type;
        if (t == 7 || t == 8) continue;      /* Parameter set is handled separately */
        if (t != 1 && t != 5) continue;      /* VCL only */

        /* Press mode to decide whether to send the argument set in front of this frame */
        int resend = 0;
        if (mode == 0) resend = (!csd_once_done);
        else           resend = 1;

        if (resend) {
            ssize_t ci = AMediaCodec_dequeueInputBuffer(c, 20000);
            if (ci >= 0) {
                size_t cap; uint8_t *ib = AMediaCodec_getInputBuffer(c, ci, &cap);
                uint8_t tmp[512]; size_t tl = csd_len;
                memcpy(tmp, csd, csd_len);
                if (mode == 3) {
                    /* Change PPS last byte, simulate num_ref_idx changes */
                    tmp[tl-1] ^= 0x01;
                }
                if (ib && cap >= tl) {
                    memcpy(ib, tmp, tl);
                    uint32_t fl = 0;
                    if (mode == 1 || (mode == 0 && !csd_once_done)) fl = FLAG_CODEC_CONFIG;
                    if (mode == 3) fl = FLAG_CODEC_CONFIG;
                    AMediaCodec_queueInputBuffer(c, ci, 0, tl, 0, fl);
                    csd_once_done = 1;
                } else {
                    AMediaCodec_queueInputBuffer(c, ci, 0, 0, 0, 0);
                }
            }
        }

        /* Send VCL */
        ssize_t bi = AMediaCodec_dequeueInputBuffer(c, 20000);
        if (bi < 0) continue;
        size_t cap; uint8_t *ib = AMediaCodec_getInputBuffer(c, bi, &cap);
        long need = 4 + g_nalus[k].len;
        if (!ib || cap < (size_t)need) {
            AMediaCodec_queueInputBuffer(c, bi, 0, 0, 0, 0);
            continue;
        }
        ib[0]=0;ib[1]=0;ib[2]=0;ib[3]=1;
        memcpy(ib+4, g_buf+g_nalus[k].off, g_nalus[k].len);
        AMediaCodec_queueInputBuffer(c, bi, 0, need, (int64_t)(sent+1)*1000, 0);
        sent++;

        /* Take Output */
        for (;;) {
            AMediaCodecBufferInfo info;
            ssize_t oi = AMediaCodec_dequeueOutputBuffer(c, &info, 0);
            if (oi < 0) break;
            size_t osz;
            uint8_t *ob = AMediaCodec_getOutputBuffer(c, oi, &osz);
            if (ob && info.size > 100000) {
                double m = luma(ob + info.offset, info.size);
                got++;
                if (m < 20.0) { black++; if (first_black < 0) first_black = got; }
            }
            AMediaCodec_releaseOutputBuffer(c, oi, false);
        }
    }

    /* & Empty */
    ssize_t bi = AMediaCodec_dequeueInputBuffer(c, 20000);
    if (bi >= 0) AMediaCodec_queueInputBuffer(c, bi, 0, 0, 0, FLAG_EOS);
    for (int guard = 0; guard < 200; guard++) {
        AMediaCodecBufferInfo info;
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(c, &info, 30000);
        if (oi == AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;
        if (oi < 0) continue;
        size_t osz;
        uint8_t *ob = AMediaCodec_getOutputBuffer(c, oi, &osz);
        if (ob && info.size > 100000) {
            double m = luma(ob + info.offset, info.size);
            got++;
            if (m < 20.0) { black++; if (first_black < 0) first_black = got; }
        }
        int eos = (info.flags & FLAG_EOS) != 0;
        AMediaCodec_releaseOutputBuffer(c, oi, false);
        if (eos) break;
    }

    AMediaCodec_stop(c);
    AMediaCodec_delete(c);

    printf("  %-34s Send %3d frame, Out %3d frame, Black %3d",
           name, sent, got, black);
    if (black) printf("  First Black Frame=number %d frame", first_black);
    printf("\n");
}

int main(void)
{
    FILE *f = fopen(STREAM, "rb");
    if (!f) { printf("I can’t read it. %s\n", STREAM); return 1; }
    fseek(f, 0, SEEK_END); g_len = ftell(f); fseek(f, 0, SEEK_SET);
    g_buf = malloc(g_len);
    if (fread(g_buf, 1, g_len, f) != (size_t)g_len) { printf("Reading Failed\n"); return 1; }
    fclose(f);
    scan();
    printf("bitstream %ld bytes,%d  NALU\n\n", g_len, g_n);

    printf("Black frame number comparison for four parameters for re-loading:\n");
    run(0, "A) Just send it at the beginning. CSD(Benchmark)");
    run(1, "B) Reload every frame + FLAG_CODEC_CONFIG");
    run(2, "C) Reload every frame + in-band(flags=0)");
    run(3, "D) Every frame is retransmitted. PPS");

    printf("\n Interpretation: if A There are also black frames that are not related to re-delivery, if only B/D There are other things. FLAG_CODEC_CONFIG He’s the killer.;\n"
           "If all 0 The black frame is not there. daemon Side, should search the drive/Browser Path.\n");
    return 0;
}
