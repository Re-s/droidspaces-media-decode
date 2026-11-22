#!/usr/bin/env python3
"""Take it. Annex B In the stream. SPS Rewrite to"Get rid of it. VUI、The rest of the bits are completely unchanged."Version of the。

Purpose: Separating authentication MediaCodec Do you really need it? SPS Yes. VUI。
If replaced, you can still decode correctly by bytes, which describes the synthesis SPS No writing. VUI It’s safe.，
Well... H.264 Path PSNR 11.5dB The root causes are on other fields, not missing. VUI。
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
    """Enter content NAL header of SPS NALU（No start code. Go back. VUI Behind NALU。"""
    rbsp = unescape(sps_nalu)
    g = BR(rbsp, 8)  # skip NAL header bytes
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
            raise SystemExit("The strap. scaling matrix，This script is not realized (without prejudice to conclusions, a new stream is sufficient)）")

    w.ue(g.ue())                # log2_max_frame_num_minus4
    poc = g.ue()
    w.ue(poc)
    if poc == 0:
        w.ue(g.ue())            # log2_max_pic_order_cnt_lsb_minus4
    elif poc == 1:
        raise SystemExit("poc_type==1 Not achieved")

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
    w.u(0, 1)                   # vui_parameters_present_flag := 0（This is the change.）
    w.trailing()

    body = w.bytes()
    return bytes([sps_nalu[0]]) + escape(body), vui


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    starts = [m.start() for m in re.finditer(b"\x00\x00\x01", d)]
    if not starts:
        raise SystemExit("No start code found")

    out = bytearray()
    replaced = 0
    for i, p in enumerate(starts):
        s = p + 3
        e = (starts[i + 1] if i + 1 < len(starts) else len(d))
        # Remove the tailing zero from the next code.
        end = e
        while end > s and d[end - 1] == 0:
            end -= 1
        nalu = d[s:end]
        prefix = d[p:s]  # Keep original starting-code form
        # Keep the original NALU pre-guiding zero (4 byte start code)
        if i == 0 and p > 0:
            out += d[:p]
        if nalu and (nalu[0] & 0x1f) == 7:
            new, had = rewrite_sps_without_vui(nalu)
            print(f"SPS: {len(nalu)} -> {len(new)} bytes, Original vui_present={had}")
            out += prefix + new
            replaced += 1
        else:
            out += prefix + nalu
        # Recover the trail zero.
        out += d[end:e]

    open(dst, "wb").write(bytes(out))
    print(f"Replace {replaced}  SPS -> {dst} ({len(out)} bytes, Original {len(d)} bytes)")


if __name__ == "__main__":
    main()
