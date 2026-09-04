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
| VP8 | ✅ 可用 | 90/90 帧，md5 与软解逐字节一致（0.4.2 新增）|
| AV1 Profile 0 | 🚧 未完成 | 帧数与 dav1d 一致，**像素未通过**，默认不声明 |
| MPEG-2 | 🚧 未完成 | 合成与原始流逐字节一致，但固件 `SYS_ERROR`，默认不声明 |
| HEVC Main10 / VP9 Profile2 | ❌ 固件限制 | 固件识别 10bit 但持续报 `INSUFFICIENT`，不出帧 |

v0.4.4 复测四路 1080p 回归结果不变（H.264 150 帧、HEVC 90 帧、VP9 90 帧、
VP8 90 帧），非 1080p 回归同样全部通过：HEVC 的 1280x720、854x480、640x360、
720x1280（竖屏）与一条 1080p→720p→480p 切换流，5 通过 0 失败。
H.264、VP9 的 720p / 360p 在 0.4.3 也已逐字节验证。判据一律是与软解 md5
逐字节一致，而不是"能解出来"。

**0.4.2 及更早版本播非 1080p 会绿屏**，详见下方自检一节的警告。
**0.4.3 及更早版本在浏览器里拖进度条或切清晰度会卡住数秒**，
已在 0.4.4 修复，见下方浏览器一节。

AV1 需要 `-DDMD_ENABLE_AV1` 编译才会声明；遗留缺陷见
[`doc/av1-v4l2-status.md`](doc/av1-v4l2-status.md)。
MPEG-2 需要 `-DDMD_ENABLE_MPEG2`；10bit 探测开关见 `CHANGELOG.md` 的 v0.4.2，
非 1080p 绿屏的根因与取证见 v0.4.3，seek / 切清晰度卡顿见 v0.4.4。

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

多分辨率回归（**需要真机**，跑的是真实硬解）：

```sh
cd vaapi-driver/tests
FFMPEG=/path/to/ffmpeg DRIVER_DIR=../build ./regress_resolutions.sh test.hevc
```

`FFMPEG` 指定带 libx265 与 vaapi 支持的 ffmpeg，`DRIVER_DIR` 指向
`msm_drm_drv_video.so` 所在目录（脚本用它设 `LIBVA_DRIVERS_PATH`）；
两者都有默认值（`ffmpeg` / `../build`），源码流参数默认 `test.hevc`，
找不到就以 77 跳过。它从源码流转出 1280x720、854x480（宽非 128 倍数）、
640x360、720x1280（竖屏）各一段，再拼一条 1080p→720p→480p 的切换流，
判据是**硬解与软解 md5 逐字节一致**，而不是"能解出来"。

> 这条回归补的是 0.4.2 及更早版本的盲区：原有四条回归流全是 1920x1080，
> 恰好等于 msm_vidc 打开设备时的默认 CAPTURE 几何，因此永远不触发
> "用 OUTPUT 协商值覆盖 CAPTURE 残留"那条分支 —— 1080p 全绿，
> 浏览器却在 720p 上绿屏。

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

> ⚠️ **0.4.1 及更早版本执行这条命令会挂死**（26 帧后 `internal decoding error`，
> 约 40s 超时），那是 CAPTURE 背压死锁，已在 0.4.2 修复。
> 在旧版上它会让你误判成"驱动不可用"。旧版请改用落盘形式自检：
> `... -i in.mp4 -pix_fmt yuv420p -f rawvideo out.yuv -y`。

> ⚠️ **0.4.2 及更早版本播非 1080p 会绿屏**，已在 0.4.3 修复 ——
> 如果你在用 0.4.2，这是升级的理由。`G_FMT(CAPTURE)` 总是返回
> msm_vidc 打开设备时的默认几何 `1920x1088`，不随 `S_FMT(OUTPUT)` 联动；
> 驱动虽然用 OUTPUT 协商值覆盖了宽高，但 `bytesperline` 与 `sizeimage`
> 驱动不回填，仍是 1080p 的值，而原有保护只检查下限
> （`if (d->stride < d->w * bpp2)`，播 1280x720 时 `1920 < 1280` 不成立），
> 于是错误 stride 被保留，造成双重损坏：按 `stride=1920` 取 1280 宽的帧
> **每行错位 640 字节**；用它反推 slice_height 得 `cap_size*2/(1920*3) = 492`，
> 把真实的 736 压成 492，于是每帧截断
> （`帧需 1416960 字节 > dumb buffer 1413120 字节`）。
> Firefox 实测 356 次导出里 351 次带着坏几何。
> 0.4.3 给 stride 加了上限校验（Venus 的 CAPTURE stride 按 128 对齐，
> 合法值必落在 `[align(w,128), align(w,128)+128)`），`cap_size` 按纠正后
> 几何重算。同版还修了起播首帧纯绿：NV12 里 `UV=0` 不是无色而是最大色偏，
> 经限制范围 BT.601 转 RGB 得到 `R≈0, G≈135, B≈0`（中性值是 128），
> 而 Firefox 会在解码之前就 `vaExportSurfaceHandle` 拿 fd 建纹理，
> 于是看到 surface 分配时 memset 出来的那一整块零 —— 改为 Y 填 0、UV 填 `0x80`
> 后，230 次导出 3 次纯绿变成 247 次导出 0 次。
> **1080p 自检通过不代表非 1080p 没问题**：旧版在 1080p 上完全正常。

想确认硬件真的在解码，可以看 Venus 的内核实况（用户态改不了这些值）：

```sh
# 解码期间 mvs0_gdsc 应为 enabled，空闲时 disabled
for r in /sys/devices/platform/soc/*gdsc/regulator/regulator.*/; do
  [ "$(cat $r/name)" = mvs0_gdsc ] && echo "mvs0_gdsc=$(cat $r/state)"; done
```

`vainfo` 也能用（0.3.x 时代它会挂在 daemon socket 上，0.4.0 起没有 socket 了）：

```
vainfo: Driver version: DroidSpaces V4L2 VA-API driver 0.4.4+<git短hash>
      VAProfileH264High               : VAEntrypointVLD
      VAProfileHEVCMain               : VAEntrypointVLD
      VAProfileVP9Profile0            : VAEntrypointVLD
      VAProfileVP8Version0_3          : VAEntrypointVLD
```

但它只证明驱动能加载、profile 列表是**静态声明**的，不证明能出帧 ——
验真实解码用上面的 ffmpeg 命令。

版本串从 0.4.3 起带构建 ID，当前形如 `0.4.4+<git短hash>`，工作区有未提交改动时
带 `-dirty`。这是排查工具 —— 浏览器把解码放在单独的 RDD/GPU 进程里，
系统目录和 `LIBVA_DRIVERS_PATH` 可能各有一份 `.so`，靠这个后缀能直接确认
实际 `dlopen` 的是哪一版，而不是你以为的那一版。

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

0.4.3 的 720p 长播复测（Firefox 硬解，45 秒）：924 次导出，纯绿 0、截断 0、
错误 0，送入 3863 单元收到 3863 帧。

### seek 与切清晰度：0.4.3 及更早会卡数秒

**0.4.3 及更早版本拖进度条或让播放器切清晰度会卡住数秒，然后循环重复。**
0.4.4 修复。如果你在用 0.4.3，这是升级的理由。

根因是分辨率变更事件没被真正处理。固件发的是 msm_vidc 私有事件
`PORT_SETTINGS_CHANGED_INSUFFICIENT`（`V4L2_EVENT_MSM_VIDC_START+3`），
要求按新几何重配 CAPTURE；旧版收到后没有真正重配，固件停在 `in_reconfig`
一帧不吐，上层白等 2 秒 flush 阈值加 5 秒 `SyncSurface` 超时后重建会话，
如此循环，**每轮约 7 秒**：

```
PORT_SETTINGS(INSUFFICIENT)
flush 触发: futile=0(recv=0 has_seq=0 pend=5) spent=2000/2000
SyncSurface: 等帧超时 5000 ms
会话已重建（codec=0 864x480）
```

`recv=0` 是它的指纹 —— 送了料但一帧没回来。

0.4.4 按厂商 OMX 的正规序列重配：先 `FLUSH_CAPTURE` 并等 `FLUSH_DONE`
把固件手里的 OPB 全部收回，再 `STREAMOFF(CAPTURE)` + `REQBUFS(CAPTURE,0)`，
之后才释放 dma-buf，最后按新几何重配并 `STREAMON(CAPTURE)`。
**第一步 flush 不可省**：跳过它时 24 个 OPB 仍在固件手里，
`STREAMON(CAPTURE)` 恒 `EINVAL` 并连锁 `SYS_ERROR`。全程只动 CAPTURE，
碰 OUTPUT 会丢掉已排队的输入。同版还修了 `SESSION_CONTINUE` 原本是会话级
一次性闩锁的问题 —— 每个事件都会重新置 `in_reconfig`，浏览器 seek/ABR
在同一 fd 上反复触发，漏发一次就永久卡在等帧，现改为逐事件发送。
序列细节与内核取证见
[`doc/verified-platform-facts.md`](doc/verified-platform-facts.md) §12。

Firefox 实测（H.264/HEVC，含 seek 与 856x480 ↔ 1920x1080 切换）：

| 指标 | 0.4.3 | 0.4.4 |
|---|---|---|
| `SYS_ERROR` | 数百次 | **0** |
| 2s flush 空等 | 反复 | **0** |
| 5s `SyncSurface` 超时 | 反复 | **0** |
| 会话重建循环 | 持续 | **0** |
| `INSUFFICIENT` | 卡死 | 9 次，**9/9 重配成功** |
| 帧配对 | 大量丢失 | 2977 帧 |

`DestroyContext` 的送入/取回计数完全平衡（1080/1080、850/850、630/630），
说明重配后既没丢帧，也没有残留未取回的 surface。

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

# ③ 确认解码进程真的加载了你以为的那一版（0.4.3 起版本串带 git 短 hash）
LIBVA_DRIVER_NAME=msm_drm vainfo 2>&1 | grep 'Driver version'
```

驱动日志（需 `DMD_VA_LOG=1`）出现这两行才是固件真的在解码：

```
[v4l2] PORT_SETTINGS(SUFFICIENT): h=1088 w=1920 ...
[v4l2] 已发 SESSION_CONTINUE（事件 seq=0）
```

拖过进度条或切过清晰度后，还应能看到重配走完的这三行（0.4.4 起）：

```
[v4l2] INSUFFICIENT：开始重配 CAPTURE 第 1 次（当前 1920x1088 ...）
[v4l2] 重配：FLUSH_DONE 已收到
[v4l2] 重配完成并已补发 SESSION_CONTINUE
```

只看到 `INSUFFICIENT` 而没有后两行，说明重配没走完，会退化成每轮约 7 秒的
会话重建循环 —— 那是 0.4.3 及更早版本的行为。

> ⚠️ 别拿 `[v4l2] 收到 SOURCE_CHANGE` 当判据 —— **那行永远不会出现。**
> msm_vidc 从不发标准 `V4L2_EVENT_SOURCE_CHANGE`，它发的是私有事件
> `0x08001002`。0.3.x/0.4.0 的文档写错了这一点，会导致误判成"没在硬解"。
> 这不只是日志措辞问题：下游 msm_vidc **不是标准 V4L2 stateful 解码器**，
> 分辨率变更只经私有 `PORT_SETTINGS_*` 通知，重配序列也与内核规范不同。
> 这正是原生 FFmpeg `v4l2_m2m` 与 GStreamer `v4l2videodec` 在这类设备上
> 用不了的根因，也是本项目自己实现驱动的原因。

> 遇到画面异常，**不要再从 CPU cache 未刷到 GPU 那条路查起**。0.4.3 排查绿屏时
> 长期怀疑这一点，已被实测推翻：用 `LD_PRELOAD` 在真实解码流程上把导出的
> dma-buf fd 重新 `mmap`，读到的字节与 `vaDeriveImage` 的 CPU 路径逐字节相同。
> `DMA_BUF_IOCTL_SYNC` 的 START/END 成对包裹仍保留（符合 `linux/dma-buf.h`
> 规范），但它不是绿屏的原因 —— 真正的原因是 CAPTURE 残留几何与 surface
> 初始值，见上面自检一节。

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

## 新内核适配（0.4.4）

老内核 msm_vidc（nabu / kernel 4.14）用 USERPTR 名义内存 + 私有事件 + SESSION_CONTINUE；
新内核（Android 12+ / kernel 5.x+，实测 Android 15 / kernel 6.6）改走**标准 V4L2 stateful
语义**：只接受 `V4L2_MEMORY_DMABUF`、发标准 `V4L2_EVENT_SOURCE_CHANGE`、在事件后协商
CAPTURE。

0.4.4 起驱动按 `REQBUFS` 结果自动分叉两条路径，老内核行为不变：

- `REQBUFS(DMABUF)` 成功 → 新内核路径：CAPTURE 延迟到 SOURCE_CHANGE 后配置；
- 被拒回退 `USERPTR` → 老内核路径：原有预配 + 私有 `PORT_SETTINGS` + `SESSION_CONTINUE`。

实测（新内核设备，Ubuntu 26.04 aarch64 容器，libva 1.23，ffmpeg 8.0.1）：H.264 High
（1080p/720p/854x480/640x360）与 HEVC Main（1080p）硬解与软解 **md5 逐字节一致**。

## 许可

见 [LICENSE](LICENSE)。
