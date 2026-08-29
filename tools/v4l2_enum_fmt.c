/*
 * v4l2_enum_fmt —— 直接枚举 V4L2 视频节点支持的编解码格式。
 *
 * 用途：绕开 MediaCodec / Codec2 / vendor HAL 的全部抽象层，问内核驱动
 * "这颗芯片到底支持哪些格式"。这是判定某个 codec 是否有硬件支持的终极判据 ——
 * MediaCodec 的 codec 列表来自 XML 配置，可能声明了硬件实际不支持的格式。
 *
 * 背景：SM8750 上 c2.qti.av1.decoder 在组件创建阶段即失败
 * （QueryrequiredInfos from downstream failed），且 dshmon 报 vidcHw=false。
 * 需要确认 vidc 硬件是否真的具备 AV1 解码能力。
 *
 * 用法（需 root，视频节点属 system:camera）:
 *     v4l2_enum_fmt /dev/video32        # 解码节点
 *     v4l2_enum_fmt /dev/video33        # 编码节点
 *
 * 交叉编译（NDK）:
 *     aarch64-linux-android21-clang -O2 -static -o v4l2_enum_fmt \
 *         tools/v4l2_enum_fmt.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void fourcc_str(unsigned int f, char out[5])
{
    out[0] = (char)(f & 0xFF);
    out[1] = (char)((f >> 8) & 0xFF);
    out[2] = (char)((f >> 16) & 0xFF);
    out[3] = (char)((f >> 24) & 0xFF);
    out[4] = 0;
}

static void enum_one(int fd, enum v4l2_buf_type type, const char *label)
{
    printf("--- %s ---\n", label);
    int found = 0;
    for (unsigned int i = 0; i < 64; i++) {
        struct v4l2_fmtdesc fd_desc;
        memset(&fd_desc, 0, sizeof(fd_desc));
        fd_desc.index = i;
        fd_desc.type = type;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fd_desc) < 0) break;
        char cc[5];
        fourcc_str(fd_desc.pixelformat, cc);
        printf("  [%u] %-6s %s%s\n", i, cc, fd_desc.description,
               (fd_desc.flags & V4L2_FMT_FLAG_COMPRESSED) ? "  (compressed)" : "");
        found++;
    }
    if (!found) printf("  (无)\n");
}

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/video32";
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "打开 %s 失败: %s\n", dev, strerror(errno));
        return 1;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        printf("设备: %s\n", dev);
        printf("  driver=%s card=%s bus=%s\n", cap.driver, cap.card, cap.bus_info);
        printf("  capabilities=0x%08x\n", cap.capabilities);
    }

    /* 输出队列（OUTPUT）= 喂进去的编码码流；捕获队列（CAPTURE）= 取出的原始帧。
     * 解码器的压缩格式在 OUTPUT 侧，所以判断"支持解什么"要看 OUTPUT。 */
    enum_one(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "OUTPUT_MPLANE（喂入的码流格式）");
    enum_one(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAPTURE_MPLANE（取出的像素格式）");
    enum_one(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, "OUTPUT（单平面）");
    enum_one(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, "CAPTURE（单平面）");

    close(fd);
    return 0;
}
