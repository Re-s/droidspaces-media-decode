/*
 * Probe_v4l2_ctrls -- all V4L2 control entries for emulation/ dev/ video32
 *
 * Purpose: AV1 show_frame=0 hardware not exported (80 with 150 and show_frame=1
 * The number matches precisely. This probe asks whether the msm_vidc provides "out of all frames (including no frames)".
 * One type of control item -- if there is one, you don't have to rewrite the flow of hardware.
 *
 * Use VIDIOC_QUERYCTRL + V4L2_CTRL_FLAG_NEXT_CTRL (ref. id per id)
 * Drives only what it really achieves. Checks several relevant CIDs that are known to be available.
 *
 * Compiled: cc-O2-o probe_v4l2_ctrls probe_v4l2_ctrls.c
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
        fprintf(stderr, "open %s failed: %s\n", dev, strerror(errno));
        return 1;
    }

    printf("=== %s All Control Items ===\n", dev);
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
    printf("  Total %d Item\n", n);

    /* Check out several known CIDs that may be relevant - the above reports only the ones that are driven by the process.
     * It's confirmed that "no report" is really not happening, not missing. */
    printf("\n=== Direct detection (relevant to output of all frames) CID)===\n");
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
               r == 0 ? "Support" : (errno == EINVAL ? "unsupported" : strerror(errno)));
    }

    /* Key: G_CTRL on msm_vidc** not verified at all.
     * Take 0xDEADBEEF to read and return to 0-value-for-nothing --so "G_CTRL Success"
     * It's not a proof of the existence of the control item. This trap has deceived me once (round 81):
     * Quaker private sector 0x0092000.ff gets "256 full readables", all fake positives.
     * The only credible finding is whether S_CTRL can be installed. */
    printf("\n=== G_CTRL Fake positive. id It never should have existed.)===\n");
    unsigned bogus[] = { 0x00992ABCu, 0x0099FFFFu, 0xDEADBEEFu };
    for (unsigned i = 0; i < sizeof bogus / sizeof *bogus; i++) {
        struct v4l2_control c;
        memset(&c, 0, sizeof(c));
        c.id = bogus[i];
        int r = ioctl(fd, VIDIOC_G_CTRL, &c);
        printf("  0x%08x G_CTRL %s%s\n", bogus[i],
               r == 0 ? "succeeded" : "failed",
               r == 0 ? "  ← Fake positive.!G_CTRL Untrusted." : "");
    }

    printf("\n=== S_CTRL The real one.)===\n");
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
               r == 0 ? "✓ Setup successfully" : strerror(errno));
    }

    close(fd);
    return 0;
}
