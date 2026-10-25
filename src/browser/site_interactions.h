#pragma once
#include "session.h"

struct DownloadRecord {
    std::string id, page_id, uri, path, status = "in_progress", reason;
    INT64 received = 0, total = -1;
    uint64_t emitted = 0, progressed = GetTickCount64();
    bool done = false, dirty = true;
    Microsoft::WRL::ComPtr<ICoreWebView2DownloadOperation> operation;
    Microsoft::WRL::ComPtr<IUnknown> identity;
    EventRegistrationToken bytes_token{}, state_token{};
    bool bytes_registered = false, state_registered = false;
    ~DownloadRecord();
    void detach();
    distinct::Json json() const;
};

struct SiteInteractions;
struct PermissionRecord {
    std::weak_ptr<SiteInteractions> owner;
    std::weak_ptr<Page> page;
    std::string id, page_id, uri, kind, status = "pending", reason;
    uint64_t created = GetTickCount64(), revision = 0;
    bool done = false;
    HWND window = nullptr;
    HFONT font = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2PermissionRequestedEventArgs> args;
    Microsoft::WRL::ComPtr<ICoreWebView2Deferral> deferral;
    ~PermissionRecord();
    distinct::Json json() const;
};

// Browser-scoped records, page-scoped origin. No permission decision is exposed
// through RPC; only the native prompt can grant a one-request permission.
struct SiteInteractions : std::enable_shared_from_this<SiteInteractions> {
    Host* host;
    std::weak_ptr<Browser> browser;
    std::vector<std::shared_ptr<DownloadRecord>> downloads;
    std::vector<std::shared_ptr<PermissionRecord>> permissions;
    HWND downloads_window = nullptr;
    HFONT downloads_font = nullptr;
    std::string selected_download_id;
    SiteInteractions(Host* host, std::weak_ptr<Browser> browser) : host(host), browser(std::move(browser)) {}
    ~SiteInteractions();
    void attach(const std::shared_ptr<Page>& page);
    void download_starting(const std::shared_ptr<Page>& page, ICoreWebView2DownloadStartingEventArgs* args);
    void permission_requested(const std::shared_ptr<Page>& page, ICoreWebView2PermissionRequestedEventArgs* args);
    void update_download(const std::shared_ptr<DownloadRecord>& record);
    void resolve_permission(const std::string& id, bool allow, const std::string& reason);
    void show_pending(const std::shared_ptr<Page>& page);
    void show_downloads(const std::shared_ptr<Page>& page);
    void refresh_downloads();
    void cancel_download(const std::shared_ptr<DownloadRecord>& record, const std::string& reason);
    void stop_page(const std::string& page_id, const std::string& reason, bool cancel_downloads);
    void tick();
    bool pending() const;
    bool pre_translate(MSG& message);
    std::wstring notice(const std::string& page_id) const;
    distinct::Json snapshot(const std::string& page_id = "") const;
};

namespace distinct {
std::wstring download_filename(const std::wstring& suggestion);
}
