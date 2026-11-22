#!/bin/bash
# Alternate between two driver builds using a cloned Firefox profile to limit system-load drift.
#
# The test device typically has a load average around 12–13. The same build
# and clip dropped 0.84% and 38.61% in separate runs. Alternating keeps
# measurements for the two builds close in time.
#
# Use a clean template copy for each round. Repeated forced termination can
# damage the user's real profile and leave Telemetry.FailedProfileLocks.txt.
#
# Usage: ab.sh <rounds> <driver A directory> <driver B directory> [clip] [seconds]

set -u
ROUNDS=${1:?Number of rounds required}
DRV_A=${2:?A Driver Directory}
DRV_B=${3:?B Driver Directory}
FILE=${4:-h264_long.mp4}
SECS=${5:-20}
PERF=/home/master/MP5WS/research/perf
CLONE=/tmp/fxbase

cd "$PERF" || exit 1
[ -d "$CLONE" ] || { echo "Missing $CLONE"; exit 1; }

run_one() {   # $1=Label $2=Driver Directory $3=Output json
    local tag=$1 drv=$2 out=$3
    pkill -9 -x firefox-esr >/dev/null 2>&1
    sleep 2
    # The entire directory reproduces preheated templates: extensions are loaded, caches are built, start is only in seconds.
    # The first time you use an unheated template, Firefox will have 14 extensions, 45s windows will not be enough, and the full time is over.
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
            TIMEOUT)  echo "  Round $i-$side: timed out" ;;
            SOFTWARE) echo "  Round $i-$side: Skipped software decode" ;;
            *)
                set -- $r
                printf "  Round %s-%s: dropped frames %6s%%  %s fps  seconds with drops %s\n" "$i" "$side" "$1" "$2" "$3"
                if [ "$side" = A ]; then AV+=("$1"); else BV+=("$1"); fi
                ;;
        esac
    done
done

summarize() {
    local name=$1; shift
    [ $# -eq 0 ] && { echo "$name: No sample"; return; }
    printf '%s\n' "$@" | sort -n | awk -v n=$# -v nm="$name" '
    {a[NR]=$1; s+=$1}
    END{ mid=(n%2)?a[(n+1)/2]:(a[n/2]+a[n/2+1])/2
         printf "%s: median %.2f%%  mean %.2f%%  range %.2f-%.2f%%  (n=%d)\n",
                nm, mid, s/n, a[1], a[n], n }'
}
echo
summarize "A($DRV_A)" ${AV[@]+"${AV[@]}"}
summarize "B($DRV_B)" ${BV[@]+"${BV[@]}"}
