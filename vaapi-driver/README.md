# VA-API Proxy Driver（容器侧）

把 Android 宿主的 MediaCodec 硬件解码能力，通过标准 VA-API 暴露给 DroidSpaces
容器内的消费者（`vainfo` / ffmpeg / Firefox），**不需要任何环境变量**。

产物：`msm_drm_drv_video.so`，安装到 `/usr/lib/aarch64-linux-gnu/dri/`。

## 当前能力边界

**已实现：能力查询 + H.264 / HEVC / VP9 / VP8 解码数据路径。** `vainfo` 能列出
profiles 与配置属性；四者都已真机端到端出画面，硬解输出与软解**逐字节完全一致**。

各 codec 实际状态：

| Profile | daemon codec | 能力查询 | 解码出画面 | 验证结果 |
|---------|-------------|---------|-----------|---------|
| `VAProfileH264ConstrainedBaseline` / `Main` / `High` | 0（H.264） | 正常 | **通过** | 1080p 150 帧 + 4K 90 帧 + 长流 3000 帧，`cmp` 与软解逐字节一致 |
| `VAProfileHEVCMain` | 1（HEVC） | 正常 | **通过** | 1080p / 4K / 720p / 无参考帧 / 长流 1500 帧，`cmp` 与软解逐字节一致 |
| `VAProfileVP9Profile0` | 2（VP9） | 正常 | **通过** | 720p 120 帧 + 1080p 60 帧，`cmp` 与软解逐字节一致 |
| `VAProfileVP8Version0_3` | 3（VP8） | 正常 | **通过** | 720p 120 帧，`cmp` 与软解逐字节一致 |

HEVC 有一类码流不支持：SPS 里 `num_short_term_ref_pic_sets > 0` 时无法重建
参数集（VA-API 只给数量、不给内容），`dmd_hevc_can_build` 返回 0，驱动回
`VA_STATUS_ERROR_UNIMPLEMENTED` 让上层干净回落软解。x265 默认不产生这类码流。

> **历史**：HEVC 的 profile 声明曾被撤销过一段时间 —— 那时码流转发还没实现，
> 声明了反而更糟（消费者会选中我们然后失败，而它本可以直接走软解），
> 撤销前 Firefox 探针会把 HEVC 读成"可硬解"（位图含 `1<<8`）。
> 现在实现已完成并逐字节验证，声明是**如实**的。
> 但那条原则仍然有效：**不要声明没有端到端验证过的能力**。

各 codec 的难度差异只有一个来源 —— **VA-API 传给 driver 的码流完整度不同**：

- VP9：`VASliceDataBufferType` 里就是**一整个 VP9 frame**（含 uncompressed
  header，从 `frame_marker` 那 2 bit 开始）。零 header 重建，原样转发
- VP8：slice data 从 partition 0 开始，只缺 RFC 6386 §9.1 的 3 字节 frame tag
  （key frame 再加 7 字节）。`first_part_size` 可从
  `partition_size[0] + (macroblock_offset+7)/8` 精确推导
- H.264：slice data 是**完整原始 NALU**（含 NAL header 与 slice header），
  只缺 4 字节起始码。但参数集**完全不以码流形式存在** —— SPS/PPS 被 ffmpeg
  解析成字段后原始比特流就丢了，必须从 `VAPictureParameterBufferH264`
  反向合成。另外还要处理帧重排配对（见下）
- HEVC：同为完整 NALU 只缺起始码，但参数集是三个（VPS/SPS/PPS），
  `profile_tier_level` 可用字段更少，且 `conf_win_*` 在 `va_dec_hevc.h` 里
  根本没有、`slice_data_num_emu_prevn_bytes` 多数客户端填 0

### H.264 的两个坑（都会导致画面错而不是报错）

**一、帧重排配对：不能按提交顺序配对 surface。**

> 注：下面描述的是**显示序输出**（解码器默认行为）下必须做的重排。
> 当前实现**不再依赖输出顺序** —— daemon 回传每帧对应的输入单元序号
> （`CAP_FRAME_PTS`），驱动按序号精确配对，见下文"配对：按输入单元序号
> 精确匹配"一节。这段基于 POC 的重排逻辑只在 daemon 不支持该能力位时
> 作为回退路径使用。
>
> 早期版本曾用编译期常量 `DMD_DECODE_ORDER_OUTPUT` 声明输出顺序，
> 两侧不一致就画面错位且不报错（实测 105/150 帧）。该常量**已删除**。
ffmpeg 按**解码序**调 `vaEndPicture`，而 MediaCodec 按**显示序**吐帧。
本测试流实测解码序 `I P B B B`（P 在第 2 位，因为 B 要参考它），
显示序 `I B B B P`。按 FIFO 配对会从第 2 帧起全部错位。
所以按 `CurrPic.TopFieldOrderCnt`（POC）配对：daemon 的第 k 帧给 POC 第 k 小
的 surface。ffmpeg 填的是已解包的 `field_poc[0]`，不是码流里会回绕的
6 bit `poc_lsb`，可直接比大小。

但 POC 只在**同一个 coded video sequence 内**单调，每个 IDR 都会重置
（实测第 2 个 IDR 处从 65562 跳回 65536）。所以还要带一个序列号，
排序时先比 seq 再比 POC，否则新序列的帧会抢在旧序列未配对的帧之前。
**序列号的判据必须是 `frame_num` 归零**（规范 7.4.3），
不能用"POC 比上一帧小"—— 提交顺序是解码序，GOP 内 POC 本来就起伏。

**二、PPS 的 `num_ref_idx_l0/l1_default_active_minus1` 必须照抄当前帧的生效值。**
VA-API 给的是"该 slice 的生效值"：对未带 `num_ref_idx_active_override_flag`
的 slice，生效值就等于 PPS 默认值；对带 override 的 slice，PPS 默认值不起作用。
所以照抄总是对的，且不需要区分 `override_flag`（VA-API 未暴露它）。
值变化时重发 PPS —— MediaCodec 接受流中反复出现的参数集。

两条歧路均已实测证伪：取**首个 IDR** 的 slice param（I slice 无此语法元素，
恒为 0，非 override 的 B slice 就只用 1 个参考帧）；用 `num_ref_frames-1`
当"安全上界"（**l0 可偏大，l1 一位都不能偏大** —— `l1_default_active`
决定非 override B slice 里 `ref_idx_l1` 的**熵解码码长**，
改了它是语法解析层错位，不是预测质量问题）。

### 流末尾需要主动 flush

MediaCodec 稳态滞后 2-3 个单元（实测送 1/2/3 个 VCL 后等 4000ms 都不出帧，
第 4 个才出）。流末尾最后几帧攥在解码器里，而此刻 ffmpeg 正阻塞在
`vaSyncSurface` 上不会再送数据 —— 双方互等。所以等待超过总超时一半时
主动 `shutdown(SHUT_WR)`，每会话只做一次（它不可逆）。

**因此 seek 需要重建 session**：写端一旦关闭，本会话就不能再送数据。
daemon 也没有连接内的 reset，所以 seek 只能由 driver 重建会话来实现
（尚未实现 —— 当前没有消费者在同一 context 上 seek）。

### 流内分辨率变化：DestroyContext 必须先排空

ffmpeg 在流内分辨率变化时先 `vaDestroyContext` 再建新的，但**之后仍会
`vaSyncSurface` 旧 context 的 surface** —— 那些帧属于前一段分辨率，它还要取走。
若 `DestroyContext` 把仍处于 PENDING 的 surface 直接标成失败，
ffmpeg 就会收到 `VA_STATUS_ERROR_OPERATION_FAILED`，整条流解不下去
（实测 `switch.h264` 在此处送入 62 单元只取回 44 帧，18 个 surface 被放弃）。

这些帧并没有解错，只是还攥在 MediaCodec 里 —— 与流末尾同一个成因。
所以 `DestroyContext` 在放弃 surface 之前先做一次 flush + 取帧循环
（与 `vaSyncSurface` 共用 `dmd_pending_take_locked()`）。
帧数据是 `memcpy` 进 surface 自有缓冲的，所以随后销毁会话不影响后续读取。

`switch.h264`（720p→480p）与 `grow.h264`（480p→720p）现均与软解逐字节一致。

**不声明高位深**（HEVC Main10、VP9 Profile2、H.264 High10）：硬件可能支持，
但未验证。谎报能力会让消费者选中我们然后失败，比不报更糟。

### 出口：CPU 路径（VAImage）+ dmabuf 导出（非零拷贝）

surface 数据放普通 heap 内存，回读主要走 `vaDeriveImage` / `vaCreateImage`。

**零拷贝这条路是死的** —— 容器环境的硬限制：ION 完全不可用
（legacy `EINVAL` / modern `ENODEV`），`/dev/dma_heap` 不存在（内核 4.14），
MediaCodec 的 NDK 公开 API 也拿不到输出缓冲的 dmabuf fd。

但 `vaExportSurfaceHandle` **已实现**（`src/export.c`，159 行，
`vtable.inc:60` 已注册）：它把 heap 里的帧内容拷进一块新分配的 dmabuf
再导出 fd。**这是"能导出"而不是"零拷贝"** —— 多了一次 CPU 拷贝。

实现它的原因是 **Firefox 硬解必须走这个入口**（它不接受纯 VAImage 路径）。
ffmpeg 则用 `vaDeriveImage`，不碰这条路 —— 所以只测 ffmpeg 时
导出路径的问题完全不可见，需要单独用浏览器验证。

回读走 `vaDeriveImage` 与 `vaCreateImage` + `vaGetImage` 两条路，**都要实现**：
ffmpeg 探测时用 derive，但 `hwdownload` 是读访问（`MAP_READ`），
`vaapi_map_frame` 的条件里 `!(flags & MAP_READ)` 不成立，所以实际取数据时
仍走 `vaCreateImage` + `vaGetImage`（`hwcontext_vaapi.c:900`/`:910`）。

### 三个使用注意

**必须显式 `-hwaccel_output_format vaapi`。** 只给 `-hwaccel vaapi` 时 ffmpeg 会
自动把帧下载成软件格式，滤镜链拿到 nv12 而不是 vaapi 帧，`hwdownload`
报 "Impossible to convert between the formats"。正确的验收命令：

```bash
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
       -hwaccel_output_format vaapi \
       -i in.ivf -vf hwdownload,format=nv12 -f rawvideo -y out.yuv
```

**`vaMapBuffer2` 必须真实现，不能留桩。** libva 只在该槽位为 **NULL** 时才回落到
`vaMapBuffer`，而本驱动为通过 `CHECK_VTABLE` 把所有槽位都填了桩函数 ——
留桩会让 ffmpeg 的回读路径（`hwcontext_vaapi.c:928`）拿到 UNIMPLEMENTED
直接失败，永远走不到兼容分支。这是"填满 vtable"与"libva 靠 NULL 判断能力"
之间的真实冲突，新增槽位时要留意同类陷阱。

**`VAImage.offsets[1]` 用缓冲高不是显示高。** 1080p 的解码输出是缓冲 1920x1088、
显示 1920x1080。`VAImage.width/height` 必须报显示尺寸（否则 ffmpeg 尺寸校验
失败），但 `offsets[1] = stride * slice_height` 要用 **1088**。按 1080 算会让
色度平面偏移 `stride*8` 字节，症状是绿边、花屏、色度错位。

## 浏览器（Firefox）：能力没问题，门槛在环境

`vainfo` / ffmpeg 能硬解**不代表** Firefox 也会硬解。Firefox 判定"能否硬解"要
过三重门，卡住任意一重都只会静默回落软解，日志里只有一句
`Codec h264 is not accelerated`，看不出原因。

Firefox 自己的探针是认可本驱动的 —— 它启动时跑 `/usr/lib/firefox-esr/vaapitest`
（`widget/gtk/GfxInfo.cpp`），`strace` 可见它 dlopen 了我们的 `.so`：

```
$ /usr/lib/firefox-esr/vaapitest --drm /dev/dri/renderD128
VAAPI_SUPPORTED
TRUE
VAAPI_HWCODECS
112          # = 0b1110000 = H264(1<<4) | VP8(1<<5) | VP9(1<<6)
```

### 三重门（`dom/media/platforms/ffmpeg/FFmpegVideoDecoder.cpp`）

| 门 | 条件 | 卡住时的日志 |
|---|---|---|
| 1 | `gfxVars::UseH264HwDecode()` | `Codec h264 is not accelerated` |
| 2 | 硬件 WebRender（非软件合成） | `Hardware WebRender is off, VAAPI is disabled` |
| 3 | 解码在 RDD 进程 | `VA-API works in RDD process only` |

第 1 重的来源链最长，也最容易踩：

```
FEATURE_H264_HW_DECODE 要求 mVAAPISupportedCodecs & CODEC_HW_H264
  ← 由 GetDataVAAPI() 跑 vaapitest 填充
    ← 只在 probeHWDecode 为真时才跑：
      probeHWDecode = mIsAccelerated && (status OK || force-enabled pref)
                      ^^^^^^^^^^^^^^
      mIsAccelerated = !mesaAccelerated.Equals("FALSE")   ← 来自 glxtest
```

**`media.hardware-video-decoding.force-enabled` 绕不过 `mIsAccelerated`** ——
它只在括号内起作用。没有硬件 GL 就没有硬解，这是 Firefox 的设计。

### 容器里的两个必要环境变量

```bash
MESA_LOADER_DRIVER_OVERRIDE=msm   # 否则 Mesa 走 zink → 挑不到 Vulkan 设备
                                  # → 回落 llvmpipe → MESA_ACCELERATED FALSE
MOZ_DISABLE_RDD_SANDBOX=1         # 否则 RDD 沙箱禁止本驱动连 daemon 的 TCP
                                  # （驱动日志：创建 TCP socket 失败: Permission denied）
```

加覆盖后 glxtest 报 `RENDERER FD640`（真 Adreno 640 / freedreno）而非 llvmpipe。

⚠️ **`MOZ_DISABLE_RDD_SANDBOX=1` 是安全权衡**：它降低 RDD 进程的隔离强度。
更干净的长期做法是驱动改用 Unix socket，或由沙箱策略放行这一个连接。
只在信任本机环境时使用。

profile 里还需要两个 pref：

```
media.ffmpeg.vaapi.enabled = true
media.hardware-video-decoding.force-enabled = true
```

`tools/firefox-hwdec` 把上述环境固化了，并带自检：

```bash
firefox-hwdec --dmd-check    # 只检查环境（DRM 设备 / daemon / 驱动 / 探针位图）
firefox-hwdec                # 带正确环境启动 Firefox
DMD_VA_LOG=1 firefox-hwdec   # 看驱动侧日志，确认硬解真的生效
```

Xvfb 不行：它不提供 DRI3，GLX 走不到 freedreno，`MESA_ACCELERATED` 恒为 FALSE。
必须是真实的 Wayland/X 会话。

实测结果（真实 kwin_wayland 会话，1080p H.264，`<video>` 播放）：

| 指标 | 值 |
|---|---|
| VA-API 硬解帧 | 143（视频共 147 帧） |
| 软解帧 | **0** |
| `vaExportSurfaceHandle` | 148 次全部成功 |
| 平均解码耗时 | 7.4 ms（帧间隔 33.3 ms） |
| 驱动侧 / 浏览器侧错误 | 无 |
| 实际帧率 | **28.9 fps**（视频标称 30fps） |
| 画面正确性 | **黑帧 0**，与软解基线逐字节一致 |
| 排空 / 会话重建 | **各 0 次** |

⚠️ 注意"零错误 + 满帧率"**不等于画面正确**。曾经这套指标全绿而画面是坏的 ——
错误日志、帧计数、解码耗时都发现不了参考帧链被摧毁。
判断解码正确性必须检查像素，或与软解基线做逐字节比对。

该结果已独立复现：容器内经 socks5 代理 `git clone` 本分支、从克隆代码
`make` + `make install`（零警告）、只靠 `firefox-hwdec` 设置环境，
六条流回归同时保持逐字节一致。

⚠️ 测帧率一定要用**墙钟时间**。中间有一版只做到"全程硬解"就以为成了，
拿解出的帧数除以视频内容时长算出 30fps —— 实际 4.92 秒的视频花了 23 秒，
真实帧率只有 6.4 fps。看 `VA-API Got one frame` 的条数和 Firefox 自报的
`average decode time` 都发现不了这件事：单帧解码确实只要 7.4 ms，
慢的是**帧与帧之间的等待**。

### 必须以桌面会话的所属用户运行

Firefox 拒绝以 root 身份使用普通用户的会话，直接退出并只留一句
`Running Firefox as root in a regular user's session is not supported.`
容器里桌面常跑在 uid 1000 下，而人习惯用 root 操作，很容易撞上：

```bash
su master -s /bin/bash -c 'firefox-hwdec'
```

`firefox-hwdec` 会提前检查并给出准确提示，不让人对着空日志猜。

### `vaExportSurfaceHandle` 是浏览器的硬性要求

**这是"ffmpeg 全绿但浏览器不动"的根本原因**，值得单独记一笔。

Firefox 取帧只走 dmabuf 导出：`CreateImageVAAPI` 拿到解码帧后立刻调
`vaExportSurfaceHandle` 要 `DRM_PRIME_2` 描述符，失败就返回
`NS_ERROR_DOM_MEDIA_DECODE_ERR`，播放器随即 `ProcessFlush()` 并重建成**软解**。
全过程没有任何错误日志，**也没有回退到拷贝的路径**
（`dom/media/platforms/ffmpeg/FFmpegVideoDecoder.cpp:1632`）。
症状就是"硬解出 1 帧然后永久软解"。

而 ffmpeg 命令行**不需要**这个入口（`hwdownload` 走 `vaDeriveImage` + `vaMapBuffer`），
所以命令行六条流全绿的同时浏览器一动不动。

实现上有个反直觉的点：**不必零拷贝**。此前"零拷贝走不通"的三条依据
（容器 ION 不可用、无 `/dev/dma_heap`、NDK 不给 MediaCodec 输出缓冲的 fd）
说的都是同一件事 —— 拿不到 MediaCodec 那块内存的 fd。但 Firefox 要的只是
**一个能被合成器导入的 dmabuf fd**，并不要求那块内存就是解码器的原始输出。
于是 surface 改为分配在 msm_drm 的 dumb buffer 里
（`DRM_IOCTL_MODE_CREATE_DUMB` + `MAP_DUMB`），帧直接落进这块可导出内存，
拷贝次数与原来的 `calloc` 方案**相同**。

描述符要点（`src/export.c`）：单 object；Firefox 传 `SEPARATE_LAYERS`，
所以默认两层 —— Y 用 `DRM_FORMAT_R8`、UV 用 `DRM_FORMAT_GR88`，
UV 的 offset 是 `stride × slice_height`（**缓冲高 1088**，不是显示高 1080，
与 `VAImage.offsets[1]` 同一个坑）。fd 必须带 `CLOEXEC`：驱动跑在浏览器
进程里，泄漏到子进程是安全问题。

### 观测教训：不要让未实现入口静默失败

上面这个根因之所以难找，是因为 25 个未实现桩**静默**返回
`VA_STATUS_ERROR_UNIMPLEMENTED`。消费者踩到未实现入口的典型症状是
"悄悄回落软解"而不是报错，静默桩让这种情况完全不可观测 —— 为此绕了两轮弯路
（先后误判为流水线深度死锁、flush 阈值误判）。

现在 `tools/gen_stubs.py` 给每个桩都生成一行日志，一跑就现形：

```
[dmd-va] 未实现入口被调用: vaExportSurfaceHandle
```

### 与解码器流水线深度的互等（已修）

浏览器稳态只保持 **3 帧在飞**（H.264 重排深度决定），而 MediaCodec 有 B 帧时
要收到**第 4 个输入单元**才吐首帧（无 B 帧时 1，实测见 `tools/probe_lag.c`）。
差正好一帧：浏览器要先拿到帧才肯送下一个单元，解码器要再收一个单元才肯出帧。

daemon 侧的 `low-latency` 降不下来 —— 那是解码器固有的流水线深度。
所以**这个互等只能由我们主动 flush 打破**，问题只在于等多久。

判据是"**再等下去也不可能有帧**"，不是"等够久了"：

- 队列深度低于 `DMD_PIPELINE_DEPTH` 时，队列不会自己变化，等待徒劳 → 立刻 flush
- 队列够深时按 `DMD_FLUSH_AFTER_MS` 等 —— 那时帧确实在路上，
  提前 flush 会白白打断正常会话

打破互等有两种手段，**优先用可逆排空**：

| | `dmd_session_drain()` | `dmd_session_finish_input()` |
|---|---|---|
| 语义 | 送 EOS 催出帧后 `flush` 复位并重送 CSD | `shutdown(SHUT_WR)` |
| 会话 | **仍可用** | 作废，必须重建 |
| 每帧成本 | 33.6 ms（满速） | 155 ms（慢 4.7 倍） |
| 协议 | 长度 0 的带内请求 | 关闭写端 |

### 已解决：曾导致黑屏闪烁的互等

**这两种手段现在都不再触发**（实测各 0 次），因为互等本身已被消除。
保留在代码里只作为 daemon 不支持解码序输出时的兜底。

之所以要消除而不是优化它们：**排空和重建都会摧毁参考帧链**。
H.264 的 P/B 帧必须依赖参考帧，从非 IDR 位置重新开始解码要一直黑到
下一个 IDR（本测试流每 30 帧一个，故最多连黑 29 帧），实测 60 帧样本里
54 帧纯黑（`tools/probe_black.c`，亮度均值恰为 16 即 BT.601 black level），
表现为播放时画面一闪一闪。`flush` 与重建会话在这点上**没有区别**。

**根治办法：让解码器跟随输入顺序输出，滞后从 4 降到 1，互等消失。**
`tools/probe_keys.c` 逐键实测（目标滞后 <= 3）：

| 配置 | 滞后 |
|---|---|
| 默认 / `low-latency=1` / `max-output-reorder-frames=0` / `output-delay=0` | 4+ |
| `vendor.qti-ext-dec-low-latency.enable=1` | 4+ |
| **`vendor.qti-ext-dec-picture-order.enable=1`** | **1** |

只有最后一个有效。

早期版本用编译期常量 `DMD_DECODE_ORDER_OUTPUT` 告诉驱动"解码器按什么顺序
出帧"，两侧不一致就画面错位且不报错（实测 105/150 帧）。**该常量已删除** ——
现在驱动根本不需要知道输出顺序，见下节。

### 配对：按输入单元序号精确匹配（与输出顺序解耦）

daemon 把每个输入单元的序号写进 `queueInputBuffer` 的
`presentationTimeUs`，MediaCodec 原样带到对应的输出帧上，再经帧头回传。
于是"这一帧属于哪次提交"是**已知事实**，不需要推断：

```
daemon:  vcl_in * 1000  →  presentationTimeUs   （乘 1000 见下）
         帧头第 4 字段  ←  presentationTimeUs / 1000
driver:  pending_unit[] 精确匹配 dmd_frame.unit_seq
```

能力用格式描述块头部第 2 个字（原保留的 0）声明 `CAP_FRAME_PTS`；
旧 daemon 恒为 0，客户端按 3 字段帧头解析，向后兼容，
此时驱动回退到 `(seq, POC)` 推断。

**为什么乘 1000**：解码器按毫秒量化 PTS，直接用序号（步长 1us）会被
全部压成 0 —— 实测 9 个单元回传的 PTS 全为 0，配对退化成"一个号对应多帧"。

验证方式是**同一份驱动分别对上两种 daemon**：

| daemon 配置 | 六条流 | 配对回退 |
|---|---|---|
| 显示序输出（去掉 vendor 键，模拟非高通平台） | 全部一致 | 0 |
| 跟随输入序（带 vendor 键） | 全部一致 | 0 |

### 排空的触发条件（黑屏根因）

排空（EOS + flush）会**摧毁解码器的参考帧链**，之后的 P/B 帧全黑到
下一个 IDR。所以判据必须严格：

```c
int wait_is_futile = dmd_session_frames_received(c->session) > 0 &&
                     !c->daemon_has_unit_seq &&
                     (c->pending_count < DMD_PIPELINE_DEPTH);
```

⚠️ 三个条件都是必需的，尤其**第一个**：`daemon_has_unit_seq` 是
"收到第一帧才置位"的运行时观测，会话刚建立时必然为 0。
只看它就会在 0 ms 判定徒劳并立刻排空 —— 实测浏览器循环播放
708 帧里 135 帧纯黑（每轮 1 次误排空，各毁掉 25~27 帧）。
日志会写"等了 0 ms 仍无帧，可逆排空（队列 3）"：队列明明是满的。

一帧都没收到时无法区分"互等死锁"与"首帧还在路上"，
此时应等到 `flush_after_ms` 阈值。详见
`../dmd-vaapi/research/M-9-black-frames.md` 第八节。

### 长流与 seek

配对改成按单元序号后补做了这两项验证，同样对上两种 daemon 配置。
素材用 3000 帧 / 100 个 IDR 的流（`-g 30 -bf 2`，带 B 帧）：

```
ffmpeg -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=100" \
       -c:v libx264 -g 30 -bf 2 -pix_fmt yuv420p long3000.h264
```

| 场景 | 结果 |
|---|---|
| 长流 3000 帧（跟随输入序 daemon） | 逐字节一致，回退/重建/排空 全 0，序号连续到 3000 |
| 长流 3000 帧（显示序 daemon） | 逐字节一致，回退/重建 0 |
| seek 10/30/55/80 秒 | 全部一致，回退/重建 0 |
| 分辨率切换流 seek（`switch.h264`/`grow.h264`） | 全部一致 |

日志里偶发一条 `SyncSurface: 等帧超时 30 ms` 属正常非阻塞轮询，
上层随后重试，不影响结果。

⚠️ 维护 `pending` 队列时，`pending_unit` 必须与 `pending`/`pending_poc`/
`pending_seq` **一起搬移**。漏搬会让序号与 surface 错位、同一个号被重复匹配
（实测 `unit 5` 与 `unit 9` 各出现两次、序号 2 和 6 消失、70/150 帧错位），
而 daemon 侧日志一切正常 —— 很难查。

另一条已验证可行但未采用的路（`tools/probe_replay.c`）：保留排空，
但重建后从最近的 IDR 重放并丢弃重放帧，黑帧与重复帧同样是 0。
没采用是因为要在驱动里缓存整个 GOP，最坏重放 29 帧，且会拉低帧率 ——
解码序输出没有这些代价。

`tools/probe_cost.c` 拆开了那 155 ms：建会话只要 2.1 ms，
真正的成本是 `flush → 首帧` 的 149.8 ms（MediaCodec 处理 EOS 的固有延迟）。
所以问题从来不是"重建会话贵"，而是"为了拿一帧就得走一遍 EOS"。

旧 daemon 不认长度 0，此时 `drain` 失败，驱动自动退回 `finish_input` +
`EndPicture` 的透明重建（重送 SPS/PPS 后可从非 IDR 帧续传，
`tools/probe_rebuild.c` 验证）—— 功能正确，只是慢。

⚠️ 两个必须加闸的地方，都是实测踩出来的：

**daemon 侧：排空等待必须有上界。** output 线程可能因解码错误提前退出，
`drain_done` 就永远追不上 `drain_req`，input 线程永久卡死 → 会话泄漏 →
累积到并发上限 8 个后开始拒绝新连接，同时 `decode-daemon` 203% CPU、
`media.codec` 180% CPU、8 核 load average 冲到 18.7。
症状（CPU 高 + 浏览器一帧不出）离病因很远，中间"泄漏到上限"这层
把因果关系彻底掩盖 —— 排查时差点归因到桌面会话不稳定。

**驱动侧：可逆排空必须"每次等待只做一次"。** 它不像 `finish_input` 会置
`input_finished` 把自己挡住 —— 排空后队列深度不变、条件依旧成立，
不加闸就是忙循环（实测触发 133 万次、只出 1 帧）。

两个反面教材，都是实测踩出来的：

- 按耐心阈值等满 2000ms 再 flush → 每帧 2 秒，播放等于卡死（硬解 7 帧就停）
- 只按队列深度判、浅队列一律不 flush → 流末尾的尾帧取不出来，
  `test1080` 少收 2 帧并报 `TIMEDOUT 38`。尾帧同样是浅队列，
  想用"上游是否还在送料"区分它和"填充中"也行不通 ——
  浏览器停止送料与流结束在该指标上无法区分

改用可逆排空后，浏览器场景下的会话重建次数降为 **0**，
帧率从 6.4 fps 提到 29.8 fps（满速）。

## 无感发现机制（改名即失效）

文件名不是随便取的，是 libva 的推导结果倒推出来的：

1. libva 用 `DRM_IOCTL_VERSION` 从 `/dev/dri/renderD128` 取内核驱动名，实测为 `msm_drm`
2. libva 的 DRM→VA 驱动名映射表（`va/drm/va_drm_utils.c` 的 `map[]`）里
   **没有 msm 条目**，于是走 fallback：原样使用内核驱动名
3. libva 只尝试**唯一一个**文件名，没有 fallback：
   `/usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so`

所以产物必须精确叫 `msm_drm_drv_video.so`（是 `msm_drm_`，不是 `msm_`）。
装到该目录后，裸跑 `vainfo`、ffmpeg、Firefox 都会自动找到它 —— 这就是"无感"。

driver 目录是编译进 libva 的硬编码值（实测 `libva.so.2` 内的串与 `libva.pc`
的 `driverdir` 一致），不是运行时探测的。

## 两个必踩的坑

**坑 1：libva 强制校验 39 个 vtable 槽位非 NULL。**
`vaInitialize` 在 driver init 返回后逐个检查（`va/va.c` 的 `CHECK_VTABLE`），
任一为 NULL 则整个初始化失败并 `dlclose`。所以未实现的入口也**必须存在**，
指向返回 `VA_STATUS_ERROR_UNIMPLEMENTED` 的桩函数。
同时校验 5 个 `max_*` 字段 > 0 与 `str_vendor` 非 NULL（`CHECK_MAXIMUM` / `CHECK_STRING`）。

`va_backend.h` 的 `VADriverVTable` 共 60 个槽位，全部由 `tools/gen_stubs.py`
从头文件自动装配，避免手抄漏项。

**坑 2：入口符号名要自己拼。**
必须导出 `__vaDriverInit_<major>_<minor>`（libva 从当前版本向下逐个 `dlsym`，
首个命中即用）。libva 头文件**没有**提供生成该名字的宏，
所以 `driver.c` 里用 `VA_MAJOR_VERSION` / `VA_MINOR_VERSION` 拼接 ——
比硬编码 `__vaDriverInit_1_22` 更耐版本变化。
又因为编译开了 `-fvisibility=hidden`，该符号必须显式标
`__attribute__((visibility("default")))`，否则不导出，
libva 会报 `has no function __vaDriverInit_1_0`。

## 构建

**必须在容器内编译**（aarch64）。开发机若是 x86_64，缺 aarch64 glibc 交叉工具链。

容器里 `git clone` GitHub 直连会挂在 TLS 上
（`GnuTLS recv error (-110): TLS 链接非正常地终止了`），走本机 socks5 代理即可：

```bash
git -c http.proxy=socks5h://127.0.0.1:1080 clone \
    --branch feat/vaapi-driver https://github.com/Re-s/droidspaces-media-decode.git
```

注意 `socks5h`（让代理做 DNS）而不是 `socks5`，且该端口不吃 `http://` 协议前缀。

依赖：`libva-dev`（只取头文件）、`libdrm-dev`（dumb buffer 的 ioctl 定义与
`drm_fourcc.h`，同样只取头文件）、`gcc`、`make`、`pkg-config`。
容器实测已全部具备。

```bash
make            # 产物 build/msm_drm_drv_video.so
make check      # 确认 __vaDriverInit_* 符号已导出
make clean
```

driver 是被 libva `dlopen` 的插件，符号由宿主进程提供，**不链接 libva 本体**。

## 安装

```bash
sudo make install     # → /usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so
sudo make uninstall   # 移除
```

支持 `DESTDIR` / `PREFIX` / `LIBDIR` / `DRIDIR` 覆盖。

这个文件名没有任何 dpkg 包占用（实测），所以新增它不会与包管理冲突，
卸载就是删掉单个文件，完全可逆。

## 验证

先用临时目录验证，确认无误再装进系统：

```bash
make
mkdir -p /tmp/vatest && cp build/msm_drm_drv_video.so /tmp/vatest/
LIBVA_DRIVERS_PATH=/tmp/vatest vainfo --display drm --device /dev/dri/renderD128
```

装好之后的验收标准是**裸跑**（零环境变量、零参数）：

```bash
vainfo
```

应输出 `Driver version: DroidSpaces MediaCodec VA-API driver ...` 与 6 个
`VAEntrypointVLD` profile，exit code 0。

其他检查：

```bash
vainfo -a          # 走 vaQuerySurfaceAttributes，会打印每个 profile 的配置属性
ffmpeg -init_hw_device vaapi=va:/dev/dri/renderD128 -v verbose \
       -f lavfi -i nullsrc -frames:v 1 -f null -
```

ffmpeg 应报 `Initialised VAAPI connection: version 1.22` 与我们的 vendor 串。

> 在无显示的 SSH 会话里裸跑 `vainfo` 会先打印 Wayland/X11 连接失败，
> 随后自动落到 drm 路径成功。那两行与本 driver 无关。

## 代码组织

```
vaapi-driver/
├── Makefile
├── src/
│   ├── driver.c      # 入口 __vaDriverInit_*、vtable 装配、日志
│   ├── driver.h      # 内部结构、能力常量、已实现入口声明
│   ├── profiles.c    # profile/entrypoint/config 查询与 config 对象管理
│   ├── stubs.c       # 自动生成：47 个 UNIMPLEMENTED 桩
│   ├── stubs.h       # 自动生成
│   └── vtable.inc    # 自动生成：60 个槽位的装配列表
└── tools/
    ├── gen_vtable.py # 从 va_backend.h 抽取全部 vtable 签名 → JSON
    └── gen_stubs.py  # JSON → stubs.c / stubs.h / vtable.inc
```

libva 版本变化后重新生成：

```bash
make gen        # 需要容器内的 /usr/include/va/va_backend.h
```

`gen_vtable.py` 有个已知陷阱：头文件里 `vaExportSurfaceHandle` 的返回类型与
`(*name)` 之间有换行，正则必须允许跨行，否则会静默漏掉该槽位。
脚本已处理并带交叉校验（抽取数 vs 头文件成员数）。

## 插件工程约束

driver 跑在别人的进程里（Firefox、ffmpeg），规矩比独立程序严：

- 不 `exit()` / `abort()` / `assert()` —— 参数非法就返回对应 `VA_STATUS_ERROR_*`
- 不写 stdout（那是宿主的）。日志走 stderr 且默认静默，`DMD_VA_LOG=1` 打开
- 全部入口线程安全：config 表由 `pthread_mutex` 保护
- `vaTerminate` 释放全部资源，且在 init 失败后被调用也安全

ffmpeg 按 `str_vendor` 字符串匹配一张 `vaapi_driver_quirks` 名单。我们的串不在
名单内，走 standard behaviour —— 意味着**语义必须标准**，尤其后续实现
surface/buffer 生命周期与 `vaSyncSurface` 时不能偷懒。

## 相关文档

- [项目总览](../README.md)
- [平台实测事实](../doc/verified-platform-facts.md)
- [VAAPI Proxy 架构调研](../doc/vaapi-mediacodec-proxy-research.md)
