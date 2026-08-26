#!/system/bin/sh
#
# 管理器里点击模块「执行」时运行。
#
# 设计意图：这是用户在图形界面里唯一能主动触发的入口，所以要一屏答完三个
# 问题 —— 现在是否正常、端点在哪（容器侧怎么连）、出问题怎么办。
# 因此除了状态展示，还带交互式的重启动作。
#
DS_DIR=/data/local/Droidspaces
RUN_DIR=${DS_DIR}/Decode/watchdog
LOG=${DS_DIR}/Logs/dmd-watchdog.log
STATE=${RUN_DIR}/watchdog.state
PIDFILE=${RUN_DIR}/watchdog.pid
MODDIR=${0%/*}
SOCK_DIR=${DS_DIR}/Decode
SOCK=${SOCK_DIR}/decode.sock
DAEMON=${DS_DIR}/bin/decode-daemon

# 与 service.sh 同款：用 awk 而非 sed，因为状态文本含 "|"（sed 的分隔符）
update_prop() {
    _mp="${MODDIR}/module.prop"
    _msg="$*"
    awk -v m="${_msg}" '
        /^description=/ { print "description=" m; next }
        { print }
    ' "${_mp}" > "${_mp}.tmp" 2>/dev/null && mv "${_mp}.tmp" "${_mp}" 2>/dev/null
}

ok()   { echo "  ✅ $*"; }
bad()  { echo "  ❌ $*"; }
warn() { echo "  ⚠️  $*"; }

echo "╔══════════════════════════════════════════╗"
echo "║   DMD Watchdog — decode-daemon 看护      ║"
echo "╚══════════════════════════════════════════╝"

# ─────────────────────────────────────────────
echo
echo "【1】看护运行状态"

if [ -f "${PIDFILE}" ]; then
    P=$(cat "${PIDFILE}" 2>/dev/null)
    if [ -n "${P}" ] && kill -0 "${P}" 2>/dev/null; then
        UP=$(ps -A -o PID,ETIME 2>/dev/null | awk -v p="${P}" '$1==p {print $2}')
        ok "看护运行中 (PID ${P}${UP:+, 已运行 ${UP}})"
    else
        bad "看护未运行（PID 文件残留: ${P}）"
    fi
else
    bad "看护未启动（无 PID 文件）"
fi
echo "     上次判定: $(cat "${STATE}" 2>/dev/null || echo '无记录')"

# ─────────────────────────────────────────────
echo
echo "【2】decode-daemon 进程"

DPS=$(ps -A -o PID,ETIME,ARGS 2>/dev/null | grep -v grep | grep 'decode-daemon')
if [ -n "${DPS}" ]; then
    echo "${DPS}" | while read -r line; do ok "${line}"; done
else
    bad "没有 decode-daemon 在运行"
fi

# ─────────────────────────────────────────────
echo
echo "【3】解码端点位置与容器侧接法"

echo "  ▸ 宿主侧 socket"
if [ -S "${SOCK}" ]; then
    ok "${SOCK}"
    echo "       权限 $(stat -c %A "${SOCK}" 2>/dev/null)   inode $(stat -c %i "${SOCK}" 2>/dev/null)"
else
    bad "${SOCK} 不存在（daemon 未以 --sock 模式启动？）"
fi

echo "  ▸ 容器侧路径"
echo "       /run/dmd/decode.sock   ← 驱动默认探测位置，命中即自动使用"
echo
echo "  ▸ 挂载要求（顺序很关键）"
echo "       1. daemon 先启动、建好 socket 文件"
echo "       2. 平台再把宿主 ${SOCK_DIR} 挂进容器 /run/dmd"
echo "       挂目录（本模块用法）：daemon 重建 socket 后容器侧自动跟上，"
echo "       无需重挂 —— 实测 daemon 重启使 inode 变化后，容器侧同步可见"
echo "       且解码立即恢复正常。这是推荐做法。"
echo "       挂单个 socket 文件则不行：bind mount 绑的是 inode 而非路径，"
echo "       daemon 一旦 unlink 重建，挂载点就指向孤立 inode，表现为"
echo "       connect 得到 ECONNREFUSED，此时必须重挂，重启 daemon 无用。"
echo
echo "  ▸ 容器内使用"
echo "       默认即可（驱动自动探测）"
echo "       显式指定: DMD_ENDPOINT=unix:/run/dmd/decode.sock"
echo "       回落 TCP: DMD_ENDPOINT=tcp:20003（需与宿主共享 netns）"

# ─────────────────────────────────────────────
echo
echo "【4】端点探活"

chmod 755 "${MODDIR}/dmd-probe" 2>/dev/null   # zip 解包丢权限位,先补齐
if [ -x "${MODDIR}/dmd-probe" ]; then
    "${MODDIR}/dmd-probe" "${SOCK}" 3000
    case "$?" in
        0) ok "健康：连得上且正常服务" ;;
        1) bad "daemon 未运行或端点不可连" ;;
        2) bad "连得上但不服务 —— 需重启 daemon（见下方操作 2）" ;;
        7) warn "endpoint inode 不匹配 —— 挂载问题，重启 daemon 无用，需重挂" ;;
        *) warn "未知返回码" ;;
    esac
    echo
    echo "     ⚠️ 探活只验证「连得上并握手成功」，不验证「能否出帧」。"
    echo "     吞吐类故障（解到一半中断）探活照样报健康，真实能力请在容器内实测："
    echo "       ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi \\"
    echo "              -c:v hevc -i test.mp4 -f null -"
    echo "     满帧且速度 >5x 为正常。必须带 -hwaccel_output_format，"
    echo "     否则拿不到硬解时会静默回落软解，什么配置都测得出\"正常\"。"
else
    bad "dmd-probe 缺失或不可执行"
fi

# ─────────────────────────────────────────────
echo
echo "【5】最近看护日志（20 行）"
tail -20 "${LOG}" 2>/dev/null | sed 's/^/  /' || echo "  （无日志）"

# ─────────────────────────────────────────────
echo
echo "══════════════════════════════════════════"
echo "【操作】"
echo "  1) 重启看护进程"
echo "  2) 重启 decode-daemon（socket 模式）"
echo "  3) 两者都重启"
echo "  回车) 仅查看，不做改动"
printf "请选择: "
read -r CHOICE 2>/dev/null || CHOICE=""

restart_watchdog() {
    echo "  → 停止旧看护…"
    for p in $(ps -A -o PID,ARGS 2>/dev/null | grep 'dmd-watchdog\.sh' | grep -v grep | awk '{print $1}'); do
        kill "$p" 2>/dev/null
    done
    sleep 1
    rm -f "${PIDFILE}"
    mkdir -p "${RUN_DIR}" 2>/dev/null
    echo "  → 拉起新看护…"
    setsid "${MODDIR}/dmd-watchdog.sh" >/dev/null 2>&1 &
    NEW=$!
    echo "${NEW}" > "${PIDFILE}"
    sleep 2
    if kill -0 "${NEW}" 2>/dev/null; then
        ok "看护已重启 (PID ${NEW})"
        update_prop "🟢 看护中 (PID ${NEW}) | 端点探活间隔 5s | 手动重启"
    else
        bad "看护重启失败，查看日志: ${LOG}"
        update_prop "🔴 看护重启失败，请查看日志"
    fi
}

restart_daemon() {
    if [ ! -x "${DAEMON}" ]; then
        bad "找不到可执行的 daemon: ${DAEMON}"
        return 1
    fi
    echo "  → 停止 --sock 模式的旧实例（不动他人的 TCP 实例）…"
    for p in $(ps -A -o PID,ARGS 2>/dev/null | grep 'decode-daemon' | grep -- '--sock' | grep -v grep | awk '{print $1}'); do
        kill "$p" 2>/dev/null
    done
    sleep 2
    echo "  → 以 socket 模式拉起…"
    cd "${DS_DIR}" 2>/dev/null || return 1
    setsid "${DAEMON}" --sock "${SOCK_DIR}" >>"${DS_DIR}/Logs/decode-daemon.log" 2>&1 &
    sleep 3
    if [ -S "${SOCK}" ]; then
        ok "daemon 已重启，socket: ${SOCK}"
        echo "       socket inode: $(stat -c %i "${SOCK}" 2>/dev/null)（已重建）"
        echo "       平台挂的是目录，容器侧会自动跟上，无需重挂"
    else
        bad "daemon 重启失败，查看 ${DS_DIR}/Logs/decode-daemon.log"
    fi
}

case "${CHOICE}" in
    1) echo; restart_watchdog ;;
    2) echo; restart_daemon ;;
    3) echo; restart_daemon; echo; restart_watchdog ;;
    *) echo "  （未做改动）" ;;
esac

echo
echo "══════════════════════════════════════════"
echo "日志:       ${LOG}"
echo "运行时文件: ${RUN_DIR}/"
