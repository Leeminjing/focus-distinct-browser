#include <windows.h>
#include <roapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include "json.h"
#include "control.h"
#include "window/native_toolbar.h"
#include "window/address_input.h"
#include "window/page_input.h"
#include "window/keyboard_guard.h"
#include "browser/site_interactions.h"
#include "host/pipe_output.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>
#include <algorithm>
#include <utility>
#include <cctype>
#include <cwctype>
#include <optional>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using namespace distinct;
namespace fs = std::filesystem;
constexpr UINT input_message = WM_APP + 1, disconnected_message = WM_APP + 2;
constexpr UINT manual_navigation_message = WM_APP + 4, focus_toolbar_message = WM_APP + 5;
constexpr UINT human_input_message = WM_APP + 6;
constexpr UINT page_shortcut_message = WM_APP + 7;
constexpr int page_host_id = 108;
static uint64_t now() { return GetTickCount64(); }
static std::string guid() {
    GUID id{}; checked(CoCreateGuid(&id), "CoCreateGuid");
    wchar_t text[40]{}; StringFromGUID2(id, text, 40);
    return winrt::to_string(std::wstring(text + 1, 36));
}
static std::string owned_text(LPWSTR text) {
    auto result = text ? winrt::to_string(text) : "";
    CoTaskMemFree(text); return result;
}
static Json parse(PCWSTR text) { return Json::Parse(text ? text : L"{}"); }
static std::wstring canonical_uri(PCWSTR text) {
    auto uri = winrt::Windows::Foundation::Uri(text).AbsoluteCanonicalUri();
    return std::wstring(uri.c_str(), uri.size());
}

#include "host/host.h"

void Host::finish_same_document_navigation(const std::shared_ptr<Page>& b) {
    auto op = b->operation();
    if (b->navigating || !op || op->kind != "navigate" || op->navigation_id != 0) return;
    LPWSTR source = nullptr;
    if (FAILED(b->view->get_Source(&source))) return;
    auto actual = owned_text(source);
    try {
        if (canonical_uri(winrt::to_hstring(actual).c_str()) == op->navigation_target) {
            navigation_succeeded(b, op, true);
            event(b, "navigation_completed", object({{L"success", jb(true)}, {L"same_document", jb(true)}}));
        }
    } catch (...) { finish(b, op, "navigation_failed", "Could not compare the same-document navigation URL"); }
}

void Page::layout() {
    if (!window) return;
    RECT bounds{}; GetClientRect(window, &bounds);
    if (toolbar) toolbar->layout();
    if (controller) {
        int height = toolbar ? toolbar->height_pixels() : MulDiv(NativeToolbar::height_dip, static_cast<int>(GetDpiForWindow(window)), 96);
        height = std::min(bounds.bottom, static_cast<LONG>(height));
        SetWindowPos(page_window, nullptr, 0, height, bounds.right, bounds.bottom - height, SWP_NOZORDER | SWP_NOACTIVATE);
        bounds.bottom -= height;
        controller->put_Bounds(bounds);
        controller->put_IsVisible(IsWindowVisible(window) && !IsIconic(window));
    }
}
void Page::refresh() {
    auto session = owner();
    if (!session || !window || closing || !toolbar) return;
    ShellState state;
    state.mode = owner()->control.mode; state.ready = ready; state.connected = host->connected;
    state.busy = owner()->control.busy; state.loading = navigating; state.notice = notice;
    if (state.notice.empty() && session->sites && (state.mode == Mode::manual || state.mode == Mode::taking_over || state.mode == Mode::paused))
        state.notice = session->sites->notice(id);
    if (keyboard) keyboard->loading = navigating;
    if (view) {
        BOOL back = FALSE, forward = FALSE;
        view->get_CanGoBack(&back); view->get_CanGoForward(&forward);
        state.can_back = back != FALSE; state.can_forward = forward != FALSE;
        LPWSTR caption = nullptr;
        if (SUCCEEDED(view->get_DocumentTitle(&caption))) {
            auto title = session->label + " | " + owned_text(caption) + " | Distinct Browser";
            SetWindowText(window, winrt::to_hstring(title).c_str());
        }
    }
    toolbar->update(state);
}
void Page::sync_input() {
    if (auto session = owner()) session->sync_input();
}
void Page::human_input() {
    if (closing || !ready || (owner()->control.mode != Mode::automatic && owner()->control.mode != Mode::available)) return;
    notice.clear();
    host->revoke(shared_from_this(), Mode::manual, "human_input");
}
void Page::user_command(ShellCommand command) {
    sync_input();
    if (closing || !ready || owner()->control.mode == Mode::faulted) return;
    auto self = shared_from_this();
    if (command == ShellCommand::downloads) {
        host->revoke(self, Mode::manual, "downloads_opened");
        if (owner()->sites) owner()->sites->show_downloads(self);
        return;
    }
    if (command == ShellCommand::takeover) {
        if (owner()->control.mode == Mode::manual || owner()->control.mode == Mode::paused) {
            if (owner()->sites && owner()->sites->pending()) {
                for (auto& [page_id, page] : owner()->pages) owner()->sites->show_pending(page);
                owner()->refresh(); return;
            }
            if (host->connected && owner()->visible() && owner()->control.hand_back().empty()) {
                notice.clear(); owner()->refresh(); host->event(self, "control_changed", object({{L"reason", js("hand_back")}}));
                controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
        } else if (owner()->control.mode == Mode::automatic || owner()->control.mode == Mode::available)
            host->revoke(self, Mode::manual, "takeover");
        return;
    }
    if (command == ShellCommand::focus_page || command == ShellCommand::focus_page_next || command == ShellCommand::focus_page_previous) {
        controller->MoveFocus(command == ShellCommand::focus_page_next ? COREWEBVIEW2_MOVE_FOCUS_REASON_NEXT :
            command == ShellCommand::focus_page_previous ? COREWEBVIEW2_MOVE_FOCUS_REASON_PREVIOUS : COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return;
    }
    // Human intent revokes the lease before touching the page. A single manual
    // navigation waits for issued commands to settle; it is never an auto queue.
    host->revoke(self, Mode::manual, command == ShellCommand::address_focus ? "address_focus" : "manual_navigation");
    notice.clear();
    if (command == ShellCommand::address_focus) { refresh(); return; }
    if (command == ShellCommand::reload_stop && navigating) {
        pending_manual.reset();
        if (FAILED(view->Stop())) { notice = L"无法停止加载"; refresh(); }
        return;
    }
    if (command == ShellCommand::navigate) {
        pending_address = toolbar->input_url();
        auto first = pending_address.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) { notice = L"请输入地址"; refresh(); return; }
        pending_address = pending_address.substr(first, pending_address.find_last_not_of(L" \t\r\n") - first + 1);
        auto url = address_url(pending_address);
        if (!url) {
            notice = L"地址格式不支持"; refresh(); return;
        }
        pending_address = std::move(*url);
    }
    pending_manual = command;
    if (operation()) {
        if (operation()->kind == "navigate") view->Stop();
    } else drain_manual();
}
void Page::drain_manual() {
    if (!pending_manual || owner()->control.busy) return;
    auto command = *pending_manual; pending_manual.reset();
    if (closing || !ready || owner()->control.mode != Mode::manual || !IsWindowVisible(window) || IsIconic(window)) return;
    HRESULT hr = S_OK;
    if (command == ShellCommand::navigate) hr = view->Navigate(pending_address.c_str());
    else if (command == ShellCommand::back) hr = view->GoBack();
    else if (command == ShellCommand::forward) hr = view->GoForward();
    else if (command == ShellCommand::reload_stop || command == ShellCommand::reload) hr = view->Reload();
    if (FAILED(hr)) notice = L"无法打开页面";
    else controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    refresh();
}

LRESULT CALLBACK PageProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto b = reinterpret_cast<Page*>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        b = static_cast<Page*>(reinterpret_cast<CREATESTRUCT*>(lparam)->lpCreateParams);
        b->window = window;
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(b));
    }
    if (!b) return DefWindowProc(window, message, wparam, lparam);
    try {
        LRESULT result = 0;
        if (b->toolbar && b->toolbar->parent_message(message, wparam, lparam, result)) return result;
        switch (message) {
        case WM_ACTIVATE:
            if (LOWORD(wparam) != WA_INACTIVE && b->ready && !b->closing)
                b->host->select(b->shared_from_this(), "window_activated");
            break;
        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_KEYMENU && b->keyboard && b->keyboard->suppress_key_menu.exchange(false)) {
                return 0;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto limits = reinterpret_cast<MINMAXINFO*>(lparam);
            UINT dpi = GetDpiForWindow(window);
            limits->ptMinTrackSize = {MulDiv(560, dpi, 96), MulDiv(360, dpi, 96)};
            return 0;
        }
        case WM_THEMECHANGED: case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE:
            if (b->toolbar) b->toolbar->theme_changed();
            return 0;
        case focus_toolbar_message:
            if (b->toolbar) b->toolbar->focus_from_page(wparam != 0);
            return 0;
        case human_input_message:
            b->sync_input();
            return 0;
        case page_shortcut_message:
            if (b->toolbar) b->toolbar->shortcut(static_cast<UINT>(wparam), (lparam & 1) != 0, (lparam & 2) != 0);
            return 0;
        case manual_navigation_message: b->drain_manual(); return 0;
        case WM_APP + 8:
            if (auto browser = b->owner(); browser && browser->sites && !b->closing)
                browser->sites->show_pending(b->shared_from_this());
            return 0;
        case WM_APP + 9:
            if (auto browser = b->owner(); browser && browser->sites && !b->closing)
                browser->sites->show_downloads(b->shared_from_this());
            return 0;
        case WM_MOVE:
            if (b->controller) b->controller->NotifyParentWindowPositionChanged();
            break;
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED && !b->closing)
                b->host->revoke(b->shared_from_this(), Mode::paused, "minimized");
            b->invalidate();
            b->layout(); return 0;
        case WM_SHOWWINDOW:
            if (!wparam && !b->closing) b->host->revoke(b->shared_from_this(), Mode::paused, "hidden");
            b->layout(); return 0;
        case WM_DPICHANGED: {
            auto rect = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            b->layout(); return 0;
        }
        case WM_CLOSE: {
            auto keep_alive = b->shared_from_this();
            b->host->close(keep_alive, "user_closed"); return 0;
        }
        case WM_NCDESTROY:
            SetWindowLongPtr(window, GWLP_USERDATA, 0); break;
        }
    } catch (...) { OutputDebugString(L"distinct-browser: window callback failed\n"); }
    return DefWindowProc(window, message, wparam, lparam);
}

void Host::initialize(const std::shared_ptr<Page>& b) {
    auto browser = b->owner();
    if (!browser->sites) browser->sites = std::make_shared<SiteInteractions>(this, browser);
    browser->sites->attach(b);
    std::weak_ptr<Page> weak = b;
    EventRegistrationToken registration{};
    // Host input processing lets the normal message loop handle shortcuts
    // before WebView2. AcceleratorKeyPressed is asynchronous in this mode.
    checked(b->controller->add_MoveFocusRequested(Callback<ICoreWebView2MoveFocusRequestedEventHandler>(
        [weak](ICoreWebView2Controller*, ICoreWebView2MoveFocusRequestedEventArgs* args) -> HRESULT {
            if (auto b = weak.lock(); b && !b->closing) {
                COREWEBVIEW2_MOVE_FOCUS_REASON reason{}; args->get_Reason(&reason);
                args->put_Handled(TRUE);
                PostMessage(b->window, focus_toolbar_message, reason == COREWEBVIEW2_MOVE_FOCUS_REASON_PREVIOUS, 0);
            }
            return S_OK;
        }).Get(), &registration), "MoveFocusRequested");
    checked(b->view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [this, weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            if (auto b = weak.lock(); b && !b->closing) {
                if (b->owner()->sites) b->owner()->sites->stop_page(b->id, "page_navigated", false);
                b->navigating = true; b->invalidate();
                b->notice.clear();
                args->get_NavigationId(&b->navigation_id);
                auto op = b->operation();
                if (op && op->kind == "navigate") {
                    if (op->navigation_id == 0) {
                        LPWSTR uri = nullptr;
                        args->get_Uri(&uri);
                        auto requested = owned_text(uri);
                        try {
                            if (canonical_uri(winrt::to_hstring(requested).c_str()) == op->navigation_target)
                                op->navigation_id = b->navigation_id;
                            else finish(b, op, "navigation_superseded", "Another navigation started before the requested navigation");
                        } catch (...) { finish(b, op, "navigation_failed", "Could not match the starting navigation URL"); }
                    }
                    else if (op->navigation_id != b->navigation_id)
                        finish(b, op, "navigation_superseded", "A new document navigation replaced the requested navigation");
                }
                b->refresh();
            }
            return S_OK;
        }).Get(), &registration), "NavigationStarting");
    checked(b->view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [this, weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            auto b = weak.lock(); if (!b || b->closing) return S_OK;
            uint64_t navigation_id = 0; args->get_NavigationId(&navigation_id);
            bool latest = navigation_id == b->navigation_id;
            if (latest) b->navigating = false;
            BOOL ok = FALSE; args->get_IsSuccess(&ok);
            if (!b->ready && latest) {
                if (!ok) { close(b, "Initial navigation failed"); return S_OK; }
                ready_page(b);
            }
            auto op = b->operation();
            // Settle the request by its own ID, independently of which document
            // now owns the browser's loading state. Old completions cannot clear
            // the replacement navigation's loading flag.
            if (op && op->kind == "navigate" && op->navigation_id == navigation_id) {
                if (ok) navigation_succeeded(b, op, false);
                else {
                    COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                    args->get_WebErrorStatus(&status);
                    bool cancelled = status == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED;
                    finish(b, op, cancelled ? "navigation_cancelled" : "navigation_failed",
                        cancelled ? "Navigation was cancelled; inspect the current page" : "Navigation did not complete successfully; inspect the current page and downloads before deciding the next action",
                        object({{L"web_error_status", jn(status)}}));
                }
            }
            if (latest) event(b, "navigation_completed", object({{L"success", jb(ok != FALSE)}, {L"same_document", jb(false)}}));
            if (latest && !ok && !op) {
                COREWEBVIEW2_WEB_ERROR_STATUS status{}; args->get_WebErrorStatus(&status);
                if (status != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED && status != COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED)
                    b->notice = L"加载未完成";
            }
            b->refresh();
            return S_OK;
        }).Get(), &registration), "NavigationCompleted");
    checked(b->view->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
        [this, weak](ICoreWebView2* view, ICoreWebView2SourceChangedEventArgs* args) -> HRESULT {
            if (auto b = weak.lock(); b && !b->closing) {
                b->invalidate();
                LPWSTR source = nullptr;
                if (SUCCEEDED(view->get_Source(&source))) {
                    if (b->toolbar) b->toolbar->set_url(source);
                    CoTaskMemFree(source);
                    BOOL new_document = TRUE; args->get_IsNewDocument(&new_document);
                    // Fragment navigation has no NavigationStarting/Completed.
                    // Match the requested URL, not an unrelated history update.
                    if (!new_document) finish_same_document_navigation(b);
                }
            }
            return S_OK;
        }).Get(), &registration), "SourceChanged");
    checked(b->view->add_HistoryChanged(Callback<ICoreWebView2HistoryChangedEventHandler>(
        [this, weak](ICoreWebView2*, IUnknown*) -> HRESULT {
            // Repeating the current fragment can change history without a
            // SourceChanged event. A same-URL full document reload must still
            // wait for its NavigationStarting/Completed sequence.
            if (auto b = weak.lock(); b && !b->closing) {
                if (b->operation() && b->operation()->navigation_target.find(L'#') != std::wstring::npos)
                    finish_same_document_navigation(b);
                b->refresh();
            }
            return S_OK;
        }).Get(), &registration), "HistoryChanged");
    checked(b->view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [this, weak](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            args->put_Handled(TRUE);
            if (auto b = weak.lock(); b && !b->closing) {
                try { new_window(b, args); }
                catch (...) { event(b->owner(), "page_creation_failed", object({{L"opener_page_id", js(b->id)}, {L"reason", js("new_window_setup_failed")}})); }
            }
            return S_OK;
        }).Get(), &registration), "NewWindowRequested");
    checked(b->view->add_DocumentTitleChanged(Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
        [weak](ICoreWebView2*, IUnknown*) -> HRESULT {
            if (auto page = weak.lock(); page && !page->closing) page->refresh();
            return S_OK;
        }).Get(), &registration), "DocumentTitleChanged");
    checked(b->view->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
        [this, weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
            if (auto b = weak.lock()) fault(b, "WebView2 process failed; no automatic action was retried");
            return S_OK;
        }).Get(), &registration), "ProcessFailed");
    checked(b->view->add_WindowCloseRequested(Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
        [this, weak](ICoreWebView2*, IUnknown*) -> HRESULT {
            if (auto b = weak.lock()) close(b, "page_requested_close");
            return S_OK;
        }).Get(), &registration), "WindowCloseRequested");
}

static std::string base64(const BYTE* bytes, DWORD count) {
    DWORD length = 0;
    if (!CryptBinaryToStringA(bytes, count, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length))
        throw Error("capture_failed", "Cannot size base64 image");
    std::string text(length, '\0');
    if (!CryptBinaryToStringA(bytes, count, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, text.data(), &length))
        throw Error("capture_failed", "Cannot encode image");
    text.resize(length); return text;
}
void Host::observe(const std::shared_ptr<Page>& b, const std::shared_ptr<Operation>& op) {
    if (b->navigating) { finish(b, op, "page_loading", "Wait for navigation_completed and observe again"); return; }
    op->revision = b->revision;
    auto error = gate(b, op);
    if (!error.empty()) { finish(b, op, error, "Control revoked before observation"); return; }
    HRESULT hr = b->view->CallDevToolsProtocolMethod(L"Page.getLayoutMetrics", L"{}",
        Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([this, b, op](HRESULT error, PCWSTR text) -> HRESULT {
            if (op->done) return S_OK;
            if (FAILED(error)) { finish(b, op, "observation_failed", "Cannot read CSS viewport"); return S_OK; }
            try {
                auto denied = gate(b, op);
                if (!denied.empty()) { finish(b, op, denied, "Control revoked during observation"); return S_OK; }
                auto metrics = parse(text);
                auto viewport = metrics.GetNamedObject(L"cssVisualViewport");
                double width = viewport.GetNamedNumber(L"clientWidth"), height = viewport.GetNamedNumber(L"clientHeight");
                ComPtr<IStream> stream;
                checked(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "CreateStreamOnHGlobal");
                HRESULT result = b->view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
                    Callback<ICoreWebView2CapturePreviewCompletedHandler>([this, b, op, stream, width, height](HRESULT error) -> HRESULT {
                        if (op->done) return S_OK;
                        try {
                            checked(error, "CapturePreview");
                            auto denied = gate(b, op);
                            if (!denied.empty()) { finish(b, op, denied, "Control revoked during capture"); return S_OK; }
                            if (op->revision != b->revision || op->selection_revision != b->owner()->selection_revision || b->navigating) {
                                finish(b, op, "page_changed", "Page navigated or viewport changed during observation; observe again"); return S_OK;
                            }
                            STATSTG info{}; checked(stream->Stat(&info, STATFLAG_NONAME), "IStream::Stat");
                            if (info.cbSize.QuadPart < 24 || info.cbSize.QuadPart > 24 * 1024 * 1024)
                                throw Error("capture_failed", "Unexpected or oversized PNG capture");
                            checked(stream->Seek({}, STREAM_SEEK_SET, nullptr), "IStream::Seek");
                            std::vector<BYTE> bytes(static_cast<size_t>(info.cbSize.QuadPart));
                            ULONG read = 0; checked(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read), "IStream::Read");
                            if (read != bytes.size()) throw Error("capture_failed", "Incomplete image stream");
                            auto dimension = [&bytes](int offset) {
                                return (static_cast<uint32_t>(bytes[offset]) << 24) | (static_cast<uint32_t>(bytes[offset + 1]) << 16)
                                    | (static_cast<uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
                            };
                            LPWSTR url = nullptr, title = nullptr;
                            checked(b->view->get_Source(&url), "get_Source");
                            auto source = owned_text(url);
                            checked(b->view->get_DocumentTitle(&title), "get_DocumentTitle");
                            auto caption = owned_text(title);
                            Json image = object({{L"mime_type", js("image/png")}, {L"base64", js(base64(bytes.data(), read))},
                                {L"width", jn(dimension(16))}, {L"height", jn(dimension(20))}});
                            b->viewport_width = width; b->viewport_height = height;
                            b->owner()->control.observed = true;
                            finish(b, op, "", "", object({{L"page_id", js(b->id)}, {L"url", js(source)}, {L"title", js(caption)},
                                {L"viewport", object({{L"width", jn(width)}, {L"height", jn(height)}, {L"coordinate_space", js("css_viewport")}})},
                                {L"image", image}, {L"state", js(mode_name(b->owner()->control.mode))},
                                {L"site_interactions", b->owner()->sites ? b->owner()->sites->snapshot(b->id) : Json()}}));
                        } catch (const Error& e) { finish(b, op, e.code, e.what()); }
                        catch (...) { finish(b, op, "observation_failed", "Capture or metadata failed"); }
                        return S_OK;
                    }).Get());
                if (FAILED(result)) finish(b, op, "capture_failed", "CapturePreview could not start");
            } catch (...) { finish(b, op, "observation_failed", "Invalid viewport result or capture initialization failure"); }
            return S_OK;
        }).Get());
    if (FAILED(hr)) finish(b, op, "observation_failed", "Viewport observation could not start");
}

void Host::dispatch_step(const std::shared_ptr<Page>& b, const std::shared_ptr<Operation>& op) {
    if (op->done) return;
    b->sync_input();
    // The callback of the last dispatched step is always recorded, including
    // after takeover. Only a subsequent dispatch requires a still-valid lease.
    if (op->completed == op->steps.size()) {
        expire(b);
        finish(b, op, "", "", object({{L"command_processed", jb(true)}, {L"business_success_verified", jb(false)},
            {L"control_still_valid", jb(b->owner()->control.check(op->token, now()).empty())}})); return;
    }
    auto error = gate(b, op);
    if (!error.empty()) { finish(b, op, error, "Control revoked; remaining steps were not dispatched"); return; }
    if (op->revision != b->revision || op->selection_revision != b->owner()->selection_revision || b->navigating) {
        finish(b, op, "page_changed", "Page or viewport changed; remaining steps were not dispatched"); return;
    }
    auto& step = op->steps[op->completed];
    auto parameters = step.second.Stringify();
    ++op->dispatched;
    HRESULT hr = b->view->CallDevToolsProtocolMethod(step.first.c_str(), parameters.c_str(),
        Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([this, b, op](HRESULT error, PCWSTR text) -> HRESULT {
            if (op->done) return S_OK;
            if (FAILED(error)) { finish(b, op, "action_failed", "CDP returned an error; effects may be partial"); return S_OK; }
            try {
                if (parse(text).HasKey(L"error")) { finish(b, op, "action_failed", "CDP rejected the command"); return S_OK; }
                ++op->completed;
                dispatch_step(b, op);
            } catch (...) { finish(b, op, "action_failed", "Invalid CDP response"); }
            return S_OK;
        }).Get());
    if (FAILED(hr)) { --op->dispatched; finish(b, op, "action_failed", "CDP dispatch failed"); }
}

void Host::handle(const std::string& line) {
    if (!connected) return;
    std::string id;
    try {
        auto request = Json::Parse(winrt::to_hstring(line));
        id = string(request, L"id", 128);
        if (pending.count(id)) {
            // Do not send a second response with the same id; it could complete
            // the original caller's promise incorrectly.
            send(object({{L"event", js("protocol_error")}, {L"detail", object({{L"code", js("duplicate_id")}, {L"id", js(id)}})}})); return;
        }
        pending.insert(id);
        auto method = string(request, L"method", 32);
        if (method != "create" && method != "close" && method != "present" && method != "acquire"
            && method != "renew" && method != "release" && method != "observe" && method != "act"
            && method != "list_pages" && method != "select_page" && method != "close_page" && method != "list_downloads")
            throw Error("unknown_method", "Unknown method");
        Json parameters = request.HasKey(L"params") ? request.GetNamedObject(L"params") : Json();
        if (method == "create") { create(id, string(parameters, L"label", 256)); return; }
        auto browser_id = string(parameters, L"browser_id", 128);
        auto found = browsers.find(browser_id);
        if (found == browsers.end()) throw Error("browser_not_found", "Browser does not exist or has been closed");
        auto session = found->second;
        session->sync_input();
        if (method == "close") { close_browser(session, "caller_closed"); success(id); return; }
        if (!session->ready) throw Error("browser_not_ready", "Browser is still initializing");
        auto b = session->selected;
        if (!b) throw Error("browser_not_ready", "No ready page");
        expire(b);
        if (method == "list_downloads") {
            auto result = session->sites ? session->sites->snapshot() : Json();
            if (result.HasKey(L"permissions")) result.Remove(L"permissions");
            success(id, result); return;
        }
        if (method == "list_pages") {
            winrt::Windows::Data::Json::JsonArray pages;
            for (auto& [page_id, page] : session->pages) {
                LPWSTR source = nullptr, title = nullptr;
                if (page->view) { page->view->get_Source(&source); page->view->get_DocumentTitle(&title); }
                pages.Append(object({{L"page_id", js(page_id)}, {L"opener_page_id", js(page->opener_id)},
                    {L"url", js(owned_text(source))}, {L"title", js(owned_text(title))},
                    {L"hwnd", js(std::to_string(reinterpret_cast<uintptr_t>(page->window)))},
                    {L"ready", jb(page->ready)}, {L"loading", jb(page->navigating)}}));
            }
            success(id, object({{L"selected_page_id", js(b->id)}, {L"pages", pages}})); return;
        }
        if (method == "present") {
            if (parameters.HasKey(L"page_id")) {
                auto page = session->pages.find(string(parameters, L"page_id", 128));
                if (page == session->pages.end()) throw Error("page_not_found", "Page does not belong to this browser or was closed");
                b = page->second;
                if (!b->ready) throw Error("page_not_ready", "Page is initializing");
                select(b, "presented");
            }
            ShowWindow(b->window, IsIconic(b->window) ? SW_RESTORE : SW_SHOW);
            b->layout();
            bool foreground = SetForegroundWindow(b->window) != FALSE;
            success(id, object({{L"visible", jb(IsWindowVisible(b->window) != FALSE)},
                {L"foreground", jb(foreground)}, {L"state", js(mode_name(session->control.mode))}})); return;
        }
        if (method == "acquire") {
            if (!session->visible()) throw Error("window_paused", "Present the browser pages and hand back control first");
            auto client = string(parameters, L"client_id", 128);
            double ttl = parameters.HasKey(L"ttl_ms") ? number(parameters, L"ttl_ms", 1000, 300000) : 30000;
            auto token = guid();
            auto error = b->owner()->control.acquire(client, token, now(), static_cast<uint64_t>(ttl));
            if (!error.empty()) throw Error(error, "Browser is occupied or does not allow automatic control");
            session->refresh(); event(b, "control_changed");
            success(id, object({{L"token", js(token)}, {L"ttl_ms", jn(ttl)}})); return;
        }
        if (method != "renew" && method != "release" && method != "observe" && method != "act" && method != "select_page" && method != "close_page")
            throw Error("unknown_method", "Unknown method");
        auto token = string(parameters, L"token", 128);
        auto error = b->owner()->control.check(token, now());
        if (!error.empty()) throw Error(error, "Lease is invalid, expired or revoked");
        if (method == "renew") {
            double ttl = parameters.HasKey(L"ttl_ms") ? number(parameters, L"ttl_ms", 1000, 300000) : 30000;
            b->owner()->control.expires = now() + static_cast<uint64_t>(ttl);
            success(id, object({{L"ttl_ms", jn(ttl)}})); return;
        }
        if (method == "release") { revoke(b, Mode::available, "released"); success(id); return; }
        if (method == "select_page" || method == "close_page" || parameters.HasKey(L"page_id")) {
            auto page_id = string(parameters, L"page_id", 128);
            auto page = session->pages.find(page_id);
            if (page == session->pages.end()) throw Error("page_not_found", "Page does not belong to this browser or was closed");
            if (method == "select_page" || method == "close_page") {
                if (session->control.busy) throw Error("busy", "A browser operation is still in flight");
                b = page->second;
                if (method == "close_page") { close(b, "caller_closed_page"); success(id); return; }
                if (!b->ready) throw Error("page_not_ready", "Page is initializing");
                select(b, "caller_selected");
                ShowWindow(b->window, IsIconic(b->window) ? SW_RESTORE : SW_SHOW);
                b->layout();
                bool foreground = SetForegroundWindow(b->window) != FALSE;
                success(id, object({{L"page_id", js(b->id)}, {L"foreground", jb(foreground)}})); return;
            }
            if (page->second != b) throw Error("page_not_selected", "Select this page and observe before acting");
        }
        auto op = std::make_shared<Operation>();
        op->request = id; op->token = token; op->kind = method; op->revision = b->revision;
        op->page = b; op->selection_revision = b->owner()->selection_revision;
        std::wstring destination;
        if (method == "act") {
            auto action = parameters.GetNamedObject(L"action");
            auto type = string(action, L"type", 32);
            if (type == "navigate") {
                auto url = string(action, L"url", 8192);
                // No JavaScript URLs or arbitrary script/CDP escape hatch.
                auto colon = url.find(':'); auto scheme = url.substr(0, colon);
                std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (scheme != "http" && scheme != "https" && scheme != "about" && scheme != "file")
                    throw Error("invalid_action", "Supported URL schemes: http, https, about, file");
                destination = winrt::to_hstring(url).c_str(); op->kind = "navigate";
                op->navigation_target = canonical_uri(destination.c_str());
            } else if (type == "text") {
                auto text = string(action, L"text", 65536);
                op->steps.emplace_back(L"Input.insertText", object({{L"text", js(text)}}));
            } else if (type == "key") {
                auto key = string(action, L"key", 32);
                static const std::map<std::string, int> keys = {{"Enter", VK_RETURN}, {"Tab", VK_TAB}, {"Escape", VK_ESCAPE},
                    {"Backspace", VK_BACK}, {"Delete", VK_DELETE}, {"ArrowLeft", VK_LEFT}, {"ArrowRight", VK_RIGHT},
                    {"ArrowUp", VK_UP}, {"ArrowDown", VK_DOWN}, {"Home", VK_HOME}, {"End", VK_END},
                    {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"Space", VK_SPACE}};
                auto found_key = keys.find(key);
                if (found_key == keys.end()) throw Error("invalid_action", "Unsupported key name");
                auto wire_key = key == "Space" ? " " : key;
                for (auto kind : {"keyDown", "keyUp"})
                    op->steps.emplace_back(L"Input.dispatchKeyEvent", object({{L"type", js(kind)}, {L"key", js(wire_key)},
                        {L"code", js(key)}, {L"windowsVirtualKeyCode", jn(found_key->second)}, {L"nativeVirtualKeyCode", jn(found_key->second)},
                        {L"text", js(std::string(kind) == "keyDown" ? (key == "Enter" ? "\r" : key == "Space" ? " " : "") : "")}}));
            } else if (type == "click" || type == "scroll") {
                double x = number(action, L"x", 0, std::max(0.0, b->viewport_width - 1));
                double y = number(action, L"y", 0, std::max(0.0, b->viewport_height - 1));
                if (type == "click") {
                    for (auto kind : {"mousePressed", "mouseReleased"})
                        op->steps.emplace_back(L"Input.dispatchMouseEvent", object({{L"type", js(kind)}, {L"x", jn(x)}, {L"y", jn(y)},
                            {L"button", js("left")}, {L"buttons", jn(std::string(kind) == "mousePressed" ? 1 : 0)}, {L"clickCount", jn(1)}}));
                } else {
                    double dx = action.HasKey(L"delta_x") ? number(action, L"delta_x", -10000, 10000) : 0;
                    double dy = number(action, L"delta_y", -10000, 10000);
                    op->steps.emplace_back(L"Input.dispatchMouseEvent", object({{L"type", js("mouseWheel")}, {L"x", jn(x)}, {L"y", jn(y)},
                        {L"deltaX", jn(dx)}, {L"deltaY", jn(dy)}}));
                }
            } else throw Error("invalid_action", "Supported actions: navigate, click, scroll, text, key");
        }
        error = b->owner()->control.begin(token, now(), method == "act");
        if (!error.empty()) throw Error(error, "Observe first, or wait for the current operation to finish");
        b->owner()->active = op;
        if (method == "observe") { observe(b, op); return; }
        error = gate(b, op);
        if (!error.empty()) { finish(b, op, error, "Control unavailable at dispatch"); return; }
        if (b->navigating) { finish(b, op, "page_loading", "Wait for navigation to complete and observe again"); return; }
        if (op->kind == "navigate") {
            op->dispatched = 1;
            HRESULT hr = b->view->Navigate(destination.c_str());
            if (FAILED(hr)) { op->dispatched = 0; finish(b, op, "navigation_failed", "Navigation could not start"); }
        } else dispatch_step(b, op);
    } catch (const Error& e) { failure(id, e.code, e.what()); }
    catch (const winrt::hresult_error& e) { failure(id, "invalid_request", winrt::to_string(e.message())); }
    catch (const std::exception& e) { failure(id, "internal_error", e.what()); }
}

static LRESULT CALLBACK DispatcherProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto host = reinterpret_cast<Host*>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        host = static_cast<Host*>(reinterpret_cast<CREATESTRUCT*>(lparam)->lpCreateParams);
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    if (host) {
        if (message == input_message) {
            std::unique_ptr<std::string> line(reinterpret_cast<std::string*>(lparam));
            ReleaseSemaphore(reinterpret_cast<HANDLE>(wparam), 1, nullptr);
            host->handle(*line); return 0;
        }
        if (message == disconnected_message) { host->disconnect(); return 0; }
        if (message == WM_TIMER) { host->tick(); return 0; }
    }
    return DefWindowProc(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        // No console allocation: redirected handles are supplied by the caller.
        auto input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (GetFileType(input) != FILE_TYPE_PIPE || GetFileType(output) != FILE_TYPE_PIPE)
            throw Error("pipes_required", "Launch using the demo/client with redirected stdin and stdout");
        using VersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto version_fn = reinterpret_cast<VersionFn>(GetProcAddress(GetModuleHandle(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW version{}; version.dwOSVersionInfoSize = sizeof(version);
        if (!version_fn || version_fn(&version) != 0 || version.dwBuildNumber < 22000)
            throw Error("unsupported_os", "Requires Windows 11 or later");
        Host host;
        PWSTR local = nullptr; checked(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local), "LocalAppData");
        host.data_root = fs::path(local) / L"DistinctBrowser" / L"Profiles";
        CoTaskMemFree(local); fs::create_directories(host.data_root);
        WNDCLASS window_class{}; window_class.lpfnWndProc = PageProc; window_class.hInstance = instance;
        window_class.hIcon = LoadIcon(instance, MAKEINTRESOURCE(2));
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW); window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = L"DistinctBrowserWindow";
        if (!RegisterClass(&window_class)) throw Error("startup_failed", "Cannot register browser class");
        WNDCLASS page_class{}; page_class.lpfnWndProc = DefWindowProc; page_class.hInstance = instance;
        page_class.hCursor = LoadCursor(nullptr, IDC_ARROW); page_class.lpszClassName = L"DistinctBrowserPage";
        if (!RegisterClass(&page_class)) throw Error("startup_failed", "Cannot register page input class");
        WNDCLASS dispatch_class{}; dispatch_class.lpfnWndProc = DispatcherProc; dispatch_class.hInstance = instance;
        dispatch_class.lpszClassName = L"DistinctBrowserDispatcher";
        if (!RegisterClass(&dispatch_class)) throw Error("startup_failed", "Cannot register dispatcher class");
        host.dispatcher = CreateWindowEx(0, dispatch_class.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, &host);
        if (!host.dispatcher) throw Error("startup_failed", "Cannot create dispatcher");
        host.output = std::make_unique<PipeOutput>(output, [dispatcher = host.dispatcher] { PostMessage(dispatcher, disconnected_message, 0, 0); });
        SetTimer(host.dispatcher, 1, 200, nullptr);
        if (!host.keyboard_guard.start(human_input_message, page_shortcut_message))
            throw Error("startup_failed", "Cannot install native keyboard takeover protection");
        host.send(object({{L"event", js("ready")}, {L"protocol_version", jn(1)}, {L"pid", jn(GetCurrentProcessId())}}));
        std::atomic<bool> stopping = false;
        HANDLE slots = CreateSemaphore(nullptr, 128, 128, nullptr);
        if (!slots) throw Error("startup_failed", "Cannot allocate input queue semaphore");
        std::thread reader([&, dispatcher = host.dispatcher] {
            std::string line; bool oversized = false;
            auto post = [&](std::string text) {
                while (!stopping) {
                    if (WaitForSingleObject(slots, 100) != WAIT_OBJECT_0) continue;
                    auto message = std::make_unique<std::string>(std::move(text));
                    if (PostMessage(dispatcher, input_message, reinterpret_cast<WPARAM>(slots), reinterpret_cast<LPARAM>(message.get()))) message.release();
                    else ReleaseSemaphore(slots, 1, nullptr);
                    return;
                }
            };
            char buffer[8192]; DWORD count = 0;
            while (!stopping && ReadFile(input, buffer, sizeof(buffer), &count, nullptr) && count) {
                for (DWORD i = 0; i < count; ++i) {
                    char ch = buffer[i];
                    if (ch == '\n') {
                        if (!oversized && !line.empty()) { if (line.back() == '\r') line.pop_back(); post(std::move(line)); }
                        else if (oversized) post("{}");
                        line.clear(); oversized = false;
                    } else if (!oversized) {
                        if (line.size() >= 1024 * 1024) { oversized = true; line.clear(); }
                        else line += ch;
                    }
                }
            }
            // A final non-newline-terminated fragment is deliberately discarded.
            PostMessage(dispatcher, disconnected_message, 0, 0);
        });
        MSG message{};
        while (GetMessage(&message, nullptr, 0, 0) > 0) {
            bool handled = false;
            bool capture_page_drag = false;
            HWND root = GetAncestor(message.hwnd, GA_ROOT);
            for (auto& [id, session] : host.browsers) {
              if (session->sites && session->sites->pre_translate(message)) { handled = true; break; }
              for (auto& [page_id, browser] : session->pages) {
                if (browser->window != root) continue;
                const bool page = message.hwnd == browser->page_window || IsChild(browser->page_window, message.hwnd);
                capture_page_drag = page && GetForegroundWindow() == root &&
                    (message.message == WM_LBUTTONDOWN || message.message == WM_LBUTTONDBLCLK);
                if (page && page_input_intent(message))
                    browser->human_input();
                if (browser->toolbar) handled = browser->toolbar->pre_translate(message);
                break;
              }
            }
            if (!handled) { TranslateMessage(&message); DispatchMessage(&message); }
            // With host input processing some runtimes lose the renderer's
            // capture outside the viewport. Retain capture on the native input
            // proxy so the original drag/release continues through WebView2.
            if (capture_page_drag && IsWindow(message.hwnd)) SetCapture(message.hwnd);
            if (message.message == WM_LBUTTONUP && GetCapture() == message.hwnd) ReleaseCapture();
        }
        stopping = true;
        while (WaitForSingleObject(reader.native_handle(), 50) == WAIT_TIMEOUT)
            CancelSynchronousIo(reader.native_handle());
        reader.join();
        MSG leftover{};
        while (PeekMessage(&leftover, host.dispatcher, input_message, input_message, PM_REMOVE))
            delete reinterpret_cast<std::string*>(leftover.lParam);
        CloseHandle(slots); KillTimer(host.dispatcher, 1);
        host.output.reset(); DestroyWindow(host.dispatcher);
        return 0;
    } catch (const std::exception& e) {
        DWORD written = 0; std::string message = std::string("distinct-browser: ") + e.what() + "\n";
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
        return 1;
    } catch (...) { return 1; }
}
