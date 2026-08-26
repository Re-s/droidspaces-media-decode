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

ui_print " "
ui_print "- 安装完成，重启后生效"
ui_print "  日志: /data/local/Droidspaces/Logs/dmd-watchdog.log"
ui_print "  状态: KSU 管理器里本模块的描述行会实时显示"
