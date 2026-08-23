/*
 * probe_shm_fallback.c - 验证「mode=SHM 只是意向」这条规格
 *
 * 这个探针**不使用 dmd_client 库**，直接讲裸协议，目的是独立验证
 * daemon 的降级行为（库的 fallback 逻辑正确性依赖于它）：
 *
 *   1) 握手时请求 SHM，daemon 回 [status=0][mode=SHM][name_len][name]
 *   2) 故意**不**去 connect 那个 abstract socket
 *   3) daemon 的 shm_handoff 用 select 等 3 秒（decode-daemon.c:606），
 *      超时后 shm_teardown 并把 s->xfer 置回 XFER_TCP（:412-416）
 *   4) 此后送 NALU，帧应当以**普通 TCP 帧格式**回来，而不是 SHM 控制消息
 *
 * 若第 4 步收到 0xFFFFFFFE 哨兵，说明规格描述有误、库的 fallback 不成立。
 *
 * 用法: probe_shm_fallback <h264 码流> [端口]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAGIC     0x444D4400u
#define VERSION   2u
#define FMTDESC   0xFFFFFFFFu
#define SHMFRAME  0xFFFFFFFEu

static int rd(int fd, void *b, size_t n)
{
    uint8_t *p = b;
    size_t g = 0;
    while (g < n) {
        ssize_t r = recv(fd, p + g, n - g, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        g += (size_t)r;
    }
    return 0;
}

static int wr(int fd, const void *b, size_t n)
{
    const uint8_t *p = b;
    size_t s = 0;
    while (s < n) {
        ssize_t r = send(fd, p + s, n - s, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        s += (size_t)r;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <h264 码流> [端口]\n", argv[0]);
        return 2;
    }
    int port = (argc > 2) ? atoi(argv[2]) : 20003;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "打不开码流\n"); return 1; }
    static uint8_t buf[4 << 20];
    size_t len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (len < 64) { fprintf(stderr, "码流太小\n"); return 1; }

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("connect"); close(fd); return 1;
    }

    /* 24 字节握手，请求 SHM */
    uint32_t hello[6] = { htonl(MAGIC), htonl(VERSION), htonl(0),
                          htonl(1920), htonl(1080), htonl(1) };
    if (wr(fd, hello, sizeof(hello)) < 0) { fprintf(stderr, "握手发送失败\n"); return 1; }

    uint32_t head[3];
    if (rd(fd, head, sizeof(head)) < 0) { fprintf(stderr, "无握手响应\n"); return 1; }
    uint32_t status = ntohl(head[0]), mode = ntohl(head[1]), nlen = ntohl(head[2]);
    char name[64];
    memset(name, 0, sizeof(name));
    if (nlen > 0 && nlen < sizeof(name) && rd(fd, name, nlen) < 0) {
        fprintf(stderr, "读名字失败\n"); return 1;
    }
    printf("握手响应: status=%u mode=%u name_len=%u name=@%s\n",
           status, mode, nlen, name);
    if (status != 0) { fprintf(stderr, "被拒绝\n"); return 1; }
    printf("响应声明模式=%s（此时 memfd 尚未交接 —— 这正是意向不是保证的证据）\n",
           mode == 1 ? "SHM" : "TCP");

    /* 故意不领 memfd，等过 daemon 的 3 秒交接窗口 */
    printf("故意不领取 memfd，睡 4 秒等 daemon 的 3 秒窗口超时...\n");
    sleep(4);

    /* 送前 40 个 NALU，同时看回来的帧是什么格式 */
    int sent = 0;
    size_t pos = 0;
    while (pos + 4 <= len && buf[pos] != 0) pos++;
    for (size_t i = pos; i + 4 < len && sent < 40; ) {
        size_t j = i + 3;
        while (j + 3 <= len && !(buf[j] == 0 && buf[j+1] == 0 &&
               (buf[j+2] == 1 || (j + 4 <= len && buf[j+2] == 0 && buf[j+3] == 1))))
            j++;
        if (j + 3 > len) j = len;
        uint32_t be = htonl((uint32_t)(j - i));
        if (wr(fd, &be, 4) < 0 || wr(fd, buf + i, j - i) < 0) {
            fprintf(stderr, "送 NALU 失败（daemon 可能已结束会话）\n");
            break;
        }
        sent++;
        i = j;
    }
    printf("已送入 %d 个 NALU\n", sent);
    shutdown(fd, SHUT_WR);

    /* 收帧头，判断格式 */
    int tcp_frames = 0, shm_msgs = 0, fmt_blocks = 0;
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (;;) {
        uint32_t h3[3];
        if (rd(fd, h3, sizeof(h3)) < 0) break;
        uint32_t sz = ntohl(h3[2]);
        if (sz == FMTDESC) {
            uint32_t fw[8];
            if (rd(fd, fw, sizeof(fw)) < 0) break;
            fmt_blocks++;
        } else if (sz == SHMFRAME) {
            uint32_t si[2];
            if (rd(fd, si, sizeof(si)) < 0) break;
            shm_msgs++;
        } else {
            static uint8_t fbuf[16 << 20];
            if (sz > sizeof(fbuf) || rd(fd, fbuf, sz) < 0) break;
            tcp_frames++;
        }
    }
    close(fd);

    printf("结果: 格式块=%d TCP帧=%d SHM控制消息=%d\n",
           fmt_blocks, tcp_frames, shm_msgs);
    if (shm_msgs > 0) {
        printf("判定: 失败 —— 交接未完成 daemon 仍发 SHM 消息，规格描述有误\n");
        return 1;
    }
    if (tcp_frames > 0) {
        printf("判定: 通过 —— 交接超时后 daemon 静默退回 TCP 帧格式，"
               "客户端自带 fallback 是必要且充分的\n");
        return 0;
    }
    printf("判定: 不确定 —— 一帧都没收到\n");
    return 1;
}
