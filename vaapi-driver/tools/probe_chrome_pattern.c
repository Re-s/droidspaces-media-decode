/* The VA-API mode for the recurrence of Chrome failed to locate vEndPicture from the browser.
 *
 * Why it is needed: Chrome's failure only occurs in the browser, which has too many variables
 * (GPU process, sandbox, synthesizer, play rhythm) ffmpeg cannot be repeated because of its usage
 * Unlike Chrome, on two key points:
 *
 *   Chrome construct config** does not pass any properties** (ffmpeg will pass RTFormat)
 *   2. Chrome submitted a large number of decoding requests at one time, with no reference to the frame (ffmpeg strictly staggered)
 *
 * This probe turns the two points into switches, which, on a case-by-case basis, triggers the failure of vEndPicture.
 *
 * Usage:
 *   Probe_chrome_pattern <h264/h265 Naked [-no-attrib] [-burg N] [-codec hevc]
 *
 *   -no-attrib construct config without RTFormat (Chrome Behaviour)
 *   --burst N is submitted continuously to N frames before the frame is removed (default 1 = strict staggered)
 *   - ..codec hevc with HEVC (default h264)
 *
 * Determined: Only the return code and failure timing of vEndPicture/ vASyncSurface.
 *
 * ⁇  Two limitations, which must be known before reading the output:
 *
 * 1. ** No check of the contents**. It can only locate "call failure" and must not be used to determine whether the decoding is correct.
 *    Correct -- that must be a byte-to-by-the-line softening baseline.
 *    Three times.
 *
 * 2. ** This probe does not send the Set of Parameters (SPS/PPS/VPS), only VCL slice**.
 *    So the decoding device could not have been decoded properly. `vaSyncSurface failed ' , the high number is **
 *    Behaviour**, not drive defects. A typical output is "Sync Success=29 Sync Failure=59,"
 *    And that 59 failures** all came from the incompleteness of the probe itself, and see it do not look for the drive.
 *
 *    The only meaningful observation of this probe is the return code and the first failed position of **vaEndPicture** —
 *    It reflects the behaviour of the internal state-driven machine (queuing capacity, back pressure) and is not relevant to the image.
 *    To verify the decoding correct, use ffmpeg to match bytes, do not use this.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>

static const char *st(VAStatus s)
{
    return vaErrorStr(s);
}

/* Find a next Annex-B startup, returns NALU start and length */
static int next_nalu(const uint8_t *b, size_t len, size_t *pos,
                     const uint8_t **out, size_t *out_len)
{
    size_t i = *pos;
    /* Jump to Start Code */
    while (i + 3 < len) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
            break;
        i++;
    }
    if (i + 3 >= len)
        return 0;
    size_t start = i + 3;
    size_t j = start;
    while (j + 3 < len) {
        if (b[j] == 0 && b[j + 1] == 0 && b[j + 2] == 1)
            break;
        j++;
    }
    if (j + 3 >= len)
        j = len;
    *out = b + start;
    *out_len = j - start;
    *pos = j;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <Naked.> [--no-attrib] [--burst N] [--codec hevc]\n",
                argv[0]);
        return 2;
    }

    const char *path = argv[1];
    int no_attrib = 0, burst = 1, is_hevc = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--no-attrib"))
            no_attrib = 1;
        else if (!strcmp(argv[i], "--burst") && i + 1 < argc)
            burst = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--codec") && i + 1 < argc)
            is_hevc = !strcmp(argv[++i], "hevc");
    }
    if (burst < 1)
        burst = 1;

    printf("Mode: %s Properties / burst=%d / codec=%s\n",
           no_attrib ? "No, no, no." : "Pass.", burst, is_hevc ? "hevc" : "h264");

    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)fsz);
    if (!buf || fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) {
        fprintf(stderr, "Failed to read file\n"); return 1;
    }
    fclose(f);

    int drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("open renderD128"); return 1; }

    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) { fprintf(stderr, "vaGetDisplayDRM failed\n"); return 1; }

    int major, minor;
    VAStatus s = vaInitialize(dpy, &major, &minor);
    if (s != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaInitialize: %s\n", st(s)); return 1;
    }
    printf("VA %d.%d  vendor=%s\n", major, minor, vaQueryVendorString(dpy));

    VAProfile prof = is_hevc ? VAProfileHEVCMain : VAProfileH264High;

    /* - Key difference 1: no properties to pass on when building config */
    VAConfigID cfg;
    if (no_attrib) {
        s = vaCreateConfig(dpy, prof, VAEntrypointVLD, NULL, 0, &cfg);
    } else {
        VAConfigAttrib a = { .type = VAConfigAttribRTFormat,
                             .value = VA_RT_FORMAT_YUV420 };
        s = vaCreateConfig(dpy, prof, VAEntrypointVLD, &a, 1, &cfg);
    }
    if (s != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaCreateConfig: %s\n", st(s)); return 1;
    }

    /* Reproduce Chrome querying the entry points advertised by the driver. */
    {
        VAConfigAttrib got[16];
        int n = 16;
        VAProfile p2; VAEntrypoint e2;
        s = vaQueryConfigAttributes(dpy, cfg, &p2, &e2, got, &n);
        printf("QueryConfigAttributes: %s  return %d Properties",
               st(s), s == VA_STATUS_SUCCESS ? n : 0);
        if (s == VA_STATUS_SUCCESS) {
            int has_rt = 0;
            for (int i = 0; i < n; i++)
                if (got[i].type == VAConfigAttribRTFormat) {
                    has_rt = 1;
                    printf("  RTFormat=0x%x", got[i].value);
                }
            printf("  %s\n", has_rt ? "(Ham RTFormat ✓)" : "(Not available RTFormat ✗)");
        } else {
            printf("\n");
        }
    }

    const int W = 1920, H = 1080;
    const int NSURF = 32;
    VASurfaceID surf[NSURF];
    s = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, surf, NSURF, NULL, 0);
    if (s != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaCreateSurfaces: %s\n", st(s)); return 1;
    }

    VAContextID ctx;
    s = vaCreateContext(dpy, cfg, W, H, VA_PROGRESSIVE, surf, NSURF, &ctx);
    if (s != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaCreateContext: %s\n", st(s)); return 1;
    }

    /* - Key difference 2: continuously submitting burst frame before removing frame - */
    size_t pos = 0;
    const uint8_t *nalu; size_t nlen;
    int submitted = 0, synced = 0, endpic_fail = 0, sync_fail = 0;
    int inflight[NSURF]; int nin = 0;
    VAStatus first_fail = VA_STATUS_SUCCESS;
    int first_fail_at = -1;

    while (next_nalu(buf, (size_t)fsz, &pos, &nalu, &nlen)) {
        if (nlen == 0)
            continue;
        int nut = is_hevc ? ((nalu[0] >> 1) & 0x3f) : (nalu[0] & 0x1f);
        /* Submission only VCL: h264 1.5 /hevc 0.31 */
        int is_vcl = is_hevc ? (nut <= 31) : (nut >= 1 && nut <= 5);
        if (!is_vcl)
            continue;

        VASurfaceID tgt = surf[submitted % NSURF];

        s = vaBeginPicture(dpy, ctx, tgt);
        if (s != VA_STATUS_SUCCESS) {
            printf("vaBeginPicture number %d Frame Failed: %s\n", submitted, st(s));
            break;
        }

        /* Send a slick data buffer (the probe is not important, it's only for call success) */
        VABufferID bid;
        s = vaCreateBuffer(dpy, ctx, VASliceDataBufferType,
                           (unsigned)nlen, 1, (void *)nalu, &bid);
        if (s == VA_STATUS_SUCCESS)
            vaRenderPicture(dpy, ctx, &bid, 1);

        s = vaEndPicture(dpy, ctx);
        if (s != VA_STATUS_SUCCESS) {
            endpic_fail++;
            if (first_fail_at < 0) {
                first_fail = s;
                first_fail_at = submitted;
                printf("★ vaEndPicture First failed %d frame: %s\n",
                       submitted, st(s));
            }
            if (endpic_fail > 5) {
                printf("Too many failures. Stop.\n");
                break;
            }
        } else {
            if (nin < NSURF)
                inflight[nin++] = submitted % NSURF;
        }
        submitted++;

        /* We'll have to save enough for the frame. */
        if (nin >= burst) {
            for (int k = 0; k < nin; k++) {
                VAStatus ss = vaSyncSurface(dpy, surf[inflight[k]]);
                if (ss != VA_STATUS_SUCCESS) {
                    sync_fail++;
                    if (sync_fail <= 3)
                        printf("  vaSyncSurface failed: %s\n", st(ss));
                } else {
                    synced++;
                }
            }
            nin = 0;
        }

        if (submitted >= 300)
            break;
    }

    printf("\n result: Submit=%d Sync successfully=%d EndPicture failed=%d Sync failed=%d\n",
           submitted, synced, endpic_fail, sync_fail);
    if (first_fail_at >= 0)
        printf("First EndPicture failed: number %d frame, %s\n",
               first_fail_at, st(first_fail));
    else
        printf("EndPicture No failure at all.\n");

    vaDestroyContext(dpy, ctx);
    vaDestroySurfaces(dpy, surf, NSURF);
    vaDestroyConfig(dpy, cfg);
    vaTerminate(dpy);
    close(drm_fd);
    free(buf);
    return endpic_fail ? 1 : 0;
}
