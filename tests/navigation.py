"""Real WebView2 navigation settlement regressions; runs without external sites."""
import argparse
from concurrent.futures import TimeoutError
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sys
import threading
import time
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "demo"))
from client import BrowserError, Client

PAGE = b'''<!doctype html><meta charset="utf-8"><title>Navigation fixture</title>
<input id="value" style="position:absolute;left:24px;top:24px;width:200px;height:36px">
<div id="section" style="margin-top:1500px">Section</div>
<script>
function report(kind){fetch('/event',{method:'POST',body:JSON.stringify({kind,url:location.href,
value:document.querySelector('input').value,scroll:scrollY})});}
addEventListener('hashchange',()=>report('hash'));
document.querySelector('input').addEventListener('input',()=>report('input'));
setInterval(async()=>{const command=await(await fetch('/command')).json();
if(command==='stop')window.stop();else if(command)location.assign(command);},50);
report('loaded');
</script>'''


class NavigationPage:
    def __init__(self):
        self.records = []
        self.condition = threading.Condition()
        self.slow_started = threading.Event()
        self.slow_release = threading.Event()
        self.command = None
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def reply(self, content, mime="text/html"):
                self.send_response(200)
                self.send_header("Content-Type", mime)
                self.send_header("Content-Length", str(len(content)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                try:
                    self.wfile.write(content)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass

            def do_GET(self):
                route = urlparse(self.path).path
                if route == "/command":
                    with fixture.condition:
                        command, fixture.command = fixture.command, None
                    self.reply(json.dumps(command).encode(), "application/json")
                    return
                if route == "/redirect":
                    self.send_response(302)
                    self.send_header("Location", "/pending#section")
                    self.end_headers()
                    return
                if route == "/slow":
                    fixture.slow_started.set()
                    fixture.slow_release.wait(timeout=35)
                self.reply(PAGE.replace(b'<script>', b'<img src="/slow"><script>') if route in ("/pending", "/replacement") else PAGE)

            def do_POST(self):
                record = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                with fixture.condition:
                    fixture.records.append(record)
                    fixture.condition.notify_all()
                self.reply(b"")

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def url(self, route):
        return f"http://127.0.0.1:{self.server.server_port}/{route}"

    def wait(self, predicate):
        with self.condition:
            assert self.condition.wait_for(lambda: any(predicate(item) for item in self.records), 5), self.records
            return next(item for item in reversed(self.records) if predicate(item))

    def close(self):
        self.slow_release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def run_case(executable, case, timeout):
    client = Client(executable)
    fixture = NavigationPage()
    browser = None
    try:
        browser = client.call("create", label="Navigation " + case)["browser_id"]
        token = client.call("acquire", browser_id=browser, client_id="navigation-test", ttl_ms=60000)["token"]
        params = {"browser_id": browser, "token": token}
        client.call("observe", **params)
        client.call("act", **params, action={"type": "navigate", "url": fixture.url("base")})
        fixture.wait(lambda r: r["kind"] == "loaded" and r["url"] == fixture.url("base"))
        client.call("observe", **params)
        if case == "fragment":
            client.call("act", **params, action={"type": "click", "x": 80, "y": 40})
            client.call("act", **params, action={"type": "text", "text": "preserved"})
            target = fixture.url("base#section")
            navigation = client.submit("act", **params, action={"type": "navigate", "url": target})
            record = fixture.wait(lambda r: r["kind"] == "hash" and r["url"] == target)
            assert record["value"] == "preserved" and record["scroll"] > 0, record
            assert navigation.result(timeout=timeout)["navigation_completed"] is True
            assert sum(r["kind"] == "loaded" for r in fixture.records) == 1
            try:
                client.call("act", **params, action={"type": "text", "text": "stale"})
            except BrowserError as failure:
                assert failure.code == "observation_required", failure
            else:
                raise AssertionError("Fragment navigation must invalidate the preceding observation")
            observation = client.call("observe", **params)
            assert observation["url"] == target and observation["state"] == "automatic"
            # Repeating the current fragment may have no SourceChanged event.
            repeated = client.submit("act", **params, action={"type": "navigate", "url": target})
            assert repeated.result(timeout=timeout)["navigation_completed"] is True
            client.call("observe", **params)
            for fragment in ("base#second%20section", "base"):
                client.submit("act", **params, action={"type": "navigate", "url": fixture.url(fragment)}).result(timeout=timeout)
                assert client.call("observe", **params)["url"] == fixture.url(fragment)
        elif case == "redirect":
            navigation = client.submit("act", **params, action={"type": "navigate", "url": fixture.url("redirect")})
            assert fixture.slow_started.wait(timeout=5)
            fixture.wait(lambda r: r["url"] == fixture.url("pending#section"))
            assert not navigation.done(), "Cross-document SourceChanged must not settle navigation early"
            fixture.slow_release.set()
            result = navigation.result(timeout=timeout)
            assert result["navigation_completed"] and result["same_document"] is False
            assert client.call("observe", **params)["url"] == fixture.url("pending#section")
        else:
            navigation = client.submit("act", **params, action={"type": "navigate", "url": fixture.url("pending")})
            assert fixture.slow_started.wait(timeout=5)
            with fixture.condition:
                fixture.command = fixture.url("replacement") if case == "superseded" else "stop"
            if case == "superseded":
                fixture.wait(lambda r: r["kind"] == "loaded" and r["url"] == fixture.url("replacement"))
            try:
                navigation.result(timeout=timeout)
            except BrowserError as failure:
                expected = ("navigation_superseded" if case == "superseded" else
                            "navigation_cancelled" if failure.detail.get("web_error_status") == 14 else "navigation_failed")
                assert failure.code == expected, (expected, failure, failure.detail)
                if case == "cancelled":
                    # window.stop() is reported by different Runtime paths as
                    # OPERATION_CANCELED (14) or CONNECTION_ABORTED (9).
                    assert failure.detail["web_error_status"] in (9, 14), failure.detail
            else:
                raise AssertionError("Interrupted navigation was incorrectly reported as successful")
            if case == "superseded":
                try:
                    client.call("observe", **params)
                except BrowserError as failure:
                    assert failure.code == "page_loading", failure
                else:
                    raise AssertionError("Old completion cleared the replacement document's loading flag")
                fixture.slow_release.set()
            # The old request must release busy while the replacement page stays usable.
            deadline = time.monotonic() + 5
            while True:
                try:
                    observation = client.call("observe", **params)
                    break
                except BrowserError as failure:
                    if failure.code != "page_loading" or time.monotonic() >= deadline:
                        raise
                    time.sleep(0.02)
            assert observation["state"] == "automatic"
            assert observation["url"] == fixture.url("replacement" if case == "superseded" else "pending")
        print(f"PASS {case}: navigation settled; browser and original lease remain usable", flush=True)
    except TimeoutError as failure:
        raise AssertionError(f"{case}: page changed but navigation request did not settle within {timeout}s") from failure
    finally:
        fixture.close()
        if browser:
            client.call("close", browser_id=browser)
        client.disconnect()
        client.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", nargs="?", default=str(ROOT / "build/distinct-browser.exe"))
    parser.add_argument("--case", choices=["all", "fragment", "superseded", "cancelled", "redirect"], default="all")
    parser.add_argument("--completion-timeout", type=float, default=5)
    args = parser.parse_args()
    for case in (["fragment", "superseded", "cancelled", "redirect"] if args.case == "all" else [args.case]):
        run_case(args.executable, case, args.completion_timeout)
