#include "../host/host.h"
#include <WebView2EnvironmentOptions.h>
#include <algorithm>
#include <utility>

using namespace distinct;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
std::string identifier() {
    GUID id{}; checked(CoCreateGuid(&id), "CoCreateGuid");
    wchar_t value[40]{}; StringFromGUID2(id, value, 40);
    return winrt::to_string(std::wstring(value + 1, 36));
}
}

std::shared_ptr<Page> Host::make_page(const std::shared_ptr<Browser>& browser, const std::string& opener) {
    auto page = std::make_shared<Page>();
    page->host = this; page->browser = browser; page->id = identifier(); page->opener_id = opener;
    browser->pages.emplace(page->id, page);
    if (!browser->selected) browser->selected = page;
    auto title = winrt::to_hstring(browser->label + " | Distinct Browser");
    page->window = CreateWindowEx(0, L"DistinctBrowserWindow", title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 760, nullptr, nullptr, GetModuleHandle(nullptr), page.get());
    if (!page->window) { close(page, "CreateWindowEx failed"); return page; }
    page->toolbar = std::make_unique<NativeToolbar>();
    std::weak_ptr<Page> weak = page;
    HRESULT hr = page->toolbar->create(page->window, [weak](ShellCommand command) {
        if (auto current = weak.lock()) current->user_command(command);
    });
    if (FAILED(hr)) { close(page, "Native toolbar initialization failed"); return page; }
    page->page_window = CreateWindowEx(0, L"DistinctBrowserPage", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, page->window, reinterpret_cast<HMENU>(108), GetModuleHandle(nullptr), nullptr);
    if (!page->page_window) { close(page, "Page input window creation failed"); return page; }
    page->keyboard = keyboard_guard.add(page->window, page->page_window);
    ShowWindow(page->window, SW_SHOWNOACTIVATE);
    return page;
}

void Host::create(const std::string& request, const std::string& label) {
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    checked(options->put_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS_STABLE), "ReleaseChannels");
    LPWSTR version = nullptr;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionStringWithOptions(nullptr, options.Get(), &version);
    CoTaskMemFree(version);
    if (FAILED(hr)) throw Error("runtime_missing", "Install Microsoft Edge WebView2 Runtime (Evergreen x64)");
    auto browser = std::make_shared<Browser>();
    browser->id = identifier(); browser->label = label; browser->creation = request;
    browser->profile = data_root / winrt::to_hstring(browser->id).c_str();
    browsers.emplace(browser->id, browser);
    auto page = make_page(browser);
    if (page->closing) return;
    hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, browser->profile.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this, browser, page](HRESULT error, ICoreWebView2Environment* env) -> HRESULT {
            if (page->closing || browser->closing) return S_OK;
            if (FAILED(error) || !env) { close(page, "WebView2 environment initialization failed"); return S_OK; }
            ComPtr<ICoreWebView2Environment7> environment7;
            LPWSTR actual = nullptr;
            if (FAILED(env->QueryInterface(IID_PPV_ARGS(&environment7))) || FAILED(environment7->get_UserDataFolder(&actual))) {
                close(page, "Cannot verify independent user data directory"); return S_OK;
            }
            fs::path actual_path(actual); CoTaskMemFree(actual);
            std::error_code actual_error, expected_error;
            auto actual_normal = fs::weakly_canonical(actual_path, actual_error);
            auto expected_normal = fs::weakly_canonical(browser->profile, expected_error);
            if (actual_error || expected_error || CompareStringOrdinal(actual_normal.c_str(), -1, expected_normal.c_str(), -1, TRUE) != CSTR_EQUAL) {
                close(page, "User data directory was overridden; independent browser isolation is unavailable"); return S_OK;
            }
            browser->environment = env;
            create_controller(page);
            return S_OK;
        }).Get());
    if (FAILED(hr)) close(page, "WebView2 environment creation failed");
}

void Host::create_controller(const std::shared_ptr<Page>& page) {
    auto browser = page->owner();
    ComPtr<ICoreWebView2Environment10> environment10;
    ComPtr<ICoreWebView2ControllerOptions> options;
    ComPtr<ICoreWebView2ControllerOptions4> input_options;
    if (!browser || FAILED(browser->environment.As(&environment10)) ||
        FAILED(environment10->CreateCoreWebView2ControllerOptions(&options)) ||
        FAILED(options.As(&input_options)) || FAILED(input_options->put_AllowHostInputProcessing(TRUE))) {
        close(page, "WebView2 Runtime lacks host input processing; update Evergreen Runtime (138 or later)"); return;
    }
    // Every page uses the same Environment and default Profile. No independent
    // environment or Navigate call is used to emulate a window.open request.
    HRESULT result = environment10->CreateCoreWebView2ControllerWithOptions(page->page_window, options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([this, browser, page](HRESULT error, ICoreWebView2Controller* controller) -> HRESULT {
            if (page->closing || browser->closing) { if (controller) controller->Close(); return S_OK; }
            if (FAILED(error) || !controller) { close(page, "WebView2 controller initialization failed"); return S_OK; }
            try {
                page->controller = controller;
                checked(controller->get_CoreWebView2(&page->view), "get_CoreWebView2");
                initialize(page); page->layout();
                if (auto pending = page->pending_window) {
                    auto opener = browser->pages.find(page->opener_id);
                    if (opener == browser->pages.end() || opener->second->closing) {
                        close(page, "opener_closed_during_creation"); return S_OK;
                    }
                    page->navigating = false;
                    checked(pending->args->put_NewWindow(page->view.Get()), "put_NewWindow");
                    // No navigation is needed for a blank popup; it is already
                    // usable after association. Actual target navigation may
                    // still be loading and will invalidate observation normally.
                    pending->complete();
                    page->pending_window.reset();
                    if (!page->closing && !browser->closing) ready_page(page);
                } else checked(page->view->Navigate(L"about:blank"), "initial navigation");
            } catch (...) { close(page, "WebView2 page setup failed"); }
            return S_OK;
        }).Get());
    if (FAILED(result)) close(page, "WebView2 controller creation failed");
}

void Host::ready_page(const std::shared_ptr<Page>& page) {
    if (page->ready || page->closing) return;
    auto browser = page->owner();
    if (!browser || browser->closing) return;
    page->ready = true;
    page->selected_at = ++browser->selection_clock;
    if (!browser->ready) {
        browser->ready = true;
        auto request = std::exchange(browser->creation, "");
        UINT32 process_id = 0; page->view->get_BrowserProcessId(&process_id);
        success(request, object({{L"browser_id", js(browser->id)}, {L"page_id", js(page->id)},
            {L"hwnd", js(std::to_string(reinterpret_cast<uintptr_t>(page->window)))}, {L"browser_process_id", jn(process_id)},
            {L"state", js(mode_name(browser->control.mode))}}));
        event(browser, "created");
    }
    event(page, "page_created", object({{L"opener_page_id", js(page->opener_id)}}));
    select(page, "page_created");
    browser->refresh();
    // Present without stealing foreground from another app/browser session.
    auto opener = browser->pages.find(page->opener_id);
    if (opener != browser->pages.end() && GetForegroundWindow() == opener->second->window) SetForegroundWindow(page->window);
}

void Host::new_window(const std::shared_ptr<Page>& opener, ICoreWebView2NewWindowRequestedEventArgs* args) {
    checked(args->put_Handled(TRUE), "NewWindowRequested.Handled");
    auto browser = opener->owner();
    if (!browser) return;
    if (browser->closing || !connected || browser->control.mode == Mode::faulted || browser->pages.size() >= 16) {
        event(browser, "page_creation_failed", object({{L"opener_page_id", js(opener->id)}, {L"reason", js("session_unavailable_or_page_limit")}})); return;
    }
    auto request = std::make_shared<PendingWindow>(); request->args = args;
    checked(args->GetDeferral(&request->deferral), "NewWindowRequested.GetDeferral");
    auto page = make_page(browser, opener->id);
    if (page->closing) {
        event(page, "page_creation_failed", object({{L"opener_page_id", js(opener->id)}, {L"reason", js("native_page_creation_failed")}}));
        request->complete(); return;
    }
    page->pending_window = request;
    create_controller(page);
}

void Host::close(const std::shared_ptr<Page>& page, const std::string& reason) {
    if (page->closing) return;
    auto browser = page->owner();
    if (!browser) return;
    page->closing = true;
    if (browser->pages.size() == 1) browser->control.revoke(Mode::closed);
    if (auto request = std::exchange(page->pending_window, {})) {
        event(page, "page_creation_failed", object({{L"opener_page_id", js(page->opener_id)}, {L"reason", js(reason)}}));
        request->complete();
    }
    auto children = browser->pages;
    for (auto& [id, child] : children)
        if (!child->ready && child->opener_id == page->id) close(child, "opener_closed_during_creation");
    if (auto operation = page->operation())
        finish(page, operation, browser->closing || browser->pages.size() == 1 ? "browser_closed" : "page_closed",
            "Page closed; dispatched effects are not rolled back");
    keyboard_guard.remove(page->keyboard);
    if (page->controller) page->controller->Close();
    page->view.Reset(); page->controller.Reset();
    if (page->window) { DestroyWindow(page->window); page->window = nullptr; }
    browser->pages.erase(page->id);
    event(page, "page_closed", object({{L"reason", js(reason)}}));
    if (browser->selected == page) {
        browser->selected.reset(); browser->control.observed = false; ++browser->selection_revision;
        std::shared_ptr<Page> next;
        for (auto& [id, other] : browser->pages)
            if (other->ready && !other->closing && (!next || other->selected_at > next->selected_at)) next = other;
        if (next && !browser->closing) select(next, "selected_page_closed");
    }
    if (browser->pages.empty() && !browser->closing) close_browser(browser, reason);
}

void Host::close_browser(const std::shared_ptr<Browser>& browser, const std::string& reason) {
    if (browser->closing) return;
    browser->closing = true; browser->control.revoke(Mode::closed);
    if (!browser->creation.empty()) failure(std::exchange(browser->creation, ""), "creation_failed", reason);
    if (browser->active) finish(browser->active->page, browser->active, "browser_closed", "Browser closed; effects are not rolled back");
    auto pages = browser->pages;
    for (auto& [id, page] : pages) close(page, reason);
    browser->selected.reset(); browser->environment.Reset();
    event(browser, "closed", object({{L"reason", js(reason)}}));
    browsers.erase(browser->id);
    if (!connected && browsers.empty()) PostQuitMessage(0);
}
