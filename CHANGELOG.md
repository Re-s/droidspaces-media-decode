# 更新日志

## 未发布：探针改真码流出帧判据 + 看护处置新故障类别（2026-08-27）

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

## 未发布：零拷贝帧回传默认开启（2026-08-26）

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

## 未发布：两项修复（2026-08-26）

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
