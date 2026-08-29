#!/usr/bin/env python3
"""
AV1 位级解析/比对器 —— 辅助排查 OBU 反向合成的字段错位。

用法:
    python3 av1_parse.py <file.obu>          逐字段打印
    python3 av1_parse.py <a.obu> <b.obu>     两份码流逐字段比对

⚠️ 重要局限（吃过教训，务必先读）：
本脚本**不是权威判据**。它是手写实现，自身可能与规范有偏差，
且历史上出现过"解析器的错误与驱动的错误互相抵消、显得已对齐"
的情况，导致三四轮排查走错方向。

定位单个错误比特时，权威工具是 ffmpeg 自带的 trace_headers：

    ffmpeg -loglevel trace -f obu -i <file> -c copy \
           -bsf:v trace_headers -f null - 2>&1 | grep -a "trace_headers"

它输出「位偏移 + 字段名 + 二进制串 + 值」，可直接与真实码流 diff。
本脚本的价值在于快速概览与批量比对，用它形成假设、用 trace_headers 定论。
"""
import sys


class BitReader:
    def __init__(self, data):
        self.d = data
        self.pos = 0                      # 位偏移

    def f(self, n):
        """f(n)：MSB 优先读 n 位（规范 4.10.2）。"""
        v = 0
        for _ in range(n):
            byte = self.pos >> 3
            if byte >= len(self.d):
                raise EOFError(f"读越界于位 {self.pos}")
            bit = (self.d[byte] >> (7 - (self.pos & 7))) & 1
            v = (v << 1) | bit
            self.pos += 1
        return v

    def su(self, n):
        """su(n)：n 位补码（规范 4.10.6）。"""
        v = self.f(n)
        sign = 1 << (n - 1)
        return v - (1 << n) if v & sign else v

    def ns(self, n):
        """ns(n)：非对称编码（规范 4.10.7）。"""
        if n <= 1:
            return 0
        w = n.bit_length()
        m = (1 << w) - n
        v = self.f(w - 1)
        if v < m:
            return v
        return ((v << 1) | self.f(1)) - m

    def leb128(self):
        v = 0
        for i in range(8):
            b = self.f(8)
            v |= (b & 0x7F) << (i * 7)
            if not (b & 0x80):
                break
        return v

    def aligned(self):
        return (self.pos & 7) == 0

    def byte_align(self):
        while not self.aligned():
            self.f(1)


class Fields:
    """收集字段名与值，供比对使用。"""
    def __init__(self, quiet=False):
        self.items = []
        self.quiet = quiet

    def add(self, name, val):
        self.items.append((name, val))
        if not self.quiet:
            print(f"    {name:36s} = {val}")
        return val

    def get(self, name, default=None):
        for n, v in self.items:
            if n == name:
                return v
        return default


def parse_sequence_header(br, F):
    """sequence_header_obu()，规范 5.5.1。"""
    prof = F.add("seq_profile", br.f(3))
    F.add("still_picture", br.f(1))
    reduced = F.add("reduced_still_picture_header", br.f(1))
    if reduced:
        F.add("seq_level_idx[0]", br.f(5))
    else:
        timing = F.add("timing_info_present_flag", br.f(1))
        if timing:
            raise NotImplementedError("timing_info 未实现解析")
        F.add("initial_display_delay_present_flag", br.f(1))
        cnt = F.add("operating_points_cnt_minus_1", br.f(5))
        for i in range(cnt + 1):
            F.add(f"operating_point_idc[{i}]", br.f(12))
            lvl = F.add(f"seq_level_idx[{i}]", br.f(5))
            if lvl > 7:
                F.add(f"seq_tier[{i}]", br.f(1))
    wb = F.add("frame_width_bits_minus_1", br.f(4))
    hb = F.add("frame_height_bits_minus_1", br.f(4))
    F.add("max_frame_width_minus_1", br.f(wb + 1))
    F.add("max_frame_height_minus_1", br.f(hb + 1))
    fid = 0
    if not reduced:
        fid = F.add("frame_id_numbers_present_flag", br.f(1))
    if fid:
        F.add("delta_frame_id_length_minus_2", br.f(4))
        F.add("additional_frame_id_length_minus_1", br.f(3))
    F.add("use_128x128_superblock", br.f(1))
    F.add("enable_filter_intra", br.f(1))
    F.add("enable_intra_edge_filter", br.f(1))
    if not reduced:
        F.add("enable_interintra_compound", br.f(1))
        F.add("enable_masked_compound", br.f(1))
        F.add("enable_warped_motion", br.f(1))
        F.add("enable_dual_filter", br.f(1))
        eoh = F.add("enable_order_hint", br.f(1))
        if eoh:
            F.add("enable_jnt_comp", br.f(1))
            F.add("enable_ref_frame_mvs", br.f(1))
        csc = F.add("seq_choose_screen_content_tools", br.f(1))
        force_sct = 2 if csc else F.add("seq_force_screen_content_tools", br.f(1))
        if force_sct > 0:
            cim = F.add("seq_choose_integer_mv", br.f(1))
            if not cim:
                F.add("seq_force_integer_mv", br.f(1))
        if eoh:
            F.add("order_hint_bits_minus_1", br.f(3))
    F.add("enable_superres", br.f(1))
    F.add("enable_cdef", br.f(1))
    F.add("enable_restoration", br.f(1))
    # color_config()，规范 5.5.2
    hbd = F.add("high_bitdepth", br.f(1))
    if prof == 2 and hbd:
        F.add("twelve_bit", br.f(1))
    mono = 0 if prof == 1 else F.add("mono_chrome", br.f(1))
    cdp = F.add("color_description_present_flag", br.f(1))
    if cdp:
        F.add("color_primaries", br.f(8))
        F.add("transfer_characteristics", br.f(8))
        F.add("matrix_coefficients", br.f(8))
    if mono:
        F.add("color_range", br.f(1))
    else:
        F.add("color_range", br.f(1))
        if prof == 0:
            sx = sy = 1
        elif prof == 1:
            sx = sy = 0
        else:
            if hbd and F.get("twelve_bit"):
                sx = F.add("subsampling_x", br.f(1))
                sy = F.add("subsampling_y", br.f(1)) if sx else 0
            else:
                sx, sy = 1, 0
        if sx and sy:
            F.add("chroma_sample_position", br.f(2))
        F.add("separate_uv_delta_q", br.f(1))
    F.add("film_grain_params_present", br.f(1))
    return F


def parse_uncompressed_header(br, F, S):
    """uncompressed_header()，规范 5.9.2。S 是序列头的 Fields（提供能力位）。"""
    def sq(name, default=0):
        v = S.get(name, default)
        return v if v is not None else default

    fid_present = sq("frame_id_numbers_present_flag")
    eoh = sq("enable_order_hint")
    ohb = sq("order_hint_bits_minus_1") + 1 if eoh else 0
    en_superres = sq("enable_superres")
    en_cdef = sq("enable_cdef")
    en_restoration = sq("enable_restoration")
    mono = sq("mono_chrome")
    sub_x = 1 if sq("seq_profile") == 0 else sq("subsampling_x", 1)
    sub_y = 1 if sq("seq_profile") == 0 else sq("subsampling_y", 1)
    use128 = sq("use_128x128_superblock")
    force_sct = 2 if sq("seq_choose_screen_content_tools") else \
        sq("seq_force_screen_content_tools")

    F.add("show_existing_frame", br.f(1))
    if F.get("show_existing_frame"):
        F.add("frame_to_show_map_idx", br.f(3))
        return F
    ft = F.add("frame_type", br.f(2))
    intra = ft in (0, 2)               # KEY_FRAME / INTRA_ONLY_FRAME
    show = F.add("show_frame", br.f(1))
    if not show:
        F.add("showable_frame", br.f(1))
    if ft == 3 or (ft == 0 and show):
        err_res = 1                   # 恒 1，不写入
    else:
        err_res = F.add("error_resilient_mode", br.f(1))
    F.add("disable_cdf_update", br.f(1))
    if force_sct == 2:
        asct = F.add("allow_screen_content_tools", br.f(1))
    else:
        asct = force_sct
    if asct:
        if sq("seq_choose_integer_mv"):
            F.add("force_integer_mv", br.f(1))
    if fid_present:
        F.add("current_frame_id", br.f(1))   # 简化
    frame_size_override = 1 if ft == 3 else F.add("frame_size_override_flag",
                                                  br.f(1))
    if eoh:
        F.add("order_hint", br.f(ohb))
    # refresh_frame_flags：KEY+show 时恒 allFrames、不写
    refresh_all = (ft == 0 and show)
    if not intra or not refresh_all:
        if not (ft == 0 and show):
            pass
    if not (intra and refresh_all):
        pass
    # primary_ref_frame
    if not intra and not err_res:
        F.add("primary_ref_frame", br.f(3))
    if not (ft == 0 and show):
        F.add("refresh_frame_flags", br.f(8))
    # 参考帧
    if not intra:
        if eoh:
            short = F.add("frame_refs_short_signaling", br.f(1))
            if short:
                F.add("last_frame_idx", br.f(3))
                F.add("gold_frame_idx", br.f(3))
        else:
            short = 0
        if not short:
            for i in range(7):
                F.add(f"ref_frame_idx[{i}]", br.f(3))
    # frame_size / render_size
    if frame_size_override:
        F.add("frame_width_minus_1", br.f(sq("frame_width_bits_minus_1") + 1))
        F.add("frame_height_minus_1", br.f(sq("frame_height_bits_minus_1") + 1))
    if en_superres:
        us = F.add("use_superres", br.f(1))
        if us:
            F.add("coded_denom", br.f(3))
    F.add("render_and_frame_size_different", br.f(1))
    if F.get("render_and_frame_size_different"):
        F.add("render_width_minus_1", br.f(16))
        F.add("render_height_minus_1", br.f(16))
    # allow_intrabc（CBS uncompressed_header 原文）：
    #   if (allow_screen_content_tools && upscaled_width == frame_width)
    #       flag(allow_intrabc);
    #   else infer(allow_intrabc, 0)
    # upscaled == frame 等价于本帧未走 superres 上采样。
    # allow_intrabc：经 ffmpeg trace_headers 校准 —— 真实码流在
    # allow_screen_content_tools=0 时该位**不出现**（位 46 是 render_and_...，
    # 位 47 直接是 disable_frame_end_update_cdf）。
    if intra:
        if asct and not F.get("use_superres", 0):
            F.add("allow_intrabc", br.f(1))
        else:
            F.add("→ allow_intrabc(infer)", 0)
    if not intra:
        F.add("allow_high_precision_mv", br.f(1))
        sw = F.add("is_filter_switchable", br.f(1))
        if not sw:
            F.add("interpolation_filter", br.f(2))
        F.add("is_motion_mode_switchable", br.f(1))
        if not err_res and eoh:
            F.add("use_ref_frame_mvs", br.f(1))
    if not F.get("disable_cdf_update"):
        F.add("disable_frame_end_update_cdf", br.f(1))
    # tile_info()，规范 5.9.15
    w = sq("max_frame_width_minus_1") + 1
    h = sq("max_frame_height_minus_1") + 1
    mi_cols = 2 * ((w + 7) >> 3)
    mi_rows = 2 * ((h + 7) >> 3)
    sb_shift = 5 if use128 else 4
    sb_size = sb_shift + 2          # ⚠ CBS: sb_size = sb_shift + 2
    sb_cols = ((mi_cols + 31) >> 5) if use128 else ((mi_cols + 15) >> 4)
    sb_rows = ((mi_rows + 31) >> 5) if use128 else ((mi_rows + 15) >> 4)
    uniform = F.add("uniform_tile_spacing_flag", br.f(1))
    max_tw_sb = 4096 >> sb_size
    max_ta_sb = (4096 * 2304) >> (2 * sb_size)

    def tlog2(blk, tgt):
        k = 0
        while (blk << k) < tgt:
            k += 1
        return k

    min_l2_cols = tlog2(max_tw_sb, sb_cols)
    max_l2_cols = tlog2(1, min(sb_cols, 64))
    max_l2_rows = tlog2(1, min(sb_rows, 64))
    min_l2_tiles = max(min_l2_cols, tlog2(max_ta_sb, sb_rows * sb_cols))
    if uniform:
        cl = min_l2_cols
        while cl < max_l2_cols:
            if br.f(1) == 0:
                break
            cl += 1
        F.add("→ tile_cols_log2", cl)
        min_l2_rows = max(min_l2_tiles - cl, 0)
        rl = min_l2_rows
        while rl < max_l2_rows:
            if br.f(1) == 0:
                break
            rl += 1
        F.add("→ tile_rows_log2", rl)
    else:
        start = 0
        i = 0
        while start < sb_cols:
            F.add(f"width_in_sbs_minus_1[{i}]", br.ns(min(sb_cols - start,
                                                          max_tw_sb)))
            start += F.get(f"width_in_sbs_minus_1[{i}]") + 1
            i += 1
        cl = tlog2(1, i)
        F.add("→ TileCols", i)
        start = 0
        i = 0
        while start < sb_rows:
            F.add(f"height_in_sbs_minus_1[{i}]", br.ns(sb_rows - start))
            start += F.get(f"height_in_sbs_minus_1[{i}]") + 1
            i += 1
        rl = tlog2(1, i)
        F.add("→ TileRows", i)
    if cl + rl > 0:
        F.add("context_update_tile_id", br.f(cl + rl))
        F.add("tile_size_bytes_minus_1", br.f(2))
    # quantization_params()，规范 5.9.12
    F.add("base_q_idx", br.f(8))

    def delta_q():
        return br.su(7) if br.f(1) else 0

    F.add("DeltaQYDc", delta_q())
    if not mono:
        if S.get("separate_uv_delta_q"):
            diff = F.add("diff_uv_delta", br.f(1))
        else:
            diff = 0
        F.add("DeltaQUDc", delta_q())
        F.add("DeltaQUAc", delta_q())
        if diff:
            F.add("DeltaQVDc", delta_q())
            F.add("DeltaQVAc", delta_q())
    uq = F.add("using_qmatrix", br.f(1))
    if uq:
        F.add("qm_y", br.f(4))
        F.add("qm_u", br.f(4))
        if S.get("separate_uv_delta_q"):
            F.add("qm_v", br.f(4))

    # segmentation_params()，规范 5.9.14
    seg = F.add("segmentation_enabled", br.f(1))
    if seg:
        primary_none = intra or err_res
        if not primary_none:
            um = F.add("segmentation_update_map", br.f(1))
            if um:
                F.add("segmentation_temporal_update", br.f(1))
            ud = F.add("segmentation_update_data", br.f(1))
        else:
            um = ud = 1
        if ud:
            bits = [8, 6, 6, 6, 6, 3, 0, 0]
            sgn = [1, 1, 1, 1, 1, 0, 0, 0]
            for i in range(8):
                for j in range(8):
                    on = br.f(1)
                    if on and bits[j]:
                        if sgn[j]:
                            br.su(bits[j] + 1)
                        else:
                            br.f(bits[j])
            F.add("→ segmentation data 已读", 1)

    # CodedLossless 推导（规范 7.12.1）
    q = F.get("base_q_idx")
    lossless = (q == 0 and F.get("DeltaQYDc") == 0 and
                F.get("DeltaQUDc", 0) == 0 and F.get("DeltaQUAc", 0) == 0)

    # delta_q_params()，规范 5.9.17
    if q > 0:
        dq = F.add("delta_q_present", br.f(1))
        if dq:
            F.add("delta_q_res", br.f(2))
    else:
        dq = 0
    # delta_lf_params()，规范 5.9.18
    if dq:
        if not F.get("allow_intrabc", 0):
            dlf = F.add("delta_lf_present", br.f(1))
            if dlf:
                F.add("delta_lf_res", br.f(2))
                F.add("delta_lf_multi", br.f(1))

    # loop_filter_params()，规范 5.9.11
    if not lossless and not F.get("allow_intrabc", 0):
        lf0 = F.add("loop_filter_level[0]", br.f(6))
        lf1 = F.add("loop_filter_level[1]", br.f(6))
        if not mono and (lf0 or lf1):
            F.add("loop_filter_level[2]", br.f(6))
            F.add("loop_filter_level[3]", br.f(6))
        F.add("loop_filter_sharpness", br.f(3))
        de = F.add("loop_filter_delta_enabled", br.f(1))
        if de:
            du = F.add("loop_filter_delta_update", br.f(1))
            if du:
                for i in range(8):
                    if br.f(1):
                        br.su(7)
                for i in range(2):
                    if br.f(1):
                        br.su(7)
                F.add("→ lf deltas 已读", 1)

    # cdef_params()，规范 5.9.19
    if not lossless and not F.get("allow_intrabc", 0) and en_cdef:
        F.add("cdef_damping_minus_3", br.f(2))
        cb = F.add("cdef_bits", br.f(2))
        for i in range(1 << cb):
            F.add(f"cdef_y_pri[{i}]", br.f(4))
            F.add(f"cdef_y_sec[{i}]", br.f(2))
            if not mono:
                F.add(f"cdef_uv_pri[{i}]", br.f(4))
                F.add(f"cdef_uv_sec[{i}]", br.f(2))

    # lr_params()，规范 5.9.20
    all_lossless = lossless and not F.get("use_superres", 0)
    if not all_lossless and not F.get("allow_intrabc", 0) and en_restoration:
        rt = [F.add(f"lr_type[{i}]", br.f(2)) for i in range(1 if mono else 3)]
        if any(rt):
            F.add("lr_unit_shift", br.f(1))
            if not use128 and F.get("lr_unit_shift"):
                F.add("lr_unit_extra_shift", br.f(1))
            if sub_x and sub_y and len(rt) > 1 and (rt[1] or rt[2]):
                F.add("lr_uv_shift", br.f(1))

    # read_tx_mode()，规范 5.9.21
    if not lossless:
        F.add("tx_mode_select", br.f(1))

    # frame_reference_mode()，规范 5.9.23
    if not intra:
        F.add("reference_select", br.f(1))

    # skip_mode_params()，规范 5.9.22
    if not intra and F.get("reference_select", 0):
        F.add("skip_mode_present", br.f(1))

    if not intra and F.get("is_motion_mode_switchable", 0) and not err_res:
        F.add("allow_warped_motion", br.f(1))
    F.add("reduced_tx_set", br.f(1))

    # global_motion_params()，规范 5.9.24
    if not intra:
        for i in range(7):
            F.add(f"is_global[{i}]", br.f(1))

    # film_grain_params()，规范 5.9.30
    if S.get("film_grain_params_present") and (show or F.get("showable_frame", 0)):
        F.add("apply_grain", br.f(1))

    F.add("→ trailing 前位置", br.pos)
    one = br.f(1)
    F.add("→ trailing_one_bit", one)
    return F


def parse_obus(data, label, quiet=False):
    """遍历 OBU，解析序列头；返回 {obu_type: Fields}。"""
    out = {}
    i = 0
    names = {1: "SEQ_HDR", 2: "TD", 3: "FRAME_HDR", 4: "TILE_GRP",
             5: "METADATA", 6: "FRAME", 7: "REDUNDANT_FH", 15: "PADDING"}
    while i < len(data):
        h = data[i]
        if (h >> 7) & 1:
            print(f"  ✗ {label}: 位 {i*8} 的 forbidden_bit=1，不是合法 OBU 头")
            return out
        t = (h >> 3) & 0xF
        ext = (h >> 2) & 1
        has_size = (h >> 1) & 1
        j = i + 1
        if ext:
            j += 1
        size = 0
        if has_size:
            br = BitReader(data[j:j + 8])
            size = br.leb128()
            j += (br.pos >> 3)
        else:
            size = len(data) - j
        if not quiet:
            print(f"  [{label}] OBU type={t} ({names.get(t,'?')}) "
                  f"size={size} @byte {i}")
        payload = data[j:j + size]
        if t == 1:
            F = Fields(quiet)
            try:
                parse_sequence_header(BitReader(payload), F)
                out[1] = F
            except Exception as e:
                print(f"    ✗ 序列头解析失败: {e}")
        elif t in (3, 6) and 1 in out:
            F = Fields(quiet)
            try:
                parse_uncompressed_header(BitReader(payload), F, out[1])
                out[3] = F
            except Exception as e:
                print(f"    ✗ 帧头解析失败于字段后: {e}")
                out[3] = F
        i = j + size
    return out


def compare(fa, fb, la, lb):
    """并排比对两组字段，指出首个不一致。"""
    na = {n: v for n, v in fa.items}
    nb = {n: v for n, v in fb.items}
    diffs = 0
    print(f"  {'字段':36s} {la:>12s} {lb:>12s}")
    for n, v in fa.items:
        w = nb.get(n, "—")
        mark = "" if w == v else "  ← 不一致"
        if mark:
            diffs += 1
        print(f"  {n:36s} {str(v):>12s} {str(w):>12s}{mark}"
              if isinstance(v, str) else
              f"  {n:36s} {v:>12} {str(w):>12}{mark}")
    only_b = [n for n, _ in fb.items if n not in na]
    if only_b:
        print(f"  仅 {lb} 有的字段: {', '.join(only_b)}")
    return diffs


def main():
    if len(sys.argv) == 2:
        data = open(sys.argv[1], "rb").read()
        print(f"=== 解析 {sys.argv[1]} ({len(data)} 字节) ===")
        parse_obus(data, "in")
        return 0
    if len(sys.argv) == 3:
        a = open(sys.argv[1], "rb").read()
        b = open(sys.argv[2], "rb").read()
        print(f"=== 比对 ===")
        fa = parse_obus(a, "A", quiet=True)
        fb = parse_obus(b, "B", quiet=True)
        for t in sorted(set(fa) | set(fb)):
            if t in fa and t in fb:
                print(f"\n--- OBU type {t} ---")
                d = compare(fa[t], fb[t], "A", "B")
                print(f"  差异 {d} 处")
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
