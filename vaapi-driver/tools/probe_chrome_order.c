/* probe_chrome_order —— 用 Chrome 的契约验证 surface 内容顺序。
 *
 * 为什么必须有这个探针：ffmpeg 的 md5 回归**证明不了 Chrome 路径正确**。
 * ffmpeg 走 vaDeriveImage/vaGetImage 读像素，而那两个入口里有
 * dmd_surface_wait() 兜底等帧（image.c:169 / :283）。也就是说：
 * 即使 EndPicture 返回时像素还没写进 surface，ffmpeg 也会在 map 时等到。
 * 所以 md5 全绿只说明"配对逻辑对"，完全不覆盖"EndPicture 返回后
 * 立刻采样能不能拿到正确像素"这一条 —— 而后者正是 Chrome 的行为。
 *
 * Chrome 的实际契约（已由 chromium 源码与驱动日志双向确认）：
 *   1. CreateSurfaces 之后、任何解码之前就 vaExportSurfaceHandle 拿 dmabuf fd
 *   2. 之后只调 vaBeginPicture / vaRenderPicture / vaEndPicture
 *   3. **从不** vaSyncSurface / vaQuerySurfaceStatus / vaDeriveImage
 *      （实测计数：Sync 0 次，Firefox 同场景 1500 次）
 *   4. 靠自己的软件 DPB 决定何时把某个 surface 交给合成器上屏
 *
 * 本探针复现 1~3，然后直接 mmap 那个 dmabuf 读像素。
 * 素材用带大号帧号的测试流（tools 目录外的 order_slow.mp4 一类），
 * 判据是"每个 surface 里的画面是不是它对应的那一帧"。
 *
 * 判定方式：不做 OCR。取每帧 Y 平面的指纹（分块均值），与软解基线
 * 逐帧比对，找出最接近的那一帧序号。若 surface k 装的是第 k 帧，
 * 顺序就是对的；若装的是别的帧，直接打印错位量。
 *
 * 用法：
 *   probe_chrome_order <annexb 裸流> <软解基线 nv12> <宽> <高> [帧数]
 *
 * 基线生成（注意必须是同一条流、同样的帧数、nv12）：
 *   ffmpeg -i in.mp4 -frames:v 40 -f rawvideo -pix_fmt nv12 base.nv12
 *
 * ══════════════════════════════════════════════════════════════════
 * ⚠️⚠️ 本探针当前**结论不可信**，不要用它的输出判断驱动对错。⚠️⚠️
 *
 * 它只送 VASliceDataBufferType，**从不送 VAPictureParameterBufferHEVC**。
 * 而本驱动靠 pic param 反向合成 VPS/SPS/PPS（VA-API 不传递参数集的原始
 * 比特流，见 decode.c 的 build_unit）。没有 pic param 就没有 CSD，
 * 解码器压根没被正确配置，输出自然是空的或残留内容。
 *
 * 实测证据（本轮，order_slow.mp4 24 帧）：
 *   驱动日志 "HEVC pic_param:" 出现次数 —— 本探针 0 次，ffmpeg 1 次
 *   本探针结果：正确 0 / 错帧 8 / 空帧 16（共 24）
 *   **加上 vaSyncSurface 后结果逐字节完全相同**（DMD_PROBE_SYNC=1）
 *
 * 最后一条是决定性的：若问题真是"不 Sync 时像素没写进 dmabuf"，
 * 补 Sync 必然改变结果。结果不变 ⇒ 差异不在同步，而在"根本没解码成功"。
 * 这个探针测的是它自己的缺陷，不是 Chrome 路径的缺陷。
 *
 * 要让它可信，必须补齐（等于把 Chrome 的
 * H265VaapiVideoDecoderDelegate 重写一遍）：
 *   1. 解析 SPS/PPS，填 VAPictureParameterBufferHEVC 并 vaRenderPicture
 *   2. 填 VASliceParameterBufferHEVC（slice 分段信息）
 *   3. 维护 DPB / ReferenceFrames 数组，否则 P/B 帧参考错
 *
 * 在补齐之前，Chrome 路径只能靠浏览器实测 + 驱动日志交叉印证。
 * 保留本文件是因为"先 Export、只 EndPicture、不 Sync"这套调用骨架
 * 是对的，缺的只是参数缓冲；下一轮在此基础上补齐即可。
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
#define BLK 8   /* 指纹分块数（BLK x BLK 个块的均值） */

static const char *st(VAStatus s) { return vaErrorStr(s); }

struct fp { double v[BLK * BLK]; };

/* 从 NV12 的 Y 平面算分块均值指纹 */
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
            "用法: %s <annexb裸流> <软解基线.nv12> <宽> <高> [帧数] [--codec h264]\n",
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

    /* ---- 读码流 ---- */
    FILE *f = fopen(spath, "rb");
    if (!f) { perror("open 裸流"); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)fsz);
    if (!buf || fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) {
        fprintf(stderr, "读裸流失败\n"); return 1;
    }
    fclose(f);

    /* ---- 读软解基线，算每帧指纹 ---- */
    size_t fsize = (size_t)W * H * 3 / 2;
    FILE *bf = fopen(bpath, "rb");
    if (!bf) { perror("open 基线"); return 1; }
    fseek(bf, 0, SEEK_END); long bsz = ftell(bf); fseek(bf, 0, SEEK_SET);
    int nbase = (int)(bsz / (long)fsize);
    if (nbase < 1) { fprintf(stderr, "基线太小（%ld 字节）\n", bsz); return 1; }
    struct fp *base = calloc((size_t)nbase, sizeof(*base));
    uint8_t *fbuf = malloc(fsize);
    for (int i = 0; i < nbase; i++) {
        if (fread(fbuf, 1, fsize, bf) != fsize) { nbase = i; break; }
        fingerprint(fbuf, W, W, H, &base[i]);
    }
    fclose(bf);
    printf("基线: %d 帧 %dx%d\n", nbase, W, H);

    /* ---- VA 初始化 ---- */
    int drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("open renderD128"); return 1; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    int major, minor;
    VAStatus s = vaInitialize(dpy, &major, &minor);
    if (s != VA_STATUS_SUCCESS) { fprintf(stderr, "vaInitialize: %s\n", st(s)); return 1; }
    printf("VA %d.%d vendor=%s\n", major, minor, vaQueryVendorString(dpy));

    VAProfile prof = is_hevc ? VAProfileHEVCMain : VAProfileH264High;
    VAConfigID cfg;
    /* Chrome 建 config 不传属性 */
    s = vaCreateConfig(dpy, prof, VAEntrypointVLD, NULL, 0, &cfg);
    if (s != VA_STATUS_SUCCESS) { fprintf(stderr, "vaCreateConfig: %s\n", st(s)); return 1; }

    VASurfaceID surf[MAXSURF];
    s = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, surf, MAXSURF, NULL, 0);
    if (s != VA_STATUS_SUCCESS) { fprintf(stderr, "vaCreateSurfaces: %s\n", st(s)); return 1; }

    VAContextID ctx;
    s = vaCreateContext(dpy, cfg, W, H, VA_PROGRESSIVE, surf, MAXSURF, &ctx);
    if (s != VA_STATUS_SUCCESS) { fprintf(stderr, "vaCreateContext: %s\n", st(s)); return 1; }

    /* ---- Chrome 步骤 1：解码之前就导出全部 dmabuf 并 mmap ---- */
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
            fprintf(stderr, "mmap dmabuf[%d] 失败（这条路不通就没法做本测试）\n", i);
            return 1;
        }
        exported++;
    }
    printf("已导出并 mmap %d 个 dmabuf（stride=%u off_uv=%u size=%zu）\n",
           exported, dm[0].stride, dm[0].off_uv, dm[0].size);

    /* ---- 逐帧提交。只 EndPicture，绝不 Sync ---- */
    /* 记录每次提交用的 surface，提交序 = 解码序 */
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
            printf("vaBeginPicture 第 %d 帧: %s\n", nsub, st(s));
            break;
        }
        VABufferID bid;
        /* 起始码要带上：驱动按 Annex-B 处理 slice data */
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
                printf("vaEndPicture 第 %d 帧失败: %s\n", nsub, st(s));
        }
        used[nsub] = si;
        nsub++;
    }
    printf("提交 %d 帧（EndPicture 失败 %d）\n", nsub, endpic_fail);

    /* ---- Chrome 步骤 2：直接读 dmabuf，判断每个 surface 装的是哪一帧 ----
     *
     * 这里刻意**不调任何 VA 同步入口**，就是要暴露"EndPicture 返回后
     * 像素到底在不在"。为了不把结果变成纯竞态，给驱动一点时间：
     * 真实 Chrome 里合成器也不是纳秒级采样。但这个等待不能靠 VA 调用，
     * 只能是墙上时钟 —— 因为 Chrome 就是这么做的。 */
    int settle_ms = 300;
    const char *env = getenv("DMD_PROBE_SETTLE_MS");
    if (env) settle_ms = atoi(env);
    /* 对照开关：DMD_PROBE_SYNC=1 时补调 vaSyncSurface（Firefox/ffmpeg 契约）。
     * 用途是校验探针自身可信度 —— 若加 Sync 后全部正确，说明解码与配对
     * 都没问题，差别纯粹在"不 Sync 时像素还没写进 dmabuf"。 */
    if (getenv("DMD_PROBE_SYNC")) {
        for (int k = 0; k < nsub && k < MAXSURF; k++)
            vaSyncSurface(dpy, surf[used[k]]);
        printf("[对照] 已对 %d 个 surface 调 vaSyncSurface\n",
               nsub < MAXSURF ? nsub : MAXSURF);
    }
    usleep((useconds_t)settle_ms * 1000);
    printf("等待 %d ms 后读 dmabuf（期间不调任何 VA 同步入口）\n\n", settle_ms);

    int ok = 0, wrong = 0, blank = 0;
    printf("提交序  VA_surf  最像基线第几帧  距离      判定\n");
    for (int k = 0; k < nsub && k < MAXSURF; k++) {
        int si = used[k];
        struct fp got;
        fingerprint(dm[si].map, dm[si].stride, W, H, &got);

        /* 全黑/未初始化：块均值几乎全相等且很低 */
        double mean = 0;
        for (int i = 0; i < BLK * BLK; i++) mean += got.v[i];
        mean /= BLK * BLK;

        int best = -1; double bestd = 1e30;
        for (int i = 0; i < nbase; i++) {
            double d = fp_dist(&got, &base[i]);
            if (d < bestd) { bestd = d; best = i; }
        }

        const char *verdict;
        if (mean < 20.0) { verdict = "空/黑帧"; blank++; }
        else if (best == k) { verdict = "✓ 正确"; ok++; }
        else { verdict = "✗ 错帧"; wrong++; }

        printf("%6d  %7u  %14d  %8.1f  %s\n", k,
               (unsigned)surf[si], best, bestd, verdict);
    }

    printf("\n结果: 正确 %d / 错帧 %d / 空帧 %d（共比对 %d）\n",
           ok, wrong, blank, ok + wrong + blank);
    if (wrong || blank)
        printf("→ Chrome 契约下 surface 内容不正确。"
               "这正是画面前后帧跳跃的直接证据。\n");
    else
        printf("→ Chrome 契约下 surface 内容全部正确。\n");

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
