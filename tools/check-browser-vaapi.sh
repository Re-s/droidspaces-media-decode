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

echo "== [1/5] decode-daemon 后端连通性 =="
# 驱动的探测顺序是 Unix socket 优先、TCP 兜底，所以两条都要看：
# 只检 TCP 会在 socket 模式下误报"后端不可达"（实测踩过）。
SOCK=/run/dmd/decode.sock
EP=""
if [ -S "$SOCK" ]; then
    ok "Unix socket 可用: $SOCK (inode $(stat -c %i "$SOCK" 2>/dev/null))"
    EP="unix"
else
    hint "Unix socket 不存在: $SOCK (平台未挂载 /run/dmd?)"
fi
if timeout 2 bash -c 'exec 3<>/dev/tcp/127.0.0.1/20003' 2>/dev/null; then
    ok "TCP 127.0.0.1:20003 可达"
    [ -z "$EP" ] && EP="tcp"
else
    hint "TCP 20003 不可达 (socket 模式下属正常)"
fi
if [ -z "$EP" ]; then
    bad "两种端点都不可用 — 解码必然失败"
    hint "宿主侧启动 socket 模式: decode-daemon --sock /data/local/Droidspaces/dmd/run"
    hint "或 TCP 模式: decode-daemon 20003 (需 ksu 域)"
else
    ok "驱动将使用: $([ "$EP" = unix ] && echo 'Unix socket (优先)' || echo 'TCP (socket 不可用时的兜底)')"
fi

echo "== [2/5] VA-API 驱动可见性 =="
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

echo "== [3/5] Chrome GPU 进程 =="
CHROME_GPU=$(pgrep -f "type=gpu-process" | head -1)
if [ -n "${CHROME_GPU:-}" ]; then
    PLAT=$(tr '\0' '\n' < /proc/$CHROME_GPU/cmdline 2>/dev/null | grep -oE 'ozone-platform=[a-z0-9]+' | head -1)
    if [ "$PLAT" = "ozone-platform=wayland" ]; then
        ok "运行于 Wayland 平台"
    else
        bad "未以 Wayland 平台运行 (${PLAT:-未指定}) — 硬解不可用"
        hint "加 --ozone-platform=wayland: 解码帧经 linux-dmabuf 提交,X11 下走不通"
    fi
    L=$(grep -c libva /proc/$CHROME_GPU/maps 2>/dev/null); L=${L:-0}
    D=$(grep -c drv_video /proc/$CHROME_GPU/maps 2>/dev/null); D=${D:-0}
    if [ "$D" -gt 0 ]; then
        ok "GPU 进程($CHROME_GPU)已加载驱动栈 (libva=$L drv_video=$D)"
    elif [ "$L" -gt 0 ]; then
        bad "libva 已加载但驱动未打开 — 解码器创建即失败"
        hint "确认 --render-node-override=/dev/dri/renderD128 与三个 enable-features"
    else
        bad "GPU 进程无任何 VA-API 痕迹 — vaInitialize 从未发生"
        hint "确认 --render-node-override=/dev/dri/renderD128 与三个 enable-features"
    fi
else
    hint "(Chrome 未运行, 跳过)"
fi

echo "== [4/5] Firefox RDD 进程 =="
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

echo "== [5/5] 实测解码能力 =="
# 这是唯一能发现**吞吐类故障**的检查。
# 端点连通性、驱动加载、进程存活全部正常，解码仍可能只跑到 0.9x 就中断
# （历史案例：Unix socket 默认 SO_RCVBUF 224KB < 单帧 1.38MB）。
# 而 daemon 侧的探活只验证"握手成功"，这种故障它照样报健康。
if command -v ffmpeg >/dev/null 2>&1; then
    T=$(mktemp -d)
    if ffmpeg -hide_banner -loglevel error -f lavfi \
              -i "testsrc2=size=1280x720:rate=30:duration=2" \
              -c:v libx265 -preset ultrafast -y "$T/t.mp4" >/dev/null 2>&1; then
        # 必须带 -hwaccel_output_format vaapi：只写 -hwaccel vaapi 时
        # 拿不到硬解会静默回落软解且不报错，测什么都"正常"。
        R=$(ffmpeg -hide_banner -loglevel info \
                   -hwaccel vaapi -hwaccel_output_format vaapi \
                   -hwaccel_device /dev/dri/renderD128 \
                   -c:v hevc -i "$T/t.mp4" -f null - 2>&1)
        FR=$(echo "$R" | grep -oE 'frame= *[0-9]+' | tail -1 | grep -oE '[0-9]+')
        SP=$(echo "$R" | grep -oE 'speed= *[0-9.]+x' | tail -1 | grep -oE '[0-9.]+')
        FR=${FR:-0}
        # ⚠️ 不要用日志里的 "hevc (native)" 判断软解 —— 那指的是 ffmpeg 以
        # 内置 hevc 解码器为前端，-hwaccel vaapi 是挂在它下面的加速后端，
        # 所以硬解生效时这行照样出现（我照它判过，误报了一次）。
        # 可靠判据只有帧数与速度，以及 daemon 侧是否记下真实会话。
        if [ "$FR" -ge 55 ]; then
            SPI=${SP%%.*}; SPI=${SPI:-0}
            if [ "$SPI" -ge 3 ]; then
                ok "硬解正常: ${FR}/60 帧, 速度 ${SP}x"
            else
                bad "硬解偏慢: ${FR}/60 帧但仅 ${SP}x — 疑吞吐瓶颈"
                hint "查 daemon 日志是否有「输入缓冲暂满」; 对比 DMD_ENDPOINT=tcp:20003"
            fi
        else
            bad "解码中断: 仅 ${FR}/60 帧 (速度 ${SP:-?}x)"
            hint "典型吞吐故障。查 daemon 日志「输入缓冲暂满，重试」"
            hint "临时绕过: DMD_ENDPOINT=tcp:20003 换通道对比"
        fi
    else
        hint "(无 libx265 编码器, 跳过 — 可手工用现成 HEVC 文件测)"
    fi
    rm -rf "$T"
else
    hint "(未装 ffmpeg, 跳过 — 强烈建议装上, 这是唯一能测出吞吐故障的检查)"
fi

echo ""
echo "== 宿主侧人工复核(容器内看不到) =="
cat <<'NOTE'
  adb shell 'su -c "tail -20 /data/local/Droidspaces/dmd/logs/decode-daemon.log"'
  → 「输出格式 WxH stride=...」出现即真实解码; 只有握手无流量 = 上层没喂数据
  → 「帧回传=SHM」= memfd 零拷贝生效; 「帧回传=内联」= 每帧经 socket 拷贝
  → 「输入缓冲暂满，重试」= 吞吐瓶颈, 回传方向堵住导致输入槽位耗尽
NOTE

echo ""
if [ "$FAIL" -eq 0 ]; then echo "结果: ${PASS} 项全绿"; else echo "结果: ${PASS} 绿 / ${FAIL} 异常"; fi
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
