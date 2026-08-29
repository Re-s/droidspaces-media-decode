# 版本归档

历史版本一览，用来快速判断该装哪个、以及某个行为是哪一版引入或改掉的。
每版的完整说明见 [CHANGELOG.md](../CHANGELOG.md) 与
[GitHub Releases](https://github.com/Re-s/droidspaces-media-decode/releases)。

**装哪个版本：用最新的 v0.3.7。** 下面的旧版只在需要回退或对照行为时才有用。

## 一览

| 版本 | 日期 | commit | 一句话 | 协议 | 建议 |
|---|---|---|---|---|---|
| **v0.3.7** | 2026-08-28 | `4bbc19c` | SHM 槽位耗尽导致真实丢帧，以及掩盖它的日志假警报 | 3 | **推荐** |
| v0.3.6 | 2026-08-27 | `3bf1856` | 模块 zip 自带 `decode-daemon`，刷入即完成安卓侧部署 | 3 | 可回退 |
| v0.3.5 | 2026-08-27 | `db92294` | 探活判据改为"能出帧"、零拷贝默认开启、Unix socket 吞吐修复 | 3 | 可回退 |
| v0.3.4 | 2026-08-26 | `cc1a897` | 浏览器硬解接入指南（Chrome / Firefox） | 3 | 见下 |
| v0.3.3 | 2026-08-25 | `3bd3be0` | endpoint inode 校验，消灭"连着但其实是死 socket" | 2→3 | — |
| v0.3.2 | 2026-08-25 | `445070c` | 日志修正与结论订正，`src/` 零变更 | 2 | — |
| v0.3.1 | 2026-08-24 | `ee2d33d` | daemon 稳定性修复 | 2 | — |
| v0.3.0 | 2026-08-24 | `a93721e` | 新增路径式 Unix socket 传输通道 | 2 | — |
| v0.2.0 | 2026-08-23 | `7300369` | Chrome / Chromium 硬解支持 | 1 | — |
| v0.1.0 | 2026-08-23 | `62597bf` | 首发：容器内经标准 VA-API 用宿主 MediaCodec | 1 | — |

> v0.3.4 在 GitHub 上标为 pre-release，因为它的主体是**接入文档**而非代码变更。
> 想照它配浏览器请优先看 [`browser-vaapi-guide.md`](browser-vaapi-guide.md)，
> 那份会随主线更新。

## 该不该回退

回退到 v0.3.6 或更早，需要知道你会失去什么：

| 退到 | 会重新遇到 |
|---|---|
| v0.3.6 及更早 | **SHM 槽位耗尽丢帧**：4K + 慢消费者场景 10/10 触发，300 帧只到 238。槽位等待硬编码 1000 ms，短于客户端 5000 ms 超时，daemon 会在客户端还愿意等时先杀会话 |
| v0.3.5 及更早 | 新设备需手工部署安卓侧 `decode-daemon`，模块 zip 不自带 |
| v0.3.4 及更早 | 探活只判"能握手"不判"能出帧"；零拷贝需手工开启；Unix socket 有吞吐塌陷 |
| v0.3.2 及更早 | 无 endpoint inode 校验，可能连上一个已死的 socket 却以为连接正常 |
| v0.2.x 及更早 | 只能走 TCP `127.0.0.1:20003`，依赖容器与宿主共享 net namespace。NAT 型容器用不了 |

**协议兼容**：v0.3.3 起握手版本为 3，daemon 接受区间（2 和 3 都收）。
v0.3.2 及更早是严格相等判定，所以**新客户端连旧 daemon 会被拒**（回 `status=1`），
反之新 daemon 可以接旧客户端。混用时请升级 daemon 一侧。

## 产物说明

每个 release 的资产：

| 文件 | 装到哪 |
|---|---|
| `msm_drm_drv_video.so` | 容器内 `/usr/lib/aarch64-linux-gnu/dri/`，名字不可改（libva 只找这一个文件，无 fallback） |
| `decode-daemon` | Android 侧，v0.3.6 起已打进模块 zip，不用单独下 |
| `dmd_watchdog-<版本>.zip` | KernelSU / Magisk 模块，v0.3.3 起提供 |
| `SHA256SUMS` | 校验用，**装之前先核对** |

早期版本（v0.1.0 / v0.2.0）的 daemon 叫 `decode-daemon-aarch64`。

校验：

```bash
sha256sum -c SHA256SUMS
```

## 一件历史遗留

**v0.3.3 到 v0.3.7 的 `msm_drm_drv_video.so`，`vainfo` 报出的
`Driver version` 全都是 `0.3.3`** —— 包括最新的 v0.3.7。
`DMD_DRIVER_VERSION` 常量忘了跟着发版更新，一直落后 4 个版本。

所以**别用 `vainfo` 的版本号判断你装的是哪一版**，用 `SHA256SUMS` 对。

该常量已在 v0.3.7 发布**之后**修正（提交 `f7a8232`），所以下一个 release
才会报出正确版本。已装 v0.3.7 的不必重装 —— 只是显示的版本号不对，
解码行为不受影响。

根因是 `vaapi-driver/Makefile` 当时不跟踪头文件依赖，改了 `driver.h` 后
`make` 认为无需重建，产物里仍是旧串。这个坑本身也已修（加 `-MMD -MP`）。
