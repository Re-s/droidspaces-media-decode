/*
 * test_dmd_client.c - dmd_client 库的真机自测程序
 *
 * 用法：
 *   test_dmd_client <码流> <codec: h264|hevc|vp9|vp8> <宽> <高> [tcp|shm]
 *                   [--dump-prefix <前缀>] [--dump-frames N] [--port P]
 *
 * 做三件事：
 *   1) 按 codec 切分数据单元并全部送入，然后 finish_input 触发 flush
 *   2) 收帧计数、校验格式块，可选把前 N 帧原始 NV12 落盘（用于两模式逐字节比对）
 *   3) 退出前打印本进程 fd 数量，用于检测 fd 泄漏
 *
 * 刻意不依赖 FFmpeg：H.264/HEVC 用手写的 Annex B 起始码扫描器切 NALU，
 * IVF（VP8/VP9）用 32 字节文件头 + 12 字节帧头的裸解析。
 *
 * 收发必须交错：daemon 的输出线程写满 socket 缓冲后会阻塞在那儿，
 * 若这里先发完全部输入再收，双方僵死。所以发送循环里每发一个单元就
 * 用 0ms 超时把已就绪的帧收干。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dmd_client.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct opts {
    const char *path;
    const char *codec_name;
    int codec;
    int width;
    int height;
    int want_shm;
    int port;
    const char *dump_prefix;
    int dump_frames;
};

static int count_open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.')
            n++;
    }
    closedir(d);
    return n - 1;   /* 减掉 opendir 自己的那个 fd */
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "打不开 %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *out_len = rd;
    return buf;
}

/* 找从 from 起的下一个 Annex B 起始码，返回其偏移，并给出起始码长度。
 * 找不到返回 len。 */
static size_t next_start_code(const uint8_t *b, size_t len, size_t from,
                              int *sc_len)
{
    for (size_t i = from; i + 3 <= len; i++) {
        if (b[i] == 0 && b[i + 1] == 0) {
            if (b[i + 2] == 1) {
                *sc_len = 3;
                return i;
            }
            if (i + 4 <= len && b[i + 2] == 0 && b[i + 3] == 1) {
                *sc_len = 4;
                return i;
            }
        }
    }
    *sc_len = 0;
    return len;
}

/* 收帧循环的返回约定 */
#define DRAIN_ERR (-1L)
#define DRAIN_EOS (-2L)

/*
 * 收帧。timeout_ms==0 时把已就绪的帧全部收干（发送阶段用，避免 daemon
 * 堵在满的 socket 缓冲上）；timeout_ms>0 时最多取一帧就返回（flush 阶段用）。
 * 返回收到的帧数，或 DRAIN_ERR / DRAIN_EOS。
 */
static long drain_ready(struct dmd_session *s, int timeout_ms,
                        const struct opts *o, long *frames_total,
                        int *fmt_checked)
{
    long got = 0;
    for (;;) {
        struct dmd_frame fr;
        int r = dmd_session_next_frame(s, &fr, timeout_ms);
        if (r == DMD_ERR_TIMEOUT)
            return got;
        if (r == DMD_EOS)
            return got > 0 ? got : DRAIN_EOS;
        if (r != DMD_OK) {
            fprintf(stderr, "收帧失败(%d): %s\n", r, dmd_session_last_error(s));
            return DRAIN_ERR;
        }

        if (!*fmt_checked) {
            const struct dmd_format *f = dmd_session_format(s);
            printf("格式块: 缓冲 %dx%d stride=%d slice_height=%d "
                   "crop=(%d,%d)-(%d,%d) 显示 %dx%d\n",
                   f->buf_width, f->buf_height, f->stride, f->slice_height,
                   f->crop_left, f->crop_top, f->crop_right, f->crop_bottom,
                   dmd_format_display_width(f), dmd_format_display_height(f));
            *fmt_checked = 1;
        }

        if (o->dump_prefix && *frames_total < o->dump_frames) {
            char name[512];
            snprintf(name, sizeof(name), "%s-%03ld.nv12", o->dump_prefix,
                     *frames_total);
            FILE *df = fopen(name, "wb");
            if (df) {
                fwrite(fr.data, 1, fr.size, df);
                fclose(df);
            } else {
                fprintf(stderr, "落盘 %s 失败: %s\n", name, strerror(errno));
            }
        }

        if (*frames_total < 3) {
            printf("帧#%llu %ux%u %zu 字节 slot=%d stride=%d slice=%d\n",
                   (unsigned long long)fr.seq, fr.width, fr.height, fr.size,
                   fr.shm_slot, fr.stride, fr.slice_height);
        }

        (*frames_total)++;
        got++;

        /* 必须归还：SHM 模式下不归还会让 daemon 约 1 秒后判定客户端卡死 */
        int rr = dmd_session_release_frame(s, &fr);
        if (rr != DMD_OK) {
            fprintf(stderr, "归还帧失败(%d): %s\n", rr,
                    dmd_session_last_error(s));
            return DRAIN_ERR;
        }
        if (timeout_ms == 0)
            continue;
        return got;   /* 阻塞模式下一次只取一帧，交给外层循环 */
    }
}

static int run_annexb(struct dmd_session *s, const uint8_t *b, size_t len,
                      const struct opts *o, long *frames, int *fmt_checked,
                      long *units)
{
    int sc_len = 0;
    size_t pos = next_start_code(b, len, 0, &sc_len);
    if (pos >= len) {
        fprintf(stderr, "码流里找不到 Annex B 起始码\n");
        return -1;
    }

    while (pos < len) {
        int next_sc = 0;
        size_t next = next_start_code(b, len, pos + 3, &next_sc);
        size_t unit_len = next - pos;   /* 含起始码，daemon 要求带起始码 */

        int r = dmd_session_send_unit(s, b + pos, unit_len);
        if (r != DMD_OK) {
            fprintf(stderr, "送单元失败(%d): %s\n", r,
                    dmd_session_last_error(s));
            return -1;
        }
        (*units)++;

        /* 交错收：不这么做 daemon 的输出线程会堵在满的 socket 缓冲上 */
        long d = drain_ready(s, 0, o, frames, fmt_checked);
        if (d == DRAIN_ERR)
            return -1;
        if (d == DRAIN_EOS) {
            fprintf(stderr, "daemon 在输入结束前关闭了连接\n");
            return -1;
        }

        pos = next;
    }
    return 0;
}

static int run_ivf(struct dmd_session *s, const uint8_t *b, size_t len,
                   const struct opts *o, long *frames, int *fmt_checked,
                   long *units)
{
    /* IVF: 32 字节文件头，之后每帧 [4B 长度 LE][8B 时间戳 LE][数据]。
     * VP8/VP9 送完整帧且**不能**带起始码。 */
    if (len < 32 || memcmp(b, "DKIF", 4) != 0) {
        fprintf(stderr, "不是 IVF 文件（VP8/VP9 需要 IVF 容器）\n");
        return -1;
    }
    size_t pos = 32;
    while (pos + 12 <= len) {
        uint32_t fsz = (uint32_t)b[pos] | ((uint32_t)b[pos + 1] << 8) |
                       ((uint32_t)b[pos + 2] << 16) | ((uint32_t)b[pos + 3] << 24);
        pos += 12;
        if (fsz == 0 || pos + fsz > len)
            break;
        int r = dmd_session_send_unit(s, b + pos, fsz);
        if (r != DMD_OK) {
            fprintf(stderr, "送 IVF 帧失败(%d): %s\n", r,
                    dmd_session_last_error(s));
            return -1;
        }
        (*units)++;
        pos += fsz;
        long d = drain_ready(s, 0, o, frames, fmt_checked);
        if (d == DRAIN_ERR)
            return -1;
        if (d == DRAIN_EOS) {
            fprintf(stderr, "daemon 在输入结束前关闭了连接\n");
            return -1;
        }
    }
    return 0;
}

static int parse_codec(const char *n)
{
    if (!strcmp(n, "h264")) return DMD_CODEC_H264;
    if (!strcmp(n, "hevc") || !strcmp(n, "h265")) return DMD_CODEC_HEVC;
    if (!strcmp(n, "vp9"))  return DMD_CODEC_VP9;
    if (!strcmp(n, "vp8"))  return DMD_CODEC_VP8;
    return -1;
}

int main(int argc, char **argv)
{
    struct opts o;
    memset(&o, 0, sizeof(o));
    o.port = 20003;
    o.dump_frames = 3;

    if (argc < 5) {
        fprintf(stderr,
                "用法: %s <码流> <h264|hevc|vp9|vp8> <宽> <高> [tcp|shm]\n"
                "         [--dump-prefix P] [--dump-frames N] [--port P]\n",
                argv[0]);
        return 2;
    }
    o.path = argv[1];
    o.codec_name = argv[2];
    o.codec = parse_codec(argv[2]);
    o.width = atoi(argv[3]);
    o.height = atoi(argv[4]);
    if (o.codec < 0) {
        fprintf(stderr, "未知 codec: %s\n", argv[2]);
        return 2;
    }

    int ai = 5;
    if (ai < argc && argv[ai][0] != '-') {
        o.want_shm = !strcmp(argv[ai], "shm");
        ai++;
    }
    for (; ai < argc; ai++) {
        if (!strcmp(argv[ai], "--dump-prefix") && ai + 1 < argc)
            o.dump_prefix = argv[++ai];
        else if (!strcmp(argv[ai], "--dump-frames") && ai + 1 < argc)
            o.dump_frames = atoi(argv[++ai]);
        else if (!strcmp(argv[ai], "--port") && ai + 1 < argc)
            o.port = atoi(argv[++ai]);
        else {
            fprintf(stderr, "未知参数: %s\n", argv[ai]);
            return 2;
        }
    }

    int fd_before = count_open_fds();

    size_t len = 0;
    uint8_t *data = read_file(o.path, &len);
    if (!data)
        return 1;

    struct dmd_session_config cfg;
    dmd_session_config_init(&cfg);
    cfg.port = (uint16_t)o.port;
    cfg.codec = o.codec;
    cfg.width = o.width;
    cfg.height = o.height;
    cfg.want_shm = o.want_shm;

    struct dmd_error err;
    struct dmd_session *s = dmd_session_create(&cfg, &err);
    if (!s) {
        fprintf(stderr, "建立会话失败(%d, hs=%d): %s\n", err.code,
                err.handshake_status, err.msg);
        free(data);
        return 1;
    }

    int mode = dmd_session_xfer_mode(s);
    printf("请求模式=%s 实际模式=%s\n", o.want_shm ? "SHM" : "TCP",
           mode == DMD_XFER_SHM ? "SHM" : "TCP");
    if (o.want_shm && mode != DMD_XFER_SHM)
        printf("注意: 已降级为 TCP（fallback 生效）\n");

    long frames = 0, units = 0;
    int fmt_checked = 0;
    int rc = 0;

    if (o.codec == DMD_CODEC_H264 || o.codec == DMD_CODEC_HEVC)
        rc = run_annexb(s, data, len, &o, &frames, &fmt_checked, &units);
    else
        rc = run_ivf(s, data, len, &o, &frames, &fmt_checked, &units);

    if (rc == 0) {
        /* 送完必须关写端，否则取不到解码器里排队的尾部帧 */
        int r = dmd_session_finish_input(s);
        if (r != DMD_OK) {
            fprintf(stderr, "finish_input 失败(%d): %s\n", r,
                    dmd_session_last_error(s));
            rc = -1;
        }
    }

    if (rc == 0) {
        /* flush 阶段：走同一条 drain 路径（落盘/格式打印/归还逻辑不分叉），
         * 每次最多阻塞 3 秒，直到 EOS 或超时 */
        for (;;) {
            long d = drain_ready(s, 3000, &o, &frames, &fmt_checked);
            if (d == DRAIN_EOS)
                break;
            if (d == DRAIN_ERR) {
                rc = -1;
                break;
            }
            if (d == 0) {
                printf("flush 阶段等待超时，收尾\n");
                break;
            }
        }
    }

    printf("统计: 送入单元=%ld (库计数 %llu) 收到帧=%ld (库计数 %llu)\n",
           units, (unsigned long long)dmd_session_units_sent(s), frames,
           (unsigned long long)dmd_session_frames_received(s));

    const struct dmd_format *f = dmd_session_format(s);
    printf("最终格式: 缓冲 %dx%d stride=%d slice=%d 显示 %dx%d 格式块次数=%d\n",
           f->buf_width, f->buf_height, f->stride, f->slice_height,
           dmd_format_display_width(f), dmd_format_display_height(f),
           f->changes);

    dmd_session_destroy(s);
    free(data);

    int fd_after = count_open_fds();
    printf("fd 计数: 之前=%d 之后=%d %s\n", fd_before, fd_after,
           (fd_before == fd_after) ? "无泄漏" : "有差异!");
    if (fd_before != fd_after)
        rc = -1;

    printf("结果: %s\n", rc == 0 ? "OK" : "FAILED");
    return rc == 0 ? 0 : 1;
}
