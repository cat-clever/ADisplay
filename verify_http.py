# -*- coding: utf-8 -*-
# 端到端验投递这一段：起一个和 C# 里 LiveTsServer 同语义的 HTTP 服务
# （同样的头、同样的 HEAD 处理、同样没有 Content-Length），
# 让 ffprobe 真的按 URL 拉一次，看它能不能解出分辨率。
import io
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import verify_ts as V

SPS_PPS = V.START + V.SPS + V.START + V.PPS
FRAMES = 150
PORT = 0


def payload(i):
    if i == 0:
        return SPS_PPS + V.slice_nal(5, 8800, 0x88)
    return V.slice_nal(1, 320, 0x11 + (i % 7))


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def _head(self, with_body):
        self.send_response(200)
        self.send_header('Content-Type', 'video/vnd.dlna.mpeg-tts')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Accept-Ranges', 'none')
        self.send_header('Connection', 'close')
        self.end_headers()
        if not with_body:
            return
        muxer = V.Muxer()
        cached = None
        for i in range(FRAMES):
            body = payload(i)
            has, sets = True, SPS_PPS if i == 0 else None
            if has and sets is not None:
                cached = sets
            elif cached is not None:
                body = cached + body
            self.wfile.write(muxer.frame(i, body, i * 33333, 0x1B))
        self.close_connection = True

    def do_GET(self):
        self._head(True)

    def do_HEAD(self):
        self._head(False)


server = HTTPServer(('127.0.0.1', PORT), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()
url = 'http://127.0.0.1:%d/live.ts' % port
print('服务地址', url)

print('--- HEAD 请求 ---')
r = subprocess.run(['curl', '-sS', '-I', url], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
print(r.stdout.decode().strip())

print('--- 按 URL 拉流（ffprobe）---')
r = subprocess.run(['ffprobe', '-v', 'error', '-show_entries',
                    'stream=codec_name,width,height', '-of', 'csv=p=0', url],
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
print('ffprobe 认出：', r.stdout.decode().strip())
if r.stderr.strip():
    print('stderr 前几行：')
    for line in r.stderr.decode('utf-8', 'replace').splitlines()[:5]:
        print('   ', line)

server.shutdown()
