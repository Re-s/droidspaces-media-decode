#!/bin/sh
# 给 Chrome / Chromium 配好 msm_drm VA-API 编解码（幂等，可重复执行）。
#
# 用法: configure-chrome-vaapi.sh [--verify|--uninstall] [--wrapper-only] [--gbm-compat-dir DIR]
#
# 做两件事：
#   1. 往 .desktop 的 Exec= 行注入必需启动参数（系统级需 sudo，
#      无 sudo 时自动改用 ~/.local/share/applications 下的用户级副本）
#   2. 生成 ~/.local/bin/chrome-msm-vaapi 包装脚本，方便命令行直接起
#
# 默认用 Chrome 156 / GNOME Wayland 实测的 ANGLE OpenGL 配置。
# 旧版 Chrome、Vulkan 及其他设备的配置见 doc/browser-vaapi-guide.md。

set -u

# --enable-features 只能出现一次：重复出现时 CommandLine 只取一个值，
# 后面那份会被丢掉 —— 所以所有开关必须并进同一个逗号列表。
FEATURES="AcceleratedVideoDecoder,AcceleratedVideoDecodeLinuxGL,VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL,VaapiIgnoreDriverChecks,PlatformHEVCDecoderSupport,AcceleratedVideoEncoder"
GBM_COMPAT_DIR="${DMD_GBM_COMPAT_DIR:-/usr/local/lib/droidspaces-gbm}"
MARKER="render-node-override"
LOCAL_BIN="$HOME/.local/bin"
WRAPPER="$LOCAL_BIN/chrome-msm-vaapi"
USER_APPS="$HOME/.local/share/applications"

say() { printf '%s\n' "$*"; }
# Escape values embedded in double quotes in the generated shell script.
shell_escape() { printf '%s' "$1" | sed 's/[\\"$`]/\\&/g'; }
err() { printf '%s\n' "error: $*" >&2; }

usage() {
    cat <<'EOF'
用法: configure-chrome-vaapi.sh [--verify|--uninstall] [--wrapper-only] [--gbm-compat-dir DIR]

默认执行安装。选项与环境变量：
  --wrapper-only       只安装、验证或卸载命令行包装脚本，不改 .desktop。
  --gbm-compat-dir DIR  指定可选 libgbm.so.1 兼容库目录，优先于环境变量。
  CHROME_BIN   指定 Chrome 可执行文件，也用于桌面入口（默认自动探测）。
  DMD_VA_LOG   安装时设为 1，默认启用驱动日志。
  MESA_LOADER_DRIVER_OVERRIDE, TU_DEBUG, LIBVA_DRIVERS_PATH
               保存安装时的值作为包装脚本默认值，启动环境可覆盖。
  DMD_GBM_COMPAT_DIR  可选 NV12 GBM 分配兼容库目录（默认 /usr/local/lib/droidspaces-gbm）。
               启动时前置该目录，并保留浏览器进程继承的 LD_LIBRARY_PATH。
  DMD_ANGLE    gl（默认）、vulkan 或 auto（让 Chrome 选择）。
               旧选项 DMD_VULKAN=on/off 分别对应 vulkan/auto。
  DMD_DISABLE_GMB_READBACK=1
               为需要它的旧版 Chrome 加入 GMB 读回调试开关。
EOF
}

# 找 Chrome 可执行文件
find_chrome() {
    if [ -n "${CHROME_BIN:-}" ]; then
        [ -x "$CHROME_BIN" ] && { printf '%s\n' "$CHROME_BIN"; return 0; }
        err "CHROME_BIN 指向的文件不可执行: $CHROME_BIN"
        return 1
    fi
    # 优先用打包的入口；/usr/local/bin 可能还有注入旧参数的包装脚本。
    if [ -x /usr/bin/google-chrome-stable ]; then
        printf '%s\n' /usr/bin/google-chrome-stable
        return 0
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
# 仅有 --enable-features=Vulkan，旧测试会回落软解；需识别这份入口并统一配置。
desktop_files() {
    [ "$wrapper_only" = 0 ] || return 0
    for d in /usr/share/applications "$USER_APPS"; do
        [ -d "$d" ] || continue
        for f in "$d"/google-chrome*.desktop "$d"/com.google.Chrome*.desktop "$d"/chromium*.desktop; do
            [ -f "$f" ] && printf '%s\n' "$f"
        done
    done
}

# 检查是否符合本脚本的默认基准；完整 Chrome Vulkan 配置另按文档验证。
configured() {
    grep -qF -- "$SIG" "$1" 2>/dev/null || return 1
    grep -qF -- "$FLAGS" "$1" 2>/dev/null || return 1
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

    if [ ! -f "$dest.dsh-bak" ]; then
        if [ "$writer" = "cat" ]; then
            cp "$target" "$dest.dsh-bak" || return 1
        else
            sudo cp "$target" "$dest.dsh-bak" || return 1
        fi
    fi
    temp=$(mktemp) || return 1

    prefix="$PREFIX"

    # 重写 Exec= 行：拆出可执行文件、我们自己的参数、用户原有参数与 %U 之类占位符，
    # 再按当前 FLAGS 拼回去。这样既是幂等的（重复执行不叠加），又能**自愈**：
    # 老版本注入的参数会统一改回当前默认基准（不自动启用完整 Chrome Vulkan）。
    # 用 awk 而不是 sed：Exec 行里有 = 和 , ，sed 的分隔符容易撞。
    awk -v flags="$FLAGS" -v pre="$prefix" '
        function is_ours(t) {
            return (t ~ /^--render-node-override=/ || t ~ /^--enable-features=/ ||
                    t ~ /^--ozone-platform=/ || t ~ /^--use-angle=/ || t ~ /^--use-gl=/ ||
                    t == "--gmb-readback-from-texture-disabled-for-debugging" ||
                    t == "--ignore-gpu-blocklist" ||
                    t ~ /^DMD_VA_LOG=/ || t ~ /^MESA_LOADER_DRIVER_OVERRIDE=/ ||
                    t ~ /^LD_LIBRARY_PATH=/)
        }
        /^Exec=/ {
            line = substr($0, 6)
            pos = index(line, pre)
            if (pos > 0) line = substr(line, 1, pos - 1) substr(line, pos + length(pre))
            n = split(line, tok, /[ \t]+/)
            bin = ""; i = 1; seen_env = 0
            if (tok[1] == "env") { seen_env = 1; i = 2 }
            rest = ""; ph = ""; assignments = ""
            for (; i <= n; i++) {
                t = tok[i]
                if (t == "") continue
                if (!bin && seen_env && t ~ /^[A-Za-z_][A-Za-z0-9_]*=/) {
                    # 保留用户的环境配置（包括原有 LD_LIBRARY_PATH）。
                    if (!is_ours(t) || t ~ /^LD_LIBRARY_PATH=/) assignments = assignments t " "
                    continue
                }
                if (!bin && t ~ /^-/) continue                            # bin 之前不该有开关
                if (!bin) { bin = t; continue }
                if (is_ours(t)) continue
                if (t ~ /^%/) { ph = ph " " t; continue }
                rest = rest " " t
            }
            if (!bin) { print; next }
            env_prefix = assignments ? "env " assignments : ""
            print "Exec=" env_prefix pre bin rest " " flags ph
            next
        }
        { print }
    ' "$target" > "$temp" || { err "改写失败: $target"; rm -f "$temp"; return 1; }

    if [ "$writer" = "cat" ]; then
        cat "$temp" > "$dest" && rm -f "$temp" || { rm -f "$temp"; return 1; }
    else
        sudo tee "$dest" < "$temp" >/dev/null && rm -f "$temp" || { rm -f "$temp"; return 1; }
    fi
    say "  ✓ 已配置: $dest"
}

install_wrapper() {
    chrome=$(find_chrome) || { err "找不到 Chrome/Chromium，可用 CHROME_BIN 指定"; return 1; }
    mkdir -p "$LOCAL_BIN" || return 1
    if [ -f "$WRAPPER" ] && [ ! -e "$WRAPPER.dsh-bak" ] && [ ! -e "$WRAPPER.dsh-created" ]; then
        cp -p "$WRAPPER" "$WRAPPER.dsh-bak" || return 1
    elif [ ! -e "$WRAPPER" ]; then
        : > "$WRAPPER.dsh-created" || return 1
    fi
    compat_escaped=$(shell_escape "$GBM_COMPAT_DIR")
    chrome_escaped=$(shell_escape "$chrome")
    chrome_override=$(shell_escape "${CHROME_BIN:-}")
    mesa_escaped=$(shell_escape "${MESA_LOADER_DRIVER_OVERRIDE:-msm}")
    turnip_escaped=$(shell_escape "${TU_DEBUG:-}")
    drivers_escaped=$(shell_escape "${LIBVA_DRIVERS_PATH:-}")
    cat > "$WRAPPER" <<EOF || return 1
#!/bin/sh
# 由 configure-chrome-vaapi.sh 生成。参数见 doc/browser-vaapi-guide.md。
compat_dir="$compat_escaped"
if [ -r "\$compat_dir/libgbm.so.1" ]; then
    case ":\${LD_LIBRARY_PATH:-}:" in
        *":\$compat_dir:"*) ;;
        *) export LD_LIBRARY_PATH="\$compat_dir\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}" ;;
    esac
fi
# 启动环境优先于安装时记录的配置。
mesa_default="$mesa_escaped"
turnip_default="$turnip_escaped"
drivers_default="$drivers_escaped"
export MESA_LOADER_DRIVER_OVERRIDE="\${MESA_LOADER_DRIVER_OVERRIDE:-\$mesa_default}"
export LIBVA_DRIVER_NAME="\${LIBVA_DRIVER_NAME:-msm_drm}"
if [ -n "\$turnip_default" ]; then export TU_DEBUG="\${TU_DEBUG-\$turnip_default}"; fi
if [ -n "\$drivers_default" ]; then
    export LIBVA_DRIVERS_PATH="\${LIBVA_DRIVERS_PATH-\$drivers_default}"
fi
# 桌面入口自带参数；直接调用包装脚本时使用本次配置的 Chrome 与默认参数。
if [ "\${1:-}" = "--dmd-exec" ]; then
    shift
    chrome="\$1"
    shift
    chrome_override="$chrome_override"
    if [ -n "\$chrome_override" ]; then chrome="\$chrome_override"; fi
else
    chrome="$chrome_escaped"
    set -- $FLAGS "\$@"
fi
# 保留调用方的其他禁用项，Chrome 只读取最后一项 --disable-features。
disabled=ResolutionBasedDecoderPriority
for arg do
    shift
    case "\$arg" in
        --disable-features=*)
            list=\${arg#*=}
            while [ -n "\$list" ]; do
                feature=\${list%%,*}
                case ",\$disabled," in *",\$feature,"*) ;; *) disabled="\$disabled,\$feature" ;; esac
                case "\$list" in *,*) list=\${list#*,} ;; *) break ;; esac
            done
            ;;
        *) set -- "\$@" "\$arg" ;;
    esac
done
exec env $LOG_ENV "\$chrome" "--disable-features=\$disabled" "\$@"
EOF
    chmod +x "$WRAPPER" || return 1
    say "  ✓ 包装脚本: $WRAPPER"
    case ":$PATH:" in
        *":$LOCAL_BIN:"*) ;;
        *) say "  提示: $LOCAL_BIN 不在 PATH 里，需要自行加入才能直接敲命令名" ;;
    esac
}

do_install() {
    say "配置 Chrome VA-API 硬件编解码..."
    install_wrapper || return 1
    found=0
    for f in $(desktop_files); do
        found=1
        patch_desktop "$f" || return 1
    done
    if [ "$wrapper_only" = 0 ] && [ "$found" = 0 ]; then
        say "  未找到 Chrome 的 .desktop，跳过桌面图标配置"
    fi
    say "ANGLE 后端: $angle。完全退出 Chrome 后，用配置好的入口重新启动。"
    say "核对 chrome://media-internals 中的 VaapiVideoDecoder 与实际画面。"
    say "编码与 NV12 GBM 兼容库说明: doc/hardware-encoding.md。"
}

do_verify() {
    rc=0
    say "检查 .desktop:"
    found=0
    for f in $(desktop_files); do
        found=1
        # 同名用户入口优先于系统入口；无 sudo 安装会使用这份有效覆盖。
        case "$f" in
            /usr/*)
                override="$USER_APPS/$(basename "$f")"
                if [ -f "$override" ]; then
                    if configured "$override"; then
                        say "  ✓ $f 由已配置的用户入口覆盖（shadow）: $override"
                    else
                        say "  ✗ $f 被尚未正确配置的用户入口覆盖: $override"
                        rc=1
                    fi
                    continue
                fi
                ;;
        esac
        # grep -c 无匹配时本身就输出 0 并以 1 退出，不能再 || echo 0，
        # 否则 n 变成两行的 "0\n0"，下面的 [ "$n" -gt 0 ] 会报 Illegal number。
        n=$(grep -c "$MARKER" "$f" 2>/dev/null || true)
        [ -n "$n" ] || n=0
        if configured "$f"; then
            say "  ✓ $f （$n 处）"
        elif [ "$n" -gt 0 ]; then
            say "  ✗ $f 的参数与本脚本默认基准不一致（完整 Chrome Vulkan 配置见文档）"
            say "    期望: $SIG"
            say "    重跑本脚本即可改写；使用原生后端可设 DMD_ANGLE=auto"
            rc=1
        else
            say "  ✗ 未配置: $f"
            rc=1
        fi
    done
    if [ "$wrapper_only" = 0 ] && [ "$found" = 0 ]; then
        say "  未找到 Chrome 的 .desktop"
        rc=1
    fi

    say "检查包装脚本:"
    if [ -x "$WRAPPER" ] && grep -qF -- "$FLAGS" "$WRAPPER" &&
        grep -qFx 'disabled=ResolutionBasedDecoderPriority' "$WRAPPER"; then
        say "  ✓ $WRAPPER"
    else
        say "  ✗ 包装脚本缺失或参数与本次配置不一致: $WRAPPER"
        rc=1
    fi
    say "验证仅检查启动配置。播放时再确认解码器、画面和丢帧。"
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
    if [ -f "$WRAPPER.dsh-bak" ]; then
        mv "$WRAPPER.dsh-bak" "$WRAPPER" && say "  ✓ 已还原: $WRAPPER"
    elif [ -f "$WRAPPER.dsh-created" ]; then
        rm -f "$WRAPPER" "$WRAPPER.dsh-created"
        say "  ✓ 已删除生成的包装脚本: $WRAPPER"
    fi
    say "如曾手工调整 chrome://flags，请核对恢复后的实际后端；完整 Vulkan 配置见文档。"
}

mode=install
wrapper_only=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --verify) mode=verify ;;
        --wrapper-only) wrapper_only=1 ;;
        --uninstall) mode=uninstall ;;
        --gbm-compat-dir)
            [ "$#" -ge 2 ] && [ -n "$2" ] || { err "--gbm-compat-dir 需要目录"; exit 2; }
            GBM_COMPAT_DIR=$2
            shift
            ;;
        -h|--help) usage; exit 0 ;;
        *) err "未知参数: $1"; usage; exit 2 ;;
    esac
    shift
done
case "$GBM_COMPAT_DIR" in
    /*) ;;
    *) err "兼容库目录必须是绝对路径: $GBM_COMPAT_DIR"; exit 2 ;;
esac
angle=${DMD_ANGLE:-}
if [ -z "$angle" ]; then
    case "${DMD_VULKAN:-}" in
        on) angle=vulkan ;;
        off) angle=auto ;;
        *) angle=gl ;;
    esac
fi
case "$angle" in
    gl|vulkan) ANGLE_FLAGS=" --use-gl=angle --use-angle=$angle" ;;
    auto) ANGLE_FLAGS="" ;;
    *) err "DMD_ANGLE 必须为 gl、vulkan 或 auto"; exit 2 ;;
esac
READBACK_FLAG=""
[ "${DMD_DISABLE_GMB_READBACK:-}" = "1" ] &&
    READBACK_FLAG=" --gmb-readback-from-texture-disabled-for-debugging"
FLAGS="--ozone-platform=wayland --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist$ANGLE_FLAGS --enable-features=$FEATURES$READBACK_FLAG"
LOG_ENV=""
[ "${DMD_VA_LOG:-}" = "1" ] && LOG_ENV="DMD_VA_LOG=1"
# 使用包装脚本在浏览器启动时合并 LD_LIBRARY_PATH，避免把安装时环境写死。
PREFIX="\"$WRAPPER\" --dmd-exec "
SIG="$PREFIX"
case "$mode" in
    verify) do_verify ;;
    uninstall) do_uninstall ;;
    install) do_install ;;
esac
