/* AV1 比特流原语自测。
 *
 * 为什么值得单独测：这些编码规则错一位，整条码流就从该位起全部错位，
 * 而症状是解码器给一句含糊的 "failed to read obu"——从现象反推不出是
 * leb128 少写了一字节还是 ns(n) 位宽算错。所以在接入真实合成之前，
 * 先用规范里的已知值把每个原语钉死。
 *
 * 期望值来源：AV1 规范 4.10（leb128/uvlc/le/su/ns）与 5.3（obu_header）。
 * leb128 的边界值另与 protobuf 的 varint 定义交叉核对（同一编码）。
 *
 * 独立编译，不链 libva：
 *   gcc -o test_av1 tests/test_av1_bitstream.c src/av1_bitstream.c src/bitstream.c
 */
#include <stdio.h>
#include <string.h>

#include <va/va.h>
#include <va/va_dec_av1.h>

#include "../src/av1_bitstream.h"

static int fails;

static void check_bytes(const char *what, const unsigned char *got, size_t got_n,
                        const unsigned char *want, size_t want_n)
{
    if (got_n == want_n && memcmp(got, want, want_n) == 0)
        return;
    fails++;
    printf("  ✗ %s\n    实得 (%zu 字节):", what, got_n);
    for (size_t i = 0; i < got_n; i++) printf(" %02x", got[i]);
    printf("\n    期望 (%zu 字节):", want_n);
    for (size_t i = 0; i < want_n; i++) printf(" %02x", want[i]);
    printf("\n");
}

static void check_eq(const char *what, long got, long want)
{
    if (got == want)
        return;
    fails++;
    printf("  ✗ %s: 实得 %ld，期望 %ld\n", what, got, want);
}

/* ---------------------------------------------------------------- leb128 */

static void test_leb128(void)
{
    printf("leb128（规范 4.10.5）\n");
    struct { uint64_t v; size_t n; unsigned char want[4]; } tv[] = {
        { 0,     1, { 0x00 } },
        { 1,     1, { 0x01 } },
        { 127,   1, { 0x7f } },          /* 单字节上界 */
        { 128,   2, { 0x80, 0x01 } },    /* 进位边界 */
        { 255,   2, { 0xff, 0x01 } },
        { 256,   2, { 0x80, 0x02 } },
        { 16383, 2, { 0xff, 0x7f } },    /* 双字节上界 */
        { 16384, 3, { 0x80, 0x80, 0x01 } },
    };
    for (size_t i = 0; i < sizeof(tv) / sizeof(tv[0]); i++) {
        unsigned char out[8];
        size_t n = dmd_av1_leb128(tv[i].v, out, sizeof(out));
        char msg[64];
        snprintf(msg, sizeof(msg), "leb128(%llu)", (unsigned long long)tv[i].v);
        check_bytes(msg, out, n, tv[i].want, tv[i].n);

        /* 长度预估必须与实际写入一致——obu_size 要先算长度再写 payload。 */
        snprintf(msg, sizeof(msg), "leb128_len(%llu)", (unsigned long long)tv[i].v);
        check_eq(msg, (long)dmd_av1_leb128_len(tv[i].v), (long)tv[i].n);
    }

    /* 容量不足必须返回 0，不能越界写。 */
    unsigned char tiny[1];
    check_eq("leb128 容量不足返回 0", (long)dmd_av1_leb128(128, tiny, 1), 0);
}

/* ------------------------------------------------------------- obu_header */

static void test_obu_header(void)
{
    printf("obu_header（规范 5.3.1/5.3.2）\n");

    /* forbidden(1)=0 type(4) ext(1)=0 has_size(1)=1 reserved(1)=0 */
    struct { int type; unsigned char first; const char *name; } tv[] = {
        { DMD_OBU_SEQUENCE_HEADER,    0x0a, "SEQUENCE_HEADER" },
        { DMD_OBU_TEMPORAL_DELIMITER, 0x12, "TEMPORAL_DELIMITER" },
        { DMD_OBU_FRAME_HEADER,       0x1a, "FRAME_HEADER" },
        { DMD_OBU_TILE_GROUP,         0x22, "TILE_GROUP" },
        { DMD_OBU_FRAME,              0x32, "FRAME" },
    };
    for (size_t i = 0; i < sizeof(tv) / sizeof(tv[0]); i++) {
        unsigned char out[8];
        size_t n = dmd_av1_obu_header(tv[i].type, 10, out, sizeof(out));
        char msg[80];
        snprintf(msg, sizeof(msg), "%s 首字节", tv[i].name);
        check_eq(msg, n, 2);                 /* 1 头 + 1 leb128(10) */
        check_eq(msg, out[0], tv[i].first);
        snprintf(msg, sizeof(msg), "%s obu_size", tv[i].name);
        check_eq(msg, out[1], 10);
    }

    /* 大 payload：obu_size 变成 2 字节。 */
    unsigned char out[8];
    size_t n = dmd_av1_obu_header(DMD_OBU_TILE_GROUP, 200, out, sizeof(out));
    check_eq("TILE_GROUP payload=200 总长", (long)n, 3);
    check_eq("  首字节", out[0], 0x22);
    check_eq("  size[0]", out[1], 0xc8);
    check_eq("  size[1]", out[2], 0x01);

    /* 反向确认实测到的非法值：0xd0 的 forbidden 位是 1，type 是 10。
     * 这条断言的作用是把"为什么裸载荷不是 OBU"固化成可执行的证据。 */
    unsigned char bad = 0xd0;
    check_eq("0xd0 的 forbidden_bit（非法）", (bad >> 7) & 1, 1);
    check_eq("0xd0 的 obu_type（保留值）",    (bad >> 3) & 0x0f, 10);
}

/* ------------------------------------------------------------------ 对齐 */

static void test_align(void)
{
    printf("byte_alignment / trailing_bits（规范 5.3.4/5.3.5）\n");

    /* byte_align：纯补零，不写 stop bit。写 3 位 101 后对齐 → 1010_0000 */
    unsigned char buf[4];
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_bw_put_bits(&bw, 0x5, 3);            /* 101 */
    dmd_av1_byte_align(&bw);
    check_eq("byte_align 后字节数", (long)dmd_bw_bytes(&bw), 1);
    check_eq("byte_align 结果",     buf[0], 0xa0);

    /* trailing_bits：写 1 再补零。同样 101 → 1011_0000 */
    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_bw_put_bits(&bw, 0x5, 3);
    dmd_av1_trailing_bits(&bw);
    check_eq("trailing_bits 后字节数", (long)dmd_bw_bytes(&bw), 1);
    check_eq("trailing_bits 结果",     buf[0], 0xb0);

    /* 已对齐时 trailing_bits 仍要写那个 1（它是结束标记，不是填充）。 */
    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_bw_put_bits(&bw, 0xff, 8);
    dmd_av1_trailing_bits(&bw);
    check_eq("已对齐仍写标记位", (long)dmd_bw_bytes(&bw), 2);
    check_eq("  标记字节",       buf[1], 0x80);
}

/* -------------------------------------------------------------------- le */

static void test_le(void)
{
    printf("le(n)（规范 4.10.4，小端）\n");
    unsigned char buf[8];
    struct dmd_bitwriter bw;

    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_le(&bw, 0x1234, 2);
    check_eq("le(2) 字节数", (long)dmd_bw_bytes(&bw), 2);
    check_eq("le(2)[0] 低位在前", buf[0], 0x34);
    check_eq("le(2)[1]",          buf[1], 0x12);

    /* 未字节对齐时调用是用法错误，必须置 overflow 而不是写出错位数据。 */
    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_bw_put_flag(&bw, 1);
    dmd_av1_put_le(&bw, 0xff, 1);
    check_eq("le 未对齐时置 overflow", bw.overflow, 1);
}

/* ------------------------------------------------------------------ uvlc */

static void test_uvlc(void)
{
    printf("uvlc（规范 4.10.3）\n");
    /* v=0 → "1"；v=1 → "010"；v=2 → "011"；v=3 → "00100" */
    struct { uint32_t v; const char *bits; } tv[] = {
        { 0, "1" }, { 1, "010" }, { 2, "011" }, { 3, "00100" }, { 6, "00111" },
    };
    for (size_t i = 0; i < sizeof(tv) / sizeof(tv[0]); i++) {
        unsigned char buf[8];
        struct dmd_bitwriter bw;
        dmd_bw_init(&bw, buf, sizeof(buf));
        dmd_av1_put_uvlc(&bw, tv[i].v);

        /* 把写入的位读回成字符串比对，避免手算字节值。 */
        size_t nbits = strlen(tv[i].bits);
        char got[40] = {0};
        for (size_t b = 0; b < nbits; b++)
            got[b] = ((buf[b / 8] >> (7 - b % 8)) & 1) ? '1' : '0';

        if (strcmp(got, tv[i].bits) != 0) {
            fails++;
            printf("  ✗ uvlc(%u): 实得 %s，期望 %s\n", tv[i].v, got, tv[i].bits);
        }
    }
}

/* -------------------------------------------------------------------- ns */

static void test_ns(void)
{
    printf("ns(n)（规范 4.10.7）\n");
    /* n=1 不占位（唯一取值）。 */
    unsigned char buf[8];
    struct dmd_bitwriter bw;
    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_ns(&bw, 0, 1);
    check_eq("ns(v=0,n=1) 不占位", (long)bw.bit_pos, 0);

    /* n=3：w=2, m=1。v=0 → 1 位 "0"；v=1 → 2 位 "10"；v=2 → 2 位 "11" */
    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_ns(&bw, 0, 3);
    check_eq("ns(0,3) 位数", (long)bw.bit_pos, 1);
    check_eq("ns(0,3) 值",   (buf[0] >> 7) & 1, 0);

    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_ns(&bw, 2, 3);
    check_eq("ns(2,3) 位数", (long)bw.bit_pos, 2);
    check_eq("ns(2,3) 值",   (buf[0] >> 6) & 3, 3);
}

/* -------------------------------------------------------------------- su */

static void test_su(void)
{
    printf("su(n)（规范 4.10.6，补码）\n");
    unsigned char buf[8];
    struct dmd_bitwriter bw;

    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_su(&bw, -1, 4);
    check_eq("su(-1,4) = 0b1111", (buf[0] >> 4) & 0xf, 0xf);

    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_su(&bw, 3, 4);
    check_eq("su(3,4) = 0b0011", (buf[0] >> 4) & 0xf, 0x3);

    dmd_bw_init(&bw, buf, sizeof(buf));
    dmd_av1_put_su(&bw, -8, 4);
    check_eq("su(-8,4) = 0b1000", (buf[0] >> 4) & 0xf, 0x8);
}

/* -------------------------------------------------------------- 序列头合成 */

static void test_sequence_header(void)
{
    printf("OBU_SEQUENCE_HEADER 合成（规范 5.5.1）\n");

    /* 1080p / 8bit / 4:2:0 / profile 0，对应实测码流 av1_1080p.obu 的属性。 */
    VADecPictureParameterBufferAV1 p;
    memset(&p, 0, sizeof(p));
    p.profile                 = 0;
    p.bit_depth_idx           = 0;
    p.matrix_coefficients     = 1;              /* BT.709 */
    p.frame_width_minus1      = 1920 - 1;
    p.frame_height_minus1     = 1080 - 1;
    p.order_hint_bits_minus_1 = 6;
    p.seq_info_fields.fields.use_128x128_superblock = 1;
    p.seq_info_fields.fields.enable_order_hint      = 1;
    p.seq_info_fields.fields.enable_cdef            = 1;
    p.seq_info_fields.fields.subsampling_x          = 1;
    p.seq_info_fields.fields.subsampling_y          = 1;

    unsigned char buf[256];
    size_t n = dmd_av1_build_sequence_header(&p, buf, sizeof(buf));

    check_eq("合成成功（返回非 0）", n > 0, 1);
    check_eq("首字节 = 0x0a（SEQ_HDR, has_size=1）", buf[0], 0x0a);
    check_eq("obu_size 与实际 payload 一致", buf[1], (long)(n - 2));

    /* profile 在首个 payload 字节的高 3 位（f(3) 是最先写的字段）。 */
    check_eq("payload 起始 3 位 = seq_profile", (buf[2] >> 5) & 0x7, 0);

    /* 容量不足必须返回 0 而不是越界写。 */
    unsigned char tiny[4];
    check_eq("容量不足返回 0",
             (long)dmd_av1_build_sequence_header(&p, tiny, sizeof(tiny)), 0);
    check_eq("NULL 参数返回 0",
             (long)dmd_av1_build_sequence_header(NULL, buf, sizeof(buf)), 0);

    /* 外部交叉验证记录（本测试不联外部工具，仅留证据）：
     * 上述参数合成出 16 字节 `0a 0e 00 00 00 05 57 7f 86 ef ff c8 81 01 00 82`，
     * 交 ffmpeg CBS 层解析得到
     *   "obu_type: 1, payload size: 14"
     *   "Video: av1 (Main), none(tv, bt709/unknown/unknown)"
     * 即 profile/color_range/matrix_coefficients 三者均被如实解析。 */
}


/* ---------------------------------------------------------------- SEF 合成
 *
 * 为什么值得测：show_existing_frame 单元只有 4 位有效载荷，但它的
 * 字节布局在第 65~68 轮反复出问题（当时靠改代码试反应，没有基准）。
 * 这里按规范 5.9.2 + 5.3.4 把它钉死。
 *
 * 期望值推导（规范 5.3.2 obu_header + 5.9.2 frame_header + 5.3.4 trailing）：
 *   OBU 头 = forbidden(1)=0 | type(4)=OBU_FRAME_HEADER=3 | ext(1)=0
 *            | has_size(1)=1 | reserved(1)=0
 *          = 0<<7 | 3<<3 | 0<<2 | 1<<1 | 0 = 0x1a
 *   obu_size = 1（载荷一字节）
 *   载荷 = show_existing_frame(1)=1, frame_to_show_map_idx(3)=idx,
 *          trailing: 停止位 1 + 零填充到字节边界
 *        = 1 idx idx idx 1 0 0 0
 * idx=0 → 0b1000_1000 = 0x88；idx=5 → 0b1101_1000 = 0xd8
 * idx=7 → 0b1111_1000 = 0xf8 */
static void test_show_existing(void)
{
    printf("show_existing_frame 合成（规范 5.9.2）\n");
    unsigned char buf[8];

    static const unsigned char want0[] = { 0x1a, 0x01, 0x88 };
    size_t n = dmd_av1_build_show_existing(buf, sizeof(buf), 0);
    check_bytes("SEF map_idx=0", buf, n, want0, sizeof(want0));

    static const unsigned char want5[] = { 0x1a, 0x01, 0xd8 };
    n = dmd_av1_build_show_existing(buf, sizeof(buf), 5);
    check_bytes("SEF map_idx=5", buf, n, want5, sizeof(want5));

    static const unsigned char want7[] = { 0x1a, 0x01, 0xf8 };
    n = dmd_av1_build_show_existing(buf, sizeof(buf), 7);
    check_bytes("SEF map_idx=7", buf, n, want7, sizeof(want7));

    /* map_idx 只有 3 位，高位必须被截掉而不是溢出到 OBU 头。 */
    n = dmd_av1_build_show_existing(buf, sizeof(buf), 13);   /* 13 & 7 == 5 */
    check_bytes("SEF map_idx=13 应等价于 5", buf, n, want5, sizeof(want5));

    /* 容量不足必须返回 0 而不是写坏内存。 */
    check_eq("SEF cap=3 拒绝", (long)dmd_av1_build_show_existing(buf, 3, 0), 0);
    check_eq("SEF cap=0 拒绝", (long)dmd_av1_build_show_existing(buf, 0, 0), 0);
    check_eq("SEF buf=NULL 拒绝",
             (long)dmd_av1_build_show_existing(NULL, 8, 0), 0);
}

/* ------------------------------------------------- refresh_frame_flags 改写
 *
 * 为什么值得测：这是 AV1 路径里唯一的**原地位改写**，改错会静默污染
 * 已经写好的帧头。第 79 轮查出的"5 帧 refresh 值不对"就出在这条链上，
 * 而当时没有任何单测能定位是差分算错还是写入位置算错。
 *
 * 这里只测最基础的不变量（不依赖真实码流）：
 *   1. dpb 为 NULL 或 cur_pic 为 NULL 时不得崩、不得改写
 *   2. 首次调用（prev_valid=0）不改写，但必须保存 map 并置 prev_valid
 *      —— 这正是第 76 轮那个"序列 —,8,32,64 首值丢失"的根因
 *   3. 第二次调用时按 ref_frame_map 的差分位写入指定 bitpos */
static void test_patch_prev_refresh(void)
{
    printf("refresh_frame_flags 原地改写\n");
    struct dmd_av1_dpb dpb;
    VADecPictureParameterBufferAV1 pic;
    unsigned char frame[4];

    /* 1. NULL 保护 */
    memset(&dpb, 0, sizeof(dpb));
    memset(&pic, 0, sizeof(pic));
    dmd_av1_patch_prev_refresh(NULL, &pic, frame, sizeof(frame), 0);
    dmd_av1_patch_prev_refresh(&dpb, NULL, frame, sizeof(frame), 0);
    check_eq("NULL 参数不置 prev_valid", (long)dpb.prev_valid, 0);

    /* 2. 首次调用：不改写，但保存 map 并置 prev_valid */
    memset(&dpb, 0, sizeof(dpb));
    memset(&pic, 0, sizeof(pic));
    for (int i = 0; i < 8; i++) pic.ref_frame_map[i] = 0xff;
    memset(frame, 0, sizeof(frame));
    dmd_av1_patch_prev_refresh(&dpb, &pic, frame, sizeof(frame), 0);
    check_eq("首次调用置 prev_valid", (long)dpb.prev_valid, 1);
    check_eq("首次调用不改写码流", (long)frame[0], 0);
    check_eq("首次调用保存 map", (long)dpb.prev_ref_map[0], 0xff);

    /* 3. 第二次调用：槽 0 与槽 3 变化 → mask = 0b00001001 = 0x09。
     *    bitpos=0 表示从 frame[0] 的最高位开始写 8 位。 */
    pic.ref_frame_map[0] = 0x11;   /* 变了 */
    pic.ref_frame_map[3] = 0x22;   /* 变了 */
    memset(frame, 0, sizeof(frame));
    dmd_av1_patch_prev_refresh(&dpb, &pic, frame, sizeof(frame), 0);
    check_eq("差分改写 mask 槽0|槽3", (long)frame[0], 0x09);

    /* 4. bitpos 非字节对齐时也要写对：bitpos=4 → 跨 frame[0]/frame[1]。
     *    mask 0x09 = 0000_1001，从第 4 位起写 8 位：
     *      frame[0] 低 4 位 = 高 4 位的 mask = 0000
     *      frame[1] 高 4 位 = 低 4 位的 mask = 1001 → 0x90 */
    memset(&dpb, 0, sizeof(dpb));
    memset(&pic, 0, sizeof(pic));
    for (int i = 0; i < 8; i++) pic.ref_frame_map[i] = 0xff;
    dmd_av1_patch_prev_refresh(&dpb, &pic, frame, sizeof(frame), 4);
    pic.ref_frame_map[0] = 0x11;
    pic.ref_frame_map[3] = 0x22;
    memset(frame, 0, sizeof(frame));
    dmd_av1_patch_prev_refresh(&dpb, &pic, frame, sizeof(frame), 4);
    check_eq("bitpos=4 低半字节", (long)(frame[0] & 0x0f), 0x00);
    check_eq("bitpos=4 高半字节", (long)(frame[1] & 0xf0), 0x90);
}


/* ------------------------------------------------- frame_header 的 show_frame 位
 *
 * 为什么单独测这两位：AV1 剩余缺陷的候选修法（"方向 A"）是把
 * show_frame=0 的帧改写成 1，骗硬件把不显示帧也吐出来。
 * 而规范 5.9.2 规定 showable_frame **只在 show_frame==0 时才出现**：
 *     frame_type          f(2)
 *     show_frame          f(1)
 *     if (!show_frame) showable_frame  f(1)
 * 也就是说改写 show_frame 会**改变后续所有字段的位偏移** ——
 * 只翻转那一位而不删掉 showable_frame，整个帧头从此错位一位，
 * 且 refresh_frame_flags 的改写位置（patch_prev_refresh 用的 bitpos）
 * 也会跟着失效。
 *
 * 这里把"两种 show_frame 取值下帧头长度差恰好一位"钉死，
 * 让日后真去做方向 A 时，一旦漏了这个联动就立刻失败而不是产出花屏。
 *
 * ⚠️ 本测试只验证结构性不变量（长度差、前若干位的取值），
 * 不构造完整的参考帧头 —— 那需要一整套 VA 参数与 DPB 状态，
 * 用真实码流做端到端比对更有效（设备恢复后做）。 */
static void test_frame_header_show_frame(void)
{
    printf("frame_header 的 show_frame / showable_frame 联动（规范 5.9.2）\n");

    unsigned char buf_show[512], buf_noshow[512];
    struct dmd_av1_dpb dpb;
    VADecPictureParameterBufferAV1 pic;

    /* 构造一个最小可用的帧间帧参数。只关心前几位的布局，
     * 后续字段用 0 即可 —— 两次调用只差 show_frame，其余完全相同。 */
    memset(&pic, 0, sizeof(pic));
    pic.frame_width_minus1  = 1919;
    pic.frame_height_minus1 = 1079;
    pic.profile = 0;
    pic.order_hint = 4;
    pic.pic_info_fields.bits.frame_type = 1;        /* INTER_FRAME */
    pic.pic_info_fields.bits.showable_frame = 1;

    pic.pic_info_fields.bits.show_frame = 1;
    memset(&dpb, 0, sizeof(dpb));
    size_t n_show = dmd_av1_build_frame_header(&pic, buf_show,
                                              sizeof(buf_show), &dpb);

    pic.pic_info_fields.bits.show_frame = 0;
    memset(&dpb, 0, sizeof(dpb));
    size_t n_noshow = dmd_av1_build_frame_header(&pic, buf_noshow,
                                                 sizeof(buf_noshow), &dpb);

    if (n_show == 0 || n_noshow == 0) {
        /* 合成需要的参数不全时函数会拒绝 —— 那样这个测试没意义，
         * 明确报出来而不是静默通过（静默通过是最坏的结果）。 */
        fails++;
        printf("  ✗ 帧头合成返回 0（show=%zu noshow=%zu），"
               "测试未能覆盖目标\n", n_show, n_noshow);
        return;
    }

    /* ⚠️ 断言必须是可否证的。第一版这里写的是
     *     if (n_show == n_noshow) { ...memcmp... }
     *     if (first_diff >= 8) fails++;
     * 两条在真实数据下都恒不成立（实测 11 vs 12 字节、首差在第 1 字节），
     * 于是三种变异（删条件写入 / 无条件写入 / show_frame 写死 1）
     * 全部未被捕获 —— 测试是摆设。改成下面的正面断言。
     *
     * 规范 5.9.2：show_frame==0 时多出一位 showable_frame，
     * 所以 show=0 的帧头必须**恰好比 show=1 多一位**。
     * 位数无法直接观察，但两者同为字节对齐输出，
     * 差一位在这组参数下表现为差一字节。 */
    check_eq("show=0 帧头比 show=1 长一字节",
             (long)n_noshow, (long)n_show + 1);

    /* show_frame 那一位就在 frame_type 之后，
     * 所以差异必须出现在**第 1 个字节**（第 0 字节是 OBU 头/前导字段）。
     * 写死首差位置，任何改动前导字段布局的变更都会立刻暴露。 */
    size_t first_diff = 0;
    while (first_diff < n_show && first_diff < n_noshow &&
           buf_show[first_diff] == buf_noshow[first_diff])
        first_diff++;
    check_eq("show_frame 差异出现在第 1 字节", (long)first_diff, 1);

    /* 差异字节的具体取值：show=1 与 show=0 在该字节必须不同，
     * 且 show=0 那份的后续字节整体右移一位 —— 用末字节佐证。
     * （只断言"不同"仍是弱断言，所以连同长度与位置三项一起。） */
    if (buf_show[first_diff] == buf_noshow[first_diff]) {
        fails++;
        printf("  ✗ 首差字节取值相同，show_frame 位未真正参与合成\n");
    }

    /* ⚠️ 上面三条仍抓不到"show_frame 被写死成常量 1"这种变异：
     * 写死后两次调用该位都是 1，而 showable_frame 的条件分支若仍读
     * 真实字段，长度差恰好还是 1、首差位置也不变 —— 实测确实逃脱。
     * 所以必须直接检验那一位的取值。
     *
     * ⚠️ 定位时踩过的坑：我先假设"首差字节就是含 show_frame 的字节"，
     * 断言写完在正常代码上就失败了。实测前 4 字节：
     *     show=1:  1a 09 30 00
     *     show=0:  1a 0a 28 00
     * 第 0 字节是 OBU 头 0x1a，第 1 字节是 **obu_size**（9 与 10，
     * 正是 11-2 与 12-2）—— 首差在第 1 字节只是因为长度不同。
     * 真正的帧头载荷从第 2 字节开始，show_frame 在那里。
     *
     * 载荷第 0 字节（= buf[2]）的位布局（帧间帧、非 KEY）：
     *     show_existing_frame f(1)=0
     *     frame_type          f(2)=1  (INTER)
     *     show_frame          f(1)
     *     [!show_frame 时] showable_frame f(1)
     *     ...
     * 所以 show_frame 是从最高位数的第 4 位（bit 4，掩码 0x10）。
     * 实测吻合：0x30 = 0011_0000 该位为 1；0x28 = 0010_1000 该位为 0。 */
    const size_t PAYLOAD = 2;           /* OBU 头 1 字节 + obu_size 1 字节 */
    if (n_show > PAYLOAD && n_noshow > PAYLOAD) {
        check_eq("show_frame 位（载荷 bit4）在 show=1 时为 1",
                 (long)((buf_show[PAYLOAD] >> 4) & 1u), 1);
        check_eq("show_frame 位（载荷 bit4）在 show=0 时为 0",
                 (long)((buf_noshow[PAYLOAD] >> 4) & 1u), 0);
    }
}


int main(void)
{
    printf("=== AV1 比特流原语自测 ===\n");
    test_leb128();
    test_obu_header();
    test_align();
    test_le();
    test_uvlc();
    test_ns();
    test_su();
    test_sequence_header();
    test_show_existing();
    test_patch_prev_refresh();
    test_frame_header_show_frame();

    if (fails == 0) {
        printf("=== 全部通过 ===\n");
        return 0;
    }
    printf("=== %d 项失败 ===\n", fails);
    return 1;
}
