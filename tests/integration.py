"""Real Windows/WebView2 acceptance checks. No third-party Python packages."""
from concurrent.futures import ThreadPoolExecutor
import ctypes
from ctypes import wintypes
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "demo"))
from client import BrowserError, Client, save_image
from fixture import Fixture

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.IsWindow.argtypes = [wintypes.HWND]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsWindowEnabled.argtypes = [wintypes.HWND]
user32.GetCursorPos.argtypes = [ctypes.POINTER(wintypes.POINT)]


def expect_error(code, work):
    try:
        work()
    except BrowserError as error:
        assert error.code == code, (code, error)
        return error
    raise AssertionError(f"Expected error: {code}")


def button(hwnd):
    # Exercise the same native WM_COMMAND handler as the toolbar button.
    user32.SendMessageW(hwnd, 0x0111, 101, 0)


def cursor():
    point = wintypes.POINT()
    assert user32.GetCursorPos(ctypes.byref(point))
    return point.x, point.y


def run():
    client = Client(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/distinct-browser.exe")
    fixture = Fixture()
    handles = []
    browsers = []
    report = []

    def passed(name):
        report.append(name)
        print(f"PASS {name}", flush=True)

    def act(params, kind, **fields):
        return client.call("act", **params, action={"type": kind, **fields})

    def acquire(browser, name="caller", **fields):
        lease = client.call("acquire", browser_id=browser, client_id=name, **fields)
        return {"browser_id": browser, "token": lease["token"]}

    try:
        assert client.events.get(timeout=10)["event"] == "ready"
        with ThreadPoolExecutor(max_workers=2) as pool:
            made = list(pool.map(lambda name: client.call("create", label=name), ["独立浏览器 A", "独立浏览器 B"]))
        browsers = [item["browser_id"] for item in made]
        handles = [int(item["hwnd"]) for item in made]
        assert len(set(browsers)) == len(set(handles)) == 2
        assert all(user32.IsWindowVisible(hwnd) for hwnd in handles)
        passed("two real windows, ready only after initialization")
        a, b = browsers
        pa, pb = acquire(a, "alice"), acquire(b, "bob")
        expect_error("observation_required", lambda: act(pa, "text", text="stale"))
        expect_error("busy", lambda: acquire(a, "competitor"))
        passed("exclusive ownership and mandatory initial observation")

        def populate(args):
            params, name = args
            client.call("observe", **params)
            act(params, "navigate", url=fixture.url(name))
            client.call("observe", **params)
            act(params, "click", x=80, y=40)
            act(params, "text", text="你好 " + name)
            act(params, "key", key="End")
            act(params, "key", key="Space")
            fixture.wait(lambda record: record["name"] == name and record["text"] == "你好 " + name + " ")
            act(params, "key", key="Backspace")
            act(params, "click", x=80, y=100)
            fixture.wait(lambda record: record["name"] == name and record["text"] == "你好 " + name and record["count"] == 1)

        mouse_before = cursor()
        with ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(populate, [(pa, "A"), (pb, "B")]))
        mouse_after = cursor()
        assert mouse_after == mouse_before, f"Desktop cursor changed during automation: {mouse_before} -> {mouse_after}; this check requires an idle desktop"
        ra = fixture.wait(lambda r: r["name"] == "A" and r["count"] == 1)
        rb = fixture.wait(lambda r: r["name"] == "B" and r["count"] == 1)
        assert ra["cookie"] != rb["cookie"] and ra["storage"] != rb["storage"]
        passed("parallel coordinate/text/key actions, isolated cookies/storage, desktop mouse unchanged")

        for name, params in [("A", pa), ("B", pb)]:
            observation = client.call("observe", **params)
            save_image(observation, ROOT / "artifacts" / f"acceptance-{name}.png")
            assert observation["image"]["width"] > 0 and observation["viewport"]["width"] > 0
        act(pa, "scroll", x=500, y=200, delta_y=400)
        fixture.wait(lambda r: r["name"] == "A" and r["scroll"] > 0)
        before = client.call("observe", **pa)
        loaded_count = sum(r["event"] == "loaded" and r["name"] == "A" for r in fixture.records)
        client.call("present", browser_id=b)
        client.call("present", browser_id=a)
        after = client.call("observe", **pa)
        assert before["url"] == after["url"] and before["title"] == after["title"]
        assert loaded_count == sum(r["event"] == "loaded" and r["name"] == "A" for r in fixture.records)
        passed("capture with CSS viewport; present preserves page, input and scroll")

        client.call("release", **pa)
        expect_error("invalid_token", lambda: client.call("observe", **pa))
        pa2 = acquire(a, "second-caller")
        expect_error("observation_required", lambda: act(pa2, "text", text="stale"))
        observation = client.call("observe", **pa2)
        assert observation["title"] == before["title"]
        act(pa2, "scroll", x=500, y=100, delta_y=-10000)
        fixture.wait(lambda r: r["name"] == "A" and r["event"] == "scroll" and r["scroll"] == 0)
        act(pa2, "click", x=80, y=100)
        fixture.wait(lambda r: r["name"] == "A" and r["count"] == 2)
        passed("second caller shares the same live page and must reobserve")

        # Native takeover revokes old tokens; hand-back never restores queued work.
        button(handles[0])
        expect_error("invalid_token", lambda: act(pa2, "text", text="after takeover"))
        expect_error("control_unavailable", lambda: acquire(a))
        button(handles[0])
        pa3 = acquire(a)
        expect_error("observation_required", lambda: act(pa3, "text", text="stale"))
        client.call("observe", **pa3)
        passed("native takeover/hand-back invalidates tokens and observation")

        # A waits for a server; B can continue. Takeover leaves the already
        # dispatched navigation pending, and only settles into manual afterward.
        slow_url = fixture.url("A").replace("/?", "/slow?")
        slow = client.submit("act", **pa3, action={"type": "navigate", "url": slow_url})
        assert fixture.slow_started.wait(timeout=10)
        client.call("observe", **pb)
        act(pb, "click", x=80, y=100)
        fixture.wait(lambda r: r["name"] == "B" and r["count"] == 2)
        button(handles[0])
        assert not slow.done()
        expect_error("invalid_token", lambda: act(pa3, "text", text="must not run"))
        expect_error("busy", lambda: acquire(a))
        fixture.slow_release.set()
        assert slow.result(timeout=15)["navigation_completed"]
        expect_error("control_unavailable", lambda: acquire(a))
        button(handles[0])
        pa4 = acquire(a)
        client.call("observe", **pa4)
        passed("cross-browser progress during slow navigation; takeover settles dispatched result")

        act(pa4, "click", x=80, y=152)
        deadline = time.monotonic() + 5
        popup = None
        while time.monotonic() < deadline:
            event = client.events.get(timeout=5)
            if (event["event"] == "page_created" and event.get("browser_id") == a
                    and event.get("detail", {}).get("opener_page_id") == made[0]["page_id"]):
                popup = event["detail"]["page_id"]
                break
        assert popup
        client.call("close_page", **pa4, page_id=popup)
        client.call("observe", **pa4)
        passed("popup is managed within the browser; closing it preserves the original page")

        # Restoring only changes visibility; explicit hand-back is still needed.
        user32.ShowWindow(handles[0], 6)
        expect_error("invalid_token", lambda: client.call("observe", **pa4))
        assert client.call("present", browser_id=a)["state"] == "paused"
        expect_error("control_unavailable", lambda: acquire(a))
        button(handles[0])
        pa5 = acquire(a)
        client.call("observe", **pa5)
        user32.ShowWindow(handles[0], 0)
        expect_error("invalid_token", lambda: client.call("observe", **pa5))
        assert client.call("present", browser_id=a)["state"] == "paused"
        button(handles[0])
        pa6 = acquire(a, ttl_ms=1000)
        client.call("observe", **pa6)
        time.sleep(1.1)
        expect_error("invalid_token", lambda: act(pa6, "text", text="expired"))
        pa7 = acquire(a)
        assert client.call("renew", **pa7, ttl_ms=5000)["ttl_ms"] == 5000
        passed("minimize/hide pause; restore requires hand-back; expiry/renewal")

        client.call("close", browser_id=b)
        expect_error("browser_not_found", lambda: client.call("present", browser_id=b))
        assert not user32.IsWindow(handles[1])
        passed("close invalidates identifier and destroys original window")
        client.disconnect()
        # Wait for EOF revocation by looking at the native toolbar label.
        get_item = user32.GetDlgItem
        get_item.argtypes = [wintypes.HWND, ctypes.c_int]
        get_item.restype = wintypes.HWND
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if not user32.IsWindowEnabled(get_item(handles[0], 101)):
                break
            time.sleep(0.02)
        assert not user32.IsWindowEnabled(get_item(handles[0], 101))
        assert user32.IsWindowVisible(handles[0])
        user32.SendMessageW(handles[0], 0x0010, 0, 0)
        client.wait()
        passed("disconnect retains manual window; final user close exits host")
    finally:
        fixture.close()
        for hwnd in handles:
            if user32.IsWindow(hwnd):
                user32.SendMessageW(hwnd, 0x0010, 0, 0)
        client.disconnect()
        if client.process.poll() is None:
            client.wait()
    (ROOT / "artifacts").mkdir(exist_ok=True)
    (ROOT / "artifacts" / "acceptance.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    print(f"{len(report)} acceptance groups passed")


if __name__ == "__main__":
    run()
