# 自定义 VAAPI Proxy Driver 调研报告：MediaCodec → VA-API 桥接

> ⚠️ **历史存档（开工前的可行性调研）。不要当作当前实现的说明。**
>
> 本文写在动手之前，其中相当一部分设计假设**没有被最终实现采用**，
> 少数结论已被实现经验推翻。保留它是因为论证过程与取舍理由仍有参考价值，
> 但凡与下列文档冲突处，**以它们为准**：
>
> - 当前能力与已知限制 → 仓库根 `CHANGELOG.md`
> - 架构与通信协议 → 仓库根 `README.md`
> - 驱动实现细节与踩坑记录 → `vaapi-driver/README.md`
> - 平台事实 → `doc/verified-platform-facts.md`
>
> 已知与本文不符的几点（举例，非穷举）：代码量估算远低于实际规模；
> 配对机制最终改为"按输入单元序号精确匹配"，与输出顺序解耦；
> 控制通道至今仍是 TCP，未迁到 Unix socket。

## 环境摘要

| 项目 | 值 |
|------|-----|
| 容器 | Debian 13 trixie aarch64 |
| Mesa | 25.0.7 |
| GPU | Adreno 640 (Turnip Vulkan driver) |
| Android 侧 | MediaCodec daemon，通过 abstract socket 通信 |
| 目标 | Chromium/Firefox 自动发现 VA-API driver，使用 MediaCodec 硬件解码 |

---

## 1. VA-API Driver 结构

### 1.1 标准接口概述

VA-API driver 是一个共享库（`.so`），由 `libva` 在运行时通过 `dlopen` 加载。核心接口定义在两个头文件中：

- **`va/va.h`** — 公开 API（profiles、entrypoints、buffer types 等）
- **`va/va_backend.h`** — 驱动后端私有接口（`VADriverVTable`、`VADriverInit` 等）

> 来源：[libva va_backend.h](https://github.com/intel/libva/blob/master/va/va_backend.h)

### 1.2 驱动加载流程

libva 通过 `va_openDriver()` 加载驱动，流程如下：

```
1. 读取环境变量 LIBVA_DRIVERS_PATH（或使用默认路径 VA_DRIVERS_PATH）
2. 拼接路径：{driver_dir}/{driver_name}_drv_video.so
3. dlopen 该 .so
4. dlsym 查找入口函数：__vaDriverInit_{major}_{minor}
   - 当前版本：__vaDriverInit_1_0（兼容 1.1、1.2 等）
5. 调用该函数，传入 VADriverContextP
6. 驱动在函数内填充 VADriverVTable
```

**驱动命名规则**：
- 文件名：`{name}_drv_video.so`（如 `nvidia_drv_video.so`、`radeonsi_drv_video.so`）
- 入口函数：`__vaDriverInit_1_0`
- 驱动名通过 `LIBVA_DRIVER_NAME` 环境变量指定

> 来源：[libva va.c — va_openDriver()](https://github.com/intel/libva/blob/master/va/va.c)

### 1.3 VADriverVTable（必须实现的函数）

`VADriverVTable` 定义在 `va/va_backend.h` 中，是驱动必须填充的函数指针表。**核心函数**（按重要性排序）：

#### 最小可工作驱动（仅解码）必须实现：

| 函数 | 用途 | 优先级 |
|------|------|--------|
| `vaTerminate` | 销毁驱动上下文 | ★★★ |
| `vaQueryConfigProfiles` | 返回支持的 codec profiles | ★★★ |
| `vaQueryConfigEntrypoints` | 返回 profile 的 entrypoints（VLD=解码） | ★★★ |
| `vaCreateConfig` | 创建解码配置 | ★★★ |
| `vaDestroyConfig` | 销毁配置 | ★★ |
| `vaGetConfigAttributes` | 查询配置属性 | ★★ |
| `vaCreateSurfaces` | 创建解码输出 surfaces | ★★★ |
| `vaDestroySurfaces` | 销毁 surfaces | ★★ |
| `vaCreateContext` | 创建解码上下文 | ★★★ |
| `vaDestroyContext` | 销毁上下文 | ★★ |
| `vaCreateBuffer` | 创建 bitstream/slice 缓冲区 | ★★★ |
| `vaMapBuffer` | 映射缓冲区到用户空间 | ★★★ |
| `vaUnmapBuffer` | 取消映射 | ★★ |
| `vaDestroyBuffer` | 销毁缓冲区 | ★★ |
| `vaBeginPicture` | 开始解码一帧 | ★★★ |
| `vaRenderPicture` | 提交解码数据 | ★★★ |
| `vaEndPicture` | 结束解码 | ★★★ |
| `vaSyncSurface` | 等待解码完成 | ★★★ |
| `vaQuerySurfaceStatus` | 查询 surface 状态 | ★★ |
| `vaExportSurfaceHandle` | **导出 DMA-BUF（Chromium 必需）** | ★★★ |

#### 可选但推荐实现：

| 函数 | 用途 |
|------|------|
| `vaQueryConfigAttributes` | 查询已创建配置的属性 |
| `vaBufferSetNumElements` | 设置缓冲区元素数 |
| `vaQuerySurfaceAttributes` | 查询 surface 支持的属性 |
| `vaQueryImageFormats` | 查询支持的图像格式 |
| `vaDeriveImage` | 从 surface 派生图像 |
| `vaLockSurface` / `vaUnlockSurface` | CPU 访问 surface |

> 来源：[nvidia-vaapi-driver vabackend.c](https://github.com/elFarto/nvidia-vaapi-driver/blob/master/src/vabackend.c) — 1684★，完整的非 Mesa VA-API driver 实现参考

### 1.4 Mesa 中的 VA-API Driver 示例

Mesa 中 VA-API driver 位于 `src/va/` 目录：

| Driver | 路径 | 说明 |
|--------|------|------|
| **Venus** | `src/va/venus/` | Qualcomm Vulkan Video → VA-API（最相关！） |
| **Nouveau** | `src/va/nouveau/` | NVIDIA 开源驱动的 VA-API |
| **Radeonsi** | `src/va/radeonsi/` | AMD Radeon 的 VA-API |
| **Intel** | `src/va/intel/` | Intel i965 VA-API |
| **Freedreno** | `src/va/freedreno/` | Qualcomm Adreno（Turnip 后端）|

> **关键发现**：Mesa 的 `src/va/freedreno/` 目录实现了 Adreno GPU 的 VA-API，但它是通过 Turnip Vulkan 驱动做硬件解码的，不是 MediaCodec。Venus driver 是通过 Vulkan Video 扩展做解码的另一个参考。

> 来源：[Mesa 源码 src/va/](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/va)

### 1.5 Driver 安装路径

```
# 默认路径（取决于编译配置）
/usr/lib/x86_64-linux-gnu/dri/          # Debian/Ubuntu aarch64
/usr/lib/aarch64-linux-gnu/dri/         # aarch64
/usr/lib64/dri/                          # Fedora/RHEL
/usr/local/lib/dri/                      # 手动安装

# 可通过 LIBVA_DRIVERS_PATH 覆盖
export LIBVA_DRIVERS_PATH=/path/to/your/drivers
```

---

## 2. VA-API Profile / EntryPoint

### 2.1 H.264 解码所需的 Profile 和 EntryPoint

| 类型 | 值 | 说明 |
|------|-----|------|
| **VAProfileH264Baseline** | 5 | Baseline profile（已弃用） |
| **VAProfileH264Main** | 6 | Main profile |
| **VAProfileH264High** | 7 | High profile |
| **VAProfileH264ConstrainedBaseline** | 13 | Constrained Baseline |
| **VAEntrypointVLD** | 1 | **Video DecodeLD（完全解码）** |

**H.264 解码的标准调用序列**：
```c
// 1. 查询支持的 entrypoints
vaQueryConfigEntrypoints(dpy, VAProfileH264Main, entrypoints, &num);

// 2. 创建配置
vaCreateConfig(dpy, VAProfileH264Main, VAEntrypointVLD, NULL, 0, &config);

// 3. 创建 surfaces
vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, width, height, surfaces, num, NULL, 0);

// 4. 创建上下文
vaCreateContext(dpy, config, width, height, VA_PROGRESSIVE, surfaces, num, &context);

// 5. 解码循环
vaBeginPicture(dpy, context, surface);
vaRenderPicture(dpy, context, buffers, num_buffers);
vaEndPicture(dpy, context);
vaSyncSurface(dpy, surface);
```

### 2.2 Chromium 常用的 Profile

Chromium 通常查询以下 profiles（按优先级）：
1. `VAProfileH264Main` / `VAProfileH264High` — H.264
2. `VAProfileHEVCMain` / `VAProfileHEVCMain10` — HEVC
3. `VAProfileVP9Profile0` / `VAProfileVP9Profile2` — VP9
4. `VAProfileAV1Profile0` / `VAProfileAV1Profile1` — AV1

### 2.3 查询设备支持的 profile

```bash
# 使用 libva-utils 的 vainfo 工具
vainfo

# 或编程查询
VAProfile profiles[vaMaxNumProfiles(dpy)];
int num_profiles;
vaQueryConfigProfiles(dpy, profiles, &num_profiles);
```

> 来源：[libva va.h — VAProfile 枚举](https://github.com/intel/libva/blob/master/va/va.h)

---

## 3. DMA-BUF 共享

### 3.1 核心机制

VA-API 与 Chromium 之间通过 **DMA-BUF** 共享解码后的视频帧。流程：

```
MediaCodec daemon                    VA-API Proxy Driver                 Chromium
     |                                      |                              |
     |  1. MediaCodec 解码完成               |                              |
     |  2. 返回 output buffer (fd)           |                              |
     |<-------- abstract socket ----------->|                              |
     |                                      |  3. 包装为 VASurface          |
     |                                      |  4. ExportSurfaceHandle()    |
     |                                      |------ DMA-BUF fd ---------->|
     |                                      |  5. 填充                      |
     |                                      |  VADRMPRIMESurfaceDescriptor |
     |                                      |                              | 6. 送入 GPU 渲染
```

### 3.2 VADRMPRIMESurfaceDescriptor 结构

这是 `vaExportSurfaceHandle()` 返回给 Chromium 的核心数据结构：

```c
typedef struct _VADRMPRIMESurfaceDescriptor {
    uint32_t fourcc;          // 像素格式（如 VA_FOURCC_NV12）
    uint32_t width;           // 宽度
    uint32_t height;          // 高度
    uint32_t num_objects;     // DRM 对象数量
    struct {
        int fd;               // DMA-BUF 文件描述符
        uint32_t size;        // 对象总大小
        uint64_t drm_format_modifier;  // DRM 格式修饰符
    } objects[4];
    uint32_t num_layers;      // 层数
    struct {
        uint32_t drm_format;  // DRM 格式（如 DRM_FORMAT_NV12）
        uint32_t num_planes;  // 平面数
        uint32_t object_index[4];
        uint32_t offset[4];   // 各平面偏移
        uint32_t pitch[4];    // 各平面 stride
    } layers[4];
} VADRMPRIMESurfaceDescriptor;
```

> 来源：[libva va_drmcommon.h](https://github.com/intel/libva/blob/master/va/va_drmcommon.h)

### 3.3 DMA-BUF Heap（Linux 5.6+）

在 aarch64 上分配 DMA-BUF 的方式：

```c
// 方式 1：通过 /dev/dma_heap/system（推荐，Linux 5.6+）
int heap_fd = open("/dev/dma_heap/system", O_RDONLY);
struct dma_heap_allocation_data alloc = {
    .len = size,
    .fd_flags = O_RDWR | O_CLOEXEC,
};
ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc);
int dmabuf_fd = alloc.fd;

// 方式 2：通过 drmPrimeHandleToDMA（从 GEM handle 转换）
struct drm_prime_handle handle = { .handle = gem_handle };
ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &handle);
int dmabuf_fd = handle.fd;

// 方式 3：Android 侧 MediaCodec 直接返回 buffer 的 fd
// MediaCodec 的 output buffer 本身可能就是 DMA-BUF
```

### 3.4 Chromium 的 DMA-BUF 消费

Chromium 通过 `vaExportSurfaceHandle()` 获取 `VADRMPRIMESurfaceDescriptor`，然后：

1. 使用 `drmPrimeFDToHandle()` 将 DMA-BUF fd 导入为 DRM GEM handle
2. 通过 EGL 的 `EGL_EXT_image_dma_buf_import` 扩展创建 EGLImage
3. 将 EGLImage 送入 OpenGL ES/Vulkan 渲染管线

**关键要求**：
- Surface 必须使用 `VA_EXPORT_SURFACE_SEPARATE_LAYERS` 标志导出
- 每个 plane 必须在独立的 DRM 对象中（或同一对象的不同偏移）
- NV12 格式需要 2 个 plane：Y（DRM_FORMAT_R8）和 UV（DRM_FORMAT_GR88）

> 来源：[nvidia-vaapi-driver export-buf.c](https://github.com/elFarto/nvidia-vaapi-driver/blob/master/src/export-buf.c)

---

## 4. 现有项目参考

### 4.1 nvidia-vaapi-driver（★★★★★ 最佳参考）

| 项目 | 说明 |
|------|------|
| **GitHub** | [elFarto/nvidia-vaapi-driver](https://github.com/elFarto/nvidia-vaapi-driver) |
| **Stars** | 1684★ |
| **架构** | NVDEC → CUDA → DMA-BUF → VA-API |
| **相关性** | **极高** — 这是一个完整的非 Mesa VA-API driver，将 NVIDIA 私有解码器桥接到 VA-API |

**为什么这是最佳参考**：
- 它是一个**独立的 VA-API driver**（不依赖 Mesa），完全在 `libva` 框架内实现
- 代码结构清晰：`vabackend.c`（主框架）、`h264.c`（codec 实现）、`export-buf.c`（DMA-BUF 导出）
- 直接展示了如何实现 `__vaDriverInit_1_0` 和填充 `VADriverVTable`
- 支持 Chromium 和 Firefox 的硬件解码

**代码结构**：
```
src/
├── vabackend.c          # 主框架：driver init、vtable、config、context、surface 管理
├── vabackend.h          # 数据结构定义（NVDriver、NVSurface、NVContext 等）
├── backend-common.c     # 公共后端逻辑
├── h264.c               # H.264 解码参数转换
├── hevc.c               # HEVC 解码
├── vp9.c                # VP9 解码
├── av1.c                # AV1 解码
├── export-buf.c         # DMA-BUF 导出（EGL 后端）
├── direct/
│   ├── nv-driver.c      # NVIDIA DRM 直接后端
│   └── direct-export-buf.c  # DMA-BUF 直接导出
├── kernels.c            # CUDA 内核（色彩转换）
└── list.c / stats.c     # 工具代码
```

### 4.2 Venus VA-API Driver（Mesa 内置）

| 项目 | 说明 |
|------|------|
| **路径** | Mesa `src/va/venus/` |
| **架构** | Vulkan Video → VA-API |
| **相关性** | **高** — Qualcomm 平台的参考实现 |

Venus 是 Mesa 中为 Qualcomm GPU 实现的 VA-API driver，它通过 Vulkan Video 扩展进行硬件解码。对于你的场景，如果 Adreno 640 支持 Vulkan Video 扩展，可以考虑直接使用 Venus driver。

> 来源：[Mesa venus driver](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/va/venus)

### 4.3 libvdpau-va-gl

| 项目 | 说明 |
|------|------|
| **GitHub** | [i-rinat/libvdpau-va-gl](https://github.com/i-rinat/libvdpau-va-gl) |
| **Stars** | 174★ |
| **架构** | VDPAU → OpenGL/VAAPI 后端 |
| **相关性** | 中 — 展示了 API 桥接的思路 |

### 4.4 Waydroid / Anbox

| 项目 | 说明 |
|------|------|
| **Waydroid** | [waydroid/waydroid](https://github.com/waydroid/waydroid) |
| **相关性** | 中 — Waydroid 在容器内运行 Android，需要解决 GPU/视频加速问题 |

Waydroid 的视频加速方案是通过 `lxc` 容器共享 `/dev/dri` 和 GPU 设备节点，但它使用的是 Android 的 Wayland compositor（`weston`），不是直接桥接 MediaCodec 到 VA-API。

---

## 5. Chromium 集成

### 5.1 Driver 发现机制

Chromium 通过 `libva` 发现 VA-API driver：

```
Chromium 启动
  → 调用 vaGetDisplayDRM(fd) 获取 display
  → 调用 vaInitialize(dpy)
    → libva 读取 LIBVA_DRIVER_NAME 环境变量
    → 在 LIBVA_DRIVERS_PATH 路径下查找 {name}_drv_video.so
    → dlopen 并调用 __vaDriverInit_1_0()
```

### 5.2 必要的环境变量

```bash
# 指定使用你的 proxy driver
export LIBVA_DRIVER_NAME=mediacodec    # 对应 libmediacodec_drv_video.so

# 指定 driver 搜索路径
export LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri:/path/to/custom/drivers

# 验证 driver 加载
vainfo  # 应该显示你的 driver 信息
```

### 5.3 Chromium 启动参数

```bash
# 标准 VA-API 硬件解码启动参数
chromium \
  --enable-features=VaapiVideoDecodeLinuxGL,VaapiVideoDecodeLinuxZeroInitialize \
  --ignore-gpu-blocklist \
  --use-gl=angle \
  --use-angle=gl \
  --ozone-platform=wayland
```

**关键 Chrome Flags**：

| Flag | 说明 |
|------|------|
| `--enable-features=VaapiVideoDecodeLinuxGL` | 启用 VA-API 视频解码（GL 路径） |
| `--enable-features=VaapiVideoDecodeLinuxZeroInitialize` | 零初始化 VA-API surfaces |
| `--ignore-gpu-blocklist` | 忽略 GPU 黑名单 |
| `--use-gl=angle` | 使用 ANGLE GL 实现 |
| `--use-angle=gl` | 使用 GL 后端 |
| `--ozone-platform=wayland` | Wayland 平台（如果使用） |

### 5.4 验证 VA-API 状态

```
# 在 Chromium 中访问
chrome://gpu

# 应该看到：
# Video Acceleration Information:
#   Decode h264 baseline:  16x16 to 4096x4096
#   Decode h264 main:      16x16 to 4096x4096
#   Decode h264 high:      16x16 to 4096x4096

# 或通过命令行验证
vainfo --display drm --device /dev/dri/renderD128
```

### 5.5 Firefox 配置

```bash
# Firefox 通过 about:config 配置
media.ffmpeg.vaapi.enabled=true
media.hardware-video-decoding.force-enabled=true
gfx.webrender.all=true
```

---

## 6. 可行的实现路径

### 6.1 架构设计

```
┌─────────────────────────────────────────────────────────┐
│                    Chromium / Firefox                     │
│                    (VA-API 客户端)                         │
└──────────────────────┬──────────────────────────────────┘
                       │ libva API 调用
                       ▼
┌─────────────────────────────────────────────────────────┐
│              libmediacodec_drv_video.so                  │
│              (自定义 VA-API Proxy Driver)                 │
│                                                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  Driver Init  │  │  Profile/    │  │  Surface/    │  │
│  │  __vaDriver   │  │  Config      │  │  Context     │  │
│  │  Init_1_0     │  │  Management  │  │  Management  │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  Bitstream   │  │  Decode      │  │  DMA-BUF     │  │
│  │  Buffer Mgmt │  │  Dispatch    │  │  Export       │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│                       │                                  │
│                       │ abstract socket                  │
└───────────────────────┼──────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────┐
│              MediaCodec Daemon (Android 侧)              │
│              接收 H.264 bitstream                        │
│              输出 NV12 DMA-BUF                           │
│              通过 abstract socket 回传 fd                │
└─────────────────────────────────────────────────────────┘
```

### 6.2 实现步骤（按优先级排序）

#### Phase 1：最小可行驱动（2-3 周）

**目标**：在 `vainfo` 中看到你的 driver，能查询 profiles

1. **创建 driver 骨架**
   ```c
   // mediacodec_drv_video.c
   
   // 1. 定义驱动内部数据结构
   typedef struct {
       int socket_fd;  // 到 MediaCodec daemon 的连接
       // ... surfaces, contexts 等
   } MediaCodecDriver;
   
   // 2. 实现 __vaDriverInit_1_0
   VAStatus __vaDriverInit_1_0(VADriverContextP ctx) {
       MediaCodecDriver *drv = calloc(1, sizeof(*drv));
       ctx->pDriverData = drv;
       ctx->vtable = &vtable;
       ctx->str_vendor = "MediaCodec VA-API Proxy";
       // 连接到 MediaCodec daemon
       drv->socket_fd = connect_to_mediacodec_daemon();
       return VA_STATUS_SUCCESS;
   }
   
   // 3. 实现 vtable 函数（先返回固定值）
   static const struct VADriverVTable vtable = {
       .vaTerminate = mcTerminate,
       .vaQueryConfigProfiles = mcQueryConfigProfiles,
       .vaQueryConfigEntrypoints = mcQueryConfigEntrypoints,
       // ... 其他函数
   };
   ```

2. **实现 profile 查询**
   ```c
   static VAStatus mcQueryConfigProfiles(VADriverContextP ctx,
                                          VAProfile *profiles, int *num) {
       profiles[0] = VAProfileH264Main;
       profiles[1] = VAProfileH264High;
       profiles[2] = VAProfileHEVCMain;
       *num = 3;
       return VA_STATUS_SUCCESS;
   }
   ```

3. **编译为共享库**
   ```bash
   gcc -shared -fPIC -o libmediacodec_drv_video.so mediacodec_drv_video.c \
       -I/usr/include/va -lva-drm -lpthread
   ```

4. **测试**
   ```bash
   export LIBVA_DRIVER_NAME=mediacodec
   export LIBVA_DRIVERS_PATH=/path/to/your/lib
   vainfo
   ```

#### Phase 2：Socket 通信 + Surface 管理（3-4 周）

1. **定义 socket 协议**
   ```c
   // 请求类型
   enum mc_request_type {
       MC_REQ_CREATE_SESSION,    // 创建解码会话
       MC_REQ_DECODE,            // 提交解码请求
       MC_REQ_RELEASE,           // 释放 buffer
       MC_REQ_QUERY_CAPABILITY,  // 查询支持的 codec
   };
   
   // 请求/响应结构
   struct mc_request {
       uint32_t type;
       uint32_t session_id;
       uint32_t data_size;
       uint8_t data[];  // bitstream 或参数
   };
   
   struct mc_response {
       uint32_t status;
       int32_t dmabuf_fd;    // 解码后的 DMA-BUF fd
       uint32_t width;
       uint32_t height;
       uint32_t fourcc;
   };
   ```

2. **实现 Surface 管理**
   - `vaCreateSurfaces`：向 MediaCodec daemon 请求 surfaces
   - `vaBeginPicture`/`vaRenderPicture`/`vaEndPicture`：发送 bitstream 并触发解码
   - `vaSyncSurface`：等待解码完成
   - `vaExportSurfaceHandle`：返回 DMA-BUF fd 给 Chromium

#### Phase 3：DMA-BUF 导出（2-3 周）

1. **实现 `vaExportSurfaceHandle`**
   ```c
   static VAStatus mcExportSurfaceHandle(VADriverContextP ctx,
                                          VASurfaceID surface_id,
                                          uint32_t mem_type,
                                          uint32_t flags,
                                          void *descriptor) {
       if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
           return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
       
       VADRMPRIMESurfaceDescriptor *desc = descriptor;
       MediaCodecSurface *surface = get_surface(ctx, surface_id);
       
       // 从 MediaCodec daemon 获取的 DMA-BUF fd
       desc->fourcc = VA_FOURCC_NV12;
       desc->width = surface->width;
       desc->height = surface->height;
       desc->num_objects = 1;
       desc->objects[0].fd = surface->dmabuf_fd;
       desc->objects[0].size = surface->size;
       desc->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
       desc->num_layers = 1;
       desc->layers[0].drm_format = DRM_FORMAT_NV12;
       desc->layers[0].num_planes = 2;
       desc->layers[0].object_index[0] = 0;
       desc->layers[0].object_index[1] = 0;
       desc->layers[0].offset[0] = 0;
       desc->layers[0].offset[1] = surface->uv_offset;
       desc->layers[0].pitch[0] = surface->stride;
       desc->layers[0].pitch[1] = surface->stride;
       
       return VA_STATUS_SUCCESS;
   }
   ```

#### Phase 4：Chromium 集成测试（1-2 周）

1. **安装 driver**
   ```bash
   cp libmediacodec_drv_video.so /usr/lib/aarch64-linux-gnu/dri/
   ```

2. **配置环境**
   ```bash
   export LIBVA_DRIVER_NAME=mediacodec
   ```

3. **启动 Chromium 测试**
   ```bash
   chromium \
     --enable-features=VaapiVideoDecodeLinuxGL \
     --ignore-gpu-blocklist \
     --use-gl=angle \
     --use-angle=gl
   ```

4. **验证**
   - 访问 `chrome://gpu` 查看 Video Acceleration 信息
   - 播放 H.264 视频，观察 GPU 使用率
   - 使用 `vainfo` 确认 driver 加载

### 6.3 关键技术挑战

| 挑战 | 解决方案 | 优先级 |
|------|----------|--------|
| **DMA-BUF fd 传递** | MediaCodec daemon 通过 socket SCM_RIGHTS 发送 fd | ★★★ |
| **线程安全** | 使用 mutex 保护共享状态 | ★★★ |
| **Surface 生命周期** | 实现引用计数，确保 DMA-BUF 在使用期间不被释放 | ★★★ |
| **色彩空间转换** | 如果 MediaCodec 输出不是 NV12，需要在 daemon 侧转换 | ★★ |
| **多实例支持** | 支持多个同时解码的 session | ★★ |
| **错误恢复** | 处理 daemon 崩溃、socket 断开等情况 | ★★ |

### 6.4 最小代码量估算

| 模块 | 代码行数 | 说明 |
|------|----------|------|
| Driver 框架 | ~500 行 | init、vtable、config、context |
| Socket 通信 | ~300 行 | 连接、协议解析、fd 传递 |
| Surface 管理 | ~400 行 | 创建、导出、生命周期 |
| Buffer 管理 | ~300 行 | 创建、映射、销毁 |
| H.264 参数转换 | ~200 行 | VA-API → MediaCodec 参数 |
| 构建脚本 | ~50 行 | meson.build 或 Makefile |
| **总计** | **~1750 行** | 最小可用驱动 |

---

## 7. 总结

### 核心发现

1. **VA-API driver 是一个 dlopen 的 .so**，必须导出 `__vaDriverInit_1_0` 函数
2. **VADriverVTable 是核心**，需要实现约 20 个函数（解码路径）
3. **DMA-BUF 是帧共享的关键**，通过 `vaExportSurfaceHandle()` 传递给 Chromium
4. **nvidia-vaapi-driver 是最佳参考**（1684★），它展示了完整的非 Mesa VA-API driver 实现
5. **Chromium 通过 LIBVA_DRIVER_NAME 环境变量发现 driver**

### 推荐的实现路径

1. **Phase 1**：基于 nvidia-vaapi-driver 的代码结构，创建最小 driver 骨架
2. **Phase 2**：实现与 MediaCodec daemon 的 socket 通信
3. **Phase 3**：实现 DMA-BUF 导出
4. **Phase 4**：集成测试和优化

### 关键参考资源

| 资源 | URL | 用途 |
|------|-----|------|
| nvidia-vaapi-driver | https://github.com/elFarto/nvidia-vaapi-driver | 完整 driver 参考 |
| libva | https://github.com/intel/libva | VA-API 规范和头文件 |
| Mesa VA-API | https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/va | Mesa 驱动实现 |
| VA-API 规范 | https://spec.oneapi.io/versions/latest/elements/oneVAAPI/source/index.html | 官方规范 |
| DRM/KMS 文档 | https://docs.kernel.org/gpu/drm-uapi.html | DMA-BUF 接口 |

---

*报告生成时间：2026-08-22*
*数据来源：GitHub API、libva 源码、nvidia-vaapi-driver 源码、Mesa 文档*
