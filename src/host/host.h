#pragma once
#include "../browser/session.h"
#include "pipe_output.h"
#include <set>

struct Host {
    distinct::KeyboardGuard keyboard_guard;
    HWND dispatcher = nullptr;
    std::unique_ptr<distinct::PipeOutput> output;
    std::map<std::string, std::shared_ptr<Browser>> browsers;
    std::set<std::string> pending;
    bool connected = true;
    std::filesystem::path data_root;
    void send(distinct::Json message);
    void success(const std::string& id, distinct::Json result = distinct::Json());
    void failure(const std::string& id, const std::string& code, const std::string& message, distinct::Json detail = distinct::Json());
    void event(const std::shared_ptr<Browser>& browser, const std::string& type, distinct::Json detail = distinct::Json());
    void event(const std::shared_ptr<Page>& page, const std::string& type, distinct::Json detail = distinct::Json());
    bool expire(const std::shared_ptr<Page>& page);
    void revoke(const std::shared_ptr<Page>& page, distinct::Mode mode, const std::string& reason);
    void finish(const std::shared_ptr<Page>& page, std::shared_ptr<Operation> op,
                const std::string& code = "", const std::string& message = "", distinct::Json result = distinct::Json());
    std::string gate(const std::shared_ptr<Page>& page, const std::shared_ptr<Operation>& op);
    void close(const std::shared_ptr<Page>& page, const std::string& reason);
    void close_browser(const std::shared_ptr<Browser>& browser, const std::string& reason);
    void fault(const std::shared_ptr<Page>& page, const std::string& reason);
    void disconnect();
    void tick();
    void create(const std::string& request, const std::string& label);
    std::shared_ptr<Page> make_page(const std::shared_ptr<Browser>& browser, const std::string& opener = "");
    void create_controller(const std::shared_ptr<Page>& page);
    void ready_page(const std::shared_ptr<Page>& page);
    void new_window(const std::shared_ptr<Page>& opener, ICoreWebView2NewWindowRequestedEventArgs* args);
    void select(const std::shared_ptr<Page>& page, const std::string& reason);
    void initialize(const std::shared_ptr<Page>& page);
    void navigation_succeeded(const std::shared_ptr<Page>& page, const std::shared_ptr<Operation>& op, bool same_document);
    void finish_same_document_navigation(const std::shared_ptr<Page>& page);
    void handle(const std::string& line);
    void observe(const std::shared_ptr<Page>& page, const std::shared_ptr<Operation>& op);
    void dispatch_step(const std::shared_ptr<Page>& page, const std::shared_ptr<Operation>& op);
};

LRESULT CALLBACK PageProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
