#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <thread>
#include <vector>
#include <algorithm>
#include "page_input.h"

namespace distinct {
// Only these atomic flags cross the hook/UI boundary. HWNDs are immutable;
// Control, COM objects and Browser collections remain owned by the UI thread.
struct KeyboardTarget {
    HWND window, page;
    DWORD ui_thread;
    std::atomic<bool> pending{false}, loading{true}, suppress_key_menu{false}, alive{true};
    KeyboardTarget(HWND window, HWND page) : window(window), page(page), ui_thread(GetWindowThreadProcessId(window, nullptr)) {}
};
class KeyboardGuard {
public:
    KeyboardGuard() = default;
    KeyboardGuard(const KeyboardGuard&) = delete;
    KeyboardGuard& operator=(const KeyboardGuard&) = delete;
    ~KeyboardGuard() {
        if (thread_.joinable()) {
            PostThreadMessage(thread_id_, WM_QUIT, 0, 0);
            thread_.join();
        }
    }
    bool start(UINT input_message, UINT shortcut_message) {
        input_message_ = input_message; shortcut_message_ = shortcut_message;
        std::promise<bool> started;
        auto ready = started.get_future();
        thread_ = std::thread([this, started = std::move(started)]() mutable {
            thread_id_ = GetCurrentThreadId();
            MSG message{}; PeekMessage(&message, nullptr, 0, 0, PM_NOREMOVE);
            active_ = this;
            HHOOK hook = SetWindowsHookEx(WH_KEYBOARD_LL, keyboard_proc, GetModuleHandle(nullptr), 0);
            started.set_value(hook != nullptr);
            if (hook) {
                while (GetMessage(&message, nullptr, 0, 0) > 0) {
                    TranslateMessage(&message); DispatchMessage(&message);
                }
                UnhookWindowsHookEx(hook);
            }
            active_ = nullptr;
        });
        return ready.get();
    }
    std::shared_ptr<KeyboardTarget> add(HWND window, HWND page) {
        auto target = std::make_shared<KeyboardTarget>(window, page);
        auto next = std::make_shared<Targets>(*std::atomic_load(&targets_));
        next->push_back(target);
        std::atomic_store(&targets_, std::shared_ptr<const Targets>(std::move(next)));
        return target;
    }
    void remove(const std::shared_ptr<KeyboardTarget>& target) {
        if (!target) return;
        target->alive = false;
        auto next = std::make_shared<Targets>(*std::atomic_load(&targets_));
        next->erase(std::remove(next->begin(), next->end(), target), next->end());
        std::atomic_store(&targets_, std::shared_ptr<const Targets>(std::move(next)));
    }
private:
    using Targets = std::vector<std::shared_ptr<KeyboardTarget>>;
    std::shared_ptr<const Targets> targets_ = std::make_shared<const Targets>();
    std::thread thread_;
    DWORD thread_id_ = 0;
    UINT input_message_ = 0, shortcut_message_ = 0;
    std::array<bool,256> suppressed_{};
    inline static thread_local KeyboardGuard* active_ = nullptr;

    LRESULT process(int code, WPARAM wparam, LPARAM lparam) {
        if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wparam, lparam);
        HWND foreground = GetForegroundWindow();
        auto targets = std::atomic_load(&targets_);
        for (const auto& target : *targets) {
            if (target->window != foreground || !target->alive) continue;
            GUITHREADINFO info{sizeof(info)};
            if (!GetGUIThreadInfo(target->ui_thread, &info) ||
                (info.hwndFocus != foreground && !IsChild(foreground, info.hwndFocus))) break;
            // Only read key data after confirming our own foreground focus.
            const auto key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
            const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
            if (key->vkCode < suppressed_.size() && suppressed_[key->vkCode]) {
                if (up) suppressed_[key->vkCode] = false;
                return 1;
            }
            if (up) return CallNextHookEx(nullptr, code, wparam, lparam);
            if (key->vkCode == VK_MENU || key->vkCode == VK_LMENU || key->vkCode == VK_RMENU)
                target->suppress_key_menu = false;
            MSG message{}; message.hwnd = info.hwndFocus; message.message = static_cast<UINT>(wparam); message.wParam = key->vkCode;
            message.lParam = key->flags & LLKHF_ALTDOWN ? (1LL << 29) : 0;
            const bool page = info.hwndFocus == target->page || IsChild(target->page, info.hwndFocus);
            const bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0, alt = (key->flags & LLKHF_ALTDOWN) != 0;
            const bool shortcut = browser_shortcut(key->vkCode, ctrl, alt, target->loading);
            if ((page && page_input_intent(message)) || shortcut) {
                // Fence before releasing original input; UI dispatch gates
                // read it even if the notification is behind a queued RPC.
                if (!target->pending.exchange(true)) PostMessage(target->window, input_message_, 0, 0);
            }
            if (shortcut && key->vkCode < suppressed_.size()) {
                if (alt) target->suppress_key_menu = true;
                if (PostMessage(target->window, shortcut_message_, key->vkCode, (ctrl ? 1 : 0) | (alt ? 2 : 0))) {
                    suppressed_[key->vkCode] = true;
                    return 1;
                }
            }
            return CallNextHookEx(nullptr, code, wparam, lparam);
        }
        suppressed_.fill(false);
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    static LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
        return active_ ? active_->process(code, wparam, lparam) : CallNextHookEx(nullptr, code, wparam, lparam);
    }
};
}
