#!/system/bin/sh
#
# 管理器里点击模块「执行」时运行：打印当前看护状态与最近日志，便于现场排查。
#
DS_DIR=/data/local/Droidspaces
LOG=${DS_DIR}/Logs/dmd-watchdog.log
STATE=${DS_DIR}/.dmd-watchdog.state
PIDFILE=${DS_DIR}/.dmd-watchdog.pid
MODDIR=${0%/*}
SOCK=${DS_DIR}/Decode/decode.sock

echo "===== dmd-watchdog 状态 ====="

if [ -f "${PIDFILE}" ]; then
    P=$(cat "${PIDFILE}" 2>/dev/null)
    if [ -n "${P}" ] && kill -0 "${P}" 2>/dev/null; then
        echo "看护进程: 运行中 (PID ${P})"
    else
        echo "看护进程: 未运行（PID 文件残留: ${P}）"
    fi
else
    echo "看护进程: 未启动"
fi

echo "状态标记: $(cat "${STATE}" 2>/dev/null || echo '无')"

echo
echo "----- 当前探活结果 -----"
chmod 755 "${MODDIR}/dmd-probe" 2>/dev/null   # zip 解包丢权限位,先补齐
if [ -x "${MODDIR}/dmd-probe" ]; then
    "${MODDIR}/dmd-probe" "${SOCK}" 3000
    rc=$?
    case "${rc}" in
        0) echo "判定: 健康" ;;
        1) echo "判定: daemon 未运行或端点不可连" ;;
        2) echo "判定: 连得上但不服务（需重启）" ;;
        7) echo "判定: endpoint inode 不匹配 —— 挂载问题，看护不插手" ;;
        *) echo "判定: 未知 (rc=${rc})" ;;
    esac
else
    echo "dmd-probe 缺失"
fi

echo
echo "----- decode-daemon 进程 -----"
ps -A -o PID,ETIME,ARGS 2>/dev/null | grep -v grep | grep decode-daemon || echo "（无）"

echo
echo "----- 最近 20 行看护日志 -----"
tail -20 "${LOG}" 2>/dev/null || echo "（无日志）"
