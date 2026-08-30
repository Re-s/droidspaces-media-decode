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

    /* ⚠️ 关键：G_CTRL 在 msm_vidc 上**完全不校验 id**。
     * 拿 0xDEADBEEF 去读也返回成功、值为 0 —— 所以"G_CTRL 成功"
     * 不能作为控制项存在的证据。这个陷阱骗过我一次（第 81 轮）：
     * 扫高通私有区间 0x00992000..ff 得到"256 项全可读"，全是假阳性。
     * 唯一可信的判据是 S_CTRL 能否设进去。 */
    printf("\n=== G_CTRL 假阳性对照（这些 id 绝不该存在）===\n");
    unsigned bogus[] = { 0x00992ABCu, 0x0099FFFFu, 0xDEADBEEFu };
    for (unsigned i = 0; i < sizeof bogus / sizeof *bogus; i++) {
        struct v4l2_control c;
        memset(&c, 0, sizeof(c));
        c.id = bogus[i];
        int r = ioctl(fd, VIDIOC_G_CTRL, &c);
        printf("  0x%08x G_CTRL %s%s\n", bogus[i],
               r == 0 ? "成功" : "失败",
               r == 0 ? "  ← 假阳性！G_CTRL 不可信" : "");
    }

    printf("\n=== S_CTRL 真判据（设进去才算支持）===\n");
    struct { unsigned id; int val; const char *nm; } sets[] = {
#ifdef V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE
        { V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE, 1, "DISPLAY_DELAY_ENABLE=1" },
#endif
#ifdef V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY
        { V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY,        0, "DISPLAY_DELAY=0" },
#endif
        { 0, 0, NULL }
    };
    for (int i = 0; sets[i].nm; i++) {
        struct v4l2_control c;
        memset(&c, 0, sizeof(c));
        c.id = sets[i].id;
        c.value = sets[i].val;
        int r = ioctl(fd, VIDIOC_S_CTRL, &c);
        printf("  %-26s %s\n", sets[i].nm,
               r == 0 ? "✓ 设置成功" : strerror(errno));
    }

    close(fd);
    return 0;
}
