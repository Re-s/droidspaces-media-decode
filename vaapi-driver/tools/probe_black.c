/* Will the emptiness path produce black frames/repeated frames?
 *
 * The browser image "Middle black screen flashes" and the driver log sees the same POC recurring.
 * Two suspicious points:
 *   1) Reload CSD after empty, the decoder may eject an uninitialized/all-black buffer
 *   2) Flush discards the reference frame, and the P/B unwinding is bad
 *
 * Here goes the daemon, running at the browser rhythm (three units are sent and the frame is empty).
 * Check the average brightness of the Y plane by frame: a black frame is much lower than normal.
 * The first bytes of fingerprints of each frame are also recorded for the purpose of detecting duplicate frames.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmd_client.h"

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

/* Y plane brightness mean: Black frame close to 16 (black level for BT.601) */
static double luma_mean(const unsigned char *y, int stride, int w, int h)
{
    double sum = 0;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            sum += y[(size_t)r * stride + c];
    return sum / ((double)w * h);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/root/decode-test/test1080.h264";
    if (load(path) < 0) { fprintf(stderr, "I can’t read it. %s\n", path); return 1; }

    unsigned char *sps = NULL, *pps = NULL;
    long sps_len = 0, pps_len = 0, off = 0, len;
    long vo[400], vl[400];
    int nv = 0;
    while ((off = next_nalu(off, &len)) >= 0 && nv < 400) {
        int nut = data[off] & 0x1f;
        if (nut == 7 && !sps) { sps = data + off - 3; sps_len = len + 3; }
        else if (nut == 8 && !pps) { pps = data + off - 3; pps_len = len + 3; }
        else if (nut == 1 || nut == 5) { vo[nv] = off - 3; vl[nv] = len + 3; nv++; }
        off += len;
    }
    printf("bitstream: %d  VCL\n\n", nv);

    struct dmd_session_config cfg;
    dmd_session_config_init(&cfg);
    cfg.codec = DMD_CODEC_H264;
    cfg.width = 1920;
    cfg.height = 1088;
    struct dmd_error e;
    struct dmd_session *s = dmd_session_create(&cfg, &e);
    if (!s) { fprintf(stderr, "Create session failed: %s\n", e.msg); return 1; }

    dmd_session_send_unit(s, sps, sps_len);
    dmd_session_send_unit(s, pps, pps_len);

    int total = 0, black = 0, dup = 0;
    unsigned long long prev_sig = 0;
    int sent = 0;

    /* Simulate browser: free frame per 3 units sent */
    while (sent < nv && total < 60) {
        for (int k = 0; k < 3 && sent < nv; k++, sent++)
            dmd_session_send_unit(s, data + vo[sent], vl[sent]);

        if (dmd_session_drain(s) != DMD_OK) {
            fprintf(stderr, "Queued Failed: %s\n", dmd_session_last_error(s));
            break;
        }
        int need_rebuild = 1;   /* EOS, recreate frame after emptying */

        struct dmd_frame fr;
        while (dmd_session_next_frame(s, &fr, 2000) == DMD_OK) {
            const struct dmd_format *fmt = dmd_session_format(s);
            int stride = fmt && fmt->valid ? fmt->stride : 1920;
            int w = 1920, h = 1080;
            double mean = luma_mean(fr.data, stride, w, h);

            /* Fingerprint: take a number of points and avoid reading only the first sentence. */
            /* Fingerprints cover the entire picture. Only 64 thin spots were taken in the early version.
             * "The same" is wrongly reported for the adjacent frame, which is clearly different in intensity. */
            unsigned long long sig = 0;
            for (int r = 0; r < h; r += 8)
                for (int cc = 0; cc < w; cc += 8)
                    sig = sig * 1000003u + fr.data[(size_t)r * stride + cc];

            total++;
            int is_black = (mean < 20.0);
            int is_dup = (total > 1 && sig == prev_sig);
            if (is_black) black++;
            if (is_dup) dup++;
            if (is_black || is_dup || total <= 6)
                printf("frame%3d: Brightness Mean %6.2f %s%s\n", total, mean,
                       is_black ? " ← Black Frame!" : "", is_dup ? " ← Same as the previous frame.!" : "");
            prev_sig = sig;
            dmd_session_release_frame(s, &fr);
        }

        /* Real behavior driven: EOS after the session is empty, recreated and re-retransmitted the argument set before the next feed.
         * Continues from non-IDR frames. Do it here so that you can see the end-to-end image correctness. */
        if (need_rebuild) {
            dmd_session_destroy(s);
            s = dmd_session_create(&cfg, &e);
            if (!s) { fprintf(stderr, "Rebuilding failed: %s\n", e.msg); break; }
            dmd_session_send_unit(s, sps, sps_len);
            dmd_session_send_unit(s, pps, pps_len);

        }
    }

    dmd_session_destroy(s);
    printf("\n=== Conclusions ===\n");
    printf("Total %d Frame: Black Frame %d,Repeat Frame %d\n", total, black, dup);
    if (black == 0 && dup == 0)
        printf("✓ Draining produced no black or duplicate frames. Look elsewhere for the black screen.\n");
    else
        printf("✗ The emptied path creates the anomaly.\n");
    return 0;
}
