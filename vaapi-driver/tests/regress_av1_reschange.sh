#!/bin/bash
# AV1 流中换分辨率回归 —— 硬解对拍软解（AV1 复用同一个 VA context，是唯一
# 会在同一会话里收到第二次 SOURCE_CHANGE 的 codec）
#
# 用法:
#   ./tests/regress_av1_reschange.sh [驱动目录]
#   不装驱动时: ./tests/regress_av1_reschange.sh build
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
LIB="${1:-$HERE/build}"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export LIBVA_DRIVERS_PATH="$LIB"

FR=25          # 每段帧数
pass=0; fail=0; skip=0

say() { printf '  %-34s %s\n' "$1" "$2"; }

encode() {     # encode <尺寸> <文件>
    ffmpeg -v error -f lavfi -i "testsrc2=size=$1:rate=25" -frames:v $FR \
        -c:v libaom-av1 -cpu-used 8 -b:v 600k -g 12 -strict experimental \
        -f ivf "$2" -y 2>/dev/null
}

framd() {      # framemd <输入> <输出> [额外参数...]
    local inp=$1 out=$2; shift 2
    timeout 600 ffmpeg -v error -y "$@" -i "$inp" -pix_fmt nv12 \
        -f framemd5 "$out" 2>"$out.err" || return 1
    return 0
}

# 比较两份 framemd5 的第 <start>..<end> 行（1 基，闭区间）
cmp_range() {  # cmp_range <sw.md5> <hw.md5> <起> <止> -> 打印不同帧数
    local a b i e
    a=$(grep '^[0-9]' "$1" | awk -F, '{print $6}')
    b=$(grep '^[0-9]' "$2" | awk -F, '{print $6}')
    paste <(echo "$a") <(echo "$b") |
        awk -F'\t' -v s="$3" -v e="$4" 'NR>=s && NR<=e { if ($1==$2) n++; else m++ }
                                        END { printf "%d/%d", n+0, n+m+0 }'
}

mklist() {     # mklist <文件> <片段...>
    local f=$1; shift
    : > "$f"
    for c in "$@"; do printf "file '%s'\n" "$c" >> "$f"; done
}

echo "== 生成码流（libaom 较慢，约 1 分钟）=="
encode 1280x720 "$W/a.ivf"     # 建池尺寸
encode 854x480  "$W/b.ivf"     # 变小
encode 640x360  "$W/c.ivf"     # 再变小
encode 1280x720 "$W/d.ivf"     # 变大（客户端未重建池 ⇒ 只可能给左上角）
[ -s "$W/a.ivf" ] || { echo "缺 libaom-av1 编码器，跳过"; exit 0; }

mklist "$W/shrink.txt" "$W/a.ivf" "$W/b.ivf" "$W/c.ivf"
mklist "$W/churn.txt"  "$W/a.ivf" "$W/b.ivf" "$W/c.ivf" "$W/a.ivf" "$W/b.ivf"
mklist "$W/grow.txt"   "$W/c.ivf" "$W/d.ivf" "$W/c.ivf"

# ------------------------------------------------- 1. 变小：必须逐字节一致
echo "== 分辨率变小（同会话重配 CAPTURE）=="
if framd "$W/shrink.txt" "$W/sw1.md5" -f concat -safe 0 &&
   framd "$W/shrink.txt" "$W/hw1.md5" -f concat -safe 0 \
        -hwaccel vaapi -hwaccel_device /dev/dri/renderD128; then
    got=$(cmp_range "$W/sw1.md5" "$W/hw1.md5" 1 $((FR*3)))
    if [ "$got" = "$((FR*3))/$((FR*3))" ]; then
        say "720p→480p→360p" "✅ 逐字节一致 ($got 帧)"
        pass=$((pass+1))
    else
        say "720p→480p→360p" "❌ 只有 $got 帧一致"
        fail=$((fail+1))
    fi
else
    say "720p→480p→360p" "❌ 解码失败（$(tail -2 "$W/hw1.md5.err" | head -1)）"
    fail=$((fail+1))
fi

# ------------------------------------------------- 2. 多次交替：整条一致
echo "== 多次交替切换（每一段都付一次重配）=="
if framd "$W/churn.txt" "$W/sw2.md5" -f concat -safe 0 &&
   framd "$W/churn.txt" "$W/hw2.md5" -f concat -safe 0 \
        -hwaccel vaapi -hwaccel_device /dev/dri/renderD128; then
    n=$((FR*5))
    got=$(cmp_range "$W/sw2.md5" "$W/hw2.md5" 1 $n)
    rows=$(grep -c '^[0-9]' "$W/hw2.md5")
    if [ "$got" = "$n/$n" ] && [ "$rows" = "$n" ]; then
        say "5 段 4 次切换" "✅ 逐字节一致 ($got 帧)"
        pass=$((pass+1))
    else
        say "5 段 4 次切换" "❌ 出帧 $rows/$n，一致 $got"
        fail=$((fail+1))
    fi
else
    say "5 段 4 次切换" "❌ 解码失败（$(tail -2 "$W/hw2.md5.err" | head -1)）"
    fail=$((fail+1))
fi

# ------------------------------------------------- 3. 变大：不许崩，帧数齐
# 客户端换分辨率时不重建 surface 池（实测仍按建池尺寸调 vaGetImage），
# 大帧本来就装不下整幅 —— 驱动保证的是：不崩、不整条流放弃、出帧数与软解
# 相同，并且**切回小尺寸之后仍然逐字节正确**。
echo "== 分辨率变大（池不重建，退化为左上角裁剪）=="
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
        say "360p→720p→360p" "✅ 出帧 $rows/$n，小尺寸段逐字节一致"
        [ "$big_same" = "$FR/$FR" ] && \
            say "大帧整幅交付" "✅ 已不再退化为裁剪"
        pass=$((pass+1))
    else
        say "360p→720p→360p" "❌ 出帧 $rows/$n 首段=$head_ok 末段=$tail_ok"
        fail=$((fail+1))
    fi
else
    say "360p→720p→360p" "❌ 解码失败（$(tail -2 "$W/hw3.md5.err" | head -1)）"
    fail=$((fail+1))
fi

# ------------------------------------------------- 4. 其他 codec 不受影响
echo "== 对照组：HEVC / H.264 换分辨率（上层会新建 context）=="
for spec in "hevc:-c:v libx265" "h264:-c:v libx264"; do
    name=${spec%%:*}; enc=${spec#*:}
    mux=hevc; [ "$name" = h264 ] && mux=h264
    if ! ffmpeg -v error -f lavfi -i "testsrc2=size=1280x720:rate=25" \
            -frames:v $FR $enc -g 12 -f $mux "$W/$name-a.$mux" -y 2>/dev/null; then
        say "$name" "⚠️ 缺编码器，跳过"
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
        say "$name 720p→480p" "✅ 逐字节一致 ($got 帧)"
        pass=$((pass+1))
    else
        say "$name 720p→480p" "❌ 只有 $got 帧一致"
        fail=$((fail+1))
    fi
done

echo
echo "AV1 换分辨率回归: 通过 $pass，失败 $fail，跳过 $skip"
[ "$fail" = 0 ]
