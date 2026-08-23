#!/usr/bin/env python3
"""把 daemon 输出的 NV12 帧与软解的 NV12 帧对齐比较。

**为什么需要这个脚本**：daemon 返回的帧是**解码器缓冲尺寸**（高通 Venus 把宽对齐
到 128、高对齐到 32，所以 1080p 实际是 1920x1088），而软解输出是**显示尺寸**
（1920x1080）。直接 `cmp` 两者会因为缓冲的填充区域而全部报"不同" —— 那些填充行
的内容是未定义的，不代表解码错误。主会话曾因此差点误判 H.264 路径。

正确的比较方式：
  - Y 平面：取硬解帧偏移 0 起的 `display_w × display_h` 字节
  - UV 平面：从偏移 `stride × slice_height` 起（**不是** stride × display_h）
    取 `display_w × display_h / 2` 字节

`offsets[1] = stride × slice_height` 这一点同样是 VAImage 的要求，用显示高去算
会让色度平面偏移 `stride × (slice_height - display_h)` 字节，症状是绿边与色度错位。

用法：
    cmp_frames.py <软解yuv> <硬解帧前缀或glob> [--display WxH] [--buffer WxH] [-n N]

示例（硬解帧由 test_dmd_client --dump-prefix /tmp/hw --dump-frames 3 产生）：
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
        raise argparse.ArgumentTypeError(f"尺寸格式应为 WxH，收到 {s!r}")


def find_frames(prefix, count):
    """支持 test_dmd_client 的 <prefix>-NNN.nv12 命名，也支持直接给 glob。"""
    if any(ch in prefix for ch in "*?["):
        files = sorted(glob.glob(prefix))
    else:
        files = sorted(glob.glob(f"{prefix}-*.nv12"))
        if not files:
            files = sorted(glob.glob(f"{prefix}*.nv12"))
    return files[:count] if count else files


def main():
    ap = argparse.ArgumentParser(description="对齐比较硬解帧与软解帧")
    ap.add_argument("software_yuv", help="软解 rawvideo 输出（显示尺寸，连续帧）")
    ap.add_argument("hw_prefix", help="硬解帧文件前缀或 glob")
    ap.add_argument("--display", type=parse_wh, default=(1920, 1080),
                    help="显示尺寸，默认 1920x1080")
    ap.add_argument("--buffer", type=parse_wh, default=None,
                    help="解码器缓冲尺寸，默认与显示尺寸相同（即无填充）")
    ap.add_argument("-n", "--frames", type=int, default=0,
                    help="比较前 N 帧，0 表示全部可用帧")
    args = ap.parse_args()

    dw, dh = args.display
    bw, bh = args.buffer if args.buffer else (dw, dh)
    if bw < dw or bh < dh:
        sys.exit(f"缓冲尺寸 {bw}x{bh} 小于显示尺寸 {dw}x{dh}")

    files = find_frames(args.hw_prefix, args.frames)
    if not files:
        sys.exit(f"找不到硬解帧：{args.hw_prefix}")

    y_bytes = dw * dh
    uv_bytes = dw * dh // 2
    sw_frame = y_bytes + uv_bytes
    uv_off = bw * bh  # 关键：用缓冲高而非显示高

    if not os.path.exists(args.software_yuv):
        sys.exit(f"软解文件不存在：{args.software_yuv}")

    ok = 0
    bad = 0
    with open(args.software_yuv, "rb") as sw:
        for idx, path in enumerate(files):
            swf = sw.read(sw_frame)
            if len(swf) < sw_frame:
                print(f"软解只有 {idx} 帧，硬解有 {len(files)} 帧，提前结束")
                break
            y_sw, uv_sw = swf[:y_bytes], swf[y_bytes:]

            with open(path, "rb") as f:
                hw = f.read()
            expected = uv_off + uv_bytes
            if len(hw) < expected:
                print(f"帧{idx} {os.path.basename(path)}: 字节数 {len(hw)} "
                      f"< 需要 {expected}（--buffer 可能设置不对）")
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
                        note = f" 首个Y差异: 偏移{k} 行{k // dw} 列{k % dw}"
                        break
            elif not us:
                for k in range(uv_bytes):
                    if uv_hw[k] != uv_sw[k]:
                        note = f" 首个UV差异: 偏移{k}"
                        break

            if ys and us:
                ok += 1
            else:
                bad += 1
            print(f"帧{idx} {os.path.basename(path)}: "
                  f"Y{'同' if ys else '异'} UV{'同' if us else '异'} "
                  f"-> {'一致' if (ys and us) else '不同'}{note}")

    print(f"\n合计: {ok} 帧一致, {bad} 帧不同")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
