#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/host/host.h"
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
}
