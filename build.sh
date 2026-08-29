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

# ARM64 主机的回退路径：Google 只发布 x86_64 主机版 NDK，NDK 自带的 clang
# 在 aarch64 上根本跑不起来（表现为 libz.so.1 缺失，实质是架构不符）。
# 但 NDK 的 sysroot 是纯数据（头文件 + aarch64-android 库），可以配合
# 系统自带的 clang 使用。链接需分两步：clang 驱动会去找它自己版本的
# libclang_rt.builtins（系统 clang 19 vs NDK clang 17，路径不匹配且无法
# 用 -rtlib 覆盖），所以只让 clang 编译，再用 GNU ld 手动链接。
USE_HOST_CLANG=0
if [ ! -x "$CC" ] || ! "$CC" --version >/dev/null 2>&1; then
    if command -v clang >/dev/null 2>&1 \
       && [ -d "$TOOLCHAIN/sysroot/usr/lib/${TARGET}/${API}" ]; then
        USE_HOST_CLANG=1
    else
        echo "错误: 找不到可用的编译器" >&2
        echo "      NDK 的 $CC 无法执行（主机架构 $(uname -m)，NDK 仅提供 x86_64 版）" >&2
        echo "      且系统未安装 clang 或 NDK sysroot 不完整" >&2
        exit 1
    fi
fi

NDK_VERSION="$(sed -n 's/^Pkg.Revision *= *//p' "$NDK/source.properties" 2>/dev/null || echo unknown)"

echo "NDK      : $NDK (r${NDK_VERSION})"
echo "编译器   : $(basename "$CC")"
echo "目标     : ${TARGET}${API}"

# --- 编译 ------------------------------------------------------------------
mkdir -p "$OUT_DIR"

# -lmediandk 同时提供 AMediaCodec_* 与 AMediaFormat_* 符号。
# 注意: README 早期版本写的 -lmEDIAndk 是笔误，该库不存在。
if [ "$USE_HOST_CLANG" = 0 ]; then
    "$CC" \
        -O2 -Wall -Wextra \
        -o "$OUT_DIR/decode-daemon" \
        "$REPO_DIR/src/decode-daemon.c" \
        "$REPO_DIR/src/v4l2_backend.c" \
        -lmediandk -llog -landroid
else
    SYSROOT="$TOOLCHAIN/sysroot"
    LIBDIR="$SYSROOT/usr/lib/${TARGET}/${API}"
    RTDIR="$(dirname "$(find "$TOOLCHAIN/lib" -name 'libclang_rt.builtins-aarch64-android.a' | head -1)")"
    LD="${LD:-/usr/bin/aarch64-linux-gnu-ld}"
    if [ ! -x "$LD" ]; then
        echo "错误: 需要 aarch64 GNU 链接器，未找到 $LD" >&2
        echo "      可设 LD=/path/to/ld 覆盖" >&2
        exit 1
    fi
    clang --target="${TARGET}${API}" --sysroot="$SYSROOT" \
        -c -O2 -Wall -Wextra -DNDEBUG -fPIE \
        -o "$OUT_DIR/decode-daemon.o" "$REPO_DIR/src/decode-daemon.c"
    clang --target="${TARGET}${API}" --sysroot="$SYSROOT" \
        -c -O2 -Wall -Wextra -DNDEBUG -fPIE \
        -o "$OUT_DIR/v4l2_backend.o" "$REPO_DIR/src/v4l2_backend.c"
    "$LD" -pie --sysroot="$SYSROOT" -L"$LIBDIR" \
        -dynamic-linker /system/bin/linker64 \
        -o "$OUT_DIR/decode-daemon" \
        "$LIBDIR/crtbegin_dynamic.o" "$OUT_DIR/decode-daemon.o" \
        "$OUT_DIR/v4l2_backend.o" \
        -lmediandk -llog -landroid -lc -lm -ldl \
        "$RTDIR/libclang_rt.builtins-aarch64-android.a" \
        "$LIBDIR/crtend_android.o"
    rm -f "$OUT_DIR/decode-daemon.o"
fi

echo "产物     : $OUT_DIR/decode-daemon ($(stat -c%s "$OUT_DIR/decode-daemon" 2>/dev/null || stat -f%z "$OUT_DIR/decode-daemon") 字节)"
echo
echo "推送并手动运行（测试阶段，无需安装 KSU/Magisk 模块）:"
echo "  adb push $OUT_DIR/decode-daemon /data/local/tmp/decode-daemon"
echo "  adb shell \"su -c 'chmod 755 /data/local/tmp/decode-daemon'\""
echo "  adb shell \"su -c 'nohup /data/local/tmp/decode-daemon 20003 >/data/local/tmp/decode-daemon.log 2>&1 &'\""
