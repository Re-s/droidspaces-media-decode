#!/usr/bin/env python3
"""收集浏览器回传的帧序观测。只服务本机，收满即退。"""
import json, os, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
OUT=os.environ.get("OUT","/tmp/cr/auto/result.json")
ROOT=os.environ.get("ROOT","/tmp/cr")
WANT=int(os.environ.get("WANT_REPORTS","1"))
DONE={"n":0}
class H(BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def _s(self,code,body=b"",ct="text/plain"):
        self.send_response(code); self.send_header("Content-Type",ct)
        self.send_header("Content-Length",str(len(body)))
        self.send_header("Access-Control-Allow-Origin","*"); self.end_headers()
        if body: self.wfile.write(body)
    def do_POST(self):
        if self.path!="/report": return self._s(404)
        raw=self.rfile.read(int(self.headers.get("Content-Length","0")))
        try: data=json.loads(raw)
        except Exception as e: return self._s(400,str(e).encode())
        ex=[]
        if os.path.exists(OUT):
            try: ex=json.load(open(OUT))
            except Exception: ex=[]
        ex.append(data)
        json.dump(ex,open(OUT,"w"),ensure_ascii=False,indent=1)
        DONE["n"]+=1
        print(f"报告 {DONE['n']}/{WANT}: {data.get('label')}",flush=True)
        self._s(200,b"ok")
    def do_GET(self):
        p=self.path.split("?")[0]
        if p=="/": p="/mse_test.html"
        fp=os.path.join(ROOT,p.lstrip("/"))
        if not os.path.isfile(fp): return self._s(404)
        ct="text/html" if fp.endswith(".html") else "video/mp4"
        body=open(fp,"rb").read()
        self.send_response(200); self.send_header("Content-Type",ct)
        self.send_header("Content-Length",str(len(body)))
        self.send_header("Accept-Ranges","bytes"); self.end_headers()
        self.wfile.write(body)
if __name__=="__main__":
    port=int(sys.argv[1]) if len(sys.argv)>1 else 8791
    if os.path.exists(OUT): os.unlink(OUT)
    srv=ThreadingHTTPServer(("127.0.0.1",port),H)
    print(f"listen {port} want {WANT}",flush=True)
    while DONE["n"]<WANT: srv.handle_request()
    print("done",flush=True)
