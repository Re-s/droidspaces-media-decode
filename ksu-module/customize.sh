#!/system/bin/sh
# customize.sh — dmd_watchdog 模块安装脚本（KSU / Magisk / APatch 通用）
#
# 为什么必须有这个文件：
#   zip 归档不保留 Unix 权限位。没有它，解包出来的 service.sh /
#   dmd-watchdog.sh / dmd-probe 全是 0644，service.sh 里的 [ -x ] 检查
#   一律失败，安装器也会报"缺少文件或执行权限"。
#   （service.sh 里另有一层 chmod 自愈兜底，但安装期就设对更干净。）

SKIPUNZIP=1

ui_print "╔══════════════════════════════════════════╗"
ui_print "║  DMD Watchdog — decode-daemon 看护模块   ║"
ui_print "╚══════════════════════════════════════════╝"
ui_print " "

ui_print "- 解包模块文件"
unzip -o "$ZIPFILE" -x 'META-INF/*' -d "$MODPATH" >&2

ui_print "- 设置权限"
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh"      0 0 0755
set_perm "$MODPATH/dmd-watchdog.sh" 0 0 0755
set_perm "$MODPATH/action.sh"       0 0 0755

# dmd-probe 是端点探活用的静态二进制，缺它看护无法判断 daemon 健康
if [ -f "$MODPATH/dmd-probe" ]; then
    set_perm "$MODPATH/dmd-probe" 0 0 0755
    ui_print "  dmd-probe 就绪"
else
    ui_print "  ⚠ 警告: dmd-probe 缺失，看护将无法探活"
    ui_print "    请用带 dmd-probe 的完整包，或从 Release 附件下载"
fi

# ── 部署安卓侧运行时 ─────────────────────────────────────────────
#
# 曾经这一段不存在，于是在**新设备**上刷完模块后 dmd/ 目录是空的 ——
# service.sh 只 mkdir 空目录，看护找不到 decode-daemon 就一直报错。
# 老设备看不出问题，因为 daemon 是当初手动部署的。
#
# 目录布局（与 service.sh 的 BIN_DIR/RUN_DIR/LOG_DIR 保持一致）：
#   dmd/bin/   decode-daemon
#   dmd/run/   socket 与看护状态（运行期生成）
#   dmd/logs/  日志
DS_DIR=/data/local/Droidspaces
DMD_DIR="${DS_DIR}/dmd"

ui_print " "
ui_print "- 部署安卓侧运行时"
mkdir -p "${DMD_DIR}/bin" "${DMD_DIR}/run" "${DMD_DIR}/logs"
set_perm_recursive "${DMD_DIR}" 0 0 0755 0644

if [ -f "$MODPATH/decode-daemon" ]; then
    # 装到 dmd/bin/ 而不是留在模块目录：daemon 的生命周期与模块无关
    # （卸载模块不该带走它），且 service.sh/看护都从这个路径找它。
    mv -f "$MODPATH/decode-daemon" "${DMD_DIR}/bin/decode-daemon"
    set_perm "${DMD_DIR}/bin/decode-daemon" 0 0 0755
    ui_print "  decode-daemon → dmd/bin/"
elif [ -x "${DMD_DIR}/bin/decode-daemon" ]; then
    ui_print "  decode-daemon 已存在，保留原有版本"
else
    ui_print "  ⚠ 警告: 包内无 decode-daemon，且设备上也没有"
    ui_print "    看护会持续报错。请从 Release 下载 decode-daemon 放到"
    ui_print "    ${DMD_DIR}/bin/ 并 chmod 755"
fi

ui_print " "
ui_print "- 安装完成，重启后生效"
ui_print "  日志: ${DMD_DIR}/logs/"
ui_print "  状态: KSU 管理器里本模块的描述行会实时显示"
ui_print " "
ui_print "  安卓侧到此已齐备。容器内还需自行准备："
ui_print "   1. msm_drm_drv_video.so 放进容器的"
ui_print "      /usr/lib/aarch64-linux-gnu/dri/（见 Release 附件）"
ui_print "   2. 装好 libva2 与 libva-drm2"
ui_print "   3. 平台已把 /dev/dri/renderD128 透传进容器"
