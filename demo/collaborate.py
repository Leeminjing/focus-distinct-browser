"""Independent manual collaboration exercise; no desktop input injection."""
from pathlib import Path
import argparse
import queue
import threading

from client import BrowserError, Client
from fixture import Fixture


def run(executable, typing=False):
    client, page = Client(executable), Fixture()
    stop = threading.Event()
    browsers, threads = [], []

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
                if error.code in ("invalid_token", "control_unavailable", "busy"):
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
        input("Press Enter here to close this demo's windows.\n")
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--typing", action="store_true", help="Keep page inputs focused for keyboard/IME intervention checks")
    parser.add_argument("executable", nargs="?", default=str(Path(__file__).resolve().parents[1] / "build/distinct-browser.exe"))
    options = parser.parse_args()
    run(options.executable, options.typing)
