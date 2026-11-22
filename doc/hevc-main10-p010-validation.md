# HEVC Main10（NV12/P010）支持与实测

验证日期：2026-11-15。设备为 OPD2413 / SM8750P / Adreno 830，DroidSpaces 的
tmp 容器，应用以 Gold 普通用户运行。Main10 已正式安装到本机系统驱动；以下分别记录
独立像素/底层探针与安装后的真实浏览器短片验收，结果不外推到其它硬件。

## 构建与能力边界

```sh
cd vaapi-driver
make HEVC10=1 AV1=0 BUILD=build-hevc10 all check tests
```

`HEVC10=1` 独立开启 `VAProfileHEVCMain10` / `VAEntrypointVLD`；源码默认构建不声明，
这保留跨平台验证边界及下述 Chrome Vulkan 显示限制，不代表本机未启用或未验证。
本机 tmp 系统驱动已开启。
此前像素回归另加 `AV1=1` 检验 AV1；开启 Main10 本身不依赖也不会开启 AV1。使用独立 `BUILD` 目录
避免混用不同编译开关的目标文件，再以 `LIBVA_DRIVERS_PATH` 选择该目录；测试无需
覆盖系统驱动。VP9 Profile2 不随之开启，所有编码入口仍限 8 位 NV12。

Main10 profile 真实接受 8 位 NV12 和 10 位 P010，均为 4:2:0；HEVC Main 仍仅接受
8 位 NV12。首份合法 HEVC picture parameter buffer（PPB）的 luma/chroma
`bit_depth_*_minus8` 必须匹配且为 0 或 2，目标 surface 必须具有相应格式。该 PPB
锁定位深，并在会话重建时保留；后续动态切换位深、混合位深、9/12 位或非 4:2:0 均拒绝。
重建的 Main10 VPS/SPS 保持 profile_idc=2，SPS 样本位深来自 PPB，不从 profile 名猜测。

SPS 短期参考集通过完整 long-format slice 与 VA DPB/POC 改写为显式内联集，
保留其它切片头位和 CABAC 载荷。SPS 长期参考表、自定义缩放表、未知或异常切片
边界等仍明确拒绝。原 RPS=0 转发路径保留。

## 标准 P010 与测试判据

正常输出协商必须请求并实际得到标准 `V4L2_PIX_FMT_P010`。即使 `S_FMT` 返回成功，
若内核把请求替换成 NV12 或其它 fourcc，驱动也在 CAPTURE 分配/排队前失败。
厂商 QP10/Q12A 等布局不能直接按线性 P010 读取；历史 `DMD_PROBE_10BIT` 探测开关
不用于本次成功判据。P010 每样本为小端 16 位、有效 10 位在高位，stride 以字节计。

每个样本分别运行独立软件解码与强制 VA-API 解码。10 位输出经
`hwdownload,format=p010le,format=yuv420p10le` 规范化，8 位输出经 NV12 规范化为
`yuv420p`，再比较有序 `framemd5`。
要求进程成功退出、实际输出 60 帧、每帧像素摘要全部相等、实际加载测试 `.so`、
真实 `hw_completed` 增长且错误为 0。能力枚举成功或只出现“Using VAAPI”不足以通过。

合成源在 10 位域加入低两位扰动，并确认软件重建帧存在低两位非零样本，避免仅将
8 位上移的输入漏检截位。显示尺寸与对齐后的驱动处理尺寸分别核对，不将
1920×1088 的分配/处理高度误写成源显示高度 1080。

## 实机结果

| Main10 VA profile 的输入 | 位置 | 软解/硬解帧数 | 逐帧像素一致 | 驱动错误 |
| --- | --- | --- | --- | --- |
| 640×360 真 10 位合成样片 | 起点 | 60/60 | 60/60 | 0 |
| 1920×1080 真 10 位合成样片 | 起点 | 60/60 | 60/60 | 0 |
| 实际 1920×1080、24000/1001 FPS Main10 文件 | 起点 | 60/60 | 60/60 | 0 |
| 同一实际文件 | 60 秒 | 60/60 | 60/60 | 0 |
| 8 位样片，以 Main10 VA profile 解码至 NV12 | 起点 | 60/60 | 60/60 | 0 |

Main10 的四组 P010 共 240 帧，加上 Main10 profile/NV12 的 60 帧，五组共 300 帧一致，
硬件完成计数均增长。同一测试构建的 H.264、HEVC Main 8 位、AV1 8 位、AV1 10 位
回归各 60 帧也像素一致；最终九组共 540 帧逐帧匹配、硬件错误 0。
该回归构建包含 AV1；正式安装的生产构建为 `HEVC10=1 AV1=0`，不连带启用 AV1。

## tmp 系统驱动安装验收

正式安装位置为 `/usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so`，旧驱动已备份。
本次安装 SHA-256：
`83bbfe3f8c88d1e1ae65b83ebd3ba9a18455e4fbbbf63caff2246c928379d3f2`。
构建为 `HEVC10=1 AV1=0`；`vainfo` 列出 Main10 VLD，原 H.264/HEVC 编码入口保留。

Gold 的安装后浏览器复测未设置 `LIBVA_DRIVER_NAME`、`LIBVA_DRIVERS_PATH` 或
`DMD_V4L2_DISPLAY_DELAY` 覆盖，实际 maps 确认自动加载上述系统 `.so`。能力枚举与
加载路径是安装检查，硬解成功另由下面的完成计数、错误和真实播放验证。

## 三处互操作根因及修复

1. **小 surface 是图像探测，不是编码视频尺寸。** mpv 会先创建真实 16×16 NV12/P010
   surface 检验上传与导出。旧代码把解码 context 的最小尺寸限制套到了独立 surface，
   使直接 `vaapi` 显示初始化失败。surface 现在允许受最大尺寸和 4:2:0 完整样本约束的
   小图像；解码 context 仍保留硬件视频最小尺寸限制。
2. **解码序设置必须早于首次 OUTPUT REQBUFS。** 新 Qualcomm 驱动在 queue_setup
   中根据 `DISPLAY_DELAY_ENABLE=1`、`DISPLAY_DELAY=0` 计算内部 OUTPUT_ORDER，
   STREAMON 只发送已经算好的值。旧位置在 REQBUFS 之后，即使两次 S_CTRL 成功，
   B 帧仍按显示序滞后，可能与应用对指定 VA surface 的同步等待互锁。
3. **Main10 profile 不能只声明 P010。** Chrome 154 的 `GetRequiredAttribs()` 对
   Main10 仍要求 RT8；P010-only 使 profile 在能力枚举阶段被过滤。现在真正实现该
   profile 的 8 位 NV12/10 位 P010 参数、surface 和后端协商，而非仅为通过查询虚报 RT8。

官方源代码对应：

- [mpv 的 16×16 VAAPI 上传/导出格式探测](https://github.com/mpv-player/mpv/blob/v0.40.0/video/out/hwdec/hwdec_vaapi.c#L443-L474)。
- [Qualcomm queue_setup 依赖计算](https://github.com/qualcomm-linux/video-driver/blob/a2883c1e49efae9c7cefe038eb1a7a9864a5e6eb/driver/vidc/src/msm_vidc_vb2.c#L835-L856)、
  [Display Delay 到输出序的推导](https://github.com/qualcomm-linux/video-driver/blob/a2883c1e49efae9c7cefe038eb1a7a9864a5e6eb/driver/platform/common/src/msm_vidc_platform.c#L1544-L1574)、
  [STREAMON 发送属性](https://github.com/qualcomm-linux/video-driver/blob/a2883c1e49efae9c7cefe038eb1a7a9864a5e6eb/driver/vidc/src/msm_vdec.c#L1592-L1614)。
- [Qualcomm 新 UAPI](https://github.com/qualcomm-linux/video-driver/blob/a2883c1e49efae9c7cefe038eb1a7a9864a5e6eb/include/uapi/vidc/media/v4l2_vidc_extensions.h)：
  0x992003 是 LOWLATENCY_MODE，0x992038 是输出 fence 元数据，0x992016 是 OPB MISR
  元数据；它们不能当作旧 ABI 的 OUTPUT_ORDER、LOWLATENCY、SECONDARY 连设。
- [Chrome 154.0.8037.97 的 GetRequiredAttribs](https://github.com/chromium/chromium/blob/154.0.8037.97/media/gpu/vaapi/vaapi_wrapper.cc#L1009-L1038)。

backend 精确核对 QUERYCTRL 名称、类型、可写状态及取值范围后，才对已识别现代 ABI
自动在 REQBUFS 前设置 Display Delay，并跳过撞号的旧私有控制。无法查询的旧 nabu
保留旧路径。`DMD_V4L2_DISPLAY_DELAY=0` 禁用请求，`=1` 强制诊断请求；未设置时采用
上述自动选择。LOWLATENCY 与 TS_REORDER 都不能代替真正的解码序输出。

感谢 AHMETBAYIR 在[上游 issue #6](https://github.com/Re-s/droidspaces-media-decode/issues/6)
定位 SM8750 私有控件 ID 撞号问题并验证标准 Display Delay 控件的组合。
本实现沿用这两个控件的取值，并在首次 OUTPUT REQBUFS 前验证和设置。

同一带 B 帧输入的四组 A/B 中，旧位置 off/on 和新位置 off 都是 16 输入、14 输出，
顺序从 `1,4,3,5,2,…` 开始；新位置 on 为 16/16，严格 `unit1..16`，每次输入立即交付，
且不发送 EOS。生产自动路径未设置环境变量时也为 16/16。探针和完整判据见
[`../vaapi-driver/tools/probe_hevc_lag.md`](../vaapi-driver/tools/probe_hevc_lag.md)。

## P010 导出、mpv 与浏览器

标准 P010 的分离层按 Y=`DRM_FORMAT_R16`、UV=`DRM_FORMAT_GR1616` 导出，
保留 `stride*slice_height` 的 UV 偏移；合并层仍为 `DRM_FORMAT_P010`。
独立 EGL/GLES 探针在真实 Adreno 830v1 / Mesa 26.3.0-devel 的 surfaceless
上下文中，Y/U/V 共 6528 个样本逐值一致，实际 GL 格式为 R16_EXT/RG16_EXT。
构建与判据见 [`../vaapi-driver/tools/probe_p010_egl.md`](../vaapi-driver/tools/probe_p010_egl.md)。
该探针的已知内容 dma-buf 数值检查，与下面实际播放器/浏览器测试分别取证。

安装后 Gold 的 mpv 原 `vaapi` 路径也已复测：无驱动名称/路径/DD 环境覆盖，实际
加载系统 SHA-256 为上述 `83bbfe…` 的 `.so`。1920×1080 合成样片完成 60 个硬件帧；
真实文件从 60 秒处播放约 6 秒，硬件完成采样最高 163。两组均报告 `hwdec=vaapi`，
输入/输出为 `vaapi[p010]`，正常退出 0、无超时、驱动错误 0、解码和显示丢帧均为 0，
并保存实际窗口截图。采样值 163 不当作完整视频帧总数。

`vaapi-copy` 此前也已通过，但不再是绕开直接 VAAPI 同步问题的必需设置。直接 VAAPI
不等于整个解码链路零拷贝：本后端仍包含 CAPTURE 到 VA surface 的 CPU 拷贝。

```sh
# 使用独立驱动测试；现代 ABI 默认自动配置，不需强制 DD 环境变量
LIBVA_DRIVER_NAME=msm_drm LIBVA_DRIVERS_PATH="$PWD/build-hevc10" \
  mpv --hwdec=vaapi main10.mkv
```

安装后的浏览器实测使用下载的真实 Main10 文件，从 60 秒起提取约 10.194 秒，
仅 remux 为 `hvc1`，没有重新编码；视频为 1920×1080、24000/1001 FPS。两款浏览器
保留既有沙箱，核对系统驱动 maps；Chrome 分别比较默认 ANGLE Vulkan 和临时 ANGLE
OpenGL，后者没有修改全局 launcher。`ended`、视频质量计数与实际窗口图像分别检查。

| 应用/显示后端 | 视频总帧 | 显示丢帧 | 硬件完成观测 | 驱动错误 | 实际图像 |
| --- | --- | --- | --- | --- | --- |
| Chrome 154 / 默认 ANGLE Vulkan | 242 | 0 | 完成 276、输出 276 | 0 | 黑屏，P010 导入失败 |
| Chrome 154 / 临时 ANGLE OpenGL | 242 | 0 | 完成 276 | 0 | 正常 |
| Firefox 160 | 243 | 3 | 采样最高完成 275，非完整硬件总数 | 0 | 正常 |

两个 Chrome 测试均到达 `ended`、报告损坏帧 0；这些计数不能证明图像成功导入。
默认 Vulkan 路径日志为 `Unknown dma_buf format 808530000`（`0x30313050`，P010）、
`Vulkan(-8)`、`EGL_BAD_PARAMETER`。Chrome 154 对应的 ANGLE 提交
`e12217f3e133cb1029b050d893b1806d141483be` 的
[`DrmFourCCFormatToVkFormats()`](https://chromium.googlesource.com/angle/angle/+/e12217f3e133cb1029b050d893b1806d141483be/src/common/linux/dma_buf_utils.cpp)
缺少 P010 映射，返回空格式列表，随后在
[`DmaBufImageSiblingVkLinux::initImpl()`](https://chromium.googlesource.com/angle/angle/+/e12217f3e133cb1029b050d893b1806d141483be/src/libANGLE/renderer/vulkan/linux/DmaBufImageSiblingVkLinux.cpp)
导入失败。这是独立的 ANGLE 格式支持阻挡，当前证据不指向 pitch/modifier 错误，
也不能通过驱动硬解错误为 0 排除。另一个独立 Vulkan 格式枚举证据显示，当前 Turnip
将 `VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` 列为不支持，
而 R16/RG16 支持。因此仅给 ANGLE 增加 P010 映射也不能保证整个 Vulkan 显示链路
可用，可能还需按 R16/RG16 分离 plane 导入等工作；这超出正常 VA P010 导出的职责。
ANGLE Vulkan 的 P010 显示仍需后续修复，不能笼统归咎于固件。

同一系统驱动、同一视频，仅临时追加下面两个参数的 OpenGL 对照到达 `ended`，
窗口与 Firefox/mpv 的同场景画面正常，且没有 P010 Vulkan 导入报错。选择后端前需
正常退出所有 Chrome 进程，再通过现有入口重启；已有实例可能复用原后端而忽略新参数：

```sh
# 通过现有 Chrome 启动入口，临时追加显示后端参数；不改全局 launcher
/usr/local/bin/google-chrome --use-gl=angle --use-angle=gl
```

该 remux 片段有负时间戳预滚，硬件会处理呈现起点之前的帧，因此硬件帧数多于视频
总帧数；不能要求两者机械相等，也不能把 Firefox 的采样最高值 275 当作最终总数。
Firefox 使用项目 RDD 兼容路径，保留沙箱并加入限定 KGSL 规则，不等同于未修改的
Mozilla 默认沙箱策略。以上是约 10 秒片段，不能称作长片或全片验证。

此前独立 D/B 构建的 60 帧样片有历史完成计数记录；Chrome Vulkan 的计数不能作为
图像正确证据，本节以上述安装后的窗口检查区分解码和显示结果。

## 尚未覆盖

- 其它 SoC、内核或固件，以及长片全程、4K、流内位深/分辨率变化和并发压力。
- VLC、OBS 的 Main10 实际消费链路，以及精确窗口 RGB/色彩管理；浏览器长片播放尚未验证。
- HDR 元数据、色调映射、显示设备的 10 位输出；Main10 像素正确不等于 HDR 显示正确。
- 10 位硬件编码和 VP9 Profile2。

v0.4.2 曾在早期 10 位探测中持续收到 `INSUFFICIENT`，重配后出现 `SYS_ERROR`。
当时流程缺少后续已修复的 CAPTURE flush 等步骤；历史现象保留在 CHANGELOG，
但它不能证明 Main10 必然受固件限制，也不能推翻本节的受限验证结果。
