/*
 * v4l2_backend —— 直接对 msm_vidc 的 V4L2 M2M 接口解码，绕开 MediaCodec/Codec2。
 *
 * 存在理由：SM8750 的 Codec2 层缺 AV1 支持 —— c2.qti.av1.decoder 在组件创建
 * 阶段就失败，logcat 可见
 *     QC2VppFilterCaps: Unknown codectype=c2.qti.av1.decoder
 *     QueryrequiredInfos from downstream failed!
 * 下游 VPP filter 不认识 AV1，组件起不来。但硬件与内核驱动完全可用：
 * /dev/video32 的 OUTPUT_MPLANE 枚举出 AV10，实测 V4L2 直通能解出帧，
 * 且像素与 ffmpeg 软解逐点完全一致（tools/v4l2_dec_probe.c 的验证结论）。
 *
 * ⚠️ 上面这段是 0.3.x 的定位，已过时：0.4.0 起**全部** codec 都走本后端，
 * 没有 MediaCodec 那条路了（daemon 已删除）。
 *
 * 关键约束（都是实测踩出来的，改动时务必保持）：
 *
 * ⚠️ 本段在 0.4.1 被整体重写。此前记录的"nabu 的 video32 解码路径起不来"
 *    这一结论**已被推翻** —— nabu 上现在能解出与 ffmpeg 软解逐字节一致的
 *    1080p 帧（300/300 帧 MD5 相同，两个不同码流各自整流一致）。
 *    当时缺的不是缓冲类型、不是 ION heap、不是权限、也不是固件能力，
 *    而是下面第 4/6/7/8 条这四项 msm_vidc 私有协议要求。
 *    厂商内核出处：MiCode/Xiaomi_Kernel_OpenSource 分支 nabu-r-oss。
 *
 * 1. 缓冲类型：msm_vidc **只接受 V4L2_MEMORY_USERPTR**，
 *    MMAP 与 DMABUF 的 REQBUFS 都返回 EINVAL
 *    （q->io_modes = VB2_MMAP | VB2_USERPTR，msm_vidc.c:1548）。
 *
 *    但 USERPTR 是个名义值 —— 驱动的 vb2_mem_ops.get_userptr 是桩函数：
 *        static void *vidc_get_userptr(...) { return (void *)0xdeadbeef; }
 *                                              (msm_vidc.c:717-720)
 *    真正的缓冲来源是
 *        b->m.planes[i].m.fd        = b->m.planes[i].reserved[0];
 *        b->m.planes[i].data_offset = b->m.planes[i].reserved[1];
 *                                              (msm_vidc.c:533-536)
 *    也就是 **dmabuf fd 必须写进 plane.reserved[0]**，m.userptr 被完全忽略。
 *    映射发生在 QBUF 时（msm_vidc_common.c:6693-6708 的 msm_smem_map_dma_buf）。
 *    这解释了当年"USERPTR 整条流程走完却出不了帧"的困局。
 *
 *    缓冲来源：nabu 无 dma_heap，只有 /dev/ion，system heap id=**25**
 *    （用 ION_IOC_HEAP_QUERY 问内核，不要逐位试探 mask）。
 *
 * 2. S_FMT(OUTPUT) 必须先用一个**不同的分辨率**做 dummy 调用。
 *    驱动在分辨率与当前值相同时提前返回，跳过 plane_sizes 赋值：
 *        if (fourcc 相同 && width 相同 && height 相同) {
 *            dprintk("No change in OUTPUT port params"); return 0;
 *        }                                     (msm_vdec.c:732-738)
 *    driver open 后 OUTPUT 默认就是 1920x1088，直接设目标分辨率必然命中它，
 *    sizeimage 永远回填 0。实测：直接设 → 0；先 1280x720 再设 → 16588800。
 *
 * 3. 输入输出分辨率必须严格相等，本设备不支持缩放：
 *        if (!scale_x.min || !scale_x.max || !scale_y.min || !scale_y.max) {
 *            if (input_w * input_h != output_w * output_h) return -ENOTSUPP;
 *        }                                     (msm_vidc_common.c:5613-5625)
 *    所以 1080p 要用 1920x**1088**（CAPTURE 的对齐值），不是 1080。
 *    尺寸不符时 STREAMON 报 524 (ENOTSUPP)，而非 EINVAL —— 可用来区分。
 *
 * 4. **必须启用 SECONDARY 分流模式**（这是当年缺的决定性一项）：
 *        V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_MODE = base + 22 = 0x00992016
 *        V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_SECONDARY = 1
 *                                     (厂商 v4l2-controls.h:865-869)
 *    PRIMARY（默认）下 CAPTURE 直接充当 DPB，start_streaming() 恒返回
 *    -EINVAL。SECONDARY 启用 HAL_BUFFER_OUTPUT2，DPB 与 OPB 分离，
 *    msm_vidc.c:1214-1222 会额外调 msm_comm_set_output_buffers()
 *    让驱动自行分配 DPB，校验路径完全不同。
 *    单一变量对照实测：SECONDARY → STREAMON(OUT) OK；PRIMARY → EINVAL。
 *    附带好处：CAPTURE 可用线性 NV12，不必解 UBWC 的 Q128。
 *
 * 5. 私有控制项基址（厂商 v4l2-controls.h:684）：
 *        V4L2_CID_MPEG_MSM_VIDC_BASE = V4L2_CTRL_CLASS_MPEG|0x2000 = 0x00992000
 *    与 OMX HAL 活体 strace 对照出的初始化项：
 *        0x00992003 base+3  VIDEO_OUTPUT_ORDER    = 0
 *        0x00992011 base+17 VIDEO_EXTRADATA       = 2, 25, 31, 29（连设四种）
 *        0x00992038 base+56 VIDEO_LOWLATENCY_MODE = 1
 *    注意 base+15 SYNC_FRAME_DECODE 设 1 会把 MIN_BUFFERS 压到 out=1 cap=1，
 *    不适用于正常解码。
 *
 *    ⚠️ 此前记录的"QUERYCTRL 0 项、S_CTRL EINVAL、这个节点没有控制项"
 *    是**错的**：QUERYCTRL 确实枚举不出私有 CID（它们不在标准控制框架里），
 *    但直接 S_CTRL 这些 id 是**成功**的。用 QUERYCTRL 的结果推断控制项
 *    不存在会得出相反结论。（G_CTRL 不校验 id、拿 0xDEADBEEF 也返回成功
 *    这一点仍然成立，别用它验证控制项存在。）
 *
 * 6. STREAMON 的真实语义：**第一个是假通过，第二个承担全部校验**。
 *        case OUTPUT_MPLANE: if (CAPTURE 已 streaming) rc = start_streaming();
 *        case CAPTURE_MPLANE: if (OUTPUT 已 streaming) rc = start_streaming();
 *                                              (msm_vidc.c:1294-1302)
 *    第一个 STREAMON 因对侧未 streaming 而**跳过** start_streaming()。
 *    所以"CAPTURE 先 STREAMON 就成功"是假象，失败只是被推迟到第二个。
 *    ⚠️ 这也推翻了旧注释第 2 条的"必须两阶段、不能双向一起 STREAMON"：
 *    SECONDARY 模式下不需要等事件才配 CAPTURE，两侧先配好再让第二个
 *    STREAMON 触发校验即可。
 *
 * 7. **必须订阅厂商私有事件**，驱动从不发标准 V4L2_EVENT_SOURCE_CHANGE：
 *        V4L2_EVENT_MSM_VIDC_START = V4L2_EVENT_PRIVATE_START + 0x1000
 *                                  = 0x08001000   (厂商 videodev2.h:2290-2305)
 *          +1 FLUSH_DONE   +2 PORT_SETTINGS_CHANGED_SUFFICIENT
 *          +3 PORT_SETTINGS_CHANGED_INSUFFICIENT  +4 BITDEPTH_CHANGED
 *          +5 SYS_ERROR    +6 RELEASE_BUFFER_REFERENCE
 *          +7 RELEASE_UNQUEUED_BUFFER
 *    只订阅标准 SOURCE_CHANGE 会导致 poll 永不返回 POLLPRI ——
 *    当年"等 SOURCE_CHANGE 永不到达"就是这个原因，不是固件没响应。
 *
 * 8. 收到 PORT_SETTINGS 事件后**必须发 SESSION_CONTINUE**。
 *    驱动在事件处理里无条件置 inst->in_reconfig = true
 *    （msm_vidc_common.c:1761），固件随后停在 reconfig 等待态。
 *    msm_comm_session_continue() 只有两个调用点：
 *      a. start_streaming() 内                  (msm_vidc.c:1244)
 *      b. msm_vidc_comm_cmd() 的 V4L2_QCOM_CMD_SESSION_CONTINUE 分支
 *                                              (msm_vidc_common.c:4155)
 *    走 (a) 需要重跑 STREAMON，实测必然触发 SYS_ERROR（STREAMOFF 把 state
 *    打回 MSM_VIDC_START_DONE 以下）。走 (b) 才是正解：
 *    VIDIOC_DECODER_CMD + cmd = 5（videodev2.h:1991），不动队列状态。
 *
 * 9. **设备必须用 O_NONBLOCK 打开**。事件队列空时 VIDIOC_DQEVENT 会阻塞在
 *    内核 v4l2_event_dequeue()，poll 循环再也回不来。
 *    定位方式：/proc/PID/wchan 显示 v4l2_event_dequeue，
 *    /proc/PID/syscall 显示 29 (ioctl) + 0x80885659 (VIDIOC_DQEVENT)。
 *
 * 10. CAPTURE 几何的两个坑：
 *     · slice_height 不要用 sizeimage 反推。3137536*2/(1920*3) = 1089.16
 *       → 1089，而真实值是 1088，多出的一行会让上层 nv12_copy 越界
 *       SIGSEGV。直接用 f.fmt.pix_mp.height。
 *     · G_FMT(CAPTURE) 可能返回 open 时的残留默认值 1920x1088，
 *       不随 S_FMT(OUTPUT) 联动（4K 码流实测），必须用 OUTPUT 侧协商值覆盖；
 *       覆盖 width 后驱动也不回填 bytesperline，stride 要自己兜底。
 *
 * 11. 输出 1088 行含 8 行对齐填充，取有效区要按 1080 裁剪。
 *     ⚠️ 比对正确性时别让参考端也输出 1088 行 —— 填充数据参与比对会让
 *     所有行错位，得出"PSNR 只有 25.9 dB"的假结论（本会话踩过）。
 *
 * 验证状态（0.4.1，nabu / Snapdragon 860 / kernel 4.14）：
 *     t.h264        1080p  300 帧  整流 MD5 与 ffmpeg 软解一致
 *     real1080p     1080p         整流 MD5 与 ffmpeg 软解一致
 *     s_3840x2160   4K            能出帧但帧数非整（166.18），内容不符，待查
  */
#ifndef DMD_V4L2_BACKEND_H
#define DMD_V4L2_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* 与 decode-daemon.c 的 CodecId 对应（wire id，追加式，不可重排）。
 *
 * ⚠️ 0.4.2 更正：VP8 恢复启用，编号 3 就是它原本的号（不是新占号）。
 * 此前两条否定理由都不成立：
 *   - "硬件没有 VP80 格式" —— 错，ENUM_FMT 实测列出
 *     MPG2/H264/HEVC/VP80/VP90，VP80 在列；
 *   - "驱动侧缺码流重建" —— 也不成立，decode.c 的 vp8_build_frame()
 *     已实现 RFC 6386 §9.1 的 uncompressed data chunk 合成。
 * 缺的只是 profiles.c 的声明、映射，与 codec_to_fourcc 的 VP80 一项。 */
enum {
    DMD_V4L2_CODEC_H264 = 0,
    DMD_V4L2_CODEC_HEVC = 1,
    DMD_V4L2_CODEC_VP9  = 2,
    DMD_V4L2_CODEC_VP8  = 3,
    DMD_V4L2_CODEC_AV1  = 4,
};

#define DMD_V4L2_MAX_OUT 8    /* 输入（码流）缓冲数 */
#define DMD_V4L2_MAX_CAP 24   /* 输出（像素）缓冲数，须 >= 驱动要求的最小值 */

struct dmd_v4l2_buf {
    int      dbuf_fd;         /* dma-heap 分配的 dmabuf，-1 表示未分配 */
    void    *map;             /* mmap 到用户空间的地址，NULL 表示未映射 */
    size_t   length;
    int      queued;          /* 1 = 已在驱动队列里 */
};

struct dmd_v4l2_dec {
    int      fd;                              /* /dev/videoN */
    int      heap_fd;                         /* dma_heap 或 /dev/ion 的 fd */
    int      heap_kind;                       /* 见 v4l2_backend.c 的 HEAP_* */
    unsigned ion_mask;                        /* ION system heap 的 mask */

    int      out_w, out_h;                    /* OUTPUT 侧协商出的对齐尺寸 */
    int      w, h;                            /* 协商后的对齐尺寸 */
    int      crop_w, crop_h;                  /* 有效显示区域 */
    unsigned in_size;                         /* OUTPUT 单缓冲字节数 */
    unsigned cap_size;                        /* CAPTURE 单缓冲字节数 */
    int      stride;                          /* CAPTURE 的行距 */
    int      slice_height;                    /* CAPTURE 的平面高度 */

    int      n_out, n_cap;
    struct dmd_v4l2_buf out[DMD_V4L2_MAX_OUT];
    struct dmd_v4l2_buf cap[DMD_V4L2_MAX_CAP];

    int      cap_planes;                      /* CAPTURE 平面数（含 extradata） */
    size_t   extra_size;                      /* extradata 平面字节数 */
    struct dmd_v4l2_buf extra[DMD_V4L2_MAX_CAP];  /* extradata dmabuf */
    int      reconfig_done;                   /* 已发过 SESSION_CONTINUE */
    int      cap_recfg_tried;                 /* 已试过 CAPTURE 重协商（探测） */
    int      cap_ready;                       /* 1 = 已完成分辨率协商并 STREAMON */
    int      out_streaming, cap_streaming;
    int      draining;                        /* 已送 EOS，等剩余帧 */
    int      saw_eos;
};

/*
 * 打开解码器并完成 OUTPUT 侧配置（第一阶段）。
 * codec_id 目前只支持 DMD_V4L2_CODEC_AV1。
 * w/h 是客户端声明的尺寸，用于初始 S_FMT；真实尺寸由 SOURCE_CHANGE 后
 * G_FMT(CAPTURE) 给出，可能不同（如 1080 → 1088 对齐）。
 * 返回 0 成功，-1 失败（失败时已清理，无需再调 close）。
 */
int dmd_v4l2_open(struct dmd_v4l2_dec *d, int codec_id, int w, int h);

/*
 * 送一个访问单元。data/len 是完整的 temporal unit（AV1）。
 * pts_us 原样透传给驱动的 timestamp，出帧时回传，用于与提交序号配对。
 * 返回 0 成功入队，1 表示当前无空闲输入缓冲（调用方应先收帧再重试），
 * -1 表示出错。
 */
int dmd_v4l2_send(struct dmd_v4l2_dec *d, const uint8_t *data, size_t len,
                  uint64_t pts_us);

/*
 * 取一帧。阻塞至多 timeout_ms。
 * 成功时 *out_data 指向 NV12 数据（属于后端的 dmabuf 映射，调用方须在
 * dmd_v4l2_release 之前用完），*out_len 为字节数，*out_pts 为回传的时间戳。
 * 返回 1 拿到帧，0 超时（无帧），-1 出错，2 表示收到 EOS（流结束）。
 *
 * 首次调用会顺带完成第二阶段（等 SOURCE_CHANGE → 配置 CAPTURE），
 * 所以调用方在 send 首个单元之后必须开始 recv，否则协商不会推进。
 */
int dmd_v4l2_recv(struct dmd_v4l2_dec *d, uint8_t **out_data, size_t *out_len,
                  uint64_t *out_pts, int *out_index, int timeout_ms);

/* 把 recv 拿到的帧缓冲还给驱动。index 用 recv 输出的值。 */
int dmd_v4l2_release(struct dmd_v4l2_dec *d, int index);

/* 送 EOS，让驱动吐出流水线里剩余的帧。之后继续 recv 直到返回 2。
 * 这是**不可逆**的：之后不应再送料。用于会话收尾。 */
int dmd_v4l2_drain(struct dmd_v4l2_dec *d);


/* 关闭并释放全部资源。可重复调用。 */
void dmd_v4l2_close(struct dmd_v4l2_dec *d);

/*
 * 探测：本机的 V4L2 解码节点是否支持该 codec。
 * 不打开会话，只做 ENUM_FMT，代价很低。用于 daemon 启动时决定后端。
 * 返回 1 支持，0 不支持。
 */
int dmd_v4l2_probe(int codec_id);

#endif /* DMD_V4L2_BACKEND_H */
