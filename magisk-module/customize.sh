#!/system/bin/sh
# customize.sh — KSU/Magisk module installation for decode-daemon

SKIPUNZIP=1

ui_print "╔══════════════════════════════════════╗"
ui_print "║  MediaCodec Decode Daemon Module     ║"
ui_print "║  v1.0 — for DroidSpaces container    ║"
ui_print "╚══════════════════════════════════════╝"
ui_print ""

# Extract module files to MODPATH
ui_print "- Extracting files"
unzip -o "$ZIPFILE" -x 'META-INF/*' -d "$MODPATH" >&2

# Permissions
ui_print "- Setting permissions"
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755

# Binary stays in module directory (service.sh looks there first)
if [ -f "$MODPATH/decode-daemon" ]; then
    chmod 755 "$MODPATH/decode-daemon"
    ui_print "  decode-daemon ready at $MODPATH/decode-daemon"
else
    ui_print "  WARNING: decode-daemon binary not found!"
fi

# Prepare socket directory
ui_print "- Preparing socket dir"
mkdir -p /data/local/tmp/anland
chmod 777 /data/local/tmp/anland

ui_print ""
ui_print "- Installed! Reboot to start the daemon."
ui_print "- Socket: /data/local/tmp/anland/decode.sock"
ui_print "- Log: /data/local/tmp/decode-daemon.log"
