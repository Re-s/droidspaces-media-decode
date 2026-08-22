/* 测量 MediaCodec 在低延迟模式下的真实输出滞后。
 *
 * 方法：逐个送 VCL 单元，每送一个就以 800ms 超时试取一帧，
 * 记录"第几个单元送进去之后第一帧才出来"。这个数字就是滞后深度。
 * 浏览器里 ffmpeg 稳态只保持 3 帧在飞，所以滞后 >=4 就会死锁。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmd_client.h"

static unsigned char *buf;
static long buflen;

static int load(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); buflen = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(buflen);
    if (fread(buf, 1, buflen, f) != (size_t)buflen) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* 找下一个起始码，返回 NALU 起点（含起始码）*/
static long next_sc(long from)
{
    for (long i = from; i + 3 < buflen; i++) {
        if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1) return i;
        if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1) return i;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/root/decode-test/test1080.h264";
    if (load(path) < 0) { fprintf(stderr, "读不了 %s\n", path); return 1; }

    struct dmd_session_config cfg;
    dmd_session_config_init(&cfg);
    cfg.codec = DMD_CODEC_H264;
    cfg.width = 1920; cfg.height = 1080;
    struct dmd_error err;
    memset(&err, 0, sizeof(err));
    struct dmd_session *s = dmd_session_create(&cfg, &err);
    if (!s) { fprintf(stderr, "会话建立失败 code=%d\n", err.code); return 1; }
    int rc;

    int sent = 0, first_frame_after = -1, frames = 0;
    long off = next_sc(0);
    while (off >= 0 && sent < 12) {
        long end = next_sc(off + 3);
        long len = (end < 0 ? buflen : end) - off;
        /* 跳过起始码取 NAL 头判类型 */
        long sc = (buf[off+2] == 1) ? 3 : 4;
        int nut = buf[off + sc] & 0x1f;

        if (nut == 1 || nut == 5) {   /* 只送 VCL，模拟驱动行为 */
            rc = dmd_session_send_unit(s, buf + off, len);
            if (rc != DMD_OK) { fprintf(stderr, "送单元失败 rc=%d\n", rc); break; }
            sent++;

            struct dmd_frame f;
            memset(&f, 0, sizeof(f));
            rc = dmd_session_next_frame(s, &f, 800);
            if (rc == DMD_OK) {
                frames++;
                if (first_frame_after < 0) first_frame_after = sent;
                printf("送第 %d 个 VCL 后取到帧（累计 %d 帧）\n", sent, frames);
                dmd_session_release_frame(s, &f);
            } else {
                printf("送第 %d 个 VCL 后 800ms 内无帧 (rc=%d)\n", sent, rc);
            }
        } else {
            /* 参数集：驱动会合成后单独送，这里也送以贴近真实 */
            dmd_session_send_unit(s, buf + off, len);
        }
        off = end;
    }

    printf("\n=== 结论 ===\n");
    printf("首帧出现在送入第 %d 个 VCL 之后\n", first_frame_after);
    printf("滞后深度 = %d\n", first_frame_after);
    printf("浏览器 ffmpeg 稳态在飞 3 帧：滞后 <=3 则不死锁，>=4 死锁\n");
    printf("判定：%s\n", (first_frame_after > 0 && first_frame_after <= 3)
                            ? "✓ 不会死锁" : "✗ 仍会死锁");

    dmd_session_destroy(s);
    free(buf);
    return 0;
}
