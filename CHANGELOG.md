# 更新日志

## v0.4.2

**新增 VP8 硬解；修复消费者不逐帧取帧时的 CAPTURE 背压死锁。**
10bit 与 MPEG-2 经实测判定受固件限制，实现保留在编译开关后但不声明。

测试环境：nabu（Xiaomi Pad 5 / Snapdragon 855 / kernel 4.14），
Ubuntu 26.04 aarch64 容器，libva 2.23，ffmpeg 8.0.1。
全部结论均以 `ffmpeg` 实测帧数 + 与软解的 md5 逐字节比对为判据。

### 🐞 修复：CAPTURE 背压死锁（消费者不逐帧同步时挂死）

**症状**：`ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -f null -`
在第 26 帧后挂死，报 `internal decoding error`，40s 超时退出。
同一条流改用 `-f rawvideo` 落盘则 150/150 正常。

**根因不是 surface 重复入队**（那是第一轮的误判，按此修改后错误反而增多）。
真正的机制是两个队列容量不匹配：

- session 的待取帧队列容量 = CAPTURE 缓冲数 = `DMD_V4L2_MAX_CAP` (24)
- 驱动侧待配对队列的背压阈值 = `DMD_MAX_SURFACES` (64)

队列里每一帧都扣着一个未归还的 CAPTURE 缓冲。占满 24 时 msm_vidc 无处写帧、
连输入缓冲也停止回收，`send_unit` 陷入永久背压 —— 而 `pending_count >= 64`
这个条件 **永远等不到触发**，保护形同虚设。

实测日志（36 次 EndPicture 提交、0 次取帧）：

```
[dmd-v4l2] 背压超时: pend=24/24 OUTPUT在驱动=8/8 CAPTURE在驱动=0/24
```

落盘路径侥幸不触发，是因为它每帧立即 `vaSyncSurface`，队列从不积压。
**Chrome 属于不逐帧同步的模式**（靠 `vaExportSurfaceHandle` 批量取 dmabuf），
因此很可能同样受影响。

**修法**：触发条件改为按 session 水位判断（到容量 3/4 开始排空，
降回一半即停），新增 `dmd_session_frames_pending()` /
`dmd_session_pending_capacity()` 供驱动侧查询。

排空量是关键权衡，两个方向都验证过：只收 1 帧救不回来（收 1 压 1，水位不降）；
循环收光会替消费者把帧全收走（仓库既有注释记录：配对 1651 次而 Chrome
只导出 24 帧，帧率反跌到 1 fps）。留一半队列深度给消费者是折中。

**效果**：40s 挂死 → 3s 正常退出，零背压超时、零解码错误。

### ✨ 新增：VP8 硬解

`VAProfileVP8Version0_3` / `VAEntrypointVLD`，实测 **90/90 帧，
md5 与软解逐字节一致**。

此前不声明的两条理由 **都不成立**：

- "msm_vidc 的 V4L2 层没有 VP80 格式" —— 错。`VIDIOC_ENUM_FMT` 实测
  `/dev/video32` 的 OUTPUT 侧列出 `MPG2 H264 HEVC VP80 VP90`，VP80 在列。
- "驱动侧缺 RFC 6386 §9.1 的码流重建" —— 也不成立。`decode.c` 的
  `vp8_build_frame()` 早已实现（frame tag + key frame 的 start code 与尺寸），
  参数收集与 `build_unit` 的分派分支都在位。

缺的只是三道闸门：`profiles.c` 的声明与映射、`codec_to_fourcc` 的 VP80 一项。
`DMD_V4L2_CODEC_VP8 = 3` 就是它原本的编号，不是新占号。

### 🔍 10bit（HEVC Main10 / VP9 Profile2）：固件限制，不声明

固件 **能识别** Main10（`PORT_SETTINGS` 上报 `bitdepth=1`），也接受
QP10 / Q12A 格式设置（CAPTURE 缓冲从 3137536 翻到 6270976 字节）。

**顺带修掉一个真实 bug**：CAPTURE stride 的下限按 1 字节/像素计算
（`if (d->stride < d->w)`），10bit 需要 2 字节。实测固件期望 stride=3840
（`3840*1088*3/2 = 6266880`，与它给的 sizeimage 只差对齐余量），
按 8bit 钳成 1920 时几何不符。已改为按 fourcc 判断每像素字节数。

但即使 stride 正确，固件仍持续报 `PORT_SETTINGS(INSUFFICIENT)`、
**从不给 SUFFICIENT**，送入 8 单元收 0 帧。尝试在收到 INSUFFICIENT 后
真正重协商 CAPTURE（STREAMOFF + REQBUFS(0) + 重配）会立刻触发 `SYS_ERROR`，
印证了 v0.4.1 记录的"STREAMOFF 把 state 打回 `MSM_VIDC_START_DONE` 以下"。

判定为平台限制。探测代码保留在 `DMD_PROBE_10BIT` / `DMD_PROBE_10BIT_RECFG`
（运行时）与 `-DDMD_PROBE_10BIT_PROFILES`（编译期）开关后，发布版不声明 ——
上层像素路径全是 8bit NV12 假设，声明了会输出错画面。

### 🚧 MPEG-2：合成逐字节正确，但固件 SYS_ERROR

新增 `mpeg2_bitstream.c`：合成 sequence_header + sequence_extension +
GOP header + picture_header + picture_coding_extension。
VA-API 只在 slice data 里给 slice 层字节，帧级与序列级头全被拆进了
`VAPictureParameterBufferMPEG2` 与 `VAIQMatrixBufferMPEG2`，必须反向写回。

**合成结果与 ffmpeg 原始流逐字节完全一致**：第 1 帧 82620 字节、
第 2 帧 129038 字节，差异数 0；层次
`seq → ext → GOP → picture → ext → slice` 也一致。

定位过程中修正的四个字段（每个都靠与原始流逐字节比对确认）：

| 字段 | 错值 | 正确值 | 说明 |
|---|---|---|---|
| `profile_and_level` | 0x48 | 0x44 | 1080p 超过 Main level 的 720x576 上限，需 High-1440 |
| `vbv_buffer_size` | 112 | 3 | 填过大等于要求固件准备不存在的缓冲 |
| `aspect_ratio` | 1 (square) | 3 (16:9) | VA 不提供，按尺寸比推断 |
| 量化矩阵 | 恒写入 | 与默认矩阵比对后决定 | ffmpeg 恒把 `load_*` 填 1，照写会让 header 从 12 字节膨胀到 140 |

即便如此，送入 `/dev/video32` 后固件在第 2 个单元处 `SYS_ERROR`、一帧不吐。
既然码流与固件自己能解的原始流没有任何字节差异，问题不在合成侧。

待查方向：MPEG-2 的 OUTPUT `sizeimage` 只有 1958400（H.264/HEVC 是
16588800），单帧 129038 字节虽装得下，但固件可能要求按 GOP/序列边界
而非逐帧送料。

实现置于 `-DDMD_ENABLE_MPEG2` 后不声明：ffmpeg 的 `-hwaccel vaapi`
遇到已声明的 profile 不会回落软解，会直接失败退出。

### ⚠️ 文档更正：自检命令会误判

README 的自检命令用的是 `-f null -`，而那恰好是本版修复的死锁路径。
修复前按 README 自检会得到"挂死"的结果并误判为驱动不可用。
判断硬解是否真在跑，除看帧数外还可核对 Venus 硬件状态：
解码期间 `mvs0_gdsc` 会从 `disabled` 翻到 `enabled`，
`venus_bus_ddr` 频率从空闲 64000 升到 221000（用户态改不了，是内核实况）。

### 回归验证

| 编解码器 | 帧数 | 硬解 | 软解 | md5 |
|---|---|---|---|---|
| H.264 1080p | 150/150 | 11s | 18s | 逐字节一致 |
| HEVC Main 1080p | 90/90 | 8s | 7s | 逐字节一致 |
| VP9 Profile0 1080p | 90/90 | 10s | 9s | 逐字节一致 |
| VP8 1080p | 90/90 | 9s | ~9s | 逐字节一致 |

`-f null` 挂死场景：40s 超时 → 3s 正常退出。AV1 单元测试全部通过。

## v0.4.1

**nabu（Xiaomi Pad 5 / Snapdragon 860 / kernel 4.14）解码路径打通。**
0.4.0 判定"这台设备的 video32 解码路径起不来"，那个结论是错的。

### ✨ 补齐 msm_vidc 的四项私有协议要求

缺的不是缓冲类型、不是 ION heap、不是容器权限，也不是固件能力：

1. **必须启用 SECONDARY 分流模式**
   `V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_MODE = base + 22 = 0x00992016`，
   值 1 = SECONDARY（厂商 `v4l2-controls.h:865-869`）。
   PRIMARY（默认）下 CAPTURE 直接充当 DPB，`start_streaming()` 恒返回 -EINVAL；
   SECONDARY 启用 `HAL_BUFFER_OUTPUT2`，DPB 与 OPB 分离，
   `msm_vidc.c:1214-1222` 会额外调 `msm_comm_set_output_buffers()`
   让驱动自行分配 DPB，校验路径完全不同。
   单一变量对照：SECONDARY → STREAMON(OUT) OK；PRIMARY → EINVAL。
   附带好处：CAPTURE 可用线性 NV12，不必解 UBWC 的 Q128。

2. **必须订阅厂商私有事件**，驱动从不发标准 `V4L2_EVENT_SOURCE_CHANGE`
   `V4L2_EVENT_MSM_VIDC_START = V4L2_EVENT_PRIVATE_START + 0x1000 = 0x08001000`
   （厂商 `videodev2.h:2290-2305`），+2 是 `PORT_SETTINGS_CHANGED_SUFFICIENT`。
   **这就是 0.3.x/0.4.0 记录的"等 SOURCE_CHANGE 永不到达"的真相** ——
   不是固件没响应，是订阅错了事件类型，poll 永远不返回 POLLPRI。

3. **收到 PORT_SETTINGS 事件后必须发 SESSION_CONTINUE**
   驱动在事件处理里无条件置 `inst->in_reconfig = true`
   （`msm_vidc_common.c:1761`），固件随后停在 reconfig 等待态。
   `msm_comm_session_continue()` 只有两个调用点：`start_streaming()` 内
   （`msm_vidc.c:1244`）和 `V4L2_QCOM_CMD_SESSION_CONTINUE` 分支
   （`msm_vidc_common.c:4155`）。前者需重跑 STREAMON，而 STREAMOFF 会把
   state 打回 `MSM_VIDC_START_DONE` 以下 → 实测必然 `SYS_ERROR`。
   正解是 `VIDIOC_DECODER_CMD` + `cmd = 5`（`videodev2.h:1991`），不动队列状态。

4. **设备必须 `O_NONBLOCK` 打开**
   `VIDIOC_DQEVENT` 在事件队列空时会阻塞在内核 `v4l2_event_dequeue()`，
   poll 循环再也回不来。内核 tracer/debugfs 全不可用的情况下，
   靠 `/proc/PID/wchan` + `/proc/PID/syscall` 定位。

### 💥 缓冲传递方式变更：DMABUF → USERPTR + `reserved[0]`

msm_vidc 只接受 `V4L2_MEMORY_USERPTR`（`q->io_modes = VB2_MMAP | VB2_USERPTR`，
`msm_vidc.c:1548`），MMAP 与 DMABUF 的 REQBUFS 都返回 EINVAL。

但 USERPTR 是个名义值 —— `vb2_mem_ops.get_userptr` 是返回 `0xdeadbeef` 的
桩函数（`msm_vidc.c:717-720`），真正的缓冲来源是：

```c
b->m.planes[i].m.fd        = b->m.planes[i].reserved[0];
b->m.planes[i].data_offset = b->m.planes[i].reserved[1];   /* msm_vidc.c:533-536 */
```

**dmabuf fd 必须写进 `plane.reserved[0]`**，`m.userptr` 被完全忽略。
这解释了 0.4.0 "USERPTR 整条流程走完却出不了帧"的困局。

分配器不变：仍是 dma_heap 优先、`/dev/ion` 兜底（nabu 无 dma_heap，
system heap id=25）。产出的都是标准 dmabuf fd，导出路径不受影响。

### 🐛 修三个缺陷

- **`decode.c` 对 mmap 地址调 `realloc`** —— exportable surface 的 `s->data`
  是 DRM dumb buffer 的 mmap 地址（`surface_alloc_dumb`），
  `surface_store_frame_locked` 无条件 realloc 它必然 SIGSEGV：glibc 拿 mmap
  地址往前找 malloc chunk 头，读到的是像素数据。
  这是既有缺陷，只因以前从没走到出帧才没暴露。
- **slice_height 用 `sizeimage` 反推** ——
  `3137536*2/(1920*3) = 1089.16 → 1089`，真实值 1088，
  多出一行让上层 `nv12_copy` 越界（`image.c:261`）。
- **4K 的 CAPTURE 几何三连坑** —— `G_FMT(CAPTURE)` 返回 open 时的残留默认值
  1920x1088，**不随 `S_FMT(OUTPUT)` 联动**；覆盖 width 后驱动不回填
  `bytesperline`；覆盖 height 后 slice_height 未按 32 对齐（2160 vs 真实
  2176），导致 **Y 平面全对、UV 平面从 Y 末尾起全错**
  （逐帧实测 Y 采样差异 0，UV 采样差异 281-301）。

### ✅ 验证

`ffmpeg -hwaccel vaapi -hwaccel_output_format nv12` 走完整 VA-API 路径，
输出走管道算 MD5，与纯软解逐字节对比。**5/5 全通，构建零告警。**

| 码流 | 分辨率 | codec | 整流 MD5 |
|---|---|---|---|
| `t.h264` | 1920x1080 | H.264 | `6b09e45570cf393e45dd42c0a3bc4c75` |
| `real1080p.h264` | 1920x1080 | H.264 | `ab51452b44d8461767d7dd86045a173f` |
| `s_3840x2160.h264` | 3840x2160 | H.264 | `bc5b31831aa7062ab9861b5456e5aebe` |
| `hevc.h265` | 1920x1080 | HEVC | `82318dd6c2e065fe9cb0b9f38244d8f5` |
| `vp9.ivf` | 1920x1080 | VP9 | `185b96897e45ef9c4d9c2669e1db450a` |

比对方法上踩过一个坑：硬解输出 1088 行含 8 行对齐填充。若让参考端也输出
1088 行，填充数据参与比对会让所有行错位，得出"PSNR 只有 25.9 dB"的假结论。
必须按 1920x1080 有效区比对。

### 📝 文档更正

`v4l2_backend.h` 的协议约束段**整段重写**。原内容结论是"这台设备的 video32
解码路径起不来，不要再为它加缓冲类型分支"，已被实测推翻。

同时更正 0.4.0 的两处方法论错误：

- **"必须两段式协商、不能双向一起 STREAMON"不成立。**
  真相是 `msm_vidc.c:1294-1302` 里第一个 STREAMON 因对侧未 streaming 而
  **跳过** `start_streaming()`（假通过），第二个才承担全部校验。
  SECONDARY 模式下两侧先配好、再让第二个 STREAMON 触发校验即可。
- **不能用 `QUERYCTRL` 枚举结果推断控制项不存在。**
  私有 CID 不在标准控制框架里、枚举确实是 0 项，但直接 `S_CTRL` 是成功的。
  （`G_CTRL` 不校验 id、拿 `0xDEADBEEF` 也返回成功，这点仍成立，
  别用它验证控制项存在。）

### ⚠️ 已知限制

- video32 解码会话一次只能有一个。Android 侧 HAL 占用时
  `MIN_BUFFERS` 会从 6/14 降到 4/12，两边会争这个节点。
- 4K 的 24 个 CAPTURE 缓冲需约 287MB ION 内存。测试机
  `MemAvailable` 2.2GB 时正常，内存紧张下的行为未测。

## v0.4.0

**架构性重写：解码改为驱动内 V4L2 直通，整条 Android 侧链路删除。**

### 💥 不兼容变更

0.3.x：`容器 ffmpeg → unix socket → Android 侧 decode-daemon → MediaCodec`
0.4.0：`容器 ffmpeg → libva 驱动 .so → /dev/video32`

解码全程在容器进程内完成。**升级只需换掉容器里的 `.so`**，
并可以卸载 `dmd_watchdog` 模块 —— Android 侧不再有本项目的任何进程或文件。

删除项（约 -9400 行）：

- `ksu-module/` 整个目录：看护启动、daemon 状态面板、探活循环、
  `dmd-probe`（690KB 探活二进制）、部署脚本、SELinux 规则
- `src/decode-daemon.c`（90KB，MediaCodec 实现）与 `build.sh`（NDK 交叉编译）
- `vaapi-driver/src/dmd_client.c`（1168 行，unix socket + SHM 槽位池 + 握手）
- `client/`：自述"验证 daemon 协议"的参考实现，它验证的协议已不存在
- `src/v4l2_backend.{c,h}`：daemon 时代的旧副本（532 行），
  驱动自带的是更新的 590 行版本 —— 两份 md5 不同，留着是误导源
- CI 从三个产物收缩到一个（删 daemon job 与模块打包 job）

删除前逐项取证，不是凭印象：`sepolicy.rule` 里**没有**任何 `/dev/video32` 或
`droidspaces-gpu` 规则（节点组权限由平台侧给，删它不影响硬解）；
产物中 `connect`/`socket`/`shm_open`/`dmd_session_open` 四个符号均为 0。

### ✨ V4L2 stateful decoder 直通

msm_vidc 要求**两段式协商**，这是 0.3.x 判定"V4L2 不可用"时踩空的地方：

1. 先只配 OUTPUT 格式并 STREAMON，喂入第一个序列头
2. 等 `V4L2_EVENT_SOURCE_CHANGE` 事件
3. **然后**才配 CAPTURE 格式并 STREAMON

两个队列一起 STREAMON 再喂数据，固件永远到不了 `START_DONE`，
零帧输出且每一步都返回成功 —— 这正是 `doc/why-not-v4l2.md` 当年测到的现象。

其它必需细节：CAPTURE 必须**显式**设 NV12（默认是 QCOM `Q08C`）；
缓冲只接受 dma-heap DMABUF（mmap 与 query offset 都返回 `ENODEV`）；
必须及时 DQBUF，否则解码器停住不再出帧。

### 🎬 编解码器支持

| 编解码器 | 状态 | 验证 |
|---|---|---|
| H.264 | ✅ | 300/300 帧，md5 `6b09e455` 与软解一致 |
| HEVC Main | ✅ | 12/12 帧，md5 `237fed06` 与软解一致 |
| VP9 Profile 0 | ✅ | 50/50 帧，md5 `3237a718` 与软解一致 |
| AV1 Profile 0 | 🚧 | 帧数与 dav1d 一致（150），**像素未通过**（30 帧仅 17 个不同画面） |
| VP8 | ❌ 移除 | msm_vidc 的 V4L2 层没有 VP80 格式，无硬件路径 |

AV1 默认**不声明** —— 像素未通过时声明它就是虚报，
Firefox / ffmpeg 会据此把任务交过来，得到重复帧与花屏，比回落软解更糟。
开发调试用 `-DDMD_ENABLE_AV1` 编译。遗留缺陷（flush 死锁、
70 个 `show_existing_frame` 未复现）见 `doc/av1-v4l2-status.md`。

VP8 的协议 codec 编号 3 **保留不复用**，以免与旧线协议撞号。

### 🔧 其它

- `DMD_VENDOR_STRING` 去掉 "MediaCodec" 字样（它描述的是已不存在的路径）
- 修好 `make tests`（此前长期 exit 2，因为两个测试要连 daemon 的 TCP 20003），
  现改为编译并直接运行 AV1 bitstream 单元测试，并纳入 CI
- `doc/why-not-v4l2.md` 加显著标注：其"V4L2 不可用（BLOCKED_HARD, certain）"
  的结论已被本版推翻。保留原文，因为探测结果表、源码定位、IRQ 数据仍准确 ——
  错在把"我这样调用不工作"写成了"这条路不通"

### ⚠️ 未测量项

- **性能**：0.3.x 的数字（1080p 峰值 194 fps 等）全部作废，
  V4L2 直通的吞吐与延迟尚未测量，也没有与旧架构的 A/B 对照
- **能效**：0.3.x 的功耗数据里"daemon 约 4.4 ms/帧落在宿主账上"一项已不存在，
  需要重新测量。定性结论（单路硬解不省电）预计仍成立

## v0.3.7 之后、v0.4.0 之前（daemon 架构的最后一批修复）

> ℹ️ 这一段记录的是 **0.3.x daemon 架构**下的修复，其中涉及的
> `client/`、`src/decode-daemon.c` 等文件已在 v0.4.0 删除。
> 保留此段是为了历史可追溯，不代表当前代码里还有这些路径。

文档审计的连带修复。三处都是审计时交叉核实发现的，与文档本身无关。

### 🐛 `client/` 参考实现漏读 PTS 字段，会让整条流错位

daemon 是**无条件**发送 PTS 的：SHM 路径 `msg[6]` = 24 字节
（`src/decode-daemon.c:1007`）、内联路径 `hdr[4]` = 16 字节（`:1101`），
两处 `htonl(pts)` 之前都没有能力位判断。能力标志 `CAP_FRAME_PTS`
只在格式描述块里**告知**客户端（`:1038`），**不是发送开关** ——
此前把它当开关理解是这个 bug 的根源。

而 `client/src/comm.c` 两条路径都少读一个字：

| 路径 | 实际读取 | daemon 发送 | 差 |
|---|---|---|---|
| SHM | 12 字节帧头 + `si[2]` = 20 | 24 | 4 字节 |
| 内联 | 12 字节帧头后直接读帧数据 | 16 | 4 字节 |

后果是下一帧从 PTS 开始解析，整条流从此错位。

真实驱动 `vaapi-driver/src/dmd_client.c:1014-1025` **处理正确**
（按 `caps` 读取并存入 `last_pts`），所以生产路径不受影响，
只有参考实现有此问题 —— 这也解释了它为何长期未暴露。

修复：两条路径都补读该字段，`DecodedFrame` 新增 `unit_seq` 承载。

实测（各解 300 帧，`rc=0`，零异常，帧尺寸全为 3133440）：
```
SHM  路径（--shm）  300/300
内联路径（默认）    300/300，57.36 fps
```

### 🐛 `DMD_DRIVER_VERSION` 落后 4 版，且是用户可见信息

该串经 `DMD_VENDOR_STRING` 显示在 `vainfo` 的 `Driver version` 里。
实测设备上显示：

```
vainfo: Driver version: DroidSpaces MediaCodec VA-API driver 0.3.3
```

而项目已到 v0.3.7，用户按此报 bug 会指向错误版本。已改为 0.3.7
并在 `driver.h` 补发版同步清单（本文件、`module.prop`、`CHANGELOG.md`）。

⚠️ 踩坑记录：`vaapi-driver/Makefile` 此前**不跟踪头文件依赖**，
改了 `driver.h` 后直接 `make` 产物里仍是旧版本串，必须 `make clean`。
**已根治**，见下一条。

### 🔧 `vaapi-driver/Makefile` 补头文件依赖跟踪

编译规则只写 `$(BUILD)/%.o: $(SRCDIR)/%.c`，不含头文件依赖，
所以改任何 `.h` 后 `make` 都认为目标是最新的。这类静默失败比编译错误
更危险 —— 构建"成功"了，装上去的却是旧行为。

标准三件套：`CFLAGS` 加 `-MMD -MP`、`DEPS := $(OBJECTS:.o=.d)`、
末尾 `-include $(DEPS)`。

实测验证：

```
clean 重建          → 10 个 .d 全部生成，0 error/warning
无改动再 make       → "对 all 无需做任何事"（增量仍然有效）
touch src/driver.h  → 重编译 9 个 .o（修复前是 0 个）
改版本串为 9.9.9    → 直接 make 即进产物，无需 clean（原踩坑场景）
```

daemon 侧不受影响：`src/` 只有一个 `.c`，`build.sh` 每次全量编译。

### 📝 补齐 v0.3.7 只做了一半的跨目录耦合注释

v0.3.7 的变更表声称「注释写明跨目录依赖，改一侧须查另一侧」，
但当时**只在 daemon 侧写了**，`vaapi-driver/src/driver.h` 侧没写 ——
`grep SHM_SLOTS vaapi-driver/src/driver.h` 零命中。该条描述与实际不符。

现补两处，均附 v0.3.7 那次事故的具体数据：

| 位置 | 约束 |
|---|---|
| `driver.h` `DMD_PIPELINE_DEPTH` | daemon 侧 `SHM_SLOTS` 必须 >= 本值 |
| `driver.h` `DMD_FRAME_TIMEOUT_MS` | daemon 侧 `SHM_SLOT_WAIT_MS` 必须 > 本值 |

### 🔧 已知遗留

- `CHANGELOG.md:222`（v0.3.5 条目）引用的 `src/decode-daemon.c:949`
  已漂移到 `:1000` —— v0.3.7 新增常量把它推后了 51 行。文字描述本身正确。
  文档引用源码位置**应改用函数名/宏名而非行号**：全仓库三处行号引用
  已全部失效，靠人工同步不现实。
- ~~本轮文档审计另查出多份文档的过时常量与失效事实，尚未修改。~~
  **已全部修完**（`73a0685` / `8923bb3` / `556c2ce`）：SHM 4→8 槽、
  1 秒→15 秒槽位等待、20→24 字节控制消息、NDK 下限 r27c→r26d、
  英文版协议版本 2→3 并补 bit31/endpoint 扩展、高度对齐 16→32、
  `doc/why-not-v4l2.md` 整份重写、SoC 865→855、新增能效小节、
  撤除 −28.6% 这个来源不可信的口径。共 8 份文档 + 2 处源码注释。

尚未审计的文档：`doc/vaapi-mediacodec-proxy-research.md`（707 行）、
`doc/browser-vaapi-guide.md`（291 行）、`ksu-module/README.md`、
`doc/release-v0.3.4-notes.md`、`dshmon/README.md`。
另 `vaapi-driver/README.md` 的 dmabuf 导出描述（说"多一次 CPU 拷贝"，
被 `export.c` 证伪且与同文件后文自相矛盾）也待修。

## v0.3.7

发布日期 2026-08-28。修一个真实丢帧的 bug，以及掩盖了它整整一年的日志假警报。

### 🐛 SHM 槽位耗尽导致真实丢帧（4K + 慢消费者 10/10 触发）

现象：4K 硬解写入文件时，客户端只拿到 238/300 帧，`ffmpeg` 报
`Conversion failed!`，daemon 日志 `等不到空闲槽位（客户端未及时归还）`。

根因是三个常量互相矛盾，且分处两个目录互不知情：

| 常量 | 值 | 位置 | 含义 |
|---|---|---|---|
| `SHM_SLOTS` | 4 | `src/decode-daemon.c` | daemon 池容量 |
| `DMD_PIPELINE_DEPTH` | 6 | `vaapi-driver/src/driver.h` | 驱动放行 6 帧在飞不取 |
| `DMD_FRAME_TIMEOUT_MS` | 5000 | `vaapi-driver/src/driver.h` | 客户端可阻塞 5 秒 |
| daemon 自旋上限 | 1000ms | `src/decode-daemon.c` | daemon 只等 1 秒 |

驱动被设计成允许 6 帧在飞而池只装 4 个，第 5 帧必然撞上"槽位全忙"；
更要命的是 `1000 < 5000` —— **daemon 比客户端先放弃，然后杀掉整个会话**。
容器侧没有后台收帧线程，槽位归还完全依赖上层调用节奏，
所以 4K 落盘这类慢消费者必然触发。

改动：

| 项 | 处理 |
|---|---|
| `SHM_SLOTS` | 4 → 8，不小于 `DMD_PIPELINE_DEPTH`（4K 下约 95MB） |
| `SHM_SLOT_WAIT_MS` | 新增，15000ms = 客户端超时的 3 倍，由客户端决定放弃 |
| 槽位全忙期间客户端已走 | 按"对端离开"处理，不再算故障 |
| 常量耦合 | 注释写明跨目录依赖，改一侧须查另一侧 |

### 🐛 日志假警报：78.6% 的正常会话被报成"发送帧失败"

客户端拿够帧后直接 `close`，而 daemon 手上还有流水线尾帧，
写进已关闭的 fd 必然得到 `EPIPE` —— 这是**正常收尾**，不是故障。
实测 412 个真实解码会话中 78.6% 走此路径，全部被记为 `发送帧失败`。

危害不只是噪音：它与真实丢帧**负相关**（真丢帧的 10 次里
`Broken pipe` 出现 0 次），却占了 97% 的失败日志量，
把上面那个真 bug 完全埋掉。排查时会 100% 指错方向。

改动：

| 项 | 处理 |
|---|---|
| `send_all` | 三态返回，新增 `SEND_PEER_GONE`；`EPIPE`/`ECONNRESET` 归此档并降到 level 2 |
| `send_frame_shm` | 透传该返回码，不再把正常收尾压成 `-1` |
| 会话结束日志 | 自述收尾原因，新增未发送尾帧计数 |

新日志形如：

```
[51] 会话结束(客户端正常关闭): 收到 293 NALU, 回传 286 帧, 尾帧 1 未发送
```

真错误仍打 `发送帧失败（传输错误），结束会话` 并保持 level 1。

### 🔧 `build.sh` 支持 ARM64 主机

Google 只发布 x86_64 主机版 NDK，其自带 clang 在 aarch64 上无法执行
（表现为 `libz.so.1` 缺失，实质是架构不符）。此前 daemon 只能靠 CI 构建。

NDK 的 sysroot 是纯数据，可配系统 clang 使用。但 clang 驱动会去找
自己版本的 `libclang_rt.builtins`（系统 19 vs NDK 17 路径不符，
`-rtlib` 覆盖不掉），必须分两步：clang 只编译成 `.o`，
再用 `aarch64-linux-gnu-ld` 手动链接 NDK 的 crt 与库。
`build.sh` 现在检测到 NDK clang 不可执行时自动走此回退路径。

### 验证

判定标准用**落盘字节数除以单帧字节**（1080p 取 crop 后的
1920×1080=3110400，用 1088 会算出 297.79 的非整数假象）。
不用 `ffmpeg` 的 `frame=` 值 —— `-f null` 时它含空壳 surface 会虚报。

```
修复前  4K+落盘: 2963013632 ÷ 12441600 = 238.15 帧, Conversion failed!
修复后  4K+落盘: 3732480000 ÷ 12441600 = 300.0000 帧, rc=0

回归 5/5 通过: 4K × 3 + 1080p × 2，全部精确 300.0000 帧
新 daemon 期间: 发送帧失败 0（原 282）、等不到空闲槽位 0（原 5）
```

### 遗留

- `vaapi-driver/src/dmd_client.c` 客户端 destroy 前不排空在途帧，
  修了能让尾帧也送完；当前行为已不误报、不丢帧
- `vaapi-driver/src/decode.c` 排空循环 `budget -= 100` 按固定步长扣减
  而非实测耗时，等价于恒定 50 次上限
- daemon 日志无时间戳，时间线只能靠行号关联
- 未验证：HEVC 路径、真实播放器收尾、8 槽在多路并发下的内存压力

## v0.3.6

发布日期 2026-08-27。修一个只在**新设备**上暴露的部署缺口。

### 🐛 模块 zip 自带 `decode-daemon`，刷入即完成安卓侧部署

新设备刷入模块后 `/data/local/Droidspaces/dmd/` 是空的，看护持续报
`decode-daemon 不存在或不可执行`。

根因是模块只装看护自身，从不部署 daemon —— `service.sh` 只 `mkdir` 空目录，
它那段 `cp` 是给老用户从旧路径 `Droidspaces/bin/` 迁移用的兼容逻辑，
新设备没有源文件所以不生效。此前一直没暴露，是因为已有设备上的 daemon
是当初手动放进去的，典型的"在我机器上是好的"。

改动：

| 项 | 处理 |
|---|---|
| CI `watchdog` job | 增加 `needs: [ daemon ]`，把 `decode-daemon` 打进模块 zip |
| `customize.sh` | 建 `dmd/{bin,run,logs}`，daemon 装到 `dmd/bin/`（0755），装完从模块目录移走 |
| 已有 daemon | 保留原版本，不覆盖 |
| 都没有 | 打印手动补救步骤，不让安装静默成功 |

容器侧的 `msm_drm_drv_video.so` **故意不自动部署**：容器名与发行版布局都不
确定（Alpine 没有 `usr/lib/aarch64-linux-gnu/dri` 这条路径），遍历
`Containers/*/` 猜路径一旦猜错就是静默装到无用位置，比不装更糟。
它继续作为独立 release 资产，由用户自行放入，安装结束时打印所需步骤。

顺带修 `ksu-module/README.md` 方式 B 的一个真 bug：它 `chmod` 的是旧路径
`Droidspaces/bin/decode-daemon`，且手动解压绕过 `customize.sh` 后 daemon
根本不会被部署。现补齐部署命令并加装后自检。

三种安装场景已用桩函数模拟验证：包内有 daemon（部署并移走）、包内无但设备
已有（保留原版本）、两者都无（打印补救步骤）。

## v0.3.5

发布日期 2026-08-27。三条主线：探活判据从"能握手"改为"能出帧"、
零拷贝帧回传转为默认开启、Unix socket 吞吐塌陷修复。

前两条各自修掉了一类**会被静默忽略的故障**：旧探针把零出帧的 daemon 判为
healthy，而缓冲不足导致的吞吐塌陷会让容器内浏览器悄悄回落 CPU 软解。

### ✅ `dmd-probe` 从"验握手"改为"验出帧"

补掉本页"已知问题"里标 🟠 的那条：探针只验握手，于是**握手正常但零出帧**
的 daemon 会被永久判定 healthy。这不是假想故障 —— 上一轮 Unix socket 吞吐
塌陷期间，容器内浏览器已静默回落 CPU 软解，而 watchdog 全程报 healthy、
不做任何处置。

新判据要求走完整条链路：内嵌 96×96 H.264 码流 → 按起始码切成单个 NALU
逐个带 4 字节长度前缀发送 → `shutdown(SHUT_WR)` 触发 flush → 解析下行，
收到 **≥1 个帧大小 > 0 的非哨兵消息**才判健康。

内嵌码流：96×96 Constrained Baseline（profile_idc=66）/ yuv420p / 5 个 IDR
帧 / **3660 字节** / 切成 7 个 NALU（1×SPS 25B + 1×PPS 9B + 5×IDR ~725B）。
生成后砍掉了重复的 SPS/PPS 与 564 字节 SEI（daemon 只需一份参数集），
体积从 9522 降到 3660，`.rodata` 只涨 4336 字节。

选型约束（每条都踩过）：
- **必须 yuv420p** —— 硬件不支持 4:4:4/4:2:2，送 4:4:4 会被 VAAPI 拒绝后
  静默回落软解（调查这个判据时就栽在这上面：用 `libx264 -preset ultrafast`
  生成的码流默认是 High 4:4:4 Predictive，ffmpeg 报 300 帧 22x 全是软解，
  daemon 侧一条会话都没有）
- **分辨率 96×96** —— daemon 接受范围是 96×96 ~ 8192×4320，取下界
- **`-bf 0 -g 1`** —— 禁 B 帧且每帧 IDR，否则解码器要第 4 个输入单元才吐首帧

真机实测（宿主侧，连续 12 次 + 并发 4 次一致）：

```
probe: 健康 帧=5 dev=64801 ino=1340986        rc=0   166~201ms
```

daemon 侧记录从 `640x480 / 0 NALU / 0 帧` 变为 `96x96 / 7 NALU / 5 帧`。
另用独立 python 客户端复核下行：格式块 `caps=0x1`（CAP_FRAME_PTS 生效）、
`stride=96 slice=96 crop=(0,0,95,95)`、5 帧各 13824 字节 = 96×96×1.5、
PTS 序号 1~5 连续；再与 ffmpeg 软解做像素对账，**Y 平面平均绝对差 0.00**
—— 回传的是真解出来的图，不是空缓冲或填充。

新增退出码 **8** = 握手成功但零帧回传，与既有 0/3/7 区分。故障注入实测：

| 构造的故障 | rc | 输出 |
|---|---|---|
| 握手成功但一直不出帧 | 8 | `握手成功但 0 帧回传（超时 3000ms，已送 7 个 NALU，原因=超时）` |
| 只发格式块不发帧 | 8 | 同上（格式块不计入帧数） |
| 收完 NALU 就关连接 | 8 | `...原因=对端关闭连接` |
| 下行帧大小离谱 | 8 | `下行解析异常：下行帧大小离谱，协议错位` |
| 只请求内联却回 SHM | 2 | `daemon 返回非内联传输模式 1，探针无法收帧` |
| endpoint dev/ino 造假 | 7 | `endpoint inode 不匹配`（语义不变，看护仍不重启） |
| 旧 daemon 拒 v3 | 0 | `健康 帧=3（协议 v2，旧 daemon）` ← 降级重连 + 跳过 inode 对账 |

**新旧对照**（同一个"握手成功但零出帧"的故障端点）：

| 探针 | 结果 |
|---|---|
| 旧（纯握手判据） | `rc=0 probe: 健康` ⚠️ **误判** |
| 新（出帧判据） | `rc=8 不健康 握手成功但 0 帧回传` ✅ |

实现取舍：
- **超时改成绝对 deadline**。原先每次 `io_all` 各传一个相对超时，步骤变多后
  （握手 + 7 次写 + 若干次读）累加会远超 3 秒，把看护的 5 秒轮询挤掉。
  现在所有 I/O 共享一个截止时间，`rc=8` 实测正好 3009ms 收敛。
- 收到首帧后用 **250ms 宽限期**数完剩余帧：判健康只需 1 帧，其余纯属诊断。
- 下行帧大小上界取 **64MB 而非 MAX_FRAME(8MB)** —— 后者只约束上行，
  4K NV12 单帧 12441600 字节本身就超它，拿它校验会把正常帧判成非法。
- `io_all` 把"超时"与"对端在消息边界关闭"分开返回：零出帧时前者是吞吐
  塌陷（daemon 读不动），后者是会话被主动放弃，输出里的 `原因=` 直接区分。

### ✅ 看护补 `rc=8` 处置分支

`dmd-watchdog.sh` 的 case 从 `1|2)` 扩为 `1|2|8)`。rc=8 与 rc=2 同性质
（进程活着却不服务），走同一条路：`pkill` 掉半死实例再拉起。此前 rc=8 会
落进 `*)`「未知码，按失败处理」——只累加失败计数、不重启，等于新判据抓到了
故障却不处置。

`rc=7` 的语义保持不变：那是挂载配置问题，重启 daemon 只会换个新 inode，
救不了容器侧的死引用。

### 📌 附带修正一处文档过期结论

`doc/performance-and-roadmap.md` 写于 08-22，其中三处关于 SHM 的判断已被
08-26 的改动推翻，本次一并修正：「实现完成但当前不可用，默认关闭」→
已默认开启且有可信端到端数字（daemon CPU −19%）；「`cfg.want_shm = 0` 是
全仓库唯一赋值，驱动从不主动请求」→ 已改为默认请求。

同时补一条容易误解的说明：**SHM ≠ 解码器直写共享内存**。MediaCodec 的输出
缓冲由 gralloc 分配，daemon 只能 `getOutputBuffer` 拿 CPU 指针后 `memcpy`
进 memfd（`src/decode-daemon.c:949`）。所谓"零拷贝"只描述
**memfd → 容器客户端**这一段；解码器到 memfd 那次拷贝仍在，
消除它要走 dmabuf，而那条路已因需依赖私有符号被否决。

### ✅ SHM（memfd 零拷贝）转为 Unix socket 模式下的默认

此前 `cfg.want_shm` 需 `DMD_WANT_SHM=1` 显式开启，源码注释记载"驱动侧
从未真正启用过，只有 tests/test_dmd_client.c 走过"。本次在真实环境
（驱动被 dlopen 进 ffmpeg、走 /run/dmd/decode.sock）实测通过：

```
[167] 共享内存已交接: 4 槽 x 3133440 字节 (共 12537856)
[167] 握手成功: video/hevc 1280x720 帧回传=SHM
```

解码结果与内联模式一致（150/150 帧）。收益（固定 1500 帧工作量，
三组交替对照，daemon 侧 CPU jiffies）：

| 模式 | 三次测量 | 中位数 |
|---|---|---|
| 内联 | 493 / 500 / 489 | 493 |
| SHM | 400 / 367 / 410 | **400** |

**daemon CPU 降低约 19%**，组内方差 ±2%。省下的正是每帧
1.38MB(720p) / 3.11MB(1080p) 经 socket 的那次拷贝。

保守边界：仅在 `use_sock` 时请求（SHM 交接走 abstract socket，属
net namespace，NAT 型容器必然降级）；daemon 侧交接失败会自动退回内联，
所以开启无硬失败风险。`DMD_WANT_SHM=0` 可显式关闭。

### 🐛 修复 daemon 日志写到旧路径

`dmd-watchdog.sh` 拉起 daemon 时的 stdout 重定向仍指向
`Droidspaces/Logs/decode-daemon.log`，是上一轮 dmd/ 路径重构的遗漏
（排查时因此在新路径下找不到会话记录，误判请求没到 daemon）。
已改为 `${LOG_DIR}/decode-daemon.log`。

### ✅ 修复 Unix socket 端点吞吐塌陷（0.92x → 7.4x）

根因是**默认 socket 缓冲容量与单帧大小不匹配**：

| | AF_UNIX | AF_INET (TCP) |
|---|---|---|
| SO_SNDBUF | 229376 (224KB) | 524288 |
| SO_RCVBUF | 229376 (224KB) | 1048576 |

而一帧 NV12 是 1.38MB(720p) / 3.11MB(1080p)，**远超 224KB**。缓冲装不下
整帧时，daemon 的 `send_all` 反复阻塞等对端取走，往返次数数倍于 TCP；
output 线程被堵住 → MediaCodec 输出帧不回收 → 输入槽位耗尽 →
报 `输入缓冲暂满，重试` → 12 次后放弃会话。上层看到的是解码失败后
**静默回落 CPU 软解**，只表现为卡顿与 CPU 飙高。

修复：两端都把 `SO_SNDBUF`/`SO_RCVBUF` 显式设为 4MB（整帧装下 1080p NV12
并留余量），受 `net.core.{w,r}mem_max` 截断则退化为原状，失败不致命。

- `vaapi-driver/src/dmd_client.c` `unix_connect()`：连接后设置缓冲
- `src/decode-daemon.c` accept 循环：对每个会话 fd 设置缓冲

实测（nabu，720p30 HEVC，连续三轮）：**150/150 帧，7.0~7.8x**，
与 TCP 基线（6.7~8.6x）持平。

### ✅ 修复模块状态永远显示"尚未启动"

`service.sh` 的 `update_prop()` 用 `sed "s|...|...|"`，而状态文本本身含
`|`（`"看护中 (PID 123) | 探活间隔 5s"`），内容里的竖线被当作 sed 分隔符：

```
sed: -e 表达式 #1，字符 58："s"的未知选项
```

错误又被 `2>/dev/null` 吞掉 → **状态静默不更新**，管理器里永远显示打包时
的初始值"尚未启动"。看护实际一直正常运行（`state: healthy`、日志持续
探活通过），是纯粹的**状态显示假故障**，却让人误判模块刷入后起不来。

修复：
- `update_prop()` 改用 `awk` 逐行重写，内容不参与模式解析
- 防重复启动的早退分支补上状态回写（此前复用已有实例时直接 `exit 0`，
  同样会把状态留在初始值）
- `module.prop` 版本号升至 v0.3.4 / versionCode 304，便于辨认刷入是否生效

## 已知问题（2026-08-26 实测记录）

### ~~🔴 Unix socket 传输通道吞吐不足~~（已在上方修复）

同一二进制、同一码流、同一硬件的对照实验（720p30 HEVC，5 秒）：

| 端点 | 结果 | 速度 |
|---|---|---|
| TCP 20003 | 153 NALU → 150 帧 | 8.6x |
| TCP 20013（另起实例复核） | 145 NALU → 136 帧 | 6.7x |
| **Unix socket** | **52 NALU → 49 帧，会话中断** | **0.92x** |

现象链：daemon 从 socket 读 NALU 跟不上 → MediaCodec 输入缓冲填不满 →
日志 `输入缓冲暂满，重试 #1（等 5000 ms）` → 5 秒后放弃会话 → 驱动向上报
解码失败 → **ffmpeg/浏览器静默回落 CPU 软解**，用户只看到卡顿与 CPU 飙高。

排除项（都验证过不是原因）：多 daemon 实例抢 MediaCodec（停掉 TCP 实例后
socket 依旧卡）、watchdog 探针每 5 秒占用 codec（停掉 watchdog 后依旧卡）、
daemon 二进制本身（同一个二进制走 TCP 正常）。

影响面很大：**驱动的端点探测顺序是 socket 优先、TCP 兜底**，
所以只要 `/run/dmd/decode.sock` 存在，不设 `DMD_ENDPOINT` 的客户端
（包括浏览器）就会默认走进这条坑。README 把路径式 Unix socket 标注为
"推荐通道"，与实测吞吐表现不符，文档需同步修正。

当前规避手段：客户端显式指定 `DMD_ENDPOINT=tcp:20003`，
见 `doc/browser-vaapi-guide.md` 第零章。

### ~~🟠 watchdog 探活判据过弱：能握手就算健康~~ —— 已修复（见本页顶部）

`dmd-probe` 只验证端点可连接 + 协议握手成功，不验证**能否出帧**。于是一个
握手正常但零出帧（或吞吐不足）的 daemon 会被持续判定为 `healthy`，永远
不会触发重启 —— 上面那个 socket 故障期间，watchdog 状态一直是 healthy。

建议改进：探针发一小段真实码流并要求收到至少 1 帧回传，才判定健康。

## v0.3.4

浏览器硬解接入实战沉淀：Chrome 与 Firefox 调用本硬解后端的完整方法文档
与一键体检脚本。全部结论来自真机验证（nabu / SD855 / Debian 13 容器）。

### 新增：`doc/browser-vaapi-guide.md`

覆盖两个浏览器的必需参数、原理、固化方法、验证步骤与排障速查表：

- **Chrome 必须 Wayland 模式**：`VaapiVideoDecodeLinux` 的解码帧经
  linux-dmabuf 协议提交合成器，X11 下无此协议 —— 解码器创建后一帧不喂
  直接空转（dmd 日志特征：握手成功但 0 NALU）。此结论终结了
  "X11 + native GL" 的旧思路：当前 Chrome 只剩 ANGLE，native GL 路径不存在。
- **`--render-node-override` 是 ARM 平台的命门**：Chromium vaapi_wrapper 的
  `PreSandboxInitialization()` 跳过一切非 PCI 总线 DRM 设备，ARM SoC 的
  renderD128 必须用该开关从 `LoadDrmFD()` 分支注入。
- **Firefox 三件套**：user.js 开 VA-API（注意 profile 按 installs.ini 的
  Default 定位，profiles.ini 老式标记无效）+ `MOZ_DISABLE_RDD_SANDBOX=1`
  （RDD seccomp 拦截设备访问）+ desktop Exec 固化。
- **平台兼容性记录**：anland 显示桥对 Chrome 存在呈现反馈缺失
  （totalVideoFrames 增长但 requestVideoFrameCallback 零回调，五组合复现），
  表现为 HEVC 掉帧跳跃/绿屏；Firefox 不受影响，为 HEVC 观看推荐。

### 新增：`tools/check-browser-vaapi.sh`

容器内一键体检：daemon 连通性、驱动部署、vainfo 初始化、Chrome GPU 进程
与 Firefox RDD 进程的驱动栈加载状态，逐项给出可行动的修复建议。

### 文档勘误

README 中"浏览器沙箱能否收 SCM_RIGHTS 未实测"更新为实测可行：
Firefox RDD 与 Chrome GPU 进程均已真机验证正常建立解码会话。

## v0.3.3

新增 endpoint inode 校验：客户端与服务端对账监听 socket 的真实身份，
不一致立即报错拒绝连接 —— 消灭"连着但其实是死 socket"的假装连接状态。
协议向后兼容：v2 客户端/服务端不受影响，可分别滚动升级。

### 新增：握手响应携带 endpoint dev/ino（协议 v3）

daemon 在 `bind+listen` 成功后对自己监听路径 `stat()` 一次，
握手响应里如实上报 `(st_dev, st_ino)`；客户端对 connect 所用路径 stat 对账。
两者不一致 = 客户端解析到的不是这个 daemon 的端点。

典型病灶：平台把**单个 socket 文件**而非目录做 bind mount。daemon 重启必
unlink+重建 socket 换 inode，容器侧持死引用 —— 此前症状是连接失败或静默退化成
软解，且两侧 stat 可能显示同一个孤立 inode，人工诊断极难（本项目多次误判）。
现在这一步变成自动报错：`DMD_ERR_ENDPOINT_MISMATCH`（驱动侧）/ 独立退出码 7
（独立客户端），错误信息直接给出四元组数值与"改用目录级挂载"的可行动结论。

细节：
- 响应名字长度字段的 bit31 作扩展标记，其后追加 16 字节
  `[dev_hi][dev_lo][ino_hi][ino_lo]`（各大端 u32）；错误路径恒为裸 12 字节
- TCP / 抽象命名空间模式无路径概念，填 0，客户端跳过校验
- 版本判定从严格相等改为区间 `2..3`，允许两端分别滚动更新
- 服务端启动日志新增 `listening endpoint: dev=%llu ino=%llu` 行
- 测试钩子（勿在生产设置）：`DMD_TEST_FAKE_INO="dev:ino"` 上报假值、
  `DMD_TEST_REPLY_LEGACY=1` 强制旧格式回包，用于验证客户端两条分支

### 修复：补 `<fcntl.h>` include

源码使用 `open(O_*)` 却未包含 `<fcntl.h>`，bionic/glibc 靠 `<sys/file.h>`
间接传递才编译通过，musl 直接失败。显式补上，交叉工具链不再挑环境。

### 修复：新客户端连旧 daemon 被硬拒（真机暴露）

v0.3.1 及更早的 daemon 按**严格相等**判协议版本，见到 v3 一律回 `status=1`
并断开。而部署现实是 daemon 由平台 App 投放（会被 App 更新覆盖回旧版），
驱动在容器内独立更新，"客户端新 / daemon 旧"是**常态**错配方向 ——
真机上容器侧新驱动连生产 daemon 直接 `拒绝握手: 协议版本不支持`。

现在客户端在 `status=1` 时自动重连并降级 v2 再试一次（daemon 拒绝后会断开，
必须重连）。降级后走无扩展路径，inode 校验随之跳过并打印说明。
只对 `status=1` 降级：codec/分辨率类拒绝换版本也没用。

### 验证

本机（glibc + aarch64-linux-musl 14.2.0，双工具链零警告）：

- v3 匹配路径：上报值 == stat 值，两客户端实现均正常建立会话
- 不匹配（`DMD_TEST_FAKE_INO`）：驱动库返回 `DMD_ERR_ENDPOINT_MISMATCH`
  并打印四元组详情；独立客户端 exit 7；SHM 名字解析不受扩展影响
- 旧格式响应（`DMD_TEST_REPLY_LEGACY`）：一次性 WARN 后照常工作
- 目录模式重启重连：inode 变更后新连接自动对上，无误报

真机（骁龙设备，Android 宿主 + Droidspaces Debian 容器，静态 aarch64 harness）：

- 宿主匹配路径：上报 `dev=64801 ino=1341980` 与 `stat` 逐位一致
- **真·文件级 bind mount**（busybox `mount -o bind`，Android toybox 的 `mount`
  会误走 losetup）：daemon 重启换 inode 后，经陈旧挂载连接在 `connect()`
  阶段即被内核 `ECONNREFUSED` 拦下 —— **走不到握手，inode 校验不参与**。
  这条路径本就由连接错误兜住，不会静默。
- 可达的 mismatch（daemon 上报值 ≠ 客户端 stat 值）：驱动库 exit 7 +
  `code=-10`，独立客户端 exit 7，两者都打全四元组
- 对照：同一时刻正确的目录级路径连接正常，无误报
- 跨 mount namespace（容器 → 宿主生产 daemon）：降级重试生效，会话建立

> **边界更正**：先前把"文件级挂载死 inode"写成 inode 校验的主要战场，
> 真机测下来不准确 —— 那一类在 connect 阶段就失败了。inode 校验真正覆盖的是
> **连得上、但对面不是你以为的那个端点**（挂载视图分叉、错配的端点路径、
> 多实例串台），这类才是原本会一路静默到出错帧的情形。

### 部署提示：daemon 崩溃后没人重启它

本仓库只提供 daemon 与驱动；**拉起与看护属于平台侧**。Droidspaces 平台目前
只在容器启动与 monitor 的 reboot_cycle 里 spawn daemon，真机实测 `kill -9`
之后容器硬解一直坏到手工干预。

自 v0.3.3 起 release 附带 **`dmd_watchdog-*.zip`**（KSU/Magisk 看护模块，
源码在 `ksu-module/`，完整安装/配置/排障文档见压缩包内 README 或仓库同目录）。
它每 5 秒对 daemon 做真实端点探活（connect + 握手，不是 `kill(pid,0)`——
僵尸进程与会话级失效都看不见），失败自动拉起并复验；与平台抢拉的问题用
flock 仲裁规避。已真机验证：kill -9 后 5 秒内补回，容器硬解无感恢复。

如果不想用模块自己写看护，两个坑必须避开（同样由真机验证得出）：

- **别用 `kill(pid,0)` 判存活。** 僵尸进程也返回 0；daemon 还有会话级失效
  模式（进程活着、持着 flock、却不再服务新会话）。可靠判据是真实 connect +
  握手，v3 的 endpoint 扩展正好能顺带确认连到的是不是同一端点。
- **别和平台抢着拉。** 两边同时 spawn 会造成双实例互相 unlink socket
  （`/proc/net/unix` 出现同路径两条监听记录，先起的退化成无名孤儿仍在跑）。
  拉起前先取文件锁，拿到锁后再探一次。

另外：inode 不匹配（挂载指向别的端点）时**重启 daemon 无用** —— 只会再换一个
inode。那是挂载配置问题，应改用目录级 bind mount。

## v0.3.2

日志修正与结论订正。协议未变，解码路径无改动，与 v0.3.1 完全兼容。

> **`decode-daemon` 源码本版无改动**（`src/` 零变更），改的只有驱动侧的
> `vaapi-driver/src/dmd_client.c` 与文档。
>
> 但 release 里的 `decode-daemon` 二进制 checksum 与 v0.3.1 **不同**
> （31672 → 31776 字节）：v0.3.1 是本地 NDK（clang 18.0.3）编的，v0.3.2 由
> CI 用 NDK r26d（clang 17.0.2）重编。已核对两者的日志与协议字符串**逐条
> 相同**、握手版本同为 2，差异纯属编译器代码生成，行为未变。
>
> 已有部署**不需要**因本版更换 daemon，只换驱动 `.so` 即可。
> 从 v0.3.3 起 release 说明会自动交代每个资产的源码改动范围。

### 修复：会话建立日志把 Unix socket 谎报成 TCP

`dmd_client.c` 里那条 `会话建立` 日志无条件打印 `port=%u` 与 `传输=TCP`，
即使连接实际走的是 `sock_path`。连接分派本身一直是对的（`sock_path` 优先），
错的只有日志。

代价不小：排查时看到 `port=20003 传输=TCP` 会认定驱动忽略了 `DMD_ENDPOINT`、
硬走 TCP 兜底，据此一路查错方向。实际上 Unix socket 早已连通。

根子是把两个概念挤进了一个字段：`s->xfer` 描述的是**帧传输方式**
（内联 / SHM），**控制通道类型**（Unix socket / TCP）是另一件事。现在分开报，
走 Unix socket 时打印路径而不是那个没用上的端口号。

### 结论订正：v0.3.1 对 SELinux 规则的判断下过头了

v0.3.1 的诊断骨架是对的 —— `binder { transfer }` 确实按 **sender** 判定、
denial 确实被 `dontaudit` 静默。但它进一步断言「以 `droidspacesd` 为 subject
加规则不会有任何效果」，**这一步是错的**。

实测：补上五条以 `hwservicemanager` 为 subject、`droidspacesd` 为 target 的
规则后，硬解从「每次必崩」变为逐字节正确，连续 117 个会话无新 tombstone。

两者不矛盾，是链条上先后两环：codec 客户端要先解析
`android.hidl.manager@1.2::IServiceManager`，拿不到 hwservicemanager 就根本
走不到 IOmx 那一步。v0.3.1 看到的 `EX_TRANSACTION_FAILED for ...::IOmx` 是
**已经越过**第一环之后的现象。

另一半原因是总线不对称：媒体编解码走 **hwbinder**，而策略里既有的
`servicemanager` 规则只覆盖 **system binder**。这正是「PulseAudio 一直能
出声、硬解却必崩」的原因 —— 音频走的是 system binder。

完整规则与推导见 [`doc/platform-integration-contract.md`](doc/platform-integration-contract.md) §2.2。

教训：`droidspacesd` 是 permissive 域，只说明「以它为 subject 的**访问检查**
不阻断」，不等于「任何写法里出现它都无效」。它作为 **target** 时判定按 sender
走，照样生效。

## v0.3.1

守护进程稳定性修复。协议未变，驱动逻辑无改动，与 v0.3.0 完全兼容。

### 修复：accept 出错时整个 daemon 退出，所有会话一起断

原实现只把 `EINTR` 与 `ECONNABORTED` 当可恢复，其余 errno 一律 `break`
主循环。真机踩到过：容器跨 netns 连宿主 TCP 时 `accept` 返回 `EMSGSIZE`，
日志只留下 `accept: Message too long` 和 `daemon 退出`，正在解码的会话被
一并带走。叠加平台侧没有崩溃自动重启（`ds_spawn_daemon` 只在容器启动与
monitor 的 reboot_cycle 里拉起），daemon 一死要等容器重启。

`accept` 的错误几乎都只影响那一个连接。Linux 还会把新连接上待处理的网络
错误从 `accept` 抛出来，man 手册明确要求把它们当 `EAGAIN` 一样重试。现在
这些一律跳过该连接继续服务，只有 fd 耗尽这类真正的进程级故障才退出，交给
上层重启。

真机验证：`droidspacesd` 域下连续三个会话全部握手成功，daemon 进程始终
存活；客户端因协议不同步先退出导致 `send_all: write 失败: Broken pipe`
时，daemon 只结束该会话。修复前这两种情况都会让进程整体退出。

### 修复：Unix 模式下白调一次 TCP_NODELAY

`TCP_NODELAY` 在 `AF_UNIX` 上返回 `EOPNOTSUPP`。原先无条件调用，返回值没
检查所以无害，但没有意义。现在只在 TCP 模式下设置。

### 重要结论修正：SELinux 阻塞项的方向搞错了

本仓库文档此前把唯一阻塞项写成「`droidspacesd` 缺 hwservicemanager /
Codec2 的 allow 规则」。**这个方向是错的**，据此加规则不会有任何效果。

真正的原因：`binder { transfer }` 的 SELinux 判定按 **sender**（服务端域，
enforcing），不按 receiver。`droidspacesd` 自身是永久 permissive 域，救不
了这一步；而 denial 被 `dontaudit` 静默，所以全程看不到 avc 行 —— 这正是
先前误判的来源。

一锤定音的对照实验：`dumpsys -l`（纯枚举，不回传句柄）两域都成功，
`dumpsys media.player`（需回传句柄）在 `droidspacesd` 下
`FAILED_TRANSACTION`、在 `ksu` 下正常，同一模式在 system binder 与
hwbinder 两条总线一致复现。

正确的规则加在服务端域侧：

```
allow mediacodec   droidspacesd binder { call transfer }
allow mediacodec   droidspacesd fd use
allow mediametrics droidspacesd binder { call transfer }
allow mediametrics droidspacesd fd use
```

DroidSpaces 平台其实早就为 PulseAudio 做过同形状的事
（`allow audioserver droidspacesd binder { call transfer }`），硬解只是补
上 OMX 对应的服务端域。补规则后端到端逐字节验证通过：150 帧 1080p，
`msm_drm_drv_video.so` 经 `DMD_ENDPOINT` 自动接入，输出与软解 `md5` 相同。

另外注意 `hal_codec2_default` 类型在测试设备的策略里**不存在**（Codec2 由
`mediacodec` 域提供），CIL 里引用不存在的类型会导致整个策略编译失败。

## v0.3.0

新增第三条传输通道：路径式 Unix socket。协议未变，向后兼容 v0.2.0。

⚠️ 但**默认行为不完全等同上一版**：不设 `DMD_ENDPOINT` 时驱动会先探测
`/run/dmd/decode.sock`，存在且能连上就用它，否则才退回 TCP。
若平台没有 bind mount 该路径（当前默认情况），行为与 v0.2.0 一致。

### 新增：路径式 Unix socket 通道

原先驱动只能连 TCP `127.0.0.1:20003`，这依赖容器与 Android **共享 net
namespace**。实测 DroidSpaces 有两类容器，归属并不相同：

| | host 型 | NAT 型 |
|---|---|---|
| net namespace | 共享 `4026531937` | **独立** `4026535650` |
| IP | 直接持有 `wlan0` 真实地址 | `eth0` 172.28.x.x/16 |
| `127.0.0.1:20003` | 可达 | **不可达** |
| abstract socket 可见数 | 31（与宿主一致） | **0** |

所以 TCP loopback 与 abstract socket **都只在 host 型容器可用**。这里有个
容易搞错的地方：abstract socket 虽然不落文件系统，但它属于 net namespace，
**并不是 TCP 的替代品** —— NAT 型容器下两者一起失效。

路径式 Unix socket 不属于 net namespace，平台 bind mount 进容器即可，
两类容器都通，且鉴权直接靠文件权限，不必把服务暴露到网络。
DroidSpaces 自己的显示通道就是这个模式（宿主
`/data/local/tmp/anland-<hash>.sock` → 容器 `/run/display.sock`，
实测两侧 inode 相同，确认是同一文件）。

**用法**：

```bash
# daemon 侧（注意 SELinux domain，见下）
runcon u:r:droidspacesd:s0 decode-daemon --sock /data/local/tmp/dmd

# 驱动侧：显式指定
DMD_ENDPOINT=unix:/run/dmd/decode.sock
DMD_ENDPOINT=tcp:20003
# 不设则探测 /run/dmd/decode.sock，存在则用，否则退回 TCP
```

### 为什么 `--sock` 要支持传目录

`bind()` 只能创建**新 inode**，而 bind mount 绑定的是 **inode 而非路径**。
挂单个 socket 文件时，daemon 每次重启都换 inode，容器侧立刻
`ECONNREFUSED` —— 更麻烦的是此时两侧 `stat` 看到的 inode 可能一致
（都是那个孤立 inode），很容易误判成别的问题。

传目录就没这个问题：目录 inode 稳定，daemon 随便重启都不影响。
平台只需挂一次 `宿主 /data/local/tmp/dmd/ → 容器 /run/dmd/`。

### 验证

十二条流走 Unix socket 通道**逐字节比对 12/12 一致**，含 4.1GB
`long3000.h264`（3000 帧）与 2.0GB `long1500.h265`（1500 帧），
长流无累积错位。并发 4 路 4/4 一致。自动探测首次即命中 Unix socket。
异常关键词（可逆排空 / 会话已重建 / 队列已满 / 等帧超时 5000 /
退回 TCP / 收帧后仍无空位）全部为 0，且这是在十二条流全部成功解码
前提下取得的**有效阴性**。

### 判活用 flock（同版内自查修正，非修复上一版）

> 说明：`--sock` 与判活逻辑都是本版新增的，v0.2.0 里并不存在。
> 所以这不是"修复上一版的缺陷"，而是本版开发过程中经代码审查发现并
> 在发布前改掉的实现问题。记在这里是为了留下决策依据。

daemon 启动时要判断是否已有实例在跑。最初实现是"connect 一下，连得上就
认为有活实例"，有两个问题：

1. 用的是**阻塞** socket 且 connect **没有上界** —— 旧实例 backlog 打满时
   新进程会挂死在启动路径上；而若内核返回 `EAGAIN`，还会被误判成
   "无监听者"，进而 `unlink` 掉**活实例正在用的** socket 文件。
2. 对活实例有副作用：每次被拒的启动都让旧实例白跑一次 `accept` +
   建线程再销毁，并短暂占用一个并发配额。

`flock` 没有这些问题：不碰对方进程、无需超时，且进程无论怎么死
（含 `SIGKILL`）内核都会释放锁。

### 修复：SHM 路径未累加 `frames_recv`

`dmd_client.c` 的 SHM 收帧路径只读 `s->frames_recv` 当序号、从不递增，
于是该计数在 SHM 模式下恒为 0。这不只是统计瑕疵 —— `decode.c` 的排空
判据拿它当护栏（`frames_received() > 0`，即"至少收到过一帧才允许判定
等待徒劳"，那是当初修黑帧加的）。恒 0 会让整个条件恒假，方向上偏保守
（只靠超时、不会误排空），但护栏语义已经失真。

### ⚠️ 已知阻塞项：SELinux domain 权限

Unix socket 通道在 Enforcing 下**尚不能用**，因为现有两个身份各只有
一半权限：

| 启动身份 | 能 `bind()` socket | 能用 MediaCodec |
|---|---|---|
| `su`（`u:r:ksu:s0`） | ✗ 各处 `EACCES` | ✓ |
| `runcon u:r:droidspacesd:s0` | ✓ | ✗ SIGABRT |

`droidspacesd` 下崩在 `CCodec::allocate`，tombstone 栈顶
`Codec2Client::GetServiceNames`，报
`Hardware service manager is not running`。

三组对照实验确认这与传输方式无关：`ksu`+TCP 正常、`droidspacesd`+TCP
**同样 SIGABRT**、`droidspacesd`+Unix socket+**SELinux permissive**
正常解码。也就是说通道本身是正确的，缺的只是一条 allow 规则。

已排除的替代方案：root 无效（两个 domain 本来都是 uid 0，SELinux 是
MAC 不看 uid）；DroidSpaces `enable_hw_access=1` 无效（只透传 `/dev`
节点、不改 domain）；`untrusted_app` 走不通（无法执行 `shell_data_file`
标签的二进制，`chcon` 被拒）；另外 11 个 domain 均无法同时满足
"可切入"与"可 bind"；`selinux_permissive=1` 有效但那是把宿主 SELinux
整体切成 permissive，不可作为交付形态。

**规则到位前驱动会自动退回 TCP，行为与 v0.2.0 一致，无退化。**
平台侧需要什么见 `doc/platform-integration-contract.md`。

### 勘误（上一版及开发过程中的三处错误结论）

1. ~~"SHM 帧交付路径有 bug，根因未定位"~~ —— 那些 0 帧现象的真实原因就是
   上述 SELinux domain 问题，与 SHM 无关（当时日志里传输模式其实是 TCP）。
2. ~~"不需要新增 SELinux 策略"~~ —— 方向相反。建 socket 不需要，
   但在该 domain 下**解码**需要。
3. ~~"能连上 Unix socket 即 `SCM_RIGHTS` 可用，零拷贝两类容器都能用"~~ ——
   SHM 的 memfd 交接**不走**这条控制通道，而是 daemon 另开一个
   **abstract** socket，它属 net namespace。所以零拷贝只在 host 型容器
   可能可用，NAT 型必然降级。教训：控制通道能跨 netns，
   不等于交接通道也跨得过去。

### 发布前修订（审查与翻译阶段发现）

这些改动发生在 v0.3.0 tag 打出之后、Release 发布之前，属于同一版本的收尾：

- **`--sock` 两个坑修复**：尾斜杠（`/path/dmd/` 会拼出 `dmd//decode.sock`，
  日志与验证清单匹配串对不上）；目录不存在时**静默降级**成单文件 socket
  （单文件挂载在 daemon 重启后必然失效）。现在剥除尾斜杠、自动建目录，
  以 `.sock` 结尾的路径才视为文件模式并打印告警。
- **日志措辞**：`传输=TCP` 改为 `帧回传=SHM|内联`。原字段描述的是帧回传方式，
  与控制通道无关——走 `--sock` 时它照样印 TCP。这个歧义曾导致把 SELinux
  domain 失败误判成 SHM 帧交付 bug。
- **文档全局审查修订**：四个单元核对全部 11 份文档共报 95 条问题；
  研究文档里"Mesa 存在 src/va/venus/ 等五个 VA-API driver 目录"的关键论据
  被证伪（那些路径不存在），已标作废；事实清单三条基础前提订正。
- **新增英文版**：`README.en.md`、`vaapi-driver/README.en.md`、
  `doc/platform-integration-contract.en.md`。以中文版为准。

### memfd 零拷贝仍默认关闭

需 `DMD_WANT_SHM=1` 显式开启。实测该路径会让**单个连接**断掉
（118 单元只取回 25 帧），但不会打死 daemon（无新 tombstone、socket 继续
`accept`、事后非 SHM 路径复测一致）→ 属该连接的错误处理问题。
另外浏览器沙箱能否收 `SCM_RIGHTS` 亦未实测。

> 📌 后续变更：该默认值已于 2026-08-26 反转 —— Unix socket 模式下 SHM 改为
> 默认开启，`DMD_WANT_SHM=0` 才关闭（TCP 模式仍恒为关闭），并已在真实环境
> 端到端实测通过。见本页顶部"零拷贝帧回传默认开启（2026-08-26）"条目。
>
> 上面那句"浏览器沙箱能否收 `SCM_RIGHTS` 亦未实测"同样已作废 ——
> Firefox RDD 与 Chrome GPU 进程均实测能正常建立 SHM 解码会话。

---

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

> 📌 后续变更：此处"目前"指 v0.1.0 当时。该默认值已于 2026-08-26 反转 ——
> Unix socket 模式下 SHM 改为默认开启，`DMD_WANT_SHM=0` 才关闭（TCP 模式
> 仍恒为关闭）。见本页顶部"零拷贝帧回传默认开启（2026-08-26）"条目。
> "Firefox 沙箱下的表现未验证"也已作废：Firefox RDD 与 Chrome GPU 进程
> 均实测能正常建立 SHM 解码会话。
