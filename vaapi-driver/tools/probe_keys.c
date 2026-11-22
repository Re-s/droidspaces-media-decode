/* Can you get MediaCodec to spit the header frame in the shallow queue (3 input units)?
 *
 * It's the key to the "black-control frame": if we can do this, we don't need to emptify/rebuild at all.
 * The frame of reference is constantly in place, and the image is naturally correct. Low-latency=1 known to be invalid (delay is still 4).
 *
 * Here, try the profile one by one, measuring the lag depth of each configuration (a few VCLs are sent out of the frame).
 * A lag of < = 3 is available.
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

/* Returns the lag depth: The first frame appears when sending the first VCL. 1 = 8 undelivered */
static int measure(const char *label, void (*tweak)(AMediaFormat *))
{
    AMediaCodec *c = AMediaCodec_createDecoderByType("video/avc");
    if (!c) return -2;
    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    if (tweak) tweak(fmt);

    if (AMediaCodec_configure(c, fmt, NULL, NULL, 0) != AMEDIA_OK) {
        printf("%-46s configure failed\n", label);
        AMediaFormat_delete(fmt); AMediaCodec_delete(c);
        return -2;
    }
    AMediaCodec_start(c);

    unsigned char csd[512];
    memcpy(csd, sps, sps_len);
    memcpy(csd + sps_len, pps, pps_len);
    ssize_t bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
    size_t cap;
    uint8_t *ib = AMediaCodec_getInputBuffer(c, bi, &cap);
    memcpy(ib, csd, sps_len + pps_len);
    AMediaCodec_queueInputBuffer(c, bi, 0, sps_len + pps_len, 0, FLAG_CODEC_CONFIG);

    int lag = -1;
    for (int i = 0; i < 8 && lag < 0; i++) {
        bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
        if (bi < 0) break;
        ib = AMediaCodec_getInputBuffer(c, bi, &cap);
        memcpy(ib, data + vo[i], vl[i]);
        AMediaCodec_queueInputBuffer(c, bi, 0, vl[i], (int64_t)i * 33333, 0);

        /* Give me time: Make sure it's not "no time," but "no time." */
        for (int spin = 0; spin < 15; spin++) {
            AMediaCodecBufferInfo info;
            ssize_t oi = AMediaCodec_dequeueOutputBuffer(c, &info, 20000);
            if (oi >= 0) {
                if (info.size > 0) lag = i + 1;
                AMediaCodec_releaseOutputBuffer(c, oi, false);
                if (lag > 0) break;
            }
        }
    }

    printf("%-46s Delay %s%s\n", label,
           lag < 0 ? "No frame out." : (lag == 1 ? "1" : lag == 2 ? "2" : lag == 3 ? "3" : "4+"),
           (lag > 0 && lag <= 3) ? "  ← Available!" : "");

    AMediaCodec_stop(c);
    AMediaCodec_delete(c);
    AMediaFormat_delete(fmt);
    return lag;
}

static void k_none(AMediaFormat *f) { (void)f; }
static void k_lowlat(AMediaFormat *f) { AMediaFormat_setInt32(f, "low-latency", 1); }
static void k_maxreorder(AMediaFormat *f) {
    AMediaFormat_setInt32(f, "low-latency", 1);
    AMediaFormat_setInt32(f, "max-output-reorder-frames", 0);
}
static void k_vendorlowlat(AMediaFormat *f) {
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
}
static void k_vendorpicorder(AMediaFormat *f) {
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", 1);
}
static void k_vendorboth(AMediaFormat *f) {
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", 1);
}
static void k_outputdelay(AMediaFormat *f) {
    AMediaFormat_setInt32(f, "low-latency", 1);
    AMediaFormat_setInt32(f, "output-delay", 0);
}
static void k_numref(AMediaFormat *f) {
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(f, "output-delay", 0);
    AMediaFormat_setInt32(f, "max-output-reorder-frames", 0);
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
    if (!sps || !pps || nv < 8) { printf("We’re running out of yards.\n"); return 1; }
    printf("Target: behind schedule <= 3(Consumer stability is flying. 3 frame)\n\n");

    measure("① Default (no key))", k_none);
    measure("② low-latency=1(current daemon It’s for.)", k_lowlat);
    measure("③ low-latency + max-output-reorder-frames=0", k_maxreorder);
    measure("④ vendor.qti-ext-dec-low-latency.enable=1", k_vendorlowlat);
    measure("⑤ vendor.qti-ext-dec-picture-order.enable=1", k_vendorpicorder);
    measure("⑥ vendor Both of them.", k_vendorboth);
    measure("⑦ low-latency + output-delay=0", k_outputdelay);
    measure("⑧ vendor low-latency + output-delay + reorder=0", k_numref);

    printf("\n If any configuration lags <= 3,I’ll be completely free of the drain./Rebuild, black frame problems disappear..\n");
    return 0;
}
