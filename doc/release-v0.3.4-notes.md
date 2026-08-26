# v0.3.4 — 浏览器硬解接入：Chrome / Firefox 完整配置方法

把 Android MediaCodec 硬解码接到容器浏览器的全部步骤。以下命令在
**DroidSpaces 容器终端**里整段复制执行即可，无需手工编辑任何文件。
实测环境：nabu（SD855）+ Debian 13 + decode-daemon TCP 模式。

**前置**：daemon 已部署并监听 `127.0.0.1:20003`，`vainfo` 能列出 profile。

---

## Chrome（H.264 完美；HEVC 见"已知问题"）

### ① 固化到桌面图标（整段复制执行）

```bash
D=/usr/share/applications/google-chrome.desktop
sudo cp "$D" "$D.bak"
FLAGS="--ozone-platform=wayland --disable-vulkan --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist --enable-features=VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL"
sudo sed -i \
  -e "s|^Exec=/usr/bin/google-chrome-stable $|Exec=/usr/bin/google-chrome-stable $FLAGS %U|" \
  -e "s|^Exec=/usr/bin/google-chrome-stable |Exec=/usr/bin/google-chrome-stable $FLAGS |" \
  "$D"
grep -c "render-node-override" "$D"    # 输出 >=2 即成功,从图标启动即可
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
mkdir -p "$P"
cat >> "$P/user.js" <<'EOF'
user_pref("media.ffmpeg.vaapi.enabled", true);
user_pref("media.hardware-video-decoding.force-enabled", true);
user_pref("media.gpu-process-decoding", true);
user_pref("media.rdd-ffmpeg.enabled", true);
EOF
echo "已写入: $P/user.js" && tail -4 "$P/user.js"
```

### ③ 关闭 RDD 解码进程沙箱（整段复制执行）

```bash
F=/usr/share/applications/firefox-esr.desktop
sudo cp "$F" "$F.bak"
sudo sed -i 's|^Exec=/usr/lib/firefox-esr/firefox-esr |Exec=env MOZ_DISABLE_RDD_SANDBOX=1 /usr/lib/firefox-esr/firefox-esr |' "$F"
grep -c "MOZ_DISABLE_RDD_SANDBOX" "$F"    # 输出 >=1 即成功
```

重启 Firefox 生效。

## 验证

一键体检：

```bash
bash tools/check-browser-vaapi.sh
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
| [doc/browser-vaapi-guide.md](doc/browser-vaapi-guide.md) | 本指南完整版：参数原理、排障速查表、已知限制定量特征 |
| [README.md](README.md) | 项目总览、架构、编译与部署 |
| [doc/platform-integration-contract.md](doc/platform-integration-contract.md) | 平台接入契约（bind mount / SELinux domain / renderD128 透传） |
| [tools/check-browser-vaapi.sh](tools/check-browser-vaapi.sh) | 浏览器硬解一键体检脚本 |
| [CHANGELOG.md](CHANGELOG.md) | 全版本更新日志 |

**Full Changelog**: https://github.com/Re-s/droidspaces-media-decode/blob/master/CHANGELOG.md
