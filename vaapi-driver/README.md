# VA-API Proxy Driver（容器侧）

把 Android 宿主的 MediaCodec 硬件解码能力，通过标准 VA-API 暴露给 DroidSpaces
容器内的消费者（`vainfo` / ffmpeg / Firefox），**不需要任何环境变量**。

产物：`msm_drm_drv_video.so`，安装到 `/usr/lib/aarch64-linux-gnu/dri/`。

## 当前能力边界

**已实现：能力查询 + H.264 / VP9 / VP8 解码数据路径。** `vainfo` 能列出 profiles
与配置属性；三者都已真机端到端出画面，硬解输出与软解**逐字节完全一致**。

**未实现：HEVC 的解码数据路径。** profile 仍然声明（能力查询正常），
但 `vaEndPicture` 返回 `VA_STATUS_ERROR_UNIMPLEMENTED`。

各 codec 实际状态：

| Profile | daemon codec | 能力查询 | 解码出画面 | 验证结果 |
|---------|-------------|---------|-----------|---------|
| `VAProfileH264ConstrainedBaseline` / `Main` / `High` | 0（H.264） | 正常 | **通过** | 1080p 150 帧 + 4K 90 帧，`cmp` 与软解逐字节一致 |
| `VAProfileVP9Profile0` | 2（VP9） | 正常 | **通过** | 720p 120 帧 + 1080p 60 帧，`cmp` 与软解逐字节一致 |
| `VAProfileVP8Version0_3` | 3（VP8） | 正常 | **通过** | 720p 120 帧，`cmp` 与软解逐字节一致 |
| `VAProfileHEVCMain` | 1（HEVC） | 正常 | 未实现 | `vaEndPicture` 返回 UNIMPLEMENTED |

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

**已知限制**：流内分辨率变化（`switch.h264` / `grow.h264`）会失败 ——
daemon 会话建立时就固定了分辨率，中途变更需重建会话，
而 flush 之后写端已不可逆关闭。属于 seek/flush 支持的一部分。

**不声明高位深**（HEVC Main10、VP9 Profile2、H.264 High10）：硬件可能支持，
但未验证。谎报能力会让消费者选中我们然后失败，比不报更糟。

### 出口只有 CPU 路径（VAImage）

`vaExportSurfaceHandle` 保持 UNIMPLEMENTED，surface 数据放普通 heap 内存。
这不是偷懒，是容器环境的硬限制：ION 完全不可用（legacy `EINVAL` / modern
`ENODEV`），`/dev/dma_heap` 不存在（内核 4.14），MediaCodec 的 NDK 公开 API
也拿不到输出缓冲的 dmabuf fd。零拷贝这条路是死的。

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

依赖：`libva-dev`（只取头文件）、`gcc`、`make`、`pkg-config`。容器实测已全部具备。

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
