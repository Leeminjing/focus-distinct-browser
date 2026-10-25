#include "host.h"
#include <algorithm>
#include <utility>

using namespace distinct;

void Browser::refresh() { for (auto& [page_id, page] : pages) page->refresh(); }
void Browser::sync_input() {
    for (auto& [page_id, page] : pages)
        if (page->keyboard && page->keyboard->pending.exchange(false)) page->human_input();
}
bool Browser::visible() const {
    for (auto& [page_id, page] : pages)
        if (page->ready && (!IsWindowVisible(page->window) || IsIconic(page->window))) return false;
    return true;
}
void Host::send(Json message) { if (connected) output->send(message); }
void Host::success(const std::string& id, Json result) {
    pending.erase(id);
    send(object({{L"id", js(id)}, {L"ok", jb(true)}, {L"result", result}}));
}
void Host::failure(const std::string& id, const std::string& code, const std::string& message, Json detail) {
    pending.erase(id);
    send(object({{L"id", id.empty() ? Value::CreateNullValue() : js(id)}, {L"ok", jb(false)},
        {L"error", object({{L"code", js(code)}, {L"message", js(message)}, {L"detail", detail}})}}));
}
void Host::event(const std::shared_ptr<Browser>& browser, const std::string& type, Json detail) {
    send(object({{L"event", js(type)}, {L"browser_id", js(browser->id)},
        {L"state", js(mode_name(browser->control.mode))}, {L"detail", detail}}));
}
void Host::event(const std::shared_ptr<Page>& page, const std::string& type, Json detail) {
    if (auto browser = page->owner()) {
        detail.SetNamedValue(L"page_id", js(page->id));
        event(browser, type, detail);
    }
}
bool Host::expire(const std::shared_ptr<Page>& page) {
    auto browser = page->owner();
    if (!browser || !browser->control.expire(GetTickCount64())) return false;
    browser->refresh(); event(browser, "control_changed", object({{L"reason", js("lease_expired")}})); return true;
}
void Host::revoke(const std::shared_ptr<Page>& page, Mode mode, const std::string& reason) {
    auto browser = page->owner();
    if (!browser || browser->control.mode == Mode::closed || (browser->control.mode == Mode::faulted && mode != Mode::closed)) return;
    browser->control.revoke(mode); browser->refresh();
    event(browser, mode == Mode::paused ? "paused" : "control_changed", object({{L"reason", js(reason)}}));
}
void Host::finish(const std::shared_ptr<Page>& page, std::shared_ptr<Operation> op,
                  const std::string& code, const std::string& message, Json result) {
    if (op->done) return;
    auto browser = page->owner();
    if (browser) browser->sync_input();
    op->done = true;
    if (browser && browser->active == op) { browser->active.reset(); browser->control.end(); }
    if (code.empty()) success(op->request, result);
    else {
        result.SetNamedValue(L"dispatched_steps", jn(static_cast<double>(op->dispatched)));
        result.SetNamedValue(L"completed_steps", jn(static_cast<double>(op->completed)));
        result.SetNamedValue(L"result_unknown", jb(op->dispatched > 0));
        failure(op->request, code, message, result);
    }
    if (!browser) return;
    browser->refresh();
    if (browser->control.mode == Mode::manual) event(browser, "control_changed");
    for (auto& [id, other] : browser->pages)
        if (other->pending_manual && !other->closing) PostMessage(other->window, WM_APP + 4, 0, 0);
}
std::string Host::gate(const std::shared_ptr<Page>& page, const std::shared_ptr<Operation>& op) {
    auto browser = page->owner();
    if (!browser || browser->closing) return "browser_closed";
    browser->sync_input();
    if (op->done || page->closing) return "page_closed";
    if (!connected) return "disconnected";
    if (!browser->visible() && browser->control.mode != Mode::paused) revoke(page, Mode::paused, "window_not_visible");
    expire(page);
    auto error = browser->control.check(op->token, GetTickCount64());
    if (!error.empty()) return error;
    if (browser->selected != page || browser->selection_revision != op->selection_revision) return "page_changed";
    return {};
}
void Host::select(const std::shared_ptr<Page>& page, const std::string& reason) {
    auto browser = page->owner();
    if (!browser || browser->closing || page->closing || !page->ready || browser->selected == page) return;
    browser->selected = page; ++browser->selection_revision;
    page->selected_at = ++browser->selection_clock;
    browser->control.observed = false;
    event(page, "page_selected", object({{L"reason", js(reason)}}));
}
void Host::fault(const std::shared_ptr<Page>& page, const std::string& reason) {
    if (page->closing) return;
    auto browser = page->owner();
    if (!browser) return;
    revoke(page, Mode::faulted, reason);
    if (!browser->ready) { close_browser(browser, reason); return; }
    if (browser->active) finish(browser->active->page, browser->active, "browser_faulted", reason);
    // Creating popups cannot be allowed to complete after a session fault.
    auto pages = browser->pages;
    for (auto& [id, child] : pages) if (!child->ready) close(child, "browser_faulted");
    event(browser, "faulted", object({{L"reason", js(reason)}}));
}
void Host::disconnect() {
    if (!connected) return;
    connected = false;
    auto copy = browsers;
    for (auto& [id, browser] : copy) {
        if (browser->selected) revoke(browser->selected, Mode::manual, "disconnected");
        auto pages = browser->pages;
        for (auto& [page_id, page] : pages) if (!page->ready) close(page, "disconnected_during_creation");
    }
    if (browsers.empty()) PostQuitMessage(0);
}
void Host::tick() {
    auto copy = browsers;
    for (auto& [id, browser] : copy) {
        browser->sync_input();
        if (browser->selected) expire(browser->selected);
        auto pages = browser->pages;
        for (auto& [page_id, page] : pages)
            if (!page->ready && GetTickCount64() - page->created > 45000) close(page, "initialization_timeout");
        if (browser->active && GetTickCount64() - browser->active->started > 20000)
            fault(browser->active->page, "Operation timed out; dispatched effects may still complete. Close this browser to recover.");
    }
}
void Host::navigation_succeeded(const std::shared_ptr<Page>& page, const std::shared_ptr<Operation>& op, bool same_document) {
    page->invalidate(); op->completed = 1;
    finish(page, op, "", "", object({{L"navigation_completed", jb(true)}, {L"same_document", jb(same_document)}, {L"page_id", js(page->id)}}));
}
