#pragma once
#include <windows.h>
#include <roapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "../json.h"
#include "../control.h"
#include "../window/native_toolbar.h"
#include "../window/keyboard_guard.h"

struct Host;
struct Page;
struct SiteInteractions;

// A request retains its original page. Selection changes never retarget CDP.
struct Operation {
    std::string request, token, kind;
    std::shared_ptr<Page> page;
    std::wstring navigation_target;
    uint64_t started = GetTickCount64(), revision = 0, navigation_id = 0, selection_revision = 0;
    size_t dispatched = 0, completed = 0;
    bool done = false;
    std::vector<std::pair<std::wstring, distinct::Json>> steps;
};

struct Browser : std::enable_shared_from_this<Browser> {
    std::string id, label, creation;
    std::filesystem::path profile;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    std::map<std::string, std::shared_ptr<Page>> pages;
    std::shared_ptr<Page> selected;
    distinct::Control control;
    std::shared_ptr<Operation> active;
    std::shared_ptr<SiteInteractions> sites;
    distinct::Json interrupted_operation;
    uint64_t selection_revision = 0, selection_clock = 0;
    bool ready = false, closing = false;
    void refresh();
    void sync_input();
    bool visible() const;
};

// The deferral is completed exactly once on success, failure or cancellation.
struct PendingWindow {
    Microsoft::WRL::ComPtr<ICoreWebView2NewWindowRequestedEventArgs> args;
    Microsoft::WRL::ComPtr<ICoreWebView2Deferral> deferral;
    void complete() {
        auto pending = std::move(deferral);
        args.Reset();
        if (pending) pending->Complete();
    }
    ~PendingWindow() { complete(); }
};

struct Page : std::enable_shared_from_this<Page> {
    Host* host = nullptr;
    std::weak_ptr<Browser> browser;
    std::string id, opener_id;
    uint64_t created = GetTickCount64(), revision = 0, navigation_id = 0, selected_at = 0;
    HWND window = nullptr, page_window = nullptr;
    std::unique_ptr<distinct::NativeToolbar> toolbar;
    std::shared_ptr<distinct::KeyboardTarget> keyboard;
    std::shared_ptr<PendingWindow> pending_window;
    std::optional<distinct::ShellCommand> pending_manual;
    std::wstring pending_address, notice;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> view;
    bool ready = false, navigating = true, closing = false;
    double viewport_width = 0, viewport_height = 0;
    std::shared_ptr<Browser> owner() const { return browser.lock(); }
    std::shared_ptr<Operation> operation() const {
        auto session = owner();
        return session && session->active && session->active->page.get() == this ? session->active : nullptr;
    }
    void invalidate() {
        ++revision;
        if (auto session = owner(); session && session->selected.get() == this) session->control.observed = false;
    }
    void layout();
    void refresh();
    void user_command(distinct::ShellCommand command);
    void human_input();
    void sync_input();
    void drain_manual();
};
