# 平台接入契约（Platform Integration Contract）

面向 DroidSpaces 平台工程师。本文只描述**平台侧需要提供什么**，以及每项要求的实测依据与验证方法。

本文所有"实测"结论的取证设备与环境见 `doc/verified-platform-facts.md`。
未经实测的内容一律标注为"待平台确认"或"未验证"，不做推断填充。

> 🌐 **English version: [platform-integration-contract.en.md](platform-integration-contract.en.md)**

> ## ⚠️ 0.4.0 起本文要求大幅缩减
>
> 0.3.x 的架构是
> `容器 ffmpeg → unix socket → Android 侧 decode-daemon → MediaCodec`，
> 因此需要平台做三件事：共享目录 bind mount、daemon 以特定 SELinux domain
> 启动（外加一条 binder allow 规则）、设备透传。
>
> 0.4.0 改为**驱动内 V4L2 直通**：
> `容器 ffmpeg → libva 驱动 .so → /dev/video32`。
> 没有 socket、没有 daemon、没有跨进程通信、没有共享内存、没有 Android 侧组件。
>
> **前两件事不再需要。** 现在只剩设备节点权限一件。
> 若平台此前按旧契约实现了 bind mount 与 SELinux domain 切换，
> 那些工作对 0.4.0 已无作用，可以撤掉。

---

## 1. 一句话概述

平台只需保证容器内的普通用户能读写两三个设备节点。驱动是一个纯用户态 `.so`，
不需要 root、不需要 SELinux domain 切换、不需要任何守护进程或共享内存。

---

## 2. 平台必须提供的唯一一件事：设备节点权限

### 2.1 需要的节点

| 节点 | 用途 | 必需性 |
|---|---|---|
| `/dev/video32` | msm_vidc 解码器（V4L2 M2M） | **必需** |
| `/dev/dma_heap/system` | DMABUF 来源（内核 5.x） | 与下一行**二者任一** |
| `/dev/ion` | DMABUF 来源（内核 4.14 一类） | 与上一行**二者任一** |

驱动先试 `dma_heap`，不存在则回落 `/dev/ion` —— 用
`ION_IOC_HEAP_QUERY` 枚举出 system heap，**不写死 heap id**
（heap id 因设备而异，实测一台设备上 system 是 25 而非常见的 0~7）。

`/dev/dri/renderD128` **不是**本驱动的依赖。它由 Mesa 用于 GL 侧，
浏览器场景通常也需要，但与本驱动的解码路径无关。

### 2.2 权限要求与实测依据

本机实测（容器内普通用户 `master`，属 `droidspaces-gpu` 组）：

```
/dev/video32   root:droidspaces-gpu 660   可读写
/dev/ion       root:droidspaces-gpu 660   可读写
```

**结论：`root:<容器可见组> 660` + 把容器用户加入该组即可。不需要 0666。**

验证（容器内，非 root）：

```bash
test -r /dev/video32 && test -w /dev/video32 && echo "video32 ok"
{ test -w /dev/ion || test -w /dev/dma_heap/system; } && echo "DMABUF 来源 ok"
```

### 2.3 不需要 root，也不需要 SELinux domain

代码依据：源码中没有任何 `geteuid` / `setuid` / SELinux /
`/proc/self/attr` 调用（全仓 `grep` 为 0 处）；动态依赖只有
`libc.so.6`（`ldd` 输出）。

实测依据：上述读写测试全部以普通用户身份完成。

---

## 3. 驱动的安装位置

libva 按 `<LIBVA_DRIVER_NAME>_drv_video.so` 查找驱动，目录由
`LIBVA_DRIVERS_PATH` 决定，默认值可查：

```bash
pkg-config --variable=driverdir libva
# 本机：/usr/lib/aarch64-linux-gnu/dri
```

安装后设置驱动名：

```bash
export LIBVA_DRIVER_NAME=msm_drm
```

免安装试用（不写系统目录）：

```bash
LIBVA_DRIVERS_PATH=/tmp/dri LIBVA_DRIVER_NAME=msm_drm ffmpeg ...
```

---

## 4. 容器类型不再影响可用性

0.3.x 必须区分 host 型与 NAT 型容器，因为传输通道依赖 network namespace：

| | host 型 | NAT 型 |
|---|---|---|
| net namespace | 与宿主共享（`4026531937`） | 独立（如 `4026535650`） |
| `127.0.0.1:20003` 连宿主 | 可达 | 不可达 |
| abstract socket 可见数 | 31 | 0 |

这些差异**对 0.4.0 没有任何影响** —— 驱动在容器进程内直接 `open()`
设备节点，不跨进程、不用网络、不用 abstract socket。
两类容器只要满足第 2 节的权限要求就同等可用。

（上表的 namespace 数据仍是准确实测结果，保留供其他用途参考。）

---

## 5. 不需要平台做的事

以下都是 0.3.x 的要求，0.4.0 一律不需要：

- ❌ 共享目录 bind mount（`/run/dmd/`）及其 inode 失效问题
- ❌ 以特定 SELinux domain 启动 daemon，以及配套的 binder allow 规则
- ❌ 为 NAT 型容器做端口转发
- ❌ 部署 KernelSU 模块（`ksu-module/` 已整体删除）
- ❌ memfd / 共享内存通道
- ❌ 任何 Android 侧常驻进程与其放置、启动、重启、日志策略

---

## 6. 验证清单

容器内依次执行，全部通过即接入完成：

```bash
# 1) 节点可读写（非 root）
test -r /dev/video32 && test -w /dev/video32 && echo "1) video32 ok"
{ test -w /dev/ion || test -w /dev/dma_heap/system; } && echo "1) DMABUF ok"

# 2) 解码器身份正确（不依赖 v4l2-utils，用 QUERYCAP 直接问）
python3 -c "
import fcntl
b=bytearray(104)
with open('/dev/video32','rb+',buffering=0) as f:
    fcntl.ioctl(f, 0x80685600, b)   # VIDIOC_QUERYCAP
print('driver =', bytes(b[0:16]).split(b\'\\0\')[0].decode())
print('card   =', bytes(b[16:48]).split(b\'\\0\')[0].decode())"
# 期望 driver=msm_vidc_driver  card=msm_vidc_vdec
# 装了 v4l2-utils 的话 v4l2-ctl -d /dev/video32 --info 等效

# 3) 硬解一帧
LIBVA_DRIVER_NAME=msm_drm ffmpeg -hwaccel vaapi \
  -i sample.h264 -frames:v 1 -f null -
```

> ⚠️ **不要用 `vainfo` 做验证。** 本平台上它会挂住，
> 即使指定一个不存在的驱动名也一样。用第 3 步的 ffmpeg 命令代替。

---

## 7. 已知限制

诚实列出当前未解决或未验证的项，便于平台评估。

| 项 | 状态 |
|---|---|
| **设备兼容性** | 并非所有设备的 `/dev/video32` 都能用。已实测一台设备（小米平板 5 / nabu，内核 4.14）解码会话起不来：两段式协商每步都返回成功，但 `V4L2_EVENT_SOURCE_CHANGE` 永不到达、输入缓冲有去无回（`DQBUF` 恒 `EAGAIN`）、msm_vidc IRQ 净增与空闲无差别。同一设备的**编码器** `/dev/video33` 正常（`screenrecord` 可用，IRQ 净增 753）。已排除：缓冲类型、ION heap、物理连续性、容器权限、喂料切分、控制项、STREAMON 时序。属该设备解码路径问题，非平台配置问题。平台可用 `vaapi-driver/tools/probe_device_support.c` 判定：能收到 `SOURCE_CHANGE` 即可用 |
| AV1 | **尚未达到像素一致**，因此发布版不声明该 profile（声明会让浏览器/ffmpeg 把工作交过来然后失败）。当前状态见 `doc/av1-v4l2-status.md`。开发版可用 `make AV1=1` 打开 |
| VP8 | **不支持**。`/dev/video32` 的 `ENUM_FMT` 确实列出 `VP80`，但驱动缺少 RFC 6386 §9.1 未压缩块的重建逻辑，故不声明 |
| 编解码覆盖 | H.264 / HEVC / VP9 已真机端到端逐字节验证。**未验证**：高位深与非 4:2:0（HEVC Main10、VP9 Profile2、H.264 High 10 / 4:2:2）—— 驱动不声明这些能力 |
| HEVC 例外 | SPS 里 `num_short_term_ref_pic_sets > 0` 的码流无法重建参数集，驱动返回 `VA_STATUS_ERROR_UNIMPLEMENTED`，上层干净回落软解 |
| seek | 驱动尚未实现会话重建式 seek（当前没有消费者在同一 context 上 seek）；ffmpeg 层面的 seek 已验证一致 |
| 鉴权 | 0.4.0 没有跨进程通道，也就没有通道鉴权问题；访问控制完全由设备节点的文件权限决定。**若把节点开成 0666，容器内任何进程都能用硬件解码器** —— 多租户或运行不可信 App 的环境应按 §2.2 用组权限而非 0666 |
| 并发上限 | 硬件支持 16 路会话，**驱动侧尚未测到上限行为**。多个 context 各自持有独立的 `/dev/video32` fd，超限时表现为 `open()` 或协商失败 |
| 性能与能效 | 0.3.x 的实测数字全部作废（架构已变），V4L2 路径的性能与功耗**尚未测量**。见 `doc/performance-and-roadmap.md` 的作废标注 |
| 分辨率变化 | 流内分辨率上升时驱动会重建会话；该路径**未做专项验证** |

---

## 相关文档

- 当前能力与已知限制 → 仓库根 `CHANGELOG.md`
- 架构说明 → 仓库根 `README.md`
- 驱动实现细节与踩坑记录 → `vaapi-driver/README.md`
- 平台实测事实 → `doc/verified-platform-facts.md`
- 为何曾判定 V4L2 不可用，及该结论的更正 → `doc/why-not-v4l2.md`
- AV1 当前状态 → `doc/av1-v4l2-status.md`
