#!/bin/bash
# AV1 Pixel regression: Multi-set coding structure softens by bytes, each at the same time. ffmpeg and Chrome Two contracts.。
#
# Why is a single script: only a 640x360 AV1 stream is available
# GOP (B Pyramid Deep, DPB Frequent), All-In Frame, 10bit, non-128 align widths of these
# The structure of the reference/shot log is really stepped on. The double-way decoding and the silent drop of hardware are all in this pocket.
# The path is not secure by one stream.
#
# Usage:
#   ./tests/regress_av1_pixels.sh
#   ./tests/regress_av1_pixels.sh --wrap   # Just run. order hint Long sequences around
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

# md5 + Frames。rawvideo Headless, frame numbers are removed by square bytes: ffmpeg - I’ll take the frame.
# Take the stats line, and -v error will swallow this line, only by the length of the document.
stat_of() {
    local f=$1 fmt=$2 w=${3%x*} h=${3#*x} bytes
    case $fmt in
        *10le|*12le|*16le) bytes=$(( w * h * 3 )) ;;
        *)                 bytes=$(( w * h * 3 / 2 )) ;;
    esac
    printf '%s %s' "$(md5sum "$f" | cut -d' ' -f1)" \
        "$(( $(stat -c%s "$f" 2>/dev/null || echo 0) / bytes ))"
}

# check <Name> <Enter Parameters> <Encoding Parameters> <pix_fmt> <Width x High> [Frames overwrite]
check() {
    local name=$1 srcarg=$2 enc=$3 fmt=$4 size=$5
    local nf=${6:-$FRAMES}
    if ! $FFMPEG -v error -f lavfi -i "$srcarg" -frames:v "$nf" $enc \
            -f obu "$W/$name.obu" -y 2>/dev/null; then
        printf '  %-14s Skip (coding failed)）\n' "$name"
        skip=$((skip + 1)); return
    fi
    local sw hw cr
    $FFMPEG -v error -i "$W/$name.obu" -pix_fmt "$fmt" -f rawvideo \
        "$W/$name.sw.yuv" -y 2>/dev/null
    sw=$(stat_of "$W/$name.sw.yuv" "$fmt" "$size")
    timeout 300 $FFMPEG -v error -hwaccel vaapi -i "$W/$name.obu" \
        -pix_fmt "$fmt" -f rawvideo "$W/$name.hw.yuv" -y 2>/dev/null
    hw=$(stat_of "$W/$name.hw.yuv" "$fmt" "$size")
    # Chrome Compact：Chrome Just the tune. BeginPicture/RenderPicture/EndPicture，
    # Never Sync/Delive/GetImage, pixels must be in place when EndPicture returns. ffmpeg go
    # map Path，vaMapBuffer In there. dmd_surface_wait You’ll get it."Not yet."Get it back!，
    # So, md5 The Green and Browser jumped. Turn off the back of the drive, and you can see the sample window.
    #
    # ⁇  This trip** is not all-powerful**: it only covers the "Pixels in Time" type of time series defects.
    # Submit rhythm differences failed - ffmpeg poured over 6 frames, something that was wrong in the drive
    # Often the hardware is submitted for correction before it actually works.
    # (See, for example, the first PPS log of the decode.c: 3177, and the first PPS l0=0,
    # tests/browser/verify_pps_fix.sh）。
    DMD_NO_MAP_WAIT=1 timeout 300 $FFMPEG -v error -hwaccel vaapi \
        -i "$W/$name.obu" -pix_fmt "$fmt" -f rawvideo \
        "$W/$name.cr.yuv" -y 2>/dev/null
    cr=$(stat_of "$W/$name.cr.yuv" "$fmt" "$size")
    rm -f "$W/$name.sw.yuv" "$W/$name.hw.yuv" "$W/$name.cr.yuv"
    local empty=d41d8cd98f00b204e9800998ecf8427e
    if [ "${sw%% *}" = "${hw%% *}" ] && [ "${sw%% *}" != "$empty" ] \
        && [ "${sw%% *}" = "${cr%% *}" ]; then
        printf '  %-14s ✅ Bytes Bytes Same (%s frame, %s)\n' "$name" "${sw##* }" "${sw%% *}"
        pass=$((pass + 1))
    else
        printf '  %-14s ❌ software decode[%s] hardware decode[%s]' "$name" "$sw" "$hw"
        [ "${sw%% *}" = "${hw%% *}" ] && [ "${sw%% *}" != "$empty" ] && \
            printf ' —— ffmpeg The contract.、Chrome Contract only (sampling window exists)）[%s]' "$cr"
        printf '\n'
        fail=$((fail + 1))
    fi
}

SRC="testsrc2=size=640x360:rate=25"
AOM="-c:v libaom-av1 -cpu-used 8 -lag-in-frames 0 -b:v 2M"

# Global campaign source: The whole picture is flat, aom will make it a gram_params of ROTZOOM/ AFFINE.
# The only source is that gm only appears when "the whole situation really exists" -- tessrc2 is not moving itself.
# Use the corp to move t to stabilize gm (actually, there are 8 frames in 24 is_global=1).
PAN="testsrc2=size=1280x720:rate=25,crop=640:360:'mod(t*120,640)':'mod(t*80,360)'"
AOM_GM="-c:v libaom-av1 -cpu-used 8 -lag-in-frames 0 -usage 0 \
-enable-global-motion 1 -enable-intrabc 0 -g 240 -keyint_min 240 -sc_threshold 0 -crf 30"

# Long GOP back: `-wrap ' only runs these (200 frames per frame, much slower than the main process).
WRAP="${WRAPFRAMES:-200}"
wrap_only=0
if [ "${1:-}" = "--wrap" ]; then wrap_only=1; shift; fi

echo "== AV1 Pixels Back（$FRAMES frame/strip, drive $LIB）=="
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
# Global Movement (is_global/is_rot_zoom/gm_params, regulation 5.9.24).
# is_global=0，- Yeah. gm The first round of the roundup was a series of events, which were held in the United States of America. gm-pan number 4 Frame 13% Pixel deviation、
# Frame 5-23 is 100% deviation. The coding structure does not overlap with the lossless/g240, and must be a separate article.
check gm-pan         "$PAN"                                  "$AOM_GM" yuv420p 640x360
# The length and alignment of multiple tile:tile_info/tile_group only appear when more tile is available.
check tiles2x1       "testsrc2=size=1280x720:rate=25"       "$AOM -tile-columns 1 -g 20" yuv420p 1280x720
# Screen contents: intrabc + palette (the frame also writes gm syntax, see 5.9.24 for call location).
check screen-10b     "testsrc=size=640x360:rate=25,format=yuv420p10le" "$AOM -enable-global-motion 1 -enable-intrabc 1 -enable-palette 1 -g 240 -keyint_min 240 -sc_threshold 0 -pix_fmt yuv420p10le -crf 20" yuv420p10le 640x360
if $FFMPEG -hide_banner -encoders 2>/dev/null | grep -q libsvtav1; then
    check svt-ra       "$SRC"                                "-c:v libsvtav1 -preset 8 -g 20" yuv420p 640x360
    check svt-ld       "$SRC"                                "-c:v libsvtav1 -preset 8 -g 240 -keyint_min 240 -svtav1-params preset=2" yuv420p 640x360
fi
fi

# order hint Rewind：B Pyramid + Long GOP，The frame of reference and the frame. order hint We’re halfway through the process.，
# ** The symbol gets_relative_dist** is determined by "v is more stringent than ohm/2" (just equals
# ohm/2 Positive. Less than one./More skip_mode_present，Frame Whole Altitude 1 bits，
# Errors will also be passed on to the end of the GOP reference chain -- station B is measured to be 1080p60 stream 1410/1800 from which the frame is broken.
# Each wall is enough to cross; aom ** can't **lag-in-frames 0,
# Otherwise the encoder will not be in B frames, level down to no backsliding.
AOM_RA="-c:v libaom-av1 -cpu-used 8 -b:v 1M -t 4"
check wrap-aom-ra     "$SRC"  "$AOM_RA -g 240 -keyint_min 240 -sc_threshold 0" yuv420p 640x360 $WRAP
check wrap-aom-g48    "$SRC"  "$AOM_RA -g 48  -keyint_min 48  -sc_threshold 0" yuv420p 640x360 $WRAP
if $FFMPEG -hide_banner -encoders 2>/dev/null | grep -q libsvtav1; then
    # SVT random access structure with aom Different，B Deeper layer, individual overlay。
    check wrap-svt-ra "$SRC" "-c:v libsvtav1 -preset 8 -b:v 1M -g 240 -keyint_min 240 -sc_threshold 0" yuv420p 640x360 $WRAP
fi

echo
echo "AV1 Pixels Back: Pass. $pass，failed $fail，skip $skip"
[ "$fail" = 0 ]
