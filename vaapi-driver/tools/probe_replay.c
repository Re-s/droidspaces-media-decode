/* 排空路径会不会产生黑帧/重复帧？
 *
 * 浏览器画面"中间黑屏一闪一闪"，而驱动日志里看到同一个 POC 反复出现。
 * 两个可疑点：
 *   1) 排空后重送 CSD，解码器可能吐出一个未初始化/全黑的缓冲
 *   2) flush 丢弃了参考帧，之后的 P/B 帧解出来是坏的
 *
 * 这里直连 daemon，按浏览器的节奏（送 3 个单元就排空取帧）跑一遍，
 * 逐帧检查 Y 平面亮度均值：远低于正常值即为黑帧。
 * 同时记录每帧的前若干字节指纹，用来发现重复帧。
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

/* Y 平面亮度均值：黑帧接近 16（BT.601 的 black level） */
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
    if (load(path) < 0) { fprintf(stderr, "读不了 %s\n", path); return 1; }

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
    printf("码流: %d 个 VCL\n\n", nv);

    struct dmd_session_config cfg;
    dmd_session_config_init(&cfg);
    cfg.codec = DMD_CODEC_H264;
    cfg.width = 1920;
    cfg.height = 1088;
    struct dmd_error e;
    struct dmd_session *s = dmd_session_create(&cfg, &e);
    if (!s) { fprintf(stderr, "建会话失败: %s\n", e.msg); return 1; }

    dmd_session_send_unit(s, sps, sps_len);
    dmd_session_send_unit(s, pps, pps_len);

    int total = 0, black = 0, dup = 0;
    unsigned long long prev_sig = 0;
    int sent = 0;

    /* 模拟浏览器：每送 3 个单元就排空取帧 */
    while (sent < nv && total < 60) {
        for (int k = 0; k < 3 && sent < nv; k++, sent++)
            dmd_session_send_unit(s, data + vo[sent], vl[sent]);

        if (dmd_session_drain(s) != DMD_OK) {
            fprintf(stderr, "排空失败: %s\n", dmd_session_last_error(s));
            break;
        }
        int need_rebuild = 1;   /* 排空后 daemon 侧已 EOS，取完帧要重建 */

        struct dmd_frame fr;
        while (dmd_session_next_frame(s, &fr, 2000) == DMD_OK) {
            const struct dmd_format *fmt = dmd_session_format(s);
            int stride = fmt && fmt->valid ? fmt->stride : 1920;
            int w = 1920, h = 1080;
            double mean = luma_mean(fr.data, stride, w, h);

            /* 指纹：取若干散点，避免只看开头误判 */
            /* 指纹要覆盖整幅画面。早期版本只取 64 个稀疏点，
             * 把亮度明显不同的相邻帧误报成"完全相同"。 */
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
                printf("帧%3d: 亮度均值 %6.2f %s%s\n", total, mean,
                       is_black ? " ← 黑帧!" : "", is_dup ? " ← 与上一帧完全相同!" : "");
            prev_sig = sig;
            dmd_session_release_frame(s, &fr);
        }

        /* 驱动的真实行为：排空后会话进入 EOS，下次送料前重建并重送参数集，
         * 从非 IDR 帧续传。这里照做，才能测到端到端的画面正确性。 */
        if (need_rebuild) {
            dmd_session_destroy(s);
            s = dmd_session_create(&cfg, &e);
            if (!s) { fprintf(stderr, "重建失败: %s\n", e.msg); break; }
            dmd_session_send_unit(s, sps, sps_len);
            dmd_session_send_unit(s, pps, pps_len);

            /* 关键：新解码器没有任何参考帧，从非 IDR 帧续传必然全黑。
             * 必须从最近的 IDR 开始重放，把参考帧链重新建立起来。
             * 重放出的帧要丢弃（它们已经交付过），只为恢复解码器状态。 */
            int idr = sent - 1;
            while (idr > 0 && (data[vo[idr] + 3] & 0x1f) != 5) idr--;
            int replay = 0;
            for (int k = idr; k < sent; k++) {
                dmd_session_send_unit(s, data + vo[k], vl[k]);
                replay++;
            }
            if (replay > 0) {
                /* 取走重放产生的帧并丢弃 */
                dmd_session_drain(s);
                struct dmd_frame junk;
                int dropped = 0;
                while (dmd_session_next_frame(s, &junk, 2000) == DMD_OK) {
                    dropped++;
                    dmd_session_release_frame(s, &junk);
                }
                if (total <= 6)
                    printf("  [重放 IDR@%d..%d 共 %d 单元，丢弃 %d 帧]\n",
                           idr, sent - 1, replay, dropped);
                /* 重放也用掉了 EOS，再建一次并重放 —— 说明这条路不可行时会看出来 */
                dmd_session_destroy(s);
                s = dmd_session_create(&cfg, &e);
                if (!s) break;
                dmd_session_send_unit(s, sps, sps_len);
                dmd_session_send_unit(s, pps, pps_len);
                for (int k = idr; k < sent; k++)
                    dmd_session_send_unit(s, data + vo[k], vl[k]);
            }
        }
    }

    dmd_session_destroy(s);
    printf("\n=== 结论 ===\n");
    printf("共 %d 帧：黑帧 %d，重复帧 %d\n", total, black, dup);
    if (black == 0 && dup == 0)
        printf("✓ 排空路径本身不产生黑帧/重复帧 → 黑屏另有原因\n");
    else
        printf("✗ 排空路径会产生异常帧 → 这就是黑屏的来源\n");
    return 0;
}
