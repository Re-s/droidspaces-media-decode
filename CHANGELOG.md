# 更新日志

## v0.1.0

首个正式版本。在 DroidSpaces 容器（Debian 13 aarch64）里通过标准 VA-API
使用 Android 宿主的 MediaCodec 硬件解码能力，应用无需改动。

### 支持的编解码器

| 编解码器 | Profile | 状态 |
|---|---|---|
| H.264 | ConstrainedBaseline / Main / High | 已验证 |
| HEVC | Main | 已验证 |
| VP9 | Profile 0 | 已验证 |
| VP8 | — | 已验证 |

高位深未验证，故不声明。

### 组成

- **Android 侧** `decode-daemon`：aarch64 原生程序，NDK MediaCodec API，
  监听 TCP `127.0.0.1:20003`
- **容器侧** VA-API 驱动：`msm_drm_drv_video.so`，装到
  `/usr/lib/aarch64-linux-gnu/dri/`

两侧用自定义 TCP 协议通信，带能力位协商（旧版能力位恒为 0，客户端向后兼容）。

### 验证

全部以**逐字节比对软解基线**为判定标准（只数帧数会误判，本项目为此栽过三次）：

- H.264 / VP8 / VP9 六条流：逐字节一致
- HEVC 五条流（含 4K、1500 帧长流）：逐字节一致
- H.264 长流 3000 帧：逐字节一致
- seek（10/25/40/55/80 秒）、并发三实例：一致
- Firefox 播放：H.264 **736 帧 0 黑帧 30.7 fps**、HEVC 697 帧 30.3 fps，
  零软解回落
- 零编译警告

性能：1080p 峰值 194 fps、4K 峰值 82 fps。

### 关键实现决策

- **按输入单元序号精确配对**：daemon 把每个输入单元的序号回传
  （`CAP_FRAME_PTS`），驱动据此配对 surface，与输出顺序完全解耦。
  早期用编译期常量声明输出顺序，两侧不一致就画面错位且不报错，该常量已删除。
- **`vendor.qti-ext-dec-picture-order.enable=1`**：把输出滞后从 4 降到 1。
  逐键实测只有这个键有效。
- **HEVC 参数集合成**：VA-API 不提供原始 VPS/SPS/PPS，需从 pic param 反向合成。

### 已知限制

- **HEVC `st_ref_pic_set` 码流不支持**：SPS 里 `num_short_term_ref_pic_sets > 0`
  时无法重建（VA-API 只给数量不给内容），驱动返回
  `VA_STATUS_ERROR_UNIMPLEMENTED` 让上层回落软解。x265 默认不产生这类码流。
- **Firefox 需要 `MOZ_DISABLE_RDD_SANDBOX=1`**：这削弱了 RDD 沙箱隔离。
  该变量的确切必要性**尚未查清** —— 实测在沙箱真正启用
  （`MOZ_DISABLE_RDD_SANDBOX=0`）时 TCP 连接依然成功、能跑 713 帧，
  所以它可能并非必需。待查清后可能直接去掉。
- **PPS `num_ref_idx` 默认值会振荡**：驱动对同一码流会发出多种 PPS 变体。
  看着丑，但**重送在承担纠错作用** —— 曾尝试收敛成单一份，结果
  `long3000.h264` 只解出 15/1323 帧。根因是 VA-API 只提供每个 slice 的
  生效值（带 `override_flag` 的与默认值无关），无法在首个 slice 之前
  确定真实默认值。保持现状。
- **daemon 未持久化**：需手工推送并启动，设备重启后丢失。生产部署由
  DroidSpace 平台托管，接入契约尚未实现。
- **无鉴权**：daemon 只绑 loopback，但容器与 Android 共享 net namespace，
  loopback **不构成隔离边界**。不要在多租户或不可信 App 环境下使用。

### 传输方式

当前用 TCP loopback。路径型 Unix socket 因 mount namespace 独立而不可用，
但 **abstract socket 可行**（属 net namespace，双向可见）——
共享内存通道（`DMD_XFER_SHM`）已在用它传递 memfd，实测跨边界成功且
解码结果与软解逐字节一致。后续可把控制通道也迁过去，无需共享挂载点。

注意 `DMD_XFER_SHM` 目前**默认关闭**（`decode.c` 里 `want_shm = 0`），
即浏览器路径从未走过它，其在 Firefox 沙箱下的表现未验证。
