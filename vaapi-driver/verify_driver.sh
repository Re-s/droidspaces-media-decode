#!/bin/bash
# Compare software and VA-API decode checksums.
#
# Usage: ./verify_driver.sh [driver directory]
# Examples:
#   ./verify_driver.sh ./build
#   ./verify_driver.sh /usr/lib/aarch64-linux-gnu/dri
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
LIB="${1:-$HERE/build}"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export LIBVA_DRIVERS_PATH="$LIB"

echo "== Driver Version =="
vainfo 2>/dev/null | sed -n '/Driver version/p;/VAProfile/p'

check() {  # check <name> <encoder arguments> <muxer>
    local name=$1 enc=$2 mux=$3
    ffmpeg -v error -f lavfi -i "testsrc2=size=640x360:rate=25" -frames:v 24 \
        $enc -f $mux "$W/$name" -y 2>/dev/null
    local sw hw
    sw=$(ffmpeg -v error -i "$W/$name" -pix_fmt yuv420p -f rawvideo - 2>/dev/null | md5sum | cut -d' ' -f1)
    hw=$(timeout 120 ffmpeg -v error -hwaccel vaapi -i "$W/$name" -pix_fmt yuv420p \
         -f rawvideo - 2>/dev/null | md5sum | cut -d' ' -f1)
    if [ "$sw" = "$hw" ] && [ -n "$sw" ]; then
        printf '  %-6s ✅ Bit-exact match (%s)\n' "$name" "$sw"
    else
        printf '  %-6s ❌ sw=%s hw=%s\n' "$name" "$sw" "$hw"
    fi
}

echo "== Pixel checks by codec =="
check h264 "-c:v libx264 -pix_fmt yuv420p" h264
check hevc "-c:v libx265 -pix_fmt yuv420p -x265-params log-level=0" hevc
check vp9  "-c:v libvpx-vp9 -pix_fmt yuv420p" ivf
check vp8  "-c:v libvpx -pix_fmt yuv420p" ivf
check av1  "-c:v libaom-av1 -cpu-used 8 -lag-in-frames 0 -b:v 2M -g 20" obu

echo
echo "VP8: the driver hides this capability when the firmware cannot decode it."
echo "FFmpeg may fall back to software. Matching checksums alone cannot confirm hardware decoding."
