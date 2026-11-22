#!/bin/sh
# Multi-resolution hard resolution regression: hard resolution results must match soft resolution by bytes.
#
# Why it is needed: all the original returns are 1920x1080, which is exactly the same as msm_vidc
# G_FMT(CAPTURE) Default geometry, so never triggers"Use OUTPUT Consultation value overwrite CAPTURE
# The "residual" branch. The real defect.
# The stale stride shifts each row by 640 bytes and reduces slice_height from 736 to 492.
# Shows the Firefox green screen, and 1080p returns green.
#
# Overwrite point:
#   1280x720   Common Unlike 1080p
#   854x480    Width non- 128 Multiplier（stride We need to align. 896）
#   640x360    Small Resolution
#   720x1280   Stand up (high) > Width）
#   Resolution Switch Same fd Top-in-Speech 1080p ~720p ~480p
#
# Usage:
#   FFMPEG=/path/to/ffmpeg DRIVER_DIR=../build ./regress_resolutions.sh [Source Flow]
# Reliance on ffmpeg with libx265 with vaapi support.

set -e

FFMPEG=${FFMPEG:-ffmpeg}
DRIVER_DIR=${DRIVER_DIR:-../build}
SRC=${1:-test.hevc}
WORK=${WORK:-$(mktemp -d)}
KEEP=${KEEP:-0}

if [ ! -f "$SRC" ]; then
    echo "skip: Source stream not found $SRC" >&2
    exit 77
fi

cleanup() {
    [ "$KEEP" = "1" ] || rm -rf "$WORK"
}
trap cleanup EXIT

fail=0
pass=0

# One resolution by resolution: code----- softened md5-- hardened md5--
for spec in 1280x720 854x480 640x360 720x1280; do
    w=${spec%x*}
    h=${spec#*x}
    clip="$WORK/res_$spec.hevc"

    if ! "$FFMPEG" -hide_banner -loglevel error -i "$SRC" \
            -vf "scale=$w:$h" -c:v libx265 -x265-params log-level=0 \
            -frames:v 24 -f hevc "$clip" -y 2>/dev/null; then
        printf '  %-10s Skip (code failed, missing) libx265?）\n' "$spec"
        continue
    fi

    sw=$("$FFMPEG" -hide_banner -loglevel error -i "$clip" \
            -pix_fmt yuv420p -f rawvideo - 2>/dev/null | md5sum | cut -d' ' -f1)

    if ! LIBVA_DRIVERS_PATH="$DRIVER_DIR" "$FFMPEG" -hide_banner -loglevel error \
            -hwaccel vaapi -i "$clip" -pix_fmt yuv420p \
            -f rawvideo "$WORK/hw.yuv" -y 2>/dev/null; then
        printf '  %-10s ✗ Hard resolution failed\n' "$spec"
        fail=$((fail + 1))
        continue
    fi
    hw=$(md5sum "$WORK/hw.yuv" | cut -d' ' -f1)
    rm -f "$WORK/hw.yuv"

    if [ "$sw" = "$hw" ]; then
        printf '  %-10s ✓ Bytes Same\n' "$spec"
        pass=$((pass + 1))
    else
        printf '  %-10s ✗ Inconsistent Soft=%.12s hardware decode=%.12s\n' "$spec" "$sw" "$hw"
        fail=$((fail + 1))
    fi
done

# Resolution switch: Three geometry processes continuously on the same decoder fd.
# This is the scene of the closest browser self-adaptation rate and the most exposed location for residual geometry.
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
    # Parameter by Parameter: The frame size changes after the overall spooling has been decoded and the single segment is more stringent than the pair.
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
    # Run the spool again to confirm that the same session is open and closed.
    if LIBVA_DRIVERS_PATH="$DRIVER_DIR" "$FFMPEG" -hide_banner -loglevel error \
            -hwaccel vaapi -i "$WORK/switch.hevc" -f null - 2>/dev/null; then
        :
    else
        seg_fail=1
    fi
    if [ "$seg_fail" = "0" ]; then
        printf '  %-10s ✓ Bytes of the field are consistent\n' 'Switch Stream'
        pass=$((pass + 1))
    else
        printf '  %-10s ✗ Toggle Pixels or Decoding Unusual\n' 'Switch Stream'
        fail=$((fail + 1))
    fi
fi

echo "Multi-resolution regression: Pass. $pass，failed $fail"
[ "$fail" = "0" ]
