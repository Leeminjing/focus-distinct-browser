"""Theme/DPI render checks using only the test process's private registry view."""
from pathlib import Path
import struct
import subprocess
import sys
import zlib
import ctypes
from native_ui import user32, snapshot, rectangle, text

ROOT = Path(__file__).resolve().parents[1]


def png_pixels(path):
    data = path.read_bytes()
    position, compressed = 8, bytearray()
    while position < len(data):
        length = struct.unpack(">I", data[position:position + 4])[0]
        kind = data[position + 4:position + 8]
        content = data[position + 8:position + 8 + length]
        if kind == b"IDAT":
            compressed.extend(content)
        position += length + 12
    width, height = struct.unpack(">II", data[16:24])
    raw = zlib.decompress(compressed)
    assert len(raw) == (width * 3 + 1) * height
    return width, raw


def run():
    executable = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/toolbar-component.exe"
    for mode in ([], ["--unaware"]):
        old_awareness = user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-1 if mode else -4))
        process = subprocess.Popen([str(executable), *mode], stdout=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
        hwnd = int(process.stdout.readline())
        try:
            colors = []
            for theme in (0, 1, 2, 3):
                dpi = user32.SendMessageW(hwnd, 0x8000 + 100, theme, 0)
                if mode:
                    assert dpi == 96, dpi
                rects = [rectangle(user32.GetDlgItem(hwnd, item)) for item in (104, 105, 106, 102, 107, 101)]
                assert all(a[2] <= b[0] for a, b in zip(rects, rects[1:])), rects
                assert abs((rects[-1][3] - rects[-1][1]) * 96 / dpi - 36) < 2
                caption = text(user32.GetDlgItem(hwnd, 107))
                if theme == 3:
                    assert "故障" in caption and not user32.IsWindowEnabled(user32.GetDlgItem(hwnd, 101))
                elif theme == 2:
                    assert caption == "人工控制"
                else:
                    assert "Agent" in caption and "操作" in caption
                label = "faulted" if theme == 3 else "manual" if theme == 2 else "auto"
                path = ROOT / "artifacts" / f"toolbar-{'light' if theme == 0 else 'dark'}-{dpi}-{label}.png"
                snapshot(hwnd, path)
                width, pixels = png_pixels(path)
                # Sample toolbar background between the edit/button row and the
                # bottom divider, using the actual button rectangle as an anchor.
                bounds = rectangle(hwnd)
                # PrintWindow renders a DPI-unaware host in its native 96-DPI
                # coordinates, while PMv2 renders in physical pixels.
                y = round((rects[-1][3] - bounds[1]) + 5 * dpi / 96)
                x = round(20 * dpi / 96)
                offset = y * (width * 3 + 1) + 1 + x * 3
                colors.append(tuple(pixels[offset:offset + 3]))
            assert sum(colors[0]) > 600 and sum(colors[1]) < 150, colors
            print(f"PASS private light/dark palettes, readable status, DPI {dpi}, disjoint native controls", flush=True)
        finally:
            user32.SendMessageW(hwnd, 0x0010, 0, 0)
            assert process.wait(timeout=10) == 0
            process.stdout.close()
            user32.SetThreadDpiAwarenessContext(old_awareness)


if __name__ == "__main__":
    run()
