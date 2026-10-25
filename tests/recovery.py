"""Real Windows process/protocol recovery; automated input is not physical acceptance."""
import json
from pathlib import Path
import subprocess
import sys
import time
import ctypes as C
import base64

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'demo'))
from reconnect_client import ReconnectClient, K, A, D, W, BLOB, INVALID, checked, private_file
from pages import Fixture, HTML, denied, eventually
from downloads import Fixture as DownloadFixture
from native_ui import click, user32

RECOVERY_HTML = HTML.replace(b'const name=', b"const documentId=crypto.randomUUID();document.body.style.minHeight='2400px';const name=")
RECOVERY_HTML = RECOVERY_HTML.replace(b'const value={kind,name,', b'const value={kind,name,documentId,scroll:scrollY,')
RECOVERY_HTML = RECOVERY_HTML.replace(b"document.title=name+' | '+input.value;", b"document.title=name+' | '+input.value+(scrollY?' | scroll='+scrollY:'');")
RECOVERY_HTML = RECOVERY_HTML.replace(b"input.oninput=()=>report('input');", b"input.oninput=()=>report('input');addEventListener('scroll',()=>report('scroll'));")


class RawPeer:
    """An independent byte-level peer for the public handshake/framing seam."""
    def __init__(self, endpoint):
        self.description = private_file(Path(endpoint))
        deadline = time.monotonic() + 10
        while True:
            self.handle = K.CreateFileW(self.description['pipe_name'], 0x0012018b, 0, None, 3, 0, None)
            if self.handle != INVALID: break
            error = C.get_last_error()
            if error != 231 or time.monotonic() > deadline: raise C.WinError(error)
            time.sleep(.025)
        self.buffer = b''

    def write(self, data):
        while data:
            buffer, size = C.create_string_buffer(data), W.DWORD()
            checked(K.WriteFile(self.handle, buffer, len(data), C.byref(size), None))
            assert size.value
            data = data[size.value:]

    def receive(self):
        while b'\n' not in self.buffer:
            buffer, size = C.create_string_buffer(65536), W.DWORD()
            checked(K.ReadFile(self.handle, buffer, len(buffer), C.byref(size), None))
            if not size.value: raise ConnectionError('Peer closed')
            self.buffer += buffer.raw[:size.value]
        line, self.buffer = self.buffer.split(b'\n', 1)
        return json.loads(line)

    def handshake(self, **overrides):
        protected = base64.b64decode(self.description['credential_dpapi'], validate=True)
        buffer = C.create_string_buffer(protected)
        encrypted, plain = BLOB(len(protected), C.addressof(buffer)), BLOB()
        checked(D.CryptUnprotectData(C.byref(encrypted), None, None, None, None, 1, C.byref(plain)))
        try: credential = C.string_at(plain.data, plain.size).decode('ascii')
        finally: K.LocalFree(plain.data)
        self.write((json.dumps({'host_id': self.description['host_id'], 'protocol_version': 1,
                                'credential': credential, **overrides}) + '\n').encode())

    def request(self, identity, method, **params):
        self.write((json.dumps({'id': identity, 'method': method, 'params': params}) + '\n').encode())
        return self.response()

    def response(self):
        while True:
            value = self.receive()
            if 'event' not in value: return value

    def close(self):
        K.CloseHandle(self.handle)


def rejected(peer):
    try:
        value = peer.receive()
    except (OSError, ConnectionError):
        return
    raise AssertionError('Unauthenticated peer received host state: ' + str(value.keys()))


def security_checks(endpoint, executable):
    # The caller's trusted process identity rejects PID reuse and a wrong EXE
    # before sending a credential. Restore this test-owned endpoint exactly.
    path, original = Path(endpoint), Path(endpoint).read_bytes()
    description = json.loads(original)
    try:
        for change in ({'process_created': str(int(description['process_created']) + 1)}, {'executable': str(executable.with_name('other.exe'))}):
            path.write_text(json.dumps({**description, **change}), encoding='utf-8')
            try: ReconnectClient(endpoint, executable)
            except ConnectionError: pass
            else: raise AssertionError('Untrusted host identity accepted')
    finally:
        path.write_bytes(original)
    for values in ({'credential': 'wrong-credential'}, {'host_id': 'wrong-host'}, {'protocol_version': 2}):
        peer = RawPeer(endpoint)
        try: peer.handshake(**values); rejected(peer)
        finally: peer.close()
    for value in (b'x' * 8193 + b'\n', b'{'):
        peer = RawPeer(endpoint)
        try: peer.write(value); rejected(peer)
        finally: peer.close()
    peer = RawPeer(endpoint)
    try:
        acl, descriptor = C.c_void_p(), C.c_void_p()
        checked(A.GetSecurityInfo(peer.handle, 6, 4, None, None, C.byref(acl), None, C.byref(descriptor)) == 0)  # SE_KERNEL_OBJECT
        try:
            assert W.WORD.from_address(acl.value + 4).value == 1
            ace = C.c_void_p(); checked(A.GetAce(acl, 0, C.byref(ace)))
            assert not W.DWORD.from_address(ace.value + 4).value & 4, 'client must not create pipe instances'
        finally: K.LocalFree(descriptor)
        peer.handshake(); assert peer.receive()['event'] == 'ready'
        identity = 'duplicate|wire-id'
        request = (json.dumps({'id': identity, 'method': 'create', 'params': {'label': 'Duplicate correlation probe'}}) + '\n').encode()
        peer.write(request + request)
        duplicate, created = None, None
        while duplicate is None or created is None:
            value = peer.receive()
            if value.get('event') == 'protocol_error': duplicate = value
            elif value.get('id') == identity:
                assert value['ok']; created = value['result']
        assert duplicate['detail']['code'] == 'duplicate_id' and duplicate['detail']['id'] == identity, duplicate['detail']
        assert peer.request('cleanup-duplicate', 'close', browser_id=created['browser_id'])['ok']
        for raw in (b'{\n', b'[]\n', b'x' * (1024 * 1024 + 1) + b'\n'):
            peer.write(raw)
            response = peer.response()
            assert response['id'] is None and response['error']['code'] == 'invalid_request'
        assert peer.request('valid', 'snapshot')['ok']
        peer.write(b'{"id":"discarded","method":"close"')  # fragment has no authority on EOF
    finally: peer.close()
    print('PASS private ACL, no create-instance rights, process birth/EXE check, bad authentication, handshake timeout/limit and JSONL recovery', flush=True)


def stale_endpoint_check(executable):
    empty = subprocess.Popen([str(executable), '--reconnect'], stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
    description = json.loads(empty.stdout.readline())
    endpoint = Path(description['endpoint'])
    try:
        empty.terminate(); empty.wait(10)
        assert endpoint.is_file(), 'a crash may leave discovery material behind'
        try: ReconnectClient(endpoint, executable)
        except ConnectionError: pass
        else: raise AssertionError('Dead host was presented as a recovered browser')
        print('PASS stale endpoint after actual host exit is unrecoverable; no replacement browser', flush=True)
    finally:
        if empty.poll() is None: empty.terminate(); empty.wait(10)
        empty.stdout.close(); empty.stderr.close()
        assert endpoint.name == 'endpoint.json' and endpoint.parent.name == description['host_id']
        endpoint.unlink(missing_ok=True); endpoint.parent.rmdir()


def worker(endpoint, executable):
    connection = reconnect(endpoint, executable, heartbeat=False)
    print(json.dumps({'connected': connection.connection_id}), flush=True)
    for line in sys.stdin:
        command = json.loads(line)
        if command.pop('submit', False):
            connection.submit(command.pop('method'), **command)
            result = {'sent': True}
        else:
            result = connection.call(command.pop('method'), **command)
        print(json.dumps(result), flush=True)


def ask(process, method, **params):
    process.stdin.write(json.dumps({'method': method, **params}) + '\n'); process.stdin.flush()
    return json.loads(process.stdout.readline())


def reconnect(endpoint, executable, **options):
    deadline = time.monotonic() + 10
    while True:
        try:
            return ReconnectClient(endpoint, executable, **options)
        except OSError as error:
            if error.winerror != 231 or time.monotonic() >= deadline:
                raise
            time.sleep(.025)


def main():
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/v06/distinct-browser.exe').resolve()
    process = subprocess.Popen([str(executable), '--reconnect'], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               creationflags=subprocess.CREATE_NO_WINDOW)
    connection, fixture, child, downloads, ready = None, Fixture(RECOVERY_HTML), None, DownloadFixture(), None
    try:
        ready = json.loads(process.stdout.readline())
        assert ready.get('transport') == 'named_pipe', ready
        assert Path(ready['endpoint']).is_file(), ready
        print('PASS opt-in reconnect endpoint')
        connection = reconnect(ready['endpoint'], executable)
        first_connection = connection.connection_id
        browser = connection.call('create', label='Recovery prototype A')
        bid, page, hwnd = browser['browser_id'], browser['page_id'], int(browser['hwnd'])
        token = connection.call('acquire', browser_id=bid, client_id='before')['token']
        connection.call('observe', browser_id=bid, token=token)
        connection.call('act', browser_id=bid, token=token, action={'type': 'navigate', 'url': fixture.url('recover-parent')})
        fixture.wait(lambda r: r['name'] == 'recover-parent' and r['kind'] == 'loaded')
        connection.call('observe', browser_id=bid, token=token)
        connection.call('act', browser_id=bid, token=token, action={'type': 'click', 'x': 70, 'y': 35})
        connection.call('act', browser_id=bid, token=token, action={'type': 'text', 'text': 'preserved-v06'})
        state = fixture.wait(lambda r: r['text'] == 'preserved-v06')
        connection.call('act', browser_id=bid, token=token, action={'type': 'scroll', 'x': 500, 'y': 300, 'delta_y': 500})
        scrolled = fixture.wait(lambda r: r['name'] == 'recover-parent' and r['scroll'] > 0)
        expected_title = 'recover-parent | preserved-v06 | scroll=' + str(scrolled['scroll'])
        try:
            ReconnectClient(ready['endpoint'], executable)
        except OSError as error:
            assert error.winerror == 231
        else: raise AssertionError('A second management client displaced the owner')
        assert connection.call('observe', browser_id=bid, token=token)['title'] == expected_title
        previous_revision = connection.call('snapshot')['revision']
        connection.disconnect()
        security_checks(ready['endpoint'], executable)
        connection = reconnect(ready['endpoint'], executable)
        assert connection.host_id == ready['host_id'] and connection.connection_id != first_connection
        snapshot = connection.call('snapshot')
        assert snapshot['revision'] > previous_revision
        recovered = snapshot['browsers'][0]
        assert recovered['browser_id'] == bid and recovered['state'] == 'manual' and not recovered['busy'], recovered
        assert recovered['selected_page_id'] == page
        assert recovered['pages'][0]['hwnd'] == str(hwnd) and recovered['pages'][0]['title'] == expected_title
        denied('control_unavailable', lambda: connection.call('acquire', browser_id=bid, client_id='after'))
        denied('invalid_token', lambda: connection.call('observe', browser_id=bid, token=token))
        click(hwnd, 101)  # Ordinary native handback, automated button input.
        handback = connection.events.get(timeout=5)
        assert handback['event'] == 'control_changed' and handback['detail']['reason'] == 'hand_back'
        assert handback['revision'] > snapshot['revision']
        fresh = connection.call('acquire', browser_id=bid, client_id='after')['token']
        assert fresh != token
        denied('observation_required', lambda: connection.call('act', browser_id=bid, token=fresh, action={'type': 'text', 'text': 'bad'}))
        view = connection.call('observe', browser_id=bid, token=fresh)
        assert view['page_id'] == page and view['title'] == expected_title
        connection.call('act', browser_id=bid, token=fresh, action={'type': 'text', 'text': '-resumed'})
        current = fixture.wait(lambda r: r['text'] == 'preserved-v06-resumed')
        assert current['cookie'] == state['cookie'] and current['storage'] == state['storage'] and current['documentId'] == state['documentId']
        assert len([r for r in fixture.records if r['name'] == 'recover-parent' and r['kind'] == 'loaded']) == 1
        print('PASS single-page live recovery')

        connection.call('act', browser_id=bid, token=fresh, action={'type': 'scroll', 'x': 500, 'y': 300, 'delta_y': -10000})
        fixture.wait(lambda r: r['name'] == 'recover-parent' and r['kind'] == 'scroll' and r['scroll'] == 0)
        connection.call('observe', browser_id=bid, token=fresh)
        connection.call('act', browser_id=bid, token=fresh, action={'type': 'click', 'x': 70, 'y': 85})
        listing = eventually(lambda: connection.call('list_pages', browser_id=bid),
            lambda value: len(value['pages']) == 2 and all(not p['loading'] for p in value['pages']), 'popup ready')
        selected = listing['selected_page_id']
        handles = {p['page_id']: p['hwnd'] for p in listing['pages']}
        popup_state = fixture.wait(lambda r: r['name'] == 'recover-parent-child1' and r['kind'] == 'loaded')
        assert popup_state['cookie'] == state['cookie'] and popup_state['storage'] == state['storage']
        other = connection.call('create', label='Recovery prototype B')
        token_b = connection.call('acquire', browser_id=other['browser_id'], client_id='independent')['token']
        connection.call('observe', browser_id=other['browser_id'], token=token_b)
        connection.call('act', browser_id=other['browser_id'], token=token_b, action={'type': 'navigate', 'url': fixture.url('independent')})
        isolated = fixture.wait(lambda r: r['name'] == 'independent' and r['kind'] == 'loaded')
        assert isolated['cookie'] != state['cookie'] and isolated['storage'] != state['storage']
        connection.disconnect(); connection = reconnect(ready['endpoint'], executable)
        snapshot = connection.call('snapshot')
        sessions = {s['browser_id']: s for s in snapshot['browsers']}
        assert set(sessions) == {bid, other['browser_id']}
        assert sessions[bid]['selected_page_id'] == selected
        assert {p['page_id']: p['hwnd'] for p in sessions[bid]['pages']} == handles
        assert all(s['state'] == 'manual' for s in sessions.values())
        click(int(handles[selected]), 101)
        shared = connection.call('acquire', browser_id=bid, client_id='shared-one')['token']
        denied('busy', lambda: connection.call('acquire', browser_id=bid, client_id='shared-two'))
        connection.call('release', browser_id=bid, token=shared)
        print('PASS browser/page identities, selection, shared lease and cross-browser isolation survive reconnect')

        # The next owner is a different OS process. It exits while navigation
        # request id 3 is in flight. A new connection deliberately reuses id 3.
        connection.disconnect(); connection = None
        child = subprocess.Popen([sys.executable, __file__, '--worker', ready['endpoint'], str(executable)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            creationflags=subprocess.CREATE_NO_WINDOW)
        assert json.loads(child.stdout.readline())['connected']
        click(int(handles[selected]), 101)
        owner_token = ask(child, 'acquire', browser_id=bid, client_id='crash-owner')['token']
        ask(child, 'observe', browser_id=bid, token=owner_token)
        ask(child, 'act', submit=True, browser_id=bid, token=owner_token,
            action={'type': 'navigate', 'url': fixture.url('unused').replace('/?name=unused', '/slow')})
        assert fixture.slow_started.wait(5)
        child.terminate(); child.wait(10)
        connection = reconnect(ready['endpoint'], executable, heartbeat=False)
        before = connection.call('snapshot')  # id 1
        interrupted = next(s for s in before['browsers'] if s['browser_id'] == bid)
        assert interrupted['busy'] and interrupted['state'] == 'taking_over', interrupted
        assert interrupted['interrupted_operation']['result_unknown']
        connection.call('ping')  # id 2
        new_creation = connection.submit('create', label='Request id collision sentinel')  # id 3
        fixture.slow_release.set()
        sentinel = new_creation.result(timeout=45)
        assert sentinel['browser_id'] not in sessions and sentinel['page_id'], sentinel
        after = eventually(lambda: connection.call('snapshot'),
            lambda value: not next(s for s in value['browsers'] if s['browser_id'] == bid)['busy'], 'old navigation settled')
        old = next(s for s in after['browsers'] if s['browser_id'] == bid)
        assert old['state'] == 'manual' and old['interrupted_operation']['settled']
        denied('invalid_token', lambda: connection.call('observe', browser_id=bid, token=owner_token))
        print('PASS actual owner process exit, in-flight result uncertainty, reused request id and late callback isolation')

        click(int(handles[selected]), 101)
        site_token = connection.call('acquire', browser_id=bid, client_id='site-check')['token']
        connection.call('observe', browser_id=bid, token=site_token)
        connection.call('act', browser_id=bid, token=site_token, action={'type': 'navigate', 'url': downloads.url()})
        connection.call('observe', browser_id=bid, token=site_token)
        try:
            connection.call('act', browser_id=bid, token=site_token, action={'type': 'navigate', 'url': downloads.url('/slow')})
        except Exception as error:
            assert getattr(error, 'code', None) == 'navigation_failed' and error.detail['web_error_status'] == 9, error
        eventually(lambda: connection.call('list_downloads', browser_id=bid),
            lambda value: value['downloads'] and value['downloads'][-1]['state'] == 'in_progress', 'download started')
        connection.disconnect(); connection = reconnect(ready['endpoint'], executable)
        site = next(s for s in connection.call('snapshot')['browsers'] if s['browser_id'] == bid)
        assert site['site_interactions']['downloads'][-1]['state'] == 'cancelled'
        assert site['site_interactions']['downloads'][-1]['reason'] == 'disconnected'
        assert site['state'] == 'manual'
        click(int(handles[selected]), 101)
        site_token = connection.call('acquire', browser_id=bid, client_id='site-check')['token']
        connection.call('observe', browser_id=bid, token=site_token)
        connection.call('act', browser_id=bid, token=site_token, action={'type': 'click', 'x': 80, 'y': 195})
        eventually(lambda: connection.call('snapshot'), lambda value:
            any(p['state'] == 'pending' for s in value['browsers'] if s['browser_id'] == bid for p in s['site_interactions']['permissions']), 'real permission waiting')
        connection.disconnect(); connection = reconnect(ready['endpoint'], executable)
        site = next(s for s in connection.call('snapshot')['browsers'] if s['browser_id'] == bid)
        assert site['state'] == 'manual' and site['site_interactions']['permissions'][-1]['state'] == 'denied'
        assert site['site_interactions']['permissions'][-1]['reason'] == 'disconnected'
        assert downloads.requests.count('/slow') == 1
        print('PASS disconnect cancels actual download and denies pending Runtime permission, no grant decision or replay', flush=True)

        connection.disconnect(); connection = None
        peer = RawPeer(ready['endpoint'])
        try:
            peer.handshake(); assert peer.receive()['event'] == 'ready'
            peer.write(b''.join((json.dumps({'id': str(i), 'method': 'snapshot'}) + '\n').encode() for i in range(1200)))
            time.sleep(.25)  # Deliberately never drain the full server output pipe.
            user32.SendMessageTimeoutW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM, W.UINT, W.UINT, C.POINTER(C.c_size_t)]
            user32.SendMessageTimeoutW.restype = W.LPARAM
            response = C.c_size_t()
            button = user32.GetDlgItem(int(handles[selected]), 101)
            assert button and user32.SendMessageTimeoutW(button, 0x00F5, 0, 0, 2, 2000, C.byref(response)), 'output backpressure blocked the native UI'
        finally: peer.close()
        connection = reconnect(ready['endpoint'], executable)
        assert all(s['state'] == 'manual' for s in connection.call('snapshot')['browsers'])
        print('PASS stalled output does not block native handback; bounded old input/output queues detach before reconnect', flush=True)

        original_connection = connection.connection_id
        time.sleep(32)
        assert connection.call('snapshot')['connection_id'] == original_connection
        connection.disconnect(); connection = None
        peer = RawPeer(ready['endpoint'])
        try:
            peer.handshake(); assert peer.receive()['event'] == 'ready'
            start = time.monotonic()
            time.sleep(15); peer.write(b'{')  # Half a frame cannot extend the lease on the connection.
            rejected(peer)
            assert 25 < time.monotonic() - start < 40
        finally: peer.close()
        connection = reconnect(ready['endpoint'], executable)
        after = connection.call('snapshot')
        assert all(s['state'] == 'manual' for s in after['browsers'])
        print('PASS heartbeat keeps idle owner alive; silent peer expires and cannot resume automation', flush=True)
        for session in after['browsers']:
            connection.call('close', browser_id=session['browser_id'])
        connection.disconnect(); connection = None
        process.wait(10)
        assert process.returncode == 0 and not Path(ready['endpoint']).exists()
        try: ReconnectClient(ready['endpoint'], executable)
        except ConnectionError: pass
        else: raise AssertionError('Closed endpoint was silently replaced')
        stale_endpoint_check(executable)
        print('PASS same live HWND/page/DOM/session, new connection, manual handback, stale token rejected, fresh observation required, no reload')
    finally:
        if connection: connection.disconnect()
        if child:
            if child.poll() is None: child.terminate(); child.wait(10)
            child.stdin.close(); child.stdout.close(); child.stderr.close()
        fixture.close()
        downloads.close()
        process.stdin.close()
        if process.poll() is None:
            process.terminate()  # This test owns this empty host process.
        process.wait(10)
        if ready:
            endpoint = Path(ready['endpoint'])
            if endpoint.is_file():
                assert endpoint.name == 'endpoint.json' and endpoint.parent.name == ready['host_id']
                endpoint.unlink(); endpoint.parent.rmdir()


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--worker': worker(sys.argv[2], sys.argv[3])
    else: main()
