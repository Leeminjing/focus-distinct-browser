"""Visible-window Win11/WebView2 baseline. All generated input is CDP automation.

Run from the repository root: python benchmarks/concurrency.py --help
Only a fresh, benchmark-owned host is manipulated. No existing Focus is touched.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, TimeoutError as FutureTimeout
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import stat
import subprocess
import sys
import threading
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'demo'))
from client import BrowserError
from reconnect_client import ReconnectClient
from workload import Fixture, HTML
from reporting import summarize, resources, write_csv, distribution
import windows_metrics as win

GIB = 1024**3


class ResourceLimit(RuntimeError): pass


def guard(system, reserve_gib=2, reserve_fraction=.15):
    floor = max(reserve_gib * GIB, reserve_fraction * system['total_physical_bytes'])
    if system['available_physical_bytes'] < floor or system['available_commit_bytes'] < 2 * GIB:
        raise ResourceLimit(f'Available physical {system["available_physical_bytes"]} < reserve {int(floor)}, '
                            f'or available commit {system["available_commit_bytes"]} < {2 * GIB}')


def remove_profile(endpoint, browser_id):
    """Refuse unexpected roots, IDs and reparse points, including tree contents."""
    if str(uuid.UUID(browser_id)).upper() != browser_id: raise ValueError('Noncanonical browser ID')
    if endpoint.name != 'endpoint.json' or endpoint.parent.parent.name != 'Hosts':
        raise ValueError('Unexpected endpoint layout')
    root = endpoint.parent.parent.parent
    if root.name != 'DistinctBrowser': raise ValueError('Unexpected data root')
    target = root / 'Profiles' / browser_id
    for parent in (root, target.parent):
        if parent.is_symlink() or parent.stat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError('Reparse point in profile root')
    if not target.exists(): return
    for folder, dirs, files in os.walk(target, followlinks=False):
        for path in [Path(folder)] + [Path(folder) / n for n in dirs + files]:
            if path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                raise ValueError('Reparse point in profile tree')
    if target.resolve().parent != target.parent.resolve(): raise ValueError('Profile escaped root')
    for attempt in range(30):
        try: shutil.rmtree(target); return
        except PermissionError:
            if attempt == 29: raise
            time.sleep(.1)


class Case:
    def __init__(self, args, count, pages, directory, sha):
        self.args, self.count, self.pages, self.directory, self.sha = args, count, pages, directory, sha
        self.requests = []; self.lock = threading.Lock(); self.sampler = None
        self.client = self.process = self.fixture = self.endpoint = None
        self.browsers = []; self.tokens = {}; self.windows = []; self.viewports = []
        self.result = {'browsers': count, 'pages_per_browser': pages, 'status': 'starting',
                       'requests': self.requests, 'windows': self.windows, 'viewports': self.viewports}

    def check(self):
        if self.sampler and self.sampler.abort.is_set():
            raise ResourceLimit(str(self.sampler.reason))

    def wait(self, future, cleanup=False):
        deadline = time.monotonic() + self.args.timeout
        while True:
            if not cleanup: self.check()
            try: return future.result(timeout=.1)
            except FutureTimeout:
                if time.monotonic() > deadline: raise TimeoutError('RPC deadline; no request replayed')

    def call(self, operation, method=None, cleanup=False, **params):
        start = time.perf_counter()
        row = {'operation': operation, 'browser_id': params.get('browser_id', ''),
               'page_id': params.get('page_id', ''), 'ok': False}
        try:
            if not cleanup: self.check()
            value = self.wait(self.client.submit(method or operation, **params), cleanup=cleanup)
            row['ok'] = True
            if 'image' in value:
                image = value['image']; encoded = image['base64']
                row['base64_bytes'] = len(encoded)
                row['png_bytes'] = len(encoded) // 4 * 3 - (len(encoded) - len(encoded.rstrip('=')))
                row['image_pixels'] = [image['width'], image['height']]
                with self.lock:
                    if value['viewport'] not in self.viewports: self.viewports.append(value['viewport'])
            return value
        except Exception as e:
            row['error'] = e.code if isinstance(e, BrowserError) else type(e).__name__
            row['outcome_unknown'] = isinstance(e, (ResourceLimit, TimeoutError, ConnectionError))
            raise
        finally:
            row['ms'] = (time.perf_counter() - start) * 1000
            with self.lock: self.requests.append(row)

    def phase(self, name):
        self.sampler.phase = name
        print(f'{self.count} browsers x {self.pages} pages: {name}', flush=True)

    def parallel(self, work, items, workers=None):
        with ThreadPoolExecutor(max_workers=workers or self.count) as pool:
            futures = [pool.submit(work, item) for item in items]
            # Every submitted operation settles or times out; never leave background mutations.
            outcomes = []; errors = []
            for future in futures:
                try: outcomes.append(future.result())
                except Exception as error: errors.append(error)
            if errors: raise errors[0]
            return outcomes

    def connect(self):
        deadline = time.monotonic() + 8
        while True:
            self.check()
            try: return ReconnectClient(self.endpoint, self.args.exe, expected_sha256=self.sha)
            except OSError as e:
                if e.winerror != 231 or time.monotonic() >= deadline: raise
                time.sleep(.025)  # Retry connection-busy only, never an accepted action.

    def listed(self, bid):
        return self.call('list_pages', browser_id=bid)

    def ready(self, bid, expected):
        deadline = time.monotonic() + self.args.timeout
        while True:
            value = self.listed(bid)
            if len(value['pages']) == expected and all(p['ready'] and not p['loading'] and
                    p['url'].startswith(self.fixture.url('').split('?')[0]) for p in value['pages']):
                return value
            if time.monotonic() > deadline: raise TimeoutError('Pages not ready')
            self.check(); time.sleep(.05)

    def observe(self, bid, page=None, operation='observe'):
        return self.call(operation, 'observe', browser_id=bid, token=self.tokens[bid],
                         **({'page_id': page} if page else {}))

    def act(self, bid, kind, operation=None, page=None, **fields):
        return self.call(operation or kind, 'act', browser_id=bid, token=self.tokens[bid],
                         action={'type': kind, **fields}, **({'page_id': page} if page else {}))

    def create(self, index):
        start = time.perf_counter(); label = f'bench-{index}'
        made = self.call('create', label=label); bid = made['browser_id']
        with self.lock: self.browsers.append(bid)
        window = win.configure_window(made['hwnd'], self.args.width, self.args.height)
        if not window['visible'] or window['minimized']: raise AssertionError('Window is not visible')
        with self.lock: self.windows.append(window)
        self.tokens[bid] = self.call('acquire', browser_id=bid, client_id=label, ttl_ms=300000)['token']
        self.observe(bid, operation='initial_blank_observe')
        self.act(bid, 'navigate', url=self.fixture.url(label))
        self.ready(bid, 1); self.fixture.wait(label)
        self.observe(bid, operation='first_fixture_observe')
        with self.lock: self.requests.append({'operation': 'create_to_first_fixture_observe',
            'browser_id': bid, 'ok': True, 'ms': (time.perf_counter() - start) * 1000})
        for _ in range(1, self.pages):
            before = self.listed(bid)
            self.observe(bid, operation='popup_setup_observe')
            try: self.act(bid, 'click', operation='open_page', x=80, y=150)
            except BrowserError as e:
                # New page selection can interrupt remaining CDP mouse steps.
                if e.code != 'page_changed': raise
            listing = self.ready(bid, len(before['pages']) + 1)
            for p in listing['pages']:
                window = win.configure_window(p['hwnd'], self.args.width, self.args.height)
                with self.lock:
                    if not any(w['hwnd'] == window['hwnd'] for w in self.windows): self.windows.append(window)
        listing = self.ready(bid, self.pages)
        states = [self.fixture.wait(p['url'].split('name=')[1]) for p in listing['pages']]
        if len({s['cookie'] for s in states}) != 1 or len({s['storage'] for s in states}) != 1:
            raise AssertionError('Pages do not share browser session')
        return {'bid': bid, 'pages': [p['page_id'] for p in listing['pages']],
                'labels': [s['label'] for s in states], 'cookie': states[0]['cookie']}

    def round(self, browser, iteration, warmup=False):
        bid = browser['bid']; prefix = 'warmup_' if warmup else ''
        self.call('renew', browser_id=bid, token=self.tokens[bid], ttl_ms=300000)
        for page, label in zip(browser['pages'], browser['labels']):
            if self.pages > 1:
                self.call(prefix+'select_page', 'select_page', browser_id=bid, token=self.tokens[bid], page_id=page)
            self.observe(bid, page, prefix+'observe')
            before = self.fixture.wait(label)
            self.act(bid, 'click', operation=prefix+'click', page=page, x=80, y=95)
            self.act(bid, 'click', operation=prefix+'focus_input', page=page, x=80, y=35)
            self.act(bid, 'text', operation=prefix+'text', page=page, text='x')
            self.fixture.wait(label, lambda s: s['clicks'] == before['clicks']+1 and len(s['text']) == len(before['text'])+1)

    @staticmethod
    def identity(snapshot):
        return {b['browser_id']: {'selected_page_id': b['selected_page_id'],
            'pages': sorted(({k: p[k] for k in ('page_id','hwnd','url','title')} for p in b['pages']), key=lambda p:p['page_id'])}
                for b in snapshot['browsers']}

    def recover(self):
        self.phase('reconnect')
        before = self.call('snapshot'); documents = dict(self.fixture.states)
        old_connection = self.client.connection_id
        self.client.disconnect(); self.client = None
        start = time.perf_counter(); self.client = self.connect()
        handshake = (time.perf_counter()-start)*1000
        after = self.call('snapshot')
        self.result['reconnect'] = {'handshake_ms': handshake,
            'handshake_snapshot_ms': (time.perf_counter()-start)*1000,
            'same_host': after['host_id'] == before['host_id'],
            'new_connection': self.client.connection_id != old_connection,
            'same_pages': self.identity(before) == self.identity(after),
            'all_manual': all(b['state']=='manual' and not b['busy'] for b in after['browsers']),
            'same_fixture_documents': documents == self.fixture.states}
        if not all(v for k,v in self.result['reconnect'].items() if not k.endswith('_ms')):
            raise AssertionError('Recovery changed identity/state')
        for bid, token in self.tokens.items():
            try: self.call('old_token_probe', 'observe', browser_id=bid, token=token)
            except BrowserError as e:
                if e.code != 'invalid_token': raise
            else: raise AssertionError('Old token remained valid')

    def cleanup(self):
        errors = []; forced = False;start=time.perf_counter()
        if self.sampler: self.sampler.phase='close'
        if not self.client and self.endpoint and self.process and self.process.poll() is None:
            try:
                # Cleanup bypasses workload resource guard; authenticates the exact owned host.
                self.client=ReconnectClient(self.endpoint,self.args.exe,expected_sha256=self.sha)
            except Exception as e:errors.append('Cleanup connection: '+type(e).__name__)
        if self.client:
            try:
                # Includes partially created browsers whose reply was interrupted by resource guard.
                snapshot = self.call('cleanup_snapshot', 'snapshot', cleanup=True)
                for browser in snapshot['browsers']:
                    bid = browser['browser_id']
                    if bid not in self.browsers: self.browsers.append(bid)
                    self.call('close', browser_id=bid, cleanup=True)
            except Exception as e: errors.append(type(e).__name__+': '+str(e))
            finally: self.client.disconnect(); self.client=None
        if self.process:
            try: self.process.wait(timeout=self.args.close_grace)
            except subprocess.TimeoutExpired:
                # Popen retains the exact process handle, never kills by name or reused PID.
                self.process.terminate(); self.process.wait(timeout=5); forced=True
            if self.sampler:
                deadline = time.monotonic()+self.args.close_grace
                while self.sampler.living_native() and time.monotonic()<deadline: time.sleep(.2)
                self.result['remaining_native_pids'] = [p.pid for p in self.sampler.living_native()]
                self.result['close_to_native_exit_ms']=(time.perf_counter()-start)*1000
                with self.sampler.lock:
                    self.sampler.phase='post_close';self.sampler.sample()
                self.sampler.stop()
                self.result['samples'] = self.sampler.rows
                self.result['sampling_errors'] = self.sampler.errors
                self.result['runtime_versions'] = sorted({v for v in self.sampler.runtime.values() if v})
                self.result['resources'] = resources(self.sampler.rows)
            for stream in (self.process.stdout, self.process.stderr):
                if stream: stream.close()
        self.result['forced_host_exit'] = forced
        if not self.result.get('remaining_native_pids') and hasattr(self, 'actual_endpoint'):
            for bid in self.browsers:
                try: remove_profile(self.actual_endpoint, bid)
                except Exception as e: errors.append('Profile cleanup: '+type(e).__name__+': '+str(e))
        self.result['cleanup_errors'] = errors
        self.result['created_browser_ids']=list(self.browsers)
        if self.fixture: self.fixture.close()

    def run(self):
        self.directory.mkdir(parents=True, exist_ok=True)
        try:
            guard(win.memory(),self.args.reserve_gib,self.args.reserve_fraction)
            self.fixture=Fixture(); env=os.environ.copy()
            env.pop('DISTINCT_BROWSER_PERF_TRACE', None)
            if self.args.trace: env['DISTINCT_BROWSER_PERF_TRACE']=str(self.directory/'native-trace.jsonl')
            self.process=subprocess.Popen([str(self.args.exe), '--reconnect'], stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW, env=env)
            self.sampler=win.Sampler(self.process.pid, self.args.sample_interval,
                min_available=self.args.reserve_gib*GIB,min_fraction=self.args.reserve_fraction);self.sampler.start()
            greeting=queue.Queue()
            threading.Thread(target=lambda:greeting.put(self.process.stdout.readline()), daemon=True).start()
            self.endpoint=Path(json.loads(greeting.get(timeout=10))['endpoint'])
            self.actual_endpoint=win.final_path(self.endpoint)
            self.client=self.connect()
            self.phase('create');start=time.perf_counter()
            browsers=self.parallel(self.create, range(self.count), workers=min(self.args.create_workers,self.count))
            self.result['batch_ready_ms']=(time.perf_counter()-start)*1000
            if len({b['cookie'] for b in browsers}) != self.count: raise AssertionError('Browser isolation failed')
            self.phase('settled')
            deadline=time.monotonic()+self.args.settle
            while time.monotonic()<deadline:self.check();time.sleep(.1)
            self.phase('warmup');self.parallel(lambda b:self.round(b, -1, True), browsers)
            self.phase('workload');start=time.perf_counter()
            for iteration in range(self.args.iterations):
                self.parallel(lambda b:self.round(b, iteration), browsers)
            self.result['workload_ms']=(time.perf_counter()-start)*1000
            self.recover();self.result['status']='completed'
        except ResourceLimit as e:
            self.result.update(status='resource_guard', reason=str(e))
        except Exception as e:
            self.result.update(status='failed', reason=type(e).__name__+': '+str(e))
            (self.directory/'failure.txt').write_text(traceback.format_exc(),encoding='utf-8')
        finally:
            try:self.cleanup()
            except Exception as e:self.result['cleanup_errors']=[type(e).__name__+': '+str(e)]
            if self.result.get('cleanup_errors') or self.result.get('remaining_native_pids') or self.result.get('forced_host_exit'):
                self.result['status']='cleanup_failed'
            self.result['latencies_ms']=summarize(self.requests)
            if self.args.trace:
                try:
                    traces=[json.loads(s) for s in (self.directory/'native-trace.jsonl').read_text().splitlines()]
                    if not traces:raise ValueError('Empty trace')
                    self.result['trace_us']={stage:distribution([r['elapsed_us'] for r in traces if r['stage']==stage])
                                             for stage in sorted({r['stage'] for r in traces})}
                except Exception as e:
                    self.result['trace_error']=str(e)
                    if self.result['status']=='completed':self.result['status']='trace_failed'
            (self.directory/'case.json').write_text(json.dumps(self.result, indent=2), encoding='utf-8')
            write_csv(self.directory/'requests.csv', self.requests,
                      ['operation','browser_id','page_id','ok','ms','error','outcome_unknown','base64_bytes','png_bytes','image_pixels'])
            flat=[]
            for sample in self.result.get('samples', []):
                for group, values in sample['groups'].items():
                    flat.append({'elapsed_s':sample['elapsed_s'],'phase':sample['phase'],'group':group,**values,
                                 **sample['system']})
            write_csv(self.directory/'resources.csv', flat, ['elapsed_s','phase','group','processes','memory_readable',
                'private_bytes','working_set_bytes','cpu_one_core_percent','cpu_capacity_percent',
                'available_physical_bytes','available_commit_bytes'])
        return self.result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=ROOT/'artifacts/releases/v0.6.2/distinct-browser.exe')
    parser.add_argument('--counts', nargs='+', type=int, default=[1,5,10,20])
    parser.add_argument('--pages', nargs='+', type=int, default=[1,3])
    parser.add_argument('--iterations', type=int, default=20)
    parser.add_argument('--create-workers', type=int, default=4)
    parser.add_argument('--width', type=int, default=960);parser.add_argument('--height', type=int, default=720)
    parser.add_argument('--sample-interval', type=float, default=.5)
    parser.add_argument('--settle', type=float, default=3)
    parser.add_argument('--timeout', type=float, default=45)
    parser.add_argument('--close-grace', type=float, default=8)
    parser.add_argument('--reserve-gib', type=float, default=2, help='Available physical reserve; minimum 1.5 GiB')
    parser.add_argument('--reserve-fraction', type=float, default=.15, help='Also reserve this fraction of physical RAM')
    parser.add_argument('--trace', action='store_true', help='Separate diagnostic build only; never label as release baseline')
    parser.add_argument('--output', type=Path)
    args=parser.parse_args();args.exe=args.exe.resolve()
    if any(n<1 or n>20 for n in args.counts) or any(p<1 or p>16 for p in args.pages):parser.error('counts 1..20; pages 1..16')
    if min(args.iterations,args.create_workers,args.sample_interval,args.timeout,args.close_grace)<=0:parser.error('Positive sampling/work/deadline values required')
    if args.width<700 or args.height<600:parser.error('Fixture requires at least 700x600 DIP')
    if args.reserve_gib<1.5 or not 0<=args.reserve_fraction<=1:parser.error('Reserve >=1.5 GiB and fraction 0..1')
    args.output=(args.output or ROOT/'artifacts/performance'/datetime.now().strftime('%Y%m%d-%H%M%S')).resolve()
    if args.output.exists():parser.error('Output directory must be new (trace/results are never appended across runs)')
    args.output.mkdir(parents=True)
    win.U.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    sha=hashlib.sha256(args.exe.read_bytes()).hexdigest()
    report={'schema_version':1,'started_utc':datetime.now(timezone.utc).isoformat(),
            'artifact':{'sha256':sha,'bytes':args.exe.stat().st_size,'file_version':win.version(args.exe)},
            'kind':'instrumented_diagnostic' if args.trace else 'release_baseline',
            'machine':win.machine(),'settings':{k:v for k,v in vars(args).items() if k not in ('exe','output')},
            'quantiles':'median; P95 nearest-rank; successes only; failures and sample counts retained',
            'fixture_sha256':hashlib.sha256(HTML).hexdigest(),
            'harness_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in
                [Path(__file__),ROOT/'benchmarks/windows_metrics.py',ROOT/'benchmarks/reporting.py',
                 ROOT/'benchmarks/workload.py',ROOT/'demo/reconnect_client.py',ROOT/'demo/client.py']},
            'cases':[]}
    metadata=args.exe.with_name('artifact.json')
    if metadata.is_file():
        packaged=json.loads(metadata.read_text(encoding='utf-8-sig'))
        if packaged.get('sha256')!=sha:raise ValueError('Adjacent artifact.json checksum mismatch')
        report['artifact']['source_commit']=packaged.get('source_commit')
    stopped=None
    for count in args.counts:
        for pages in args.pages:
            if stopped:
                report['cases'].append({'browsers':count,'pages_per_browser':pages,'status':'not_run',
                                        'reason':'Previous case stopped: '+stopped})
            else:
                case=Case(args,count,pages,args.output/f'{count}b-{pages}p',sha).run()
                report['cases'].append(case)
                print(f'RESULT {count}b/{pages}p {case["status"]}', flush=True)
                if case['status']!='completed':stopped=case['status']
            (args.output/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(str(args.output/'report.json'),flush=True)
    return 0 if not stopped else 77 if stopped=='resource_guard' else 1


if __name__=='__main__':sys.exit(main())
