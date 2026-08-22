#!/system/bin/sh
# service.sh — Start decode-daemon on boot (TCP mode, no fork loop)

while [ "$(getprop sys.boot_completed)" != "1" ]; do sleep 1; done
sleep 5

LOG="/data/local/tmp/decode-daemon.log"
PORT=19876

BIN="/data/adb/modules/decode-daemon/decode-daemon"
[ ! -x "$BIN" ] && BIN="/data/local/tmp/decode-daemon"
[ ! -x "$BIN" ] && { echo "daemon: no binary" >> "$LOG"; exit 1; }

# Kill any existing instances
killall decode-daemon 2>/dev/null
sleep 2

chmod 755 "$BIN"
echo "daemon: start $(date) port=$PORT" >> "$LOG"
nohup "$BIN" "$PORT" >> "$LOG" 2>&1 &
sleep 3

if grep -q "listening" "$LOG" 2>/dev/null; then
    echo "daemon: OK port=$PORT" >> "$LOG"
else
    echo "daemon: FAILED to bind port=$PORT" >> "$LOG"
fi

# NO fork loop - just start once and exit
# The daemon will stay running as a background process
exit 0
