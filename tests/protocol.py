"""Malformed framing and request-correlation checks against the real EXE."""
from pathlib import Path
import json
import queue
import subprocess
import threading
import sys


def run():
    executable = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "build/distinct-browser.exe"
    process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               creationflags=subprocess.CREATE_NO_WINDOW)
    received = queue.Queue()

    def read():
        for line in process.stdout:
            received.put(json.loads(line))

    reader = threading.Thread(target=read, daemon=True)
    reader.start()

    def write(raw):
        process.stdin.write(raw)
        process.stdin.flush()

    def response():
        while True:
            item = received.get(timeout=15)
            if "event" not in item:
                return item

    try:
        assert received.get(timeout=10)["event"] == "ready"
        for malformed in [b"{\n", b"[]\n", b'{"id":12,"method":"create"}\n', b"x" * (1024 * 1024 + 1) + b"\n"]:
            write(malformed)
            reply = response()
            assert reply["ok"] is False and reply["error"]["code"] == "invalid_request"
        write(b'{"id":"unknown","method":"missing"}\r\n')
        assert response()["error"]["code"] == "unknown_method"
        # Split UTF-8 across writes; parsing happens only at the newline.
        request = (json.dumps({"id": "split", "method": "create", "params": {"label": "分片 UTF-8"}}, ensure_ascii=False) + "\n").encode()
        cut = request.index("分".encode()) + 1
        write(request[:cut])
        write(request[cut:])
        reply = response()
        assert reply["ok"]
        browser = reply["result"]["browser_id"]
        write((json.dumps({"id": "close", "method": "close", "params": {"browser_id": browser}}) + "\n").encode())
        assert response()["ok"]
        write(b'{"id":"unfinished"')
        process.stdin.close()
        assert process.wait(timeout=10) == 0
        reader.join(timeout=2)
        assert not process.stderr.read()
        print("PASS malformed JSON/types, 1 MiB frame limit, CRLF, split UTF-8, incomplete EOF, stdout framing")
    finally:
        if process.poll() is None:
            process.stdin.close()
            process.wait(timeout=10)
        process.stdout.close()
        process.stderr.close()


if __name__ == "__main__":
    run()
