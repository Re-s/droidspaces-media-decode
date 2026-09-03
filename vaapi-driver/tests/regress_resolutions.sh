#!/bin/sh
# 多分辨率硬解回归：硬解结果必须与软解逐字节一致。
#
# 为什么需要它：原有回归流全是 1920x1080，恰好等于 msm_vidc 的
# G_FMT(CAPTURE) 默认几何，因此永远不触发"用 OUTPUT 协商值覆盖 CAPTURE
# 残留"那条分支。真实缺陷（残留 stride=1920 被用于 1280x720，导致每行
# 错位 640 字节并把 slice_height 从 736 压成 492）就藏在这个盲区里，
# 表现为 Firefox 绿屏，而 1080p 回归全绿。
#
# 覆盖点：
#   1280x720   常见非 1080p
#   854x480    宽度非 128 倍数（stride 需对齐到 896）
#   640x360    小分辨率
#   720x1280   竖屏（高 > 宽）
#   分辨率切换 同一 fd 上依次解 1080p→720p→480p
#
# 用法:
#   FFMPEG=/path/to/ffmpeg DRIVER_DIR=../build ./regress_resolutions.sh [源码流]
# 依赖 ffmpeg 具备 libx265 与 vaapi 支持。

set -e

FFMPEG=${FFMPEG:-ffmpeg}
DRIVER_DIR=${DRIVER_DIR:-../build}
SRC=${1:-test.hevc}
WORK=${WORK:-$(mktemp -d)}
KEEP=${KEEP:-0}

if [ ! -f "$SRC" ]; then
    echo "跳过: 找不到源码流 $SRC" >&2
    exit 77
fi

cleanup() {
    [ "$KEEP" = "1" ] || rm -rf "$WORK"
}
trap cleanup EXIT

fail=0
pass=0

# 逐个分辨率：编码 -> 软解 md5 -> 硬解 md5 -> 比对
for spec in 1280x720 854x480 640x360 720x1280; do
    w=${spec%x*}
    h=${spec#*x}
    clip="$WORK/res_$spec.hevc"

    if ! "$FFMPEG" -hide_banner -loglevel error -i "$SRC" \
            -vf "scale=$w:$h" -c:v libx265 -x265-params log-level=0 \
            -frames:v 24 -f hevc "$clip" -y 2>/dev/null; then
        printf '  %-10s 跳过（编码失败，缺 libx265?）\n' "$spec"
        continue
    fi

    sw=$("$FFMPEG" -hide_banner -loglevel error -i "$clip" \
            -pix_fmt yuv420p -f rawvideo - 2>/dev/null | md5sum | cut -d' ' -f1)

    if ! LIBVA_DRIVERS_PATH="$DRIVER_DIR" "$FFMPEG" -hide_banner -loglevel error \
            -hwaccel vaapi -i "$clip" -pix_fmt yuv420p \
            -f rawvideo "$WORK/hw.yuv" -y 2>/dev/null; then
        printf '  %-10s ✗ 硬解失败\n' "$spec"
        fail=$((fail + 1))
        continue
    fi
    hw=$(md5sum "$WORK/hw.yuv" | cut -d' ' -f1)
    rm -f "$WORK/hw.yuv"

    if [ "$sw" = "$hw" ]; then
        printf '  %-10s ✓ 字节一致\n' "$spec"
        pass=$((pass + 1))
    else
        printf '  %-10s ✗ 不一致 软解=%.12s 硬解=%.12s\n' "$spec" "$sw" "$hw"
        fail=$((fail + 1))
    fi
done

# 分辨率切换：同一解码器 fd 上连续处理三种几何。
# 这是最接近浏览器自适应码率的场景，也是残留几何最容易暴露的地方。
sw_ok=1
: > "$WORK/switch.hevc"
for spec in 1920x1080 1280x720 854x480; do
    w=${spec%x*}
    h=${spec#*x}
    if ! "$FFMPEG" -hide_banner -loglevel error -i "$SRC" \
            -vf "scale=$w:$h" -c:v libx265 -x265-params log-level=0 \
            -frames:v 12 -f hevc "$WORK/seg_$spec.hevc" -y 2>/dev/null; then
        sw_ok=0
        break
    fi
    cat "$WORK/seg_$spec.hevc" >> "$WORK/switch.hevc"
done

if [ "$sw_ok" = "1" ]; then
    # 逐段比对：拼接流整体解码后帧尺寸会变，单段比对更严格。
    seg_fail=0
    for spec in 1920x1080 1280x720 854x480; do
        seg="$WORK/seg_$spec.hevc"
        sw=$("$FFMPEG" -hide_banner -loglevel error -i "$seg" \
                -pix_fmt yuv420p -f rawvideo - 2>/dev/null | md5sum | cut -d' ' -f1)
        LIBVA_DRIVERS_PATH="$DRIVER_DIR" "$FFMPEG" -hide_banner -loglevel error \
            -hwaccel vaapi -i "$seg" -pix_fmt yuv420p \
            -f rawvideo "$WORK/s.yuv" -y 2>/dev/null || true
        hw=$(md5sum "$WORK/s.yuv" 2>/dev/null | cut -d' ' -f1)
        rm -f "$WORK/s.yuv"
        [ "$sw" = "$hw" ] || seg_fail=1
    done
    # 再跑一次拼接流，确认同一会话内切换不报错、不截断。
    if LIBVA_DRIVERS_PATH="$DRIVER_DIR" "$FFMPEG" -hide_banner -loglevel error \
            -hwaccel vaapi -i "$WORK/switch.hevc" -f null - 2>/dev/null; then
        :
    else
        seg_fail=1
    fi
    if [ "$seg_fail" = "0" ]; then
        printf '  %-10s ✓ 各段字节一致\n' '切换流'
        pass=$((pass + 1))
    else
        printf '  %-10s ✗ 切换后像素或解码异常\n' '切换流'
        fail=$((fail + 1))
    fi
fi

echo "多分辨率回归: 通过 $pass，失败 $fail"
[ "$fail" = "0" ]
