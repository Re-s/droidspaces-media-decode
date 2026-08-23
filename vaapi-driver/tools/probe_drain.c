/* MediaCodec 在 EOS + flush 之后还能继续解码吗？
 *
 * 这决定 daemon 能不能提供"可逆的排空"：
 * 现在驱动为了拿到帧只能 finish_input（不可逆 shutdown(SHUT_WR)），
 * 之后必须重建会话 —— 实测每帧 155 ms、播放慢 4.7 倍。
 *
 * 若 EOS 后 AMediaCodec_flush + 重送 CSD 能让同一个 codec 实例继续工作，
 * daemon 就能把"催出帧"做成不销毁会话的操作，省掉重建。
 *
 * 注意这是在**容器外**（Android 侧）跑的，直接用 NDK MediaCodec，
 * 不经过 daemon 协议 —— 目的是先验证解码器语义，再决定要不要改 daemon。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define FLAG_CODEC_CONFIG 2
#define FLAG_EOS          4

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

/* 排空：送 EOS，收完所有帧，返回收到的帧数 */
static int drain(AMediaCodec *c)
{
    ssize_t bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
    if (bi < 0) { printf("  排空: 取不到输入缓冲 %zd\n", bi); return -1; }
    AMediaCodec_queueInputBuffer(c, bi, 0, 0, 0, FLAG_EOS);

    int got = 0;
    for (int spin = 0; spin < 300; spin++) {
        AMediaCodecBufferInfo info;
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(c, &info, 20000);
        if (oi >= 0) {
            if (info.size > 0) got++;
            int eos = (info.flags & FLAG_EOS) != 0;
            AMediaCodec_releaseOutputBuffer(c, oi, false);
            if (eos) break;
        }
    }
    return got;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/data/local/tmp/test1080.h264";
    if (load(path) < 0) { printf("读不了 %s\n", path); return 1; }

    unsigned char *sps = NULL, *pps = NULL;
    long sps_len = 0, pps_len = 0, off = 0, len;
    long vo[64], vl[64];
    int nv = 0;
    while ((off = next_nalu(off, &len)) >= 0 && nv < 64) {
        int nut = data[off] & 0x1f;
        if (nut == 7 && !sps) { sps = data + off - 3; sps_len = len + 3; }
        else if (nut == 8 && !pps) { pps = data + off - 3; pps_len = len + 3; }
        else if (nut == 1 || nut == 5) { vo[nv] = off - 3; vl[nv] = len + 3; nv++; }
        off += len;
    }
    if (!sps || !pps || nv < 12) { printf("码流不足\n"); return 1; }

    AMediaCodec *c = AMediaCodec_createDecoderByType("video/avc");
    if (!c) { printf("建解码器失败\n"); return 1; }
    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    if (AMediaCodec_configure(c, fmt, NULL, NULL, 0) != AMEDIA_OK) {
        printf("configure 失败\n"); return 1;
    }
    AMediaCodec_start(c);

    /* CSD */
    unsigned char csd[256];
    memcpy(csd, sps, sps_len);
    memcpy(csd + sps_len, pps, pps_len);
    ssize_t bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
    size_t cap;
    uint8_t *ib = AMediaCodec_getInputBuffer(c, bi, &cap);
    memcpy(ib, csd, sps_len + pps_len);
    AMediaCodec_queueInputBuffer(c, bi, 0, sps_len + pps_len, 0, FLAG_CODEC_CONFIG);

    /* 第一段：送 3 帧（模拟浏览器稳态），排空 */
    printf("--- 第一段：送 3 个 VCL 后排空 ---\n");
    for (int i = 0; i < 3; i++) {
        bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
        ib = AMediaCodec_getInputBuffer(c, bi, &cap);
        memcpy(ib, data + vo[i], vl[i]);
        AMediaCodec_queueInputBuffer(c, bi, 0, vl[i], i * 33333, 0);
    }
    int g1 = drain(c);
    printf("  排空取到 %d 帧\n", g1);

    /* 关键一步：flush 能否让它复活 */
    printf("--- flush 后重送 CSD，继续送第 4..9 个 VCL ---\n");
    media_status_t fs = AMediaCodec_flush(c);
    printf("  AMediaCodec_flush -> %d (0=OK)\n", fs);

    bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
    if (bi < 0) {
        printf("  ✗ flush 后取不到输入缓冲（%zd）→ EOS 不可逆，必须重建\n", bi);
        return 2;
    }
    ib = AMediaCodec_getInputBuffer(c, bi, &cap);
    memcpy(ib, csd, sps_len + pps_len);
    AMediaCodec_queueInputBuffer(c, bi, 0, sps_len + pps_len, 0, FLAG_CODEC_CONFIG);

    for (int i = 3; i < 9; i++) {
        bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
        if (bi < 0) { printf("  第%d个VCL取不到输入缓冲\n", i); break; }
        ib = AMediaCodec_getInputBuffer(c, bi, &cap);
        memcpy(ib, data + vo[i], vl[i]);
        AMediaCodec_queueInputBuffer(c, bi, 0, vl[i], i * 33333, 0);
    }
    int g2 = drain(c);
    printf("  第二段排空取到 %d 帧\n", g2);

    printf("\n=== 结论 ===\n");
    if (fs == AMEDIA_OK && g2 > 0)
        printf("✓ EOS + flush 可逆：同一 codec 实例能继续解码\n"
               "  → daemon 可以提供\"不销毁会话的排空\"，省掉每帧重建\n");
    else
        printf("✗ EOS 后即使 flush 也拿不到新帧（g2=%d）\n"
               "  → 排空必然作废实例，当前的重建方案已是必需\n", g2);

    AMediaCodec_stop(c);
    AMediaCodec_delete(c);
    AMediaFormat_delete(fmt);
    return 0;
}
