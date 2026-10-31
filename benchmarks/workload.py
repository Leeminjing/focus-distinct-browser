"""Deterministic offline workload; no external sites, model or device input."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading

HTML = b'''<!doctype html><meta charset="utf-8"><title>Benchmark</title>
<style>body{margin:20px;font:16px sans-serif}input,button{position:absolute;left:20px;
width:280px;height:36px;box-sizing:border-box}input{top:20px}#count{top:78px}
#popup{top:132px}canvas{position:absolute;left:20px;top:200px}</style>
<input id="entry"><button id="count">Increment</button><button id="popup">New page</button>
<canvas id="picture" width="620" height="220"></canvas>
<script>
const label=new URL(location.href).searchParams.get('name')||'root';
const documentId=crypto.randomUUID();let clicks=0,children=0;
if(!document.cookie.includes('bench='))document.cookie='bench='+crypto.randomUUID()+';path=/';
if(!localStorage.getItem('bench'))localStorage.setItem('bench',crypto.randomUUID());
const ctx=picture.getContext('2d'),img=ctx.createImageData(620,220);let seed=123456789;
for(let i=0;i<img.data.length;i+=4){seed=(Math.imul(seed,1664525)+1013904223)|0;
img.data[i]=seed>>>24;img.data[i+1]=seed>>>16;img.data[i+2]=seed>>>8;img.data[i+3]=255;}
ctx.putImageData(img,0,0);
function report(kind){document.title=label+'|'+documentId+'|'+entry.value+'|'+clicks;
fetch('/event',{method:'POST',body:JSON.stringify({label,documentId,kind,text:entry.value,
clicks,cookie:document.cookie,storage:localStorage.getItem('bench')})});}
count.onclick=()=>{clicks++;report('click')};entry.oninput=()=>report('input');
popup.onclick=()=>window.open('/?name='+encodeURIComponent(label+'-child'+(++children)),'_blank');
report('loaded');
</script>'''


class Fixture:
    def __init__(self):
        self.states = {}
        self.condition = threading.Condition()
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args): pass

            def do_GET(self):
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(HTML)))
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                try: self.wfile.write(HTML)
                except (BrokenPipeError, ConnectionError): pass

            def do_POST(self):
                size = int(self.headers.get('Content-Length', '0'))
                if not 0 < size < 16384:
                    self.send_error(400); return
                record = json.loads(self.rfile.read(size))
                with fixture.condition:
                    fixture.states[record['label']] = record
                    fixture.condition.notify_all()
                self.send_response(204); self.end_headers()

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def url(self, name):
        return f'http://127.0.0.1:{self.server.server_port}/?name={name}'

    def wait(self, name, predicate=lambda r: True, timeout=10):
        with self.condition:
            if not self.condition.wait_for(lambda: name in self.states and predicate(self.states[name]), timeout):
                raise TimeoutError('Fixture state did not arrive: ' + name)
            return dict(self.states[name])

    def close(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join(2)
