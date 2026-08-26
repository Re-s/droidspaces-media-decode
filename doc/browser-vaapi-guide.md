# 浏览器 VA-API 硬解接入指南（Chrome / Firefox）

> 实测环境：nabu（SD855 / Adreno 640）+ DroidSpaces Debian 13 trixie 容器 +
> decode-daemon TCP 模式（127.0.0.1:20003，ksu 域）+ msm_drm_drv_video.so。
> 全部结论来自真机验证，非理论推导。验证日期：2026-08-26。

前置：daemon 与驱动已按 README 部署完成，`vainfo` 能列出 H264/HEVC profile，
`ffmpeg -hwaccel vaapi` 解码正常。若这一步不通，先解决后端，别碰浏览器。

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

### 3. 固化到桌面图标

在容器终端里**整段复制执行**（自动备份原文件、给所有启动入口追加参数）：

```shell
D=/usr/share/applications/google-chrome.desktop
sudo cp "$D" "$D.bak"
FLAGS="--ozone-platform=wayland --disable-vulkan --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist --enable-features=VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL"
sudo sed -i \
  -e "s|^Exec=/usr/bin/google-chrome-stable $|Exec=/usr/bin/google-chrome-stable $FLAGS %U|" \
  -e "s|^Exec=/usr/bin/google-chrome-stable |Exec=/usr/bin/google-chrome-stable $FLAGS |" \
  "$D"
grep -c "render-node-override" "$D"    # 输出 >=2 即固化成功
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

# ② daemon 侧出现真实解码会话(有 NALU 流量、有输出格式行)
tail -f /data/local/Droidspaces/Logs/decode-daemon-tcp.log
#   [N] 握手成功: video/avc 1280x720 ...
#   [N] 输出格式 1280x720 stride=1280 ...   ← 有这行才是真实解码

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

### 1. 写入硬解配置（一条命令，整段复制执行）

背景：Firefox 的配置写在所选 profile 目录的 `user.js` 文件里；而"选中的
profile"由 `~/.mozilla/firefox/installs.ini` 的 `Default=` 决定（注意
`profiles.ini` 里老式 `Default=1` 标记无效，别看错文件）。下面的命令自动
定位真实 profile 并追加配置：

```shell
P=~/.mozilla/firefox/$(grep -m1 '^Default=' ~/.mozilla/firefox/installs.ini | cut -d= -f2)
mkdir -p "$P"
cat >> "$P/user.js" <<'EOF'
user_pref("media.ffmpeg.vaapi.enabled", true);
user_pref("media.hardware-video-decoding.force-enabled", true);
user_pref("media.gpu-process-decoding", true);
user_pref("media.rdd-ffmpeg.enabled", true);
EOF
echo "已写入: $P/user.js" && tail -4 "$P/user.js"
```

看到四行 `user_pref(...)` 输出即成功。重启 Firefox 生效。

### 2. 关闭 RDD 沙箱（必需）

RDD（远程解码进程）的 seccomp 沙箱会拦截 `/dev/dri` 打开与 socket 连接，
容器环境下必须放开。在容器终端整段复制执行（自动备份、改所有 Exec 入口）：

```shell
F=/usr/share/applications/firefox-esr.desktop
sudo cp "$F" "$F.bak"
sudo sed -i 's|^Exec=/usr/lib/firefox-esr/firefox-esr |Exec=env MOZ_DISABLE_RDD_SANDBOX=1 /usr/lib/firefox-esr/firefox-esr |' "$F"
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

## 三、快速体检脚本

见 `tools/check-browser-vaapi.sh`。一键检查 daemon 连通性、两个浏览器的
驱动栈加载状态、最近解码会话流量。

## 四、排障速查表

| 症状 | 根因 | 处置 |
|---|---|---|
| dmd 会话握手成功但 0 NALU | Chrome 跑在 X11，dmabuf 输出走不通 | 换 Wayland 模式 |
| GPU 进程 maps 无 drv_video | PCI 白名单跳过了平台设备 | 确认 `--render-node-override` |
| 启动即崩报 Vulkan 不兼容 | wayland+vulkan 硬性冲突 | 加 `--disable-vulkan` |
| Firefox 有进程不解码 | RDD 沙箱拦设备 | `MOZ_DISABLE_RDD_SANDBOX=1` |
| user.js 写了没生效 | 写错了 profile | 查 installs.ini 的 Default |
| Chrome HEVC 在线流掉帧/绿屏 | anland 呈现反馈缺失（平台 bug） | 用 Firefox；或等平台修复 |
