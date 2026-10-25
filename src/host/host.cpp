#include "host.h"
#include "../browser/site_interactions.h"
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
bool Host::connection_valid() const {
    return connected && (!reconnectable || (connection && connection->alive));
}
bool Host::current_request(const std::string& id) const {
    return !reconnectable || (connection_valid() && id.rfind(std::to_string(generation) + "|", 0) == 0);
}
void Host::send(Json message) {
    if (!connection_valid()) return;
    if (reconnectable) {
        message.SetNamedValue(L"host_id", js(host_id));
        message.SetNamedValue(L"connection_id", js(std::to_string(generation)));
        message.SetNamedValue(L"revision", jn(static_cast<double>(revision)));
    }
    output->send(message);
}
void Host::success(const std::string& id, Json result) {
    pending.erase(id);
    if (!current_request(id)) return;
    auto wire = reconnectable ? id.substr(id.find('|') + 1) : id;
    send(object({{L"id", js(wire)}, {L"ok", jb(true)}, {L"result", result}}));
}
void Host::failure(const std::string& id, const std::string& code, const std::string& message, Json detail) {
    pending.erase(id);
    if (!id.empty() && !current_request(id)) return;
    auto wire = reconnectable && !id.empty() ? id.substr(id.find('|') + 1) : id;
    send(object({{L"id", wire.empty() ? Value::CreateNullValue() : js(wire)}, {L"ok", jb(false)},
        {L"error", object({{L"code", js(code)}, {L"message", js(message)}, {L"detail", detail}})}}));
}
void Host::event(const std::shared_ptr<Browser>& browser, const std::string& type, Json detail) {
    ++revision;
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
    if (browser && reconnectable && !current_request(op->request)) {
        browser->interrupted_operation = object({{L"kind", js(op->kind)}, {L"page_id", js(page->id)},
            {L"dispatched_steps", jn(static_cast<double>(op->dispatched))}, {L"completed_steps", jn(static_cast<double>(op->completed))},
            {L"result_unknown", jb(op->dispatched > 0)}, {L"settled", jb(true)}});
    }
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
    if (!connection_valid() || !current_request(op->request)) return "disconnected";
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
    if (browser->sites) for (auto& [id, other] : browser->pages) browser->sites->stop_page(id, "browser_faulted", true);
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
    pending.clear();
    auto copy = browsers;
    for (auto& [id, browser] : copy) {
        if (auto op = browser->active; reconnectable && op) {
            browser->interrupted_operation = object({{L"kind", js(op->kind)}, {L"page_id", js(op->page->id)},
                {L"dispatched_steps", jn(static_cast<double>(op->dispatched))}, {L"completed_steps", jn(static_cast<double>(op->completed))},
                {L"result_unknown", jb(op->dispatched > 0)}, {L"settled", jb(false)}});
        }
        if (browser->selected) revoke(browser->selected, Mode::manual, "disconnected");
        auto pages = browser->pages;
        for (auto& [page_id, page] : pages) {
            if (browser->sites) browser->sites->stop_page(page_id, "disconnected", true);
            if (!page->ready) close(page, "disconnected_during_creation");
        }
        browser->refresh();
    }
    if (browsers.empty()) PostQuitMessage(0);
}
void Host::tick() {
    if (connected && !connection_valid()) disconnect();
    auto copy = browsers;
    for (auto& [id, browser] : copy) {
        if (browser->sites) browser->sites->tick();
        browser->sync_input();
        if (browser->selected) expire(browser->selected);
        auto pages = browser->pages;
        for (auto& [page_id, page] : pages)
            if (!page->ready && GetTickCount64() - page->created > 45000) close(page, "initialization_timeout");
        if (browser->active && GetTickCount64() - browser->active->started > 20000)
            fault(browser->active->page, "Operation timed out; dispatched effects may still complete. Close this browser to recover.");
    }
}
void Host::transport(TransportMessage& message) {
    auto incoming = message.connection;
    if (message.kind == TransportMessage::Kind::attach) {
        if (connected) disconnect();
        output.reset(); connection = incoming; generation = incoming->generation;
        connected = true;
        output = std::make_unique<PipeOutput>(incoming->pipe, [incoming] { incoming->alive = false; CancelIoEx(incoming->pipe, nullptr); }, true);
        for (auto& [id, browser] : browsers) browser->refresh();
        send(object({{L"event", js("ready")}, {L"protocol_version", jn(1)}, {L"pid", jn(GetCurrentProcessId())},
            {L"transport", js("named_pipe")}, {L"generation", jn(static_cast<double>(generation))}}));
        SetEvent(incoming->acknowledged);
    } else if (message.kind == TransportMessage::Kind::detach) {
        if (connection == incoming) { disconnect(); output.reset(); connection.reset(); }
        SetEvent(incoming->acknowledged);
    } else if (connection == incoming && incoming->alive) handle(message.line, incoming->generation);
}
Json Host::snapshot() {
    winrt::Windows::Data::Json::JsonArray sessions;
    for (auto& [id, browser] : browsers) {
        browser->sync_input();
        winrt::Windows::Data::Json::JsonArray pages;
        for (auto& [page_id, page] : browser->pages) {
            LPWSTR source = nullptr, title = nullptr;
            if (page->view) { page->view->get_Source(&source); page->view->get_DocumentTitle(&title); }
            pages.Append(object({{L"page_id", js(page_id)}, {L"opener_page_id", js(page->opener_id)},
                {L"url", js(source ? winrt::to_string(source) : "")}, {L"title", js(title ? winrt::to_string(title) : "")},
                {L"hwnd", js(std::to_string(reinterpret_cast<uintptr_t>(page->window)))}, {L"ready", jb(page->ready)},
                {L"loading", jb(page->navigating)}, {L"visible", jb(IsWindowVisible(page->window) != FALSE)},
                {L"minimized", jb(IsIconic(page->window) != FALSE)}}));
            CoTaskMemFree(source); CoTaskMemFree(title);
        }
        sessions.Append(object({{L"browser_id", js(id)}, {L"label", js(browser->label)},
            {L"state", js(mode_name(browser->control.mode))}, {L"busy", jb(browser->control.busy)},
            {L"selected_page_id", js(browser->selected ? browser->selected->id : "")}, {L"pages", pages},
            {L"interrupted_operation", browser->interrupted_operation},
            {L"site_interactions", browser->sites ? browser->sites->snapshot() : Json()}}));
    }
    return object({{L"host_id", js(host_id)}, {L"connection_id", js(std::to_string(generation))},
        {L"revision", jn(static_cast<double>(revision))}, {L"browsers", sessions}});
}
void Host::navigation_succeeded(const std::shared_ptr<Page>& page, const std::shared_ptr<Operation>& op, bool same_document) {
    page->invalidate(); op->completed = 1;
    finish(page, op, "", "", object({{L"navigation_completed", jb(true)}, {L"same_document", jb(same_document)}, {L"page_id", js(page->id)}}));
}
