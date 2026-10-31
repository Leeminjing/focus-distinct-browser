"""Stdlib Win32 accounting for one owned host and its WebView2 descendants."""
import ctypes as C
from ctypes import wintypes as W
import os
import platform
import threading
import time
import winreg
from pathlib import Path

K = C.WinDLL('kernel32', use_last_error=True)
U = C.WinDLL('user32', use_last_error=True)
V = C.WinDLL('version', use_last_error=True)
INVALID = C.c_void_p(-1).value

class Memory(C.Structure):
    _fields_ = [('length', W.DWORD), ('load', W.DWORD)] + [(n, C.c_ulonglong) for n in
        ('total_phys', 'avail_phys', 'total_commit', 'avail_commit', 'total_virtual', 'avail_virtual', 'avail_extended')]

class Counters(C.Structure):
    _fields_ = [('cb', W.DWORD), ('faults', W.DWORD)] + [(n, C.c_size_t) for n in
        ('peak_ws', 'working_set', 'peak_paged', 'paged', 'peak_nonpaged', 'nonpaged', 'commit', 'peak_commit', 'private')]

class ProcessEntry(C.Structure):
    _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('pid', W.DWORD), ('heap', C.c_size_t),
        ('module', W.DWORD), ('threads', W.DWORD), ('parent', W.DWORD), ('priority', W.LONG),
        ('flags', W.DWORD), ('name', W.WCHAR * 260)]

for lib, name, args, result in [
    (K,'OpenProcess',[W.DWORD,W.BOOL,W.DWORD],W.HANDLE),
    (K,'CloseHandle',[W.HANDLE],W.BOOL),
    (K,'WaitForSingleObject',[W.HANDLE,W.DWORD],W.DWORD),
    (K,'GetProcessTimes',[W.HANDLE,C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p],W.BOOL),
    (K,'QueryFullProcessImageNameW',[W.HANDLE,W.DWORD,W.LPWSTR,C.POINTER(W.DWORD)],W.BOOL),
    (K,'K32GetProcessMemoryInfo',[W.HANDLE,C.c_void_p,W.DWORD],W.BOOL),
    (K,'GlobalMemoryStatusEx',[C.POINTER(Memory)],W.BOOL),
    (K,'CreateToolhelp32Snapshot',[W.DWORD,W.DWORD],W.HANDLE),
    (K,'Process32FirstW',[W.HANDLE,C.POINTER(ProcessEntry)],W.BOOL),
    (K,'Process32NextW',[W.HANDLE,C.POINTER(ProcessEntry)],W.BOOL),
    (K,'CreateFileW',[W.LPCWSTR,W.DWORD,W.DWORD,C.c_void_p,W.DWORD,W.DWORD,W.HANDLE],W.HANDLE),
    (K,'GetFinalPathNameByHandleW',[W.HANDLE,W.LPWSTR,W.DWORD,W.DWORD],W.DWORD),
    (K,'TerminateProcess',[W.HANDLE,W.UINT],W.BOOL),
    (U,'SetProcessDpiAwarenessContext',[C.c_void_p],W.BOOL),
    (U,'GetDpiForWindow',[W.HWND],W.UINT),
    (U,'SetWindowPos',[W.HWND,W.HWND,C.c_int,C.c_int,C.c_int,C.c_int,W.UINT],W.BOOL),
    (U,'GetWindowRect',[W.HWND,C.POINTER(W.RECT)],W.BOOL),
    (U,'IsWindowVisible',[W.HWND],W.BOOL),
    (U,'IsIconic',[W.HWND],W.BOOL),
    (V,'GetFileVersionInfoSizeW',[W.LPCWSTR,C.POINTER(W.DWORD)],W.DWORD),
    (V,'GetFileVersionInfoW',[W.LPCWSTR,W.DWORD,W.DWORD,C.c_void_p],W.BOOL),
    (V,'VerQueryValueW',[C.c_void_p,W.LPCWSTR,C.POINTER(C.c_void_p),C.POINTER(W.UINT)],W.BOOL),
]:
    fn=getattr(lib,name); fn.argtypes,fn.restype=args,result

def check(ok):
    if not ok: raise C.WinError(C.get_last_error())
    return ok

def memory():
    m=Memory();m.length=C.sizeof(m);check(K.GlobalMemoryStatusEx(C.byref(m)))
    return {'total_physical_bytes':m.total_phys,'available_physical_bytes':m.avail_phys,
            'commit_limit_bytes':m.total_commit,'available_commit_bytes':m.avail_commit}

def version(path):
    unused=W.DWORD();size=V.GetFileVersionInfoSizeW(str(path),C.byref(unused))
    if not size:return None
    buf=C.create_string_buffer(size);check(V.GetFileVersionInfoW(str(path),0,size,buf))
    ptr=C.c_void_p();length=W.UINT();check(V.VerQueryValueW(buf,'\\',C.byref(ptr),C.byref(length)))
    values=C.cast(ptr,C.POINTER(W.DWORD))
    return '.'.join(map(str,(values[2]>>16,values[2]&65535,values[3]>>16,values[3]&65535)))

def machine():
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
        cpu=winreg.QueryValueEx(key,'ProcessorNameString')[0].strip()
    return {'os':platform.platform(),'windows_build':os.sys.getwindowsversion().build,'architecture':platform.machine(),
        'cpu':cpu,'logical_processors':os.cpu_count(),'memory':memory(),
        'cpu_percent_definition':'100% = one logical processor; capacity_percent divides by logical_processors',
        'memory_definition':'PrivateUsage = private commit; summed working sets may double-count shared pages'}

def configure_window(hwnd,width,height):
    hwnd=int(hwnd);dpi=check(U.GetDpiForWindow(hwnd));scale=dpi/96
    check(U.SetWindowPos(hwnd,None,20,20,round(width*scale),round(height*scale),0x0014))
    rect=W.RECT();check(U.GetWindowRect(hwnd,C.byref(rect)))
    return {'hwnd':str(hwnd),'dpi':dpi,'window_pixels':[rect.right-rect.left,rect.bottom-rect.top],
            'visible':bool(U.IsWindowVisible(hwnd)),'minimized':bool(U.IsIconic(hwnd))}

def final_path(path):
    handle=K.CreateFileW(str(path),0x80000000,1,None,3,0x02200000,None)
    if handle==INVALID:raise C.WinError(C.get_last_error())
    try:
        text=C.create_unicode_buffer(32768);n=check(K.GetFinalPathNameByHandleW(handle,text,len(text),0))
        if n>=len(text) or not text.value.startswith('\\\\?\\'):raise ValueError('Unsupported endpoint final path')
        return Path(text.value[4:])
    finally:K.CloseHandle(handle)

def process_table():
    snapshot=K.CreateToolhelp32Snapshot(2,0)
    if snapshot==INVALID:raise C.WinError(C.get_last_error())
    try:
        row=ProcessEntry();row.size=C.sizeof(row);rows={}
        ok=K.Process32FirstW(snapshot,C.byref(row))
        while ok:
            rows[row.pid]={'parent':row.parent,'name':row.name}
            ok=K.Process32NextW(snapshot,C.byref(row))
        return rows
    finally:K.CloseHandle(snapshot)

def times(handle):
    values=[C.c_ulonglong() for _ in range(4)]
    check(K.GetProcessTimes(handle,*(C.byref(v) for v in values)))
    return values[0].value,values[2].value+values[3].value

class Process:
    def __init__(self,pid,kind,parent=0):
        self.pid,self.kind,self.parent=pid,kind,parent
        self.handle=check(K.OpenProcess(0x101000,False,pid))
        try:
            self.birth,_=times(self.handle)
            image=C.create_unicode_buffer(32768);size=W.DWORD(len(image))
            check(K.QueryFullProcessImageNameW(self.handle,0,image,C.byref(size)))
            self.path=Path(image.value);self.previous=None
        except BaseException:K.CloseHandle(self.handle);raise
    @property
    def alive(self):return K.WaitForSingleObject(self.handle,0)==258
    def sample(self):
        birth,cpu=times(self.handle);assert birth==self.birth
        # New native children have done startup work before discovery. Charge
        # their lifetime CPU to the discovery interval rather than losing it.
        # The already-running Python client starts accounting at its first sample.
        delta=(0 if self.kind=='client' else cpu) if self.previous is None else max(0,cpu-self.previous)
        self.previous=cpu
        result={'pid':self.pid,'parent_pid':self.parent,'birth_100ns':birth,'kind':self.kind,
            'alive':self.alive,'cpu_total_100ns':cpu,'cpu_delta_100ns':delta}
        if result['alive']:
            m=Counters();m.cb=C.sizeof(m)
            if K.K32GetProcessMemoryInfo(self.handle,C.byref(m),C.sizeof(m)):
                result.update(private_bytes=m.private,working_set_bytes=m.working_set)
            else:result['memory_error']=C.get_last_error()
        return result
    def close(self):K.CloseHandle(self.handle)

class Sampler:
    def __init__(self,pid,interval=.5,min_available=2*1024**3,min_fraction=.15,max_private=8*1024**3):
        self.tracked={pid:Process(pid,'host'),os.getpid():Process(os.getpid(),'client')}
        self.pid=pid;self.interval=interval;self.min_available=min_available;self.min_fraction=min_fraction;self.max_private=max_private
        self.rows=[];self.errors=[];self.phase='startup';self.abort=threading.Event();self.reason=None
        self.stop_event=threading.Event();self.started=time.perf_counter();self.last=self.started;self.runtime={}
        self.thread=threading.Thread(target=self.run,daemon=True)
        self.lock=threading.RLock()
    def discover(self):
        table=process_table()
        owners={pid:p for pid,p in self.tracked.items() if p.kind!='client' and p.alive}
        changed=True
        while changed:
            changed=False
            for pid,row in table.items():
                if pid in self.tracked or row['parent'] not in owners:continue
                if row['name'].lower()!='msedgewebview2.exe':continue
                try:p=Process(pid,'webview',row['parent'])
                except OSError as e:
                    if e.winerror!=87:  # 87: PID already gone between snapshot and open.
                        self.errors.append({'discovery_pid':pid,'error':e.winerror,'phase':self.phase})
                        self.abort.set();self.reason={'kind':'process_discovery_failed','pid':pid}
                    continue  # Never substitute an unrelated PID.
                if p.birth<owners[row['parent']].birth:p.close();continue
                self.tracked[pid]=owners[pid]=p;changed=True
                if str(p.path) not in self.runtime:
                    try:self.runtime[str(p.path)]=version(p.path)
                    except OSError:self.runtime[str(p.path)]=None
    def sample(self):
        self.discover();now=time.perf_counter();elapsed=max(.001,now-self.last);self.last=now
        processes=[]
        for p in self.tracked.values():
            try:processes.append(p.sample())
            except OSError as e:
                self.errors.append({'pid':p.pid,'error':e.winerror,'phase':self.phase})
                self.abort.set();self.reason={'kind':'process_accounting_failed','pid':p.pid}
        if any(p['alive'] and 'memory_error' in p for p in processes):
            self.abort.set();self.reason={'kind':'incomplete_memory_accounting'}
        groups={}
        for group in ('host','webview','client'):
            members=[p for p in processes if p['kind']==group];alive=[p for p in members if p['alive']]
            delta=sum(p['cpu_delta_100ns'] or 0 for p in members)
            groups[group]={'processes':len(alive),'memory_readable':sum('private_bytes' in p for p in alive),
                'private_bytes':sum(p.get('private_bytes',0) for p in alive),
                'working_set_bytes':sum(p.get('working_set_bytes',0) for p in alive),
                'cpu_one_core_percent':delta/1e7/elapsed*100,'cpu_capacity_percent':delta/1e7/elapsed*100/(os.cpu_count() or 1)}
        system=memory();native_private=groups['host']['private_bytes']+groups['webview']['private_bytes']
        floor=max(self.min_available,system['total_physical_bytes']*self.min_fraction)
        if system['available_physical_bytes']<floor or system['available_commit_bytes']<2*1024**3 or native_private>self.max_private:
            self.reason={'kind':'resource_guard','system':system,'native_private_bytes':native_private,'floor_bytes':floor}
            self.abort.set()
        row={'elapsed_s':now-self.started,'phase':self.phase,'groups':groups,'system':system,'processes':processes}
        self.rows.append(row);return row
    def run(self):
        while not self.stop_event.is_set():
            try:
                with self.lock:self.sample()
            except Exception as e:
                self.errors.append({'sampling_error':type(e).__name__+': '+str(e)})
                self.abort.set();self.reason={'kind':'sampling_failed'}
            self.stop_event.wait(self.interval)
    def start(self):self.thread.start()
    def stop(self):
        self.stop_event.set();self.thread.join(5)
        if self.thread.is_alive():raise RuntimeError('Sampler did not stop; handles remain open')
        for p in self.tracked.values():p.close()
    def living_native(self):
        with self.lock:return [p for p in self.tracked.values() if p.kind!='client' and p.alive]
