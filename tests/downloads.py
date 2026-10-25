"""Real downloads and permission waiting through JSONL; never grants device access.

No desktop input is injected. Permission grant/deny semantics are exercised with
COM probes in lifecycle-tests, while this suite checks the real Runtime wait and
page/connection cleanup paths. Physical devices remain a separate acceptance.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import hashlib
import json
import queue
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "demo"))
from client import Client, BrowserError

DATA = b"distinct-browser-v05\n" * 8192
HTML = b'''<!doctype html><meta charset="utf-8"><title>Download and permissions</title>
<style>body{font:16px sans-serif;margin:24px}a,button{display:block;width:280px;height:36px;margin-bottom:16px}</style>
<a href="/file">Download normal</a><a href="/slow">Download slow</a><a href="/broken">Download broken</a>
<button onclick="navigator.geolocation.getCurrentPosition(()=>document.title='location received',()=>document.title='location denied')">Request location</button>
<button onclick="window.open('/child','_blank')">Open child</button>'''


class Fixture:
    def __init__(self):
        self.requests = []
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                owner.requests.append(self.path)
                if self.path in {"/file", "/slow", "/broken"}:
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Disposition", 'attachment; filename="../CON?.txt"')
                    self.send_header("Content-Length", str(len(DATA)))
                    self.end_headers()
                    try:
                        if self.path == "/broken":
                            self.wfile.write(DATA[:512]); self.wfile.flush(); self.close_connection = True
                        else:
                            for offset in range(0, len(DATA), 4096):
                                self.wfile.write(DATA[offset:offset+4096]); self.wfile.flush()
                                if self.path == "/slow":
                                    time.sleep(.25)
                    except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                        pass
                else:
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.end_headers(); self.wfile.write(HTML)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def url(self, path="/"):
        return f"http://127.0.0.1:{self.server.server_port}{path}"

    def close(self):
        self.server.shutdown(); self.server.server_close()


def eventually(work, predicate, timeout=15):
    deadline = time.monotonic() + timeout
    value = None
    while time.monotonic() < deadline:
        value = work()
        if predicate(value):
            return value
        time.sleep(.05)
    raise AssertionError(f"deadline; last result={value}")


def run():
    executable = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "build/distinct-browser.exe"
    client, fixture = Client(executable), Fixture()
    browsers, results, files = [], [], []

    def passed(name):
        results.append(name); print("PASS " + name, flush=True)

    def make(label):
        made = client.call("create", label=label); browsers.append(made["browser_id"])
        params = {"browser_id": made["browser_id"], "token": client.call("acquire", browser_id=made["browser_id"], client_id=label)["token"]}
        client.call("observe", **params)
        client.call("act", **params, page_id=made["page_id"], action={"type": "navigate", "url": fixture.url()})
        client.call("observe", **params, page_id=made["page_id"])
        return made, params

    def denied(code, method, **params):
        try:
            client.call(method, **params)
        except BrowserError as error:
            assert error.code == code, error
        else:
            raise AssertionError("expected " + code)

    def downloads(made):
        return client.call("list_downloads", browser_id=made["browser_id"])["downloads"]

    def click(params, y):
        try:
            return client.call("act", **params, action={"type": "click", "x": 100, "y": y})
        except BrowserError as error:
            # Already dispatched input can revoke its own lease; never replay.
            assert error.code in {"invalid_token", "page_changed"}, error

    def navigate_download(params, url, page_id):
        try:
            result = client.call("act", **params, page_id=page_id, action={"type": "navigate", "url": url})
            assert result["download_started"]
        except BrowserError as error:
            # Runtime can report CONNECTION_ABORTED before DownloadStarting.
            # The navigation did not complete, while the later independent
            # download record supplies its actual transfer result. Never replay.
            assert error.code == "navigation_failed" and error.detail["web_error_status"] == 9, error

    try:
        a, pa = make("Download A")
        click(pa, 42)
        record = eventually(lambda: downloads(a), lambda rows: rows and rows[0]["state"] == "completed")[0]
        path = Path(record["path"])
        assert path.is_absolute() and a["browser_id"] in path.parts and record["download_id"] in path.parts
        assert path.name.startswith("download-") and path.read_bytes() == DATA
        files.append({**record, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
        denied("invalid_token", "observe", **pa)
        denied("control_unavailable", "acquire", browser_id=a["browser_id"], client_id="old")
        passed("real download completes with exact bytes, unique confined path, original page identity and revoked lease")

        b, pb = make("Download B")
        navigate_download(pb, fixture.url("/file"), b["page_id"])
        other = eventually(lambda: downloads(b), lambda rows: rows and rows[0]["state"] == "completed")[0]
        assert other["path"] != record["path"] and Path(other["path"]).read_bytes() == DATA
        assert client.call("list_pages", browser_id=a["browser_id"])["pages"][0]["page_id"] == a["page_id"]
        passed("navigation becoming an attachment settles without timeout; another browser has an independent destination")

        slow, ps = make("Slow download")
        click(ps, 94)
        ongoing = eventually(lambda: downloads(slow), lambda rows: rows and 0 < rows[0]["bytes_received"] < len(DATA))[0]
        assert ongoing["state"] == "in_progress"
        assert client.call("list_downloads", browser_id=slow["browser_id"])["interaction_pending"]
        denied("control_unavailable", "acquire", browser_id=slow["browser_id"], client_id="wait")
        client.call("close", browser_id=slow["browser_id"])
        events = []
        while True:
            try:
                events.append(client.events.get_nowait())
            except queue.Empty:
                break
        assert any(event.get("event") == "download_changed" and event.get("detail", {}).get("download_id") == ongoing["download_id"]
                   and event["detail"]["state"] == "cancelled" for event in events)
        passed("slow download exposes intermediate progress and blocks lease acquisition; closing cancels its own download")

        broken, pf = make("Broken download")
        click(pf, 146)
        failed = eventually(lambda: downloads(broken), lambda rows: rows and rows[0]["state"] == "interrupted", timeout=40)[0]
        assert len(downloads(broken)) == 1, "a stalled URL must not accumulate duplicate active downloads"
        assert failed["reason"] and failed["bytes_received"] < len(DATA)
        assert not client.call("list_downloads", browser_id=broken["browser_id"])["interaction_pending"]
        passed("broken transfer reports interruption and reason without marking the browser faulted or replaying the action")

        multi, pm = make("Child download")
        click(pm, 250)
        listing = eventually(lambda: client.call("list_pages", browser_id=multi["browser_id"]),
            lambda value: len(value["pages"]) == 2 and any(page["url"] == fixture.url("/child") and not page["loading"] for page in value["pages"]))
        child = next(page["page_id"] for page in listing["pages"] if page["page_id"] != multi["page_id"])
        client.call("observe", **pm, page_id=child)
        navigate_download(pm, fixture.url("/file"), child)
        child_file = eventually(lambda: downloads(multi), lambda rows: rows and rows[0]["state"] == "completed")[0]
        assert child_file["page_id"] == child and len(client.call("list_pages", browser_id=multi["browser_id"])["pages"]) == 2
        passed("child-page download retains its page_id and both real pages; no retargeting to its parent")

        permission, pp = make("Permission wait")
        click(pp, 198)
        def permission_event():
            while True:
                try:
                    event = client.events.get_nowait()
                    if event.get("event") == "permission_requested" and event.get("browser_id") == permission["browser_id"]:
                        return event
                except queue.Empty:
                    return None
        request = eventually(permission_event, bool)
        assert request["detail"]["kind"] == "geolocation" and request["detail"]["page_id"] == permission["page_id"]
        denied("invalid_token", "observe", **pp)
        denied("control_unavailable", "acquire", browser_id=permission["browser_id"], client_id="cannot-grant")
        denied("unknown_method", "resolve_permission", browser_id=permission["browser_id"], request_id=request["detail"]["request_id"], allow=True)
        assert client.call("list_downloads", browser_id=permission["browser_id"])["interaction_pending"]
        client.call("close", browser_id=permission["browser_id"])
        passed("real permission request pauses the browser, has exact page/origin, cannot be granted by RPC and closes cleanly")

        destination = Path(__file__).resolve().parents[1] / "artifacts/v05-download-acceptance.json"
        destination.write_text(json.dumps({"executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
            "checks": results, "files": files,
            "scope": "real Runtime protocol/transfer tests plus COM-probe permission decisions; not physical input or real device permission grant"}, indent=2), encoding="utf-8")
    finally:
        for browser in browsers:
            try:
                client.call("close", browser_id=browser)
            except BrowserError:
                pass
        client.disconnect(); client.wait(); fixture.close()


if __name__ == "__main__":
    run()
