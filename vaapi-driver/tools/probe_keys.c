/* 能否让 MediaCodec 在浅队列（3 个输入单元）就吐首帧？
 *
 * 这是"根治黑帧"的关键：若能做到，就完全不需要排空/重建，
 * 参考帧链不断，画面自然正确。已知 low-latency=1 无效（滞后仍是 4）。
 *
 * 这里逐个试候选配置，量每种配置下的滞后深度（送几个 VCL 才出首帧）。
 * 滞后 <= 3 即为可用。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define FLAG_CODEC_CONFIG 2

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

static unsigned char *sps, *pps;
static long sps_len, pps_len, vo[64], vl[64];
static int nv;

/* 返回滞后深度：送第几个 VCL 时首帧出现。-1 = 送完 8 个都没出 */
static int measure(const char *label, void (*tweak)(AMediaFormat *))
{
    AMediaCodec *c = AMediaCodec_createDecoderByType("video/avc");
    if (!c) return -2;
    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    if (tweak) tweak(fmt);

    if (AMediaCodec_configure(c, fmt, NULL, NULL, 0) != AMEDIA_OK) {
        printf("%-46s configure 失败\n", label);
        AMediaFormat_delete(fmt); AMediaCodec_delete(c);
        return -2;
    }
    AMediaCodec_start(c);

    unsigned char csd[512];
    memcpy(csd, sps, sps_len);
    memcpy(csd + sps_len, pps, pps_len);
    ssize_t bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
    size_t cap;
    uint8_t *ib = AMediaCodec_getInputBuffer(c, bi, &cap);
    memcpy(ib, csd, sps_len + pps_len);
    AMediaCodec_queueInputBuffer(c, bi, 0, sps_len + pps_len, 0, FLAG_CODEC_CONFIG);

    int lag = -1;
    for (int i = 0; i < 8 && lag < 0; i++) {
        bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
        if (bi < 0) break;
        ib = AMediaCodec_getInputBuffer(c, bi, &cap);
        memcpy(ib, data + vo[i], vl[i]);
        AMediaCodec_queueInputBuffer(c, bi, 0, vl[i], (int64_t)i * 33333, 0);

        /* 给足时间：确认不是"来不及"而是"真不出" */
        for (int spin = 0; spin < 15; spin++) {
            AMediaCodecBufferInfo info;
            ssize_t oi = AMediaCodec_dequeueOutputBuffer(c, &info, 20000);
            if (oi >= 0) {
                if (info.size > 0) lag = i + 1;
                AMediaCodec_releaseOutputBuffer(c, oi, false);
                if (lag > 0) break;
            }
        }
    }

    printf("%-46s 滞后 %s%s\n", label,
           lag < 0 ? "未出帧" : (lag == 1 ? "1" : lag == 2 ? "2" : lag == 3 ? "3" : "4+"),
           (lag > 0 && lag <= 3) ? "  ← 可用!" : "");

    AMediaCodec_stop(c);
    AMediaCodec_delete(c);
    AMediaFormat_delete(fmt);
    return lag;
}

static void k_none(AMediaFormat *f) { (void)f; }
static void k_lowlat(AMediaFormat *f) { AMediaFormat_setInt32(f, "low-latency", 1); }
static void k_maxreorder(AMediaFormat *f) {
    AMediaFormat_setInt32(f, "low-latency", 1);
    AMediaFormat_setInt32(f, "max-output-reorder-frames", 0);
}
static void k_vendorlowlat(AMediaFormat *f) {
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
}
static void k_vendorpicorder(AMediaFormat *f) {
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", 1);
}
static void k_vendorboth(AMediaFormat *f) {
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", 1);
}
static void k_outputdelay(AMediaFormat *f) {
    AMediaFormat_setInt32(f, "low-latency", 1);
    AMediaFormat_setInt32(f, "output-delay", 0);
}
static void k_numref(AMediaFormat *f) {
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(f, "output-delay", 0);
    AMediaFormat_setInt32(f, "max-output-reorder-frames", 0);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/data/local/tmp/test1080.h264";
    if (load(path) < 0) { printf("读不了 %s\n", path); return 1; }

    long off = 0, len;
    while ((off = next_nalu(off, &len)) >= 0 && nv < 64) {
        int nut = data[off] & 0x1f;
        if (nut == 7 && !sps) { sps = data + off - 3; sps_len = len + 3; }
        else if (nut == 8 && !pps) { pps = data + off - 3; pps_len = len + 3; }
        else if (nut == 1 || nut == 5) { vo[nv] = off - 3; vl[nv] = len + 3; nv++; }
        off += len;
    }
    if (!sps || !pps || nv < 8) { printf("码流不足\n"); return 1; }
    printf("目标：滞后 <= 3（消费者稳态在飞 3 帧）\n\n");

    measure("① 默认（无任何键）", k_none);
    measure("② low-latency=1（当前 daemon 用的）", k_lowlat);
    measure("③ low-latency + max-output-reorder-frames=0", k_maxreorder);
    measure("④ vendor.qti-ext-dec-low-latency.enable=1", k_vendorlowlat);
    measure("⑤ vendor.qti-ext-dec-picture-order.enable=1", k_vendorpicorder);
    measure("⑥ vendor 两个都开", k_vendorboth);
    measure("⑦ low-latency + output-delay=0", k_outputdelay);
    measure("⑧ vendor low-latency + output-delay + reorder=0", k_numref);

    printf("\n若有任一配置滞后 <= 3，就能彻底不用排空/重建，黑帧问题随之消失。\n");
    return 0;
}
