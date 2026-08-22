#!/usr/bin/env python3
"""判定硬解输出帧序与软解（显示序）的对应关系。

用 Y 平面的 md5 做帧指纹，把每个硬解帧映射回它在软解输出里的位置。
- 若映射是 0,1,2,3,... → 输出即显示序，正确
- 若映射是乱的 → 帧序错乱（B 帧重排未按 PTS 归位）
- 若某帧"未找到" → 该帧内容在软解结果里不存在，是真解错而非顺序问题

用法：frame_order.py <软解yuv> <硬解帧前缀> [帧数] [显示WxH]
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
    print(f"软解帧数 {idx}（唯一指纹 {len(sw)}）")

    files = sorted(glob.glob(f"{prefix}-*.nv12"))
    if n:
        files = files[:n]
    if not files:
        sys.exit(f"找不到硬解帧 {prefix}-*.nv12")

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
        print(f"  hw[{i:3d}] -> sw[{pos if pos is not None else '未找到'}]")

    found = [m for m in mapping if m is not None]
    ordered = found == sorted(found)
    print()
    print(f"硬解帧数 {len(files)}，未在软解中找到 {missing} 帧")
    print(f"已找到的帧是否单调递增: {'是' if ordered else '否（帧序错乱）'}")
    if found and ordered and found == list(range(len(found))):
        print("结论: 输出即显示序，完全正确")
    elif missing:
        print("结论: 存在真解错的帧（内容在软解结果中不存在）")
    else:
        print("结论: 内容都对但顺序错 —— 典型的 B 帧重排未按 PTS 归位")


if __name__ == "__main__":
    main()
