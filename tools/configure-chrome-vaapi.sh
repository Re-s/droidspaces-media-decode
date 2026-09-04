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
# ⚠️ 有一项本脚本无法代劳：Vulkan 必须在 chrome://flags 里手动关。
# ozone wayland 与 Vulkan 硬性冲突，而 Chrome 没有可用的命令行开关
# —— 实测 --disable-vulkan（这个开关根本不存在）、--disable-features=Vulkan、
# --use-vulkan=disabled 及其组合全部无效，GPU 进程照样报
# "'--ozone-platform=wayland' is not compatible with Vulkan"。
# 详见 doc/browser-vaapi-guide.md 第 2.5 节。

set -u

FLAGS="--ozone-platform=wayland --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist --enable-features=VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL"
MARKER="render-node-override"
LOCAL_BIN="$HOME/.local/bin"
WRAPPER="$LOCAL_BIN/chrome-msm-vaapi"
USER_APPS="$HOME/.local/share/applications"

say() { printf '%s\n' "$*"; }
err() { printf '%s\n' "error: $*" >&2; }

usage() {
    cat <<'EOF'
用法: configure-chrome-vaapi.sh [--verify|--uninstall]

默认执行安装。环境变量：
  CHROME_BIN   指定 Chrome 可执行文件（默认自动探测）
  DMD_VA_LOG   设为 1 时，注入的 Exec 会带上 DMD_VA_LOG=1 以便看驱动日志
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
desktop_files() {
    for d in /usr/share/applications "$USER_APPS"; do
        [ -d "$d" ] || continue
        for f in "$d"/google-chrome*.desktop "$d"/chromium*.desktop; do
            [ -f "$f" ] && printf '%s\n' "$f"
        done
    done
}

# 往一个 .desktop 注入参数。已注入过则跳过。
patch_desktop() {
    target="$1"
    if grep -q "$MARKER" "$target" 2>/dev/null; then
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
                if grep -q "$MARKER" "$dest" 2>/dev/null; then
                    say "  用户级副本已配置，跳过: $dest"
                    return 0
                fi
                say "  无 sudo，改写用户级副本: $dest"
            fi
            ;;
    esac

    [ -f "$dest.dsh-bak" ] || cp "$target" "$dest.dsh-bak" 2>/dev/null

    prefix=""
    [ "${DMD_VA_LOG:-}" = "1" ] && prefix="env DMD_VA_LOG=1 MESA_LOADER_DRIVER_OVERRIDE=msm "

    # 在可执行文件路径后插入 FLAGS，保留末尾的 %U / %F 等占位符。
    # 用 awk 而不是 sed：Exec 行里有 = 和 , ，sed 的分隔符容易撞。
    awk -v flags="$FLAGS" -v pre="$prefix" '
        /^Exec=/ {
            line = substr($0, 6)
            # 已有 flags 就不重复加
            if (index(line, "render-node-override") > 0) { print; next }
            # 拆出末尾占位符
            ph = ""
            if (match(line, /[ ]%[a-zA-Z]$/)) {
                ph = substr(line, RSTART, RLENGTH)
                line = substr(line, 1, RSTART - 1)
            }
            print "Exec=" pre line " " flags ph
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
# 记得在 chrome://flags 里把 Vulkan 设为 Disabled（命令行关不掉）。
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
    say "还需手动做一步：打开 chrome://flags，把 Vulkan 设为 Disabled，然后重启浏览器。"
    say "这一项没有可用的命令行开关，见 doc/browser-vaapi-guide.md 第 2.5 节。"
    say ""
    say "验证: bash tools/check-browser-vaapi.sh"
}

do_verify() {
    rc=0
    say "检查 .desktop:"
    found=0
    for f in $(desktop_files); do
        found=1
        n=$(grep -c "$MARKER" "$f" 2>/dev/null || echo 0)
        if [ "$n" -gt 0 ]; then
            say "  ✓ $f （$n 处）"
        else
            say "  ✗ 未配置: $f"
            rc=1
        fi
    done
    [ "$found" = 0 ] && { say "  未找到 Chrome 的 .desktop"; rc=1; }

    say "检查包装脚本:"
    if [ -x "$WRAPPER" ]; then
        say "  ✓ $WRAPPER"
    else
        say "  ✗ 缺失: $WRAPPER"
        rc=1
    fi

    say ""
    say "无法自动检查的一项：chrome://flags 里 Vulkan 是否已设为 Disabled。"
    say "判据是启动后 GPU 进程有没有打印 'not compatible with Vulkan'。"
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
    say "chrome://flags 里的 Vulkan 设置请自行还原。"
}

case "${1:-}" in
    --verify)    do_verify ;;
    --uninstall) do_uninstall ;;
    -h|--help)   usage ;;
    "")          do_install ;;
    *)           err "未知参数: $1"; usage; exit 2 ;;
esac
