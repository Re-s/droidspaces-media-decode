/*
 * dmd_v4l2_session —— dmd_client.h 接口的 V4L2 直通实现。
 *
 * 取代原先的 socket + decode-daemon 链路：驱动进程自己打开
 * /dev/video32（msm_vidc_driver）解码，不再需要跨进程通信。
 *
 * 为什么能这么做（实测推翻了早先的判断）：容器内 /dev/video32 与
 * /dev/dma_heap/system 都属 droidspaces-gpu 组，容器用户在该组内，
 * **无需 root** 即可直接解码。实测容器内无 root 解 AV1 得 10 帧，
 * 四个 codec 全部可用，三个并发实例互不干扰。
 *
 * 于是 daemon 存在的唯一理由（跨进程访问 Android MediaCodec）消失了。
 * 去掉它省下：socket 往返、每帧一次全画面 memcpy（1080p 是 3MB）、
 * SHM 槽位池与归还协议、握手与版本协商、daemon 进程与 watchdog。
 *
 * 刻意保持 dmd_client.h 的接口不变 —— decode.c 一行都不用改，
 * 它看到的仍是 create/send_unit/next_frame/release_frame/drain/destroy。
 * 这样这次改造对上层是透明的，也便于对照排查。
 *
 * 与 daemon 版的语义差异（已在各函数处注明）：
 * - dmd_frame.data 直接指向 V4L2 的 dmabuf 映射，不再是 socket 收进来的
 *   拷贝。release_frame 之前不能覆写，release 之后立刻失效。
 * - shm_slot 字段复用为 CAPTURE 缓冲索引（>=0），语义仍是"归还凭据"。
 * - unit_seq 由本实现通过 V4L2 timestamp 透传，规则与 daemon 版一致
 *   （送料时写 seq*1000 微秒，收帧时除回来）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "dmd_client.h"
#include "v4l2_backend.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 与 dmd_client.c 保持一致的日志开关：只有字面 "1" 打开。 */
static int v4l2_log_wanted(void)
{
    const char *e = getenv("DMD_VA_LOG");
    return e && e[0] == '1';
}

struct dmd_session {
    struct dmd_v4l2_dec dec;
    int    log;
    int    codec;
    int    io_timeout_ms;

    struct dmd_format fmt;
    struct dmd_error  err;

    uint64_t units_sent;
    uint64_t frames_recv;
    uint64_t frame_seq;

    int  input_finished;
    int  eos;

    /* 待取帧队列。
     *
     * 为什么需要它：daemon 版是 input/output 双线程并发，送料与收帧互不
     * 阻塞。驱动内直通是**单线程** —— 调用序列由 libva 决定，通常是
     * 送若干单元后才来取一帧。当输入缓冲满而调用方还没来取帧时，必须把
     * 已解出的帧先接住，否则只能丢帧（毁掉参考帧链）或报错（实测表现为
     * 送 6 单元只回 2 帧，4 帧待配对，ffmpeg 报 internal decoding error）。
     *
     * 容量取 CAPTURE 缓冲数：解码器最多同时持有这么多帧，不会溢出。 */
    struct {
        uint8_t *data;
        size_t   len;
        uint64_t pts;
        int      index;
    } pend[DMD_V4L2_MAX_CAP];
    int  pend_head, pend_tail, pend_count;

    /* 已交给调用方、尚未 release 的缓冲索引集合。
     * 用位图而非单个变量：队列化之后可能同时有多帧在调用方手里。 */
    unsigned char held[DMD_V4L2_MAX_CAP];
};

static void publish_format(struct dmd_session *s);

/* 把一帧放进待取队列。返回 0 成功，-1 队列满（不应发生）。 */
static int pend_push(struct dmd_session *s, uint8_t *data, size_t len,
                     uint64_t pts, int index)
{
    if (s->pend_count >= DMD_V4L2_MAX_CAP) return -1;
    s->pend[s->pend_tail].data  = data;
    s->pend[s->pend_tail].len   = len;
    s->pend[s->pend_tail].pts   = pts;
    s->pend[s->pend_tail].index = index;
    s->pend_tail = (s->pend_tail + 1) % DMD_V4L2_MAX_CAP;
    s->pend_count++;
    return 0;
}

/* 从待取队列取一帧。返回 1 有帧，0 队列空。 */
static int pend_pop(struct dmd_session *s, uint8_t **data, size_t *len,
                    uint64_t *pts, int *index)
{
    if (s->pend_count <= 0) return 0;
    *data  = s->pend[s->pend_head].data;
    *len   = s->pend[s->pend_head].len;
    *pts   = s->pend[s->pend_head].pts;
    *index = s->pend[s->pend_head].index;
    s->pend_head = (s->pend_head + 1) % DMD_V4L2_MAX_CAP;
    s->pend_count--;
    return 1;
}

static void sess_log(struct dmd_session *s, const char *fmt, ...)
{
    if (!s || !s->log) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[dmd-v4l2] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void set_err(struct dmd_session *s, int code, const char *msg)
{
    if (!s) return;
    s->err.code = code;
    snprintf(s->err.msg, sizeof(s->err.msg), "%s", msg ? msg : "");
}

/* ---------------------------------------------------------------- 配置 */

void dmd_session_config_init(struct dmd_session_config *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    /* port / sock_path / want_shm 在 V4L2 直通下已无意义，保留字段是为了
     * 不破坏 dmd_client.h 的 ABI 与调用方代码；本实现一律忽略它们。 */
    cfg->codec = DMD_CODEC_H264;
    cfg->connect_timeout_ms = 2000;
    cfg->io_timeout_ms = 5000;
}

int dmd_format_display_width(const struct dmd_format *fmt)
{
    if (!fmt || !fmt->valid) return 0;
    if (fmt->crop_right > fmt->crop_left)
        return fmt->crop_right - fmt->crop_left + 1;
    return fmt->buf_width;
}

int dmd_format_display_height(const struct dmd_format *fmt)
{
    if (!fmt || !fmt->valid) return 0;
    if (fmt->crop_bottom > fmt->crop_top)
        return fmt->crop_bottom - fmt->crop_top + 1;
    return fmt->buf_height;
}

/* ------------------------------------------------------------ 会话生命周期 */

struct dmd_session *dmd_session_create(const struct dmd_session_config *cfg,
                                       struct dmd_error *err)
{
    if (!cfg) {
        if (err) { err->code = DMD_ERR_INVAL; snprintf(err->msg, sizeof(err->msg),
                                                "配置为空"); }
        return NULL;
    }

    struct dmd_session *s = calloc(1, sizeof(*s));
    if (!s) {
        if (err) { err->code = DMD_ERR_NOMEM; snprintf(err->msg, sizeof(err->msg),
                                                "内存不足"); }
        return NULL;
    }
    s->log = v4l2_log_wanted();
    s->codec = cfg->codec;
    s->io_timeout_ms = cfg->io_timeout_ms > 0 ? cfg->io_timeout_ms : 5000;

    /* VP8 已不支持：msm_vidc 的 V4L2 层没有 VP80 格式。
     * 这里给出明确错误而不是静默失败，便于上层定位。 */
    if (!dmd_v4l2_probe(cfg->codec)) {
        set_err(s, DMD_ERR_REJECTED, "V4L2 解码器不支持该 codec");
        if (err) *err = s->err;
        sess_log(s, "probe 失败: codec=%d 不被 /dev/video32 支持", cfg->codec);
        free(s);
        return NULL;
    }

    if (dmd_v4l2_open(&s->dec, cfg->codec, cfg->width, cfg->height) < 0) {
        set_err(s, DMD_ERR_IO, "打开 V4L2 解码器失败");
        if (err) *err = s->err;
        free(s);
        return NULL;
    }

    sess_log(s, "会话建立: codec=%d %dx%d（V4L2 直通，无 daemon）",
             cfg->codec, cfg->width, cfg->height);
    if (err) { err->code = 0; err->msg[0] = 0; }
    return s;
}

void dmd_session_destroy(struct dmd_session *s)
{
    if (!s) return;
    sess_log(s, "会话结束: 送入 %llu 单元, 收到 %llu 帧",
             (unsigned long long)s->units_sent,
             (unsigned long long)s->frames_recv);
    dmd_v4l2_close(&s->dec);
    free(s);
}

/* 判断一个 Annex-B 单元是否只含参数集（VPS/SPS/PPS 及 H.264 的 SPS/PPS）。
 *
 * 为什么必须区分：配对依赖"驱动登记的序号"与"V4L2 回传的 timestamp"用同一套
 * 编号。驱动侧在 EndPicture 里每**帧**加一号（参数集不经过 EndPicture），
 * 而这里若对每个 send_unit 都加号，参数集就会白占编号，两侧从此错开。
 *
 * 实测后果（HEVC 1920x1080 12 帧）：ffmpeg 调 12 次 EndPicture 登记 1..12，
 * 而 session 层送出 15 个单元编号 1..15。配对按值相等匹配，于是系统性错帧 ——
 * 帧数看着完全正确（12/12），画面却张冠李戴：标称 POC 0 的 surface
 * 实际装的是显示序第 7 帧（逐帧匹配平均差 0.000）。
 *
 * 这个 bug 只在动态画面下暴露：测试素材上半 797 行恰好是静态图案，
 * 与真帧逐字节相同，只有下半的动态渐变带露馅。 */
static int unit_is_param_set_only(int codec, const unsigned char *d, size_t len)
{
    if (codec != DMD_CODEC_H264 && codec != DMD_CODEC_HEVC)
        return 0;
    size_t i = 0;
    int saw_vcl = 0, saw_any = 0;
    while (i + 4 <= len) {
        /* 定位起始码（3 或 4 字节） */
        size_t sc = 0;
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1)
            sc = 3;
        else if (i + 4 <= len && d[i] == 0 && d[i+1] == 0 &&
                 d[i+2] == 0 && d[i+3] == 1)
            sc = 4;
        if (!sc) { i++; continue; }
        size_t h = i + sc;
        if (h >= len) break;
        saw_any = 1;
        if (codec == DMD_CODEC_HEVC) {
            /* HEVC NAL 头 2 字节，type = (byte0 >> 1) & 0x3F。
             * VCL NAL 是 0..31；VPS=32 SPS=33 PPS=34 AUD=35 EOS/EOB/FD/SEI=36..40。 */
            unsigned t = (unsigned)((d[h] >> 1) & 0x3F);
            if (t < 32) saw_vcl = 1;
        } else {
            /* H.264 NAL 头 1 字节，type = byte0 & 0x1F。
             * VCL 是 1(非IDR)/5(IDR)；SPS=7 PPS=8 SEI=6 AUD=9。 */
            unsigned t = (unsigned)(d[h] & 0x1F);
            if (t == 1 || t == 5 || (t >= 2 && t <= 4)) saw_vcl = 1;
        }
        i = h + 1;
    }
    /* 只有确实解析到了 NAL 且其中没有任何 VCL，才算纯参数集单元。 */
    return saw_any && !saw_vcl;
}

/* -------------------------------------------------------------------- 送料 */

int dmd_session_send_unit(struct dmd_session *s, const void *data, size_t len)
{
    if (!s || !data || len == 0) return DMD_ERR_INVAL;

    /* 背压处理：后端返回 1 表示当前无空闲输入缓冲。这不是错误 ——
     * 收帧后缓冲会被回收。必须重试而不能丢单元：丢任何一个访问单元
     * 都会毁掉后续的参考帧链，画面必然坏掉。
     *
     * 与 daemon 版的差异：那边是靠 output 线程并发回收，这边是单线程，
     * 所以要主动调 recv 推进流水线。 */
    /* 纯参数集单元不占编号：驱动侧在 EndPicture 里每帧加一号，
     * 参数集不经过 EndPicture，这里也必须跳过，否则两侧编号错开。 */
    const int ps_only = unit_is_param_set_only(s->codec,
                                               (const unsigned char *)data, len);

    int spins = 0;
    for (;;) {
        /* 参数集沿用当前编号（不预占下一号），VCL 单元才推进。 */
        int r = dmd_v4l2_send(&s->dec, data, len,
                              (uint64_t)(s->units_sent + (ps_only ? 0 : 1))
                                  * 1000ULL);
        if (r == 0) {
            if (!ps_only)
                s->units_sent++;

            /* ⚠️ 送完必须顺手推一次收帧，哪怕调用方还没来取。
             *
             * msm_vidc 要求及时 DQBUF 才会继续解码：CAPTURE 缓冲一直挂在
             * 驱动手里不取回，它就停在那里不再出帧。
             *
             * 实测对照（tools 侧的 nodrain 试验）：送料后立刻循环收帧，
             * 送 6 单元收 6 帧、送 10 收 10，零滞后；而只在背压时才收，
             * 表现为送 6 单元只收 2 帧后彻底停住 —— 上层随即误判为互等，
             * 触发排空并终结会话，ffmpeg 报 internal decoding error。
             *
             * 收到的帧进待取队列，next_frame 会优先从队列取，帧序不乱。
             *
             * ⚠️ 超时不能用 0：poll(timeout=0) 立即返回，几乎永远拿不到帧，
             * 这个循环就形同虚设 —— 实测表现与"完全不收"一样（送 6 收 2）。
             * 对照 tools/pattern 试验：用 50~100ms 时两种送料节奏都 6/6。
             * 取 5ms 是折中：足够让已解好的帧被 DQBUF 取到，又不明显拖慢
             * 送料路径（真正的等待仍在 next_frame）。 */
            for (;;) {
                if (s->pend_count >= DMD_V4L2_MAX_CAP - 1) break;
                uint8_t *pd = NULL; size_t pl = 0; uint64_t pp = 0; int pi = -1;
                int pr = dmd_v4l2_recv(&s->dec, &pd, &pl, &pp, &pi, 5);
                if (pr == 1) {
                    if (!s->fmt.valid && s->dec.cap_ready) publish_format(s);
                    if (pend_push(s, pd, pl, pp, pi) < 0) {
                        dmd_v4l2_release(&s->dec, pi);
                        break;
                    }
                    continue;
                }
                if (pr == 2) { s->eos = 1; break; }
                break;                    /* 0 = 暂无就绪帧，正常 */
            }
            return 0;
        }
        if (r < 0) {
            set_err(s, DMD_ERR_IO, "V4L2 送料失败");
            return DMD_ERR_IO;
        }

        /* r == 1：输入缓冲满。推一次收帧循环让驱动回收缓冲。
         * 这里不能把帧丢掉 —— 若真收到帧，说明调用方还没取，
         * 属于调用序列问题，报错比静默丢帧好。 */
        uint8_t *fd = NULL; size_t fl = 0; uint64_t fp = 0; int fi = -1;
        int rr = dmd_v4l2_recv(&s->dec, &fd, &fl, &fp, &fi, 20);
        if (rr == 1) {
            /* 收到帧但调用方还没来取 —— 存进待取队列，绝不丢弃。
             * 丢任何一帧都会毁掉后续的参考帧链。 */
            if (pend_push(s, fd, fl, fp, fi) < 0) {
                /* 队列容量等于 CAPTURE 缓冲数，理论上到不了这里。
                 * 真到了说明缓冲计数有问题，归还该帧避免泄漏。 */
                dmd_v4l2_release(&s->dec, fi);
                set_err(s, DMD_ERR_STATE, "待取帧队列溢出");
                return DMD_ERR_STATE;
            }
            if (!s->fmt.valid && s->dec.cap_ready) publish_format(s);
            continue;      /* 队列腾出了缓冲，立刻重试送料 */
        }
        if (rr == 2) {
            s->eos = 1;    /* 排空中；余下的帧仍会从队列/recv 取出 */
        }
        if (rr < 0) {
            set_err(s, DMD_ERR_IO, "背压等待中收帧出错");
            return DMD_ERR_IO;
        }
        if (++spins > 500) {          /* 500 × 20ms = 10s */
            set_err(s, DMD_ERR_TIMEOUT, "输入缓冲持续不可用");
            return DMD_ERR_TIMEOUT;
        }
    }
}

int dmd_session_finish_input(struct dmd_session *s)
{
    if (!s) return DMD_ERR_INVAL;
    if (s->input_finished) return 0;
    s->input_finished = 1;
    /* 告知驱动没有更多数据，逼出流水线里剩余的帧。 */
    if (dmd_v4l2_drain(&s->dec) < 0) {
        set_err(s, DMD_ERR_IO, "请求排空失败");
        return DMD_ERR_IO;
    }
    sess_log(s, "输入结束，已请求排空");
    return 0;
}

int dmd_session_drain(struct dmd_session *s)
{
    if (!s) return DMD_ERR_INVAL;

    /* 语义说明：上层（decode.c 的 SyncSurface）在等帧超时时调本函数，
     * 想要"催出积压帧但会话仍可用"。它按返回值分流：
     *   DMD_OK   → 认为可逆，置 drained_once，继续送料
     *   非 DMD_OK → 退回不可逆的 finish_input，会话就此收尾
     *
     * msm_vidc 上这两条路都不能走真正的 DECODER_CMD：
     *   · STOP 会把会话推进 DRAIN 子状态，之后 CMD_START 被拒
     *     （dmesg: "av1D: msm_vidc_streaming_state: (CMD_START) not allowed,
     *      sub_state (DRAIN)"），此后**永久不再出帧**
     *   · 故 V4L2 规范的 drain-and-resume（STOP+START）在本平台不可用，
     *     实测 START 返回 EBUSY
     *
     * 而返回失败同样有害：上层会立刻 finish_input 终结流。
     *
     * 所以这里返回成功的 no-op —— 不动解码器，让上层以为"已排空、可继续"。
     * 这是安全的：本实现的 send_unit 每次送料都顺手 DQBUF 收帧
     * （msm_vidc 要求及时归还 CAPTURE 缓冲才继续解码），积压帧本来就会
     * 被及时取走，不需要额外的排空动作来催。
     *
     * 真正结束时走 finish_input，那里才发不可逆的 STOP。
     *
     * ⚠️ 但返回 DMD_OK 会让上层置 drained_once 后**再也不会**触发真正的排空，
     * 而解码器确实会攥住尾部帧不放 —— 那几帧只有 EOS 才吐得出来。
     *
     * 实测（H.264 300 帧）：drain 返回 OK 时，surface 7 等满 5000ms 超时、
     * ffmpeg 放弃该帧，之后尾部帧才在 DestroyContext 的"排空补齐"里到达，
     * 但那时上游已不再收货，结果硬解 298 帧 / 软解 300 帧。
     * 解码器本身一帧不少（日志：送入 300 单元，收到 300 帧），
     * 丢帧发生在驱动与 ffmpeg 之间。
     *
     * 所以这里返回"不可用"，让上层退回 finish_input 去发真正的 EOS。
     * 代价是会话此后不可再送料 —— 但上层只在等帧超时（默认 2000ms）
     * 且输入尚未结束时才走到这里，那时流实际上已经到尾了。 */
    (void)s;
    sess_log(s, "排空请求：V4L2 无可逆排空，交由上层发 EOS 催出尾部帧");
    return DMD_ERR_STATE;
}

/* -------------------------------------------------------------------- 收帧 */

/* 把 V4L2 协商出的几何写进 dmd_format。
 * V4L2 的 G_SELECTION 已给出有效显示区域，等价于 MediaCodec 的 display crop。 */
static void publish_format(struct dmd_session *s)
{
    struct dmd_v4l2_dec *d = &s->dec;
    int cw = d->crop_w > 0 ? d->crop_w : d->w;
    int ch = d->crop_h > 0 ? d->crop_h : d->h;

    s->fmt.buf_width    = d->w;
    s->fmt.buf_height   = d->h;
    s->fmt.stride       = d->stride;
    s->fmt.slice_height = d->slice_height;
    s->fmt.crop_left    = 0;
    s->fmt.crop_top     = 0;
    s->fmt.crop_right   = cw - 1;
    s->fmt.crop_bottom  = ch - 1;
    if (!s->fmt.valid) s->fmt.changes++;
    s->fmt.valid = 1;

    sess_log(s, "格式: 缓冲 %dx%d 有效 %dx%d stride=%d slice=%d",
             d->w, d->h, cw, ch, d->stride, d->slice_height);
}

/* 填充 dmd_frame。几何信息随帧自洽（与 daemon 版一致）。 */
static void fill_frame(struct dmd_session *s, struct dmd_frame *out,
                       uint8_t *data, size_t len, uint64_t pts, int index)
{
    memset(out, 0, sizeof(*out));
    out->data   = data;
    out->size   = len;
    out->width  = (uint32_t)s->dec.w;
    out->height = (uint32_t)s->dec.h;
    /* PTS 规则与 daemon 版一致：送料时写 seq*1000 微秒，这里除回来得到
     * 输入单元序号，驱动据此精确配对 surface，无需知道出帧顺序。 */
    out->unit_seq = (uint32_t)(pts / 1000ULL);
    out->stride       = s->fmt.stride;
    out->slice_height = s->fmt.slice_height;
    out->crop_left    = s->fmt.crop_left;
    out->crop_top     = s->fmt.crop_top;
    out->crop_right   = s->fmt.crop_right;
    out->crop_bottom  = s->fmt.crop_bottom;
    /* shm_slot 复用为 CAPTURE 缓冲索引：语义仍是"归还凭据"。 */
    out->shm_slot = index;
    out->seq = s->frame_seq++;

    if (index >= 0 && index < DMD_V4L2_MAX_CAP) s->held[index] = 1;
    s->frames_recv++;
}

int dmd_session_next_frame(struct dmd_session *s, struct dmd_frame *out,
                           int timeout_ms)
{
    if (!s || !out) return DMD_ERR_INVAL;

    /* 先给队列里积压的帧 —— 它们是 send_unit 背压期间接住的，序号更早。
     * 不先清队列会让帧序错乱。 */
    {
        uint8_t *qd = NULL; size_t ql = 0; uint64_t qp = 0; int qi = -1;
        if (pend_pop(s, &qd, &ql, &qp, &qi)) {
            fill_frame(s, out, qd, ql, qp, qi);
            return DMD_OK;
        }
    }

    int budget = timeout_ms > 0 ? timeout_ms : s->io_timeout_ms;
    /* 分片轮询：单片较小，便于及时处理 SOURCE_CHANGE 并在协商完成后
     * 立刻发布格式（调用方靠它算 surface 布局）。 */
    const int slice = 50;
    int waited = 0;

    for (;;) {
        uint8_t *fdata = NULL;
        size_t flen = 0;
        uint64_t fpts = 0;
        int fidx = -1;

        int r = dmd_v4l2_recv(&s->dec, &fdata, &flen, &fpts, &fidx, slice);

        if (s->dec.cap_ready && !s->fmt.valid)
            publish_format(s);

        if (r < 0) {
            set_err(s, DMD_ERR_IO, "V4L2 收帧出错");
            return DMD_ERR_IO;
        }
        if (r == 2) {
            s->eos = 1;
            sess_log(s, "收到 EOS");
            return DMD_EOS;
        }
        if (r == 1) {
            fill_frame(s, out, fdata, flen, fpts, fidx);
            return DMD_OK;
        }

        /* r == 0：本片超时无帧 */
        waited += slice;
        if (waited >= budget) {
            {
                int q = 0, h = 0;
                for (int i = 0; i < s->dec.n_cap; i++)
                    if (s->dec.cap[i].queued) q++;
                for (int i = 0; i < DMD_V4L2_MAX_CAP; i++)
                    if (s->held[i]) h++;
                sess_log(s, "next_frame 超时 %d ms（队列 %d, 已送 %llu, 已收 %llu, "
                            "CAPTURE 在驱动 %d/%d, 调用方持有 %d）",
                         budget, s->pend_count,
                         (unsigned long long)s->units_sent,
                         (unsigned long long)s->frames_recv,
                         q, s->dec.n_cap, h);
                int oq = 0;
                for (int i = 0; i < s->dec.n_out; i++)
                    if (s->dec.out[i].queued) oq++;
                sess_log(s, "  OUTPUT 在驱动 %d/%d", oq, s->dec.n_out);
            }
            return DMD_ERR_TIMEOUT;
        }
    }
}

int dmd_session_release_frame(struct dmd_session *s, struct dmd_frame *f)
{
    if (!s || !f) return DMD_ERR_INVAL;
    int idx = f->shm_slot;
    if (idx < 0) return 0;

    if (dmd_v4l2_release(&s->dec, idx) < 0) {
        set_err(s, DMD_ERR_IO, "归还缓冲失败");
        return DMD_ERR_IO;
    }
    if (idx < DMD_V4L2_MAX_CAP) s->held[idx] = 0;
    /* 帧数据位于刚归还的 dmabuf 里，指针立即失效 —— 明确清掉，
     * 避免调用方误用（daemon 版是内部缓冲，容错性更高，这里更严格）。 */
    f->data = NULL;
    f->size = 0;
    f->shm_slot = -1;
    return 0;
}

/* -------------------------------------------------------------------- 查询 */

const struct dmd_format *dmd_session_format(const struct dmd_session *s)
{
    return s ? &s->fmt : NULL;
}

const char *dmd_session_last_error(const struct dmd_session *s)
{
    return (s && s->err.msg[0]) ? s->err.msg : "";
}

int dmd_session_last_error_code(const struct dmd_session *s)
{
    return s ? s->err.code : DMD_ERR_INVAL;
}

int dmd_session_xfer_mode(const struct dmd_session *s)
{
    (void)s;
    /* V4L2 直通没有传输模式之分：帧就在本进程的 dmabuf 映射里。
     * 返回 SHM 是因为它在语义上更接近"零拷贝共享缓冲"，
     * 且上层用它判断是否需要额外拷贝。 */
    return DMD_XFER_SHM;
}

int dmd_session_fd(const struct dmd_session *s)
{
    return s ? s->dec.fd : -1;
}

uint64_t dmd_session_units_sent(const struct dmd_session *s)
{
    return s ? s->units_sent : 0;
}

uint64_t dmd_session_frames_received(const struct dmd_session *s)
{
    return s ? s->frames_recv : 0;
}
