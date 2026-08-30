# 浏览器 VA-API 硬解接入指南（Chrome / Firefox）

> 实测环境：nabu（SD855 / Adreno 640）+ DroidSpaces Debian 13 trixie 容器 +
> msm_drm_drv_video.so（驱动内 V4L2 直通）。
> 浏览器侧的结论来自真机验证（验证日期 2026-08-26），非理论推导。

前置：驱动已按 README 部署完成，且 `ffmpeg -hwaccel vaapi` 解码正常。
若这一步不通，先解决后端，别碰浏览器。

> ⚠️ **不要用 `vainfo` 做前置检查** —— 它在本平台会挂住，
> 即使指定不存在的驱动名也一样。用下面的 ffmpeg 命令代替。

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
  --disable-vulkan \
  --render-node-override=/dev/dri/renderD128 \
  --ignore-gpu-blocklist \
  --enable-features="VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL"
```

| 参数 | 原理 |
|---|---|
| `--ozone-platform=wayland` | 见上节，dmabuf 输出的前提 |
| `--disable-vulkan` | ozone wayland 与 Vulkan 不兼容（Chrome 硬性检查），必须显式关闭 |
| `--render-node-override=/dev/dri/renderD128` | **核心**。Chromium `vaapi_wrapper.cc` 的 `PreSandboxInitialization()` 只枚举 PCI 总线 DRM 设备，ARM 平台设备的 renderD128 会被 `if (device->bustype != DRM_BUS_PCI) continue;` 跳过。此开关走 `LoadDrmFD()` 分支绕过白名单 |
| `--ignore-gpu-blocklist` | ARM GPU 在 Chrome 的软件渲染黑名单里 |
| `--enable-features=...` | Linux VA-API 解码总开关（DMABUF/GL 两路都开） |

注意：容器里通常需要 `MESA_LOADER_DRIVER_OVERRIDE=msm` 让 GL 栈认出 Adreno。

### 3. 固化到桌面图标（幂等：重复执行不会叠加）

在容器终端里**整段复制执行**——已配置过的文件自动跳过，备份只在首次创建：

```shell
D=/usr/share/applications/google-chrome.desktop
if ! grep -q "render-node-override" "$D"; then
    [ -f "$D.bak" ] || sudo cp "$D" "$D.bak"
    FLAGS="--ozone-platform=wayland --disable-vulkan --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist --enable-features=VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL"
    sudo sed -i \
      -e "s|^Exec=/usr/bin/google-chrome-stable $|Exec=/usr/bin/google-chrome-stable $FLAGS %U|" \
      -e "s|^Exec=/usr/bin/google-chrome-stable |Exec=/usr/bin/google-chrome-stable $FLAGS |" \
      "$D"
fi
grep -c "render-node-override" "$D"    # 每个 Exec 入口 1 次,不应随执行次数增长

# 执行后每个 Exec= 行应形如(注意开头就是 --ozone-platform=wayland):
# Exec=/usr/bin/google-chrome-stable --ozone-platform=wayland --disable-vulkan --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist --enable-features=VaapiVideoDecodeLinux,... %U
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
#    0.4.0 没有 daemon 日志了；驱动日志走 stderr，需要以 DMD_VA_LOG=1
#    启动浏览器才能看到（.desktop 里 Exec=env DMD_VA_LOG=1 ...）
#    期望看到：
#      [dmd-va] init: ... vendor=DroidSpaces V4L2 VA-API driver 0.4.0
#      [dmd-va] 会话已建立: codec=0 1280x720 端点=/dev/video32
#      [v4l2] 收到 SOURCE_CHANGE          ← 有这行才是固件真的在解码
#    只有 init 而没有"会话已建立"，说明浏览器没把解码交给本驱动

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

```shell
P=~/.mozilla/firefox/$(grep -m1 '^Default=' ~/.mozilla/firefox/installs.ini | cut -d= -f2)
mkdir -p "$P"; touch "$P/user.js"
add() { grep -qxF "$1" "$P/user.js" || echo "$1" >> "$P/user.js"; }
add 'user_pref("media.ffmpeg.vaapi.enabled", true);'
add 'user_pref("media.hardware-video-decoding.force-enabled", true);'
add 'user_pref("media.gpu-process-decoding", true);'
add 'user_pref("media.rdd-ffmpeg.enabled", true);'
# 第五件:解码帧经 dmabuf 零拷贝纹理直进合成器 —— 缺了它硬解照跑,
# 但每帧都要拷回内存再做 CPU 软件转色,内容进程能吃掉半个到多个核心
add 'user_pref("media.vaapi-dmabuf-textures.enabled", true);'
# 顺手清理旧版本命令可能产生的重复行
awk '!(/^user_pref/ && seen[$0]++)' "$P/user.js" > "$P/user.js.tmp" && mv "$P/user.js.tmp" "$P/user.js"
grep -c '^user_pref' "$P/user.js"    # 每项应为 1
```

重启 Firefox 生效。

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
- daemon 日志出现 `video/avc|hevc ... 收到 N NALU, 回传 M 帧`（N>0）
- glxtest 报 "No GPUs detected via PCI" 是噪声，不影响 DMABUF 解码路径

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
> 协议解析客户端（TCP 内联模式演示）：[tools/test_decode.py](../tools/test_decode.py)。

## 五、排障速查表

| 症状 | 根因 | 处置 |
|---|---|---|
| 会话建立成功但 0 帧 | Chrome 跑在 X11，dmabuf 输出走不通 | 换 Wayland 模式 |
| GPU 进程 maps 无 drv_video | PCI 白名单跳过了平台设备 | 确认 `--render-node-override` |
| 启动即崩报 Vulkan 不兼容 | wayland+vulkan 硬性冲突 | 加 `--disable-vulkan` |
| Firefox 有进程不解码 | RDD 沙箱拦设备 | `MOZ_DISABLE_RDD_SANDBOX=1` |
| user.js 写了没生效 | 写错了 profile | 查 installs.ini 的 Default |
| Chrome HEVC 在线流掉帧/绿屏 | anland 呈现反馈缺失（平台 bug） | 用 Firefox；或等平台修复 |
| 视频卡顿 + CPU 飙高，但"硬解已启用" | 实际回落软解了（0.4.0 无端点问题，多为浏览器侧没走硬解路径） | 按第零章的 ffmpeg 判据自查后端；再用验证三步确认浏览器进程加载了驱动 |
| ffmpeg 日志 `hevc (native)` | 根本没用硬解，静默回落软解了 | 测试命令要带 `-hwaccel_output_format vaapi` |
| 驱动 init 了但一帧不出 | 该设备的 V4L2 解码会话起不来（已知有此类设备） | 跑 `vaapi-driver/tools/probe_device_support.c`：收不到 `SOURCE_CHANGE` 即该设备不可用 |
