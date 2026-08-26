#!/usr/bin/env bash
# check-browser-vaapi.sh — 浏览器 VA-API 硬解一键体检（容器内执行）
#
# 用法: bash tools/check-browser-vaapi.sh
# 退出码: 0 全绿 / 1 有异常（逐项给出处置建议）

set -u
PASS=0; FAIL=0
ok()   { echo "  ✓ $1"; PASS=$((PASS+1)); }
bad()  { echo "  ✗ $1"; FAIL=$((FAIL+1)); }
hint() { echo "    → $1"; }

echo "== [1/4] decode-daemon 后端连通性 (TCP 20003) =="
if timeout 2 bash -c 'exec 3<>/dev/tcp/127.0.0.1/20003' 2>/dev/null; then
    ok "TCP 127.0.0.1:20003 可达"
else
    bad "后端不可达"
    hint "宿主侧启动: decode-daemon 20003 (需 ksu 域, droidspacesd 域 TCP accept 被 SELinux 拒)"
fi

echo "== [2/4] VA-API 驱动可见性 =="
if ls /usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so >/dev/null 2>&1 \
   || find /usr/lib -name "msm_drm_drv_video.so" 2>/dev/null | grep -q .; then
    ok "msm_drm_drv_video.so 已部署"
else
    bad "找不到 msm_drm_drv_video.so"
    hint "按 README 部署 vaapi-driver 到 dri 目录"
fi
if command -v vainfo >/dev/null 2>&1; then
    if vainfo 2>/dev/null | grep -qE "VA-API|driver"; then
        ok "vainfo 初始化成功 ($(vainfo 2>/dev/null | grep -c 'VAProfile') profiles)"
    else
        bad "vainfo 初始化失败 (LIBVA_DRIVER_NAME? 端点探测?)"
        hint "试: LIBVA_DRIVER_NAME=msm vainfo; 或确认驱动回落到 tcp:20003"
    fi
else
    hint "(未装 vainfo, 跳过 — 非必需)"
fi

echo "== [3/4] Chrome GPU 进程 =="
CHROME_GPU=$(pgrep -f "type=gpu-process" | head -1)
if [ -n "${CHROME_GPU:-}" ]; then
    L=$(grep -c libva /proc/$CHROME_GPU/maps 2>/dev/null); L=${L:-0}
    D=$(grep -c drv_video /proc/$CHROME_GPU/maps 2>/dev/null); D=${D:-0}
    if [ "$D" -gt 0 ]; then
        ok "GPU 进程($CHROME_GPU)已加载驱动栈 (libva=$L drv_video=$D)"
    elif [ "$L" -gt 0 ]; then
        bad "libva 已加载但驱动未打开 — 解码器创建即失败"
        hint "X11 模式典型症状(0 NALU)。必须 --ozone-platform=wayland"
    else
        bad "GPU 进程无任何 VA-API 痕迹 — vaInitialize 从未发生"
        hint "确认 --render-node-override=/dev/dri/renderD128 与三个 enable-features"
    fi
else
    hint "(Chrome 未运行, 跳过)"
fi

echo "== [4/4] Firefox RDD 进程 =="
FF_RDD=$(pgrep -f 'rdd$' 2>/dev/null | head -1)
if [ -n "$FF_RDD" ] && [ -d "/proc/$FF_RDD" ]; then
    D=$(grep -c drv_video /proc/$FF_RDD/maps 2>/dev/null); D=$(echo "$D" | head -1); D=${D:-0}
    if [ "$D" -gt 0 ]; then
        ok "RDD($FF_RDD)已加载驱动栈"
    else
        ok "RDD($FF_RDD)存活 (驱动未加载 — 播放视频后才按需初始化, 非故障)"
        hint "若播放时仍不加载: MOZ_DISABLE_RDD_SANDBOX=1 启动 + user.js 四件套"
    fi
else
    hint "(Firefox 未运行, 跳过)"
fi

echo ""
echo "== 宿主侧人工复核(容器内看不到) =="
cat <<'NOTE'
  adb shell 'su -c "tail -20 /data/local/Droidspaces/Logs/decode-daemon-tcp.log"'
  → 找「输出格式 WxH stride=...」行: 出现即真实解码; 只有握手无流量 = 上层没喂数据
NOTE

echo ""
if [ "$FAIL" -eq 0 ]; then echo "结果: ${PASS} 项全绿"; else echo "结果: ${PASS} 绿 / ${FAIL} 异常"; fi
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
