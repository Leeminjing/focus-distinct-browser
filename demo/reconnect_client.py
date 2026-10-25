"""Stdlib Windows client for the opt-in, authenticated local recovery transport.

The caller persists the bootstrap endpoint path and supplies its trusted EXE.
No lease, action, or failed request is ever retried by this client.
"""
from concurrent.futures import Future
import base64
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import queue
import threading
import time
from client import BrowserError

K, A, D = C.WinDLL('kernel32', use_last_error=True), C.WinDLL('advapi32', use_last_error=True), C.WinDLL('crypt32', use_last_error=True)
INVALID = C.c_void_p(-1).value


class OVERLAPPED(C.Structure):
    _fields_ = [('Internal', C.c_size_t), ('InternalHigh', C.c_size_t), ('Offset', W.DWORD), ('OffsetHigh', W.DWORD), ('hEvent', W.HANDLE)]


class BLOB(C.Structure):
    _fields_ = [('size', W.DWORD), ('data', C.c_void_p)]


class SID_ENTRY(C.Structure):
    _fields_ = [('sid', C.c_void_p), ('attributes', W.DWORD)]


class GROUPS(C.Structure):
    _fields_ = [('count', W.DWORD), ('groups', SID_ENTRY * 1)]


for library, name, args, result in [
    (K, 'CreateFileW', [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE], W.HANDLE),
    (K, 'CloseHandle', [W.HANDLE], W.BOOL),
    (K, 'CreateEventW', [C.c_void_p, W.BOOL, W.BOOL, W.LPCWSTR], W.HANDLE),
    (K, 'ReadFile', [W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p], W.BOOL),
    (K, 'WriteFile', [W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p], W.BOOL),
    (K, 'WaitForSingleObject', [W.HANDLE, W.DWORD], W.DWORD),
    (K, 'CancelIoEx', [W.HANDLE, C.c_void_p], W.BOOL),
    (K, 'GetOverlappedResult', [W.HANDLE, C.POINTER(OVERLAPPED), C.POINTER(W.DWORD), W.BOOL], W.BOOL),
    (K, 'OpenProcess', [W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
    (K, 'GetCurrentProcess', [], W.HANDLE),
    (K, 'GetProcessTimes', [W.HANDLE, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p], W.BOOL),
    (K, 'QueryFullProcessImageNameW', [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)], W.BOOL),
    (K, 'GetNamedPipeServerProcessId', [W.HANDLE, C.POINTER(W.ULONG)], W.BOOL),
    (K, 'GetFileInformationByHandle', [W.HANDLE, C.c_void_p], W.BOOL),
    (K, 'LocalFree', [C.c_void_p], C.c_void_p),
    (A, 'OpenProcessToken', [W.HANDLE, W.DWORD, C.POINTER(W.HANDLE)], W.BOOL),
    (A, 'GetTokenInformation', [W.HANDLE, C.c_int, C.c_void_p, W.DWORD, C.POINTER(W.DWORD)], W.BOOL),
    (A, 'GetSecurityInfo', [W.HANDLE, C.c_int, W.DWORD, C.c_void_p, C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p, C.POINTER(C.c_void_p)], W.DWORD),
    (A, 'GetSecurityDescriptorControl', [C.c_void_p, C.POINTER(W.WORD), C.POINTER(W.DWORD)], W.BOOL),
    (A, 'GetAce', [C.c_void_p, W.DWORD, C.POINTER(C.c_void_p)], W.BOOL),
    (A, 'EqualSid', [C.c_void_p, C.c_void_p], W.BOOL),
    (D, 'CryptUnprotectData', [C.POINTER(BLOB), C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p, W.DWORD, C.POINTER(BLOB)], W.BOOL),
]:
    fn = getattr(library, name); fn.argtypes, fn.restype = args, result


def checked(value):
    if not value:
        raise C.WinError(C.get_last_error())
    return value


def logon():
    token, size = W.HANDLE(), W.DWORD()
    checked(A.OpenProcessToken(K.GetCurrentProcess(), 8, C.byref(token)))
    try:
        A.GetTokenInformation(token, 28, None, 0, C.byref(size))  # TokenLogonSid
        buffer = C.create_string_buffer(size.value)
        checked(A.GetTokenInformation(token, 28, buffer, size, C.byref(size)))
        assert GROUPS.from_buffer(buffer).count == 1
        return buffer
    finally:
        K.CloseHandle(token)


def private_file(path, directory=False):
    handle = K.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x02200000, None)
    if handle == INVALID:
        raise C.WinError(C.get_last_error())
    descriptor, acl = C.c_void_p(), C.c_void_p()
    try:
        info = C.create_string_buffer(64)
        checked(K.GetFileInformationByHandle(handle, info))
        if W.DWORD.from_buffer(info).value & 0x400:
            raise PermissionError('Recovery endpoint cannot be a reparse point')
        status = A.GetSecurityInfo(handle, 1, 4, None, None, C.byref(acl), None, C.byref(descriptor))
        if status:
            raise C.WinError(status)
        control, revision = W.WORD(), W.DWORD()
        checked(A.GetSecurityDescriptorControl(descriptor, C.byref(control), C.byref(revision)))
        if not acl.value or not control.value & 0x1000:
            raise PermissionError('Endpoint must have an explicit protected DACL')
        identity = logon()
        count = W.WORD.from_address(acl.value + 4).value
        if count != 1:
            raise PermissionError('Endpoint must grant only this logon access')
        ace = C.c_void_p(); checked(A.GetAce(acl, 0, C.byref(ace)))
        if C.c_ubyte.from_address(ace.value).value != 0 or not A.EqualSid(ace.value + 8, GROUPS.from_buffer(identity).groups[0].sid):
            raise PermissionError('Endpoint belongs to a different logon')
        if directory:
            return None
        content, read = C.create_string_buffer(16385), W.DWORD()
        checked(K.ReadFile(handle, content, len(content), C.byref(read), None))
        if read.value > 16384:
            raise ValueError('Oversized recovery descriptor')
        return json.loads(content.raw[:read.value])
    finally:
        if descriptor.value: K.LocalFree(descriptor)
        K.CloseHandle(handle)


class ReconnectClient:
    def __init__(self, endpoint, executable, *, expected_sha256=None, heartbeat=True):
        self.endpoint, self.executable = Path(endpoint).absolute(), Path(executable).resolve()
        try:
            private_file(self.endpoint.parent, directory=True)
            description = private_file(self.endpoint)
        except FileNotFoundError as error:
            raise ConnectionError('Host endpoint is unavailable; no browser was recreated') from error
        if description['protocol_version'] != 1 or os.path.normcase(os.path.normpath(description['executable'])) != os.path.normcase(str(self.executable)):
            raise ConnectionError('Host executable/protocol does not match the trusted caller configuration')
        if expected_sha256 and hashlib.sha256(self.executable.read_bytes()).hexdigest() != expected_sha256:
            raise ConnectionError('Host artifact checksum does not match')
        self.host_id = description['host_id']
        self.connection_id = None
        if description['pipe_name'] != r'\\.\pipe\DistinctBrowser-' + self.host_id:
            raise ConnectionError('Invalid host endpoint')
        self._peer = K.OpenProcess(0x101000, False, description['pid'])
        if not self._peer:
            raise ConnectionError('Original host process is unavailable; no browser was recreated')
        self._pipe = None
        try:
            if K.WaitForSingleObject(self._peer, 0) != 258:
                raise ConnectionError('Original host process has exited; no browser was recreated')
            times = [C.c_ulonglong() for _ in range(4)]
            image, size = C.create_unicode_buffer(32768), W.DWORD(32768)
            try:
                checked(K.GetProcessTimes(self._peer, *(C.byref(t) for t in times)))
                checked(K.QueryFullProcessImageNameW(self._peer, 0, image, C.byref(size)))
            except OSError as error:
                raise ConnectionError('Original host process identity is unavailable') from error
            if str(times[0].value) != description['process_created'] or Path(image.value).resolve() != self.executable or K.WaitForSingleObject(self._peer, 0) != 258:
                raise ConnectionError('Original host is no longer running')
            self._pipe = K.CreateFileW(description['pipe_name'], 0x0012018b, 0, None, 3, 0x40000000, None)
            if self._pipe == INVALID:
                self._pipe = None
                raise C.WinError(C.get_last_error())
            pid = W.ULONG(); checked(K.GetNamedPipeServerProcessId(self._pipe, C.byref(pid)))
            if pid.value != description['pid']:
                raise ConnectionError('Pipe server identity does not match')
            protected = C.create_string_buffer(base64.b64decode(description['credential_dpapi'], validate=True))
            encrypted, plain = BLOB(len(protected) - 1, C.addressof(protected)), BLOB()
            checked(D.CryptUnprotectData(C.byref(encrypted), None, None, None, None, 1, C.byref(plain)))
            try:
                credential = C.string_at(plain.data, plain.size).decode('ascii')
            finally:
                K.LocalFree(plain.data)
            self._stopping = threading.Event()
            self._lock, self._write_lock = threading.Lock(), threading.Lock()
            self.events, self._pending, self._serial = queue.Queue(), {}, 0
            self._send({'host_id': self.host_id, 'credential': credential, 'protocol_version': 1})
            self._reader = threading.Thread(target=self._read, daemon=True); self._reader.start()
            try: ready = self.events.get(timeout=6)
            except queue.Empty as error: raise ConnectionError('Host handshake was rejected or timed out') from error
            if ready.get('event') != 'ready' or ready.get('host_id') != self.host_id:
                raise ConnectionError('Invalid recovered host greeting')
            self.connection_id = ready['connection_id']
            self._heartbeat = threading.Thread(target=self._ping, daemon=True) if heartbeat else None
            if self._heartbeat: self._heartbeat.start()
        except BaseException:
            if hasattr(self, '_stopping'): self.disconnect()
            else:
                if self._pipe: K.CloseHandle(self._pipe)
                K.CloseHandle(self._peer)
            raise

    def _io(self, writing, data=None):
        buffer = C.create_string_buffer(data) if writing else C.create_string_buffer(65536)
        count = W.DWORD()
        event = checked(K.CreateEventW(None, True, False, None))
        operation = OVERLAPPED(hEvent=event)
        try:
            ok = (K.WriteFile if writing else K.ReadFile)(self._pipe, buffer, len(data) if writing else len(buffer), C.byref(count), C.byref(operation))
            if not ok and C.get_last_error() == 997:
                deadline = time.monotonic() + (10 if writing else 60)
                while K.WaitForSingleObject(event, 25) != 0:
                    if self._stopping.is_set() or time.monotonic() > deadline or K.WaitForSingleObject(self._peer, 0) == 0:
                        K.CancelIoEx(self._pipe, C.byref(operation))
                        K.GetOverlappedResult(self._pipe, C.byref(operation), C.byref(count), True)
                        raise ConnectionError('Host connection ended')
                ok = K.GetOverlappedResult(self._pipe, C.byref(operation), C.byref(count), False)
            checked(ok)
            if not count.value: raise ConnectionError('Host pipe closed')
            return count.value if writing else buffer.raw[:count.value]
        finally:
            K.CloseHandle(event)

    def _send(self, message):
        with self._write_lock:
            data = (json.dumps(message, ensure_ascii=False) + '\n').encode('utf-8')
            if len(data) - 1 > 1024 * 1024:
                raise ValueError('Request exceeds the host frame limit')
            while data:
                data = data[self._io(True, data):]

    def _read(self):
        data = b''
        try:
            while not self._stopping.is_set():
                data += self._io(False)
                if len(data) > 36 * 1024 * 1024: raise ValueError('Oversized host response')
                while b'\n' in data:
                    line, data = data.split(b'\n', 1)
                    response = json.loads(line)
                    if response.get('host_id') != self.host_id:
                        raise ConnectionError('Response from a different host')
                    if self.connection_id is None:
                        if response.get('event') != 'ready': raise ConnectionError('Missing connection greeting')
                        self.connection_id = response['connection_id']
                    elif response.get('connection_id') != self.connection_id:
                        raise ConnectionError('Response from a different connection')
                    if 'event' in response: self.events.put(response)
                    else:
                        with self._lock: future = self._pending.pop(response['id'], None)
                        if future:
                            if response['ok']: future.set_result(response['result'])
                            else: future.set_exception(BrowserError(response['error']))
        except Exception:
            pass  # Pending callers receive connection loss below; no replay.
        finally:
            self._stopping.set()
            with self._lock:
                for future in self._pending.values(): future.set_exception(ConnectionError('Host disconnected; requests are not replayed'))
                self._pending.clear()

    def _ping(self):
        while not self._stopping.wait(5):
            try: self.call('ping')
            except Exception: return

    def submit(self, method, **params):
        with self._lock:
            if self._stopping.is_set(): raise ConnectionError('Client disconnected')
            self._serial += 1
            identity = str(self._serial); future = Future(); self._pending[identity] = future
        try: self._send({'id': identity, 'method': method, 'params': params})
        except BaseException:
            with self._lock: self._pending.pop(identity, None)
            raise
        return future

    def call(self, method, **params):
        return self.submit(method, **params).result(timeout=60)

    def disconnect(self):
        self._stopping.set()
        if self._pipe: K.CancelIoEx(self._pipe, None)
        for name in ('_reader', '_heartbeat'):
            thread = getattr(self, name, None)
            if thread and thread is not threading.current_thread(): thread.join(12)
        if self._pipe: K.CloseHandle(self._pipe); self._pipe = None
        if self._peer: K.CloseHandle(self._peer); self._peer = None
