# 更新日志

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
