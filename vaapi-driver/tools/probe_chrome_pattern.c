/* 复现 Chrome 的 VA-API 使用模式，脱离浏览器定位 vaEndPicture 失败。
 *
 * 为什么需要它：Chrome 的失败只在浏览器里出现，而浏览器有太多变量
 * （GPU 进程、沙箱、合成器、播放节奏）。ffmpeg 复现不了，因为它的用法
 * 和 Chrome 在两个关键点上不同：
 *
 *   1. Chrome 建 config **不传任何属性**（ffmpeg 会传 RTFormat）
 *   2. Chrome 一次性提交大批解码请求，不等取帧（ffmpeg 严格交错）
 *
 * 本探针把这两点分别做成开关，逐个验证哪一个触发 vaEndPicture 失败。
 *
 * 用法：
 *   probe_chrome_pattern <h264/h265 裸流> [--no-attrib] [--burst N] [--codec hevc]
 *
 *   --no-attrib   建 config 时不传 RTFormat（Chrome 行为）
 *   --burst N     连续提交 N 帧后才开始取帧（默认 1 = 严格交错）
 *   --codec hevc  用 HEVC（默认 h264）
 *
 * 判定：只看 vaEndPicture / vaSyncSurface 的返回码与失败时机。
 * ⚠️ 本探针**不检查画面内容**，所以它只能定位"调用失败"，
 * 绝不能用它判断解码是否正确 —— 那必须逐字节比对软解基线。
 * （本项目已因"只数帧数不看像素"栽过三次。）
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

/* 找下一个 Annex-B 起始码，返回 NALU 起点与长度 */
static int next_nalu(const uint8_t *b, size_t len, size_t *pos,
                     const uint8_t **out, size_t *out_len)
{
    size_t i = *pos;
    /* 跳到起始码 */
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
        fprintf(stderr, "用法: %s <裸流> [--no-attrib] [--burst N] [--codec hevc]\n",
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

    printf("模式: %s属性 / burst=%d / codec=%s\n",
           no_attrib ? "不传" : "传", burst, is_hevc ? "hevc" : "h264");

    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)fsz);
    if (!buf || fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) {
        fprintf(stderr, "读文件失败\n"); return 1;
    }
    fclose(f);

    int drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("open renderD128"); return 1; }

    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) { fprintf(stderr, "vaGetDisplayDRM 失败\n"); return 1; }

    int major, minor;
    VAStatus s = vaInitialize(dpy, &major, &minor);
    if (s != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaInitialize: %s\n", st(s)); return 1;
    }
    printf("VA %d.%d  vendor=%s\n", major, minor, vaQueryVendorString(dpy));

    VAProfile prof = is_hevc ? VAProfileHEVCMain : VAProfileH264High;

    /* ---- 关键差异 1：建 config 时传不传属性 ---- */
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

    /* Chrome 会回查驱动声明了什么 —— 复现这一步 */
    {
        VAConfigAttrib got[16];
        int n = 16;
        VAProfile p2; VAEntrypoint e2;
        s = vaQueryConfigAttributes(dpy, cfg, &p2, &e2, got, &n);
        printf("QueryConfigAttributes: %s  返回 %d 个属性",
               st(s), s == VA_STATUS_SUCCESS ? n : 0);
        if (s == VA_STATUS_SUCCESS) {
            int has_rt = 0;
            for (int i = 0; i < n; i++)
                if (got[i].type == VAConfigAttribRTFormat) {
                    has_rt = 1;
                    printf("  RTFormat=0x%x", got[i].value);
                }
            printf("  %s\n", has_rt ? "（含 RTFormat ✓）" : "（缺 RTFormat ✗）");
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

    /* ---- 关键差异 2：连续提交 burst 帧后才取帧 ---- */
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
        /* 只提交 VCL：h264 1..5 / hevc 0..31 */
        int is_vcl = is_hevc ? (nut <= 31) : (nut >= 1 && nut <= 5);
        if (!is_vcl)
            continue;

        VASurfaceID tgt = surf[submitted % NSURF];

        s = vaBeginPicture(dpy, ctx, tgt);
        if (s != VA_STATUS_SUCCESS) {
            printf("vaBeginPicture 第 %d 帧失败: %s\n", submitted, st(s));
            break;
        }

        /* 送一个 slice data buffer（内容不重要，本探针只看调用成败） */
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
                printf("★ vaEndPicture 首次失败于第 %d 帧: %s\n",
                       submitted, st(s));
            }
            if (endpic_fail > 5) {
                printf("失败过多，停止\n");
                break;
            }
        } else {
            if (nin < NSURF)
                inflight[nin++] = submitted % NSURF;
        }
        submitted++;

        /* 攒够 burst 才取帧 */
        if (nin >= burst) {
            for (int k = 0; k < nin; k++) {
                VAStatus ss = vaSyncSurface(dpy, surf[inflight[k]]);
                if (ss != VA_STATUS_SUCCESS) {
                    sync_fail++;
                    if (sync_fail <= 3)
                        printf("  vaSyncSurface 失败: %s\n", st(ss));
                } else {
                    synced++;
                }
            }
            nin = 0;
        }

        if (submitted >= 300)
            break;
    }

    printf("\n结果: 提交=%d 同步成功=%d EndPicture失败=%d Sync失败=%d\n",
           submitted, synced, endpic_fail, sync_fail);
    if (first_fail_at >= 0)
        printf("首次 EndPicture 失败: 第 %d 帧, %s\n",
               first_fail_at, st(first_fail));
    else
        printf("EndPicture 全程无失败\n");

    vaDestroyContext(dpy, ctx);
    vaDestroySurfaces(dpy, surf, NSURF);
    vaDestroyConfig(dpy, cfg);
    vaTerminate(dpy);
    close(drm_fd);
    free(buf);
    return endpic_fail ? 1 : 0;
}
