/* DroidSpaces MediaCodec VA-API driver — 内部数据结构与公共声明
 *
 * 本驱动被 libva 以 dlopen 方式加载进消费者进程（vainfo / ffmpeg / Firefox），
 * 因此遵守插件规矩：不 exit/abort、不写 stdout、日志默认静默、全部入口线程安全。
 */

#ifndef DMD_DRIVER_H
#define DMD_DRIVER_H

#include <pthread.h>
#include <stdint.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_vp8.h>
#include <va/va_dec_vp9.h>
/* struct drm_state 定义在这里，va_backend.h 只做前向声明。 */
#include <va/va_drmcommon.h>

#include "dmd_client.h"
#include "stubs.h"

#include "av1_bitstream.h"

/* 驱动版本，随 vendor 串一起被消费者看到。
 * 注意 ffmpeg 按 vendor 串匹配 vaapi_driver_quirks 名单；我们不在名单内，
 * 走 standard behaviour，即要求语义标准。 */
/* ⚠️ 发版时必须同步：本串经 DMD_VENDOR_STRING 显示在 vainfo 的
 * "Driver version" 里，是用户可见信息。曾长期停在 0.3.3 而项目已到
 * 0.3.7（落后 4 版），用户按 vainfo 报 bug 时会指向错误的版本。
 * 同步清单：本文件、ksu-module/module.prop、CHANGELOG.md。 */
#define DMD_DRIVER_VERSION "0.3.7"
#define DMD_VENDOR_STRING "DroidSpaces MediaCodec VA-API driver " DMD_DRIVER_VERSION

/* 设备硬件解码能力，来自 /vendor/etc/media_codecs.xml（真机取证）。
 * H.264/HEVC 解码器规格：96x96 ~ 8192x4320，并发实例 16，支持 adaptive-playback。 */
#define DMD_MIN_WIDTH 96
#define DMD_MIN_HEIGHT 96
#define DMD_MAX_WIDTH 8192
#define DMD_MAX_HEIGHT 4320

/* libva 要求 max_* 字段全部 > 0（va/va.c 的 CHECK_MAXIMUM），
 * 且这些值是消费者分配查询数组的依据，必须 >= 我们实际返回的数量。 */
#define DMD_MAX_PROFILES 16
#define DMD_MAX_ENTRYPOINTS 4
#define DMD_MAX_CONFIG_ATTRIBUTES 32
#define DMD_MAX_IMAGE_FORMATS 4
#define DMD_MAX_SUBPIC_FORMATS 4
#define DMD_MAX_DISPLAY_ATTRIBUTES 4

/* 同时存活的 config 对象上限。VA-API 的 config 极轻量，
 * 消费者通常只建 1-2 个；给足余量即可。 */
#define DMD_MAX_CONFIGS 32

/* surface 上限：ffmpeg 的 hwframe pool 默认 20+ 个（initial_pool_size 加
 * 解码器的 DPB 需求），Firefox 会更多。64 对 8 个并发实例足够。 */
#define DMD_MAX_SURFACES 64

/* MediaCodec 的输出滞后深度（实测值，见 vaapi-driver/tools 里的 probe_lag）：
 * 有 B 帧的 H.264 流要送进第 4 个输入单元才吐出第 1 帧，无 B 帧时是 1。
 *
 * SyncSurface 用它作为"放行额度"：在飞帧数低于此值时不阻塞消费者，
 * 让它继续送料把流水线填满；达到之后才真正阻塞等帧。
 * 取 6（滞后 4 再留 2 的余量）—— 太小会退回死锁，太大会让队列变长、
 * 增加延迟且浪费 surface。
 *
 * ⚠️ 跨目录耦合（历史，socket 架构时期）：daemon 侧的 SHM_SLOTS 必须
 * >= 本值。历史事故：SHM_SLOTS=4 < 本值 6，第 5 帧必然撞池满，daemon 在
 * 1 秒后判死并杀掉会话 —— 4K + 慢消费者场景 10/10 丢帧（238/300）。
 * V4L2 直通架构下这条耦合消失了（没有 SHM 池），但约束换成了
 * V4L2 的 CAPTURE 缓冲数（DMD_V4L2_MAX_CAP，实测驱动给 24）。
 *
 * 取值 12 的依据（V4L2 直通实测）：msm_vidc 的输出滞后比 MediaCodec 深。
 * 原值 6 是按 MediaCodec 的实测滞后（有 B 帧时 4）定的，换到 V4L2 后
 * 表现为：ffmpeg 送 6 单元即停下等帧，而解码器此时只吐了 2 帧，
 * SyncSurface 等 2000ms 超时 → 触发排空 → 会话终结 → 4 帧待配对，
 * ffmpeg 报 internal decoding error。
 * 而直连后端实测送 10 单元可稳定解出 10 帧，故取 12（10 再留 2 余量），
 * 且 12 < 24 不会撞 CAPTURE 缓冲上限。 */
#define DMD_PIPELINE_DEPTH 12

/* 队列满时为腾空位收一帧的等待上限。
 *
 * 短一点：这条路径在 EndPicture 里（提交路径），等太久会拖慢提交。
 * 长一点：太短则腾不出空位、白白返回失败。
 * 200ms 足够 MediaCodec 在正常负载下吐出一帧（实测 1080p p95 约 10ms）。 */
#define DMD_BACKPRESSURE_MS 200

/* 等这么久（毫秒）仍取不到帧，才认定上游不再送料、执行不可逆的 flush。
 *
 * 必须是固定值，不能按调用方的超时推算：调用方的耐心不是"流已结束"的证据。
 * Firefox 用很短的超时轮询 vaSyncSurface，按比例推算会得到极小的阈值，
 * 才送 3 帧就误判流结束 → flush → 会话作废 → 永久回落软解。 */
#define DMD_FLUSH_AFTER_MS 2000
/* context 上限对齐 media_codecs.xml 声明的并发解码实例数（16）。 */
#define DMD_MAX_CONTEXTS 16
/* buffer 上限：每帧 4 个左右（pic param / IQ matrix / slice param / slice data），
 * ffmpeg 在 EndPicture 后才 Destroy，同时存活量与 DPB 深度相关。 */
#define DMD_MAX_BUFFERS 256
/* image 上限：ffmpeg 每次 map 建一个、unmap 即销毁，同时存活量很小。 */
#define DMD_MAX_IMAGES 64

/* 缓冲几何对齐：Venus 解码器输出宽对齐 128、高对齐 32
 * （1080p → 1920x1088，真机取证）。surface 按此预分配，
 * 保证 daemon 回来的帧一定装得下，不必等到收到格式块才分配。 */
#define DMD_WIDTH_ALIGN 128
#define DMD_HEIGHT_ALIGN 32

/* 等一帧从 daemon 回来的上限。driver 跑在宿主进程里，绝不允许无限等：
 * 超时返回错误让消费者自己决定重试或放弃，比挂死整个进程好。
 *
 * ⚠️ 跨目录耦合：daemon 侧的 SHM_SLOT_WAIT_MS（src/decode-daemon.c）
 * 必须 > 本值。本值是客户端愿意等一帧的时长，daemon 等空闲槽位的上限
 * 若比它短，daemon 就会在客户端还愿意等的时候先放弃并杀掉会话。
 * 历史事故：daemon 只等 1000ms < 本值 5000ms，把本可正常完成的解码
 * 变成丢帧。调大本值时必须同步检查 SHM_SLOT_WAIT_MS。 */
#define DMD_FRAME_TIMEOUT_MS 5000

/* 一个 surface：VA-API 的解码目标，持有一块 NV12 缓冲。
 *
 * 数据流：EndPicture 把 slice data 送给 daemon 并把 surface 挂进 context 的
 * 待解码队列；SyncSurface 从 daemon 取帧、按 FIFO 顺序填进队首 surface 的缓冲。
 */
struct dmd_surface {
    int in_use;
    VASurfaceID id;
    /* 请求尺寸（显示尺寸，ffmpeg 传 1920x1080）。VAImage 以此为准。 */
    unsigned int width;
    unsigned int height;
    /* 缓冲几何：宽对齐 128、高对齐 32 后的值，data 按此分配。 */
    unsigned int buf_width;
    unsigned int buf_height;
    unsigned int stride;
    unsigned int slice_height;
    unsigned int format; /* VA_RT_FORMAT_* */
    unsigned char *data;
    size_t data_size;
    /* dumb buffer 后备存储：exportable 非 0 表示 data 是 mmap 出来的可导出
     * 内存，可通过 DRM_IOCTL_PRIME_HANDLE_TO_FD 导出 dmabuf fd。
     * Firefox 取帧走 vaExportSurfaceHandle，没有它就整条流回落软解；
     * 分配失败时回落普通 heap（exportable=0），ffmpeg 的 hwdownload 仍可用。 */
    uint32_t dumb_handle;
    size_t dumb_size;
    int exportable;
    /* 释放 dumb buffer 需要当初分配它的 drm fd。存在 surface 上而不是让
     * 释放函数多收一个参数 —— 这样不可能传错 fd。 */
    int dumb_drm_fd;
    /* 解码状态：0=空闲（VASurfaceReady），1=已提交待解码（VASurfaceRendering），
     * 2=帧已就绪（VASurfaceReady 且 data 有效）。 */
    int state;
    /* 该 surface 上一次解码的结果，SyncSurface 返回它。 */
    VAStatus decode_status;
    VAContextID context; /* 提交时所属 context，未提交为 VA_INVALID_ID */
};

#define DMD_SURFACE_IDLE 0
#define DMD_SURFACE_PENDING 1
#define DMD_SURFACE_READY 2

/* 一个 buffer：VA-API 参数/码流缓冲。 */
struct dmd_buffer {
    int in_use;
    VABufferID id;
    VAContextID context;
    VABufferType type;
    unsigned int element_size;
    unsigned int num_elements;
    size_t size; /* element_size * num_elements */
    void *data;
    int mapped;
};

/* 一个 image：VAImage 的后备存储。data 是普通 heap 内存
 * （容器内 ION/dma_heap 均不可用，dmabuf 零拷贝这条路已被否掉）。 */
struct dmd_image {
    int in_use;
    VAImageID id;
    VABufferID buf_id; /* 供 vaMapBuffer 用的伪 buffer ID */
    VAImage image;
    unsigned char *data;
    size_t data_size;
    int mapped;
    /* derive 出来的 image 绑定在某个 surface 上；CreateImage 的为 VA_INVALID_ID */
    VASurfaceID derived_from;
};

/* 一个 context：config + render target 集合 + 一条 daemon 会话。
 *
 * 待解码队列 pending[] 是 VA-API 乱序语义与 MediaCodec 流式语义之间的桥。
 *
 * ⚠️ 配对规则按 codec 分两种，因为两侧的顺序不一定相同：
 *
 * - VP9/VP8（无帧重排）：ffmpeg 的提交顺序 == daemon 的出帧顺序，
 *   直接 FIFO 配对（取队首）。实测严格 1 进 1 出。
 *
 * - H.264/HEVC（有 B 帧重排）：ffmpeg 按**解码顺序**调 EndPicture，
 *   而 MediaCodec 按**显示顺序**吐帧 —— 两者不同序！
 *   本流实测解码序 I P B B B...（P 在第 2 位，因为 B 要参考它），
 *   显示序 I B B B P...。若按 FIFO 配对，第 2 个提交（P 的 surface）
 *   会被填进第 2 个输出（B 的内容），从第 2 帧起全错。
 *   所以必须按 POC（显示顺序）配对：daemon 的第 k 帧给 POC 第 k 小的 surface。
 *   POC 取自 VAPictureParameterBufferH264.CurrPic.TopFieldOrderCnt，
 *   ffmpeg 传的是已解包的 field_poc[0]（vaapi_h264.c:74），
 *   不是码流里 6 bit 会回绕的 poc_lsb，故可直接比较大小。
 */
struct dmd_context {
    int in_use;
    VAContextID id;
    VAConfigID config_id;
    VAProfile profile;
    int codec; /* DMD_CODEC_* */
    unsigned int picture_width;
    unsigned int picture_height;
    int flag;

    /* daemon 会话。惰性创建：CreateContext 时建，失败则留 NULL 由
     * EndPicture 重试 —— 建 context 时 daemon 不可用不该让整个初始化失败。 */
    struct dmd_session *session;
    int session_failed; /* 建会话失败过，避免每帧重试拖慢失败路径 */

    /* 待解码队列：EndPicture 提交的 surface。
     * VP9/VP8 取队首；H.264/HEVC 取 pending_poc 最小者（见结构体头注释）。 */
    VASurfaceID pending[DMD_MAX_SURFACES];
    int32_t pending_poc[DMD_MAX_SURFACES];
    /* 所属 coded video sequence 序号：POC 每逢 IDR 会重置，跨序列比 POC
     * 大小没有意义，所以排序时先比 seq 再比 POC。 */
    unsigned int pending_seq[DMD_MAX_SURFACES];
    /* 该 surface 对应的**提交序号**（第几次向 daemon 送数据单元，1 起）。
     * daemon 把它作为 PTS 原样回传到帧上（dmd_frame.unit_seq），
     * 于是配对可以精确匹配，而不必假设解码器的出帧顺序。 */
    uint64_t pending_unit[DMD_MAX_SURFACES];
    int pending_head;
    int pending_count;
    /* 已提交的数据单元计数，给 pending_unit 发号。 */
    uint64_t units_submitted;
    /* daemon 是否回传 unit_seq（运行时从帧里观测，非编译期假设）。
     * 有它就能精确配对，且说明 daemon 已开跟随输入序输出（滞后 1）。 */
    int daemon_has_unit_seq;
    /* 本帧的 POC，RenderPicture 从 pic param 取、EndPicture 随 surface 入队。
     * 仅 H.264/HEVC 有意义。 */
    int32_t current_poc;
    int have_current_poc;
    /* frame_num 在每个 IDR 处归零（规范 7.4.3），用它检测新序列 ——
     * 不能用 POC 比较，因为解码序内 POC 本来就起伏。 */
    unsigned int current_frame_num;
    int32_t last_poc;
    unsigned int last_frame_num;
    int have_last_poc;
    unsigned int last_seq;

    /* 当前 BeginPicture 选定的目标 surface，EndPicture 用完清空 */
    VASurfaceID current_target;
    /* 本帧累积的码流数据（RenderPicture 可能分多次给 slice data） */
    unsigned char *slice_data;
    size_t slice_len;
    size_t slice_cap;
    /* 本帧的 VP8 slice 参数，合成 uncompressed chunk 需要 */
    int have_vp8_slice_param;
    VASliceParameterBufferVP8 vp8_slice_param;
    int have_vp8_pic_param;
    VAPictureParameterBufferVP8 vp8_pic_param;

    /* AV1：合成 OBU 需要两样东西 ——
     *   1) pic param 的结构化字段：序列头与帧头**只存在于这里**，
     *      VASliceDataBuffer 里仅有 tile 载荷、不含任何 OBU 封装
     *      （va_dec_av1.h:643-645 明确说明码流按 per-tile 粒度传入）
     *   2) 每个 tile 在码流缓冲里的偏移与长度：tile_group 必须逐 tile 写
     *      tile_size_minus_1，而 slice_data 是连成一片累积的。
     *
     * ⚠️ 边界来源必须是 VASliceParameterBufferAV1，**不能**数
     * VASliceDataBuffer 的追加次数。实机实测：ffmpeg 每帧只调一次
     * vaRenderPicture 送 slice data，却在同一帧声明 8 个 tile ——
     * 即整帧 tile 打包在一个 buffer 里，靠追加次数只会得到 1。
     * 正确来源是每 tile 一份的 slice param（va_dec_av1.h:635 "should be
     * sent once per tile"），其 slice_data_size/slice_data_offset 分别是
     * tile 字节数与在缓冲内的偏移（:649-658，注释明确说 slice_data_size
     * "actually means tile_data_size"）。
     *
     * 512 项足以覆盖 8K/多 tile 场景（规范 MAX_TILE_COLS×MAX_TILE_ROWS
     * 名义上 64×64，实际受 MAX_TILE_AREA 约束远小于此）。 */
    int have_av1_pic_param;
    VADecPictureParameterBufferAV1 av1_pic_param;
    VASliceParameterBufferAV1 av1_tile_param[512];
    int    av1_tile_count;

    /* AV1 自洽 DPB：refresh_frame_flags 与 ref_frame_idx 都由我们写入码流，
     * 而 VA-API 不提供前者（那是编码器的 GOP 决策，随源码流被丢弃）。
     * 故自己维护一套槽位分配，保证写入码流的两者互相自洽。
     * 详见 av1_bitstream.h 的 struct dmd_av1_dpb 说明。 */
    struct dmd_av1_dpb av1_dpb;

    /* AV1 延迟一帧送料所需的暂存。
     *
     * 为什么必须延迟：refresh_frame_flags 的正确值等于"下一帧的
     * ref_frame_map 与本帧的差异位"（实测 4 帧全部命中源码流真实值），
     * 所以本帧合成完还不能立刻送走 —— 要等下一帧到来、反算出正确值、
     * 就地改写那 8 位之后才能送。
     *
     * av1_hold 持有已合成但尚未送出的上一帧字节（本模块 malloc，
     * 送出后 free）。av1_hold_len 是其长度。 */
    unsigned char *av1_hold;
    size_t         av1_hold_len;
    /* 暂存帧的 refresh_frame_flags 位偏移。
     * ⚠️ 不能复用 dpb.last_refresh_bitpos —— 那个字段会被**本帧**的
     * build_frame 覆盖。实测踩过：日志显示 "bitpos=19 len=655"，
     * 而 len=655 是新合成帧的长度，暂存帧长 2686，一眼即知拿错了帧。 */
    size_t         av1_hold_bitpos;
    /* AV1 延迟一帧送料，EndPicture 里送出的是**上一帧**的数据，
     * 所以待配对队列必须登记那一帧的 surface 而不是当前 surface。
     * 实测不这样做的后果：回传 unit_seq 在队列里找不到对应项
     * （"unit_seq=6 无匹配项，回退顺序推断"），帧被配到错误的 surface，
     * ffmpeg 等不到它要的那个 → 超时 → flush → 整段码流只出 1 帧。
     *   av1_hold_surface：当前压在 av1_hold 里那一帧的 surface
     *   av1_send_surface：build_unit 本次实际送出的那一帧的 surface */
    VASurfaceID    av1_hold_surface;
    VASurfaceID    av1_send_surface;
    /* 最近一个拿到真实像素的 surface，供 show_frame=0 的空壳承接像素。 */
    VASurfaceID    av1_last_ready;

    /* ---- show_existing_frame 补插 ----
     * 硬件不输出 show_frame=0 的帧（符合规范），源码流靠 70 个 SEF 头
     * 让它们各复显一次，凑满 150 帧。缺 SEF 头时实测只收 80 帧。
     *
     * 位置规律（150 帧样本，100% 成立）：
     *   全部 70 个 SEF 都紧跟在一个 show_frame=1 的帧之后
     *   80 个 show=1 的帧里有 70 个带 SEF
     * 引用规律：SEF 的 map_idx 指向的槽，正是某个 show_frame=0 帧
     *   最后刷新的那个槽。
     *
     * 于是驱动这样推导：show_frame=0 的帧进队（记下它占的 DPB 槽），
     * 遇到 show_frame=1 的帧送出后，从队首取一个补一个 SEF 头。 */
    /* 待复显帧所占的 DPB 槽，环形。容量取 64：实测 8 太小 ——
     * 金字塔 B 结构下 show_frame=0 的帧会连续到来，
     * 150 帧样本里队列长期处于满状态，入队被丢弃了 58 次
     * （只入队 12 次，而 show=0 帧有 70 个）。 */
    unsigned       av1_sef_slot[64];
    int            av1_sef_head;
    int            av1_sef_count;
    /* 无条件计数器。⚠️ 不要用日志行数推断执行次数 ——
     * 本会话已两次因此误判（第 54 轮把 220 行当 220 次调用，
     * 第 59 轮把 75 条日志当 75 次执行）。 */
    unsigned long  av1_sef_visits;   /* 取槽点被执行的次数 */
    unsigned long  av1_sef_show1;    /* 其中 send_show 为 1 的次数 */
    unsigned long  av1_sef_enq;      /* 入队次数 */
    unsigned long  av1_sef_drop;     /* 队列满而丢弃的次数 */
    unsigned long  av1_sef_sent;     /* 实际追加的 SEF 单元数 */
    /* 配套记录 show_frame：show_frame=0 的帧不产生输出（解码器实测只对
     * show_frame=1 的帧吐 CAPTURE 缓冲），不能为它登记待配对项，
     * 否则队列里多出永远配不上的条目。 */
    int            av1_hold_show;
    int            av1_send_show;

    /* H.264：VA-API 从不传递参数集（SPS/PPS 被解析成字段后原始比特流就丢了），
     * 所以要从 pic param 反向合成，并在首个 VCL 之前发给 daemon。 */
    int have_h264_pic_param;
    VAPictureParameterBufferH264 h264_pic_param;
    /* HEVC 图像参数：合成 VPS/SPS/PPS 用。与 h264 那份互斥使用。 */
    /* PPS 的 num_ref_idx 默认值推导：l0 取见过的最大、l1 取见过的最小。
     * 见 decode.c 的说明 —— 直接照抄每帧生效值会写出错误的 PPS 默认值。 */
    unsigned int refidx_l0_max;
    unsigned int refidx_l1_min;
    int refidx_seen;

    VAPictureParameterBufferHEVC hevc_pic_param;
    int hevc_pic_param_valid;
    int have_h264_slice_param;
    VASliceParameterBufferH264 h264_slice_param;
    int have_h264_iq_matrix;
    VAIQMatrixBufferH264 h264_iq_matrix;
    int param_sets_sent;
    unsigned int sent_mbs_wide;
    unsigned int sent_mbs_high;
    /* 上次随 PPS 发出的 num_ref_idx 默认值。它必须跟随当前帧的生效值，
     * 变化时要重发 PPS —— l1 偏大会改变 ref_idx_l1 的熵解码码长。 */
    unsigned int sent_l0;
    unsigned int sent_l1;
    unsigned int pending_l0;
    unsigned int pending_l1;
    /* 已 shutdown(SHUT_WR)：不能再发送，也不必重复 flush */
    int input_finished;
};

/* 一个 VA-API config 对象：profile + entrypoint + 属性集合 */
struct dmd_config {
    int in_use;
    VAConfigID id;
    VAProfile profile;
    VAEntrypoint entrypoint;
    VAConfigAttrib attribs[DMD_MAX_CONFIG_ATTRIBUTES];
    int num_attribs;
};

/* 驱动私有数据，挂在 ctx->pDriverData
 *
 * 锁策略：单把 lock 保护所有对象表。这不是性能瓶颈 —— 表操作都是 O(表长)
 * 的内存操作。**但阻塞 IO 绝不能持锁做**：与 daemon 的收发在放锁后进行，
 * 靠 context 的 busy 标志串行化同一 context 上的 IO。
 */
struct dmd_driver {
    pthread_mutex_t lock;
    /* IO 完成时广播，等 context 的 busy 标志用 */
    pthread_cond_t io_done;

    struct dmd_config configs[DMD_MAX_CONFIGS];
    struct dmd_surface surfaces[DMD_MAX_SURFACES];
    struct dmd_context contexts[DMD_MAX_CONTEXTS];
    struct dmd_buffer buffers[DMD_MAX_BUFFERS];
    struct dmd_image images[DMD_MAX_IMAGES];

    unsigned int next_config_id;
    unsigned int next_surface_id;
    unsigned int next_context_id;
    unsigned int next_buffer_id;
    unsigned int next_image_id;

    /* 某个 context 正在做 daemon IO（放锁期间的互斥标志）。
     * 用数组下标而非 ID，查找更快。 */
    int io_busy[DMD_MAX_CONTEXTS];

    int drm_fd; /* 来自 ctx->drm_state，仅记录，当前不做 ioctl */
};

/* 从 VADriverContext 取私有数据；ctx 或 pDriverData 为空时返回 NULL。 */
struct dmd_driver *dmd_get_driver(VADriverContextP ctx);

/* 日志：默认静默，设 DMD_VA_LOG=1 后输出到 stderr。
 * 绝不写 stdout —— 那会污染宿主进程的输出。 */
void dmd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- 已实现的 vtable 入口（其余在自动生成的 stubs.c 中） ---- */

VAStatus dmd_Terminate(VADriverContextP ctx);

VAStatus dmd_QueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list,
                                 int *num_profiles);

VAStatus dmd_QueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile,
                                    VAEntrypoint *entrypoint_list,
                                    int *num_entrypoints);

VAStatus dmd_GetConfigAttributes(VADriverContextP ctx, VAProfile profile,
                                 VAEntrypoint entrypoint,
                                 VAConfigAttrib *attrib_list, int num_attribs);

VAStatus dmd_CreateConfig(VADriverContextP ctx, VAProfile profile,
                          VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
                          int num_attribs, VAConfigID *config_id);

VAStatus dmd_DestroyConfig(VADriverContextP ctx, VAConfigID config_id);

VAStatus dmd_QueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id,
                                   VAProfile *profile, VAEntrypoint *entrypoint,
                                   VAConfigAttrib *attrib_list,
                                   int *num_attribs);

VAStatus dmd_QuerySurfaceAttributes(VADriverContextP ctx, VAConfigID config,
                                    VASurfaceAttrib *attrib_list,
                                    unsigned int *num_attribs);

VAStatus dmd_QueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list,
                               int *num_formats);

VAStatus dmd_QuerySubpictureFormats(VADriverContextP ctx,
                                    VAImageFormat *format_list,
                                    unsigned int *flags,
                                    unsigned int *num_formats);

VAStatus dmd_QueryDisplayAttributes(VADriverContextP ctx,
                                    VADisplayAttribute *attr_list,
                                    int *num_attributes);

VAStatus dmd_GetDisplayAttributes(VADriverContextP ctx,
                                  VADisplayAttribute *attr_list,
                                  int num_attributes);

VAStatus dmd_SetDisplayAttributes(VADriverContextP ctx,
                                  VADisplayAttribute *attr_list,
                                  int num_attributes);

/* ---- decode.c：解码数据路径 ---- */

VAStatus dmd_CreateSurfaces(VADriverContextP ctx, int width, int height,
                            int format, int num_surfaces,
                            VASurfaceID *surfaces);

VAStatus dmd_CreateSurfaces2(VADriverContextP ctx, unsigned int format,
                             unsigned int width, unsigned int height,
                             VASurfaceID *surfaces, unsigned int num_surfaces,
                             VASurfaceAttrib *attrib_list,
                             unsigned int num_attribs);

VAStatus dmd_DestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list,
                             int num_surfaces);

VAStatus dmd_CreateContext(VADriverContextP ctx, VAConfigID config_id,
                           int picture_width, int picture_height, int flag,
                           VASurfaceID *render_targets, int num_render_targets,
                           VAContextID *context);

VAStatus dmd_DestroyContext(VADriverContextP ctx, VAContextID context);

VAStatus dmd_CreateBuffer(VADriverContextP ctx, VAContextID context,
                          VABufferType type, unsigned int size,
                          unsigned int num_elements, void *data,
                          VABufferID *buf_id);

VAStatus dmd_BufferSetNumElements(VADriverContextP ctx, VABufferID buf_id,
                                  unsigned int num_elements);

VAStatus dmd_MapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf);

VAStatus dmd_MapBuffer2(VADriverContextP ctx, VABufferID buf_id, void **pbuf,
                        uint32_t flags);

VAStatus dmd_UnmapBuffer(VADriverContextP ctx, VABufferID buf_id);

VAStatus dmd_DestroyBuffer(VADriverContextP ctx, VABufferID buffer_id);

VAStatus dmd_BufferInfo(VADriverContextP ctx, VABufferID buf_id,
                        VABufferType *type, unsigned int *size,
                        unsigned int *num_elements);

VAStatus dmd_BeginPicture(VADriverContextP ctx, VAContextID context,
                          VASurfaceID render_target);

VAStatus dmd_RenderPicture(VADriverContextP ctx, VAContextID context,
                           VABufferID *buffers, int num_buffers);

VAStatus dmd_EndPicture(VADriverContextP ctx, VAContextID context);

VAStatus dmd_SyncSurface(VADriverContextP ctx, VASurfaceID render_target);

VAStatus dmd_SyncSurface2(VADriverContextP ctx, VASurfaceID surface,
                          uint64_t timeout_ns);

/* ---- export.c：dmabuf 出口 ---- */

/* 把 surface 导出成 dmabuf（DRM_PRIME_2）。
 *
 * Firefox 取帧只走这条路：CreateImageVAAPI 拿不到 DRM_PRIME_2 描述符就
 * 返回 DECODE_ERR，播放器随即回落软解，且没有拷贝回退路径。
 * ffmpeg 命令行不需要它（hwdownload 走 vaDeriveImage + vaMapBuffer）。 */
VAStatus dmd_ExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id,
                                 uint32_t mem_type, uint32_t flags,
                                 void *descriptor);

/* 等到 surface 像素真的就绪（内部自行加锁，调用方不得持锁）。
 *
 * SyncSurface 为避免与 MediaCodec 的流水线深度死锁，会对"已提交但未就绪"
 * 的 surface 直接报成功放行，真正的等待推迟到取像素时。所以 image.c 的
 * DeriveImage / GetImage 在读 surface 几何或拷像素之前必须先调用本函数。 */
VAStatus dmd_surface_wait(struct dmd_driver *drv, VASurfaceID surface);

VAStatus dmd_QuerySurfaceStatus(VADriverContextP ctx,
                                VASurfaceID render_target,
                                VASurfaceStatus *status);

/* ---- image.c：VAImage 出口 ---- */

VAStatus dmd_CreateImage(VADriverContextP ctx, VAImageFormat *format,
                         int width, int height, VAImage *image);

VAStatus dmd_DeriveImage(VADriverContextP ctx, VASurfaceID surface,
                         VAImage *image);

VAStatus dmd_DestroyImage(VADriverContextP ctx, VAImageID image);

VAStatus dmd_GetImage(VADriverContextP ctx, VASurfaceID surface, int x, int y,
                      unsigned int width, unsigned int height, VAImageID image);

/* ---- 跨文件内部辅助（均要求调用方已持 drv->lock） ---- */

struct dmd_surface *dmd_find_surface_locked(struct dmd_driver *drv,
                                            VASurfaceID id);
struct dmd_context *dmd_find_context_locked(struct dmd_driver *drv,
                                            VAContextID id);
struct dmd_buffer *dmd_find_buffer_locked(struct dmd_driver *drv,
                                          VABufferID id);
struct dmd_image *dmd_find_image_locked(struct dmd_driver *drv, VAImageID id);

/* 隐式同步一个 surface（GetImage 在 surface 未就绪时用）。
 * 要求调用方已持锁；内部会临时放锁做 IO，返回时仍持锁 ——
 * 调用方必须重新查找所有缓存的对象指针。 */
VAStatus dmd_surface_sync_locked(struct dmd_driver *drv, VASurfaceID surface,
                                 int timeout_ms);

/* 释放一个 surface 槽位持有的资源（不加锁）。 */
void dmd_surface_reset_locked(struct dmd_surface *s);
/* 释放一个 context 槽位持有的资源，含 daemon 会话（不加锁；会做 socket 关闭）。 */
void dmd_context_reset_locked(struct dmd_context *c);

/* 按 surface 几何填一个 VAImage 描述（不含 image_id/buf）。
 * 这是 1088-vs-1080 那处几何的唯一真源，derive 与 create 共用。 */
void dmd_fill_image_geometry(VAImage *img, unsigned int disp_width,
                             unsigned int disp_height, unsigned int stride,
                             unsigned int slice_height);

/* 对齐辅助 */
unsigned int dmd_align_up(unsigned int v, unsigned int align);

/* ---- profiles.c 内部辅助 ---- */

/* 该 profile 是否由本驱动支持（即 Android 侧 MediaCodec 有对应硬件解码器）。 */
int dmd_profile_supported(VAProfile profile);

/* profile → 协议 codec id（DMD_CODEC_*）。不支持的 profile 返回 -1。 */
int dmd_profile_to_codec(VAProfile profile);

/* ---- h264_bitstream.c ---- */

/* 从 VA-API 的 pic param 反向合成带 4 字节起始码的 SPS / PPS NALU。
 * VA-API 从不传递参数集原始比特流，而 daemon 靠起始码识别 NAL 类型把它们
 * 累积成 codec-specific data，所以必须自己写出来。
 * 返回写入 out 的字节数，0 表示失败（含 out_cap 不足）。 */
/* HEVC 参数集合成（hevc_bitstream.c）。
 *
 * dmd_hevc_can_build 返回 0 表示这个码流无法合成（VA-API 缺少必要信息），
 * 调用方应让上层回落软解，而不是产出坏画面。 */
int    dmd_hevc_can_build(const VAPictureParameterBufferHEVC *pp);
size_t dmd_hevc_build_vps_nalu(const VAPictureParameterBufferHEVC *pp,
                               VAProfile profile,
                               unsigned char *out, size_t cap);
size_t dmd_hevc_build_sps_nalu(const VAPictureParameterBufferHEVC *pp,
                               VAProfile profile,
                               unsigned char *out, size_t cap);
size_t dmd_hevc_build_pps_nalu(const VAPictureParameterBufferHEVC *pp,
                               unsigned char *out, size_t cap);

size_t dmd_h264_build_sps_nalu(const VAPictureParameterBufferH264 *pp,
                               VAProfile profile, unsigned int disp_width,
                               unsigned int disp_height, unsigned char *out,
                               size_t out_cap);
size_t dmd_h264_build_pps_nalu(const VAPictureParameterBufferH264 *pp,
                               const VAIQMatrixBufferH264 *iq, int have_iq,
                               const VASliceParameterBufferH264 *sp,
                               unsigned int def_l0_minus1,
                               unsigned int def_l1_minus1,
                               unsigned char *out, size_t out_cap);

#endif /* DMD_DRIVER_H */
