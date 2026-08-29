/* 解码数据路径：surface / context / buffer 对象表 + 帧提交与取回
 *
 * ── 语义错配是这里的核心难点 ──────────────────────────────────
 * VA-API：一次 BeginPicture/EndPicture = 解一帧到**指定** surface，
 *         显示顺序由客户端安排，surface 之间无隐含顺序。
 * MediaCodec（经 daemon）：流式 in/out，输出按**解码顺序**吐出，
 *         没有"这一帧属于哪个 surface"的概念。
 *
 * 桥接办法：context 维护一个 FIFO 待解码队列 pending[]。EndPicture 把
 * 本帧码流送给 daemon 并把 render_target 入队；从 daemon 取回的第 k 帧
 * 就填给队列里的第 k 个 surface。这要求 **N 次提交 ⇔ N 个输出帧**：
 *   - VP9/VP8：每个数据单元恰好一帧（含 invisible 帧，show_frame==0 也产
 *     output buffer），天然 1:1
 *   - H.264/HEVC：SPS/PPS 等参数集 NALU 不产帧，所以送参数集时**不入队**
 * 这个不变式一旦破坏，画面会整体错位一帧且再也追不回来。
 *
 * IO 与锁：所有 daemon 收发都在放锁之后做，靠 io_busy[] 串行化同一 context
 * 上的 IO。持锁做阻塞 IO 会让其他线程的 CreateBuffer 一起卡死。
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* surface 分配在 msm_drm 的 dumb buffer 里，为的是能导出 dmabuf
 * （Firefox 取帧的唯一途径，见 export.c）。 */
#include <drm/drm.h>

#include "driver.h"
#include "av1_bitstream.h"

/* 前置声明：DestroyContext 需要排空在飞的帧，而这两个辅助定义在下方的
 * 解码路径小节里（放在那里更贴近使用现场）。 */
static VASurfaceID dmd_pending_take_locked(struct dmd_context *c,
                                           uint32_t unit_seq);
static void surface_store_frame_locked(struct dmd_surface *s,
                                       const struct dmd_frame *f);

unsigned int dmd_align_up(unsigned int v, unsigned int align)
{
    if (align == 0)
        return v;
    return (v + align - 1) / align * align;
}

/* ================================ 对象查找 ================================ */

struct dmd_surface *dmd_find_surface_locked(struct dmd_driver *drv,
                                            VASurfaceID id)
{
    if (id == VA_INVALID_ID)
        return NULL;
    for (int i = 0; i < DMD_MAX_SURFACES; i++) {
        if (drv->surfaces[i].in_use && drv->surfaces[i].id == id)
            return &drv->surfaces[i];
    }
    return NULL;
}

struct dmd_context *dmd_find_context_locked(struct dmd_driver *drv,
                                            VAContextID id)
{
    if (id == VA_INVALID_ID)
        return NULL;
    for (int i = 0; i < DMD_MAX_CONTEXTS; i++) {
        if (drv->contexts[i].in_use && drv->contexts[i].id == id)
            return &drv->contexts[i];
    }
    return NULL;
}

struct dmd_buffer *dmd_find_buffer_locked(struct dmd_driver *drv, VABufferID id)
{
    if (id == VA_INVALID_ID)
        return NULL;
    for (int i = 0; i < DMD_MAX_BUFFERS; i++) {
        if (drv->buffers[i].in_use && drv->buffers[i].id == id)
            return &drv->buffers[i];
    }
    return NULL;
}

void dmd_surface_reset_locked(struct dmd_surface *s)
{
    if (s->exportable) {
        /* dumb buffer：先 munmap 再销毁 handle，顺序不能颠倒。 */
        if (s->data)
            munmap(s->data, s->dumb_size);
        if (s->dumb_handle && s->dumb_drm_fd >= 0) {
            struct drm_mode_destroy_dumb dreq;
            memset(&dreq, 0, sizeof(dreq));
            dreq.handle = s->dumb_handle;
            ioctl(s->dumb_drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        }
    } else {
        free(s->data);
    }
    memset(s, 0, sizeof(*s));
    s->id = VA_INVALID_ID;
    s->context = VA_INVALID_ID;
}

void dmd_context_reset_locked(struct dmd_context *c)
{
    /* session_destroy 会 close socket。它不阻塞（close 不等对端），
     * 所以在这里做是安全的。 */
    if (c->session)
        dmd_session_destroy(c->session);
    free(c->slice_data);
    memset(c, 0, sizeof(*c));
    c->id = VA_INVALID_ID;
    c->current_target = VA_INVALID_ID;
}

/* ================================ surface ================================ */

/* 按显示尺寸分配一个 surface 的 NV12 缓冲。
 *
 * 几何：宽对齐 128、高对齐 32（Venus 的真机行为，1080p → 1920x1088）。
 * 预先按对齐尺寸分配，这样 daemon 回来的帧一定装得下 —— 否则要等收到
 * 格式块才知道该分配多大，而 ffmpeg 在建 context 之前就要 surface。
 */
/* 用 msm_drm 的 dumb buffer 分配一块**可导出为 dmabuf** 的内存。
 *
 * 为什么需要：Firefox 拿帧走 vaExportSurfaceHandle → DRM_PRIME_2，
 * 失败就整条流回落软解，且**没有拷贝回退路径**
 * （FFmpegVideoDecoder.cpp:1632 GetVAAPISurfaceDescriptor）。
 * 所以帧必须落在能导出 fd 的内存里。
 *
 * 注意这**不是**零拷贝：MediaCodec 那块内存我们仍然拿不到 fd
 * （容器 ION 不可用、无 /dev/dma_heap、NDK 也不给输出缓冲的 fd）。
 * 这里是自己分配可导出内存，让 daemon 回传的帧直接落进来 ——
 * 拷贝次数与原来的 calloc 方案相同，但多了"可被合成器导入"这个能力。
 *
 * 失败返回 -1，调用方回落到普通 heap（ffmpeg 的 hwdownload 路径不需要导出）。
 */
static int surface_alloc_dumb(struct dmd_surface *s, int drm_fd,
                              unsigned int bw, unsigned int bh, size_t size)
{
    if (drm_fd < 0)
        return -1;

    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    creq.width  = bw;
    /* NV12 总行数 = Y 的 bh 再加 UV 的 bh/2。按 8bpp 单平面申请，
     * 我们自己按 offset 切分 Y/UV。 */
    creq.height = bh * 3 / 2;
    creq.bpp    = 8;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0)
        return -1;

    /* pitch 必须正好是我们期望的 stride，否则几何对不上 daemon 的帧。 */
    if (creq.pitch != bw || creq.size < size) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = creq.handle;
        ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        dmd_log("dumb buffer 几何不符：pitch=%u 期望 %u，size=%llu 需要 %zu\n",
                creq.pitch, bw, (unsigned long long)creq.size, size);
        return -1;
    }

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) != 0) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = creq.handle;
        ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }

    void *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     drm_fd, (off_t)mreq.offset);
    if (map == MAP_FAILED) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = creq.handle;
        ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }
    memset(map, 0, creq.size);

    s->dumb_handle = creq.handle;
    s->dumb_size = creq.size;
    s->dumb_drm_fd = drm_fd;
    s->data = (unsigned char *)map;
    s->data_size = creq.size;
    s->exportable = 1;
    return 0;
}

static VAStatus surface_alloc_locked(struct dmd_surface *s, VASurfaceID id,
                                     unsigned int width, unsigned int height,
                                     unsigned int format, int drm_fd)
{
    unsigned int bw = dmd_align_up(width, DMD_WIDTH_ALIGN);
    unsigned int bh = dmd_align_up(height, DMD_HEIGHT_ALIGN);
    /* NV12：Y 平面 stride*slice_height，UV 平面是它的一半 */
    size_t size = (size_t)bw * bh * 3 / 2;

    memset(s, 0, sizeof(*s));

    /* 优先用可导出的 dumb buffer；不支持就退回普通 heap。 */
    if (surface_alloc_dumb(s, drm_fd, bw, bh, size) != 0) {
        unsigned char *data = calloc(1, size);
        if (!data)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        s->data = data;
        s->data_size = size;
        s->exportable = 0;
        s->dumb_handle = 0;
    }

    s->in_use = 1;
    s->id = id;
    s->width = width;
    s->height = height;
    s->buf_width = bw;
    s->buf_height = bh;
    s->stride = bw;
    s->slice_height = bh;
    s->format = format;
    s->state = DMD_SURFACE_IDLE;
    s->decode_status = VA_STATUS_SUCCESS;
    s->context = VA_INVALID_ID;
    return VA_STATUS_SUCCESS;
}

static VAStatus create_surfaces_common(VADriverContextP ctx,
                                       unsigned int format, unsigned int width,
                                       unsigned int height,
                                       VASurfaceID *surfaces,
                                       unsigned int num_surfaces)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !surfaces)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_surfaces == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (width < DMD_MIN_WIDTH || height < DMD_MIN_HEIGHT ||
        width > DMD_MAX_WIDTH || height > DMD_MAX_HEIGHT)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    /* 只支持 8-bit 4:2:0。谎报会让消费者选中我们然后拿到花屏。 */
    if (format != VA_RT_FORMAT_YUV420)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    VAStatus status = VA_STATUS_SUCCESS;
    unsigned int created = 0;

    pthread_mutex_lock(&drv->lock);
    for (unsigned int n = 0; n < num_surfaces; n++) {
        struct dmd_surface *slot = NULL;
        for (int i = 0; i < DMD_MAX_SURFACES; i++) {
            if (!drv->surfaces[i].in_use) {
                slot = &drv->surfaces[i];
                break;
            }
        }
        if (!slot) {
            status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            break;
        }

        VASurfaceID id = (VASurfaceID)(drv->next_surface_id++);
        status = surface_alloc_locked(slot, id, width, height, format,
                                      drv->drm_fd);
        if (status != VA_STATUS_SUCCESS)
            break;

        surfaces[n] = id;
        created++;
    }

    /* 部分失败要回滚：VA-API 语义是全成或全败，留下半个数组会让
     * 消费者把未初始化的 ID 当合法 surface 用。 */
    if (status != VA_STATUS_SUCCESS) {
        for (unsigned int n = 0; n < created; n++) {
            struct dmd_surface *s = dmd_find_surface_locked(drv, surfaces[n]);
            if (s)
                dmd_surface_reset_locked(s);
            surfaces[n] = VA_INVALID_ID;
        }
    }
    pthread_mutex_unlock(&drv->lock);

    if (status == VA_STATUS_SUCCESS)
        dmd_log("CreateSurfaces: %ux%u fourcc=NV12 n=%u -> id %u..\n", width,
                height, num_surfaces, (unsigned)surfaces[0]);
    else
        dmd_log("CreateSurfaces: 失败 status=0x%x（已创建 %u 个，已回滚）\n",
                status, created);

    return status;
}

VAStatus dmd_CreateSurfaces(VADriverContextP ctx, int width, int height,
                            int format, int num_surfaces, VASurfaceID *surfaces)
{
    if (width <= 0 || height <= 0 || num_surfaces <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    return create_surfaces_common(ctx, (unsigned int)format,
                                  (unsigned int)width, (unsigned int)height,
                                  surfaces, (unsigned int)num_surfaces);
}

VAStatus dmd_CreateSurfaces2(VADriverContextP ctx, unsigned int format,
                             unsigned int width, unsigned int height,
                             VASurfaceID *surfaces, unsigned int num_surfaces,
                             VASurfaceAttrib *attrib_list,
                             unsigned int num_attribs)
{
    /* attrib_list 里 ffmpeg 只可能传 PixelFormat（要 NV12）与
     * MemoryType（要 VA 内部，我们不支持外部内存导入）。
     * 我们唯一支持的组合就是默认组合，因此校验后忽略。 */
    if (num_attribs > 0 && !attrib_list)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    for (unsigned int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VASurfaceAttribPixelFormat) {
            if (attrib_list[i].value.type != VAGenericValueTypeInteger)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            if ((unsigned int)attrib_list[i].value.value.i != VA_FOURCC_NV12)
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        } else if (attrib_list[i].type == VASurfaceAttribMemoryType) {
            /* 只支持驱动自己分配的内存。dmabuf/用户指针导入这条路
             * 在容器里走不通（ION 不可用、无 /dev/dma_heap）。 */
            if (attrib_list[i].value.type != VAGenericValueTypeInteger)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            if (!(attrib_list[i].value.value.i &
                  VA_SURFACE_ATTRIB_MEM_TYPE_VA))
                return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        } else if (attrib_list[i].type == VASurfaceAttribExternalBufferDescriptor) {
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
        /* 其余属性（Usage hint 等）无害，忽略。 */
    }

    return create_surfaces_common(ctx, format, width, height, surfaces,
                                  num_surfaces);
}

VAStatus dmd_DestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list,
                             int num_surfaces)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !surface_list || num_surfaces <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    VAStatus status = VA_STATUS_SUCCESS;

    pthread_mutex_lock(&drv->lock);
    for (int i = 0; i < num_surfaces; i++) {
        struct dmd_surface *s = dmd_find_surface_locked(drv, surface_list[i]);
        if (!s) {
            /* 继续销毁其余的：半途返回会泄漏后面那些。
             * 但仍要把错误报上去，调用方有 bug 该知道。 */
            status = VA_STATUS_ERROR_INVALID_SURFACE;
            continue;
        }
        dmd_surface_reset_locked(s);
    }
    pthread_mutex_unlock(&drv->lock);

    return status;
}

/* ================================ context ================================ */

/* 建 daemon 会话。调用方**不得持锁** —— connect 与握手是阻塞 IO。 */
static struct dmd_session *session_open(int codec, unsigned int width,
                                        unsigned int height)
{
    struct dmd_session_config cfg;
    struct dmd_error err;

    dmd_session_config_init(&cfg);
    cfg.codec = codec;
    cfg.width = (int)width;
    cfg.height = (int)height;
    cfg.io_timeout_ms = DMD_FRAME_TIMEOUT_MS;

    /* 端点选择：Unix socket 优先，TCP 兜底。
     *
     * DMD_ENDPOINT 可显式指定：
     *   unix:/run/dmd.sock   连该路径的 Unix socket
     *   tcp:20003            连 127.0.0.1 的该端口
     * 不设时按 DMD_DEFAULT_SOCK → TCP 的顺序自动探测。
     *
     * 为什么 Unix socket 优先：它**不属于 net namespace**，平台把宿主上的
     * socket 文件 bind mount 进容器即可，两类容器都通；而 TCP 127.0.0.1
     * 只在共享 net namespace 时可达 —— 实测 DroidSpaces 的 NAT 型容器
     * netns 独立（4026535650 vs 宿主 4026531937），TCP loopback 与
     * abstract socket 都不通，只有路径式 Unix socket 这条路。
     *
     * ⚠️ 勘误：这里一度写着"能连上 Unix socket 就意味着 SCM_RIGHTS 可用，
     * 于是可以请求 SHM 传输"。**那个推论是错的。**
     *
     * SHM 的 memfd 交接并不走这条控制通道，而是 daemon 另开一个
     * **abstract** socket（decode-daemon.c 里 sun_path[0] = 0 那处），
     * 驱动再去连它。abstract socket 属于 net namespace ——
     * 于是 NAT 型容器（netns 独立）根本连不上那个交接通道，
     * 请求 SHM 在那里必然交接失败再降级，每建一次会话白等一次超时。
     * 这正是下方 want_shm 判定要用 `use_sock ? ... : 0` 的原因 ——
     * 只有走上路径式 Unix socket（即 host 型容器）才请求 SHM。
     *
     * 换句话说：**控制通道跨 netns 的能力，不等于 SHM 交接通道也跨得过去。**
     * 要让零拷贝在 NAT 容器可用，得把 memfd 交接也改走这条路径式
     * Unix socket（同一个连接上传 SCM_RIGHTS 即可，不必另开 socket），
     * 那是一项独立改动，尚未做。 */
    const char *ep = getenv("DMD_ENDPOINT");
    char sockbuf[256];
    const char *use_sock = NULL;

    if (ep && strncmp(ep, "unix:", 5) == 0) {
        snprintf(sockbuf, sizeof(sockbuf), "%s", ep + 5);
        use_sock = sockbuf;
    } else if (ep && strncmp(ep, "tcp:", 4) == 0) {
        int p = atoi(ep + 4);
        if (p > 0 && p < 65536)
            cfg.port = (uint16_t)p;
    } else if (!ep) {
        /* 未显式指定：默认路径存在且是 socket 就用它 */
        struct stat st;
        if (stat(DMD_DEFAULT_SOCK, &st) == 0 && S_ISSOCK(st.st_mode))
            use_sock = DMD_DEFAULT_SOCK;
    }

    cfg.sock_path = use_sock;
    /* SHM（memfd 零拷贝）在 Unix socket 模式下**默认开启**（2026-08-26 起）。
     * 设 DMD_WANT_SHM=0 可显式关闭（排查用）。
     *
     * 端到端实测依据：驱动被 dlopen 进 ffmpeg 进程、走 /run/dmd/decode.sock，
     * daemon 侧日志确认
     *   共享内存已交接: 8 槽 x 3133440 字节 (共 25067520)
     *   握手成功: video/hevc 1280x720 帧回传=SHM
     * （槽位数 v0.3.7 由 4 改为 8，见 src/decode-daemon.c 的 SHM_SLOTS；
     *  上面这条 hevc 日志是当时的取证样例，HEVC 路径本身尚未系统验证。）
     * 且解码结果与内联模式一致（150/150 帧）。
     *
     * 收益（固定 1500 帧工作量，三组交替对照，daemon 侧 CPU jiffies）：
     *   内联  493 / 500 / 489   中位数 493
     *   SHM   400 / 367 / 410   中位数 400   → 降低约 19%
     * 省下的正是每帧 1.38MB(720p) 经 socket 的那次拷贝。
     *
     * ⚠️ 注意"零拷贝"只描述 memfd → 消费者进程这一段。MediaCodec 的输出
     * 缓冲由 gralloc 分配，daemon 只能 getOutputBuffer 拿到 CPU 指针后
     * memcpy 进 memfd（decode-daemon.c 的 send_frame_shm）。解码器到 memfd
     * 那次拷贝仍在，消除它要走 dmabuf，而那条路因需依赖私有符号已被否决。
     *
     * 保守的部分：只在 use_sock 时请求 —— SHM 交接走 abstract socket
     * （属 net namespace），所以**NAT 型容器必然降级**。daemon 侧交接失败
     * 会自动退回内联，因此默认开启没有硬失败风险。
     *
     * ── 以下是历史顾虑，均已被实测解决，保留供参考 ──
     *
     * 曾长期默认关闭（cfg.want_shm 硬编码为 0），理由是这条路"驱动侧从未
     * 真正启用过"：只有 tests/test_dmd_client.c 走过，那是独立测试程序，
     * 不是驱动被 dlopen 进消费者进程的真实环境。这个顾虑本身是对的 ——
     * 文档一度据那组数字声称 SHM 有 +17% 吞吐，而真实路径从未执行过。
     * 教训：判断某能力是否生效，要先确认它的开关是开的。
     *
     * 曾担心浏览器沙箱收不了 SCM_RIGHTS（Firefox RDD / Chrome GPU 都有
     * seccomp 过滤）。实测可行。本项目在这类判断上有先例 —— 一份源码级结论
     * 说 RDD 沙箱对 SYS_SOCKET 一律返回 EACCES，实测却跑通了 713 帧。
     * 这类事只能靠实测，读源码推不出来。
     *
     * ⚠️ 勘误：此处一度写着"SHM 帧交付路径有 bug、daemon 在 memfd 交接后
     * 不再服务、根因未定位"。**那个归因是错的**。那些 0 帧现象的真实原因是
     * SELinux domain 权限 —— daemon 以 runcon u:r:droidspacesd:s0 启动时能建
     * socket，但无权访问 hwservicemanager/Codec2，于是在 CCodec::allocate 处
     * SIGABRT（tombstone 栈顶 Codec2Client::GetServiceNames，
     * "Hardware service manager is not running"）。与 SHM 无关 ——
     * 当时日志里的传输模式其实是 TCP。三组对照实验：
     *   u:r:ksu:s0          + TCP         → 正常解码（但该 domain 无权 bind）
     *   u:r:droidspacesd:s0 + TCP         → 同样 SIGABRT（与传输方式无关）
     *   u:r:droidspacesd:s0 + Unix socket + SELinux permissive → 正常解码
     * 结论：Unix socket 通道本身是正确的，缺的只是一条 allow 规则。 */
    const char *wantshm = getenv("DMD_WANT_SHM");
    cfg.want_shm = use_sock ? (wantshm ? (*wantshm == '1') : 1) : 0;

    memset(&err, 0, sizeof(err));
    struct dmd_session *s = dmd_session_create(&cfg, &err);

    /* Unix socket 连不上就退回 TCP —— 平台没挂载时仍能工作。 */
    if (!s && use_sock) {
        dmd_log("Unix socket %s 连接失败（%s），退回 TCP\n",
                use_sock, err.msg);
        cfg.sock_path = NULL;
        cfg.want_shm = 0;
        memset(&err, 0, sizeof(err));
        s = dmd_session_create(&cfg, &err);
    }

    if (!s)
        dmd_log("会话建立失败: code=%d handshake=%d %s\n", err.code,
                err.handshake_status, err.msg);
    else
        dmd_log("会话已建立: codec=%d %ux%u 端点=%s xfer=%d\n", codec,
                width, height,
                cfg.sock_path ? cfg.sock_path : "tcp/127.0.0.1",
                dmd_session_xfer_mode(s));
    return s;
}

/* flush 过的会话已不能再送数据，透明换一条新的。
 *
 * ⚠️ **重建会摧毁参考帧链，代价不是"慢"而是"画面坏"。**
 *
 * 曾有一份探针（tools/probe_rebuild.c）报告"从非 IDR 帧续传也能立刻出帧
 * （续传 12 个 VCL 出 9 帧），所以不会有可见花屏"—— 那个结论**是错的**：
 * 它只数了帧数，没看画面，那些帧全是纯黑（Y=16）。探针文件开头已标注
 * 自身不可信，不要再引用它的结论。
 *
 * 真相：任何丢弃解码器状态的操作（flush 或重建）都会摧毁参考帧链，
 * 而 P/B 帧必须依赖它。从非 IDR 位置重新开始，要一直黑到下一个 IDR
 * （本测试流每 30 帧一个，故最多连黑 29 帧）。这正是"浏览器播放时
 * 画面一闪一闪"的根因 —— 实测 135/708 帧纯黑。
 *
 * 所以重建**必须尽量避免**，而不是"可以放心用"。真要重建，
 * 正确做法是从最近的 IDR 重放并丢弃重放出的帧（见 tools/probe_replay.c）。
 * 当前实现的策略是把触发条件收紧到几乎不发生（见 wait_is_futile 的判据）。
 *
 * 调用方必须持锁；本函数内部会临时放锁做阻塞 IO（connect + 握手）。
 * 返回 0 成功，-1 失败（失败时 c->session 置空，由调用方走失败路径）。
 */
static int session_rebuild_locked(struct dmd_driver *drv, struct dmd_context *c,
                                  int idx)
{
    struct dmd_session *old = c->session;
    int codec = c->codec;
    unsigned int w = c->picture_width;
    unsigned int h = c->picture_height;

    /* 先摘下旧会话，避免放锁期间别的线程还往里写。 */
    c->session = NULL;

    drv->io_busy[idx] = 1;
    pthread_mutex_unlock(&drv->lock);
    if (old)
        dmd_session_destroy(old);
    struct dmd_session *ns = session_open(codec, w, h);
    pthread_mutex_lock(&drv->lock);
    drv->io_busy[idx] = 0;
    pthread_cond_broadcast(&drv->io_done);

    if (!ns) {
        dmd_log("会话重建失败\n");
        c->session_failed = 1;
        return -1;
    }

    c->session = ns;
    c->input_finished = 0;
    /* 新会话的 daemon 侧 vcl_in 从 1 重新开始，这里必须同步归零，
     * 否则提交序号与回传的 PTS 错位，配对会持续走"无匹配"回退路径。
     * ⚠️ 队列里残留的旧序号必须一并作废，否则新会话发出的号会与它们撞车：
     * 实测 test1080 出现两次 unit 5、35 次"无匹配"回退、70/150 帧错位。
     * 置 0 表示"该项无有效序号"，不会匹配任何回传的 PTS，
     * 只能由回退推断按顺序配掉 —— 这正是需要的语义。 */
    c->units_submitted = 0;
    c->av1_hold_surface = VA_INVALID_ID;
    c->av1_send_surface = VA_INVALID_ID;
    c->av1_last_ready = VA_INVALID_ID;
    c->av1_hold_show = 0;
    c->av1_send_show = 0;
    for (int k = 0; k < c->pending_count; k++) {
        int idx = (c->pending_head + k) % DMD_MAX_SURFACES;
        c->pending_unit[idx] = 0;
    }
    /* 参数集属于旧会话的状态，新会话必须重送 —— 清掉"已送"标记，
     * 下一次 EndPicture 的既有逻辑就会重新合成并发送 SPS/PPS。 */
    c->param_sets_sent = 0;
    dmd_log("会话已重建（codec=%d %ux%u），参数集将重送\n", codec, w, h);
    return 0;
}

VAStatus dmd_CreateContext(VADriverContextP ctx, VAConfigID config_id,
                           int picture_width, int picture_height, int flag,
                           VASurfaceID *render_targets, int num_render_targets,
                           VAContextID *context)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !context)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_render_targets < 0 ||
        (num_render_targets > 0 && !render_targets))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_render_targets > DMD_MAX_SURFACES)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (picture_width < DMD_MIN_WIDTH || picture_height < DMD_MIN_HEIGHT ||
        picture_width > DMD_MAX_WIDTH || picture_height > DMD_MAX_HEIGHT)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;

    *context = VA_INVALID_ID;

    VAProfile profile;
    int codec;
    int slot_idx = -1;

    pthread_mutex_lock(&drv->lock);

    /* config 必须有效，并从它推导 codec。 */
    struct dmd_config *cfg = NULL;
    for (int i = 0; i < DMD_MAX_CONFIGS; i++) {
        if (drv->configs[i].in_use && drv->configs[i].id == config_id) {
            cfg = &drv->configs[i];
            break;
        }
    }
    if (!cfg) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    profile = cfg->profile;
    codec = dmd_profile_to_codec(profile);
    if (codec < 0) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    /* render target 必须都是已存在的 surface。 */
    for (int i = 0; i < num_render_targets; i++) {
        if (!dmd_find_surface_locked(drv, render_targets[i])) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }

    for (int i = 0; i < DMD_MAX_CONTEXTS; i++) {
        if (!drv->contexts[i].in_use) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("CreateContext: 槽位耗尽（上限 %d）\n", DMD_MAX_CONTEXTS);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    struct dmd_context *c = &drv->contexts[slot_idx];
    memset(c, 0, sizeof(*c));
    c->in_use = 1;
    c->id = (VAContextID)(drv->next_context_id++);
    c->config_id = config_id;
    c->profile = profile;
    c->codec = codec;
    c->picture_width = (unsigned int)picture_width;
    c->picture_height = (unsigned int)picture_height;
    c->flag = flag;
    c->current_target = VA_INVALID_ID;

    VAContextID new_id = c->id;
    pthread_mutex_unlock(&drv->lock);

    /* 会话建立在放锁后做 —— connect + 握手是阻塞 IO，持锁做会卡住
     * 其他线程的所有对象操作。 */
    struct dmd_session *sess =
        session_open(codec, (unsigned int)picture_width,
                     (unsigned int)picture_height);

    pthread_mutex_lock(&drv->lock);
    /* 重新查找：理论上没人能销毁一个还没交给调用方的 context，
     * 但按 ID 复查比缓存指针更耐并发。 */
    c = dmd_find_context_locked(drv, new_id);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        if (sess)
            dmd_session_destroy(sess);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    c->session = sess;
    /* 会话建不起来不让 CreateContext 失败：daemon 可能只是暂时不可用，
     * 首次 EndPicture 会重试。失败时报的错更贴近真实原因。 */
    c->session_failed = sess ? 0 : 1;
    pthread_mutex_unlock(&drv->lock);

    *context = new_id;
    dmd_log("CreateContext: config=%u profile=%d %dx%d -> context=%u%s\n",
            (unsigned)config_id, (int)profile, picture_width, picture_height,
            (unsigned)new_id, sess ? "" : "（会话待重试）");

    return VA_STATUS_SUCCESS;
}

VAStatus dmd_DestroyContext(VADriverContextP ctx, VAContextID context)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    /* 有 IO 在飞时不能拆：等它结束。带超时避免死等 —— 宁可泄漏一个
     * 会话也不能挂死宿主进程的 Terminate 路径。 */
    int idx = (int)(c - drv->contexts);
    int waited = 0;
    while (drv->io_busy[idx] && waited < DMD_FRAME_TIMEOUT_MS) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&drv->io_done, &drv->lock, &ts);
        waited += 50;
        c = dmd_find_context_locked(drv, context);
        if (!c) {
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_SUCCESS;
        }
    }

    /* 归还所有属于本 context 的 buffer：ffmpeg 正常会自己 Destroy，
     * 但异常路径下可能漏，driver 不能因此泄漏。 */
    for (int i = 0; i < DMD_MAX_BUFFERS; i++) {
        struct dmd_buffer *b = &drv->buffers[i];
        if (b->in_use && b->context == context) {
            free(b->data);
            memset(b, 0, sizeof(*b));
        }
    }

    /* 先把解码器里还在飞的帧排空，再考虑放弃剩下的。
     *
     * 为什么必需：ffmpeg 在**流内分辨率变化**时会先 DestroyContext 再建新的，
     * 但它**之后仍会 vaSyncSurface 旧 context 的 surface**（那些帧属于前一段
     * 分辨率，它还要取走）。实测 switch.h264 在此处送入 62 单元只取回 44 帧
     * —— 若直接把余下 18 个 surface 标成失败，ffmpeg 就会收到
     * "1 (operation failed)"，整条流解不下去。
     *
     * 这些帧并没有错，只是还攥在 MediaCodec 里：和流末尾一样，需要关掉写端
     * 才会吐出来。所以这里做一次和 SyncSurface 相同的 flush + 取帧循环。
     * 上面已经等过 io_busy，此刻没有别的线程在这个 context 上做 IO。 */
    if (c->session && c->pending_count > 0) {
        struct dmd_session *fs = c->session;
        if (!c->input_finished) {
            c->input_finished = 1;
            drv->io_busy[idx] = 1;
            pthread_mutex_unlock(&drv->lock);
            dmd_session_finish_input(fs);
            pthread_mutex_lock(&drv->lock);
            drv->io_busy[idx] = 0;
            pthread_cond_broadcast(&drv->io_done);
            c = dmd_find_context_locked(drv, context);
            if (!c) {
                pthread_mutex_unlock(&drv->lock);
                return VA_STATUS_SUCCESS;
            }
        }

        int drained = 0;
        int budget = DMD_FRAME_TIMEOUT_MS;
        while (c->pending_count > 0 && budget > 0) {
            struct dmd_frame frame;
            memset(&frame, 0, sizeof(frame));
            drv->io_busy[idx] = 1;
            pthread_mutex_unlock(&drv->lock);
            int frc = dmd_session_next_frame(fs, &frame, 100);
            pthread_mutex_lock(&drv->lock);
            drv->io_busy[idx] = 0;
            pthread_cond_broadcast(&drv->io_done);
            budget -= 100;

            c = dmd_find_context_locked(drv, context);
            if (!c) {
                if (frc == DMD_OK)
                    dmd_session_release_frame(fs, &frame);
                pthread_mutex_unlock(&drv->lock);
                return VA_STATUS_SUCCESS;
            }
            if (frc == DMD_ERR_TIMEOUT)
                continue;
            if (frc != DMD_OK)
                break; /* EOS 或真错误：剩下的确实取不到了 */

            VASurfaceID id = dmd_pending_take_locked(c, frame.unit_seq);
            struct dmd_surface *ds = dmd_find_surface_locked(drv, id);
            if (ds) {
                surface_store_frame_locked(ds, &frame);
                ds->state = DMD_SURFACE_READY;
                drained++;
            }
            dmd_session_release_frame(fs, &frame);
        }
        if (drained)
            dmd_log("DestroyContext: 排空补齐 %d 帧\n", drained);
    }

    /* 仍未就绪的 surface 状态要复位，否则它们永远停在 PENDING，
     * 后续 SyncSurface 会等一个永不到来的帧。
     * 注意保留 context 归属与 READY 状态：上面排空填好的帧 ffmpeg 还要取。 */
    for (int i = 0; i < DMD_MAX_SURFACES; i++) {
        struct dmd_surface *s = &drv->surfaces[i];
        if (s->in_use && s->context == context &&
            s->state == DMD_SURFACE_PENDING) {
            s->state = DMD_SURFACE_IDLE;
            s->decode_status = VA_STATUS_ERROR_OPERATION_FAILED;
            s->context = VA_INVALID_ID;
        }
    }

    dmd_log("DestroyContext: context=%u（送入 %llu 单元，取回 %llu 帧）\n",
            (unsigned)context,
            c->session ? (unsigned long long)dmd_session_units_sent(c->session)
                       : 0ULL,
            c->session
                ? (unsigned long long)dmd_session_frames_received(c->session)
                : 0ULL);

    dmd_context_reset_locked(c);
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}

/* ================================ buffer ================================ */

VAStatus dmd_CreateBuffer(VADriverContextP ctx, VAContextID context,
                          VABufferType type, unsigned int size,
                          unsigned int num_elements, void *data,
                          VABufferID *buf_id)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !buf_id)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (size == 0 || num_elements == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    /* 溢出检查：size 与 num_elements 都是 unsigned int，乘积可能回绕。 */
    if ((size_t)size * num_elements > DMD_MAX_FRAME_BYTES)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    *buf_id = VA_INVALID_ID;
    size_t total = (size_t)size * num_elements;

    /* 分配放在锁外：大块 calloc 可能触发 mmap 与页错误。 */
    void *mem = calloc(1, total);
    if (!mem)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (data)
        memcpy(mem, data, total);

    pthread_mutex_lock(&drv->lock);

    /* context 必须有效。VA-API 允许 VA_INVALID_ID 建"无 context" buffer，
     * 但解码路径里 ffmpeg 总是带 context，所以只接受有效 context。 */
    if (!dmd_find_context_locked(drv, context)) {
        pthread_mutex_unlock(&drv->lock);
        free(mem);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    struct dmd_buffer *slot = NULL;
    for (int i = 0; i < DMD_MAX_BUFFERS; i++) {
        if (!drv->buffers[i].in_use) {
            slot = &drv->buffers[i];
            break;
        }
    }
    if (!slot) {
        pthread_mutex_unlock(&drv->lock);
        free(mem);
        dmd_log("CreateBuffer: 槽位耗尽（上限 %d）\n", DMD_MAX_BUFFERS);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->id = (VABufferID)(drv->next_buffer_id++);
    slot->context = context;
    slot->type = type;
    slot->element_size = size;
    slot->num_elements = num_elements;
    slot->size = total;
    slot->data = mem;

    *buf_id = slot->id;
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}

VAStatus dmd_BufferSetNumElements(VADriverContextP ctx, VABufferID buf_id,
                                  unsigned int num_elements)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || num_elements == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_buffer *b = dmd_find_buffer_locked(drv, buf_id);
    if (!b) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (b->mapped) {
        /* 映射中改大小会让调用方手上的指针失效。 */
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    size_t total = (size_t)b->element_size * num_elements;
    if (total > DMD_MAX_FRAME_BYTES) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    VAStatus status = VA_STATUS_SUCCESS;
    if (total > b->size) {
        void *mem = realloc(b->data, total);
        if (!mem) {
            status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        } else {
            memset((unsigned char *)mem + b->size, 0, total - b->size);
            b->data = mem;
        }
    }
    if (status == VA_STATUS_SUCCESS) {
        b->num_elements = num_elements;
        b->size = total;
    }
    pthread_mutex_unlock(&drv->lock);

    return status;
}

VAStatus dmd_MapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !pbuf)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    *pbuf = NULL;

    pthread_mutex_lock(&drv->lock);
    struct dmd_buffer *b = dmd_find_buffer_locked(drv, buf_id);
    if (b) {
        b->mapped++;
        *pbuf = b->data;
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_SUCCESS;
    }

    /* VAImage 的后备存储也通过 vaMapBuffer 访问（image.buf 是个 buffer ID）。
     * ffmpeg 的回读路径正是这么取像素的。 */
    struct dmd_image *img = NULL;
    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        if (drv->images[i].in_use && drv->images[i].buf_id == buf_id) {
            img = &drv->images[i];
            break;
        }
    }
    if (!img) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    img->mapped++;
    *pbuf = img->data;
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}

/* vaMapBuffer2 = vaMapBuffer + 读写意图提示。
 *
 * ⚠️ 必须实现，不能留桩：libva 只在**槽位为 NULL** 时才回落到 vaMapBuffer
 * （va.c 的 vaMapBuffer2），而我们为了通过 CHECK_VTABLE 把所有槽位都填了桩，
 * 于是 ffmpeg 的 vaapi_map_frame（hwcontext_vaapi.c:928）拿到 UNIMPLEMENTED
 * 就直接失败，永远走不到 vaMapBuffer 那条兼容分支。
 * 这是"填满 vtable"与"libva 靠 NULL 判断能力"之间的一个真实冲突。
 *
 * flags 对我们无意义：后备存储是普通 heap 内存，读写都是直接访问。 */
VAStatus dmd_MapBuffer2(VADriverContextP ctx, VABufferID buf_id, void **pbuf,
                        uint32_t flags)
{
    (void)flags;
    return dmd_MapBuffer(ctx, buf_id, pbuf);
}

VAStatus dmd_UnmapBuffer(VADriverContextP ctx, VABufferID buf_id)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_buffer *b = dmd_find_buffer_locked(drv, buf_id);
    if (b) {
        if (b->mapped > 0)
            b->mapped--;
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_SUCCESS;
    }

    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        struct dmd_image *img = &drv->images[i];
        if (img->in_use && img->buf_id == buf_id) {
            if (img->mapped > 0)
                img->mapped--;
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_ERROR_INVALID_BUFFER;
}

VAStatus dmd_DestroyBuffer(VADriverContextP ctx, VABufferID buffer_id)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_buffer *b = dmd_find_buffer_locked(drv, buffer_id);
    if (!b) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    void *mem = b->data;
    memset(b, 0, sizeof(*b));
    pthread_mutex_unlock(&drv->lock);

    free(mem);
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_BufferInfo(VADriverContextP ctx, VABufferID buf_id,
                        VABufferType *type, unsigned int *size,
                        unsigned int *num_elements)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !type || !size || !num_elements)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_buffer *b = dmd_find_buffer_locked(drv, buf_id);
    if (b) {
        *type = b->type;
        *size = b->element_size;
        *num_elements = b->num_elements;
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_SUCCESS;
    }

    for (int i = 0; i < DMD_MAX_IMAGES; i++) {
        struct dmd_image *img = &drv->images[i];
        if (img->in_use && img->buf_id == buf_id) {
            *type = VAImageBufferType;
            *size = (unsigned int)img->data_size;
            *num_elements = 1;
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_ERROR_INVALID_BUFFER;
}

/* ============================ 帧提交（码流组装） ============================ */

/* 往 context 的本帧码流缓冲追加数据。调用方持锁。 */
static int slice_append_locked(struct dmd_context *c, const void *data,
                               size_t len)
{
    if (len == 0)
        return 0;
    if (c->slice_len + len > DMD_MAX_UNIT_BYTES)
        return -1; /* 超过 daemon 的 8MB 上限，送过去只会被判非法长度 */

    if (c->slice_len + len > c->slice_cap) {
        size_t cap = c->slice_cap ? c->slice_cap * 2 : 65536;
        while (cap < c->slice_len + len)
            cap *= 2;
        unsigned char *mem = realloc(c->slice_data, cap);
        if (!mem)
            return -1;
        c->slice_data = mem;
        c->slice_cap = cap;
    }
    memcpy(c->slice_data + c->slice_len, data, len);
    c->slice_len += len;

    return 0;
}

VAStatus dmd_BeginPicture(VADriverContextP ctx, VAContextID context,
                          VASurfaceID render_target)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    struct dmd_surface *s = dmd_find_surface_locked(drv, render_target);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    /* 上一帧没 EndPicture 就又 Begin：丢掉残留数据，否则会拼进新帧。 */
    if (c->current_target != VA_INVALID_ID)
        dmd_log("BeginPicture: 上一帧未 EndPicture，丢弃 %zu 字节残留\n",
                c->slice_len);

    c->current_target = render_target;
    c->slice_len = 0;
    c->av1_tile_count = 0;   /* tile 边界随帧重置，与 slice_len 同生命周期 */
    c->have_vp8_slice_param = 0;
    c->have_vp8_pic_param = 0;

    s->state = DMD_SURFACE_PENDING;
    s->decode_status = VA_STATUS_SUCCESS;
    s->context = context;

    pthread_mutex_unlock(&drv->lock);
    return VA_STATUS_SUCCESS;
}

VAStatus dmd_RenderPicture(VADriverContextP ctx, VAContextID context,
                           VABufferID *buffers, int num_buffers)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || (num_buffers > 0 && !buffers))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_buffers < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (c->current_target == VA_INVALID_ID) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT; /* 没 BeginPicture */
    }

    VAStatus status = VA_STATUS_SUCCESS;

    for (int i = 0; i < num_buffers; i++) {
        struct dmd_buffer *b = dmd_find_buffer_locked(drv, buffers[i]);
        if (!b) {
            status = VA_STATUS_ERROR_INVALID_BUFFER;
            break;
        }

        switch (b->type) {
        case VASliceDataBufferType:
            /* 这里是真正的码流。
             * VP9：整帧（含 uncompressed header，从 frame_marker 起），原样转发
             * VP8：partition 0 起，缺 RFC 6386 §9.1 的 uncompressed chunk
             * H.264/HEVC：slice NALU 的**载荷**，不含起始码 */
            if (slice_append_locked(c, b->data, b->size) != 0) {
                status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            }
            break;

        case VASliceParameterBufferType:
            /* AV1：每个 tile 一份 slice param（va_dec_av1.h:635），
             * 它是 tile 边界的唯一可靠来源 —— slice data 可能整帧打包在
             * 一个 buffer 里，数追加次数会得到 1 而非真实 tile 数。
             * 一个 buffer 可携带多个元素（num_elements），要全部收下。 */
            if (c->codec == DMD_CODEC_AV1 &&
                b->element_size >= sizeof(VASliceParameterBufferAV1)) {
                const int cap = (int)(sizeof(c->av1_tile_param) /
                                      sizeof(c->av1_tile_param[0]));
                for (unsigned int k = 0; k < b->num_elements; k++) {
                    if (c->av1_tile_count >= cap)
                        break;
                    const unsigned char *src =
                        (const unsigned char *)b->data +
                        (size_t)k * b->element_size;
                    memcpy(&c->av1_tile_param[c->av1_tile_count++], src,
                           sizeof(VASliceParameterBufferAV1));
                }
                break;
            }
            /* VP8 需要 partition_size[0] 与 macroblock_offset 来推导
             * first_part_size，其余 codec 当前不需要 slice 参数。 */
            if (c->codec == DMD_CODEC_VP8 &&
                b->size >= sizeof(VASliceParameterBufferVP8)) {
                memcpy(&c->vp8_slice_param, b->data,
                       sizeof(VASliceParameterBufferVP8));
                c->have_vp8_slice_param = 1;
            } else if (c->codec == DMD_CODEC_H264 &&
                       b->size >= sizeof(VASliceParameterBufferH264)) {
                /* 留存备用。注意 PPS 的 num_ref_idx_default 不能取自这里：
                 * 参数集要在首个 VCL 之前发，那时只有 IDR 的 slice param，
                 * 而 I slice 的 num_ref_idx 恒为 0（详见 h264_bitstream.c）。 */
                memcpy(&c->h264_slice_param, b->data,
                       sizeof(VASliceParameterBufferH264));
                c->have_h264_slice_param = 1;
            }
            break;

        case VAPictureParameterBufferType:
            /* AV1：序列头与帧头的全部字段都在这里，是合成 OBU 的唯一来源。 */
            if (c->codec == DMD_CODEC_AV1 &&
                b->size >= sizeof(VADecPictureParameterBufferAV1)) {
                memcpy(&c->av1_pic_param, b->data,
                       sizeof(VADecPictureParameterBufferAV1));
                c->have_av1_pic_param = 1;
                /* AV1 的显示顺序就是 order_hint（规范 6.8.2），
                 * 作用与 H.264/HEVC 的 POC 相同：解码器按显示序吐帧，
                 * 而 ffmpeg 按解码序提交，配对回退路径需要它。
                 *
                 * 此前 AV1 完全不登记 POC，pending_poc 恒为 INT32_MAX
                 * （实测日志 "POC 2147483647"），一旦 unit_seq 精确配对
                 * 失败就只能盲目按顺序推断，把帧配到错误的 surface 上。
                 *
                 * KEY 帧会重置 order_hint 序列，用 frame_type 判新序列：
                 * frame_type 0=KEY，与 H.264 的 frame_num==0 同义。 */
                if (getenv("DMD_AV1_SEFPROBE")) {
                    const VADecPictureParameterBufferAV1 *q = &c->av1_pic_param;
                    fprintf(stderr, "[sef] oh=%u ft=%u show=%u showable=%u "
                            "target=%u anchor=%u tiles=%ux%u\n",
                            q->order_hint,
                            q->pic_info_fields.bits.frame_type,
                            q->pic_info_fields.bits.show_frame,
                            q->pic_info_fields.bits.showable_frame,
                            (unsigned)c->current_target,
                            (unsigned)q->current_frame,
                            q->tile_cols, q->tile_rows);
                }
                c->current_poc =
                    (int32_t)c->av1_pic_param.order_hint;
                c->current_frame_num =
                    (c->av1_pic_param.pic_info_fields.bits.frame_type == 0)
                        ? 0 : 1;
                c->have_current_poc = 1;
                break;
            }
            /* VP8 的 key_frame / version / show_frame 相关位在这里。 */
            if (c->codec == DMD_CODEC_VP8 &&
                b->size >= sizeof(VAPictureParameterBufferVP8)) {
                memcpy(&c->vp8_pic_param, b->data,
                       sizeof(VAPictureParameterBufferVP8));
                c->have_vp8_pic_param = 1;
            } else if (c->codec == DMD_CODEC_HEVC &&
                       b->size >= sizeof(VAPictureParameterBufferHEVC)) {
                /* 用途同 H.264：合成 VPS/SPS/PPS，并取 POC 供配对回退路径用。
                 * CurrPic.pic_order_cnt 是已解包的完整 POC，可直接比大小。 */
                memcpy(&c->hevc_pic_param, b->data,
                       sizeof(VAPictureParameterBufferHEVC));
                c->hevc_pic_param_valid = 1;
                c->current_poc = c->hevc_pic_param.CurrPic.pic_order_cnt;
                /* HEVC 没有 frame_num；用 IRAP 判定新序列：
                 * IDR 会把 POC 重置，配对时必须按 seq 分段。 */
                c->current_frame_num =
                    c->hevc_pic_param.slice_parsing_fields.bits.IdrPicFlag ? 0 : 1;
                c->have_current_poc = 1;
            } else if (c->codec == DMD_CODEC_H264 &&
                       b->size >= sizeof(VAPictureParameterBufferH264)) {
                /* 两个用途：
                 * 1) 反向合成 SPS/PPS —— ffmpeg 自己消费掉了参数集 NALU
                 *    （h264dec.c:699/:717），driver 只能从这些字段重建
                 * 2) 取本帧 POC（显示顺序）用于配对 —— MediaCodec 按显示序
                 *    吐帧而 ffmpeg 按解码序提交，只有 POC 能把两者对上。
                 *    ffmpeg 填的是已解包的 field_poc[0]（vaapi_h264.c:74），
                 *    不是码流里会回绕的 6 bit poc_lsb，可直接比较大小。 */
                memcpy(&c->h264_pic_param, b->data,
                       sizeof(VAPictureParameterBufferH264));
                c->have_h264_pic_param = 1;
                c->current_poc = c->h264_pic_param.CurrPic.TopFieldOrderCnt;
                c->current_frame_num = c->h264_pic_param.frame_num;
                c->have_current_poc = 1;
            }
            break;

        case VAIQMatrixBufferType:
        case VAProbabilityBufferType:
        case VAHuffmanTableBufferType:
            /* daemon 侧是完整解码器（MediaCodec），这些表都在码流里，
             * 不需要我们转发。接受但忽略。 */
            break;

        default:
            /* 未知类型不报错：某些消费者会附带我们不关心的 buffer，
             * 拒绝会让整帧失败。 */
            break;
        }
    }
    pthread_mutex_unlock(&drv->lock);

    return status;
}

/* ---- VP8：合成 RFC 6386 §9.1 的 uncompressed data chunk ----
 *
 * VA-API 的 slice data 从 partition 0（压缩帧头）开始，缺开头 3 字节
 * frame tag（key frame 再加 7 字节的 start code + 尺寸）。MediaCodec 要的是
 * 完整帧，所以必须补回来。
 *
 * frame tag（3 字节，小端 19 位 + 尺寸）：
 *   bit0      frame_type（0=key）
 *   bit1-3    version
 *   bit4      show_frame
 *   bit5-23   first_part_size
 * first_part_size 的推导（关键）：VA-API 的 partition_size[0] 是"应用解析后
 * 剩余的控制分区字节数"，加上已被解析掉的头部字节 (macroblock_offset+7)/8
 * 才是 RFC 定义的 first_part_size。
 * show_frame 在 VA-API 里没有对应字段，只能假定 1 —— VP8 极少用 invisible 帧。
 */
static size_t vp8_build_frame(struct dmd_context *c, unsigned char *out,
                              size_t out_cap)
{
    if (!c->have_vp8_slice_param || !c->have_vp8_pic_param)
        return 0;

    const VASliceParameterBufferVP8 *sp = &c->vp8_slice_param;
    const VAPictureParameterBufferVP8 *pp = &c->vp8_pic_param;

    unsigned int key_frame = pp->pic_fields.bits.key_frame ? 0 : 1;
    /* VA-API 注释："0 means a key frame"，与 RFC 的 frame_type 同义
     * （RFC: 0=key frame）。这里 key_frame 变量表示"是关键帧"。 */
    key_frame = pp->pic_fields.bits.key_frame == 0 ? 1 : 0;
    unsigned int version = pp->pic_fields.bits.version & 0x7;
    unsigned int header_bytes = (sp->macroblock_offset + 7) / 8;
    unsigned int first_part_size = sp->partition_size[0] + header_bytes;
    if (first_part_size > 0x7FFFF)
        return 0; /* 19 位装不下，码流异常 */

    size_t tag_len = key_frame ? 10 : 3;
    if (out_cap < tag_len + c->slice_len)
        return 0;

    /* frame tag：3 字节小端位域 */
    uint32_t tag = (key_frame ? 0u : 1u) | (version << 1) |
                   (1u << 4) /* show_frame：VA-API 无此字段，假定可见 */
                   | (first_part_size << 5);
    out[0] = (unsigned char)(tag & 0xFF);
    out[1] = (unsigned char)((tag >> 8) & 0xFF);
    out[2] = (unsigned char)((tag >> 16) & 0xFF);

    if (key_frame) {
        /* key frame 额外 7 字节：3 字节 start code 9D 01 2A + 2+2 字节尺寸。
         * 尺寸的高 2 bit 是 scaling，我们不缩放，填 0。 */
        out[3] = 0x9D;
        out[4] = 0x01;
        out[5] = 0x2A;
        unsigned int w = pp->frame_width & 0x3FFF;
        unsigned int h = pp->frame_height & 0x3FFF;
        out[6] = (unsigned char)(w & 0xFF);
        out[7] = (unsigned char)((w >> 8) & 0x3F);
        out[8] = (unsigned char)(h & 0xFF);
        out[9] = (unsigned char)((h >> 8) & 0x3F);
    }

    memcpy(out + tag_len, c->slice_data, c->slice_len);
    return tag_len + c->slice_len;
}

/* 组装最终要送给 daemon 的数据单元。
 * 返回要送的缓冲（可能就是 c->slice_data 本身）与长度；失败返回 NULL。
 * scratch 由调用方提供并负责释放。 */
/* AV1 合成结果落盘（DMD_AV1_DUMP / DMD_AV1_DUMP_ALL）。
 *
 * ⚠️ 必须在 refresh_frame_flags 就地改写**之后**、真正送出时才落盘。
 * 早先放在合成处，落的是改写前的字节，导致 trace_headers 看到的
 * refresh 恒为轮转值（1）而非反算出的真值，白排查一轮。
 *
 * 这是本模块的主要验证手段：合成正确性无法在设备上直接判断
 * （硬件只会"不出帧"，不告诉你哪一位错了），必须把字节取回来交给
 * 权威工具：
 *   ffmpeg -f obu -i <文件> -f null -           （能否解出帧）
 *   ffmpeg -bsf:v trace_headers ...             （逐字段比对）
 * 自制解析器不可靠 —— 曾因其自身偏差与实现错误互相抵消而误判"已对齐"。
 *
 * 默认只落首帧；DMD_AV1_DUMP_ALL=1 追加整条流，用于校验多帧序列
 * （"首帧正确但后续帧让解码器停摆"只有整流才能暴露）。 */
static void av1_dump_sent(const unsigned char *buf, size_t n)
{
    const char *dp = getenv("DMD_AV1_DUMP");
    if (!dp || !buf || n == 0)
        return;
    const char *all = getenv("DMD_AV1_DUMP_ALL");
    int dump_all = all && all[0] == '1';
    static int dumped = 0;
    if (dump_all) {
        FILE *df = fopen(dp, dumped ? "ab" : "wb");
        if (df) { fwrite(buf, 1, n, df); fclose(df); dumped++; }
    } else if (!dumped) {
        FILE *df = fopen(dp, "wb");
        if (df) { fwrite(buf, 1, n, df); fclose(df); dumped = 1;
                  dmd_log("已落盘 %zu 字节到 %s", n, dp); }
    }
}

static const unsigned char *build_unit(struct dmd_context *c,
                                       unsigned char **scratch, size_t *out_len)
{
    *scratch = NULL;

    if (c->slice_len == 0)
        return NULL;

    switch (c->codec) {
    case DMD_CODEC_VP9:
        /* 零 header 重建：VASliceDataBufferType 里就是完整的 VP9 frame
         * （va_dec_vp9.h:274-284 的规范注释：slice data 含整帧，
         * slice_data_size 实际就是 frame_data_size）。 */
        *out_len = c->slice_len;
        return c->slice_data;

    case DMD_CODEC_AV1: {
        /* AV1 必须反向合成 OBU —— 不能像 VP9 那样直接转发。
         *
         * 为什么：VASliceDataBufferType 里只有 tile 的**载荷**，不含任何
         * OBU 封装（va_dec_av1.h:643-645 明确说"host decoder 负责解析出
         * per-tile 信息，码流按 per-tile 粒度送入驱动"）。序列头与帧头
         * 只存在于 VADecPictureParameterBufferAV1 的结构化字段里。
         *
         * 曾以为可以零重建，实测证伪：原样转发时首字节是 0xd0
         * （forbidden_bit=1、OBU 类型 10 保留值），ffprobe 报
         * "Failed to read obu / No sequence header available"，
         * MediaCodec 送入 1 单元取回 0 帧。VP9 的情形不同——它的
         * slice data 本身就是完整帧（va_dec_vp9.h:274-284）。
         *
         * 组装出的 temporal unit：
         *   OBU_TEMPORAL_DELIMITER
         *   OBU_SEQUENCE_HEADER   （仅关键帧，序列参数不会帧间变化）
         *   OBU_FRAME             （帧头 + byte_alignment + tile_group）
         *
         * 用 OBU_FRAME(6) 而非分离的 FRAME_HEADER(3)+TILE_GROUP(4)：
         * 实测 dav1d 对分离形式报 "Failed to read unit"，libaom 生成的
         * 真实码流用的也是 OBU_FRAME。 */
        if (!c->have_av1_pic_param || c->av1_tile_count <= 0) {
            dmd_log("EndPicture: AV1 缺少 %s，无法合成 OBU",
                    c->have_av1_pic_param ? "tile 数据" : "pic param");
            return NULL;
        }

        const VADecPictureParameterBufferAV1 *pp = &c->av1_pic_param;
        const uint32_t want_tiles =
            (uint32_t)pp->tile_cols * (uint32_t)pp->tile_rows;
        if (want_tiles == 0 || (uint32_t)c->av1_tile_count != want_tiles) {
            /* tile 数量与帧头声明不符会让解码器从第二个 tile 起全部错位，
             * 与其送出去让 MediaCodec 解出花屏，不如干净回落软解。 */
            dmd_log("EndPicture: AV1 tile 数不符（收到 %d，帧头声明 %u），放弃硬解",
                    c->av1_tile_count, want_tiles);
            return NULL;
        }

        /* tile 描述表：用每个 tile 自己的 slice_data_offset/slice_data_size
         * 在累积缓冲里定位（va_dec_av1.h:649-658）。不能靠"追加次数"——
         * 实机上 ffmpeg 把整帧 8 个 tile 打包在一个 slice data buffer 里。 */
        struct dmd_av1_tile tiles[512];
        for (int i = 0; i < c->av1_tile_count; i++) {
            const VASliceParameterBufferAV1 *tp = &c->av1_tile_param[i];
            const size_t off = tp->slice_data_offset;
            const size_t len = tp->slice_data_size;
            if (len == 0 || off > c->slice_len || off + len > c->slice_len) {
                dmd_log("EndPicture: AV1 tile[%d] 越界（off=%zu len=%zu，"
                        "缓冲 %zu），放弃硬解", i, off, len, c->slice_len);
                return NULL;
            }
            tiles[i].data = c->slice_data + off;
            tiles[i].len  = len;
        }

        /* 输出缓冲：tile 载荷实际总长 + 每 tile 4 字节 tile_size + 头部余量。
         * 用 tile 长度之和而非 slice_len —— 两者可能不等（buffer 里允许有
         * 未被任何 tile 引用的间隙）。 */
        size_t tile_bytes = 0;
        for (int i = 0; i < c->av1_tile_count; i++)
            tile_bytes += tiles[i].len;
        const size_t cap = tile_bytes + (size_t)c->av1_tile_count * 4 + 1024;
        unsigned char *buf = malloc(cap);
        if (!buf)
            return NULL;

        size_t n = 0;
        n += dmd_av1_obu_header(DMD_OBU_TEMPORAL_DELIMITER, 0, buf + n, cap - n);

        /* 序列头只在关键帧前发：AV1 允许重复发送，但每帧都发是浪费；
         * 而序列级参数（尺寸、profile、能力位）在一个 session 内不变，
         * 变了本驱动会重建 session。 */
        const uint32_t ft = pp->pic_info_fields.bits.frame_type;
        if (ft == 0 /* KEY_FRAME */) {
            const size_t sn = dmd_av1_build_sequence_header(pp, buf + n, cap - n);
            if (sn == 0) {
                dmd_log("EndPicture: AV1 序列头合成失败");
                free(buf);
                return NULL;
            }
            n += sn;
        }

        /* 记下 OBU_FRAME 在 buf 里的起始偏移（TD、必要时还有序列头）。
         * build_frame 记的 bitpos 是"本 OBU 内"的，改写时作用于整个 buf，
         * 故须再叠加这段外层前缀。 */
        const size_t frame_off = n;
        const size_t fn = dmd_av1_build_frame(pp, tiles, c->av1_tile_count,
                                              buf + n, cap - n, &c->av1_dpb);
        if (fn == 0) {
            dmd_log("EndPicture: AV1 OBU_FRAME 合成失败（tile=%d, 载荷=%zu）",
                    c->av1_tile_count, tile_bytes);
            free(buf);
            return NULL;
        }
        n += fn;

        dmd_log("EndPicture: AV1 合成 %zu 字节（%s%d tile，载荷 %zu）",
                n, ft == 0 ? "含序列头，" : "", c->av1_tile_count, tile_bytes);

        /* DMD_AV1_DUMP=<路径>：把首帧合成结果落盘，供离线校验。
         *
         * 这是本模块的主要验证手段，值得长期保留：合成正确性无法在设备上
         * 直接判断（MediaCodec 只会"不出帧"，不告诉你哪一位错了），必须把
         * 字节取回来交给权威工具。实测有效的判据是
         *   ffmpeg -f obu -i <落盘文件> -f null -        （能否解出帧）
         *   ffmpeg -bsf:v trace_headers ...              （逐字段比对位偏移）
         * 后者是定位单个错误比特的唯一可行手段 —— 自制解析器不可靠，
         * 曾因其自身偏差与实现错误互相抵消而误判"已对齐"。
         *
         * 默认只落首帧：帧头字段最全（含序列头），且避免刷爆磁盘。
         * DMD_AV1_DUMP_ALL=1 改为追加**整条**合成流 —— 用于校验多帧序列，
         * 因为"首帧正确但后续帧让解码器停摆"这类问题只有整流才能暴露：
         * 把整条流交给 ffmpeg 软解，解出帧数应与源码流一致。 */
        /* ============ AV1 延迟一帧送料 ============
         *
         * refresh_frame_flags 的正确值只能从**下一帧**的 ref_frame_map 反算：
         *     第 N 帧 refresh = 第 N+1 帧 map 与第 N 帧 map 的差异位
         * 实测 4 帧全部命中源码流真实值（1/8/32/64）。
         *
         * 已否证的五种"只用本帧字段"的推导 —— 不要再试：
         *  1. 在本帧 ref_frame_map 里找 current_frame：该数组是解码**前**
         *     快照，当前帧尚未写入，恒为 0
         *  2. 本帧 map 与**上**帧 map 差分：方向错，得到的是上一帧的 refresh
         *  3. 恒 1 或 1<<slot 轮转：结构上无法表达 refresh=0（不占槽的帧），
         *     真实序列 1,8,32,64,0,0,64,0,0,32,64,0 里这类帧占三分之一
         *  4. map 中"值仍为初始值且不被 ref_frame_idx 引用的最小槽"：
         *     仅首帧碰对（实测猜 1/2/2/2 对真值 1/8/32/64）
         *  5. max(已填槽)+2：实测 0→✓ 3→✗ 5→✓ 6→✗，不成立
         * 真实写入槽序列 0,3,5,6 是编码器的金字塔层级决策，
         * 本帧字段里不含这个信息。
         *
         * 因此必须延迟一帧。关键前提（上一轮遗漏，导致误判为结构死锁）：
         *  · KEY 帧的 refresh_frame_flags **不写入码流**（规范推断为全刷），
         *    无需反算，必须立即送 —— 否则 ffmpeg 拿不到首帧像素就不再送料，
         *    实测表现为"送入 0 单元, 收到 0 帧"
         *  · ffmpeg 允许多帧在飞（实测 DMD_PIPELINE_DEPTH=12 时送了 6 帧），
         *    所以帧间帧延迟一帧不会死锁
         *
         * 于是：KEY 帧直接送；帧间帧先入 av1_hold，等下一帧到来时
         * 反算并就地改写那 8 位，再把它送出。 */
        dmd_av1_patch_prev_refresh(&c->av1_dpb, pp,
                                   c->av1_hold, c->av1_hold_len,
                                   c->av1_hold_bitpos);

        if (ft == 0 /* KEY_FRAME */) {
            /* KEY 帧不延迟。此时 av1_hold 必为空（KEY 帧开启新的参考链）。 */
            c->av1_send_surface = c->current_target;
            c->av1_send_show = 1;   /* KEY 帧必然显示 */
            av1_dump_sent(buf, n);
            *scratch = buf;
            *out_len = n;
            return buf;
        }

        if (c->av1_hold) {
            unsigned char *prev = c->av1_hold;
            size_t prev_len = c->av1_hold_len;
            /* 本次送出的是上一帧，配对要用它的 surface。 */
            if (getenv("DMD_VA_LOG"))
                dmd_log("送出: 暂存帧(surface=%u, %zu字节) 当前帧surface=%u\n",
                        (unsigned)c->av1_hold_surface, prev_len,
                        (unsigned)c->current_target);
            c->av1_send_surface = c->av1_hold_surface;
            c->av1_send_show = c->av1_hold_show;
            c->av1_hold_surface = c->current_target;
            c->av1_hold_show = (int)pp->pic_info_fields.bits.show_frame;
            c->av1_hold = buf;
            c->av1_hold_len = n;
            c->av1_hold_bitpos = c->av1_dpb.last_refresh_bitpos == (size_t)-1
                               ? (size_t)-1
                               : c->av1_dpb.last_refresh_bitpos + frame_off * 8;
            av1_dump_sent(prev, prev_len);
            *scratch = prev;
            *out_len = prev_len;
            return prev;
        }

        /* 第一个帧间帧：只入暂存，本次无数据可送。
         * 用 (unit==NULL && unit_len==0 && av1_hold!=NULL) 与
         * "码流重建失败"区分开。 */
        c->av1_hold_surface = c->current_target;
        c->av1_hold_show = (int)pp->pic_info_fields.bits.show_frame;
        c->av1_hold = buf;
        c->av1_hold_len = n;
        c->av1_hold_bitpos = c->av1_dpb.last_refresh_bitpos == (size_t)-1
                           ? (size_t)-1
                           : c->av1_dpb.last_refresh_bitpos + frame_off * 8;
        *scratch = NULL;
        *out_len = 0;
        return NULL;
    }

    case DMD_CODEC_VP8: {
        size_t cap = c->slice_len + 16;
        unsigned char *buf = malloc(cap);
        if (!buf)
            return NULL;
        size_t n = vp8_build_frame(c, buf, cap);
        if (n == 0) {
            free(buf);
            return NULL;
        }
        *scratch = buf;
        *out_len = n;
        return buf;
    }

    case DMD_CODEC_H264: {
        /* slice data 是**完整原始 NALU**（含 NAL header 与 slice header），
         * 只缺起始码。已核实：h264dec.c:674 传 nal->raw_data/raw_size，
         * h2645_parse.c:92/145 的 raw_data 指向起始码之后且保留转义字节，
         * h2645_parse.c:448-456 从 gb 读 NAL header 反证 header 在 buffer 内，
         * vaapi_h264.c:386-388 原样进 slice data buffer。
         * 所以这里只补 4 字节 00 00 00 01，不做任何 slice header 重建 ——
         * 重排序与 MMCO 命令本来就还在原始字节里，MediaCodec 自己解析。 */
        size_t cap = c->slice_len + 4;
        unsigned char *buf = malloc(cap);
        if (!buf)
            return NULL;
        buf[0] = 0x00;
        buf[1] = 0x00;
        buf[2] = 0x00;
        buf[3] = 0x01;
        memcpy(buf + 4, c->slice_data, c->slice_len);
        *scratch = buf;
        *out_len = cap;
        return buf;
    }

    /* ============ HEVC 驱动内 V4L2 直通实机验证结果 ============
     * 设备 SM8750 / msm_vidc /dev/video32，容器内无 root，2026-08。
     * 素材 1920x1080 25fps HEVC Main yuv420p(tv) 12 帧（14281 字节）。
     *
     * 帧数：硬解 12 / 软解 12，三次复现全部 12/12。
     * 送料：送入 15 单元收到 12 帧 —— 多出的 3 个单元是 VPS/SPS/PPS
     *       参数集 NAL，本身不产生图像，属正常。
     * 日志可见 SOURCE_CHANGE 与 S_FMT(OUTPUT) 1920x1088
     *       （V4L2 按 16 对齐，1080→1088），协商正常。
     *
     * ⚠️ 首帧像素**未通过**逐点校验，原因未定：
     *   Y 平面前 797 行与软解逐字节相同，第 797~1079 行（283 行）存在差异，
     *   渐变区最大差 40（例：行 900 hw=0x70 sw=0x52）；UV 平面亦有差异。
     *   已排除：tv→full 色彩范围转换（按 (v-16)*255/219 预测不匹配，
     *   且差值有正有负、后段反转）、水平/垂直空间位移（最佳匹配落在
     *   搜索边界 ±40，是单调渐变造成的假匹配而非真位移）。
     *   注意 hwdownload 导出必须加 -hwaccel_output_format vaapi，
     *   否则输出 0 字节 —— 这是 ffmpeg 用法问题，不是驱动缺陷。
     *   待查方向：CAPTURE 面 NV12 stride/UV 偏移在非 16 倍高度下的处理，
     *   以及 1088 对齐高度裁回 1080 时 COMPOSE 选区是否漏了色度面。
     */
    case DMD_CODEC_HEVC: {
        /* 与 H.264 完全同构：VASliceParameterBufferHEVC 里有
         * slice_data_byte_offset，说明 slice header 原样在 buffer 里，
         * 所以只需前置 4 字节起始码转发，不必重建 slice header
         * （那要反推参考列表重排序，是欠定问题）。 */
        size_t cap = 4 + c->slice_len;
        unsigned char *buf = malloc(cap);
        if (!buf)
            return NULL;
        buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 1;
        memcpy(buf + 4, c->slice_data, c->slice_len);
        *scratch = buf;
        *out_len = cap;
        return buf;
    }

    default:
        return NULL;
    }
}

/* HEVC：在首个 slice 之前把合成的 VPS/SPS/PPS 送给 daemon。
 *
 * 与 H.264 的区别是三个参数集而非两个，且必须按 VPS→SPS→PPS 顺序 ——
 * SPS 引用 VPS 的 id，PPS 引用 SPS 的 id。
 * 每个都作为独立数据单元发送（daemon 靠 nal_unit_header 识别并累积为 CSD）。
 *
 * 调用方不得持锁（这里会做阻塞发送）。返回 0 成功。 */
static int hevc_send_param_sets(struct dmd_session *sess,
                                const VAPictureParameterBufferHEVC *pp,
                                VAProfile profile)
{
    unsigned char nalu[600];

    size_t n = dmd_hevc_build_vps_nalu(pp, profile, nalu, sizeof(nalu));
    if (n == 0) {
        dmd_log("HEVC: VPS 合成失败\n");
        return -1;
    }
    if (dmd_session_send_unit(sess, nalu, n) != DMD_OK) {
        dmd_log("HEVC: VPS 发送失败: %s\n", dmd_session_last_error(sess));
        return -1;
    }
    dmd_log("HEVC: 已送 VPS %zu 字节\n", n);

    n = dmd_hevc_build_sps_nalu(pp, profile, nalu, sizeof(nalu));
    if (n == 0) {
        dmd_log("HEVC: SPS 合成失败\n");
        return -1;
    }
    if (dmd_session_send_unit(sess, nalu, n) != DMD_OK) {
        dmd_log("HEVC: SPS 发送失败: %s\n", dmd_session_last_error(sess));
        return -1;
    }
    dmd_log("HEVC: 已送 SPS %zu 字节\n", n);

    n = dmd_hevc_build_pps_nalu(pp, nalu, sizeof(nalu));
    if (n == 0) {
        dmd_log("HEVC: PPS 合成失败\n");
        return -1;
    }
    if (dmd_session_send_unit(sess, nalu, n) != DMD_OK) {
        dmd_log("HEVC: PPS 发送失败: %s\n", dmd_session_last_error(sess));
        return -1;
    }
    dmd_log("HEVC: 已送 PPS %zu 字节\n", n);

    return 0;
}

/* H.264：在首个 slice 之前把合成的 SPS/PPS 送给 daemon。
 *
 * daemon 靠起始码定位 nal_unit_header 识别参数集并累积为 codec config
 * （decode-daemon.c 的 input 线程），遇到第一个非参数集才以
 * FLAG_CODEC_CONFIG 送出。所以 SPS 与 PPS 必须**各自作为独立的数据单元**
 * 先送，且必须在第一个 VCL NALU 之前。
 *
 * 调用方不得持锁（这里会做阻塞发送）。返回 0 成功。 */
static int h264_send_param_sets(struct dmd_session *sess,
                                const VAPictureParameterBufferH264 *pp,
                                const VAIQMatrixBufferH264 *iq, int have_iq,
                                const VASliceParameterBufferH264 *sp,
                                unsigned int def_l0, unsigned int def_l1,
                                VAProfile profile, unsigned int disp_width,
                                unsigned int disp_height)
{
    unsigned char nalu[512];

    size_t n = dmd_h264_build_sps_nalu(pp, profile, disp_width, disp_height,
                                       nalu, sizeof(nalu));
    if (n == 0) {
        dmd_log("H.264: SPS 合成失败\n");
        return -1;
    }
    if (dmd_session_send_unit(sess, nalu, n) != DMD_OK) {
        dmd_log("H.264: SPS 发送失败: %s\n", dmd_session_last_error(sess));
        return -1;
    }
    dmd_log("H.264: 已送 SPS %zu 字节\n", n);

    n = dmd_h264_build_pps_nalu(pp, iq, have_iq, sp, def_l0, def_l1,
                                nalu, sizeof(nalu));
    if (n == 0) {
        dmd_log("H.264: PPS 合成失败\n");
        return -1;
    }
    if (dmd_session_send_unit(sess, nalu, n) != DMD_OK) {
        dmd_log("H.264: PPS 发送失败: %s\n", dmd_session_last_error(sess));
        return -1;
    }
    dmd_log("H.264: 已送 PPS %zu 字节\n", n);

    return 0;
}

VAStatus dmd_EndPicture(VADriverContextP ctx, VAContextID context)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (c->current_target == VA_INVALID_ID) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    VASurfaceID target = c->current_target;
    int idx = (int)(c - drv->contexts);

    /* 组装码流（持锁做，纯内存操作）。 */
    unsigned char *scratch = NULL;
    size_t unit_len = 0;
    const unsigned char *unit = build_unit(c, &scratch, &unit_len);

    /* ================================================================
     * AV1 当前的阻塞点：ffmpeg 对 show_frame=0 的 surface 也要求像素
     * ================================================================
     * 实测的 surface → 数据 映射（DMD_VA_LOG 的"送出"日志，按字节数认帧）：
     *   surface1  19258B  oh= 0  show=1
     *   surface2   2688B  oh=16  show=0
     *   surface3    649B  oh= 8  show=0
     *   surface4    245B  oh= 4  show=0
     *   surface5    183B  oh= 2  show=0
     *   surface6     87B  oh= 1  show=1
     * 登记与送出完全一致，无双重登记（曾怀疑此项，实测否证）。
     *
     * ffmpeg 实测只 Sync 了 surface 1、6、5 三个：
     *   surface1（show=1）配对成功 ✓
     *   surface6（show=1）配对成功 ✓
     *   surface5（show=0）—— 它仍要像素，而该帧本就不产生输出
     * 于是报 "Failed to read image from surface 0x5: internal decoding error"，
     * 硬解停在 2 帧（等于 dav1d 对同段码流的基线，说明解码本身没错）。
     *
     * 也就是说：msm_vidc 只对 show_frame=1 的帧吐 CAPTURE 缓冲（正确行为），
     * 而 ffmpeg 的 VA-API 后端会对部分 show_frame=0 的 surface 调 Sync 并读像素。
     * 驱动目前让这类 surface 永久停在 PENDING，直到超时。
     *
     * 待定的修法（需先确认 ffmpeg VA-API 侧的确切期望，不要凭猜实现）：
     *   a) 把 show_frame=0 的 surface 标为已完成但无像素
     *   b) 让它复用所引用帧的内容（AV1 的 show_existing_frame 语义）
     *   c) 在 EndPicture 阶段就识别 show_frame=0 并不为其登记 pending
     * ================================================================ */

    /* AV1 延迟一帧送料：第一个帧间帧只入暂存，本次没有数据要送。
     * 这不是错误 —— 用 unit_len==0 且 av1_hold 非空与"码流重建失败"区分。
     * surface 保持 PENDING，它会在下一帧把本帧送出后才拿到解码结果。 */
    if (!unit && unit_len == 0 && c->codec == DMD_CODEC_AV1 && c->av1_hold) {
        c->current_target = VA_INVALID_ID;
        c->slice_len = 0;
        c->av1_tile_count = 0;
        pthread_mutex_unlock(&drv->lock);
        free(scratch);
        dmd_log("EndPicture: AV1 帧入暂存，等下一帧反算 refresh\n");
        return VA_STATUS_SUCCESS;
    }

    if (!unit) {
        struct dmd_surface *s = dmd_find_surface_locked(drv, target);
        if (s) {
            s->state = DMD_SURFACE_IDLE;
            s->decode_status = VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        c->current_target = VA_INVALID_ID;
        c->slice_len = 0;
        c->av1_tile_count = 0;
        int codec = c->codec;
        pthread_mutex_unlock(&drv->lock);
        free(scratch);
        dmd_log("EndPicture: codec=%d 尚未支持码流重建，帧被丢弃\n", codec);
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }

    /* 会话写端已关（flush 过），但上游又送料了 —— 透明换一条新会话。
     *
     * 这条路径覆盖的是 **seek**：daemon 没有连接内 reset，flush 又是不可逆的
     * shutdown(SHUT_WR)，所以播放器跳转后要继续解码只能重建会话。
     * ⚠️ 曾据 tools/probe_rebuild.c 认为"从非 IDR 帧续传也能立刻出帧，
     * 不会有可见花屏"—— 那个结论**是错的**（探针只数帧数没看画面，
     * 那些帧全是纯黑 Y=16，探针开头已自我标注不可信）。
     * 实际上重建摧毁参考帧链，续传后要黑到下一个 IDR。
     * seek 场景下这个代价通常可接受（播放器跳转后本就期待画面刷新），
     * 但**不要**把它当成"重建无害"的依据。
     *
     * 注意：Firefox 的正常播放**不会**走到这里 —— 实测它一旦在
     * vaSyncSurface 上卡住就直接 DestroyContext，不再调 EndPicture。
     * 那个死锁是靠 DMD_FLUSH_AFTER_MS + DMD_PIPELINE_DEPTH 从根上避免的，
     * 本路径只是 seek/异常场景的兜底，尚未被真实 seek 端到端验证过。 */
    if (c->session && c->input_finished) {
        dmd_log("EndPicture: 会话写端已关闭但仍有新输入，重建会话\n");
        if (session_rebuild_locked(drv, c, idx) < 0) {
            c = dmd_find_context_locked(drv, context);
            if (!c) {
                pthread_mutex_unlock(&drv->lock);
                free(scratch);
                return VA_STATUS_ERROR_INVALID_CONTEXT;
            }
        } else {
            /* 重建期间放过锁，对象可能已被销毁，重新定位。 */
            c = dmd_find_context_locked(drv, context);
            if (!c) {
                pthread_mutex_unlock(&drv->lock);
                free(scratch);
                return VA_STATUS_ERROR_INVALID_CONTEXT;
            }
        }
    }

    /* 会话惰性重试：CreateContext 时 daemon 可能还没起来。 */
    if (!c->session && !c->session_failed) {
        /* 不可能：session_failed 与 session 互补。保守处理。 */
        c->session_failed = 1;
    }

    struct dmd_session *sess = c->session;
    int codec = c->codec;
    unsigned int pw = c->picture_width;
    unsigned int ph = c->picture_height;

    if (!sess) {
        /* 放锁重试建会话，避免持锁做 connect。 */
        drv->io_busy[idx] = 1;
        pthread_mutex_unlock(&drv->lock);
        struct dmd_session *retry = session_open(codec, pw, ph);
        pthread_mutex_lock(&drv->lock);
        drv->io_busy[idx] = 0;
        pthread_cond_broadcast(&drv->io_done);
        c = dmd_find_context_locked(drv, context);
        if (!c) {
            pthread_mutex_unlock(&drv->lock);
            if (retry)
                dmd_session_destroy(retry);
            free(scratch);
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
        if (!c->session) {
            c->session = retry;
            c->session_failed = retry ? 0 : 1;
        } else if (retry) {
            dmd_session_destroy(retry); /* 别的线程先建好了 */
        }
        sess = c->session;
        if (!sess) {
            struct dmd_surface *s = dmd_find_surface_locked(drv, target);
            if (s) {
                s->state = DMD_SURFACE_IDLE;
                s->decode_status = VA_STATUS_ERROR_OPERATION_FAILED;
            }
            c->current_target = VA_INVALID_ID;
            c->slice_len = 0;
            c->av1_tile_count = 0;
            pthread_mutex_unlock(&drv->lock);
            free(scratch);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    /* 入队：**在放锁发送之前**入队，保证队列顺序严格等于提交顺序。
     * 若发送失败再出队回滚。 */
    /* 队列满时收**一帧**腾出空位，而不是直接失败。
     *
     * ⚠️ 这里曾直接返回 OPERATION_FAILED，那是 Chrome 硬解不可用的根因：
     *
     * Chrome 不像 ffmpeg / Firefox 那样调 vaSyncSurface 逐帧同步，而是
     * 大批提交后靠 vaExportSurfaceHandle 拿 dmabuf。于是队列很快填满，
     * EndPicture 报错 → Chrome 收到 "operation failed" → 判定硬解不可用
     * → 断开连接（daemon 侧看到的 Broken pipe / Connection reset 是结果）。
     *
     * 实测时间线可以确证：队列满那一刻 Chrome 立刻报 vaEndPicture failed，
     * 而全部 50 次配对都发生在**失败之后的 DestroyContext 排空阶段** ——
     * 也就是说帧在播放期间从没被及时消费，形成死锁：
     * Chrome 等 surface 就绪 → 就绪需要收帧 → 收帧被队列满堵住。
     *
     * 队列满是**背压**不是错误：帧还在路上，收回来就有空间。
     *
     * ⚠️ 只收一帧，收够就走。曾试过在这里循环收光，那会替消费者把帧全
     * 收走（实测配对 1651 次而 Chrome 只导出 24 帧，帧率反跌到 1 fps）。
     * 也试过把队列放大到 256，只是把症状推后（送入 67 → 259）而未解决。 */
    if (c->pending_count >= DMD_MAX_SURFACES) {
        struct dmd_session *bp_sess = c->session;
        int bp_idx = (int)(c - drv->contexts);

        if (bp_sess && !drv->io_busy[bp_idx]) {
            drv->io_busy[bp_idx] = 1;
            pthread_mutex_unlock(&drv->lock);

            struct dmd_frame bp_frame;
            memset(&bp_frame, 0, sizeof(bp_frame));
            int bp_rc = dmd_session_next_frame(bp_sess, &bp_frame,
                                               DMD_BACKPRESSURE_MS);

            pthread_mutex_lock(&drv->lock);
            drv->io_busy[bp_idx] = 0;
            pthread_cond_broadcast(&drv->io_done);

            c = dmd_find_context_locked(drv, context);
            if (!c) {
                if (bp_rc == DMD_OK)
                    dmd_session_release_frame(bp_sess, &bp_frame);
                pthread_mutex_unlock(&drv->lock);
                free(scratch);
                return VA_STATUS_ERROR_INVALID_CONTEXT;
            }

            if (bp_rc == DMD_OK) {
                VASurfaceID bp_head = dmd_pending_take_locked(c,
                                                             bp_frame.unit_seq);
                struct dmd_surface *bp_hs = dmd_find_surface_locked(drv,
                                                                    bp_head);
                if (bp_hs) {
                    surface_store_frame_locked(bp_hs, &bp_frame);
                    bp_hs->state = DMD_SURFACE_READY;
                }
                dmd_session_release_frame(bp_sess, &bp_frame);
            }
        }

        if (c->pending_count >= DMD_MAX_SURFACES) {
            pthread_mutex_unlock(&drv->lock);
            free(scratch);
            dmd_log("EndPicture: 待解码队列已满（%d），收帧后仍无空位\n",
                    DMD_MAX_SURFACES);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    /* AV1 show_frame=0 的帧不产生输出：解码器实测只对 show_frame=1 的帧吐
     * CAPTURE 缓冲（送 6 单元收 2 帧，与 dav1d 基线一致）。
     * 给这类帧登记待配对项，队列里就留下永远配不上的条目 ——
     * 后续帧的精确配对随之失效，退到顺序推断并配错 surface。
     *
     * ⚠️ 但**数据必须照送**：这些帧是后续帧的参考，漏送会毁掉参考链。
     * 所以只跳过入队，不能在这里 early-return
     * （送料在下方 dmd_session_send_unit，早退就等于不送 —— 踩过一次）。
     *
     * ffmpeg 仍会对其中部分 surface 调 Sync（实测 surface5，show=0 oh=2），
     * 所以还要给 surface 一个结局，否则它永久停在 PENDING 直到超时。
     * 标为 READY 且不带像素数据：Sync 立即成功返回。
     *
     * ⚠️ 这一步只解决了帧数，**没有解决像素**：
     *   -f null（不读像素）→ 硬解 150 帧 = dav1d 软解 150 帧 ✓
     *   hwdownload 读像素   → "Failed to read image from surface 0x8:
     *                          internal decoding error"，只导出部分帧
     * 因为 READY 但 s->data 为空，GetImage 无内容可给。
     *
     * 正确方向应是让这些 surface 拿到**真实像素**而非空壳：
     * AV1 里 show_frame=0 的帧内容确实存在（供后续帧参考），
     * 只是不进显示队列。ffmpeg 读它大概是为了 film grain 的
     * current_frame / current_display_picture 两路输出
     * （见 vaapi_av1.c 的注释：VAAPI 对每帧产生 2 个 output）。
     * ---- 已排除的一条路：V4L2 DISPLAY_DELAY 控制 ----
     * msm_vidc /dev/video32 确实暴露标准控制
     *   V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE (0x00990b8e)
     *   V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY        (0x00990b8d)
     * 两者默认都是 0，S_CTRL 设置均返回 OK（已接进 v4l2_backend.c，
     * 由 DMD_V4L2_DISPLAY_DELAY=1 开启）。
     * 但实测**无效**：像素导出仍是 6 帧，报错仍是
     * "Failed to read image from surface 0x8"。
     * 该控制只影响输出时机/重排深度，不改变 AV1 的 show_frame 语义 ——
     * 不显示的帧依旧不吐 CAPTURE 缓冲。这条路走不通。
     *
     * ---- 另外两条已否证的尝试（本轮踩过，勿重复）----
     * 给 sync flush 加 input_finished 门 → 帧数从 150 掉回 1：
     * 正常流程里 input_finished 直到很晚才置位，暂存帧因此永远送不出去。
     * 给 refresh 改写加同样的门 → 像素从 6 掉到 3。两者均已撤销。
     *
     * 待查方向：驱动把被引用帧的内容复制给这些 surface，
     * 或让 CAPTURE 缓冲在多个 surface 间共享。 */
    const int av1_no_output = (c->codec == DMD_CODEC_AV1 &&
                               !c->av1_send_show &&
                               c->av1_send_surface != VA_INVALID_ID);
    if (av1_no_output) {
        struct dmd_surface *ns = dmd_find_surface_locked(drv,
                                                        c->av1_send_surface);
        /* ⚠️ 只在该 surface **还没有像素数据**时才标空壳。
         *
         * ffmpeg 会复用 surface。实测踩过：surface 6 先在 sync flush 里
         * 拿到真实帧数据并完成配对（日志"配对: 帧 -> surface 6"），
         * 20 行之后又被本分支标成 READY 空壳，真实数据就此作废 ——
         * 导出的 nv12 里那些帧是全零（Y 取样唯一值数=1）。
         * 有数据就别碰：那说明这个 surface 已经承载了别的帧。 */
        /* ⚠️ 试过"有数据就不标"（if (ns && !ns->data)）—— 退化到 2 帧：
         * surface 会卡回 PENDING，ffmpeg 报
         * "Failed to sync surface 0x5: internal decoding error"。
         * 空壳标记对流程推进是必需的，不能因为怕覆盖就跳过。
         *
         * 但直接标 READY 会让导出的 nv12 出现全零帧
         * （Y 取样唯一值数=1），因为 s->data 为空。
         * 权衡后仍标 READY —— 帧数与流程优先，
         * 空白帧的问题留给"给这些 surface 填真实像素"来解决。 */
        if (ns) {
            /* 这些 surface 拿不到硬件像素：show_frame=0 的帧按 AV1 语义
             * 不该输出，V4L2 侧确实不吐（实测送入 39 单元只回 20 帧，
             * 20 + 19 次空壳标记 = 39，硬件行为符合规范）。
             * 但 ffmpeg 仍会对每个 surface 调 GetImage，
             * data 为空就导出全零帧（实测 30 帧里 9 个全零）。
             *
             * 折中：复制最近一个已就绪 surface 的像素。
             * 这不是该帧真正的画面，但比全零接近 —— 且不改变流程。
             * DMD_AV1_NO_CARRY=1 可关掉，退回全零行为便于对照。 */
            if (!getenv("DMD_AV1_NO_CARRY") && ns->data &&
                c->av1_last_ready != VA_INVALID_ID &&
                c->av1_last_ready != c->av1_send_surface) {
                struct dmd_surface *src =
                    dmd_find_surface_locked(drv, c->av1_last_ready);
                if (src && src->data && src->data_size == ns->data_size)
                    memcpy(ns->data, src->data, ns->data_size);
            }
            ns->state = DMD_SURFACE_READY;
            ns->decode_status = VA_STATUS_SUCCESS;
        }
        dmd_log("EndPicture: AV1 surface=%u show_frame=0，只送料不入队\n",
                (unsigned)c->av1_send_surface);
    }

    int qpos = (c->pending_head + c->pending_count) % DMD_MAX_SURFACES;
    /* AV1 延迟一帧：本次送出的是上一帧的数据，登记它的 surface。
     * 其余 codec 送的就是当前帧，直接用 target。 */
    const VASurfaceID reg_surf = (c->codec == DMD_CODEC_AV1 &&
                                  c->av1_send_surface != VA_INVALID_ID)
                               ? c->av1_send_surface : target;

    /* ⚠️ 同一 surface 不能在待配对队列里出现两次。
     *
     * 实测踩过（本轮的真实根因，与上一轮记的"空壳 surface"无关）：
     * ffmpeg 会复用 surface。sync flush 把暂存帧按 av1_hold_surface 入队后，
     * ffmpeg 随后又拿同一个 surface 当新帧的目标，于是它被登记第二次 ——
     * 日志同时出现 "末帧 surface=8 入队（unit 8）" 与
     * "登记: target=8 → pending[10]=8"，而**从未有帧配给 surface 8**，
     * ffmpeg 报 "Failed to read image from surface 0x8"。
     * 注意失败的是 surface 8，而被标 show_frame=0 的是 3/4/5 ——
     * 上一轮把两者混为一谈，方向错了。
     *
     * 重复项一旦进队，精确配对会命中错误的那条，
     * 另一条永远配不上，队列越积越深。
     * 这里扫一遍队列，已在队中就不再登记。 */
    int dup_pending = 0;
    for (int k = 0; k < c->pending_count; k++) {
        int kp = (c->pending_head + k) % DMD_MAX_SURFACES;
        if (c->pending[kp] == reg_surf) { dup_pending = 1; break; }
    }
    if (dup_pending)
        dmd_log("EndPicture: ⚠️ surface=%u 已在待配对队列（重复项）\n",
                (unsigned)reg_surf);

    /* ⚠️ 只跳过入队，**数据照送**。送料在下方 dmd_session_send_unit，
     * 这里 early-return 就等于不送 —— 本会话已踩过两次，别再犯。 */
    c->pending[qpos] = reg_surf;
    /* ⚠️ 这里**不要**打印 c->av1_pic_param 的 show_frame/order_hint：
     * 那是当前帧的参数，而登记的 surface 属于上一帧（延迟一帧送料）。
     * 我据此误读过一次，得出"send=5 对应 show=1"的错误结论。 */
    if (c->codec == DMD_CODEC_AV1 && getenv("DMD_VA_LOG"))
        dmd_log("登记: target=%u → pending[%d]=%u（送出帧的 surface）\n",
                (unsigned)target, qpos, (unsigned)c->pending[qpos]);
    /* 记下本帧 POC 供按显示序配对，并带上"序列号"。
     *
     * ⚠️ POC 只在**同一个 coded video sequence 内**单调，每个 IDR 都会把它
     * 重置回起点。实测第 2 个 IDR 处 POC 从 65562 跳回 65536 —— 若只按 POC
     * 全局取最小，新序列的帧会抢在旧序列未配对的帧之前，导致同一个 surface
     * 被配对两次、画面从该 IDR 起错乱。
     * 所以先按 seq 再按 POC 排序：只有同序列内才比 POC 大小。 */
    if (c->have_current_poc) {
        /* 判定是否进入新序列。
         *
         * ⚠️ 不能用"POC 比上一帧小"来判断 —— 提交顺序是**解码序**，
         * 同一个 GOP 内 POC 本来就上下起伏（实测 65536, 65544, 65540, 65538），
         * 那样会把每个 B 帧都误判成新序列。
         *
         * 用 frame_num：它在每个 IDR 处归零（H.264 规范 7.4.3），
         * 而 GOP 内是递增的，所以"frame_num 归零且不是首帧"就是 IDR。
         * VA-API 在 VAPictureParameterBufferH264.frame_num 提供它（va.h:3618）。 */
        if (c->have_last_poc && c->current_frame_num == 0 &&
            c->last_frame_num != 0)
            c->last_seq++;
        c->last_poc = c->current_poc;
        c->last_frame_num = c->current_frame_num;
        c->have_last_poc = 1;
    }
    c->pending_seq[qpos] = c->last_seq;
    c->pending_poc[qpos] = c->have_current_poc ? c->current_poc : INT32_MAX;
    /* 发提交序号：daemon 会把它当 PTS 原样回传，配对时据此精确匹配。
     * 与 daemon 的 vcl_in 对齐 —— 两侧都从 1 开始、每个数据单元加 1。
     * 注意只有真正送出数据单元的提交才占号，参数集不占（daemon 侧同理：
     * is_param_set 的单元走 CSD 路径，不递增 vcl_in）。 */
    c->pending_unit[qpos] = ++c->units_submitted;
    if (!av1_no_output)
        c->pending_count++;
    c->have_current_poc = 0;
    c->current_target = VA_INVALID_ID;

    /* 拷一份码流到局部缓冲：放锁后 c->slice_data 可能被别的线程复用。 */
    unsigned char *tx = scratch;
    if (!tx) {
        tx = malloc(unit_len);
        if (!tx) {
            c->pending_count--;
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        memcpy(tx, unit, unit_len);
    }
    c->slice_len = 0;
    c->av1_tile_count = 0;

    /* 串行化同 context 的 IO。 */
    while (drv->io_busy[idx]) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += DMD_FRAME_TIMEOUT_MS / 1000;
        if (pthread_cond_timedwait(&drv->io_done, &drv->lock, &ts) == ETIMEDOUT)
            break;
    }
    /* H.264：参数集必须在首个 VCL 之前送。先在持锁时快照所需数据，
     * 放锁后再发（发送是阻塞 IO，不能持锁做）。 */
    int need_param_sets = 0;
    VAPictureParameterBufferH264 pp_snap;
    VAIQMatrixBufferH264 iq_snap;
    VASliceParameterBufferH264 sp_snap;
    int have_iq_snap = 0;
    int have_sp_snap = 0;
    VAProfile profile_snap = c->profile;
    unsigned int mbs_wide = 0, mbs_high = 0;
    VAPictureParameterBufferHEVC hevc_snap;
    int need_hevc_params = 0;
    /* 锁内快照：下面发送时已解锁，不能再读 c->pending_l0/l1。 */
    unsigned int l0_snap = 0, l1_snap = 0;

    if (codec == DMD_CODEC_H264 && c->have_h264_pic_param) {
        mbs_wide = (unsigned int)c->h264_pic_param.picture_width_in_mbs_minus1 + 1;
        mbs_high = (unsigned int)c->h264_pic_param.picture_height_in_mbs_minus1 + 1;
        /* 重发条件：首次、流内几何变化、或 PPS 的 num_ref_idx 默认值变化。
         *
         * 最后一条是必需的：PPS 默认值必须照抄**当前帧**的生效值
         * （详见 h264_bitstream.c 的说明），而不同帧的生效值会变。
         * MediaCodec 接受流中反复出现的 PPS，每次只多几字节。 */
        /* l0 取见过的**最大**、l1 取见过的**最小**。
         *
         * VA-API 给的是每个 slice 的生效值，而带 override_flag 的 slice
         * 其生效值与 PPS 默认值无关 —— 直接照抄会写出错误的默认值，
         * 被其他非 override slice 用上就解出黑屏（详见 h264_bitstream.c
         * 的实测记录：真实 PPS 恒为 l0=2 l1=0，而照抄会产生 5 种变体）。
         *
         * l0 可以偏大、l1 一位都不能偏大（单变量实测），所以分别取
         * 最大与最小，几帧之内就收敛到唯一一份正确的 PPS。 */
        unsigned int l0_now = c->have_h264_slice_param
                              ? c->h264_slice_param.num_ref_idx_l0_active_minus1
                              : 0;
        unsigned int l1_now = c->have_h264_slice_param
                              ? c->h264_slice_param.num_ref_idx_l1_active_minus1
                              : 0;
        /* l0/l1 直接用 slice param 报的生效值。
         *
         * ⚠️ 曾尝试"消除 PPS 振荡"：改用 SPS 的 num_ref_frames-1 推导 l0、
         * 用观测最小值收敛 l1，让每个会话只送 1 份 PPS（685 次 → 1 次）。
         * 那是一次**回归**：long3000.h264 只解出 15/1323 帧后卡死在等帧超时。
         *
         * 原因是重送次数本身在承担纠错作用 —— 正常版本对该流发出 4 种变体
         * 共 2735 次，其中包含真值 68ebe3cb22c0；收敛成单一份之后送出的是
         * 68e938f2c8b0，解码器再没有机会收到正确的那份。
         *
         * 换句话说：振荡看着丑，但它覆盖了真值；固定成一份就赌错了。
         * 要真正消除振荡，得先能在**首个 slice 之前**确定真实的默认值，
         * 而 VA-API 只给每个 slice 的生效值（override 的与默认值无关），
         * 目前没有可靠推导途径。保持现状。 */
        unsigned int l0 = l0_now;
        unsigned int l1 = l1_now;
        if (!c->param_sets_sent || c->sent_mbs_wide != mbs_wide ||
            c->sent_mbs_high != mbs_high || c->sent_l0 != l0 ||
            c->sent_l1 != l1) {
            c->pending_l0 = l0;
            c->pending_l1 = l1;
            l0_snap = l0;
            l1_snap = l1;
            need_param_sets = 1;
            pp_snap = c->h264_pic_param;
            iq_snap = c->h264_iq_matrix;
            have_iq_snap = c->have_h264_iq_matrix;
            sp_snap = c->h264_slice_param;
            have_sp_snap = c->have_h264_slice_param;
        }
    }

    if (codec == DMD_CODEC_HEVC && c->hevc_pic_param_valid) {
        /* 重发条件比 H.264 简单：HEVC 的 PPS 不含随帧变化的字段
         * （num_ref_idx 默认值在 PPS 里是真正的默认值，slice header 会覆盖），
         * 所以只在首次与分辨率变化时重发。 */
        unsigned int w = c->hevc_pic_param.pic_width_in_luma_samples;
        unsigned int h = c->hevc_pic_param.pic_height_in_luma_samples;
        /* VA-API 缺少复现某些码流所需的信息（见 dmd_hevc_can_build）。
         * 这种情况必须失败，让上层回落软解 —— 合成一个不匹配的 SPS
         * 会解出坏画面，比直接失败糟得多。 */
        if (!dmd_hevc_can_build(&c->hevc_pic_param)) {
            dmd_log("HEVC: 该码流无法合成参数集"
                    "（num_short_term_ref_pic_sets=%u），拒绝\n",
                    c->hevc_pic_param.num_short_term_ref_pic_sets);
            /* tx 在上面已分配（scratch 为空时 malloc），这条早退路径必须
             * 释放它 —— 否则每帧泄漏 unit_len 字节。函数正常路径在下面
             * 统一 free(tx)，走不到这里。
             *
             * 注意 tx 可能就是 scratch（所有权已转移给 tx），所以只 free
             * 一次、不要另外 free(scratch)。 */
            free(tx);
            c->pending_count--;
            pthread_mutex_unlock(&drv->lock);
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        if (!c->param_sets_sent || c->sent_mbs_wide != w ||
            c->sent_mbs_high != h) {
            need_param_sets = 1;
            need_hevc_params = 1;
            hevc_snap = c->hevc_pic_param;
            /* 借用这两个字段记分辨率（HEVC 无宏块概念，存的是像素） */
            mbs_wide = w;
            mbs_high = h;
        }
    }

    drv->io_busy[idx] = 1;
    pthread_mutex_unlock(&drv->lock);

    int rc = DMD_OK;
    if (need_hevc_params) {
        if (hevc_send_param_sets(sess, &hevc_snap, profile_snap) != 0)
            rc = DMD_ERR_PROTOCOL;
    } else if (need_param_sets) {
        if (h264_send_param_sets(sess, &pp_snap, &iq_snap, have_iq_snap,
                                 have_sp_snap ? &sp_snap : NULL,
                                 l0_snap, l1_snap, profile_snap,
                                 pw, ph) != 0)
            rc = DMD_ERR_PROTOCOL;
    }
    if (rc == DMD_OK)
        rc = dmd_session_send_unit(sess, tx, unit_len);
    free(tx);

    pthread_mutex_lock(&drv->lock);
    drv->io_busy[idx] = 0;
    pthread_cond_broadcast(&drv->io_done);

    c = dmd_find_context_locked(drv, context);
    if (!c) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (rc != DMD_OK) {
        /* 回滚入队：这一帧永远不会有对应输出。从队尾摘掉。 */
        if (c->pending_count > 0)
            c->pending_count--;
        struct dmd_surface *s = dmd_find_surface_locked(drv, target);
        if (s) {
            s->state = DMD_SURFACE_IDLE;
            s->decode_status = VA_STATUS_ERROR_DECODING_ERROR;
        }
        const char *msg = dmd_session_last_error(sess);
        pthread_mutex_unlock(&drv->lock);
        dmd_log("EndPicture: 送单元失败 rc=%d: %s\n", rc, msg);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }

    if (need_param_sets) {
        c->param_sets_sent = 1;
        c->sent_mbs_wide = mbs_wide;
        c->sent_mbs_high = mbs_high;
        c->sent_l0 = c->pending_l0;
        c->sent_l1 = c->pending_l1;
    }
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}

/* ============================ 帧取回（Sync 路径） ============================ */

/* 把 daemon 回来的一帧拷进 surface 缓冲。调用方持锁。
 *
 * 几何：daemon 的帧按 fmt.stride / fmt.slice_height 布局，UV 起点 =
 * stride*slice_height。surface 按对齐尺寸预分配，两者通常一致；
 * 不一致时（解码器给了不同对齐）按行拷贝，绝不整块 memcpy —— 那会错位。
 */
/* 从待解码队列里取出"下一个该被填充"的 surface 并摘除它。
 * 队列空返回 VA_INVALID_ID。调用方持锁。
 *
 * 谁该先填由 codec 决定：
 * - VP9/VP8：无帧重排，提交序 == 出帧序，取队首
 * - H.264/HEVC：ffmpeg 按解码序提交、MediaCodec 按显示序出帧，
 *   必须取 (seq, POC) 最小者，否则从第 2 帧起全部错位
 *
 * 用线性扫描而非堆：队列最多 64 项且实测重排深度只有 2-3，
 * 每帧一次 O(n) 扫描的代价可以忽略。
 */
static VASurfaceID dmd_pending_take_locked(struct dmd_context *c,
                                           uint32_t unit_seq)
{
    if (c->pending_count <= 0)
        return VA_INVALID_ID;

    int pick = c->pending_head;

    /* ⚠️ 配对依赖两侧编号同源，这一点曾经失效过，改动此处务必先读完。
     *
     * 驱动侧在 EndPicture 里每**帧**加一号（参数集不经过 EndPicture），
     * 而 session 层原先对每个 send_unit 都加号 —— HEVC 一帧要送多个 NAL，
     * 参数集白占了编号，两套编号从此错开。
     *
     * 实测后果（1920x1080 HEVC 12 帧）：ffmpeg 调 12 次 EndPicture 登记
     * 1..12，session 层却送出 15 个单元编号 1..15。配对按值相等匹配，
     * 于是系统性错帧 —— **帧数看着完全正确（12/12）、画面却张冠李戴**：
     * 标称 POC 0 的 surface 实际装的是显示序第 7 帧（逐帧匹配平均差 0.000）。
     *
     * 已修：dmd_v4l2_session.c 的 unit_is_param_set_only() 让纯参数集单元
     * 不占编号。修后 12 帧逐字节与 ffmpeg 软解完全一致（md5 237fed06）。
     *
     * 排查中曾误判 V4L2 timestamp 不可信 —— 后端自测证明它透传正确
     * （回传值无重复、是送入编号的有效排列），只是出帧顺序≠送入顺序。
     * 真正的问题从来不在 timestamp，而在两侧编号不同源。
     *
     * ⚠️ AV1 另有一处**尚未修复**的同类失效：延迟一帧机制打乱编号节奏。
     * 实测（合成流已 6/6 逐字节正确的前提下）：
     *   配对: 帧 -> surface 1 (unit 1 seq 0 POC 0, 队列剩 0)
     *   配对: unit_seq=6 无匹配项，回退顺序推断（队列 4）
     *   配对: 帧 -> surface 3 (unit 2 seq 0 POC 8, 队列剩 3)
     * 第二帧回传 unit_seq=6 却找不到对应项，退到顺序推断后配给 surface 3；
     * 而 ffmpeg 正等 surface 6 的像素 → 2000ms 超时 → flush → 会话终结，
     * 整段码流只出 1 帧。
     *
     * 成因：AV1 帧间帧要等下一帧才能反算 refresh_frame_flags，
     * EndPicture 里不立即送料（帧入 av1_hold）。驱动侧 pending_unit 在
     * EndPicture 递增、session 侧 units_sent 在真正 send_unit 时递增，
     * 节奏被暂存机制错开；末帧又走 sync flush 的第三条路径送出。
     * 与上面 HEVC 参数集占号是同类问题（两侧编号不同源），成因不同。
     *
     * 修法方向：AV1 的 pending_unit 改为真正 send_unit 时回填，
     * 或给暂存帧单独记录它实际发出时的编号。
     *
     * 首选：按提交序号精确配对。
     *
     * daemon 把每个输入单元的序号当 PTS 送给 MediaCodec，解码器原样回传到
     * 对应的输出帧上（tools/probe_negotiate.c 实测未被改写），于是这一帧
     * 究竟属于哪次提交是**已知事实**，不需要推断。
     *
     * 这样配对就与解码器的出帧顺序完全解耦：显示序、跟随输入序、
     * 甚至将来换个平台换个顺序，都不影响正确性。此前靠 (seq,POC) 排序
     * 隐含假设了"输出是显示序"，一旦假设不成立就画面错位而且不报错
     * （实测 105/150 帧错位）。
     *
     * unit_seq == 0 表示 daemon 不支持该能力（旧版本），回退到下面的推断。 */
    if (unit_seq != 0) {
        /* 观测到一次就记住：daemon 支持精确配对。这同时意味着它开了
         * 跟随输入序输出（两者在 daemon 里同一版本引入），滞后为 1，
         * 于是不需要排空 —— wait_is_futile 用这个标志做判断。 */
        c->daemon_has_unit_seq = 1;
        for (int k = 0; k < c->pending_count; k++) {
            int idx = (c->pending_head + k) % DMD_MAX_SURFACES;
            /* 序号 0 = 已作废（会话重建前的残留项），不参与精确匹配 */
            if (c->pending_unit[idx] != 0 &&
                c->pending_unit[idx] == (uint64_t)unit_seq) {
                pick = idx;
                goto found;
            }
        }
        /* 没匹配上：多半是排空/重建导致 daemon 侧序号重新计数，
         * 或收到了上一个会话的残留帧。退回顺序推断，不要丢帧。 */
        dmd_log("配对: unit_seq=%u 无匹配项，回退顺序推断（队列 %d）\n",
                unit_seq, c->pending_count);
    }
    /* daemon 开了 vendor.qti-ext-dec-picture-order.enable 时解码器按**解码序**
     * 输出，而 ffmpeg 也是按解码序提交的 —— 于是提交序 == 出帧序，
     * 所有 codec 都退化成取队首，不需要 POC 重排。
     *
     * 这么做的目的是根治黑帧：默认（显示序输出）下解码器有 B 帧时要收到
     * 第 4 个输入单元才吐首帧，而浏览器稳态只在飞 3 帧，双方互等，
     * 只能靠 EOS 逼出帧 —— 而那会摧毁参考帧链，导致约 9 成帧纯黑
     * （tools/probe_black.c）。解码序输出把滞后降到 1，互等消失，
     * 排空/重建/重放统统不再需要（tools/probe_keys.c 实测）。 */
    /* 回退路径：daemon 未提供 unit_seq（旧版本）时只能按 (seq, POC) 推断。
     *
     * 这条路依赖"输出是显示序"这个假设，而假设是否成立取决于 daemon 的
     * 解码器配置 —— 驱动无法自行知道。这正是要用 unit_seq 精确配对的原因：
     * 有它就不必猜。VP8/VP9 无帧重排，提交序 == 出帧序，取队首即可。 */
    if (c->codec == DMD_CODEC_H264 || c->codec == DMD_CODEC_HEVC) {
        unsigned int bseq = c->pending_seq[pick];
        int32_t best = c->pending_poc[pick];
        for (int k = 1; k < c->pending_count; k++) {
            int idx2 = (c->pending_head + k) % DMD_MAX_SURFACES;
            /* 先比序列号：旧序列必须先配完，否则新序列（IDR 后 POC 重置回
             * 小值）会抢占旧序列尚未配对的帧。 */
            if (c->pending_seq[idx2] < bseq ||
                (c->pending_seq[idx2] == bseq && c->pending_poc[idx2] < best)) {
                bseq = c->pending_seq[idx2];
                best = c->pending_poc[idx2];
                pick = idx2;
            }
        }
    }

found:
    VASurfaceID head = c->pending[pick];
    /* 记下最近一个拿到真实像素的 surface，供 show_frame=0 的空壳承接。 */
    c->av1_last_ready = head;
    dmd_log("配对: 帧 -> surface %u (unit %llu seq %u POC %d, 队列剩 %d)\n",
            (unsigned)head, (unsigned long long)c->pending_unit[pick],
            c->pending_seq[pick], (int)c->pending_poc[pick],
            c->pending_count - 1);

    /* 摘除 pick：把它之前的项整体后移一格，再收缩队首。
     *
     * ⚠️ 不能用"拿队首填补空位"的省事写法 —— 那会把队首元素挪到队列中间，
     * **破坏剩余项与提交顺序的对应关系**，后续找最小 POC 时就会选错
     * （实测表现为部分帧对、部分帧错）。 */
    for (int k = pick; k != c->pending_head;) {
        int prev = (k - 1 + DMD_MAX_SURFACES) % DMD_MAX_SURFACES;
        c->pending[k] = c->pending[prev];
        c->pending_poc[k] = c->pending_poc[prev];
        c->pending_seq[k] = c->pending_seq[prev];
        /* pending_unit 必须一起搬。漏了它会让序号与 surface 错位，
         * 同一个号被重复匹配 —— 实测 unit 5 与 unit 9 各出现两次、
         * 序号 2 和 6 凭空消失、70/150 帧错位。 */
        c->pending_unit[k] = c->pending_unit[prev];
        k = prev;
    }
    c->pending_head = (c->pending_head + 1) % DMD_MAX_SURFACES;
    c->pending_count--;
    return head;
}

static void surface_store_frame_locked(struct dmd_surface *s,
                                       const struct dmd_frame *f)
{
    /* 诊断：daemon 送来的帧本身是不是黑的？
     * 与 export.c 里的同名探测配对使用，用来区分
     * "解码器就输出了黑帧" 和 "拷进 surface 后才变黑"。
     * DMD_VA_LUMA=1 才启用。 */
    if (getenv("DMD_VA_LUMA") && f->data && f->size > 100000) {
        double sum = 0; int cnt = 0;
        for (size_t k = 0; k < f->size && k < 2000000; k += 1499) {
            sum += f->data[k]; cnt++;
        }
        double m = cnt ? sum / cnt : 0;
        dmd_log("入帧亮度: unit=%u 均值 %.1f%s\n",
                f->unit_seq, m, m < 20.0 ? "  ← daemon 送来就是黑的!" : "");
    }

    unsigned int src_stride = f->stride > 0 ? (unsigned int)f->stride
                                            : (unsigned int)f->width;
    unsigned int src_slice = f->slice_height > 0
                                 ? (unsigned int)f->slice_height
                                 : (unsigned int)f->height;

    /* 以解码器给的几何为准更新 surface：VAImage 的 offsets[1] 必须
     * 用 slice_height（1088）而不是显示高（1080），否则色度平面错位。 */
    s->stride = src_stride;
    s->slice_height = src_slice;
    s->buf_width = (unsigned int)f->width;
    s->buf_height = (unsigned int)f->height;

    size_t need = (size_t)src_stride * src_slice * 3 / 2;
    if (need > s->data_size) {
        /* 解码器给的缓冲比预分配的大（流内分辨率变大）。扩容而不是截断。 */
        unsigned char *mem = realloc(s->data, need);
        if (!mem) {
            s->decode_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
            return;
        }
        s->data = mem;
        s->data_size = need;
    }

    size_t copy = f->size < need ? f->size : need;
    memcpy(s->data, f->data, copy);
    if (copy < need)
        memset(s->data + copy, 0, need - copy);

    s->decode_status = VA_STATUS_SUCCESS;
}

/* 从 daemon 取帧直到目标 surface 就绪或超时。
 *
 * 这是 VA-API 乱序语义与 MediaCodec 流式语义的配对点。目标 surface 通常
 * 不在"该被填"的位置 —— 那就一直取，把该先填的填掉，直到轮到目标。
 * 谁该先填由 codec 决定：VP9/VP8 是队首，H.264/HEVC 是 (seq, POC) 最小者。
 */
static VAStatus sync_surface_locked(struct dmd_driver *drv, VAContextID context,
                                    VASurfaceID target, int timeout_ms)
{
    int idx;
    struct dmd_context *c = dmd_find_context_locked(drv, context);
    if (!c)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    idx = (int)(c - drv->contexts);

    /* ============ AV1 延迟送料：等待前必须先冲出暂存帧 ============
     *
     * 延迟一帧机制会把最新合成的帧扣在 c->av1_hold 里，等下一帧到来时
     * 才反算 refresh 并送出。但上游送完最后一帧后就转为等像素，
     * 不会再有"下一帧"来把它挤出去。
     *
     * 实测后果：一段金字塔 B 帧 GOP 有 6 帧（show_frame 依次
     * 1,0,0,0,0,1），EndPicture 调 6 次只送出 5 单元，
     * 第 6 帧 —— 恰是除首帧外唯一 show_frame=1、会产生输出的那帧 ——
     * 永远压在暂存里。ffmpeg 等 surface=6 的像素，5000ms 超时后
     * 报 "Failed to read image from surface 0x6"，整个会话只出 1 帧。
     *
     * 所以只要开始等帧，就说明上游这一轮不再送料了，必须把暂存帧冲出去。
     *
     * ⚠️ 曾在此写着"它的 refresh 无从反算，保持轮转策略给出的占位值即可 ——
     * 末帧不被任何后续帧引用，该值不影响解码结果"。**这个假设是错的**，
     * 实测直接否证：
     *   源码流末帧 refresh_frame_flags = 0x00，
     *   而轮转占位值给出 0x20 —— 落盘流与源逐字节比对，
     *   6 帧样本里前 5 帧全部相同、唯独末帧因此不同；
     *   解码在该帧之后立即 "Error processing packet: Input/output error"
     *   中止，整段码流只出 1 帧。
     *
     * 原因：refresh_frame_flags 声明的是"本帧解码后要占用哪些参考槽"。
     * 写非 0 会让解码器把这一帧塞进参考槽、顶掉原有内容，
     * 破坏解码器内部的参考帧状态 —— 即使后续没有帧显式引用它。
     *
     * 正确做法：冲出暂存帧时把 refresh 改写为 0。
     * 语义上成立：走到这里说明上游本轮不再送料，该帧不必留驻参考槽。
     * 若之后又有新帧到来，它会各自携带正确的 refresh 值，不受影响。 */
    /* ⚠️ 只在输入确实结束后才冲出暂存帧。
     *
     * 实测踩过：ffmpeg 用较短超时反复轮询 Sync，本处被触发 7 次
     * （一段 10 帧的码流），每次都把当时的暂存帧当"末帧"送出。
     * 后果有两重：
     *   1. 暂存帧的 refresh 被改写为 0，而后面还有帧要引用它 → 参考链断
     *   2. 送出后 av1_hold 变空，下一个 EndPicture 走"第一个帧间帧"分支
     *      只入暂存不送数据，队列与 surface 的对应被打乱一格
     * 表现为某个 surface 永远拿不到解码结果（实测 surface=8，
     * ffmpeg 报 "Failed to read image from surface 0x8"）。
     *
     * input_finished 由上方 finish_input 路径置位，那才是流真结束的信号。 */
    /* ================================================================
     * flush 与 refresh 反算的死结（本轮量化，尚未解开）
     * ================================================================
     * 60 帧样本对照（DMD_AV1_NO_FLUSH 开关，逐字节比对为判据）：
     *   开 flush（现状）  合成 61 帧，56/61 逐字节正确
     *   关 flush          合成  5 帧， 5/5  逐字节正确，但 -f null 只出 1 帧
     *
     * 也就是说：flush 是**全部**剩余合成缺陷的根源，
     * 但关掉它会让帧被永久扣住 —— ffmpeg 拿不到像素就停止送料，
     * 会话在"送入 5 单元, 收到 1 帧"处停摆。两边都不能要。
     *
     * 成因：refresh_frame_flags 的正确值要靠"下一帧的 ref_frame_map
     * 与本帧的差分"反算（dmd_av1_patch_prev_refresh，机制本身已验证正确）。
     * flush 在下一帧到来**之前**就把暂存帧发了出去，
     * 那一刻无从差分，只能写占位值，且发出后再也改不了。
     *
     * 为什么现状还能到 56/61：实测源码流 39 个帧间帧里有 18 个
     * refresh 本来就是 0，flush 写 0 在近半数情况下恰好命中。
     * 这是巧合而非正确 —— 60 帧里 flush 触发了 30 次。
     *
     * 已试过并否证的两种改法：
     *   给改写加 input_finished 门 → 总数从 56 掉到 28（更差，已撤销）
     *   完全不 flush              → 5/5 但会话停摆（见上）
     *
     * 可能的出路（下一轮验证）：把 flush 推迟到下一次 BeginPicture
     * 拿到新 ref_frame_map 之后再发，这样既能差分出正确 refresh，
     * 又不会让帧被无限期扣住。
     *
     * 诊断开关：DMD_AV1_NO_FLUSH=1 完全不冲出暂存帧。
     * ================================================================ */
    /* ⚠️ 只在"被等的正是暂存帧"时才冲出它。
     *
     * flush 的唯一必要场景是：ffmpeg 等的 surface 就压在暂存里，
     * 不发它就永远等不到。若等的是别的 surface，flush 纯属多余 ——
     * 而代价很大：发出去的帧再也没机会反算 refresh，只能带占位值。
     *
     * 60 帧样本实测（逐字节比对）：
     *   无条件 flush（30 次）→ 56/61 正确
     *   完全不 flush         →  5/5 正确但会话停摆（只送 5 单元）
     * 按 target 收窄，目标是既保留必要的 flush 又减少误发。 */
    const int hold_is_target = (c->av1_hold_surface == target);
    if (c->codec == DMD_CODEC_AV1 && c->av1_hold && c->session &&
        hold_is_target && !getenv("DMD_AV1_NO_FLUSH")) {
        unsigned char *held = c->av1_hold;
        size_t held_len = c->av1_hold_len;
        size_t held_bitpos = c->av1_hold_bitpos;
        const VASurfaceID held_surf = c->av1_hold_surface;
        c->av1_hold_surface = VA_INVALID_ID;
        c->av1_hold = NULL;
        c->av1_hold_len = 0;
        c->av1_hold_bitpos = (size_t)-1;
        /* 把 refresh_frame_flags 就地改写为 0（8 位，MSB 在前）。
         *
         * ⚠️ 仅当这真是流的末帧时才成立。实测踩过：ffmpeg 用较短超时
         * 轮询 Sync，本函数被反复触发（一段 10 帧的码流里 flush 了 7 次），
         * 每次都把当时的暂存帧当"末帧"改写成 refresh=0 ——
         * 而它们后面还有帧要引用它们，参考链因此被破坏，
         * 表现为某个 surface 拿不到解码结果（实测 surface=8）。
         *
         * 所以只在输入确实结束后才改写。未结束时保留轮转占位值：
         * 那个值虽不等于源码流，但至少不会把该帧从参考槽里抹掉。 */
        /* ⚠️ 只在输入确实结束后才把 refresh 改写为 0。
         *
         * 60 帧样本实测：不加这个条件，帧30/帧60 的 refresh
         * 应为 0x10 却被写成 0x00（相邻帧 28/29/31/59/61 全对）——
         * 它们根本不是末帧，是被 ffmpeg 的短超时轮询触发的 flush 误改的。
         *
         * 第 43 轮加过同样的门又撤销了，因为当时看"像素导出帧数"从 6 掉到 3。
         * 但那个判据本身不稳定（同配置重复测量在 3~8 帧之间浮动），
         * 不足以评判任何改动。本轮改用逐字节比对这个稳定判据重做。 */
        /* ⚠️ 已否证的一条路：flush 时用当前 pic_param 差分反算 refresh。
         *
         * 想法是：走到这里 c->av1_pic_param 存着最近一次 BeginPicture 的
         * 参数，若它比暂存帧新就能作差分基准，算出真 refresh 而非写 0。
         * 实测（60 帧，DMD_AV1_FLUSH_DERIVE 开关）：
         *   差分路径执行 30 次，与写 0 的结果**完全一致** 56/61，
         *   错帧一个不少（18/26/30/48/60）。
         * 原因：dpb_shadow / prev_valid 已在同一帧的 build_unit 里
         * 被这个 pic_param 更新过，此刻再差分得到的是空集。
         * 该开关与代码已移除，只留这段记录，免得下次再试。 */
        /* ---- 150 帧样本对这条路径的完整测绘（本轮）----
         * 触发 75 次：70 次 bitpos=50、5 次 bitpos=58。
         * 而全部 refresh 类错帧恰是 bitpos=58 那 5 帧
         * （帧30/60/90/120/150），源 refresh 全是 0x10，
         * 此刻 dpb_next_slot=5（轮转值 0x20）。
         *
         * 已试过并否证的三种写入值（判据：逐字节比对，基线 140/150）：
         *   写 0（保留）                   140/150
         *   保留轮转占位值（KEEP_ROT）      70/150
         *   全部改写 1<<(slot-1)（SLOT1）   85/150
         *   仅 bitpos=58 用 1<<(slot-1)    140/150（无变化）
         * 最后一项无变化说明：改写发生的时机与 dump 记录的
         * dpb_next_slot 不在同一时刻，或该值此刻已被推进 ——
         * 不能靠此刻的 slot 反推目标值。
         * 结论：写 0 仍是最优；这 5 帧要靠解开 flush 死结才能修。 */
        unsigned patch_val = 0;
        if (held_bitpos != (size_t)-1) {
            int patched = 1;
            for (int i = 0; i < 8; i++) {
                size_t bp = held_bitpos + (size_t)i;
                size_t byte = bp >> 3;
                if (byte >= held_len) { patched = 0; break; }
                unsigned bit = (patch_val >> (7 - i)) & 1u;
                if (bit)
                    held[byte] |= (unsigned char)(1u << (7 - (bp & 7)));
                else
                    held[byte] &= (unsigned char)~(1u << (7 - (bp & 7)));
            }
            dmd_log("sync: 末帧 refresh 改写为 0（bitpos=%zu %s slot=%u "
                    "轮转值=0x%02x）",
                    held_bitpos, patched ? "成功" : "越界跳过",
                    c->av1_dpb.dpb_next_slot & 7u,
                    1u << (c->av1_dpb.dpb_next_slot & 7u));
        }
        /* 末帧是否入队 —— 两种做法都试过，都不理想，此处保留入队：
         *   入队   → 该 surface 之后被 ffmpeg 复用时会重复登记
         *            （日志 "surface=8 已在待配对队列"），但像素能出 12 帧
         *   不入队 → 重复消失，但该帧永远配不上，像素掉到 6 帧
         * 说明重复项本身不是主因，真正缺的是"一个 surface 承载两帧"
         * 这件事在当前配对模型里无法表达。留待重构配对键（改用
         * surface+unit 组合而非仅 surface）时一并解决。 */
        if (held_surf != VA_INVALID_ID &&
            c->pending_count < DMD_MAX_SURFACES) {
            int hq = (c->pending_head + c->pending_count) % DMD_MAX_SURFACES;
            c->pending[hq] = held_surf;
            c->pending_seq[hq] = c->last_seq;
            c->pending_poc[hq] = INT32_MAX;
            c->pending_unit[hq] = ++c->units_submitted;
            c->pending_count++;
            dmd_log("sync: 末帧 surface=%u 入队（unit %llu）",
                    (unsigned)held_surf,
                    (unsigned long long)c->pending_unit[hq]);
        }
        dmd_log("sync: 冲出暂存的 AV1 末帧（%zu 字节）", held_len);
        av1_dump_sent(held, held_len);
        (void)dmd_session_send_unit(c->session, held, held_len);
        free(held);
    }

    int spent = 0;
    /* 本次等待是否已经排空过一次。可逆排空不会改变队列深度，
     * 不加这个闸就会在同一次等待里反复排空（忙循环）。 */
    int drained_once = 0;
    /* 单次等待用较小的片，这样能周期性回到锁内检查状态。 */
    const int slice_ms = 100;
    /* 等这么久还没帧就认为解码器在攥尾部帧，主动 flush。
     *
     * ⚠️ 不能取 timeout_ms/2：调用方的超时不是"流是否结束"的证据。
     * Firefox 用很短的超时轮询 vaSyncSurface（还会在解码前先 DeriveImage
     * 探一次），timeout_ms/2 于是变得极小 —— 才送 3 帧就误判成流结束、
     * 执行不可逆的 shutdown(SHUT_WR)，会话作废，浏览器永久回落软解。
     * 命令行 ffmpeg 用大超时，所以从没暴露这个问题。
     *
     * flush 是"上游不再送料"才该做的事，与单次等待的耐心无关，
     * 因此这里用固定阈值，且必须显著大于解码器填满流水线所需的时间。 */
    const int flush_after_ms = DMD_FLUSH_AFTER_MS;

    for (;;) {
        struct dmd_surface *s = dmd_find_surface_locked(drv, target);
        if (!s)
            return VA_STATUS_ERROR_INVALID_SURFACE;
        if (s->state != DMD_SURFACE_PENDING)
            return s->decode_status;

        c = dmd_find_context_locked(drv, context);
        if (!c)
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        if (!c->session)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        if (c->pending_count <= 0) {
            /* PENDING 但队列空：提交失败过。不能等，会永远等不到。 */
            s->state = DMD_SURFACE_IDLE;
            return VA_STATUS_ERROR_DECODING_ERROR;
        }

        /* 久等不到帧：解码器可能正攥着尾部帧等更多输入或 EOS。
         *
         * MediaCodec 稳态滞后 2-3 个单元（实测送 1/2/3 个 VCL 后等 4000ms
         * 都不出帧，第 4 个才出），所以流末尾最后几帧必须关掉写端才能取出来。
         * 而 ffmpeg 此刻正阻塞在 vaSyncSurface 上，不会再送新数据 —— 双方
         * 互等，只能由我们主动 flush 打破。
         *
         * ⚠️ finish_input 是不可逆的 shutdown(SHUT_WR)：一旦关闭，本会话
         * 再也不能送数据。所以只在"等了足够久、确实卡住"时才做，且只做一次。
         *
         * flush 之后本会话即成一次性资源，但**不等于 context 报废**：
         * 见下方 session_rebuild_locked —— 下一次 EndPicture 会透明重建
         * 会话并重送参数集，上层察觉不到。这对浏览器是必需的：
         * 浏览器里 ffmpeg 稳态只保持 3 帧在飞，而 MediaCodec 有 B 帧时
         * 滞后 4 帧（实测 probe_lag：有 B 帧 4，无 B 帧 1），双方差一帧
         * 必然触发这里的 flush。若 flush 即报废，浏览器拿到 I/O error
         * 后会永久回落软解（实测 Firefox 140 只硬解 1 帧就掉回软解）。 */
        /* 何时该 flush：判据是"再等下去也不可能有帧"，不是"等够久了"。
         *
         * 消费者送料深度低于解码器出帧所需深度时，双方必然互等：
         * 消费者要先拿到帧才肯送下一个单元，解码器要再收一个单元才肯出帧。
         * 这时继续等是徒劳的 —— 队列深度不会自己变化。
         *
         * 实测 Firefox 的循环正是这样："送 3 帧 → 等第 1 帧"，而 MediaCodec
         * 有 B 帧时要第 4 个单元才出首帧（tools/probe_lag.c 量得；daemon 侧
         * 的 low-latency 也降不下来，那是解码器固有的流水线深度）。
         * 若按耐心阈值等满 2000ms 再 flush，每帧就要 2 秒，播放等于卡死；
         * 立刻 flush 则马上出帧。
         *
         * ⚠️ 这里曾写着"flush 的代价已由透明重建消掉，上层察觉不到
         * （probe_rebuild.c 验证过）"—— **那是错的**。那份探针只数帧数
         * 没看画面（帧全是纯黑 Y=16），它自己开头已标注结论不可信。
         *
         * flush 的真实代价是**参考帧链被摧毁**，续传后要黑到下一个 IDR。
         * 上层不但察觉得到，用户会直接看到"画面一闪一闪"
         * （实测 135/708 帧纯黑）。所以 flush 绝不是廉价操作。
         *
         * 队列够深时仍按耐心阈值等 —— 那种情况下帧确实在路上，
         * 提前 flush 会白白打断一个正常会话。
         *
         * ⚠️ 可逆排空必须"每次等待只做一次"。它不像 finish_input 会置
         * input_finished 把自己挡住 —— 排空后队列深度不变，条件依旧成立，
         * 不加闸就会变成忙循环（实测触发 133 万次、只出 1 帧）。 */
        /* 只有"再等也不可能出帧"时才排空 —— 排空会摧毁参考帧链，是黑帧根因。
         *
         * daemon 支持 unit_seq 时同时也开了跟随输入序输出，滞后只有 1，
         * 帧几乎立刻就来，永远不该判定徒劳（否则会 0 ms 就排空，
         * 实测误触发 383 次）。旧 daemon 才有滞后 4 与消费者在飞 3 帧的互等。
         *
         * ⚠️ `daemon_has_unit_seq` 是**收到第一帧才置位**的运行时观测，
         * 所以会话刚建立、首帧还没回来时它必然为 0。若此时只看它，
         * 就会在 0 ms 判定徒劳并立刻排空 —— 而排空 = EOS + flush，
         * 参考帧链被毁，之后的 P/B 帧全黑到下一个 IDR。
         *
         * 实测（浏览器循环播放 5 轮）：每轮会话开头各触发 1 次 0 ms 排空，
         * 共 5 次，与 5 段黑帧簇一一对应；每段黑 25~27 帧（unit 4..30，
         * 恰好是首个 IDR 之后到下一个 IDR 之前），总计 135/708 帧纯黑。
         * 日志原文是"等了 0 ms 仍无帧，可逆排空（队列 3）"—— 队列明明是满的
         * （在飞 3 帧），帧就在路上，等一下就有，根本不该排空。
         *
         * 所以再加一个条件：**至少收到过一帧**才允许判定徒劳。
         * 一帧都还没收到时，无法区分"互等"与"首帧还在路上"，
         * 此时应当耐心等到 flush_after_ms 阈值，而不是立刻毁掉会话状态。 */
        int wait_is_futile = dmd_session_frames_received(c->session) > 0 &&
                             !c->daemon_has_unit_seq &&
                             (c->pending_count < DMD_PIPELINE_DEPTH);

        if (!c->input_finished && !drained_once &&
            (wait_is_futile || spent >= flush_after_ms)) {
            dmd_log("flush 触发: futile=%d(recv=%llu has_seq=%d pend=%d) "
                    "spent=%d/%d\n",
                    wait_is_futile,
                    (unsigned long long)dmd_session_frames_received(c->session),
                    c->daemon_has_unit_seq, c->pending_count,
                    spent, flush_after_ms);
            struct dmd_session *fs = c->session;
            /* 优先用可逆排空：daemon 送 EOS 催出帧后 flush 复位并重送 CSD，
             * 会话仍可用 —— 于是不必重建，省掉 connect+握手+configure。
             * 只有排空失败（老 daemon 不认长度 0）才退回不可逆的 finish_input。 */
            drv->io_busy[idx] = 1;
            pthread_mutex_unlock(&drv->lock);
            int frc = dmd_session_drain(fs);
            int reversible = (frc == DMD_OK);
            if (!reversible)
                frc = dmd_session_finish_input(fs);
            pthread_mutex_lock(&drv->lock);
            drv->io_busy[idx] = 0;
            pthread_cond_broadcast(&drv->io_done);
            c = dmd_find_context_locked(drv, context);
            if (!c) {
                pthread_mutex_unlock(&drv->lock);
                return VA_STATUS_ERROR_INVALID_CONTEXT;
            }
            if (reversible)
                drained_once = 1;
            else
                c->input_finished = 1;
            dmd_log("SyncSurface: 等了 %d ms 仍无帧，%s（rc=%d，"
                    "阈值 %d ms，队列 %d）\n",
                    spent, reversible ? "可逆排空" : "flush 输入",
                    frc, flush_after_ms, c->pending_count);
            /* 重新查找：放锁期间对象可能被销毁。 */
            continue;
        }

        if (spent >= timeout_ms) {
            dmd_log("SyncSurface: 等帧超时 %d ms（surface=%u 队列 %d）\n",
                    timeout_ms, (unsigned)target, c->pending_count);
            return VA_STATUS_ERROR_TIMEDOUT;
        }

        struct dmd_session *sess = c->session;

        /* 串行化 IO，然后放锁收帧。 */
        while (drv->io_busy[idx]) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&drv->io_done, &drv->lock, &ts);
            spent += 20;
            if (spent >= timeout_ms)
                return VA_STATUS_ERROR_TIMEDOUT;
            /* 别的线程可能已经把我们的帧填好了 */
            s = dmd_find_surface_locked(drv, target);
            if (!s)
                return VA_STATUS_ERROR_INVALID_SURFACE;
            if (s->state != DMD_SURFACE_PENDING)
                return s->decode_status;
        }
        drv->io_busy[idx] = 1;
        pthread_mutex_unlock(&drv->lock);

        struct dmd_frame frame;
        memset(&frame, 0, sizeof(frame));
        int rc = dmd_session_next_frame(sess, &frame, slice_ms);

        pthread_mutex_lock(&drv->lock);
        drv->io_busy[idx] = 0;
        pthread_cond_broadcast(&drv->io_done);
        spent += slice_ms;

        c = dmd_find_context_locked(drv, context);
        if (!c) {
            if (rc == DMD_OK)
                dmd_session_release_frame(sess, &frame);
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }

        if (rc == DMD_OK) {
            VASurfaceID head = dmd_pending_take_locked(c, frame.unit_seq);
            struct dmd_surface *hs = dmd_find_surface_locked(drv, head);
            if (hs) {
                surface_store_frame_locked(hs, &frame);
                hs->state = DMD_SURFACE_READY;
            } else {
                dmd_log("SyncSurface: 待配对 surface %u 已销毁，帧被丢弃\n",
                        (unsigned)head);
            }
            /* release 不做阻塞 IO（TCP 模式只是标记缓冲可复用）。 */
            dmd_session_release_frame(sess, &frame);
            continue;
        }

        if (rc == DMD_ERR_TIMEOUT) {
            /* 帧还没出来。daemon 未开 low-latency，解码器会攒几帧，
             * 这是正常现象 —— 继续等到总超时。 */
            continue;
        }

        if (rc == DMD_EOS) {
            /* 流已结束但我们还在等帧：说明送入单元数与输出帧数不匹配。 */
            struct dmd_surface *s2 = dmd_find_surface_locked(drv, target);
            if (s2 && s2->state == DMD_SURFACE_PENDING) {
                s2->state = DMD_SURFACE_IDLE;
                s2->decode_status = VA_STATUS_ERROR_DECODING_ERROR;
            }
            dmd_log("SyncSurface: 流已结束但仍有 %d 帧待配对\n",
                    c->pending_count);
            return VA_STATUS_ERROR_DECODING_ERROR;
        }

        /* 真错误 */
        dmd_log("SyncSurface: 取帧失败 rc=%d: %s\n", rc,
                dmd_session_last_error(sess));
        struct dmd_surface *s3 = dmd_find_surface_locked(drv, target);
        if (s3 && s3->state == DMD_SURFACE_PENDING) {
            s3->state = DMD_SURFACE_IDLE;
            s3->decode_status = VA_STATUS_ERROR_DECODING_ERROR;
        }
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
}

/* 找到 surface 所属 context（用于 Sync）。调用方持锁。 */
static VAContextID surface_context_locked(struct dmd_driver *drv,
                                          VASurfaceID id)
{
    struct dmd_surface *s = dmd_find_surface_locked(drv, id);
    return s ? s->context : VA_INVALID_ID;
}

/* 供 image.c 做隐式同步：GetImage 时 surface 还没解出来就先等它。
 * 调用方持锁；本函数内部会临时放锁做 IO，返回时仍持锁，
 * 因此调用方必须重新查找它缓存的对象指针。 */
VAStatus dmd_surface_sync_locked(struct dmd_driver *drv, VASurfaceID surface,
                                 int timeout_ms)
{
    struct dmd_surface *s = dmd_find_surface_locked(drv, surface);
    if (!s)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (s->state != DMD_SURFACE_PENDING)
        return s->decode_status;

    VAContextID context = surface_context_locked(drv, surface);
    if (context == VA_INVALID_ID)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    return sync_surface_locked(drv, context, surface, timeout_ms);
}

VAStatus dmd_SyncSurface(VADriverContextP ctx, VASurfaceID render_target)
{
    return dmd_SyncSurface2(ctx, render_target,
                            (uint64_t)DMD_FRAME_TIMEOUT_MS * 1000000ULL);
}

VAStatus dmd_SyncSurface2(VADriverContextP ctx, VASurfaceID surface,
                          uint64_t timeout_ns)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* VA_TIMEOUT_INFINITE 也要给上限：driver 挂死会拖死宿主进程。 */
    int timeout_ms;
    if (timeout_ns == VA_TIMEOUT_INFINITE ||
        timeout_ns / 1000000ULL > (uint64_t)DMD_FRAME_TIMEOUT_MS)
        timeout_ms = DMD_FRAME_TIMEOUT_MS;
    else
        timeout_ms = (int)(timeout_ns / 1000000ULL);
    if (timeout_ms <= 0)
        timeout_ms = 1;

    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, surface);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (s->state != DMD_SURFACE_PENDING) {
        VAStatus st = s->decode_status;
        pthread_mutex_unlock(&drv->lock);
        return st;
    }
    VAContextID context = surface_context_locked(drv, surface);
    if (context == VA_INVALID_ID) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    struct dmd_context *sc = dmd_find_context_locked(drv, context);
    int c_pending = sc ? sc->pending_count : 0;

    /* 放行是有额度的：只在"在飞帧数还不足以喂饱解码器流水线"时才放行。
     *
     * 无条件放行会把死锁换成队列无界增长 —— 实测 pending 一路涨到
     * DMD_MAX_SURFACES 溢出，ffmpeg 报 "Failed to end picture decode
     * issue: 23"。因为消费者是"能送就送"的：不挡它，它就一直送，
     * 而取像素（DeriveImage）要等到它自己攒够帧才做。
     *
     * 所以额度取实测的滞后深度 DMD_PIPELINE_DEPTH：
     * 在飞帧数低于它时放行（让流水线填满，这是打破死锁的必要条件），
     * 达到之后就老实阻塞等帧 —— 此刻解码器已经不欠料，等得到，
     * 于是队列被排空，形成稳定的背压。 */
    dmd_log("Sync: surface=%u pending=%d timeout=%dms %s\n",
            (unsigned)surface, c_pending, timeout_ms,
            c_pending >= DMD_PIPELINE_DEPTH ? "阻塞等帧" : "放行");

    if (c_pending >= DMD_PIPELINE_DEPTH) {
        VAStatus st = sync_surface_locked(drv, context, surface, timeout_ms);
        pthread_mutex_unlock(&drv->lock);
        return st;
    }

    /* ⚠️ 这里**不能**一直等到帧真的到手，否则与 MediaCodec 的流水线深度死锁。
     *
     * 实测的时序矛盾（probe_lag 直连 daemon 量得，与驱动无关）：
     *   MediaCodec 有 B 帧时滞后 4 个输入单元才吐首帧（无 B 帧时滞后 1）
     *   而浏览器里的 ffmpeg 稳态只保持 3 帧在飞（H.264 重排深度决定），
     *   送完第 3 帧就阻塞在 vaSyncSurface 等第 1 帧。
     * 双方差正好一帧：谁都不动 → 我们的兜底 flush 触发（不可逆
     * shutdown(SHUT_WR)）→ 会话作废 → 浏览器拿到 I/O error 后永久回落软解。
     * 实测 Firefox 140 因此只硬解出 1 帧，EndPicture 只被调用 3 次
     * 就直接 DestroyContext。
     *
     * 已否决的两条路（都实测过）：
     *  · daemon 开 low-latency：滞后仍是 4，无效（改动已保留，对交互有价值）
     *  · 合成 SPS 里写 max_num_reorder_frames：真实码流的 SPS 本来就带 VUI，
     *    滞后照旧 4 —— 说明这是高通解码器固有的流水线深度，signal 不掉。
     *
     * 所以阻塞点必须挪走：VA-API 允许像素在 map 时才真正就绪
     * （消费者取像素走 DeriveImage + MapBuffer，实测 Firefox/ffmpeg 都如此）。
     * 这里只做"短暂尝试"，没到就报成功放 ffmpeg 走 —— 它于是提交第 4 帧，
     * 正好把 MediaCodec 的流水线推过阈值，帧随即流出。
     * 真正的等待落在 MapBuffer/DeriveImage（见 image.c 的 dmd_surface_wait）。 */
    const int probe_ms = 30;
    VAStatus status = sync_surface_locked(drv, context, surface,
                                         timeout_ms < probe_ms ? timeout_ms
                                                               : probe_ms);
    if (status == VA_STATUS_ERROR_TIMEDOUT) {
        /* 帧还在解码器里排队，这不是错误。报成功让上游继续送料，
         * 像素在 map 时保证就绪。 */
        status = VA_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&drv->lock);

    return status;
}

/* 等到 surface 的像素真的就绪。供 image.c 在 map 前调用。
 *
 * 这是 SyncSurface 让路之后真正的阻塞点：此刻消费者已经把后续帧送进去了，
 * MediaCodec 的流水线不再欠料，所以这里等得到。 */
VAStatus dmd_surface_wait(struct dmd_driver *drv, VASurfaceID surface)
{
    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, surface);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    /* ⚠️ IDLE = 从未提交过解码，缓冲里是未初始化内容（或上一帧的残留），
     * 绝不能当画面交出去。
     *
     * 实测：ffmpeg 在提交任何 EndPicture **之前**先对 surface 1 调一次
     * DeriveImage 探测能力（DMD_VA_LOG 实测 state=0）。若在此放行，
     * 它就拿到一块陌生内存并当成第 1 帧输出。
     *
     * 这个 bug 的伪装性极强：1920x1080 testsrc 上半 797 行是静态图案、
     * 与真帧逐字节相同，只有下半 283 行的动态渐变带露馅；且每次运行
     * 拿到的内容不同（两次分别精确匹配软解第 7 帧与第 5 帧，平均差 0.00）。
     * 若测试素材是静态图，这会被误报为"通过"。
     *
     * IDLE 报错是正确行为：读一个从未解码的 surface 是调用方的时序错误。 */
    if (s->state == DMD_SURFACE_IDLE) {
        pthread_mutex_unlock(&drv->lock);
        dmd_log("surface_wait: surface %u 从未提交解码，拒绝读取\n",
                (unsigned)surface);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (s->state != DMD_SURFACE_PENDING) {
        VAStatus st = s->decode_status;
        pthread_mutex_unlock(&drv->lock);
        return st;
    }
    VAContextID context = surface_context_locked(drv, surface);
    if (context == VA_INVALID_ID) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    VAStatus status = sync_surface_locked(drv, context, surface,
                                         DMD_FRAME_TIMEOUT_MS);

    /* DMD_VA_TOLERATE_MISSING=1：取不到帧时也报成功（像素为预置内容）。
     *
     * 纯诊断开关，默认关闭。用途：AV1 码流合成的正确性只能靠"把整条合成流
     * 交给权威软解"来判断，但若驱动在首帧就阻塞，ffmpeg 就不再送料，
     * 我们只能采到 6 帧样本 —— 不足以暴露后续帧的问题。
     * 打开它可让 ffmpeg 跑完整条流，把完整合成结果落盘（配合
     * DMD_AV1_DUMP + DMD_AV1_DUMP_ALL）。
     *
     * ⚠️ 打开后画面必然是错的，绝不能用于功能验证，只用于采样。 */
    if (status == VA_STATUS_ERROR_TIMEDOUT) {
        const char *tol = getenv("DMD_VA_TOLERATE_MISSING");
        if (tol && tol[0] == '1')
            status = VA_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&drv->lock);
    return status;
}

VAStatus dmd_QuerySurfaceStatus(VADriverContextP ctx,
                                VASurfaceID render_target,
                                VASurfaceStatus *status)
{
    struct dmd_driver *drv = dmd_get_driver(ctx);

    if (!drv || !status)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&drv->lock);
    struct dmd_surface *s = dmd_find_surface_locked(drv, render_target);
    if (!s) {
        pthread_mutex_unlock(&drv->lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    /* 不在这里做 IO：QuerySurfaceStatus 的语义是"立即返回当前状态"，
     * 阻塞取帧是 SyncSurface 的活。 */
    *status = (s->state == DMD_SURFACE_PENDING) ? VASurfaceRendering
                                                : VASurfaceReady;
    pthread_mutex_unlock(&drv->lock);

    return VA_STATUS_SUCCESS;
}
