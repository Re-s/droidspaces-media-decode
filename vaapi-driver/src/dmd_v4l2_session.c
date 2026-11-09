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

#include "dmd_v4l2_session_private.h"

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

int dmd_format_display_height(const struct dmd_format *fmt){
    if (!fmt || !fmt->valid) return 0;
    if (fmt->crop_bottom > fmt->crop_top)
        return fmt->crop_bottom - fmt->crop_top + 1;
    return fmt->buf_height;
}

/* ------------------------------------------------------------ 会话生命周期 */

void dmd_session_set_ten_bit(struct dmd_session *s, int ten_bit)
{
    if (!s)
        return;
    /* 只在 CAPTURE 还没配好时有效：配好之后再改几何就没人重协商了，
     * 那种情况（同一会话内位深变化）本驱动不支持，宁可留个日志。 */
    if (s->dec.cap_size && !!ten_bit != !!s->dec.ten_bit) {
        dmd_session_log(s, "ignore bit-depth change (%d to %d) after CAPTURE setup",
                 s->dec.ten_bit, ten_bit);
        return;
    }
    s->dec.ten_bit = ten_bit ? 1 : 0;
}

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

    /* VP8 未实现：驱动侧缺码流重建。
     * ⚠️ 硬件支持 VP80（第 80 轮实测枚举确认），此前注释说"没有 VP80 格式"是错的。
     * 这里给出明确错误而不是静默失败，便于上层定位。 */
    if (!dmd_v4l2_probe(cfg->codec)) {
        dmd_session_set_error(s, DMD_ERR_REJECTED, "V4L2 decoder does not support this codec");
        if (err) *err = s->err;
        dmd_session_log(s, "probe failed: codec=%d is unsupported by /dev/video32", cfg->codec);
        free(s);
        return NULL;
    }

    if (dmd_v4l2_open(&s->dec, cfg->codec, cfg->width, cfg->height) < 0) {
        dmd_session_set_error(s, DMD_ERR_IO, "failed to open V4L2 decoder");
        if (err) *err = s->err;
        free(s);
        return NULL;
    }
    /* open 里 memset 过 d，位深必须在它之后落。 */
    s->dec.ten_bit = cfg->ten_bit ? 1 : 0;

    dmd_session_log(s, "session opened: codec=%d %dx%d (direct V4L2)",
             cfg->codec, cfg->width, cfg->height);
    if (err) { err->code = 0; err->msg[0] = 0; }
    return s;
}

void dmd_session_destroy(struct dmd_session *s)
{
    if (!s) return;
    dmd_session_log(s, "session closed: sent=%llu units, received=%llu frames",
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
    /* AV1：show_existing_frame 单元不推进单元序号。
     *
     * 这类单元是 TD + 一个 SEF 帧头（共 3~4 字节），不含任何 tile 数据，
     * 硬件会为它复显一个已解码帧。驱动侧的 pending 队列只为真实帧登记，
     * 所以它**不能**占用一个 unit_seq —— 否则其后所有真实帧的 PTS
     * 都偏移一位，配对全部落到回退路径上。
     *
     * 这与 HEVC 参数集是同一类问题：第 b86ab9f 次提交修的就是
     * "units_submitted 按帧递增、units_sent 按 send_unit 递增"导致的错位。
     *
     * 判据：长度 <= 8，且首个 OBU 是 TD、第二个是 FRAME_HEADER，
     * 或首个直接就是 FRAME_HEADER。
     *
     * ⚠️ 范围说明：这个修复是必要的（不修则 SEF 之后所有真实帧的
     * PTS 偏移一位、配对全部落到回退路径），但它**不是**
     * "SEF 数量超过 20 就崩"的原因 —— 修完再测 SEF_MAX=40 仍然超时。
     * 那个上限另有成因，尚未定位。 */
    if (codec == DMD_CODEC_AV1) {
        if (len > 8 || len < 2)
            return 0;
        size_t i = 0;
        /* 可选的 TD（type=2, obu_size=0） */
        if (((d[i] >> 3) & 0xF) == 2) {
            if ((d[i] >> 1) & 1) {
                if (i + 1 >= len || d[i + 1] != 0)
                    return 0;
                i += 2;
            } else {
                i += 1;
            }
        }
        if (i >= len)
            return 0;
        /* 随后必须是 FRAME_HEADER（type=3），且载荷首位 show_existing_frame=1 */
        if (((d[i] >> 3) & 0xF) != 3)
            return 0;
        size_t pl = i + 1;
        if ((d[i] >> 1) & 1)
            pl = i + 2;             /* 跳过 leb128 obu_size（此处恒 1 字节） */
        if (pl >= len)
            return 0;
        return (d[pl] >> 7) & 1;    /* show_existing_frame */
    }

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
             *
             * ⚠️ 但这个值也**不能大**：它是每次成功送料的固定死等 —— 帧没
             * 好就整段白等，而且每送一个单元付一次。原先取 5ms，实测
             * 1080p60 真流每次 send_unit 平均耗 3.9ms（就是这一下），两遍法
             * 每帧两次提交 ⇒ 每帧纯等约 8ms，整条流只跑到 51fps（低于 60，
             * 实际播放会卡）。改成 1ms 后同一条流 120~190fps，像素逐字节不变。
             *
             * Preserve the 1ms opportunistic harvest used by send_unit.
             * Explicit waits use next_frame. The background receiver only
             * collects completed output, allowing further input to proceed. */
            for (;;) {
                if (s->pend_count >= DMD_V4L2_MAX_CAP - 1) break;
                uint8_t *pd = NULL; size_t pl = 0; uint64_t pp = 0; int pi = -1;
                int pr = dmd_v4l2_recv(&s->dec, &pd, &pl, &pp, &pi, 1);
                if (pr == 1) {
                    dmd_session_publish_format_if_needed(s);
                    if (dmd_session_pending_push(s, pd, pl, pp, pi) < 0) {
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
            dmd_session_set_error(s, DMD_ERR_IO, "V4L2 input submission failed");
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
            if (dmd_session_pending_push(s, fd, fl, fp, fi) < 0) {
                /* 队列容量等于 CAPTURE 缓冲数，理论上到不了这里。
                 * 真到了说明缓冲计数有问题，归还该帧避免泄漏。 */
                dmd_v4l2_release(&s->dec, fi);
                dmd_session_set_error(s, DMD_ERR_STATE, "pending-frame queue overflow");
                return DMD_ERR_STATE;
            }
            dmd_session_publish_format_if_needed(s);
            continue;      /* 队列腾出了缓冲，立刻重试送料 */
        }
        if (rr == 2) {
            s->eos = 1;    /* 排空中；余下的帧仍会从队列/recv 取出 */
        }
        if (rr < 0) {
            dmd_session_set_error(s, DMD_ERR_IO, "receive failed while relieving input backpressure");
            return DMD_ERR_IO;
        }
        if (++spins > 500) {          /* 500 × 20ms = 10s */
            /* 诊断这类死锁时，这三个计数是关键：pend_count 满而
             * OUTPUT 全在驱动手里，说明消费者不取帧、CAPTURE 没归还、
             * 固件停摆。DMD_VA_LOG=1 时打印。 */
            int qout = 0, qcap = 0;
            for (int i = 0; i < s->dec.n_out; i++)
                if (s->dec.out[i].queued) qout++;
            for (int i = 0; i < s->dec.n_cap; i++)
                if (s->dec.cap[i].queued) qcap++;
            dmd_session_log(s, "input backpressure timed out: pending=%d/%d OUTPUT queued=%d/%d "
                        "CAPTURE queued=%d/%d",
                     s->pend_count, DMD_V4L2_MAX_CAP,
                     qout, s->dec.n_out, qcap, s->dec.n_cap);
            dmd_session_set_error(s, DMD_ERR_TIMEOUT, "input buffers remained unavailable");
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
        dmd_session_set_error(s, DMD_ERR_IO, "failed to request decoder drain");
        return DMD_ERR_IO;
    }
    dmd_session_log(s, "input finished, drain requested");
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
    dmd_session_log(s, "reversible V4L2 drain unavailable; caller must finish input to receive tail frames");
    return DMD_ERR_STATE;
}
