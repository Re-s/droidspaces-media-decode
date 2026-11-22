/* Measuring the real output of MediaCodec in low-delay mode.
 *
 * Method: One VCL unit per unit, each delivered with a frame to be tested at 800 ms per hour.
 * The record says "The first frame comes out after the first units are in." That's the number of the lag depths.
 * The ffmpeg steady state in the browser is only 3 frames in flight, so the delay > = 4 is locked dead.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmd_client.h"

static unsigned char *buf;
static long buflen;

static int load(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); buflen = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(buflen);
    if (fread(buf, 1, buflen, f) != (size_t)buflen) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* Find a new start, return to NALU starting (including startup)*/
static long next_sc(long from)
{
    for (long i = from; i + 3 < buflen; i++) {
        if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1) return i;
        if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1) return i;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/root/decode-test/test1080.h264";
    if (load(path) < 0) { fprintf(stderr, "I can’t read it. %s\n", path); return 1; }

    struct dmd_session_config cfg;
    dmd_session_config_init(&cfg);
    cfg.codec = DMD_CODEC_H264;
    cfg.width = 1920; cfg.height = 1080;
    struct dmd_error err;
    memset(&err, 0, sizeof(err));
    struct dmd_session *s = dmd_session_create(&cfg, &err);
    if (!s) { fprintf(stderr, "Session Create Failed code=%d\n", err.code); return 1; }
    int rc;

    int sent = 0, first_frame_after = -1, frames = 0;
    long off = next_sc(0);
    while (off >= 0 && sent < 12) {
        long end = next_sc(off + 3);
        long len = (end < 0 ? buflen : end) - off;
        /* Skip Start-Up NAL Header Type */
        long sc = (buf[off+2] == 1) ? 3 : 4;
        int nut = buf[off + sc] & 0x1f;

        if (nut == 1 || nut == 5) {   /* Send VCL only, simulate drive behavior */
            rc = dmd_session_send_unit(s, buf + off, len);
            if (rc != DMD_OK) { fprintf(stderr, "Sending module failed rc=%d\n", rc); break; }
            sent++;

            struct dmd_frame f;
            memset(&f, 0, sizeof(f));
            rc = dmd_session_next_frame(s, &f, 800);
            if (rc == DMD_OK) {
                frames++;
                if (first_frame_after < 0) first_frame_after = sent;
                printf("Send the first one to the first. %d  VCL Then take frame (cumulative) %d frame)\n", sent, frames);
                dmd_session_release_frame(s, &f);
            } else {
                printf("Send the first one to the first. %d  VCL After 800ms No frame inside. (rc=%d)\n", sent, rc);
            }
        } else {
            /* Parameter set: The driver is synthesized and sent separately, and here is also sent to the real */
            dmd_session_send_unit(s, buf + off, len);
        }
        off = end;
    }

    printf("\n=== Conclusions ===\n");
    printf("The frame is in the first place. %d  VCL After\n", first_frame_after);
    printf("Latitude = %d\n", first_frame_after);
    printf("Browser ffmpeg Steady. 3 Frame: Backward <=3 And don’t die of locking.,>=4 Damn lock.\n");
    printf("Decision:%s\n", (first_frame_after > 0 && first_frame_after <= 3)
                            ? "✓ It’s not locked." : "✗ Still locks.");

    dmd_session_destroy(s);
    free(buf);
    return 0;
}
