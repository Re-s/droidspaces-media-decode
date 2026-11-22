#!/bin/bash
# Double-contract return: the same stream uses the ffmpeg and chrome contracts separately to verify the accuracy of pixels.
#
# ── Why do you need it? ────────────────────────────────────────────────
# Normal md5 returns** only covers ffmpeg compacts**, which does not justify chrome paths.
#
# ffmpeg Let’s go. vaDeriveImage / vaGetImage Read pixels. Both entrances are adjusted in the drive.
# dmd_surface_wait() The bottom line.（src/image.c:169 / :283）。So even if...
# vaEndPicture The pixels were not written when the return took place. surface，ffmpeg And so will he. map Wait till the time comes.，
# md5 It’s all green.。
#
# The contract for Chrome is completely different (chromium source + driver log double-directional confirmation):
#   1. CreateSurfaces And then, before any decoding. vaExportSurfaceHandle Take it. dmabuf
#   2. And then only vaBeginPicture / vaRenderPicture / vaEndPicture
#   3. Never vaSyncSurface / vaQuerySurfaceStatus / vaDeriveImage
#      (Sync 0, Firefox co-situation 1,500)
#   4. With your own software. DPB - I’ll decide when to put it. surface Hand over to synth sampler
# That is, **Pixels must be in place when väEndPicture returns**.
#
# The project in Round 2 and 3 was misjudged "reconstructed" by looking only at md5 and the actual browser still jumped.
#
# ── Practice ────────────────────────────────────────────────────────
# DMD_NO_MAP_WAIT=1 disables the fallback wait when mapping. FFmpeg still submits all parameter buffers.
# But pixels must be in position at the moment of the Map, otherwise they can read the empty buffer.
# → md5 Immediately inconsistent. So, yes."Chrome Will you get it to the unfinished part? surface"Be automatic
# The return is determined without eye-to-eye viewing the browser.
#
# Do not replace the script with the table/ probe_chrome_order.c: the probe is not sent
# VAPictureParameterBuffer，Decoding itself is not viable (for details see the actual records on the header of the document)）。
#
# Usage: regress_chrome_contract.sh [ffmpeg path] [Driver build directory]

set -u

FF="${1:-/home/master/Documents/DSHWK/vatest/sysroot/usr/bin/ffmpeg}"
DRV="${2:-$(cd "$(dirname "$0")/../build" && pwd)}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$(dirname "$(dirname "$FF")")/lib/aarch64-linux-gnu"

if [ ! -x "$FF" ]; then echo "Not found ffmpeg: $FF"; exit 2; fi
if [ ! -f "$DRV/msm_drm_drv_video.so" ]; then echo "No driver found: $DRV"; exit 2; fi

echo "Driver: $(strings "$DRV/msm_drm_drv_video.so" | grep -m1 'DroidSpaces V4L2')"
echo

pass=0; fail=0

check() {
    local name="$1" file="$2" frames="$3"
    shift 3
    local extra=("$@")

    [ -f "$file" ] || { printf '%-22s SKIP（Shortage of material）\n' "$name"; return; }

    local sw hw_ff hw_cr
    sw=$("$FF" -v error -i "$file" -frames:v "$frames" \
         -f rawvideo -pix_fmt nv12 - 2>/dev/null | md5sum | cut -d' ' -f1)

    hw_ff=$(LIBVA_DRIVER_NAME=msm_drm LIBVA_DRIVERS_PATH="$DRV" \
            "$FF" -v error -hwaccel vaapi -hwaccel_output_format nv12 \
            "${extra[@]}" -i "$file" -frames:v "$frames" \
            -f rawvideo -pix_fmt nv12 - 2>/dev/null | md5sum | cut -d' ' -f1)

    hw_cr=$(DMD_NO_MAP_WAIT=1 LIBVA_DRIVER_NAME=msm_drm LIBVA_DRIVERS_PATH="$DRV" \
            "$FF" -v error -hwaccel vaapi -hwaccel_output_format nv12 \
            "${extra[@]}" -i "$file" -frames:v "$frames" \
            -f rawvideo -pix_fmt nv12 - 2>/dev/null | md5sum | cut -d' ' -f1)

    local a b
    [ "$hw_ff" = "$sw" ] && a=PASS || a=FAIL
    [ "$hw_cr" = "$sw" ] && b=PASS || b=FAIL
    printf '%-22s ffmpeg=%s  Chrome=%s\n' "$name" "$a" "$b"
    if [ "$a" = PASS ] && [ "$b" = PASS ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        [ "$b" = FAIL ] && [ "$a" = PASS ] && \
            echo "    ↑ Only Chrome The contract failed. = A sampling window exists, and the browser will see the jump frame."
    fi
}

V=/home/master/Documents/DSHWK/vatest
C=/tmp/cr

check "H.264"            "$V/test.h264"        60
check "HEVC"             "$V/test.hevc"        60
check "VP9"              "$V/test.vp9.ivf"     60
check "VP8"              "$V/test.vp8.ivf"     60
check "HEVC B frame 150"     "$C/jinjie_265.mp4"   150
check "HEVC B frame 300"     "$C/jinjie_265.mp4"   300
check "Slow-number stream"       "$C/order_slow.mp4"   40
check "None B Frame Contrast"        "$C/noB_slow.mp4"     40
# Zoom in in on the frame number: bigger surface pool, late recovery of effmpeg, closer to the intense submission of Chrome
check "It’s flying in. x16"     "$C/order_slow.mp4"   40  -extra_hw_frames 16

echo
echo "Total: $pass Pass., $fail failed"

# Real sample window count: PENDING only (submitted, not available pixels).
# IDLE Yes. ffmpeg Capability detection before decoding, which runs just right on each time. 1 The second time, it’s not about the jump.。
if [ -f "$C/order_slow.mp4" ]; then
    log=$(mktemp)
    DMD_NO_MAP_WAIT=1 DMD_VA_LOG=1 LIBVA_DRIVER_NAME=msm_drm \
        LIBVA_DRIVERS_PATH="$DRV" "$FF" -v error -hwaccel vaapi \
        -hwaccel_output_format nv12 -i "$C/order_slow.mp4" \
        -frames:v 40 -f null - 2>"$log" >/dev/null
    echo "Pending surface samples: $(grep -c 'submitted but pixels are not ready' "$log")"
    echo "EndPicture frame writes: $(grep -c 'wrote before returning' "$log")"
    echo "Background frame receives: $(grep -c 'ORDER reap' "$log")"
    rm -f "$log"
fi

exit $((fail ? 1 : 0))
