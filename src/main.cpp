#include <windows.h>
#include <roapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <winrt/base.h>
#include "json.h"
#include "control.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <algorithm>
#include <utility>
#include <cctype>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using namespace distinct;
namespace fs = std::filesystem;
constexpr UINT input_message = WM_APP + 1, disconnected_message = WM_APP + 2;
constexpr int takeover_button = 101, address_edit = 102, go_button = 103;
constexpr uint64_t operation_timeout = 20000, create_timeout = 45000;
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

// Pipe backpressure must never block the window's UI thread. A failed writer
// revokes the connection just like EOF on stdin. The bounded queue caps memory.
class Output {
    HANDLE pipe_;
    HWND target_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;
    size_t bytes_ = 0;
    bool stopped_ = false;
    std::thread thread_;
public:
    Output(HANDLE pipe, HWND target) : pipe_(pipe), target_(target), thread_([this] { run(); }) {}
    ~Output() {
        { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
        wake_.notify_one();
        while (WaitForSingleObject(thread_.native_handle(), 50) == WAIT_TIMEOUT)
            CancelSynchronousIo(thread_.native_handle());
        thread_.join();
    }
    void send(const Json& value) {
        auto line = winrt::to_string(value.Stringify()) + "\n";
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return;
        if (bytes_ + line.size() > 64 * 1024 * 1024) {
            stopped_ = true; PostMessage(target_, disconnected_message, 0, 0);
        } else { bytes_ += line.size(); queue_.push_back(std::move(line)); }
        wake_.notify_one();
    }
private:
    void run() {
        for (;;) {
            std::string line;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
                if (stopped_) return;
                line = std::move(queue_.front()); queue_.pop_front(); bytes_ -= line.size();
            }
            size_t offset = 0;
            while (offset < line.size()) {
                DWORD written = 0;
                if (!WriteFile(pipe_, line.data() + offset, static_cast<DWORD>(line.size() - offset), &written, nullptr) || !written) {
                    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
                    PostMessage(target_, disconnected_message, 0, 0); return;
                }
                offset += written;
            }
        }
    }
};
struct Host;
struct Operation {
    std::string request, token, kind;
    uint64_t started = now(), revision = 0, navigation_id = 0;
    size_t dispatched = 0, completed = 0;
    bool done = false;
    std::vector<std::pair<std::wstring, Json>> steps;
};
struct Browser : std::enable_shared_from_this<Browser> {
    Host* host = nullptr;
    std::string id, label, creation;
    uint64_t created = now(), revision = 0, navigation_id = 0;
    fs::path profile;
    HWND window = nullptr, name = nullptr, address = nullptr, status = nullptr, button = nullptr, go = nullptr;
    HFONT font = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    Control control;
    std::shared_ptr<Operation> active;
    bool ready = false, navigating = true, closing = false;
    double viewport_width = 0, viewport_height = 0;
    void layout();
    void refresh();
};
struct Host {
    HWND dispatcher = nullptr;
    std::unique_ptr<Output> output;
    std::map<std::string, std::shared_ptr<Browser>> browsers;
    std::set<std::string> pending;
    bool connected = true;
    fs::path data_root;

    void send(Json message) { if (connected) output->send(message); }
    void success(const std::string& id, Json result = Json()) {
        pending.erase(id);
        send(object({{L"id", js(id)}, {L"ok", jb(true)}, {L"result", result}}));
    }
    void failure(const std::string& id, const std::string& code, const std::string& message, Json detail = Json()) {
        pending.erase(id);
        send(object({{L"id", id.empty() ? Value::CreateNullValue() : js(id)}, {L"ok", jb(false)},
            {L"error", object({{L"code", js(code)}, {L"message", js(message)}, {L"detail", detail}})}}));
    }
    void event(const std::shared_ptr<Browser>& b, const std::string& type, Json detail = Json()) {
        send(object({{L"event", js(type)}, {L"browser_id", js(b->id)},
            {L"state", js(mode_name(b->control.mode))}, {L"detail", detail}}));
    }
    bool expire(const std::shared_ptr<Browser>& b) {
        if (!b->control.expire(now())) return false;
        b->refresh(); event(b, "control_changed", object({{L"reason", js("lease_expired")}})); return true;
    }
    void revoke(const std::shared_ptr<Browser>& b, Mode mode, const std::string& reason) {
        if (b->control.mode == Mode::closed || (b->control.mode == Mode::faulted && mode != Mode::closed)) return;
        b->control.revoke(mode); b->refresh();
        event(b, mode == Mode::paused ? "paused" : "control_changed", object({{L"reason", js(reason)}}));
    }
    void finish(const std::shared_ptr<Browser>& b, std::shared_ptr<Operation> op,
                const std::string& code = "", const std::string& message = "", Json result = Json()) {
        if (op->done) return;
        op->done = true;
        if (b->active == op) { b->active.reset(); b->control.end(); }
        if (code.empty()) success(op->request, result);
        else failure(op->request, code, message, object({{L"dispatched_steps", jn(static_cast<double>(op->dispatched))},
            {L"completed_steps", jn(static_cast<double>(op->completed))}, {L"result_unknown", jb(op->dispatched > 0)}}));
        b->refresh();
        if (b->control.mode == Mode::manual) event(b, "control_changed");
    }
    std::string gate(const std::shared_ptr<Browser>& b, const std::shared_ptr<Operation>& op) {
        if (op->done || b->closing) return "browser_closed";
        if (!connected) return "disconnected";
        if (!IsWindowVisible(b->window) || IsIconic(b->window)) {
            if (b->control.mode != Mode::paused) revoke(b, Mode::paused, "window_not_visible");
        }
        expire(b);
        return b->control.check(op->token, now());
    }
    void close(const std::shared_ptr<Browser>& b, const std::string& reason) {
        if (b->closing) return;
        b->closing = true; b->control.revoke(Mode::closed);
        if (!b->creation.empty()) { failure(b->creation, "creation_failed", reason); b->creation.clear(); }
        if (b->active) finish(b, b->active, "browser_closed", "Window closed; dispatched effects are not rolled back");
        if (b->controller) b->controller->Close();
        b->view.Reset(); b->controller.Reset(); b->environment.Reset();
        if (b->window) { DestroyWindow(b->window); b->window = nullptr; }
        event(b, "closed", object({{L"reason", js(reason)}}));
        browsers.erase(b->id);
        if (!connected && browsers.empty()) PostQuitMessage(0);
    }
    void fault(const std::shared_ptr<Browser>& b, const std::string& reason) {
        if (b->closing) return;
        revoke(b, Mode::faulted, reason);
        if (!b->creation.empty()) { close(b, reason); return; }
        if (b->active) finish(b, b->active, "browser_faulted", reason);
        event(b, "faulted", object({{L"reason", js(reason)}}));
    }
    void disconnect() {
        if (!connected) return;
        connected = false;
        auto copy = browsers;
        for (auto& [id, b] : copy) {
            revoke(b, Mode::manual, "disconnected");
            if (!b->ready) close(b, "disconnected_during_creation");
        }
        if (browsers.empty()) PostQuitMessage(0);
    }
    void tick() {
        auto copy = browsers;
        for (auto& [id, b] : copy) {
            expire(b);
            if (!b->ready && now() - b->created > create_timeout) close(b, "initialization_timeout");
            else if (b->active && now() - b->active->started > operation_timeout)
                fault(b, "Operation timed out; dispatched effects may still complete. Close this browser to recover.");
        }
    }
    void create(const std::string& request, const std::string& label);
    void initialize(const std::shared_ptr<Browser>& b);
    void handle(const std::string& line);
    void observe(const std::shared_ptr<Browser>& b, const std::shared_ptr<Operation>& op);
    void dispatch_step(const std::shared_ptr<Browser>& b, const std::shared_ptr<Operation>& op);
};

void Browser::layout() {
    if (!window) return;
    RECT bounds{}; GetClientRect(window, &bounds);
    // All control dimensions scale from DIPs, including fractional DPI values.
    const auto px = [this](int dip) { return MulDiv(dip, static_cast<int>(GetDpiForWindow(window)), 96); };
    int width = bounds.right;
    MoveWindow(name, px(12), px(8), std::max(0, width - px(24)), px(22), TRUE);
    MoveWindow(address, px(12), px(36), std::max(0, width - px(90)), px(28), TRUE);
    MoveWindow(go, std::max(px(12), width - px(66)), px(36), px(54), px(28), TRUE);
    MoveWindow(status, px(12), px(76), std::max(0, width - px(126)), px(25), TRUE);
    MoveWindow(button, std::max(px(12), width - px(110)), px(72), px(98), px(30), TRUE);
    if (controller) {
        bounds.top = std::min(bounds.bottom, static_cast<LONG>(px(112)));
        controller->put_Bounds(bounds);
        controller->put_IsVisible(IsWindowVisible(window) && !IsIconic(window));
    }
}
void Browser::refresh() {
    if (!window || closing) return;
    std::wstring text;
    switch (control.mode) {
    case Mode::available: text = control.busy ? L"等待已发操作收尾" : L"可申请自动控制"; break;
    case Mode::automatic: text = L"自动控制 · "; text += winrt::to_hstring(control.owner).c_str(); break;
    case Mode::taking_over: text = L"接管中 · 等待已发操作收尾"; break;
    case Mode::manual: text = L"人工控制"; break;
    case Mode::paused: text = L"已暂停 · 展示后点击交还"; break;
    case Mode::faulted: text = L"故障 · 请关闭此浏览器"; break;
    case Mode::closed: text = L"已关闭"; break;
    }
    if (!host->connected) text = L"连接已断开 · 人工控制";
    SetWindowText(status, text.c_str());
    bool handback = control.mode == Mode::manual || control.mode == Mode::paused;
    SetWindowText(button, handback ? L"交还" : L"接管");
    EnableWindow(button, host->connected && control.mode != Mode::faulted && control.mode != Mode::taking_over);
}

static LRESULT CALLBACK BrowserProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto b = reinterpret_cast<Browser*>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        b = static_cast<Browser*>(reinterpret_cast<CREATESTRUCT*>(lparam)->lpCreateParams);
        b->window = window;
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(b));
    }
    if (!b) return DefWindowProc(window, message, wparam, lparam);
    try {
        switch (message) {
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED && !b->closing)
                b->host->revoke(b->shared_from_this(), Mode::paused, "minimized");
            b->control.observed = false; ++b->revision;
            b->layout(); return 0;
        case WM_SHOWWINDOW:
            if (!wparam && !b->closing) b->host->revoke(b->shared_from_this(), Mode::paused, "hidden");
            b->layout(); return 0;
        case WM_DPICHANGED: {
            auto rect = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            b->layout(); return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == takeover_button && HIWORD(wparam) == BN_CLICKED) {
                auto shared = b->shared_from_this();
                if (b->control.mode == Mode::manual || b->control.mode == Mode::paused) {
                    if (b->host->connected && IsWindowVisible(window) && !IsIconic(window) && b->control.hand_back().empty()) {
                        b->refresh(); b->host->event(shared, "control_changed", object({{L"reason", js("hand_back")}}));
                    }
                } else if (b->control.mode == Mode::automatic || b->control.mode == Mode::available)
                    b->host->revoke(shared, Mode::manual, "takeover");
                return 0;
            }
            if (LOWORD(wparam) == go_button && b->view && !b->control.busy && b->control.mode != Mode::faulted) {
                b->host->revoke(b->shared_from_this(), Mode::manual, "manual_navigation");
                int length = GetWindowTextLength(b->address);
                std::wstring url(static_cast<size_t>(length) + 1, L'\0');
                GetWindowText(b->address, url.data(), length + 1); url.resize(length);
                if (!url.empty()) b->view->Navigate(url.c_str());
                return 0;
            }
            break;
        case WM_CLOSE: {
            auto keep_alive = b->shared_from_this();
            b->host->close(keep_alive, "user_closed"); return 0;
        }
        case WM_NCDESTROY:
            if (b->font) { DeleteObject(b->font); b->font = nullptr; }
            SetWindowLongPtr(window, GWLP_USERDATA, 0); break;
        }
    } catch (...) { OutputDebugString(L"distinct-browser: window callback failed\n"); }
    return DefWindowProc(window, message, wparam, lparam);
}

void Host::create(const std::string& request, const std::string& label) {
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    checked(options->put_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS_STABLE), "ReleaseChannels");
    LPWSTR version = nullptr;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionStringWithOptions(nullptr, options.Get(), &version);
    CoTaskMemFree(version);
    if (FAILED(hr)) throw Error("runtime_missing", "Install Microsoft Edge WebView2 Runtime (Evergreen x64)");
    auto b = std::make_shared<Browser>(); b->host = this; b->id = guid(); b->label = label; b->creation = request;
    browsers.emplace(b->id, b);
    auto title = winrt::to_hstring(label + " | Distinct Browser");
    b->window = CreateWindowEx(0, L"DistinctBrowserWindow", title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 760, nullptr, nullptr, GetModuleHandle(nullptr), b.get());
    if (!b->window) { close(b, "CreateWindowEx failed"); return; }
    auto control = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id) {
        return CreateWindowEx(type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, type, text,
            WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, b->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
    };
    b->name = control(L"STATIC", winrt::to_hstring(label).c_str(), SS_LEFT, 0);
    b->address = control(L"EDIT", L"about:blank", ES_AUTOHSCROLL | WS_TABSTOP, address_edit);
    b->go = control(L"BUTTON", L"打开", BS_PUSHBUTTON | WS_TABSTOP, go_button);
    b->status = control(L"STATIC", L"正在初始化网页…", SS_LEFT, 0);
    b->button = control(L"BUTTON", L"接管", BS_PUSHBUTTON | WS_TABSTOP, takeover_button);
    b->font = CreateFont(-MulDiv(14, static_cast<int>(GetDpiForWindow(b->window)), 96), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    for (HWND item : {b->name, b->address, b->go, b->status, b->button}) SendMessage(item, WM_SETFONT, reinterpret_cast<WPARAM>(b->font), TRUE);
    SendMessage(b->address, EM_SETLIMITTEXT, 8192, 0);
    ShowWindow(b->window, SW_SHOWNOACTIVATE); b->layout();
    b->profile = data_root / winrt::to_hstring(b->id).c_str();
    hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, b->profile.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this, b](HRESULT error, ICoreWebView2Environment* env) -> HRESULT {
            if (b->closing) return S_OK;
            if (FAILED(error) || !env) { close(b, "WebView2 environment initialization failed"); return S_OK; }
            // Machine policy/environment overrides can replace userDataFolder.
            // Fail creation rather than silently sharing a profile.
            ComPtr<ICoreWebView2Environment7> environment7;
            LPWSTR actual = nullptr;
            if (FAILED(env->QueryInterface(IID_PPV_ARGS(&environment7))) || FAILED(environment7->get_UserDataFolder(&actual))) {
                close(b, "Cannot verify independent user data directory"); return S_OK;
            }
            fs::path actual_path(actual); CoTaskMemFree(actual);
            std::error_code actual_error, expected_error;
            auto actual_normal = fs::weakly_canonical(actual_path, actual_error);
            auto expected_normal = fs::weakly_canonical(b->profile, expected_error);
            if (actual_error || expected_error || CompareStringOrdinal(actual_normal.c_str(), -1, expected_normal.c_str(), -1, TRUE) != CSTR_EQUAL) {
                close(b, "User data directory was overridden; independent browser isolation is unavailable"); return S_OK;
            }
            b->environment = env;
            HRESULT result = env->CreateCoreWebView2Controller(b->window,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([this, b](HRESULT error, ICoreWebView2Controller* controller) -> HRESULT {
                    if (b->closing) { if (controller) controller->Close(); return S_OK; }
                    if (FAILED(error) || !controller) { close(b, "WebView2 controller initialization failed"); return S_OK; }
                    try {
                        b->controller = controller;
                        checked(controller->get_CoreWebView2(&b->view), "get_CoreWebView2");
                        initialize(b); b->layout();
                        checked(b->view->Navigate(L"about:blank"), "initial navigation");
                    } catch (...) { close(b, "WebView2 setup failed"); }
                    return S_OK;
                }).Get());
            if (FAILED(result)) close(b, "WebView2 controller creation failed");
            return S_OK;
        }).Get());
    if (FAILED(hr)) close(b, "WebView2 environment creation failed");
}

void Host::initialize(const std::shared_ptr<Browser>& b) {
    std::weak_ptr<Browser> weak = b;
    EventRegistrationToken registration{};
    checked(b->view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            if (auto b = weak.lock(); b && !b->closing) {
                b->navigating = true; ++b->revision; b->control.observed = false;
                args->get_NavigationId(&b->navigation_id);
                if (b->active && b->active->kind == "navigate" && b->active->navigation_id == 0)
                    args->get_NavigationId(&b->active->navigation_id);
            }
            return S_OK;
        }).Get(), &registration), "NavigationStarting");
    checked(b->view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [this, weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            auto b = weak.lock(); if (!b || b->closing) return S_OK;
            uint64_t navigation_id = 0; args->get_NavigationId(&navigation_id);
            if (navigation_id != b->navigation_id) return S_OK;
            b->navigating = false;
            BOOL ok = FALSE; args->get_IsSuccess(&ok);
            if (!b->ready) {
                if (!ok) { close(b, "Initial navigation failed"); return S_OK; }
                b->ready = true;
                auto id = std::exchange(b->creation, "");
                UINT32 process_id = 0; b->view->get_BrowserProcessId(&process_id);
                success(id, object({{L"browser_id", js(b->id)}, {L"hwnd", js(std::to_string(reinterpret_cast<uintptr_t>(b->window)))},
                    {L"browser_process_id", jn(process_id)},
                    {L"state", js(mode_name(b->control.mode))}}));
                b->refresh(); event(b, "created");
            }
            auto op = b->active;
            if (op && op->kind == "navigate" && op->navigation_id == navigation_id) {
                if (ok) { op->completed = 1; finish(b, op, "", "", object({{L"navigation_completed", jb(true)}})); }
                else finish(b, op, "navigation_failed", "Navigation did not complete successfully; inspect the current page");
            }
            event(b, "navigation_completed", object({{L"success", jb(ok != FALSE)}}));
            return S_OK;
        }).Get(), &registration), "NavigationCompleted");
    checked(b->view->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
        [weak](ICoreWebView2* view, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
            if (auto b = weak.lock(); b && !b->closing) {
                b->control.observed = false; ++b->revision;
                LPWSTR source = nullptr;
                if (SUCCEEDED(view->get_Source(&source))) { SetWindowText(b->address, source); CoTaskMemFree(source); }
            }
            return S_OK;
        }).Get(), &registration), "SourceChanged");
    checked(b->view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [this, weak](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            args->put_Handled(TRUE);
            if (auto b = weak.lock(); b && !b->closing) {
                LPWSTR uri = nullptr; args->get_Uri(&uri);
                event(b, "popup_blocked", object({{L"url", js(owned_text(uri))}}));
            }
            return S_OK;
        }).Get(), &registration), "NewWindowRequested");
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
void Host::observe(const std::shared_ptr<Browser>& b, const std::shared_ptr<Operation>& op) {
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
                            if (op->revision != b->revision || b->navigating) {
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
                            b->control.observed = true;
                            finish(b, op, "", "", object({{L"url", js(source)}, {L"title", js(caption)},
                                {L"viewport", object({{L"width", jn(width)}, {L"height", jn(height)}, {L"coordinate_space", js("css_viewport")}})},
                                {L"image", image}, {L"state", js(mode_name(b->control.mode))}}));
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

void Host::dispatch_step(const std::shared_ptr<Browser>& b, const std::shared_ptr<Operation>& op) {
    if (op->done) return;
    // The callback of the last dispatched step is always recorded, including
    // after takeover. Only a subsequent dispatch requires a still-valid lease.
    if (op->completed == op->steps.size()) {
        expire(b);
        finish(b, op, "", "", object({{L"command_processed", jb(true)}, {L"business_success_verified", jb(false)},
            {L"control_still_valid", jb(b->control.check(op->token, now()).empty())}})); return;
    }
    auto error = gate(b, op);
    if (!error.empty()) { finish(b, op, error, "Control revoked; remaining steps were not dispatched"); return; }
    if (op->revision != b->revision || b->navigating) {
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
            && method != "renew" && method != "release" && method != "observe" && method != "act")
            throw Error("unknown_method", "Unknown method");
        Json parameters = request.HasKey(L"params") ? request.GetNamedObject(L"params") : Json();
        if (method == "create") { create(id, string(parameters, L"label", 256)); return; }
        auto browser_id = string(parameters, L"browser_id", 128);
        auto found = browsers.find(browser_id);
        if (found == browsers.end()) throw Error("browser_not_found", "Browser does not exist or has been closed");
        auto b = found->second;
        if (method == "close") { close(b, "caller_closed"); success(id); return; }
        if (!b->ready) throw Error("browser_not_ready", "Browser is still initializing");
        expire(b);
        if (method == "present") {
            ShowWindow(b->window, IsIconic(b->window) ? SW_RESTORE : SW_SHOW);
            b->layout();
            bool foreground = SetForegroundWindow(b->window) != FALSE;
            success(id, object({{L"visible", jb(IsWindowVisible(b->window) != FALSE)},
                {L"foreground", jb(foreground)}, {L"state", js(mode_name(b->control.mode))}})); return;
        }
        if (method == "acquire") {
            if (!IsWindowVisible(b->window) || IsIconic(b->window)) throw Error("window_paused", "Present the browser and hand back control first");
            auto client = string(parameters, L"client_id", 128);
            double ttl = parameters.HasKey(L"ttl_ms") ? number(parameters, L"ttl_ms", 1000, 300000) : 30000;
            auto token = guid();
            auto error = b->control.acquire(client, token, now(), static_cast<uint64_t>(ttl));
            if (!error.empty()) throw Error(error, "Browser is occupied or does not allow automatic control");
            b->refresh(); event(b, "control_changed");
            success(id, object({{L"token", js(token)}, {L"ttl_ms", jn(ttl)}})); return;
        }
        if (method != "renew" && method != "release" && method != "observe" && method != "act")
            throw Error("unknown_method", "Unknown method");
        auto token = string(parameters, L"token", 128);
        auto error = b->control.check(token, now());
        if (!error.empty()) throw Error(error, "Lease is invalid, expired or revoked");
        if (method == "renew") {
            double ttl = parameters.HasKey(L"ttl_ms") ? number(parameters, L"ttl_ms", 1000, 300000) : 30000;
            b->control.expires = now() + static_cast<uint64_t>(ttl);
            success(id, object({{L"ttl_ms", jn(ttl)}})); return;
        }
        if (method == "release") { revoke(b, Mode::available, "released"); success(id); return; }
        auto op = std::make_shared<Operation>();
        op->request = id; op->token = token; op->kind = method; op->revision = b->revision;
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
        error = b->control.begin(token, now(), method == "act");
        if (!error.empty()) throw Error(error, "Observe first, or wait for the current operation to finish");
        b->active = op;
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
        WNDCLASS window_class{}; window_class.lpfnWndProc = BrowserProc; window_class.hInstance = instance;
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW); window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = L"DistinctBrowserWindow";
        if (!RegisterClass(&window_class)) throw Error("startup_failed", "Cannot register browser class");
        WNDCLASS dispatch_class{}; dispatch_class.lpfnWndProc = DispatcherProc; dispatch_class.hInstance = instance;
        dispatch_class.lpszClassName = L"DistinctBrowserDispatcher";
        if (!RegisterClass(&dispatch_class)) throw Error("startup_failed", "Cannot register dispatcher class");
        host.dispatcher = CreateWindowEx(0, dispatch_class.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, &host);
        if (!host.dispatcher) throw Error("startup_failed", "Cannot create dispatcher");
        host.output = std::make_unique<Output>(output, host.dispatcher);
        SetTimer(host.dispatcher, 1, 200, nullptr);
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
        while (GetMessage(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessage(&message); }
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
