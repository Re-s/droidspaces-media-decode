#!/bin/bash
# AV1 像素回归：多组编码结构逐字节对拍软解，每条同时验 ffmpeg 与 Chrome 两种契约。
#
# 为什么单独一个脚本：verify_driver.sh 只有一条 640x360 的 AV1 流，覆盖不到
# 长 GOP（B 金字塔深、DPB 趟频繁）、全内帧、10bit、非 128 对齐宽度这些
# 真正会踩到参考帧/槽位记账的结构。双趟解码与硬件静默丢帧的兜底都在这条
# 路径上，只靠一条流守不住。
#
# 用法:
#   ./tests/regress_av1_pixels.sh
#   ./tests/regress_av1_pixels.sh --wrap   # 只跑跨 order hint 回绕的长序列
#   DRIVER_DIR=../build FRAMES=24 ./tests/regress_av1_pixels.sh
#   WRAPFRAMES=400 ./tests/regress_av1_pixels.sh --wrap
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
LIB="${DRIVER_DIR:-$DIR/../build}"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export LIBVA_DRIVERS_PATH="$LIB"
FFMPEG="${FFMPEG:-ffmpeg}"
FRAMES="${FRAMES:-24}"

pass=0; fail=0; skip=0

# md5 + 帧数。rawvideo 无头，帧数直接按平面字节数除出来：用 ffmpeg 数帧要
# 收 stats 行，而 -v error 会把这行吞掉，只能靠文件长度。
stat_of() {
    local f=$1 fmt=$2 w=${3%x*} h=${3#*x} bytes
    case $fmt in
        *10le|*12le|*16le) bytes=$(( w * h * 3 )) ;;
        *)                 bytes=$(( w * h * 3 / 2 )) ;;
    esac
    printf '%s %s' "$(md5sum "$f" | cut -d' ' -f1)" \
        "$(( $(stat -c%s "$f" 2>/dev/null || echo 0) / bytes ))"
}

# check <名字> <输入参数> <编码参数> <pix_fmt> <宽x高> [帧数覆盖]
check() {
    local name=$1 srcarg=$2 enc=$3 fmt=$4 size=$5
    local nf=${6:-$FRAMES}
    if ! $FFMPEG -v error -f lavfi -i "$srcarg" -frames:v "$nf" $enc \
            -f obu "$W/$name.obu" -y 2>/dev/null; then
        printf '  %-14s 跳过（编码失败）\n' "$name"
        skip=$((skip + 1)); return
    fi
    local sw hw cr
    $FFMPEG -v error -i "$W/$name.obu" -pix_fmt "$fmt" -f rawvideo \
        "$W/$name.sw.yuv" -y 2>/dev/null
    sw=$(stat_of "$W/$name.sw.yuv" "$fmt" "$size")
    timeout 300 $FFMPEG -v error -hwaccel vaapi -i "$W/$name.obu" \
        -pix_fmt "$fmt" -f rawvideo "$W/$name.hw.yuv" -y 2>/dev/null
    hw=$(stat_of "$W/$name.hw.yuv" "$fmt" "$size")
    # Chrome 契约：Chrome 只调 BeginPicture/RenderPicture/EndPicture，
    # 从不 Sync/Derive/GetImage，像素必须在 EndPicture 返回时就位。ffmpeg 走
    # map 路径，vaMapBuffer 里有 dmd_surface_wait 兜底会把"还没就位"救回来，
    # 于是 md5 全绿而浏览器跳帧。关掉这个兜底，才测得到那个采样窗口。
    #
    # ⚠️ 这一趟**不是万能的**：它只覆盖"像素到位时机"这一类时序缺陷。
    # 提交节奏差异抓不住 —— ffmpeg 一次灌 6 帧以上，驱动里有问题的东西
    # 往往在硬件真正用上之前就被后续提交修正了，这类 bug 只有浏览器能暴露
    # （实例见 decode.c:3177 那段首份 PPS l0=0 的实测记录，以及
    # tests/browser/verify_pps_fix.sh）。
    DMD_NO_MAP_WAIT=1 timeout 300 $FFMPEG -v error -hwaccel vaapi \
        -i "$W/$name.obu" -pix_fmt "$fmt" -f rawvideo \
        "$W/$name.cr.yuv" -y 2>/dev/null
    cr=$(stat_of "$W/$name.cr.yuv" "$fmt" "$size")
    rm -f "$W/$name.sw.yuv" "$W/$name.hw.yuv" "$W/$name.cr.yuv"
    local empty=d41d8cd98f00b204e9800998ecf8427e
    if [ "${sw%% *}" = "${hw%% *}" ] && [ "${sw%% *}" != "$empty" ] \
        && [ "${sw%% *}" = "${cr%% *}" ]; then
        printf '  %-14s ✅ 逐字节一致 (%s 帧, %s)\n' "$name" "${sw##* }" "${sw%% *}"
        pass=$((pass + 1))
    else
        printf '  %-14s ❌ 软解[%s] 硬解[%s]' "$name" "$sw" "$hw"
        [ "${sw%% *}" = "${hw%% *}" ] && [ "${sw%% *}" != "$empty" ] && \
            printf ' —— ffmpeg 契约过、Chrome 契约不过（存在采样窗口）[%s]' "$cr"
        printf '\n'
        fail=$((fail + 1))
    fi
}

SRC="testsrc2=size=640x360:rate=25"
AOM="-c:v libaom-av1 -cpu-used 8 -lag-in-frames 0 -b:v 2M"

# 全局运动专用源：整幅画面平移，aom 会把它编成 ROTZOOM/AFFINE 的 gm_params。
# 单独一条源是因为 gm 只在"真的存在全局位移"时出现 —— testsrc2 自身不动，
# 用 crop 按 t 平移才能稳定编出 gm（实测 24 帧里有 8 帧带 is_global=1）。
PAN="testsrc2=size=1280x720:rate=25,crop=640:360:'mod(t*120,640)':'mod(t*80,360)'"
AOM_GM="-c:v libaom-av1 -cpu-used 8 -lag-in-frames 0 -usage 0 \
-enable-global-motion 1 -enable-intrabc 0 -g 240 -keyint_min 240 -sc_threshold 0 -crf 30"

# 长 GOP 跨回绕：`--wrap` 只跑这几条（每条 200 帧，比主流程慢得多）。
WRAP="${WRAPFRAMES:-200}"
wrap_only=0
if [ "${1:-}" = "--wrap" ]; then wrap_only=1; shift; fi

echo "== AV1 像素回归（$FRAMES 帧/条，驱动 $LIB）=="
if [ "$wrap_only" = 0 ]; then
check g20            "$SRC"                                  "$AOM -g 20"  yuv420p 640x360
check all-intra      "$SRC"                                  "$AOM -g 1"   yuv420p 640x360
check g240-long      "$SRC"                                  "$AOM -g 240 -keyint_min 240 -sc_threshold 0" yuv420p 640x360
check g60-cpu6       "$SRC"                                  "$AOM -g 60 -cpu-used 6" yuv420p 640x360
check r720           "testsrc2=size=1280x720:rate=25"        "$AOM -g 20"  yuv420p 1280x720
check r854x480       "testsrc2=size=854x480:rate=25"         "$AOM -g 20"  yuv420p 854x480
check r1080          "testsrc2=size=1920x1080:rate=25"       "$AOM -g 20"  yuv420p 1920x1080
check bit10          "testsrc2=size=640x360:rate=25,format=yuv420p10le" "$AOM -g 20 -pix_fmt yuv420p10le" yuv420p10le 640x360
check lossless-cq0   "$SRC"                                  "-c:v libaom-av1 -cpu-used 8 -usage 1 -b:v 0 -crf 0 -g 20" yuv420p 640x360
# 全局运动（is_global/is_rot_zoom/gm_params，规范 5.9.24）。此前恒写
# is_global=0，带 gm 的流整帧运动补偿丢失：实测 gm-pan 第 4 帧起 13% 像素偏差、
# 第 5~23 帧 100% 偏差。编码结构与 lossless/g240 都不重叠，必须单独一条。
check gm-pan         "$PAN"                                  "$AOM_GM" yuv420p 640x360
# 多 tile：tile_info/tile_group 的长度与排列只在多 tile 时出现。
check tiles2x1       "testsrc2=size=1280x720:rate=25"       "$AOM -tile-columns 1 -g 20" yuv420p 1280x720
# 屏幕内容：intrabc + palette（帧内帧也要写 gm 语法，见 5.9.24 的调用位置）。
check screen-10b     "testsrc=size=640x360:rate=25,format=yuv420p10le" "$AOM -enable-global-motion 1 -enable-intrabc 1 -enable-palette 1 -g 240 -keyint_min 240 -sc_threshold 0 -pix_fmt yuv420p10le -crf 20" yuv420p10le 640x360
if $FFMPEG -hide_banner -encoders 2>/dev/null | grep -q libsvtav1; then
    check svt-ra       "$SRC"                                "-c:v libsvtav1 -preset 8 -g 20" yuv420p 640x360
    check svt-ld       "$SRC"                                "-c:v libsvtav1 -preset 8 -g 240 -keyint_min 240 -svtav1-params preset=2" yuv420p 640x360
fi
fi

# order hint 回绕：B 金字塔 + 长 GOP，参考帧与本帧的 order hint 差会跨过半量程，
# 此时 get_relative_dist 的**符号**由 "v 是否严格大于 ohm/2" 决定（恰好等于
# ohm/2 算正向）。差一位就少写/多写 skip_mode_present，帧头整体错位 1 位，
# 错误还会随参考链传到 GOP 结尾 —— 实测 B 站 1080p60 流 1410/1800 帧坏就来自这里。
# 每条 $WRAP 帧（>128）才够跨回绕；aom 这里**不能**带 -lag-in-frames 0，
# 否则编码器不排 B 帧，层级浅到踩不到回绕。
AOM_RA="-c:v libaom-av1 -cpu-used 8 -b:v 1M -t 4"
check wrap-aom-ra     "$SRC"  "$AOM_RA -g 240 -keyint_min 240 -sc_threshold 0" yuv420p 640x360 $WRAP
check wrap-aom-g48    "$SRC"  "$AOM_RA -g 48  -keyint_min 48  -sc_threshold 0" yuv420p 640x360 $WRAP
if $FFMPEG -hide_banner -encoders 2>/dev/null | grep -q libsvtav1; then
    # SVT 的随机访问结构与 aom 不同，B 层数更深，单独覆盖。
    check wrap-svt-ra "$SRC" "-c:v libsvtav1 -preset 8 -b:v 1M -g 240 -keyint_min 240 -sc_threshold 0" yuv420p 640x360 $WRAP
fi

echo
echo "AV1 像素回归: 通过 $pass，失败 $fail，跳过 $skip"
[ "$fail" = 0 ]
