#!/bin/bash
# H.264 首份 PPS 修复的 A-B 反证。
#
# ── 这个脚本存在的理由 ────────────────────────────────────────
# 被修的 bug 是：I/IDR slice 没有 num_ref_idx_l0/l1_active_minus1 语法元素，
# VA-API 对它恒报 0，而会话第一帧必然是 IDR，于是驱动把 0 当成 PPS 默认值
# 的真值写出去（真实值为 l0=2）。
#
# **这个 bug 在 ffmpeg 路径上完全测不出来。** 实测：旧版驱动跑
# h264_slow.mp4，前 8 帧与软解逐字节一致，md5 PASS，加 DMD_NO_MAP_WAIT
# 也一样 PASS。
#
# 原因是提交节奏。ffmpeg 一次灌 6 帧以上（日志里 pend 涨到 6），驱动在
# unit 1 送出错的 PPS、unit 3 就送出了对的，硬件真正开始解码时正确的
# PPS 已经到了，bug 被掩盖。Chrome 是逐帧提交，第一帧解码时只有那份错的。
#
# 所以验证这个修复**必须用浏览器**，判据是像素而不是帧号单调性：
# 抓 canvas 像素算签名，与软解基线比对，得出"画面里实际是第几帧"。
#
# ── 实测结果（三轮，完全可重复）────────────────────────────
#   旧版驱动（首份 PPS 9 字节 l0=0）：一致 5/4/5，无法识别 13/13/13（共 20）
#   新版驱动（首份 PPS 10 字节 l0=2）：一致 20/20/20，不符 0
#
# 用法：verify_pps_fix.sh <旧版build目录> <新版build目录> [轮数]
# 旧版可用 git worktree 构建：
#   git worktree add --detach /tmp/oldwt 9f00b1a6
#   cd /tmp/oldwt/vaapi-driver && make VA_CFLAGS=... DRM_CFLAGS=...

set -u
OLD="${1:?旧版 build 目录}"
NEW="${2:?新版 build 目录}"
ROUNDS="${3:-1}"
CR=/tmp/cr
PORT_BASE=8971

for d in "$OLD" "$NEW"; do
    [ -f "$d/msm_drm_drv_video.so" ] || { echo "找不到驱动: $d"; exit 2; }
done
[ -f "$CR/order_pixel.html" ] || { echo "缺 $CR/order_pixel.html"; exit 2; }
[ -f "$CR/h264_sig.json" ]    || { echo "缺基准签名表 $CR/h264_sig.json"; exit 2; }
[ -f "$CR/h264_slow.mp4" ]    || { echo "缺素材 $CR/h264_slow.mp4"; exit 2; }

echo "旧版: $(strings "$OLD/msm_drm_drv_video.so" | grep -m1 'DroidSpaces V4L2')"
echo "新版: $(strings "$NEW/msm_drm_drv_video.so" | grep -m1 'DroidSpaces V4L2')"
echo

mkdir -p "$CR/rev"

run_one() {
    local mode="$1" port="$2" drv="$3"
    rm -f "$CR/rev/$mode.json"
    OUT="$CR/rev/$mode.json" ROOT="$CR" WANT_REPORTS=1 \
        python3 "$CR/auto/report.py" "$port" >/dev/null 2>&1 &
    local srv=$!
    sleep 1
    rm -rf "$CR/rev/p_$mode"
    env XDG_RUNTIME_DIR="/run/user/$(id -u)" WAYLAND_DISPLAY=wayland-0 \
        LIBVA_DRIVER_NAME=msm_drm LIBVA_DRIVERS_PATH="$drv" DMD_VA_LOG=1 \
        nohup /usr/bin/google-chrome --ozone-platform=wayland --disable-vulkan \
        --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist \
        --enable-features=VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiIgnoreDriverChecks \
        --user-data-dir="$CR/rev/p_$mode" --no-first-run --no-default-browser-check \
        --autoplay-policy=no-user-gesture-required \
        "http://127.0.0.1:$port/order_pixel.html?mode=$mode&src=h264_slow.mp4" \
        >"$CR/rev/c_$mode.log" 2>&1 &
    local i
    for i in $(seq 1 28); do
        [ -s "$CR/rev/$mode.json" ] && break
        sleep 2
    done
    pkill -9 -x google-chrome 2>/dev/null
    kill $srv 2>/dev/null
    sleep 2
}

pkill -9 -x google-chrome 2>/dev/null
sleep 1

for r in $(seq 1 "$ROUNDS"); do
    echo "--- 第 $r 轮 ---"
    run_one "old$r" $((PORT_BASE + r * 4))     "$OLD"
    run_one "new$r" $((PORT_BASE + r * 4 + 2)) "$NEW"
done

python3 - "$ROUNDS" <<'PY'
import json, os, sys
CR = '/tmp/cr'
base = json.load(open(f'{CR}/h264_sig.json'))['sigs']
def dist(a, b): return sum((x - y) ** 2 for x, y in zip(a, b))

def evaluate(path):
    if not os.path.exists(path) or not os.path.getsize(path):
        return None
    r = json.load(open(path))[0]
    ok = mism = unk = 0
    for s in r['samples']:
        mt = s['mtIdx']
        best = min(range(len(base)), key=lambda i: dist(s['sig'], base[i]))
        if dist(s['sig'], base[best]) > 300000:
            unk += 1
        elif best == mt:
            ok += 1
        else:
            mism += 1
    return len(r['samples']), ok, mism, unk

rounds = int(sys.argv[1])
print()
print('轮次  旧版 一致/不符/无法识别        新版 一致/不符/无法识别')
bad_new = 0
for r in range(1, rounds + 1):
    o = evaluate(f'{CR}/rev/old{r}.json')
    n = evaluate(f'{CR}/rev/new{r}.json')
    fo = f'{o[1]}/{o[2]}/{o[3]} (采{o[0]})' if o else '无结果'
    fn = f'{n[1]}/{n[2]}/{n[3]} (采{n[0]})' if n else '无结果'
    print(f'  {r}   {fo:28s}  {fn}')
    if n and (n[2] or n[3]):
        bad_new += 1

print()
if bad_new:
    print(f'✗ 新版有 {bad_new} 轮出现不符或无法识别 —— 修复未达预期')
else:
    print('✓ 新版全部一致；旧版应显著更差（预期一致约 4~5/20、无法识别约 13/20）')
PY

for r in $(seq 1 "$ROUNDS"); do
    for t in old new; do
        f="$CR/rev/c_$t$r.log"
        [ -f "$f" ] && printf '%-8s 首份PPS=%s 配对=%s\n' "$t$r" \
            "$(grep -m1 -o '已送 PPS [0-9]* 字节' "$f")" \
            "$(grep -c '配对: 帧' "$f")"
    done
done
