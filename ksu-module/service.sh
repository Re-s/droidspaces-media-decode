#!/system/bin/sh
#
# KSU/Magisk 模块入口：拉起 dmd-watchdog 看护循环。
#
# 为什么独立成一个模块而不是改 droidspaces 模块：
#   droidspaces 模块由上游（ravindu644）维护，改它会在每次上游更新时被覆盖。
#   本模块只做看护这一件事，与它并存、互不干扰。
#
# 时序：service.sh 在 boot_completed 之前就会被调用，那时容器还没起、
# decode-daemon 也不该由我们抢先拉。所以先等 boot_completed，再多等一会儿
# 让平台完成自己的容器自启与 daemon spawn，之后才进看护循环 ——
# 目的是"平台正常时我完全不出手，只在它失手后补位"。
#
set -u

MODDIR=${0%/*}
DS_DIR=/data/local/Droidspaces
LOG=${DS_DIR}/Logs/dmd-watchdog.log
WD=${MODDIR}/dmd-watchdog.sh
PIDFILE=${DS_DIR}/.dmd-watchdog.pid

# 平台自启完成后的额外让位时间（秒）
GRACE=${DMD_WD_GRACE:-45}

mkdir -p "${DS_DIR}/Logs" 2>/dev/null

log() {
    echo "[$(date '+%m-%d %H:%M:%S' 2>/dev/null || date +%s)] [service] $*" >> "${LOG}"
}

update_prop() {
    # module.prop 的 description 是用户在管理器里唯一能看到的状态窗口。
    #
    # ⚠️ 不能用 sed：状态文本里含 "|"（如 "看护中 (PID 123) | 探活间隔 5s"），
    # 而 sed 的 s 命令分隔符也是 "|"，内容里的竖线会被当成分隔符导致
    #   sed: -e 表达式 #1，字符 58："s"的未知选项
    # 再被 2>/dev/null 吞掉 —— 状态静默不更新，表现为管理器里永远显示
    # 打包时的初始值"尚未启动"，让人误判模块起不来（实测踩过这个坑）。
    #
    # 改用 awk 逐行重写：内容当普通字符串处理，不参与任何模式解析。
    _mp="${MODDIR}/module.prop"
    _msg="$*"
    awk -v m="${_msg}" '
        /^description=/ { print "description=" m; next }
        { print }
    ' "${_mp}" > "${_mp}.tmp" 2>/dev/null && mv "${_mp}.tmp" "${_mp}" 2>/dev/null
}

# 模块被禁用时不做任何事（KSU 会创建 disable 文件）
if [ -f "${MODDIR}/disable" ]; then
    log "模块已禁用，退出"
    exit 0
fi

# 防重复：已有存活的看护就不再起第二个
#
# ⚠️ 这条早退路径必须先回写状态再退出，否则会制造"功能正常但管理器显示
# 尚未启动"的假故障：平台或上一次开机已经拉起过看护，本次 service.sh 走到
# 这里直接 exit 0，module.prop 的 description 就永远停在打包时的初始值
# "尚未启动"，用户据此误判模块刷入后起不来（实测踩过）。
if [ -f "${PIDFILE}" ]; then
    OLD=$(cat "${PIDFILE}" 2>/dev/null)
    if [ -n "${OLD}" ] && kill -0 "${OLD}" 2>/dev/null; then
        log "看护已在运行（PID ${OLD}），本次不重复启动"
        update_prop "🟢 看护中 (PID ${OLD}) | 端点探活间隔 5s | 复用已有实例"
        exit 0
    fi
fi

log "等待 boot_completed"
while [ "$(getprop sys.boot_completed 2>/dev/null)" != "1" ]; do
    sleep 2
done

log "boot_completed，让位 ${GRACE}s 给平台完成容器自启与 daemon spawn"
sleep "${GRACE}"

# zip 归档不保留 Unix 权限位：从 Release 下载解包刷入的模块，脚本必然丢 +x。
# 先无条件补齐再检查，否则下面的 [ -x ] 会永远失败。
chmod 755 "${WD}" 2>/dev/null
[ -f "${MODDIR}/dmd-probe" ] && chmod 755 "${MODDIR}/dmd-probe" 2>/dev/null

if [ ! -x "${WD}" ]; then
    log "错误：${WD} 不存在或不可执行"
    update_prop "🔴 错误: dmd-watchdog.sh 缺失或不可执行"
    exit 1
fi
if [ ! -x "${MODDIR}/dmd-probe" ]; then
    log "错误：${MODDIR}/dmd-probe 不存在或不可执行"
    update_prop "🔴 错误: dmd-probe 缺失或不可执行"
    exit 1
fi

setsid "${WD}" &
WDPID=$!
echo "${WDPID}" > "${PIDFILE}"
log "看护循环已启动（PID ${WDPID}）"
update_prop "🟢 看护中 (PID ${WDPID}) | 端点探活间隔 5s"
exit 0
