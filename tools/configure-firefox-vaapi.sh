#!/bin/sh
# Configure per-user Firefox VA-API integration for msm_drm.
#
# Usage: configure-firefox-vaapi.sh [--verify|--uninstall]
# Environment: DMD_DRIVER_DIR overrides automatic driver-directory selection.

set -u

BEGIN_MARKER='// BEGIN DMD MSM VA-API MANAGED PREFS'
END_MARKER='// END DMD MSM VA-API MANAGED PREFS'
# Firefox profile 根目录不能写死 —— 不同安装方式放在不同地方，
# 而且同一台机器上可能同时存在多处（本机实测 ~/.config/mozilla/firefox
# 与 ~/.mozilla/firefox 都有真实 profile 和 prefs.js）：
#
#   ~/.mozilla/firefox                              apt / tarball（传统位置）
#   ~/.config/mozilla/firefox                       较新版本遵循 XDG 后的新位置
#   ~/snap/firefox/common/.mozilla/firefox          snap
#   ~/.var/app/org.mozilla.firefox/.mozilla/firefox flatpak
#
# 策略：对每个存在 profiles.ini 的根目录都写一遍。宁可多写也不能漏
# —— 写错位置的后果是"配了但没生效"，而这种失败最难排查（页面能播、
# 统计看着正常，实际静默走软解）。
# FIREFOX_HOME 环境变量可覆盖，只处理指定的那一个。
firefox_homes() {
    if [ -n "${FIREFOX_HOME:-}" ]; then
        [ -f "$FIREFOX_HOME/profiles.ini" ] && printf '%s\n' "$FIREFOX_HOME"
        return 0
    fi
    for h in \
        "$HOME/.mozilla/firefox" \
        "$HOME/.config/mozilla/firefox" \
        "$HOME/snap/firefox/common/.mozilla/firefox" \
        "$HOME/.var/app/org.mozilla.firefox/.mozilla/firefox"
    do
        [ -f "$h/profiles.ini" ] && printf '%s\n' "$h"
    done
}
LOCAL_BIN="$HOME/.local/bin"
WRAPPER="$LOCAL_BIN/firefox-msm-vaapi"
APPLICATIONS="$HOME/.local/share/applications"
DESKTOP="$APPLICATIONS/firefox.desktop"
DESKTOP_BACKUP="$APPLICATIONS/firefox.desktop.dsh-msm-vaapi.backup"

say() { printf '%s\n' "$*"; }
err() { printf '%s\n' "error: $*" >&2; }

usage() {
    cat <<'EOF'
Usage: configure-firefox-vaapi.sh [--verify|--uninstall]

Install is the default. DMD_DRIVER_DIR can select the directory containing
msm_drm_drv_video.so; otherwise the highest-versioned
~/Documents/DSHWK/release-v* directory that contains the driver is used,
falling back to /usr/lib/aarch64-linux-gnu/dri.
EOF
}

firefox_running() {
    command -v pgrep >/dev/null 2>&1 || return 1
    pgrep -x firefox >/dev/null 2>&1 || pgrep -x firefox-esr >/dev/null 2>&1
}

select_driver_dir() {
    if [ -n "${DMD_DRIVER_DIR:-}" ]; then
        DRIVER_DIR=$DMD_DRIVER_DIR
    else
        # 优先系统 dri 目录 —— 那是 `make install` 的正式落点，也是 libva
        # 默认加载的位置。此前反过来先挑 release-v* 目录，结果用户装了
        # 0.4.6 到系统目录，脚本却把 LIBVA_DRIVERS_PATH 指向遗留的
        # release-v0.4.4，配完仍跑旧驱动（实测踩过）。
        DRIVER_DIR=/usr/lib/aarch64-linux-gnu/dri
        if [ ! -f "$DRIVER_DIR/msm_drm_drv_video.so" ]; then
            # 系统目录没装，才退而找版本号最大的 release-v*（sort -V 版本序）。
            for d in $(ls -d "$HOME"/Documents/DSHWK/release-v* 2>/dev/null | sort -V); do
                [ -f "$d/msm_drm_drv_video.so" ] && DRIVER_DIR=$d
            done
        fi
    fi

    if [ ! -f "$DRIVER_DIR/msm_drm_drv_video.so" ]; then
        err "msm_drm_drv_video.so not found in $DRIVER_DIR"
        return 1
    fi
    return 0
}

# Print each profile directory. profiles.ini paths can be absolute or relative
# to ~/.mozilla/firefox and may contain spaces, so do not split the output.
profile_dirs() {
    _home="$1"
    [ -f "$_home/profiles.ini" ] || return 1
    awk -v root="$_home" '
        function emit() {
            if (in_profile && path != "") {
                if (relative == "0" || path ~ /^\//) print path
                else print root "/" path
            }
        }
        /^\[/ { emit(); in_profile = ($0 ~ /^\[Profile[^]]*\]$/); path=""; relative="1"; next }
        in_profile && /^Path=/ { path=substr($0, 6); next }
        in_profile && /^IsRelative=/ { relative=substr($0, 12); next }
        END { emit() }
    ' "$_home/profiles.ini"
}

# 另一套已知的管理块标记（DroidSpaces 集成器写的）。它管的 pref 与本脚本
# 高度重叠，两套并存会让同一个 pref 被重复定义 —— Firefox 取最后一次赋值，
# 行为未必错，但 user.js 变得没法读，改一处不生效很难查。
# 检测到它就跳过该 profile，把选择权留给用户。
FOREIGN_BEGIN='// >>> DroidSpaces Firefox VA-API integration >>>'

rewrite_user_js() {
    userjs=$1

    # 已有另一套管理块 → 跳过，不叠加
    if [ -f "$userjs" ] && grep -qF "$FOREIGN_BEGIN" "$userjs" 2>/dev/null; then
        say "跳过（已由 DroidSpaces 集成器管理）: $userjs"
        say "  两套配置的 pref 重叠，叠加会导致重复定义。"
        say "  要改用本脚本，请先删掉 user.js 里 '>>> DroidSpaces ... >>>' 到"
        say "  '<<< ... <<<' 之间的整段，再重新执行。"
        return 2          # 2 = 有意跳过，区别于 0 成功 / 1 失败
    fi

    tmp="$userjs.dsh-vaapi.$$"
    mkdir -p "$(dirname "$userjs")" || return 1
    [ -f "$userjs" ] || : > "$userjs" || return 1

    # Delete precisely our prior managed range, preserving all user-owned lines.
    awk -v begin="$BEGIN_MARKER" -v end="$END_MARKER" '
        $0 == begin { skipping=1; next }
        skipping && $0 == end { skipping=0; next }
        !skipping { print }
    ' "$userjs" > "$tmp" || { rm -f "$tmp"; return 1; }
    cat >> "$tmp" <<EOF
$BEGIN_MARKER
// Managed by configure-firefox-vaapi.sh. Do not edit inside this block.
user_pref("media.hardware-video-decoding.enabled", true);
// ⚠️ force-enabled 不是"强制开启硬解"那么简单，它同时关掉 Firefox 的
// 硬解性能看门狗。FFmpegVideoDecoder 会统计解码耗时，判定跟不上就走
//   "HW decoding is slow, switching back to SW decode"
// → NS_ERROR_DOM_MEDIA_DECODE_ERR → disable HW acceleration → 换新解码器。
// B 站 1080p 实测触发：不加这项 55 次 NS_ERROR_DOM_MEDIA_FATAL_ERR，
// 解码器被反复销毁重建，页面表现为"播几秒就卡在加载"；
// 加上之后 "HW decoding is slow" 与 "disable HW acceleration" 都是 0 次。
// 本机固件首帧滞后 4 个输入单元，天然容易被这个看门狗误判。
user_pref("media.hardware-video-decoding.force-enabled", true);
user_pref("media.ffmpeg.vaapi.enabled", true);
user_pref("media.hevc.enabled", true);
user_pref("media.ffmpeg.vaapi.force-surface-zero-copy", 2);
// 播放队列深度：不是可选项。Venus 固件的解码流水线滞后 4 个输入单元才吐首帧
// （无 B 帧码流同样如此），Firefox 默认队列太浅，吸收不了这个滞后带来的交付
// 抖动。1080p30 27Mbps 实测：不加丢帧 14.25%、顺序回退 20.75%；加上降到
// 0.89% / 4.04%（软解基线 0.5% / 1.85%，即与软解同量级）。
user_pref("media.video-queue.hw-accel-size", 10);
user_pref("media.video-queue.default-size", 10);
user_pref("media.video-queue.send-to-compositor-size", 6);
$END_MARKER
EOF
    mv "$tmp" "$userjs"
}

remove_user_js_block() {
    userjs=$1
    [ -f "$userjs" ] || return 0
    tmp="$userjs.dsh-vaapi.$$"
    awk -v begin="$BEGIN_MARKER" -v end="$END_MARKER" '
        $0 == begin { skipping=1; next }
        skipping && $0 == end { skipping=0; next }
        !skipping { print }
    ' "$userjs" > "$tmp" || { rm -f "$tmp"; return 1; }
    mv "$tmp" "$userjs"
}

write_wrapper() {
    mkdir -p "$LOCAL_BIN" || return 1
    cat > "$WRAPPER" <<EOF
#!/bin/sh
# Generated by configure-firefox-vaapi.sh.
export MOZ_DISABLE_RDD_SANDBOX=1
export LIBVA_DRIVER_NAME=msm_drm
export LIBVA_DRIVERS_PATH='$DRIVER_DIR'
exec firefox "\$@"
EOF
    chmod 755 "$WRAPPER"
}

write_desktop() {
    mkdir -p "$APPLICATIONS" || return 1
    # 自愈：上一次卸载可能留下指向已删启动器的孤儿 .desktop（那会让图标报
    # "无法找到程序"）。它不算用户数据，别把它备份成"用户的启动器"。
    if [ -f "$DESKTOP" ] && grep -q '^Exec=' "$DESKTOP" 2>/dev/null; then
        _exec=$(sed -n 's/^Exec=//p' "$DESKTOP" | head -1 | awk '{print $1}')
        case "$_exec" in
            "$LOCAL_BIN"/*)
                if [ ! -x "$_exec" ]; then
                    say "清理失效的启动器条目（Exec 不存在）: $DESKTOP"
                    rm -f "$DESKTOP"
                fi
                ;;
        esac
    fi
    # Preserve the user's launcher exactly once. A re-install must not back up
    # our generated launcher as though it were user data.
    if [ -f "$DESKTOP" ] && [ ! -f "$DESKTOP_BACKUP" ]; then
        cp "$DESKTOP" "$DESKTOP_BACKUP" || return 1
    fi
    cat > "$DESKTOP" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Firefox
Comment=Web Browser (msm VA-API)
Exec=$WRAPPER %u
Terminal=false
Icon=firefox
Categories=Network;WebBrowser;
MimeType=text/html;text/xml;application/xhtml+xml;application/xml;application/rss+xml;application/x-xpinstall;application/pdf;x-scheme-handler/http;x-scheme-handler/https;
StartupNotify=true
EOF
}

install() {
    if firefox_running; then
        err "Firefox is running; close Firefox before changing its profiles"
        return 1
    fi
    select_driver_dir || return 1

    homes=$(firefox_homes)
    if [ -z "$homes" ]; then
        err "找不到任何 Firefox profile（已查 ~/.mozilla、~/.config/mozilla、snap、flatpak）"
        err "可用 FIREFOX_HOME 指定包含 profiles.ini 的目录"
        return 1
    fi

    # ⚠️ 不要写成 `... | while ...`：管道让循环跑在子 shell 里，
    # 循环内的 exit/return 只终止子 shell，写失败会被静默吞掉并照常报
    # "installed"（实测过）。改用重定向喂 while，循环就在当前 shell 里。
    failed=0
    any=0
    skipped=0
    while IFS= read -r home; do
        [ -n "$home" ] || continue
        profile_list=$(profile_dirs "$home") || continue
        [ -n "$profile_list" ] || { say "跳过（无 [Profile*] 条目）: $home"; continue; }
        while IFS= read -r profile; do
            [ -n "$profile" ] || continue
            # profiles.ini 可能列出已删除的 profile。目录不存在就跳过，
            # 否则会凭空造出一个只有我们 pref 的目录，Firefox 根本不读。
            if [ ! -d "$profile" ]; then
                say "跳过（目录不存在）: $profile"
                continue
            fi
            rewrite_user_js "$profile/user.js"
            case $? in
                0) any=1; say "configured: $profile/user.js" ;;
                2) skipped=$((skipped + 1)) ;;   # 有意跳过，不算失败也不算成功
                *) err "failed to write $profile/user.js"; failed=1 ;;
            esac
        done <<INNER
$profile_list
INNER
    done <<OUTER
$homes
OUTER
    if [ "$any" -eq 0 ]; then
        if [ "$skipped" -gt 0 ]; then
            err "所有 profile 都已由 DroidSpaces 集成器管理，未做改动"
            err "沿用现有配置即可；要换成本脚本管理，按上面提示先清掉那一段"
        else
            err "没有可配置的 profile"
        fi
        return 1
    fi
    [ "$failed" -eq 0 ] || return 1
    write_wrapper || return 1
    write_desktop || return 1
    say "installed Firefox VA-API integration (driver: $DRIVER_DIR)"
}

uninstall() {
    if firefox_running; then
        err "Firefox is running; close Firefox before changing its profiles"
        return 1
    fi
    failed=0
    # 同 install()：遍历所有 profile 根目录，且不用管道（避免子 shell 吞失败）。
    homes=$(firefox_homes)
    while IFS= read -r home; do
        [ -n "$home" ] || continue
        profile_list=$(profile_dirs "$home") || continue
        while IFS= read -r profile; do
            [ -n "$profile" ] || continue
            if remove_user_js_block "$profile/user.js"; then
                say "removed managed prefs: $profile/user.js"
            else
                err "failed to rewrite $profile/user.js"
                failed=1
            fi
        done <<INNER
$profile_list
INNER
    done <<OUTER
$homes
OUTER
    # ⚠️ 顺序要紧：先处理 .desktop，再删启动器。
    # 反过来的话，一旦 .desktop 这步失败（或被中断），桌面图标就指向一个
    # 已被删除的 Exec，点击报"无法找到程序" —— 实测踩过，比不卸载更糟，
    # 因为用户失去的是能用的 Firefox 图标。
    if [ -f "$DESKTOP_BACKUP" ]; then
        mv "$DESKTOP_BACKUP" "$DESKTOP"
        say "restored: $DESKTOP"
    elif [ -f "$DESKTOP" ]; then
        # 只删我们生成的那个（Exec 指向本脚本的启动器）；用户自己的不动。
        if grep -qF "Exec=$WRAPPER" "$DESKTOP" 2>/dev/null; then
            rm -f "$DESKTOP"
            say "removed: $DESKTOP"
        else
            say "保留（不是本脚本生成的）: $DESKTOP"
        fi
    fi
    rm -f "$WRAPPER"
    [ "$failed" -eq 0 ] || return 1
    say "uninstalled Firefox VA-API integration"
}

report() { printf '%-28s %s\n' "$1" "$2"; }
read_one_line() { [ -r "$1" ] && sed -n '1p' "$1" || printf 'unavailable'; }

verify_devfreq() {
    found=0
    for d in /sys/class/devfreq/*; do
        [ -d "$d" ] || continue
        name=$(read_one_line "$d/name")
        case "$name" in
            *mvs0*|*mvs1*|*mvsc*|*venus*|*Venus*)
                found=1
                gov=$(read_one_line "$d/governor")
                cur=$(read_one_line "$d/cur_freq")
                report "Venus $name" "governor=$gov cur_freq=$cur"
                ;;
            *bus*|*Bus*|*bw*|*BW*)
                found=1
                cur=$(read_one_line "$d/cur_freq")
                report "bus frequency $name" "$cur"
                ;;
        esac
    done
    [ "$found" -eq 1 ] || report "Venus/bus devfreq" "unavailable under /sys/class/devfreq"
}

verify() {
    if select_driver_dir; then
        report "driver" "OK: $DRIVER_DIR/msm_drm_drv_video.so"
    else
        report "driver" "MISSING (set DMD_DRIVER_DIR if needed)"
    fi
    [ -x "$WRAPPER" ] && report "launcher" "OK: $WRAPPER" || report "launcher" "MISSING: $WRAPPER"
    if [ -f "$DESKTOP" ]; then
        _e=$(sed -n 's/^Exec=//p' "$DESKTOP" | head -1 | awk '{print $1}')
        if [ -n "$_e" ] && [ ! -x "$_e" ] && ! command -v "$_e" >/dev/null 2>&1; then
            report "desktop override" "BROKEN: Exec 不存在（$_e）—— 图标会报"无法找到程序"，重新执行安装或手动删除 $DESKTOP"
        else
            report "desktop override" "OK: $DESKTOP"
        fi
    else
        report "desktop override" "MISSING: $DESKTOP"
    fi

    # 找真正在解码的进程。⚠️ 不要用 `pgrep -f 'rdd$'`：RDD 的命令行末尾是
    # -sandboxReporter 之类的参数，不是 "rdd"，实测匹配不到任何进程并误报
    # "not running"。可靠判据是"加载了本驱动"或"持有 /dev/video32"。
    rdds=''
    if command -v pgrep >/dev/null 2>&1; then
        for pid in $(pgrep -f '/firefox' 2>/dev/null || :); do
            [ -r "/proc/$pid/maps" ] || continue
            if grep -q 'msm_drm_drv_video\.so' "/proc/$pid/maps" 2>/dev/null ||
               ls -l "/proc/$pid/fd" 2>/dev/null | grep -q 'video32'; then
                rdds="$rdds $pid"
            fi
        done
    fi
    if [ -n "$rdds" ]; then
        for pid in $rdds; do
            envs=$(tr '\000' '\n' < "/proc/$pid/environ" 2>/dev/null | sed -n '/^MOZ_DISABLE_RDD_SANDBOX=/p;/^LIBVA_DRIVER_NAME=/p;/^LIBVA_DRIVERS_PATH=/p')
            maps=$(grep -c 'msm_drm_drv_video\.so' "/proc/$pid/maps" 2>/dev/null || :)
            fds=$(ls -l "/proc/$pid/fd" 2>/dev/null | grep -c 'video32' || :)
            report "decoder $pid environment" "${envs:-missing}"
            report "decoder $pid driver maps" "$maps"
            report "decoder $pid /dev/video32 fds" "$fds"
        done
    else
        report "decoder process" "none loaded this driver (play a video first)"
    fi

    if [ -e /dev/video32 ]; then
        report "/dev/video32" "present (readable=$([ -r /dev/video32 ] && printf yes || printf no), writable=$([ -w /dev/video32 ] && printf yes || printf no))"
    else
        report "/dev/video32" "missing"
    fi
    verify_devfreq
}

case "${1:-}" in
    '') install ;;
    --verify) verify ;;
    --uninstall) uninstall ;;
    -h|--help) usage ;;
    *) err "unknown option: $1"; usage >&2; exit 2 ;;
esac
