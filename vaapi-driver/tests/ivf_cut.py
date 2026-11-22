#!/usr/bin/env python3
"""From IVF For the first time, start The bag starts to be intercepted. Repackaged. IVF，And report on the location of the first key frame after the intercept.。

Why? IVF Not naked. .obu：ffmpeg of obu Unlock the sealer requirements**First in the file. temporal
unit The key frame.**，From GOP It’s middle cut. .obu Directly reported. "Invalid data found when
processing input"，It’s not going to feed you.。IVF Yes."Length per bag + PTS"A defined frame container，
It’s not a package. It’s a package."From GOP Centre Decomposition"This scene.。

Key frame position. ffprobe The package is marked and read without re-dissecting here. OBU —— The outer container.
Key frame information is inherently authoritative.。

Two cut-off methods (for two different defunct paths, see regress_av1_midgop.sh）：
  Default --Downshot. The beginnings GOP Middle bag.**No Sequence Head**，
                ffmpeg You can’t solve it, you can’t leave it up there, you can’t drive it out.。
  --seqhdr    —— Put it in the first key box in the back. OBU_SEQUENCE_HEADER After the intercept.
                In the first bag. So the sequence is all in order, and the frames are normally parsed and fed.
                Driver -- presses the drive"Reference frame is not solventable"That way.。

Usage: ivf_cut.py [--seqhdr] Enter.ivf Output.ivf Start package number
Output（stdout）: TOTAL=<Number of packages after intercept> KEY_AT=<Subscript for first frame after intercept>
"""
import json
import struct
import subprocess
import sys


def packets(path):
    """ffprobe Take each bag. (Offset in file, Length, Whether or not the key frame)。"""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-count_packets", "-show_entries", "packet=pos,size,flags",
         "-of", "json", path],
        capture_output=True, text=True, check=True).stdout
    pkts = json.loads(out)["packets"]
    return [(int(p["pos"]), int(p["size"]), "K" in p.get("flags", ""))
            for p in pkts]


def obu_span(d, i, end):
    """From i Start reading one. OBU Head, back (Type, Load Start, The whole thing. OBU The end of the world.)。"""
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
    return typ, i, end        # No length field to fragment End


def seq_header_obus(d, lo, hi):
    """Take [lo,hi) All of it. OBU_SEQUENCE_HEADER（type=1）Full bytes of the。

    aom Only put the head of the sequence in front of the key frame, so... GOP The middle bag isn’t -- that’s exactly what it is.
    "From the middle, we can’t even untie the frame."Reasons。"""
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
        sys.exit(f"{src}: No, it’s not. IVF（Magic Number {tag!r}）")
    pkts = packets(src)
    if not pkts:
        sys.exit(f"{src}: ffprobe I didn’t read the bag.")
    if start >= len(pkts):
        sys.exit(f"{src}: Start Package {start} Total excess {len(pkts)}")
    # ⁇ pos point to ffprobe ** frame header** (4 bytes long + 8 bytes PTS = 12 bytes),
    # The size is **load length**, excluding the 12 bytes. Actual full.ivf first two packages:
    #   pos=32 size=6242、pos=6286 —— 32+12+6242 = 6286 That’s what I’m talking about.。
    # "Invalid Obu length:6564, but only only 6552"
    # bytes remaining"（It’s just the same. 12）。
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
                sys.exit(f"{src}: number {start} There’s no key frame after the bag. It’s not gonna fill the head.")
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
