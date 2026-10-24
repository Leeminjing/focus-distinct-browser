"""Opt-in OS input / UI-stall regression, confined to test-owned windows.

Suspends only this test's host UI thread and always resumes it in finally.
Usage: python tests/keyboard_stability.py --run [executable]
"""
import argparse
import ctypes
from ctypes import wintypes as W
from pathlib import Path
import sys
import time
import hashlib
import json
from datetime import datetime, timezone

from desktop_input import Client, Fixture, user32, kernel32, activate, send, key, move_event, mouse, Input, Key
from native_ui import click, text, rectangle, focused_window
from collaboration import wait_until, expect_error

kernel32.OpenThread.argtypes = [W.DWORD, W.BOOL, W.DWORD]
kernel32.OpenThread.restype = W.HANDLE
kernel32.SuspendThread.argtypes = [W.HANDLE]
kernel32.SuspendThread.restype = W.DWORD
kernel32.ResumeThread.argtypes = [W.HANDLE]
kernel32.ResumeThread.restype = W.DWORD


def run(executable, cycles, idle_seconds):
    original_cursor = W.POINT(); assert user32.GetCursorPos(ctypes.byref(original_cursor))
    original_foreground = user32.GetForegroundWindow()
    started=time.monotonic()
    passed=[]
    def record(message):
        passed.append(message); print("PASS "+message, flush=True)
    client, fixture = Client(executable), Fixture()
    browser = hwnd = None
    try:
        made = client.call("create", label="Keyboard stability acceptance")
        browser, hwnd = made["browser_id"], int(made["hwnd"])
        activate(hwnd)
        def acquire():
            result = dict(browser_id=browser, token=client.call("acquire", browser_id=browser,
                          client_id="stability", ttl_ms=120000)["token"])
            client.call("observe", **result)
            return result
        params = acquire()
        client.call("act", **params, action={"type":"navigate", "url":fixture.url("Stability")})
        client.call("observe", **params)
        client.call("act", **params, action={"type":"click", "x":80, "y":44})
        page=user32.GetDlgItem(hwnd,108)
        left,top,_,_=rectangle(page)
        scale=user32.GetDpiForWindow(hwnd)/96
        mark=len(fixture.records)
        send(move_event(round(left+80*scale),round(top+44*scale)),mouse(2),mouse(4))
        fixture.wait(lambda r:r["event"]=="pointerup" and r["focus"]=="text",after=mark)

        def stroke(letter):
            assert user32.GetForegroundWindow() == hwnd, "Test lost foreground; refusing input"
            send(key(ord(letter)), key(ord(letter), True))
        def hand_back():
            wait_until(lambda: text(user32.GetDlgItem(hwnd,107)) == "人工控制", "Takeover did not settle")
            click(hwnd,101)
            return acquire()

        params=hand_back()
        stroke("A")
        fixture.wait(lambda r:r["name"]=="Stability" and r["text"].lower()=="a")
        expect_error("invalid_token", lambda:client.call("observe", **params))
        record("baseline first key takeover")
        params = hand_back()

        pid=W.DWORD()
        thread_id = user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        assert pid.value == client.process.pid
        thread = kernel32.OpenThread(0x0002, False, thread_id)
        assert thread
        suspended = False
        try:
            assert kernel32.SuspendThread(thread) == 0
            suspended = True
            start=time.monotonic()
            # This RPC is submitted before the keyboard notification. A UI
            # message alone cannot enforce revocation when it sits behind RPCs.
            queued=client.submit("act", **params, action={"type":"text", "text":"STALE_AGENT"})
            time.sleep(.1)
            stroke("B")
            send_elapsed=time.monotonic()-start
            # Exceeds Windows' maximum LowLevelHooksTimeout (1000 ms).
            time.sleep(max(0, 2.0-(time.monotonic()-start)))
        finally:
            if suspended:
                assert kernel32.ResumeThread(thread) != 0xFFFFFFFF
            kernel32.CloseHandle(thread)
        expect_error("invalid_token", lambda:queued.result(timeout=5))
        fixture.wait(lambda r:r["name"]=="Stability" and r["text"].lower()=="ab")
        assert user32.GetAncestor(focused_window(hwnd),2) == hwnd
        time.sleep(.3)  # Allow queued host input notifications to drain.
        expect_error("invalid_token", lambda:client.call("observe", **params))
        record("2-second UI stall: first key preserved, earlier submitted RPC rejected by input fence")

        params = hand_back()
        stroke("C")
        fixture.wait(lambda r:r["name"]=="Stability" and r["text"].lower()=="abc")
        expect_error("invalid_token", lambda:client.call("observe", **params))
        record("first key after hand-back still revokes lease after UI stall")

        expected="abc"
        def unicode_key(up=False):
            event=Input(); event.kind=1; event.key=Key(0,ord("d"),4|(2 if up else 0),0,0)
            return event
        for cycle in range(cycles):
            params=hand_back()
            assert user32.GetForegroundWindow()==hwnd, "Test lost foreground; refusing input"
            # Unicode packets avoid interpreting the burst as an IME pinyin
            # composition. This does NOT certify physical-key/IME behavior.
            send(*(event for _ in range(50) for event in (unicode_key(),unicode_key(True))))
            expected+="d"*50
            fixture.wait(lambda r:r["name"]=="Stability" and r["text"].lower()==expected)
            expect_error("invalid_token",lambda:client.call("observe",**params))
        record(f"{cycles} hand-back / first-key cycles with {cycles*50} Unicode packet burst characters preserved")

        params=hand_back()
        deadline=time.monotonic()+idle_seconds
        while time.monotonic()<deadline:
            assert user32.GetForegroundWindow()==hwnd, "Foreground changed during soak; rerun on an idle desktop"
            client.call("renew",**params,ttl_ms=120000)
            time.sleep(min(1,max(0,deadline-time.monotonic())))
        stroke("E"); expected+="e"
        fixture.wait(lambda r:r["name"]=="Stability" and r["text"].lower()==expected)
        expect_error("invalid_token",lambda:client.call("observe",**params))
        record(f"first key after {idle_seconds:g} seconds idle / lease renewal still revokes")
        destination=Path(__file__).resolve().parents[1]/"artifacts/keyboard-stability.json"
        report=dict(timestamp=datetime.now(timezone.utc).isoformat(),method="Windows SendInput; suspension of test-owned host UI thread",
                    executable_sha256=hashlib.sha256(Path(executable).read_bytes()).hexdigest(),
                    windows_build=sys.getwindowsversion().build,ui_stall_seconds=2,
                    submitted_rpc_before_key=True,send_during_stall_seconds=round(send_elapsed,3),
                    cycles=cycles,burst_characters=cycles*50,idle_seconds=idle_seconds,
                    duration_seconds=round(time.monotonic()-started,2),passed=passed,
                    manual_pending=["physical devices","actual Chinese IME candidate UI","touch/pen","cross-monitor DPI","multi-hour operation"])
        destination.parent.mkdir(parents=True,exist_ok=True)
        destination.write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    finally:
        if browser:
            client.call("close", browser_id=browser)
        client.disconnect(); client.wait(); fixture.close()
        user32.SetCursorPos(original_cursor.x,original_cursor.y)
        if original_foreground:
            user32.SetForegroundWindow(original_foreground)


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", required=True)
    parser.add_argument("--cycles", type=int, default=20)
    parser.add_argument("--idle-seconds", type=float, default=60)
    parser.add_argument("executable", nargs="?", default=str(Path(__file__).resolve().parents[1]/"build/distinct-browser.exe"))
    options=parser.parse_args()
    if not 1<=options.cycles<=10000 or not 0<=options.idle_seconds<=86400:
        parser.error("cycles must be 1..10000; idle-seconds must be 0..86400")
    run(options.executable,options.cycles,options.idle_seconds)
