#!/usr/bin/env bash
# 交叉编译 decode-daemon（Android aarch64）
#
# 用法:
#   ./build.sh                    # 使用 $NDK 或自动探测
#   NDK=/path/to/ndk ./build.sh
#   API=29 ./build.sh             # 覆盖目标 API level
#
# 产物: build/decode-daemon (ELF aarch64, 动态链接 /system/bin/linker64)

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
API="${API:-29}"
TARGET="${TARGET:-aarch64-linux-android}"
OUT_DIR="${OUT_DIR:-$REPO_DIR/build}"

# --- 定位 NDK ---------------------------------------------------------------
if [ -z "${NDK:-}" ]; then
    for candidate in \
        "$HOME/android-ndk" \
        "$HOME/Android/Sdk/ndk-bundle" \
        /opt/android-ndk \
        /usr/lib/android-ndk
    do
        if [ -d "$candidate/toolchains/llvm/prebuilt" ]; then
            NDK="$candidate"
            break
        fi
    done
    # SDK 风格的多版本目录: ~/Android/Sdk/ndk/<version>/
    if [ -z "${NDK:-}" ] && [ -d "$HOME/Android/Sdk/ndk" ]; then
        NDK="$(find "$HOME/Android/Sdk/ndk" -maxdepth 1 -mindepth 1 -type d | sort -V | tail -1)"
    fi
fi

if [ -z "${NDK:-}" ] || [ ! -d "$NDK" ]; then
    echo "错误: 未找到 Android NDK。请设置 NDK 环境变量指向 NDK 根目录。" >&2
    echo "      下载: https://developer.android.com/ndk/downloads" >&2
    exit 1
fi

# --- 定位 toolchain --------------------------------------------------------
HOST_TAG="linux-x86_64"
case "$(uname -s)" in
    Darwin) HOST_TAG="darwin-x86_64" ;;
esac

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
CC="$TOOLCHAIN/bin/${TARGET}${API}-clang"

if [ ! -x "$CC" ]; then
    echo "错误: 找不到编译器 $CC" >&2
    echo "      NDK=$NDK  API=$API" >&2
    echo "      可用的 target: " >&2
    ls "$TOOLCHAIN/bin/" 2>/dev/null | grep -E 'aarch64.*clang$' | sed 's/^/        /' >&2 || true
    exit 1
fi

NDK_VERSION="$(sed -n 's/^Pkg.Revision *= *//p' "$NDK/source.properties" 2>/dev/null || echo unknown)"

echo "NDK      : $NDK (r${NDK_VERSION})"
echo "编译器   : $(basename "$CC")"
echo "目标     : ${TARGET}${API}"

# --- 编译 ------------------------------------------------------------------
mkdir -p "$OUT_DIR"

# -lmediandk 同时提供 AMediaCodec_* 与 AMediaFormat_* 符号。
# 注意: README 早期版本写的 -lmEDIAndk 是笔误，该库不存在。
"$CC" \
    -O2 -Wall -Wextra \
    -o "$OUT_DIR/decode-daemon" \
    "$REPO_DIR/src/decode-daemon.c" \
    -lmediandk -llog -landroid

echo "产物     : $OUT_DIR/decode-daemon ($(stat -c%s "$OUT_DIR/decode-daemon" 2>/dev/null || stat -f%z "$OUT_DIR/decode-daemon") 字节)"
echo
echo "推送并手动运行（测试阶段，无需安装 KSU/Magisk 模块）:"
echo "  adb push $OUT_DIR/decode-daemon /data/local/tmp/decode-daemon"
echo "  adb shell \"su -c 'chmod 755 /data/local/tmp/decode-daemon'\""
echo "  adb shell \"su -c 'nohup /data/local/tmp/decode-daemon 20003 >/data/local/tmp/decode-daemon.log 2>&1 &'\""
