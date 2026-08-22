# 平台实测事实（Verified Platform Facts）

本文只记录**在真机上实测确认**的事实，用于替代文档中的推测性描述。
每条都标注取证方式。未经验证的内容不写入本文。

- 测试设备：小米平板 5（代号 `nabu`），骁龙 865（SM8150 / Adreno 640）
- Android 13（SDK 33），内核 `4.14.336-Kuugo-v1.0-260728`，aarch64
- Root：KernelSU `ksud 3.3.0`，daemon 运行于 SELinux context `u:r:ksu:s0`
- 容器：DroidSpaces 内的 Debian 13 (trixie) aarch64
- 验证日期：2026-08-22

---

## 1. 跨边界通信：为什么用 TCP

容器与 Android 宿主的 namespace 关系（实测 `/proc/self/ns/`）：

| namespace | 容器内值 | 与宿主关系 |
|-----------|---------|-----------|
| net | `4026531937` | **共享宿主初始 namespace** |
| mnt | `4026535443` | 独立 |
| pid | `4026535442` | 独立 |
| ipc | `4026535440` | 独立 |
| uts | `4026535439` | 独立 |
| cgroup | `4026535441` | 独立 |

结论：

- **mount namespace 独立** → 容器默认看不到 Android 的 `/data`（实测容器内 `/data` 只有 `local/` 一个空壳，`nsenter -t 1 -m -- ls /data/adb` 报"没有那个文件或目录"）。因此**基于文件系统路径的 Unix domain socket 不可用** —— 两侧的挂载表不一致，同一路径在对侧不指向同一个 socket 节点。

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
- **net namespace 共享** → TCP `127.0.0.1` 在两侧直接互通，无需端口转发。这是当前方案的基础。

> 文档中"Socket 路径 `/tmp/anland/decode.sock`"的描述属于早期 Unix socket 方案的遗留，与当前 TCP 实现不符。

## 2. 端到端解码链路已验证可用

测试方式：`build.sh` 交叉编译后 `adb push` 到 `/data/local/tmp/`，用 `su -c` 手动启动（测试阶段不安装 KSU/Magisk 模块），容器内运行 `src/test_decode.py`。

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

## 7. 服务端稳定性

跨 3 次客户端连接后：`VmRSS 42488 kB`、`Threads 2`、打开的 fd 数 10。无泄漏迹象。会话断开后能正确重新 `accept`，服务端不退出。

## 8. 构建

`-lmEDIAndk` 这个链接参数不存在（早期文档笔误）。实际所需：

```
-lmediandk -llog -landroid
```

`libmediandk` 同时提供 `AMediaCodec_*` 与 `AMediaFormat_*` 符号。使用 NDK r27c、API 29 交叉编译验证通过，产物为 aarch64 PIE 可执行文件。用 `./build.sh` 一键构建。
