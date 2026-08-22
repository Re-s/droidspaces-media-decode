# 性能实测与优化路线

本文记录压测结论、瓶颈归因，以及由此确定的优化优先级。
所有数字均为真机实测，测试环境见 [平台实测事实](verified-platform-facts.md)。

测试日期：2026-08-22

## 结论先行

当前 TCP 全拷贝架构的性能**已经满足 1080p60 与 4K60**，
瓶颈不在传输、不在硬件解码器，而在 **daemon 的单线程串行结构**。

该瓶颈**已经拆除**：收发分离后单客户端吞吐提升 20–30%，
CPU 占用从 83%（卡在单核上限）升到 102%（跨出单核），
并且支持了多客户端并发（4 路同时解码合计约 253 fps）。
详见下文"优化路线"第 1 节。

后续优化优先级：少拷贝（memfd）**已完成**（吞吐 +17%，daemon CPU −28.6%）；
零拷贝（dmabuf）**已否决**——上限收益仅 4.4% 单核 CPU，却需依赖私有符号，
理由见下文「成本核算」。
两者都不再是性能急需，而是为了降低 CPU 与内存带宽占用。

## 实测数据

| 指标 | 1080p (1920×1088) | 4K (3840×2160) | 640×480 |
|------|------------------|----------------|---------|
| 峰值帧率 | **194.47 fps** | **82.22 fps** | 527.80 fps |
| 字节速率 | 581 MB/s | 976 MB/s | 232 MB/s |
| 60fps 需求 | 179.3 MB/s | 711.9 MB/s | — |
| 富余倍数 | 3.24× | 1.37× | ~20× |
| 延迟 p50 | 4.46 ms | 19.3 ms | — |
| 延迟 p95 | 9.77 ms | 30.9 ms | — |
| 延迟 max | 59.3 ms | 95.2 ms | — |

纯 TCP loopback 基线（同帧形状、不含解码）：465–484 fps / 1392–1448 MB/s。

稳定性：70 秒长跑 11011 帧，无崩溃、无协议错误，RSS 从 64132 kB 微降至 63808 kB（无泄漏）。

## 瓶颈归因

判定：**daemon 单线程串行**（置信度 high）。

四个候选逐一排查：

**MediaCodec 硬件解码 — 排除。**
daemon 的 `dequeueOutputBuffer` 超时仅 1 ms，实测 2227 次命中、23 次空转（命中率 98.98%），
说明解码器几乎总有帧就绪在等着被取走。高通硬件 codec HAL 进程在 11 秒满负载窗口内
CPU 时间增量为 0.00 秒。

**TCP loopback 拷贝 — 排除。**
纯 TCP 基线 465–484 fps / 1448 MB/s，解码链路只用掉其中约 40%，富余 2.4–2.8 倍。

**客户端开销 — 排除。**
Python 客户端 185.72 fps 与 C 客户端 184–194 fps 落在同一区间；
C 客户端 user CPU 仅 0.07 s/30 s，8 核只跑满不到半核。

**daemon 串行 — 判定为瓶颈。**
daemon 进程占用 85.6% 单核（其中 sys 占 73%），主线程 39/60 采样处于 R 状态。
其主循环是完全串行的：

```
recv NALU → memcpy → dequeueOutputBuffer(1ms) → 同步 fprintf+fflush → 阻塞发送 3.1MB（12×262144 分片）
```

**发送 3.1 MB 期间既不接收新 NALU 也不取解码帧**，硬件解码器只能空等。
小分辨率场景字节速率反而下降（640×480 仅 232 MB/s）是 per-frame 固定开销主导的特征。

## 优化路线（按性价比排序）

### 1. 拆解 daemon 串行结构 — 已完成

三项改动全部落地：

- **收发分离**：每个会话跑 input / output 两个线程。
  input 负责 `recv NALU → queueInputBuffer`，output 负责
  `dequeueOutputBuffer → send`，发送大帧期间不再阻塞接收与入队。
- **分级日志**：改为 `-q`/默认/`-v` 三档，逐帧日志降到 debug 级并默认关闭。
  实测一次 20 帧的会话日志从旧版的逐帧输出降到默认级别 5 行。
- **多客户端并发**：accept 后为每个客户端派生独立会话线程，
  各自持有自己的 MediaCodec 实例，上限 `MAX_CLIENTS`（8，硬件支持 16）。

顺带修正了 EOS 处理：缓冲模式下应 queue 一个带 `BUFFER_FLAG_END_OF_STREAM`
的空输入缓冲，而不是调 `AMediaCodec_signalEndOfInputStream`（那是给 Surface
输入用的）。修正后测试流的输出从 148 帧变为 **150 帧**，尾帧不再丢失。

#### 重构前后实测对比

单客户端吞吐（1080p，2500 帧，剔除前 3 秒暖机，各测两次）：

| 版本 | 第一次 | 第二次 | CPU 占用 | 线程数 |
|------|--------|--------|----------|--------|
| 重构前（串行） | 201.09 fps / 630 MB/s | 197.28 fps / 618 MB/s | 83% | 4 |
| 重构后（分离） | 262.43 fps / 822 MB/s | 232.25 fps / 728 MB/s | **102%** | 7 |

吞吐提升约 **20–30%**。更有意义的是 **CPU 占用突破 100%**：
重构前 83% 卡在单核上限内（典型的单线程瓶颈特征），
重构后跨出单核说明串行结构确已拆开。

多客户端并发（4 个客户端同时解码 1080p，各取 100 帧）：

| 客户端 | 1 | 2 | 3 | 4 | 合计 |
|--------|---|---|---|---|------|
| 帧率 | 62.58 | 65.62 | 65.24 | 59.82 | **≈253 fps** |

重构前的串行架构下多客户端只能排队，无法并发。

（注：上表单客户端数字高于本文前面 A-2 报告的 194 fps，
因为压测客户端不同——A-2 用的是逐帧校验的 C 客户端，
这里用的是只计帧数的 Python 压测脚本。同一脚本下的前后对比才有意义。）

### 2. 少拷贝：memfd 共享内存

用 `memfd_create` + abstract socket 传 fd，双方 mmap 同一块内存，
省掉 TCP 的两次拷贝，仍需一次 CPU 拷贝。已实测可行，改动量中等。

### 3. 零拷贝：dmabuf 跨边界传递

**已在本设备端到端实测跑通**，完整链路：

```
Android ION/gralloc dmabuf → abstract socket + SCM_RIGHTS → 容器
  → DRM PRIME import → EGL dmabuf import → samplerExternalOES 采样
```

关键验证点：

- 容器与 Android 的 **net namespace inode 完全相同**（`4026531937`），
  abstract socket 双向可见且互连成功（含阴性对照）
- ION 分配的 dmabuf fd 传入容器后可 `mmap` 读到 Android 写入的内容
- `DRM_IOCTL_PRIME_FD_TO_HANDLE` 在 `renderD128` 上成功返回 GEM handle
  （memfd 走同路径返回 `EINVAL`，证明非假阳性）
- 容器 DRM 驱动确实是 msm（`msm_drm 1.2.0 / MSM Snapdragon DRM`），
  与 KGSL 共存；缺 `card0`（KMS 节点）只影响 GBM/KMS，不影响 render node 上的 PRIME import
- MediaCodec 侧 `AHardwareBuffer_getNativeHandle`（LLNDK）可用，
  `numFds=2`、`data[0]` 即像素 dmabuf，转发到容器同样导入采样成功

**硬约束（实现时必须遵守）**：

- **UBWC 不可用，必须线性输出。** 带 QCOM modifier 的 NV12 被 EGL 以
  `EGL_BAD_MATCH` 拒绝。原因不是格式白名单（Mesa 的 `ok_ubwc_format`
  显式接受 NV12），而是 **plane 模型冲突**：Venus 的 `NV12_UBWC` 是 4 plane
  （Y_Meta/Y_UBWC/UV_Meta/UV_UBWC），Mesa 按 2 plane 处理，
  meta plane 的 offset/stride 在 EGL 属性里无从表达。
  因此**唯一出路是让解码器输出线性格式**，换 RGB 格式绕不过去。
- 每 plane pitch 须 **64 B 对齐**（Venus 的 128 对齐天然满足）
- 传入的 pitch 必须**精确等于** Mesa 计算的 `layout.pitch0`，给更大的值会被拒绝
- plane1 offset = `align(W,128) × align(H,32)`；1920×1080 即 **2088960**，
  且 `EGL_HEIGHT` 须传**逻辑高度** 1080
- 调试：`FD_MESA_DEBUG=layout,msgs` 会打印 `invalid pitch (%u vs %u)` 等原因

另注意本机 GL 栈是 **zink over turnip**（走 `VK_EXT_image_drm_format_modifier`），
若改走 freedreno gallium 路径，pitch 精确匹配的约束需重新验证。

#### 修正：线性输出不是障碍，取不到 fd 才是

上面"必须线性输出"的约束**在当前架构下已天然满足**，之前列为遗留未验证项的
那一条现已实测：不指定 `AMEDIAFORMAT_KEY_COLOR_FORMAT` 时，
解码器输出格式为 `color-format: int32(21)`，即
`COLOR_FormatYUV420SemiPlanar`（线性 NV12），`stride=1920 slice-height=1080`，
并非 UBWC。UBWC 那套 4-plane 冲突只出现在 Surface / gralloc 路径。

真正堵死零拷贝的是另一件事：**NDK 公开 API 没有任何从缓冲导出 dmabuf fd 的入口。**
`android/hardware_buffer.h` 的全部公开函数里，与句柄相关的只有
`AHardwareBuffer_sendHandleToUnixSocket` / `recvHandleFromUnixSocket`，
而前者已实测跨容器不可用（对端需同为 Android 进程、依赖 gralloc registerBuffer）。
`AHardwareBuffer_lock` 系列只返回 CPU 指针 —— 拿到指针就意味着还要拷一次。
ByteBuffer 模式同理：缓冲由 codec 自己分配，既不能指定用我们的 memfd，
也没有 `getOutputImage` 之类的接口取 fd（A-3 已确认）。

因此 A-3 验证成功的那条链路有个前提差异必须讲清：它传递的是**探针自己用
ION/gralloc 分配的 buffer**，不是 MediaCodec 的输出缓冲。链路本身没问题，
但要接到真实解码输出上，就得依赖 `libui` / gralloc mapper 的**私有符号**
从 AHardwareBuffer 里挖 fd —— 那会绑死特定 Android 版本的 C++ ABI。

#### 成本核算：剩余那次拷贝只值 4.4%

零拷贝要省掉的就是 MediaCodec 输出缓冲 → 共享内存这一次 `memcpy`。
先量它到底值多少（设备实测，3133440 字节 × 300 次，预热 30 次）：

```
单次 0.227 ms，等效带宽 13137 MB/s
```

按 SHM 模式实测的 194 fps 折算，这次拷贝占约 **4.4% 的单核 CPU**；
对比 daemon 每帧总开销约 3.03 ms（545 ticks / 1800 帧），占比约 7.5%。

**结论：不做。** 上限收益不到一成，代价是私有符号依赖与版本脆弱性，
而当前吞吐对 60 fps 需求已有 3.2× 余量。TCP → SHM 那一步省掉两次内核拷贝
（CPU −28.6%）是划算的，再往下就不是了。
若将来出现真实瓶颈（例如多路 4K 并发），应优先重估这里的假设而不是直接开工。

### 不采用：容器内 V4L2 直解

看似最优（绕开整个跨边界问题），但**已实测否决**，详见
[为什么不直接用 V4L2](why-not-v4l2.md)。

## 压测注意事项

复核性能数据时有三个坑，不留意会得出错误结论：

1. **DVFS 爬频**：设备空闲频率极低（cpu4 约 710 MHz、cpu7 约 825 MHz），
   10 秒以内的测量会失真近 3 倍。纯 TCP 基线短跑读数 159 fps 曾一度
   指向"TCP 就是天花板"的错误结论，延长到 2500 帧后才跳到 465–484 fps。
   **所有测量应 ≥12 秒并剔除前 3 秒。**
2. **延迟必须配流控**：不限在途帧数时会积压到 7348 帧，测出的中位数无意义。
   但 window=1/2 又会因流水线深度（约 5 帧）饿死，反把延迟推高到 20–30 ms。
   有效档位是 window=8（实测在途 11.98 帧）。
3. **绑核可能反而更慢**：把 daemon 绑到大核簇后 fps 从 194 降至 136。

## 数据来源

完整压测报告（含测试程序源码与原始输出）：`A-2-tcp-decode-benchmark.md`
零拷贝可行性报告（452 行，含探针源码）：`A-3-zerocopy-feasibility.md`

### 共享内存协议的独立验证

SHM 协议由一个独立单元用 Python 客户端复核（不看实现、只按协议规格），
覆盖功能对等性、并发正确性、异常路径与吞吐对比。它确认的项：

- 功能对等 10/10，前 3 帧 NV12 逐字节 12/12 全等
- 8 路并发 × 3 轮共 24 次全部拿到自己的 memfd，无串台无死锁；
  交接完成后名字即注销，重复领取被拒
- 槽位不归还：约 1 秒报错、会话干净结束、fd 恒为 12、进程存活
- 中途 SIGKILL 客户端 5 轮：fd 12→12 无泄漏，新会话立即恢复

吞吐对比它测了两轮（不同二进制、不同设备热状态）：

| 轮次 | TCP | SHM | 相对提升 |
|------|-----|-----|----------|
| 第一轮 | 244.0 fps | 275.5 fps | +12.9% |
| 第二轮 | 221.9 fps | 252.9 fps | +14.0% |

绝对值整体下移约 8%（归因设备热状态），但**相对差距稳定在 +13~14%**。
配合我自己 2500 帧全程平均测得的 +17%，可以认为 SHM 的收益在 +13% 以上。
它还精确测到 `shm_handoff` 的 accept 超时是 **3 秒**
（2.8 s 领取成功、3.2 s `Connection refused`），与代码里的 `select` 超时一致。

它发现并已修复的三个问题：

1. **分辨率变大时 SHM 静默丢帧**（唯一 blocker）。`480p→720p` 声明 640×480 时
   SHM 只解 60 帧、TCP 解满 120。修法：槽位改按 adaptive-playback 上限预留。
   修复后 SHM/TCP 均为 120 帧。
2. **abstract socket 名字可预测可被抢注**。原为 `dmd-shm-<pid>-<sid>`，
   同 net namespace 内任何进程都能抢先 bind 使 daemon 降级。
   已加 32 位随机后缀（实测三次连续取名后缀互不相同）。
3. **`status=4` 只回 4 字节**，与其他拒绝路径的 12 字节不一致，
   客户端按 12 字节读会阻塞。已统一。

它同时纠正了文档里一处错误：握手响应的 `mode` 字段在 memfd 交接**之前**发出，
所以 `mode=SHM` 只是意向而非保证 —— daemon 可能先宣告 SHM 再静默降级
（实测降级发生在 3.0~3.5 s，即 `shm_handoff` 的 select 超时，
不是槽位等待的 1 秒）。客户端必须自带 fallback。

**已修复的独立复核**：上述三项修复后（产物 26136 字节），用它自己的客户端
重跑关键项，结论一致 —— `grow.h264` 的 SHM 从 60 帧变为 **120 帧**
（`"shm_frames": 120`、`"geometry_match": true`），与 TCP 相同；
名字后缀三次连续取值 `af3c109d` / `cb8ec488` / `c38828de` 互不相同；
`status=4` 回 12 字节。

**仍存在的瑕疵（已评估，不改）**：抢注 abstract socket 现在会诚实回 `mode=0`
（因为 bind 发生在回响应之前），但**交接超时**导致的降级仍发生在响应之后，
且没有补发通知。要修就得新增一类控制消息，而客户端侧 fallback 已完整覆盖
（`shm_attach` 失败即按 TCP 帧格式继续），收益有限，因此保留现状并在协议文档里写明。
两者均为工作区文档，未随仓库分发。
