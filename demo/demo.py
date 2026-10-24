"""Usage: python demo/demo.py [build/distinct-browser.exe]."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import sys
from client import Client, save_image
from fixture import Fixture


def main():
    root = Path(__file__).resolve().parents[1]
    client = Client(sys.argv[1] if len(sys.argv) > 1 else root / "build/distinct-browser.exe")
    fixture = Fixture()
    browsers = []
    try:
        with ThreadPoolExecutor(max_workers=2) as pool:
            created = list(pool.map(lambda label: client.call("create", label=label), ["Browser A", "Browser B"]))
            browsers = [item["browser_id"] for item in created]

            def work(pair):
                browser, name = pair
                lease = client.call("acquire", browser_id=browser, client_id=name)
                params = {"browser_id": browser, "token": lease["token"]}
                client.call("observe", **params)
                client.call("act", **params, action={"type": "navigate", "url": fixture.url(name)})
                client.call("observe", **params)
                client.call("act", **params, action={"type": "click", "x": 80, "y": 40})
                client.call("act", **params, action={"type": "text", "text": f"Hello from {name}"})
                client.call("act", **params, action={"type": "click", "x": 80, "y": 100})
                save_image(client.call("observe", **params), root / "artifacts" / f"{name}.png")
                client.call("release", **params)

            list(pool.map(work, zip(browsers, ["A", "B"])))
        # A second logical caller uses the same real page after observing it.
        lease = client.call("acquire", browser_id=browsers[0], client_id="second-caller")
        params = {"browser_id": browsers[0], "token": lease["token"]}
        observation = client.call("observe", **params)
        assert observation["title"].startswith("A | 1 | Hello from A")
        client.call("act", **params, action={"type": "click", "x": 80, "y": 100})
        client.call("release", **params)
        client.call("present", browser_id=browsers[0])
        print("Two independent windows are open; caller 2 advanced A's counter to 2.")
        print("Try the native takeover/hand-back buttons, minimizing and restoring either window.")
        input("Press Enter here to close the demo windows: ")
    finally:
        for browser in browsers:
            try:
                client.call("close", browser_id=browser)
            except Exception:
                pass
        client.disconnect()
        client.wait()
        fixture.close()


if __name__ == "__main__":
    main()
