#!/usr/bin/env bash
# Check browser VA-API hardware decoding from inside the container.
#
# Run with bash tools/check-browser-vaapi.sh.
# Exit status is 0 when all checks pass, or 1 if any check finds a problem.

set -u
PASS=0; FAIL=0
ok()   { echo "  ✓ $1"; PASS=$((PASS+1)); }
bad()  { echo "  ✗ $1"; FAIL=$((FAIL+1)); }
hint() { echo "    → $1"; }

echo "== [1/5] Device access =="
# Since 0.4.0, the driver opens /dev/video32 directly. It also needs a
# DMA-BUF allocator such as dma_heap or ION.
if [ -r /dev/video32 ] && [ -w /dev/video32 ]; then
    ok "/dev/video32 is readable and writable"
else
    bad "Cannot read and write /dev/video32"
    hint "Use mode 660 with ownership root:<container-visible group> and add the current user to that group"
    hint "Current user groups: $(id -Gn 2>/dev/null)"
fi
HEAP=""
[ -w /dev/dma_heap/system ] && HEAP="/dev/dma_heap/system"
[ -z "$HEAP" ] && [ -w /dev/ion ] && HEAP="/dev/ion"
if [ -n "$HEAP" ]; then
    ok "DMA-BUF allocator: $HEAP"
else
    bad "Neither dma_heap nor ION is writable"
    hint "Check /dev/dma_heap/system on 5.x kernels or /dev/ion on older kernels such as 4.14"
fi
# Query the decoder identity without requiring v4l2-utils.
if [ -r /dev/video32 ] && command -v python3 >/dev/null 2>&1; then
    CARD=$(python3 - <<'PYEOF' 2>/dev/null
import fcntl
b=bytearray(104)
try:
    with open('/dev/video32','rb+',buffering=0) as f:
        fcntl.ioctl(f, 0x80685600, b)
    print(bytes(b[16:48]).split(b'\0')[0].decode())
except Exception:
    pass
PYEOF
)
    if [ "$CARD" = "msm_vidc_vdec" ]; then
        ok "Decoder card: $CARD"
    elif [ -n "$CARD" ]; then
        hint "Expected msm_vidc_vdec at /dev/video32, found $CARD"
    fi
fi

echo "== [2/5] VA-API driver installation =="
if ls /usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so >/dev/null 2>&1 \
   || find /usr/lib -name "msm_drm_drv_video.so" 2>/dev/null | grep -q .; then
    ok "msm_drm_drv_video.so is installed"
else
    bad "msm_drm_drv_video.so was not found"
    hint "See the README for installing the VA-API driver in the dri directory"
fi
# Run a real FFmpeg decode. On the original test platform, vainfo hung
# even when given a nonexistent driver name.
if command -v ffmpeg >/dev/null 2>&1; then
    ok "FFmpeg is available for a hardware decode test"
    hint "Test command: LIBVA_DRIVER_NAME=msm_drm ffmpeg -hwaccel vaapi \\"
    hint "              -hwaccel_output_format vaapi -i test.mp4 -f null -"
    hint "Include -hwaccel_output_format vaapi to avoid silent fallback to software decoding"
else
    hint "Install FFmpeg to test hardware decoding"
fi

echo "== [3/5] Chrome GPU process =="
CHROME_GPU=$(pgrep -f "type=gpu-process" | head -1)
if [ -n "${CHROME_GPU:-}" ]; then
    PLAT=$(tr '\0' '\n' < /proc/$CHROME_GPU/cmdline 2>/dev/null | grep -oE 'ozone-platform=[a-z0-9]+' | head -1)
    if [ "$PLAT" = "ozone-platform=wayland" ]; then
        ok "Chrome is running on Wayland"
    else
        bad "Chrome is not running on Wayland (${PLAT:-platform unspecified})"
        hint "Add --ozone-platform=wayland for the Linux DMA-BUF presentation path"
    fi
    L=$(grep -c libva /proc/$CHROME_GPU/maps 2>/dev/null); L=${L:-0}
    D=$(grep -c drv_video /proc/$CHROME_GPU/maps 2>/dev/null); D=${D:-0}
    if [ "$D" -gt 0 ]; then
        ok "GPU process $CHROME_GPU loaded the driver (libva=$L drv_video=$D)"
    elif [ "$L" -gt 0 ]; then
        bad "The GPU process loaded libva but did not open a VA-API driver"
        hint "Check --render-node-override=/dev/dri/renderD128 and the VA-API feature flags"
    else
        bad "The GPU process has no VA-API library loaded"
        hint "Check --render-node-override=/dev/dri/renderD128 and the VA-API feature flags"
    fi
else
    hint "Skipping Chrome because it is not running"
fi

echo "== [4/5] Firefox RDD process =="
FF_RDD=$(pgrep -f 'rdd$' 2>/dev/null | head -1)
if [ -n "$FF_RDD" ] && [ -d "/proc/$FF_RDD" ]; then
    D=$(grep -c drv_video /proc/$FF_RDD/maps 2>/dev/null); D=$(echo "$D" | head -1); D=${D:-0}
    if [ "$D" -gt 0 ]; then
        ok "RDD process $FF_RDD loaded the driver"
    else
        ok "RDD process $FF_RDD is running. The driver is loaded on demand during playback"
        hint "If the driver stays unloaded during playback, check MOZ_DISABLE_RDD_SANDBOX=1 and the user.js preferences"
    fi
    # RDD releases VA-API resources when idle. The original test went from
    # zero driver mappings and render-node handles to four of each on playback.
    # Check while a video is playing. Current builds handle decoding in the
    # driver, so legacy daemon logs cannot tell whether Firefox is decoding.
else
    hint "Skipping Firefox because it is not running"
fi

echo "== [5/5] Decode test =="
# A real decode can reveal throughput failures even when the driver loads.
# In an older socket-based release, a 224 KB SO_RCVBUF could not hold a
# 1.38 MB frame, although the watchdog handshake reported success.
if command -v ffmpeg >/dev/null 2>&1; then
    T=$(mktemp -d)
    if ffmpeg -hide_banner -loglevel error -f lavfi \
              -i "testsrc2=size=1280x720:rate=30:duration=2" \
              -c:v libx265 -preset ultrafast -y "$T/t.mp4" >/dev/null 2>&1; then
        # Select this driver explicitly. An older system installation can
        # take precedence unless LIBVA_DRIVERS_PATH points to the test build.
        R=$(DMD_VA_LOG=1 LIBVA_DRIVER_NAME=msm_drm \
            ffmpeg -hide_banner -loglevel info \
                   -hwaccel vaapi -hwaccel_output_format vaapi \
                   -c:v hevc -i "$T/t.mp4" -f null - 2>&1)
        FR=$(echo "$R" | grep -oE 'frame= *[0-9]+' | tail -1 | grep -oE '[0-9]+')
        SP=$(echo "$R" | grep -oE 'speed= *[0-9.]+x' | tail -1 | grep -oE '[0-9.]+')
        FR=${FR:-0}
        # The hevc (native) stream mapping names the FFmpeg front end even
        # when VA-API is active. Look for V4L2 evidence in the driver log.
        # The device path also matches the session message in older builds.
        SESS=$(echo "$R" | grep -acE 'session opened|codec=.*(/dev/video32|direct V4L2)|SOURCE_CHANGE')
        if [ "${SESS:-0}" -eq 0 ]; then
            bad "No V4L2 session was detected in the driver log"
            hint "Inspect the output with DMD_VA_LOG=1 LIBVA_DRIVER_NAME=msm_drm ffmpeg ..."
            hint "Use vaapi-driver/tools/probe_device_support.c to investigate failures such as REQBUFS errors"
        else
            ok "The driver reported a V4L2 decoding session"
        fi
        if [ "$FR" -ge 55 ]; then
            SPI=${SP%%.*}; SPI=${SPI:-0}
            if [ "$SPI" -ge 3 ]; then
                ok "Decode completed: ${FR}/60 frames at ${SP}x"
            else
                bad "Decode throughput is low: ${FR}/60 frames at ${SP}x"
                hint "On legacy daemon builds, check for a full input buffer or compare with DMD_ENDPOINT=tcp:20003"
            fi
        else
            bad "Decode stopped at ${FR}/60 frames (speed ${SP:-?}x)"
            hint "Check above for evidence that the V4L2 session started"
            hint "Since 0.4.0, the driver uses V4L2 directly and ignores DMD_ENDPOINT"
        fi
    else
        hint "Skipping generated test because libx265 is unavailable. Test manually with an existing HEVC file"
    fi
    rm -rf "$T"
else
    hint "Skipping the decode throughput test because FFmpeg is unavailable"
fi

echo ""
echo "== Further diagnostics from inside the container =="
cat <<'NOTE'
  Enable driver logging during an actual decode:
    DMD_VA_LOG=1 LIBVA_DRIVER_NAME=msm_drm ffmpeg -hwaccel vaapi \
      -hwaccel_output_format vaapi -i test.mp4 -frames:v 3 -f null -
    → "session opened" indicates successful V4L2 initialization.
    → "SOURCE_CHANGE" indicates the decoder parsed the stream.
    → A failing REQBUFS indicates the device could not initialize the decode path.

  Compare the installed binary with the current build:
    md5sum /usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so
    md5sum vaapi-driver/build/msm_drm_drv_video.so
    If they differ, reinstall the current build or set LIBVA_DRIVERS_PATH to its directory.

  Check whether the device can start a decoding session on the input:
    cc -O2 -o /tmp/pds vaapi-driver/tools/probe_device_support.c && /tmp/pds test.h264
    This probe expects a SOURCE_CHANGE event from the decoder.
NOTE

echo ""
echo "== How to confirm hardware decoding =="
cat <<'NOTE'
  ▸ FFmpeg
      Check the V4L2 output format, decoded frame count, and throughput.
      On legacy daemon builds, a successful handshake only confirms connectivity.
      If the frame count is low, check for a full input buffer.

  ▸ Firefox
      During playback, check for drv_video mappings and renderD handles in the RDD process.
      Example: RDD=$(pgrep -f 'rdd$'); grep -c drv_video /proc/$RDD/maps
      The driver and render-node handles can disappear when playback stops.

  ▸ Chrome
      Check for drv_video in the GPU process maps and ozone-platform=wayland in its command line.
      The Linux DMA-BUF presentation path needs Wayland on the tested setup.

  ▸ Interpreting the checks
      FFmpeg can show "hevc (native)" with hardware acceleration enabled.
      Include -hwaccel_output_format vaapi to detect software fallback.
      A short 720p clip may decode faster in software. Compare CPU usage and power
      at higher resolutions or bitrates over a sustained playback run.

  ▸ Playback still stutters
      If the hardware decode checks pass, inspect frame presentation. The original
      container setup composited each frame through KWin, the anland bridge, and
      the host SurfaceFlinger and HWC. These components together used about 122%
      of one CPU core during the test.
NOTE

echo ""
if [ "$FAIL" -eq 0 ]; then echo "Result: ${PASS} passed"; else echo "Result: ${PASS} passed / ${FAIL} failed"; fi
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
