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

    if (fails == 0) {
        printf("=== 全部通过 ===\n");
        return 0;
    }
    printf("=== %d 项失败 ===\n", fails);
    return 1;
}
