#!/bin/bash
# A/B regression test for H.264 and HEVC frame delivery in Chrome.
#
# Select a clip and software-decoded reference signatures with SRC and SIG.
#   SRC=h264_slow.mp4  SIG=h264_sig.json   (default)
#   SRC=order_slow.mp4 SIG=hevc_sig.json   (HEVC)
#
# An I/IDR slice has no num_ref_idx_l0/l1_active_minus1 fields. VA-API
# reported zero, which the old driver used as the initial PPS default even
# though the stream required l0=2. FFmpeg queued enough frames for a correct
# PPS to arrive before the decoder needed it. Chrome submitted one frame at
# a time and exposed the first incorrect PPS. Compare canvas signatures to
# software-decoded pixels to identify the frame that was actually displayed.
#
# H.264, three rounds of 20 samples:
#   Old (first PPS 9 bytes, l0=0): 5/4/5 matched, 13/13/13 unrecognized.
#   New (first PPS 10 bytes, l0=2): 20/20/20 matched, zero mismatches.
# HEVC, two rounds of 20 samples:
#   Old: 3/4 matched, 13/11 unrecognized.
#   New: 20/20 matched, zero mismatches.
#
# HEVC submits its parameter sets once per session. The improvement came
# from receiving frames before EndPicture returned. With SPS reorder set
# back to zero, that change alone still gave 20 matches out of 20, with no
# frames received by the background thread. See the comments in decode.c.
#
# Usage: verify_pps_fix.sh <old build directory> <new build directory> [rounds]
# To build the old driver in a separate worktree:
#   git worktree add --detach /tmp/oldwt 9f00b1a6
#   cd /tmp/oldwt/vaapi-driver && make VA_CFLAGS=... DRM_CFLAGS=...

set -u
OLD="${1:?old build directory}"
NEW="${2:?new build directory}"
ROUNDS="${3:-1}"
CR=/tmp/cr
PORT_BASE=8971
SRC="${SRC:-h264_slow.mp4}"
SIG="${SIG:-h264_sig.json}"

for d in "$OLD" "$NEW"; do
    [ -f "$d/msm_drm_drv_video.so" ] || { echo "No driver found: $d"; exit 2; }
done
[ -f "$CR/order_pixel.html" ] || { echo "Not available $CR/order_pixel.html"; exit 2; }
[ -f "$CR/$SIG" ] || { echo "Missing base signature table $CR/$SIG"; exit 2; }
[ -f "$CR/$SRC" ] || { echo "Shortage of material $CR/$SRC"; exit 2; }
echo "Clip: $SRC  Reference signatures: $SIG"

echo "old version: $(strings "$OLD/msm_drm_drv_video.so" | grep -m1 'DroidSpaces V4L2')"
echo "new version: $(strings "$NEW/msm_drm_drv_video.so" | grep -m1 'DroidSpaces V4L2')"
echo

mkdir -p "$CR/rev"

run_one() {
    # Chrome 151 ignores --disable-vulkan. On the original Snapdragon 8
    # Elite test system, disabling Vulkan in chrome://flags caused display
    # corruption. See doc/browser-vaapi-guide.md section 2.5 for the nabu case.
    local mode="$1" port="$2" drv="$3"
    rm -f "$CR/rev/$mode.json"
    OUT="$CR/rev/$mode.json" ROOT="$CR" WANT_REPORTS=1 \
        python3 "$CR/auto/report.py" "$port" >/dev/null 2>&1 &
    local srv=$!
    sleep 1
    rm -rf "$CR/rev/p_$mode"
    env XDG_RUNTIME_DIR="/run/user/$(id -u)" WAYLAND_DISPLAY=wayland-0 \
        LIBVA_DRIVER_NAME=msm_drm LIBVA_DRIVERS_PATH="$drv" DMD_VA_LOG=1 \
        nohup /usr/bin/google-chrome --ozone-platform=wayland \
        --render-node-override=/dev/dri/renderD128 --ignore-gpu-blocklist \
        --enable-features=VaapiVideoDecodeLinux,VaapiVideoDecoder,VaapiIgnoreDriverChecks \
        --user-data-dir="$CR/rev/p_$mode" --no-first-run --no-default-browser-check \
        --autoplay-policy=no-user-gesture-required \
        "http://127.0.0.1:$port/order_pixel.html?mode=$mode&src=$SRC" \
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
    echo "--- Round $r ---"
    run_one "old$r" $((PORT_BASE + r * 4))     "$OLD"
    run_one "new$r" $((PORT_BASE + r * 4 + 2)) "$NEW"
done

SIG="$SIG" python3 - "$ROUNDS" <<'PY'
import json, os, sys
CR = '/tmp/cr'
base = json.load(open(f"{CR}/{os.environ.get('SIG','h264_sig.json')}"))['sigs']
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
print('Round  Old (match/mismatch/unknown)  New (match/mismatch/unknown)')
bad_new = 0
for r in range(1, rounds + 1):
    o = evaluate(f'{CR}/rev/old{r}.json')
    n = evaluate(f'{CR}/rev/new{r}.json')
    fo = f'{o[1]}/{o[2]}/{o[3]} ({o[0]} samples)' if o else 'No result'
    fn = f'{n[1]}/{n[2]}/{n[3]} ({n[0]} samples)' if n else 'No result'
    print(f'  {r}   {fo:28s}  {fn}')
    if n and (n[2] or n[3]):
        bad_new += 1

print()
if bad_new:
    print(f'✗ {bad_new} round(s) with the new driver had mismatches or unrecognized frames')
else:
    print('✓ All frames matched with the new driver. The old driver is expected to match about 4–5 of 20, with about 13 unrecognized.')
PY

for r in $(seq 1 "$ROUNDS"); do
    for t in old new; do
        # Match fields shared by both the current and historical driver logs.
        f="$CR/rev/c_$t$r.log"
        [ -f "$f" ] && printf '%-8s First PPS=%s match=%s\n' "$t$r" \
            "$(grep -m1 -o 'PPS [0-9][0-9]*' "$f")" \
            "$(grep -c ' -> surface .* (unit .* seq .* POC ' "$f")"
    done
done
