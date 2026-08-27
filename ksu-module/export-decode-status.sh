#!/system/bin/sh
# 容器内运行：每秒把解码状态写到共享文件
# 宿主侧 DSH Monitor APK 读这个文件判断容器内是否有硬解
#
# ⚠️ 必须兼容 mksh（Android 默认 sh）：
#    - 不用 bash 特性
#    - [ ] 里变量必须加引号（空值会语法错误）
#    - 不用 [ ... ] && VAR=value（mksh 会把 VAR 当命令执行）
OUT=/data/local/Droidspaces/dmd/run/decode-status
STICKY=0
PREV_RDD=0
PREV_GPU=0
PREV_DD=0

echo "IDLE|init" > "$OUT"

while :; do
    DETECTED=""
    T=0

    # ── Firefox RDD ──
    RDD=$(pgrep -f 'rdd$' 2>/dev/null | head -1)
    if [ -n "$RDD" ]; then
        J=$(awk '{print $14+$15}' /proc/"$RDD"/stat 2>/dev/null)
        J=${J:-0}
        D=$((J - PREV_RDD))
        PREV_RDD=$J
        T=$((T + D))
        if [ "$D" -gt 5 ]; then
            DETECTED="Firefox"
        fi
    else
        PREV_RDD=0
    fi

    # ── Chrome GPU 进程 ──
    GPU=$(pgrep -f 'chrom''e.*type=gpu-process' 2>/dev/null | head -1)
    if [ -n "$GPU" ]; then
        J=$(awk '{print $14+$15}' /proc/"$GPU"/stat 2>/dev/null)
        J=${J:-0}
        D=$((J - PREV_GPU))
        PREV_GPU=$J
        T=$((T + D))
        if [ "$D" -gt 5 ]; then
            DETECTED="Chrome"
        fi
    else
        PREV_GPU=0
    fi

    # ── decode-daemon ──
    DD=$(pidof decode-daemon 2>/dev/null)
    if [ -n "$DD" ]; then
        J=$(awk '{print $14+$15}' /proc/"$DD"/stat 2>/dev/null)
        J=${J:-0}
        D=$((J - PREV_DD))
        PREV_DD=$J
        T=$((T + D))
        if [ "$D" -gt 5 ]; then
            if [ -z "$DETECTED" ]; then
                DETECTED="ffmpeg"
            fi
        fi
    else
        PREV_DD=0
    fi

    # ── 输出 ──
    if [ -n "$DETECTED" ]; then
        STICKY=4
        echo "HW|${DETECTED}|cpu=${T}" > "$OUT"
    elif [ "$STICKY" -gt 0 ]; then
        STICKY=$((STICKY - 1))
        echo "HW|?|cpu=${T}" > "$OUT"
    else
        echo "IDLE|cpu=${T}" > "$OUT"
    fi

    sleep 1
done
