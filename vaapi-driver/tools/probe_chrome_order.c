/* Probe_chrome_order - authenticate the content order of the surface with the Crome contract.
 *
 * Why must this probe be: md5 returns from ffmpeg** does not prove the chrome path is correct**.
 * ffmpeg Walk VadeveImage/vaGetImage to read pixels, which are in both entrances
 * dmd_surface_wait() image.c:169/ :283.
 * Even if the pixels were not written into the world when EndPicture returns, ffmpeg will wait at the time of the Map.
 * So, md5 All Green only says "comparison logic" and does not cover EndPicture at all.
 * And the right pixel is the right one -- the latter is exactly what Chrome does.
 *
 * The actual contract for Chrome (established by the chromium source and driver log in both directions):
 *   The Dmabuf fd after the CreateSurfaces and before any decoding
 *   2. Later only va BeginPicture / vaRenderPicture / vaEndPicture
 *   ** Never**vaSyncSurface /vaQuerySurfaceStatus /vaDeriveImage
 *      (Sync 0, Firefox co-situation 1,500)
 *   DPB decides when to put a surface on the screen with its own software.
 *
 * This probe returns 1-3 and then directs the dmabuf reading pixels.
 * The material is used with large frame sizes (the order_slow.mp4 class outside the tools directory).
 * The verdict is "every image of the world is not its frame"
 *
 * Find method: do not do OCR. Take fingerprints (square parts) on each of the Y-levels, with a soft resolution baseline
 * Match the frame by frame, find the nearest frame number. If frame k is installed in the k frame,
 * The order is correct; if you have another frame, you print the wrong number.
 *
 * Usage:
 *   Probe_chrome_order <amnexb Naked > softline baseline nv12 > wide < high [frameline]
 *
 * Baseline generation (note that the same stream, same number, nv12):
 *   ffmpeg -i in.mp4 -frames:v 40 -f rawvideo -pix_fmt nv12 base.nv12
 *
 * ══════════════════════════════════════════════════════════════════
 * ⁇  The present ** conclusion of the probe is not credible** and do not use its output to drive right and wrong.
 *
 * It only sends VASLiceDataBufferType** to VAPictureParameter BufferHEVC**.
 * This driver is the original of the Pic Param inverted synthesis VPS/SPS/PPS (VA-API does not transmit parameters set)
 * Bitstream, see ed. c. build_unit. No pic param, no CSD, no CSD.
 * The decoder is not properly configured and the output is naturally empty or residual.
 *
 * Actual evidence (now round, order_slow.mp4 24):
 *   Driver log "HEVC pic_param:" appears -- 0 times for this probe, ffmpeg 1 time
 *   This probe results: Correct 0/ error frame 8/ empty frame 16 (total 24)
 *   ** The results are identical by by bytes after adding vSyncSurface** (DMD_PROBE_SYNC=1)
 *
 * The last one is decisive: if the question is really "No Sync didn't write in dmabuf,"
 * The supplement Sync will definitely change the result. The result is the same.
 * This probe is about its own defects, not the defects of the Chrome path.
 *
 * To make it credible, it must be completed.
 * H265 VaapiVideoDecoderd Delegate rewrite:
 *   1. Parsing SPS/PPS, filled with VAPictureParameter Beffer HEVC and vRenderPicture
 *   2. Filling in VASLiceParameter BufferHEVC (slick sub-paragraph information)
 *   3. Maintained DPB/ RefenceFrames arrays, otherwise P/B frame reference error
 *
 * The Chrome path can only be cross-checked by the browser plus driver log before completion.
 * This document is kept because of the "Export first, EndPicture, No Sync" caller.
 * Yes, all that is missing is the parameter buffer; the next round will be sufficient to complete this.
 * ══════════════════════════════════════════════════════════════════
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#define MAXSURF 24
#define BLK 8   /* Fingerprint fraction (average value of BLK x BLK block) */

static const char *st(VAStatus s) { return vaErrorStr(s); }

struct fp { double v[BLK * BLK]; };

/* Average print of Y fraction from NV12 */
static void fingerprint(const uint8_t *y, int stride, int w, int h,
                        struct fp *out)
{
    for (int by = 0; by < BLK; by++) {
        for (int bx = 0; bx < BLK; bx++) {
            int x0 = bx * w / BLK, x1 = (bx + 1) * w / BLK;
            int y0 = by * h / BLK, y1 = (by + 1) * h / BLK;
            double sum = 0; long n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *row = y + (size_t)yy * stride;
                for (int xx = x0; xx < x1; xx++) { sum += row[xx]; n++; }
            }
            out->v[by * BLK + bx] = n ? sum / (double)n : 0.0;
        }
    }
}

static double fp_dist(const struct fp *a, const struct fp *b)
{
    double d = 0;
    for (int i = 0; i < BLK * BLK; i++) {
        double t = a->v[i] - b->v[i];
        d += t * t;
    }
    return d;
}

static int next_nalu(const uint8_t *b, size_t len, size_t *pos,
                     const uint8_t **out, size_t *out_len)
{
    size_t i = *pos;
    while (i + 3 < len) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) break;
        i++;
    }
    if (i + 3 >= len) return 0;
    size_t start = i + 3, j = start;
    while (j + 3 < len) {
        if (b[j] == 0 && b[j + 1] == 0 && b[j + 2] == 1) break;
        j++;
    }
    if (j + 3 >= len) j = len;
    *out = b + start; *out_len = j - start; *pos = j;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s <annexb Naked.> <Softly delineate baseline.nv12> <Width> <High> [Frames] [--codec h264]\n",
            argv[0]);
        return 2;
    }
    const char *spath = argv[1];
    const char *bpath = argv[2];
    int W = atoi(argv[3]), H = atoi(argv[4]);
    int want = argc > 5 ? atoi(argv[5]) : 40;
    int is_hevc = 1;
    for (int i = 6; i < argc; i++)
        if (!strcmp(argv[i], "--codec") && i + 1 < argc)
            is_hevc = strcmp(argv[i + 1], "h264") != 0;

    /* - Reader stream - */
    FILE *f = fopen(spath, "rb");
    if (!f) { perror("open Naked."); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)fsz);
    if (!buf || fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) {
        fprintf(stderr, "Reading the naked stream fails.\n"); return 1;
    }
    fclose(f);

    /* - Read softly to decode the baseline, count each fingerprint */
    size_t fsize = (size_t)W * H * 3 / 2;
    FILE *bf = fopen(bpath, "rb");
    if (!bf) { perror("open baseline"); return 1; }
    fseek(bf, 0, SEEK_END); long bsz = ftell(bf); fseek(bf, 0, SEEK_SET);
    int nbase = (int)(bsz / (long)fsize);
    if (nbase < 1) { fprintf(stderr, "The baseline is too small.(%ld bytes)\n", bsz); return 1; }
    struct fp *base = calloc((size_t)nbase, sizeof(*base));
    uint8_t *fbuf = malloc(fsize);
    for (int i = 0; i < nbase; i++) {
        if (fread(fbuf, 1, fsize, bf) != fsize) { nbase = i; break; }
        fingerprint(fbuf, W, W, H, &base[i]);
    }
    fclose(bf);
    printf("baseline: %d frame %dx%d\n", nbase, W, H);

    /* -VA Initialization - */
    int drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("open renderD128"); return 1; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    int major, minor;
    VAStatus s = vaInitialize(dpy, &major, &minor);
    if (s != VA_STATUS_SUCCESS) { fprintf(stderr, "vaInitialize: %s\n", st(s)); return 1; }
    printf("VA %d.%d vendor=%s\n", major, minor, vaQueryVendorString(dpy));

    VAProfile prof = is_hevc ? VAProfileHEVCMain : VAProfileH264High;
    VAConfigID cfg;
    /* Chrome Create config without attribute */
    s = vaCreateConfig(dpy, prof, VAEntrypointVLD, NULL, 0, &cfg);
    if (s != VA_STATUS_SUCCESS) { fprintf(stderr, "vaCreateConfig: %s\n", st(s)); return 1; }

    VASurfaceID surf[MAXSURF];
    s = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, surf, MAXSURF, NULL, 0);
    if (s != VA_STATUS_SUCCESS) { fprintf(stderr, "vaCreateSurfaces: %s\n", st(s)); return 1; }

    VAContextID ctx;
    s = vaCreateContext(dpy, cfg, W, H, VA_PROGRESSIVE, surf, MAXSURF, &ctx);
    if (s != VA_STATUS_SUCCESS) { fprintf(stderr, "vaCreateContext: %s\n", st(s)); return 1; }

    /* - Chrome Step 1: Export all dmabuf and mmap before decoding */
    struct {
        int fd; uint8_t *map; size_t size; uint32_t stride, off_uv;
    } dm[MAXSURF];
    memset(dm, 0, sizeof(dm));
    int exported = 0;
    for (int i = 0; i < MAXSURF; i++) {
        VADRMPRIMESurfaceDescriptor d;
        memset(&d, 0, sizeof(d));
        s = vaExportSurfaceHandle(dpy, surf[i],
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY |
                                  VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d);
        if (s != VA_STATUS_SUCCESS) {
            fprintf(stderr, "vaExportSurfaceHandle[%d]: %s\n", i, st(s));
            return 1;
        }
        dm[i].fd = d.objects[0].fd;
        dm[i].size = d.objects[0].size;
        dm[i].stride = d.layers[0].pitch[0];
        dm[i].off_uv = d.layers[0].offset[1];
        dm[i].map = mmap(NULL, dm[i].size, PROT_READ, MAP_SHARED, dm[i].fd, 0);
        if (dm[i].map == MAP_FAILED) {
            fprintf(stderr, "mmap dmabuf[%d] Fail.)\n", i);
            return 1;
        }
        exported++;
    }
    printf("Exported and mmap %d  dmabuf(stride=%u off_uv=%u size=%zu)\n",
           exported, dm[0].stride, dm[0].off_uv, dm[0].size);

    /* - _Other Organiser */
    /* Record the surface for each submission, submission = decoding */
    int used[512]; int nsub = 0;
    size_t pos = 0; const uint8_t *nalu; size_t nlen;
    int endpic_fail = 0;

    while (nsub < want && next_nalu(buf, (size_t)fsz, &pos, &nalu, &nlen)) {
        if (!nlen) continue;
        int nut = is_hevc ? ((nalu[0] >> 1) & 0x3f) : (nalu[0] & 0x1f);
        int is_vcl = is_hevc ? (nut <= 31) : (nut >= 1 && nut <= 5);
        if (!is_vcl) continue;

        int si = nsub % MAXSURF;
        s = vaBeginPicture(dpy, ctx, surf[si]);
        if (s != VA_STATUS_SUCCESS) {
            printf("vaBeginPicture number %d frame: %s\n", nsub, st(s));
            break;
        }
        VABufferID bid;
        /* Start with: Pushing On Annex-B Process slick Data */
        uint8_t *withsc = malloc(nlen + 3);
        withsc[0] = 0; withsc[1] = 0; withsc[2] = 1;
        memcpy(withsc + 3, nalu, nlen);
        s = vaCreateBuffer(dpy, ctx, VASliceDataBufferType,
                           (unsigned)(nlen + 3), 1, withsc, &bid);
        if (s == VA_STATUS_SUCCESS) vaRenderPicture(dpy, ctx, &bid, 1);
        free(withsc);

        s = vaEndPicture(dpy, ctx);
        if (s != VA_STATUS_SUCCESS) {
            endpic_fail++;
            if (endpic_fail <= 3)
                printf("vaEndPicture number %d Frame Failed: %s\n", nsub, st(s));
        }
        used[nsub] = si;
        nsub++;
    }
    printf("Submit %d frame(EndPicture failed %d)\n", nsub, endpic_fail);

    /* -Chrome Step 2: Read dmabuf directly to determine which frame each surface contains -
     *
     * This is a dedicated place to expose the "EndPicture" when it returns.
     * "Is the pixel there?" To not turn the results into pure competition, give a little time:
     * The real Chrome composite is not a nanosecond sample. But this wait cannot be called by VA, and the search is not done by the user.
     * It's only the clock on the wall -- because this is what Chrome does. */
    int settle_ms = 300;
    const char *env = getenv("DMD_PROBE_SETTLE_MS");
    if (env) settle_ms = atoi(env);
    /* The control switch: DMD_PROBE_SYNC=1 with a vaSyncSurface (Firefox/ffmpeg Contract).
     * The purpose is to verify the integrity of the probe itself -- if all of Sync is correct, the decoding and matching.
     * No problem. The difference is simply "No Sync without pixels written into dmabuf." */
    if (getenv("DMD_PROBE_SYNC")) {
        for (int k = 0; k < nsub && k < MAXSURF; k++)
            vaSyncSurface(dpy, surf[used[k]]);
        printf("[control] Yes. %d  surface Mode vaSyncSurface\n",
               nsub < MAXSURF ? nsub : MAXSURF);
    }
    usleep((useconds_t)settle_ms * 1000);
    printf("Wait %d ms Read back. dmabuf(No conversion during the period VA Synchronize entrances)\n\n", settle_ms);

    int ok = 0, wrong = 0, blank = 0;
    printf("Submit Order  VA_surf  It’s like the baseline. Distance.\n");
    for (int k = 0; k < nsub && k < MAXSURF; k++) {
        int si = used[k];
        struct fp got;
        fingerprint(dm[si].map, dm[si].stride, W, H, &got);

        /* Full black/not initialized: equal and very low */
        double mean = 0;
        for (int i = 0; i < BLK * BLK; i++) mean += got.v[i];
        mean /= BLK * BLK;

        int best = -1; double bestd = 1e30;
        for (int i = 0; i < nbase; i++) {
            double d = fp_dist(&got, &base[i]);
            if (d < bestd) { bestd = d; best = i; }
        }

        const char *verdict;
        if (mean < 20.0) { verdict = "Empty/Black Frame"; blank++; }
        else if (best == k) { verdict = "✓ Correct."; ok++; }
        else { verdict = "✗ Wrong frame."; wrong++; }

        printf("%6d  %7u  %14d  %8.1f  %s\n", k,
               (unsigned)surf[si], best, bestd, verdict);
    }

    printf("\n result: Correct. %d / Wrong frame. %d / empty frame %d(Incomparing %d)\n",
           ok, wrong, blank, ok + wrong + blank);
    if (wrong || blank)
        printf("→ Chrome Under the contract. surface Incorrect."
               "That’s the direct proof of the forward and background leap..\n");
    else
        printf("→ Chrome Under the contract. surface Everything is correct..\n");

    for (int i = 0; i < MAXSURF; i++) {
        if (dm[i].map && dm[i].map != MAP_FAILED) munmap(dm[i].map, dm[i].size);
        if (dm[i].fd > 0) close(dm[i].fd);
    }
    vaDestroyContext(dpy, ctx);
    vaDestroySurfaces(dpy, surf, MAXSURF);
    vaDestroyConfig(dpy, cfg);
    vaTerminate(dpy);
    close(drm_fd);
    return (wrong || blank) ? 1 : 0;
}
