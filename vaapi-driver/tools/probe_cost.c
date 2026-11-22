/* How expensive is "communiqué re-establishment" once?
 *
 * Background: Each frame triggers a flash + reconstruction under the browser path (147 frames x 147).
 * The function is correct and fast enough (Firefox self-reported average 7.4ms/frames, frames intervals 33.3ms),
 * But before you move the daemon, you need to know the actual amount of the expenses...
 * If reconstruction is only a small part of the single-frame budget, then the gains from the change are limited.
 *
 * Directly lined daemon to measure three periods of time:
 *   1) build Session (cont + handshake + confuture + start)
 *   2) Send Parameter Set
 *   3) Flush (finish_input) to get the frame
 * It's timed out with "Step a VCL to get a frame down."
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dmd_client.h"

static unsigned char *data;
static long dlen;

static int load(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    dlen = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(dlen);
    if (fread(data, 1, dlen, f) != (size_t)dlen) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* Find next start code, returns the NALU starting point,*len is length (excluding next start code) */
static long next_nalu(long from, long *len)
{
    long i = from;
    while (i + 3 < dlen) {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            long s = i + 3;
            long j = s;
            while (j + 3 < dlen) {
                if (data[j] == 0 && data[j+1] == 0 && data[j+2] == 1) break;
                j++;
            }
            *len = (j + 3 >= dlen) ? dlen - s : j - s;
            return s;
        }
        i++;
    }
    return -1;
}

static double ms_since(struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000.0 +
           (t1.tv_nsec - t0->tv_nsec) / 1e6;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/root/decode-test/test1080.h264";
    if (load(path) < 0) { fprintf(stderr, "I can’t read it. %s\n", path); return 1; }

    /* Collecting SPS/PPS and previous VCLs */
    unsigned char *sps = NULL, *pps = NULL;
    long sps_len = 0, pps_len = 0;
    long off = 0, len;
    long vcl_off[64];
    long vcl_len[64];
    int nvcl = 0;

    while ((off = next_nalu(off, &len)) >= 0 && nvcl < 64) {
        int nut = data[off] & 0x1f;
        /* The delivery must be with the Annex B starting code (dmd_clit.h explicitly requested), which is a very important part of the story.
         * Next_nulu returns the payload starting point, so returns 3 bytes, length +3. */
        if (nut == 7 && !sps) { sps = data + off - 3; sps_len = len + 3; }
        else if (nut == 8 && !pps) { pps = data + off - 3; pps_len = len + 3; }
        else if (nut == 1 || nut == 5) { vcl_off[nvcl] = off - 3; vcl_len[nvcl] = len + 3; nvcl++; }
        off += len;
    }
    if (!sps || !pps || nvcl < 8) {
        fprintf(stderr, "The stream is not enough. SPS/PPS/VCL\n");
        return 1;
    }
    printf("bitstream: SPS %ld bytes, PPS %ld bytes, Collection %d  VCL\n\n",
           sps_len, pps_len, nvcl);

    struct dmd_error e;
    struct timespec t0;
    double t_open = 0, t_param = 0, t_flush = 0;
    struct dmd_frame fr;

    /* - First paragraph: construction session - */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct dmd_session_config cfg;
    dmd_session_config_init(&cfg);
    cfg.codec = DMD_CODEC_H264;
    cfg.width = 1920;
    cfg.height = 1088;
    struct dmd_session *s = dmd_session_create(&cfg, &e);
    t_open = ms_since(&t0);
    if (!s) { fprintf(stderr, "Create session failed code=%d %s\n", e.code, e.msg); return 1; }
    printf("① Build Session(connect+Shake hands.+configure+start): %.1f ms\n", t_open);

    /* - Second paragraph: send the argument set - */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    dmd_session_send_unit(s, sps, sps_len);
    dmd_session_send_unit(s, pps, pps_len);
    t_param = ms_since(&t0);
    printf("② Send SPS+PPS: %.1f ms\n", t_param);

    /* - Simulate browser: only 3 VCLs sent, then Flush takes frames - */
    for (int i = 0; i < 3; i++)
        dmd_session_send_unit(s, data + vcl_off[i], vcl_len[i]);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    dmd_session_finish_input(s);
    int got = 0;
    double t_first = -1.0;
    while (dmd_session_next_frame(s, &fr, 3000) == DMD_OK) {
        if (got == 0) t_first = ms_since(&t0);   /* Only the frame affects the delay. */
        got++;
        dmd_session_release_frame(s, &fr);
    }
    t_flush = ms_since(&t0);
    printf("③ flush → Homeframe: %.1f ms;Take the full piece. %d Frame (Equal) EOS): %.1f ms\n",
           t_first, got, t_flush);
    /* Key: Next time you re-establish the path, Flush, you get multiple frames, the costs are all set up.
     * The flow rate is "How much per frame" and not the total time of a flash. */
    printf("   → I’ll put it all in the frame.: %.1f ms\n", got ? t_flush / got : 0.0);
    t_flush = t_first;   /* Follow up the aggregation with a lead frame delay, avoid waiting for EOS to be charged to single frame costs */
    dmd_session_destroy(s);

    printf("\n Total cost of single reconstruction ≈ %.1f ms(①+②+③)\n", t_open + t_param + t_flush);

    /* - Contrast: The steady state of successive deliveries in a session takes time - */
    s = dmd_session_create(&cfg, &e);
    if (!s) { fprintf(stderr, "The second session failed.\n"); return 1; }
    dmd_session_send_unit(s, sps, sps_len);
    dmd_session_send_unit(s, pps, pps_len);
    /* Fill the water line first. */
    for (int i = 0; i < 4; i++)
        dmd_session_send_unit(s, data + vcl_off[i], vcl_len[i]);
    int warm = 0;
    while (warm < 1 && dmd_session_next_frame(s, &fr, 3000) == DMD_OK) { warm++; dmd_session_release_frame(s, &fr); }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    int steady = 0;
    for (int i = 4; i < nvcl && steady < 4; i++) {
        dmd_session_send_unit(s, data + vcl_off[i], vcl_len[i]);
        if (dmd_session_next_frame(s, &fr, 3000) == DMD_OK) { steady++; dmd_session_release_frame(s, &fr); }
    }
    double t_steady = ms_since(&t0);
    printf("Contrast: Stable down %d  VCL Every one. 1 frame: %.1f ms(%.1f ms/frame)\n",
           steady, t_steady, steady ? t_steady / steady : 0.0);
    dmd_session_destroy(s);

    printf("\n=== Judgement ===\n");
    printf("Build Session+Set of parameters = %.1f ms,flush→Homeframe = %.1f ms\n",
           t_open + t_param, t_flush);
    printf("Comparison to Steady %.1f ms/frames,frame budgets 33.3 ms(30fps)\n",
           steady ? t_steady / steady : 0.0);
    printf("\n Note: Once. flush We can get multiple frames, and the cost of rebuilding is spread out..\n");
    printf("Is it worth changing? daemon,Depends on whether the frame rate is enough. ——\n");
    printf("Browser Actual 143 The frame is finished at normal speed.,Firefox Self-reporting 7.4 ms/frame,\n");
    printf("The current path is filled with vomit. 30fps.\n");
    return 0;
}
