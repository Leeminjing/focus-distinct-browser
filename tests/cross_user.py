"""Opt-in real local-user isolation. Default never elevates or creates accounts.

Exit 0: verified (or explicitly labelled owner-only self-test); 77: not run;
1: failed, including failed cleanup. Passwords never leave the native broker.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]


def validate_broker(result, owner, nonce):
    """Public evidence contract; busy/missing/unauthenticated runs never pass."""
    probe = result.get('probe') or {}
    expected = result.get('expected_user_sid')
    checks = {
        'broker_executed': result.get('status') == 'executed',
        'real_other_user': bool(expected and expected != owner.get('user_sid')
            and result.get('actual_process_sid') == expected == probe.get('user_sid')),
        'different_logon': bool(probe.get('logon_sid') and probe.get('authentication_id')
            and probe['logon_sid'] != owner.get('logon_sid')
            and probe['authentication_id'] != owner.get('authentication_id')),
        'standard_user': probe.get('administrator') is False and probe.get('elevated') is False,
        'probe_control': probe.get('control_readable') is True and probe.get('nonce') == nonce,
        'endpoint_denied': type(probe.get('endpoint_error')) is int and probe['endpoint_error'] == 5,
        'pipe_denied': type(probe.get('pipe_error')) is int and probe['pipe_error'] == 5,
        'account_created_and_deleted': result.get('account_created') is True and result.get('account_deleted') is True,
        'profile_cleanup': result.get('profile_removed') is True,
        'staging_cleanup': result.get('stage_removed') is True,
    }
    failures = [name for name, passed in checks.items() if not passed]
    if failures: raise ValueError('Cross-user evidence rejected: ' + ', '.join(failures))
    return checks


def final_path(path):
    from reconnect_client import K, INVALID, checked
    K.GetFinalPathNameByHandleW.argtypes = [W.HANDLE, W.LPWSTR, W.DWORD, W.DWORD]
    K.GetFinalPathNameByHandleW.restype = W.DWORD
    handle = K.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x02200000, None)
    if handle == INVALID: raise C.WinError(C.get_last_error())
    try:
        name = C.create_unicode_buffer(32768)
        length = checked(K.GetFinalPathNameByHandleW(handle, name, len(name), 0))
        if length >= len(name) or not name.value.startswith('\\\\?\\'): raise ValueError('Unsupported endpoint final path')
        return Path(name.value[4:])
    finally: K.CloseHandle(handle)


def allow_broker_report(path, owner_sid):
    from reconnect_client import A, K, checked
    A.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [W.LPCWSTR, W.DWORD, C.POINTER(C.c_void_p), C.c_void_p]
    A.SetFileSecurityW.argtypes = [W.LPCWSTR, W.DWORD, C.c_void_p]
    descriptor = C.c_void_p()
    sddl = f'D:P(A;;FA;;;{owner_sid})(A;;FA;;;BA)(A;;FA;;;SY)'
    checked(A.ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, 1, C.byref(descriptor), None))
    try: checked(A.SetFileSecurityW(str(path), 0x80000004, descriptor))
    finally: K.LocalFree(descriptor)


def elevated_broker(executable, arguments):
    from reconnect_client import K, checked
    class EXECUTE(C.Structure):
        _fields_ = [('size', W.DWORD), ('mask', W.ULONG), ('window', W.HWND), ('verb', W.LPCWSTR),
            ('file', W.LPCWSTR), ('parameters', W.LPCWSTR), ('directory', W.LPCWSTR), ('show', C.c_int),
            ('instance', W.HINSTANCE), ('id_list', C.c_void_p), ('class_name', W.LPCWSTR), ('class_key', W.HKEY),
            ('hot_key', W.DWORD), ('icon', W.HANDLE), ('process', W.HANDLE)]
    shell = C.WinDLL('shell32', use_last_error=True)
    shell.ShellExecuteExW.argtypes = [C.POINTER(EXECUTE)]
    shell.ShellExecuteExW.restype = W.BOOL
    operation = EXECUTE(size=C.sizeof(EXECUTE), mask=0x140, verb='runas', file=str(executable),
        parameters=subprocess.list2cmdline(arguments), directory=str(executable.parent), show=0)
    checked(shell.ShellExecuteExW(C.byref(operation)))
    try:
        # A pending approval/blocked broker is never killed during account
        # cleanup. The broker bounds its own worker and always runs cleanup.
        interrupted = False
        while True:
            try:
                if K.WaitForSingleObject(operation.process, 250) != 258: break
            except KeyboardInterrupt:
                interrupted = True
                print('Waiting for the broker to finish temporary-account cleanup before cancelling.', flush=True)
        K.GetExitCodeProcess.argtypes = [W.HANDLE, C.POINTER(W.DWORD)]
        code = W.DWORD(); checked(K.GetExitCodeProcess(operation.process, C.byref(code)))
        return code.value, interrupted
    finally: K.CloseHandle(operation.process)


def remove_owned_profile(endpoint, browser_id):
    # Exactly the UUID returned by our fresh host, beneath its actual data root.
    # Reject reparse points before recursive deletion; never touch other IDs.
    assert str(uuid.UUID(browser_id)).upper() == browser_id
    root = endpoint.parents[2] / 'Profiles'
    target = root / browser_id
    if not target.exists(): return
    for item in (root, target):
        if item.lstat().st_file_attributes & 0x400: raise ValueError('Profile cleanup refuses reparse points')
    absolute = target.resolve()
    if absolute.parent != root.resolve() or absolute.name != browser_id: raise ValueError('Profile cleanup escaped owned directory')
    for parent, dirs, files in os.walk(absolute, followlinks=False):
        for name in dirs + files:
            if (Path(parent) / name).lstat().st_file_attributes & 0x400: raise ValueError('Profile contains a reparse point')
    for attempt in range(30):
        try: shutil.rmtree(absolute); return
        except PermissionError:
            if attempt == 29: raise
            time.sleep(.1)


def run(options, report):
    sys.path[:0] = [str(ROOT / 'demo'), str(ROOT / 'tests')]
    from reconnect_client import ReconnectClient, private_file
    from recovery import RECOVERY_HTML, reconnect
    from pages import Fixture
    tools = options.tools.resolve() if options.tools else options.executable.parent
    worker = tools / 'cross-user-worker.exe'
    broker = tools / 'cross-user-runner.exe'
    if not worker.is_file() or not broker.is_file(): raise ValueError('Build the cross-user test tools first')
    report['tools_sha256'] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (worker, broker)}
    nonce = uuid.uuid4().hex
    stage = ROOT / 'artifacts' / 'cross-user' / nonce
    stage.mkdir(parents=True)
    control, owner_result, broker_result = [stage / name for name in ('control.txt', 'owner.json', 'broker.json')]
    control.write_text(nonce, encoding='ascii'); owner_result.touch(); broker_result.touch()
    process = fixture = connection = browser_id = actual_endpoint = endpoint = description = None
    try:
        process = subprocess.Popen([str(options.executable), '--reconnect'], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
        fixture = Fixture(RECOVERY_HTML)
        ready_frames = queue.Queue()
        threading.Thread(target=lambda: ready_frames.put(process.stdout.readline()), daemon=True).start()
        ready = json.loads(ready_frames.get(timeout=15))
        endpoint = Path(ready['endpoint']); description = private_file(endpoint)
        assert ready['pid'] == description['pid'] == process.pid and description['host_id'] == ready['host_id']
        assert endpoint.name == 'endpoint.json' and endpoint.parent.name == ready['host_id']
        assert Path(description['executable']).resolve() == options.executable
        actual_endpoint = final_path(endpoint)
        connection = reconnect(endpoint, options.executable)
        created = connection.call('create', label='Cross-user security acceptance')
        browser_id = created['browser_id']
        params = {'browser_id': browser_id, 'token': connection.call('acquire', browser_id=browser_id, client_id='security-owner')['token']}
        connection.call('observe', **params)
        connection.call('act', **params, action={'type': 'navigate', 'url': fixture.url('security-owner')})
        connection.call('observe', **params)
        connection.call('act', **params, action={'type': 'click', 'x': 70, 'y': 35})
        connection.call('act', **params, action={'type': 'text', 'text': 'preserve-' + nonce[:8]})
        original = fixture.wait(lambda row: row.get('text') == 'preserve-' + nonce[:8])
        before = connection.call('snapshot')['browsers'][0]
        connection.disconnect(); connection = None
        subprocess.run([str(worker), '--probe', str(actual_endpoint), description['pipe_name'], str(control), str(owner_result), nonce],
            check=True, timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
        owner = json.loads(owner_result.read_text(encoding='utf-8'))
        assert owner['endpoint_error'] == owner['pipe_error'] == 0 and owner['control_readable'] and owner['nonce'] == nonce
        report['owner_positive_controls'] = True
        if options.self_test:
            # Same-user controls must be rejected by the cross-user contract.
            try: validate_broker({'probe': owner}, owner, nonce)
            except ValueError: pass
            else: raise AssertionError('Same-user evidence incorrectly certified')
            report['status'] = 'self_test_passed'
        else:
            allow_broker_report(broker_result, owner['user_sid'])
            arguments = ['--run', str(actual_endpoint), description['pipe_name'], str(final_path(broker_result)), nonce, owner['user_sid']]
            interrupted = False
            if options.elevate: code, interrupted = elevated_broker(broker, arguments)
            else:
                # Do not terminate a privileged account-cleanup controller on
                # Ctrl+C. Its exact worker handle has a native 30-second bound.
                with subprocess.Popen([str(broker), *arguments], creationflags=subprocess.CREATE_NO_WINDOW) as controller:
                    while True:
                        try: code = controller.wait(timeout=.25); break
                        except subprocess.TimeoutExpired: pass
                        except KeyboardInterrupt: interrupted = True
            evidence = json.loads(broker_result.read_text(encoding='utf-8'))
            report['broker'] = evidence
            if interrupted: raise InterruptedError('Cancelled after broker cleanup')
            if code == 77 and evidence.get('status') == 'not_run':
                report.update(status='not_run', reason=evidence.get('reason'))
            else:
                assert code == 0, 'Elevated broker failed; inspect cleanup fields in report'
                report['checks'] = validate_broker(evidence, owner, nonce)
                report['status'] = 'passed'
        connection = reconnect(endpoint, options.executable)
        after = connection.call('snapshot')['browsers'][0]
        for key in ('browser_id', 'selected_page_id'): assert after[key] == before[key]
        for key in ('page_id', 'hwnd', 'url', 'title'): assert after['pages'][0][key] == before['pages'][0][key]
        assert after['state'] == 'manual'
        assert original['documentId'] == next(row['documentId'] for row in reversed(fixture.records) if row.get('name') == 'security-owner')
        report['owner_reconnected_same_page'] = True
    finally:
        cleanup_errors = []
        try:
            if browser_id:
                if connection is None: connection = reconnect(endpoint, options.executable)
                connection.call('close', browser_id=browser_id)
        except Exception:
            cleanup_errors.append('graceful_native_close')
        finally:
            if connection: connection.disconnect()
            if process:
                if process.poll() is None:
                    try: process.wait(timeout=10)
                    except subprocess.TimeoutExpired: process.terminate(); process.wait(10)
                process.stdout.close(); process.stderr.close()
            if fixture: fixture.close()
            try:
                if browser_id and actual_endpoint: remove_owned_profile(actual_endpoint, browser_id)
                if actual_endpoint and endpoint.exists():
                    if private_file(endpoint) != description or final_path(endpoint) != actual_endpoint:
                        raise ValueError('Unexpected residual endpoint identity')
                    endpoint.unlink(); endpoint.parent.rmdir()
            except Exception: cleanup_errors.append('owned_profile_or_endpoint')
            for path in (control, owner_result, broker_result):
                try: path.unlink(missing_ok=True)
                except OSError: cleanup_errors.append('owned_staging_file')
            try: stage.rmdir()
            except OSError: cleanup_errors.append('owned_staging_directory')
        report['native_test_resources_removed'] = not cleanup_errors
        if cleanup_errors: raise RuntimeError('Test cleanup failed: ' + ', '.join(cleanup_errors))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, default=ROOT / 'build/distinct-browser.exe')
    parser.add_argument('--report', type=Path, default=ROOT / 'artifacts/cross-user-security.json')
    parser.add_argument('--tools', type=Path, help='Directory of native test tools, when testing a separately packaged EXE')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--elevate', action='store_true', help='Request one Windows UAC approval for the narrow native broker')
    mode.add_argument('--self-test', action='store_true', help='Owner-only controls; never certifies cross-user isolation or creates accounts')
    options = parser.parse_args(); options.executable = options.executable.resolve()
    report = {'status': 'not_run', 'cross_user_verified': False, 'physical_input': False}
    try:
        if os.name != 'nt': report['reason'] = 'windows_required'
        elif not options.self_test and not options.elevate and not C.windll.shell32.IsUserAnAdmin():
            report['reason'] = 'administrator_required_use_elevate'
        else:
            report['executable_sha256'] = hashlib.sha256(options.executable.read_bytes()).hexdigest()
            run(options, report)
    except OSError as error:
        report.update(status='not_run' if getattr(error, 'winerror', None) == 1223 else 'failed',
            reason='elevation_cancelled' if getattr(error, 'winerror', None) == 1223 else type(error).__name__)
    except Exception as error:
        report.update(status='failed', reason=str(error))
    except KeyboardInterrupt:
        report.update(status='not_run', reason='cancelled_before_broker')
    report['cross_user_verified'] = report['status'] == 'passed'
    options.report.parent.mkdir(parents=True, exist_ok=True)
    options.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"{report['status']}: cross_user_verified={report['cross_user_verified']}; report={options.report.resolve()}")
    return 0 if report['status'] in ('passed', 'self_test_passed') else 77 if report['status'] == 'not_run' else 1


if __name__ == '__main__': sys.exit(main())
