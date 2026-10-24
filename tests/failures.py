"""Failure boundaries using only processes/windows created by this test."""
import ctypes
from ctypes import wintypes
from pathlib import Path
import os
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "demo"))
from client import Client, BrowserError
from fixture import Fixture
from native_ui import text, snapshot, user32 as ui

EXE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/distinct-browser.exe"
user32 = ctypes.WinDLL("user32")
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageW.restype = ctypes.c_ssize_t
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


def error(code, work):
    try:
        work()
    except BrowserError as failure:
        assert failure.code == code, failure
        return failure
    raise AssertionError(f"Expected {code}")


def wait_event(client, name, browser):
    while True:
        event = client.events.get(timeout=30)
        if event["event"] == name and event.get("browser_id") == browser:
            return event


def creation_failure(env, code):
    client = Client(EXE, env=env)
    try:
        failure = error(code, lambda: client.call("create", label="Failure probe"))
        print(f"PASS {code}: {failure}", flush=True)
    finally:
        client.disconnect()
        client.wait()


def run():
    with tempfile.TemporaryDirectory(prefix="distinct-boundary-") as temporary:
        # Missing runtime and profile overrides must not produce a usable id.
        creation_failure({**os.environ, "WEBVIEW2_BROWSER_EXECUTABLE_FOLDER": str(Path(temporary) / "missing-runtime")}, "runtime_missing")
        creation_failure({**os.environ, "WEBVIEW2_USER_DATA_FOLDER": str(Path(temporary) / "shared-profile")}, "creation_failed")
    client = Client(EXE)
    fixture = Fixture()
    browsers = []
    try:
        made = client.call("create", label="Process failure probe")
        browser = made["browser_id"]
        browsers.append(browser)
        lease = client.call("acquire", browser_id=browser, client_id="probe")
        params = {"browser_id": browser, "token": lease["token"]}
        client.call("observe", **params)
        # Verify the process identity before terminating this test-owned runtime.
        handle = kernel32.OpenProcess(0x1001, False, made["browser_process_id"])
        assert handle
        try:
            name = ctypes.create_unicode_buffer(32768)
            length = wintypes.DWORD(len(name))
            assert kernel32.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(length))
            assert Path(name.value).name.lower() == "msedgewebview2.exe"
            assert kernel32.TerminateProcess(handle, 123)
        finally:
            kernel32.CloseHandle(handle)
        wait_event(client, "faulted", browser)
        assert "故障" in text(ui.GetDlgItem(int(made["hwnd"]), 107))
        snapshot(int(made["hwnd"]), ROOT / "artifacts" / "v0.2-faulted.png")
        error("invalid_token", lambda: client.call("observe", **params))
        user32.ShowWindow(int(made["hwnd"]), 6)
        assert client.call("present", browser_id=browser)["state"] == "faulted"
        user32.SendMessageW(int(made["hwnd"]), 0x0111, 101, 0)
        error("control_unavailable", lambda: client.call("acquire", browser_id=browser, client_id="probe"))
        client.call("close", browser_id=browser)
        browsers.remove(browser)
        print("PASS process failure revokes control; minimize/restore cannot bypass fault", flush=True)

        made = client.call("create", label="Timeout probe")
        browser = made["browser_id"]
        browsers.append(browser)
        lease = client.call("acquire", browser_id=browser, client_id="probe", ttl_ms=60000)
        params = {"browser_id": browser, "token": lease["token"]}
        client.call("observe", **params)
        slow_url = fixture.url("slow").replace("/?", "/slow?")
        failed = error("browser_faulted", lambda: client.call("act", **params, action={"type": "navigate", "url": slow_url}))
        assert failed.detail["result_unknown"] is True
        assert failed.detail["dispatched_steps"] == 1
        error("control_unavailable", lambda: client.call("acquire", browser_id=browser, client_id="probe"))
        fixture.slow_release.set()
        client.call("close", browser_id=browser)
        browsers.remove(browser)
        print("PASS operation timeout reports unknown effects, revokes lease, never retries", flush=True)
    finally:
        fixture.close()
        for browser in browsers:
            try:
                client.call("close", browser_id=browser)
            except Exception:
                pass
        client.disconnect()
        client.wait()


if __name__ == "__main__":
    run()
