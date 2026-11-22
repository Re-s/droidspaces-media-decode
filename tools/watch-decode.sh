#!/usr/bin/env bash
# watch-decode.sh — Hardly decomposing real-time surveillance panel (in-containment execution)）
#
# Problem solved: A single medical check-up can only answer "No Access at This Time," but what the user sees when the video is played is that
# The CPU curve is volatile, and it is not possible to determine whether hard-collect is continuing. This tool is sampled every second.
# The four-way evidence is shown in parallel into time series, with a clear indication of whether it is persistent hard or intermittent/downswing soft.
#
# Usage:
#   bash tools/watch-decode.sh            # Ongoing surveillance，Ctrl-C Exit
#   bash tools/watch-decode.sh 60         # Just run. 60 sec
#   ADB=1 bash tools/watch-decode.sh      # Show additional host side evidence (required in container) adb）
#
# The interpretation is given in the example at the end.

set -u
DUR=${1:-0}                     # 0 = Unlimited
OMX_PID=${OMX_PID:-}            # Overwrite: host omx Services pid
USE_ADB=${ADB:-0}

command -v adb >/dev/null 2>&1 || USE_ADB=0

# ── Hostside Hardware Decoding Service pid（Enable only adb We’ll do it once.）──────────
# CPU activity in omx@1.0-service is evidence that the VPU is decoding:
# 12 jiffies/5s, 107 jiffies/5s decoded, about 9 times the difference.
if [ "$USE_ADB" = "1" ] && [ -z "$OMX_PID" ]; then
    OMX_PID=$(adb shell "su -c 'ps -A -o PID,ARGS | grep \"[o]mx@1.0-service\" | head -1'" 2>/dev/null \
              | tr -d '\r' | awk '{print $1}')
fi

jf() {  # Cumulative CPU（jiffies）
    [ -r "/proc/$1/stat" ] || { echo 0; return; }
    awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0
}

# Find the current decoding related process
find_rdd()    { pgrep -f 'rdd$' 2>/dev/null | head -1; }
find_chrome() {
    # Confirm that Chrome is running. Other browsers can also have a GPU process.
    pgrep -f 'google-chrome' >/dev/null 2>&1 || return
    for p in $(pgrep -f 'type=gpu-process' 2>/dev/null); do echo "$p"; return; done
}

printf '%-8s │ %-22s │ %-20s │ %s\n' "Time" "Firefox RDD" "Chrome GPU" "Host VPU"
printf '%-8s │ %-22s │ %-20s │ %s\n' "--------" "----------------------" "--------------------" "----------"

declare -A PREV
OMX_PREV=""
I=0
while :; do
    I=$((I+1))
    TS=$(date +%H:%M:%S)

    # ── Firefox RDD ──
    R=$(find_rdd)
    if [ -n "$R" ]; then
        # Avoid `grep -c ... || echo 0`. With no matches, grep prints 0 and returns
        # If you don't quit the code, `I'll add another line to the "Quick", which is later [ ] the comparison is called.
        # "Integer expression required"。Take the values first, then use them. ${x:-0} It’s a little bit of a go-ahead.。
        VA=$(grep -c drv_video "/proc/$R/maps" 2>/dev/null); VA=${VA:-0}
        DRI=$(ls -l "/proc/$R/fd" 2>/dev/null | grep -c renderD); DRI=${DRI:-0}
        C=$(jf "$R"); D=$(( C - ${PREV[rdd]:-$C} )); PREV[rdd]=$C
        if [ "$VA" -gt 0 ] && [ "$DRI" -gt 0 ]; then
            FF=$(printf '✅hardware decode cpu=%-4s' "$D")
        elif [ "$D" -gt 30 ]; then
            FF=$(printf '⚠️ software decode? cpu=%-4s' "$D")
        else
            FF=$(printf '·Free cpu=%-4s' "$D")
        fi
    else
        FF="—— None RDD"
    fi

    # ── Chrome GPU Process ──
    G=$(find_chrome)
    if [ -n "$G" ]; then
        GVA=$(grep -c drv_video "/proc/$G/maps" 2>/dev/null); GVA=${GVA:-0}
        GC=$(jf "$G"); GD=$(( GC - ${PREV[gpu]:-$GC} )); PREV[gpu]=$GC
        if [ "$GVA" -gt 0 ]; then
            CH=$(printf '✅Driver loaded cpu=%-4s' "$GD")
        else
            CH=$(printf '⚠️ Unmounted Driver cpu=%-4s' "$GD")
        fi
    else
        CH="—— None GPU Process"
    fi

    # ── Host VPU（Hardware Decoding Service）──
    if [ -n "$OMX_PID" ]; then
        OC=$(adb shell "su -c 'awk \"{print \\\$14+\\\$15}\" /proc/$OMX_PID/stat'" 2>/dev/null | tr -d '\r')
        if [ -n "$OC" ] && [ -n "$OMX_PREV" ]; then
            OD=$(( OC - OMX_PREV ))
            # The jiffies under the omx window are distraught (decoding is sudden by frame), and the omx is a very powerful tool for the omx.
            # A single judgment is ignored. A single judgement is counted five times in the last five years. The threshold is measured by:
            # Free approximately 12/5s, decode about 107/5s, take 30 for the line to leave the full amount of surplus.
            OMX_WIN="${OMX_WIN:-} $OD"
            set -- $OMX_WIN
            while [ $# -gt 5 ]; do shift; done      # Just the closest. 5 A sample
            OMX_WIN="$*"
            OSUM=0
            for v in $OMX_WIN; do OSUM=$(( OSUM + v )); done
            if [ "$OSUM" -gt 30 ]; then VP="✅Decoding ${OSUM}/5s"; else VP="·Free ${OSUM}/5s"; fi
        else
            VP="(Sampled)"
        fi
        OMX_PREV=${OC:-$OMX_PREV}
    else
        VP="(Not enabled ADB=1)"
    fi

    printf '%-8s │ %-22s │ %-20s │ %s\n' "$TS" "$FF" "$CH" "$VP"

    [ "$DUR" -gt 0 ] && [ "$I" -ge "$DUR" ] && break
    sleep 1
done

cat <<'LEGEND'

──── How? ────
  Firefox RDD
    ✅hardware decode    drv_video Map >0 and renderD Word >0 —— Hard-linked.
    ⚠️ software decode?  It’s burning without loading the drive. CPU —— It’s probably a little softer.
    ·Free. Not decoding. Normal when you stop.，RDD It’ll be released. VA-API Resources
    —— None RDD Firefox No running or never triggering decoding requirements

  Host VPU（Yes ADB=1）
    It’s the hardtest evidence: hardware decoding service. CPU。In-situ free-flowing engagement 12、Decode the contract. 107（Every 5 sec），
    Poor 9 Double. It’s up. VPU It’s a frame. It’s not about how to count the top floor.。

  About"CPU It looks like it’s intermittent."
    Normal. Decoding occurs by frame, and the player has a frame buffer, so... CPU Pulse.。
    It depends on whether the hard spell is sustainable. ✅ The mark is stable, not to see. cpu Whether the value is smooth。
    If hardware decoding stays active, fluctuations in CPU usage do not indicate an interruption.

  ✅ Green but still image of Carton.
    Problem is the display chain, not decoding: inside the container kwin → anland Bridges. SurfaceFlinger
    + HWC，Three times each frame is synthesized (four-person aggregates) 122% A core. The codec is not aware of that.。
LEGEND
