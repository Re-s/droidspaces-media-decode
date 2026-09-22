#!/bin/bash
# AV1 从 GOP 中间起解（seek 落到非关键帧 / 半截码流）的容错回归。
#
# 为什么单独一个脚本：regress_av1_pixels.sh 每条流都从关键帧开始，覆盖不到
# "头几帧的参考帧压根不存在"这一类。这类流在修好之前是**整条废掉**：
# 驱动把引用空槽的帧原样送进 Venus，Venus 对这种帧既不出帧也不报错，
# vaSyncSurface 白等 2s（触发不可逆的 finish_input）再等到 5s 超时，
# ffmpeg 收到错误后直接放弃整条码流 —— 实测 rc=251、输出 0 帧。
#
# 两个变体分别压两条路（同一份码流，差别只在截取后第一个包里有没有序列头）：
#   plain  —— 原样截取。GOP 中间的包没有序列头，ffmpeg 自己就解不了那几帧、
#             丢在上游，驱动收不到它们。期望：输出 = 关键帧之后的那些帧，
#             且画面与"从该关键帧起解"逐字节一致。
#   seqhdr —— 把后面关键帧包里的 OBU_SEQUENCE_HEADER 提到第一个包。序列头齐了，
#             那几帧就会被解析并喂给驱动 —— 压的是驱动侧 dmd_av1_refs_resolvable
#             那条路：引用不出的帧按空壳交付（READY + VA_STATUS_SUCCESS），
#             既不能卡同步、也不能让 vaEndPicture 返错（返错会让调用方
#             放弃整条码流）。期望：输出 = 全部包（含空壳），关键帧之后
#             与软解逐字节一致。
#
# ⚠️ 空壳交付不能省、但丢帧判断的位置也不能放错：影子槽位的登记发生在
# patch_prev_refresh 里（上一帧的真实 refresh 由本帧的 map 差分反算出来才登记），
# 判断放在它之前会让上一帧永远进不了 DPB，于是其后每一帧都被判成"引用不可
# 解析"而级联误丢 —— 实测级联版 43 帧里只有 KEY 与其后继 1 帧出画。
#
# 用法: ./tests/regress_av1_midgop.sh
#       DRIVER_DIR=../build FRAMES=36 GOP=12 CUT=5 ./tests/regress_av1_midgop.sh
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
LIB="${DRIVER_DIR:-$DIR/../build}"
FFMPEG="${FFMPEG:-ffmpeg}"
PY="${PYTHON:-python3}"
FRAMES="${FRAMES:-36}"
GOP="${GOP:-12}"
CUT="${CUT:-5}"                 # 从第几个包起解（> 0 且 < GOP 才是 GOP 中间）
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export LIBVA_DRIVERS_PATH="$LIB"

SIZE_W=640
SIZE_H=360
BPE=$(( SIZE_W * SIZE_H * 3 / 2 ))   # nv12 每帧字节数

pass=0; fail=0
ok()  { printf '  %-34s ✅ %s\n' "$1" "$2"; pass=$((pass + 1)); }
bad() { printf '  %-34s ❌ %s\n' "$1" "$2"; fail=$((fail + 1)); }

# md5from <文件> <跳过帧数> —— 只取尾段算 md5，避免整文件比对时头段差异掩盖问题
md5from() {
    dd if="$1" bs=$BPE skip="$2" status=none | md5sum | cut -d' ' -f1
}
nframes() { printf '%s' "$(( $(stat -c%s "$1" 2>/dev/null || echo 0) / BPE ))"; }

echo "== AV1 从 GOP 中间起解（$FRAMES 帧 / GOP=$GOP / 从第 $CUT 包起，驱动 $LIB）=="

# ---- 造流：36 帧、每 12 帧一个关键帧，封进 IVF ----
# sc_threshold 0 + keyint_min=gop 才能稳定得到 0/12/24 三个关键帧；
# 场景切换检测一开就到处插关键帧，"从 GOP 中间起解"这个前提会失效。
if ! $FFMPEG -v error -f lavfi \
        -i "testsrc2=size=${SIZE_W}x${SIZE_H}:rate=25" \
        -frames:v "$FRAMES" -c:v libaom-av1 -cpu-used 8 -lag-in-frames 0 \
        -b:v 2M -g "$GOP" -keyint_min "$GOP" -sc_threshold 0 \
        -pix_fmt yuv420p "$W/full.ivf" -y 2>/dev/null; then
    echo "  跳过：编码失败（没有 libaom-av1？）"
    exit 77
fi
if ! $PY "$DIR/ivf_cut.py" "$W/full.ivf" "$W/plain.ivf" "$CUT" \
        > "$W/plain.info" 2>/dev/null; then
    echo "  跳过：截取失败（ffprobe/python3 不可用？）"
    exit 77
fi
TOTAL=$(sed -n 's/.*TOTAL=\([0-9]*\).*/\1/p' "$W/plain.info")
KEY_AT=$(sed -n 's/.*KEY_AT=\([0-9]*\).*/\1/p' "$W/plain.info")
if [ -z "$TOTAL" ] || [ "$KEY_AT" = "None" ] || [ "$KEY_AT" -lt 1 ] \
    || [ "$KEY_AT" -gt "$((TOTAL - 4))" ]; then
    echo "  跳过：截取后关键帧位置不合适（TOTAL=${TOTAL:-?} KEY_AT=${KEY_AT:-?}）"
    exit 77
fi
$PY "$DIR/ivf_cut.py" --seqhdr "$W/full.ivf" "$W/seq.ivf" "$CUT" \
    > "$W/seq.info" 2>/dev/null
$PY "$DIR/ivf_cut.py" "$W/full.ivf" "$W/key.ivf" "$((CUT + KEY_AT))" \
    > /dev/null 2>&1
# 参考画面：从那个关键帧对齐起解的软解（起解点合法，一帧都不该少）。
$FFMPEG -v error -i "$W/key.ivf" -pix_fmt nv12 -f rawvideo "$W/key.sw.yuv" -y 2>/dev/null
REF=$(md5from "$W/key.sw.yuv" 0)
if [ "$(nframes "$W/key.sw.yuv")" != "$((TOTAL - KEY_AT))" \
    ] || [ "$REF" = d41d8cd98f00b204e9800998ecf8427e ]; then
    echo "  跳过：参考解码异常（$(nframes "$W/key.sw.yuv") 帧，应为 $((TOTAL - KEY_AT))）"
    exit 77
fi

# ---- 控制组：起解点合法时硬件本来就没错，先把它钉住 ----
timeout 300 $FFMPEG -v error -hwaccel vaapi -i "$W/key.ivf" \
    -pix_fmt nv12 -f rawvideo "$W/key.hw.yuv" -y 2>/dev/null
rc=$?
if [ "$rc" = 0 ] && [ "$(md5from "$W/key.hw.yuv" 0)" = "$REF" ]; then
    ok "关键帧对齐起解（控制组）" "与软解逐字节一致 ($(nframes "$W/key.hw.yuv") 帧)"
else
    bad "关键帧对齐起解（控制组）" "rc=$rc 硬解=$(nframes "$W/key.hw.yuv") 帧"
fi

# check_mid <用例名> <输入文件> [all]  —— all 表示"空壳也要交付"，即输出必须等于包数
check_mid() {
    local name=$1 in=$2 want=${3:-}
    timeout 300 $FFMPEG -v error -hwaccel vaapi -i "$in" \
        -pix_fmt nv12 -f rawvideo "$W/$name.hw.yuv" -y 2>/dev/null
    local rc=$? n skip
    n=$(nframes "$W/$name.hw.yuv")
    if [ "$rc" != 0 ]; then
        bad "$name 不卡死不整条放弃" "rc=$rc，输出 $n 帧（同步超时或被整条放弃）"
        return
    fi
    # 两种交付方式都算对：空壳一起交（n=TOTAL），或 ffmpeg 上游就把解不出的
    # 头帧丢了（n=TOTAL-KEY_AT）。但尾段必须完整，且跳过帧数据此确定。
    if [ "$n" = "$TOTAL" ]; then
        skip=$KEY_AT
    elif [ "$n" = "$((TOTAL - KEY_AT))" ]; then
        skip=0
    else
        bad "$name 帧数合理" "输出 $n 帧，应为 $TOTAL（含空壳）或 $((TOTAL - KEY_AT))"
        return
    fi
    if [ "$want" = all ] && [ "$n" != "$TOTAL" ]; then
        bad "$name 空壳按帧交付" "只输出 $n 帧，应为 $TOTAL（头 $KEY_AT 帧丢了就没验到空壳路径）"
        return
    fi
    if [ "$(md5from "$W/$name.hw.yuv" "$skip")" = "$REF" ]; then
        ok "$name 遇关键帧恢复精确画面" \
           "输出 $n 帧（跳过 $skip），关键帧之后 $((TOTAL - KEY_AT)) 帧与软解逐字节一致"
    else
        bad "$name 遇关键帧恢复精确画面" \
            "$(md5from "$W/$name.hw.yuv" "$skip") != $REF"
    fi
    # Chrome 契约：只 EndPicture 不 Sync，像素就得就位。空壳交付这条路尤其
    # 容易只在 ffmpeg 的 map 兜底下看起来没事。
    DMD_NO_MAP_WAIT=1 timeout 300 $FFMPEG -v error -hwaccel vaapi -i "$in" \
        -pix_fmt nv12 -f rawvideo "$W/$name.cr.yuv" -y 2>/dev/null
    local n2
    n2=$(nframes "$W/$name.cr.yuv")
    if [ "$n2" = "$n" ] && [ "$(md5from "$W/$name.cr.yuv" "$skip")" = "$REF" ]; then
        ok "$name 的 Chrome 契约趟" "尾段仍逐字节一致"
    else
        bad "$name 的 Chrome 契约趟" "帧数 $n2（ffmpeg 趟 $n）或尾段不符"
    fi
}

check_mid "纯截取"        "$W/plain.ivf"
check_mid "补序列头截取"  "$W/seq.ivf"  all

echo
printf 'AV1 GOP 中间起解回归: 通过 %d，失败 %d\n' "$pass" "$fail"
[ "$fail" = 0 ]
