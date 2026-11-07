from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import threading,tempfile,json,subprocess,sys,time
requests=[]
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        data=json.dumps({'data':[{'id':'mock-model'}]}).encode();self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append((self.headers.get('Authorization'),body))
        if not body.get('stream'):
            data=json.dumps({'choices':[{'message':{'content':'完整回复：你好'}}]},ensure_ascii=False).encode();self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
        self.send_response(200);self.send_header('Content-Type','text/event-stream');self.end_headers()
        slow=body['messages'][-1]['content']=='slow'
        try:
            for piece in (['piece']*30 if slow else ['流式回复：','你好']):
                self.wfile.write(('data: '+json.dumps({'choices':[{'delta':{'content':piece}}]},ensure_ascii=False)+'\n\n').encode());self.wfile.flush();time.sleep(.05 if slow else .01)
            self.wfile.write(b'data: [DONE]\n\n');self.wfile.flush()
        except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError):pass
server=ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix='ccride-ai-中文-') as root:
        Path(root,'settings.json').write_text(json.dumps({'ai':{'enabled':True,'baseUrl':f'http://127.0.0.1:{server.server_port}/v1','model':'mock-model','apiKey':'plugin-test-key'}}),encoding='utf-8')
        result=subprocess.run([sys.argv[1],root],capture_output=True,timeout=25)
        print(result.stdout.decode('utf-8','replace'));assert result.returncode==0,result.stderr.decode('utf-8','replace')
        assert 5 <= len(requests) <= 6 and all(auth=='Bearer plugin-test-key' for auth,_ in requests)
        assert 'int selectedValue=42;' in requests[0][1]['messages'][-1]['content']
        assert len(requests[1][1]['messages'])==4
        print('AI HTTP mock: auth, selection and multi-turn history passed')
finally:server.shutdown();server.server_close()
