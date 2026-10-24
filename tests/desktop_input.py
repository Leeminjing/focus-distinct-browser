"""Opt-in OS-input checks for test-owned windows on an interactive Windows desktop.

Usage: python tests/desktop_input.py --run [path/to/distinct-browser.exe]
Moves the cursor and types only while this test window is foreground. It restores
the cursor afterwards. This is SendInput testing, not physical-hardware acceptance.
"""
import argparse
import ctypes
from ctypes import wintypes as W
from pathlib import Path
import sys
import time
import subprocess
import hashlib
import json
from datetime import datetime, timezone, timedelta

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "demo"))
from client import Client
from fixture import Fixture
from native_ui import user32, text, set_text, click, rectangle, focused_window
from collaboration import wait_until, expect_error
kernel32 = ctypes.WinDLL("kernel32")
kernel32.GetCurrentThreadId.restype = W.DWORD
kernel32.OpenProcess.argtypes = [W.DWORD,W.BOOL,W.DWORD]
kernel32.OpenProcess.restype = W.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = [W.HANDLE,W.DWORD,W.LPWSTR,ctypes.POINTER(W.DWORD)]
kernel32.CloseHandle.argtypes = [W.HANDLE]


class Mouse(ctypes.Structure):
    _fields_ = [("dx", W.LONG), ("dy", W.LONG), ("data", W.DWORD), ("flags", W.DWORD),
                ("time", W.DWORD), ("extra", ctypes.c_size_t)]


class Key(ctypes.Structure):
    _fields_ = [("vk", W.WORD), ("scan", W.WORD), ("flags", W.DWORD),
                ("time", W.DWORD), ("extra", ctypes.c_size_t)]


class Hardware(ctypes.Structure):
    _fields_ = [("message", W.DWORD), ("low", W.WORD), ("high", W.WORD)]


class Payload(ctypes.Union):
    _fields_ = [("mouse", Mouse), ("key", Key), ("hardware", Hardware)]


class Input(ctypes.Structure):
    _anonymous_ = ("payload",)
    _fields_ = [("kind", W.DWORD), ("payload", Payload)]


for name, args, result in [
    ("SendInput", [W.UINT, ctypes.POINTER(Input), ctypes.c_int], W.UINT),
    ("GetForegroundWindow", [], W.HWND),
    ("SetForegroundWindow", [W.HWND], W.BOOL),
    ("GetAncestor", [W.HWND, W.UINT], W.HWND),
    ("WindowFromPoint", [W.POINT], W.HWND),
    ("GetCursorPos", [ctypes.POINTER(W.POINT)], W.BOOL),
    ("SetCursorPos", [ctypes.c_int, ctypes.c_int], W.BOOL),
    ("GetSystemMetrics", [ctypes.c_int], ctypes.c_int),
    ("AttachThreadInput", [W.DWORD, W.DWORD, W.BOOL], W.BOOL),
    ("BringWindowToTop", [W.HWND], W.BOOL),
]:
    api = getattr(user32, name)
    api.argtypes, api.restype = args, result


def send(*events):
    batch = (Input * len(events))(*events)
    assert user32.SendInput(len(batch), batch, ctypes.sizeof(Input)) == len(events), ctypes.get_last_error()


def key(vk, up=False):
    scan = user32.MapVirtualKeyW(vk, 4)
    extended = vk in (0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E, 0xA3, 0xA5)
    event = Input(); event.kind = 1; event.key = Key(0, scan & 0xFF, 8 | (2 if up else 0) | (1 if extended or scan >> 8 == 0xE0 else 0), 0, 0)
    return event


def chord(modifier, vk):
    send(key(modifier), key(vk), key(vk, True), key(modifier, True))


def activate(hwnd):
    old_thread = user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), None)
    this_thread = kernel32.GetCurrentThreadId()
    attached = old_thread != this_thread and user32.AttachThreadInput(this_thread, old_thread, True)
    try:
        user32.BringWindowToTop(hwnd); user32.SetForegroundWindow(hwnd)
    finally:
        if attached:
            user32.AttachThreadInput(this_thread, old_thread, False)
    wait_until(lambda: user32.GetForegroundWindow() == hwnd, "Cannot activate test-owned window")


def mouse(flags, data=0, x=0, y=0):
    event = Input(); event.kind = 0; event.mouse = Mouse(x, y, data & 0xFFFFFFFF, flags, 0, 0)
    return event


def move_event(x, y):
    left, top, width, height = (user32.GetSystemMetrics(i) for i in (76, 77, 78, 79))
    return mouse(0x8000 | 0x4000 | 1, x=round((x-left)*65535/(width-1)), y=round((y-top)*65535/(height-1)))


def run(executable):
    results=[]
    def passed(name):
        results.append(name); print("PASS " + name,flush=True)
    original_cursor = W.POINT(); assert user32.GetCursorPos(ctypes.byref(original_cursor))
    original_foreground = user32.GetForegroundWindow()
    client, fixture = Client(executable), Fixture()
    browser = hwnd = extra_browser = None
    try:
        made = client.call("create", label="v0.3 desktop input test")
        handle=kernel32.OpenProcess(0x1000,False,made["browser_process_id"])
        assert handle
        try:
            runtime_path=ctypes.create_unicode_buffer(32768); size=W.DWORD(len(runtime_path))
            assert kernel32.QueryFullProcessImageNameW(handle,0,runtime_path,ctypes.byref(size))
            runtime=Path(runtime_path.value).parent.name
        finally:
            kernel32.CloseHandle(handle)
        browser, hwnd = made["browser_id"], int(made["hwnd"])
        assert user32.SetWindowPos(hwnd, W.HWND(-1), 80, 80, 1000, 760, 0x0010), ctypes.get_last_error()
        user32.SetForegroundWindow(hwnd)
        params = {"browser_id": browser, "token": client.call("acquire", browser_id=browser, client_id="desktop", ttl_ms=120000)["token"]}
        client.call("observe", **params)
        client.call("act", **params, action={"type": "navigate", "url": fixture.url("Desktop")})
        client.call("observe", **params)
        assert user32.SetWindowPos(hwnd, W.HWND(-1), 80, 80, 1000, 760, 0x0010)
        activate(hwnd)
        page = user32.GetDlgItem(hwnd, 108)
        # CSS clientWidth excludes the scrollbar, so dividing the HWND width
        # by it is not a valid CSS-to-screen scale. This fresh page has zoom=1.
        scale = user32.GetDpiForWindow(hwnd) / 96
        status = user32.GetDlgItem(hwnd, 107)

        def location(x, y):
            left, top, _, _ = rectangle(page)
            screen = W.POINT(round(left+x*scale), round(top+y*scale))
            wait_until(lambda: user32.GetAncestor(user32.WindowFromPoint(screen), 2) == hwnd,
                       "Test page is obscured; refusing desktop input", timeout=3)
            return screen

        def at(x,y):
            point = location(x,y); send(move_event(point.x,point.y))

        def click_page(x,y):
            point = location(x,y)
            send(move_event(point.x,point.y),mouse(2),mouse(4))

        # A desktop click may activate this window even when background apps
        # cannot request activation with SetForegroundWindow themselves.
        click_page(80,100)
        fixture.wait(lambda r: r["name"] == "Desktop" and r["count"] == 1, timeout=5)
        assert user32.GetForegroundWindow() == hwnd, "Test window did not receive foreground focus"
        wait_until(lambda: text(status) == "人工控制", "Desktop click did not revoke control")
        expect_error("invalid_token", lambda: client.call("observe", **params))
        passed("OS mouse injection: first click reaches page and automatically takes over")

        def foreground():
            assert user32.GetForegroundWindow() == hwnd, "Focus left test window; refusing further desktop input"

        def hand_back():
            foreground(); click(hwnd, 101)
            fresh = {"browser_id": browser, "token": client.call("acquire", browser_id=browser, client_id="desktop", ttl_ms=120000)["token"]}
            expect_error("observation_required", lambda: client.call("act", **fresh, action={"type": "text", "text": "unobserved"}))
            client.call("observe", **fresh)
            return fresh

        # Focus the input while manual, then hand back: typing itself must be
        # sufficient to take over, without a preceding click during automation.
        mark = len(fixture.records)
        click_page(80,44)
        fixture.wait(lambda r: r["name"] == "Desktop" and r["event"] == "pointerup" and r["focus"] == "text", after=mark)
        params = hand_back(); foreground()
        send(key(ord("A")), key(ord("A"), True))
        fixture.wait(lambda r: r["name"] == "Desktop" and r["text"].lower() == "a")
        expect_error("invalid_token", lambda: client.call("observe", **params))
        passed("OS keyboard injection: first keystroke reaches original page and revokes lease")

        # Actual modifier sequences; the shortcut must execute only once.
        foreground(); chord(0x11, ord("L"))
        address = user32.GetDlgItem(hwnd, 102)
        wait_until(lambda: focused_window(hwnd) == address and (user32.SendMessageW(address, 0x00B0, 0, 0) >> 16) == len(text(address)), "Ctrl+L did not focus/select address")
        loads = sum(r["name"] == "Desktop" and r["event"] == "loaded" for r in fixture.records)
        foreground(); chord(0x11, ord("R"))
        wait_until(lambda: sum(r["name"] == "Desktop" and r["event"] == "loaded" for r in fixture.records) > loads, "Ctrl+R did not reload")
        def reload_once(press):
            before = sum(r["name"] == "Desktop" and r["event"] == "loaded" for r in fixture.records)
            foreground(); press()
            wait_until(lambda: sum(r["name"] == "Desktop" and r["event"] == "loaded" for r in fixture.records) > before, "Page shortcut did not reload")
            wait_until(lambda: "刷新" in text(user32.GetDlgItem(hwnd, 106)), "Reload did not settle")
            time.sleep(.15)  # Detect an asynchronous duplicate browser reload.
            assert sum(r["name"] == "Desktop" and r["event"] == "loaded" for r in fixture.records) == before + 1

        reload_once(lambda: chord(0x11, ord("R")))
        reload_once(lambda: send(key(0x74), key(0x74), key(0x74, True)))  # Auto-repeat must not double reload.
        foreground(); chord(0x12, ord("D"))
        wait_until(lambda: focused_window(hwnd) == address, "Alt+D did not focus native address")
        second_url = fixture.url("History second")
        set_text(address, second_url); send(key(13), key(13, True))
        fixture.wait(lambda r: r["name"] == "History second" and r["event"] == "loaded")
        wait_until(lambda: "刷新" in text(user32.GetDlgItem(hwnd,106)) and user32.IsWindowEnabled(user32.GetDlgItem(hwnd,104)), "Second page did not settle history")
        foreground(); chord(0x12, 0x25)
        wait_until(lambda: text(address) == fixture.url("Desktop"), "Alt+Left did not navigate back")
        foreground(); chord(0x12, 0x27)
        wait_until(lambda: text(address) == second_url, "Alt+Right did not navigate forward")
        # Restore the original document using a real address-bar submission.
        foreground(); chord(0x11, ord("L"))
        wait_until(lambda: focused_window(hwnd) == address, "Ctrl+L after history")
        set_text(address, fixture.url("Desktop")); mark = len(fixture.records)
        send(key(13), key(13, True))
        fixture.wait(lambda r: r["name"] == "Desktop" and r["event"] == "loaded", after=mark)
        passed("OS Ctrl+L/Alt+D, Ctrl+R/F5 exactly once, Alt+Left/Right with the same page identity")

        params = hand_back()
        client.call("act", **params, action={"type": "click", "x": 80, "y": 44})
        mark = len(fixture.records)
        foreground(); send(key(9), key(9, True))
        fixture.wait(lambda r: r["name"] == "Desktop" and r["event"] == "focus" and r["focus"] == "increment", after=mark)
        expect_error("invalid_token", lambda: client.call("observe", **params))
        mark = len(fixture.records)
        chord(0x10, 9)
        fixture.wait(lambda r: r["name"] == "Desktop" and r["event"] == "focus" and r["focus"] == "text", after=mark)
        passed("OS Tab/Shift+Tab preserve DOM traversal and automatic takeover")

        params = hand_back()
        client.call("act", **params, action={"type": "text", "text": "abcdefghij"})
        fixture.wait(lambda r: r["name"] == "Desktop" and r["text"] == "abcdefghij")
        mark = len(fixture.records)
        point = location(33,44); send(move_event(point.x,point.y),mouse(2))
        for x in (70, 120, 200, 280, 400):
            at(x, 44); time.sleep(.025)
        point = location(400,-12)  # Outside page viewport, over the same window's toolbar.
        send(move_event(point.x,point.y),mouse(4))
        released = fixture.wait(lambda r: r["name"] == "Desktop" and r["event"] == "pointerup", after=mark)
        assert released["selection"] == [0, 10] and released["pointer"]["buttons"] == 0, released
        assert released["pointer"]["y"] < 0, released
        expect_error("invalid_token", lambda: client.call("observe", **params))
        foreground(); send(key(ord("Z")), key(ord("Z"), True))
        fixture.wait(lambda r: r["name"] == "Desktop" and r["text"].lower() == "z")
        mark = len(fixture.records)
        click_page(80,100)
        fixture.wait(lambda r: r["name"] == "Desktop" and r["event"] == "click" and r["count"] == 1, after=mark)
        passed("OS drag outside viewport, release, replace actual selection and click again without stuck buttons")

        params = hand_back(); point=location(400,220); foreground()
        send(move_event(point.x,point.y),mouse(0x0800,-120))
        fixture.wait(lambda r: r["name"] == "Desktop" and r["scroll"] > 0)
        expect_error("invalid_token", lambda: client.call("observe", **params))
        passed("OS wheel injection: first wheel scroll reaches page and revokes lease")

        # Keep A automatic while a different test-owned process has focus.
        # Its native EDIT must receive text, and A's lease must remain valid.
        params = hand_back()
        component = Path(executable).resolve().with_name("toolbar-component.exe")
        probe = subprocess.Popen([str(component)], stdout=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
        probe_hwnd = None
        try:
            probe_hwnd = int(probe.stdout.readline())
            user32.SetWindowPos(probe_hwnd, W.HWND(-1), 100, 100, 1000, 220, 0x0010)
            activate(probe_hwnd)
            probe_address = user32.GetDlgItem(probe_hwnd, 102)
            user32.PostMessageW(user32.GetDlgItem(probe_hwnd,106),0x0100,9,0)
            wait_until(lambda: focused_window(probe_hwnd) == probe_address, "Probe EDIT focus")
            set_text(probe_address, "")
            # Unicode packets avoid depending on this separate process's IME
            # language mode; they still pass through the OS keyboard hook.
            packet_down = Input(); packet_down.kind=1; packet_down.key=Key(0,ord("q"),4,0,0)
            packet_up = Input(); packet_up.kind=1; packet_up.key=Key(0,ord("q"),6,0,0)
            send(packet_down,packet_up)
            wait_until(lambda: text(probe_address).lower() == "q", "Other process did not receive original key")
            client.call("renew", **params, ttl_ms=120000)
        finally:
            if probe_hwnd:
                user32.SendMessageW(probe_hwnd,0x0010,0,0)
            probe.wait(timeout=5); probe.stdout.close()
        activate(hwnd)
        passed("keyboard guard leaves another test process's input and browser A's lease untouched")

        # Reuse A's valid lease to reload/focus without any native click. A
        # second browser remains automatic through the next real keystroke.
        client.call("act", **params, action={"type":"navigate", "url":fixture.url("Desktop")})
        client.call("observe", **params)
        client.call("act", **params, action={"type":"click", "x":80, "y":44})
        extra = client.call("create", label="Desktop isolation B")
        extra_browser = extra["browser_id"]
        other = {"browser_id": extra_browser, "token":client.call("acquire",browser_id=extra_browser,client_id="other",ttl_ms=120000)["token"]}
        client.call("observe",**other)
        client.call("act",**other,action={"type":"navigate","url":fixture.url("Unchanged B")})
        client.call("observe",**other)
        activate(hwnd)
        mark=len(fixture.records); click_page(80,44)
        fixture.wait(lambda r:r["name"]=="Desktop" and r["event"]=="pointerup" and r["focus"]=="text",after=mark)
        params=hand_back()
        send(key(ord("B")),key(ord("B"),True))
        fixture.wait(lambda r:r["name"]=="Desktop" and r["text"].lower()=="b")
        expect_error("invalid_token",lambda:client.call("observe",**params))
        client.call("act",**other,action={"type":"click","x":80,"y":100})
        fixture.wait(lambda r:r["name"]=="Unchanged B" and r["count"]==1)
        passed("first OS key takes over only A; independent B keeps automating")
        report={"timestamp":datetime.now(timezone(timedelta(hours=8))).isoformat(),
                "method":"Windows SendInput on the interactive desktop; not physical hardware or IME acceptance",
                "windows_build":sys.getwindowsversion().build,"webview2_runtime":runtime,"dpi":user32.GetDpiForWindow(hwnd),
                "executable_sha256":hashlib.sha256(Path(executable).read_bytes()).hexdigest(),"passed":results,
                "manual_pending":["physical keyboard/mouse","real IME composition and candidate UI","touch/pen","cross-monitor DPI"],
                "keyboard_guard":"WH_KEYBOARD_LL, filtered by this process's foreground window and page focus"}
        destination=ROOT/"artifacts"/"desktop-validation.json"
        destination.parent.mkdir(parents=True,exist_ok=True)
        destination.write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    finally:
        if hwnd and user32.GetForegroundWindow() == hwnd:
            send(key(0x11, True), key(0x10, True), key(0x12, True), mouse(4))
        if browser:
            client.call("close", browser_id=browser)
        if extra_browser:
            client.call("close", browser_id=extra_browser)
        client.disconnect(); client.wait(); fixture.close()
        user32.SetCursorPos(original_cursor.x, original_cursor.y)
        if original_foreground:
            user32.SetForegroundWindow(original_foreground)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", required=True, help="Allow OS keyboard/mouse injection into the test window")
    parser.add_argument("executable", nargs="?", default=str(ROOT / "build/distinct-browser.exe"))
    run(parser.parse_args().executable)
