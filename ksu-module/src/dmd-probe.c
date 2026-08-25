/*
 * dmd-probe —— decode-daemon 端点探活工具（供 KSU 守护模块使用）
 *
 * 为什么不用 kill(pid,0)：
 *   1) 僵尸进程 kill(pid,0) 仍返回 0，看起来"活着"
 *   2) daemon 有会话级失效模式（SHM 交接失败等）——进程活着、持着 flock，
 *      但已不能服务新会话。进程级判据看不见这一类。
 * 所以这里做真实探活：connect + 完整握手，daemon 回 status=0 才算健康。
 *
 * 退出码（守护脚本按此分流）：
 *   0 = 健康（握手成功）
 *   1 = 端点不存在 / connect 失败（daemon 没在跑，或 socket 是死引用）
 *   2 = 连上了但握手失败（进程活着却不服务 —— 需要重启它）
 *   3 = 参数错误
 *   7 = endpoint inode 不匹配（连上的不是这个端点，挂载配置问题，重启 daemon 无用）
 *
 * 协议：请求 24B 大端 [magic][ver][codec][w][h][xfer]
 *       响应 12B [status][xfer][namelen]，v3 下 namelen 的 bit31 置位时
 *       其后追加 16B endpoint dev/ino。与 src/decode-daemon.c 保持一致。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <arpa/inet.h>

#define HELLO_MAGIC   0x444D4400u
#define HELLO_VERSION 3u
#define PROBE_W       640
#define PROBE_H       480

/* 带超时的全量读写：守护进程里绝不能因为对端半死而永久阻塞 */
static int io_all(int fd, void *buf, size_t len, int writing, int timeout_ms)
{
    unsigned char *p = buf;
    size_t done = 0;
    while (done < len) {
        struct pollfd pfd = { .fd = fd, .events = writing ? POLLOUT : POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0)
            return -1;                      /* 超时或 poll 出错 */
        ssize_t n = writing ? write(fd, p + done, len - done)
                            : read(fd, p + done, len - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n == 0)
            return -1;                      /* 对端关闭 */
        if (errno == EINTR || errno == EAGAIN)
            continue;
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: dmd-probe <socket 路径> [超时毫秒，默认 3000]\n");
        return 3;
    }
    const char *path = argv[1];
    int timeout_ms = (argc > 2) ? atoi(argv[2]) : 3000;
    if (timeout_ms <= 0)
        timeout_ms = 3000;

    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "probe: 端点不存在 %s: %s\n", path, strerror(errno));
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "probe: socket(): %s\n", strerror(errno));
        return 1;
    }
    struct sockaddr_un ua;
    memset(&ua, 0, sizeof(ua));
    ua.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(ua.sun_path)) {
        fprintf(stderr, "probe: 路径过长\n");
        close(fd);
        return 3;
    }
    memcpy(ua.sun_path, path, strlen(path));

    if (connect(fd, (struct sockaddr *)&ua, sizeof(ua)) != 0) {
        /* 目录级挂载下这通常意味着 daemon 真的没在跑；
         * 文件级挂载下也可能是挂载钉着死 inode（那要修挂载，不是重启 daemon）。 */
        fprintf(stderr, "probe: connect 失败: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    uint32_t hello[6] = {
        htonl(HELLO_MAGIC), htonl(HELLO_VERSION), htonl(0),
        htonl(PROBE_W), htonl(PROBE_H), htonl(0)
    };
    if (io_all(fd, hello, sizeof(hello), 1, timeout_ms) != 0) {
        fprintf(stderr, "probe: 发送握手失败/超时\n");
        close(fd);
        return 2;
    }

    uint32_t head[3];
    if (io_all(fd, head, sizeof(head), 0, timeout_ms) != 0) {
        fprintf(stderr, "probe: 未收到握手响应/超时（进程可能活着但不服务）\n");
        close(fd);
        return 2;
    }
    uint32_t status = ntohl(head[0]);
    uint32_t nlen_w = ntohl(head[2]);

    if (status == 1) {
        /* 旧 daemon 严格判版本会拒 v3。它仍是健康的，只是版本旧，
         * 用 v2 再探一次，避免守护把好 daemon 当坏的杀掉。 */
        close(fd);
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return 1;
        if (connect(fd, (struct sockaddr *)&ua, sizeof(ua)) != 0) {
            close(fd);
            return 1;
        }
        hello[1] = htonl(2u);
        if (io_all(fd, hello, sizeof(hello), 1, timeout_ms) != 0 ||
            io_all(fd, head, sizeof(head), 0, timeout_ms) != 0) {
            close(fd);
            return 2;
        }
        status = ntohl(head[0]);
        nlen_w = ntohl(head[2]);
        if (status == 0)
            printf("probe: 健康（协议 v2，旧 daemon）\n");
    }

    if (status != 0) {
        fprintf(stderr, "probe: daemon 拒绝握手 status=%u\n", status);
        close(fd);
        return 2;
    }

    /* v3 扩展存在时顺手做 inode 对账：不匹配说明连上的不是这个路径背后的
     * 端点，重启 daemon 解决不了（是挂载/配置问题），单独用退出码 7 区分。 */
    if (nlen_w >> 31) {
        uint32_t ext[4];
        if (io_all(fd, ext, sizeof(ext), 0, timeout_ms) == 0) {
            unsigned long long dev = ((unsigned long long)ntohl(ext[0]) << 32) | ntohl(ext[1]);
            unsigned long long ino = ((unsigned long long)ntohl(ext[2]) << 32) | ntohl(ext[3]);
            if (dev != (unsigned long long)st.st_dev ||
                ino != (unsigned long long)st.st_ino) {
                fprintf(stderr, "probe: endpoint inode 不匹配 "
                        "stat(dev=%llu,ino=%llu) != daemon(dev=%llu,ino=%llu)\n",
                        (unsigned long long)st.st_dev,
                        (unsigned long long)st.st_ino, dev, ino);
                close(fd);
                return 7;
            }
            printf("probe: 健康 dev=%llu ino=%llu\n", dev, ino);
            close(fd);
            return 0;
        }
    }

    printf("probe: 健康（无 endpoint 扩展）\n");
    close(fd);
    return 0;
}
