#pragma once
#include <windows.h>
#include <functional>
#include <array>
#include <utility>
#include "page_input.h"

namespace distinct {
// Some WebView2 runtime keydowns never reach the host message loop. This hook
// only inspects a key when the caller confirms that its own browser window
// owns foreground keyboard focus. Only handled shell shortcuts are consumed;
// all page text/IME input continues unchanged. No input is synthesized.
class KeyboardGuard {
public:
    ~KeyboardGuard() { if (hook_) UnhookWindowsHookEx(hook_); active_ = nullptr; }
    KeyboardGuard() = default;
    KeyboardGuard(const KeyboardGuard&) = delete;
    KeyboardGuard& operator=(const KeyboardGuard&) = delete;
    bool start(std::function<HWND()> own_focus, std::function<bool(const MSG&)> on_key) {
        own_focus_ = std::move(own_focus); on_key_ = std::move(on_key);
        active_ = this;
        hook_ = SetWindowsHookEx(WH_KEYBOARD_LL, keyboard_proc, GetModuleHandle(nullptr), 0);
        if (!hook_) active_ = nullptr;
        return hook_ != nullptr;
    }
private:
    HHOOK hook_ = nullptr;
    std::function<HWND()> own_focus_;
    std::function<bool(const MSG&)> on_key_;
    std::array<bool, 256> suppressed_{};
    inline static thread_local KeyboardGuard* active_ = nullptr;
    static LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
        if (code == HC_ACTION && active_) {
            if (HWND focus = active_->own_focus_()) {
                const auto key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
                const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
                if (key->vkCode < active_->suppressed_.size() && active_->suppressed_[key->vkCode]) {
                    if (up) active_->suppressed_[key->vkCode] = false;
                    return 1;
                }
                if (up) return CallNextHookEx(nullptr, code, wparam, lparam);
                MSG message{}; message.hwnd = focus; message.message = static_cast<UINT>(wparam); message.wParam = key->vkCode;
                message.lParam = key->flags & LLKHF_ALTDOWN ? (1LL << 29) : 0;
                if (active_->on_key_(message) && key->vkCode < active_->suppressed_.size()) {
                    active_->suppressed_[key->vkCode] = true; return 1;
                }
            } else active_->suppressed_.fill(false);
        }
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }
};
}
