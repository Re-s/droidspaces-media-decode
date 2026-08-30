#!/bin/bash
# 用克隆 profile 做多轮测量，交替 A/B 两个驱动构建以消除系统状态漂移。
#
# 为什么要交替：这台设备 load average 常年 12-13，单个配置连测多轮会被
# 时段性负载污染（实测同配置同素材跑出过 0.84% 与 38.61%）。
# A/B 交替能让两个配置暴露在同一段时间窗口里，差值才有意义。
#
# ⚠️ 绝不使用用户的真实 profile：反复 pkill -9 会损坏它
# （踩过一次，profile 起不来、留下 Telemetry.FailedProfileLocks.txt）。
# 每轮从 /tmp/fxclone 复制一份干净副本。
#
# 用法: ab.sh <轮数> <A驱动目录> <B驱动目录> [素材] [秒数]

set -u
ROUNDS=${1:?轮数}
DRV_A=${2:?A 驱动目录}
DRV_B=${3:?B 驱动目录}
FILE=${4:-h264_long.mp4}
SECS=${5:-20}
PERF=/home/master/MP5WS/research/perf
CLONE=/tmp/fxbase

cd "$PERF" || exit 1
[ -d "$CLONE" ] || { echo "缺少 $CLONE"; exit 1; }

run_one() {   # $1=标签 $2=驱动目录 $3=输出json
    local tag=$1 drv=$2 out=$3
    pkill -9 -x firefox-esr >/dev/null 2>&1
    sleep 2
    # 整目录复制预热过的模板：扩展已装、缓存已建，启动只需几秒。
    # 首次用未预热的模板时 Firefox 要装 14 个扩展，45s 窗口不够，全轮超时。
    rm -rf /tmp/fxrun
    cp -r "$CLONE" /tmp/fxrun
    rm -f /tmp/fxrun/lock /tmp/fxrun/.parentlock
    rm -f "$out"

    python3 sink.py "$out" 8931 >/dev/null 2>&1 &
    local sink=$!
    sleep 1

    WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 \
    MOZ_ENABLE_WAYLAND=1 MOZ_DISABLE_RDD_SANDBOX=1 \
    LIBVA_DRIVERS_PATH="$drv" MESA_LOADER_DRIVER_OVERRIDE=msm \
    DMD_VA_LOG=1 \
    firefox-esr --profile /tmp/fxrun --no-remote --new-instance \
        "file://$PERF/track.html?f=$FILE&secs=$SECS" >/tmp/ab_$tag.log 2>&1 &
    local fx=$!

    local w=0
    while [ $w -lt $((SECS + 45)) ]; do
        [ -s "$out" ] && break
        sleep 1; w=$((w + 1))
    done
    kill -9 $fx $sink >/dev/null 2>&1
    wait $fx $sink 2>/dev/null

    if [ ! -s "$out" ]; then echo "TIMEOUT"; return; fi
    if [ "$(grep -c '\[v4l2\]' /tmp/ab_$tag.log)" -eq 0 ]; then echo "SOFTWARE"; return; fi
    python3 -c "
import json
d = json.load(open('$out'))
tl = d['timeline']
bad = [r['at'] for r in tl if r['dropped'] > 2]
print('%.2f %.1f %s' % (d['drop_pct'], d['avg_fps'],
      ('%g-%g' % (bad[0], bad[-1])) if bad else '-'))
"
}

declare -a AV=() BV=()
for i in $(seq 1 "$ROUNDS"); do
    for side in A B; do
        if [ "$side" = A ]; then drv=$DRV_A; else drv=$DRV_B; fi
        r=$(run_one "$side$i" "$drv" "/tmp/ab_$side$i.json")
        case "$r" in
            TIMEOUT)  echo "  轮$i-$side: 超时" ;;
            SOFTWARE) echo "  轮$i-$side: ⚠️ 走了软解，不计入" ;;
            *)
                set -- $r
                printf "  轮%s-%s: 丢帧 %6s%%  %s fps  丢帧秒 %s\n" "$i" "$side" "$1" "$2" "$3"
                if [ "$side" = A ]; then AV+=("$1"); else BV+=("$1"); fi
                ;;
        esac
    done
done

summarize() {
    local name=$1; shift
    [ $# -eq 0 ] && { echo "$name: 无样本"; return; }
    printf '%s\n' "$@" | sort -n | awk -v n=$# -v nm="$name" '
    {a[NR]=$1; s+=$1}
    END{ mid=(n%2)?a[(n+1)/2]:(a[n/2]+a[n/2+1])/2
         printf "%s: 中位 %.2f%%  均值 %.2f%%  范围 %.2f-%.2f%%  (n=%d)\n",
                nm, mid, s/n, a[1], a[n], n }'
}
echo
summarize "A($DRV_A)" ${AV[@]+"${AV[@]}"}
summarize "B($DRV_B)" ${BV[@]+"${BV[@]}"}
