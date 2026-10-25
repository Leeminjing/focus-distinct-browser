#pragma once
#include <windows.h>

namespace distinct {
inline bool browser_shortcut(UINT key, bool ctrl, bool alt, bool loading) {
    return (ctrl && (key == 'L' || key == 'R' || key == 'J')) || (alt && (key == 'D' || key == VK_LEFT || key == VK_RIGHT)) ||
        key == VK_F5 || (key == VK_ESCAPE && loading);
}
// Called only for messages targeting a browser's page HWND subtree, before
// TranslateMessage/DispatchMessage. CDP does not enter this native input path.
inline bool page_input_intent(const MSG& message) {
    switch (message.message) {
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_CHAR: case WM_SYSCHAR:
    case WM_IME_STARTCOMPOSITION: case WM_IME_COMPOSITION:
    case WM_TOUCH: case WM_GESTURE:
        return true;
    case WM_UNICHAR: return message.wParam != UNICODE_NOCHAR;
    case WM_MOUSEMOVE:
        return (message.wParam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) != 0;
    case WM_POINTERDOWN: return true;
    case WM_POINTERUPDATE: return IS_POINTER_INCONTACT_WPARAM(message.wParam);
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        switch (message.wParam) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN: return false;
        // Alt+Tab switches windows; its keydown may still target the old page.
        case VK_TAB: return (message.lParam & (1LL << 29)) == 0;
        default: return true;
        }
    default: return false;
    }
}
}
