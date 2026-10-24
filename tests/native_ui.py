"""Win32 helpers for test-owned windows; screenshots need no Python packages."""
import ctypes
from ctypes import wintypes as W
from pathlib import Path
import struct
import zlib

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
user32.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
user32.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
for name, args, result in [
    ("GetDlgItem", [W.HWND, ctypes.c_int], W.HWND),
    ("SendMessageW", [W.HWND, W.UINT, W.WPARAM, W.LPARAM], ctypes.c_ssize_t),
    ("PostMessageW", [W.HWND, W.UINT, W.WPARAM, W.LPARAM], W.BOOL),
    ("GetWindowTextW", [W.HWND, W.LPWSTR, ctypes.c_int], ctypes.c_int),
    ("SetWindowTextW", [W.HWND, W.LPCWSTR], W.BOOL),
    ("GetWindowRect", [W.HWND, ctypes.POINTER(W.RECT)], W.BOOL),
    ("GetClientRect", [W.HWND, ctypes.POINTER(W.RECT)], W.BOOL),
    ("IsWindowEnabled", [W.HWND], W.BOOL),
    ("GetDpiForWindow", [W.HWND], W.UINT),
    ("SetWindowPos", [W.HWND, W.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, W.UINT], W.BOOL),
    ("GetWindowDC", [W.HWND], W.HDC),
    ("ReleaseDC", [W.HWND, W.HDC], ctypes.c_int),
    ("PrintWindow", [W.HWND, W.HDC, W.UINT], W.BOOL),
]:
    function = getattr(user32, name)
    function.argtypes, function.restype = args, result
for name, args, result in [
    ("CreateCompatibleDC", [W.HDC], W.HDC),
    ("CreateCompatibleBitmap", [W.HDC, ctypes.c_int, ctypes.c_int], W.HBITMAP),
    ("SelectObject", [W.HDC, W.HGDIOBJ], W.HGDIOBJ),
    ("GetDIBits", [W.HDC, W.HBITMAP, W.UINT, W.UINT, ctypes.c_void_p, ctypes.c_void_p, W.UINT], ctypes.c_int),
    ("DeleteObject", [W.HGDIOBJ], W.BOOL),
    ("DeleteDC", [W.HDC], W.BOOL),
]:
    function = getattr(gdi32, name)
    function.argtypes, function.restype = args, result


def text(hwnd):
    buffer = ctypes.create_unicode_buffer(16384)
    # GetWindowText intentionally does not retrieve another process's EDIT text.
    user32.SendMessageW(hwnd, 0x000D, len(buffer), ctypes.addressof(buffer))
    return buffer.value


def set_text(hwnd, value):
    buffer = ctypes.create_unicode_buffer(value)
    assert user32.SendMessageW(hwnd, 0x000C, 0, ctypes.addressof(buffer))


def click(hwnd, control):
    target = user32.GetDlgItem(hwnd, control)
    assert target and user32.IsWindowEnabled(target), (control, text(target))
    user32.SendMessageW(target, 0x00F5, 0, 0)  # BM_CLICK, same WM_COMMAND as a native button.


def rectangle(hwnd):
    value = W.RECT()
    assert user32.GetWindowRect(hwnd, ctypes.byref(value))
    return value.left, value.top, value.right, value.bottom


def snapshot(hwnd, path):
    """Print just this HWND; never capture the user's desktop or other apps."""
    user32.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user32.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
    old_dpi = user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    left, top, right, bottom = rectangle(hwnd)
    width, height = right - left, bottom - top
    dc = user32.GetWindowDC(hwnd)
    memory = gdi32.CreateCompatibleDC(dc)
    bitmap = gdi32.CreateCompatibleBitmap(dc, width, height)
    old = gdi32.SelectObject(memory, bitmap)
    try:
        assert user32.PrintWindow(hwnd, memory, 2), "PrintWindow failed"
        gdi32.SelectObject(memory, old)
        header = struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0)
        info = ctypes.create_string_buffer(header + bytes(16))
        pixels = ctypes.create_string_buffer(width * height * 4)
        assert gdi32.GetDIBits(memory, bitmap, 0, height, pixels, info, 0) == height
        raw = pixels.raw
        rgb = bytearray(width * height * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::4], raw[1::4], raw[0::4]
        def chunk(kind, content):
            return struct.pack(">I", len(content)) + kind + content + struct.pack(">I", zlib.crc32(kind + content))
        rows = b"".join(b"\0" + rgb[row * width * 3:(row + 1) * width * 3] for row in range(height))
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(png)
    finally:
        gdi32.DeleteObject(bitmap)
        gdi32.DeleteDC(memory)
        user32.ReleaseDC(hwnd, dc)
        user32.SetThreadDpiAwarenessContext(old_dpi)
