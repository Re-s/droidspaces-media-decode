# DroidSpaces Media Decode Daemon

> 🌐 **English version: [README.en.md](README.en.md)**

Android MediaCodec 硬件解码代理服务，为 Linux 容器提供视频硬解能力。

## 项目简介

本项目提供了一个运行在 Android 设备上的 MediaCodec 硬件解码守护进程，把硬件解码能力暴露给 Linux 容器（如 DroidSpaces 中的 Debian 容器）。容器内的应用无需任何改动即可通过标准 VA-API 用上 Android 的硬件解码。

### 核心特性

- **硬件加速解码**：利用 Android MediaCodec API 硬件解码 H.264 / HEVC / VP8 / VP9
- **标准 VA-API 接口**：容器侧提供 VA-API 驱动，应用（ffmpeg、Firefox、Chrome）无需改动即可用
- **两种传输通道，自动选择**：
  - **路径式 Unix socket**（推荐）：不属于 net namespace，靠 bind mount 跨界，
    **host 型与 NAT 型容器都能用**；鉴权靠文件权限与 SELinux，服务不上网络。
    ⚠️ 当前 daemon 建 socket 后 `chmod 0666`（`src/decode-daemon.c` 里标注为
    "先跑通"的放宽值），真实部署应收紧到特定 gid。
  - **TCP 127.0.0.1**（兜底）：仅在容器与宿主**共享 net namespace** 时可用
    （host 型容器满足，NAT 型不满足）。
  
  驱动通过 `DMD_ENDPOINT` 显式指定，或自动探测默认路径后退回 TCP。
  
  ⚠️ **memfd 零拷贝默认关闭**，需 `DMD_WANT_SHM=1` 显式开启。
  这条路驱动侧从未真正启用过（`want_shm` 长期硬编码为 0），只有单元测试走过
  （150 帧、与 TCP 前 20 帧逐字节一致、无 fd 泄漏），真实消费者环境下未验证。
  另一个未实测的风险是浏览器沙箱能否收 `SCM_RIGHTS`
  （Firefox RDD / Chrome GPU 进程都有 seccomp 过滤）。
- **最小化实现**：基于 anland 项目的 libdisplay_daemon 库简化而来，代码简洁易懂
- **进程托管**：daemon 设计为被平台托管的前台进程（DroidSpace 负责启动与守护）

## 架构图

```
┌─────────────────────────────────────────────────────┐
│                Android 设备 (Root)                  │
│                                                     │
│  ┌───────────────────────────────────────────────┐ │
│  │              decode-daemon                    │ │
│  │                                               │ │
│  │  • 监听 Unix socket（--sock，推荐）           │ │
│  │    或 TCP 127.0.0.1:20003（兜底）             │ │
│  │  • 接收 H.264 / HEVC / VP8 / VP9 码流         │ │
│  │  • 使用 MediaCodec 硬件解码                   │ │
│  │  • 返回 NV12 帧（TCP 传输或 memfd 零拷贝）    │ │
│  └───────────────────────┬───────────────────────┘ │
│                          │ Unix socket（bind mount）│
│                          │ 或 TCP 127.0.0.1:20003   │
│                          │                          │
│  ┌───────────────────────┴───────────────────────┐ │
│  │          Linux 容器 (Debian aarch64)          │ │
│  │                                               │ │
│  │  • decode-client (client/)                    │ │
│  │      FFmpeg 解复用 → 发送 NALU                │ │
│  │      接收 NV12 帧 → EGL/GLESv2 渲染或导出 PPM │ │
│  │  • tools/test_decode.py — 协议参考实现        │ │
│  └───────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────┘
```

DroidSpaces 有**两类容器**，namespace 归属不同，这直接决定可用的传输方式：

| | host 型 | NAT 型 |
|---|---|---|
| net namespace | 与宿主**共享**（`4026531937`） | **独立**（如 `4026535650`） |
| IP | 直接持有 `wlan0` 真实地址 | `eth0` 172.28.x.x/16，网关 172.28.0.1 |
| `127.0.0.1:20003` | 可达 | **不可达** |
| abstract socket 可见数 | 31（与宿主一致） | **0** |
| mnt / pid / uts / ipc / cgroup ns | 均隔离 | 均隔离 |

所以 **TCP loopback 与 abstract socket 都依赖共享 net namespace**，只在 host 型
容器可用。NAT 型容器两者皆不通 —— abstract socket 属于 net namespace，
并不是 TCP 的替代品。

**路径式 Unix socket 才是通用解**：它不属于 net namespace，平台把宿主上的
socket 文件 bind mount 进容器即可，两类容器都能用，且鉴权直接靠文件权限，
不必把服务暴露到网络。实测跑通了完整链路 —— 容器 `connect` 成功、
通过 `SCM_RIGHTS` 收到 memfd、`mmap` 读到宿主写入的内容，
一次同时证明三件事：路径式 Unix socket 跨 mount namespace 可用、
`SCM_RIGHTS` 能跨界传 fd、memfd 跨 namespace 可映射。

DroidSpaces 自己的显示通道就是这个模式：宿主
`/data/local/tmp/anland-<hash>.sock` → 容器 `/run/display.sock`
（实测两侧 inode 相同，确认是同一文件）。

⚠️ **两个部署约束 + 一条当前状态**（约束都是实测踩出来的）：

1. **必须挂目录，不能挂单个 socket 文件。** `bind()` 只能创建新 inode，
   而 bind mount 绑的是 **inode 而非路径** —— 挂单文件时 daemon 一重启就换
   inode，容器侧立刻变成 `ECONNREFUSED`，且此时两侧 `stat` 看到的 inode
   可能一致（都是那个孤立 inode），极易误判。挂目录则 inode 稳定：
   宿主 `/data/local/tmp/dmd/` → 容器 `/run/dmd/`，daemon 随便重启都不影响。

2. **daemon 需要合适的 SELinux domain，而现有 domain 都只有一半权限。**
   这是当前**阻塞 Unix socket 通道交付**的唯一问题：

   | 启动身份 | 能 bind socket | 能用 MediaCodec |
   |---|---|---|
   | `su`（`u:r:ksu:s0`） | ✗ 各处 `EACCES` | ✓ |
   | `runcon u:r:droidspacesd:s0` | ✓ | ✗ SIGABRT |

   `droidspacesd` 下崩在 `CCodec::allocate`，tombstone 栈顶
   `Codec2Client::GetServiceNames`，报 `Hardware service manager is not running`
   —— 该 domain 无权访问 hwservicemanager / Codec2 HAL。

   三组对照实验确认这与传输方式无关：`ksu`+TCP 正常、`droidspacesd`+TCP
   同样 SIGABRT、`droidspacesd`+Unix socket+**SELinux permissive** 正常解码。
   也就是说 **Unix socket 通道本身是正确的**，缺的只是一条 allow 规则。

   已排除的替代方案：root 无效（两个 domain 本来都是 uid 0，SELinux 是 MAC
   不看 uid）；DroidSpaces 的 `enable_hw_access=1` 无效（只透传 `/dev` 节点、
   不改 domain，且作用于容器而非宿主侧的 daemon）；`untrusted_app` domain
   走不通（无法执行 `shell_data_file` 标签的二进制，改标签被拒）；
   `selinux_permissive=1` 有效但等于全系统关防护，不可作为交付形态。

   **需要平台补一条规则**：允许 `droidspacesd` 访问 hwservicemanager 与
   Codec2 HAL。DroidSpaces 已为该 domain 配过 `/dev/dri`、`/dev/ashmem`
   的访问权，这属同类工作。

3. **在规则到位前，默认路径仍是 TCP。** 驱动探测不到可用 socket 会自动退回
   TCP（`u:r:ksu:s0` 下正常），行为与 v0.2.0 一致，无退化。

### 性能

1080p 峰值 **194 fps**、4K 峰值 **82 fps**，两者均满足 60fps（富余 3.24× / 1.37×）。
延迟 p50 4.46 ms、p95 9.77 ms。

> 这两个峰值是**端到端单客户端**测得（TCP 通道）。文档别处出现的
> 275.5 / 244.0 fps 是 SHM 与 TCP 的**稳态窗口**对比，口径不同，
> 不能与这里横向比较，也不意味着 194 fps 是硬件上限。原先的瓶颈是 daemon 的单线程串行结构
（不是 TCP 传输也不是硬件解码器），已通过收发分离拆除：
单客户端吞吐提升 20–30%，并支持多客户端并发（4 路合计约 253 fps）。

> **这些是墙钟测量的吞吐上界**（`tools/probe_cost.c` 用 `CLOCK_MONOTONIC`
> 逐帧计时），指"尽快连续解码能跑多快"，**不是实时播放帧率**。
> 浏览器实时播放约 30 fps —— 那受播放节奏限制，不是解码能力上限。
>
> ⚠️ 引用帧率必须说明是墙钟还是内容时长。本项目曾把
> "143 帧 ÷ 5 秒内容时长"当成实时播放帧率，真实只有 6.4 fps。
详见 [性能实测与优化路线](doc/performance-and-roadmap.md)。

### 通信协议

- **输入格式**：`[4字节 NALU 长度 (大端)][NALU 数据]`，NALU 需带起始码（3 字节 `00 00 01` 或 4 字节 `00 00 00 01` 均可）
- **输出格式**：`[4字节 宽度][4字节 高度][4字节 帧大小][4字节 输入单元序号][NV12 帧数据]`，均为大端
  - 第 4 个字段是该帧对应的**输入单元序号**（第几次送数据单元，1 起）：
    daemon 把它写进 `queueInputBuffer` 的 `presentationTimeUs`，
    MediaCodec 原样带到输出帧上。客户端据此精确配对，
    **无需知道解码器按什么顺序出帧**。
  - 仅当格式描述块声明了 `CAP_FRAME_PTS`（见下）才有这个字段；
    旧 daemon 不发，客户端按 3 字段解析。
  - 共享内存模式对应地在 `[槽位][长度]` 之后多一个同义字段。
- **最大单元大小 8MB 只约束上行**：`MAX_FRAME` 仅用于校验客户端送来的数据单元
  （`src/decode-daemon.c:579` 的 `sz > MAX_FRAME`），**下行帧大小没有任何上限检查**。
  客户端不要拿 8MB 去校验下行 —— 4K NV12 单帧 12441600 字节就已超过它，
  照 8MB 判定会把正常的 4K 流误判成协议错误。
- **格式描述块的能力位**：块头第 2 个字（原为保留的 0）声明 daemon 能力，
  `0x1` = `CAP_FRAME_PTS`（帧头带输入单元序号）。旧 daemon 恒为 0。
- **解码器输入超时**：5 秒
- **参数集处理**：服务端识别参数集 NALU（H.264 的 type 7/8，HEVC 的 32/33/34）并累积为 codec-specific data，用 `BUFFER_FLAG_CODEC_CONFIG` 送入，这类 NALU 不产出帧
- **帧收发不对称**：送入 NALU 数 ≠ 返回帧数（解码器排队/重排序），客户端发完须 `shutdown(SHUT_WR)` 触发 flush 才能取全剩余帧
- **长度 0 = 可逆排空请求**（不是数据单元，也不是非法长度）：
  daemon 送 EOS 逼解码器吐出在手的帧，收齐后 `AMediaCodec_flush` 复位并重送 CSD，
  **会话之后仍然可用**。用于打破"消费者等帧、解码器等料"的互等 ——
  浏览器只保持 3 帧在飞，而解码器有 B 帧时要第 4 个输入单元才吐首帧。
  与 `shutdown(SHUT_WR)` 的区别是不作废会话：实测每帧 33.6 ms（满速 29.8 fps），
  而走关闭写端要 155 ms（6.4 fps，因为每帧都得重建会话）。
  排空后 daemon 会重新下发格式描述块。
  选长度 0 承载是因为它原本就被判为非法，老客户端不会发，无需版本协商。

#### 握手（必需）

客户端必须在发送任何数据之前完成握手，声明编解码器、分辨率与传输模式。
daemon 与客户端配套发布，不保留无握手的兼容路径：

```
[4B 魔数 0x444D4400][4B 版本=3][4B codec][4B 宽][4B 高][4B 传输模式]   共 24 字节
```

`codec` 取值：`0`=H.264 `1`=HEVC `2`=VP9 `3`=VP8。
传输模式：`0`=TCP，`1`=共享内存（见下节）。

**版本协商（v3 起）**：daemon 支持 `2..3`，取 `min(3, 客户端版本)`。
v2 旧客户端收到的响应与历史版本逐字节相同；请求更高版本回 `status=1` 断开。
（v0.3.x 及之前按严格相等判定，daemon 与驱动必须同步升级；v3 放宽为区间，
允许两端分别滚动更新。）

不同编码的数据单元切分方式不同，客户端必须按对应规则送数据：

| 编码 | 长度前缀里放什么 | 起始码 | 参数集 |
|------|-----------------|--------|--------|
| H.264 / HEVC | 单个 NALU | **必须带**（3 或 4 字节） | 从 extradata 提取后先送 |
| VP9 / VP8 | 一个完整帧 | **不能带** | 无独立参数集，在关键帧内 |

给 VP8/VP9 数据补 Annex B 起始码会破坏帧内容，解码器会拒绝整个码流。

服务端响应是变长的：

```
[4B status][4B 实际采用的传输模式][4B 名字长度 n（bit31=v3 扩展标记）]
[v3 扩展：16 字节 endpoint dev/ino（仅 status=0 且客户端版本≥3）]
[n 字节名字]
```

`status`：`0`=接受，`1`=版本不支持，`2`=codec 不支持，
`3`=分辨率超出硬件范围（96×96 ~ 8192×4320），`4`=缺少握手。非 0 时随后关闭连接，
且错误响应恒为裸 12 字节。

**endpoint 扩展（v3 新增，inode 校验）**：
`bit31` 置入名字长度字段作标记（真实名字长度远小于 2³¹），随后、名字字节之前
追加 16 字节：`[u32 dev_hi][u32 dev_lo][u32 ino_hi][u32 ino_lo]`（各自大端），
即 daemon 对**自己监听路径** `stat()` 得到的 `st_dev`/`st_ino` 各拆高低两个 u32。
TCP 模式与抽象命名空间模式填 0。

客户端必须拿它和自己 **stat connect 所用路径**的结果对账（注意不能用 fstat
连接 fd —— 那是 socket 匿名 inode）。两者不一致说明解析到的不是 daemon 的
监听端点，典型病因是平台把单个 socket 文件而非目录做 bind mount：daemon 重启
换 inode 后容器侧持死引用。此场景历史上表现为静默退化成软解或断流，极难诊断；
现在客户端应当报错拒绝连接（本仓库两个客户端实现均如此），而不是带病继续跑。

请求 SHM 可能被降级为 TCP（内存不足、交接超时等）。TCP 模式下名字长度为 0。

**注意：`mode=SHM` 只是意向声明，不是保证。** 响应在 memfd 交接**之前**发出，
所以 daemon 可能先宣告 SHM、随后因交接失败（客户端没来领、超时）静默退回 TCP。
客户端**必须自带 fallback**：领取 memfd 失败时要能按 TCP 帧格式继续解析，
不能因为响应说了 SHM 就假定后续一定是槽位消息。
本仓库的 `client/` 与 `tools/test_decode.py` 都按此实现。

#### 共享内存传输（可选，省两次拷贝）

TCP 模式每帧要经过两次内核拷贝（发送进 socket 缓冲、接收再拷出）。
1080p NV12 一帧 3MB，60fps 下就是 180MB/s 的额外内存带宽。

SHM 模式把帧数据放进 `memfd`，socket 只传 20 字节控制消息：

```
[4B 宽][4B 高][4B 0xFFFFFFFE][4B 槽位号][4B 数据长度]
```

握手响应里的名字是一个 **abstract socket**（形如 `dmd-shm-<pid>-<会话id>`），
由 daemon 命名并预先监听。客户端连上去，daemon 用 `SCM_RIGHTS` 把 memfd 交过来，
附带 12 字节 `[槽位数][单槽字节数][池总字节数]`。

之所以用 abstract socket 而非路径形式：容器与 Android 共享 net namespace
（abstract socket 属于 net ns，双向可见），但 mount namespace 是隔离的，
基于路径的 Unix socket 在对侧根本不存在。

之所以由 daemon 命名：客户端无从得知自己是第几个连接。让客户端猜名字必然串台
或连不上 —— 这是实现过程中真实撞到的缺陷。

池布局与归还协议：

```
[控制区 4096 字节][槽位 0][槽位 1][槽位 2][槽位 3]
```

控制区开头每个槽位一个 32 位状态字。daemon 写完数据置 1，
**客户端处理完必须置 0 归还**，否则池耗尽后 daemon 等约 1 秒判定客户端卡死并结束会话。
归还走共享内存而非 socket，不与上行 NALU 流交织。

两个超时不要混淆：**槽位等待约 1 秒**（客户端不归还），
**memfd 交接等待 3 秒**（客户端拿到名字后不来 connect，实测降级发生在 3.0~3.5 s）。

槽位按 **adaptive-playback 上限**（`max(声明宽,1920) × max(声明高,1088)`）计算，
而不是按握手声明的实际分辨率。原因：只按当前分辨率开槽时，
流中途分辨率变大（480p→720p）会超出槽位，SHM 会话被迫终止 ——
同一码流 TCP 解满 120 帧、SHM 只有 60 帧，是实测到的功能退化。
代价是 720p 流也按 1080p 占池（4 × 3133440 ≈ 12 MB），用内存换正确性。

仍存在的边界：分辨率超过 adaptive-playback 声明的上限时依然会终止会话
（报 `帧 N 字节超出槽位 M，需重建池`），但不会越界写、不影响其他会话。

实测收益（1080p，同一码流，两组独立测量）：

| 测法 | TCP | SHM | 提升 |
|------|-----|-----|------|
| 2500 帧全程平均 | 166.8 fps | 194.7 fps | +17% |
| 4500 帧稳态窗口（剔前 3 s） | 244.0 fps | 275.5 fps | +12.9% |

两组差异来自窗口选择：全程平均含 DVFS 爬频段，稳态窗口更接近真实上限；
提升幅度则以稳态窗口的 +12.9% 更可信（SHM 两轮离散 1.0%，TCP 7.2%）。

daemon CPU：763 → 545 ticks / 1800 帧，**-28.6%**。
两种模式解出的 PPM 逐字节完全一致（前 3 帧 NV12 12/12 全等）。
省掉的是内核拷贝；MediaCodec 输出缓冲到共享内存那一次 CPU 拷贝仍然存在，
要去掉需要 dmabuf 零拷贝（见 `doc/performance-and-roadmap.md`）。

#### 格式描述块

服务端用一个**哨兵帧头**引出格式描述块，客户端读到它就更新格式：

```
[4B 0][4B 0][4B 0xFFFFFFFF]                          ← 哨兵帧头
[4B 缓冲宽][4B 缓冲高][4B stride][4B slice_height]
[4B crop_left][4B crop_top][4B crop_right][4B crop_bottom]
```

合法帧大小不可能等于 `0xFFFFFFFF`（4 GiB，远超任何 NV12 帧），所以不存在歧义。
crop 为闭区间，显示宽 = `crop_right - crop_left + 1`。

格式块在**第一帧之前**必定出现一次。**流中途分辨率变化时会再次下发**
（已启用 adaptive-playback，解码器无需重建），客户端必须据此更新
stride 与 crop，否则后续帧会整体错位。

**为什么需要它**：帧头只给缓冲尺寸，而高通 Venus 的输出按 128/32 对齐 ——
1080p 实际输出 1920×1088，末 8 行是填充。客户端拿不到 `stride` 就无法正确定位 UV 平面
（UV 起点是 `stride × slice_height`，不是 `width × height`），
拿不到 crop 就会把填充行当画面内容。宽度非 128 倍数时若按缓冲宽度逐行读取，整幅画面会斜切。

**版本配套**：客户端与 daemon 作为一个整体发布，协议只有一种形态。
服务端窥探前 4 字节，非魔数即以 `status=4` 拒绝连接 ——
这样省掉了兼容分叉，帧路径上没有任何格式判断分支。

> ⚠️ **无鉴权**：服务端仅绑定 loopback，但同设备上任何进程都可连接。由于容器与 Android 共享 net namespace，loopback **不构成隔离边界**。不要在多租户或不可信 App 环境下使用当前版本。

## 编译方法

### 环境要求

- Android NDK r27c 或更高版本
- 支持 ARM64 架构的编译环境
- Android API Level 21+ (MediaCodec NDK 支持)

### 编译步骤

1. **设置 NDK 环境**
   ```bash
   # 下载 Android NDK r27c
   # https://developer.android.com/ndk/downloads
   
   export NDK=/path/to/android-ndk-r27c
   export TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
   export TARGET=aarch64-linux-android
   export API=21
   ```

2. **编译 decode-daemon**
   ```bash
   cd src
   
   # 交叉编译
   $TOOLCHAIN/bin/${TARGET}${API}-clang \
       -O2 -Wall -Wextra \
       -o decode-daemon \
       decode-daemon.c \
       -lmediandk -llog -landroid
   ```

3. **编译选项说明**
   - `-lmediandk`：MediaCodec NDK 库，同时提供 `AMediaCodec_*` 与 `AMediaFormat_*` 符号
   - `-llog`：Android 日志库
   - `-landroid`：Android 基础库

### 快速编译脚本

仓库已提供 `build.sh`，会自动探测 NDK 路径并给出清晰的报错：

```bash
./build.sh                          # 自动探测 NDK
NDK=/path/to/android-ndk ./build.sh  # 显式指定
API=29 ./build.sh                    # 覆盖目标 API level
```

产物为 `build/decode-daemon`（aarch64 PIE 可执行文件）。

## 部署：由 DroidSpace 平台托管

> **变更说明**：早期计划用 KSU/Magisk 模块实现开机自启，`magisk-module/` 目录是
> 那个方案的产物。**该方案已放弃** —— daemon 的启动、重启、守护与日志收集
> 统一交由 DroidSpace 平台托管，本项目不再自带进程管理。
> `magisk-module/` 保留供参考，不再维护，也不作为发布形态。

### daemon 的定位

`decode-daemon` 被设计成一个适合被托管的**前台进程**：

- 前台运行，不自我 daemonize（托管方需要能直接监督子进程）
- 日志走 stderr，由托管方收集
- `SIGTERM` 优雅退出
- 判活**用监听端点而不是 PID**（PID 会反复变化，这是踩过的坑）

### 部署步骤（开发与测试）

```bash
# 1) 构建
./build.sh                     # 产物 build/decode-daemon

# 2) 推送到设备
adb push build/decode-daemon /data/local/tmp/
adb shell chmod 755 /data/local/tmp/decode-daemon

# 3) 启动（监听 127.0.0.1:20003）
adb shell su -c 'nohup /data/local/tmp/decode-daemon 20003 >/data/local/tmp/dd.log 2>&1 &'

# 4) 确认在听 —— 用端口判活，不要看 PID
adb shell 'timeout 3 sh -c "echo > /dev/tcp/127.0.0.1/20003" && echo 在听'
```

⚠️ **adb 端口会变化**。设备重连后端口号可能不同，编译与推送要放在同一条命令里
完成，否则容易出现"改了代码但设备上还是旧二进制"—— 新旧协议不匹配会表现为
`帧头数值不合理`，很容易误判成协议缺陷。

### 生产部署

由 DroidSpace 负责：二进制放置位置、启动时机（需 media 服务就绪）、
崩溃重启、日志收集、健康检查。**接入契约详见
[`doc/platform-integration-contract.md`](doc/platform-integration-contract.md)**
—— 那份文档列出平台必须提供的三件事（bind mount 目录、正确的 SELinux domain
启动、`/dev/dri/renderD128` 透传）、各自的实测依据与验证命令，
以及当前唯一的阻塞项（`droidspacesd` 缺一条访问 Codec2 的 allow 规则）。

## 测试方法

开发与测试阶段直接推送二进制手动启动即可（见上文"部署"一节）。生产部署由 DroidSpace 平台托管。

### 1. 构建并推送 daemon

```bash
./build.sh
adb push build/decode-daemon /data/local/tmp/decode-daemon
adb shell "su -c 'chmod 755 /data/local/tmp/decode-daemon'"
```

### 2. 在 Android 侧启动 daemon

```bash
# 用 setsid 而非 nohup：su -c 下 nohup 常常起不来，disown 也不可用
adb shell "su -c 'cd /data/local/tmp && (setsid ./decode-daemon 20003 </dev/null >decode-daemon.log 2>&1 &)'"

# 确认已监听
adb shell "su -c 'ps -A | grep decode-daemon'"
adb shell "su -c 'cat /data/local/tmp/decode-daemon.log'"   # 应输出 listening on 20003
```

> **省事提示**：Android 的 `/data/local/tmp` 与容器 `/tmp` 是同一个目录
> （bind mount，同一 f2fs 设备）。所以容器里可以直接
> `tail /tmp/decode-daemon.log` 看 daemon 日志，不用 `adb pull`；
> 放测试码流也不用 `scp` 两份。
> 但**别拿它传帧数据** —— 它是磁盘背书的，比 memfd 慢 65%，
> 详见 `doc/verified-platform-facts.md`。

### 3. 在容器内运行测试客户端

测试视频放在**容器内**（客户端负责读文件，daemon 只收网络上的 NALU）：

```bash
python3 tools/test_decode.py 20003 /path/to/test.h264
```

### 4. 实测输出

```
Connected to port 20003
Sent 30 NALUs
Frame 1: 1920x1088 3133440 bytes
Frame 2: 1920x1088 3133440 bytes
...
RESULT: 20 frames decoded from /root/decode-test/test1080.h264
```

注意两处容易误解的地方：

- **输出是 1920x1088，不是 1920x1080**。高通 Venus 解码器的高度按 16 对齐，每帧 `1920*1088*1.5 = 3133440` 字节 NV12。客户端必须以返回的 w/h 为准，按实际显示尺寸裁剪。
- **发送的 NALU 数与收到的帧数不相等**。解码器有排队与重排序延迟，且 SPS/PPS 不产出帧；客户端发完需 `shutdown(SHUT_WR)` 触发 flush 才能取全剩余帧。

详见 [平台实测事实](doc/verified-platform-facts.md)。

### 5. 手动测试

```bash
# 在 Android 设备上启动 daemon（开发测试用；生产由 DroidSpace 托管）
adb shell
su
./decode-daemon 20003

# 在容器内连接测试
nc -zv 127.0.0.1 20003
```

daemon 的命令行选项：

```
用法: decode-daemon [端口] [--sock 路径] [-v|-q]
  端口          监听的 TCP 端口（默认 20003，仅绑定 127.0.0.1）
  --sock 路径   改为监听该路径的 Unix socket（推荐）
                传目录时在其中建 decode.sock
  -v            逐帧调试日志
  -q            只输出错误
```

**推荐用法（Unix socket，两类容器都支持）**：

```bash
# 宿主侧：注意必须用 droidspacesd domain，否则 bind() 得到 EACCES
adb shell su -c 'runcon u:r:droidspacesd:s0 \
  /data/local/tmp/decode-daemon --sock /data/local/tmp/dmd'
# 成功时输出：
#   --sock 是目录，实际监听 /data/local/tmp/dmd/decode.sock
#   listening on /data/local/tmp/dmd/decode.sock

# 平台把该目录 bind mount 进容器（宿主 /data/local/tmp/dmd → 容器 /run/dmd）
# 容器侧：
DMD_ENDPOINT=unix:/run/dmd/decode.sock ffmpeg -hwaccel vaapi ...
# 或不设 DMD_ENDPOINT，驱动会自动探测 /run/dmd/decode.sock
```

⚠️ **传目录而非单个 socket 文件**：`bind()` 只能创建新 inode，而 bind mount
绑的是 **inode 而非路径** —— 挂单文件时 daemon 一重启容器侧就 `ECONNREFUSED`。
挂目录则目录 inode 稳定，daemon 随便重启都不影响。

⚠️ **Enforcing 下这条路当前走不通**：`droidspacesd` domain 能 bind 但无权访问
Codec2，会在 `CCodec::allocate` 处 SIGABRT。详见"已知问题"。

**兜底用法（TCP，仅 host 型容器）**：

```bash
adb shell su -c '/data/local/tmp/decode-daemon 20003'
```

默认级别只输出连接与会话统计。排查解码问题时用 `-v` 看逐帧信息
（逐帧日志会带来可观的 sys 开销，压测时务必保持默认或 `-q`）。

## 已知问题

### 0. Unix socket 通道在 SELinux Enforcing 下不可用（当前最大阻塞项）

现有两个启动身份**各只有一半权限**：

| 启动身份 | 能 `bind()` socket | 能用 MediaCodec |
|---|---|---|
| `su`（`u:r:ksu:s0`） | ✗ 各处 `EACCES` | ✓ |
| `runcon u:r:droidspacesd:s0` | ✓ | ✗ SIGABRT |

`droidspacesd` 下崩在 `CCodec::allocate`，tombstone 栈顶
`Codec2Client::GetServiceNames`，报 `Hardware service manager is not running`。

三组对照实验确认这与传输方式**无关**：

| 身份 + 传输 | 结果 |
|---|---|
| `ksu` + TCP | 正常解码（但该 domain 无权 bind） |
| `droidspacesd` + TCP | **同样 SIGABRT** ← 证明与 Unix socket 无关 |
| `droidspacesd` + Unix socket + **SELinux permissive** | **正常解码** |

所以**通道本身是正确的**（permissive 下十二条流逐字节比对 12/12 一致），
缺的只是一条 allow 规则：允许 `droidspacesd` 访问 hwservicemanager 与
Codec2 HAL。

已排除的替代方案：root 无效（两个 domain 本来都是 uid 0，SELinux 是 MAC
不看 uid）；DroidSpaces `enable_hw_access=1` 无效（只透传 `/dev` 节点、
不改 domain）；`untrusted_app` 走不通（无法执行 `shell_data_file` 标签的
二进制，`chcon` 被拒）；另外 11 个 domain 均无法同时满足"可切入"与
"可 bind"；`selinux_permissive=1` 有效但那是把宿主 SELinux 整体切成
permissive，不可作为交付形态。

**规则到位前驱动会自动退回 TCP，行为与 v0.2.0 一致，无退化。**
详见 [`doc/platform-integration-contract.md`](doc/platform-integration-contract.md) §2.2。

### 0b. memfd 零拷贝开启后单连接会断

`DMD_WANT_SHM=1` 时实测：`xfer=1`、4 槽 memfd 挂载与 `帧回传=SHM` 握手都成功，
随后该连接断开（`Broken pipe` / `Connection reset by peer`），
118 个输入单元只取回 25 帧。

但**不会打死 daemon** —— 无新 tombstone、socket 继续 `accept`、
事后非 SHM 路径复测逐字节一致。所以属该连接的错误处理问题，
不是进程级崩溃 —— 排查方向应放在该连接的错误返回与提前 `close`，
而不是"daemon 崩了"。具体根因未定位，故默认关闭。

> 与"已知问题 0"区分：那条是 SELinux domain 导致**任何**解码都不成，
> 与传输方式无关；这条是 SHM 帧交付路径特有的。两者曾被我误当成同一件事。

另外零拷贝的 memfd 交接走的是**另开的 abstract socket**，属 net namespace，
所以它**只在 host 型容器可能可用，NAT 型必然降级**。

### 1. 共享内存池不支持超出上限的分辨率
槽位已按 adaptive-playback 上限（≥1920×1088）预留，覆盖了常见的
流内分辨率变化。但若分辨率涨到超过该上限（例如声明 1080p 却出现 4K），
仍只能报错结束会话 —— 重建池要重走 memfd 创建与 SCM_RIGHTS 交接，
协议上需新增一类控制消息，目前未做。
规避：握手按预期最大分辨率声明，或改用 TCP 模式（无此限制）。

### 2. 分辨率协商
已完成：握手声明初始尺寸，服务端回传 stride / slice_height / 显示裁剪区域，
并已启用 `adaptive-playback` —— 流中途分辨率变化时会重新下发格式块，
解码器无需重建。实测 720p→480p 拼接流可正确切分（60 帧 1280×720 + 60 帧 640×480）。

仍未覆盖：
- 分辨率**上升**超过握手声明的 `MAX_WIDTH`/`MAX_HEIGHT`（取声明值与 1080p 的较大者）时，
  解码器仍需内部重配，未验证该路径
- 帧率、色彩空间、位深的变化未处理（只跟踪尺寸与 crop）

### 3. 编解码器支持范围
- **H.264 / HEVC / VP9 / VP8 四种均已真机端到端验证。**
  VP9 / VP8 的解码帧数与源流帧数**精确匹配**；H.264 / HEVC 那轮当时
  没有记录源流帧数，所以严格说只有"解码出 150 帧、无错误"，
  **不能声称帧数匹配**：

  | 编码 | 分辨率 | 源流帧数 | 解码帧数 | 是否可判定匹配 |
  |------|--------|----------|----------|---------------|
  | H.264 | 1080p | 未记录 | 150 | ✗ 缺基准 |
  | HEVC | 720p | 未记录 | 150 | ✗ 缺基准 |
  | VP9 | 720p | 120 | 120 | ✓ |
  | VP8 | 720p | 120 | 120 | ✓ |

  > H.264 / HEVC 的正确性由另一条更强的证据支撑：**十二条流与软解基线
  > 逐字节一致**（含 3000 帧与 1500 帧长流）。逐字节比对比帧数计数严格得多，
  > 所以这里缺的只是这张表的完整性，不是能力上的疑点。

- MPEG2 硬件支持但协议未列入（无实际需求）
- 未覆盖：VP9 10-bit、HEVC Main10、H.264 High 10/4:2:2 等高位深与非 4:2:0 采样

### 4. 并发上限低于硬件能力
- 已支持多客户端并发：每个连接一个会话线程，各持独立 MediaCodec 实例
- 实测 4 路同时解码 1080p 合计约 253 fps
- 当前上限 `MAX_CLIENTS` 设为 8，硬件支持 16，尚未测到上限行为
- 超出上限时直接关闭新连接，客户端侧只看到连接被断，没有明确的拒绝原因

### 5. 帧数据仍有一次 CPU 拷贝（已评估，不打算消除）
SHM 模式已省掉 TCP 的两次内核拷贝（吞吐 +17%，daemon CPU −28.6%），
但 MediaCodec 输出缓冲 → 共享内存这一次拷贝仍在。

实测这次拷贝单帧 0.227 ms（13137 MB/s），按 194 fps 折算只占约
**4.4% 的单核 CPU**。要去掉它必须拿到输出缓冲的 dmabuf fd，
而 NDK 公开 API 没有这个入口（`AHardwareBuffer` 只能 `lock` 出 CPU 指针，
`sendHandleToUnixSocket` 跨容器不可用），只能依赖 `libui`/gralloc 私有符号，
会绑死特定 Android 版本的 C++ ABI。

收益不到一成、代价是版本脆弱性，因此**暂不实施**。
详细核算与 UBWC 结论修正见 `doc/performance-and-roadmap.md`。

### 6. TCP 通信无鉴权
- 明文 TCP，无认证、无加密
- 仅绑定 loopback (`INADDR_LOOPBACK`)，但**同设备上任何进程（含普通 App）都能连接并使用该解码服务**
- 由于容器与 Android 共享 net namespace，loopback 不构成隔离边界

### 7. 错误处理
- 资源释放路径已补齐（配置/启动失败均走统一的 goto 清理链）
- 客户端正常关闭写端与异常断开已能区分，会话统计正常落日志
- 仍缺少的是重试与降级：解码器创建失败时直接结束会话，不尝试备选解码器

### 8. profile / level 未协商
- 握手已能声明 codec 与分辨率，但 profile、level 仍由解码器自行推断
- 依赖码流内 SPS 携带的信息，对绝大多数流可行
- 特殊 profile（如 High 10、4:2:2）未验证

## 开发信息

- **源码基于**：anland 项目的 libdisplay_daemon 库
- **技术栈**：Android NDK (C), Python 测试脚本
- **通信方式**：路径式 Unix socket（推荐，两类容器都支持）或 TCP socket（IPv4 loopback，仅 host 型容器）
- **目标平台**：Android ARM64 设备

## 相关文档

- [容器侧客户端](client/README.md) - decode-client 构建、用法与实现要点
- [平台实测事实](doc/verified-platform-facts.md) - 真机验证的 namespace 关系、协议行为、硬件能力清单
- [性能实测与优化路线](doc/performance-and-roadmap.md) - 压测数据、瓶颈归因、零拷贝可行性与硬约束
- [为什么不直接用 V4L2](doc/why-not-v4l2.md) - 容器内 `/dev/video32` 实测不可用的取证结论
- [VAAPI Proxy 架构调研报告](doc/vaapi-mediacodec-proxy-research.md) - 详细的 VA-API 代理驱动实现方案
- [Magisk 模块文档](magisk-module/README.md) - ⚠️ 已废弃方案，保留供参考

## 许可证

本项目遵循 Apache 2.0 许可证。

## 贡献

欢迎提交 Issue 和 Pull Request。在贡献代码前，请确保：
1. 代码符合项目风格
2. 添加必要的注释
3. 更新相关文档
4. 测试通过