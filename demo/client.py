"""Small stdlib-only client for the JSONL protocol; no application policy."""
from concurrent.futures import Future
import json
from pathlib import Path
import queue
import subprocess
import threading


class BrowserError(RuntimeError):
    def __init__(self, error):
        super().__init__(f"{error['code']}: {error['message']}")
        self.code = error["code"]
        self.detail = error.get("detail", {})


class Client:
    def __init__(self, executable, *, env=None):
        self.process = subprocess.Popen(
            [str(Path(executable).resolve())], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW,
            env=env,
        )
        self.events = queue.Queue()
        self._pending = {}
        self._lock = threading.Lock()
        self._serial = 0
        self._closed = False
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def _read(self):
        try:
            for line in self.process.stdout:
                response = json.loads(line)
                if "event" in response:
                    self.events.put(response)
                else:
                    with self._lock:
                        future = self._pending.pop(response["id"], None)
                    if future:
                        if response["ok"]:
                            future.set_result(response["result"])
                        else:
                            future.set_exception(BrowserError(response["error"]))
        finally:
            with self._lock:
                self._closed = True
                for future in self._pending.values():
                    future.set_exception(ConnectionError("Browser host disconnected"))
                self._pending.clear()

    def submit(self, method, **params):
        future = Future()
        with self._lock:
            if self._closed:
                raise ConnectionError("Client is disconnected")
            self._serial += 1
            request_id = str(self._serial)
            self._pending[request_id] = future
            request = {"id": request_id, "method": method, "params": params}
            try:
                self.process.stdin.write((json.dumps(request, ensure_ascii=False) + "\n").encode("utf-8"))
                self.process.stdin.flush()
            except (BrokenPipeError, OSError) as error:
                self._pending.pop(request_id)
                future.set_exception(error)
        return future

    def call(self, method, **params):
        return self.submit(method, **params).result(timeout=60)

    def disconnect(self):
        """Revoke automation, retaining existing windows for manual use."""
        with self._lock:
            if not self.process.stdin.closed:
                self.process.stdin.close()

    def wait(self, timeout=10):
        code = self.process.wait(timeout=timeout)
        self._reader.join(timeout=2)
        self.process.stdout.close()
        errors = self.process.stderr.read().decode("utf-8", errors="replace")
        self.process.stderr.close()
        if code:
            raise RuntimeError(f"Host exited {code}: {errors}")


def save_image(observation, path):
    import base64
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(base64.b64decode(observation["image"]["base64"]))
