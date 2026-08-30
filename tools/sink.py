#!/usr/bin/env python3
"""接收 play.html 的测量结果并落盘。

页面在采样结束时 POST 一条 JSON 到 /result。Wayland 下拿不到窗口标题
（xdotool / wmctrl 都取不到），所以用这条 HTTP 回传代替。

用法: sink.py [输出文件] [端口]
拿到一条结果就退出，便于脚本串联。
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
print(f"[sink] 等待结果 -> {OUT} (port {PORT})", flush=True)
try:
    srv.serve_forever()
except SystemExit:
    pass
