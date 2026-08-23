/* 中途重送参数集到底会不会毁掉参考帧？直接对 MediaCodec 测，不经驱动。
 *
 * 背景：浏览器场景 725 帧里有 135 帧纯黑（Y=16），而 ffmpeg 路径零黑帧。
 * 唯一显著差异是驱动会随 B/P 帧类型重送 PPS（643 次），
 * 于是怀疑"中途重送参数集复位了解码器"。
 *
 * 但两次修改（内容去重、只首次用 FLAG_CODEC_CONFIG）都没有减少黑帧，
 * 说明这个假设本身没有被验证过。这个探针直接测四种情形：
 *
 *   A) 只在开头送一次 CSD（基准）
 *   B) 每帧前重送参数集，用 FLAG_CODEC_CONFIG
 *   C) 每帧前重送参数集，用普通 in-band（flags=0）
 *   D) 每帧前重送**内容被改过**的 PPS（模拟 num_ref_idx 变化）
 *
 * 每种情形都统计黑帧数。这样能一次性判定：
 *  - 若 A 就有黑帧 → 与重送无关，是别的原因（例如输出顺序/配对）
 *  - 若只有 B 有黑帧 → FLAG_CODEC_CONFIG 是元凶
 *  - 若 B/C/D 都有 → 任何中途参数集都不行
 *  - 若都没有 → 黑帧根本不在 daemon 侧，问题在驱动或浏览器
 *
 * 编译：
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang \
 *     -O2 -o probe_csd_reset probe_csd_reset.c -lmediandk -llog
 */
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define FLAG_CODEC_CONFIG 2
#define FLAG_EOS          4
#define STREAM "/data/local/tmp/test1080.h264"

static uint8_t *g_buf;
static long g_len;

struct nalu { long off; long len; int type; };
static struct nalu g_nalus[8192];
static int g_n;

static void scan(void)
{
    long i = 0;
    while (i + 3 < g_len) {
        if (g_buf[i] == 0 && g_buf[i+1] == 0 && g_buf[i+2] == 1) {
            long s = i + 3;
            long e = s;
            while (e + 3 < g_len &&
                   !(g_buf[e] == 0 && g_buf[e+1] == 0 && g_buf[e+2] == 1))
                e++;
            long n = (e + 3 >= g_len) ? g_len - s : e - s;
            if (n > 0 && g_n < 8192) {
                g_nalus[g_n].off = s;
                g_nalus[g_n].len = n;
                g_nalus[g_n].type = g_buf[s] & 0x1f;
                g_n++;
            }
            i = s;
        } else {
            i++;
        }
    }
}

/* 抽样算亮度均值 */
static double luma(const uint8_t *p, size_t n)
{
    double sum = 0; int cnt = 0;
    size_t lim = n > 2000000 ? 2000000 : n;
    for (size_t k = 0; k < lim; k += 1499) { sum += p[k]; cnt++; }
    return cnt ? sum / cnt : 0;
}

/* mode: 0=只开头一次  1=每帧 FLAG_CODEC_CONFIG  2=每帧 in-band  3=每帧改过内容 */
static void run(int mode, const char *name)
{
    /* 收集 SPS/PPS */
    uint8_t csd[512]; size_t csd_len = 0;
    int sps_i = -1, pps_i = -1;
    for (int k = 0; k < g_n; k++) {
        if (g_nalus[k].type == 7 && sps_i < 0) sps_i = k;
        if (g_nalus[k].type == 8 && pps_i < 0) pps_i = k;
    }
    if (sps_i < 0 || pps_i < 0) { printf("  找不到 SPS/PPS\n"); return; }

    csd[0]=0;csd[1]=0;csd[2]=0;csd[3]=1;
    memcpy(csd+4, g_buf+g_nalus[sps_i].off, g_nalus[sps_i].len);
    csd_len = 4 + g_nalus[sps_i].len;
    csd[csd_len]=0;csd[csd_len+1]=0;csd[csd_len+2]=0;csd[csd_len+3]=1;
    memcpy(csd+csd_len+4, g_buf+g_nalus[pps_i].off, g_nalus[pps_i].len);
    csd_len += 4 + g_nalus[pps_i].len;

    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    AMediaFormat_setInt32(fmt, "low-latency", 1);
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);
    AMediaFormat_setBuffer(fmt, "csd-0", csd, csd_len);

    AMediaCodec *c = AMediaCodec_createDecoderByType("video/avc");
    if (!c) { printf("  创建解码器失败\n"); AMediaFormat_delete(fmt); return; }
    if (AMediaCodec_configure(c, fmt, NULL, NULL, 0) != AMEDIA_OK) {
        printf("  configure 失败\n");
        AMediaCodec_delete(c); AMediaFormat_delete(fmt); return;
    }
    AMediaCodec_start(c);
    AMediaFormat_delete(fmt);

    int sent = 0, got = 0, black = 0, first_black = -1;
    int csd_once_done = 0;

    for (int k = 0; k < g_n && sent < 150; k++) {
        int t = g_nalus[k].type;
        if (t == 7 || t == 8) continue;      /* 参数集单独处理 */
        if (t != 1 && t != 5) continue;      /* 只送 VCL */

        /* 按 mode 决定是否在这一帧前重送参数集 */
        int resend = 0;
        if (mode == 0) resend = (!csd_once_done);
        else           resend = 1;

        if (resend) {
            ssize_t ci = AMediaCodec_dequeueInputBuffer(c, 20000);
            if (ci >= 0) {
                size_t cap; uint8_t *ib = AMediaCodec_getInputBuffer(c, ci, &cap);
                uint8_t tmp[512]; size_t tl = csd_len;
                memcpy(tmp, csd, csd_len);
                if (mode == 3) {
                    /* 改 PPS 最后一个字节，模拟 num_ref_idx 变化 */
                    tmp[tl-1] ^= 0x01;
                }
                if (ib && cap >= tl) {
                    memcpy(ib, tmp, tl);
                    uint32_t fl = 0;
                    if (mode == 1 || (mode == 0 && !csd_once_done)) fl = FLAG_CODEC_CONFIG;
                    if (mode == 3) fl = FLAG_CODEC_CONFIG;
                    AMediaCodec_queueInputBuffer(c, ci, 0, tl, 0, fl);
                    csd_once_done = 1;
                } else {
                    AMediaCodec_queueInputBuffer(c, ci, 0, 0, 0, 0);
                }
            }
        }

        /* 送 VCL */
        ssize_t bi = AMediaCodec_dequeueInputBuffer(c, 20000);
        if (bi < 0) continue;
        size_t cap; uint8_t *ib = AMediaCodec_getInputBuffer(c, bi, &cap);
        long need = 4 + g_nalus[k].len;
        if (!ib || cap < (size_t)need) {
            AMediaCodec_queueInputBuffer(c, bi, 0, 0, 0, 0);
            continue;
        }
        ib[0]=0;ib[1]=0;ib[2]=0;ib[3]=1;
        memcpy(ib+4, g_buf+g_nalus[k].off, g_nalus[k].len);
        AMediaCodec_queueInputBuffer(c, bi, 0, need, (int64_t)(sent+1)*1000, 0);
        sent++;

        /* 取输出 */
        for (;;) {
            AMediaCodecBufferInfo info;
            ssize_t oi = AMediaCodec_dequeueOutputBuffer(c, &info, 0);
            if (oi < 0) break;
            size_t osz;
            uint8_t *ob = AMediaCodec_getOutputBuffer(c, oi, &osz);
            if (ob && info.size > 100000) {
                double m = luma(ob + info.offset, info.size);
                got++;
                if (m < 20.0) { black++; if (first_black < 0) first_black = got; }
            }
            AMediaCodec_releaseOutputBuffer(c, oi, false);
        }
    }

    /* 排空 */
    ssize_t bi = AMediaCodec_dequeueInputBuffer(c, 20000);
    if (bi >= 0) AMediaCodec_queueInputBuffer(c, bi, 0, 0, 0, FLAG_EOS);
    for (int guard = 0; guard < 200; guard++) {
        AMediaCodecBufferInfo info;
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(c, &info, 30000);
        if (oi == AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;
        if (oi < 0) continue;
        size_t osz;
        uint8_t *ob = AMediaCodec_getOutputBuffer(c, oi, &osz);
        if (ob && info.size > 100000) {
            double m = luma(ob + info.offset, info.size);
            got++;
            if (m < 20.0) { black++; if (first_black < 0) first_black = got; }
        }
        int eos = (info.flags & FLAG_EOS) != 0;
        AMediaCodec_releaseOutputBuffer(c, oi, false);
        if (eos) break;
    }

    AMediaCodec_stop(c);
    AMediaCodec_delete(c);

    printf("  %-34s 送 %3d 帧, 出 %3d 帧, 黑 %3d",
           name, sent, got, black);
    if (black) printf("  首个黑帧=第 %d 帧", first_black);
    printf("\n");
}

int main(void)
{
    FILE *f = fopen(STREAM, "rb");
    if (!f) { printf("读不了 %s\n", STREAM); return 1; }
    fseek(f, 0, SEEK_END); g_len = ftell(f); fseek(f, 0, SEEK_SET);
    g_buf = malloc(g_len);
    if (fread(g_buf, 1, g_len, f) != (size_t)g_len) { printf("读取失败\n"); return 1; }
    fclose(f);
    scan();
    printf("码流 %ld 字节，%d 个 NALU\n\n", g_len, g_n);

    printf("四种参数集重送方式的黑帧数对照：\n");
    run(0, "A) 只开头送一次 CSD（基准）");
    run(1, "B) 每帧重送 + FLAG_CODEC_CONFIG");
    run(2, "C) 每帧重送 + in-band(flags=0)");
    run(3, "D) 每帧重送改过内容的 PPS");

    printf("\n判读：若 A 也有黑帧则与重送无关；若仅 B/D 有则 FLAG_CODEC_CONFIG 是元凶；\n"
           "若全为 0 则黑帧不在 daemon 侧，应转查驱动/浏览器路径。\n");
    return 0;
}
