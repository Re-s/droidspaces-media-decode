/* Can you use msm_drm to distribute and export dmabuf?
 *
 * This determines whether the vExportSurfaceHandle can be achieved -- and it's a hard-wipeed requirement for Firefox.
 * (CreateImageVAAPI must get DRM_PRIME_2 descriptors, no return to the path of the copy).
 *
 * The previous conclusion was that "The Zero Copy Road is Dead" based on the basis that Ion is not available, no /dev/dma_heap,
 * NDK MediaCodec cannot export the fd that produces the buffer. But all three are saying
 * ** "No MediaCodec can get its own memory of the fd,"** and here's another thing:
 * Can we just divide a piece of exported dmabuf and then make a copy of the decoding?
 * That's still "crushing the CPU," but Firefox is enough -- it's just one thing.
 * dmabuf fd that can be imported by GL/Synthesizer does not require a zero copy.
 *
 * Try with GEM Universal Interface, without quoting the msm private head of libdram.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <drm/drm.h>

int main(void)
{
    const char *dev = "/dev/dri/renderD128";
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) { printf("It won’t open. %s: %s\n", dev, strerror(errno)); return 1; }

    /* 1) The driver name first */
    struct drm_version ver;
    char name[64] = {0};
    memset(&ver, 0, sizeof(ver));
    ver.name = name;
    ver.name_len = sizeof(name) - 1;
    if (ioctl(fd, DRM_IOCTL_VERSION, &ver) == 0)
        printf("Driver: %s %d.%d.%d\n", name, ver.version_major,
               ver.version_minor, ver.version_patchlevel);

    /* 2) DUMB buffer: GEM 's Universal Allocation Interface, supported by many drivers.
     *    1920x1088 NV12 requires stride*height*3/2. */
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    creq.width  = 1920;
    creq.height = 1088 * 3 / 2;   /* NV12 Total rows */
    creq.bpp    = 8;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) {
        printf("DRM_IOCTL_MODE_CREATE_DUMB failed: %s\n", strerror(errno));
        printf("→ dumb buffer unavailable(render node Not usually supported dumb,Normal)\n");
        close(fd);
        return 2;
    }
    printf("dumb buffer Distribution Success: handle=%u stride=%u size=%llu\n",
           creq.handle, creq.pitch, (unsigned long long)creq.size);

    /* 3) Key step: Is it possible to export to dmabuf fd */
    struct drm_prime_handle prime;
    memset(&prime, 0, sizeof(prime));
    prime.handle = creq.handle;
    prime.flags  = DRM_CLOEXEC | DRM_RDWR;
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
        printf("PRIME_HANDLE_TO_FD failed: %s\n", strerror(errno));
        close(fd);
        return 3;
    }
    printf("✓ Export dmabuf fd = %d\n", prime.fd);
    printf("→ vaExportSurfaceHandle Achieved: Exportable by assigning yourself buffer,\n");
    printf("  Take it. daemon The back frame is in. Give it to me. fd\n");
    close(prime.fd);
    close(fd);
    return 0;
}
