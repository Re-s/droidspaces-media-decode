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

判据一律是与软解 **md5 逐字节一致**，而不是"能解出来"。除 1080p 外，
HEVC 的 720p、854x480（宽非 128 倍数）、640x360、720x1280（竖屏）与一条
1080p→720p→480p 切换流均已逐字节验证。

AV1 需要 `-DDMD_ENABLE_AV1` 才会声明，MPEG-2 需要 `-DDMD_ENABLE_MPEG2`。
这两者的现状、10bit 的固件限制、以及 HEVC 带 `st_ref_pic_set` 的码流为什么
只能回落软解，见 [`doc/upgrade-and-pitfalls.md`](doc/upgrade-and-pitfalls.md)。

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

不想装进系统目录，可以用 `LIBVA_DRIVERS_PATH` 指向 `.so` 所在目录。

## 自检

```sh
LIBVA_DRIVER_NAME=msm_drm ffmpeg -hwaccel vaapi \
  -hwaccel_output_format vaapi -i in.mp4 -f null -
```

`vainfo` 只证明驱动能加载、profile 列表是**静态声明**的，不证明能出帧；
验真实解码用上面这条命令。想确认硬件真的在跑，可以看 Venus 的内核实况
（用户态改不了这些值，解码期间 `mvs0_gdsc` 应为 `enabled`）：

```sh
for r in /sys/devices/platform/soc/*gdsc/regulator/regulator.*/; do
  [ "$(cat $r/name)" = mvs0_gdsc ] && echo "mvs0_gdsc=$(cat $r/state)"; done
```

版本串形如 `0.4.6+<git短hash>`，工作区有未提交改动时带 `-dirty`。
浏览器把解码放在单独的 RDD/GPU 进程里，系统目录和 `LIBVA_DRIVERS_PATH`
可能各有一份 `.so`，靠这个后缀能确认实际 `dlopen` 的是哪一版。

多分辨率回归（**需要真机**，判据是硬解与软解 md5 逐字节一致）：

```sh
cd vaapi-driver/tests
FFMPEG=/path/to/ffmpeg DRIVER_DIR=../build ./regress_resolutions.sh test.hevc
```

### 环境变量

| 变量 | 默认 | 作用 |
|---|---|---|
| `DMD_VA_LOG=1` | 关 | 输出驱动日志到 stderr。排查任何问题的第一步。 |
| `DMD_TRACE_ORDER=1` | 关 | 打印 surface 提交/收帧的配对时序。 |
| `DMD_HARVEST_EXPORTED_ONLY=0` | **开** | 关闭"仅对导出过 dmabuf 的 surface 收帧"。默认开启可省下每帧一次 memcpy（33.0 → 56.1 fps）。 |
| `DMD_NO_MAP_WAIT=1` | 关 | 取消 map 时的兜底等帧，让 ffmpeg 跑在 Chrome 的处境下。**测试用**。 |
| `DMD_CSD_DUMP=1` | 关 | 打印合成 SPS 的原始字节。改参数集合成逻辑时用。 |
| `DMD_VA_TOLERATE_MISSING=1` | 关 | 取不到帧也报成功。**纯诊断**，画面必然是错的。 |

## 浏览器

先确认后端能出帧（上面的 ffmpeg 自检），**这一步不通就别碰浏览器**。

### 快速配置

下面两段都是幂等的，重复执行不会叠加。不需要下载任何东西，直接粘进终端。

**Chrome** —— 给 `.desktop` 的每个 `Exec=` 注入必需参数：

```bash
D=/usr/share/applications/google-chrome.desktop
F="--ozone-platform=wayland --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist --enable-features=VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL"
grep -q render-node-override "$D" || {
  sudo cp "$D" "$D.bak"
  sudo sed -i "s|^\(Exec=[^ ]*\)|\1 $F|" "$D"
}
grep -c render-node-override "$D"   # 每个 Exec 行 1 次，不随执行次数增长
```

然后打开 `chrome://flags`，搜 `Vulkan`，设为 `Disabled`，重启浏览器。
这一步没有命令行等价物，原因见下方提示。

驱动没有 AV1。B 站等站点默认给 AV1 时 Chrome 会静默走软解（页面流畅、
驱动 0 配对帧），看起来像"配了没生效"。播放器里选 AVC / HEVC 清晰度。

**Firefox** —— 两步都要做，只写 `user.js` 不会生效。

第一步，给 `.desktop` 注入环境变量：

```bash
D=/usr/share/applications/firefox.desktop
grep -q MOZ_DISABLE_RDD_SANDBOX "$D" || {
  sudo cp "$D" "$D.bak"
  sudo sed -i 's|^Exec=|Exec=env MOZ_DISABLE_RDD_SANDBOX=1 LIBVA_DRIVER_NAME=msm_drm |' "$D"
}
grep -c MOZ_DISABLE_RDD_SANDBOX "$D"   # 每个 Exec 行 1 次
```

`MOZ_DISABLE_RDD_SANDBOX=1` 是必需的：Firefox 把解码放在 RDD 进程里，
它的沙箱会挡掉 `/dev/video32`，日志里表现为
`Sandbox: Couldn't open video device`。这一项和 `user.js` 里的
pref 是两回事 —— pref 只开 Firefox 自己的硬解开关，不解决设备访问。

第二步，写进每个 profile 的 `user.js`（先完全退出 Firefox）：

```bash
pkill -x firefox 2>/dev/null; sleep 1
for R in ~/.mozilla/firefox ~/.config/mozilla/firefox \
         ~/snap/firefox/common/.mozilla/firefox \
         ~/.var/app/org.mozilla.firefox/.mozilla/firefox; do
  [ -f "$R/profiles.ini" ] || continue
  sed -n 's/^Path=//p' "$R/profiles.ini" | while read -r p; do
    case "$p" in /*) U="$p/user.js" ;; *) U="$R/$p/user.js" ;; esac
    [ -d "$(dirname "$U")" ] || continue
    touch "$U"
    for k in \
      'user_pref("media.hardware-video-decoding.enabled", true);' \
      'user_pref("media.hardware-video-decoding.force-enabled", true);' \
      'user_pref("media.ffmpeg.vaapi.enabled", true);' \
      'user_pref("media.hevc.enabled", true);' \
      'user_pref("media.ffmpeg.vaapi.force-surface-zero-copy", 2);' \
      'user_pref("media.ffmpeg.disable-software-fallback", true);' \
      'user_pref("media.video-queue.hw-accel-size", 10);' \
      'user_pref("media.video-queue.default-size", 10);' \
      'user_pref("media.video-queue.send-to-compositor-size", 6);'
    do grep -qxF "$k" "$U" || printf '%s\n' "$k" >> "$U"; done
    echo "已配置 $U"
  done
done
```

profile 根目录要遍历这四处：不同安装方式（apt、XDG 新位置、snap、flatpak）
放在不同地方，同一台机器上也可能并存多份。只写 `~/.mozilla` 的话，
Firefox 实际用另一处时就会"配了但没生效"——页面能播、统计正常，实际走软解。

三处不能省：

`media.ffmpeg.disable-software-fallback` 是**唯一**能关掉硬解性能看门狗的
开关。FFmpegVideoDecoder 累计 16 帧 `decodeTime > frameDuration` 就打印
`HW decoding is slow, switching back to SW decode`，然后禁用硬解并重建
解码器。此前误把 `force-enabled` 当成这个开关 —— 154 源码里看门狗条件
只有 `IsDecodingSlow() && !media.ffmpeg.disable-software-fallback`，
`force-enabled` 只绕过 gfxInfo 黑名单，完全不参与判定。B 站 1080p 每个
解码器建立后立刻 `PORT_SETTINGS(INSUFFICIENT)` 重配，叠固件首帧滞后 4
个输入单元，看门狗在解码器初期最敏感，于是进入杀解码器 → Seek 回同一
PTS → 重建 → 再重配 → 再误判的死循环，页面表现为"播几秒卡在加载"。
实测同一视频：加上后 100s / 3min 的 HW slow、FATAL、Seek 全 0；3min
配对 4645 帧、送入 3099 收到 3099。代价是真正硬解损坏时不再自动退回软解。

`media.hardware-video-decoding.force-enabled` 仍建议开，作用是绕过
gfxInfo 黑名单，与看门狗无关。

三项 `media.video-queue.*`：1080p30 27Mbps 实测不加丢帧 14.25%，加上
降到 0.89%。

**验证**：最可靠的判据是驱动日志。带 `DMD_VA_LOG=1` 启动浏览器，
播一段视频，看有没有 `[v4l2] 会话就绪` 和 `配对: 帧`：

```bash
DMD_VA_LOG=1 firefox 2>&1 | grep -E '会话就绪|配对: 帧'
```

播放中另开一个终端看硬件门控（解码期间应为 `enabled`，空闲时 `disabled`，
所以要在播放时查）：

```bash
for r in /sys/devices/platform/soc/*gdsc/regulator/regulator.*/; do
  [ "$(cat $r/name)" = mvs0_gdsc ] && echo "mvs0_gdsc=$(cat $r/state)"; done
```

不要只看播放是否流畅，也不要用 `vainfo` —— 前者软解一样流畅，
后者只证明驱动能加载。

**还原**：

```bash
# Chrome
sudo mv /usr/share/applications/google-chrome.desktop.bak \
        /usr/share/applications/google-chrome.desktop
# Firefox 的 .desktop
sudo mv /usr/share/applications/firefox.desktop.bak \
        /usr/share/applications/firefox.desktop
# Firefox：删掉上面那些 user_pref 行（四处都扫，和安装对称）
pkill -x firefox 2>/dev/null; sleep 1
for R in ~/.mozilla/firefox ~/.config/mozilla/firefox \
         ~/snap/firefox/common/.mozilla/firefox \
         ~/.var/app/org.mozilla.firefox/.mozilla/firefox; do
  for U in "$R"/*/user.js; do
    [ -f "$U" ] && sed -i '/media\.\(hardware-video-decoding\|ffmpeg\|hevc\|video-queue\)/d' "$U"
  done
done
```

> ⚠️ 这条还原按 pref 名匹配删除，会连带删掉你自己写的同类 pref
> （例如 `media.hardware-video-decoding.force-enabled`）。在意的话先
> `cp user.js user.js.bak`，或改用 `tools/configure-firefox-vaapi.sh --uninstall`
> ——那个脚本用标记块界定范围，只删自己写的部分。

仓库里也有等价脚本，多了 `--verify` / `--uninstall` 和冲突检测：
`tools/configure-chrome-vaapi.sh`、`tools/configure-firefox-vaapi.sh`、
`tools/check-browser-vaapi.sh`（也附在 [Release](../../releases/latest) 里）。

Firefox 脚本会自动找 profile，覆盖 `~/.mozilla/firefox`、
`~/.config/mozilla/firefox`（较新版本遵循 XDG 后的位置）、snap 与 flatpak
四处，每个找到的 profile 都写一遍；用 `FIREFOX_HOME` 可指定只处理某一个。
已有其它工具管理的 profile（`user.js` 里带 `>>> DroidSpaces ... >>>` 块）
不会整段覆盖，但仍会补上对方不管的
`media.ffmpeg.disable-software-fallback` —— 缺了它 B 站 1080p 会播几秒
卡在加载。

驱动目录优先取系统 dri 目录，用 `DMD_DRIVER_DIR` 可指定别处 ——
`--verify` 第一行会显示实际选中的是哪一个，升级后建议瞄一眼，
确认不是遗留的旧版本。

Chrome 脚本在容器内没有 sudo 时，会自动改用
`~/.local/share/applications` 下的用户级副本。

> ⚠️ **Chrome 有一步脚本代劳不了**：打开 `chrome://flags`，把 **Vulkan** 设为
> `Disabled`，重启浏览器。ozone wayland 与 Vulkan 硬性冲突，而这一项没有
> 可用的命令行开关 —— `--disable-vulkan` 这个开关在 Chrome 里根本不存在，
> `--disable-features=Vulkan`、`--use-vulkan=disabled` 实测同样无效。
> 详见[指南第 2.5 节](doc/browser-vaapi-guide.md)。

### 手动配置

Chrome 必须用 Wayland 模式，X11 下解码器能创建但一帧不走：

```sh
google-chrome \
  --ozone-platform=wayland \
  --render-node-override=/dev/dri/renderD128 \
  --ignore-gpu-blocklist \
  --enable-features="VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL"
```

Firefox 把 pref 写进 profile 的 `user.js`。三项 `media.video-queue.*`
不是可选项（1080p30 27Mbps 不加丢帧 14.25%，加上 0.89%）；
`media.ffmpeg.disable-software-fallback` 也不是可选项（关掉硬解看门狗，
否则 B 站 1080p 播几秒卡在加载）。完整清单用上面的粘贴块，不要手抄漏项。

每个参数为什么不能省、Firefox 完整 pref 与其特有的坑、验证三步、实时监视、
排障速查表，都在 [`doc/browser-vaapi-guide.md`](doc/browser-vaapi-guide.md)。

## 旧版本的坑

如果你不是在用最新版，先看 [`doc/upgrade-and-pitfalls.md`](doc/upgrade-and-pitfalls.md)：

| 你在用 | 症状 | 修复版本 |
|---|---|---|
| 0.4.1 及更早 | `-f null -` 自检挂死 | 0.4.2 |
| 0.4.2 及更早 | 播非 1080p 绿屏（1080p 正常） | 0.4.3 |
| 0.4.3 及更早 | 浏览器拖进度条 / 切清晰度卡数秒 | 0.4.4 |
| 0.4.5 及更早 | Chrome 硬解开头几帧顺序错乱 | 0.4.6 |

那份文档也记录了不随版本变化的能力边界（HEVC `st_ref_pic_set`、10bit、
AV1 与 MPEG-2 的现状）。


## 性能与能效

**"硬解"只指用专用硬件解码，不代表省电或省 CPU。** 实测单路 1080p 硬解的
整机功耗比软解略高（6.12 W vs 5.74 W），墙钟也不占优；真实价值在并发，
以及把 CPU 让给其它工作。

> ⚠️ 这组数字测的是 0.3.x 的 daemon 架构，V4L2 直通的能效与吞吐**尚未重新测量**。
> 定性结论预计仍成立，具体数字应视为待复测。0.3.x 的性能数字（1080p 峰值
> 194 fps 等）与当前架构无关，已全部作废。

完整数据、口径说明与两个不可引用的错数字见
[`doc/performance-and-roadmap.md`](doc/performance-and-roadmap.md)。

## 文档

| 文档 | 内容 |
|---|---|
| [`vaapi-driver/README.md`](vaapi-driver/README.md) | 驱动内部实现、V4L2 协商细节 |
| [`doc/av1-v4l2-status.md`](doc/av1-v4l2-status.md) | AV1 遗留缺陷的完整测绘与方法教训 |
| [`doc/platform-integration-contract.md`](doc/platform-integration-contract.md) | 平台侧需要提供什么（0.4.0 只剩设备节点权限一项） |
| [`doc/browser-vaapi-guide.md`](doc/browser-vaapi-guide.md) | 浏览器接入：完整参数、固化脚本、验证与排障 |
| [`doc/upgrade-and-pitfalls.md`](doc/upgrade-and-pitfalls.md) | 各版本已知缺陷与升级理由、编解码器能力边界 |
| [`doc/performance-and-roadmap.md`](doc/performance-and-roadmap.md) | 性能与能效实测数据 |
| [`doc/verified-platform-facts.md`](doc/verified-platform-facts.md) | 平台取证事实 |
| [`doc/why-not-v4l2.md`](doc/why-not-v4l2.md) | ⚠️ 结论已被推翻的历史文档，保留其取证过程与方法教训 |
| [`CHANGELOG.md`](CHANGELOG.md) | 版本历史 |

## 测试设备

小米平板 5（`nabu`），骁龙 855（SM8150 / Adreno 640），Android 13，内核 `4.14.336`；
容器为 DroidSpaces 内 Debian 13 aarch64。

AV1 需要更新的硬件，在另一台设备上验证。

## 内核兼容

驱动按 `REQBUFS` 结果自动分叉，两类内核都支持，无需配置：

- **老内核**（nabu / kernel 4.14）：`USERPTR` 名义内存 + 私有 `PORT_SETTINGS`
  事件 + `SESSION_CONTINUE`
- **新内核**（Android 12+ / kernel 5.x+，实测 Android 15 / kernel 6.6）：
  标准 V4L2 stateful 语义，只接受 `DMABUF`、发标准 `SOURCE_CHANGE`、
  在事件后协商 CAPTURE

新内核适配自 0.4.5 起（上游 PR #4）。两条路径的差异细节见
[`vaapi-driver/README.md`](vaapi-driver/README.md) 与
[`CHANGELOG.md`](CHANGELOG.md) 的 v0.4.5。

## 许可

见 [LICENSE](LICENSE)。
