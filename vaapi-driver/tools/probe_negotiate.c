/* Preconditions for running consultations: how does daemon ** reliably ** determine the actual output order of the decoder?
 *
 * The Vendor key was set successfully. It is effective. Non-high access platforms ignore unknown keys quietly.
 * The decoder was still in display order, and the driver was mispositioned if the decoder was still in code order.
 * (actually 105/150 frames are mispositioned). So there must be a running time basis.
 *
 * Here is a comparison of two candidate paths:
 *   Path A: Confuture Read Back to the key with getOutputFormat
 *           — Simple, but the vendor key is usually not visible and needs to be measured
 *   Path B: Send an increment PTS to see if the output PTS is single-telephone
 *           - Show sequence output PTS single-line; decoding output encounters B frames are not single-lined
 *
 * Pass by authentication path C (more fundamental resolution): can you export the frame presentation TimeUs?
 * Back to the value when transmitted to? If you can, the driver can match the value with the PTS precisely.
 * There's no need to know the order of output -- that's not even a consultation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define FLAG_CODEC_CONFIG 2

static unsigned char *data;
static long dlen;

static int load(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); dlen = ftell(f); fseek(f, 0, SEEK_SET);
    data = malloc(dlen);
    if (fread(data, 1, dlen, f) != (size_t)dlen) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

static long next_nalu(long from, long *len)
{
    long i = from;
    while (i + 3 < dlen) {
        if (!data[i] && !data[i+1] && data[i+2] == 1) {
            long s = i + 3, j = s;
            while (j + 3 < dlen) {
                if (!data[j] && !data[j+1] && data[j+2] == 1) break;
                j++;
            }
            *len = (j + 3 >= dlen) ? dlen - s : j - s;
            return s;
        }
        i++;
    }
    return -1;
}

static unsigned char *sps, *pps;
static long sps_len, pps_len, vo[64], vl[64];
static int nv;

/* PTS Use a code that is well identified: 1,000,000 + i*10000 to confirm the return of the original */
/* Two steps are in contrast: the driver actually uses the step length 1 (unit numbering directly as PTS),
 * This probe was used for 1,000 steps. If the step is 1 it is quantified/combined by the decoder.
 * There will be multiple input units sharing the same PTS return -- that's exactly what drives the log.
 * "unit 5" is a suspicious phenomenon twice in a row. */
static int g_step = 1000;
#define MKPTS(i) ((int64_t)1000000 + (int64_t)(i) * g_step)

static void run(const char *label, int set_vendor_key)
{
    printf("\n===== %s =====\n", label);

    AMediaCodec *c = AMediaCodec_createDecoderByType("video/avc");
    if (!c) { printf("  Failed to create decoder\n"); return; }
    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    AMediaFormat_setInt32(fmt, "low-latency", 1);
    if (set_vendor_key)
        AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);

    if (AMediaCodec_configure(c, fmt, NULL, NULL, 0) != AMEDIA_OK) {
        printf("  configure failed\n");
        AMediaFormat_delete(fmt); AMediaCodec_delete(c); return;
    }
    AMediaCodec_start(c);

    /* - Path A: Confuture/ Start read the key back */
    {
        AMediaFormat *of = AMediaCodec_getOutputFormat(c);
        if (!of) {
            printf("  Ways A: getOutputFormat return NULL(start No output format yet)\n");
        } else {
            int32_t v = -12345;
            int got = AMediaFormat_getInt32(of,
                        "vendor.qti-ext-dec-picture-order.enable", &v);
            printf("  Ways A: Read Back vendor Keys -> %s",
                   got ? "succeeded" : "Failed (key does not echo))");
            if (got) printf(",Value=%d", v);
            printf("\n");
            AMediaFormat_delete(of);
        }
    }

    unsigned char csd[512];
    memcpy(csd, sps, sps_len);
    memcpy(csd + sps_len, pps, pps_len);
    ssize_t bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
    size_t cap;
    uint8_t *ib = AMediaCodec_getInputBuffer(c, bi, &cap);
    memcpy(ib, csd, sps_len + pps_len);
    AMediaCodec_queueInputBuffer(c, bi, 0, sps_len + pps_len, 0, FLAG_CODEC_CONFIG);

    /* Send 12 VCLs to collect output PTS sequences */
    int64_t out_pts[32];
    int nout = 0;
    for (int i = 0; i < 12; i++) {
        bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
        if (bi < 0) break;
        ib = AMediaCodec_getInputBuffer(c, bi, &cap);
        memcpy(ib, data + vo[i], vl[i]);
        AMediaCodec_queueInputBuffer(c, bi, 0, vl[i], MKPTS(i), 0);

        for (int spin = 0; spin < 6 && nout < 32; spin++) {
            AMediaCodecBufferInfo info;
            ssize_t oi = AMediaCodec_dequeueOutputBuffer(c, &info, 20000);
            if (oi >= 0) {
                if (info.size > 0) out_pts[nout++] = info.presentationTimeUs;
                AMediaCodec_releaseOutputBuffer(c, oi, false);
            }
        }
    }

    /* - Path B: Whether PTS output is single-form */
    printf("  Output PTS Sequence (into code as 1000000+i*1000):\n    ");
    for (int i = 0; i < nout; i++) {
        /* Invert to the serial number of the delivery, so that people can see the order. */
        long idx = (long)((out_pts[i] - 1000000) / 1000);
        printf("%ld ", idx);
    }
    printf("\n");

    int dupe = 0;
    for (int i = 0; i < nout; i++)
        for (int j = i + 1; j < nout; j++)
            if (out_pts[i] == out_pts[j]) { dupe++; }
    if (dupe)
        printf("  ⚠️ Yes. %d Repeat PTS —— Can not get a pair of these!\n", dupe);

    int monotonic = 1;
    for (int i = 1; i < nout; i++)
        if (out_pts[i] < out_pts[i-1]) { monotonic = 0; break; }
    printf("  Ways B: PTS %s → Deciding that**%s**Output\n",
           monotonic ? "Single Increment" : "Non-single",
           monotonic ? "Show Order" : "Decoding");

    /* - Route C: Whether PTS is back-to-back as it is (for precise pairing) */
    int exact = 1;
    for (int i = 0; i < nout; i++) {
        if ((out_pts[i] - 1000000) % 1000 != 0 ||
            out_pts[i] < 1000000 || out_pts[i] > 1000000 + 11 * 1000) {
            exact = 0; break;
        }
    }
    printf("  Ways C: PTS %s → %s For precise pairing\n",
           exact ? "Back as you wish.)" : "Rewrited/Recalculation",
           exact ? "Yeah." : "Not at all.");

    printf("  Frames %d\n", nout);

    AMediaCodec_stop(c);
    AMediaCodec_delete(c);
    AMediaFormat_delete(fmt);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/data/local/tmp/test1080.h264";
    if (load(path) < 0) { printf("I can’t read it. %s\n", path); return 1; }

    long off = 0, len;
    while ((off = next_nalu(off, &len)) >= 0 && nv < 64) {
        int nut = data[off] & 0x1f;
        if (nut == 7 && !sps) { sps = data + off - 3; sps_len = len + 3; }
        else if (nut == 8 && !pps) { pps = data + off - 3; pps_len = len + 3; }
        else if (nut == 1 || nut == 5) { vo[nv] = off - 3; vl[nv] = len + 3; nv++; }
        off += len;
    }
    if (!sps || !pps || nv < 12) { printf("We’re running out of yards.\n"); return 1; }

    printf("########## Long 1000us ##########\n");
    g_step = 1000;
    run("Do Not Set vendor Keys (Show order))", 0);
    run("Set vendor Key (following input order))", 1);

    printf("\n########## Long 1us(Driver practical usage)##########\n");
    g_step = 1;
    run("Do Not Set vendor Keys (Show order))", 0);
    run("Set vendor Key (following input order))", 1);

    printf("\n The court has chosen: the path. A (b) Not available without a return, B - Yes, I do. B The flow of the frame is the difference.;\n");
    printf("Ways C Best if created -- drive PTS Accurate pairing, no need to know the order of output.\n");
    return 0;
}
