"""Independent manual collaboration exercise; no desktop input injection."""
from pathlib import Path
import argparse
import queue
import threading
import hashlib
import json
import platform
from datetime import datetime, timezone
from collections import deque

from client import BrowserError, Client
from fixture import Fixture


MANUAL_CASES = [
    ("physical_keyboard", "物理首键：点击 A 输入框后交还，等 Agent 写入点号，再直接按字母。首字保留，A 停止，B 继续；交还后才恢复。"),
    ("physical_mouse_wheel", "物理鼠标/滚轮：交还 A，再首次点击计数器或滚动。原页面有效果，A 接管，B 不受影响；仅移动鼠标不接管。"),
    ("shortcuts_drag", "实体快捷键/拖拽：Ctrl+L、Alt+D 后 Escape 返回网页；Ctrl+R、F5 单次重载，Tab/Shift+Tab 焦点正常。拖选后移出网页到工具栏释放，再输入替换选区，无粘键。"),
    ("chinese_ime", "真实中文 IME：A 输入框聚焦后交还，从拼音组合开始接管；候选窗可见且定位正确，选词、确认、取消、连续中文输入均正常，无后续 Agent 覆盖。记录输入法及版本。"),
    ("touch_pen", "触摸/笔：分别交还后触碰、滑动、拖动；首次效果保留且接管。笔悬停不接管。无设备请 skip。"),
    ("cross_monitor_dpi", "跨 DPI 显示器：拖动 A 至不同缩放显示器，核对工具栏、点击、IME 候选定位、页面拖拽和首次按键接管。只有一屏或同 DPI 请 skip。"),
    ("extended_operation", "持续运行：至少 1 小时使用、反复接管/交还，最后首次按键仍撤销旧令牌。记录实际持续时长；未达到时长请 skip。"),
]


def manual_results(evidence):
    print("\n人工验收：本程序不注入系统输入，也不会自动认定物理设备或 IME 通过。")
    print("测试仅用本地演示页。每项操作后回到终端输入 pass/fail/skip 和观察说明；无设备请 skip。")
    evidence["environment"] = input("测试者、键鼠设备、输入法版本、显示器和缩放：\n> ").strip()
    for case, instruction in MANUAL_CASES:
        print("\n" + instruction, flush=True)
        while True:
            answer = input("> ").strip().split(maxsplit=1)
            if answer and answer[0].lower() in ("pass", "fail", "skip") and len(answer)==2:
                evidence["results"].append(dict(case=case,status=answer[0].lower(),notes=answer[1],
                                    recorded_at=datetime.now(timezone.utc).isoformat()))
                break
            print("请输入 pass/fail/skip 加说明，例如：skip 无触摸设备。")


def run(executable, typing=False, manual_report=None):
    client, page = Client(executable), Fixture()
    stop = threading.Event()
    browsers, threads = [], []
    history = deque(maxlen=2000)
    evidence = dict(environment="",results=[])
    started=datetime.now(timezone.utc).isoformat()

    def automate(browser, label):
        params = None
        while not stop.is_set():
            try:
                if params is None:
                    token = client.call("acquire", browser_id=browser, client_id=label, ttl_ms=30000)["token"]
                    params = {"browser_id": browser, "token": token}
                # Every new lease observes before its first mutation, and every
                # subsequent loop observes the page the user actually left.
                client.call("observe", **params)
                action = {"type": "text", "text": "."} if typing else {"type": "click", "x": 80, "y": 100}
                client.call("act", **params, action=action)
                client.call("renew", **params, ttl_ms=30000)
            except BrowserError as error:
                if stop.is_set():
                    return
                if error.code in ("invalid_token", "control_unavailable", "busy"):
                    if error.code == "invalid_token":
                        print(f"{label}: old lease rejected (invalid_token)",flush=True)
                        history.append(dict(time=datetime.now(timezone.utc).isoformat(),browser=label,
                                            event="automation_rejected",reason="invalid_token"))
                    params = None  # A manual page stays manual until hand-back.
                elif error.code in ("page_changed", "page_loading"):
                    pass
                else:
                    print(f"{label}: {error}", flush=True)
                    return
            except ConnectionError:
                return
            stop.wait(.7)

    def events():
        while not stop.is_set():
            try:
                event = client.events.get(timeout=.2)
            except queue.Empty:
                continue
            if event["event"] in ("control_changed", "closed", "faulted", "paused"):
                name = next((label for browser, label in browsers if browser == event.get("browser_id")), "browser")
                print(f"{name}: {event.get('state')} {event.get('detail', {}).get('reason', '')}", flush=True)
                history.append(dict(time=datetime.now(timezone.utc).isoformat(),browser=name,
                                    event=event["event"],state=event.get("state"),reason=event.get("detail",{}).get("reason")))

    try:
        for label in ("Collaboration A", "Collaboration B"):
            made = client.call("create", label=label)
            browser = made["browser_id"]
            browsers.append((browser, label))
            params = {"browser_id": browser, "token": client.call("acquire", browser_id=browser, client_id="setup")["token"]}
            client.call("observe", **params)
            client.call("act", **params, action={"type": "navigate", "url": page.url(label)})
            if typing:
                client.call("observe", **params)
                client.call("act", **params, action={"type": "click", "x": 80, "y": 44})
            client.call("release", **params)
        for browser, label in browsers:
            thread = threading.Thread(target=automate, args=(browser, label), daemon=True)
            thread.start(); threads.append(thread)
        monitor = threading.Thread(target=events, daemon=True)
        monitor.start(); threads.append(monitor)
        print("Both Agents type in their page inputs." if typing else "Both Agents increment their page counters.")
        print("Click/type/scroll in A: A must stop; B keeps going.")
        print("Move the pointer or present a window: control must stay automatic. Click Hand back to resume.")
        if manual_report:
            manual_results(evidence)
        else:
            input("Press Enter here to close this demo's windows.\n")
    except KeyboardInterrupt:
        print("Manual exercise stopped; completed entries will be retained.",flush=True)
    finally:
        stop.set()
        for browser, _ in browsers:
            try:
                client.call("close", browser_id=browser)
            except (BrowserError, ConnectionError):
                pass
        for thread in threads:
            thread.join(timeout=3)
        client.disconnect(); client.wait(); page.close()
        if manual_report:
            report=dict(started_at=started,ended_at=datetime.now(timezone.utc).isoformat(),
                        executable_sha256=hashlib.sha256(Path(executable).read_bytes()).hexdigest(),
                        platform=platform.platform(),source="human-reported; not automatically certified",
                        **evidence,
                        pending=[case for case,_ in MANUAL_CASES if not any(r["case"]==case and r["status"]=="pass" for r in evidence["results"])],
                        control_events=list(history),event_history_limit=2000)
            destination=Path(manual_report); destination.parent.mkdir(parents=True,exist_ok=True)
            destination.write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
            print(f"Manual evidence saved: {destination.resolve()}",flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--typing", action="store_true", help="Keep page inputs focused for keyboard/IME intervention checks")
    parser.add_argument("--manual-report", help="Guide physical-device checks and save human-reported evidence; not an automated pass")
    parser.add_argument("executable", nargs="?", default=str(Path(__file__).resolve().parents[1] / "build/distinct-browser.exe"))
    options = parser.parse_args()
    run(options.executable, options.typing, options.manual_report)
