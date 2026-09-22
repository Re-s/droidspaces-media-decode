#!/usr/bin/env python3
"""从 IVF 的第 start 个包开始截取，重新打包成 IVF，并报告截取后第一个关键帧的位置。

为什么用 IVF 而不是裸 .obu：ffmpeg 的 obu 解封装器要求**文件里第一个 temporal
unit 就是关键帧**，从 GOP 中间切的 .obu 直接报 "Invalid data found when
processing input"，根本喂不进去。IVF 是"每包长度 + PTS"的定长帧头容器，
对包内容不作要求，正好能表达"从 GOP 中间起解"这个场景。

关键帧位置靠 ffprobe 的包标记读，不在这里重新解析 OBU —— 容器外置的
关键帧信息本来就是权威值。

两种截法（对应两条不同的失效路径，见 regress_av1_midgop.sh）：
  默认        —— 原样截取。开头那几个 GOP 中间的包**没有序列头**，
                ffmpeg 自己就解不了、丢在上游，驱动压根收不到它们。
  --seqhdr    —— 把后面第一个关键帧包里的 OBU_SEQUENCE_HEADER 提到截取后的
                第一个包里。这样序列头齐了，那几帧就会被正常解析并喂给
                驱动 —— 压的是驱动侧"引用帧不可解析"那条路。

用法: ivf_cut.py [--seqhdr] 输入.ivf 输出.ivf 起始包号
输出（stdout）: TOTAL=<截取后包数> KEY_AT=<截取后第一个关键帧的下标>
"""
import json
import struct
import subprocess
import sys


def packets(path):
    """ffprobe 取每个包的 (文件内偏移, 长度, 是否关键帧)。"""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-count_packets", "-show_entries", "packet=pos,size,flags",
         "-of", "json", path],
        capture_output=True, text=True, check=True).stdout
    pkts = json.loads(out)["packets"]
    return [(int(p["pos"]), int(p["size"]), "K" in p.get("flags", ""))
            for p in pkts]


def obu_span(d, i, end):
    """从 i 开始读一个 OBU 头，返回 (类型, 载荷起点, 整个 OBU 的终点)。"""
    h = d[i]
    typ = (h >> 3) & 0xF
    i += 1
    if (h >> 2) & 1:          # extension
        i += 1
    if (h >> 1) & 1:          # has_size_field
        size = 0
        for k in range(8):
            b = d[i]
            i += 1
            size |= (b & 0x7F) << (7 * k)
            if not (b & 0x80):
                break
        return typ, i, min(i + size, end)
    return typ, i, end        # 无长度字段 → 到 fragment 末尾


def seq_header_obus(d, lo, hi):
    """取 [lo,hi) 这个包里所有 OBU_SEQUENCE_HEADER（type=1）的完整字节。

    aom 只在关键帧包前放序列头，所以 GOP 中间的包没有 —— 这正是
    "从中间起解连帧头都解不出来"的原因。"""
    out = bytearray()
    i = lo
    while i < hi:
        typ, p0, end = obu_span(d, i, hi)
        if typ == 1:
            out += d[i:end]
        if end <= i:
            break
        i = end
    return bytes(out)


def main():
    args = sys.argv[1:]
    prefix = False
    if args and args[0] == "--seqhdr":
        prefix = True
        args = args[1:]
    src, dst, start = args[0], args[1], int(args[2])
    d = open(src, "rb").read()
    tag, ver, hlen, fourcc, w, h, num, den, nframes, _res = \
        struct.unpack_from("<4sHHIHHIIII", d, 0)
    if tag != b"DKIF":
        sys.exit(f"{src}: 不是 IVF（魔数 {tag!r}）")
    pkts = packets(src)
    if not pkts:
        sys.exit(f"{src}: ffprobe 没读到包")
    if start >= len(pkts):
        sys.exit(f"{src}: 起始包 {start} 超出总数 {len(pkts)}")
    # ⚠️ ffprobe 报的 pos 指向**帧记录头**（4 字节长度 + 8 字节 PTS = 12 字节），
    # 而 size 是**载荷长度**、不含这 12 字节。实测 full.ivf 前两个包：
    #   pos=32 size=6242、pos=6286 —— 32+12+6242 = 6286 才对得上。
    # 少算这 12 字节 dav1d 就报 "Invalid OBU length: 6564, but only 6552
    # bytes remaining"（差的正好是 12）。
    REC = 12
    body = bytearray()
    key_at = None
    for i in range(start, len(pkts)):
        off, size, is_key = pkts[i]
        if is_key and key_at is None:
            key_at = i - start
        payload = d[off + REC:off + REC + size]
        if prefix and i == start:
            k = next((j for j in range(start, len(pkts)) if pkts[j][2]), None)
            if k is None:
                sys.exit(f"{src}: 第 {start} 包之后没有关键帧，无法补序列头")
            klo, kn, _ = pkts[k]
            payload = seq_header_obus(d, klo + REC, klo + REC + kn) + payload
        body += struct.pack("<IQ", len(payload), i - start)
        body += payload
    head = struct.pack("<4sHHIHHIIII", tag, ver, hlen, fourcc, w, h,
                       num, den, len(pkts) - start, 0)
    open(dst, "wb").write(head + body)
    print(f"TOTAL={len(pkts) - start} KEY_AT={key_at}")


if __name__ == "__main__":
    main()
