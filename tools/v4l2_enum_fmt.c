/*
 * v4l2_enum_fmt - direct listing of codec formats supported by V4L2 video nodes.
 *
 * Use: Disclosing all abstract layers of MediaCodec/ Codec2/ vendor HAL, asking for inner core drivers
 * "What format does this chip support?" This is the final decision to determine whether a codec is supported by hardware --
 * MediaCodec 's list of codec is derived from the XML configuration and may state the format that hardware is not actually supported.
 *
 * Background: SM8750 on c2.qti.av1.decoder failed during component creation.
 * (Queryrequired Infos from downstream failed) and dshmon idw=false.
 * Requires confirmation that the vidc hardware has the AV1 decoding capability.
 *
 * Usage (root needed, video node system:camera):
 *     v42_enum_fmt /dev/video32# Decoding node
 *     v42_enum_fmt /dev/video33# Encoded Node
 *
 * Cross-compiling (NDK):
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
    if (!found) printf("  (None)\n");
}

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/video32";
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", dev, strerror(errno));
        return 1;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        printf("Equipment: %s\n", dev);
        printf("  driver=%s card=%s bus=%s\n", cap.driver, cap.card, cap.bus_info);
        printf("  capabilities=0x%08x\n", cap.capabilities);
    }

    /* Output queue (OUTPUT) = incoming bitstream; capture queue (CAPTURE) = original frame taken.
     * The decoder is in a compressed format on the OUTPUT side, so the "support to what" is to be judged by the OUTPUT. */
    enum_one(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "OUTPUT_MPLANE(The flow format for feeding)");
    enum_one(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAPTURE_MPLANE(Extracted pixel format)");
    enum_one(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, "OUTPUT(Mono-Plate)");
    enum_one(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, "CAPTURE(Mono-Plate)");

    close(fd);
    return 0;
}
