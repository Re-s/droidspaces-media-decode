#!/usr/bin/env python3
"""把 Annex B 码流里的 SPS 改写成"去掉 VUI、其余比特完全不变"的版本。

目的：隔离验证 MediaCodec 是否真的需要 SPS 里的 VUI。
若替换后仍能逐字节正确解码，说明合成 SPS 不写 VUI 是安全的，
那么 H.264 路径 PSNR 11.5dB 的根因就在别的字段上，而不是缺 VUI。
"""
import re
import sys


def unescape(b):
    out = bytearray()
    i = 0
    while i < len(b):
        if i + 2 < len(b) and b[i] == 0 and b[i + 1] == 0 and b[i + 2] == 3:
            out += b[i:i + 2]
            i += 3
        else:
            out.append(b[i])
            i += 1
    return bytes(out)


def escape(b):
    out = bytearray()
    zeros = 0
    for x in b:
        if zeros == 2 and x <= 3:
            out.append(3)
            zeros = 0
        out.append(x)
        zeros = zeros + 1 if x == 0 else 0
    return bytes(out)


class BR:
    def __init__(self, d, start_bit=0):
        self.d = d
        self.p = start_bit

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.d[self.p >> 3] >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v

    def ue(self):
        z = 0
        while self.u(1) == 0:
            z += 1
        return (1 << z) - 1 + (self.u(z) if z else 0)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k % 2 else -(k // 2)


class BW:
    def __init__(self):
        self.bits = []

    def u(self, v, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def ue(self, v):
        v += 1
        n = v.bit_length()
        self.u(0, n - 1)
        self.u(v, n)

    def trailing(self):
        self.bits.append(1)
        while len(self.bits) % 8:
            self.bits.append(0)

    def bytes(self):
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            byte = 0
            for b in self.bits[i:i + 8]:
                byte = (byte << 1) | b
            out.append(byte)
        return bytes(out)


def rewrite_sps_without_vui(sps_nalu):
    """输入含 NAL header 的 SPS NALU（无起始码），返回去 VUI 后的 NALU。"""
    rbsp = unescape(sps_nalu)
    g = BR(rbsp, 8)  # 跳过 NAL header 字节
    w = BW()

    profile_idc = g.u(8)
    w.u(profile_idc, 8)
    w.u(g.u(8), 8)              # constraint_set flags + reserved
    w.u(g.u(8), 8)              # level_idc
    w.ue(g.ue())                # sps_id

    if profile_idc in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        cfi = g.ue()
        w.ue(cfi)
        if cfi == 3:
            w.u(g.u(1), 1)      # separate_colour_plane_flag
        w.ue(g.ue())            # bit_depth_luma_minus8
        w.ue(g.ue())            # bit_depth_chroma_minus8
        w.u(g.u(1), 1)          # qpprime_y_zero_transform_bypass_flag
        smp = g.u(1)
        w.u(smp, 1)             # seq_scaling_matrix_present_flag
        if smp:
            raise SystemExit("该流带 scaling matrix，本脚本未实现（不影响结论，换个流即可）")

    w.ue(g.ue())                # log2_max_frame_num_minus4
    poc = g.ue()
    w.ue(poc)
    if poc == 0:
        w.ue(g.ue())            # log2_max_pic_order_cnt_lsb_minus4
    elif poc == 1:
        raise SystemExit("poc_type==1 未实现")

    w.ue(g.ue())                # max_num_ref_frames
    w.u(g.u(1), 1)              # gaps_in_frame_num_value_allowed_flag
    w.ue(g.ue())                # pic_width_in_mbs_minus1
    w.ue(g.ue())                # pic_height_in_map_units_minus1
    fmo = g.u(1)
    w.u(fmo, 1)                 # frame_mbs_only_flag
    if not fmo:
        w.u(g.u(1), 1)          # mb_adaptive_frame_field_flag
    w.u(g.u(1), 1)              # direct_8x8_inference_flag
    cr = g.u(1)
    w.u(cr, 1)                  # frame_cropping_flag
    if cr:
        for _ in range(4):
            w.ue(g.ue())        # crop offsets

    vui = g.u(1)
    w.u(0, 1)                   # vui_parameters_present_flag := 0（这就是本次改动）
    w.trailing()

    body = w.bytes()
    return bytes([sps_nalu[0]]) + escape(body), vui


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    starts = [m.start() for m in re.finditer(b"\x00\x00\x01", d)]
    if not starts:
        raise SystemExit("找不到起始码")

    out = bytearray()
    replaced = 0
    for i, p in enumerate(starts):
        s = p + 3
        e = (starts[i + 1] if i + 1 < len(starts) else len(d))
        # 去掉下一个起始码前的拖尾零
        end = e
        while end > s and d[end - 1] == 0:
            end -= 1
        nalu = d[s:end]
        prefix = d[p:s]  # 保留原始起始码形态
        # 保留原 NALU 前的前导零（4 字节起始码的情形）
        if i == 0 and p > 0:
            out += d[:p]
        if nalu and (nalu[0] & 0x1f) == 7:
            new, had = rewrite_sps_without_vui(nalu)
            print(f"SPS: {len(nalu)} -> {len(new)} 字节, 原 vui_present={had}")
            out += prefix + new
            replaced += 1
        else:
            out += prefix + nalu
        # 补回拖尾零
        out += d[end:e]

    open(dst, "wb").write(bytes(out))
    print(f"替换 {replaced} 个 SPS -> {dst} ({len(out)} 字节, 原 {len(d)} 字节)")


if __name__ == "__main__":
    main()
