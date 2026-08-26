#!/system/bin/sh
#
# 管理器里点击模块「执行」时运行。
#
# 设计意图：这是用户在图形界面里唯一能主动触发的入口，所以要一屏答完三个
# 问题 —— 现在是否正常、端点在哪（容器侧怎么连）、出问题怎么办。
#
# ⚠️ KSU/APatch 的 action 界面**没有 stdin**：read 会立刻返回空值，
# 所以菜单必须用**音量键**选择（getevent 读按键），不能用 read。
#
DS_DIR=/data/local/Droidspaces          # 注意大写 D，路径大小写敏感
DMD_DIR=${DS_DIR}/dmd
BIN_DIR=${DMD_DIR}/bin
RUN_DIR=${DMD_DIR}/run
LOG_DIR=${DMD_DIR}/logs
# socket 与其余运行时文件一同放在 dmd/run/，保持布局统一。
#
# ⚠️ 平台侧需要把宿主的这个目录挂进容器：
#     /data/local/Droidspaces/dmd/run  →  容器 /run/dmd
# 挂**目录**而不是单个 socket 文件：bind mount 绑的是 inode 而非路径，
# 挂文件的话 daemon 一旦 unlink 重建 socket，挂载点就指向孤立 inode，
# 表现为 connect 得到 ECONNREFUSED，必须重挂；挂目录则能自动跟上。
#
# 另外别想着"socket 建在别处再软链到挂载点" —— 软链是路径引用，
# 容器内不存在宿主路径，链接目标解析不了（实测无效）。
SOCK_DIR=${RUN_DIR}
SOCK=${SOCK_DIR}/decode.sock
DAEMON=${BIN_DIR}/decode-daemon
LOG=${LOG_DIR}/watchdog.log
STATE=${RUN_DIR}/watchdog.state
PIDFILE=${RUN_DIR}/watchdog.pid
MODDIR=${0%/*}

mkdir -p "${BIN_DIR}" "${RUN_DIR}" "${LOG_DIR}" 2>/dev/null

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

# ── 音量键选择 ──────────────────────────────
# KSU 的 action 界面无 stdin，用 getevent 抓音量键。
#   音量上 = 上一项 / 音量下 = 下一项 / 长按音量上 2s = 确认
# 为避免卡死，总超时 20s 后按"仅查看"处理。
key_wait() {
    # 输出: UP / DOWN / TIMEOUT
    _res=$(timeout 20 getevent -lqc 1 2>/dev/null | grep -oE "KEY_VOLUME(UP|DOWN)")
    case "${_res}" in
        *VOLUMEUP)   echo UP ;;
        *VOLUMEDOWN) echo DOWN ;;
        *)           echo TIMEOUT ;;
    esac
}

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
echo "【2】decode-daemon"

if [ -x "${DAEMON}" ]; then
    ok "二进制: ${DAEMON}"
else
    bad "二进制缺失或不可执行: ${DAEMON}"
    warn "若是首次安装，请把 decode-daemon 放进 ${BIN_DIR}/"
fi

DPS=$(ps -A -o PID,ETIME,ARGS 2>/dev/null | grep -v grep | grep 'decode-daemon')
if [ -n "${DPS}" ]; then
    echo "${DPS}" | while read -r line; do ok "${line}"; done
else
    bad "没有 decode-daemon 在运行"
fi

# ─────────────────────────────────────────────
echo
echo "【3】文件布局（全部集中在 dmd/ 下）"
echo "     ${DMD_DIR}/"
echo "       ├─ bin/     可执行文件（decode-daemon）"
echo "       ├─ run/     运行时（socket / pid / lock / state）"
echo "       └─ logs/    日志"
echo "     磁盘占用: $(du -sh "${DMD_DIR}" 2>/dev/null | awk '{print $1}')"

# ─────────────────────────────────────────────
echo
echo "【4】解码端点位置与容器侧接法"

echo "  ▸ 宿主侧 socket"
if [ -S "${SOCK}" ]; then
    ok "${SOCK}"
    echo "       权限 $(stat -c %A "${SOCK}" 2>/dev/null)   inode $(stat -c %i "${SOCK}" 2>/dev/null)"
else
    bad "${SOCK} 不存在（daemon 未以 --sock 模式启动？）"
fi

echo "  ▸ 容器侧路径"
echo "       /run/dmd/decode.sock   ← 驱动默认探测位置，命中即自动使用"
echo "       （宿主侧无法验证容器内是否挂好，请在容器里执行"
echo "         ls -la /run/dmd/decode.sock 确认 inode 与上面一致）"
echo
echo "  ▸ 挂载要求（顺序很关键）"
echo "       1. daemon 先启动、建好 socket 文件"
echo "       2. 平台再把宿主目录挂进容器："
echo "            ${SOCK_DIR}  →  /run/dmd"
echo "       挂目录（本模块用法）：daemon 重建 socket 后容器侧自动跟上，"
echo "       无需重挂 —— 实测 inode 变化后容器侧同步可见且解码立即恢复。"
echo "       挂单个 socket 文件则不行：bind mount 绑 inode 而非路径，"
echo "       daemon 一旦 unlink 重建，挂载点即指向孤立 inode，表现为"
echo "       connect 得到 ECONNREFUSED，必须重挂，重启 daemon 无用。"
echo
echo "  ▸ 容器内使用"
echo "       默认即可（驱动自动探测）"
echo "       显式指定: DMD_ENDPOINT=unix:/run/dmd/decode.sock"
echo "       回落 TCP: DMD_ENDPOINT=tcp:20003（需与宿主共享 netns）"

# ─────────────────────────────────────────────
echo
echo "【5】端点探活"

chmod 755 "${MODDIR}/dmd-probe" 2>/dev/null   # zip 解包丢权限位,先补齐
if [ -x "${MODDIR}/dmd-probe" ]; then
    "${MODDIR}/dmd-probe" "${SOCK}" 3000
    case "$?" in
        0) ok "健康：连得上且正常服务" ;;
        1) bad "daemon 未运行或端点不可连" ;;
        2) bad "连得上但不服务 —— 需重启 daemon" ;;
        7) warn "endpoint inode 不匹配 —— 挂载问题，重启 daemon 无用" ;;
        *) warn "未知返回码" ;;
    esac
    echo
    echo "     ⚠️ 探活只验证「连得上并握手成功」，不验证「能否出帧」。"
    echo "     吞吐类故障（解到一半中断）探活照样报健康，真实能力请在容器内实测："
    echo "       ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi \\"
    echo "              -c:v hevc -i test.mp4 -f null -"
    echo "     满帧且速度 >5x 为正常。必须带 -hwaccel_output_format，"
    echo "     否则拿不到硬解会静默回落软解，什么配置都测得出'正常'。"
else
    bad "dmd-probe 缺失或不可执行"
fi

# ─────────────────────────────────────────────
echo
echo "【6】最近看护日志（15 行）"
tail -15 "${LOG}" 2>/dev/null | sed 's/^/  /' || echo "  （无日志）"

# ─────────────────────────────────────────────
# 音量键菜单
echo
echo "══════════════════════════════════════════"
echo "【操作】用音量键选择"
echo
echo "   音量上(+) ── 重启看护进程"
echo "   音量下(-) ── 下一组操作"
echo "   20 秒无操作 ── 仅查看，退出"
echo
printf "等待按键…"
K=$(key_wait)
echo " [${K}]"

restart_watchdog() {
    echo "  → 停止旧看护…"
    for p in $(ps -A -o PID,ARGS 2>/dev/null | grep 'dmd-watchdog\.sh' | grep -v grep | awk '{print $1}'); do
        kill "$p" 2>/dev/null
    done
    sleep 1
    rm -f "${PIDFILE}"
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
    cd "${DMD_DIR}" 2>/dev/null || return 1
    setsid "${DAEMON}" --sock "${SOCK_DIR}" >>"${LOG_DIR}/decode-daemon.log" 2>&1 &
    sleep 3
    if [ -S "${SOCK}" ]; then
        ok "daemon 已重启，socket: ${SOCK}"
        echo "       inode: $(stat -c %i "${SOCK}" 2>/dev/null)（已重建）"
        echo "       平台挂的是目录，容器侧会自动跟上，无需重挂"
    else
        bad "daemon 重启失败，查看 ${LOG_DIR}/decode-daemon.log"
    fi
}

clean_logs() {
    echo "  → 清理日志…"
    _before=$(du -sk "${LOG_DIR}" 2>/dev/null | awk '{print $1}')
    # 保留最后 200 行，其余截断（直接删会让正在写的 daemon 丢失 fd）
    for f in "${LOG_DIR}"/*.log; do
        [ -f "$f" ] || continue
        _n=$(wc -l < "$f" 2>/dev/null)
        tail -200 "$f" > "$f.keep" 2>/dev/null && cat "$f.keep" > "$f" && rm -f "$f.keep"
        echo "       $(basename "$f"): ${_n} 行 → $(wc -l < "$f" 2>/dev/null) 行"
    done
    # 备份类文件直接删
    rm -f "${LOG_DIR}"/*.log.old "${LOG_DIR}"/*.bak 2>/dev/null
    _after=$(du -sk "${LOG_DIR}" 2>/dev/null | awk '{print $1}')
    ok "日志已清理: ${_before}KB → ${_after}KB"
}

if [ "${K}" = "UP" ]; then
    echo
    restart_watchdog
elif [ "${K}" = "DOWN" ]; then
    echo
    echo "── 第二组操作 ──"
    echo "   音量上(+) ── 重启 decode-daemon（socket 模式）"
    echo "   音量下(-) ── 下一组"
    echo
    printf "等待按键…"
    K2=$(key_wait)
    echo " [${K2}]"
    if [ "${K2}" = "UP" ]; then
        echo
        restart_daemon
    elif [ "${K2}" = "DOWN" ]; then
        echo
        echo "── 第三组操作 ──"
        echo "   音量上(+) ── 清理日志（各文件保留最后 200 行）"
        echo "   音量下(-) ── 全部重启（daemon + 看护）"
        echo
        printf "等待按键…"
        K3=$(key_wait)
        echo " [${K3}]"
        case "${K3}" in
            UP)   echo; clean_logs ;;
            DOWN) echo; restart_daemon; echo; restart_watchdog ;;
            *)    echo "  （超时，未做改动）" ;;
        esac
    else
        echo "  （超时，未做改动）"
    fi
else
    echo "  （超时，仅查看）"
fi

echo
echo "══════════════════════════════════════════"
echo "根目录:     ${DMD_DIR}/"
echo "日志:       ${LOG}"
echo "运行时文件: ${RUN_DIR}/"
