# 为什么不直接用容器内的 V4L2 解码器

一个自然的疑问：DroidSpaces 容器内 `/dev/video32` 就是高通 Venus 硬件解码器节点，
为什么还要绕一圈通过 TCP 把码流送到 Android 侧的 MediaCodec？

答案是**实测过，这条路走不通**。本文记录取证结论，避免后续重复投入。

- 测试设备：小米平板 5（`nabu`），骁龙 865，Android 13，内核 `4.14.336`
- 容器：DroidSpaces 内 Debian 13 aarch64，容器内 root，属 `droidspaces-gpu` 组
- 测试日期：2026-08-22
- 判定：**不可用（BLOCKED_HARD）**，置信度 high

## 驱动确实是活的

容器内对 `/dev/video32` 的探测大部分都成功，这也是这条路容易让人误以为可行的原因：

| 环节 | 结果 |
|------|------|
| `VIDIOC_QUERYCAP` | 成功，`driver=msm_vidc_driver` `card=msm_vidc_vdec` |
| `VIDIOC_ENUM_FMT`（OUTPUT） | 成功，支持 MPG2 / **H264** / HEVC / VP80 / VP90 |
| `VIDIOC_ENUM_FMT`（CAPTURE） | 成功，支持 NV12 / QP10 / Q128 / Q12A（含 UBWC 变体） |
| `VIDIOC_S_FMT` / `G_FMT` | 双队列均被接受 |
| `VIDIOC_G_CTRL` 最小缓冲数 | 成功，CAPTURE=12、OUTPUT=4 |
| ION 内存分配（system heap） | 成功 |
| `VIDIOC_QBUF` | 成功，OUTPUT 8/8、CAPTURE 16/16 |
| Venus 固件状态 | dmesg 确认 `Brought out of reset`（宿主侧已加载） |

## 卡在哪里

**第二个队列的 `VIDIOC_STREAMON` 恒定失败**，返回 `EINVAL` 或 `ENOTSUPP(524)`。

关键观察：

- 与队列顺序无关 —— 先 OUTPUT 或先 CAPTURE 都一样，**失败的总是第二个**
- 与分辨率、码流内容无关
- 编码器 `/dev/video33` 表现完全相同 —— 整个 `msm_vidc` 驱动的会话层对容器一致不可用
- 缓冲区数量已满足并超出驱动声明的最小值，排除资源不足
- 固件**从未消费任何码流缓冲**：`DQBUF` OUTPUT 零次，也从未产生 `PORT_CHANGED` 事件
- 6 个并发实例都能通过第一个 `STREAMON` —— 反证第一个 `STREAMON` 只是 vb2 层空操作，并未真正分配硬件资源

三条彼此独立的实现路线撞在同一个点上：

1. **原生 ffmpeg**：更早就失败了，`VIDIOC_TRY_FMT` 返回 `ENOTTY`，probe 阶段即判定 `v4l2 output format not supported`
2. **LD_PRELOAD shim**：逐个绕过三处 ABI 偏离（伪造 `TRY_FMT`、`YU12→NV12` 重映射、`REQBUFS` 由 MMAP 改 USERPTR）后，ffmpeg 前进到 `Using device /dev/video32`、格式协商成功、`REQBUFS got=16`，最终卡在 `QUERYBUF → ENOTTY`（该驱动没有 MMAP 模型）
3. **裸 python ioctl 完整实现**：绕过 ffmpeg 全部限制，按 Android 私有约定（`USERPTR` + `reserved[0]=ion_fd`）成功 QBUF 全部缓冲，仍停在同一个 `STREAMON`

所有变体的解码帧产出均为 **0**。软解基线正常（150 帧，rc=0），排除素材问题。

## 归因

Venus 会话建立（HFI `session_init`/`session_start`）依赖容器内不可得的宿主侧上下文——
TrustZone 授权、`subsys-pil-tz` client handle，或 downstream 驱动对调用者所在
pid namespace 的隐式依赖。这属于**内核↔固件会话握手层**的问题，不是用户态工具缺失：
`v4l-utils` 装得上（`apt-get --dry-run` rc=0），但装了也没用，因为 libv4l2 本就在系统内。

## 已知盲区

拿不到 `msm_vidc` 内核侧的确切拒绝原因：该驱动编译进内核而非模块，
`/sys/module/msm_vidc/parameters/` 不存在，debugfs 未挂载，因此 dprintk 无法在容器内开启。
失败瞬间的 dmesg 差分只有无关的 DSI 刷新率日志，`msm_vidc` 一行未打。

保留余量：若能在宿主 Android 侧开启 dprintk，或取到该 SoC 的 vendor HAL 源码，
不排除存在未公开的私有 `ioctl`/`S_CTRL` 前置调用可解锁会话。
因此判定为 high 而非 certain。若将来这条路被解锁，收益是显著的
（可省掉整个 TCP 代理层与每帧拷贝），值得复查。

## 附带发现

容器内以 `STREAMOFF` 清理时会触发内核 `WARN`（`__vb2_queue_cancel → msm_vidc_streamoff`），
与"会话从未真正建立"的判断一致。设备事后仍可正常打开和枚举，无残留损坏。

## 结论对架构的意义

**当前基于 MediaCodec + TCP 的代理架构是必要的，不是过度设计。**
Android 侧的 MediaCodec 能正常驱动同一个 Venus 硬件，是因为它经由
mediaserver / codec HAL 在宿主上下文中完成会话建立——这正是容器内缺失的那一环。

完整取证记录（489 行，含全部命令与原始输出）见调研报告
`A-1-v4l2-vdec-viability.md`（工作区文档，未随仓库分发）。
