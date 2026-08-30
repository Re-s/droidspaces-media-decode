/*
 * probe_v4l2_ctrls —— 枚举 /dev/video32 的全部 V4L2 控制项
 *
 * 目的：AV1 的 show_frame=0 帧硬件不输出（送 150 收 80，与 show_frame=1 的
 * 数量精确吻合）。本探针查 msm_vidc 是否提供"输出全部帧（含不显示帧）"
 * 一类的控制项 —— 若有，就不必改写码流去骗硬件。
 *
 * 用 VIDIOC_QUERYCTRL + V4L2_CTRL_FLAG_NEXT_CTRL 遍历（比逐个 id 试可靠：
 * 驱动只报它真正实现的项）。同时单独探几个已知的相关 CID。
 *
 * 编译：cc -O2 -o probe_v4l2_ctrls probe_v4l2_ctrls.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static const char *type_name(unsigned t)
{
    switch (t) {
    case V4L2_CTRL_TYPE_INTEGER:      return "int";
    case V4L2_CTRL_TYPE_BOOLEAN:      return "bool";
    case V4L2_CTRL_TYPE_MENU:         return "menu";
    case V4L2_CTRL_TYPE_BUTTON:       return "button";
    case V4L2_CTRL_TYPE_INTEGER64:    return "int64";
    case V4L2_CTRL_TYPE_CTRL_CLASS:   return "class";
    case V4L2_CTRL_TYPE_STRING:       return "string";
    case V4L2_CTRL_TYPE_BITMASK:      return "bitmask";
    case V4L2_CTRL_TYPE_INTEGER_MENU: return "intmenu";
    default:                          return "other";
    }
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/video32";
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "打开 %s 失败: %s\n", dev, strerror(errno));
        return 1;
    }

    printf("=== %s 的全部控制项 ===\n", dev);
    struct v4l2_queryctrl q;
    unsigned id = 0;
    int n = 0;
    for (;;) {
        memset(&q, 0, sizeof(q));
        q.id = id | V4L2_CTRL_FLAG_NEXT_CTRL;
        if (ioctl(fd, VIDIOC_QUERYCTRL, &q) != 0)
            break;
        id = q.id;
        if (q.type == V4L2_CTRL_TYPE_CTRL_CLASS)
            continue;
        printf("  0x%08x %-8s min=%-6d max=%-8d def=%-8d %s\n",
               q.id, type_name(q.type), q.minimum, q.maximum,
               q.default_value, q.name);
        n++;
    }
    printf("  共 %d 项\n", n);

    /* 单独探几个可能相关的已知 CID —— 上面的遍历只报驱动实现的项，
     * 这里确认"没报"是真的没实现，而不是遍历漏了。 */
    printf("\n=== 定向探测（与输出全部帧相关的 CID）===\n");
    struct { unsigned id; const char *nm; } want[] = {
#ifdef V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY
        { V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY,        "DEC_DISPLAY_DELAY" },
#endif
#ifdef V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE
        { V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE, "DEC_DISPLAY_DELAY_ENABLE" },
#endif
#ifdef V4L2_CID_MPEG_VIDEO_DECODER_CONCEAL_COLOR
        { V4L2_CID_MPEG_VIDEO_DECODER_CONCEAL_COLOR,    "DECODER_CONCEAL_COLOR" },
#endif
#ifdef V4L2_CID_MPEG_VIDEO_AV1_PROFILE
        { V4L2_CID_MPEG_VIDEO_AV1_PROFILE,              "AV1_PROFILE" },
#endif
#ifdef V4L2_CID_MPEG_VIDEO_AV1_LEVEL
        { V4L2_CID_MPEG_VIDEO_AV1_LEVEL,                "AV1_LEVEL" },
#endif
        { 0, NULL }
    };
    for (int i = 0; want[i].nm; i++) {
        memset(&q, 0, sizeof(q));
        q.id = want[i].id;
        int r = ioctl(fd, VIDIOC_QUERYCTRL, &q);
        printf("  %-26s 0x%08x %s\n", want[i].nm, want[i].id,
               r == 0 ? "支持" : (errno == EINVAL ? "不支持" : strerror(errno)));
    }

    close(fd);
    return 0;
}
