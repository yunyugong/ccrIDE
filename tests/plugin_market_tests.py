from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import sys,threading,subprocess,hashlib,io,zipfile,json
blob=Path(sys.argv[2]).read_bytes()
payload=io.BytesIO()
with zipfile.ZipFile(payload,'w',zipfile.ZIP_DEFLATED) as z:
    z.writestr('zip-docs/completion-docs.dll',blob)
    z.writestr('zip-docs/plugin.json',json.dumps({'id':'zip-docs','name':'ZIP docs','version':'1.0','entry':'completion-docs.dll','apiVersion':2,'kind':'soft'}))
archive=payload.getvalue()
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path=='/digest':data=hashlib.sha256(blob).hexdigest().encode()
        elif self.path=='/plugin.zip':data=archive
        elif self.path=='/catalog.json':data=json.dumps({'schema':1,'plugins':[{'id':'zip-docs','name':'ZIP docs','version':'1.0','entry':'completion-docs.dll','apiVersion':2,'packageUrl':f'http://127.0.0.1:{self.server.server_port}/plugin.zip','sha256':hashlib.sha256(archive).hexdigest()}]}).encode()
        else:data=blob
        self.send_response(200);self.send_header('Content-Length',str(len(data)+(100 if self.path=='/truncated' else 0)));self.end_headers()
        try:self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError):pass
        if self.path=='/truncated':self.close_connection=True
    def log_message(self,*args):pass
server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
try:result=subprocess.run([sys.argv[1],'--market-check',f'http://127.0.0.1:{server.server_port}'],timeout=45)
finally:server.shutdown();server.server_close()
sys.exit(result.returncode)
