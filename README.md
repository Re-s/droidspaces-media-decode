# DroidSpaces Media Decode

> 🌐 **English version: [README.en.md](README.en.md)**

给 Linux 容器提供视频硬件解码：一个 VA-API 驱动，直连高通 msm_vidc 的 V4L2 接口。

## 项目简介

容器内的应用（ffmpeg、Firefox、Chrome）通过标准 VA-API 用上硬件解码，**无需任何改动**。
驱动被 libva `dlopen` 进消费者进程，直接操作 `/dev/video32`。

```
容器 ffmpeg / Firefox
   ↓  标准 VA-API 调用
libva → dlopen → msm_drm_drv_video.so     ← 本项目
   ↓  V4L2 stateful decoder ioctl
/dev/video32  (高通 msm_vidc / Venus)
```

**Android 侧没有本项目的任何进程或文件。**

> ⚠️ **0.4.0 是架构性重写。** 0.3.x 走的是
> `容器 → unix socket → Android 侧 decode-daemon → MediaCodec`，
> 需要刷 KSU 模块并让 daemon 常驻。那条链路已整条删除：
> 不再有 daemon、看护、socket、SHM 与 MediaCodec 调用。
> 升级只需换掉容器里的 `.so`，并可以卸载 `dmd_watchdog` 模块。
>
> 历史架构的记录见 [`CHANGELOG.md`](CHANGELOG.md) 与
> [`doc/release-archive.md`](doc/release-archive.md)。

## 支持的编解码器

| 编解码器 | 状态 | 实机验证 |
|---|---|---|
| H.264 | ✅ 可用 | 300/300 帧，md5 与软解逐字节一致 |
| HEVC Main | ✅ 可用 | 12/12 帧，md5 与软解逐字节一致 |
| VP9 Profile 0 | ✅ 可用 | 50/50 帧，md5 与软解逐字节一致 |
| AV1 Profile 0 | 🚧 未完成 | 帧数与 dav1d 一致，**像素未通过**，默认不声明 |
| VP8 | ❌ 不支持 | msm_vidc 的 V4L2 层没有 VP80 格式，无硬件路径 |

AV1 需要 `-DDMD_ENABLE_AV1` 编译才会声明；遗留缺陷见
[`doc/av1-v4l2-status.md`](doc/av1-v4l2-status.md)。

> HEVC 有一类码流无法支持：SPS 带 `st_ref_pic_set` 的
> （`num_short_term_ref_pic_sets > 0`）。VA-API 只给个数不给内容，无法复现，
> 此时 `vaEndPicture` 返回 `UNIMPLEMENTED` 让上层回落软解。
> 实测 x265 默认输出 0，常见码流不受影响。

## 编译与安装

必须在 **aarch64** 上编译（x86_64 交叉工具链缺 aarch64 glibc）：

```sh
# 依赖：build-essential pkg-config libva-dev libdrm-dev
cd vaapi-driver
make            # 产物 build/msm_drm_drv_video.so
make check      # 确认导出了 __vaDriverInit_* 入口
make tests      # 单元测试
```

安装到容器（**这是全部步骤**）：

```sh
install -m 0644 build/msm_drm_drv_video.so \
  /usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so
```

> 产物名不可更改：libva 从 msm render node 推导出的驱动名就是 `msm_drm`，
> 只会尝试打开 `<dridir>/msm_drm_drv_video.so` 这一个文件，无 fallback。

前提条件：

- 容器内有 `libva2` 与 `libva-drm2`
- 容器用户在 `droidspaces-gpu` 组内（平台侧通常已代为加组）——
  `/dev/video32` 属 `root:droidspaces-gpu`、模式 `crw-rw----`
- 平台已透传 `/dev/dri/renderD128`

自检：

```sh
LIBVA_DRIVER_NAME=msm_drm ffmpeg -hwaccel vaapi \
  -hwaccel_output_format vaapi -i in.mp4 -f null -
```

`vainfo` 也能用（0.3.x 时代它会挂在 daemon socket 上，0.4.0 起没有 socket 了）：

```
vainfo: Driver version: DroidSpaces V4L2 VA-API driver 0.4.1
      VAProfileH264High               : VAEntrypointVLD
      VAProfileHEVCMain               : VAEntrypointVLD
      VAProfileVP9Profile0            : VAEntrypointVLD
```

但它只证明驱动能加载、profile 列表是**静态声明**的，不证明能出帧 ——
验真实解码用上面的 ffmpeg 命令。

不想装进系统目录可以用 `LIBVA_DRIVERS_PATH` 指向 `.so` 所在目录：

```sh
LIBVA_DRIVERS_PATH=/path/to/vaapi-driver/build vainfo
```

调试日志：`DMD_VA_LOG=1`。

## 容器内浏览器调用 VA-API

先确认后端能出帧（上一节的 ffmpeg 自检），**这一步不通就别碰浏览器**。

一键体检：`bash tools/check-browser-vaapi.sh`
更多背景与排障见 [`doc/browser-vaapi-guide.md`](doc/browser-vaapi-guide.md)。

### Chrome / Chromium

两个参数缺一不可，原因是平台限制而非偏好：

```sh
google-chrome \
  --ozone-platform=wayland \
  --disable-vulkan \
  --render-node-override=/dev/dri/renderD128 \
  --ignore-gpu-blocklist \
  --enable-features="VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL"
```

| 参数 | 为什么不能省 |
|---|---|
| `--ozone-platform=wayland` | 解码帧经 linux-dmabuf 提交。X11 下解码器能创建，之后零流量空转 —— 看着像在硬解，实际一帧不走 |
| `--disable-vulkan` | ozone wayland 与 Vulkan 不兼容（Chrome 硬性检查），必须显式关 |
| `--render-node-override` | Chromium `vaapi_wrapper.cc` 只枚举 PCI 总线 DRM 设备，ARM 平台设备被 `if (device->bustype != DRM_BUS_PCI) continue;` 跳过。此开关走 `LoadDrmFD()` 绕过白名单 |
| `--ignore-gpu-blocklist` | ARM GPU 在 Chrome 的软件渲染黑名单里 |
| `--enable-features=...` | Linux VA-API 解码总开关（DMABUF / GL 两路都开） |

容器里通常还需要 `MESA_LOADER_DRIVER_OVERRIDE=msm` 让 GL 栈认出 Adreno。

固化到桌面图标（参数紧跟在可执行文件后）：

```sh
sudo sed -i 's|^Exec=/usr/bin/google-chrome-stable|Exec=env DMD_VA_LOG=1 MESA_LOADER_DRIVER_OVERRIDE=msm /usr/bin/google-chrome-stable --ozone-platform=wayland --disable-vulkan --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist --enable-features=VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL|' \
  /usr/share/applications/google-chrome.desktop
```

`DMD_VA_LOG=1` 可选，加上才能在 stderr 看到驱动日志。指南里有幂等版脚本
（重复执行不叠加参数），批量改多个 `Exec=` 行建议用那个。

### Firefox ESR

配置写进 profile 的 `user.js`。profile 路径按 `installs.ini` 里的 `Default=` 找，
别猜目录名：

```sh
P=~/.mozilla/firefox/$(grep -m1 '^Default=' ~/.mozilla/firefox/installs.ini | cut -d= -f2)
cat >> "$P/user.js" << 'EOF'
user_pref("media.ffmpeg.vaapi.enabled", true);
user_pref("media.hardware-video-decoding.force-enabled", true);
user_pref("media.rdd-ffmpeg.enabled", true);
user_pref("media.gpu-process-decoding", true);
user_pref("media.vaapi-dmabuf-textures.enabled", true);
user_pref("media.hardware-video-decoding.enabled", true);
# 播放队列深度 —— 不是可选项，见下
user_pref("media.video-queue.hw-accel-size", 10);
user_pref("media.video-queue.default-size", 10);
user_pref("media.video-queue.send-to-compositor-size", 6);
EOF
```

第五项 `media.vaapi-dmabuf-textures.enabled` 容易被当成可选项漏掉：
缺了它硬解照跑，但每帧都要拷回内存再做 CPU 软件转色，
内容进程能吃掉半个到多个核心 —— 表现为"开了硬解 CPU 反而更高"。

`media.hardware-video-decoding.enabled` 要显式写 `true`。它默认就是 true，
但 profile 里一旦残留过 `false`（比如做过软解对照测试），`user.js` 缺这一项
就不会覆盖回来，于是**静默走软解**。这种情况下页面统计的丢帧率会很好看，
容易误判成成功 —— 判断硬解是否真在跑要看 `DMD_VA_LOG=1` 有没有 `[v4l2]`
输出，或 gdsc 是否 `enabled`，不能只看播放是否流畅。

**三项 `media.video-queue.*` 是浏览器流畅播放的关键，不加就丢帧。**
Venus 固件的解码流水线滞后 4 个输入单元才吐首帧（无 B 帧码流同样如此，
`research/slowfeed.c` 实测），而 Firefox 默认的播放队列很浅，
吸收不了这个滞后带来的交付抖动。1080p30 真实 27Mbps 码流实测（各 3-7 轮取中位）：

| 配置 | 丢帧率 | 顺序回退 |
|------|--------|----------|
| 不加 `video-queue.*` | 14.25% | 20.75% |
| 加上 | **0.89%** | 4.04% |

软解基线是 0.5% / 1.85%，也就是说加上之后硬解与软解同量级。

配齐之后的 Firefox 实测（各 3 轮取中位，`research/perf/bench.sh`）：

| 素材 | 丢帧率 | 呈现帧率 | 顺序回退 |
|------|--------|----------|----------|
| H.264 1080p30 27Mbps | 1.34% | 29.38 fps（标称 30） | 4.54% |
| HEVC 1080p30 | 0.67% | 29.45 fps | 3.48% |

### 4K 不要开硬解

同样的配置在 4K30 上是**倒退**：丢帧中位 72.38%、呈现只有 8.24 fps。
原因是硬件能力不够，不是配置问题 —— ffmpeg 直接量吞吐：

| 4K30 25Mbps | 吞吐 |
|-------------|------|
| VA-API 硬解 | 21.8 fps |
| CPU 软解 | 48.7 fps |

硬解 21.8 fps 达不到 30fps 的实时需求，而软解反而快一倍多。
所以 4K 应当让浏览器走软解，硬解的价值在 1080p 及以下（省电、省 CPU）。
顺序回退在 4K 下反而很低（0.76%），那是因为帧根本没跟上、无从错位，
不是"4K 顺序更好"。

还需要关掉 RDD 沙箱，否则解码进程打不开 `/dev/video32`：

```sh
sudo sed -i 's|^Exec=/usr/lib/firefox-esr/firefox-esr |Exec=env MOZ_DISABLE_RDD_SANDBOX=1 /usr/lib/firefox-esr/firefox-esr |' \
  /usr/share/applications/firefox-esr.desktop
```

容器里没有 sudo 就从宿主侧改
`/mnt/Droidspaces/<容器名>/usr/share/applications/firefox-esr.desktop`。
手动启动同理：`env MOZ_DISABLE_RDD_SANDBOX=1 firefox <地址>`。

### 验证

```sh
# ① GPU 进程加载了驱动栈（两个数字都应 > 0）
for p in $(pgrep -f "type=gpu-process"); do
  grep -c libva /proc/$p/maps; grep -c drv_video /proc/$p/maps; done

# ② 页面侧帧计数在增长（DevTools console，播放中执行两次比较）
#    document.querySelector('video').getVideoPlaybackQuality().totalVideoFrames
```

驱动日志（需 `DMD_VA_LOG=1`）出现这两行才是固件真的在解码：

```
[v4l2] PORT_SETTINGS(SUFFICIENT): h=1088 w=1920
[v4l2] 已发 SESSION_CONTINUE
```

> ⚠️ 别拿 `[v4l2] 收到 SOURCE_CHANGE` 当判据 —— **那行永远不会出现。**
> msm_vidc 从不发标准 `V4L2_EVENT_SOURCE_CHANGE`，它发的是私有事件
> `0x08001002`。0.3.x/0.4.0 的文档写错了这一点，会导致误判成"没在硬解"。

### 选哪个浏览器

**HEVC 观看推荐 Firefox。** Chrome 在 anland 显示桥上有呈现反馈缺失的平台级
问题（不是本驱动的缺陷）。H.264 两者都正常。

### 已知限制

- **video32 解码会话一次只能有一个。** Android 侧 HAL 占用时驱动上报的
  `MIN_BUFFERS` 会从 6/14 降到 4/12，两边会争这个节点 ——
  容器里放视频的同时 Android 侧也在播，会有一方起不来。
- **4K 需约 287MB ION 内存**（24 个 CAPTURE 缓冲）。测试机 `MemAvailable`
  2.2GB 时正常，内存紧张下的行为未测。
- AV1 帧数与 dav1d 一致但像素未通过，浏览器里遇到 AV1 会回落软解。

## 能效口径：硬解不省电，价值在并发

**"加速"只指吞吐，不代表省电或省 CPU。** 0.3.x 时代的系统级实测
（宿主 `/proc/stat`，High profile 27.2 Mbps，满速 300 帧）：

| 口径 | 硬解 | 软解 | 差异 |
|------|------|------|------|
| 系统 CPU 时间 | 57.70 ms/帧 | 57.40 ms/帧 | +0.5%，在噪声内 |
| 墙钟 | 3557 ms | 2366 ms | 硬解**慢 50%** |

整机功耗（屏幕开，放电）：空闲 1208 mA / 4.59 W，软解 1524 mA / 5.74 W，
硬解 **1630 mA / 6.12 W**。

原因：调速器不因硬解降频；负载摊到 CPU + Venus。

> ⚠️ 这组数字测的是 **0.3.x 的 daemon 架构**，其中"daemon 那约 4.4 ms/帧落在宿主账上"
> 一项在 0.4.0 已不存在（没有 daemon 进程了）。
> **V4L2 直通的能效尚未重新测量。** 定性结论（单路硬解不省电、价值在并发与
> 释放 CPU 给其它工作）预计仍成立，但具体数字应视为待复测。

## 性能

> ⚠️ **0.3.x 的性能数字已全部作废。** 那些是 daemon + socket + MediaCodec 链路
> 的端到端测量（1080p 峰值 194 fps 等），与当前架构无关。
> **V4L2 直通的吞吐与延迟尚未测量**，也没有与旧架构的 A/B 对照。
>
> 历史数据见 [`doc/performance-and-roadmap.md`](doc/performance-and-roadmap.md)，
> 阅读时请注意它描述的是已删除的架构。

## 文档

| 文档 | 内容 |
|---|---|
| [`vaapi-driver/README.md`](vaapi-driver/README.md) | 驱动内部实现、V4L2 协商细节 |
| [`doc/av1-v4l2-status.md`](doc/av1-v4l2-status.md) | AV1 遗留缺陷的完整测绘与方法教训 |
| [`doc/platform-integration-contract.md`](doc/platform-integration-contract.md) | 平台侧需要提供什么（0.4.0 只剩设备节点权限一项） |
| [`doc/browser-vaapi-guide.md`](doc/browser-vaapi-guide.md) | 浏览器接入 |
| [`doc/verified-platform-facts.md`](doc/verified-platform-facts.md) | 平台取证事实 |
| [`doc/why-not-v4l2.md`](doc/why-not-v4l2.md) | ⚠️ 结论已被推翻的历史文档，保留其取证过程与方法教训 |
| [`CHANGELOG.md`](CHANGELOG.md) | 版本历史 |

## 测试设备

小米平板 5（`nabu`），骁龙 855（SM8150 / Adreno 640），Android 13，内核 `4.14.336`；
容器为 DroidSpaces 内 Debian 13 aarch64。

AV1 需要更新的硬件，在另一台设备上验证。

## 许可

见 [LICENSE](LICENSE)。
