"""Local deterministic page. Records actual DOM effects for the demo/tests."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading
from urllib.parse import urlparse

HTML = b'''<!doctype html>
<html><head><meta charset="utf-8"><title>Browser fixture</title>
<style>
*{box-sizing:border-box}body{margin:0;padding:24px;font:16px 'Segoe UI',sans-serif;background:#f2f5f4;color:#15372a}
input,button{display:block;width:280px;height:40px;margin:0 0 16px;font:inherit;border:1px solid #9cafa4;border-radius:5px;padding:8px}
button{background:#164e38;color:white;cursor:pointer}h1{font-size:26px;margin:30px 0 8px}p{max-width:620px;line-height:1.7}
#values{background:white;border-radius:8px;padding:20px;max-width:620px;white-space:pre-wrap;line-height:1.8}footer{margin-top:1200px}
</style></head><body>
<input id="text" placeholder="Type here"><button id="increment">Increment counter</button>
<button id="popup">Request popup</button><h1 id="name"></h1>
<p>This is a real WebView2 page. Each window has independent cookies and local storage.
The native toolbar controls ownership of automation.</p><div id="values"></div><footer>End of scroll test</footer>
<script>
const name=new URL(location.href).searchParams.get('name')||'Browser';
let count=0; document.getElementById('name').textContent=name;
if(!document.cookie.includes('session=')) document.cookie='session='+crypto.randomUUID()+'; path=/';
if(!localStorage.getItem('session')) localStorage.setItem('session',crypto.randomUUID());
function report(event){const data={event,name,count,text:document.getElementById('text').value,cookie:document.cookie,
storage:localStorage.getItem('session'),scroll:scrollY};document.title=name+' | '+count+' | '+data.text;
document.getElementById('values').textContent=JSON.stringify(data,null,2);
fetch('/event',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});}
document.getElementById('text').addEventListener('input',()=>report('input'));
document.getElementById('increment').onclick=()=>{count++;report('click')};
document.getElementById('popup').onclick=()=>window.open('/?name=popup','_blank');
addEventListener('scroll',()=>report('scroll')); report('loaded');
</script></body></html>'''


class Fixture:
    def __init__(self):
        self.records = []
        self.condition = threading.Condition()
        self.slow_started = threading.Event()
        self.slow_release = threading.Event()
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                if urlparse(self.path).path == "/slow":
                    fixture.slow_started.set()
                    fixture.slow_release.wait(timeout=35)
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(HTML)))
                self.end_headers()
                try:
                    self.wfile.write(HTML)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass  # Stop/navigation cancellation intentionally closes the request.

            def do_POST(self):
                data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                with fixture.condition:
                    fixture.records.append(data)
                    fixture.condition.notify_all()
                self.send_response(204)
                self.end_headers()

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def url(self, name):
        from urllib.parse import quote
        return f"http://127.0.0.1:{self.server.server_port}/?name={quote(name)}"

    def wait(self, predicate, timeout=10):
        with self.condition:
            if not self.condition.wait_for(lambda: any(predicate(item) for item in self.records), timeout):
                raise AssertionError(f"Expected DOM effect missing. Recorded: {self.records[-5:]}")
            return next(item for item in reversed(self.records) if predicate(item))

    def close(self):
        self.slow_release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
