/* 从 VA-API 参数缓冲反向合成 H.264 SPS/PPS 比特流
 *
 * ── 为什么需要这个 ────────────────────────────────────────
 * daemon 侧是 MediaCodec，它要的是标准 Annex B 码流，靠起始码定位
 * nal_unit_header 把 SPS/PPS 识别为 codec config（decode-daemon.c:461-489）。
 * 但 ffmpeg 的 hwaccel 不把 SPS/PPS 交给 driver —— 它自己解析掉了
 * （h264dec.c:699 / :717 的 case H264_NAL_SPS / PPS），只把 VCL NALU
 * 经 decode_slice 传下来。所以参数集必须由我们从
 * VAPictureParameterBufferH264 反向合成。
 *
 * ── 好消息：slice 不用重建 ────────────────────────────────
 * 主会话核实（research/M-1-forwarding-spec.md）并经本单元复核：
 *   h264dec.c:674        FF_HW_CALL(avctx, decode_slice, nal->raw_data, nal->raw_size)
 *   h2645_parse.c:92/145 nal->raw_data = src，src 指向起始码之后
 *   h2645_parse.h:102-107 raw_data 保留 emulation prevention 字节（未去转义）
 *   vaapi_h264.c:386-388 这对 buffer/size 原样进 slice data buffer
 *   vaapi_h264.c:349-350 slice_data_offset=0、slice_data_flag=VA_SLICE_DATA_FLAG_ALL
 * 即 slice data 里是**完整原始 NALU（含 NALU header 与 slice header）**，
 * 只缺起始码。决定性的旁证是 vaapi_h264.c:351 的
 * slice_data_bit_offset = get_bits_count(&sl->gb) —— 它是 slice header
 * **结束**的 bit 位置，这个字段能存在就说明 slice header 在 buffer 里。
 *
 * 所以我们不需要重建 slice header，也就绕开了参考列表重排序与 MMCO 命令的
 * 反推（那是欠定问题：VAPictureH264 数组给的是最终列表而非重排序命令）。
 * 转发 slice 只需前置 4 字节 00 00 00 01。
 *
 * ── 仍然要猜的字段 ────────────────────────────────────────
 * profile_idc / level_idc / VUI 在 VA-API 里不存在。处理见下面各处注释：
 * profile_idc 由 VAProfile 映射，level_idc 按分辨率与 DPB 查表推一个刚好够用的。
 */

#include <string.h>

#include "driver.h"
#include "bitstream.h"

/* 比特写入原语与 EPB 转义已提取到 bitstream.c，与 HEVC 共用。 */

/* ------------------------------------------------------------ 字段推导 */

/* profile_idc：VA-API 没有这个字段，只能从 VAProfile 映射。
 *
 * ⚠️ 关键约束：profile_idc >= 100 时 SPS 比特流里才出现 chroma_format_idc /
 * bit_depth_* / seq_scaling_matrix_present_flag 这一组字段。所以 profile_idc
 * 与我们是否写这组字段必须自洽，否则解析器整体错位、码流报废。
 *
 * 策略：只要 pic param 里的 chroma/bit_depth 是非默认值，就必须用 High(100)
 * 才能表达它们；否则按 VAProfile 映射。ffmpeg 的 H264High 是最常见情况。
 */
static unsigned int derive_profile_idc(VAProfile profile,
                                       const VAPictureParameterBufferH264 *pp)
{
    unsigned int chroma = pp->seq_fields.bits.chroma_format_idc;
    unsigned int bd_luma = pp->bit_depth_luma_minus8;
    unsigned int bd_chroma = pp->bit_depth_chroma_minus8;

    /* 非 4:2:0 或非 8-bit 只能用 High 系列表达。 */
    if (chroma != 1 || bd_luma != 0 || bd_chroma != 0)
        return 100;
    /* transform_8x8 是 High profile 独有的工具。 */
    if (pp->pic_fields.bits.transform_8x8_mode_flag)
        return 100;

    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        return 66;
    case VAProfileH264Main:
        return 77;
    case VAProfileH264High:
    default:
        return 100;
    }
}

/* level_idc：VA-API 完全没有。填过高的风险是 MediaCodec 按 level 预留过大
 * 缓冲甚至拒绝配置；填过低的风险是解码器认为码流超规格。
 *
 * 按 Annex A 的 MaxMBPS/MaxFS 表推一个**刚好够用**的 level，比填固定值稳。
 * 只用帧尺寸推（帧率未知），所以取每个 level 的 MaxFS（帧大小上限，单位宏块）。
 */
static unsigned int derive_level_idc(unsigned int mbs_wide,
                                     unsigned int mbs_high)
{
    unsigned int fs = mbs_wide * mbs_high; /* 帧大小，宏块数 */

    /* Annex A 表 A-1 的 MaxFS 列。level 值是 idc（level×10）。 */
    static const struct {
        unsigned int max_fs;
        unsigned int idc;
    } tbl[] = {
        { 99, 10 },     /* 1.0  QCIF */
        { 396, 11 },    /* 1.1 */
        { 396, 12 },    /* 1.2 */
        { 396, 20 },    /* 2.0  CIF */
        { 792, 21 },    /* 2.1 */
        { 1620, 22 },   /* 2.2  SD */
        { 1620, 30 },   /* 3.0 */
        { 3600, 31 },   /* 3.1  720p */
        { 5120, 32 },   /* 3.2 */
        { 8192, 40 },   /* 4.0  1080p */
        { 8192, 41 },   /* 4.1 */
        { 8704, 42 },   /* 4.2 */
        { 22080, 50 },  /* 5.0 */
        { 36864, 51 },  /* 5.1  4K */
        { 36864, 52 },  /* 5.2 */
        { 139264, 60 }, /* 6.0  8K */
    };

    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (fs <= tbl[i].max_fs)
            return tbl[i].idc;
    }
    return 62; /* 超出表范围，给最高 */
}

/* ------------------------------------------------------------ SPS 序列化 */

/* 按 ISO/IEC 14496-10 §7.3.2.1.1 的语法顺序写 seq_parameter_set_rbsp。
 * 每个字段标注来源：A = VA-API 有、B = 可推导、C = 只能填默认值。 */
static size_t build_sps_rbsp(const VAPictureParameterBufferH264 *pp,
                             VAProfile profile, unsigned int disp_width,
                             unsigned int disp_height, unsigned char *out,
                             size_t out_cap)
{
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, out, out_cap);

    unsigned int mbs_wide = (unsigned int)pp->picture_width_in_mbs_minus1 + 1;
    unsigned int mbs_high = (unsigned int)pp->picture_height_in_mbs_minus1 + 1;
    unsigned int profile_idc = derive_profile_idc(profile, pp);
    unsigned int frame_mbs_only = pp->seq_fields.bits.frame_mbs_only_flag;

    /* C：profile_idc 由 VAProfile 推 */
    dmd_bw_put_bits(&bw, profile_idc, 8);
    /* C：constraint_set0..5 + 2 位 reserved。
     *
     * ⚠️ 这 8 位不能随便填：h264_ps.c:677-689 的 more_rbsp_data_in_pps()
     * 在 `profile_idc ∈ {66,77,88} 且 (constraint_set_flags & 7) != 0` 时
     * 返回 0，**PPS 解析会跳过尾部**的 transform_8x8_mode_flag /
     * pic_scaling_matrix_present_flag / second_chroma_qp_index_offset
     * （调用点 h264_ps.c:795）。
     *
     * 所以只在 ConstrainedBaseline 上置 set0|set1（那是它的定义），
     * 其余一律 0 —— 我们需要 PPS 尾部能被读到。 */
    unsigned int constraints = 0;
    if (profile_idc == 66 && profile == VAProfileH264ConstrainedBaseline) {
        /* set0 (bit7) + set1 (bit6)：baseline 且 constrained */
        constraints = 0xC0;
    }
    dmd_bw_put_bits(&bw, constraints, 8);
    /* C：level_idc 按帧尺寸推 */
    dmd_bw_put_bits(&bw, derive_level_idc(mbs_wide, mbs_high), 8);
    /* C：seq_parameter_set_id 固定 0（slice header 经 PPS 间接引用，
     * 而 PPS 里我们也写 0，两边自洽即可） */
    dmd_bw_put_ue(&bw, 0);

    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 || profile_idc == 128) {
        /* A：这一组只在 High 系列出现，取值必须与 profile_idc 的选择自洽 */
        dmd_bw_put_ue(&bw, pp->seq_fields.bits.chroma_format_idc);
        if (pp->seq_fields.bits.chroma_format_idc == 3) {
            /* A：VA-API 的 residual_colour_transform_flag 即新标准的
             * separate_colour_plane_flag */
            dmd_bw_put_flag(&bw, pp->seq_fields.bits.residual_colour_transform_flag);
        }
        dmd_bw_put_ue(&bw, pp->bit_depth_luma_minus8);   /* A */
        dmd_bw_put_ue(&bw, pp->bit_depth_chroma_minus8); /* A */
        /* C：qpprime_y_zero_transform_bypass_flag，填 0（仅无损编码用） */
        dmd_bw_put_flag(&bw, 0);
        /* B：不写 SPS 级缩放矩阵。VAIQMatrixBufferH264 给的是**最终**矩阵，
         * 反推 delta_scale 序列需要区分它来自 SPS 还是 PPS 的 fallback 规则，
         * 无法判定。改为把矩阵放进 PPS（见 build_pps_rbsp），那里语义明确。 */
        dmd_bw_put_flag(&bw, 0);
    }

    dmd_bw_put_ue(&bw, pp->seq_fields.bits.log2_max_frame_num_minus4); /* A */
    dmd_bw_put_ue(&bw, pp->seq_fields.bits.pic_order_cnt_type);        /* A */

    if (pp->seq_fields.bits.pic_order_cnt_type == 0) {
        /* A */
        dmd_bw_put_ue(&bw, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
    } else if (pp->seq_fields.bits.pic_order_cnt_type == 1) {
        /* A */
        dmd_bw_put_flag(&bw, pp->seq_fields.bits.delta_pic_order_always_zero_flag);
        /* C：offset_for_non_ref_pic / offset_for_top_to_bottom_field /
         * num_ref_frames_in_pic_order_cnt_cycle 与 offset_for_ref_frame[]
         * 在 VA-API 里完全没有。pic_order_cnt_type==1 极少见（ffmpeg 生成的
         * 码流几乎都是 type 0 或 2），这里填最简形式：cycle 长度 0。
         * 若真遇到 type 1 码流，POC 会算错 → 帧序错乱。 */
        dmd_bw_put_se(&bw, 0);
        dmd_bw_put_se(&bw, 0);
        dmd_bw_put_ue(&bw, 0);
    }

    dmd_bw_put_ue(&bw, pp->num_ref_frames); /* A */
    /* A */
    dmd_bw_put_flag(&bw, pp->seq_fields.bits.gaps_in_frame_num_value_allowed_flag);
    dmd_bw_put_ue(&bw, pp->picture_width_in_mbs_minus1); /* A */
    /* B：⚠️ VA-API 给的是**帧高**宏块数（vaapi_h264.c:251 填的是
     * h->mb_width/mb_height-1，而 h264_ps.c:457 解析时做过
     * mb_height *= 2 - frame_mbs_only_flag）。比特流里这个字段是
     * map units 数，交错流时是帧高的一半，必须换算回去。
     * 直接原值写入会让交错码流的高度算成两倍。 */
    dmd_bw_put_ue(&bw, mbs_high / (frame_mbs_only ? 1u : 2u) - 1);
    dmd_bw_put_flag(&bw, frame_mbs_only); /* A */
    if (!frame_mbs_only) {
        /* A */
        dmd_bw_put_flag(&bw, pp->seq_fields.bits.mb_adaptive_frame_field_flag);
    }
    dmd_bw_put_flag(&bw, pp->seq_fields.bits.direct_8x8_inference_flag); /* A */

    /* B：frame_cropping 由"宏块对齐后的尺寸"与"实际显示尺寸"之差推出。
     * 这是 1088-vs-1080 在码流侧的对应物：1080 不是 16 的倍数，
     * 所以 SPS 里编码高度是 1088，靠 crop 砍掉 8 行。
     * 单位：crop offset 的单位是"色度采样"，4:2:0 下垂直方向要再除 2，
     * 且 frame_mbs_only=0 时垂直单位再乘 2。 */
    unsigned int coded_w = mbs_wide * 16;
    unsigned int coded_h = mbs_high * 16 * (frame_mbs_only ? 1 : 2);
    unsigned int crop_right = 0, crop_bottom = 0;
    unsigned int chroma = pp->seq_fields.bits.chroma_format_idc;
    /* SubWidthC/SubHeightC：4:2:0 → 2/2，4:2:2 → 2/1，4:4:4 → 1/1 */
    unsigned int sub_w = (chroma == 3) ? 1 : 2;
    unsigned int sub_h = (chroma == 1) ? 2 : 1;
    unsigned int unit_x = sub_w;
    unsigned int unit_y = sub_h * (frame_mbs_only ? 1 : 2);

    if (disp_width > 0 && disp_width < coded_w)
        crop_right = (coded_w - disp_width) / unit_x;
    if (disp_height > 0 && disp_height < coded_h)
        crop_bottom = (coded_h - disp_height) / unit_y;

    if (crop_right || crop_bottom) {
        dmd_bw_put_flag(&bw, 1);
        dmd_bw_put_ue(&bw, 0); /* crop_left */
        dmd_bw_put_ue(&bw, crop_right);
        dmd_bw_put_ue(&bw, 0); /* crop_top */
        dmd_bw_put_ue(&bw, crop_bottom);
    } else {
        dmd_bw_put_flag(&bw, 0);
    }

    /* C：VUI 只写 bitstream_restriction 一段，其余（色彩空间、SAR、帧率）
     * 仍然缺失 —— VA-API 不提供它们，解码器用默认值：SAR 1:1、
     * 色彩空间 unspecified。对 NV12 输出，几何与像素值都不受影响
     * （显示区域由我们按 crop 处理）。
     *
     * 为什么必须写 bitstream_restriction：
     * 没有它时 max_num_reorder_frames 的缺省值等于 DPB 上限（§E.2.1），
     * MediaCodec 只能按最坏情况攥帧 —— 实测滞后 4 个输入单元才吐首帧
     * （无 B 帧的流滞后 1，证明攥帧确实来自重排假设）。
     * 而浏览器里的 ffmpeg 稳态只保持 3 帧在飞（H.264 重排深度决定），
     * 送完第 3 帧就阻塞在 vaSyncSurface 等第 1 帧 → 双方差一帧 → 死锁 →
     * 驱动兜底 flush（不可逆）→ 会话作废 → 浏览器永久回落软解。
     * 实测 Firefox 140 因此只硬解出 1 帧。
     *
     * ⚠️ 不要把 max_num_reorder_frames 写 0（曾经试过，已否决）：
     * 那是谎报流的重排需求，会破坏语义 —— 软解侧实测 150 帧掉到 115 帧。
     * 这里用 num_ref_frames 作上界：它是 SPS 自带的参考帧数上限，
     * 重排深度不可能超过它，所以这是**真实且不放大**的约束。 */
    dmd_bw_put_flag(&bw, 1); /* vui_parameters_present_flag */

    dmd_bw_put_flag(&bw, 0); /* aspect_ratio_info_present_flag */
    dmd_bw_put_flag(&bw, 0); /* overscan_info_present_flag */
    dmd_bw_put_flag(&bw, 0); /* video_signal_type_present_flag */
    dmd_bw_put_flag(&bw, 0); /* chroma_loc_info_present_flag */
    dmd_bw_put_flag(&bw, 0); /* timing_info_present_flag */
    dmd_bw_put_flag(&bw, 0); /* nal_hrd_parameters_present_flag */
    dmd_bw_put_flag(&bw, 0); /* vcl_hrd_parameters_present_flag */
    dmd_bw_put_flag(&bw, 0); /* pic_struct_present_flag */

    dmd_bw_put_flag(&bw, 1); /* bitstream_restriction_flag */
    dmd_bw_put_flag(&bw, 1); /* motion_vectors_over_pic_boundaries_flag：
                          * 缺省即 1，写 1 不改变语义 */
    dmd_bw_put_ue(&bw, 0);   /* max_bytes_per_pic_denom：0 = 无限制 */
    dmd_bw_put_ue(&bw, 0);   /* max_bits_per_mb_denom：0 = 无限制 */
    /* log2_max_mv_length_horizontal/vertical：写规范允许的最大值 15，
     * 等价于不施加额外限制。 */
    dmd_bw_put_ue(&bw, 15);
    dmd_bw_put_ue(&bw, 15);
    dmd_bw_put_ue(&bw, pp->num_ref_frames); /* max_num_reorder_frames */
    dmd_bw_put_ue(&bw, pp->num_ref_frames); /* max_dec_frame_buffering */

    dmd_bw_rbsp_trailing(&bw);

    return bw.overflow ? 0 : dmd_bw_bytes(&bw);
}

/* ------------------------------------------------------------ PPS 序列化 */

/* 按 §7.3.2.2 写 pic_parameter_set_rbsp。 */
static size_t build_pps_rbsp(const VAPictureParameterBufferH264 *pp,
                             const VAIQMatrixBufferH264 *iq, int have_iq,
                             int have_sp, unsigned int sp_l0_minus1,
                             unsigned int sp_l1_minus1, unsigned char *out,
                             size_t out_cap)
{
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, out, out_cap);

    dmd_bw_put_ue(&bw, 0); /* C：pic_parameter_set_id，与 slice header 引用的一致 */
    dmd_bw_put_ue(&bw, 0); /* C：seq_parameter_set_id，与 SPS 里写的一致 */

    dmd_bw_put_flag(&bw, pp->pic_fields.bits.entropy_coding_mode_flag); /* A */
    /* A：VA-API 的 pic_order_present_flag 即
     * bottom_field_pic_order_in_frame_present_flag */
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.pic_order_present_flag);
    /* B：FMO 不支持（VA-API 头文件明确 "FMO is not supported"），
     * 所以 slice group 数恒为 1。 */
    dmd_bw_put_ue(&bw, 0);

    /* B：num_ref_idx_l0/l1_default_active_minus1。
     *
     * 照抄**当前帧** slice param 的 num_ref_idx_l0/l1_active_minus1，
     * 并且每当这两个值变化就重发 PPS（见 decode.c 的 h264_send_param_sets）。
     *
     * 为什么照抄是对的：VA-API 给的是"该 slice 的生效值"。
     * - 对未带 num_ref_idx_active_override_flag 的 slice，生效值**就等于**
     *   PPS 默认值 → 照抄即得真值
     * - 对带 override 的 slice，PPS 默认值对它不起作用 → 填什么都无妨
     * 两种情况都正确，且不需要区分 override_flag（VA-API 并未暴露它）。
     *
     * ⚠️ 两条不能走的歧路，都已实测证伪：
     * 1) 取**首个 IDR** 的 slice param —— I slice 没有这个语法元素，恒为 0，
     *    于是非 override 的 B slice 只用 1 个参考帧。这是旧实现的 bug。
     * 2) 用 num_ref_frames-1 当"安全上界" —— **l1 一位都不能偏大**。
     *    单变量实测（另一值保持真值 l0=2/l1=0）：
     *      l0_m1 = 0,1 → 错；2,3,4,15 → 对（l0 可偏大）
     *      l1_m1 = 0 → 对；1,2,3 → 全错（l1 不可偏大）
     *    因为 l1_default_active 决定非 override B slice 里 ref_idx_l1 的
     *    **熵解码码长**，改了它就是语法解析层错位，不是预测质量问题。
     *
     * ── 实测修正（浏览器黑屏的真正原因）─────────────────────
     * 上面"照抄当前帧生效值"的推理漏了一种情况：**带 override 的 slice**。
     * 对它们 VA-API 给的是 override 后的值，与 PPS 默认值无关，
     * 于是我们会写出一个错的默认值，而**其他**非 override slice 会用它。
     *
     * 实测证据：test1080.h264 的真实 PPS 恒为 l0=2 l1=0（原码流只有一份
     * PPS），而驱动送出了 5 种变体：l0 取 0/1/2/3、l1 取 0/1。
     * 其中只有 l0=2 l1=0 那份是对的。会话开头送的恰好是 l0=0 那份，
     * 于是从第 4 帧起（前 3 帧是流水线里已解好的）画面全黑到下一个 IDR ——
     * 每轮循环黑 25 帧左右，就是用户看到的"每隔一段黑屏一次"。
     *
     * 修正办法：**取见过的最大 l0、最小 l1**。
     * - l0 可以偏大（已实测：l0_m1=2,3,4,15 都对），所以取最大值安全，
     *   且能覆盖真实默认值（真实值必 <= 某个 slice 的生效值）。
     * - l1 一位都不能偏大，所以取最小值 —— 非 override slice 报的就是
     *   真实默认值，而 override slice 只会报得更大。
     * 这样多轮之后收敛到唯一一份 PPS，也顺带消除了无意义的反复重送。 */
    unsigned int def_l0 = have_sp ? sp_l0_minus1 : 0;
    unsigned int def_l1 = have_sp ? sp_l1_minus1 : 0;
    (void)pp;
    if (def_l0 > 31)
        def_l0 = 31;
    if (def_l1 > 31)
        def_l1 = 31;
    dmd_bw_put_ue(&bw, def_l0);
    dmd_bw_put_ue(&bw, def_l1);

    dmd_bw_put_flag(&bw, pp->pic_fields.bits.weighted_pred_flag);  /* A */
    dmd_bw_put_bits(&bw, pp->pic_fields.bits.weighted_bipred_idc, 2); /* A */
    dmd_bw_put_se(&bw, pp->pic_init_qp_minus26);                   /* A */
    dmd_bw_put_se(&bw, pp->pic_init_qs_minus26);                   /* A */
    dmd_bw_put_se(&bw, pp->chroma_qp_index_offset);                /* A */
    /* A */
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.deblocking_filter_control_present_flag);
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.constrained_intra_pred_flag); /* A */
    dmd_bw_put_flag(&bw, pp->pic_fields.bits.redundant_pic_cnt_present_flag); /* A */

    /* PPS 的可选尾部：只在需要表达 transform_8x8 或缩放矩阵时才写。
     * 注意这三个字段是"要么全写要么全不写"（more_rbsp_data 判定）。
     *
     * 前提：SPS 侧的 profile_idc / constraint_set 组合必须允许解码器读到
     * 这段尾部（见 build_sps_rbsp 里 constraint_set 的说明）。我们只在
     * ConstrainedBaseline 上置 constraint 位，而 ConstrainedBaseline 不允许
     * transform_8x8，所以 need_tail 与"尾部可读"这两件事不会冲突。 */
    int need_tail = pp->pic_fields.bits.transform_8x8_mode_flag || have_iq ||
                    pp->second_chroma_qp_index_offset !=
                        pp->chroma_qp_index_offset;

    if (need_tail) {
        dmd_bw_put_flag(&bw, pp->pic_fields.bits.transform_8x8_mode_flag); /* A */

        /* B：缩放矩阵。VAIQMatrixBufferH264 给的是最终绝对值矩阵，
         * 而码流里是相对 fallback 的差分（delta_scale）。
         *
         * 这里选择**不写**（pic_scaling_matrix_present_flag = 0）：
         * 反推 delta_scale 需要知道 fallback 基准（规则 A 用默认矩阵、
         * 规则 B 用前一个矩阵），而"这个矩阵到底来自 SPS 还是 PPS"
         * 在 VA-API 层面无法判定 —— 猜错会让整张矩阵偏移。
         *
         * 后果：若码流用了非默认缩放矩阵，反量化系数会与编码端不一致，
         * 表现为**轻微失真而非花屏**（矩阵只影响系数缩放，不影响熵解码
         * 与预测结构）。实测 ffmpeg 默认编码不启用自定义矩阵，
         * 这条路径在常见码流上不触发。 */
        dmd_bw_put_flag(&bw, 0);

        dmd_bw_put_se(&bw, pp->second_chroma_qp_index_offset); /* A */
    }

    dmd_bw_rbsp_trailing(&bw);

    (void)iq;
    return bw.overflow ? 0 : dmd_bw_bytes(&bw);
}

/* ------------------------------------------------------------ 对外接口 */

/* 组装一个带起始码的 NALU：00 00 00 01 + nal_header + 转义后的 RBSP。
 * nal_ref_idc 对参数集固定 3（最高优先级，标准推荐）。 */
static size_t emit_nalu(unsigned int nal_ref_idc, unsigned int nal_unit_type,
                        const unsigned char *rbsp, size_t rbsp_len,
                        unsigned char *out, size_t out_cap)
{
    if (out_cap < 5)
        return 0;

    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x01;
    out[4] = (unsigned char)(((nal_ref_idc & 0x3) << 5) |
                             (nal_unit_type & 0x1F));

    size_t n = dmd_rbsp_escape(rbsp, rbsp_len, out + 5, out_cap - 5);
    if (n == 0 && rbsp_len > 0)
        return 0;

    return 5 + n;
}

size_t dmd_h264_build_sps_nalu(const VAPictureParameterBufferH264 *pp,
                               VAProfile profile, unsigned int disp_width,
                               unsigned int disp_height, unsigned char *out,
                               size_t out_cap)
{
    unsigned char rbsp[256];
    size_t n = build_sps_rbsp(pp, profile, disp_width, disp_height, rbsp,
                              sizeof(rbsp));
    if (n == 0)
        return 0;
    return emit_nalu(3, 7 /* SPS */, rbsp, n, out, out_cap);
}

size_t dmd_h264_build_pps_nalu(const VAPictureParameterBufferH264 *pp,
                               const VAIQMatrixBufferH264 *iq, int have_iq,
                               const VASliceParameterBufferH264 *sp,
                               unsigned int def_l0_minus1,
                               unsigned int def_l1_minus1,
                               unsigned char *out, size_t out_cap)
{
    unsigned char rbsp[256];
    /* l0/l1 由调用方推导后传入，**不能**直接取 sp 里的生效值 ——
     * 带 override_flag 的 slice 其生效值与 PPS 默认值无关。
     * 见 decode.c 里的推导说明。 */
    size_t n = build_pps_rbsp(pp, iq, have_iq, sp != NULL,
                              def_l0_minus1, def_l1_minus1, rbsp,
                              sizeof(rbsp));
    if (n == 0)
        return 0;
    return emit_nalu(3, 8 /* PPS */, rbsp, n, out, out_cap);
}
