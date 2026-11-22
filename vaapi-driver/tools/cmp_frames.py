#!/usr/bin/env python3
"""Take it. daemon Output NV12 Frames and softened NV12 Frame Alignment。

**Why do you need this script?**：daemon The returned frame is...**Decoder buffer size**（Hightower. Venus Align the width.
Here. 128、High alignment 32，So... 1080p Actually, it is. 1920x1088），And the softened output is**Show Size**
（1920x1080）。Direct `cmp` Both will be reported as buffers filled in areas."Different" —— The fill lines.
The content is undefined and does not represent a decoding error. H.264 Path。

Correct comparison：
  - Y Plane: Remove Hard Frame Offset 0 Up. `display_w × display_h` bytes
  - UV Plane: Offset `stride × slice_height` Get up.（**No, it’s not.** stride × display_h）
    Take `display_w × display_h / 2` bytes

`offsets[1] = stride × slice_height` And that’s the same thing. VAImage The request is to use the display to calculate the value.
It’s gonna deflect the color level. `stride × (slice_height - display_h)` Bytes, the symptoms are the green edge and coloring.。

Usage：
    cmp_frames.py <software decode yuv> <Hard frame prefix or glob> [--display WxH] [--buffer WxH] [-n N]

Example (hard frame by test_dmd_client --dump-prefix /tmp/hw --dump-frames 3 Generate）：
    cmp_frames.py /tmp/sw.yuv /tmp/hw --display 1920x1080 --buffer 1920x1088 -n 3
"""
import argparse
import glob
import os
import sys


def parse_wh(s):
    try:
        w, h = s.lower().split("x")
        return int(w), int(h)
    except Exception:
        raise argparse.ArgumentTypeError(f"The size format should read WxH，received {s!r}")


def find_frames(prefix, count):
    """Support test_dmd_client of <prefix>-NNN.nv12 Named and supported direct to glob。"""
    if any(ch in prefix for ch in "*?["):
        files = sorted(glob.glob(prefix))
    else:
        files = sorted(glob.glob(f"{prefix}-*.nv12"))
        if not files:
            files = sorted(glob.glob(f"{prefix}*.nv12"))
    return files[:count] if count else files


def main():
    ap = argparse.ArgumentParser(description="Harder than softer frame alignment")
    ap.add_argument("software_yuv", help="software decode rawvideo Output (Show size, continuously frame)）")
    ap.add_argument("hw_prefix", help="Hard frame prefix or glob")
    ap.add_argument("--display", type=parse_wh, default=(1920, 1080),
                    help="Display dimensions, default 1920x1080")
    ap.add_argument("--buffer", type=parse_wh, default=None,
                    help="Decoder buffer size, default is the same size as the display (i.e. no fill)）")
    ap.add_argument("-n", "--frames", type=int, default=0,
                    help="Before comparing N frame，0 Show all available frames")
    args = ap.parse_args()

    dw, dh = args.display
    bw, bh = args.buffer if args.buffer else (dw, dh)
    if bw < dw or bh < dh:
        sys.exit(f"Buffer Size {bw}x{bh} Less than Display Size {dw}x{dh}")

    files = find_frames(args.hw_prefix, args.frames)
    if not files:
        sys.exit(f"Hard frame not found：{args.hw_prefix}")

    y_bytes = dw * dh
    uv_bytes = dw * dh // 2
    sw_frame = y_bytes + uv_bytes
    uv_off = bw * bh  # Key: buffer high instead of display high

    if not os.path.exists(args.software_yuv):
        sys.exit(f"Soft File does not exist：{args.software_yuv}")

    ok = 0
    bad = 0
    with open(args.software_yuv, "rb") as sw:
        for idx, path in enumerate(files):
            swf = sw.read(sw_frame)
            if len(swf) < sw_frame:
                print(f"Softly only. {idx} Frame, hard work. {len(files)} Frame, early end.")
                break
            y_sw, uv_sw = swf[:y_bytes], swf[y_bytes:]

            with open(path, "rb") as f:
                hw = f.read()
            expected = uv_off + uv_bytes
            if len(hw) < expected:
                print(f"frame{idx} {os.path.basename(path)}: Bytes {len(hw)} "
                      f"< Yes. {expected}（--buffer Could not be set right）")
                bad += 1
                continue

            y_hw = hw[:y_bytes]
            uv_hw = hw[uv_off:uv_off + uv_bytes]

            ys = y_hw == y_sw
            us = uv_hw == uv_sw
            note = ""
            if not ys:
                for k in range(y_bytes):
                    if y_hw[k] != y_sw[k]:
                        note = f" First Y Variance: Offset{k} Okay.{k // dw} Columns{k % dw}"
                        break
            elif not us:
                for k in range(uv_bytes):
                    if uv_hw[k] != uv_sw[k]:
                        note = f" First UV Variance: Offset{k}"
                        break

            if ys and us:
                ok += 1
            else:
                bad += 1
            print(f"frame{idx} {os.path.basename(path)}: "
                  f"Y{'Same' if ys else 'Different'} UV{'Same' if us else 'Different'} "
                  f"-> {'Unanimously.' if (ys and us) else 'Different'}{note}")

    print(f"\n Total: {ok} Frames are consistent., {bad} Frames are different.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
