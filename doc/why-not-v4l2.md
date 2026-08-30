# 为什么不直接用容器内的 V4L2 解码器（结论已被推翻，见下）

> # ⚠️ 本文的最终结论是错的
>
> **2026-08-29 更新：V4L2 直通已经实现并在真机验证通过，成为 0.4.0 的唯一架构。**
>
> 本文原判"不可用（BLOCKED_HARD）、置信度 certain"。实际结果：
>
> | 编解码器 | 现状 |
> |---|---|
> | H.264 | 300/300 帧，md5 与软解逐字节一致 |
> | HEVC | 12/12 帧，md5 与软解逐字节一致 |
> | VP9 Profile 0 | 50/50 帧，md5 与软解逐字节一致 |
> | AV1 | 帧数与 dav1d 一致，像素仍未通过 |
>
> **错在哪：** 本文正确地测出"喂数据对固件毫无贡献"，也正确地定位到
> 会话状态机没到 `START_DONE`，但把它归因为**不可逾越**。
> 真正的原因是协商方式不对，而非能力不存在——
> msm_vidc 是**有状态**解码器，要求两段式协商：
>
> 1. 先只配 OUTPUT 格式并 STREAMON，喂入第一个序列头
> 2. 等 `V4L2_EVENT_SOURCE_CHANGE` 事件
> 3. **然后**才配 CAPTURE 格式（必须显式设 NV12，默认是 QCOM `Q08C`）并 STREAMON
>
> 本文测的是"两个队列一起 STREAMON 再喂数据"——那样固件确实永远到不了
> `START_DONE`，零帧输出，且每一步都返回成功。另需 dma-heap DMABUF：
> mmap 与 query offset 都会失败（`ENODEV`）。
>
> **方法教训（比技术结论更值钱）：** 本文做了源码级追踪、分阶段 IRQ 计数、
> SELinux 排除，证据链扎实，却仍给出了错误的判定。
> 原因是把"我这样调用不工作"写成了"这条路不通"——
> **排除了若干种失败原因，不等于穷尽了所有成功路径。**
> 置信度标成 `certain` 更是过头：它劝退了后续投入。
>
> ---
>
> ## ⚠️ 二次更正（0.4.1）：上面那段"正确做法"也是错的
>
> **2026-08-30：nabu（Snapdragon 860 / kernel 4.14）上跑通了，
> 但用的不是上面写的两段式协商。** 上面第 19-27 行的说法有三处不成立：
>
> **一、"等 `V4L2_EVENT_SOURCE_CHANGE`"永远等不到。**
> msm_vidc 从不发这个标准事件，它发的是厂商私有事件：
> `V4L2_EVENT_MSM_VIDC_START = V4L2_EVENT_PRIVATE_START + 0x1000 = 0x08001000`
> （厂商 `videodev2.h:2290-2305`），+2 是 `PORT_SETTINGS_CHANGED_SUFFICIENT`。
> 订阅错了类型，poll 就永远不返回 POLLPRI ——
> **这才是本文原始版本"喂数据对固件毫无贡献"现象的真正解释**，
> 不是固件没响应，是我们没在听正确的频道。
>
> **二、"不能两个队列一起 STREAMON"不成立。**
> `msm_vidc.c:1294-1302` 显示第一个 STREAMON 因对侧未 streaming 而
> **跳过** `start_streaming()`（假通过），第二个才承担全部校验。
> 所以"CAPTURE 先 STREAMON 就成功"是假象，失败只是被推迟。
> 正确做法是两侧都先配好，再让第二个 STREAMON 触发校验。
>
> **三、"另需 dma-heap DMABUF"不成立。**
> msm_vidc 只接受 `V4L2_MEMORY_USERPTR`
> （`q->io_modes = VB2_MMAP | VB2_USERPTR`，`msm_vidc.c:1548`），
> DMABUF 的 REQBUFS 恒 EINVAL。而 USERPTR 是名义值：
> `get_userptr` 是返回 `0xdeadbeef` 的桩函数（`msm_vidc.c:717-720`），
> dmabuf fd 必须写进 `plane.reserved[0]`（`msm_vidc.c:533-536`）。
>
> 真正缺的四项是：**SECONDARY 分流模式**（`0x00992016`，PRIMARY 下
> `start_streaming()` 恒 -EINVAL）、**私有事件订阅**、
> **`DECODER_CMD(cmd=5)` SESSION_CONTINUE**（`in_reconfig` 后固件停在等待态）、
> **`O_NONBLOCK`**（否则 `DQEVENT` 阻塞在 `v4l2_event_dequeue`）。
> 完整协议见 `vaapi-driver/src/v4l2_backend.h` 的头部注释与 CHANGELOG v0.4.1。
>
> **叠加的方法教训：** 第一次更正修对了结论、却把新的失败模式又写成了定论。
> 一份文档连续两版给出错误的"正确做法"，说明**在诊断手段受限时
> （本设备无 function tracer、无 kprobe、debugfs 挂不上、printk 提到 8
> 也没有 msm_vidc 输出），"我找到了原因"这句话本身就该降低置信度。**
> 真正管用的是逐项单一变量对照 + 读厂商源码，而不是从现象反推机制。
>
> 下文保留原样（除本标注外未删改），因为它的**探测结果表、源码定位、
> IRQ 数据仍然准确**，只是不该由此推出"不可用"。
>
> ## 补充（2026-08-29，第 85 轮）：本文对 nabu 的判断其实是对的
>
> 上面说"结论是错的"要再精确一层：**错的是推广，不是测量。**
>
> 在本文的测试设备（小米平板 5 / nabu，内核 4.14）上重测，
> 结论与本文完全一致 —— 而且换了缓冲类型也一样：
>
> | 步骤 | 结果 |
> |---|---|
> | `REQBUFS(OUTPUT, USERPTR)` | 成功 count=4（DMABUF/MMAP 均 EINVAL） |
> | `STREAMON(OUTPUT)` | 成功 |
> | QBUF 4 个单元共 512KB | 成功 |
> | 等 `V4L2_EVENT_SOURCE_CHANGE` | **永不到达** |
> | `G_FMT(CAPTURE)` | 仍是残留默认值 1920x1088 / Q128 |
> | IRQ 510 净增（等时长对照） | 空闲 50/10s，喂料 **49/12s** —— 无差别 |
>
> 也就是说本文"喂数据对固件毫无贡献"这个测量在 nabu 上**至今复现**。
>
> 真正的更正是：**这是设备限制，不是平台限制。** 在另一台有 dma-heap
> 的设备上，同样的两段式协商跑满 300 帧且像素与软解逐字节一致。
> 所以本文该说的是"nabu 走不通"，而它说成了"这条路走不通"。
>
> 顺带更正本文一个次要错误：它把原因归为 TrustZone / SELinux / netns，
> 并说"源码证明这些全部不成立"—— 后半句对，前半句的排查方向也对，
> 但漏了最简单的一项：**没有确认自己的协商顺序是否正确**。
> 当前实现见 `vaapi-driver/src/v4l2_backend.c`，
> AV1 的遗留问题见 `doc/av1-v4l2-status.md`。

---

一个自然的疑问：DroidSpaces 容器内 `/dev/video32` 就是高通 Venus 硬件解码器节点，
为什么还要绕一圈把码流送到 Android 侧的 MediaCodec？

答案是**实测过，这条路走不通**。本文记录取证结论，避免后续重复投入。

- 测试设备：小米平板 5（`nabu`），骁龙 855（SM8150 / Adreno 640），Android 13，内核 `4.14.336`
- 容器：DroidSpaces 内 Debian 13 aarch64，容器内 root，属 `droidspaces-gpu` 组
- 取证日期：2026-08-22 首轮；2026-08-28 定位到确切机制（共 11 轮）
- 判定：**不可用（BLOCKED_HARD）**，置信度 **certain**（源码级证据，见下）

> 本文 2026-08-28 重写。初版的症状描述、归因与收益判断均已被后续取证推翻，
> 保留下来的只有仍然成立的探测结果表。修正内容见文末「修订记录」。

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
| **双队列 `VIDIOC_STREAMON`** | **成功**（需先设私有 CID，见下） |
| Venus 固件状态 | dmesg 确认 `Brought out of reset`（宿主侧已加载） |

这台设备的 `msm_vidc` 是 **pre-M2M 的 downstream 实现**：`device_caps` 为
`0x04203000`，**缺** `V4L2_CAP_VIDEO_M2M_MPLANE`(0x4000) 与
`V4L2_CAP_VIDEO_M2M`(0x8000)。因此标准 M2M 用法不适用，必须按 Android 私有约定：

- 内存类型必须是 `V4L2_MEMORY_USERPTR`，且 ION dmabuf fd 要**同时**放进
  `m.userptr` 和 `reserved[0]`；匿名内存会让 `STREAMON` 失败
- 私有 CID `0x00992016`（split_mode）设 1，才能解锁双队列 `STREAMON`
- `VIDIOC_TRY_FMT` 返回 `ENOTTY`；`S_PARM` 是空实现（`G_PARM` 回读 0/0）
- `ENUM_FRAMESIZES` 报 96x96 ~ 8192x8192，step 1x1

满足这些之后，**两个队列的 `STREAMON` 都会返回成功**。

## 真正卡在哪里：缓冲被静默丢弃

`STREAMON` 成功之后，喂进去的码流**从未到达固件**，且没有任何错误返回。

拿到设备真实内核源码（`MiCode/Xiaomi_Kernel_OpenSource` 分支 `nabu-r-oss`）后，
在 `msm_vidc_common.c` 的 `msm_comm_qbuf()` 里找到确切原因（约 `:4458`）：

```c
if (inst->state != MSM_VIDC_START_DONE) {
    mbuf->flags |= MSM_VIDC_FLAG_DEFERRED;
    print_vidc_buffer(VIDC_DBG, "qbuf deferred", inst, mbuf);
    return 0;                    /* 静默丢弃，却返回成功 */
}

rc = msm_comm_scale_clocks_and_bus(inst);   /* 这之后才真正下发到 HFI */
```

会话状态机未达 `MSM_VIDC_START_DONE` 时，缓冲被标记 `DEFERRED` 后丢弃，
而函数**返回 0（成功）**。唯一的提示走 `VIDC_DBG` 级别，默认关闭。

这解释了为什么所有用户态尝试都"一切正常但零帧输出"——
内核在每一步都报成功。

### 分阶段 IRQ 实测吻合

用 `/proc/interrupts` 的 vidc 中断（IRQ **510**）逐阶段计数，每阶段严格 20 秒：

| 阶段 | 净增 IRQ/s | 说明 |
|---|---|---|
| 空闲基线 | 4.30 | 参照 |
| `open` | 0 | 无固件活动 |
| `+ S_FMT` | 0 | 无固件活动 |
| `+ REQBUFS + ION + QBUF` | 0 | 无固件活动 |
| `+ 双队列 STREAMON` | **+1.55** | 有响应 |
| `+ 喂 199 KB 码流` | **+1.65** | 只多 0.10，在噪声内 |
| 对照：MediaCodec 真解码 | **+6.20** | 真实解码的量级 |

**喂数据对固件毫无贡献**，与源码里的静默丢弃完全一致。

> ⚠️ 累计计数器必须按**相同时长**比较。此前两次用不等长窗口测同一指标，
> 一次高估（误判"固件有响应"）、一次低估（误判"固件完全不活动"），
> 结论都错。这是本轮最值钱的方法教训。

## 归因：不是权限问题

初版把原因归结为 TrustZone 授权、`subsys-pil-tz` client handle 或
pid namespace 隐式依赖。**源码证明这些全部不成立。**

整条调用链：

```
msm_vidc_streamon → vb2_streamon → msm_vidc_start_streaming
                  → start_streaming → msm_comm_try_state(START_DONE)
```

其中**不存在任何** uid / pid / pid namespace / SELinux / TrustZone / 进程名判断。

SELinux 也已实测排除：宿主 root（`u:r:ksu:s0`）与容器
（`u:r:droidspacesd:s0`）表现完全一致，无 avc denial。

所以这不是"容器权限不够"，而是**会话状态机没能到达 `START_DONE`**。

## 仍然未解的一环

`wait_for_state(..., HAL_SESSION_START_DONE)` 为什么既拿不到固件响应、
又不报错，目前不清楚。

已排除的解释：

- **不是 dmesg 被过滤**：`msm_vidc_debug` = `VIDC_ERR|VIDC_WARN|VIDC_FW`、
  `msm_vidc_debug_out` = `VIDC_OUT_PRINTK`，`dprintk` 门控已验证放行。
  所以"dmesg 里没有 `VIDC_ERR`"是可信的——它确实没报错。
  （注意 MediaCodec 成功解码时同样不产生 vidc 输出，所以这个通道只能排除
  "报错退出"，不能证明"固件在工作"。）
- **不是缓冲数量不匹配**：`verify_buffer_counts` 虽会在不匹配时失败，
  但 `msm_vidc_common.c:3214-3271` 显示三个计数都由内核在 `REQBUFS` 内部自行算出。
- **不是缺 META 端口**：新版 `qualcomm-linux/video-driver`（QCLinux）要求先对
  meta 端口 `STREAMON`，并定义 `INPUT_META_PLANE = V4L2_BUF_TYPE_META_OUTPUT(14)`、
  `OUTPUT_META_PLANE = V4L2_BUF_TYPE_META_CAPTURE(13)`。但本设备对这两个类型的
  `ENUM_FMT` / `G_FMT` / `REQBUFS` 全部返回 EINVAL——META 端口是新驱动才有的。

## 观测手段已穷尽

要继续查只能看内核内部状态，而四条路径全被**编译期配置**封死：

| 手段 | 状态 |
|---|---|
| kprobe | `# CONFIG_KPROBES is not set` |
| debugfs | `# CONFIG_DEBUG_FS is not set` —— `/sys/kernel/debug` **目录不存在**，`mount -t debugfs` 报 `No such file or directory`，宿主 root 也一样 |
| ftrace | `# CONFIG_FUNCTION_TRACER is not set`，`available_tracers` 只有 `nop` |
| tracepoint | `CONFIG_TRACING=y`，`msm_vidc_events:msm_vidc_common_state_change` 等事件存在，单事件 `enable` 能写入并回读为 1，但总开关 `tracing_on` 被静默拒绝（`dd` 报写入 2 字节，值仍为 0，dmesg 无提示）——厂商内核锁定 |

初版写的"debugfs 未挂载，若能在宿主侧开启 dprintk 则不排除解锁"这个保留余量
**不成立**：不是没挂载，是内核根本没编译进这个功能。

**剩下唯一的路是自行编译并刷入内核**（打开 `CONFIG_KPROBES` 或 `CONFIG_DEBUG_FS`），
代价远超收益（见下）。

## 收益重估：架构收益，不是性能收益

初版写「若将来这条路被解锁，收益是显著的（可省掉整个 TCP 代理层与每帧拷贝）」。
**这个前提已被实测否定。**

系统级口径实测（宿主 `/proc/stat`，真实内容 High profile 27.2 Mbps，满速 300 帧）：

| | 系统 jiffies | 墙钟 | 每帧 |
|---|---|---|---|
| 硬解（经 MediaCodec 代理） | 1731 | 3557 ms | **57.70 ms** |
| 软解（容器内 CPU） | 1722 | 2366 ms | **57.40 ms** |

差 0.5%，在噪声内；硬解墙钟反而慢 50%。整机功耗同向：
空闲 1208mA / 4.59W，软解 1524mA / 5.74W，硬解 **1630mA / 6.12W**。

**也就是说，V4L2 直通想省掉的那些开销（socket 往返、每帧 memcpy），
在系统级账面上并不存在。** 免掉 socket 是**架构收益**——少一个 daemon 进程、
少一层 IPC、少一次拷贝路径——**不是性能收益**。

这个量级的架构收益，不足以支撑"自编内核并长期维护"的代价。

## 附带发现

容器内以 `STREAMOFF` 清理时会触发内核 `WARN`（`__vb2_queue_cancel → msm_vidc_streamoff`），
与"会话从未真正建立"的判断一致。设备事后仍可正常打开和枚举，无残留损坏。

## 结论对架构的意义

**当前基于 MediaCodec 代理的架构是必要的，不是过度设计。**
Android 侧的 MediaCodec 能正常驱动同一个 Venus 硬件，是因为它经由
mediaserver / codec HAL 在宿主上下文中完成了会话建立，让状态机真正到达
`START_DONE`——这正是容器内直连缺失的那一环。

另外，代理架构还有一个直连方案不具备的性质：**与 Android 侧 MediaCodec 不互斥**。
实测占住 `/dev/video32` 并保持 `STREAMON` 25 秒期间，对侧 MediaCodec
照常以 6.89x 实时解完 300 帧。

## 修订记录

2026-08-28 重写。初版（2026-08-22）以下四处已被推翻：

| 初版说法 | 实况 |
|---|---|
| 测试设备骁龙 865 | **骁龙 855**（`ro.board.platform=msmnile`、`ro.soc.model=SM8150`、GPU `Adreno640v2`、CPU 最高 2.84 GHz 四项一致） |
| 第二个队列 `STREAMON` 恒定失败 | 设私有 CID `0x00992016`=1 后**双队列都成功**；卡点在其后的缓冲提交 |
| 归因 TrustZone / pid namespace 隐式依赖 | 源码证明整条 streamon 链**无任何身份检查**；真实机制是 `msm_comm_qbuf()` 的 `DEFERRED` 静默丢弃 |
| debugfs 未挂载，宿主侧开 dprintk 可能解锁 | `CONFIG_DEBUG_FS` 编译期未开，**目录不存在**，宿主 root 也挂不上；四条观测路径全被封死 |
| 打通收益显著（省 TCP 层与每帧拷贝） | 系统级 CPU 实测差 0.5%，**性能收益接近零**，仅剩架构收益 |

置信度从 high 提升为 **certain**：初版基于黑盒实测，本版有内核源码级证据。

完整取证记录见工作区文档（未随仓库分发）：
`A-1-v4l2-vdec-viability.md`（首轮 489 行）、
`BREAKTHROUGH-v4l2-venus.md`（11 轮全过程 1704 行，含全部撤回与更正）、
`ARCHIVE-2026-08-28.md`（归档摘要）。
