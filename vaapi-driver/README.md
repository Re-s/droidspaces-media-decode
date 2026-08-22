# VA-API Proxy Driver（容器侧）

把 Android 宿主的 MediaCodec 硬件解码能力，通过标准 VA-API 暴露给 DroidSpaces
容器内的消费者（`vainfo` / ffmpeg / Firefox），**不需要任何环境变量**。

产物：`msm_drm_drv_video.so`，安装到 `/usr/lib/aarch64-linux-gnu/dri/`。

## 当前能力边界

**已实现：能力查询。** `vainfo` 能列出 profiles 与配置属性，ffmpeg 能建立 VAAPI 连接。

**未实现：解码数据路径。** `vaCreateSurfaces` / `vaCreateContext` / `vaCreateBuffer` /
`vaBeginPicture` / `vaRenderPicture` / `vaEndPicture` / `vaSyncSurface` /
`vaDeriveImage` / `vaExportSurfaceHandle` 目前全部返回 `VA_STATUS_ERROR_UNIMPLEMENTED`。
现在还**不能真正解码出画面** —— 这一步是骨架，不是完整驱动。

声明的 profile 严格对应 daemon 已验证的能力：

| Profile | 对应 daemon codec | 真机验证 |
|---------|------------------|---------|
| `VAProfileH264ConstrainedBaseline` / `Main` / `High` | 0（H.264） | 已验证 |
| `VAProfileHEVCMain` | 1（HEVC） | 已验证 |
| `VAProfileVP9Profile0` | 2（VP9） | 已验证 |
| `VAProfileVP8Version0_3` | 3（VP8） | 已验证 |

**不声明高位深**（HEVC Main10、VP9 Profile2、H.264 High10）：硬件可能支持，
但未验证。谎报能力会让消费者选中我们然后失败，比不报更糟。

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
