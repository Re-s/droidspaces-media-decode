/* MediaCodec can still decode after EOS + Flush?
 *
 * This decision determines whether Damon can provide "reversible emptiness":
 * The driver can retrieve the frame only by calling finish_input, which irreversibly calls shutdown(SHUT_WR),
 * Then you have to rebuild the session - to measure 155 ms per frame, and to play 4.7 times slower.
 *
 * If the AmediaCodec_flush+ re-transmission CSD after EOS allows the same codec example to continue working, the CSD will be able to do so.
 * Daemon can make "circle" into a non-destructive session operation, save reconstruction.
 *
 * Note that this is running outside of ** container** (android side) using NDK MediaCodec,
 * Without the daemon protocol -- the purpose is to verify the semantic syntax of the decoder before deciding whether to change the daemon.
 *
 * Limitations: This probe** only counts the number of frames and the codec example, without checking the contents of the image**.
 *
 * It was "Funneled" when it came to the conclusion that "Flush is reloaded after three frames" and the CSD can continue to decipher the six frames."
 * "Deficit reversible, non-subsequent" -- that's ** overread**. It proves only that the codec case is not dead.
 * And the frame, and it didn't prove the frame was right.
 *
 * Later black frame investigations confirmed that **flush would destroy the reference frame** and that the P/B frame would be rediscovered forever.
 * Black to next IDR (actual browser 135/708 frames are black and fixed to zero).
 * So "reverseable" is only established in the sense that "the object of the conversation can continue to be used", and the image is costly.
 *
 * This qualification must be used when quoting this probe. To verify the image's correctness, the byte-to-by-the-the-tem.
 * Softly delineate the baseline, or a black frame is judged by the mean brightness (see probe_replay.c/ probe_black.c).
 * This project has been planted three times because of the "no pixels at all" which is the source of one of the cases.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define FLAG_CODEC_CONFIG 2
#define FLAG_EOS          4

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

/* Queued: send EOS, take all frames, return the number of frames received */
static int drain(AMediaCodec *c)
{
    ssize_t bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
    if (bi < 0) { printf("  Empty: Unable to take input buffer %zd\n", bi); return -1; }
    AMediaCodec_queueInputBuffer(c, bi, 0, 0, 0, FLAG_EOS);

    int got = 0;
    for (int spin = 0; spin < 300; spin++) {
        AMediaCodecBufferInfo info;
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(c, &info, 20000);
        if (oi >= 0) {
            if (info.size > 0) {
                got++;
                /* Only the frames are not enough -- the previous version was used for miscalculation.
                 * Check brightness: The flash excavated the frame after it was dropped is pure black (Y=16). */
                size_t osz; uint8_t *ob = AMediaCodec_getOutputBuffer(c, oi, &osz);
                if (ob && osz > 200000) {
                    double sum = 0; int nsamp = 0;
                    for (size_t k = 0; k < 100000; k += 97) { sum += ob[k]; nsamp++; }
                    double mean = sum / nsamp;
                    if (got <= 8)
                        printf("    frame%d Brightness Mean %.1f%s\n", got, mean,
                               mean < 20.0 ? "  ← Black Frame!" : "");
                }
            }
            int eos = (info.flags & FLAG_EOS) != 0;
            AMediaCodec_releaseOutputBuffer(c, oi, false);
            if (eos) break;
        }
    }
    return got;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/data/local/tmp/test1080.h264";
    if (load(path) < 0) { printf("I can’t read it. %s\n", path); return 1; }

    unsigned char *sps = NULL, *pps = NULL;
    long sps_len = 0, pps_len = 0, off = 0, len;
    long vo[64], vl[64];
    int nv = 0;
    while ((off = next_nalu(off, &len)) >= 0 && nv < 64) {
        int nut = data[off] & 0x1f;
        if (nut == 7 && !sps) { sps = data + off - 3; sps_len = len + 3; }
        else if (nut == 8 && !pps) { pps = data + off - 3; pps_len = len + 3; }
        else if (nut == 1 || nut == 5) { vo[nv] = off - 3; vl[nv] = len + 3; nv++; }
        off += len;
    }
    if (!sps || !pps || nv < 12) { printf("We’re running out of yards.\n"); return 1; }

    AMediaCodec *c = AMediaCodec_createDecoderByType("video/avc");
    if (!c) { printf("Failed to build codec\n"); return 1; }
    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    if (AMediaCodec_configure(c, fmt, NULL, NULL, 0) != AMEDIA_OK) {
        printf("configure failed\n"); return 1;
    }
    AMediaCodec_start(c);

    /* CSD */
    unsigned char csd[256];
    memcpy(csd, sps, sps_len);
    memcpy(csd + sps_len, pps, pps_len);
    ssize_t bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
    size_t cap;
    uint8_t *ib = AMediaCodec_getInputBuffer(c, bi, &cap);
    memcpy(ib, csd, sps_len + pps_len);
    AMediaCodec_queueInputBuffer(c, bi, 0, sps_len + pps_len, 0, FLAG_CODEC_CONFIG);

    /* First paragraph: Send 3 frames (simulator steady state), empty */
    printf("--- Paragraph 1: Send 3  VCL Back row empty ---\n");
    for (int i = 0; i < 3; i++) {
        bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
        ib = AMediaCodec_getInputBuffer(c, bi, &cap);
        memcpy(ib, data + vo[i], vl[i]);
        AMediaCodec_queueInputBuffer(c, bi, 0, vl[i], i * 33333, 0);
    }
    int g1 = drain(c);
    printf("  Free to %d frame\n", g1);

    /* Key step: whether Flush can bring it back to life. */
    printf("--- control A:EOS After**No, no, no.** flush,Keep the feed going. ---\n");
    {
        ssize_t t = AMediaCodec_dequeueInputBuffer(c, 200000);
        printf("  dequeueInputBuffer -> %zd %s\n", t,
               t >= 0 ? "(And you can take the input.)" : "(Deny input,EOS - I’ll have to. flush)");
        if (t >= 0) {
            /* I'm sending a VCL. I can't see the frame. */
            size_t cp; uint8_t *p2 = AMediaCodec_getInputBuffer(c, t, &cp);
            memcpy(p2, data + vo[3], vl[3]);
            AMediaCodec_queueInputBuffer(c, t, 0, vl[3], 3 * 33333, 0);
            AMediaCodecBufferInfo bi2;
            ssize_t o2 = AMediaCodec_dequeueOutputBuffer(c, &bi2, 500000);
            printf("  One for you. VCL After dequeueOutput -> %zd\n", o2);
            if (o2 >= 0) AMediaCodec_releaseOutputBuffer(c, o2, false);
        }
    }

    printf("--- flush Reload CSD,Keep sending the first one. 4..9  VCL ---\n");
    media_status_t fs = AMediaCodec_flush(c);
    printf("  AMediaCodec_flush -> %d (0=OK)\n", fs);

    bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
    if (bi < 0) {
        printf("  ✗ flush Unable to get input buffer(%zd)→ EOS Irreversible. Rebuilding.\n", bi);
        return 2;
    }
    ib = AMediaCodec_getInputBuffer(c, bi, &cap);
    memcpy(ib, csd, sps_len + pps_len);
    AMediaCodec_queueInputBuffer(c, bi, 0, sps_len + pps_len, 0, FLAG_CODEC_CONFIG);

    for (int i = 3; i < 9; i++) {
        bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
        if (bi < 0) { printf("  number%d  VCL Unable to take input buffer\n", i); break; }
        ib = AMediaCodec_getInputBuffer(c, bi, &cap);
        memcpy(ib, data + vo[i], vl[i]);
        AMediaCodec_queueInputBuffer(c, bi, 0, vl[i], i * 33333, 0);
    }
    int g2 = drain(c);
    printf("  Second line empty to %d frame\n", g2);

    printf("\n=== Conclusions ===\n");
    if (fs == AMEDIA_OK && g2 > 0)
        printf("✓ EOS + flush Reversible: same codec The example can be decoded.\n"
               "  → daemon - Yes, I can.\"Do not destroy the empty session\",Save every frame of reconstruction.\n");
    else
        printf("✗ EOS And then, flush I can’t get a new frame.(g2=%d)\n"
               "  → Draining invalidates the instance, so the current rebuild path is required.\n", g2);

    AMediaCodec_stop(c);
    AMediaCodec_delete(c);
    AMediaFormat_delete(fmt);
    return 0;
}
