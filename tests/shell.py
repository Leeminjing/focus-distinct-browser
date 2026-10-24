"""Native shell behavior against the real EXE/WebView2, with unchanged IPC."""
import ctypes
from ctypes import wintypes as W
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "demo"))
from client import Client, BrowserError
from fixture import Fixture
from native_ui import user32, click, rectangle, text, set_text, snapshot, focused_window


def wait_until(predicate, description):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError(description)


def run():
    client = Client(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/distinct-browser.exe")
    page = Fixture()
    browser = None
    try:
        made = client.call("create", label="网页调研")
        browser, hwnd = made["browser_id"], int(made["hwnd"])
        address, status = user32.GetDlgItem(hwnd, 102), user32.GetDlgItem(hwnd, 107)
        assert address and status
        params = {"browser_id": browser, "token": client.call("acquire", browser_id=browser, client_id="shell-tests")["token"]}
        initial_observation = client.call("observe", **params)
        client_rect = W.RECT()
        assert user32.GetClientRect(hwnd, ctypes.byref(client_rect))
        assert abs(initial_observation["image"]["height"] - (client_rect.bottom - round(64 * user32.GetDpiForWindow(hwnd) / 96))) <= 1
        client.call("act", **params, action={"type": "navigate", "url": page.url("First")})
        client.call("observe", **params)
        first = page.wait(lambda r: r["name"] == "First")
        wait_until(lambda: text(address) == page.url("First"), "Address did not follow navigation")
        assert "Agent" in text(status) and text(user32.GetDlgItem(hwnd, 101)) == "接管"
        snapshot(hwnd, ROOT / "artifacts" / "v0.2-agent.png")

        def native_navigate(url):
            set_text(address, url)
            user32.SendMessageW(address, 0x0100, 13, 0)  # Enter through the real EDIT subclass.

        native_navigate(page.url("Second"))
        second = page.wait(lambda r: r["name"] == "Second")
        assert first["cookie"] == second["cookie"] and first["storage"] == second["storage"]
        wait_until(lambda: text(status) == "人工控制", "Native navigation did not settle into manual control")
        try:
            client.call("act", **params, action={"type": "text", "text": "must not run"})
        except BrowserError as failure:
            assert failure.code == "invalid_token"
        else:
            raise AssertionError("Native address navigation did not revoke automatic control")
        assert text(user32.GetDlgItem(hwnd, 101)) == "交还"
        click(hwnd, 104)
        wait_until(lambda: text(address) == page.url("First"), "Back did not return to the original page")
        click(hwnd, 105)
        wait_until(lambda: text(address) == page.url("Second"), "Forward did not restore the page")
        loads = sum(r["name"] == "Second" and r["event"] == "loaded" for r in page.records)
        click(hwnd, 106)
        wait_until(lambda: sum(r["name"] == "Second" and r["event"] == "loaded" for r in page.records) > loads,
                   "Reload did not reload the real page")
        refreshed = page.wait(lambda r: r["name"] == "Second" and r["event"] == "loaded")
        assert refreshed["cookie"] == first["cookie"] and refreshed["storage"] == first["storage"]
        print("PASS address Enter, back/forward/reload, persistent identity and lease revocation", flush=True)

        # A slow issued auto navigation is cancelled and settled before the manual
        # destination is dispatched; no old lease survives this handover.
        click(hwnd, 101)
        params["token"] = client.call("acquire", browser_id=browser, client_id="shell-tests")["token"]
        client.call("observe", **params)
        slow_url = page.url("Slow").replace("/?", "/slow?")
        pending = client.submit("act", **params, action={"type": "navigate", "url": slow_url})
        assert page.slow_started.wait(5)
        assert "停止" in text(user32.GetDlgItem(hwnd, 106))
        native_navigate(page.url("Manual"))
        page.wait(lambda r: r["name"] == "Manual")
        try:
            pending.result(timeout=5)
        except BrowserError as failure:
            assert failure.code in ("navigation_failed", "navigation_cancelled"), failure
        else:
            raise AssertionError("Stopped automatic navigation reported success")
        assert text(status) == "人工控制"
        page.slow_started.clear()
        native_navigate(slow_url)
        assert page.slow_started.wait(5)
        click(hwnd, 106)
        wait_until(lambda: "刷新" in text(user32.GetDlgItem(hwnd, 106)), "Stop did not finish navigation")
        assert text(status) == "人工控制", "An intentional Stop must retain the control state, not show an error"
        page.slow_release.set()
        native_navigate(page.url("Manual"))
        wait_until(lambda: text(address) == page.url("Manual"), "Address after stop")
        print("PASS stop/loading feedback and safe manual handover during an issued operation", flush=True)

        # Real child rectangles at several widths: the native edit stays within
        # its address container, and state/action controls never overlap.
        dpi = user32.GetDpiForWindow(hwnd)
        for width in (560, 800, 1100):
            user32.SetWindowPos(hwnd, None, 0, 0, round(width * dpi / 96), round(640 * dpi / 96), 0x0016)
            controls = [rectangle(user32.GetDlgItem(hwnd, item)) for item in (104, 105, 106, 102, 107, 101)]
            assert all(a[2] <= b[0] for a, b in zip(controls, controls[1:])), controls
            assert abs((controls[-1][3] - controls[-1][1]) * 96 / dpi - 36) < 2
        snapshot(hwnd, ROOT / "artifacts" / "v0.2-manual.png")
        user32.SendMessageW(hwnd, 0x031A, 0, 0)  # WM_THEMECHANGED resource recreation.
        user32.SendMessageW(hwnd, 0x0015, 0, 0)  # WM_SYSCOLORCHANGE.
        assert text(status) == "人工控制"
        print(f"PASS resize without overlap and native {dpi} DPI geometry; theme resource recreation", flush=True)

        # Native EDIT retains selection, Unicode insertion and Escape semantics;
        # these messages do not replace Windows' IME or clipboard handling.
        set_text(address, "原生地址输入")
        user32.SendMessageW(address, 0x00B1, 2, 4)  # EM_SETSEL
        insert = ctypes.create_unicode_buffer("中文")
        user32.SendMessageW(address, 0x00C2, 1, ctypes.addressof(insert))  # EM_REPLACESEL
        assert text(address) == "原生中文输入"
        user32.SendMessageW(address, 0x0100, 27, 0)
        assert text(address) == page.url("Manual")
        set_text(address, page.url("IME"))
        user32.SendMessageW(address, 0x010D, 0, 0)  # WM_IME_STARTCOMPOSITION
        user32.SendMessageW(address, 0x0100, 13, 0)
        time.sleep(0.2)
        assert not any(r["name"] == "IME" for r in page.records), "IME confirmation prematurely navigated"
        user32.SendMessageW(address, 0x010E, 0, 0)  # WM_IME_ENDCOMPOSITION
        user32.SendMessageW(address, 0x0100, 13, 0)
        page.wait(lambda r: r["name"] == "IME")
        print("PASS native Unicode selection, Escape and IME confirmation guard", flush=True)

        # F5 now takes the common host message route. Actual modifier-key
        # combinations are covered separately by opt-in desktop_input.py.
        user32.PostMessageW(user32.GetDlgItem(hwnd, 106), 0x0100, 9, 0)  # Tab from reload into EDIT.
        wait_until(lambda: focused_window(hwnd) == address, "Native Tab did not focus address")
        set_text(address, "unsubmitted.invalid")
        loads = sum(r["name"] == "IME" and r["event"] == "loaded" for r in page.records)
        user32.PostMessageW(hwnd, 0x0100, 0x74, 0)  # F5 through the actual message loop.
        wait_until(lambda: sum(r["name"] == "IME" and r["event"] == "loaded" for r in page.records) > loads,
                   "F5 did not refresh")
        assert text(address) == page.url("IME"), "An unsubmitted draft must not hide the actual URL after blur"
        print("PASS F5 shortcut through the native message loop", flush=True)

        # A port separator is not a URI scheme. Localhost is case insensitive;
        # this exercises the same native EDIT path without external DNS/network.
        port_url = page.url("AddressPort").replace("127.0.0.1", "LOCALHOST")
        native_navigate(port_url.removeprefix("http://"))
        assert text(status) != "地址格式不支持", "Scheme-less host:port rejected as a URI scheme"
        page.wait(lambda r: r["name"] == "AddressPort")
        wait_until(lambda: text(address).lower() == port_url.lower(), "Host:port address did not navigate")
        assert text(status) == "人工控制"
        print("PASS scheme-less host:port through native address input", flush=True)

        client.disconnect()
        wait_until(lambda: text(status) == "连接已断开", "Disconnect is not visible")
        assert not user32.IsWindowEnabled(user32.GetDlgItem(hwnd, 101))
        snapshot(hwnd, ROOT / "artifacts" / "v0.2-disconnected.png")
        user32.SendMessageW(hwnd, 0x0010, 0, 0)
        browser = None
        client.wait()
        print("PASS disconnected state is visible and manual window remains usable", flush=True)
    finally:
        page.close()
        if browser:
            client.call("close", browser_id=browser)
        client.disconnect()
        if client.process.poll() is None:
            client.wait()


if __name__ == "__main__":
    run()
