#!/usr/bin/env python3
"""Save a playback measurement posted by play.html to /result.

Wayland does not expose the window title to xdotool or wmctrl. The page
posts its JSON results over HTTP instead. The server exits after one report.

Usage: sink.py [output file] [port]
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/play_result.json"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8931


class H(BaseHTTPRequestHandler):
    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n).decode("utf-8", "replace")
        self.send_response(200)
        self._cors()
        self.end_headers()
        self.wfile.write(b"ok")
        try:
            d = json.loads(raw)
        except Exception:
            d = {"raw": raw}
        with open(OUT, "w") as f:
            json.dump(d, f, ensure_ascii=False, indent=2)
        print(json.dumps(d, ensure_ascii=False))
        sys.stdout.flush()
        raise SystemExit(0)

    def log_message(self, *a):
        pass


srv = HTTPServer(("127.0.0.1", PORT), H)
print(f"[sink] Waiting for results -> {OUT} (port {PORT})", flush=True)
try:
    srv.serve_forever()
except SystemExit:
    pass
