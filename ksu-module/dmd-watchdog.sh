#!/system/bin/sh
#
# dmd-watchdog —— decode-daemon 看护循环
#
# 背景：Droidspaces 平台只在容器启动与 monitor 的 reboot_cycle 里 spawn
# decode-daemon，daemon 崩溃后没有任何东西会重新拉起它（真机实测：kill -9
# 之后容器硬解一直坏到手工干预）。本脚本补上这一层。
#
# 设计要点（每条都对应实测约束，改动前请先读完）：
#
# 1) 存活判据用端点探活，不用 kill(pid,0)。
#    僵尸进程 kill(pid,0) 返回 0；daemon 还有会话级失效模式（进程活着、
#    持着 flock、却不再服务）。dmd-probe 做真实 connect+握手，最贴近
#    "容器现在能不能硬解"这个真正关心的问题。
#
# 2) 探到 inode 不匹配（probe rc=7）不重启 daemon。
#    那是挂载配置问题（平台把 socket 文件而非目录 bind mount 进容器），
#    重启 daemon 只会换个新 inode，救不了容器侧的死引用，只会白折腾。
#
# 3) 不与平台抢。平台自己也会 spawn daemon；两边都拉会造成双实例互相
#    unlink socket（真机见过：/proc/net/unix 同路径两条监听记录，先起的
#    退化成无名孤儿）。所以：先探活，健康就什么都不做；要拉起时先用
#    flock 取独占，避免与另一份自己或平台并发。
#
# 4) 重启有退避与上限，避免 daemon 因环境问题必崩时无限刷进程。
#    连续失败达上限后进入长睡眠观察，而不是彻底放弃——容器可能稍后才启动。
#
# 5) 拉起后必须复验。启动成功 ≠ 能服务；再探一次确认握手通得过才计成功。
#
set -u

MODDIR=${0%/*}
DS_DIR=/data/local/Droidspaces
SOCK_DIR=${DS_DIR}/Decode
SOCK=${SOCK_DIR}/decode.sock
DAEMON=${DS_DIR}/bin/decode-daemon
PROBE=${MODDIR}/dmd-probe
RUN_DIR=${DS_DIR}/Decode/watchdog
LOG=${DS_DIR}/Logs/dmd-watchdog.log
LOCK=${RUN_DIR}/watchdog.lock
STATE=${RUN_DIR}/watchdog.state

# 探活间隔（秒）。5s 足够及时，又不会让探活本身成为负担：
# 每次探活是一次 connect+握手，daemon 侧会记一条会话日志。
INTERVAL=${DMD_WD_INTERVAL:-5}
# 连续重启失败上限，达到后进入 COOLDOWN 长睡眠
MAX_FAILS=${DMD_WD_MAX_FAILS:-5}
COOLDOWN=${DMD_WD_COOLDOWN:-300}
# 单条日志文件上限，超过就轮转一次（守护是长命进程，不能让日志无限涨）
LOG_MAX=${DMD_WD_LOG_MAX:-262144}

# 配置文件覆盖：开机流程里设不了环境变量，改这里才是正道。
# 文件内容就是普通 shell 赋值，例如：
#   INTERVAL=10
#   MAX_FAILS=3
CONF=${DS_DIR}/.dmd-watchdog.conf
[ -f "${CONF}" ] && . "${CONF}"

mkdir -p "${DS_DIR}/Logs" "${RUN_DIR}" 2>/dev/null

log() {
    echo "[$(date '+%m-%d %H:%M:%S' 2>/dev/null || date +%s)] $*" >> "${LOG}"
}

rotate_log() {
    if [ -f "${LOG}" ]; then
        sz=$(stat -c %s "${LOG}" 2>/dev/null || echo 0)
        if [ "${sz}" -gt "${LOG_MAX}" ] 2>/dev/null; then
            mv -f "${LOG}" "${LOG}.1" 2>/dev/null
            log "日志轮转（上一份存为 $(basename "${LOG}").1）"
        fi
    fi
}

# 记录状态供外部查看（module.prop 描述、人工排查都用得上）
set_state() {
    echo "$*" > "${STATE}" 2>/dev/null
}

start_daemon() {
    # 独占：宁可这一轮不拉，也不要造出第二个实例
    # -n 拿不到锁立刻返回，说明有别人正在拉起
    (
        flock -n 9 || {
            log "拉起跳过：另一方持有启动锁（平台或另一份看护正在处理）"
            exit 10
        }
        # 拿到锁后再探一次：可能就在等锁的这一瞬间对方已经拉起来了
        if "${PROBE}" "${SOCK}" 2000 >/dev/null 2>&1; then
            log "拉起跳过：拿到锁后复探发现已健康"
            exit 11
        fi

        [ -x "${DAEMON}" ] || {
            log "错误：${DAEMON} 不存在或不可执行"
            exit 12
        }
        mkdir -p "${SOCK_DIR}" 2>/dev/null

        # 目录模式：把目录传给 --sock，daemon 自己在里面建 decode.sock。
        # 目录 inode 稳定，daemon 重启不会让容器侧的 bind mount 失效
        # ——这正是容器能扛住 daemon 换代的原因，别改成传文件路径。
        cd "${DS_DIR}" || exit 13
        setsid "${DAEMON}" --sock "${SOCK_DIR}" \
            >> "${DS_DIR}/Logs/decode-daemon.log" 2>&1 &
        log "已拉起 decode-daemon（PID $!）"
        exit 0
    ) 9>"${LOCK}"
    return $?
}

rotate_log
log "===== dmd-watchdog 启动（间隔 ${INTERVAL}s，失败上限 ${MAX_FAILS}）"
set_state "starting"

fails=0
last_state=""

while : ; do
    "${PROBE}" "${SOCK}" 3000 >/dev/null 2>&1
    rc=$?

    case "${rc}" in
    0)
        # 健康。仅在状态发生变化时记日志，避免每 5 秒刷一行
        if [ "${last_state}" != "healthy" ]; then
            log "daemon 健康（探活通过）"
            set_state "healthy"
            last_state="healthy"
        fi
        fails=0
        ;;
    7)
        # 挂载配置问题：重启 daemon 只会换个新 inode，救不了容器侧的死引用。
        # 明确不插手，把可行动的结论写进日志。
        if [ "${last_state}" != "mismatch" ]; then
            log "警告：endpoint inode 不匹配 —— 探活连到的不是本机 daemon 的监听端点。"
            log "      成因：平台把单个 socket 文件（而非目录）bind mount 进容器，"
            log "      daemon 换代后容器侧钉着已死的 inode。"
            log "      本看护不插手：重启 daemon 只会再换一个 inode，容器侧依旧连不上。"
            log "      修法：用带目录级挂载的平台版本（容器内应为 /run/dmd 目录挂载），"
            log "      或重建容器让挂载重新绑定当前 socket。"
            set_state "endpoint-mismatch (需修挂载，看护不插手)"
            last_state="mismatch"
        fi
        fails=0
        ;;
    1|2)
        # 1 = 连不上（没在跑）；2 = 连上了但不服务（僵死/半死）
        if [ "${rc}" = "2" ]; then
            log "daemon 连得上但握手失败 —— 进程活着却不服务，需要重启"
            # 半死实例持着 flock 与 socket，必须先清掉再拉
            pkill -f "decode-daemon --sock" 2>/dev/null
            sleep 1
        else
            log "daemon 探活失败（连不上），准备拉起"
        fi
        set_state "restarting (fails=${fails})"
        last_state="down"

        start_daemon
        src=$?
        if [ "${src}" = "0" ]; then
            sleep 2
            if "${PROBE}" "${SOCK}" 3000 >/dev/null 2>&1; then
                log "重启成功并复验通过"
                set_state "healthy (restarted)"
                last_state="healthy"
                fails=0
            else
                fails=$((fails + 1))
                log "重启后复验未通过（连续失败 ${fails}/${MAX_FAILS}）"
            fi
        elif [ "${src}" = "10" ] || [ "${src}" = "11" ]; then
            : # 让给别人处理，不计失败
        else
            fails=$((fails + 1))
            log "拉起失败 src=${src}（连续失败 ${fails}/${MAX_FAILS}）"
        fi

        if [ "${fails}" -ge "${MAX_FAILS}" ]; then
            log "连续 ${fails} 次未能恢复，进入 ${COOLDOWN}s 冷却观察"
            log "（不彻底放弃：容器可能稍后才启动，或环境问题会被人工修复）"
            set_state "cooldown after ${fails} fails"
            last_state="cooldown"
            sleep "${COOLDOWN}"
            fails=0
        fi
        ;;
    *)
        log "探活返回未知码 ${rc}，按失败处理"
        fails=$((fails + 1))
        ;;
    esac

    rotate_log
    sleep "${INTERVAL}"
done
