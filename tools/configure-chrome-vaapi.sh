#!/bin/sh
# 给 Chrome / Chromium 配好 msm_drm VA-API 编解码（幂等，可重复执行）。
#
# 用法: configure-chrome-vaapi.sh [--verify|--uninstall] [--gbm-compat-dir DIR]
#
# 做两件事：
#   1. 往 .desktop 的 Exec= 行注入必需启动参数（系统级需 sudo，
#      无 sudo 时自动改用 ~/.local/share/applications 下的用户级副本）
#   2. 生成 ~/.local/bin/chrome-msm-vaapi 包装脚本，方便命令行直接起
#
# 默认采用已验证的 ANGLE Vulkan + Skia GL 基准：
#   --use-angle=vulkan 选择 ANGLE 的后端；Chrome 的 Vulkan 合成/光栅化是另一项。
#   Chrome 151 的旧测试仅加 feature Vulkan 会回落软解；Chrome 154 在完整开启
#   Vulkan,DefaultANGLEVulkan,VulkanFromANGLE,VaapiIgnoreDriverChecks 后已通过
#   这台 SM8750P 的硬解与硬编测试。不要从旧结果推广到所有版本或设备。
#   本脚本保持默认基准参数不变；完整 Chrome Vulkan 配置见文档第 2.5 节。
#   旧 nabu(SD855) 配置还需去掉 --use-angle=vulkan，可用 DMD_VULKAN=off。
#
# 验证实际 Skia/ANGLE/Vulkan 后端、解码器、帧数及驱动会话，不能只看
# "not compatible with Vulkan" warning；成功配置也可能打印它。
# 详见 doc/browser-vaapi-guide.md 第 2 / 2.5 节。

set -u

# --enable-features 只能出现一次：重复出现时 CommandLine 只取一个值，
# 后面那份会被丢掉 —— 所以所有开关必须并进同一个逗号列表。
# 默认基准不含 Chrome Vulkan；完整 Vulkan 配置需另按文档独立验证。
FEATURES="VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiVideoDecodeLinuxGL,AcceleratedVideoEncoder"
ANGLE_FLAG=""
[ "${DMD_VULKAN:-on}" != "off" ] && ANGLE_FLAG="--use-angle=vulkan"

FLAGS="--ozone-platform=wayland --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist $ANGLE_FLAG --enable-features=$FEATURES --gmb-readback-from-texture-disabled-for-debugging"
GBM_COMPAT_DIR="${DMD_GBM_COMPAT_DIR:-/usr/local/lib/droidspaces-gbm}"
MARKER="render-node-override"
LOCAL_BIN="$HOME/.local/bin"
WRAPPER="$LOCAL_BIN/chrome-msm-vaapi"
USER_APPS="$HOME/.local/share/applications"

say() { printf '%s\n' "$*"; }
err() { printf '%s\n' "error: $*" >&2; }

usage() {
    cat <<'EOF'
用法: configure-chrome-vaapi.sh [--verify|--uninstall] [--gbm-compat-dir DIR]
      DMD_VULKAN=off sh configure-chrome-vaapi.sh    # nabu(SD855)：不注入 ANGLE Vulkan

默认执行安装。选项与环境变量：
  --gbm-compat-dir DIR  指定可选 libgbm.so.1 兼容库目录，优先于环境变量。
  CHROME_BIN   指定 Chrome 可执行文件（默认自动探测）
  DMD_VA_LOG   设为 1 时，注入的 Exec 会带上 DMD_VA_LOG=1 以便看驱动日志
  DMD_GBM_COMPAT_DIR  可选 NV12 GBM 分配兼容库目录（默认 /usr/local/lib/droidspaces-gbm）。
               启动时前置该目录，并保留浏览器进程继承的 LD_LIBRARY_PATH。
  DMD_VULKAN   默认 on（ANGLE Vulkan + Skia GL 基准）；设为 off 则不注入
               --use-angle=vulkan（旧 nabu / SD855 配置）。该变量不切换 Chrome
               的 Vulkan 合成后端；Chrome 154 完整 Vulkan 配置见文档第 2.5 节。
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
# 仅有 --enable-features=Vulkan，旧测试会回落软解；需识别这份入口并统一配置。
desktop_files() {
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
                    t ~ /^--ozone-platform=/ || t ~ /^--use-angle=/ ||
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
    mkdir -p "$LOCAL_BIN"
    if [ -f "$WRAPPER" ] && [ ! -e "$WRAPPER.dsh-bak" ] && [ ! -e "$WRAPPER.dsh-created" ]; then
        cp -p "$WRAPPER" "$WRAPPER.dsh-bak" || return 1
    elif [ ! -e "$WRAPPER" ]; then
        : > "$WRAPPER.dsh-created" || return 1
    fi
    # 路径写入 shell 字符串时转义，允许空格及 shell 特殊字符。
    compat_escaped=$(printf '%s' "$GBM_COMPAT_DIR" | sed 's/[\\"$`]/\\&/g')
    chrome_escaped=$(printf '%s' "$chrome" | sed 's/[\\"$`]/\\&/g')
    cat > "$WRAPPER" <<EOF
#!/bin/sh
# 由 configure-chrome-vaapi.sh 生成。带 msm_drm VA-API 编解码参数启动 Chrome。
# 注入的参数见 doc/browser-vaapi-guide.md 第 2 节。
# 默认采用 ANGLE Vulkan + Skia GL 基准；不要仅额外开启单项 Chrome Vulkan。
# Chrome 154 的完整 Vulkan 配置见 doc/browser-vaapi-guide.md 第 2.5 节。
# 请用实际后端、解码器与播放帧数验证，warning 不能单独判定成功或失败。
compat_dir="$compat_escaped"
if [ -r "\$compat_dir/libgbm.so.1" ]; then
    case ":\${LD_LIBRARY_PATH:-}:" in
        *":\$compat_dir:"*) ;;
        *) export LD_LIBRARY_PATH="\$compat_dir\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}" ;;
    esac
fi
# 桌面入口自带参数；直接调用包装脚本时使用本次配置的 Chrome 与默认参数。
if [ "\${1:-}" = "--dmd-exec" ]; then
    shift
else
    set -- "$chrome_escaped" $FLAGS "\$@"
fi
exec env MESA_LOADER_DRIVER_OVERRIDE=msm $LOG_ENV "\$@"
EOF
    chmod +x "$WRAPPER"
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
    [ "$found" = 0 ] && say "  未找到 Chrome 的 .desktop，跳过桌面图标配置"
    say ""
    if [ -n "$ANGLE_FLAG" ]; then
        say "默认基准：ANGLE Vulkan + Skia GL（已注入 --use-angle=vulkan）。"
        say "如果这台是 nabu(SD855)，重跑一次 DMD_VULKAN=off $0 覆盖配置。"
    else
        say "本次未注入 --use-angle=vulkan，适用于旧 nabu(SD855) 配置。"
        say "骁龙 8 Elite 的基准配置保留 ANGLE Vulkan；请按实际显示验证。"
    fi
    say "本脚本不切换 Chrome Vulkan 后端；Chrome 154 可用完整四项 feature 配置，"
    say "见文档第 2.5 节，不能只额外开启单项 Vulkan。"
    say "改完完全退出并重启浏览器，核对实际后端与视频文字显示。"
    say "'not compatible with Vulkan' warning 单独不能证明失败。"
    say "真正确认硬解在跑：播放时驱动日志要有 CreateContext 与"
    say "'会话结束: 送入 N 单元, 收到 M 帧'，且 N==M。"
    say "详见 doc/browser-vaapi-guide.md 第 2.5 节。"
    say ""
    say "编码条件：AcceleratedVideoEncoder + gmb-readback 调试开关；按需使用 NV12 GBM 兼容库。"
    say "Chrome 编码验证与适用范围：doc/hardware-encoding.md。"
    say "验证硬解: bash tools/check-browser-vaapi.sh"
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
        grep -E "^(exec | *set -- )" "$WRAPPER" 2>/dev/null | grep -qE -- "--enable-features=[^ ]*Vulkan" && {
            say "  ✗ 包装脚本不符合默认基准；完整 Chrome Vulkan 配置请按文档验证"
            rc=1
        }
    else
        say "  ✗ 缺失: $WRAPPER"
        rc=1
    fi

    say ""
    say "本次检查默认 ANGLE Vulkan + Skia GL 基准；旧 nabu 可用 DMD_VULKAN=off。"
    say "若需 Chrome Vulkan 后端，请参照文档第 2.5 节完整四项 feature 配置。"
    say "不要仅依据 warning 或 chrome://gpu 的能力列表确认硬解。"
    say "最终判据是播放时驱动日志里有没有 CreateContext 与"
    say "'会话结束: 送入 N, 收到 M'（N==M），以及视频文字显示是否正常；"
    say "还应核对 Media 解码器、播放帧数、丢帧及 Skia/ANGLE/Vulkan 实际后端。"
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
while [ "$#" -gt 0 ]; do
    case "$1" in
        --verify) mode=verify ;;
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
