# 视频编解码遥测协议 v1

本驱动默认给每个 VA context 创建一个名为 `dmd-video-v1` 的匿名 memfd。
它仅用于观察，不提供控制接口，不需要解码守护进程或监控服务。
设置 `DMD_VIDEO_STATS=0` 可关闭；创建、映射或封印失败会静默停用该 context
的遥测，保留原有编解码行为和 errno。`DMD_VA_LOG` 与此功能独立。

驱动二进制包含只读标记 `DMD_VIDEO_STATS_PROTOCOL=1`，用于区分“安装了支持
遥测的版本”与“当前有可观察任务”。标记不能证明运行中的应用已经加载新版驱动；
活动记录里的 `build` 才描述实际生产记录的版本。应用需要重新建立 context。

## 4096 字节布局

当前实现支持小端 Linux（AArch64 与 x86-64）。整数均使用小端编码：

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0 | 8 | `DMDVID1\0` |
| 8 | 4 | uint32，协议版本 `1` |
| 12 | 4 | uint32，容量 `4096` |
| 16 | 8 | uint64，序号；奇数表示写入中，偶数表示稳定 |
| 24 | 4 | uint32，JSON 字节数，不包含末尾 NUL |
| 28 | 4 | 保留，当前为零 |
| 32 | json_len | ASCII JSON，无须、也不保证 NUL 结尾 |

生产者以 mutex 串行化并发更新，在写奇数序号后使用完整内存屏障，再更新
JSON／长度，最后以 release 语义发布偶数序号。读者必须只读打开并：

1. 从 `/proc/PID/fd` 查找 `memfd:dmd-video-v1` 候选，打开 FD 后核对大小、magic、
   version、capacity，不信任单独的 FD 名称。
2. 读取 header，保存偶数 seq，并验证 `0 < json_len <= 4064`。
3. 按该长度读取偏移 32 的 JSON，再读取偏移 16 的 seq。
4. 仅当两次 seq 完全相同且均为偶数时接受。其余情况有限重试，不能在坏记录上忙等。
5. 校验 JSON 的 `pid` 与扫描到的进程相符，数值类型、范围、字符串长度符合预期。

不要缓存 `/proc/PID/fd/N` 路径作为流身份：PID、FD、VA context ID 都可能复用。
读者应结合 PID 启动时间和 `session_id` 区分实例。枚举、打开或读取过程中 FD
消失属于正常退出竞态；权限错误表示不可观察，不表示软件解码。

## JSON 字段

| 字段 | 含义 |
| --- | --- |
| pid、uid | 生产进程的 `getpid()`、`getuid()`；UID 为真实 UID |
| context | VAContextID，JSON 整数 |
| session_id | 启动单调时间与进程内序号组成的 ASCII 实例标识；JSON 字符串 |
| started_ns | 此记录创建时的 CLOCK_MONOTONIC 纳秒 |
| build | 实际驱动 DMD_VENDOR_STRING；必要字符转义为 ASCII JSON |
| direction | `decode` 或 `encode` |
| codec | `h264`、`hevc`、`vp9`、`vp8`、`av1`、`mpeg2` 或 `unknown` |
| profile | libva VAProfile 的整数值 |
| width、height | 最后驱动处理输出尺寸，可能仍包含源图像的对齐行；尚未协商时均为 0 |
| resolution_kind | `driver-output` 或尚无尺寸时的 `unknown`；不是源视频显示分辨率 |
| source_width、source_height | 当前始终为 null；原始媒体的显示裁剪信息未必传给 VA 驱动 |
| hw_completed | 成功硬件输出累计：解码为非空且无 ERROR 的 V4L2 CAPTURE 出队；编码为后端成功返回完整非空码流 |
| output_frames | 驱动成功存入 VA surface 或提交 coded buffer 的累计数；已知带内核 ERROR 的帧排除，不是上屏／播放计数 |
| reused_frames | 引用已有像素交付或旧 AV1 恢复分支复用原 surface 内容的次数；后者不增加 output_frames，也不增加 hw_completed |
| errors | 记录到的错误事件累计，不保证等于失败帧数；底层 IO 与上层 VA 失败可能各记录一个事件 |
| updated_ns | 本次快照发布的 CLOCK_MONOTONIC 纳秒 |
| last_frame_ns | 最近硬件完成或成功交付事件的单调时间；尚无此事件时为 0 |
| target_fps | 编码配置目标帧率的 JSON number，未知为 null；解码始终为 null |
| target_fps_num、target_fps_den | 目标帧率有理数；未知时均为 0 |
| state | `created`、`active`、`error`、`closed`；active 表示曾完成活动，当前是否空闲由时间判断 |

编码初次参数到达后才公布最终处理配置。若传入的可见区域确实标为 1080，
即使 HEVC surface 池高为 1088，记录也可以显示处理 1080。这不保证总能取得源
裁剪：H.264 VA picture 参数只有宏块尺寸，驱动又须自行重建 SPS；若应用传入
的 context 本身为 1088，原视频的 1080 crop 没有透传，后端 G_SELECTION 看到的
也是重建后的 1088。此时必须显示“处理尺寸 1920×1088”，源显示尺寸仍未知，
不能硬编码 1088→1080，也不能把 GetImage 的任意局部读取区域当作源分辨率。
解码尺寸来自当前协商结果和实际交付 surface；分辨率变化时更新。
需要准确源显示分辨率时应增加应用侧适配。target_fps 是配置值，不能作为实测 FPS。

AV1 的 DPB 修复趟可能产生额外硬件输出但不交付新的 VA 图像，所以
`hw_completed` 可以大于 `output_frames`。旧恢复分支把已有 surface 标为 READY
不等于又完成一次硬件解码；复用计数不证明这是源码流的新有效画面。
零长度 EOS、参数集包、错误缓冲不算成功硬件帧。编码完成后 coded buffer 被
并发销毁时，可以出现 `hw_completed=1`、`output_frames=0`、`errors>0`。
内核错误标志经 CAPTURE 槽和 session 队列传到内部 dmd_frame，阻止错误帧增加
output_frames 或把状态重新标为 active；这只修正遥测，不改变旧有像素复制和 VA
返回行为。没有错误标志仍不构成像素正确性验证。

## 发布频率与开销

每次硬件完成、交付、复用、错误事件都立即把完整小型 JSON 写入共享映射。
配置只有变化时才发布，创建和销毁也立即发布。因此短片或暂停前的最后几帧不会
留到下一次事件。此处没有逐帧 open、write、pwrite、mmap、文件分配或额外线程；
热路径只包括计数、CLOCK_MONOTONIC、轻量 mutex、字符串格式化和内存写入。
监控界面可按 1 Hz 采样，无须逐帧读取。`build/test_video_stats --benchmark`
可测量双快照（硬件完成＋交付）每帧开销，不能把主机结果代替 ARM 实测。

处理 FPS 可用相邻快照的 `output_frames` 差除以单调时间差；硬件处理次数应另列。
在没有新快照、`updated_ns` 未变化、进程退出、实例切换或计数不连续时，应显示
空闲／未知或重新建立采样基线，不能把旧快照当作持续以旧速率运行。

## 生命周期、权限和沙箱

context 持有记录引用。session／encoder 另持引用，保护已放锁的 IO 和延后销毁。
context 销毁即发布 closed、关闭生产 FD、解除映射；剩余引用只维持对象寿命，
后续事件成为空操作，不得继续写已销毁记录。进程崩溃时由内核关闭 FD。
监控器已打开的只读 FD 可以暂时保留最后快照，应结合进程存活检查清理；不要仅
因一段时间没有视频帧就判进程退出。

memfd 使用 CLOEXEC，并封印 GROW／SHRINK／SEAL，避免外部截断导致 SIGBUS。
没有磁盘路径，不执行 fchmod：Firefox RDD 的现有策略可能将 fchmod 作为
SIGSYS 拒绝，不能把它当作普通可捕获的权限错误。`/proc/PID/fd` 的访问仍受
Linux ptrace、dumpable、用户／PID namespace、挂载 hidepid 与安全模块限制。
目录的 stat UID 也可能因 non-dumpable 变为 root；应按 `/proc/PID/status` 的
真实 UID 核对记录，不能由目录 owner 猜用户。

同 UID 客户端不是互相隔离的安全边界；读者必须把候选数据当作不可信输入。
监控器扫描当前 PID namespace 中系统权限允许读取的记录，root 可以跨 UID
观察，界面明确列出 UID；它不额外实施“仅本人”过滤或新增确认流程。不要放宽系统
ptrace、/proc 或浏览器沙箱来获取数据。memfd_create、ftruncate、mmap、fcntl
是否被目标浏览器版本允许仍需实测；被 seccomp 直接杀进程的 syscall 无法靠
“忽略 errno”补救。环境变量关闭入口可用于兼容性诊断。

本协议仅覆盖实际加载此驱动的 VA-API context。应用使用软件解码、其它 VA
驱动、Android MediaCodec 或其它硬件接口时不会产生此记录。不能从无记录、
高 CPU 占用或缺少 video FD 推断软件路径。GPU/RDD PID 也不能直接当作浏览器
主进程；应用名来自外部进程树识别，应注明推断。解码完成不证明像素正确、
窗口已经显示、无播放掉帧或芯片占用率；这些需要应用指标或独立像素验证。

当前不发布纯芯片执行时间、媒体 PTS、文件路径、网址、像素或码流。驱动观察
的排队／等待墙钟时间包含调度、拷贝和内核等待，不能冒充硬件运行时间。

## 主机检查

运行 `make -C vaapi-driver tests check`，覆盖协议、禁用／创建失败、errno 保留、
封印、短片尾帧、并发快照、引用寿命、编码错误和销毁竞态、有效尺寸，以及
模拟 V4L2 CAPTURE 的成功／ERROR／EOS／AV1 额外趟计数。尺寸测试也验证
1088 不会被猜成 1080，source 尺寸保持未知。这些测试不访问真实
视频设备，也不宣称已经验证目标浏览器沙箱或真实硬件。
