# 独立 P010 分层 EGL/GLES 数值探针

`tools/probe_p010_egl.c` 只验证一块已知内容的标准线性 P010 dma-buf，在 EGL 导入为 R16 与 GR1616 两层后，GLES 是否能完整采样 Y/U/V 的高 10 位。它不调用 VAAPI、不读取视频、不测试编解码器，不声明 Firefox/Chrome 已支持该路径，也不修改任何驱动。

## 构建

在 `vaapi-driver/` 中，有开发头与 pkg-config 的主机/容器：

```sh
make probe-p010-egl
./build/probe_p010_egl --selftest

# 或独立编译
cc -O2 -g -std=c11 -Wall -Wextra -Werror tools/probe_p010_egl.c \
  $(pkg-config --cflags --libs egl glesv2 libdrm) -lm -o probe_p010_egl
./probe_p010_egl --selftest
```

程序不调用 libdrm 函数；DRM 仅需要头文件。若 tmp 只有运行库，可把 `EGL/`、`GLES2/`、`GLES3/`、`KHR/` 和 `libdrm/` 头目录复制到独立 `headers/`，然后：

```sh
cc -O2 -g -std=c11 -Wall -Wextra -Werror tools/probe_p010_egl.c \
  -Iheaders -Iheaders/libdrm \
  -l:libEGL.so.1 -l:libGLESv2.so.2 -lm -o probe_p010_egl
```

依赖实际 Linux DRM dumb/PRIME、DMA-BUF CPU 同步、EGL dma-buf import、GLES3 与 `GL_EXT_texture_norm16`。缺失能力会打印阶段与 EGL/GL/errno 后返回非零，不能当作采样测试成功。

## 设备运行

以 Gold 在其容器内执行，保留权限与桌面环境，不需要关闭任何沙箱或安装服务：

```sh
MESA_LOADER_DRIVER_OVERRIDE=msm ./probe_p010_egl --platform surfaceless
MESA_LOADER_DRIVER_OVERRIDE=msm EGL_PLATFORM=x11 \
  ./probe_p010_egl --platform default
MESA_LOADER_DRIVER_OVERRIDE=msm EGL_PLATFORM=wayland \
  ./probe_p010_egl --platform default
```

X11 需要 Gold 的 `DISPLAY`/认证；Wayland 需要其 `XDG_RUNTIME_DIR`/`WAYLAND_DISPLAY`。`--platform auto` 为默认：先尝试 Mesa surfaceless display，失败再尝试默认 display。优先使用 pbuffer；若桌面 EGL 没有 pbuffer config，但支持 `EGL_KHR_surfaceless_context`，则在该 display 用无窗口 context 与独立 FBO。

可选 `--device /dev/dri/renderD128 --width 644 --height 360`，尺寸必须是 2..4096 的偶数。默认 64×34、slice 64，刻意使 UV offset 与 `pitch*visible_height` 不同，避免只测恰好对齐的小样本。

## 成功判据与输出

程序自行创建并导出 msm DRM dumb buffer，按返回的实际字节 pitch 填充 P010；CPU 写前后严格成对使用 DMA_BUF_IOCTL_SYNC，不吞失败。Y/U/V 分别含 0、1、2、3、63、64、65、511、512、940、1023 等不同 10bit 码值，写入小端 `value<<6`，填充区使用独立哨兵。

同一个 dma-buf 以两个 EGLImage 导入：Y 层 R16，UV 层 GR1616；pitch 相同，UV 宽高减半，UV offset 为 `pitch*slice_height`。使用普通 `GL_TEXTURE_2D`/高精度 sampler2D 的 texelFetch，不做 YUV 自动转换。ES3.1+ 同时查询实际 GL internal format；其它情况记录请求的 R16_EXT/RG16_EXT，并仍要求下面的数值验证通过。

片元着色器把归一化采样乘 65535 恢复完整 16 位 word，拆成低/高两字节写入 RGBA8 FBO；关闭 dithering/blending 后 glReadPixels，逐位置核对 Y、U、V。默认共核对 6528 个样本，必须三个 channel 均 `mismatches=0`，末行 `RESULT PASS`，进程退出码 0。日志同时记录 DRM/EGL/GL vendor、renderer、版本、扩展、导入布局与实际 GL 格式。

标准 P010 的有效位在 16bit 容器的高 10 位，因此 R16_UNORM 采样白码 1023 得到 `65472/65535 ≈ 0.999038682`，不会天然暗 64 倍。只有把 1023 错放在低位时才会得到约 0.01561。本探针要求 raw word 精确相同，可以检出这种位对齐错误、R8 降精度和 UV 交换；后续播放器仍需正确处理有限范围/矩阵/传递函数，本探针不覆盖颜色管理。

格式依据：[Linux P010 定义](https://docs.kernel.org/userspace-api/media/v4l/pixfmt-yuv-planar.html#p010-and-tiled-p010)、[Khronos EXT_texture_norm16](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_norm16.txt)、[Khronos EGL_EXT_image_dma_buf_import](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt)。

## 已执行验证

- `-Wall -Wextra -Werror` 完整编译通过。
- `--selftest`（仅 CPU pattern/字节序/归一化恢复）通过 6528 样本。
- 2026-11-15 的 tmp 容器中，以 Gold 在 Adreno 830v1 / Mesa 26.3.0-devel（git-98f3d6229d）执行最终探针的 surfaceless display + pbuffer 路径：Y/U/V 各 2176 样本、合计 6528 样本，全部 mismatch=0，退出码 0；实际 GL 格式分别为 0x822a / 0x822c，即 R16_EXT / RG16_EXT。UV offset=8192，与 pitch128×slice64 一致。最终源码在同设备重新构建后，再次通过相同的数值验证。
- 桌面 display 的无 pbuffer fallback 未在该次运行中覆盖。以上仅证明已知标准 P010 buffer 的分层导入和采样；不等于真实 HEVC/AV1 码流、VA ExportSurfaceHandle 或浏览器集成验收。

`P010_EGL_CFLAGS`、`P010_EGL_LIBS` 仅供 `probe-p010-egl` 使用；`all` / `tests` 不因此增加 EGL/GLES 依赖。运行库与自带头构建例：

```sh
make probe-p010-egl P010_EGL_CFLAGS=-Iheaders \
  DRM_CFLAGS=-Iheaders/libdrm \
  P010_EGL_LIBS="-l:libEGL.so.1 -l:libGLESv2.so.2"
```
