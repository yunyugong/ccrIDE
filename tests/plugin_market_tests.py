from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import sys,threading,subprocess
dll=Path(sys.argv[2]).read_bytes()
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200);self.send_header('Content-Length',str(len(dll)));self.end_headers();self.wfile.write(dll)
    def log_message(self,*args):pass
server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
try:
    result=subprocess.run([sys.argv[1],'--market-check',f'http://127.0.0.1:{server.server_port}'],timeout=30)
finally:server.shutdown();server.server_close()
sys.exit(result.returncode)
