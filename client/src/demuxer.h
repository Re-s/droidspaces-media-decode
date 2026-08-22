/*
 * demuxer.h - 视频解复用与访问单元提取
 *
 * 使用 ffmpeg libavformat/libavcodec 读取视频文件，提取压缩数据单元，
 * 供客户端发送到 Android MediaCodec 解码。
 *
 * 两类码流的切分方式不同：
 *   H.264 / HEVC — 按 Annex B 起始码切成 NALU，SPS/PPS 从 extradata 注入
 *   VP8  / VP9   — 无 NALU 概念，每个 packet 就是一个完整帧，整包送出
 */
#ifndef DEMUXER_H
#define DEMUXER_H

#include <stdint.h>
#include <stddef.h>

/* 编码类型标识 */
typedef enum {
    NALU_TYPE_UNKNOWN = 0,
    NALU_TYPE_H264    = 1,   /* H.264 / AVC */
    NALU_TYPE_HEVC    = 2,   /* H.265 / HEVC */
    NALU_TYPE_VP9     = 3,   /* VP9，整帧送出 */
    NALU_TYPE_VP8     = 4,   /* VP8，整帧送出 */
} NaluCodecType;

/* 单个访问单元（H.264/HEVC 为一个 NALU；VP8/VP9 为一整帧） */
typedef struct {
    uint8_t *data;       /* 数据（NALU 时不含 start code） */
    size_t   size;       /* 数据大小 */
    int      is_keyframe;/* 是否为关键帧 */
    int64_t  pts;        /* 显示时间戳（微秒），未知时为 -1 */
} NaluUnit;

/* 解复用器上下文（前向声明） */
typedef struct DemuxerContext DemuxerContext;

/*
 * 打开视频文件并初始化解复用器。
 * 返回 NULL 表示失败。
 * codec_out: 输出检测到的编码类型（H264 / HEVC）。
 */
DemuxerContext *demuxer_open(const char *filename, NaluCodecType *codec_out);

/*
 * 读取下一个 NALU。成功返回 0，EOF 或错误返回 -1。
 * 返回的 NaluUnit.data 指向内部缓冲区，下次调用后失效。
 * 注意：对于 Annex B 格式（TS/MKV），会自动提取并转为 length-prefixed；
 *       对于 MP4 (AVCC/HVCC)，直接提取长度前缀格式的 NALU。
 */
int demuxer_read_nalu(DemuxerContext *ctx, NaluUnit *nalu);

/*
 * 重置解复用器到文件开头（用于循环播放）。
 * 返回 0 成功，-1 失败。
 */
int demuxer_seek_start(DemuxerContext *ctx);

/*
 * 获取视频宽度和高度。
 */
void demuxer_get_video_size(DemuxerContext *ctx, int *width, int *height);

/*
 * 获取视频帧率（fps）。返回 0.0 表示未知。
 */
double demuxer_get_framerate(DemuxerContext *ctx);

/*
 * 关闭解复用器并释放资源。
 */
void demuxer_close(DemuxerContext *ctx);

#endif /* DEMUXER_H */
