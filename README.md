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

> ⚠️ **不要用 `vainfo` 测试** —— 它在本平台会挂住，即使指定不存在的驱动名也一样，
> 因此它挂住不代表驱动有问题。用上面的 ffmpeg 命令。

调试日志：`DMD_VA_LOG=1`。

## 浏览器接入硬解

完整方法、启动参数、profile 配置与验证步骤见
[`doc/browser-vaapi-guide.md`](doc/browser-vaapi-guide.md)。要点：

- **Chrome 必须 Wayland 模式**：解码帧经 linux-dmabuf 提交，X11 下解码器创建后零流量空转
- **Chrome 必带 `--render-node-override=/dev/dri/renderD128`**：
  Chromium 只枚举 PCI 总线 DRM 设备，ARM 平台设备会被跳过
- **Firefox 需 `MOZ_DISABLE_RDD_SANDBOX=1`** + user.js 开 VA-API 四件套；
  按 `installs.ini` 的 Default 找真实 profile
- HEVC 观看推荐 Firefox（Chrome 在 anland 显示桥上有呈现反馈缺失的平台级问题）
- 一键体检：`bash tools/check-browser-vaapi.sh`

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
