/* 运行时协商的前提：daemon 如何**可靠**判定解码器实际的输出顺序？
 *
 * 设置 vendor 键成功 ≠ 它生效了。非高通平台会静默忽略未知键，
 * 那时解码器仍按显示序输出，而驱动若仍按解码序配对就会画面错位
 * （实测 105/150 帧错位）。所以必须有运行时判据。
 *
 * 这里对比两条候选途径：
 *   途径 A：configure 后用 getOutputFormat 读回该键
 *           —— 简单，但 vendor 键通常不回显，需实测
 *   途径 B：送递增 PTS，看输出 PTS 是否单调
 *           —— 显示序输出 PTS 单调；解码序输出遇 B 帧则非单调
 *
 * 顺带验证途径 C（更根本的解法）：输出帧的 presentationTimeUs 能否
 * 原样回传送入时的值？若能，驱动就可以用 PTS 精确配对 surface，
 * 完全不必知道输出顺序 —— 那样连协商都不需要。
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

/* PTS 用一个好识别的编码：1000000 + i*1000，便于确认原样回传 */
/* 两种步长对比：驱动实际用的是步长 1（单元序号直接当 PTS），
 * 而本探针原先用步长 1000。若步长 1 会被解码器量化/合并，
 * 就会出现多个输入单元共享同一个回传 PTS —— 那正是驱动日志里
 * "unit 5 连续出现两次"的可疑现象。 */
static int g_step = 1000;
#define MKPTS(i) ((int64_t)1000000 + (int64_t)(i) * g_step)

static void run(const char *label, int set_vendor_key)
{
    printf("\n===== %s =====\n", label);

    AMediaCodec *c = AMediaCodec_createDecoderByType("video/avc");
    if (!c) { printf("  创建解码器失败\n"); return; }
    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    AMediaFormat_setInt32(fmt, "low-latency", 1);
    if (set_vendor_key)
        AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);

    if (AMediaCodec_configure(c, fmt, NULL, NULL, 0) != AMEDIA_OK) {
        printf("  configure 失败\n");
        AMediaFormat_delete(fmt); AMediaCodec_delete(c); return;
    }
    AMediaCodec_start(c);

    /* ---- 途径 A：configure/start 后读回该键 ---- */
    {
        AMediaFormat *of = AMediaCodec_getOutputFormat(c);
        if (!of) {
            printf("  途径A: getOutputFormat 返回 NULL（start 后尚无输出格式）\n");
        } else {
            int32_t v = -12345;
            int got = AMediaFormat_getInt32(of,
                        "vendor.qti-ext-dec-picture-order.enable", &v);
            printf("  途径A: 读回 vendor 键 -> %s",
                   got ? "成功" : "失败（键不回显）");
            if (got) printf("，值=%d", v);
            printf("\n");
            AMediaFormat_delete(of);
        }
    }

    unsigned char csd[512];
    memcpy(csd, sps, sps_len);
    memcpy(csd + sps_len, pps, pps_len);
    ssize_t bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
    size_t cap;
    uint8_t *ib = AMediaCodec_getInputBuffer(c, bi, &cap);
    memcpy(ib, csd, sps_len + pps_len);
    AMediaCodec_queueInputBuffer(c, bi, 0, sps_len + pps_len, 0, FLAG_CODEC_CONFIG);

    /* 送 12 个 VCL，收集输出 PTS 序列 */
    int64_t out_pts[32];
    int nout = 0;
    for (int i = 0; i < 12; i++) {
        bi = AMediaCodec_dequeueInputBuffer(c, 1000000);
        if (bi < 0) break;
        ib = AMediaCodec_getInputBuffer(c, bi, &cap);
        memcpy(ib, data + vo[i], vl[i]);
        AMediaCodec_queueInputBuffer(c, bi, 0, vl[i], MKPTS(i), 0);

        for (int spin = 0; spin < 6 && nout < 32; spin++) {
            AMediaCodecBufferInfo info;
            ssize_t oi = AMediaCodec_dequeueOutputBuffer(c, &info, 20000);
            if (oi >= 0) {
                if (info.size > 0) out_pts[nout++] = info.presentationTimeUs;
                AMediaCodec_releaseOutputBuffer(c, oi, false);
            }
        }
    }

    /* ---- 途径 B：输出 PTS 是否单调 ---- */
    printf("  输出 PTS 序列（送入编码为 1000000+i*1000）:\n    ");
    for (int i = 0; i < nout; i++) {
        /* 反解成送入时的序号，便于人眼看顺序 */
        long idx = (long)((out_pts[i] - 1000000) / 1000);
        printf("%ld ", idx);
    }
    printf("\n");

    int dupe = 0;
    for (int i = 0; i < nout; i++)
        for (int j = i + 1; j < nout; j++)
            if (out_pts[i] == out_pts[j]) { dupe++; }
    if (dupe)
        printf("  ⚠️ 有 %d 对重复 PTS —— 无法据此唯一配对!\n", dupe);

    int monotonic = 1;
    for (int i = 1; i < nout; i++)
        if (out_pts[i] < out_pts[i-1]) { monotonic = 0; break; }
    printf("  途径B: PTS %s → 判定为**%s**输出\n",
           monotonic ? "单调递增" : "非单调",
           monotonic ? "显示序" : "解码序");

    /* ---- 途径 C：PTS 是否原样回传（可用于精确配对）---- */
    int exact = 1;
    for (int i = 0; i < nout; i++) {
        if ((out_pts[i] - 1000000) % 1000 != 0 ||
            out_pts[i] < 1000000 || out_pts[i] > 1000000 + 11 * 1000) {
            exact = 0; break;
        }
    }
    printf("  途径C: PTS %s → %s用于精确配对\n",
           exact ? "原样回传（值域与步长都对得上）" : "被改写/重算",
           exact ? "可" : "不可");

    printf("  出帧数 %d\n", nout);

    AMediaCodec_stop(c);
    AMediaCodec_delete(c);
    AMediaFormat_delete(fmt);
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
    if (!sps || !pps || nv < 12) { printf("码流不足\n"); return 1; }

    printf("########## 步长 1000us ##########\n");
    g_step = 1000;
    run("不设 vendor 键（显示序）", 0);
    run("设 vendor 键（跟随输入序）", 1);

    printf("\n########## 步长 1us（驱动实际用法）##########\n");
    g_step = 1;
    run("不设 vendor 键（显示序）", 0);
    run("设 vendor 键（跟随输入序）", 1);

    printf("\n判据选择：途径A 若不回显则不可用；途径B 需要有 B 帧的流才有区分度；\n");
    printf("途径C 若成立则最优 —— 驱动用 PTS 精确配对，无需知道输出顺序。\n");
    return 0;
}
