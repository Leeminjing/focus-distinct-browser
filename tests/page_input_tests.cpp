#include "../src/window/page_input.h"
#include <cstdlib>
#include <iostream>

int main() {
    struct Case { UINT message; WPARAM wparam; LPARAM lparam; bool intent; };
    const Case cases[] = {
        {WM_LBUTTONDOWN, MK_LBUTTON, 0, true}, {WM_RBUTTONDOWN, MK_RBUTTON, 0, true},
        {WM_MBUTTONDOWN, MK_MBUTTON, 0, true}, {WM_XBUTTONDOWN, MK_XBUTTON1, 0, true},
        {WM_LBUTTONDBLCLK, MK_LBUTTON, 0, true}, {WM_MOUSEWHEEL, 120 << 16, 0, true},
        {WM_MOUSEHWHEEL, 120 << 16, 0, true}, {WM_KEYDOWN, 'A', 0, true},
        {WM_KEYDOWN, VK_PROCESSKEY, 0, true}, {WM_CHAR, L'中', 0, true},
        {WM_IME_STARTCOMPOSITION, 0, 0, true}, {WM_IME_COMPOSITION, 0, 0, true},
        {WM_MOUSEMOVE, MK_LBUTTON, 0, true}, {WM_POINTERDOWN, 1, 0, true},
        {WM_POINTERUPDATE, MAKEWPARAM(1, POINTER_MESSAGE_FLAG_INCONTACT), 0, true},
        {WM_TOUCH, 1, 0, true}, {WM_GESTURE, 0, 0, true},
        {WM_KEYDOWN, VK_TAB, 0, true}, {WM_UNICHAR, L'文', 0, true},
        {WM_MOUSEMOVE, 0, 0, false}, {WM_MOUSEMOVE, MK_SHIFT, 0, false},
        {WM_POINTERUPDATE, MAKEWPARAM(1, POINTER_MESSAGE_FLAG_INRANGE), 0, false},
        {WM_POINTERENTER, 1, 0, false}, {WM_SETFOCUS, 0, 0, false},
        {WM_ACTIVATE, WA_ACTIVE, 0, false}, {WM_MOUSEACTIVATE, 0, 0, false},
        {WM_KEYDOWN, VK_SHIFT, 0, false}, {WM_SYSKEYDOWN, VK_MENU, 0, false},
        {WM_KEYDOWN, VK_CONTROL, 0, false}, {WM_KEYDOWN, VK_LWIN, 0, false},
        {WM_SYSKEYDOWN, VK_TAB, 1LL << 29, false},
        {WM_UNICHAR, UNICODE_NOCHAR, 0, false}, {WM_PAINT, 0, 0, false},
    };
    for (const auto& test : cases) {
        MSG message{}; message.message = test.message; message.wParam = test.wparam; message.lParam = test.lparam;
        if (distinct::page_input_intent(message) != test.intent) {
            std::cerr << "Unexpected input arbitration for message " << test.message << '\n';
            return EXIT_FAILURE;
        }
    }
    std::cout << "page input intent tests passed (33 cases)\n";
}
