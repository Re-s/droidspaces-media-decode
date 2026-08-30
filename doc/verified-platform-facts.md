# 平台实测事实（Verified Platform Facts）

本文只记录**在真机上实测确认**的事实，用于替代文档中的推测性描述。
每条都标注取证方式。未经验证的内容不写入本文。

- 测试设备：小米平板 5（代号 `nabu`），骁龙 855（SM8150 / Adreno 640）
- Android 13（SDK 33），内核 `4.14.336-Kuugo-v1.0-260728`，aarch64
- Root：KernelSU `ksud 3.3.0`，daemon 运行于 SELinux context `u:r:ksu:s0`
- 容器：DroidSpaces 内的 Debian 13 (trixie) aarch64
- 验证日期：主体 2026-08-22；§1.4 的 SHM 部分为 2026-08-26 复测更新；
  §1.1.5、§6.5、§9 为后续追加，各节在小节内单独标注验证日期，
  不受本行日期覆盖

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
| `/dev/dri/renderD128` | 有 | 需平台透传（可配置） |

> **pid ns 独立 ≠ 互不可见**：宿主能看到容器进程，反之不行。
> 这是宿主侧监控方案成立的根据，详见 §1.1.5。

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

所以**零拷贝只在 host 型容器可用（已实测跑通，见下），NAT 型必然降级**
—— 即使控制通道已经是路径式 Unix socket 也一样。

> **教训**：控制通道能跨 netns，**不等于** memfd 交接通道也跨得过去。
> 要让零拷贝在 NAT 容器可用，需把交接改走同一条路径式 Unix socket
> （在已有连接上传 `SCM_RIGHTS`，不必另开 socket）。那是独立改动，尚未实施。

SHM 在 **Unix socket 模式下默认开启**（2026-08-26 起；此前默认关闭）。
判据是驱动里唯一那处赋值，`vaapi-driver/src/decode.c:483`：

```c
cfg.want_shm = use_sock ? (wantshm ? (*wantshm == '1') : 1) : 0;
```

即走 Unix socket 时无 `DMD_WANT_SHM` 环境变量便取 **1**，
显式设 `DMD_WANT_SHM=0` 才关闭；**TCP 模式恒为 0**。

已在真实消费者进程里端到端实测（驱动被 dlopen 进 ffmpeg、走
`/run/dmd/decode.sock`），daemon 日志：

```
[167] 共享内存已交接: 8 槽 x 3133440 字节 (共 25067520)
[167] 握手成功: video/hevc 1280x720 帧回传=SHM
```

解码结果与内联模式一致（150/150 帧）。收益（固定 1500 帧工作量、
三组交替对照、daemon 侧 CPU jiffies）：

| 模式 | 三次测量 | 中位数 |
|---|---|---|
| 内联 | 493 / 500 / 489 | 493 |
| SHM | 400 / 367 / 410 | **400** |

**daemon CPU 降低约 19%**。

浏览器沙箱能收 `SCM_RIGHTS` —— **已实测**：Firefox RDD 与 Chrome GPU 进程
均能正常建立解码会话。此前本文记为"未验证、只在 ffmpeg（无沙箱）下验证过"。

> **已解决的历史现象**（v0.3.0 时期，留档备查）：驱动侧首次启用 SHM 时，
> `xfer=1`、4 槽 memfd 挂载与 `帧回传=SHM` 握手都成功，但随后该连接断开
> （`Broken pipe` / `Connection reset by peer`），118 个输入单元只取回 25 帧
> —— 且不会打死 daemon（无新 tombstone、socket 继续 `accept`）。
> 该问题**现已修复**，上面那组端到端数据就是修复后测得的。

#### "零拷贝"只覆盖 memfd → 消费者这一段

⚠️ **SHM 不等于"解码器直写共享内存"。** MediaCodec 的输出缓冲由 gralloc
分配，daemon 无法指定输出缓冲位置，只能 `AMediaCodec_getOutputBuffer`
拿到 CPU 指针后 `memcpy` 进 memfd（`src/decode-daemon.c` 的
`send_frame_shm`，约 949 行）。所以 SHM 省掉的是**帧经 socket 的那次拷贝**，
解码器 → memfd 那一次仍然存在。

实测这次拷贝：3133440 字节单帧 **0.227 ms**（13137 MB/s，300 次测量、
预热 30 次），按 1080p 峰值 194 fps 折算约占 **4.4% 的单核 CPU**。
其中 194 fps 是 **TCP 内联模式的峰值口径**（与
`doc/performance-and-roadmap.md:256-264` 的勘误一致），不是 SHM 模式或
真实高码率内容的稳定帧率，折算结果只作上界参考。
要消除它必须拿到输出缓冲的 dmabuf fd，而 NDK 公开 API 没有这个入口
（`AHardwareBuffer_lock` 只返回 CPU 指针，`sendHandleToUnixSocket`
实测跨容器不可用），只能依赖 `libui` / gralloc 私有符号，绑死特定
Android 版本的 C++ ABI —— 因此**已否决**。

这条事实同时解释了三个实测观测：

| 观测 | 实测值 | 原因 |
|---|---|---|
| daemon 侧 CPU | SHM 模式下 300 帧解码仅 **0.12 s** | 每帧只做一次 memcpy，帧数据不再过 socket |
| `/proc/<pid>/io` 的 `wchar` | 一次 300 帧 720p 会话增长约 **92 KB** | 若帧走 socket 应是 300 × 1.38MB ≈ **414 MB**，实测只有万分之二 —— 那 92 KB 是控制消息与日志 |
| `/proc/meminfo` 的 `Shmem` | 会话建立时**跳增约 36 MB**，随后回落 | memfd 池一次性分配后循环复用，稳态不再增长 |

> ⚠️ 上面两行曾被记为「`wchar` 恒为 **0**」和「`Shmem` **无跳变**」，
> **那是采样错误**：`wchar` 那次把 daemon PID 抓错（daemon 曾被看护重启，
> 需每次重新 `pidof`），`Shmem` 那次的采样窗口错过了会话建立瞬间。
> 复测取证（0.25 s 窗口连续采样，720p30 十秒内容）：
> ```
> t=3.25s  wchar+76614   Shmem +7052 kB
> t=3.50s  wchar+83533   Shmem+36980 kB   ← memfd 池分配
> t=4.25s  wchar+91712   Shmem  -780 kB   ← 回落，稳态不增长
> ```
> **结论方向不变**（帧确实不过 socket：92 KB vs 应有的 414 MB），
> 但"恒为 0"这种绝对表述是错的 —— 控制消息本身也要过 socket。

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
（⚠️ 这套取证方式属 0.3.x：`build.sh`、daemon 与 `test_decode.py` 均已在 0.4.0 移除。结论本身仍是当时的真实测量。）

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

高度按 32 对齐（宽按 128 对齐）是高通 Venus 解码器的规范行为，设备自身的 `/vendor/etc/media_codecs.xml` 里也直接以 `1920x1088` 作为性能点标注：

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
- 并发实例：最多 16 —— 但**会话数不是瓶颈**，实测 3 路与 6 路 1080p 的
  总吞吐都停在约 8.8x（Venus 硬件吞吐上限，详见 §9）
- 支持 `adaptive-playback`（流内动态分辨率切换）

结论：**扩展 HEVC/VP9 支持不受硬件限制**，仅是服务端 MIME 配置与客户端协议协商的工作量。

## 6. 容器内 V4L2 编解码节点状态

容器内 `/dev/video*` 已直通（属主 `root:droidspaces-gpu`，容器 root 在该组内）：

| 节点 | driver | card | 说明 |
|------|--------|------|------|
| `/dev/video0`、`video1` | — | — | 打开返回 `EBUSY`，已被占用 |
| `/dev/video2` | `sde_rotator` | `sde_rotator` | 显示旋转器，非编解码 |
| `/dev/video32` | `msm_vidc_driver` | `msm_vidc_vdec` | **解码器**，`capabilities` `0x84203000` / `device_caps` `0x04203000` |
| `/dev/video33` | `msm_vidc_driver` | `msm_vidc_venc` | 编码器 |
| `/dev/video34` | — | — | `VIDIOC_QUERYCAP` 返回 `EINVAL` |

`/dev/video32` 的 caps 位含义（两个数值自洽，差的就是
`V4L2_CAP_DEVICE_CAPS` = `0x80000000`：它只出现在 `capabilities` 里，
表示 `device_caps` 字段有效）：

| 位 | 宏 | 出现在 |
|---|---|---|
| `0x80000000` | `V4L2_CAP_DEVICE_CAPS` | 仅 `capabilities` |
| `0x04000000` | `V4L2_CAP_STREAMING` | 两者 |
| `0x00200000` | `V4L2_CAP_VIDEO_CAPTURE_MPLANE` | 两者 |
| `0x00002000` | `V4L2_CAP_VIDEO_OUTPUT_MPLANE` | 两者 |
| `0x00001000` | `V4L2_CAP_EXT_PIX_FORMAT` | 两者 |

**关键含义**：既没有 `V4L2_CAP_VIDEO_M2M_MPLANE`（`0x4000`）也没有
`V4L2_CAP_VIDEO_M2M`（`0x8000`）—— 它把解码器暴露成一对独立的
capture / output mplane 队列，而不是一个 mem2mem 设备，属于
**pre-M2M 时代的 downstream 实现**。因此依赖 `V4L2_CAP_VIDEO_M2M*`
探测 m2m 解码器的通用用户态（含部分 ffmpeg / GStreamer 路径）
会直接认不出这个节点。

节点存在不等于可用。**容器内直接走 V4L2 喂码流会"成功但无解码"，
根因已由设备真实内核源码（`MiCode/Xiaomi_Kernel_OpenSource` 分支
`nabu-r-oss`）定位到状态机而非权限**：

```c
// msm_vidc_common.c:4444  msm_comm_qbuf()
if (inst->state != MSM_VIDC_START_DONE) {
    mbuf->flags |= MSM_VIDC_FLAG_DEFERRED;
    print_vidc_buffer(VIDC_DBG, "qbuf deferred", inst, mbuf);
    return 0;                    /* 静默丢弃却报成功 */
}
```

即会话状态机未达到 `MSM_VIDC_START_DONE` 时，入队缓冲被打上
`MSM_VIDC_FLAG_DEFERRED` 后丢弃，而 `msm_comm_qbuf()` 仍返回 0（成功）；
唯一的提示走 `VIDC_DBG` 级别日志，默认关闭 —— 所以用户态看到的是
"每次 `VIDIOC_QBUF` 都成功、但一帧也出不来"。

分阶段 IRQ 实测与此吻合（每阶段 20 秒，vidc IRQ 号 510）：

| 阶段 | 净增 IRQ/s |
|---|---|
| 空闲基线 | 4.30（基线本身） |
| `open` / `S_FMT` / `REQBUFS` | 0 |
| 双队列 `STREAMON` | **+1.55** |
| 喂 199KB 码流 | **+1.65**（只比上一阶段多 0.10，落在噪声内） |
| 对照：MediaCodec 真解码 | **+6.20** |

`STREAMON` 让固件动了一点，而**喂数据对固件毫无贡献** —— 码流从未到达固件。

**归因边界（重要）**：通读
`msm_vidc_streamon → vb2_streamon → msm_vidc_start_streaming →
start_streaming → msm_comm_try_state(START_DONE)` 整条链，源码中
**不存在任何 uid / pid / pid namespace / SELinux / TrustZone 判断**。
因此"权限限制""vendor 独占""TrustZone 授权"这类归因都是错的
（`doc/why-not-v4l2.md` 里 TrustZone / `subsys-pil-tz` /
pid namespace 隐式依赖的说法已被上述源码证伪，勿再引用）。
SELinux 也已实测排除：宿主 root `u:r:ksu:s0` 与容器
`u:r:droidspacesd:s0` 表现完全一致，无 avc denial。

其他相关设备节点：`/dev/ion`、`/dev/kgsl-3d0`、`/dev/binder`、`/dev/hwbinder`、`/dev/vndbinder`、`/dev/ashmem` 均存在；**`/dev/dma_heap` 不存在**（内核 4.14 时代仍用旧式 ION 分配器）。

容器内 `vainfo` 无法加载**厂商的** `msm_drm_drv_video.so`（缺 display，
且该 so 打不开）——**厂商 VA-API 驱动这条路径不通**。

但这不等于 VA-API 整条路径不通：**本项目的自制 VA-API 驱动已走通该路径**
（同名 `msm_drm_drv_video.so`，自行实现，把请求代理给 Android 侧
MediaCodec），容器内 `vainfo` 可正常报出驱动版本与 6 个 VLD profile，
验收项见 `doc/platform-integration-contract.md:425`。

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

**本表的码流参数（profile / 码率）未记录**，只能确定是 H.264 yuv420p。
它的耗时量级与 2026-08-28 真实内容基线（High profile、27.2 Mbps，
1080p 硬解 **57.70 ms/帧**，见 §9）相差约 10 倍，说明标定用的极可能是
低码率合成或轻量码流。因此**本表只用于判据形态（IRQ/帧 恒定、连续性
优于幅度），不可当作性能基准或适用性依据**。

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

跨 3 次客户端连接后，**在最后一个会话结束、进程回到静止态时采样**：
`VmRSS 42488 kB`、`Threads 2`、打开的 fd 数 10。会话断开后能正确重新
`accept`，服务端不退出。

两点限定，别过度解读这组数字：

- **采样时刻必须是会话结束后的静止态**。会话进行中每路会话实际有
  **3 个线程**（`src/decode-daemon.c:1291`、`:1293`、`:1675`），
  所以 `Threads 2` / `fd 10` 只可能是静止态读数，不能拿来推断运行期占用。
- 3 次连接的样本量支撑不起"无泄漏"的结论。准确表述是
  **3 次连接未见 RSS / 线程 / fd 增长，样本量不足以断言无泄漏**。

## 8. 构建

`-lmEDIAndk` 这个链接参数不存在（早期文档笔误）。实际所需：

```
-lmediandk -llog -landroid
```

`libmediandk` 同时提供 `AMediaCodec_*` 与 `AMediaFormat_*` 符号。CI 使用 NDK r26d、API 29 交叉编译验证通过（r27c 亦可），产物为 aarch64 PIE 可执行文件。用 `./build.sh` 一键构建。

## 9. 能效实测：单路硬解不省电，价值在并发

验证日期：2026-08-28。测试内容为**真实高码率片源**（H.264 High profile、
27.2 Mbps、1080p），满速解 300 帧。

### 9.1 单路：与软解打平，功耗更高

系统级口径（宿主 `/proc/stat`，覆盖 daemon + Android codec 服务 + 容器侧消费者）：

| 口径 | 硬解 | 软解 | 差异 |
|---|---|---|---|
| 每帧耗时 | **57.70 ms** | 57.40 ms | 0.5%，在噪声内 |
| 墙钟总时长 | 3557 ms | 2366 ms | 硬解**慢 50%** |

整机功耗（屏幕开、放电状态下读电流）：

| 场景 | 电流 | 功率 | CPU |
|---|---|---|---|
| 空闲 | 1208 mA | 4.59 W | 43% |
| 软解 | 1524 mA | 5.74 W | 52% |
| 硬解 | **1630 mA** | **6.12 W** | **68%** |

**结论：单路硬解既不省电、也不省系统级 CPU。** 跨边界代理本身要花掉
CPU 与内存带宽，抵消了 Venus 的能效优势。

### 9.2 硬解的真实价值是并发吞吐

| 场景 | 相对实时倍速 |
|---|---|
| 4K 单路 | 2.46x |
| 3 路 1080p 合计 | 8.76x |
| 6 路 1080p 合计 | 8.75x |

3 路与 6 路合计吞吐几乎相同 → **8.8x 是 Venus 的吞吐上限，会话数不是瓶颈**
（硬件上限 16 实例，见 §5）。

另一条实测：**与 Android 侧 MediaCodec 不互斥** —— 容器侧占住
`/dev/video32` 并保持 `STREAMON` 25 秒期间，Android 侧 MediaCodec
照常以 6.89x 解完 300 帧。

### 9.3 两个不可引用的错口径数字

以下两组数据仍会在旧记录里出现，**都不能用来论证能效**：

- **进程级口径**：硬解 7.90 ms/帧 vs 软解 35.63 ms/帧（看着像省 78%）。
  它只统计了容器侧消费者进程，把 daemon 与 Android codec 服务的开销
  全漏在测量之外，所以"省"出来的是记账口径而非真实功耗。
- **合成 testsrc 码流**（0.16 Mbps）：硬解 36.77 ms/帧 vs 软解 19.80 ms/帧。
  码率低到软解几乎不花钱，量级与真实内容差两个数量级，结论不可外推。
