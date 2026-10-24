"""Real WebView2/native host queue arbitration. No global keyboard/mouse injection.

Posted Win32 input is not physical hardware input; actual DOM effects are checked.
See desktop_input.py and docs/v0.3.md for the separate desktop/manual checks.
"""
import ctypes
from ctypes import wintypes as W
from pathlib import Path
import queue
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "demo"))
from client import Client, BrowserError
from fixture import Fixture
from native_ui import user32, text, click, rectangle, focused_window

CALLBACK = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
user32.EnumChildWindows.argtypes = [W.HWND, CALLBACK, W.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [W.HWND, ctypes.POINTER(W.DWORD)]
user32.GetClassNameW.argtypes = [W.HWND, W.LPWSTR, ctypes.c_int]
user32.ClientToScreen.argtypes = [W.HWND, ctypes.POINTER(W.POINT)]


def wait_until(predicate, message, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.01)
    raise AssertionError(message)


def input_window(hwnd, process):
    page = user32.GetDlgItem(hwnd, 108)
    assert page, "Missing native page input boundary"
    candidates = []

    @CALLBACK
    def collect(child, _):
        pid = W.DWORD()
        user32.GetWindowThreadProcessId(child, ctypes.byref(pid))
        name = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(child, name, len(name))
        if pid.value == process and name.value == "Chrome_WidgetWin_0":
            candidates.append(child)
        return True

    user32.EnumChildWindows(page, collect, 0)
    assert len(candidates) == 1, candidates
    return candidates[0]


def expect_error(code, operation):
    try:
        operation()
    except BrowserError as error:
        assert error.code == code, (code, error)
        return error
    raise AssertionError(f"Expected {code}")


def run():
    client = Client(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/distinct-browser.exe")
    fixture = Fixture()
    browsers = []
    try:
        def make(name):
            made = client.call("create", label=name)
            browsers.append(made["browser_id"])
            params = {"browser_id": made["browser_id"], "token": client.call(
                "acquire", browser_id=made["browser_id"], client_id=name, ttl_ms=120000)["token"]}
            client.call("observe", **params)
            client.call("act", **params, action={"type": "navigate", "url": fixture.url(name)})
            observation = client.call("observe", **params)
            fixture.wait(lambda r: r["name"] == name)
            return made, params, observation

        a, pa, observation = make("Human A")
        b, pb, _ = make("Agent B")
        hwnd = int(a["hwnd"])
        target = input_window(hwnd, client.process.pid)
        status = user32.GetDlgItem(hwnd, 107)
        scale = (rectangle(target)[2] - rectangle(target)[0]) / observation["viewport"]["width"]

        def post(message, wparam=0, lparam=0):
            assert user32.PostMessageW(target, message, wparam, lparam)

        def position(x, y):
            return (round(y * scale) << 16) | (round(x * scale) & 0xFFFF)

        def human_click(x, y):
            post(0x0201, 1, position(x, y))
            post(0x0202, 0, position(x, y))

        def act(params, kind, **fields):
            return client.call("act", **params, action={"type": kind, **fields})

        def hand_back():
            wait_until(lambda: text(status) == "人工控制", "Manual takeover did not settle")
            click(hwnd, 101)
            token = client.call("acquire", browser_id=a["browser_id"], client_id="new caller", ttl_ms=120000)["token"]
            params = {"browser_id": a["browser_id"], "token": token}
            expect_error("observation_required", lambda: act(params, "text", text="unobserved"))
            client.call("observe", **params)
            return params

        # Activation, presentation and hover cannot revoke either lease.
        client.call("present", browser_id=a["browser_id"])
        post(0x0200, 0, position(80, 100))
        post(0x0007)  # WM_SETFOCUS is not user intent.
        time.sleep(.1)
        client.call("renew", **pa, ttl_ms=120000)
        act(pa, "click", x=80, y=100)
        fixture.wait(lambda r: r["name"] == "Human A" and r["count"] == 1)
        act(pa, "click", x=80, y=44)
        act(pa, "text", text="Agent")
        act(pa, "key", key="End")
        client.call("renew", **pa, ttl_ms=120000)
        print("PASS hover/present/focus and CDP click/text/key preserve automatic control", flush=True)

        human_click(80, 100)
        human = fixture.wait(lambda r: r["name"] == "Human A" and r["count"] == 2)
        wait_until(lambda: text(status) == "人工控制", "First click did not take over")
        expect_error("invalid_token", lambda: act(pa, "text", text="must not run"))
        expect_error("control_unavailable", lambda: client.call("acquire", browser_id=a["browser_id"], client_id="shared caller"))
        act(pb, "click", x=80, y=100)
        fixture.wait(lambda r: r["name"] == "Agent B" and r["count"] == 1)
        assert human["text"] == "Agent"
        print("PASS first human click reaches original page, shared old lease dies, browser B continues", flush=True)

        pa = hand_back()
        act(pa, "click", x=80, y=44)
        # A real key message passes TranslateMessage and becomes page text.
        post(0x0100, ord("Z"), 1 | (0x2C << 16))
        post(0x0101, ord("Z"), 1 | (0x2C << 16) | (3 << 30))
        fixture.wait(lambda r: r["name"] == "Human A" and r["text"].lower() == "agentz")
        expect_error("invalid_token", lambda: act(pa, "text", text="overwrite"))
        print("PASS first native key reaches page; no later Agent write or implicit resumption", flush=True)

        pa = hand_back()
        point = W.POINT(round(400 * scale), round(200 * scale))
        assert user32.ClientToScreen(target, ctypes.byref(point))
        post(0x020A, ((-120) & 0xFFFF) << 16, ((point.y & 0xFFFF) << 16) | (point.x & 0xFFFF))
        fixture.wait(lambda r: r["name"] == "Human A" and r["scroll"] > 0)
        expect_error("invalid_token", lambda: client.call("renew", **pa, ttl_ms=10000))
        # Native wheel scrolling animates asynchronously in the compositor.
        # Wait for it to settle before testing a new CDP wheel in reverse.
        previous, stable_since = None, time.monotonic()
        while time.monotonic() - stable_since < .25:
            latest = next(r["scroll"] for r in reversed(fixture.records) if r["name"] == "Human A")
            if latest != previous:
                previous, stable_since = latest, time.monotonic()
            time.sleep(.02)
        print("PASS first native wheel scrolls real page and revokes lease", flush=True)

        pa = hand_back()
        act(pa, "scroll", x=400, y=200, delta_y=-10000)
        fixture.wait(lambda r: r["name"] == "Human A" and r["event"] == "scroll" and r["scroll"] == 0)
        client.call("renew", **pa, ttl_ms=120000)  # CDP scroll is not human input.
        act(pa, "click", x=80, y=44)
        # Native IME result text is delivered through the normal Unicode path.
        # This does not simulate a hardware IME candidate window.
        post(0x0100, 0xE5, 1)  # VK_PROCESSKEY
        post(0x0102, ord("中"), 1)
        fixture.wait(lambda r: r["name"] == "Human A" and "中" in r["text"])
        expect_error("invalid_token", lambda: client.call("observe", **pa))
        print("PASS IME process-key arbitration and Unicode result delivery (not candidate UI)", flush=True)

        pa = hand_back()
        # Select the actual page text with a native drag, then replace selection.
        post(0x0201, 1, position(33, 44))
        post(0x0200, 1, position(280, 44))
        post(0x0202, 0, position(280, 44))
        post(0x0102, ord("x"), 1)
        fixture.wait(lambda r: r["name"] == "Human A" and r["text"] == "x")
        expect_error("invalid_token", lambda: client.call("observe", **pa))
        print("PASS native drag selects original page text; first takeover does not consume drag", flush=True)

        pa = hand_back()
        loads = sum(r["name"] == "Human A" and r["event"] == "loaded" for r in fixture.records)
        post(0x0100, 0x74, 1)  # F5 arriving from the WebView2 input subtree.
        wait_until(lambda: sum(r["name"] == "Human A" and r["event"] == "loaded" for r in fixture.records) > loads, "Page F5 did not reload")
        expect_error("invalid_token", lambda: client.call("observe", **pa))
        pa = hand_back()
        act(pa, "click", x=80, y=44)
        focus_mark = len(fixture.records)
        post(0x0100, 9, 1)
        fixture.wait(lambda r: r["name"] == "Human A" and r["event"] == "focus" and r["focus"] == "increment", after=focus_mark)
        expect_error("invalid_token", lambda: client.call("observe", **pa))
        focus_mark = len(fixture.records)
        post(0x0100, 9, 1)
        fixture.wait(lambda r: r["name"] == "Human A" and r["event"] == "focus" and r["focus"] == "popup", after=focus_mark)
        post(0x0100, 9, 1)
        def toolbar_has_focus():
            return focused_window(hwnd) in [user32.GetDlgItem(hwnd, item) for item in (104, 105, 106, 102, 101)]
        wait_until(toolbar_has_focus, "Tab did not leave the page for the native toolbar")
        print("PASS page F5, Tab traversal within WebView2 and native toolbar focus boundary", flush=True)

        pa = hand_back()
        slow = fixture.url("Issued navigation").replace("/?", "/slow?")
        pending = client.submit("act", **pa, action={"type": "navigate", "url": slow})
        assert fixture.slow_started.wait(5)
        # While the old document is still visible, the first click takes effect
        # immediately; the issued network/navigation request is not rolled back.
        click_mark = len(fixture.records)
        human_click(80, 100)
        fixture.wait(lambda r: r["name"] == "Human A" and r["count"] == 1 and r["event"] == "click", after=click_mark)
        wait_until(lambda: text(status) == "接管中", "Issued navigation must show taking_over")
        assert not pending.done(), "Issued navigation was falsely cancelled by input arbitration"
        expect_error("invalid_token", lambda: act(pa, "click", x=80, y=100))
        act(pb, "click", x=80, y=100)
        fixture.wait(lambda r: r["name"] == "Agent B" and r["count"] == 2)
        fixture.slow_release.set()
        result = pending.result(timeout=10)
        assert result["navigation_completed"]
        wait_until(lambda: text(status) == "人工控制", "Issued navigation did not settle truthfully")
        pa = hand_back()
        assert client.call("observe", **pa)["url"] == slow
        # Block a renderer pointerdown handler, so CDP press is definitely in
        # flight when native input arrives. The release must never be dispatched.
        held_url = fixture.url("Held press") + "&hold_pointer=1"
        act(pa, "navigate", url=held_url)
        client.call("observe", **pa)
        pressed = client.submit("act", **pa, action={"type": "click", "x": 80, "y": 100})
        assert fixture.hold_started.wait(5)
        assert not pressed.done(), "Pointer handler did not retain the in-flight press"
        post(0x020A, ((-120) & 0xFFFF) << 16, ((point.y & 0xFFFF) << 16) | (point.x & 0xFFFF))
        wait_until(lambda: text(status) == "接管中", "Compound click did not enter taking_over")
        fixture.hold_release.set()
        try:
            pressed.result(timeout=10)
        except BrowserError as error:
            assert error.code == "invalid_token", error
            assert error.detail["dispatched_steps"] == error.detail["completed_steps"] == 1, error.detail
            assert error.detail["result_unknown"] is True
        else:
            raise AssertionError("CDP release was dispatched after human takeover")
        fixture.wait(lambda r: r["name"] == "Held press" and r["scroll"] > 0)
        assert all(r["count"] == 0 for r in fixture.records if r["name"] == "Held press")
        wait_until(lambda: text(status) == "人工控制", "Compound action did not settle into manual control")
        print("PASS human wheel interleaves with issued CDP press; undispatched release is rejected (1/1 steps)", flush=True)
        reasons = []
        while True:
            try:
                event = client.events.get_nowait()
                if event.get("browser_id") == a["browser_id"] and event["event"] == "control_changed":
                    if event.get("detail", {}).get("reason") == "human_input":
                        reasons.append(event["state"])
            except queue.Empty:
                break
        assert reasons.count("manual") >= 4 and "taking_over" in reasons, reasons
        print("PASS in-flight action retained, taking_over then manual, reason=human_input, hand-back reobserves", flush=True)
    finally:
        fixture.close()
        for browser in browsers:
            client.call("close", browser_id=browser)
        client.disconnect()
        client.wait()


if __name__ == "__main__":
    run()
