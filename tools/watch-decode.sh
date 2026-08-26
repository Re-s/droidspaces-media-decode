#!/usr/bin/env bash
# watch-decode.sh — 硬解实时监视面板（容器内执行）
#
# 解决的问题：单次体检只能回答"此刻通不通"，但播放视频时用户看到的是
# 起伏不定的 CPU 曲线，无法判断"硬解是否持续在用"。本工具每秒采样，
# 把四路证据并排显示成时间序列，一眼看出是持续硬解、还是断续/回落软解。
#
# 用法:
#   bash tools/watch-decode.sh            # 持续监视，Ctrl-C 退出
#   bash tools/watch-decode.sh 60         # 只跑 60 秒
#   ADB=1 bash tools/watch-decode.sh      # 额外显示宿主侧证据（需容器内有 adb）
#
# 判读方式见末尾图例。

set -u
DUR=${1:-0}                     # 0 = 无限
OMX_PID=${OMX_PID:-}            # 可覆盖：宿主 omx 服务 pid
USE_ADB=${ADB:-0}

command -v adb >/dev/null 2>&1 || USE_ADB=0

# ── 宿主侧硬件解码服务 pid（只在启用 adb 时探测一次）──────────
# omx@1.0-service 的 CPU 是"VPU 真在解码"的最直接证据：
# 实测空闲 12 jiffies/5s，解码时 107 jiffies/5s，差约 9 倍。
if [ "$USE_ADB" = "1" ] && [ -z "$OMX_PID" ]; then
    OMX_PID=$(adb shell "su -c 'ps -A -o PID,ARGS | grep \"[o]mx@1.0-service\" | head -1'" 2>/dev/null \
              | tr -d '\r' | awk '{print $1}')
fi

jf() {  # 取进程累计 CPU（jiffies）
    [ -r "/proc/$1/stat" ] || { echo 0; return; }
    awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0
}

# 找当前的解码相关进程
find_rdd()    { pgrep -f 'rdd$' 2>/dev/null | head -1; }
find_chrome() {
    # 先确认 Chrome 在运行，否则 gpu-process 的匹配可能落到别的浏览器上
    pgrep -f 'google-chrome' >/dev/null 2>&1 || return
    for p in $(pgrep -f 'type=gpu-process' 2>/dev/null); do echo "$p"; return; done
}

printf '%-8s │ %-22s │ %-20s │ %s\n' "时刻" "Firefox RDD" "Chrome GPU" "宿主 VPU"
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
        # ⚠️ 不能写 `grep -c ... || echo 0`：无匹配时 grep 既输出 0 又返回
        # 非零退出码，`||` 会再追加一行，变成 "0\n0"，后面的 [ ] 比较即报
        # "需要整数表达式"。先取值、再用 ${x:-0} 兜底才对。
        VA=$(grep -c drv_video "/proc/$R/maps" 2>/dev/null); VA=${VA:-0}
        DRI=$(ls -l "/proc/$R/fd" 2>/dev/null | grep -c renderD); DRI=${DRI:-0}
        C=$(jf "$R"); D=$(( C - ${PREV[rdd]:-$C} )); PREV[rdd]=$C
        if [ "$VA" -gt 0 ] && [ "$DRI" -gt 0 ]; then
            FF=$(printf '✅硬解 cpu=%-4s' "$D")
        elif [ "$D" -gt 30 ]; then
            FF=$(printf '⚠️ 软解? cpu=%-4s' "$D")
        else
            FF=$(printf '·空闲 cpu=%-4s' "$D")
        fi
    else
        FF="—— 无 RDD"
    fi

    # ── Chrome GPU 进程 ──
    G=$(find_chrome)
    if [ -n "$G" ]; then
        GVA=$(grep -c drv_video "/proc/$G/maps" 2>/dev/null); GVA=${GVA:-0}
        GC=$(jf "$G"); GD=$(( GC - ${PREV[gpu]:-$GC} )); PREV[gpu]=$GC
        if [ "$GVA" -gt 0 ]; then
            CH=$(printf '✅驱动已载 cpu=%-4s' "$GD")
        else
            CH=$(printf '⚠️ 未载驱动 cpu=%-4s' "$GD")
        fi
    else
        CH="—— 无 GPU 进程"
    fi

    # ── 宿主 VPU（硬件解码服务）──
    if [ -n "$OMX_PID" ]; then
        OC=$(adb shell "su -c 'awk \"{print \\\$14+\\\$15}\" /proc/$OMX_PID/stat'" 2>/dev/null | tr -d '\r')
        if [ -n "$OC" ] && [ -n "$OMX_PREV" ]; then
            OD=$(( OC - OMX_PREV ))
            # ⚠️ 1 秒窗口下 omx 的 jiffies 抖动很大（解码是按帧突发的），
            # 单点判断会忽 ✅ 忽 ·。用最近 5 次累计判断，阈值按实测定：
            # 空闲约 12/5s、解码约 107/5s，取 30 作分界留足余量。
            OMX_WIN="${OMX_WIN:-} $OD"
            set -- $OMX_WIN
            while [ $# -gt 5 ]; do shift; done      # 只留最近 5 个采样
            OMX_WIN="$*"
            OSUM=0
            for v in $OMX_WIN; do OSUM=$(( OSUM + v )); done
            if [ "$OSUM" -gt 30 ]; then VP="✅解码中 ${OSUM}/5s"; else VP="·空闲 ${OSUM}/5s"; fi
        else
            VP="(采样中)"
        fi
        OMX_PREV=${OC:-$OMX_PREV}
    else
        VP="(未启用 ADB=1)"
    fi

    printf '%-8s │ %-22s │ %-20s │ %s\n' "$TS" "$FF" "$CH" "$VP"

    [ "$DUR" -gt 0 ] && [ "$I" -ge "$DUR" ] && break
    sleep 1
done

cat <<'LEGEND'

──── 怎么判读 ────
  Firefox RDD
    ✅硬解    drv_video 映射 >0 且 renderD 句柄 >0 —— 硬解链路挂着
    ⚠️ 软解?  没加载驱动却在烧 CPU —— 很可能回落软解了
    ·空闲     没在解码。停播时属正常，RDD 会释放 VA-API 资源
    —— 无 RDD Firefox 未运行，或从未触发解码需求

  宿主 VPU（需 ADB=1）
    这是最硬的证据：硬件解码服务的 CPU。实测空闲约 12、解码约 107（每5秒），
    差 9 倍。它涨说明 VPU 真在出帧，与上层怎么统计无关。

  关于"CPU 看起来断断续续"
    正常现象。解码是按帧突发的，且播放器有帧缓冲，所以 CPU 呈脉冲状。
    判断硬解是否持续，要看 ✅ 标记是否稳定，而不是看 cpu 数值是否平滑。
    只要 ✅硬解 一直在，即使 cpu 数值起伏，硬解也没有中断。

  ✅ 全绿但画面仍卡顿
    问题在呈现链，不在解码：容器内 kwin → anland 桥 → 宿主 SurfaceFlinger
    + HWC，每帧被合成三次（实测四者合计约 122% 一个核）。解码器对此无感。
LEGEND
