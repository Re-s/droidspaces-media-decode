#!/usr/bin/env python3
"""Determines the correspondence between hard-line output and soft-squeak (show sequence)。

Use Y The plane. md5 Make frame prints and map every hard frame back to its position in the soft modem output.。
- If the map is... 0,1,2,3,... → Output is display order, correct
- If the map is messy, the frame is in a disorderly order.（B Frame rehearsing failed PTS Return to your posts.）
- An unmatched frame is absent from the software-decoded output, indicating a pixel error rather than reordering.

Usage：frame_order.py <software decode yuv> <Hard frame prefix> [Frames] [Display WxH]
"""
import glob
import hashlib
import sys


def main():
    sw_path = sys.argv[1]
    prefix = sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    if len(sys.argv) > 4:
        fw, fh = (int(x) for x in sys.argv[4].lower().split("x"))
    else:
        fw, fh = 1920, 1080

    y = fw * fh
    frame = y + y // 2

    sw = {}
    with open(sw_path, "rb") as f:
        idx = 0
        while True:
            d = f.read(frame)
            if len(d) < frame:
                break
            sw.setdefault(hashlib.md5(d[:y]).hexdigest(), idx)
            idx += 1
    print(f"Soft Frames {idx}（The only fingerprints. {len(sw)}）")

    files = sorted(glob.glob(f"{prefix}-*.nv12"))
    if n:
        files = files[:n]
    if not files:
        sys.exit(f"Hard frame not found {prefix}-*.nv12")

    mapping = []
    missing = 0
    for i, p in enumerate(files):
        with open(p, "rb") as f:
            d = f.read()
        h = hashlib.md5(d[:y]).hexdigest()
        pos = sw.get(h)
        mapping.append(pos)
        if pos is None:
            missing += 1
        print(f"  hw[{i:3d}] -> sw[{pos if pos is not None else 'Not found'}]")

    found = [m for m in mapping if m is not None]
    ordered = found == sorted(found)
    print()
    print(f"Hard frame number {len(files)}，Not found in soft resolution {missing} frame")
    print(f"Whether the found frame is single-transit increment: {'Yes.' if ordered else 'No (frames are in a disorder)）'}")
    if found and ordered and found == list(range(len(found))):
        print("Conclusions: Output is display order, exactly correct")
    elif missing:
        print("Conclusions: A true frame to solve the problem (the content does not exist in the soluble result)）")
    else:
        print("Conclusions: Everything is right, but in the wrong order -- typical. B Frame rehearsing failed PTS Return to your posts.")


if __name__ == "__main__":
    main()
