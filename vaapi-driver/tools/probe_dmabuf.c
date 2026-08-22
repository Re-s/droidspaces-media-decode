/* 能否用 msm_drm 自己分配并导出 dmabuf？
 *
 * 这决定 vaExportSurfaceHandle 能不能实现 —— 而它是 Firefox 硬解的硬性要求
 * （CreateImageVAAPI 必须拿到 DRM_PRIME_2 描述符，没有回退到拷贝的路径）。
 *
 * 之前的结论是"零拷贝这条路是死的"，依据是 ION 不可用、没有 /dev/dma_heap、
 * NDK MediaCodec 无法导出输出缓冲的 fd。但那三条都是在说
 * **"拿不到 MediaCodec 自己那块内存的 fd"**，而这里要问的是另一件事：
 * 我们能不能自己分配一块可导出的 dmabuf，然后把解码结果拷进去？
 * 那样依然是"CPU 拷一次"，但对 Firefox 就够了 —— 它要的只是一个
 * 能被 GL/合成器导入的 dmabuf fd，并不要求零拷贝。
 *
 * 用 GEM 通用接口试，不引 libdrm 的 msm 私有头。
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
    if (fd < 0) { printf("打不开 %s: %s\n", dev, strerror(errno)); return 1; }

    /* 1) 先看驱动名 */
    struct drm_version ver;
    char name[64] = {0};
    memset(&ver, 0, sizeof(ver));
    ver.name = name;
    ver.name_len = sizeof(name) - 1;
    if (ioctl(fd, DRM_IOCTL_VERSION, &ver) == 0)
        printf("驱动: %s %d.%d.%d\n", name, ver.version_major,
               ver.version_minor, ver.version_patchlevel);

    /* 2) DUMB buffer：GEM 的通用分配接口，很多驱动都支持。
     *    1920x1088 NV12 需要 stride*height*3/2。用 8bpp 单平面凑够大小。 */
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    creq.width  = 1920;
    creq.height = 1088 * 3 / 2;   /* NV12 总行数 */
    creq.bpp    = 8;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) {
        printf("DRM_IOCTL_MODE_CREATE_DUMB 失败: %s\n", strerror(errno));
        printf("→ dumb buffer 不可用（render node 通常不支持 dumb，属正常）\n");
        close(fd);
        return 2;
    }
    printf("dumb buffer 分配成功: handle=%u stride=%u size=%llu\n",
           creq.handle, creq.pitch, (unsigned long long)creq.size);

    /* 3) 关键一步：能否导出成 dmabuf fd */
    struct drm_prime_handle prime;
    memset(&prime, 0, sizeof(prime));
    prime.handle = creq.handle;
    prime.flags  = DRM_CLOEXEC | DRM_RDWR;
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
        printf("PRIME_HANDLE_TO_FD 失败: %s\n", strerror(errno));
        close(fd);
        return 3;
    }
    printf("✓ 导出 dmabuf fd = %d\n", prime.fd);
    printf("→ vaExportSurfaceHandle 可以实现：自己分配可导出 buffer，\n");
    printf("  把 daemon 回传的帧拷进去，再交出 fd\n");
    close(prime.fd);
    close(fd);
    return 0;
}
