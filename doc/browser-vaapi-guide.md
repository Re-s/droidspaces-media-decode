# 浏览器 VA-API 硬解接入指南（Chrome / Firefox）

> 实测环境：nabu（SD855 / Adreno 640）+ DroidSpaces Debian 13 trixie 容器 +
> msm_drm_drv_video.so（驱动内 V4L2 直通）。
> 浏览器侧的结论来自真机验证（验证日期 2026-08-26；
> 第二章第 4、5 节为 0.4.3 绿屏排查追加，2026-09-03；
> 第二章第 6 节为 0.4.4 seek / 切清晰度卡顿排查追加，2026-09-03），
> 非理论推导。

前置：驱动已按 README 部署完成，且 `ffmpeg -hwaccel vaapi` 解码正常。
若这一步不通，先解决后端，别碰浏览器。

> ⚠️ **0.4.1 更正：`vainfo` 现在可以正常用。** 本文原写"它在本平台会挂住，
> 即使指定不存在的驱动名也一样"—— 那是 0.3.x daemon 架构下的现象
> （`vainfo` 初始化时会去连 socket，daemon 没起来就卡在那里）。
> 0.4.0 起没有 daemon 与 socket，`vainfo` 直接打开 `/dev/video32`，
> 正常返回：
>
> ```
> vainfo: Driver version: DroidSpaces V4L2 VA-API driver 0.4.1
>       VAProfileH264ConstrainedBaseline: VAEntrypointVLD
>       VAProfileH264Main               : VAEntrypointVLD
>       VAProfileH264High               : VAEntrypointVLD
>       VAProfileHEVCMain               : VAEntrypointVLD
>       VAProfileVP9Profile0            : VAEntrypointVLD
> ```
>
> 但**它只证明驱动能加载、profile 列表是静态声明的**，不证明能出帧。
> 要验真实解码仍用下面的 ffmpeg 命令。

---

## 零、先确认后端真的在硬解

这是唯一一条必须先跑的检查，因为后面所有浏览器配置都建立在它成立的基础上。

```shell
LIBVA_DRIVER_NAME=msm_drm ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi \
       -i test.mp4 -f null - 2>&1 | tail -1
```

⚠️ **必须带 `-hwaccel_output_format vaapi`。** 只写 `-hwaccel vaapi` 时
ffmpeg 拿不到硬解会**静默回落软解且不报错**，日志里出现 `hevc (native)`
就是软解 —— 这样测什么配置都"正常"，等于没测。

> ### 0.4.0 起不再需要选择端点
>
> 0.3.x 的驱动要在 Unix socket 与 TCP 之间选，因为解码走
> `容器 → socket → Android 侧 decode-daemon → MediaCodec`。
> 那时有个大坑：Unix 端点默认 `SO_RCVBUF` 只有 224KB 装不下整帧
> （720p NV12 是 1.38MB），导致吞吐塌陷到 0.92x 并静默回落软解，
> 需要给浏览器钉 `DMD_ENDPOINT=tcp:20003` 绕开。
>
> **这些全部不再适用。** 0.4.0 的驱动在浏览器进程内直接打开
> `/dev/video32`，没有 socket、没有 daemon、没有端点可选。
> `DMD_ENDPOINT` 与 `DMD_WANT_SHM` 两个环境变量已无任何读者，
> 设了也不起作用 —— 如果你的 `.desktop` 文件里还留着它们，
> 可以删掉（留着无害，只是没用）。

---

## 一、Chrome / Chromium（必须 Wayland 模式）

### 1. 为什么必须 Wayland

`VaapiVideoDecodeLinux` 系 feature 的解码帧经 **linux-dmabuf 协议**零拷贝提交给
合成器。X11/XWayland 下没有这条协议，后果是：

```
dmd 日志特征 —— 握手成功但零流量:
[12] 握手成功: video/hevc 1280x720 帧回传=内联
[12] 会话结束: 收到 0 NALU, 回传 0 帧     ← 解码器创建后输出无门,空转自杀
```

驱动栈看似加载（GPU 进程 maps 里 libva/drv_video 都在），实际一帧不解。
**X11 模式下此路不通，没有变通。**

### 2. 必需启动参数（缺一不可）

```shell
google-chrome \
  --ozone-platform=wayland \
  --render-node-override=/dev/dri/renderD128 \
  --ignore-gpu-blocklist \
  --enable-features="VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL"
```

| 参数 | 原理 |
|---|---|
| `--ozone-platform=wayland` | 见上节，dmabuf 输出的前提 |
| `--render-node-override=/dev/dri/renderD128` | **核心**。Chromium `vaapi_wrapper.cc` 的 `PreSandboxInitialization()` 只枚举 PCI 总线 DRM 设备，ARM 平台设备的 renderD128 会被 `if (device->bustype != DRM_BUS_PCI) continue;` 跳过。此开关走 `LoadDrmFD()` 分支绕过白名单 |
| `--ignore-gpu-blocklist` | ARM GPU 在 Chrome 的软件渲染黑名单里 |
| `--enable-features=...` | Linux VA-API 解码总开关（DMABUF/GL 两路都开） |

注意：容器里通常需要 `MESA_LOADER_DRIVER_OVERRIDE=msm` 让 GL 栈认出 Adreno。

### 2.5 关闭 Vulkan：只能在 chrome://flags 里改

ozone wayland 与 Vulkan 硬性冲突，不关掉的话 GPU 进程会报：

```
ui/ozone/platform/wayland/gpu/wayland_surface_factory.cc:249] ERROR:
'--ozone-platform=wayland' is not compatible with Vulkan.
Consider switching to '--ozone-platform=x11' or disabling Vulkan
```

**唯一可靠的关法是打开 `chrome://flags`，把 Vulkan 设为 `Disabled`，重启浏览器。**

⚠️ 命令行开关关不掉它。本文此前写的 `--disable-vulkan` 是错的 ——
Chrome 151 的二进制里根本没有这个开关（只有 `enable-vulkan` 与 `use-vulkan`），
而 Chromium 的 switch 不像 feature flag 那样自动生成 `disable-` 反面，
传进去既不报错也不生效，纯粹被忽略。实测下列写法**全部无效**，
GPU 进程照样打印上面那条警告：

```sh
--disable-vulkan                            # 开关不存在，被忽略
--disable-features=Vulkan                   # 无效
--use-vulkan=disabled                       # 无效
--use-vulkan=disabled --disable-features=Vulkan,VulkanFromANGLE   # 仍然无效
```

实测环境 Chrome 151.0.7922.173，判据是 GPU 进程还会不会继续打印
`not compatible with Vulkan`。

后果是这一项**没法写进 `.desktop` 固化**，换机器或重建 profile 后要手动再关一次
（`chrome://flags` 的选择存在 profile 的 `Local State` 里，备份 profile 会带走）。

### 3. 固化到桌面图标（幂等：重复执行不会叠加）

在容器终端里**整段复制执行**——已配置过的文件自动跳过，备份只在首次创建：

```shell
D=/usr/share/applications/google-chrome.desktop
if ! grep -q "render-node-override" "$D"; then
    [ -f "$D.bak" ] || sudo cp "$D" "$D.bak"
    FLAGS="--ozone-platform=wayland --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist --enable-features=VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL"
    sudo sed -i \
      -e "s|^Exec=/usr/bin/google-chrome-stable $|Exec=/usr/bin/google-chrome-stable $FLAGS %U|" \
      -e "s|^Exec=/usr/bin/google-chrome-stable |Exec=/usr/bin/google-chrome-stable $FLAGS |" \
      "$D"
fi
grep -c "render-node-override" "$D"    # 每个 Exec 入口 1 次,不应随执行次数增长

# 执行后每个 Exec= 行应形如(注意开头就是 --ozone-platform=wayland):
# Exec=/usr/bin/google-chrome-stable --ozone-platform=wayland --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist --enable-features=VaapiVideoDecodeLinux,... %U
```

容器内没有 sudo 时，从宿主侧改同一个文件
`/mnt/Droidspaces/<容器名>/usr/share/applications/google-chrome.desktop`
（宿主 root 直写容器 rootfs）。

只想临时试一次，不固化：把上面 FLAGS 的值接在 `google-chrome` 后面手动启动。

### 4. 验证三步

```shell
# ① GPU 进程加载了驱动栈(数字均应 >0)
for p in $(pgrep -f "type=gpu-process"); do
  grep -c libva /proc/$p/maps; grep -c drv_video /proc/$p/maps; done

# ② 驱动侧出现真实解码会话
#    0.4.0 起没有 daemon 日志了；驱动日志走 stderr，需要以 DMD_VA_LOG=1
#    启动浏览器才能看到（.desktop 里 Exec=env DMD_VA_LOG=1 ...）
#    期望看到（0.4.1 实测输出）：
#      [dmd-va] init: ... vendor=DroidSpaces V4L2 VA-API driver 0.4.1
#      [v4l2] S_FMT(OUTPUT) OK: 1920x1088 sizeimage=16588800
#      [v4l2] 缓冲来源: /dev/ion mask=0x2000000（无 dma_heap: ...）
#      [v4l2] CAPTURE 就绪: 1920x1088（有效 1920x1088）stride=1920 slice_h=1088
#      [v4l2] 会话就绪: OUTPUT 8 x 16588800 字节, CAPTURE 24 x 3137536 字节
#      [v4l2] PORT_SETTINGS(SUFFICIENT): h=1088 w=1920 ← 固件解析出序列参数
#      [v4l2] 已发 SESSION_CONTINUE（事件 seq=0）      ← 有这两行才真在解码
#
#    ⚠️ 0.4.1 更正：本文原写"[v4l2] 收到 SOURCE_CHANGE ← 有这行才是固件
#    真的在解码"。那行**永远不会出现** —— msm_vidc 从不发标准
#    V4L2_EVENT_SOURCE_CHANGE，它发的是私有事件 0x08001002
#    (PORT_SETTINGS_CHANGED_SUFFICIENT)。拿 SOURCE_CHANGE 当判据会
#    误判成"没在硬解"。这不只是措辞问题：下游 msm_vidc 不是标准 V4L2
#    stateful 解码器，分辨率变更只经私有 PORT_SETTINGS_* 通知，
#    重配序列也与内核规范不同（见第二章第 6 节）。
#
#    ⚠️ 0.4.4 起 SESSION_CONTINUE 是**每个事件发一次**（带事件 seq），
#    不再是一个会话只发一次。日志里出现多行属正常，缺行才是问题。
#
#    只有 init 而没有"会话就绪"，说明浏览器没把解码交给本驱动

# ③ 页面侧帧计数增长
video.getVideoPlaybackQuality().totalVideoFrames
```

H.264 实测：720p30 连续播放数小时级会话稳定（单会话 16900 帧）。
HEVC 实测：本地文件解码与渲染正常；在线流见"已知限制"。

### 5. 已知限制：anland 显示桥呈现反馈缺失

在 DroidSpaces 的 anland 合成桥上，Chrome 提交的帧存在 **presentation
feedback 断链**，定量特征（向平台方报障时请附上）：

- `getVideoPlaybackQuality().totalVideoFrames` 持续增长、dropped≈0
  （Chrome 认为一切正常）
- 但 `requestVideoFrameCallback` **零回调**（帧从未收到"已呈现"回执）
- 五种组合全部复现：{Wayland, X11} × {硬解, 软解} ± dmabuf 开关
- 用户观感：视频掉帧跳跃、间歇绿屏（YUV 平面错配的典型表现）
- 同机 Firefox 播放同一视频完全正常 → 排除解码层，锁定显示桥

这是 anland 对 Chrome 提交协议的兼容缺陷，不是本项目解码管线的问题。
**观看 HEVC 视频目前推荐 Firefox**（见下节）。

另：X11 下即使不硬解（纯软解）也存在同类呈现异常，进一步佐证问题在
显示桥对 Chrome 提交模式的支持，与 VA-API 无关。

---

## 二、Firefox ESR（HEVC 观看推荐）

Firefox 的 VA-API 走 ffmpeg 后端 + WebRender 提交，实测同视频完美流畅，
是当前 HEVC 观看的首选。

### 1. 写入硬解配置（幂等：重复执行不会写重）

背景：Firefox 的配置写在所选 profile 目录的 `user.js` 文件里；而"选中的
profile"由 `~/.mozilla/firefox/installs.ini` 的 `Default=` 决定（注意
`profiles.ini` 里老式 `Default=1` 标记无效，别看错文件）。下面的命令自动
定位真实 profile、**逐条检查已存在则跳过**，重复执行安全：

推荐直接用脚本，它会自动找到所有 profile 并幂等写入：

```shell
bash tools/configure-firefox-vaapi.sh            # 配置（Firefox 需先关闭）
bash tools/configure-firefox-vaapi.sh --verify   # 查状态
bash tools/configure-firefox-vaapi.sh --uninstall # 还原
```

⚠️ **profile 路径不能写死。** 不同安装方式放在不同地方，而且同一台机器上
可能同时存在多处 —— 本机实测 `~/.mozilla/firefox` 与
`~/.config/mozilla/firefox` 下各有两个真实 profile（都有 `prefs.js`）。
只写前者的话，Firefox 实际用的是后者时就会"配了但没生效"，
而这种失败最难排查：页面能播、统计看着正常，实际静默走软解。
脚本会遍历这四处：

| 位置 | 对应安装方式 |
|---|---|
| `~/.mozilla/firefox` | apt / 官方 tarball（传统位置） |
| `~/.config/mozilla/firefox` | 较新版本遵循 XDG 后的新位置 |
| `~/snap/firefox/common/.mozilla/firefox` | snap |
| `~/.var/app/org.mozilla.firefox/.mozilla/firefox` | flatpak |

`FIREFOX_HOME` 可指定只处理某一个，`DMD_DRIVER_DIR` 指定驱动目录。

手动配置的话，pref 清单如下（写进对应 profile 的 `user.js`，重启生效）：

```js
user_pref("media.hardware-video-decoding.enabled", true);
user_pref("media.ffmpeg.vaapi.enabled", true);
user_pref("media.hevc.enabled", true);
// 零拷贝：解码帧经 dmabuf 直进合成器。缺了它硬解照跑，但每帧要拷回内存
// 再做 CPU 软件转色，内容进程能吃掉半个到多个核心 ——
// 表现为"开了硬解 CPU 反而更高"。
user_pref("media.ffmpeg.vaapi.force-surface-zero-copy", 2);
// 播放队列深度：不是可选项。Venus 固件的流水线滞后 4 个输入单元才吐首帧，
// Firefox 默认队列吸收不了这个抖动。1080p30 27Mbps 实测：
// 不加丢帧 14.25% / 顺序回退 20.75%，加上降到 0.89% / 4.04%
// （软解基线 0.5% / 1.85%，即与软解同量级）。
user_pref("media.video-queue.hw-accel-size", 10);
user_pref("media.video-queue.default-size", 10);
user_pref("media.video-queue.send-to-compositor-size", 6);
```

> ⚠️ `media.vaapi-dmabuf-textures.enabled` 已废弃 —— 本文此前推荐过它，
> 但 Firefox 154 的 libxul 里已无此 pref（实测），写了不起作用。
> 零拷贝现在由 `media.ffmpeg.vaapi.force-surface-zero-copy` 控制。
> `media.hardware-video-decoding.force-enabled`、`media.gpu-process-decoding`、
> `media.rdd-ffmpeg.enabled` 这三项仍然存在，但不是必需项，脚本没有写入。

### 2. 关闭 RDD 沙箱（必需，幂等）

RDD（远程解码进程）的 seccomp 沙箱会拦截 `/dev/dri` 打开与 socket 连接，
容器环境下必须放开。已改过的文件会自动跳过：

```shell
F=/usr/share/applications/firefox-esr.desktop
if ! sudo grep -q "MOZ_DISABLE_RDD_SANDBOX" "$F"; then
    sudo cp "$F" "$F.bak"
    sudo sed -i 's|^Exec=/usr/lib/firefox-esr/firefox-esr |Exec=env MOZ_DISABLE_RDD_SANDBOX=1 /usr/lib/firefox-esr/firefox-esr |' "$F"
fi
grep -c "MOZ_DISABLE_RDD_SANDBOX" "$F"   # 输出 >=1 即成功
```

没有 sudo 就从宿主侧改 `/mnt/Droidspaces/<容器名>/usr/share/applications/firefox-esr.desktop`。
手动启动时同理：`env MOZ_DISABLE_RDD_SANDBOX=1 firefox <地址>`。
Wayland 原生模式（ESR 121+ 默认开启）无需额外变量。

### 3. 验证

```shell
# RDD 进程加载驱动栈(数字应 >0; 未播放视频时为 0,属正常,播一段再看)
grep -c drv_video "/proc/$(pgrep -f 'rdd$' | head -1)/maps"
```

- `about:support` → 图形 → 应显示 VA-API 相关条目
- 驱动日志（需 `DMD_VA_LOG=1`）出现 `会话就绪` 与 `已发 SESSION_CONTINUE`
  （0.4.0 起没有 daemon，本文旧版写的 `daemon 日志 ... 收到 N NALU` 已不适用）
- 拖过进度条或切过清晰度后，还应能看到 `重配：FLUSH_DONE 已收到` 与
  `重配完成并已补发 SESSION_CONTINUE`（0.4.4 起；只有 `INSUFFICIENT`
  没有这两行，说明重配没走完，见第 6 节）
- glxtest 报 "No GPUs detected via PCI" 是噪声，不影响 DMABUF 解码路径

### 4. Firefox 特有的三个坑（0.4.3 绿屏排查所得）

**① 环境变量可能进不了 RDD 进程 —— 会让你误以为在测硬解。**

Firefox 把解码放在单独的 RDD 进程里。用 `nohup firefox` 之类方式设置的
`LIBVA_DRIVER_NAME` / `LIBVA_DRIVERS_PATH` **未必传得进去**，此时它静默走
CPU 软解，而你以为在测硬解 —— 排查中就因此白跑过一轮。

判据（软解时的表现）：`MOZ_LOG` 里 `VAAPI` 与 `DMABUF` 出现 **0 次**，
解码器描述只有 `ffmpeg video decoder (RDD remote)`。可靠的核实方法是直接
查进程：

```shell
R=$(pgrep -f 'RDD Process' | head -1)
tr '\0' '\n' < /proc/$R/environ | grep -E 'LIBVA|MOZ_DISABLE_RDD'
grep -c drv_video /proc/$R/maps        # 应 >0
ls -l /proc/$R/fd | grep -c video32    # 硬解唯一通路，应 >0
```

0.4.3 起版本串带 git 短 hash，`vainfo` 能直接确认**实际加载的是哪一版 `.so`**，
不必靠文件时间戳猜（系统目录与 `LIBVA_DRIVERS_PATH` 常各有一份）。

**② `media.ffmpeg.vaapi.force-surface-zero-copy` 是三态整数，且改它无法隔离问题。**

它不是布尔。Firefox 源码 `modules/libpref/init/StaticPrefList.yaml` 的注释是
`0 - force disable`、`1 - force enable`、`2 - default`，**默认 2**。
注释首句 "Force to copy dmabuf video frames" 容易读反 —— 这个 pref 管的是
「是否强制零拷贝」。

关键在于**设成 0 并不会让 Firefox 改走 CPU 下载路径**。Linux 上
`DMABufSurfaceYUV::CopyYUVDataImpl` 仍然先 `vaExportSurfaceHandle` 把源
dmabuf 导入成 EGL 纹理，再 `BlitTextureToTexture` 拷到另一块纹理。
所以源描述有错时，设 0 **照样绿屏** —— 这个对照实验区分不出
「驱动导出的几何错」和「Firefox 导入有问题」，不要用它下结论。

**③ `LD_PRELOAD` 拦不到 Firefox 的 `vaExportSurfaceHandle`。**

Firefox 经 libva 的 driver vtable 直接进入驱动回调，不走公共符号。
实测 RDD 进程确实加载了 hook 库（`/proc/<pid>/maps` 可见），但**没有任何
记录产生**。要拿它的真实入参只能在驱动内部打日志 —— 这也是 `DMD_VA_LOG`
会打印导出 flags 的原因。

实测 Firefox 传的是 `flags=0x5`，即
`VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_READ_ONLY`，
驱动返回两层：Y 用 `DRM_FORMAT_R8`、UV 用 `DRM_FORMAT_GR88`，
单个 dmabuf object，modifier 为 `DRM_FORMAT_MOD_LINEAR`。
另一个要点：**Firefox 在解码之前就会导出** dmabuf 去建纹理，所以驱动的
export 路径不能等帧就绪（Chrome 同样如此）。

### 5. 判断画面是否真的正常（不必截图）

想客观确认「有没有绿屏」时会撞到两道墙：KWin 对未授权进程返回
`org.kde.KWin.ScreenShot2.Error.NoAuthorized`；XWayland 的 `xwd -root` 拿不到
Wayland 原生窗口内容（报 `BadMatch`）。

所以改用驱动内的探针 —— `DMD_VA_LUMA=1` 会在每次导出时抽样打印：

```
像素: surface=3 Y均值 29.3 UV均值 123.7 UV近零 2%
像素: surface=1 Y均值 0.0  UV均值 128.0 UV近零 0%   ← 黑帧
```

**只看 Y 判不出绿屏，判据在 UV**（NV12 的 `UV=0` 是最大色偏，转 RGB 就是纯绿）：

| 画面 | UV 均值 |
|---|---|
| 正常彩色 | 123.6 – 124.8 |
| 纯黑（起播前的合法初值） | 128.0 |
| 纯绿（异常） | 0.0，且 `UV近零` > 80% 会标 `← 纯绿!` |

0.4.3 在 720p 上的 45 秒长播复测：924 次导出，纯绿 0、截断 0、错误 0，
送入 3863 单元收到 3863 帧。

### 6. seek / 切清晰度卡数秒：0.4.3 及更早的固定循环（0.4.4 已修）

**症状**：拖进度条或播放器自动切清晰度（ABR）后，画面卡住数秒才恢复，
而且会反复发生。0.4.4 修复；在旧版上这不是配置问题，换 pref 没用。

**判据在驱动日志里，是一个每轮约 7 秒的固定循环**（需 `DMD_VA_LOG=1`）：

```
PORT_SETTINGS(INSUFFICIENT)
flush 触发: futile=0(recv=0 has_seq=0 pend=5) spent=2000/2000
SyncSurface: 等帧超时 5000 ms
会话已重建（codec=0 864x480）
```

7 秒 = 2 秒 flush 阈值 + 5 秒 `SyncSurface` 超时，然后重建会话。
`recv=0` 是关键 —— 送了料但一帧没回来，固件停在 `in_reconfig` 等重配。

**根因**：固件用 msm_vidc 私有事件 `PORT_SETTINGS_CHANGED_INSUFFICIENT`
（`V4L2_EVENT_MSM_VIDC_START+3`）要求按新几何重配 CAPTURE，旧版收到后没有
真正重配。0.4.4 按厂商 OMX 的正规序列处理：先 `FLUSH_CAPTURE` 并等
`FLUSH_DONE` 把固件手里的输出缓冲全部收回，再 `STREAMOFF(CAPTURE)` +
`REQBUFS(CAPTURE,0)`，之后才释放 dma-buf，最后按新几何重配并
`STREAMON(CAPTURE)`。序列细节与内核取证见
[`verified-platform-facts.md`](verified-platform-facts.md) §12。

**0.4.4 上该看到的日志**（一次成功的重配）：

```
[v4l2] PORT_SETTINGS(INSUFFICIENT): h=480 w=856 ...
[v4l2] INSUFFICIENT：开始重配 CAPTURE 第 1 次（当前 1920x1088 ...）
[v4l2] 重配：FLUSH_DONE 已收到
[v4l2] CAPTURE 就绪: 856x480 ...
[v4l2] 重配完成并已补发 SESSION_CONTINUE
```

看到 `INSUFFICIENT` 却没有 `重配完成`，说明重配中断了（日志里会有
`重配中收到 SYS_ERROR` 或 `CAPTURE 重配失败`）—— 这时才是真的缺陷，
可带这段日志报障。

**另一处易误判**：`已发 SESSION_CONTINUE` 在 0.4.4 起是**逐事件**打印的
（带 `事件 seq=`），一次播放里出现多行属正常，不是重复发送的 bug。
反过来，浏览器 seek 会在同一 fd 上反复触发事件，**漏发一次就永久卡在等帧**。

**实测**（Firefox，H.264/HEVC，含 seek 与 856x480 ↔ 1920x1080 切换）：
`SYS_ERROR` 从数百次降到 0，2 秒 flush 空等 0、5 秒 `SyncSurface` 超时 0、
会话重建循环 0，9 次 `INSUFFICIENT` 全部重配成功（9/9），2977 帧配对，
`DestroyContext` 送入/取回完全平衡（1080/1080、850/850、630/630）。

---

## 三、实时监视：确认硬解"持续"在用

单次体检只能回答"此刻通不通"。播放视频时 CPU 曲线本身是脉冲状的
（解码按帧突发 + 播放器有帧缓冲），光看占用起伏无法判断硬解是否中断。

```shell
# 持续监视，Ctrl-C 退出；加 ADB=1 额外显示宿主侧 VPU 证据
ADB=1 bash tools/watch-decode.sh

# 只跑 60 秒
bash tools/watch-decode.sh 60
```

输出形如：

```
时刻     │ Firefox RDD          │ Chrome GPU      │ 宿主 VPU
16:50:47 │ ✅硬解 cpu=61        │ —— 无 GPU 进程   │ ✅解码中 36/5s
16:50:48 │ ✅硬解 cpu=77        │ —— 无 GPU 进程   │ ✅解码中 71/5s
16:50:49 │ ✅硬解 cpu=59        │ —— 无 GPU 进程   │ ✅解码中 127/5s
```

**判读要点：看 ✅ 标记是否稳定，不要看 cpu 数值是否平滑。**
数值在 59~95 之间起伏是正常的，只要 `✅硬解` 一直在，硬解就没有中断。

其中「宿主 VPU」是最硬的证据 —— 宿主 `omx@1.0-service`（硬件解码服务）
的 CPU 消耗。实测空闲约 12 jiffies/5s、解码约 107 jiffies/5s，**差约 9 倍**，
阈值取 30 分界。它涨说明 VPU 真在出帧，与上层怎么统计无关。

## 四、快速体检脚本

一键检查 daemon 连通性、两个浏览器的驱动栈加载状态、最近解码会话流量。
无需 clone 整个仓库，直接下载运行：

```shell
# 方式一: 下载后执行(推荐,可先审阅内容)
curl -fsSLO https://raw.githubusercontent.com/Re-s/droidspaces-media-decode/v0.3.4/tools/check-browser-vaapi.sh
bash check-browser-vaapi.sh

# 方式二: 管道直跑
curl -fsSL https://raw.githubusercontent.com/Re-s/droidspaces-media-decode/v0.3.4/tools/check-browser-vaapi.sh | bash
```

> 直连 GitHub 失败时走系统代理，例如：
> `curl -fsSL --proxy socks5h://127.0.0.1:1080 ...`
>
> 链接锚定在 `v0.3.4` 标签上，脚本行为与本文档描述严格一致；
> 想要最新版把 URL 里的 `v0.3.4` 换成 `master`。

## 五、排障速查表

| 症状 | 根因 | 处置 |
|---|---|---|
| 会话建立成功但 0 帧 | Chrome 跑在 X11，dmabuf 输出走不通 | 换 Wayland 模式 |
| GPU 进程 maps 无 drv_video | PCI 白名单跳过了平台设备 | 确认 `--render-node-override` |
| 日志报 `not compatible with Vulkan` | wayland 与 Vulkan 硬性冲突 | 在 `chrome://flags` 把 **Vulkan** 设为 Disabled 后重启，命令行开关无效，见下 |
| Firefox 有进程不解码 | RDD 沙箱拦设备 | `MOZ_DISABLE_RDD_SANDBOX=1` |
| user.js 写了没生效 | 写错了 profile | 查 installs.ini 的 Default |
| Chrome HEVC 在线流掉帧/绿屏 | anland 呈现反馈缺失（平台 bug） | 用 Firefox；或等平台修复 |
| 视频卡顿 + CPU 飙高，但"硬解已启用" | 实际回落软解了（0.4.0 无端点问题，多为浏览器侧没走硬解路径） | 按第零章的 ffmpeg 判据自查后端；再用验证三步确认浏览器进程加载了驱动 |
| ffmpeg 日志 `hevc (native)` | 根本没用硬解，静默回落软解了 | 测试命令要带 `-hwaccel_output_format vaapi` |
| 拖进度条/切清晰度卡数秒并反复 | `INSUFFICIENT` 重配没做（0.4.3 及更早） | 升级到 0.4.4；判据见第二章第 6 节的 7 秒循环 |
| 驱动 init 了但一帧不出 | 该设备的 V4L2 解码会话起不来（已知有此类设备） | 跑 `vaapi-driver/tools/probe_device_support.c`，看有没有事件到达（⚠️ 该探针只订阅标准 `SOURCE_CHANGE`，而 msm_vidc 只发私有 `PORT_SETTINGS_*`，所以"没有 `SOURCE_CHANGE`"本身不构成不可用的判据，见第二章第 6 节） |
