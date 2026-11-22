#!/bin/bash
# AV1 Re-resolution regression -- hard-coupled snipes（AV1 Reuse same VA context，It’s the only one.
# The second sorcerer of SOURCE_CHANGE will be received in the same session
#
# Usage:
#   ./tests/regress_av1_reschange.sh [Driver Directory]
#   When not loaded: ./tess/regress_av1_reschange.sh build
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
LIB="${1:-$HERE/build}"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export LIBVA_DRIVERS_PATH="$LIB"

FR=25          # Number of frames per segment
pass=0; fail=0; skip=0

say() { printf '  %-34s %s\n' "$1" "$2"; }

encode() {     # encode <Dimensions> <Documentation>
    ffmpeg -v error -f lavfi -i "testsrc2=size=$1:rate=25" -frames:v $FR \
        -c:v libaom-av1 -cpu-used 8 -b:v 600k -g 12 -strict experimental \
        -f ivf "$2" -y 2>/dev/null
}

framd() {      # framemd <Enter> <Output> [Additional parameters...]
    local inp=$1 out=$2; shift 2
    timeout 600 ffmpeg -v error -y "$@" -i "$inp" -pix_fmt nv12 \
        -f framemd5 "$out" 2>"$out.err" || return 1
    return 0
}

# Compare two framed5 lines <start>..end..
cmp_range() {  # cmp_range <sw.md5> <hw.md5> <Get up.> <Stop> -> Print Different Frames
    local a b i e
    a=$(grep '^[0-9]' "$1" | awk -F, '{print $6}')
    b=$(grep '^[0-9]' "$2" | awk -F, '{print $6}')
    paste <(echo "$a") <(echo "$b") |
        awk -F'\t' -v s="$3" -v e="$4" 'NR>=s && NR<=e { if ($1==$2) n++; else m++ }
                                        END { printf "%d/%d", n+0, n+m+0 }'
}

mklist() {     # mklist <Documentation> <Snippets...>
    local f=$1; shift
    : > "$f"
    for c in "$@"; do printf "file '%s'\n" "$c" >> "$f"; done
}

echo "== Generate Code Stream（libaom Slower, Joe. 1 min）=="
encode 1280x720 "$W/a.ivf"     # Build pool size
encode 854x480  "$W/b.ivf"     # Smallen
encode 640x360  "$W/c.ivf"     # Smaller.
encode 1280x720 "$W/d.ivf"     # Larger (clients have not rebuilt pool) ⇒ It’s only possible for the upper left corner.）
[ -s "$W/a.ivf" ] || { echo "Not available libaom-av1 Encoder, skip"; exit 0; }

mklist "$W/shrink.txt" "$W/a.ivf" "$W/b.ivf" "$W/c.ivf"
mklist "$W/churn.txt"  "$W/a.ivf" "$W/b.ivf" "$W/c.ivf" "$W/a.ivf" "$W/b.ivf"
mklist "$W/grow.txt"   "$W/c.ivf" "$W/d.ivf" "$W/c.ivf"

# ------------------------------------------------- 1. Small: Bytes must be consistent
echo "== Smaller resolution (synthesis rematched) CAPTURE）=="
if framd "$W/shrink.txt" "$W/sw1.md5" -f concat -safe 0 &&
   framd "$W/shrink.txt" "$W/hw1.md5" -f concat -safe 0 \
        -hwaccel vaapi -hwaccel_device /dev/dri/renderD128; then
    got=$(cmp_range "$W/sw1.md5" "$W/hw1.md5" 1 $((FR*3)))
    if [ "$got" = "$((FR*3))/$((FR*3))" ]; then
        say "720p→480p→360p" "✅ Bytes Bytes Same ($got frame)"
        pass=$((pass+1))
    else
        say "720p→480p→360p" "❌ Only $got Frames are consistent."
        fail=$((fail+1))
    fi
else
    say "720p→480p→360p" "❌ Decoding failed（$(tail -2 "$W/hw1.md5.err" | head -1)）"
    fail=$((fail+1))
fi

# ------------------------------------------------- 2. Multiple alternations: the whole article is identical
echo "== Multiple rotational switching (with one rematching for each section)）=="
if framd "$W/churn.txt" "$W/sw2.md5" -f concat -safe 0 &&
   framd "$W/churn.txt" "$W/hw2.md5" -f concat -safe 0 \
        -hwaccel vaapi -hwaccel_device /dev/dri/renderD128; then
    n=$((FR*5))
    got=$(cmp_range "$W/sw2.md5" "$W/hw2.md5" 1 $n)
    rows=$(grep -c '^[0-9]' "$W/hw2.md5")
    if [ "$got" = "$n/$n" ] && [ "$rows" = "$n" ]; then
        say "5 Paragraph 4 Sub-toggle" "✅ Bytes Bytes Same ($got frame)"
        pass=$((pass+1))
    else
        say "5 Paragraph 4 Sub-toggle" "❌ Frame $rows/$n，Unanimously. $got"
        fail=$((fail+1))
    fi
else
    say "5 Paragraph 4 Sub-toggle" "❌ Decoding failed（$(tail -2 "$W/hw2.md5.err" | head -1)）"
    fail=$((fail+1))
fi

# ------------------------------------------------- 3. Larger: No falling, frame numbered.
# The client does not rebuild the surface pool when it changes resolution (the result is still vaGetImage) and the user is not able to recreate the surface pool when it is re-established.
# The frame can't fit the whole picture -- the drive is to keep it in the air, to give up the whole stream, to get the frame number and to get it soft.
# Same, and ** it's still bytes correct after the small sizes.**
echo "== Resolution size (pool not rebuilt, degraded to top left corner cropping)）=="
if framd "$W/grow.txt" "$W/sw3.md5" -f concat -safe 0 &&
   framd "$W/grow.txt" "$W/hw3.md5" -f concat -safe 0 \
        -hwaccel vaapi -hwaccel_device /dev/dri/renderD128; then
    n=$((FR*3))
    rows=$(grep -c '^[0-9]' "$W/hw3.md5")
    head_ok=$(cmp_range "$W/sw3.md5" "$W/hw3.md5" 1 $FR)
    tail_ok=$(cmp_range "$W/sw3.md5" "$W/hw3.md5" $((FR*2+1)) $((FR*3)))
    big_same=$(cmp_range "$W/sw3.md5" "$W/hw3.md5" $((FR+1)) $((FR*2)))
    if [ "$rows" = "$n" ] && [ "$head_ok" = "$FR/$FR" ] &&
       [ "$tail_ok" = "$FR/$FR" ]; then
        say "360p→720p→360p" "✅ Frame $rows/$n，Small sizes match by bytes"
        [ "$big_same" = "$FR/$FR" ] && \
            say "Large frame delivery." "✅ No longer degenerate to crop"
        pass=$((pass+1))
    else
        say "360p→720p→360p" "❌ Frame $rows/$n First Paragraph=$head_ok Last paragraph=$tail_ok"
        fail=$((fail+1))
    fi
else
    say "360p→720p→360p" "❌ Decoding failed（$(tail -2 "$W/hw3.md5.err" | head -1)）"
    fail=$((fail+1))
fi

# ------------------------------------------------- 4. Other codec Not affected
echo "== Control：HEVC / H.264 Change Resolution (upgrade will be created) context）=="
for spec in "hevc:-c:v libx265" "h264:-c:v libx264"; do
    name=${spec%%:*}; enc=${spec#*:}
    mux=hevc; [ "$name" = h264 ] && mux=h264
    if ! ffmpeg -v error -f lavfi -i "testsrc2=size=1280x720:rate=25" \
            -frames:v $FR $enc -g 12 -f $mux "$W/$name-a.$mux" -y 2>/dev/null; then
        say "$name" "⚠️ Unencrypted encoder, skip"
        skip=$((skip+1)); continue
    fi
    ffmpeg -v error -f lavfi -i "testsrc2=size=854x480:rate=25" \
        -frames:v $FR $enc -g 12 -f $mux "$W/$name-b.$mux" -y 2>/dev/null
    mklist "$W/$name.txt" "$W/$name-a.$mux" "$W/$name-b.$mux"
    framd "$W/$name.txt" "$W/sw-$name.md5" -f concat -safe 0 || true
    framd "$W/$name.txt" "$W/hw-$name.md5" -f concat -safe 0 \
        -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 || true
    got=$(cmp_range "$W/sw-$name.md5" "$W/hw-$name.md5" 1 $((FR*2)))
    if [ "$got" = "$((FR*2))/$((FR*2))" ]; then
        say "$name 720p→480p" "✅ Bytes Bytes Same ($got frame)"
        pass=$((pass+1))
    else
        say "$name 720p→480p" "❌ Only $got Frames are consistent."
        fail=$((fail+1))
    fi
done

echo
echo "AV1 Change Resolution Re-entry: Pass. $pass，failed $fail，skip $skip"
[ "$fail" = 0 ]
