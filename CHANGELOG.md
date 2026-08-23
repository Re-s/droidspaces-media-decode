# 更新日志

## v0.2.0

新增 Chrome / Chromium 硬解支持，修复三个缺陷。向后兼容，协议与 v0.1.0 一致。

### 新增：Chrome / Chromium 可用

v0.1.0 发布时 Chrome 完全用不了硬解。两处驱动缺陷各自都足以让它失败，
而 ffmpeg 与 Firefox 都不会触发 —— 这也是它们一直没暴露的原因。

**1. `vaQueryConfigAttributes` 必须声明驱动能力，不能回显入参**

原实现把 `vaCreateConfig` 存下的 attribs 抄回去。ffmpeg 与 Firefox 建 config
时自己就传了 `RTFormat`，回显恰好等于真值；Chrome **不传任何属性**，再调本
函数查询驱动支持什么，于是拿到 `num_attribs=0`、读不到 `VAConfigAttribRTFormat`，
`FillProfileInfo_Locked` 判定六个 profile 全部不可用：

```
FillProfileInfo_Locked failed for va_profile VAProfileH264ConstrainedBaseline
（H264 Main/High、HEVCMain、VP9Profile0、VP8 同样失败）
```

VA-API 的语义是"驱动声明自己的能力"，回显是错的。现在无论入参如何，
`RTFormat` 一定出现在返回集里。

**2. 收帧不能只发生在 `vaSyncSurface` 里**

Chrome **从不调 `vaSyncSurface`**（实测该调用计数为 0），而是大批提交后靠
`vaExportSurfaceHandle` 拿 dmabuf。驱动原先只在 `SyncSurface` 里收帧，于是
待解码队列迅速填满，`vaEndPicture` 返回 `OPERATION_FAILED`，Chrome 判定硬解
不可用并断开连接。

时间线可以确证这是死锁而非容量问题：队列满那一刻 Chrome 立刻报错，而全部
50 次配对都发生在**失败之后的 `DestroyContext` 排空阶段** —— 播放期间帧从
没被消费。Chrome 等 surface 就绪 → 就绪要收帧 → 收帧被队列满堵住。

现改为队列满时收**一帧**腾出空位（上限 200ms）。队列满是背压不是错误。

### 修复

- **HEVC 拒绝路径内存泄漏**：`dmd_hevc_can_build` 失败时未释放已分配的
  码流缓冲、未回滚 `pending_count`。触发条件是 `num_short_term_ref_pic_sets > 0`
  的码流（会回落软解），每帧泄漏一个单元的字节数。
- **daemon 静默丢弃 NALU**：取输入缓冲失败时原本只记日志后 `continue`，
  等于丢掉一个 NALU —— 丢任何一个 VCL 都会毁掉后续参考帧链。现区分
  `TRY_AGAIN_LATER`（背压，重试）与真错误码。该路径同样只有 Chrome 会触发。
- **`send_all` 补 errno**：原本无法区分"对端关闭"与其他写失败。

### 文档修正

- `media.ffmpeg.vaapi.enabled` 在 Firefox 137 前后已被移除，140 ESR 的 libxul
  里不存在这个 pref，设了无效。单变量实测：删掉后硬解照常工作（872 帧、
  零软解回落）。真正起作用的是 `media.hardware-video-decoding.force-enabled`。
- 驱动 README 新增「消费者契约差异」对照表，记录 ffmpeg / Firefox / Chrome
  在传属性、调 `SyncSurface`、取帧方式上的三种不同用法 —— 只测一个会漏掉
  另两个的坑。
- 补全探针局限标注：`probe_chrome_pattern.c` 说明它不发参数集（所以 Sync
  失败数偏高属预期）、`probe_drain.c` 说明它只数帧数不看画面。

### 验证

| 项目 | 结果 |
|---|---|
| 十二条流逐字节一致 | 12/12（H.264 六条含 long3000、HEVC 五条含 4K/long1500、VP8/VP9） |
| Firefox | 842 帧、0 黑帧、30.1 fps（墙钟） |
| Chrome 151 + HEVC | 1062 帧配对、29.5 fps（墙钟）、队列满 0、`vaEndPicture failed` 0 |
| 并发 | 8 路并发（含 4K）逐字节一致，零排空零重建 |
| 编译警告 | 0 |

### 已知限制

v0.1.0 的已知限制全部仍然成立（HEVC `st_ref_pic_set`、`MOZ_DISABLE_RDD_SANDBOX`、
PPS `num_ref_idx` 振荡、daemon 未持久化、无鉴权）。

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
