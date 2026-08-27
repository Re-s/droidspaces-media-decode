# 平台实测事实（Verified Platform Facts）

本文只记录**在真机上实测确认**的事实，用于替代文档中的推测性描述。
每条都标注取证方式。未经验证的内容不写入本文。

- 测试设备：小米平板 5（代号 `nabu`），骁龙 865（SM8150 / Adreno 640）
- Android 13（SDK 33），内核 `4.14.336-Kuugo-v1.0-260728`，aarch64
- Root：KernelSU `ksud 3.3.0`，daemon 运行于 SELinux context `u:r:ksu:s0`
- 容器：DroidSpaces 内的 Debian 13 (trixie) aarch64
- 验证日期：2026-08-22

---

## 1. 跨边界通信：三条通道与各自的适用范围

> ⚠️ **本节在 v0.3.0 大幅订正。** 早前版本把"net namespace 共享"当作
> DroidSpaces 容器的普遍事实，并据此断言"abstract socket 两侧可见、
> 路径式 Unix socket 不可用"。实测证明**那只对 host 型容器成立**，
> 而且结论正好相反 —— 路径式 Unix socket 才是唯一通用的通道。

### 1.1 DroidSpaces 有两类容器，namespace 归属不同

容器类型由 `/data/local/Droidspaces/Containers/<name>/container.config`
里的 `net_mode` 决定（`host` 或 `nat`）。两者实测差异：

| | host 型 | NAT 型 |
|---|---|---|
| net namespace | **共享** 宿主初始 ns `4026531937` | **独立**（实测过 `4026535650`、`4026535651`） |
| IP | 直接持有 `wlan0` 真实地址（如 `192.168.17.96`） | `eth0` `172.28.x.x/16`，网关 `172.28.0.1` |
| `127.0.0.1:20003` 连宿主 | 可达 | **不可达** |
| abstract socket 可见数 | 31（与宿主一致） | **0** |
| mnt / pid / uts / ipc / cgroup | 均独立 | 均独立 |

> **pid ns 独立 ≠ 互不可见**：宿主能看到容器进程，反之不行。
> 这是宿主侧监控方案成立的根据，详见 §1.1.5。
| `/dev/dri/renderD128` | 有 | 需平台透传（可配置） |

宿主侧 `ds-br0`（`172.28.0.1/16`）加 veth 对承载 NAT 型容器的网络，
桥本身在宿主 netns 上，所以 NAT 容器能经网关访问宿主服务 ——
但那是**转发**，不是共享网络栈。

> **易错点 1**：NAT 型容器"能访问宿主路由"与"共享 net namespace"是两回事。
> 前者靠 NAT 转发，后者是同一个网络栈。判据看 `readlink /proc/self/ns/net`
> 的 inode 与 `grep -c @ /proc/net/unix` 的可见数（NAT 型为 0）。
>
> **易错点 2**：NAT 型容器的 netns inode **不是固定值** —— 容器每次重启都会
> 变（本项目先后实测到 `4026535651` 与 `4026535650`）。文档各处引用的具体
> 数字只是取证快照，**判断依据应当是"与宿主的 `4026531937` 是否相同"**，
> 而不是去比对某个记下来的值。宿主侧那个是内核初始 namespace，才是稳定的。

### 1.1.5 pid namespace 独立但**可见性是单向的**（易错）

上表把 pid ns 记为"均独立"，这没错但不完整 —— 独立不等于互相不可见。
实测方向性：

| 方向 | 结果 |
|---|---|
| 宿主 → 容器进程 | ✅ **可见**，能列出容器内的 firefox / chrome 并读其 `/proc/<pid>/stat` |
| 容器 → 宿主进程 | ❌ 不可见，容器内 `pidof decode-daemon` 查不到宿主上的 daemon |

取证：同一个 Firefox RDD 进程，宿主侧 `pgrep -f 'rdd$'` 得到 **26043**，
容器内同样命令得到 **21381** —— 两个视角的同一进程，PID 不同。
拿宿主那个号读 `cmdline` 能取到完整的容器内命令行
（`/usr/lib/firefox-esr/firefox-esr -contentproc … 3 rdd`）。

原因：容器的 pid ns 是宿主 ns 的**子** ns。子 ns 里的进程在父 ns 中同样有
一个 pid（只是号不同），反之父 ns 的进程在子 ns 里根本没有编号。

**这条事实的用处**：它是"宿主侧监控容器内解码"这一整类方案成立的根据。
容器内浏览器走 VA-API 解码时不经 Android 媒体栈，`media.metrics` 与 HAL CPU
都测不到，但既然宿主能直接读到那些进程的 `/proc/<pid>/stat`，
就能在宿主侧完成来源归因，**不需要在容器内驻留任何脚本**。

> ⚠️ 本项目在这里栽过：`ksu-module/export-decode-status.sh` 的注释写着
> "容器内运行"，实际是被 KSU 的 `service.sh` 在**宿主侧**启动的 ——
> 它的 shebang 是 `/system/bin/sh`（容器内无此路径），依赖的
> `pgrep`/`pidof`/`awk` 也都是 `/system/bin` 下的 Android 版本，
> 在容器里根本跑不起来。那句注释误导了一整轮排查。
> 该脚本已删除，职责搬进 dshmon APK。

附带一条权限事实：宿主 `/proc` 挂载带 **`hidepid=2`**
（`proc /proc proc rw,relatime,gid=3009,hidepid=2 0 0`），
所以普通 Android 应用看不到别的进程 —— 宿主侧做进程归因**需要 root**。
不需要 root 的替代信号见本文 §6.5（`msm_vidc` 中断计数）。

### 1.2 三条通道的适用范围

| 通道 | 依赖 | host 型 | NAT 型 |
|---|---|---|---|
| TCP `127.0.0.1:20003` | **net namespace 共享** | 可用 | **不可用** |
| abstract socket（`@` 前缀） | **net namespace 共享** | 可用 | **不可用** |
| **路径式 Unix socket** | 一个 bind mount | **可用** | **可用** |

⚠️ **abstract socket 不是 TCP 的替代品。** 它虽然不落文件系统、绕开了
mount namespace，但它属于 **net namespace** —— 所以在 NAT 型容器里
和 TCP 一起失效（实测可见数 0）。早前文档把它当作"跨界通用方案"是错的。

**路径式 Unix socket 才是通用解**：它不属于任何 namespace 边界内的抽象名字空间，
靠 bind mount 让两侧看到同一个 inode 即可。v0.3.0 起这是**首选通道**
（`--sock` / `DMD_ENDPOINT=unix:...`，见 §1.4）。

DroidSpaces 自己的显示通道就是这个模式：宿主
`/data/local/tmp/anland-<hash>.sock` → 容器 `/run/display.sock`，
实测两侧 inode 相同（`1279492` / `1298133`，每容器一个）。

### 1.3 mount namespace 独立的后果

容器默认看不到 Android 的 `/data`（实测容器内 `/data` 只有 `local/` 一个空壳，
`nsenter -t 1 -m -- ls /data/adb` 报"没有那个文件或目录"）。所以路径式
Unix socket **必须靠 bind mount** 才能跨界 —— 同一路径在对侧默认不指向
同一个 socket 节点。这条限制依然成立，只是解法从"改用 abstract socket"
变成了"让平台挂一个 bind mount"。

### 1.4 memfd 零拷贝的通道归属（易错）

SHM（`DMD_XFER_SHM`）的 memfd 交接**不走**控制通道，而是 daemon
另开一个 **abstract** socket（`src/decode-daemon.c` 里 `sun_path[0] = 0` 处），
驱动再去连它。

所以**零拷贝只在 host 型容器可能可用，NAT 型必然降级** —— 即使控制通道
已经是路径式 Unix socket 也一样。

> **教训**：控制通道能跨 netns，**不等于** memfd 交接通道也跨得过去。
> 要让零拷贝在 NAT 容器可用，需把交接改走同一条路径式 Unix socket
> （在已有连接上传 `SCM_RIGHTS`，不必另开 socket）。那是独立改动，尚未实施。

SHM 目前**默认关闭**（需 `DMD_WANT_SHM=1`）。实测开启后单个连接会断
（118 单元只取回 25 帧），但不会打死 daemon。浏览器沙箱能否收
`SCM_RIGHTS` 亦**未验证** —— 已验证的只是 ffmpeg（无沙箱）下可用。

#### 例外：存在一个双向共享目录

mount namespace 隔离**不等于完全不共享**。实测容器 `/proc/self/mountinfo` 里有一条
bind mount：

```
19994 19989 253:33 /local/tmp /tmp rw,nosuid,nodev,noatime - f2fs /dev/block/dm-33
```

即 Android 的 `/data/local/tmp` 被 bind 到容器 `/tmp`，同一个 f2fs 设备（`dm-33`），
两侧 `stat -f` 的文件系统 ID 一致（`fd2100000000`）。双向读写实测均通：
Android 写的文件容器立即可读，反之亦然。

这解释了一个容易困惑的现象：用 `adb shell` 看 `/data/local/tmp` 时，
会列出 `plasma-csd-generator.*`、`dbus-*`、`kwinrc` 这类明显属于容器 KDE 会话的文件
—— 它们确实是同一个目录。

**用途与边界**：
- ✅ 适合控制面：daemon 日志、测试码流、PPM 导出。容器可直接
  `tail /tmp/decode-daemon.log` 读 Android daemon 的日志，省掉 `adb pull` / `scp`。
- ❌ 不适合帧数据：这个共享点是 **f2fs 磁盘背书**的。实测写入 3133440 字节
  单帧 0.374 ms（7998 MB/s），比 memfd 的 0.227 ms（13137 MB/s）**慢 65%**，
  且带来页缓存回写压力。60 fps 下帧流量约 180 MB/s，不该走文件系统。
- ❌ 内存背书的 tmpfs 不跨边界：容器内 `/dev/shm`（12669 MB/s）、`/run`（14325 MB/s）
  速度与 memfd 相当，但它们是容器 mount namespace 内部的挂载点，
  **Android 侧挂载表里根本不存在**，Android 进程无法访问。

因此帧传输仍须走 memfd + `SCM_RIGHTS`：传递的是 fd 而非路径，
天然绕开文件系统命名空间的不一致。
- **net namespace（仅 host 型容器）共享** → TCP `127.0.0.1` 两侧直接互通，
  无需端口转发。这是 v0.2.0 及更早版本的唯一通道，现在是**兜底通道**。
  NAT 型容器下不可达（见 §1.1），那里必须走路径式 Unix socket。

> ⚠️ **订正**：早前此处写着"文档中『Socket 路径 `/tmp/anland/decode.sock`』
> 属于早期 Unix socket 方案的遗留，与当前 TCP 实现不符"。
>
> 那个判断已被 v0.3.0 推翻 —— 路径式 Unix socket 不是遗留方案，而是
> **当前首选通道**（`--sock` / `DMD_ENDPOINT=unix:...`）。它是唯一对
> host 型与 NAT 型容器都成立的通道，且鉴权靠文件权限而非把服务暴露到网络。
> 当年那条记录的真正问题只是**路径写错了**（实际由平台 bind mount 决定，
> 默认 `/run/dmd/decode.sock`），不是方案方向错。

## 2. 端到端解码链路已验证可用

测试方式：`build.sh` 交叉编译后 `adb push` 到 `/data/local/tmp/`，用 `su -c` 手动启动，容器内运行 `src/test_decode.py`。

（早期计划用 KSU/Magisk 模块做开机自启，该方案**已放弃** —— daemon 改由 DroidSpace 平台托管。手动启动现在是开发测试的标准方式，不再是"临时替代"。）

```
Connected to port 20003
Sent 30 NALUs
Frame 1: 1920x1088 3133440 bytes
...
RESULT: 20 frames decoded from /root/decode-test/test1080.h264
```

帧数据经统计验证为**真实图像**而非填充数据：Y 平面取值 16–235，逐行 stdev 45–78，色度平面非中性灰（mean 126.7 / stdev 71.1）。

## 3. 输出分辨率是 1920x1088，不是 1920x1080

输入 1920x1080 的 H.264 流，解码器返回 **1920x1088**，每帧 `3133440` 字节（= 1920 × 1088 × 1.5，NV12）。

高度按 16 对齐是高通 Venus 解码器的规范行为，设备自身的 `/vendor/etc/media_codecs.xml` 里也直接以 `1920x1088` 作为性能点标注：

```xml
<Limit name="performance-point-1920x1088" range="480" />
```

**客户端必须以解码器返回的 w/h 为准处理数据，并按实际显示尺寸裁剪**，不能假设等于输入分辨率。这也说明源码中硬编码 `1920x1080` 的 `AMediaFormat` 只是初始配置值，真实尺寸需从 `AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED` 事件读取（现有代码已正确处理这一点）。

## 4. 输入 NALU 数与输出帧数不是 1:1

同一次会话的服务端日志：

```
loop end: 40 nalu, 36 in, 24 out
FLUSH ... (8 帧)
total: 32 out
```

- 送入 40 个 NALU，其中 4 个是 SPS/PPS（被累积为 CSD，不产帧），36 个入队
- 主循环内只取出 24 帧——解码器有内部排队与重排序延迟
- 客户端关闭写端后，服务端 flush 阶段补出 8 帧

**客户端不能假设"发一个 NALU 收一帧"**，也必须在发完后走 flush 流程（`shutdown(SHUT_WR)`）才能取全所有帧，否则丢帧。

## 5. 设备硬件解码能力清单

来自 `/vendor/etc/media_codecs.xml` 与 `media_codecs_c2.xml`：

| 编解码器 | MIME | 硬件解码器 |
|---------|------|-----------|
| H.264 | `video/avc` | `OMX.qcom.video.decoder.avc` |
| HEVC | `video/hevc` | `OMX.qcom.video.decoder.hevc`、`c2.qti.hevc.decoder` |
| VP9 | `video/x-vnd.on2.vp9` | `OMX.qcom.video.decoder.vp9`、`c2.qti.vp9.decoder` |
| VP8 | `video/x-vnd.on2.vp8` | `OMX.qcom.video.decoder.vp8`、`c2.qti.vp8.decoder` |
| MPEG2 | `video/mpeg2` | `OMX.qcom.video.decoder.mpeg2` |

H.264 / HEVC 解码器规格（两者一致）：

- 分辨率：`96x96` 至 `8192x4320`
- 帧率上限：480fps；性能点 3840x2160@120、4096x2304@60
- 码率上限：220 Mbps
- 并发实例：最多 16
- 支持 `adaptive-playback`（流内动态分辨率切换）

结论：**扩展 HEVC/VP9 支持不受硬件限制**，仅是服务端 MIME 配置与客户端协议协商的工作量。

## 6. 容器内 V4L2 编解码节点状态

容器内 `/dev/video*` 已直通（属主 `root:droidspaces-gpu`，容器 root 在该组内）：

| 节点 | driver | card | 说明 |
|------|--------|------|------|
| `/dev/video0`、`video1` | — | — | 打开返回 `EBUSY`，已被占用 |
| `/dev/video2` | `sde_rotator` | `sde_rotator` | 显示旋转器，非编解码 |
| `/dev/video32` | `msm_vidc_driver` | `msm_vidc_vdec` | **解码器**，caps `0x84203000` |
| `/dev/video33` | `msm_vidc_driver` | `msm_vidc_venc` | 编码器 |
| `/dev/video34` | — | — | `VIDIOC_QUERYCAP` 返回 `EINVAL` |

节点存在不等于可用，实测结论见 `doc/` 下 V4L2 可行性报告。

其他相关设备节点：`/dev/ion`、`/dev/kgsl-3d0`、`/dev/binder`、`/dev/hwbinder`、`/dev/vndbinder`、`/dev/ashmem` 均存在；**`/dev/dma_heap` 不存在**（内核 4.14 时代仍用旧式 ION 分配器）。

容器内 `vainfo` 无法初始化（缺 display，且 `msm_drm_drv_video.so` 打不开）——VA-API 路径当前不通。

## 6.5 `msm_vidc` 中断计数可作帧级硬解证据（外部无侵入判据）

`/proc/interrupts` 里 Venus 的中断行可用来**从宿主外部判断容器内是否真在
硬解**，无需 root、无需改 daemon、无需容器侧脚本配合。

```
510:  33  36751  0 0 0 0 0 0   PDC-GIC 206 Level   msm_vidc
```

**为什么可行**：`/proc/interrupts` 是内核全局视图，不受 pid/net namespace
隔离 —— 实测容器内直读与宿主 adb 读到的计数完全一致（同为 36751），
`run-as <应用uid>` 也能读。这让它成为 NOROOT 状态下唯一可用的硬解证据，
比 GPU/HAL 那些需要 root 的指标更有价值。

**标定数据**（三分辨率各 300 帧 H.264 yuv420p，0.25s 采样窗口）：

| 分辨率 | 总 IRQ | IRQ/帧 | 单点峰值 | 连续>17 的窗口数 |
|---|---|---|---|---|
| 720p | 563 | 1.88 | 180 | 4 |
| 1080p | 607 | 2.02 | 110 | 6 |
| 4K | 611 | 2.04 | **37** | 21 |
| 看护探活（新探针，5 帧） | 23~30 | — | 30 | **1** |

两条结论：

1. **IRQ/帧 恒定在 1.88~2.04** —— 跨 9 倍像素量只波动 8%，说明它计的是
   每帧固定的输入+输出事件，与分辨率无关。因此可据此**反推实时帧率**：
   `fps ≈ ΔIRQ / (2 × 窗口秒数)`。实测 1080p 反推 178~208 fps
   vs ffmpeg 报告 162 fps，同量级（IRQ 计的是解码器侧速率，含尾帧 flush）。

2. ⚠️ **幅度不是不变量，连续性才是。** 4K 峰值仅 37（帧大解码慢，同样数量的
   IRQ 摊到 5.25 秒而非 1 秒），与探活的 30 只差一点；而 720p 峰值 180。
   **按幅度设阈值必然漏判 4K** —— 实测把门限设到 30 后，4K 的"连续超阈窗口"
   最长只有 1，整个 4K 场景被判成 IDLE。

   正确判据：**门限 17 + 要求连续 ≥3 个窗口**。真实解码最少连续 4 个（720p），
   探活永远只有 1 个（5 帧 96×96 解得极快，落在单个窗口内）。

**这条判据的价值**在于它绕过了其它三个判据各自的盲区：

| 既有判据 | 盲区 |
|---|---|
| MediaCodec `media.metrics` | 滞后一个会话；容器侧 VA-API 解码不写 metrics |
| HAL / daemon CPU jiffies | SHM 生效后 daemon 每帧只做一次 memcpy，300 帧仅 0.12s CPU，与探活开销无法区分 |
| ~~`export-decode-status.sh` 共享文件~~（已删除） | 依赖一个独立后台循环存活（实测会静默死掉且不自愈、不告警），粒度只有 `IDLE\|cpu=2`，且用单一 CPU 阈值兼任判定与归因 —— SHM 生效后实测 240 帧解码有 1/5 采样点漏判，还会把看护探针的开销误判成真实解码 |

**踩过的坑（重要）**：调查初期得出过"IRQ 测不到帧级解码"的**错误**结论 ——
因为测试码流是 `libx264 -preset ultrafast` 默认生成的 High 4:4:4 Predictive /
yuv444p，硬件不支持，VAAPI 拒绝后 ffmpeg **静默回落软解**。当时 ffmpeg 报
300 帧 22x 速度，看着像硬解成功，实际 daemon 侧一条会话记录都没有。
**凡是验证硬件路径，必须同时确认 daemon 侧确实收到了会话与 NALU。**

另一个曾误导判断的因素：看护探针每 4.75 秒握手一次会建 MediaCodec 实例，
产生 13~17 个 IRQ 的窄脉冲。旧探针不送码流，所以那些 IRQ **全部来自
codec 实例创建/销毁**，与帧无关 —— 一度让人以为"IRQ 只反映生命周期事件"。
停掉 watchdog 后基线归零（20 秒 0 个 IRQ）才把噪声源坐实。

## 7. 服务端稳定性

跨 3 次客户端连接后：`VmRSS 42488 kB`、`Threads 2`、打开的 fd 数 10。无泄漏迹象。会话断开后能正确重新 `accept`，服务端不退出。

## 8. 构建

`-lmEDIAndk` 这个链接参数不存在（早期文档笔误）。实际所需：

```
-lmediandk -llog -landroid
```

`libmediandk` 同时提供 `AMediaCodec_*` 与 `AMediaFormat_*` 符号。使用 NDK r27c、API 29 交叉编译验证通过，产物为 aarch64 PIE 可执行文件。用 `./build.sh` 一键构建。
