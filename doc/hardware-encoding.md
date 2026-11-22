# V4L2 硬件编码（0.5.0-dev）

编码功能参考 Termux:VA 的使用场景，在当前驱动内按公开 VA-API 与 V4L2
stateful encoder 接口独立实现。没有移入 Termux:VA 的 MediaCodec daemon、
Socket 协议或 Mesa bridge，项目继续使用原有 Apache-2.0 许可证。

## 架构与权限

```text
FFmpeg / VA-API 应用
  → libva → msm_drm_drv_video.so
  → V4L2 OUTPUT：NV12 原始图像
  → /dev/video33：硬件编码器
  → V4L2 CAPTURE：H.264 / HEVC / VP9 码流
  → VACodedBufferSegment
```

默认编码节点为 `/dev/video33`，可用 `DMD_VA_ENCODE_DEVICE=/dev/videoN` 覆盖。
解码仍使用现有 `/dev/video32` 后端。运行驱动不要求 root，但平台必须为容器
用户提供编码节点的读写权限；msm DMA-BUF 路径还需要可访问的
`/dev/dma_heap/system`，或旧内核上的 `/dev/ion`。
无需为编码部署 Android 进程或修改 Mesa。

后端支持普通 MMAP、现代 msm 的 DMABUF，以及旧 msm 的 USERPTR
`plane.reserved[0]` fd 约定。新旧 msm 内核的适配代码都经过模拟测试，
本轮真机验证仅覆盖下表中的 SM8750P。

## 声明能力与实际范围

| 项目 | 支持范围 |
|---|---|
| H.264 | Constrained Baseline / Main / High |
| HEVC | Main |
| VP9 | Profile 0；仅在设备枚举该编码器时声明 |
| 输入 | progressive 8bit NV12；宽高为偶数；不含编码器内缩放 |
| 码率控制 | CBR / VBR；H.264 / HEVC Main 的 CQP 通过 VBR 尝试固定 QP，缺少 HEVC QP 控制时回退到 VBR |
| 帧结构 | I/P、单前向参考、单 slice；不支持 B 帧或长期参考 |
| 参数 | 分数帧率、GOP、强制关键帧、profile/level |
| 不支持 | 10bit/HDR、AV1 编码、ICQ、ROI、自定义量化矩阵、packed headers |

`VAEntrypointEncSlice` 依据所选节点的 `ENUM_FMT` 与 profile 控件实际探测。
分辨率查询与打开会话还会检查设备的 `ENUM_FRAMESIZES`，并核对协商结果，
不会把驱动静默改成其他编码格式或尺寸当作成功。
profile 可枚举不代表所有分辨率、码率模式、level 或固件组合均已出流验证。

硬件负责生成 SPS/PPS/VPS 和具体编码工具；应用的初始 QP、HRD/VBV、
VBR 峰值参数作为码率控制建议传入，设备缺少相关控件时保留硬件默认值。
这不提供精确 VBV/峰值或逐字段码流控制。QP 上下界与 GOP 请求会转换为
设备控件；设备无法满足必要约束时明确报错。FFmpeg 可能提示没有 packed/global headers；本轮 MP4
验证中，硬件首包内的参数集可以正常封装与读取，但其他容器仍需实测。

H.264 slice 的 deblocking mode、alpha/beta offset 会传到对应 V4L2 控件，
并校验范围及回读值。MSM 固件在低 QP 时仍可能把非零 offset 改成 `-6`：
本机 GStreamer 默认 `2/2` 请求在 QP 降至 21 时出现这种变化，不能视为等价。
因此 MSM 后端仅在调用者明确设置 `min_qp >= 24` 时接受非零有效 offset；
未设置或低于此值会报不支持，不自动提高 QP，也不修改输出码流。
本机 GStreamer 1.28.2 的 `vah264enc min-qp=24 max-qp=51` 受限配置已完成
60 帧编码与独立软件解码，逐帧检查全部保持 `2/2`。这不代表默认低 QP 配置
或其他非零 offset 组合均已验证。禁用滤波时推导为零的 offset 不受此限制。

编码会话打开后，修改分辨率、码率、帧率或 GOP 需重建 context。
后端错误、超时、空包、时间戳不符或输出溢出会让该会话进入失败状态，
必须销毁并重建；不会返回上一帧的码流冒充成功。
每帧编码同步完成，硬件 IO 在驱动对象表锁外执行。
源图像与输出使用独立快照，其他 context 的对象操作不会被等待固件的线程堵住。

`DMD_VA_LOG=1` 开启诊断日志。
`DMD_VA_ENCODE_TIMEOUT_MS` 设置单帧硬件等待上限，默认 15000 ms。
这条链路包含 CPU 图像复制，不是端到端零拷贝。

## 构建与使用

```sh
cd vaapi-driver
make AV1=1
make check tests
```

仍只需安装 `build/msm_drm_drv_video.so`。不覆盖系统文件的试用方式：

```sh
export LIBVA_DRIVERS_PATH="$PWD/build"
export LIBVA_DRIVER_NAME=msm_drm

ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 \
  -vf format=nv12,hwupload -an \
  -c:v h264_vaapi -bf 0 -g 30 -rc_mode CBR -b:v 6M output.mp4
```

HEVC 将 `h264_vaapi` 换成 `hevc_vaapi`；VBR 使用 `-rc_mode VBR`。
VP9 只有设备确实声明该入口时才能用 `vp9_vaapi`。
视频缩放可放在上传前，例如 `-vf scale=1280:720,format=nv12,hwupload`。

硬解后再硬编，建议明确传递可见图像区域：

```sh
ffmpeg -vaapi_device /dev/dri/renderD128 \
  -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 \
  -vf hwdownload,format=nv12,hwupload -an \
  -c:v hevc_vaapi -bf 0 -g 30 -rc_mode CBR -b:v 6M output.mp4
```

这里的下载/上传是像素内存复制，解码与编码仍由视频硬件执行。
VA surface 池尺寸可能是 1920×1088，而视频可见尺寸为 1920×1080。
驱动会保留解码器回报的 crop；但既有 H.264 参数集重建路径在某些输入上
只保留了对齐尺寸，VA HEVC sequence 本身也不提供 conformance-window 偏移。
**本轮不带上述过滤器的 H.264→HEVC 直接 surface 转码曾输出 1088 行**；
明确下载/上传后已验证输出为 1080 行。不要仅以转码退出码为 0 判断尺寸正确。
非零左/上 crop 的编码输入暂不支持，需应用先整理为从原点开始的 NV12 图像。

## 自动测试与真机探针

`make tests` 包含既有 AV1 单测，以及新增的三组编码测试：

- 前端参数翻译：FFmpeg 默认 CBR/VBR 字段、分数帧率、对齐/crop、错误拒绝。
- V4L2 模拟后端：MMAP/USERPTR、独立 headers、延迟归还输入、PTS、超时、
  溢出、设备错误、尺寸范围与 fd/mapping 清理。
- 完整 VA vtable：PutImage、编码 buffer、MapBuffer/SyncBuffer、失败状态、
  并发销毁源 surface/coded buffer，以及独立 context 不被硬件等待堵住。

真机探针直接调用本驱动的 VA vtable，不依赖 libva 的显示初始化：

```sh
make probe-va-encode
./build/probe_va_encode /tmp/probe 320 240 30 cbr
```

该命令输出 `.h264`、`.hevc`，设备支持时还输出 VP9 `.ivf`。
它只创建测试流，不会修改驱动安装位置或系统配置。

## 本轮真机结果

设备：OPD2413、SM8750P（骁龙 8 Elite）、内核 `6.6.89-Gold_bug`；
Ubuntu 26.04.1 aarch64 容器、FFmpeg 8.0.1、运行时 libva 2.23。
初次测试使用临时目录中的原生 aarch64 glibc 驱动；后续系统安装复测见下节。

| FFmpeg 编码 | 分辨率 | RC | 输出与独立软解 |
|---|---|---|---|
| H.264 Constrained Baseline | 320×240 | CBR | 60/60 帧 |
| HEVC Main | 320×240 | CBR | 60/60 帧 |
| H.264 Main | 640×360 | VBR | 60/60 帧，未带入对齐填充 |
| HEVC Main | 640×360 | VBR | 60/60 帧，未带入对齐填充 |
| H.264 High | 1920×1080 | CBR | 60/60 帧 |
| HEVC Main | 1920×1080 | CBR | 60/60 帧 |

上述六组 MP4 的 `avg_frame_rate` 均为 `30000/1001`；均通过
`ffmpeg -v error -xerror -i output.mp4 -f null -` 独立软解。
H.264 硬解→显式像素传递→HEVC 硬编亦输出 1920×1080、60/60 帧。
后端独立测试还验证了强制关键帧及 I/P 帧型。
这些是有损编码测试，验证码流、profile、尺寸、帧率和帧数，不是像素逐字节一致。

本机编码器枚举 H.264/HEVC，没有 VP9；VP9 只经过参数与模拟后端测试，
**未在真机验证出流**。设备报告范围为 128×128～8192×8192，驱动另受现有
VA surface 上限 8192×4320 约束。Chrome 验证见下节；本轮没有进行 4K、
长时间编码、OBS 或旧内核真机测试，不将这些结果扩展到其他设备。

## tmp 容器安装复测（2026-11-14）

通过 DroidSpaces 的 `--name=tmp run` 在上述设备的 Ubuntu 容器中安装到
`/usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so`。实际 Android 挂载路径为
`/mnt/Droidspaces/tmp`，原驱动备份在容器的 `/var/backups/droidspaces-media-decode/`。

普通用户 `Gold` 在清除 `LIBVA_DRIVERS_PATH` 和 `LIBVA_DRIVER_NAME` 后，
由 libva 自动发现系统驱动，完成以下复测：

- H.264 Main / HEVC Main：640×360、VBR，各 60 帧。
- H.264 High / HEVC Main：1920×1080、CBR，各 60 帧。
- 以上输出均经独立软件解码，确认帧数、尺寸与 `30000/1001` 帧率。
- H.264 640×360 硬解与软解的 NV12 `framemd5` 逐帧一致。
- H.264 硬解→显式下载/上传→HEVC 硬编：1920×1080、60 帧，独立软解通过。

HEVC 硬解存在既有参数集重建限制：本机编码输出的
`num_short_term_ref_pic_sets=1` 码流目前不能由该解码路径处理。
备份的 `0.4.7+5ff77f38` 驱动同样拒绝该流并触发重复释放。
本轮已修复拒绝时遗漏 IO 所有权归还及码流缓冲区重复释放；此类输入会
报告不支持，HEVC 硬解能力没有因此扩展。HEVC 编码输出的独立软解通过。

## Chrome 154 编码实测（2026-11-15）

同一 tmp 容器中的 Google Chrome `154.0.8037.97` 已通过 WebCodecs
`VideoEncoder` 调用本驱动，直接访问 V4L2 硬件编码器；没有部署 Android
编码 daemon。此次使用 NV12 GBM 兼容库完成浏览器输入分配，驱动导入
DRM PRIME surface 后复制 NV12，再提交 V4L2，不是端到端零拷贝。

| WebCodecs H.264 | 输入 | 分辨率 | 编码与独立软件解码 |
|---|---|---|---|
| Baseline（`avc1.420028`） | CPU / Canvas | 640×480 | 各 60/60 帧 |
| Main（`avc1.4d0028`） | Canvas | 640×480 | 60/60 帧 |
| High（`avc1.640028`） | Canvas | 640×480 | 60/60 帧 |
| High（`avc1.640028`） | Canvas | 1920×1080 | 300/300 帧 |

上述输出均另用软件解码核对 profile、可见分辨率和帧数。它们验证的是这台
设备和 Chrome 版本的 H.264 编码路径；HEVC 目前仅验证 VA-API / FFmpeg，
没有在 Chrome 中验证或承诺 HEVC 编码。浏览器报告支持某个 codec 也不能
代替实际出流和解码验证。

Chrome 154 需要以下启动条件：

- 在同一个 `--enable-features` 列表中加入 `AcceleratedVideoEncoder`；
  驱动支持编码本身不会自动打开 Chrome 的 Linux VA-API 编码功能。
- 加入 `--gmb-readback-from-texture-disabled-for-debugging`。这是本次实测所需的
  Chrome 调试开关，版本变化后需重新验证。
- 若平台 GBM 无法为 Chrome 创建可导入的线性 NV12 buffer，使用随包提供的
  NV12 GBM 兼容库。默认目录为 `/usr/local/lib/droidspaces-gbm`；只给 Chrome
  前置该目录的 `LD_LIBRARY_PATH`，保留原有路径，不替换系统 `libgbm`。
- 驱动及设备权限仍按上文配置；Chrome 使用普通用户启动。

配置桌面入口和命令行包装脚本：

```sh
sh tools/configure-chrome-vaapi.sh \
  --gbm-compat-dir /usr/local/lib/droidspaces-gbm
~/.local/bin/chrome-msm-vaapi
```

目录中存在 `libgbm.so.1` 时，包装脚本才启用它；兼容库目录也可通过
`DMD_GBM_COMPAT_DIR` 指定。脚本保留 `.dsh-bak` 回滚备份，重复运行不会叠加
参数；`--uninstall` 恢复备份或删除本脚本新建的包装脚本。先完全退出已有
Chrome 进程再启动，使新参数与环境生效。

脚本默认采用已验证的 ANGLE Vulkan + Skia GL 基准，不自动切换 Chrome
Vulkan 后端。该基准也完成 360p / 1080p H.264 硬解，各 60 帧、零丢帧，
GPU 进程未崩溃。旧 nabu / SD855 配置可用 `DMD_VULKAN=off`，其他设备按实际
显示与硬解验证结果配置。

本轮另测试了 Chrome 154 的 Vulkan 后端：只加 feature `Vulkan` 时，
Skia 为 GaneshVulkan，但 H.264 播放回落 `FFmpegVideoDecoder`；同时开启
`Vulkan,DefaultANGLEVulkan,VulkanFromANGLE,VaapiIgnoreDriverChecks` 后，
实际为 GaneshVulkan / ANGLE Vulkan，360p 和 1080p H.264 均由
`VaapiVideoDecoder` 播放 60 帧、零丢帧，无 GPU 进程崩溃。两种配置下还分别
完成 H.264 High 硬件编码 60 帧。完整 Vulkan 配置还完成 1920×1080 High
Canvas 编码 300 帧，独立软件解码确认 profile、尺寸及全部帧数。
完整命令与 Chrome 151 历史结果的适用限制见
[浏览器指南第 2.5 节](browser-vaapi-guide.md#25-chrome-的-vulkan-后端chrome-154-实测与历史差异)。

浏览器硬解回归与编码出流应分别验证，不能仅凭 `chrome://gpu` 的能力列表或
`not compatible with Vulkan` warning 判断实际后端与硬解结果。
