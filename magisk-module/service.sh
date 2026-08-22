#!/system/bin/sh
# service.sh — 开机启动 decode-daemon（TCP 模式）
#
# 安全设计要点（这些不是风格偏好，是防 bootloop 的硬要求）：
#
#  1. **所有等待必须有上界**。KernelSU / Magisk 对模块脚本没有超时机制，
#     `while [ "$(getprop sys.boot_completed)" != "1" ]; do sleep 1; done`
#     这类无界循环一旦条件永不满足，脚本就永久阻塞，可能拖死开机流程。
#     下面的等待带最大重试次数，超时也照常继续。
#
#  2. **不使用 killall / pkill 按名字杀进程**。名字匹配可能命中系统进程。
#     只杀我们自己记录在 pidfile 里、且确认可执行文件路径一致的进程。
#
#  3. **任何情况下都以 exit 0 结束**，绝不因自身失败影响系统启动。

MODDIR="${0%/*}"
LOG=/data/local/tmp/decode-daemon.log
PIDFILE=/data/local/tmp/decode-daemon.pid
PORT=20003                     # 与 src/decode-daemon.c 的默认端口保持一致
BOOT_WAIT_MAX=120              # 最多等待开机完成 120 次（约 120 秒）

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"; }

# --- 1. 有上界地等待开机完成 ------------------------------------------------
i=0
while [ "$(getprop sys.boot_completed)" != "1" ]; do
    i=$((i + 1))
    if [ "$i" -ge "$BOOT_WAIT_MAX" ]; then
        log "warn: 等待 sys.boot_completed 超时（${BOOT_WAIT_MAX}s），继续启动"
        break
    fi
    sleep 1
done

sleep 5    # 让 mediaserver / codec HAL 就绪

# --- 2. 定位二进制 ----------------------------------------------------------
BIN="$MODDIR/decode-daemon"
[ -x "$BIN" ] || BIN=/data/local/tmp/decode-daemon
if [ ! -x "$BIN" ]; then
    log "error: 找不到可执行的 decode-daemon，放弃启动"
    exit 0
fi

# --- 3. 只精确停掉此前由本脚本启动的实例 ------------------------------------
# 不用 killall/pkill：按名字匹配有误杀系统进程的风险。
if [ -f "$PIDFILE" ]; then
    OLDPID="$(cat "$PIDFILE" 2>/dev/null)"
    case "$OLDPID" in
        ''|*[!0-9]*) : ;;                      # 内容不是纯数字，忽略
        *)
            # 确认该 pid 当前确实是 decode-daemon，避免 pid 复用误杀
            if [ -d "/proc/$OLDPID" ] && \
               readlink "/proc/$OLDPID/exe" 2>/dev/null | grep -q 'decode-daemon'; then
                kill "$OLDPID" 2>/dev/null
                # 必须确认真的退出：daemon 阻塞在 accept() 时，SIGTERM 只设置
                # 标志位而不会立刻生效。等待有上界，超时后升级为 SIGKILL，
                # 否则会出现两个实例同时监听同一端口（SO_REUSEADDR 允许绑定成功）。
                k=0
                while [ -d "/proc/$OLDPID" ] && [ "$k" -lt 10 ]; do
                    k=$((k + 1)); sleep 1
                done
                if [ -d "/proc/$OLDPID" ]; then
                    kill -9 "$OLDPID" 2>/dev/null
                    sleep 1
                    log "旧实例 pid=$OLDPID 未响应 SIGTERM，已强制终止"
                else
                    log "已停止旧实例 pid=$OLDPID"
                fi
            fi
            ;;
    esac
    rm -f "$PIDFILE"
fi

# --- 4. 启动 ----------------------------------------------------------------
chmod 755 "$BIN" 2>/dev/null
log "启动 $BIN port=$PORT"
nohup "$BIN" "$PORT" >> "$LOG" 2>&1 &
NEWPID=$!
echo "$NEWPID" > "$PIDFILE"

sleep 3
if [ -d "/proc/$NEWPID" ]; then
    log "ok: 运行中 pid=$NEWPID port=$PORT"
else
    log "error: 进程已退出，未能绑定 port=$PORT（详见上方日志）"
fi

# 无论结果如何都正常退出，不阻塞开机
exit 0
