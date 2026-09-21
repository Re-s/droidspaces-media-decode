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
#include <stdlib.h>      /* setenv/unsetenv：切 DMD_AV1_NO_SHOWFORCE */
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
 *   3. 第二次调用时按 ref_frame_map 的差分位写入指定 bitpos
 *
 * 末位实参是 prev_frame（被改写那一帧的帧号），只用于影子 DPB 登记，
 * 本用例断言的是码流字节与 map 基准，传 0 表示"不登记影子"。 */
static void test_patch_prev_refresh(void)
{
    printf("refresh_frame_flags 原地改写\n");
    struct dmd_av1_dpb dpb;
    VADecPictureParameterBufferAV1 pic;
    unsigned char frame[4];

    /* 1. NULL 保护 */
    memset(&dpb, 0, sizeof(dpb));
    memset(&pic, 0, sizeof(pic));
    dmd_av1_patch_prev_refresh(NULL, &pic, frame, sizeof(frame), 0, 0);
    dmd_av1_patch_prev_refresh(&dpb, NULL, frame, sizeof(frame), 0, 0);
    check_eq("NULL 参数不置 prev_valid", (long)dpb.prev_valid, 0);

    /* 2. 首次调用：不改写，但保存 map 并置 prev_valid */
    memset(&dpb, 0, sizeof(dpb));
    memset(&pic, 0, sizeof(pic));
    for (int i = 0; i < 8; i++) pic.ref_frame_map[i] = 0xff;
    memset(frame, 0, sizeof(frame));
    dmd_av1_patch_prev_refresh(&dpb, &pic, frame, sizeof(frame), 0, 0);
    check_eq("首次调用置 prev_valid", (long)dpb.prev_valid, 1);
    check_eq("首次调用不改写码流", (long)frame[0], 0);
    check_eq("首次调用保存 map", (long)dpb.prev_ref_map[0], 0xff);

    /* 3. 第二次调用：槽 0 与槽 3 变化 → mask = 0b00001001 = 0x09。
     *    bitpos=0 表示从 frame[0] 的最高位开始写 8 位。 */
    pic.ref_frame_map[0] = 0x11;   /* 变了 */
    pic.ref_frame_map[3] = 0x22;   /* 变了 */
    memset(frame, 0, sizeof(frame));
    dmd_av1_patch_prev_refresh(&dpb, &pic, frame, sizeof(frame), 0, 0);
    check_eq("差分改写 mask 槽0|槽3", (long)frame[0], 0x09);

    /* 4. bitpos 非字节对齐时也要写对：bitpos=4 → 跨 frame[0]/frame[1]。
     *    mask 0x09 = 0000_1001，从第 4 位起写 8 位：
     *      frame[0] 低 4 位 = 高 4 位的 mask = 0000
     *      frame[1] 高 4 位 = 低 4 位的 mask = 1001 → 0x90 */
    memset(&dpb, 0, sizeof(dpb));
    memset(&pic, 0, sizeof(pic));
    for (int i = 0; i < 8; i++) pic.ref_frame_map[i] = 0xff;
    dmd_av1_patch_prev_refresh(&dpb, &pic, frame, sizeof(frame), 4, 0);
    pic.ref_frame_map[0] = 0x11;
    pic.ref_frame_map[3] = 0x22;
    memset(frame, 0, sizeof(frame));
    dmd_av1_patch_prev_refresh(&dpb, &pic, frame, sizeof(frame), 4, 0);
    check_eq("bitpos=4 低半字节", (long)(frame[0] & 0x0f), 0x00);
    check_eq("bitpos=4 高半字节", (long)(frame[1] & 0xf0), 0x90);
}


/* ------------------------------------------------- 引用帧可解析性判定
 *
 * dmd_av1_refs_resolvable() 是"从 GOP 中间起解"那道闸门：引用了硬件 DPB 里
 * 根本不存在帧的码流，Venus 既不出帧也不报错，会把 vaSyncSurface 拖到超时、
 * 调用方随后放弃整条码流（实测 rc=251、0 帧）。所以判错的两个方向都有代价：
 *   该放行却拦 → 正常播放凭空丢帧（画面一直黑到下个关键帧）；
 *   该拦却放  → 整条码流废掉。
 * 本用例把这两侧各钉一遍。 */
static void test_refs_resolvable(void)
{
    printf("引用帧可解析性判定（从 GOP 中间起解的闸门）\n");
    struct dmd_av1_dpb dpb;
    VADecPictureParameterBufferAV1 pic;

    /* 约定：影子表槽 2 存帧号 7，帧 7 属于 surface 100。
     * surf_hist 是静态接口，测试里直接按它的语义填。 */
#define SETUP()   do {                                              \
        memset(&dpb, 0, sizeof(dpb));                               \
        memset(&pic, 0, sizeof(pic));                               \
        dpb.dpb_shadow[2] = 7;                                      \
        dpb.surf_hist[0].surf = 100; dpb.surf_hist[0].frame = 7;    \
        dpb.surf_hist_n = 1;                                        \
        dpb.frame_seq = 7;                                          \
        for (int i = 0; i < 8; i++)                                 \
            pic.ref_frame_map[i] = (VASurfaceID)100;                \
        pic.ref_frame_idx[0] = 2;   /* LAST 在槽 2 */               \
        pic.ref_frame_idx[3] = 2;   /* ALTREF 也在槽 2 */           \
    } while (0)

    /* 1. 帧间帧、引用都查得到 → 放行 */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;   /* INTER_FRAME */
    check_eq("引用齐全的帧间帧放行", (long)dmd_av1_refs_resolvable(&pic, &dpb), 1);

    /* 2. KEY / INTRA_ONLY / SWITCH 不需要参考 → 一律放行，
     *    哪怕 map 里全是 INVALID（关键帧起解就是这个样子）。 */
    SETUP();
    for (int ft = 0; ft <= 3; ft++) {
        if (ft == 1)
            continue;
        memset(pic.ref_frame_map, 0xff, sizeof(pic.ref_frame_map));
        pic.pic_info_fields.bits.frame_type = (unsigned)ft;
        check_eq("非帧间帧放行", (long)dmd_av1_refs_resolvable(&pic, &dpb), 1);
    }

    /* 3. allow_intrabc 的帧内帧不引用任何帧 → 放行 */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;
    pic.pic_info_fields.bits.allow_intrabc = 1;
    memset(pic.ref_frame_map, 0xff, sizeof(pic.ref_frame_map));
    check_eq("intrabc 帧放行", (long)dmd_av1_refs_resolvable(&pic, &dpb), 1);

    /* 4. LAST 指向一个从未解码过的 surface（VA_INVALID_ID）→ 拦。
     *    这正是"拖到 GOP 中间起解"的头几帧：ffmpeg 的 ref_frame_map 里
     *    那些槽是 4294967295，硬件 DPB 里对应槽是空的。 */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;
    pic.ref_frame_map[2] = 0xffffffffu;
    check_eq("LAST 引用不到时拦截", (long)dmd_av1_refs_resolvable(&pic, &dpb), 0);

    /* 5. LAST 能解析、ALTREF（槽号 5）解析不出 → 也拦。
     *    复合参考与 skip_mode 真的会用到的槽，引用不到同样解不出来。 */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;
    pic.ref_frame_idx[3] = 5;
    pic.ref_frame_map[5] = 0xffffffffu;    /* 该槽里没有帧 */
    check_eq("ALTREF 引用不到时拦截", (long)dmd_av1_refs_resolvable(&pic, &dpb), 0);

    /* 6. ⚠️ reference_select=0 **不能**当成"这帧不引用"放行条件。
     *    规范 5.9.2：它为 0 只是帧头里不显式给 7 个槽号，帧照样引用 LAST
     *    （ref_frame_idx[0]）。第一版就是加了这个提前放行，结果该拦的没拦住，
     *    日志仍显示"合成 2957 字节"、照样 rc=251 卡死。
     *    两个方向都要钉：引用坏 + reference_select=0 → 拦。 */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;
    pic.mode_control_fields.bits.reference_select = 0;
    pic.ref_frame_map[2] = 0xffffffffu;
    check_eq("reference_select=0 且引用坏时仍拦截",
             (long)dmd_av1_refs_resolvable(&pic, &dpb), 0);
    /* 引用好 + reference_select=0 → 放行（正常播放里大量帧是这种，
     * 误拦就会级联：实测 43 帧丢 37 帧）。 */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;
    pic.mode_control_fields.bits.reference_select = 0;
    check_eq("reference_select=0 且引用齐全时放行",
             (long)dmd_av1_refs_resolvable(&pic, &dpb), 1);

    /* 7. surface 号复用：ffmpeg 回收 surface 后同一个号属于新帧。
     *    surf_hist 里 100 号已被帧 9 拥有，而影子表里只有帧 7 → 帧 9 查不到
     *    → 拦。这条防的是"拿 surface 当身份"那种错法。 */
    SETUP();
    pic.pic_info_fields.bits.frame_type = 1;
    dpb.surf_hist[0].frame = 9;
    check_eq("surface 已易主时拦截", (long)dmd_av1_refs_resolvable(&pic, &dpb), 0);

    /* 8. NULL 保护：没有 pic 时无从判断，放行（交给后面的 tile 检查）；
     *    没有 dpb 时判"解析不出"→ 拦。真实调用里 dpb 是 context 的常驻
     *    成员、永不为 NULL，这里只是把语义钉死，别日后改成静默透传。 */
    SETUP();
    check_eq("pic 为 NULL 时放行", (long)dmd_av1_refs_resolvable(NULL, &dpb), 1);
    pic.pic_info_fields.bits.frame_type = 1;
    check_eq("dpb 为 NULL 时按解析不出处理",
             (long)dmd_av1_refs_resolvable(&pic, NULL), 0);
#undef SETUP
}


/* 载荷是按位写的，断言就得按位读。
 * bit_at：第 idx 位（idx=0 是 buf[0] 的最高位），越界返回 -1。 */
static long bit_at(const unsigned char *buf, size_t n, size_t idx)
{
    if (idx >= n * 8)
        return -1;
    return (long)((buf[idx / 8] >> (7 - (idx % 8))) & 1u);
}

/* 最后一个置 1 的位的下标；trailing_bits()（规范 5.3.4）在 payload 末尾
 * 写一个 1 再补零到字节边界，所以这个下标 + 1 就是 payload 的**位数**。
 * 用它比较长度不受字节取整影响 —— 这是本用例的关键观察。
 * 全零时返回 -1。 */
static long last_set_bit(const unsigned char *buf, size_t n)
{
    for (size_t i = n * 8; i-- > 0; )
        if (bit_at(buf, n, i) == 1)
            return (long)i;
    return -1;
}


/* ------------------------------------------------- frame_header 的 show_frame 位
 *
 * 为什么单独测这两位：驱动把合成头里的 show_frame **恒置 1**（"方向 A"，
 * 已落地）—— 硬件只对 show_frame=1 的帧吐 CAPTURE 缓冲，而 ffmpeg 的后端
 * 会为每个提交帧的 surface 要像素。
 * 而规范 5.9.2 规定 showable_frame **只在 show_frame==0 时才出现**：
 *     frame_type          f(2)
 *     show_frame          f(1)
 *     if (!show_frame) showable_frame  f(1)
 * 也就是说改写 show_frame 会**改变后续所有字段的位偏移** ——
 * 只翻转那一位而不删掉 showable_frame，整个帧头从此错位一位，
 * 且 refresh_frame_flags 的改写位置（patch_prev_refresh 用的 bitpos）
 * 也会跟着失效。
 *
 * 所以本用例分两段：先在 A/B 开关（DMD_AV1_NO_SHOWFORCE）下把规范的
 * 条件分支钉死（多一位、右移一位、那两位的取值），
 * 再验证默认路径确实把两种 PPB 合成了逐字节相同的头（强制生效）。
 *
 * ⚠️ 本测试只验证结构性不变量（位数、逐位偏移、特定位的取值），
 * 不构造完整的参考帧头 —— 那需要一整套 VA 参数与 DPB 状态，
 * 端到端比对由 verify_driver.sh 与 14 样本回归覆盖。 */
static void test_frame_header_show_frame(void)
{
    printf("frame_header 的 show_frame / showable_frame 联动（规范 5.9.2）\n");

    /* 驱动默认把 show_frame **恒置 1**（两遍法的地基：硬件只对 show=1 的帧
     * 吐 CAPTURE 缓冲），那样本用例要测的 showable_frame 分支根本不会出现。
     * 先打开 A/B 开关 DMD_AV1_NO_SHOWFORCE 还原规范分支再断言语法，
     * 末尾再单独钉住"默认强制置 1"这个行为本身。 */
    setenv("DMD_AV1_NO_SHOWFORCE", "1", 1);

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

    /* ⚠️ 断言必须是可否证的，而且不能把"字节数"当"位数"用。
     *
     * 第一版写的是 `n_noshow == n_show + 1`（差一字节）与
     * "首差在第 1 字节"。两条都建立在"这次恰好跨了字节边界"之上：
     * 帧头末尾是 trailing_bits()（规范 5.3.4：一个 1 再补零到边界），
     * 多写一位到底需不需要多一个字节，取决于该位前面已有几位。
     * 实测随实现微调在 11/12 与 12/12 之间变过 —— 断言本身是错的，
     * 一旦不成立就把一个正确的实现判成失败。
     *
     * 规范 5.9.2 真正的不变量是：show_frame==0 时多写一位 showable_frame，
     * 所以 show=0 的 payload **恰好比 show=1 多一位**，且第 5 位起整体右移一位。
     * 位数可以观察：payload 结束标记（trailing_bits 的那个 1）是缓冲区里
     * 最后一个置 1 的位，它之后全是补零。于是"最后一个 1 的下标"就是
     * payload 的位数，与字节取整无关。 */
    const size_t PAYLOAD = 2;           /* OBU 头 1 字节 + obu_size 1 字节 */
    const long stop_s = (long)last_set_bit(buf_show, n_show);
    const long stop_t = (long)last_set_bit(buf_noshow, n_noshow);
    check_eq("show=0 的 payload 恰好比 show=1 多一位",
             stop_t, stop_s + 1);

    /* show_frame 那一位在 frame_type 之后，载荷内第 3 位（从最高位数）。
     *
     * 载荷第 0 字节（= buf[2]）的位布局（帧间帧、非 KEY）：
     *     show_existing_frame f(1)=0
     *     frame_type          f(2)=1  (INTER)
     *     show_frame          f(1)
     *     [!show_frame 时] showable_frame f(1)
     *     ...
     * 所以 show_frame 是字节内 bit 4（掩码 0x10），showable_frame 是 bit 3。
     * 实测吻合：show=1 → 0x30 = 0011_0000；show=0 → 0x28 = 0010_1000。 */
    if (n_show > PAYLOAD && n_noshow > PAYLOAD) {
        check_eq("show_frame 位（载荷 bit4）在 show=1 时为 1",
                 (long)((buf_show[PAYLOAD] >> 4) & 1u), 1);
        check_eq("show_frame 位（载荷 bit4）在 show=0 时为 0",
                 (long)((buf_noshow[PAYLOAD] >> 4) & 1u), 0);
        check_eq("showable_frame 位（载荷 bit3）在 show=0 时为 1",
                 (long)((buf_noshow[PAYLOAD] >> 3) & 1u), 1);
    }

    /* 右移一位：showable_frame 之后的所有位，show=0 与 show=1 必须逐位错开一格。
     * 这条抓的是"只多写了位但后续字段没跟着挪"那类错（挪错就是整段错位花屏）。 */
    {
        size_t mism = 0, checked = 0;
        for (long i = (long)PAYLOAD * 8 + 5; i <= stop_s + 1; i++) {
            long a = bit_at(buf_noshow, n_noshow, (size_t)i);
            long b = bit_at(buf_show, n_show, (size_t)(i - 1));
            if (a < 0 || b < 0)
                break;
            checked++;
            if (a != b)
                mism++;
        }
        if (checked < 16) {
            fails++;
            printf("  ✗ 右移校验只覆盖了 %zu 位，太少，测试未能覆盖目标\n",
                   checked);
        } else if (mism != 0) {
            fails++;
            printf("  ✗ show=0 的后续位未整体右移一位：%zu/%zu 位不符\n",
                   mism, checked);
        }
    }

    /* ---- 再钉住**默认行为**本身：恒置 show_frame=1 ----
     * 上面整段是在 A/B 开关下验证规范分支；真实跑的是另一种：
     * 驱动把 show_frame 写成 1（硬件只对 show=1 吐 CAPTURE 缓冲，
     * 两遍法的地基）。所以两种 PPB 必须产出**逐字节相同**的帧头。
     * 谁哪天把这个强制去掉，896 帧回归立刻在这里先炸。 */
    unsetenv("DMD_AV1_NO_SHOWFORCE");

    pic.pic_info_fields.bits.show_frame = 1;
    memset(&dpb, 0, sizeof(dpb));
    size_t f_show = dmd_av1_build_frame_header(&pic, buf_show,
                                              sizeof(buf_show), &dpb);
    pic.pic_info_fields.bits.show_frame = 0;
    memset(&dpb, 0, sizeof(dpb));
    size_t f_noshow = dmd_av1_build_frame_header(&pic, buf_noshow,
                                                 sizeof(buf_noshow), &dpb);
    check_eq("默认：show=0 与 show=1 帧头等长（被强制置 1）",
             (long)f_noshow, (long)f_show);
    if (f_show > 0 && f_show == f_noshow) {
        check_eq("默认：两种 PPB 帧头逐字节相同",
                 (long)memcmp(buf_show, buf_noshow, f_show), 0);
        check_eq("默认：载荷 bit4 恒为 1",
                 (long)((buf_noshow[PAYLOAD] >> 4) & 1u), 1);
    }
}



/* ------------------------------------------- refresh_frame_flags 的位偏移不变量
 *
 * 背景：AV1 剩余缺陷里有 5 帧（150 帧样本的第 30/60/90/120/150）
 * refresh_frame_flags 改写不正确。测绘数据是：
 *     70 帧 last_refresh_bitpos=50，这 5 帧 =58，差 **8 位**
 * 而改写用的正是这个 bitpos，偏了就写到别的字段上。
 *
 * 本测试把影响该偏移的各因素钉住，并记录已排除的可能：
 *   frame_type / show_frame / error_resilient_mode 三者的组合，
 *   最大差值只有 3 位（primary_ref_frame 的宽度），**凑不出 8**。
 * 所以那 8 位差不来自这三项 —— 缩小了后续排查范围。
 *
 * ⚠️ 这里的绝对值（15~19）比真实码流的 50/58 小很多，因为最小构造
 * 缺少真实序列头带来的前置字段。测的是**差值关系**而非绝对偏移，
 * 绝对值随实现调整会变，差值关系由规范 5.9.2 固定。 */
static void test_refresh_bitpos_invariants(void)
{
    printf("refresh_frame_flags 位偏移的不变量（规范 5.9.2）\n");

    /* 本用例遍历 show_frame 的 0/1 两种取值，断言的是**规范分支**下的
     * 位偏移关系。驱动默认把 show_frame 恒置 1（见 build_frame_header），
     * 那会让 KEY(show=0) 走进 KEY+show 的 refresh_all 分支、
     * 整个 show=0 维度全部塌缩成 show=1 —— 断言就失去意义。
     * 所以先开 A/B 开关还原规范行为。默认行为由
     * test_frame_header_show_frame 末尾单独钉住。 */
    setenv("DMD_AV1_NO_SHOWFORCE", "1", 1);

    struct dmd_av1_dpb dpb;
    VADecPictureParameterBufferAV1 pic;
    unsigned char buf[512];

    /* 返回 last_refresh_bitpos；(size_t)-1 表示该帧不写该字段。 */
    size_t bp[4][2][2];
    for (int ft = 0; ft < 4; ft++)
        for (int show = 0; show < 2; show++)
            for (int er = 0; er < 2; er++) {
                memset(&dpb, 0, sizeof(dpb));
                memset(&pic, 0, sizeof(pic));
                dpb.last_refresh_bitpos = (size_t)-1;
                pic.frame_width_minus1 = 1919;
                pic.frame_height_minus1 = 1079;
                pic.order_hint = 4;
                pic.order_hint_bits_minus_1 = 6;
                pic.primary_ref_frame = 0;
                pic.pic_info_fields.bits.frame_type = (unsigned)ft;
                pic.pic_info_fields.bits.show_frame = (unsigned)show;
                pic.pic_info_fields.bits.showable_frame = 1;
                pic.pic_info_fields.bits.error_resilient_mode = (unsigned)er;
                pic.seq_info_fields.fields.enable_order_hint = 1;
                dmd_av1_build_frame_header(&pic, buf, sizeof(buf), &dpb);
                bp[ft][show][er] = dpb.last_refresh_bitpos;
            }

    /* KEY+show 与 SWITCH 走 refresh_all 路径，不写该字段 —— 必须是 -1。
     * 这条很重要：若它们误写了，改写就会落到一个不存在的字段上。 */
    check_eq("KEY+show 不写 refresh 字段",
             (long)(bp[0][1][0] == (size_t)-1), 1);
    check_eq("SWITCH 不写 refresh 字段",
             (long)(bp[3][1][0] == (size_t)-1), 1);
    check_eq("SWITCH(show=0) 也不写",
             (long)(bp[3][0][0] == (size_t)-1), 1);

    /* 会写该字段的帧类型必须给出有效偏移。 */
    check_eq("INTER 写 refresh 字段",
             (long)(bp[1][1][0] != (size_t)-1), 1);
    check_eq("INTRA_ONLY 写 refresh 字段",
             (long)(bp[2][1][0] != (size_t)-1), 1);
    check_eq("KEY(show=0) 写 refresh 字段",
             (long)(bp[0][0][0] != (size_t)-1), 1);

    /* error_resilient_mode 省掉 primary_ref_frame(3 位)，所以 er=1 少 3 位。
     * 这是规范 5.9.2 的条件 !intra_only && !error_resilient_mode。 */
    check_eq("INTER: er=1 比 er=0 少 3 位",
             (long)(bp[1][1][0] - bp[1][1][1]), 3);

    /* intra_only 同样跳过 primary_ref_frame，与 INTER(er=0) 差 3 位。 */
    check_eq("INTRA_ONLY 比 INTER(er=0) 少 3 位",
             (long)(bp[1][1][0] - bp[2][1][0]), 3);

    /* ⚠️ 关键结论：上面所有组合的最大差值是 3，**凑不出实测的 8 位**。
     * 所以第 30/60/90/120/150 帧那 8 位偏移不来自 frame_type /
     * show_frame / error_resilient_mode 中的任何一个。
     * 这条断言把它固定下来，防止后续误以为是这三项之一。 */
    size_t mx = 0, mn = (size_t)-1;
    for (int ft = 0; ft < 4; ft++)
        for (int show = 0; show < 2; show++)
            for (int er = 0; er < 2; er++) {
                size_t v = bp[ft][show][er];
                if (v == (size_t)-1) continue;
                if (v > mx) mx = v;
                if (v < mn) mn = v;
            }
    if (mx - mn >= 8) {
        fails++;
        printf("  ✗ 三因素已能造成 %zu 位差（>=8），"
               "则实测的 8 位差可能就是它们，需重新排查\n", mx - mn);
    }

    unsetenv("DMD_AV1_NO_SHOWFORCE");
}

/* -------------------------------------------------------------- 全局运动 */

/* 在按位写的载荷里找一段连续的位，返回起始位下标；找不到返回 -1。 */
static long find_bits(const unsigned char *buf, size_t n, const char *pat)
{
    const size_t plen = strlen(pat);
    for (size_t i = 0; i + plen <= n * 8; i++) {
        size_t k = 0;
        while (k < plen && bit_at(buf, n, i + k) == (long)(pat[k] - '0'))
            k++;
        if (k == plen)
            return (long)i;
    }
    return -1;
}

static void check_bits(const char *what, const unsigned char *buf, size_t n,
                       const char *pat)
{
    if (find_bits(buf, n, pat) >= 0)
        return;
    fails++;
    printf("  ✗ %s：码流里找不到 %zu 位模式\n      %s\n",
           what, strlen(pat), pat);
}

static void check_no_bits(const char *what, const unsigned char *buf, size_t n,
                          const char *pat)
{
    long at = find_bits(buf, n, pat);
    if (at < 0)
        return;
    fails++;
    printf("  ✗ %s：码流第 %ld 位出现了不该有的模式\n      %s\n", what, at, pat);
}

/* 一个最小可用的帧间帧参数（全局运动只在帧间帧出现）。
 * ref_frame_map 全 0xff、dpb 全零：my_idx[] 会算出无意义的槽号，但
 * ① prev=默认值的用例根本不查表；② prev=非默认的用例把**八个槽都填成
 * 同一份**参数，槽号落在哪儿都一样。所以本用例只考编码算法，不考槽翻译
 * （槽翻译由 test_patch_prev_refresh / 硬件回归覆盖）。 */
static void gm_pic(VADecPictureParameterBufferAV1 *pic, int hp)
{
    memset(pic, 0, sizeof(*pic));
    pic->frame_width_minus1  = 63;
    pic->frame_height_minus1 = 63;
    pic->order_hint = 2;
    pic->order_hint_bits_minus_1 = 6;
    pic->pic_info_fields.bits.frame_type = 1;          /* INTER_FRAME */
    pic->pic_info_fields.bits.show_frame = 1;
    pic->pic_info_fields.bits.allow_high_precision_mv = (unsigned)hp;
    pic->seq_info_fields.fields.enable_order_hint = 1;
}

static void gm_set(VADecPictureParameterBufferAV1 *pic, int i, int type,
                   const int32_t m[6])
{
    pic->wm[i].wmtype = (VAAV1TransformationType)type;
    pic->wm[i].invalid = 0;
    for (int j = 0; j < 6; j++)
        pic->wm[i].wmmat[j] = m[j];
}

/* 期望位串的来源（三重复核，任何一处出错都会被另两处抓到）：
 *   1) "1111110110110…" 这 109 位**逐字抄自源码流的 ffmpeg CBS trace**
 *      （av1work/cov/orig_tr.txt 第 158~266 行的位列），即 aom 编码器自己写的位。
 *      语法：ref1 是 ROTZOOM(1,1) + 符号 118/31/936/2873，ref2、ref3 无，
 *      ref4 是 ROTZOOM + 符号 2/5/512/1541，ref5~7 无。
 *   2) 另用一个独立写的读侧（av1work/gmbits.py）把这串位解回
 *      118/31/936/2873、2/5/512/1541，与 CBS 打印的符号一致。
 *   3) 硬件实测：该片段 24 帧输出与软解逐字节相同。
 * 其余三串（diff/hp0/fallback）由同一份 Python 生成并做写→读 round-trip。 */
static const char GM_SRC[] =
    "11111101101101101111111111101101010001111111110011001110010011001001"
    "01111111100000000001111111101000000101000";
static const char GM_DIFF[] =
    "10100011000010011111111100000000000011111111101000000000011111111100"
    "1000000000111001000111011110111000110";
static const char GM_HP0[] = "10101100001";
static const char GM_FALLBACK[] = "010100100001";
/* GM_SRC 里 ref1 那一段（58 位）：用例 4 的 wm[0] 就是它加了一个 LSB，
 * 用来断言"没有被四舍五入到最近格点后照样写出去"。 */
static const char GM_M0[] =
    "1111110110110110111111111110110101000111111111001100111001";

static void test_global_motion(void)
{
    printf("全局运动 global_motion_params（规范 5.9.24 / 7.11.3.7）\n");

    static const int32_t M0[6] = { 479232, -1471488, 65654, -32, 32, 65654 };
    static const int32_t M3[6] = { 262144, -789504, 65538, -6, 6, 65538 };
    /* TRANSLATION(hp=1) 格点 1<<13、(hp=0) 1<<14；ROTZOOM/AFFINE 的 idx0/1
     * 格点 1<<10、idx2..5 格点 1<<1。不在格点上的值实现会退回 IDENTITY。 */
    static const int32_t D_TR[6] = { 40960, -16384, 0, 0, 0, 65536 };
    static const int32_t D_AF[6] = { 1024, -3072, 65536, 2048, -2048, 65536 };
    static const int32_t H_TR[6] = { 49152, -16384, 0, 0, 0, 65536 };
    static const int32_t F_TR[6] = { 8192, -8192, 0, 0, 0, 65536 };
    static const int32_t PREV[6] = { 32768, 16384, 67584, -1024, 512, 65576 };

    unsigned char buf[1024];
    struct dmd_av1_dpb dpb;
    VADecPictureParameterBufferAV1 pic;
    size_t n;

    /* --- 1) prev = 默认值（primary_ref_frame = NONE），与源码流同值 --- */
    gm_pic(&pic, 1);
    pic.primary_ref_frame = 7;                    /* PRIMARY_REF_NONE */
    gm_set(&pic, 0, VAAV1TransformationRotzoom, M0);
    gm_set(&pic, 3, VAAV1TransformationRotzoom, M3);
    memset(&dpb, 0, sizeof(dpb));
    n = dmd_av1_build_frame_header(&pic, buf, sizeof(buf), &dpb);
    if (n == 0) {
        fails++;
        printf("  ✗ 帧头合成返回 0，用例未覆盖目标\n");
        return;
    }
    check_bits("prev=默认时应写出与源码流逐位相同的 gm 语法", buf, n, GM_SRC);

    /* --- 2) prev = 非默认值：查表按槽取 prev，符号是差分结果 --- */
    gm_pic(&pic, 1);
    pic.primary_ref_frame = 0;
    gm_set(&pic, 0, VAAV1TransformationTranslation, D_TR);
    gm_set(&pic, 1, VAAV1TransformationAffine, D_AF);
    memset(&dpb, 0, sizeof(dpb));
    for (int k = 0; k < 8; k++)
        for (int i = 0; i < 7; i++)
            for (int j = 0; j < 6; j++)
                dpb.gm_slot[k].p[i][j] = PREV[j];
    n = dmd_av1_build_frame_header(&pic, buf, sizeof(buf), &dpb);
    if (n == 0) {
        fails++;
        printf("  ✗ 差分用例帧头合成返回 0\n");
        return;
    }
    check_bits("prev=非默认时应按差分写符号（ROTZOOM/AFFINE 顺序 [2][3][0][1]/[2][3][4][5][0][1]）",
               buf, n, GM_DIFF);

    /* --- 3) hp=0 的 TRANSLATION：abs_bits 8 / prec_bits 2，档位算错就全错 --- */
    gm_pic(&pic, 0);
    pic.primary_ref_frame = 7;
    gm_set(&pic, 0, VAAV1TransformationTranslation, H_TR);
    memset(&dpb, 0, sizeof(dpb));
    n = dmd_av1_build_frame_header(&pic, buf, sizeof(buf), &dpb);
    check_bits("hp=0 TRANSLATION 应走 9-1/3-1 那一档", buf, n, GM_HP0);

    /* --- 4) 编不出来的值必须整参考帧退回 IDENTITY，且不留半截 ---
     * wm[0] 的 wmmat[0] 比格点偏 1（479232+1 不能被 1024 整除）；
     * wm[1] 是合法的 TRANSLATION。退回正确时码流是 "0" + wm[1] 的位串，
     * 若写了一半参数，wm[1] 的位串就不会紧贴在 is_global=0 之后。 */
    gm_pic(&pic, 1);
    pic.primary_ref_frame = 7;
    {
        int32_t bad[6];
        memcpy(bad, M0, sizeof(bad));
        bad[0] += 1;
        gm_set(&pic, 0, VAAV1TransformationRotzoom, bad);
    }
    gm_set(&pic, 1, VAAV1TransformationTranslation, F_TR);
    memset(&dpb, 0, sizeof(dpb));
    n = dmd_av1_build_frame_header(&pic, buf, sizeof(buf), &dpb);
    check_bits("不可编码的 gm 应整帧退回 IDENTITY 且不写半截参数",
               buf, n, GM_FALLBACK);
    check_no_bits("退回时不得把越界值四舍五入后照常写出（那会让硬件重建出错误矩阵）",
                  buf, n, GM_M0);
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
    test_refs_resolvable();
    test_frame_header_show_frame();
    test_refresh_bitpos_invariants();
    test_global_motion();

    if (fails == 0) {
        printf("=== 全部通过 ===\n");
        return 0;
    }
    printf("=== %d 项失败 ===\n", fails);
    return 1;
}
