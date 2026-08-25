/*
 * main.c - 视频解码客户端主程序
 *
 * 功能：
 *   1. 从文件或标准输入读取 H.264/HEVC 视频流
 *   2. 通过 Unix socket 发送 NALU 到 Android 侧 MediaCodec 解码 daemon
 *   3. 接收解码后的 NV12 帧
 *   4. 通过 EGL + OpenGL ES 渲染到 X11 窗口（或输出到 PPM 文件）
 *
 * 协议：
 *   发送: [4 bytes NALU size big-endian] [NALU data]
 *   接收: [4 bytes width] [4 bytes height] [4 bytes frame_size] [NV12 data]
 *
 * 用法：
 *   decode-client [选项] <视频文件>
 *   cat video.h264 | decode-client [选项] -
 *
 * 选项：
 *   -s <socket_path>   Unix socket 路径（默认 /tmp/anland/display_daemon.sock）
 *   -o <output_dir>    输出 PPM 帧到目录（同时渲染）
 *   -l                 循环播放
 *   -f <fps>           限制帧率（默认不限制）
 *   -n <count>         最大帧数（默认无限）
 *   --no-display       不渲染到窗口，仅输出文件（需配合 -o）
 *   --shm              用共享内存传帧，省掉 TCP 的两次内核拷贝
 *   -h                 帮助
 */
#include "demuxer.h"
#include "comm.h"
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <getopt.h>

/* 默认 Unix socket 路径（abstract namespace） */
/* 默认连接 TCP 20003，与 src/decode-daemon.c 的默认监听端口一致。
 * 纯数字被 comm_connect 识别为 TCP 端口；也可传路径使用 Unix socket。 */
#define DEFAULT_SOCKET_PATH "20003"

/* 全局退出标志（信号处理） */
static volatile int g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/*
 * 保存一帧为 PPM 文件（P6 格式，每像素 3 字节 RGB）。
 */
static int save_ppm(const char *path, const uint8_t *rgb,
                    int width, int height)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror("[main] 保存 PPM 失败");
        return -1;
    }
    fprintf(fp, "P6\n%d %d\n255\n", width, height);
    fwrite(rgb, 1, width * height * 3, fp);
    fclose(fp);
    return 0;
}

/*
 * NV12 → RGB 软件转换（用于文件输出时）。
 * y_data: Y 平面 (width * height)
 * uv_data: UV 交织平面 (width * height / 2)
 * rgb_out: 输出缓冲区 (width * height * 3)，调用者分配。
 */
/*
 * NV12 → RGB24。
 *
 * stride 是源缓冲的行距，可能大于 width：解码器输出按 128 对齐，
 * 若用 width 当行距逐行读取，画面会随行数递增地横向错位（斜切）。
 * width/height 是要取出的显示区域尺寸。
 */
static void nv12_to_rgb(const uint8_t *y_data, const uint8_t *uv_data,
                        uint8_t *rgb_out, int width, int height, int stride)
{
    if (stride < width) stride = width;
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            int y = y_data[row * stride + col];
            int uv_idx = (row / 2) * stride + (col / 2) * 2;
            int u = uv_data[uv_idx]     - 128;
            int v = uv_data[uv_idx + 1] - 128;

            /* BT.601 */
            int c = y - 16;
            int r = (298 * c + 409 * v + 128) >> 8;
            int g = (298 * c - 100 * u - 208 * v + 128) >> 8;
            int b = (298 * c + 516 * u + 128) >> 8;

            /* 钳位 */
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;

            int out_idx = (row * width + col) * 3;
            rgb_out[out_idx + 0] = r;
            rgb_out[out_idx + 1] = g;
            rgb_out[out_idx + 2] = b;
        }
    }
}

/*
 * 处理一帧解码结果：渲染到窗口并/或写出 PPM。
 * 收发交错与 flush 两处都会用到，抽出以避免逻辑分叉。
 */
static void handle_frame(const DecodedFrame *frame, int index,
                         int display_enabled, RendererContext *renderer,
                         const char *output_dir, const CommFormat *fmt)
{
    /* 缓冲尺寸与真实显示区域可能不同：高通 Venus 把高度对齐到 32，
     * 1080p 解码输出 1920x1088，末 8 行是填充（内容为最后一行的复制）。
     * 帧头给的是缓冲尺寸，crop 才是应当显示的范围。 */
    uint32_t stride = frame->width;
    uint32_t slice  = frame->height;
    uint32_t out_w  = frame->width;
    uint32_t out_h  = frame->height;

    if (fmt && fmt->stride > 0 && fmt->slice_height > 0) {
        stride = (uint32_t)fmt->stride;
        slice  = (uint32_t)fmt->slice_height;
        out_w  = (uint32_t)(fmt->crop_right  - fmt->crop_left + 1);
        out_h  = (uint32_t)(fmt->crop_bottom - fmt->crop_top  + 1);
        if (out_w == 0 || out_w > stride) out_w = stride;
        if (out_h == 0 || out_h > slice)  out_h = slice;
    }

    if (index == 1) {
        printf("[main] 帧 #1: 缓冲 %ux%u, 显示 %ux%u (%u bytes)\n",
               frame->width, frame->height, out_w, out_h, frame->frame_size);
    } else {
        printf("[main] 帧 #%d: %ux%u (%u bytes)\n",
               index, out_w, out_h, frame->frame_size);
    }

    /* UV 平面起点由 stride*slice_height 决定，不是 width*height */
    const uint8_t *y_plane  = frame->data;
    const uint8_t *uv_plane = frame->data + (size_t)stride * slice;

    if (display_enabled && renderer) {
        renderer_draw_frame(renderer, y_plane, uv_plane, out_w, out_h, stride);
    }

    if (output_dir) {
        char ppm_path[512];
        snprintf(ppm_path, sizeof(ppm_path), "%s/frame_%06d.ppm", output_dir, index);
        uint8_t *rgb_buf = malloc((size_t)out_w * out_h * 3);
        if (rgb_buf) {
            nv12_to_rgb(y_plane, uv_plane, rgb_buf, out_w, out_h, stride);
            save_ppm(ppm_path, rgb_buf, out_w, out_h);
            free(rgb_buf);
        }
    }
}

/*
 * 显示用法。
 */
static void usage(const char *prog)
{
    printf("用法: %s [选项] <视频文件|->\n"
           "\n"
           "视频解码客户端 - 发送 NALU 到 Android MediaCodec daemon 解码并取回 NV12 帧\n"
           "\n"
           "选项:\n"
           "  -s <地址>      daemon 地址 (默认 %s)\n"
           "                 纯数字      = TCP 127.0.0.1:<端口>，当前 daemon 用这种\n"
           "                 路径        = Unix domain socket\n"
           "                 @ 开头路径  = abstract namespace socket\n"
           "  -o <dir>       输出 PPM 帧到目录\n"
           "  -l             循环播放\n"
           "  -f <fps>       限制帧率 (默认不限制)\n"
           "  -n <count>     最大帧数 (默认无限)\n"
           "  --no-display   不渲染到窗口 (需配合 -o)\n"
           "  --shm          用共享内存传帧，省掉 TCP 的两次内核拷贝\n"
           "  -h             显示此帮助\n"
           "\n"
           "示例:\n"
           "  %s -s 20003 video.mp4                      # TCP，最常用\n"
           "  %s -s 20003 --no-display -o out/ test.h264 # 只导出 PPM 不开窗口\n"
           "  cat test.h264 | %s -s 20003 -              # 从标准输入读\n"
           "  %s -s '@/anland/decode.sock' video.mp4     # abstract socket\n",
           prog, DEFAULT_SOCKET_PATH, prog, prog, prog, prog);
}

/*
 * 获取当前时间（微秒）。
 */
static int64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int main(int argc, char *argv[])
{
    const char *socket_path = DEFAULT_SOCKET_PATH;
    const char *output_dir = NULL;
    int loop_mode = 0;
    double target_fps = 0.0;  /* 0 = 不限制 */
    int max_frames = -1;      /* -1 = 无限 */
    int display_enabled = 1;
    int use_shm = 0;          /* 1 = 请求共享内存传输 */

    /* 解析命令行参数 */
    static struct option long_opts[] = {
        {"no-display", no_argument, NULL, 'D'},
        {"shm",        no_argument, NULL, 'S'},
        {"help",       no_argument, NULL, 'h'},
        {NULL,         0,           NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:o:lf:n:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's':
            socket_path = optarg;
            break;
        case 'o':
            output_dir = optarg;
            break;
        case 'l':
            loop_mode = 1;
            break;
        case 'f':
            target_fps = atof(optarg);
            if (target_fps <= 0) {
                fprintf(stderr, "错误: 帧率必须 > 0\n");
                return 1;
            }
            break;
        case 'n':
            max_frames = atoi(optarg);
            if (max_frames <= 0) {
                fprintf(stderr, "错误: 帧数必须 > 0\n");
                return 1;
            }
            break;
        case 'D':
            display_enabled = 0;
            break;
        case 'S':
            use_shm = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "错误: 未指定输入文件\n\n");
        usage(argv[0]);
        return 1;
    }

    const char *input_file = argv[optind];

    /* 如果没有输出目录也没有显示，至少启用显示 */
    if (!output_dir && !display_enabled) {
        fprintf(stderr, "警告: 未指定输出目录且禁用显示，启用显示\n");
        display_enabled = 1;
    }

    /* 创建输出目录 */
    if (output_dir) {
        mkdir(output_dir, 0755);
    }

    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("=== 视频解码客户端 ===\n");
    printf("输入: %s\n", input_file);
    printf("Socket: %s\n", socket_path);
    if (output_dir)   printf("输出目录: %s\n", output_dir);
    if (target_fps > 0) printf("帧率限制: %.2f fps\n", target_fps);
    if (max_frames > 0)  printf("最大帧数: %d\n", max_frames);
    printf("\n");

    /* ---- 1. 打开视频 ---- */
    NaluCodecType codec_type;
    DemuxerContext *demuxer = demuxer_open(input_file, &codec_type);
    if (!demuxer) {
        fprintf(stderr, "错误: 无法打开视频文件\n");
        return 1;
    }

    int vid_width, vid_height;
    demuxer_get_video_size(demuxer, &vid_width, &vid_height);
    double fps = demuxer_get_framerate(demuxer);
    const char *codec_label =
        (codec_type == NALU_TYPE_H264) ? "H.264" :
        (codec_type == NALU_TYPE_HEVC) ? "HEVC"  :
        (codec_type == NALU_TYPE_VP9)  ? "VP9"   :
        (codec_type == NALU_TYPE_VP8)  ? "VP8"   : "未知";
    printf("[main] 视频: %dx%d, %.2f fps, %s\n",
           vid_width, vid_height, fps, codec_label);

    /* ---- 2. 初始化渲染器（如需要） ---- */
    RendererContext *renderer = NULL;
    if (display_enabled) {
        renderer = renderer_init(vid_width, vid_height, "Decode Client");
        if (!renderer) {
            fprintf(stderr, "警告: 无法初始化渲染器，回退到无显示模式\n");
            display_enabled = 0;
        }
    }

    /* ---- 4. 主循环: 交错收发 ----
     *
     * 必须边发边收。daemon 是单线程串行的：每收下一个 NALU 就尝试取出一帧
     * 并写回同一个 socket。若客户端"先发完全部 NALU 再开始收"，daemon 的写
     * 很快会把 socket 缓冲填满而阻塞，它便再也回不到 recv —— 双方僵死，
     * 直到客户端关闭写端才被打断，结果是解码帧全部丢失（实测 0 帧）。
     * 改为交错后同一码流可稳定取回 148 帧。 */
    g_running = 1;
    int64_t start_time = get_time_us();
    int frame_count = 0;
    int nalu_total = 0;
    int pass = 0;
    NaluUnit nalu;

    /* 每一轮播放使用一个独立连接。daemon 在每个连接上只配置一次解码器，
     * 并在客户端关闭写端后进入 flush 并结束该会话，因此循环播放必须重连，
     * 单纯 seek 回起点并重发 SPS/PPS 不会让 daemon 重建解码器。 */
    do {
        pass++;
        if (pass > 1) {
            if (demuxer_seek_start(demuxer) < 0) {
                fprintf(stderr, "[main] 循环播放: 无法回到流起点，停止\n");
                break;
            }
            printf("[main] --- 第 %d 轮播放 ---\n", pass);
        }

        CommContext *comm = comm_connect(socket_path);
        if (!comm) {
            fprintf(stderr, "错误: 无法连接到 daemon\n");
            if (pass == 1) { if (renderer) renderer_destroy(renderer); return 1; }
            break;
        }

        /* VP8/VP9 是整帧码流，没有 Annex B 起始码，必须关闭自动补齐 */
        if (codec_type == NALU_TYPE_VP9 || codec_type == NALU_TYPE_VP8)
            comm_set_annexb(comm, 0);

        /* 请求共享内存传输（省掉 TCP 的两次内核拷贝）。
         * daemon 可能降级，握手后要看实际生效的模式。 */
        if (use_shm) comm_set_xfer(comm, COMM_XFER_SHM);

        /* 握手：声明编解码器与分辨率，并换回 stride / 显示裁剪信息。
         * 失败不致命——daemon 若是不支持握手的旧版本，退回默认 H.264 假设。 */
        int cid;
        switch (codec_type) {
        case NALU_TYPE_HEVC: cid = COMM_CODEC_HEVC; break;
        case NALU_TYPE_VP9:  cid = COMM_CODEC_VP9;  break;
        case NALU_TYPE_VP8:  cid = COMM_CODEC_VP8;  break;
        default:             cid = COMM_CODEC_H264; break;
        }
        /* 握手是必需的：失败说明 daemon 版本不匹配或拒绝了该配置，
         * 继续发数据只会得到错位的结果，直接中止更有意义。
         * 返回 -2 是 endpoint inode 校验失败（连到了旧 socket）——
         * 用独立退出码 7 标识，脚本可据此与其它失败区分。 */
        int hr = comm_handshake(comm, cid, vid_width, vid_height);
        if (hr != 0) {
            if (hr == -2)
                fprintf(stderr, "[main] endpoint inode 校验失败，中止\n");
            else
                fprintf(stderr, "[main] 握手失败，中止（客户端与 daemon 需配套发布）\n");
            comm_close(comm);
            if (pass == 1) {
                if (renderer) renderer_destroy(renderer);
                return (hr == -2) ? 7 : 1;
            }
            break;
        }
        if (pass == 1) {
            printf("[main] 传输模式: %s\n",
                   comm_get_xfer(comm) == COMM_XFER_SHM ? "共享内存" : "TCP");
        }

        if (pass == 1) printf("[main] 开始传输 NALU（交错收发）...\n");

        int nalu_sent = 0;

        while (g_running) {
            if (demuxer_read_nalu(demuxer, &nalu) < 0) break;   /* 本轮码流读完 */
            if (comm_send_nalu(comm, nalu.data, nalu.size) < 0) {
                fprintf(stderr, "[main] 发送 NALU 失败\n");
                break;
            }
            nalu_sent++;

            /* 排空当前所有已就绪的帧，避免 daemon 写阻塞 */
            while (g_running) {
                int fd = comm_get_fd(comm);
                if (fd < 0) break;
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(fd, &rfds);
                struct timeval tv = { 0, 0 };      /* 非阻塞探测 */
                if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) break;

                DecodedFrame frame;
                memset(&frame, 0, sizeof(frame));
                if (comm_recv_frame(comm, &frame) != 0) { g_running = 0; break; }
                frame_count++;
                CommFormat cf;
                int has_cf = (comm_get_format(comm, &cf) == 0);
                handle_frame(&frame, frame_count, display_enabled, renderer,
                             output_dir, has_cf ? &cf : NULL);
                /* SHM 模式必须归还槽位，否则池耗尽后 daemon 判定客户端卡死。
                 * TCP 模式下是空操作。 */
                comm_release_frame(comm, &frame);
                if (max_frames > 0 && frame_count >= max_frames) { g_running = 0; break; }
            }

            if (max_frames > 0 && frame_count >= max_frames) break;
        }

        nalu_total += nalu_sent;
        printf("[main] 本轮发送: %d NALU\n", nalu_sent);

        /* 通知 daemon 输入结束，触发 flush 阶段 */
        comm_close_write(comm);

        /* 收取 flush 阶段剩余的帧 */
        while (max_frames <= 0 || frame_count < max_frames) {
            DecodedFrame frame;
            memset(&frame, 0, sizeof(frame));
            int ret = comm_recv_frame(comm, &frame);
            /* comm_recv_frame 的契约: 0=成功, 1=连接正常关闭(EOF), <0=错误。
             * 只判断 ret < 0 会把 EOF 当成一帧有效数据处理，此时 frame 未被填充，
             * 打印和渲染都会用到未初始化的尺寸（曾表现为 3373589840x125 这类垃圾值，
             * 并写出 4KB 大小的畸形 PPM）。 */
            if (ret != 0) break;

            frame_count++;
            CommFormat cf;
            int has_cf = (comm_get_format(comm, &cf) == 0);
            handle_frame(&frame, frame_count, display_enabled, renderer,
                         output_dir, has_cf ? &cf : NULL);
            comm_release_frame(comm, &frame);
        }

        comm_close(comm);

        if (nalu_sent == 0) break;    /* 本轮没读到数据，避免空转 */

    } while (loop_mode && g_running &&
             (max_frames <= 0 || frame_count < max_frames));

    /* 统计 */
    int64_t total_time = get_time_us() - start_time;
    printf("\n=== 传输完成 ===\n");
    if (pass > 1) printf("播放轮数: %d\n", pass);
    printf("发送 NALU: %d\n", nalu_total);
    printf("总帧数: %d\n", frame_count);
    printf("总耗时: %.2f 秒\n", total_time / 1000000.0);
    if (frame_count > 0) {
        printf("平均帧率: %.2f fps\n", frame_count * 1000000.0 / total_time);
    }

    if (renderer) {
        renderer_destroy(renderer);
    }

    return 0;
}
