#!/bin/sh
# 给 Chrome / Chromium 配好 msm_drm VA-API 硬解（幂等，可重复执行）。
#
# 用法: configure-chrome-vaapi.sh [--verify|--uninstall]
#
# 做两件事：
#   1. 往 .desktop 的 Exec= 行注入必需启动参数（系统级需 sudo，
#      无 sudo 时自动改用 ~/.local/share/applications 下的用户级副本）
#   2. 生成 ~/.local/bin/chrome-msm-vaapi 包装脚本，方便命令行直接起
#
# ⚠️ Vulkan 要分清是两个不同的东西，本脚本只动其中一个：
#   --use-angle=vulkan  = 只把 ANGLE（WebGL/GL 呈现）切到 Vulkan 后端。
#       骁龙 8 Elite 需要它，否则文字糊成一团、画面重影；实测**不影响** VA-API 硬解。
#   --enable-features=Vulkan = 打开 Vulkan 图形后端。实测（8 Elite + Chrome 151）
#       它会让 Chrome **完全不创建 VA-API 解码上下文**——GPU 进程照常探测、
#       建满各 profile 的 config，然后 vaTerminate，一个 CreateContext 都没有，
#       视频静默回落软解。所以**任何机型都不要加这一项**。
#   nabu(SD855) 是另一种情况：连 --use-angle=vulkan 也要去掉（wayland 与 Vulkan
#   冲突），用 DMD_VULKAN=off。
#
# 判据是显示是否正常 + 驱动日志里有没有 CreateContext/会话结束，不是 GPU 进程有没有
# 打 "not compatible with Vulkan"（那句两台机器都会打，可以无视）。
# 详见 doc/browser-vaapi-guide.md 第 2 / 2.5 节。

set -u

# --enable-features 只能出现一次：重复出现时 CommandLine 只取一个值，
# 后面那份会被丢掉 —— 所以所有开关必须并进同一个逗号列表。
# 注意这里**故意不含 Vulkan**，理由见文件头。
FEATURES="VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL"
ANGLE_FLAG=""
[ "${DMD_VULKAN:-on}" != "off" ] && ANGLE_FLAG="--use-angle=vulkan"

FLAGS="--ozone-platform=wayland --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist $ANGLE_FLAG --enable-features=$FEATURES"
PREFIX=""
[ "${DMD_VA_LOG:-}" = "1" ] && PREFIX="env DMD_VA_LOG=1 MESA_LOADER_DRIVER_OVERRIDE=msm "
# 判断 .desktop 是否已经和"当前这套参数"一致（换机型/换 DMD_VULKAN 时会不一致 → 重写）
SIG="$PREFIX$FLAGS"
MARKER="render-node-override"
LOCAL_BIN="$HOME/.local/bin"
WRAPPER="$LOCAL_BIN/chrome-msm-vaapi"
USER_APPS="$HOME/.local/share/applications"

say() { printf '%s\n' "$*"; }
err() { printf '%s\n' "error: $*" >&2; }

usage() {
    cat <<'EOF'
用法: configure-chrome-vaapi.sh [--verify|--uninstall]
      DMD_VULKAN=off sh configure-chrome-vaapi.sh    # nabu(SD855)：不注入 ANGLE Vulkan

默认执行安装。环境变量：
  CHROME_BIN   指定 Chrome 可执行文件（默认自动探测）
  DMD_VA_LOG   设为 1 时，注入的 Exec 会带上 DMD_VA_LOG=1 以便看驱动日志
  DMD_VULKAN   默认 on（注入 --use-angle=vulkan，骁龙 8 Elite 显示正常需要它，
               实测不影响硬解）；设为 off 则不注入（nabu / SD855 用这个）。
               无论 on/off 都**不会**注入 --enable-features=Vulkan —— 那一项
               会让 Chrome 完全不建 VA-API 解码上下文，见文件头。
EOF
}

# 找 Chrome 可执行文件
find_chrome() {
    if [ -n "${CHROME_BIN:-}" ]; then
        [ -x "$CHROME_BIN" ] && { printf '%s\n' "$CHROME_BIN"; return 0; }
        err "CHROME_BIN 指向的文件不可执行: $CHROME_BIN"
        return 1
    fi
    for c in google-chrome google-chrome-stable chromium chromium-browser; do
        p=$(command -v "$c" 2>/dev/null) && { printf '%s\n' "$p"; return 0; }
    done
    return 1
}

# 列出候选 .desktop（系统级 + 用户级）
# 桌面文件名不止 google-chrome.desktop：Chrome 自己会在 ~/.local/share/applications
# 注册 com.google.Chrome.desktop（Labels 里 Name=Google Chrome），用户点图标启动时
# DE 用的正是这份。只匹配 google-chrome*.desktop 会漏掉它 —— 实测那份里残留着
# --enable-features=Vulkan，等于点图标打开的 Chrome 一个硬解上下文都不建。
desktop_files() {
    for d in /usr/share/applications "$USER_APPS"; do
        [ -d "$d" ] || continue
        for f in "$d"/google-chrome*.desktop "$d"/com.google.Chrome*.desktop "$d"/chromium*.desktop; do
            [ -f "$f" ] && printf '%s\n' "$f"
        done
    done
}

# 算不算"已配好"：文件里已经是当前这套参数，且没残留 --enable-features 里的 Vulkan
# （老版本脚本会注入它，实测那一项会让 Chrome 完全不建 VA-API 解码上下文）。
configured() {
    grep -qF -- "$SIG" "$1" 2>/dev/null || return 1
    grep -qE -- "--enable-features=[^ \"]*Vulkan" "$1" && return 1
    return 0
}

# 往一个 .desktop 注入参数。已配好则跳过；残留错误参数的会被重写（自愈）。
patch_desktop() {
    target="$1"
    if configured "$target"; then
        say "  已配置，跳过: $target"
        return 0
    fi

    # 系统目录需要 sudo；没有 sudo 就复制到用户目录再改
    writer="cat"
    dest="$target"
    case "$target" in
        /usr/*)
            if [ -w "$target" ]; then
                :
            elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
                writer="sudo tee"
            else
                mkdir -p "$USER_APPS"
                dest="$USER_APPS/$(basename "$target")"
                if configured "$dest"; then
                    say "  用户级副本已配置，跳过: $dest"
                    return 0
                fi
                say "  无 sudo，改写用户级副本: $dest"
            fi
            ;;
    esac

    [ -f "$dest.dsh-bak" ] || cp "$target" "$dest.dsh-bak" 2>/dev/null

    prefix="$PREFIX"

    # 重写 Exec= 行：拆出可执行文件、我们自己的参数、用户原有参数与 %U 之类占位符，
    # 再按当前 FLAGS 拼回去。这样既是幂等的（重复执行不叠加），又能**自愈**：
    # 老版本脚本注入过的错误参数（--enable-features 里的 Vulkan）会被清掉。
    # 用 awk 而不是 sed：Exec 行里有 = 和 , ，sed 的分隔符容易撞。
    awk -v flags="$FLAGS" -v pre="$prefix" '
        function is_ours(t) {
            return (t ~ /^--render-node-override=/ || t ~ /^--enable-features=/ ||
                    t ~ /^--ozone-platform=/ || t ~ /^--use-angle=/ ||
                    t == "--ignore-gpu-blocklist" ||
                    t ~ /^DMD_VA_LOG=/ || t ~ /^MESA_LOADER_DRIVER_OVERRIDE=/)
        }
        /^Exec=/ {
            line = substr($0, 6)
            n = split(line, tok, /[ \t]+/)
            bin = ""; i = 1; seen_env = 0
            if (tok[1] == "env") { seen_env = 1; i = 2 }
            rest = ""; ph = ""
            for (; i <= n; i++) {
                t = tok[i]
                if (t == "") continue
                if (!bin && seen_env && t !~ /^\// && t !~ /^-/) continue  # env 的赋值
                if (!bin && t ~ /^-/) continue                            # bin 之前不该有开关
                if (!bin) { bin = t; continue }
                if (is_ours(t)) continue
                if (t ~ /^%/) { ph = ph " " t; continue }
                rest = rest " " t
            }
            if (!bin) { print; next }
            print "Exec=" pre bin rest " " flags ph
            next
        }
        { print }
    ' "$target" > "$dest.tmp" || { err "改写失败: $target"; rm -f "$dest.tmp"; return 1; }

    if [ "$writer" = "cat" ]; then
        mv "$dest.tmp" "$dest"
    else
        sudo tee "$dest" < "$dest.tmp" >/dev/null && rm -f "$dest.tmp"
    fi
    say "  ✓ 已配置: $dest"
}

install_wrapper() {
    chrome=$(find_chrome) || { err "找不到 Chrome/Chromium，可用 CHROME_BIN 指定"; return 1; }
    mkdir -p "$LOCAL_BIN"
    cat > "$WRAPPER" <<EOF
#!/bin/sh
# 由 configure-chrome-vaapi.sh 生成。带 msm_drm VA-API 硬解参数启动 Chrome。
# 注入的参数见 doc/browser-vaapi-guide.md 第 2 节。
# ⚠️ 不要在 chrome://flags 里开 "Vulkan"（等价于 --enable-features=Vulkan）：
#    实测那样 Chrome 一个 VA-API 解码上下文都不会建，视频静默回落软解。
#    显示要正常只需 --use-angle=vulkan（本脚本按机型决定是否注入它）。
exec env MESA_LOADER_DRIVER_OVERRIDE=msm \\
    "$chrome" $FLAGS "\$@"
EOF
    chmod +x "$WRAPPER"
    say "  ✓ 包装脚本: $WRAPPER"
    case ":$PATH:" in
        *":$LOCAL_BIN:"*) ;;
        *) say "  提示: $LOCAL_BIN 不在 PATH 里，需要自行加入才能直接敲命令名" ;;
    esac
}

do_install() {
    say "配置 Chrome VA-API 硬解..."
    found=0
    for f in $(desktop_files); do
        found=1
        patch_desktop "$f"
    done
    [ "$found" = 0 ] && say "  未找到 Chrome 的 .desktop，跳过桌面图标配置"
    install_wrapper || return 1
    say ""
    if [ -n "$ANGLE_FLAG" ]; then
        say "ANGLE: 已注入 --use-angle=vulkan（骁龙 8 Elite 显示正常需要它，不影响硬解）。"
        say "如果这台是 nabu(SD855)，重跑一次 DMD_VULKAN=off $0 覆盖配置。"
    else
        say "ANGLE: 本次按 nabu(SD855) 处理，未注入 --use-angle=vulkan。"
        say "（骁龙 8 Elite 不能这么设：少了它文字糊成一团、画面重影。）"
    fi
    say "两机型共同要求：--enable-features 里**不能**有 Vulkan，chrome://flags 里的"
    say "\"Vulkan\" 也要保持 Disabled —— 实测开着就没有任何 VA-API 解码上下文。"
    say "改完重启浏览器。判据是视频与文字是否正常，不看日志有没有"
    say "'not compatible with Vulkan'（那句两台机器都会打，可无视）。"
    say "真正确认硬解在跑：播放时驱动日志要有 CreateContext 与"
    say "'会话结束: 送入 N 单元, 收到 M 帧'，且 N==M。"
    say "详见 doc/browser-vaapi-guide.md 第 2.5 节。"
    say ""
    say "验证: bash tools/check-browser-vaapi.sh"
}

do_verify() {
    rc=0
    say "检查 .desktop:"
    found=0
    for f in $(desktop_files); do
        found=1
        # grep -c 无匹配时本身就输出 0 并以 1 退出，不能再 || echo 0，
        # 否则 n 变成两行的 "0\n0"，下面的 [ "$n" -gt 0 ] 会报 Illegal number。
        n=$(grep -c "$MARKER" "$f" 2>/dev/null || true)
        [ -n "$n" ] || n=0
        if configured "$f"; then
            say "  ✓ $f （$n 处）"
        elif [ "$n" -gt 0 ]; then
            say "  ✗ $f 的参数与当前机型设置不一致（或 --enable-features 里残留 Vulkan）"
            say "    期望: $SIG"
            say "    重跑本脚本即可自动改写； nabu 用 DMD_VULKAN=off"
            rc=1
        else
            say "  ✗ 未配置: $f"
            rc=1
        fi
    done
    [ "$found" = 0 ] && { say "  未找到 Chrome 的 .desktop"; rc=1; }

    say "检查包装脚本:"
    if [ -x "$WRAPPER" ]; then
        say "  ✓ $WRAPPER"
        grep -E "^exec " "$WRAPPER" 2>/dev/null | grep -qE -- "--enable-features=[^ ]*Vulkan" && {
            say "  ✗ 包装脚本里残留 --enable-features=...Vulkan，重跑本脚本改写"
            rc=1
        }
    else
        say "  ✗ 缺失: $WRAPPER"
        rc=1
    fi

    say ""
    say "正确配置里只应出现 --use-angle=vulkan（8 Elite 需要，nabu 用 DMD_VULKAN=off 去掉）。"
    say "chrome://flags 里的 \"Vulkan\"（= --enable-features=Vulkan）应保持 Disabled："
    say "开着就没有任何硬解上下文，页面照放、驱动 0 配对帧，像配了没生效。"
    say "最终判据是播放时驱动日志里有没有 CreateContext 与"
    say "'会话结束: 送入 N, 收到 M'（N==M），以及视频文字显示是否正常；"
    say "GPU 进程有没有打 'not compatible with Vulkan' 可以无视（两台都会打）。"
    return $rc
}

do_uninstall() {
    say "还原 Chrome 配置..."
    for f in $(desktop_files); do
        if [ -f "$f.dsh-bak" ]; then
            if [ -w "$f" ]; then
                mv "$f.dsh-bak" "$f" && say "  ✓ 已还原: $f"
            elif command -v sudo >/dev/null 2>&1; then
                sudo mv "$f.dsh-bak" "$f" && say "  ✓ 已还原: $f"
            else
                err "无权还原: $f"
            fi
        fi
    done
    # 用户级副本整体删掉
    for f in "$USER_APPS"/google-chrome*.desktop "$USER_APPS"/chromium*.desktop; do
        [ -f "$f" ] && grep -q "$MARKER" "$f" 2>/dev/null && rm -f "$f" && say "  ✓ 已删除用户级副本: $f"
    done
    [ -f "$WRAPPER" ] && rm -f "$WRAPPER" && say "  ✓ 已删除: $WRAPPER"
    say "chrome://flags 里若开过 \"Vulkan\"，请保持 Disabled（开着就没有硬解）。"
}

case "${1:-}" in
    --verify)    do_verify ;;
    --uninstall) do_uninstall ;;
    -h|--help)   usage ;;
    "")          do_install ;;
    *)           err "未知参数: $1"; usage; exit 2 ;;
esac
