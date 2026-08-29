/*
 * test_v4l2_backend —— V4L2 后端的实机自测。
 *
 * 与 tools/v4l2_dec_probe.c 的区别：探针是一次性验证 V4L2 流程本身，
 * 这里验证的是 src/v4l2_backend.c 的 **API 设计与状态机**是否可用 ——
 * daemon 将按同样的调用序列使用它。
 *
 * 需要 root（视频节点属 system:camera）。用法:
 *     test_v4l2_backend <codec> <file> [单元数]
 *     codec: h264 | hevc | vp9 | av1
 *
 * h264/hevc 按 Annex-B 起始码切分，vp9 读 IVF，av1 按 temporal delimiter 切。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v4l2_backend.h"

#define MAX_UNITS 1024

struct unit { size_t off, len; };

/* ---- AV1: 按 OBU_TEMPORAL_DELIMITER(type=2) 切 temporal unit ---- */
static int split_av1(const uint8_t *d, size_t len, struct unit *o, int cap)
{
    int n = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t h = d[i];
        int type = (h >> 3) & 0xF, has_size = (h >> 1) & 1;
        size_t j = i + 1;
        if ((h >> 2) & 1) j++;
        size_t size = 0;
        if (has_size) {
            int shift = 0;
            while (j < len) {
                uint8_t b = d[j++];
                size |= (size_t)(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80)) break;
            }
        } else {
            size = len - j;
        }
        size_t end = j + size;
        if (end > len) break;
        if (type == 2) {
            if (n >= cap) break;
            o[n].off = i; o[n].len = end - i; n++;
        } else if (n > 0) {
            o[n - 1].len = end - o[n - 1].off;
        } else {
            if (n >= cap) break;
            o[n].off = i; o[n].len = end - i; n++;
        }
        i = end;
    }
    return n;
}

/* ---- H264/HEVC: 按 Annex-B 起始码切 NALU，并把参数集与首个 VCL 合并 ----
 *
 * 合并的理由：V4L2 解码器要在第一个访问单元里看到 SPS/PPS 才能完成
 * 分辨率协商。逐个 NALU 送会让驱动在只有 SPS 时就被要求出帧。 */
static int split_annexb(const uint8_t *d, size_t len, struct unit *o, int cap)
{
    size_t starts[MAX_UNITS * 4];
    int ns = 0;
    for (size_t i = 0; i + 3 < len && ns < (int)(sizeof(starts)/sizeof(starts[0])); ) {
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) { starts[ns++] = i; i += 3; }
        else if (i + 4 < len && d[i] == 0 && d[i+1] == 0 && d[i+2] == 0 && d[i+3] == 1) {
            starts[ns++] = i; i += 4;
        } else i++;
    }
    if (ns == 0) return 0;

    /* 简单策略：每个 NALU 一个单元，但把开头连续的非 VCL（参数集）
     * 并入其后的第一个 VCL。对测试足够，daemon 侧另有 CSD 逻辑。 */
    int n = 0;
    for (int k = 0; k < ns && n < cap; k++) {
        size_t s = starts[k];
        size_t e = (k + 1 < ns) ? starts[k + 1] : len;
        if (n > 0 && o[n-1].len < 64) {          /* 前一个很小，视为参数集，合并 */
            o[n-1].len = e - o[n-1].off;
            continue;
        }
        o[n].off = s; o[n].len = e - s; n++;
    }
    return n;
}

/* ---- VP9: IVF 容器，32 字节文件头 + 每帧 12 字节头 ---- */
static int split_ivf(const uint8_t *d, size_t len, struct unit *o, int cap)
{
    if (len < 32 || memcmp(d, "DKIF", 4) != 0) {
        fprintf(stderr, "不是 IVF 文件（VP9 需要 IVF 容器）\n");
        return 0;
    }
    int n = 0;
    size_t i = 32;
    while (i + 12 <= len && n < cap) {
        uint32_t fsz = (uint32_t)d[i] | ((uint32_t)d[i+1] << 8) |
                       ((uint32_t)d[i+2] << 16) | ((uint32_t)d[i+3] << 24);
        i += 12;
        if (i + fsz > len) break;
        o[n].off = i; o[n].len = fsz; n++;
        i += fsz;
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <h264|hevc|vp9|av1> <file> [单元数]\n", argv[0]);
        return 2;
    }
    const char *cname = argv[1];
    const char *path = argv[2];
    int limit = (argc > 3) ? atoi(argv[3]) : 10;

    int codec_id;
    if      (!strcmp(cname, "h264")) codec_id = DMD_V4L2_CODEC_H264;
    else if (!strcmp(cname, "hevc")) codec_id = DMD_V4L2_CODEC_HEVC;
    else if (!strcmp(cname, "vp9"))  codec_id = DMD_V4L2_CODEC_VP9;
    else if (!strcmp(cname, "av1"))  codec_id = DMD_V4L2_CODEC_AV1;
    else { fprintf(stderr, "未知 codec: %s\n", cname); return 2; }

    printf("=== V4L2 后端自测: codec=%s file=%s ===\n", cname, path);

    if (!dmd_v4l2_probe(codec_id)) {
        printf("probe: 驱动不支持该 codec\n");
        return 1;
    }
    printf("probe: 驱动支持 ✓\n");

    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)fsz);
    if (!data || fread(data, 1, (size_t)fsz, f) != (size_t)fsz) return 1;
    fclose(f);

    struct unit *units = calloc(MAX_UNITS, sizeof(*units));
    int nu;
    if (codec_id == DMD_V4L2_CODEC_AV1)      nu = split_av1(data, (size_t)fsz, units, MAX_UNITS);
    else if (codec_id == DMD_V4L2_CODEC_VP9) nu = split_ivf(data, (size_t)fsz, units, MAX_UNITS);
    else                                     nu = split_annexb(data, (size_t)fsz, units, MAX_UNITS);
    if (nu <= 0) { fprintf(stderr, "切分失败\n"); return 1; }
    if (limit > 0 && limit < nu) nu = limit;
    printf("切出 %d 个单元（首个 %zu 字节）\n", nu, units[0].len);

    struct dmd_v4l2_dec dec;
    if (dmd_v4l2_open(&dec, codec_id, 1920, 1080) < 0) {
        fprintf(stderr, "dmd_v4l2_open 失败\n");
        return 1;
    }

    int sent = 0, frames = 0, dumped = 0;
    /* 先送首个单元推动分辨率协商，然后进入送料/收帧循环。 */
    for (int loop = 0; loop < 400 && frames < nu + 8; loop++) {
        /* 有余料就尽量送 */
        while (sent < nu) {
            int r = dmd_v4l2_send(&dec, data + units[sent].off, units[sent].len,
                                  (uint64_t)(sent + 1) * 1000ULL);
            if (r == 0) { sent++; continue; }
            if (r == 1) break;               /* 无空闲输入缓冲，先收帧 */
            fprintf(stderr, "send 出错\n");
            goto done;
        }

        uint8_t *fdata = NULL; size_t flen = 0; uint64_t fpts = 0; int fidx = -1;
        int r = dmd_v4l2_recv(&dec, &fdata, &flen, &fpts, &fidx, 200);
        if (r == 1) {
            frames++;
            if (frames <= 3 || frames % 10 == 0)
                printf("  帧 %d: %zu 字节 pts=%llu idx=%d\n", frames, flen,
                       (unsigned long long)fpts, fidx);
            if (!dumped && flen > 0) {
                FILE *df = fopen("/data/local/tmp/v4l2_backend_frame0.nv12", "wb");
                if (df) { fwrite(fdata, 1, flen, df); fclose(df); dumped = 1;
                          printf("    → 首帧已落盘 %zu 字节\n", flen); }
            }
            dmd_v4l2_release(&dec, fidx);
        } else if (r == 2) {
            printf("  收到 EOS\n");
            break;
        } else if (r < 0) {
            fprintf(stderr, "recv 出错\n");
            break;
        }

        /* 料送完了就请求排空，把流水线里的帧逼出来 */
        if (sent >= nu && !dec.draining && dec.cap_ready)
            dmd_v4l2_drain(&dec);
    }

done:
    printf("结果: 送入 %d 单元，解出 %d 帧（%dx%d 有效 %dx%d stride=%d）\n",
           sent, frames, dec.w, dec.h, dec.crop_w, dec.crop_h, dec.stride);
    dmd_v4l2_close(&dec);
    free(units);
    free(data);
    return frames > 0 ? 0 : 1;
}
