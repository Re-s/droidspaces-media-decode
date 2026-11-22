#!/bin/sh
# Configure Chrome or Chromium to use the msm_drm VA-API driver on Wayland.
# Install a per-user wrapper and, unless --wrapper-only is given, update
# desktop entries. Re-running the installer refreshes the managed options.
# ANGLE OpenGL is the default tested with Chrome 156 on GNOME Wayland.
# See doc/browser-vaapi-guide.md for earlier Chrome and Vulkan setups.

set -u

# Chrome reads only the last --enable-features option. Use one comma-separated list.
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
Usage: configure-chrome-vaapi.sh [--verify|--uninstall] [--wrapper-only] [--gbm-compat-dir DIR]

Install by default. Options and environment:
  --wrapper-only       Install, verify or remove only the command-line wrapper.
  --gbm-compat-dir DIR  Set the optional NV12 GBM library directory.
  CHROME_BIN           Override the browser executable, including desktop launches.
  DMD_VA_LOG=1         Enable driver logging by default in the wrapper.
  MESA_LOADER_DRIVER_OVERRIDE, TU_DEBUG, LIBVA_DRIVERS_PATH
                       Save the install-time settings as overridable defaults.
  DMD_GBM_COMPAT_DIR   Set the GBM directory if no option is given.
                       By default, look in /usr/local/lib/droidspaces-gbm.
  DMD_ANGLE            Select gl (default), vulkan or auto.
                       Legacy DMD_VULKAN=on/off maps to vulkan/auto.
  DMD_DISABLE_GMB_READBACK=1
                       Add the debug readback switch for older Chrome versions.
EOF
}

# Locate the browser executable
find_chrome() {
    if [ -n "${CHROME_BIN:-}" ]; then
        [ -x "$CHROME_BIN" ] && { printf '%s\n' "$CHROME_BIN"; return 0; }
        err "CHROME_BIN is not executable: $CHROME_BIN"
        return 1
    fi
    # Prefer the packaged browser over wrappers in /usr/local/bin with stale flags.
    if [ -x /usr/bin/google-chrome-stable ]; then
        printf '%s\n' /usr/bin/google-chrome-stable
        return 0
    fi
    for c in google-chrome google-chrome-stable chromium chromium-browser; do
        p=$(command -v "$c" 2>/dev/null) && { printf '%s\n' "$p"; return 0; }
    done
    return 1
}

# Chrome can create a com.google.Chrome.desktop launcher in the user's
# applications directory. Include it as well as the packaged desktop entries.
desktop_files() {
    [ "$wrapper_only" = 0 ] || return 0
    for d in /usr/share/applications "$USER_APPS"; do
        [ -d "$d" ] || continue
        for f in "$d"/google-chrome*.desktop "$d"/com.google.Chrome*.desktop "$d"/chromium*.desktop; do
            [ -f "$f" ] && printf '%s\n' "$f"
        done
    done
}

# Check whether a desktop entry uses the configured wrapper and flags.
configured() {
    grep -qF -- "$SIG" "$1" 2>/dev/null || return 1
    grep -qF -- "$FLAGS" "$1" 2>/dev/null || return 1
    grep -qE -- "--enable-features=[^ \"]*Vulkan" "$1" && return 1
    return 0
}

# Update a desktop entry only if its configuration differs.
patch_desktop() {
    target="$1"
    if configured "$target"; then
        say "  Already configured: $target"
        return 0
    fi

    # Use the per-user desktop entry when the system entry is not writable.
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
                    say "  User override already configured: $dest"
                    return 0
                fi
                say "  Configuring a user override: $dest"
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

    # Replace old managed flags while retaining the executable, other
    # arguments and desktop field codes such as %U.
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
                    # Keep existing user environment settings, including LD_LIBRARY_PATH.
                    if (!is_ours(t) || t ~ /^LD_LIBRARY_PATH=/) assignments = assignments t " "
                    continue
                }
                if (!bin && t ~ /^-/) continue                            # Skip switches before the executable.
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
    ' "$target" > "$temp" || { err "Could not update: $target"; rm -f "$temp"; return 1; }

    if [ "$writer" = "cat" ]; then
        cat "$temp" > "$dest" && rm -f "$temp" || { rm -f "$temp"; return 1; }
    else
        sudo tee "$dest" < "$temp" >/dev/null && rm -f "$temp" || { rm -f "$temp"; return 1; }
    fi
    say "  ✓ Configured: $dest"
}

install_wrapper() {
    chrome=$(find_chrome) || { err "Chrome or Chromium not found. Set CHROME_BIN to its executable."; return 1; }
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
# Generated by configure-chrome-vaapi.sh. See doc/browser-vaapi-guide.md.
compat_dir="$compat_escaped"
if [ -r "\$compat_dir/libgbm.so.1" ]; then
    case ":\${LD_LIBRARY_PATH:-}:" in
        *":\$compat_dir:"*) ;;
        *) export LD_LIBRARY_PATH="\$compat_dir\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}" ;;
    esac
fi
# The launch environment takes priority over the saved defaults.
mesa_default="$mesa_escaped"
turnip_default="$turnip_escaped"
drivers_default="$drivers_escaped"
export MESA_LOADER_DRIVER_OVERRIDE="\${MESA_LOADER_DRIVER_OVERRIDE:-\$mesa_default}"
export LIBVA_DRIVER_NAME="\${LIBVA_DRIVER_NAME:-msm_drm}"
if [ -n "\$turnip_default" ]; then export TU_DEBUG="\${TU_DEBUG-\$turnip_default}"; fi
if [ -n "\$drivers_default" ]; then
    export LIBVA_DRIVERS_PATH="\${LIBVA_DRIVERS_PATH-\$drivers_default}"
fi
# Desktop entries supply their own arguments. Direct calls use the saved defaults.
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
# Combine disabled features because Chrome reads only the last --disable-features.
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
    say "  ✓ Wrapper: $WRAPPER"
    case ":$PATH:" in
        *":$LOCAL_BIN:"*) ;;
        *) say "  Tip: Add $LOCAL_BIN to PATH to invoke the wrapper by name." ;;
    esac
}

do_install() {
    say "Configuring Chrome VA-API..."
    install_wrapper || return 1
    found=0
    for f in $(desktop_files); do
        found=1
        patch_desktop "$f" || return 1
    done
    if [ "$wrapper_only" = 0 ] && [ "$found" = 0 ]; then
        say "  No Chrome desktop entry found"
    fi
    say "ANGLE backend: $angle. Fully quit Chrome before relaunching."
    say "Check VaapiVideoDecoder in chrome://media-internals during playback."
    say "Encoding and the NV12 GBM library: doc/hardware-encoding.md"
}

do_verify() {
    rc=0
    say "Checking desktop entries:"
    found=0
    for f in $(desktop_files); do
        found=1
        # A user desktop entry with the same name overrides the system entry.
        case "$f" in
            /usr/*)
                override="$USER_APPS/$(basename "$f")"
                if [ -f "$override" ]; then
                    if configured "$override"; then
                        say "  ✓ $f has a configured user override: $override"
                    else
                        say "  ✗ $f has a user override that needs configuring: $override"
                        rc=1
                    fi
                    continue
                fi
                ;;
        esac
        # grep -c prints 0 and returns 1 when it finds no matches.
        n=$(grep -c "$MARKER" "$f" 2>/dev/null || true)
        [ -n "$n" ] || n=0
        if configured "$f"; then
            say "  ✓ $f ($n entries)"
        elif [ "$n" -gt 0 ]; then
            say "  ✗ $f has different launch options from this configuration."
            say "    Expected wrapper: $SIG"
            say "    Rerun the installer to update it. Set DMD_ANGLE=auto to let Chrome choose the backend."
            rc=1
        else
            say "  ✗ Not configured: $f"
            rc=1
        fi
    done
    if [ "$wrapper_only" = 0 ] && [ "$found" = 0 ]; then
        say "  No Chrome desktop entry found"
        rc=1
    fi

    say "Checking the wrapper:"
    if [ -x "$WRAPPER" ] && grep -qF -- "$FLAGS" "$WRAPPER" &&
        grep -qFx 'disabled=ResolutionBasedDecoderPriority' "$WRAPPER"; then
        say "  ✓ $WRAPPER"
    else
        say "  ✗ Wrapper missing or out of date: $WRAPPER"
        rc=1
    fi
    say "This checks launch settings. Check the decoder, picture and dropped frames during playback."
    return $rc
}

do_uninstall() {
    say "Restoring Chrome settings..."
    for f in $(desktop_files); do
        if [ -f "$f.dsh-bak" ]; then
            if [ -w "$f" ]; then
                mv "$f.dsh-bak" "$f" && say "  ✓ Restored: $f"
            elif command -v sudo >/dev/null 2>&1; then
                sudo mv "$f.dsh-bak" "$f" && say "  ✓ Restored: $f"
            else
                err "Cannot restore: $f"
            fi
        fi
    done
    if [ -f "$WRAPPER.dsh-bak" ]; then
        mv "$WRAPPER.dsh-bak" "$WRAPPER" && say "  ✓ Restored: $WRAPPER"
    elif [ -f "$WRAPPER.dsh-created" ]; then
        rm -f "$WRAPPER" "$WRAPPER.dsh-created"
        say "  ✓ Removed the generated wrapper: $WRAPPER"
    fi
    say "If you changed chrome://flags separately, check the backend after restarting Chrome."
}

mode=install
wrapper_only=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --verify) mode=verify ;;
        --wrapper-only) wrapper_only=1 ;;
        --uninstall) mode=uninstall ;;
        --gbm-compat-dir)
            [ "$#" -ge 2 ] && [ -n "$2" ] || { err "--gbm-compat-dir requires a directory"; exit 2; }
            GBM_COMPAT_DIR=$2
            shift
            ;;
        -h|--help) usage; exit 0 ;;
        *) err "Unknown argument: $1"; usage; exit 2 ;;
    esac
    shift
done
case "$GBM_COMPAT_DIR" in
    /*) ;;
    *) err "The GBM directory must be an absolute path: $GBM_COMPAT_DIR"; exit 2 ;;
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
    *) err "DMD_ANGLE must be gl, vulkan or auto"; exit 2 ;;
esac
READBACK_FLAG=""
[ "${DMD_DISABLE_GMB_READBACK:-}" = "1" ] &&
    READBACK_FLAG=" --gmb-readback-from-texture-disabled-for-debugging"
FLAGS="--ozone-platform=wayland --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist$ANGLE_FLAGS --enable-features=$FEATURES$READBACK_FLAG"
LOG_ENV=""
[ "${DMD_VA_LOG:-}" = "1" ] && LOG_ENV="DMD_VA_LOG=1"
# Use the wrapper so it can extend the browser process environment at launch.
PREFIX="\"$WRAPPER\" --dmd-exec "
SIG="$PREFIX"
case "$mode" in
    verify) do_verify ;;
    uninstall) do_uninstall ;;
    install) do_install ;;
esac
