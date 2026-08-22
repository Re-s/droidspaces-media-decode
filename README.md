# DroidSpaces Media Decode Daemon

Android MediaCodec 硬件解码代理服务，为 Linux 容器提供视频硬解能力。

## 项目简介

本项目提供了一个运行在 Android 设备上的 MediaCodec 硬件解码守护进程，通过 TCP socket 将硬件解码能力暴露给 Linux 容器（如 DroidSpaces 中的 Debian 容器）。这使得容器内的应用程序能够利用 Android 设备的 GPU 硬件解码能力，实现高效的视频播放。

### 核心特性

- **硬件加速解码**：利用 Android MediaCodec API 进行 H.264 硬件解码
- **TCP 跨 namespace 通信**：容器与 Android 宿主共享 net namespace 但 mount namespace 独立，因此基于路径的 Unix socket 不可用，TCP loopback 可直接互通
- **最小化实现**：基于 anland 项目的 libdisplay_daemon 库简化而来，代码简洁易懂
- **KSU/Magisk 模块**：提供发布用的 Magisk 模块以支持开机自启（开发测试阶段无需安装，直接推送二进制运行即可）

## 架构图

```
┌─────────────────────────────────────────────────────┐
│                Android 设备 (Root)                  │
│                                                     │
│  ┌───────────────────────────────────────────────┐ │
│  │         decode-daemon (TCP 服务端)            │ │
│  │                                               │ │
│  │  • 监听 TCP 端口 (默认 20003)                │ │
│  │  • 接收 H.264 NALU 数据                      │ │
│  │  • 使用 MediaCodec 硬件解码                   │ │
│  │  • 返回解码后的 NV12 帧数据                  │ │
│  └───────────────────────┬───────────────────────┘ │
│                          │ TCP (127.0.0.1:20003)    │
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

容器与 Android 宿主**共享 net namespace**（实测 inode 均为 `4026531937`），
所以 TCP loopback 直接互通；而 mount namespace 独立，基于路径的 Unix socket 不可用。

### 性能

1080p 峰值 **194 fps**、4K 峰值 **82 fps**，两者均满足 60fps（富余 3.24× / 1.37×）。
延迟 p50 4.46 ms、p95 9.77 ms。原先的瓶颈是 daemon 的单线程串行结构
（不是 TCP 传输也不是硬件解码器），已通过收发分离拆除：
单客户端吞吐提升 20–30%，并支持多客户端并发（4 路合计约 253 fps）。
详见 [性能实测与优化路线](doc/performance-and-roadmap.md)。

### 通信协议

- **输入格式**：`[4字节 NALU 长度 (大端)][NALU 数据]`，NALU 需带起始码（3 字节 `00 00 01` 或 4 字节 `00 00 00 01` 均可）
- **输出格式**：`[4字节 宽度][4字节 高度][4字节 帧大小][NV12 帧数据]`，均为大端
- **最大单元大小 8MB 只约束上行**：`MAX_FRAME` 仅用于校验客户端送来的数据单元
  （`src/decode-daemon.c:451`），**下行帧大小没有任何上限检查**。
  客户端不要拿 8MB 去校验下行 —— 4K NV12 单帧 12441600 字节就已超过它，
  照 8MB 判定会把正常的 4K 流误判成协议错误。
- **解码器输入超时**：5 秒
- **参数集处理**：服务端识别参数集 NALU（H.264 的 type 7/8，HEVC 的 32/33/34）并累积为 codec-specific data，用 `BUFFER_FLAG_CODEC_CONFIG` 送入，这类 NALU 不产出帧
- **帧收发不对称**：送入 NALU 数 ≠ 返回帧数（解码器排队/重排序），客户端发完须 `shutdown(SHUT_WR)` 触发 flush 才能取全剩余帧

#### 握手（必需）

客户端必须在发送任何数据之前完成握手，声明编解码器、分辨率与传输模式。
daemon 与客户端配套发布，不保留无握手的兼容路径：

```
[4B 魔数 0x444D4400][4B 版本=2][4B codec][4B 宽][4B 高][4B 传输模式]   共 24 字节
```

`codec` 取值：`0`=H.264 `1`=HEVC `2`=VP9 `3`=VP8。
传输模式：`0`=TCP，`1`=共享内存（见下节）。

**版本必须精确等于 2**：daemon 按严格相等判定（`src/decode-daemon.c:363`），
不接受更低的版本号，不匹配直接回 `status=1` 并断开。

不同编码的数据单元切分方式不同，客户端必须按对应规则送数据：

| 编码 | 长度前缀里放什么 | 起始码 | 参数集 |
|------|-----------------|--------|--------|
| H.264 / HEVC | 单个 NALU | **必须带**（3 或 4 字节） | 从 extradata 提取后先送 |
| VP9 / VP8 | 一个完整帧 | **不能带** | 无独立参数集，在关键帧内 |

给 VP8/VP9 数据补 Annex B 起始码会破坏帧内容，解码器会拒绝整个码流。

服务端响应是变长的：

```
[4B status][4B 实际采用的传输模式][4B 名字长度 n][n 字节名字]
```

`status`：`0`=接受，`1`=版本不支持，`2`=codec 不支持，
`3`=分辨率超出硬件范围（96×96 ~ 8192×4320），`4`=缺少握手。非 0 时随后关闭连接。

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

槽位大小按握手声明的分辨率算（宽对齐 128、高对齐 32，再乘 1.5），
1080p 为 3133440 字节、720p 为 1413120 字节。

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

## KSU/Magisk 模块安装方法

### 安装要求

- 已安装 Magisk 20.0+ 或 KernelSU
- Android 10.0+ (API 29+)
- 支持 MediaCodec 硬件解码的设备
- ARM64 架构

### 安装步骤

1. **准备模块包**
   ```bash
   # 进入 magisk-module 目录
   cd magisk-module
   
   # 确保 decode-daemon 二进制文件已编译并放置在此目录
   # 可以从 src/ 目录复制编译好的文件
   cp ../src/decode-daemon .
   
   # 打包为 ZIP 文件
   zip -r ../decode-daemon-module.zip . -x "*.git*"
   ```

2. **安装模块**
   - 打开 Magisk Manager 应用
   - 点击"模块" → "从本地安装"
   - 选择生成的 `decode-daemon-module.zip` 文件
   - 等待安装完成，重启设备

3. **验证安装**
   ```bash
   # 检查模块是否激活
   magisk --list
   
   # 检查服务是否运行
   ps -ef | grep decode-daemon
   ```

### 模块配置

- **监听端口**：默认 TCP `127.0.0.1:20003`，可在 `service.sh` 中修改 `PORT`
- **日志文件**：`/data/local/tmp/decode-daemon.log`
- **PID 文件**：`/data/local/tmp/decode-daemon.pid`
- **SELinux**：daemon 以 KernelSU root（`u:r:ksu:s0`）运行，实测无需额外策略；`sepolicy.rule` 中多数规则是早期 Unix socket 方案的遗留，待清理

模块设计细节与防 bootloop 要求见 [模块文档](magisk-module/README.md)。

## 测试方法

开发与测试阶段**不需要安装 KSU/Magisk 模块**，直接推送二进制手动启动即可。模块只是最终发布形态。

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
# 在 Android 设备上启动 daemon（如果未使用 Magisk 模块）
adb shell
su
./decode-daemon 20003

# 在容器内连接测试
nc -zv 127.0.0.1 20003
```

daemon 的命令行选项：

```
用法: decode-daemon [端口] [-v|-q]
  端口   监听的 TCP 端口（默认 20003，仅绑定 127.0.0.1）
  -v     逐帧调试日志
  -q     只输出错误
```

默认级别只输出连接与会话统计。排查解码问题时用 `-v` 看逐帧信息
（逐帧日志会带来可观的 sys 开销，压测时务必保持默认或 `-q`）。

## 已知问题

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
- **H.264 / HEVC / VP9 / VP8 四种均已真机端到端验证**，帧数与源流精确匹配：

  | 编码 | 分辨率 | 源流帧数 | 解码帧数 |
  |------|--------|----------|----------|
  | H.264 | 1080p | — | 150 |
  | HEVC | 720p | — | 150 |
  | VP9 | 720p | 120 | 120 |
  | VP8 | 720p | 120 | 120 |

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
- **通信方式**：TCP socket (IPv4 loopback)
- **目标平台**：Android ARM64 设备

## 相关文档

- [容器侧客户端](client/README.md) - decode-client 构建、用法与实现要点
- [平台实测事实](doc/verified-platform-facts.md) - 真机验证的 namespace 关系、协议行为、硬件能力清单
- [性能实测与优化路线](doc/performance-and-roadmap.md) - 压测数据、瓶颈归因、零拷贝可行性与硬约束
- [为什么不直接用 V4L2](doc/why-not-v4l2.md) - 容器内 `/dev/video32` 实测不可用的取证结论
- [VAAPI Proxy 架构调研报告](doc/vaapi-mediacodec-proxy-research.md) - 详细的 VA-API 代理驱动实现方案
- [Magisk 模块详细文档](magisk-module/README.md) - 模块安装和配置说明

## 许可证

本项目遵循 Apache 2.0 许可证。

## 贡献

欢迎提交 Issue 和 Pull Request。在贡献代码前，请确保：
1. 代码符合项目风格
2. 添加必要的注释
3. 更新相关文档
4. 测试通过