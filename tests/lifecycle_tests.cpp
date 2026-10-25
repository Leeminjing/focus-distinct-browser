#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/host/host.h"
#include "../src/browser/site_interactions.h"
#include <cassert>
#include <iostream>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using namespace distinct;

struct DeferralProbe : RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2Deferral> {
    int completions = 0;
    HRESULT STDMETHODCALLTYPE Complete() override { ++completions; return S_OK; }
};
struct PermissionProbe : RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2PermissionRequestedEventArgs3> {
    ComPtr<DeferralProbe> probe = Microsoft::WRL::Make<DeferralProbe>();
    COREWEBVIEW2_PERMISSION_STATE state = COREWEBVIEW2_PERMISSION_STATE_DEFAULT;
    COREWEBVIEW2_PERMISSION_KIND kind = COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION;
    BOOL saves = TRUE, handled = FALSE;
    HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR* output) override {
        *output = static_cast<LPWSTR>(CoTaskMemAlloc(128)); wcscpy_s(*output, 64, L"https://example.com/"); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_PermissionKind(COREWEBVIEW2_PERMISSION_KIND* output) override { *output = kind; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsUserInitiated(BOOL* output) override { *output = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_State(COREWEBVIEW2_PERMISSION_STATE* output) override { *output = state; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_State(COREWEBVIEW2_PERMISSION_STATE output) override { state = output; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDeferral(ICoreWebView2Deferral** output) override { return probe.CopyTo(output); }
    HRESULT STDMETHODCALLTYPE get_Handled(BOOL* output) override { *output = handled; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_Handled(BOOL output) override { handled = output; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_SavesInProfile(BOOL* output) override { *output = saves; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_SavesInProfile(BOOL output) override { saves = output; return S_OK; }
};
struct Setup {
    Host host;
    std::shared_ptr<Browser> browser = std::make_shared<Browser>();
    std::shared_ptr<Page> root = std::make_shared<Page>();
    std::shared_ptr<Page> child = std::make_shared<Page>();
    ComPtr<DeferralProbe> probe = Microsoft::WRL::Make<DeferralProbe>();
    Setup() {
        host.connected = false; // exercise lifecycle without an output pipe
        browser->id = "browser"; browser->ready = true;
        host.browsers.emplace(browser->id, browser);
        root->id = "root"; root->browser = browser; root->host = &host; root->ready = true;
        child->id = "child"; child->opener_id = root->id; child->browser = browser; child->host = &host;
        child->pending_window = std::make_shared<PendingWindow>();
        child->pending_window->deferral = probe;
        browser->pages.emplace(root->id, root); browser->pages.emplace(child->id, child);
        browser->selected = root;
        assert(browser->control.acquire("caller", "lease", GetTickCount64(), 300000).empty());
        browser->control.observed = true;
    }
};
struct InteractionSetup : Setup {
    HANDLE sink = INVALID_HANDLE_VALUE;
    InteractionSetup() {
        sink = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        assert(sink != INVALID_HANDLE_VALUE);
        host.output = std::make_unique<PipeOutput>(sink, [] {}); host.connected = true;
        child->ready = true;
        browser->sites = std::make_shared<SiteInteractions>(&host, browser);
    }
    ~InteractionSetup() {
        host.close_browser(browser, "cleanup");
        host.output.reset(); CloseHandle(sink);
    }
};

int main() {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    {
        auto probe = Microsoft::WRL::Make<DeferralProbe>();
        {
            PendingWindow request; request.deferral = probe;
            request.complete(); request.complete();
        }
        assert(probe->completions == 1);
    }
    {
        Setup s;
        s.child->created = GetTickCount64() - 45001;
        s.host.tick();
        assert(s.probe->completions == 1 && s.child->closing);
        assert(s.browser->pages.size() == 1 && s.browser->selected == s.root);
        assert(s.browser->control.check("lease", GetTickCount64(), true).empty());
        s.host.close(s.child, "late_failure");
        assert(s.probe->completions == 1);
        s.host.close_browser(s.browser, "cleanup");
    }
    {
        Setup s;
        s.host.close(s.root, "opener_closed");
        assert(s.child->closing && s.root->closing && s.browser->closing);
        assert(s.probe->completions == 1 && s.host.browsers.empty());
    }
    {
        Setup s;
        auto operation = std::make_shared<Operation>();
        operation->request = "inflight"; operation->page = s.root; operation->dispatched = 1;
        s.browser->active = operation; s.browser->control.busy = true;
        s.host.close_browser(s.browser, "caller_closed");
        assert(operation->done && !s.browser->active && !s.browser->control.busy);
        assert(s.probe->completions == 1 && s.browser->pages.empty());
        s.host.finish(s.root, operation); // a late COM result cannot settle twice
        assert(s.browser->control.mode == Mode::closed && s.probe->completions == 1);
    }
    {
        Setup s;
        s.child->ready = true;
        s.host.select(s.child, "test");
        assert(!s.browser->control.observed && s.browser->selection_revision == 1);
        s.browser->control.observed = true;
        s.host.select(s.child, "same_page");
        assert(s.browser->control.observed && s.browser->selection_revision == 1);
        s.host.close(s.root, "background_closed");
        assert(s.browser->selected == s.child && s.browser->control.observed);
        s.host.close_browser(s.browser, "cleanup");
    }
    {
        Setup s;
        s.child->ready = true;
        s.host.fault(s.child, "renderer_failure");
        assert(s.browser->control.mode == Mode::faulted && !s.browser->control.observed);
        assert(s.browser->control.token.empty());
        s.host.close_browser(s.browser, "cleanup");
    }
    std::cout << "Page lifecycle tests passed: deferral once, timeout, opener close, session close, late completion, selection\n";
    {
        InteractionSetup s;
        auto args = Microsoft::WRL::Make<PermissionProbe>();
        auto operation = std::make_shared<Operation>(); operation->request = "issued"; operation->page = s.root;
        s.browser->active = operation; s.browser->control.busy = true;
        s.browser->sites->permission_requested(s.root, args.Get());
        assert(!args->saves && args->handled && !args->probe->completions && s.browser->sites->pending());
        assert(s.browser->control.mode == Mode::taking_over && s.browser->control.token.empty());
        s.host.finish(s.root, operation);
        s.root->user_command(ShellCommand::takeover);
        assert(s.browser->control.mode == Mode::manual && s.browser->sites->pending());
        auto id = s.browser->sites->permissions.back()->id;
        s.browser->sites->resolve_permission(id, true, "test_native_decision");
        s.browser->sites->resolve_permission(id, false, "late_duplicate");
        assert(args->state == COREWEBVIEW2_PERMISSION_STATE_ALLOW && args->probe->completions == 1);
        assert(!s.browser->sites->pending() && s.browser->control.mode == Mode::manual);
        s.root->user_command(ShellCommand::takeover); // no HWND, so visibility cannot pass; use the state contract below
        assert(s.browser->control.hand_back().empty());
        assert(s.browser->control.acquire("next", "new", GetTickCount64(), 30000).empty());
        assert(s.browser->control.check("new", GetTickCount64(), true) == "observation_required");
    }
    {
        InteractionSetup s;
        auto args = Microsoft::WRL::Make<PermissionProbe>();
        s.browser->sites->permission_requested(s.child, args.Get());
        s.browser->sites->permissions.back()->created = GetTickCount64() - 120001;
        s.host.tick();
        assert(args->state == COREWEBVIEW2_PERMISSION_STATE_DENY && args->probe->completions == 1);
        assert(s.browser->sites->permissions.back()->reason == "timeout");
        auto navigation = Microsoft::WRL::Make<PermissionProbe>();
        s.browser->sites->permission_requested(s.root, navigation.Get());
        s.browser->sites->stop_page("root", "page_navigated", false);
        assert(navigation->state == COREWEBVIEW2_PERMISSION_STATE_DENY && navigation->probe->completions == 1);
    }
    {
        InteractionSetup s;
        auto args = Microsoft::WRL::Make<PermissionProbe>();
        s.browser->sites->permission_requested(s.child, args.Get());
        s.host.close(s.child, "user_closed");
        assert(args->state == COREWEBVIEW2_PERMISSION_STATE_DENY && args->probe->completions == 1 && !s.root->closing);
        auto disconnected = Microsoft::WRL::Make<PermissionProbe>();
        s.browser->sites->permission_requested(s.root, disconnected.Get());
        s.host.disconnect();
        assert(disconnected->state == COREWEBVIEW2_PERMISSION_STATE_DENY && disconnected->probe->completions == 1);
    }
    {
        InteractionSetup s;
        auto args = Microsoft::WRL::Make<PermissionProbe>(); args->kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
        s.browser->sites->permission_requested(s.root, args.Get());
        assert(args->state == COREWEBVIEW2_PERMISSION_STATE_DENY && args->probe->completions == 1);
        assert(download_filename(L"C:\\Downloads\\..\\CON") == L"download-CON");
        assert(download_filename(L"../../a:b?.txt ") == L"download-a_b_.txt");
        assert(download_filename(L"..") == L"download-file");
    }
    {
        InteractionSetup s;
        s.root->window = CreateWindowW(L"STATIC", L"Download history regression", WS_OVERLAPPEDWINDOW,
            0, 0, 800, 600, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        assert(s.root->window);
        auto sites = s.browser->sites;
        for (int i = 0; i < 33; ++i) {
            auto record = std::make_shared<DownloadRecord>();
            record->id = std::to_string(i); record->page_id = "root"; record->done = true;
            record->status = "completed"; record->path = "C:/test/file.txt";
            sites->downloads.push_back(record);
        }
        sites->show_downloads(s.root);
        assert(sites->downloads_window);
        SendDlgItemMessage(sites->downloads_window, 301, LB_SETCURSEL, 5, 0);
        SendMessage(sites->downloads_window, WM_COMMAND, MAKEWPARAM(301, LBN_SELCHANGE), 0);
        sites->tick(); // Removing the oldest record must not retarget the selected download.
        assert(sites->downloads.size() == 32 && sites->selected_download_id == "5");
        assert(SendDlgItemMessage(sites->downloads_window, 301, LB_GETCURSEL, 0, 0) == 4);
        SendMessage(sites->downloads_window, WM_CLOSE, 0, 0);
        assert(!sites->downloads_window && IsWindow(s.root->window));
    }
    {
        InteractionSetup s;
        s.host.reconnectable = true;
        s.host.connection = std::make_shared<PipeConnection>();
        auto pending = Microsoft::WRL::Make<PermissionProbe>();
        s.browser->sites->permission_requested(s.root, pending.Get());
        assert(s.browser->sites->pending());
        s.host.connection->alive = false; // Transport detected loss before the STA detach message.
        s.browser->sites->resolve_permission(s.browser->sites->permissions.back()->id, true, "test_native_decision");
        assert(pending->state == COREWEBVIEW2_PERMISSION_STATE_DENY && pending->probe->completions == 1);
        assert(s.browser->sites->permissions.back()->reason == "disconnected");
        auto late = Microsoft::WRL::Make<PermissionProbe>();
        s.browser->sites->permission_requested(s.root, late.Get());
        assert(late->state == COREWEBVIEW2_PERMISSION_STATE_DENY && late->probe->completions == 1);
        assert(!s.browser->sites->pending());
    }
    std::cout << "Site interaction lifecycle tests passed: revoke, drain, one-request grant, fresh observation, deny, timeout, navigation, close, disconnect, filename, stable download selection, atomic connection-loss guard (COM probes, not device permission acceptance)\n";
}
