"""Real WebView2 page/session acceptance, using only local deterministic pages.

Native messages are automated input, never physical device acceptance.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import ctypes
from pathlib import Path
import sys
import threading
import time
from urllib.parse import urlparse, parse_qs

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "demo"))
from client import Client, BrowserError
from native_ui import user32, click, text, snapshot

HTML = b'''<!doctype html><meta charset="utf-8"><title>Pages</title>
<style>body{margin:24px;font:16px sans-serif}input,button,a{display:block;box-sizing:border-box;
width:280px;height:32px;margin-bottom:16px}a{background:#ddd}#state{white-space:pre-wrap}</style>
<input id="input"><button id="popup">window.open</button><a id="link" target="_blank">target blank</a>
<button id="named">named window</button><button id="blank">blank popup</button>
<button id="close">window.close</button><button id="nested">open and close opener</button><pre id="state"></pre>
<script>
const name=new URL(location.href).searchParams.get('name')||'root';
if(!document.cookie.includes('session='))document.cookie='session='+crypto.randomUUID()+';path=/';
if(!localStorage.getItem('session'))localStorage.setItem('session',crypto.randomUUID());
let ping=false; let sequence=0;
function report(kind){const value={kind,name,text:input.value,cookie:document.cookie,
storage:localStorage.getItem('session'),opener:!!opener,ping,url:location.href};
document.title=name+' | '+input.value;state.textContent=JSON.stringify(value,null,2);
fetch('/event',{method:'POST',body:JSON.stringify(value)});}
popup.onclick=()=>window.open('/?name='+name+'-child'+(++sequence),'_blank');
link.href='/?name='+name+'-link';
named.onclick=()=>window.open('/?name='+name+'-named','shared-name');
blank.onclick=()=>window.open('','_blank');
document.getElementById('close').onclick=()=>window.close();
nested.onclick=()=>{window.open('/?name='+name+'-nested');window.close()};
input.oninput=()=>report('input');
addEventListener('message',e=>{if(e.data==='child-ready')e.source.postMessage('opener-reply',location.origin);
if(e.data==='opener-reply'){ping=true;report('ping')}});
if(opener)opener.postMessage('child-ready',location.origin);
setInterval(async()=>{let command=await(await fetch('/command?name='+encodeURIComponent(name))).json();
if(command==='close')window.close();if(command==='open')popup.click();},50);
report('loaded');
</script>'''


class Fixture:
    def __init__(self, html=HTML):
        self.records, self.commands = [], {}
        self.condition = threading.Condition()
        self.slow_started, self.slow_release = threading.Event(), threading.Event()
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                if urlparse(self.path).path == '/slow':
                    fixture.slow_started.set()
                    fixture.slow_release.wait(15)
                if urlparse(self.path).path == '/command':
                    name = parse_qs(urlparse(self.path).query).get('name', [''])[0]
                    with fixture.condition:
                        content = json.dumps(fixture.commands.pop(name, None)).encode()
                else:
                    content = html
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(content)))
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                try:
                    self.wfile.write(content)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass

            def do_POST(self):
                record = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                with fixture.condition:
                    fixture.records.append(record)
                    fixture.condition.notify_all()
                self.send_response(204)
                self.end_headers()

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def url(self, name):
        return f'http://127.0.0.1:{self.server.server_port}/?name={name}'

    def wait(self, predicate, after=0):
        with self.condition:
            assert self.condition.wait_for(lambda: any(predicate(r) for r in self.records[after:]), 10), self.records[-4:]
            return next(r for r in reversed(self.records[after:]) if predicate(r))

    def close(self):
        self.slow_release.set()
        self.server.shutdown(); self.server.server_close(); self.thread.join(2)


def denied(code, work):
    try:
        work()
    except BrowserError as failure:
        assert failure.code == code, (code, failure)
        return failure
    raise AssertionError(f'Expected {code}')


def eventually(work, predicate, message, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = work()
        if predicate(value):
            return value
        time.sleep(.02)
    raise AssertionError(message)


def run():
    client = Client(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/distinct-browser.exe')
    fixture = Fixture()
    browsers, results = [], []

    def passed(name):
        results.append(name); print('PASS ' + name, flush=True)

    def make(name):
        made = client.call('create', label=name); browsers.append(made['browser_id'])
        params = {'browser_id': made['browser_id'], 'token': client.call('acquire', browser_id=made['browser_id'], client_id=name, ttl_ms=300000)['token']}
        client.call('observe', **params)
        client.call('act', **params, action={'type': 'navigate', 'url': fixture.url(name)})
        client.call('observe', **params)
        return made, params

    def pages(browser):
        return client.call('list_pages', browser_id=browser)

    def observed(params, page=None):
        def get():
            try:
                return client.call('observe', **params, **({'page_id': page} if page else {}))
            except BrowserError as failure:
                if failure.code not in {'page_loading', 'page_changed', 'capture_failed', 'observation_failed'}:
                    raise
                return None
        return eventually(get, bool, 'Page did not become observable')

    def act(params, kind, **fields):
        return client.call('act', **params, action={'type': kind, **fields})

    def select(params, page):
        return client.call('select_page', **params, page_id=page)

    def press(params, y, *, closes_page=False):
        # Opening a popup can invalidate the remaining mouse step. The host
        # reports this rather than retargeting it or silently retrying it.
        try:
            return act(params, 'click', x=100, y=y)
        except BrowserError as failure:
            allowed = {'page_changed', 'page_closed'} if closes_page else {'page_changed'}
            assert failure.code in allowed, failure

    def new_page(params, y, count=2):
        previous = {p['page_id'] for p in pages(params['browser_id'])['pages']}
        observed(params)
        press(params, y)
        listed = eventually(lambda: pages(params['browser_id']),
                            lambda r: len(r['pages']) == count and r['selected_page_id'] not in previous,
                            'Popup did not become selected')
        child = listed['selected_page_id']
        observed(params, child)
        return child

    try:
        a, pa = make('root'); root = a['page_id']
        b, pb = make('independent')
        observed(pa, root)
        act(pa, 'click', x=80, y=40); act(pa, 'text', text='parent input')
        child = new_page(pa, 88)
        listed = pages(a['browser_id'])
        info = next(p for p in listed['pages'] if p['page_id'] == child)
        assert info['opener_page_id'] == root and info['hwnd'] != a['hwnd']
        snapshot(int(a['hwnd']), ROOT / 'artifacts/v0.4-parent.png')
        snapshot(int(info['hwnd']), ROOT / 'artifacts/v0.4-child.png')
        first = fixture.wait(lambda r: r['name'] == 'root')
        pop = fixture.wait(lambda r: r['name'] == 'root-child1' and r['ping'])
        other = fixture.wait(lambda r: r['name'] == 'independent')
        assert pop['opener'] and first['cookie'] == pop['cookie'] and first['storage'] == pop['storage']
        assert other['cookie'] != pop['cookie'] and other['storage'] != pop['storage']
        passed('window.open keeps opener, shares session and WindowProxy messaging; another browser stays isolated')

        denied('page_not_selected', lambda: client.call('observe', **pa, page_id=root))
        denied('page_not_found', lambda: client.call('observe', **pa, page_id=b['page_id']))
        denied('busy', lambda: client.call('acquire', browser_id=a['browser_id'], client_id='second'))
        select(pa, root)
        denied('observation_required', lambda: act(pa, 'text', text='stale screenshot'))
        assert observed(pa, root)['title'] == 'root | parent input'
        select(pa, child)
        denied('observation_required', lambda: act(pa, 'click', x=80, y=40))
        assert observed(pa, child)['page_id'] == child
        user32.SendMessageW(int(a['hwnd']), 0x0006, 1, 0)  # WM_ACTIVATE / WA_ACTIVE
        assert pages(a['browser_id'])['selected_page_id'] == root
        denied('observation_required', lambda: act(pa, 'text', text='old native-window observation'))
        observed(pa, root)
        select(pa, child); observed(pa, child)
        passed('explicit target rejects cross-browser and unselected pages; every selection invalidates the old observation')

        # One browser-wide in-flight slot, even when callers specify other pages.
        pending = client.submit('observe', **pa, page_id=child)
        denied('busy', lambda: client.call('select_page', **pa, page_id=root))
        pending.result(10)
        client.call('close_page', **pa, page_id=child)
        denied('page_not_found', lambda: client.call('observe', **pa, page_id=child))
        assert pages(a['browser_id'])['selected_page_id'] == root
        assert observed(pa)['title'] == 'root | parent input'
        passed('browser-wide busy and child close preserve the parent; page ids do not revive')

        linked = new_page(pa, 136)
        # Modern target=_blank links imply noopener unless rel=opener is set.
        fixture.wait(lambda r: r['name'] == 'root-link' and not r['opener'])
        client.call('close_page', **pa, page_id=linked)
        named = new_page(pa, 184)
        select(pa, root); observed(pa)
        press(pa, 184)
        time.sleep(.3)
        assert len(pages(a['browser_id'])['pages']) == 2
        select(pa, named); observed(pa)
        press(pa, 280, closes_page=True)
        eventually(lambda: pages(a['browser_id']), lambda r: len(r['pages']) == 1, 'window.close did not close the child')
        passed('target=_blank, named-window reuse and script window.close are managed')

        blank = new_page(pa, 232)
        assert observed(pa, blank)['url'] == 'about:blank'
        client.call('close_page', **pa, page_id=blank)
        child = new_page(pa, 88)
        info = next(p for p in pages(a['browser_id'])['pages'] if p['page_id'] == child)
        # Exercise the child's native message path, not webpage JS detection.
        hwnd = int(info['hwnd'])
        target = user32.GetDlgItem(hwnd, 108)
        user32.PostMessageW(target, 0x0201, 1, (40 << 16) | 80)
        user32.PostMessageW(target, 0x0202, 0, (40 << 16) | 80)
        eventually(lambda: text(user32.GetDlgItem(int(a['hwnd']), 107)), lambda s: '人工' in s, 'Parent toolbar did not reflect child takeover')
        denied('invalid_token', lambda: client.call('observe', **pa, page_id=child))
        observed(pb); act(pb, 'click', x=80, y=40); act(pb, 'text', text='B continues')
        fixture.wait(lambda r: r['name'] == 'independent' and r['text'] == 'B continues')
        click(hwnd, 101)
        pa['token'] = client.call('acquire', browser_id=a['browser_id'], client_id='after-human', ttl_ms=300000)['token']
        denied('observation_required', lambda: act(pa, 'text', text='must not replay'))
        observed(pa, child)
        passed('blank popup is observable; child native input revokes browser-wide control; other browser continues; handback reobserves')

        user32.ShowWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
        user32.ShowWindow(int(a['hwnd']), 6)
        denied('invalid_token', lambda: client.call('observe', **pa))
        assert client.call('present', browser_id=a['browser_id'], page_id=root)['state'] == 'paused'
        assert pages(a['browser_id'])['selected_page_id'] == root
        click(int(a['hwnd']), 101)
        pa['token'] = client.call('acquire', browser_id=a['browser_id'], client_id='restored', ttl_ms=300000)['token']
        observed(pa, root)
        select(pa, child); observed(pa, child)
        passed('present can restore a paused background page without a lease; handback can then resume the browser')

        # A page can close while its navigation is already issued. That request
        # must settle for the closed page; it must never navigate the parent.
        slow = client.submit('act', **pa, page_id=child,
                             action={'type': 'navigate', 'url': fixture.url('slow').replace('/?', '/slow?')})
        assert fixture.slow_started.wait(10)
        denied('busy', lambda: client.call('select_page', **pa, page_id=root))
        observed(pb)
        user32.SendMessageW(hwnd, 0x0010, 0, 0)
        denied('page_closed', lambda: slow.result(10))
        fixture.slow_release.set()
        assert observed(pa, root)['title'] == 'root | parent input'
        child = new_page(pa, 88)
        passed('closing a child settles its in-flight navigation without retargeting the parent or blocking another browser')

        client.call('close_page', **pa, page_id=root)
        assert pages(a['browser_id'])['selected_page_id'] == child
        observed(pa, child)
        client.call('close_page', **pa, page_id=child)
        denied('browser_not_found', lambda: pages(a['browser_id']))
        browsers.remove(a['browser_id'])
        passed('closing the original page preserves child; closing last page invalidates browser')

        # Session close also destroys every child HWND and old page target.
        c, pc = make('root'); root = c['page_id']
        new_page(pc, 88)
        handles = [int(p['hwnd']) for p in pages(c['browser_id'])['pages']]
        client.call('close', browser_id=c['browser_id']); browsers.remove(c['browser_id'])
        user32.IsWindow.argtypes = [ctypes.c_void_p]
        assert not any(user32.IsWindow(hwnd) for hwnd in handles)
        passed('connection-level browser close destroys all page windows')

        # A bounded page limit must reject the extra popup, not let WebView2
        # create an unmanaged fallback window or damage existing pages.
        c, pc = make('limit'); root = c['page_id']
        for count in range(2, 17):
            new_page(pc, 232, count=count)
            select(pc, root)
        observed(pc)
        press(pc, 232)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            event = client.events.get(timeout=5)
            if event['event'] == 'page_creation_failed' and event.get('browser_id') == c['browser_id']:
                assert event['detail']['reason'] == 'session_unavailable_or_page_limit'
                break
        else:
            raise AssertionError('Page-limit rejection was not reported')
        assert len(pages(c['browser_id'])['pages']) == 16
        observed(pc, root)
        client.call('close', browser_id=c['browser_id']); browsers.remove(c['browser_id'])
        passed('16-page limit fails explicitly while existing pages and the browser lease remain usable')

        # EOF retains all pages for manual use. Closing one does not exit the
        # host; closing the final retained page does.
        c, pc = make('disconnect'); root = c['page_id']
        new_page(pc, 88)
        handles = [int(p['hwnd']) for p in pages(c['browser_id'])['pages']]
        for browser in list(browsers):
            if browser != c['browser_id']:
                client.call('close', browser_id=browser); browsers.remove(browser)
        client.disconnect(); browsers.clear()
        for hwnd in handles:
            eventually(lambda: user32.IsWindowEnabled(user32.GetDlgItem(hwnd, 101)),
                       lambda enabled: not enabled, 'Disconnected page still offers handback')
            assert user32.IsWindow(hwnd)
        user32.SendMessageW(handles[0], 0x0010, 0, 0)
        assert client.process.poll() is None and user32.IsWindow(handles[1])
        user32.SendMessageW(handles[1], 0x0010, 0, 0)
        passed('disconnect retains every managed page; only the final user close exits the host')
    finally:
        for browser in browsers:
            try:
                client.call('close', browser_id=browser)
            except BrowserError:
                pass
        client.disconnect(); client.wait(); fixture.close()
    (ROOT / 'artifacts/pages-acceptance.json').write_text(json.dumps({'checks': results, 'physical_input': False}, indent=2) + '\n', encoding='utf-8')
    print(f'{len(results)} page acceptance groups passed')


if __name__ == '__main__':
    run()
