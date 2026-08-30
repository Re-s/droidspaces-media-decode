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

echo "== [1/5] 设备节点可用性 =="
# 0.4.0 起驱动直接打开 /dev/video32，没有 daemon 也没有端点探测。
# 需要：解码器节点 + 一个 DMABUF 来源（dma_heap 或 ion，二者任一）。
if [ -r /dev/video32 ] && [ -w /dev/video32 ]; then
    ok "/dev/video32 可读写"
else
    bad "/dev/video32 不可读写"
    hint "需要 root:<容器可见组> 660，并把当前用户加入该组"
    hint "当前用户组: $(id -Gn 2>/dev/null)"
fi
HEAP=""
[ -w /dev/dma_heap/system ] && HEAP="/dev/dma_heap/system"
[ -z "$HEAP" ] && [ -w /dev/ion ] && HEAP="/dev/ion"
if [ -n "$HEAP" ]; then
    ok "DMABUF 来源: $HEAP"
else
    bad "无可用 DMABUF 来源（dma_heap 与 ion 都不可写）"
    hint "内核 5.x 用 /dev/dma_heap/system，4.14 一类用 /dev/ion"
fi
# 解码器身份（不依赖 v4l2-utils）
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
        ok "解码器身份正确: card=$CARD"
    elif [ -n "$CARD" ]; then
        hint "card=$CARD（期望 msm_vidc_vdec，可能探到了别的节点）"
    fi
fi

echo "== [2/5] VA-API 驱动可见性 =="
if ls /usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so >/dev/null 2>&1 \
   || find /usr/lib -name "msm_drm_drv_video.so" 2>/dev/null | grep -q .; then
    ok "msm_drm_drv_video.so 已部署"
else
    bad "找不到 msm_drm_drv_video.so"
    hint "按 README 部署 vaapi-driver 到 dri 目录"
fi
# ⚠️ 不用 vainfo 检查 —— 它在本平台会挂住，即使给不存在的驱动名也一样。
# 改为用 ffmpeg 实际解一帧，这也是唯一能证明"真的在硬解"的判据。
if command -v ffmpeg >/dev/null 2>&1; then
    ok "ffmpeg 存在（用它验证硬解，勿用 vainfo）"
    hint "验证命令: LIBVA_DRIVER_NAME=msm_drm ffmpeg -hwaccel vaapi \\"
    hint "           -hwaccel_output_format vaapi -i test.mp4 -f null -"
    hint "必须带 -hwaccel_output_format vaapi，否则会静默回落软解"
else
    hint "没装 ffmpeg，无法验证硬解是否真的生效"
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
        hint "若播放时仍不加载: MOZ_DISABLE_RDD_SANDBOX=1 启动 + user.js 五件套"
    fi
    # ⚠️ 两个极易误判之处（都实测踩过）：
    #
    # 1) **必须在播放时检查**。RDD 空闲时会释放 VA-API 资源，此时
    #    drv_video 映射数与 renderD 句柄数都是 0、线程只剩 5 个 ——
    #    看起来像"从没做过硬解"，其实一开播立刻变成 va=4 dri=4 thr=19。
    #    判"Firefox 没用硬解"之前，先确认视频正在播放。
    #
    # 2) **Firefox 硬解不经 daemon**。它由驱动直接对接 MediaCodec，
    #    surface 走 msm_drm dumb buffer 导出 dmabuf，所以播放期间
    #    daemon 日志里**不会**出现它的会话 —— 那是正常的，不是故障。
    #    daemon 日志里持续刷的 640x480 空会话是 watchdog 探针（每 5s 一次）。
    #    因此"daemon 无会话"不能用来判断 Firefox 硬解失败。
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
        # ⚠️ 必须显式指定 LIBVA_DRIVER_NAME 与打开 DMD_VA_LOG：
        # 不指定驱动名时 libva 可能加载**系统目录里的旧版** .so，
        # 测出来的结果与当前构建无关（实测踩过：/usr/lib 下那个比
        # build/ 里的旧，日志里还带着早已删除的"退回 TCP"字样）。
        R=$(DMD_VA_LOG=1 LIBVA_DRIVER_NAME=msm_drm \
            ffmpeg -hide_banner -loglevel info \
                   -hwaccel vaapi -hwaccel_output_format vaapi \
                   -c:v hevc -i "$T/t.mp4" -f null - 2>&1)
        FR=$(echo "$R" | grep -oE 'frame= *[0-9]+' | tail -1 | grep -oE '[0-9]+')
        SP=$(echo "$R" | grep -oE 'speed= *[0-9.]+x' | tail -1 | grep -oE '[0-9.]+')
        FR=${FR:-0}
        # ⚠️ 不要用日志里的 "hevc (native)" 判断软解 —— 那指的是 ffmpeg 以
        # 内置 hevc 解码器为前端，-hwaccel vaapi 是挂在它下面的加速后端，
        # 所以硬解生效时这行照样出现（我照它判过，误报了一次）。
        # ⚠️ 帧数与速度**也不足以**判断硬解：软解同样能跑满帧数且速度不低
        # （实测本机 52/60 帧、3.57x，全是软解 —— V4L2 会话根本没建起来）。
        # 唯一可靠的判据是驱动日志里有没有真正建立会话：
        #   "会话已建立"  → V4L2 协商成功
        #   "SOURCE_CHANGE" → 固件真的在解析码流
        # 两者缺一就是没在硬解，无论帧数多好看。
        SESS=$(echo "$R" | grep -acE '会话已建立|SOURCE_CHANGE')
        if [ "${SESS:-0}" -eq 0 ]; then
            bad "V4L2 会话未建立 — 帧数再好看也是软解"
            hint "看日志: DMD_VA_LOG=1 LIBVA_DRIVER_NAME=msm_drm ffmpeg ..."
            hint "若日志有「REQBUFS 失败」等，说明该设备解码路径起不来；"
            hint "用 vaapi-driver/tools/probe_device_support.c 确认该设备是否可用"
        else
            ok "V4L2 会话已建立（真实硬解）"
        fi
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
            hint "先看上面 V4L2 会话是否建立；未建立即为软解"
            hint "0.4.0 无端点可换；DMD_ENDPOINT 已无读者，设了不起作用"
        fi
    else
        hint "(无 libx265 编码器, 跳过 — 可手工用现成 HEVC 文件测)"
    fi
    rm -rf "$T"
else
    hint "(未装 ffmpeg, 跳过 — 强烈建议装上, 这是唯一能测出吞吐故障的检查)"
fi

echo ""
echo "== 补充排查（0.4.0 全部在容器内完成，无需宿主侧）=="
cat <<'NOTE'
  驱动日志（这是唯一权威判据）：
    DMD_VA_LOG=1 LIBVA_DRIVER_NAME=msm_drm ffmpeg -hwaccel vaapi \
      -hwaccel_output_format vaapi -i test.mp4 -frames:v 3 -f null -
    → 「会话已建立: codec=N WxH 端点=/dev/video32」 V4L2 协商成功
    → 「[v4l2] 收到 SOURCE_CHANGE」            固件真的在解析码流
    → 「[v4l2] REQBUFS 失败」等                该设备解码路径起不来

  ⚠️ 确认你测的是当前构建，而不是系统目录里的旧 .so：
    md5sum /usr/lib/aarch64-linux-gnu/dri/msm_drm_drv_video.so
    md5sum vaapi-driver/build/msm_drm_drv_video.so
    两者不同就先重新安装，否则测的是旧版本（实测踩过这个坑）。
    免安装测法: LIBVA_DRIVERS_PATH=/tmp/dri 指向放了新 .so 的目录。

  设备是否根本不支持：
    cc -O2 -o /tmp/pds vaapi-driver/tools/probe_device_support.c && /tmp/pds test.h264
    → 收不到 SOURCE_CHANGE 即该设备的解码会话起不来，驱动帮不上
NOTE

echo ""
echo "== 怎么判断"解码成功了" =="
cat <<'NOTE'
  按客户端类型选判据 —— 用错判据是本项目最常见的误诊来源。

  ▸ ffmpeg / 走 daemon 的客户端
      充分证据: daemon 日志有「输出格式 WxH stride=...」+ 帧数达标 + 速度 >3x
      只有「握手成功」而无「输出格式」= 通道通了但没真解码
      有「输出格式」却帧数不足 = 吞吐故障（查「输入缓冲暂满」）

  ▸ Firefox
      充分证据（播放时）: RDD 进程的 drv_video 映射 >0 且 renderD 句柄 >0
      检查命令: RDD=$(pgrep -f 'rdd$'); grep -c drv_video /proc/$RDD/maps
      ⚠️ 停播后这些归零属正常，不是故障
      ⚠️ daemon 日志无 Firefox 会话属正常（它不经 daemon）

  ▸ Chrome
      充分证据: GPU 进程 maps 里有 drv_video，且 cmdline 含 ozone-platform=wayland
      X11 下解码器会创建但零帧（dmabuf 提交路径不通），日志表现为
      「握手成功」后「收到 0 NALU, 回传 0 帧」

  ▸ 不可靠的判据（别用）
      ✗ ffmpeg 日志里的 "hevc (native)" —— 硬解生效时这行照样出现
      ✗ watchdog 报 healthy —— 只验证握手，吞吐故障照样报健康
      ✗ 只写 -hwaccel vaapi 测试 —— 拿不到硬解会静默回落软解且不报错
      ✗ 短片跑分比软硬解耗时 —— 720p 小片软解可能更快，硬解优势在
        高分辨率/高码率/长时间播放的 CPU 与功耗，不是跑分

  ▸ 画面卡顿但上述判据全绿
      说明解码正常，问题在呈现链：容器内 kwin → anland 桥 →
      宿主 SurfaceFlinger + HWC，每帧被合成三次（实测四者合计约 122%
      一个核）。这条链抖动表现为画面断续，解码器无感。
NOTE

echo ""
if [ "$FAIL" -eq 0 ]; then echo "结果: ${PASS} 项全绿"; else echo "结果: ${PASS} 绿 / ${FAIL} 异常"; fi
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
