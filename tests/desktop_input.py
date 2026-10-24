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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "demo"))
from client import Client
from fixture import Fixture
from native_ui import user32, text, click, rectangle, focused_window
from collaboration import wait_until, expect_error


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
]:
    api = getattr(user32, name)
    api.argtypes, api.restype = args, result


def send(*events):
    batch = (Input * len(events))(*events)
    assert user32.SendInput(len(batch), batch, ctypes.sizeof(Input)) == len(events), ctypes.get_last_error()


def key(vk, up=False):
    event = Input(); event.kind = 1; event.key = Key(vk, 0, 2 if up else 0, 0, 0)
    return event


def mouse(flags, data=0, x=0, y=0):
    event = Input(); event.kind = 0; event.mouse = Mouse(x, y, data & 0xFFFFFFFF, flags, 0, 0)
    return event


def move(x, y):
    left, top, width, height = (user32.GetSystemMetrics(i) for i in (76, 77, 78, 79))
    send(mouse(0x8000 | 0x4000 | 1, x=round((x-left)*65535/(width-1)), y=round((y-top)*65535/(height-1))))


def run(executable):
    original_cursor = W.POINT(); assert user32.GetCursorPos(ctypes.byref(original_cursor))
    original_foreground = user32.GetForegroundWindow()
    client, fixture = Client(executable), Fixture()
    browser = hwnd = None
    try:
        made = client.call("create", label="v0.3 desktop input test")
        browser, hwnd = made["browser_id"], int(made["hwnd"])
        assert user32.SetWindowPos(hwnd, W.HWND(-1), 80, 80, 1000, 760, 0x0010), ctypes.get_last_error()
        user32.SetForegroundWindow(hwnd)
        params = {"browser_id": browser, "token": client.call("acquire", browser_id=browser, client_id="desktop", ttl_ms=120000)["token"]}
        client.call("observe", **params)
        client.call("act", **params, action={"type": "navigate", "url": fixture.url("Desktop")})
        observed = client.call("observe", **params)
        page = user32.GetDlgItem(hwnd, 108)
        left, top, right, _ = rectangle(page)
        scale = (right-left) / observed["viewport"]["width"]
        status = user32.GetDlgItem(hwnd, 107)

        def at(x, y):
            screen = W.POINT(round(left+x*scale), round(top+y*scale))
            wait_until(lambda: user32.GetAncestor(user32.WindowFromPoint(screen), 2) == hwnd,
                       "Test page is obscured; refusing desktop input", timeout=3)
            move(screen.x, screen.y)

        # A desktop click may activate this window even when background apps
        # cannot request activation with SetForegroundWindow themselves.
        at(80, 100)
        send(mouse(2), mouse(4))
        fixture.wait(lambda r: r["name"] == "Desktop" and r["count"] == 1, timeout=5)
        assert user32.GetForegroundWindow() == hwnd, "Test window did not receive foreground focus"
        wait_until(lambda: text(status) == "人工控制", "Desktop click did not revoke control")
        expect_error("invalid_token", lambda: client.call("observe", **params))
        print("PASS OS mouse injection: first click reaches page and automatically takes over", flush=True)

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
        at(80, 44); send(mouse(2), mouse(4))
        fixture.wait(lambda r: r["name"] == "Desktop" and r["event"] == "focus")
        params = hand_back(); foreground()
        send(key(ord("A")), key(ord("A"), True))
        fixture.wait(lambda r: r["name"] == "Desktop" and r["text"].lower() == "a")
        expect_error("invalid_token", lambda: client.call("observe", **params))
        print("PASS OS keyboard injection: first keystroke reaches original page and revokes lease", flush=True)

        # Native shortcut handling now lives in the host queue, not an async
        # AcceleratorKeyPressed callback. Check actual Ctrl+L and Ctrl+R.
        foreground(); send(key(0x11), key(ord("L")), key(ord("L"), True), key(0x11, True))
        address = user32.GetDlgItem(hwnd, 102)
        wait_until(lambda: focused_window(hwnd) == address and (user32.SendMessageW(address, 0x00B0, 0, 0) >> 16) == len(text(address)), "Ctrl+L did not focus/select address")
        loads = sum(r["name"] == "Desktop" and r["event"] == "loaded" for r in fixture.records)
        foreground(); send(key(0x11), key(ord("R")), key(ord("R"), True), key(0x11, True))
        wait_until(lambda: sum(r["name"] == "Desktop" and r["event"] == "loaded" for r in fixture.records) > loads, "Ctrl+R did not reload")
        print("PASS OS Ctrl+L/Ctrl+R through host input processing", flush=True)

        params = hand_back(); at(400, 220); foreground(); send(mouse(0x0800, -120))
        fixture.wait(lambda r: r["name"] == "Desktop" and r["scroll"] > 0)
        expect_error("invalid_token", lambda: client.call("observe", **params))
        print("PASS OS wheel injection: first wheel scroll reaches page and revokes lease", flush=True)
    finally:
        if hwnd and user32.GetForegroundWindow() == hwnd:
            send(key(0x11, True), key(0x10, True), key(0x12, True), mouse(4))
        if browser:
            client.call("close", browser_id=browser)
        client.disconnect(); client.wait(); fixture.close()
        user32.SetCursorPos(original_cursor.x, original_cursor.y)
        if original_foreground:
            user32.SetForegroundWindow(original_foreground)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", required=True, help="Allow OS keyboard/mouse injection into the test window")
    parser.add_argument("executable", nargs="?", default=str(ROOT / "build/distinct-browser.exe"))
    run(parser.parse_args().executable)
