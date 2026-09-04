#!/bin/bash
# 双契约回归：同一条流分别按 ffmpeg 契约与 Chrome 契约验证像素正确性。
#
# ── 为什么需要它 ────────────────────────────────────────────────
# 普通 md5 回归**只覆盖 ffmpeg 契约**，证明不了 Chrome 路径正确。
#
# ffmpeg 走 vaDeriveImage / vaGetImage 读像素，这两个入口在驱动里都调
# dmd_surface_wait() 兜底等帧（src/image.c:169 / :283）。所以即使
# vaEndPicture 返回时像素还没写进 surface，ffmpeg 也会在 map 时等到，
# md5 照样全绿。
#
# 而 Chrome 的契约完全不同（chromium 源码 + 驱动日志双向确认）：
#   1. CreateSurfaces 之后、任何解码之前就 vaExportSurfaceHandle 拿 dmabuf
#   2. 之后只 vaBeginPicture / vaRenderPicture / vaEndPicture
#   3. 从不 vaSyncSurface / vaQuerySurfaceStatus / vaDeriveImage
#      （实测计数：Sync 0 次，Firefox 同场景 1500 次）
#   4. 靠自己的软件 DPB 决定何时把 surface 交给合成器采样
# 也就是说 **像素必须在 vaEndPicture 返回时就已就位**。
#
# 本项目第 2、3 轮都因为只看 md5 而误判"已修复"，实际浏览器仍跳帧。
#
# ── 做法 ────────────────────────────────────────────────────────
# DMD_NO_MAP_WAIT=1 关掉 map 时的兜底等待。ffmpeg 仍送全套参数缓冲
# （解码正确性有保证），但像素必须在 map 那一刻就位，否则读到空缓冲
# → md5 立刻不一致。于是"Chrome 会不会采到未写完的 surface"变成可自动
# 判定的回归，不必靠人眼盯浏览器。
#
# ⚠️ 不要用 tools/probe_chrome_order.c 替代本脚本：那个探针不送
# VAPictureParameterBuffer，解码本身就不成立（详见其文件头的实测记录）。
#
# 用法：regress_chrome_contract.sh [ffmpeg路径] [驱动build目录]

set -u

FF="${1:-/home/master/Documents/DSHWK/vatest/sysroot/usr/bin/ffmpeg}"
DRV="${2:-$(cd "$(dirname "$0")/../build" && pwd)}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$(dirname "$(dirname "$FF")")/lib/aarch64-linux-gnu"

if [ ! -x "$FF" ]; then echo "找不到 ffmpeg: $FF"; exit 2; fi
if [ ! -f "$DRV/msm_drm_drv_video.so" ]; then echo "找不到驱动: $DRV"; exit 2; fi

echo "驱动: $(strings "$DRV/msm_drm_drv_video.so" | grep -m1 'DroidSpaces V4L2')"
echo

pass=0; fail=0

check() {
    local name="$1" file="$2" frames="$3"
    shift 3
    local extra=("$@")

    [ -f "$file" ] || { printf '%-22s SKIP（缺素材）\n' "$name"; return; }

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
            echo "    ↑ 仅 Chrome 契约失败 = 存在采样窗口，浏览器会看到跳帧"
    fi
}

V=/home/master/Documents/DSHWK/vatest
C=/tmp/cr

check "H.264"            "$V/test.h264"        60
check "HEVC"             "$V/test.hevc"        60
check "VP9"              "$V/test.vp9.ivf"     60
check "VP8"              "$V/test.vp8.ivf"     60
check "HEVC B帧 150"     "$C/jinjie_265.mp4"   150
check "HEVC B帧 300"     "$C/jinjie_265.mp4"   300
check "慢速编号流"       "$C/order_slow.mp4"   40
check "无B帧对照"        "$C/noB_slow.mp4"     40
# 放大在飞帧数：surface 池更大 ⇒ ffmpeg 更晚回收 ⇒ 更接近 Chrome 的猛提交
check "在飞放大 x16"     "$C/order_slow.mp4"   40  -extra_hw_frames 16

echo
echo "合计: $pass 通过, $fail 失败"

# 真实采样窗口计数：只统计 PENDING（已提交、像素未到）。
# IDLE 是 ffmpeg 解码前的能力探测，每次运行恰好 1 次，与跳帧无关。
if [ -f "$C/order_slow.mp4" ]; then
    log=$(mktemp)
    DMD_NO_MAP_WAIT=1 DMD_VA_LOG=1 LIBVA_DRIVER_NAME=msm_drm \
        LIBVA_DRIVERS_PATH="$DRV" "$FF" -v error -hwaccel vaapi \
        -hwaccel_output_format nv12 -i "$C/order_slow.mp4" \
        -frames:v 40 -f null - 2>"$log" >/dev/null
    echo "采样窗口(PENDING): $(grep -c '已提交但像素未到' "$log")"
    echo "EndPicture 写入  : $(grep -c '返回前写入' "$log")"
    echo "后台线程收帧     : $(grep -c 'ORDER reap' "$log")"
    rm -f "$log"
fi

exit $((fail ? 1 : 0))
