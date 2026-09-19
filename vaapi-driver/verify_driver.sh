#!/bin/bash
# dmd 驱动验证脚本 —— 硬解格式逐一对拍软解（md5 必须完全一致）
#
# 用法:
#   ./verify_driver.sh [驱动目录]
#   不装驱动时: ./verify_driver.sh ../vaapi-driver/build
#   已安装系统驱动时: ./verify_driver.sh /usr/lib/aarch64-linux-gnu/dri
set -e
LIB="${1:-/home/xieyizhou/Documents/piliplus/硬解/vaapi-driver/build}"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export LIBVA_DRIVERS_PATH="$LIB"

echo "== 驱动版本 =="
vainfo 2>/dev/null | sed -n '/Driver version/p;/VAProfile/p'

check() {  # check <名字> <编码器参数> <容器> <输入参数>
    local name=$1 enc=$2 mux=$3
    ffmpeg -v error -f lavfi -i "testsrc2=size=640x360:rate=25" -frames:v 24 \
        $enc -f $mux "$W/$name" -y 2>/dev/null
    local sw hw
    sw=$(ffmpeg -v error -i "$W/$name" -pix_fmt yuv420p -f rawvideo - 2>/dev/null | md5sum | cut -d' ' -f1)
    hw=$(timeout 120 ffmpeg -v error -hwaccel vaapi -i "$W/$name" -pix_fmt yuv420p \
         -f rawvideo - 2>/dev/null | md5sum | cut -d' ' -f1)
    if [ "$sw" = "$hw" ] && [ -n "$sw" ]; then
        printf '  %-6s ✅ 逐字节一致 (%s)\n' "$name" "$sw"
    else
        printf '  %-6s ❌ sw=%s hw=%s\n' "$name" "$sw" "$hw"
    fi
}

echo "== 四格式像素对拍 =="
check h264 "-c:v libx264 -pix_fmt yuv420p" h264
check hevc "-c:v libx265 -pix_fmt yuv420p -x265-params log-level=0" hevc
check vp9  "-c:v libvpx-vp9 -pix_fmt yuv420p" ivf
check vp8  "-c:v libvpx -pix_fmt yuv420p" ivf
check av1  "-c:v libaom-av1 -cpu-used 8 -lag-in-frames 0 -b:v 2M -g 20" obu

echo
echo "说明: vp8 若报 codec 不支持属预期（8 Elite 固件无 VP8 硬解，驱动已按"
echo "      固件能力隐藏声明，ffmpeg 会回落软解，两边 md5 相同即正确）。"
