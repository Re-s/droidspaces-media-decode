/*
 * demuxer.c - 视频解复用与 NALU 提取
 *
 * 通过 ffmpeg 的 libavformat 读取视频容器格式（MP4/MKV/TS 等），
 * 利用 libavcodec 的 bitstream filter 将数据转为 length-prefixed 格式，
 * 逐个提取 NALU 供网络传输。
 */
#include "demuxer.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 内部上下文结构 */
struct DemuxerContext {
    AVFormatContext *fmt_ctx;
    AVCodecContext  *dec_ctx;
    AVPacket        *pkt;
    int              video_stream_idx;
    NaluCodecType    codec_type;

    /* Annex B → length-prefixed 转换缓冲区 */
    uint8_t *extraction_buf;
    size_t   extraction_buf_size;

    /* 当前包中待发送的 NALU 偏移 */
    uint8_t *pending_data;
    size_t   pending_size;
    int      pending_keyframe;
    int64_t  pending_pts;

    /* bitstream filter（将 Annex B 转为 length-prefixed） */
    const AVBitStreamFilter *bsf;
    AVBSFContext *bsf_ctx;

    /* 视频尺寸 */
    int width;
    int height;

    /* 参数集（SPS/PPS）。FFmpeg 把它们放在 AVCodecParameters::extradata 中，
     * 而不会出现在 av_read_frame 返回的 packet 里。必须在第一个 NALU 之前
     * 主动送出，否则解码器拿不到序列参数，会接收全部输入却不产出任何帧。 */
    uint8_t *param_sets;        /* Annex B 形式的 SPS/PPS 序列 */
    size_t   param_sets_size;
    size_t   param_sets_pos;    /* 已消费到的偏移 */
    int      param_sets_done;

    /* 1 = 整包送出（VP8/VP9），不做 NALU 切分也不注入参数集 */
    int      whole_packet;
};

/* 编码类型的可读名称 */
static const char *codec_name(NaluCodecType t)
{
    switch (t) {
    case NALU_TYPE_H264: return "H.264";
    case NALU_TYPE_HEVC: return "HEVC";
    case NALU_TYPE_VP9:  return "VP9";
    case NALU_TYPE_VP8:  return "VP8";
    default:             return "未知";
    }
}

/*
 * 检测 H.264 的 NALU 类型（单字节 NAL header 的 type 字段）。
 * H.264 NAL type:nal_unit_type = header & 0x1F
 */
static int is_h264_keyframe_nalu(const uint8_t *data, size_t size)
{
    if (size < 1) return 0;
    int type = data[0] & 0x1F;
    /* IDR (5), BLA (16-18), IDR_W_RADL (19), IDR_N_LP (20), CRA (21) */
    return (type == 5 || (type >= 16 && type <= 21));
}

/*
 * 检测 HEVC 的 NALU 类型（双字节 NAL header）。
 * HEVC NAL type: nal_unit_type = (header[0] >> 1) & 0x3F
 */
static int is_hevc_keyframe_nalu(const uint8_t *data, size_t size)
{
    if (size < 2) return 0;
    int type = (data[0] >> 1) & 0x3F;
    /* BLA_W_LP(16), BLA_W_RADL(17), BLA_N_LP(18),
       IDR_W_RADL(19), IDR_N_LP(20), CRA_NUT(21) */
    return (type >= 16 && type <= 21);
}

/*
 * 把 extradata 中的参数集转成 Annex B 形式存入 ctx->param_sets。
 *
 * extradata 有两种可能的形式：
 *   1. 已是 Annex B（裸 .h264/.ts 流）：以 00 00 01 或 00 00 00 01 开头，直接拷贝。
 *   2. avcC / hvcC（MP4 等容器）：长度前缀的配置记录，需要解析出各参数集
 *      并逐个加上 start code。
 * 成功返回 0，失败返回 -1。
 */
static int extract_param_sets(DemuxerContext *ctx, const uint8_t *ed, size_t ed_size)
{
    static const uint8_t sc[4] = { 0x00, 0x00, 0x00, 0x01 };

    int is_annexb = (ed_size >= 4 && ed[0] == 0x00 && ed[1] == 0x00 &&
                     (ed[2] == 0x01 || (ed[2] == 0x00 && ed[3] == 0x01)));

    if (is_annexb) {
        ctx->param_sets = malloc(ed_size);
        if (!ctx->param_sets) return -1;
        memcpy(ctx->param_sets, ed, ed_size);
        ctx->param_sets_size = ed_size;
        return 0;
    }

    /* 非 Annex B：按 avcC(H.264) 解析。容量取 extradata 大小加上每个参数集
     * 最多 4 字节 start code 的余量。 */
    size_t cap = ed_size * 2 + 64;
    uint8_t *out = malloc(cap);
    if (!out) return -1;
    size_t n = 0;

    if (ctx->codec_type == NALU_TYPE_H264 && ed_size > 6 && ed[0] == 0x01) {
        /* avcC: [0]=version [1..3]=profile/compat/level [4]=lengthSizeMinusOne
         *       [5]=numSPS(低5位) 后接 (len16 + SPS)*N，然后 numPPS + (len16+PPS)*M */
        size_t p = 5;
        int num_sps = ed[p++] & 0x1f;
        for (int i = 0; i < num_sps && p + 2 <= ed_size; i++) {
            size_t len = ((size_t)ed[p] << 8) | ed[p + 1];
            p += 2;
            if (len == 0 || p + len > ed_size || n + 4 + len > cap) break;
            memcpy(out + n, sc, 4); n += 4;
            memcpy(out + n, ed + p, len); n += len;
            p += len;
        }
        if (p < ed_size) {
            int num_pps = ed[p++];
            for (int i = 0; i < num_pps && p + 2 <= ed_size; i++) {
                size_t len = ((size_t)ed[p] << 8) | ed[p + 1];
                p += 2;
                if (len == 0 || p + len > ed_size || n + 4 + len > cap) break;
                memcpy(out + n, sc, 4); n += 4;
                memcpy(out + n, ed + p, len); n += len;
                p += len;
            }
        }
    } else if (ed_size > 22 && ctx->codec_type == NALU_TYPE_HEVC) {
        /* hvcC: 固定头 22 字节，随后 numOfArrays，每个 array:
         *       [0]=完成标志|nal_type [1..2]=numNalus 然后 (len16 + NALU)*K */
        size_t p = 22;
        int num_arrays = ed[p++];
        for (int a = 0; a < num_arrays && p + 3 <= ed_size; a++) {
            p++;  /* array_completeness | nal_unit_type */
            int num_nalus = ((int)ed[p] << 8) | ed[p + 1];
            p += 2;
            for (int i = 0; i < num_nalus && p + 2 <= ed_size; i++) {
                size_t len = ((size_t)ed[p] << 8) | ed[p + 1];
                p += 2;
                if (len == 0 || p + len > ed_size || n + 4 + len > cap) break;
                memcpy(out + n, sc, 4); n += 4;
                memcpy(out + n, ed + p, len); n += len;
                p += len;
            }
        }
    }

    if (n == 0) { free(out); return -1; }
    ctx->param_sets = out;
    ctx->param_sets_size = n;
    return 0;
}

DemuxerContext *demuxer_open(const char *filename, NaluCodecType *codec_out)
{
    DemuxerContext *ctx = calloc(1, sizeof(DemuxerContext));
    if (!ctx) return NULL;

    ctx->video_stream_idx = -1;

    /* 打开输入文件 */
    int ret = avformat_open_input(&ctx->fmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "[demuxer] 无法打开文件: %s (错误 %d)\n", filename, ret);
        goto fail;
    }

    /* 读取流信息 */
    ret = avformat_find_stream_info(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "[demuxer] 无法读取流信息\n");
        goto fail;
    }

    /* 查找视频流 */
    for (unsigned i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
        if (ctx->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ctx->video_stream_idx = i;
            break;
        }
    }
    if (ctx->video_stream_idx < 0) {
        fprintf(stderr, "[demuxer] 未找到视频流\n");
        goto fail;
    }

    AVStream *vstream = ctx->fmt_ctx->streams[ctx->video_stream_idx];
    AVCodecParameters *par = vstream->codecpar;

    /* 记录视频尺寸 */
    ctx->width  = par->width;
    ctx->height = par->height;

    /* 检测编码类型 */
    if (par->codec_id == AV_CODEC_ID_H264) {
        ctx->codec_type = NALU_TYPE_H264;
        ctx->bsf = av_bsf_get_by_name("h264_mp4toannexb");
        /* 注意：我们实际上需要反向操作（Annex B → MP4），
           但 ffmpeg 没有内置的 annexb_to_mp4 bsf。
           我们将手动处理格式转换。 */
    } else if (par->codec_id == AV_CODEC_ID_HEVC) {
        ctx->codec_type = NALU_TYPE_HEVC;
    } else if (par->codec_id == AV_CODEC_ID_VP9) {
        ctx->codec_type = NALU_TYPE_VP9;
    } else if (par->codec_id == AV_CODEC_ID_VP8) {
        ctx->codec_type = NALU_TYPE_VP8;
    } else {
        fprintf(stderr, "[demuxer] 不支持的编码: %d "
                        "(支持 H.264 / HEVC / VP9 / VP8)\n", par->codec_id);
        goto fail;
    }

    /* VP8/VP9 没有 NALU 与参数集的概念，每个 packet 就是一个完整帧，
     * 整包直接送给解码器，不做起始码扫描也不注入 extradata。 */
    ctx->whole_packet = (ctx->codec_type == NALU_TYPE_VP9 ||
                         ctx->codec_type == NALU_TYPE_VP8);

    if (codec_out) *codec_out = ctx->codec_type;

    /* 打开解码器上下文（仅用于解析，不做实际解码） */
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        fprintf(stderr, "[demuxer] 找不到解码器\n");
        goto fail;
    }
    ctx->dec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx->dec_ctx, par);

    /* 分配 packet */
    ctx->pkt = av_packet_alloc();
    if (!ctx->pkt) goto fail;

    /* 分配提取缓冲区 */
    ctx->extraction_buf_size = 4 * 1024 * 1024; /* 4MB 初始大小 */
    ctx->extraction_buf = malloc(ctx->extraction_buf_size);
    if (!ctx->extraction_buf) goto fail;

    /* 取出 extradata 里的参数集，转成 Annex B 形式备用。
     * VP8/VP9 的序列参数在每个关键帧内部，没有独立参数集，跳过这一步。 */
    if (!ctx->whole_packet) {
        if (par->extradata && par->extradata_size > 0) {
            if (extract_param_sets(ctx, par->extradata, (size_t)par->extradata_size) < 0)
                fprintf(stderr, "[demuxer] 警告: 参数集解析失败，解码可能无法产出帧\n");
        } else {
            fprintf(stderr, "[demuxer] 警告: 流中没有 extradata，"
                            "若码流本身不含带内 SPS/PPS 则无法解码\n");
        }
    }

    if (ctx->whole_packet) {
        printf("[demuxer] 已打开: %s (%dx%d, %s, 整帧模式)\n",
               filename, ctx->width, ctx->height, codec_name(ctx->codec_type));
    } else {
        printf("[demuxer] 已打开: %s (%dx%d, %s, 参数集 %zu 字节)\n",
               filename, ctx->width, ctx->height, codec_name(ctx->codec_type),
               ctx->param_sets_size);
    }

    return ctx;

fail:
    demuxer_close(ctx);
    return NULL;
}

/*
 * 从 Annex B 格式的数据中提取下一个 NALU。
 * Annex B 格式: [start_code (00 00 00 01 或 00 00 01)] [NALU data] [start_code] [NALU data] ...
 * 返回 NALU 数据的起始偏移和大小（不含 start code）。
 * next_offset: 下一个 NALU 的搜索起始偏移。
 */
static int find_nalu_annexb(const uint8_t *buf, size_t buf_size,
                             size_t search_offset,
                             size_t *nalu_start, size_t *nalu_size,
                             size_t *next_offset)
{
    size_t pos = search_offset;
    size_t sc_start = 0;
    int first_sc_len = 0;    /* 首个 start code 的长度，必须单独保存 */
    int found_start = 0;

    /* 跳过前导 start code，找到 NALU 起始 */
    while (pos < buf_size) {
        int sc_len = 0;
        if (pos + 3 <= buf_size &&
            buf[pos] == 0x00 && buf[pos+1] == 0x00) {
            if (buf[pos+2] == 0x01) {
                sc_len = 3;
            } else if (pos + 4 <= buf_size && buf[pos+2] == 0x00 && buf[pos+3] == 0x01) {
                sc_len = 4;
            }
            if (sc_len > 0) {
                if (!found_start) {
                    sc_start = pos;
                    first_sc_len = sc_len;
                    found_start = 1;
                    pos += sc_len;
                } else {
                    /* 找到下一个 start code，当前 NALU 结束。
                     * 这里必须用 first_sc_len（首个起始码长度）。早期版本共用一个
                     * sc_len 变量并在循环末尾将其复位为 0，导致此处按 0 计算，
                     * 切出的 NALU 会把起始码算进数据、边界整体偏移，
                     * 表现为发出 `01 68 ...`、`05 ff ...` 这类残缺 NALU，
                     * 解码器全部拒绝（入队 N 个、输出 0 帧）。 */
                    *nalu_start = sc_start + first_sc_len;
                    *nalu_size = pos - sc_start - first_sc_len;
                    *next_offset = pos;
                    return 1;
                }
                continue;
            }
        }
        pos++;
    }

    /* 到达缓冲区末尾，最后一个 NALU */
    if (found_start) {
        *nalu_start = sc_start + first_sc_len;
        *nalu_size = buf_size - sc_start - first_sc_len;
        *next_offset = buf_size;
        return 1;
    }

    return 0; /* 未找到任何 NALU */
}

/*
 * 检查数据是否已经是 length-prefixed (AVCC) 格式。
 * AVCC 格式: [4字节长度][NALU数据][4字节长度][NALU数据]...
 * 启发式：读取前 4 字节作为长度，看是否合理。
 */
static int is_avcc_format(const uint8_t *data, size_t size)
{
    if (size < 5) return 0;

    /* 必须先排除 Annex B。否则 `00 00 00 01 ...` 会被当成长度前缀读成
     * len = 0x00000001，条件 len <= size-4 成立而误判为 AVCC，
     * 于是每个 packet 只切出 1 字节的"NALU"（实测 163 个 NALU 里
     * 有 156 个是 4 字节起始码 + 1 字节数据的残片，解码器全部拒绝）。
     * Annex B 的 3 字节起始码 `00 00 01` 同理会被读成 0x000001xx。 */
    if (data[0] == 0x00 && data[1] == 0x00 &&
        (data[2] == 0x01 || (data[2] == 0x00 && data[3] == 0x01)))
        return 0;

    uint32_t len = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                   ((uint32_t)data[2] << 8)  |  (uint32_t)data[3];
    /* 合理的 NALU 长度: 1 ~ size-4，且不应太大（比如超过 10MB） */
    return (len > 0 && len <= size - 4 && len < 10 * 1024 * 1024);
}

int demuxer_read_nalu(DemuxerContext *ctx, NaluUnit *nalu)
{
    if (!ctx || !nalu) return -1;

    /* VP8/VP9：一个 packet 就是一个完整帧，整包送出。
     * 不做起始码扫描（这类码流没有 Annex B 结构），也没有参数集要注入。 */
    if (ctx->whole_packet) {
        while (1) {
            int ret = av_read_frame(ctx->fmt_ctx, ctx->pkt);
            if (ret < 0) return -1;                       /* EOF 或错误 */
            if (ctx->pkt->stream_index != ctx->video_stream_idx ||
                ctx->pkt->size <= 0) {
                av_packet_unref(ctx->pkt);
                continue;
            }
            /* 拷进自有缓冲：av_packet_unref 之后 pkt->data 立即失效，
             * 而调用方会在下次调用之前一直使用返回的指针。 */
            size_t need = (size_t)ctx->pkt->size;
            if (need > ctx->extraction_buf_size) {
                uint8_t *nb = realloc(ctx->extraction_buf, need * 2);
                if (!nb) { av_packet_unref(ctx->pkt); return -1; }
                ctx->extraction_buf = nb;
                ctx->extraction_buf_size = need * 2;
            }
            memcpy(ctx->extraction_buf, ctx->pkt->data, need);
            nalu->data        = ctx->extraction_buf;
            nalu->size        = need;
            nalu->pts         = ctx->pkt->pts;
            nalu->is_keyframe = (ctx->pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
            av_packet_unref(ctx->pkt);
            return 0;
        }
    }

    /* 先把 extradata 里的参数集逐个吐出，再开始读 packet。
     * 解码器必须先收到 SPS/PPS 才能建立序列上下文。 */
    if (!ctx->param_sets_done && ctx->param_sets && ctx->param_sets_size > 0) {
        size_t ns, nz, no;
        if (find_nalu_annexb(ctx->param_sets, ctx->param_sets_size,
                             ctx->param_sets_pos, &ns, &nz, &no) && nz > 0) {
            nalu->data = ctx->param_sets + ns;
            nalu->size = nz;
            nalu->pts = -1;
            nalu->is_keyframe = 0;
            ctx->param_sets_pos = no;
            if (no >= ctx->param_sets_size) ctx->param_sets_done = 1;
            return 0;
        }
        ctx->param_sets_done = 1;   /* 解析不出更多，转入正常读取 */
    }

    /* 如果有上次剩余的 NALU 数据，继续处理 */
    while (1) {
        /* 尝试从 pending 数据中提取 NALU */
        if (ctx->pending_data && ctx->pending_size > 0) {
            if (is_avcc_format(ctx->pending_data, ctx->pending_size)) {
                /* AVCC 格式: 直接读取 4 字节长度 */
                if (ctx->pending_size < 4) {
                    ctx->pending_data = NULL;
                    ctx->pending_size = 0;
                    continue;
                }
                uint32_t nalu_len = (ctx->pending_data[0] << 24) |
                                    (ctx->pending_data[1] << 16) |
                                    (ctx->pending_data[2] << 8)  |
                                    ctx->pending_data[3];
                if (nalu_len > ctx->pending_size - 4) {
                    fprintf(stderr, "[demuxer] AVCC NALU 长度越界: %u > %zu\n",
                            nalu_len, ctx->pending_size - 4);
                    ctx->pending_data = NULL;
                    ctx->pending_size = 0;
                    continue;
                }
                nalu->data = ctx->pending_data + 4;
                nalu->size = nalu_len;
                nalu->pts  = ctx->pending_pts;

                /* 判断关键帧 */
                if (ctx->codec_type == NALU_TYPE_H264)
                    nalu->is_keyframe = is_h264_keyframe_nalu(nalu->data, nalu->size);
                else
                    nalu->is_keyframe = is_hevc_keyframe_nalu(nalu->data, nalu->size);

                /* 推进 pending 指针 */
                size_t advance = 4 + nalu_len;
                ctx->pending_data += advance;
                ctx->pending_size -= advance;

                return 0;
            } else {
                /* Annex B 格式: 查找 start code */
                size_t nalu_start, nalu_size, next_offset;
                if (find_nalu_annexb(ctx->pending_data, ctx->pending_size, 0,
                                     &nalu_start, &nalu_size, &next_offset)) {
                    nalu->data = ctx->pending_data + nalu_start;
                    nalu->size = nalu_size;
                    nalu->pts  = ctx->pending_pts;

                    if (ctx->codec_type == NALU_TYPE_H264)
                        nalu->is_keyframe = is_h264_keyframe_nalu(nalu->data, nalu->size);
                    else
                        nalu->is_keyframe = is_hevc_keyframe_nalu(nalu->data, nalu->size);

                    /* 保留剩余数据 */
                    ctx->pending_data += next_offset;
                    ctx->pending_size -= next_offset;

                    return 0;
                }
            }
            /* pending 数据无法产生 NALU，清空并读下一个包 */
            ctx->pending_data = NULL;
            ctx->pending_size = 0;
        }

        /* 读取下一个 packet */
        int ret = av_read_frame(ctx->fmt_ctx, ctx->pkt);
        if (ret < 0) {
            return -1; /* EOF 或错误 */
        }

        /* 跳过非视频流 */
        if (ctx->pkt->stream_index != ctx->video_stream_idx) {
            av_packet_unref(ctx->pkt);
            continue;
        }

        /* 记录 PTS（转换为微秒） */
        AVStream *vstream = ctx->fmt_ctx->streams[ctx->video_stream_idx];
        if (ctx->pkt->pts != AV_NOPTS_VALUE) {
            AVRational tb = vstream->time_base;
            ctx->pending_pts = (int64_t)(ctx->pkt->pts * tb.num * 1000000.0 / tb.den);
        } else {
            ctx->pending_pts = -1;
        }

        ctx->pending_keyframe = (ctx->pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;

        /* 必须把 packet 数据拷入自有缓冲区：pending_data 会跨多次
         * demuxer_read_nalu 调用持续使用（一个 packet 通常含多个 NALU），
         * 而下面的 av_packet_unref 会立刻释放 ctx->pkt->data。
         * 早期版本让 pending_data 直接指向 ctx->pkt->data，unref 后即成为悬垂指针，
         * 表现为整个流只能读出 7 个 NALU（实际有 161 个）便误报 EOF。
         * extraction_buf 正是为此预留的。 */
        if ((size_t)ctx->pkt->size > ctx->extraction_buf_size) {
            size_t need = (size_t)ctx->pkt->size * 2;
            uint8_t *nb = realloc(ctx->extraction_buf, need);
            if (!nb) {
                fprintf(stderr, "[demuxer] 扩展提取缓冲失败 (%zu bytes)\n", need);
                av_packet_unref(ctx->pkt);
                return -1;
            }
            ctx->extraction_buf = nb;
            ctx->extraction_buf_size = need;
        }
        memcpy(ctx->extraction_buf, ctx->pkt->data, (size_t)ctx->pkt->size);
        ctx->pending_data = ctx->extraction_buf;
        ctx->pending_size = (size_t)ctx->pkt->size;

        av_packet_unref(ctx->pkt);
    }
}

int demuxer_seek_start(DemuxerContext *ctx)
{
    if (!ctx || !ctx->fmt_ctx) return -1;

    int ret = av_seek_frame(ctx->fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        /* 裸码流（.h264/.hevc）没有索引，av_seek_frame 会失败。
         * 退回到直接把底层 IO 指针拨回开头，对裸 Annex B 流有效。 */
        AVIOContext *pb = ctx->fmt_ctx->pb;
        if (pb && pb->seekable) {
            if (avio_seek(pb, 0, SEEK_SET) >= 0)
                ret = 0;
        }
    }

    avformat_flush(ctx->fmt_ctx);
    ctx->pending_data = NULL;
    ctx->pending_size = 0;
    /* 循环播放时需重新送出参数集：daemon 每个会话会新建解码器，
     * 且部分解码器在 seek 后也要求重新收到 SPS/PPS。 */
    ctx->param_sets_pos = 0;
    ctx->param_sets_done = 0;
    return ret < 0 ? -1 : 0;
}

void demuxer_get_video_size(DemuxerContext *ctx, int *width, int *height)
{
    if (width)  *width  = ctx ? ctx->width  : 0;
    if (height) *height = ctx ? ctx->height : 0;
}

double demuxer_get_framerate(DemuxerContext *ctx)
{
    if (!ctx || !ctx->fmt_ctx || ctx->video_stream_idx < 0) return 0.0;
    AVStream *vs = ctx->fmt_ctx->streams[ctx->video_stream_idx];
    AVRational fr = av_guess_frame_rate(ctx->fmt_ctx, vs, NULL);
    if (fr.num > 0 && fr.den > 0)
        return (double)fr.num / (double)fr.den;
    return 0.0;
}

void demuxer_close(DemuxerContext *ctx)
{
    if (!ctx) return;
    if (ctx->bsf_ctx)     av_bsf_free(&ctx->bsf_ctx);
    if (ctx->pkt)         av_packet_free(&ctx->pkt);
    if (ctx->dec_ctx)     avcodec_free_context(&ctx->dec_ctx);
    if (ctx->fmt_ctx)     avformat_close_input(&ctx->fmt_ctx);
    if (ctx->extraction_buf) free(ctx->extraction_buf);
    if (ctx->param_sets)     free(ctx->param_sets);
    free(ctx);
}
