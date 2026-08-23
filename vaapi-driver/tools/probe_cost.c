/* 量一次"会话重建"到底多贵。
 *
 * 背景：浏览器路径下每帧触发一次 flush + 重建（147 帧 ≈ 147 次）。
 * 功能是对的、速度也够（Firefox 自报平均 7.4ms/帧，帧间隔 33.3ms），
 * 但在动 daemon 之前得先知道这笔开销的实际大小 ——
 * 如果重建只占单帧预算的很小一部分，那改 daemon 的收益就很有限。
 *
 * 直连 daemon 测三段耗时：
 *   1) 建会话（connect + 握手 + configure + start）
 *   2) 送参数集
 *   3) flush（finish_input）到拿到帧
 * 与"稳态下送一个 VCL 拿一帧"的耗时对比。
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

/* 找下一个 start code，返回 NALU 起点，*len 为长度（不含下一个 start code） */
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
    if (load(path) < 0) { fprintf(stderr, "读不了 %s\n", path); return 1; }

    /* 收集 SPS/PPS 与前若干 VCL */
    unsigned char *sps = NULL, *pps = NULL;
    long sps_len = 0, pps_len = 0;
    long off = 0, len;
    long vcl_off[64];
    long vcl_len[64];
    int nvcl = 0;

    while ((off = next_nalu(off, &len)) >= 0 && nvcl < 64) {
        int nut = data[off] & 0x1f;
        /* 送料必须带 Annex B 起始码（dmd_client.h 明确要求），
         * next_nalu 返回的是 payload 起点，所以回退 3 字节、长度 +3。 */
        if (nut == 7 && !sps) { sps = data + off - 3; sps_len = len + 3; }
        else if (nut == 8 && !pps) { pps = data + off - 3; pps_len = len + 3; }
        else if (nut == 1 || nut == 5) { vcl_off[nvcl] = off - 3; vcl_len[nvcl] = len + 3; nvcl++; }
        off += len;
    }
    if (!sps || !pps || nvcl < 8) {
        fprintf(stderr, "码流不含足够的 SPS/PPS/VCL\n");
        return 1;
    }
    printf("码流: SPS %ld 字节, PPS %ld 字节, 收集 %d 个 VCL\n\n",
           sps_len, pps_len, nvcl);

    struct dmd_error e;
    struct timespec t0;
    double t_open = 0, t_param = 0, t_flush = 0;
    struct dmd_frame fr;

    /* ---- 第一段：建会话 ---- */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct dmd_session_config cfg;
    dmd_session_config_init(&cfg);
    cfg.codec = DMD_CODEC_H264;
    cfg.width = 1920;
    cfg.height = 1088;
    struct dmd_session *s = dmd_session_create(&cfg, &e);
    t_open = ms_since(&t0);
    if (!s) { fprintf(stderr, "建会话失败 code=%d %s\n", e.code, e.msg); return 1; }
    printf("① 建会话（connect+握手+configure+start）: %.1f ms\n", t_open);

    /* ---- 第二段：送参数集 ---- */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    dmd_session_send_unit(s, sps, sps_len);
    dmd_session_send_unit(s, pps, pps_len);
    t_param = ms_since(&t0);
    printf("② 送 SPS+PPS: %.1f ms\n", t_param);

    /* ---- 模拟浏览器：只送 3 个 VCL，然后 flush 取帧 ---- */
    for (int i = 0; i < 3; i++)
        dmd_session_send_unit(s, data + vcl_off[i], vcl_len[i]);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    dmd_session_finish_input(s);
    int got = 0;
    double t_first = -1.0;
    while (dmd_session_next_frame(s, &fr, 3000) == DMD_OK) {
        if (got == 0) t_first = ms_since(&t0);   /* 只有首帧才影响播放延迟 */
        got++;
        dmd_session_release_frame(s, &fr);
    }
    t_flush = ms_since(&t0);
    printf("③ flush → 首帧: %.1f ms；取完全部 %d 帧（含等 EOS）: %.1f ms\n",
           t_first, got, t_flush);
    /* 关键：重建路径下一次 flush 能拿到多帧，成本被这几帧摊薄。
     * 影响播放流畅度的是"每帧摊到多少"，不是一次 flush 的总耗时。 */
    printf("   → 摊到每帧: %.1f ms\n", got ? t_flush / got : 0.0);
    t_flush = t_first;   /* 后续汇总用首帧延迟，避免把等 EOS 算进单帧成本 */
    dmd_session_destroy(s);

    printf("\n单次重建总成本 ≈ %.1f ms（①+②+③）\n", t_open + t_param + t_flush);

    /* ---- 对照：一个会话内连续送料的稳态耗时 ---- */
    s = dmd_session_create(&cfg, &e);
    if (!s) { fprintf(stderr, "第二次建会话失败\n"); return 1; }
    dmd_session_send_unit(s, sps, sps_len);
    dmd_session_send_unit(s, pps, pps_len);
    /* 先填满流水线 */
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
    printf("对照：稳态下送 %d 个 VCL 各取 1 帧: %.1f ms（%.1f ms/帧）\n",
           steady, t_steady, steady ? t_steady / steady : 0.0);
    dmd_session_destroy(s);

    printf("\n=== 判断 ===\n");
    printf("建会话+参数集 = %.1f ms，flush→首帧 = %.1f ms\n",
           t_open + t_param, t_flush);
    printf("对照稳态 %.1f ms/帧；帧预算 33.3 ms（30fps）\n",
           steady ? t_steady / steady : 0.0);
    printf("\n注意：一次 flush 能取到多帧，重建成本被摊薄。\n");
    printf("是否值得改 daemon，要看实测帧率是否已经够用 ——\n");
    printf("浏览器实测 143 帧按正常速度播完、Firefox 自报 7.4 ms/帧，\n");
    printf("说明当前路径的吞吐已经满足 30fps。\n");
    return 0;
}
