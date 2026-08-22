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
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* surface 分配在 msm_drm 的 dumb buffer 里，为的是能导出 dmabuf
 * （Firefox 取帧的唯一途径，见 export.c）。 */
#include <drm/drm.h>

#include "driver.h"

/* 前置声明：DestroyContext 需要排空在飞的帧，而这两个辅助定义在下方的
 * 解码路径小节里（放在那里更贴近使用现场）。 */
static VASurfaceID dmd_pending_take_locked(struct dmd_context *c);
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
    /* 阶段 1 固定 TCP：SHM 是后续优化，先把正确性做对。 */
    cfg.want_shm = 0;
    cfg.io_timeout_ms = DMD_FRAME_TIMEOUT_MS;

    memset(&err, 0, sizeof(err));
    struct dmd_session *s = dmd_session_create(&cfg, &err);
    if (!s)
        dmd_log("会话建立失败: code=%d handshake=%d %s\n", err.code,
                err.handshake_status, err.msg);
    else
        dmd_log("会话已建立: codec=%d %ux%u xfer=%d\n", codec, width, height,
                dmd_session_xfer_mode(s));
    return s;
}

/* flush 过的会话已不能再送数据，透明换一条新的。
 *
 * 为什么可以这么做（已实测，probe_rebuild）：
 * 新建会话 + 重送 SPS/PPS 后，**从非 IDR 帧续传也能立刻出帧**
 * （实测续传 12 个 VCL 出 9 帧，首个续传单元 nal_unit_type=1）。
 * 所以不必等下一个 IDR，不会有可见花屏。
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

            VASurfaceID id = dmd_pending_take_locked(c);
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
            /* VP8 的 key_frame / version / show_frame 相关位在这里。 */
            if (c->codec == DMD_CODEC_VP8 &&
                b->size >= sizeof(VAPictureParameterBufferVP8)) {
                memcpy(&c->vp8_pic_param, b->data,
                       sizeof(VAPictureParameterBufferVP8));
                c->have_vp8_pic_param = 1;
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

    case DMD_CODEC_HEVC:
        /* HEVC 同样只缺起始码（同一个 h2645 解析器），但参数集是三个
         * （VPS/SPS/PPS）且 profile_tier_level 里 VA-API 提供的字段更少，
         * 另需处理 conf_win 与 EPB 计数。先把 H.264 验通再做，
         * 避免同时引入两处不确定性。 */
        return NULL;

    default:
        return NULL;
    }
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

    n = dmd_h264_build_pps_nalu(pp, iq, have_iq, sp, nalu, sizeof(nalu));
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

    if (!unit) {
        struct dmd_surface *s = dmd_find_surface_locked(drv, target);
        if (s) {
            s->state = DMD_SURFACE_IDLE;
            s->decode_status = VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        c->current_target = VA_INVALID_ID;
        c->slice_len = 0;
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
     * 可行性已实测（probe_rebuild）：新会话重送 SPS/PPS 后，
     * 从非 IDR 帧续传也能立刻出帧，不必等下一个 IDR，因此不会有可见花屏。
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
            pthread_mutex_unlock(&drv->lock);
            free(scratch);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    /* 入队：**在放锁发送之前**入队，保证队列顺序严格等于提交顺序。
     * 若发送失败再出队回滚。 */
    if (c->pending_count >= DMD_MAX_SURFACES) {
        pthread_mutex_unlock(&drv->lock);
        free(scratch);
        dmd_log("EndPicture: 待解码队列已满（%d）\n", DMD_MAX_SURFACES);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    int qpos = (c->pending_head + c->pending_count) % DMD_MAX_SURFACES;
    c->pending[qpos] = target;
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

    if (codec == DMD_CODEC_H264 && c->have_h264_pic_param) {
        mbs_wide = (unsigned int)c->h264_pic_param.picture_width_in_mbs_minus1 + 1;
        mbs_high = (unsigned int)c->h264_pic_param.picture_height_in_mbs_minus1 + 1;
        /* 重发条件：首次、流内几何变化、或 PPS 的 num_ref_idx 默认值变化。
         *
         * 最后一条是必需的：PPS 默认值必须照抄**当前帧**的生效值
         * （详见 h264_bitstream.c 的说明），而不同帧的生效值会变。
         * MediaCodec 接受流中反复出现的 PPS，每次只多几字节。 */
        unsigned int l0 = c->have_h264_slice_param
                              ? c->h264_slice_param.num_ref_idx_l0_active_minus1
                              : 0;
        unsigned int l1 = c->have_h264_slice_param
                              ? c->h264_slice_param.num_ref_idx_l1_active_minus1
                              : 0;
        if (!c->param_sets_sent || c->sent_mbs_wide != mbs_wide ||
            c->sent_mbs_high != mbs_high || c->sent_l0 != l0 ||
            c->sent_l1 != l1) {
            c->pending_l0 = l0;
            c->pending_l1 = l1;
            need_param_sets = 1;
            pp_snap = c->h264_pic_param;
            iq_snap = c->h264_iq_matrix;
            have_iq_snap = c->have_h264_iq_matrix;
            sp_snap = c->h264_slice_param;
            have_sp_snap = c->have_h264_slice_param;
        }
    }

    drv->io_busy[idx] = 1;
    pthread_mutex_unlock(&drv->lock);

    int rc = DMD_OK;
    if (need_param_sets) {
        if (h264_send_param_sets(sess, &pp_snap, &iq_snap, have_iq_snap,
                                 have_sp_snap ? &sp_snap : NULL, profile_snap,
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
static VASurfaceID dmd_pending_take_locked(struct dmd_context *c)
{
    if (c->pending_count <= 0)
        return VA_INVALID_ID;

    int pick = c->pending_head;
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

    VASurfaceID head = c->pending[pick];
    dmd_log("配对: 帧 -> surface %u (seq %u POC %d, 队列剩 %d)\n",
            (unsigned)head, c->pending_seq[pick], (int)c->pending_poc[pick],
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
        k = prev;
    }
    c->pending_head = (c->pending_head + 1) % DMD_MAX_SURFACES;
    c->pending_count--;
    return head;
}

static void surface_store_frame_locked(struct dmd_surface *s,
                                       const struct dmd_frame *f)
{
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

    int spent = 0;
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
         * flush 的代价（会话作废）已由 EndPicture 的透明重建消掉：
         * 重送 SPS/PPS 后可从非 IDR 帧续传，上层察觉不到
         * （tools/probe_rebuild.c 验证过）。
         *
         * 队列够深时仍按耐心阈值等 —— 那种情况下帧确实在路上，
         * 提前 flush 会白白打断一个正常会话。 */
        int wait_is_futile = (c->pending_count < DMD_PIPELINE_DEPTH);

        if (!c->input_finished &&
            (wait_is_futile || spent >= flush_after_ms)) {
            struct dmd_session *fs = c->session;
            c->input_finished = 1;
            drv->io_busy[idx] = 1;
            pthread_mutex_unlock(&drv->lock);
            int frc = dmd_session_finish_input(fs);
            pthread_mutex_lock(&drv->lock);
            drv->io_busy[idx] = 0;
            pthread_cond_broadcast(&drv->io_done);
            dmd_log("SyncSurface: 等了 %d ms 仍无帧，flush 输入（rc=%d，"
                    "阈值 %d ms，队列 %d）\n",
                    spent, frc, flush_after_ms, c->pending_count);
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
            VASurfaceID head = dmd_pending_take_locked(c);
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
