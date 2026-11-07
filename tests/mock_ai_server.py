# MiniCPP 测试用模拟 OpenAI 兼容服务
# 记录收到的请求到 ai_server_log.txt, 并返回 SSE 流式回复
import json, time, sys, io

LOG = io.open(r"E:\cppIDE\build\ai_server_log.txt", "w", encoding="utf-8")

def log(msg):
    LOG.write(msg + "\n")
    LOG.flush()

from http.server import BaseHTTPRequestHandler, HTTPServer

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _log(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode("utf-8", "replace") if n else ""
        log("=== %s %s ===" % (self.command, self.path))
        log(body)

    def _send(self, status, ctype, payload, headers=None):
        b = payload.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        if headers:
            for k, v in headers.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(b)
        self.wfile.flush()

    def do_GET(self):
        self._log()
        if self.path == "/v1/models":
            self._send(200, "application/json", json.dumps({"data": [{"id": "mock-model"}]}))
        else:
            self._send(404, "application/json", json.dumps({"error": "not found"}))

    def do_POST(self):
        self._log()
        try:
            req = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode("utf-8"))
            log("stream=%s model=%s msgs=%d" % (req.get("stream"), req.get("model"), len(req.get("messages", []))))
        except Exception as e:
            log("parse-err " + str(e))
        # SSE 流式: 两个增量块 + [DONE]
        if self.path == "/v1/chat/completions":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            try:
                for piece in ("你好", ", 这是模拟回复"):
                    ev = "data: " + json.dumps({
                        "choices": [{"delta": {"content": piece}}]
                    }, ensure_ascii=False) + "\n\n"
                    self.wfile.write(ev.encode("utf-8"))
                    self.wfile.flush()
                    time.sleep(0.1)
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()
            except Exception as e:
                log("stream-err " + str(e))
        else:
            self._send(404, "application/json", json.dumps({"error": "unknown path"}))

    def log_message(self, *a):
        pass

HTTPServer(("127.0.0.1", 18899), H).serve_forever()
