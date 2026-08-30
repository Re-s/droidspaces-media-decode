把 Android MediaCodec 硬解码接到容器浏览器的全部步骤。以下命令在
**DroidSpaces 容器终端**里整段复制执行即可，无需手工编辑任何文件。
实测环境：nabu（SD855）+ Debian 13 + decode-daemon TCP 模式。

**前置**：daemon 已部署并监听 `127.0.0.1:20003`，`vainfo` 能列出 profile。

---

## Chrome（H.264 完美；HEVC 见"已知问题"）

### ① 固化到桌面图标（整段复制执行）

```bash
D=/usr/share/applications/google-chrome.desktop
if ! grep -q "render-node-override" "$D"; then     # 幂等:已配置则跳过
    [ -f "$D.bak" ] || sudo cp "$D" "$D.bak"
    FLAGS="--ozone-platform=wayland --disable-vulkan --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist --enable-features=VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL"
    sudo sed -i \
      -e "s|^Exec=/usr/bin/google-chrome-stable $|Exec=/usr/bin/google-chrome-stable $FLAGS %U|" \
      -e "s|^Exec=/usr/bin/google-chrome-stable |Exec=/usr/bin/google-chrome-stable $FLAGS |" \
      "$D"
fi
grep -c "render-node-override" "$D"    # 每个 Exec 入口 1 次,不随执行次数增长
```

无 sudo 就从宿主侧改 `/mnt/Droidspaces/<容器名>/usr/share/applications/google-chrome.desktop`。

> 为什么是这几个参数：解码帧经 **linux-dmabuf 协议**提交 → 必须 Wayland
> （X11 下解码器创建后一帧不解）；Chromium 只枚举 PCI 总线 DRM 设备，
> **ARM 平台设备必须用 `--render-node-override` 注入**；Wayland 与 Vulkan
> 硬性互斥需显式关闭。

## Firefox（H.264 / HEVC 都推荐）

### ② 写入硬解配置（自动定位真实 profile，整段复制执行）

```bash
P=~/.mozilla/firefox/$(grep -m1 '^Default=' ~/.mozilla/firefox/installs.ini | cut -d= -f2)
mkdir -p "$P"; touch "$P/user.js"
add() { grep -qxF "$1" "$P/user.js" || echo "$1" >> "$P/user.js"; }   # 幂等:已存在则跳过
add 'user_pref("media.ffmpeg.vaapi.enabled", true);'
add 'user_pref("media.hardware-video-decoding.force-enabled", true);'
add 'user_pref("media.gpu-process-decoding", true);'
add 'user_pref("media.rdd-ffmpeg.enabled", true);'
# 第五件:解码帧经 dmabuf 零拷贝纹理直进合成器 —— 缺了它硬解照跑,
# 但每帧都要拷回内存再做 CPU 软件转色,内容进程能吃掉半个到多个核心
add 'user_pref("media.vaapi-dmabuf-textures.enabled", true);'
awk '!(/^user_pref/ && seen[$0]++)' "$P/user.js" > "$P/user.js.tmp" && mv "$P/user.js.tmp" "$P/user.js"
grep -c '^user_pref' "$P/user.js"    # 每项应为 1
```

### ③ 关闭 RDD 解码进程沙箱（整段复制执行）

```bash
F=/usr/share/applications/firefox-esr.desktop
if ! sudo grep -q "MOZ_DISABLE_RDD_SANDBOX" "$F"; then   # 幂等:已配置则跳过
    sudo cp "$F" "$F.bak"
    sudo sed -i 's|^Exec=/usr/lib/firefox-esr/firefox-esr |Exec=env MOZ_DISABLE_RDD_SANDBOX=1 /usr/lib/firefox-esr/firefox-esr |' "$F"
fi
grep -c "MOZ_DISABLE_RDD_SANDBOX" "$F"    # 输出 >=1 即成功
```

重启 Firefox 生效。

## 验证

一键体检（无需 clone，直接下载运行）：

```bash
curl -fsSLO https://raw.githubusercontent.com/Re-s/droidspaces-media-decode/v0.3.4/tools/check-browser-vaapi.sh
bash check-browser-vaapi.sh
# 直连失败可走系统代理: curl --proxy socks5h://127.0.0.1:1080 ...
```

或手动确认 daemon 日志出现真实解码会话（宿主侧执行）：

```
adb shell 'su -c "tail -20 /data/local/Droidspaces/Logs/decode-daemon-tcp.log"'
# [N] 握手成功: video/hevc 1280x720 ...
# [N] 输出格式 1280x720 stride=1280 ...   ← 有这行才是真在硬解
```

## 已知问题

- **Chrome 播 HEVC 在线视频掉帧跳跃/绿屏**：anland 显示桥对 Chrome 存在
  呈现反馈缺失（平台层兼容缺陷，五种启动组合均复现），H.264 不受影响。
  **HEVC 视频请用 Firefox**，同机实测完美流畅。
- X11 模式下 Chrome 硬解完全不可用（0 NALU），不要尝试改回 X11。

## 文档导航

| 文档 | 内容 |
|---|---|
| [browser-vaapi-guide.md](browser-vaapi-guide.md) | 本指南完整版：参数原理、排障速查表、已知限制定量特征 |
| [README.md](../README.md) | 项目总览、架构、编译与部署 |
| [platform-integration-contract.md](platform-integration-contract.md) | 平台接入契约（⚠️ 已按 0.4.0 重写，bind mount 与 SELinux domain 两项要求已取消） |
| `check-browser-vaapi.sh`（见本页 Assets） | 浏览器硬解一键体检脚本 |
| `test_decode.py`（见本页 Assets） | 协议解析演示客户端（TCP 内联模式） |
| 编译产物：`decode-daemon` / `msm_drm_drv_video.so` / `dmd_watchdog-v0.3.4.zip` / `SHA256SUMS` | CI 构建的部署包，校验和见 SHA256SUMS |
| [CHANGELOG.md](../CHANGELOG.md) | 全版本更新日志 |

**Full Changelog**: https://github.com/Re-s/droidspaces-media-decode/blob/master/CHANGELOG.md
