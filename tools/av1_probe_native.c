/*
 * av1_probe_native —— AV1 直送探针的 Android 原生版本。
 *
 * 与 tools/av1_probe.py 同样的用途：绕过 VA-API 驱动，把 OBU temporal unit
 * 按 daemon 线路协议直接喂进去，判定"MediaCodec 不出帧"是合成问题还是
 * 解码器侧问题。
 *
 * 为什么需要原生版：Python 版要在 Linux 容器里跑，而容器可能没启动
 * （rootfs.img 未挂载时 ssh 不通）。本程序交叉编译成 aarch64 静态可执行，
 * 用 adb + root 直接在 Android 侧运行，不依赖容器。
 *
 * 用法:
 *     av1_probe_native <port> <file.obu> [单元数] [burst]
 *
 *     burst 模式：一次性灌入全部单元再统一收帧。用于解码器声明
 *     output delay N 的场合 —— 逐单元等帧会死锁（它要收满 N 个输入
 *     才吐首帧，而我们在第 1 个就阻塞等待）。
 *
 * 交叉编译（NDK）:
 *     aarch64-linux-android21-clang -O2 -static -o av1_probe_native \
 *         tools/av1_probe_native.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HELLO_MAGIC        0x444D4400u
#define HELLO_VERSION      3u
#define CODEC_AV1          4u
#define XFER_TCP           0u
#define FMTDESC_SENTINEL   0xFFFFFFFFu
#define SHMFRAME_SENTINEL  0xFFFFFFFEu

#define MAX_UNITS 4096

struct unit { size_t off, len; };

static int send_all(int fd, const void *b, size_t l)
{
    const char *p = b;
    while (l > 0) {
        ssize_t n = write(fd, p, l);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; l -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *b, size_t l)
{
    char *p = b;
    while (l > 0) {
        ssize_t n = read(fd, p, l);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; l -= (size_t)n;
    }
    return 0;
}

/* 按 OBU_TEMPORAL_DELIMITER(type=2) 把裸 OBU 流切成 temporal unit。 */
static int split_tu(const uint8_t *d, size_t len, struct unit *out, int cap)
{
    int n = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t h = d[i];
        int type = (h >> 3) & 0xF;
        int has_size = (h >> 1) & 1;
        size_t j = i + 1;
        if ((h >> 2) & 1) j++;                    /* extension flag */
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
        size_t obu_end = j + size;
        if (obu_end > len) break;

        if (type == 2) {                          /* TD 开启新单元 */
            if (n >= cap) break;
            out[n].off = i;
            out[n].len = obu_end - i;
            n++;
        } else if (n > 0) {
            out[n - 1].len = obu_end - out[n - 1].off;
        } else {
            if (n >= cap) break;
            out[n].off = i;
            out[n].len = obu_end - i;
            n++;
        }
        i = obu_end;
    }
    return n;
}

static void set_timeout(int fd, int sec)
{
    struct timeval tv = { .tv_sec = sec, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* 收一帧。返回 1=收到帧，0=收到控制消息，-1=超时/断开。 */
static int recv_one(int fd, int *frames)
{
    uint32_t hdr[3];
    if (recv_all(fd, hdr, sizeof(hdr)) < 0) return -1;
    uint32_t w = ntohl(hdr[0]), h = ntohl(hdr[1]), sz = ntohl(hdr[2]);

    if (sz == FMTDESC_SENTINEL) {
        uint8_t desc[32];
        if (recv_all(fd, desc, sizeof(desc)) < 0) return -1;
        printf("  收到格式描述块\n");
        return 0;
    }
    if (sz == SHMFRAME_SENTINEL) {
        uint32_t rest[3];
        if (recv_all(fd, rest, sizeof(rest)) < 0) return -1;
        printf("  收到 SHM 帧通知（本探针只用 TCP，异常）\n");
        return 0;
    }
    uint8_t *buf = malloc(sz);
    if (!buf) return -1;
    if (recv_all(fd, buf, sz) < 0) { free(buf); return -1; }
    free(buf);
    (*frames)++;
    printf("  帧 %d: %ux%u %u 字节\n", *frames, w, h, sz);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <port> <file.obu> [单元数] [burst]\n", argv[0]);
        return 2;
    }
    int port = atoi(argv[1]);
    const char *path = argv[2];
    int limit = (argc > 3) ? atoi(argv[3]) : 0;
    int burst = (argc > 4 && strcmp(argv[4], "burst") == 0);

    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)fsz);
    if (!data || fread(data, 1, (size_t)fsz, f) != (size_t)fsz) {
        fprintf(stderr, "读文件失败\n"); return 1;
    }
    fclose(f);

    struct unit *units = calloc(MAX_UNITS, sizeof(*units));
    int nu = split_tu(data, (size_t)fsz, units, MAX_UNITS);
    if (limit > 0 && limit < nu) nu = limit;
    printf("切出 %d 个 temporal unit，前 3 个长度: %zu %zu %zu\n", nu,
           nu > 0 ? units[0].len : 0, nu > 1 ? units[1].len : 0,
           nu > 2 ? units[2].len : 0);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET,
                              .sin_port = htons((uint16_t)port) };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect"); return 1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    uint32_t hello[6] = { htonl(HELLO_MAGIC), htonl(HELLO_VERSION),
                          htonl(CODEC_AV1), htonl(1920), htonl(1080),
                          htonl(XFER_TCP) };
    if (send_all(fd, hello, sizeof(hello)) < 0) { perror("握手发送"); return 1; }

    set_timeout(fd, 10);
    uint32_t resp[3];
    if (recv_all(fd, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "握手无响应\n"); return 1;
    }
    uint32_t status = ntohl(resp[0]), acc_xfer = ntohl(resp[1]);
    printf("握手响应: status=%u xfer=%u\n", status, acc_xfer);
    if (status != 0) { fprintf(stderr, "daemon 拒绝握手\n"); return 1; }
    if (HELLO_VERSION >= 3) {
        uint32_t ext[4];
        recv_all(fd, ext, sizeof(ext));      /* endpoint dev/ino，忽略内容 */
    }

    int frames = 0;

    if (burst) {
        for (int i = 0; i < nu; i++) {
            uint32_t l = htonl((uint32_t)units[i].len);
            if (send_all(fd, &l, 4) < 0 ||
                send_all(fd, data + units[i].off, units[i].len) < 0) {
                printf("  第 %d 个单元发送失败\n", i + 1);
                break;
            }
        }
        printf("  已突发送入 %d 个单元，开始收帧\n", nu);
        set_timeout(fd, 8);
        while (recv_one(fd, &frames) >= 0) { }
    } else {
        for (int i = 0; i < nu; i++) {
            uint32_t l = htonl((uint32_t)units[i].len);
            if (send_all(fd, &l, 4) < 0 ||
                send_all(fd, data + units[i].off, units[i].len) < 0) {
                printf("  第 %d 个单元发送失败\n", i + 1);
                break;
            }
            set_timeout(fd, 3);
            int r;
            while ((r = recv_one(fd, &frames)) == 0) { }
            if (r < 0 && i == 0) printf("  第 1 个单元送完，累计回帧 %d\n", frames);
        }
    }

    /* 排空：长度 0 触发 EOS，逼出流水线里压着的帧。 */
    uint32_t zero = 0;
    if (send_all(fd, &zero, 4) == 0) {
        set_timeout(fd, 5);
        while (recv_one(fd, &frames) >= 0) { }
    }

    close(fd);
    printf("结论: 送入 %d 单元，回传 %d 帧\n", nu, frames);
    return 0;
}
