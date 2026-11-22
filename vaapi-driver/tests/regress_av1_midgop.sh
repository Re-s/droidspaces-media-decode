#!/bin/bash
# AV1 From GOP Centre Decomposition（seek Dropping into non-key frame / Half-clan flow’s misconversion.。
#
# Why a single script: regress_av1_pixels.sh each stream starts with the key frame and does not cover it
# "The first few frames of reference do not exist."This type. This type of flow is before it’s fixed.**The whole thing is dead.**：
# Venus accepts frames that reference empty DPB slots but returns neither a frame nor an error.
# vaSyncSurface White 2s（Trigger irreversible. finish_input）Wait a minute. 5s timed out，
# ffmpeg Discard the entire bar stream when you receive an error -- actual rc=251、Output 0 frame。
#
# Two variants press on two paths separately (the same stream, the difference being only whether there is a serial head in the first bag after the intercept):
#   plain  —— Copy as。GOP The middle bag has no serial head.，ffmpeg I can’t solve those frames myself.、
#             Dropped up upstream, driving them out of reach. Expect: Output = those frames behind the key frame,
#             And the image is the same by-section as "exactly excavated from this key frame."
#   seqhdr —— Put it in the key box back there. OBU_SEQUENCE_HEADER The first bag is mentioned. The sequence is all in line.，
#             The frames will be parsed and fed to the driver -- the drive is pressured on dmd_av1_refs_resolve
#             The road: Unquoted frame delivered by empty shell (READY + VA_STATUS_SUCCESS)
#             Neither can you sync the vaEndPicture (the error will allow the caller to be returned)
#             Discards the entire bar stream. Expect: Output = All Packages (with empty shells), Behind Key Frames
#             Consistent with soft resolution by bytes.
#
# ⁇  No empty shell delivery can be saved, but no misdirected position can be wrong: the shadow slot is registered.
# patch_prev_refresh Ree. refresh From this frame. map The difference is all over the place.），
# The DPB, and every frame is then judged "not to be quoted."
# Parsing "and the cascades were lost -- the actual cascade 43 frames only have KEY and a frame next.
#
# Usage: ./tess/regress_av1_midgop.sh
#       DRIVER_DIR=../build FRAMES=36 GOP=12 CUT=5 ./tests/regress_av1_midgop.sh
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
LIB="${DRIVER_DIR:-$DIR/../build}"
FFMPEG="${FFMPEG:-ffmpeg}"
PY="${PYTHON:-python3}"
FRAMES="${FRAMES:-36}"
GOP="${GOP:-12}"
CUT="${CUT:-5}"                 # Start with the first batch.（> 0 and < GOP Yes. GOP Middle）
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export LIBVA_DRIVERS_PATH="$LIB"

SIZE_W=640
SIZE_H=360
BPE=$(( SIZE_W * SIZE_H * 3 / 2 ))   # nv12 Bytes per frame

pass=0; fail=0
ok()  { printf '  %-34s ✅ %s\n' "$1" "$2"; pass=$((pass + 1)); }
bad() { printf '  %-34s ❌ %s\n' "$1" "$2"; fail=$((fail + 1)); }

# md5from <Documentation> <Skip Frame Count> —— Only end of the term. md5，Avoiding cross-referencing of documents to hide differences in the headlines
md5from() {
    dd if="$1" bs=$BPE skip="$2" status=none | md5sum | cut -d' ' -f1
}
nframes() { printf '%s' "$(( $(stat -c%s "$1" 2>/dev/null || echo 0) / BPE ))"; }

echo "== AV1 From GOP Centre Decomposition（$FRAMES frame / GOP=$GOP / From the first $CUT Wrap up, drive $LIB）=="

# ---- The current.：36 Frames, every 12 A frame. A key frame. IVF ----
# sc_threshold 0 + keyint_min=gop To be stable. 0/12/24 Three key frames.；
# The scene switching test goes around the key frame, and the premise "exact from the GOP" will fail.
if ! $FFMPEG -v error -f lavfi \
        -i "testsrc2=size=${SIZE_W}x${SIZE_H}:rate=25" \
        -frames:v "$FRAMES" -c:v libaom-av1 -cpu-used 8 -lag-in-frames 0 \
        -b:v 2M -g "$GOP" -keyint_min "$GOP" -sc_threshold 0 \
        -pix_fmt yuv420p "$W/full.ivf" -y 2>/dev/null; then
    echo "  Skip: Encoding failed (no) libaom-av1？）"
    exit 77
fi
if ! $PY "$DIR/ivf_cut.py" "$W/full.ivf" "$W/plain.ivf" "$CUT" \
        > "$W/plain.info" 2>/dev/null; then
    echo "  Skip: Failed to intercept（ffprobe/python3 unavailable？）"
    exit 77
fi
TOTAL=$(sed -n 's/.*TOTAL=\([0-9]*\).*/\1/p' "$W/plain.info")
KEY_AT=$(sed -n 's/.*KEY_AT=\([0-9]*\).*/\1/p' "$W/plain.info")
if [ -z "$TOTAL" ] || [ "$KEY_AT" = "None" ] || [ "$KEY_AT" -lt 1 ] \
    || [ "$KEY_AT" -gt "$((TOTAL - 4))" ]; then
    echo "  Skip: Unsuited key frame position after intercept（TOTAL=${TOTAL:-?} KEY_AT=${KEY_AT:-?}）"
    exit 77
fi
$PY "$DIR/ivf_cut.py" --seqhdr "$W/full.ivf" "$W/seq.ivf" "$CUT" \
    > "$W/seq.info" 2>/dev/null
$PY "$DIR/ivf_cut.py" "$W/full.ivf" "$W/key.ivf" "$((CUT + KEY_AT))" \
    > /dev/null 2>&1
# Reference: A soft solution (the point of departure is legal, no frame should be missing) that is to be aligned from that key frame.
$FFMPEG -v error -i "$W/key.ivf" -pix_fmt nv12 -f rawvideo "$W/key.sw.yuv" -y 2>/dev/null
REF=$(md5from "$W/key.sw.yuv" 0)
if [ "$(nframes "$W/key.sw.yuv")" != "$((TOTAL - KEY_AT))" \
    ] || [ "$REF" = d41d8cd98f00b204e9800998ecf8427e ]; then
    echo "  Skip: Reference decoding anomaly（$(nframes "$W/key.sw.yuv") Frame, read $((TOTAL - KEY_AT))）"
    exit 77
fi

# ---- Control: The hardware is correct when the site is legal. ----
timeout 300 $FFMPEG -v error -hwaccel vaapi -i "$W/key.ivf" \
    -pix_fmt nv12 -f rawvideo "$W/key.hw.yuv" -y 2>/dev/null
rc=$?
if [ "$rc" = 0 ] && [ "$(md5from "$W/key.hw.yuv" 0)" = "$REF" ]; then
    ok "Key frame alignment deconstruct (control group)）" "Same as soft resolution by bytes ($(nframes "$W/key.hw.yuv") frame)"
else
    bad "Key frame alignment deconstruct (control group)）" "rc=$rc hardware decode=$(nframes "$W/key.hw.yuv") frame"
fi

# check_mid <Use the example name> <Enter File> [all]  —— all Organisation"The empty shell is also delivered."，That output must be equal to the number of packages
check_mid() {
    local name=$1 in=$2 want=${3:-}
    timeout 300 $FFMPEG -v error -hwaccel vaapi -i "$in" \
        -pix_fmt nv12 -f rawvideo "$W/$name.hw.yuv" -y 2>/dev/null
    local rc=$? n skip
    n=$(nframes "$W/$name.hw.yuv")
    if [ "$rc" != 0 ]; then
        bad "$name You can’t get stuck, you can’t give up." "rc=$rc，Output $n Frame (sync timed out or abandoned by whole）"
        return
    fi
    # Both modes of delivery are correct: empty shells are delivered together (n=TOTAL), or ffmpeg is unsettled.
    # The header is missing (n=TOTAL-KEY_AT). But the end section must be complete and the frame data should be crossed to determine.
    if [ "$n" = "$TOTAL" ]; then
        skip=$KEY_AT
    elif [ "$n" = "$((TOTAL - KEY_AT))" ]; then
        skip=0
    else
        bad "$name Frames are reasonable." "Output $n Frame, read $TOTAL（(b) or $((TOTAL - KEY_AT))"
        return
    fi
    if [ "$want" = all ] && [ "$n" != "$TOTAL" ]; then
        bad "$name The shell is delivered by frame." "Output Only $n Frame, read $TOTAL（Head $KEY_AT The frame is missing and the path to the shell is missing.）"
        return
    fi
    if [ "$(md5from "$W/$name.hw.yuv" "$skip")" = "$REF" ]; then
        ok "$name Resume accurate footage of critical frames." \
           "Output $n Frame (Jumping) $skip），Behind the key frame. $((TOTAL - KEY_AT)) Frames match soft by bytes"
    else
        bad "$name Resume accurate footage of critical frames." \
            "$(md5from "$W/$name.hw.yuv" "$skip") != $REF"
    fi
    # Chrome Contract: only EndPicture No, no, no. Sync，Pixels are in place. The shell is delivered this way, especially.
    # Easy to just look fine under the ffmpeg 's pop.
    DMD_NO_MAP_WAIT=1 timeout 300 $FFMPEG -v error -hwaccel vaapi -i "$in" \
        -pix_fmt nv12 -f rawvideo "$W/$name.cr.yuv" -y 2>/dev/null
    local n2
    n2=$(nframes "$W/$name.cr.yuv")
    if [ "$n2" = "$n" ] && [ "$(md5from "$W/$name.cr.yuv" "$skip")" = "$REF" ]; then
        ok "$name of Chrome Contract trip." "The end of the field is still bytes-by-te"
    else
        bad "$name of Chrome Contract trip." "Frames $n2（ffmpeg Go on. $n）or end paragraph does not match"
    fi
}

check_mid "Pure Intercept"        "$W/plain.ivf"
check_mid "Toc-Sequence Heading Intercept"  "$W/seq.ivf"  all

echo
printf 'AV1 GOP Centre de-entry: Pass. %d，failed %d\n' "$pass" "$fail"
[ "$fail" = 0 ]
