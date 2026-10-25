#include "site_interactions.h"
#include "../host/host.h"
#include <shlobj.h>
#include <algorithm>
#include <utility>

using namespace distinct;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace {
std::string identifier() {
    GUID id{}; checked(CoCreateGuid(&id), "CoCreateGuid");
    wchar_t value[40]{}; StringFromGUID2(id, value, 40);
    return winrt::to_string(std::wstring(value + 1, 36));
}
std::string take(LPWSTR value) {
    auto text = value ? winrt::to_string(value) : "";
    CoTaskMemFree(value); return text;
}
std::string permission_kind(COREWEBVIEW2_PERMISSION_KIND kind) {
    switch (kind) {
    case COREWEBVIEW2_PERMISSION_KIND_MICROPHONE: return "microphone";
    case COREWEBVIEW2_PERMISSION_KIND_CAMERA: return "camera";
    case COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION: return "geolocation";
    case COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS: return "notifications";
    case COREWEBVIEW2_PERMISSION_KIND_OTHER_SENSORS: return "sensors";
    case COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ: return "clipboard_read";
    case COREWEBVIEW2_PERMISSION_KIND_MULTIPLE_AUTOMATIC_DOWNLOADS: return "automatic_downloads";
    case COREWEBVIEW2_PERMISSION_KIND_FILE_READ_WRITE: return "file_read_write";
    case COREWEBVIEW2_PERMISSION_KIND_AUTOPLAY: return "autoplay";
    case COREWEBVIEW2_PERMISSION_KIND_LOCAL_FONTS: return "local_fonts";
    case COREWEBVIEW2_PERMISSION_KIND_MIDI_SYSTEM_EXCLUSIVE_MESSAGES: return "midi";
    case COREWEBVIEW2_PERMISSION_KIND_WINDOW_MANAGEMENT: return "window_management";
    default: return "unknown";
    }
}
std::wstring permission_label(const std::string& kind) {
    static const std::map<std::string, std::wstring> labels = {
        {"microphone", L"麦克风"}, {"camera", L"摄像头"}, {"geolocation", L"位置"}, {"notifications", L"通知"},
        {"sensors", L"传感器"}, {"clipboard_read", L"读取剪贴板"}, {"automatic_downloads", L"自动下载多个文件"},
        {"file_read_write", L"文件读写"}, {"autoplay", L"自动播放"}, {"local_fonts", L"本机字体"},
        {"midi", L"MIDI 设备"}, {"window_management", L"管理屏幕与窗口"}};
    auto found = labels.find(kind);
    return found == labels.end() ? L"未知权限" : found->second;
}
void layout_prompt(HWND window, PermissionRecord* record) {
    const auto dpi = GetDpiForWindow(window);
    auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    RECT client{}; GetClientRect(window, &client);
    int width = client.right, height = client.bottom;
    SetWindowPos(GetDlgItem(window, 203), nullptr, px(20), px(16), width - px(40), px(32), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, 204), nullptr, px(20), px(55), width - px(40), height - px(125), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, 201), nullptr, width - px(260), height - px(52), px(115), px(32), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, 202), nullptr, width - px(135), height - px(52), px(115), px(32), SWP_NOZORDER);
    auto old = record->font;
    record->font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (int id : {201, 202, 203, 204}) SendMessage(GetDlgItem(window, id), WM_SETFONT, reinterpret_cast<WPARAM>(record->font), TRUE);
    if (old) DeleteObject(old);
}
LRESULT CALLBACK permission_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto record = reinterpret_cast<PermissionRecord*>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        record = static_cast<PermissionRecord*>(reinterpret_cast<CREATESTRUCT*>(lparam)->lpCreateParams);
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(record));
    }
    if (!record) return DefWindowProc(window, message, wparam, lparam);
    if (message == WM_COMMAND && (LOWORD(wparam) == 201 || LOWORD(wparam) == 202 || LOWORD(wparam) == IDCANCEL)) {
        if (auto owner = record->owner.lock()) owner->resolve_permission(record->id, LOWORD(wparam) == 201, "human_decision");
        return 0;
    }
    if (message == WM_CLOSE) {
        if (auto owner = record->owner.lock()) owner->resolve_permission(record->id, false, "prompt_closed");
        return 0;
    }
    if (message == WM_DPICHANGED) {
        auto rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout_prompt(window, record); return 0;
    }
    if (message == WM_NCDESTROY) {
        record->window = nullptr;
        if (record->font) { DeleteObject(record->font); record->font = nullptr; }
        SetWindowLongPtr(window, GWLP_USERDATA, 0);
    }
    return DefWindowProc(window, message, wparam, lparam);
}
void layout_downloads(HWND window, SiteInteractions* owner) {
    auto dpi = GetDpiForWindow(window);
    auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    RECT client{}; GetClientRect(window, &client);
    int width = client.right, height = client.bottom;
    SetWindowPos(GetDlgItem(window, 301), nullptr, px(16), px(16), width - px(32), std::max(px(60), height - px(245)), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, 302), nullptr, px(16), std::max(px(90), height - px(215)), width - px(32), px(145), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, 303), nullptr, width - px(270), height - px(52), px(125), px(32), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, 304), nullptr, width - px(135), height - px(52), px(115), px(32), SWP_NOZORDER);
    auto old = owner->downloads_font;
    owner->downloads_font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (int id : {301, 302, 303, 304}) SendMessage(GetDlgItem(window, id), WM_SETFONT, reinterpret_cast<WPARAM>(owner->downloads_font), TRUE);
    if (old) DeleteObject(old);
}
LRESULT CALLBACK downloads_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto owner = reinterpret_cast<SiteInteractions*>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        owner = static_cast<SiteInteractions*>(reinterpret_cast<CREATESTRUCT*>(lparam)->lpCreateParams);
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
    }
    if (!owner) return DefWindowProc(window, message, wparam, lparam);
    if (message == WM_COMMAND) {
        if (LOWORD(wparam) == 304 || LOWORD(wparam) == IDCANCEL) { DestroyWindow(window); return 0; }
        if (LOWORD(wparam) == 301 && HIWORD(wparam) == LBN_SELCHANGE) {
            auto index = SendDlgItemMessage(window, 301, LB_GETCURSEL, 0, 0);
            if (index >= 0 && static_cast<size_t>(index) < owner->downloads.size())
                owner->selected_download_id = owner->downloads[static_cast<size_t>(index)]->id;
            owner->refresh_downloads(); return 0;
        }
        if (LOWORD(wparam) == 303) {
            auto index = SendDlgItemMessage(window, 301, LB_GETCURSEL, 0, 0);
            if (index >= 0 && static_cast<size_t>(index) < owner->downloads.size())
                owner->cancel_download(owner->downloads[static_cast<size_t>(index)], "human_cancel");
            return 0;
        }
    }
    if (message == WM_SIZE) { layout_downloads(window, owner); return 0; }
    if (message == WM_DPICHANGED) {
        auto rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout_downloads(window, owner); return 0;
    }
    if (message == WM_GETMINMAXINFO) {
        auto limits = reinterpret_cast<MINMAXINFO*>(lparam); auto dpi = GetDpiForWindow(window);
        limits->ptMinTrackSize = {MulDiv(560, dpi, 96), MulDiv(390, dpi, 96)}; return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_NCDESTROY) {
        owner->downloads_window = nullptr;
        if (owner->downloads_font) { DeleteObject(owner->downloads_font); owner->downloads_font = nullptr; }
        SetWindowLongPtr(window, GWLP_USERDATA, 0);
    }
    return DefWindowProc(window, message, wparam, lparam);
}
}

std::wstring distinct::download_filename(const std::wstring& suggestion) {
    auto name = std::filesystem::path(suggestion).filename().wstring();
    if (name.size() > 100) name.resize(100);
    for (auto& ch : name) if (ch < 32 || std::wstring(L"<>:\"/\\|?*").find(ch) != std::wstring::npos) ch = L'_';
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    return L"download-" + (name.empty() ? L"file" : name);
}
void DownloadRecord::detach() {
    if (operation) {
        if (bytes_registered) operation->remove_BytesReceivedChanged(bytes_token);
        if (state_registered) operation->remove_StateChanged(state_token);
    }
    bytes_registered = state_registered = false;
}
DownloadRecord::~DownloadRecord() { detach(); }
Json DownloadRecord::json() const {
    return object({{L"download_id", js(id)}, {L"page_id", js(page_id)}, {L"url", js(uri)}, {L"path", js(path)},
        {L"state", js(status)}, {L"reason", js(reason)}, {L"bytes_received", jn(static_cast<double>(received))},
        {L"total_bytes", jn(static_cast<double>(total))}});
}
PermissionRecord::~PermissionRecord() {
    if (window) DestroyWindow(window);
    if (deferral) { if (args) args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); deferral->Complete(); }
}
Json PermissionRecord::json() const {
    return object({{L"request_id", js(id)}, {L"page_id", js(page_id)}, {L"url", js(uri)},
        {L"kind", js(kind)}, {L"state", js(status)}, {L"reason", js(reason)}});
}
SiteInteractions::~SiteInteractions() {
    if (downloads_window) DestroyWindow(downloads_window);
    for (auto& record : downloads) if (!record->done && record->operation) record->operation->Cancel();
}
bool SiteInteractions::pending() const {
    for (auto& item : permissions) if (!item->done) return true;
    for (auto& item : downloads) if (!item->done) return true;
    return false;
}
Json SiteInteractions::snapshot(const std::string& page_id) const {
    winrt::Windows::Data::Json::JsonArray files, decisions;
    for (auto& item : downloads) if (page_id.empty() || item->page_id == page_id) files.Append(item->json());
    for (auto& item : permissions) if (page_id.empty() || item->page_id == page_id) decisions.Append(item->json());
    return object({{L"downloads", files}, {L"permissions", decisions}, {L"interaction_pending", jb(pending())}});
}
std::wstring SiteInteractions::notice(const std::string& page_id) const {
    for (auto& item : permissions) if (!item->done) return L"网站权限待决定";
    for (auto& item : downloads) if (!item->done) return item->total > 0 ?
        L"下载中 " + std::to_wstring(static_cast<int>(std::clamp(static_cast<double>(item->received) / item->total * 100, 0.0, 100.0))) + L"%" : L"正在下载";
    for (auto i = downloads.rbegin(); i != downloads.rend(); ++i) if ((*i)->page_id == page_id)
        return (*i)->status == "completed" ? L"下载完成 · Ctrl+J" : L"下载中断 · Ctrl+J";
    for (auto i = permissions.rbegin(); i != permissions.rend(); ++i) if ((*i)->page_id == page_id)
        return (*i)->status == "allowed_once" ? L"已允许此次权限" : (*i)->status == "denied" ? L"已拒绝网站权限" : L"权限决定失败";
    return {};
}
void SiteInteractions::attach(const std::shared_ptr<Page>& page) {
    std::weak_ptr<SiteInteractions> weak = shared_from_this();
    std::weak_ptr<Page> target = page;
    EventRegistrationToken token{};
    ComPtr<ICoreWebView2_4> view4;
    checked(page->view.As(&view4), "DownloadStarting interface");
    checked(view4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
        [weak, target](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
            try {
                if (auto owner = weak.lock()) if (auto page = target.lock(); page && !page->closing) {
                    owner->download_starting(page, args); return S_OK;
                }
            } catch (...) {}
            args->put_Cancel(TRUE); return S_OK;
        }).Get(), &token), "DownloadStarting");
    checked(page->view->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
        [weak, target](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            try {
                if (auto owner = weak.lock()) if (auto page = target.lock(); page && !page->closing) {
                    owner->permission_requested(page, args); return S_OK;
                }
            } catch (...) {}
            args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); return S_OK;
        }).Get(), &token), "PermissionRequested");
}
void SiteInteractions::download_starting(const std::shared_ptr<Page>& page, ICoreWebView2DownloadStartingEventArgs* args) {
    ComPtr<ICoreWebView2DownloadOperation> native;
    ComPtr<IUnknown> identity;
    checked(args->get_DownloadOperation(&native), "DownloadOperation");
    checked(native.As(&identity), "Download identity");
    for (auto& existing : downloads) if (existing->identity.Get() == identity.Get()) {
        if (existing->done) args->put_Cancel(TRUE);
        else args->put_ResultFilePath(winrt::to_hstring(existing->path).c_str());
        return;
    }
    LPWSTR native_uri = nullptr;
    checked(native->get_Uri(&native_uri), "Download URI"); auto uri = take(native_uri);
    for (auto& existing : downloads) if (!existing->done && existing->page_id == page->id && existing->uri == uri) {
        args->put_Cancel(TRUE);
        host->event(page, "download_rejected", object({{L"reason", js("same_page_url_already_downloading")}, {L"download_id", js(existing->id)}}));
        return;
    }
    if (std::count_if(downloads.begin(), downloads.end(), [](auto& item) { return !item->done; }) >= 4) {
        args->put_Cancel(TRUE); host->event(page, "download_rejected", object({{L"reason", js("download_limit")}})); return;
    }
    auto record = std::make_shared<DownloadRecord>(); record->id = identifier(); record->page_id = page->id;
    downloads.push_back(record);
    host->revoke(page, Mode::manual, "download_started");
    try {
    record->operation = native; record->identity = identity;
    LPWSTR value = nullptr;
    checked(record->operation->get_Uri(&value), "Download URI"); record->uri = take(value);
    checked(args->get_ResultFilePath(&value), "Suggested download path"); auto name = download_filename(winrt::to_hstring(take(value)).c_str());
    checked(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &value), "Downloads folder");
    std::filesystem::path directory(value); CoTaskMemFree(value);
    auto session = browser.lock();
    directory /= L"Distinct Browser"; directory /= winrt::to_hstring(session->id).c_str(); directory /= winrt::to_hstring(record->id).c_str();
    std::filesystem::create_directories(directory.parent_path());
    if (!std::filesystem::create_directory(directory)) throw Error("download_path_failed", "Unique download folder already exists");
    auto destination = directory / name;
    checked(args->put_ResultFilePath(destination.c_str()), "Download result path");
    checked(args->put_Handled(TRUE), "Host download UI");
    record->path = winrt::to_string(destination.wstring());
    std::weak_ptr<SiteInteractions> weak = shared_from_this(); std::weak_ptr<DownloadRecord> item = record;
    checked(record->operation->add_BytesReceivedChanged(Callback<ICoreWebView2BytesReceivedChangedEventHandler>(
        [weak, item](ICoreWebView2DownloadOperation*, IUnknown*) -> HRESULT {
            if (auto owner = weak.lock()) if (auto record = item.lock()) owner->update_download(record);
            return S_OK;
        }).Get(), &record->bytes_token), "BytesReceivedChanged"); record->bytes_registered = true;
    checked(record->operation->add_StateChanged(Callback<ICoreWebView2StateChangedEventHandler>(
        [weak, item](ICoreWebView2DownloadOperation*, IUnknown*) -> HRESULT {
            if (auto owner = weak.lock()) if (auto record = item.lock()) owner->update_download(record);
            return S_OK;
        }).Get(), &record->state_token), "Download StateChanged"); record->state_registered = true;
    // A navigation that becomes an attachment is settled here, rather than
    // being faulted by the navigation timeout while a user handles a download.
    if (auto operation = page->operation(); operation && operation->kind == "navigate") {
        operation->completed = 1;
        host->finish(page, operation, "", "", object({{L"download_started", jb(true)}, {L"download_id", js(record->id)},
            {L"page_id", js(page->id)}, {L"control_still_valid", jb(false)}}));
    }
    update_download(record);
    selected_download_id = record->id;
    PostMessage(page->window, WM_APP + 9, 0, 0);
    } catch (...) {
        args->put_Cancel(TRUE);
        record->done = true; record->status = "interrupted"; record->reason = "host_setup_failed"; record->detach();
        host->event(page, "download_changed", record->json()); page->owner()->refresh();
        if (auto operation = page->operation(); operation && operation->kind == "navigate")
            host->finish(page, operation, "download_failed", "Download could not be initialized; inspect its record before deciding the next action");
    }
}
void SiteInteractions::update_download(const std::shared_ptr<DownloadRecord>& record) {
    if (record->done) return;
    const auto previous = record->received;
    const auto previous_total = record->total;
    record->operation->get_BytesReceived(&record->received);
    record->operation->get_TotalBytesToReceive(&record->total);
    if (record->received != previous) record->progressed = GetTickCount64();
    COREWEBVIEW2_DOWNLOAD_STATE state{};
    if (FAILED(record->operation->get_State(&state))) {
        record->done = true; record->status = "interrupted"; record->reason = "operation_unavailable";
    }
    if (state == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED) { record->done = true; record->status = "completed"; }
    if (state == COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED) {
        COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason{}; record->operation->get_InterruptReason(&reason);
        record->done = true; record->status = reason == COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_CANCELED ? "cancelled" : "interrupted";
        record->reason = std::to_string(static_cast<int>(reason));
    }
    record->dirty = record->dirty || record->done || previous != record->received || previous_total != record->total;
    if (record->done) record->detach();
    if (record->dirty && (record->done || GetTickCount64() - record->emitted >= 250)) {
        if (auto session = browser.lock()) {
            host->event(session, "download_changed", record->json()); session->refresh();
        }
        refresh_downloads();
        record->emitted = GetTickCount64(); record->dirty = false;
    }
}
void SiteInteractions::permission_requested(const std::shared_ptr<Page>& page, ICoreWebView2PermissionRequestedEventArgs* args) {
    auto record = std::make_shared<PermissionRecord>();
    record->owner = shared_from_this(); record->page = page; record->page_id = page->id; record->revision = page->revision;
    record->id = identifier(); record->args = args;
    checked(args->GetDeferral(&record->deferral), "Permission deferral");
    COREWEBVIEW2_PERMISSION_KIND kind{}; checked(args->get_PermissionKind(&kind), "Permission kind"); record->kind = permission_kind(kind);
    LPWSTR uri = nullptr; checked(args->get_Uri(&uri), "Permission URI"); record->uri = take(uri);
    permissions.push_back(record);
    ComPtr<ICoreWebView2PermissionRequestedEventArgs3> extended;
    bool supported = SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&extended))) &&
        SUCCEEDED(extended->put_SavesInProfile(FALSE)) && SUCCEEDED(extended->put_Handled(TRUE));
    if (!supported || record->kind == "unknown" || !host->connected ||
        std::count_if(permissions.begin(), permissions.end(), [](auto& item) { return !item->done; }) > 8) {
        resolve_permission(record->id, false, !host->connected ? "disconnected" : "unsupported_or_request_limit"); return;
    }
    host->revoke(page, Mode::manual, "permission_requested");
    host->event(page, "permission_requested", record->json());
    PostMessage(page->window, WM_APP + 8, 0, 0);
}
void SiteInteractions::resolve_permission(const std::string& id, bool allow, const std::string& reason) {
    auto found = std::find_if(permissions.begin(), permissions.end(), [&](auto& item) { return item->id == id; });
    if (found == permissions.end()) return;
    auto record = *found;
    if (record->done) return;
    auto page = record->page.lock();
    allow = allow && page && !page->closing && host->connected && page->revision == record->revision;
    record->done = true; record->reason = reason; record->status = allow ? "allowed_once" : "denied";
    if (record->window) { auto window = std::exchange(record->window, nullptr); DestroyWindow(window); }
    HRESULT result = record->args->put_State(allow ? COREWEBVIEW2_PERMISSION_STATE_ALLOW : COREWEBVIEW2_PERMISSION_STATE_DENY);
    if (FAILED(result)) { record->status = "failed"; record->reason = "permission_state_failed"; }
    auto deferral = std::move(record->deferral); record->args.Reset();
    if (deferral && FAILED(deferral->Complete())) { record->status = "failed"; record->reason = "permission_complete_failed"; }
    if (auto session = browser.lock()) { host->event(session, "permission_resolved", record->json()); session->refresh(); }
}
void SiteInteractions::show_pending(const std::shared_ptr<Page>& page) {
    if (!page->window || page->closing) return;
    static ATOM atom = 0;
    if (!atom) {
        WNDCLASSEXW type{sizeof(type)}; type.lpfnWndProc = permission_proc; type.hInstance = GetModuleHandle(nullptr);
        type.lpszClassName = L"DistinctBrowserPermission"; type.hCursor = LoadCursor(nullptr, IDC_ARROW);
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); atom = RegisterClassExW(&type);
    }
    for (auto& record : permissions) if (!record->done && record->page_id == page->id) {
        if (record->window) { ShowWindow(record->window, SW_RESTORE); SetForegroundWindow(record->window); continue; }
        auto dpi = GetDpiForWindow(page->window);
        auto label = permission_label(record->kind);
        auto title = L"网站权限 · " + label;
        record->window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"DistinctBrowserPermission", title.c_str(),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(640, dpi, 96), MulDiv(270, dpi, 96),
            page->window, nullptr, GetModuleHandle(nullptr), record.get());
        if (!record->window) { resolve_permission(record->id, false, "prompt_creation_failed"); continue; }
        auto text = L"网站请求" + label + L"，仅对本次请求授权。";
        CreateWindowW(L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, record->window, reinterpret_cast<HMENU>(203), nullptr, nullptr);
        auto uri = winrt::to_hstring(record->uri + "\r\n页面：" + record->page_id);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", uri.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, record->window, reinterpret_cast<HMENU>(204), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"允许一次", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
            record->window, reinterpret_cast<HMENU>(201), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"拒绝", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0,
            record->window, reinterpret_cast<HMENU>(202), nullptr, nullptr);
        layout_prompt(record->window, record.get());
        ShowWindow(record->window, SW_SHOWNORMAL); SetFocus(GetDlgItem(record->window, 202));
    }
}
void SiteInteractions::show_downloads(const std::shared_ptr<Page>& page) {
    if (!page->window || page->closing) return;
    if (downloads_window) { ShowWindow(downloads_window, SW_RESTORE); SetForegroundWindow(downloads_window); refresh_downloads(); return; }
    static ATOM atom = 0;
    if (!atom) {
        WNDCLASSEXW type{sizeof(type)}; type.lpfnWndProc = downloads_proc; type.hInstance = GetModuleHandle(nullptr);
        type.lpszClassName = L"DistinctBrowserDownloads"; type.hCursor = LoadCursor(nullptr, IDC_ARROW);
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); atom = RegisterClassExW(&type);
    }
    auto session = browser.lock(); if (!session) return;
    auto title = winrt::to_hstring("下载 · " + session->label); auto dpi = GetDpiForWindow(page->window);
    downloads_window = CreateWindowExW(WS_EX_CONTROLPARENT, L"DistinctBrowserDownloads", title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(720, dpi, 96), MulDiv(480, dpi, 96), page->window, nullptr, GetModuleHandle(nullptr), this);
    if (!downloads_window) return;
    CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        0, 0, 0, 0, downloads_window, reinterpret_cast<HMENU>(301), nullptr, nullptr);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        0, 0, 0, 0, downloads_window, reinterpret_cast<HMENU>(302), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"取消所选下载", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
        downloads_window, reinterpret_cast<HMENU>(303), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"关闭面板", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
        downloads_window, reinterpret_cast<HMENU>(304), nullptr, nullptr);
    layout_downloads(downloads_window, this); refresh_downloads(); ShowWindow(downloads_window, SW_SHOWNORMAL);
}
void SiteInteractions::refresh_downloads() {
    if (!downloads_window) return;
    auto selected = std::find_if(downloads.begin(), downloads.end(), [&](auto& record) { return record->id == selected_download_id; });
    auto index = selected == downloads.end() ? (downloads.empty() ? -1 : static_cast<LRESULT>(downloads.size() - 1)) :
        static_cast<LRESULT>(std::distance(downloads.begin(), selected));
    selected_download_id = index < 0 ? "" : downloads[static_cast<size_t>(index)]->id;
    SendDlgItemMessage(downloads_window, 301, WM_SETREDRAW, FALSE, 0);
    SendDlgItemMessage(downloads_window, 301, LB_RESETCONTENT, 0, 0);
    for (auto& record : downloads) {
        auto state = record->status == "completed" ? L"已完成" : record->status == "cancelled" ? L"已取消" : record->status == "interrupted" ? L"已中断" : L"下载中";
        auto text = std::wstring(state) + L"  " + std::filesystem::path(winrt::to_hstring(record->path).c_str()).filename().wstring() +
            L"  " + std::to_wstring(record->received) + L" / " + (record->total < 0 ? L"未知" : std::to_wstring(record->total)) + L" 字节";
        SendDlgItemMessage(downloads_window, 301, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendDlgItemMessage(downloads_window, 301, LB_SETCURSEL, index, 0);
    SendDlgItemMessage(downloads_window, 301, WM_SETREDRAW, TRUE, 0); InvalidateRect(GetDlgItem(downloads_window, 301), nullptr, TRUE);
    std::wstring detail = L"下载进行中时自动操作暂停；处理结束后到网页工具栏交还控制。";
    bool cancel = false;
    if (index >= 0) {
        auto record = downloads[static_cast<size_t>(index)]; cancel = !record->done;
        detail = winrt::to_hstring("地址：" + record->uri + "\r\n路径：" + record->path + "\r\n页面：" + record->page_id +
            "\r\n状态：" + record->status + "\r\n原因：" + record->reason).c_str();
    }
    SetDlgItemText(downloads_window, 302, detail.c_str()); EnableWindow(GetDlgItem(downloads_window, 303), cancel);
}
void SiteInteractions::cancel_download(const std::shared_ptr<DownloadRecord>& record, const std::string& reason) {
    if (record->done) return;
    record->done = true; record->status = reason == "no_progress_timeout" ? "interrupted" : "cancelled"; record->reason = reason;
    if (FAILED(record->operation->Cancel())) { record->status = "interrupted"; record->reason = "cancel_failed"; }
    record->detach();
    if (auto session = browser.lock()) { host->event(session, "download_changed", record->json()); session->refresh(); }
    refresh_downloads();
}
bool SiteInteractions::pre_translate(MSG& message) {
    if (downloads_window && (message.hwnd == downloads_window || IsChild(downloads_window, message.hwnd)))
        return IsDialogMessage(downloads_window, &message) != FALSE;
    for (auto& record : permissions)
        if (record->window && (message.hwnd == record->window || IsChild(record->window, message.hwnd))) {
            if (message.message == WM_KEYDOWN && (message.wParam == VK_ESCAPE || message.wParam == VK_RETURN)) {
                resolve_permission(record->id, message.wParam == VK_RETURN && GetFocus() == GetDlgItem(record->window, 201), "human_decision");
                return true;
            }
            return IsDialogMessage(record->window, &message) != FALSE;
        }
    return false;
}
void SiteInteractions::stop_page(const std::string& page_id, const std::string& reason, bool cancel_downloads) {
    auto copy = permissions;
    for (auto& item : copy) if (!item->done && item->page_id == page_id) resolve_permission(item->id, false, reason);
    if (!cancel_downloads) return;
    for (auto& item : downloads) if (!item->done && item->page_id == page_id) {
        cancel_download(item, reason);
    }
}
void SiteInteractions::tick() {
    auto copy = permissions;
    for (auto& item : copy) if (!item->done && GetTickCount64() - item->created >= 120000) resolve_permission(item->id, false, "timeout");
    // Reconcile the native state as well as listening for callbacks. This also
    // records a terminal state after an interruption invalidates the COM object.
    for (auto& item : downloads) if (!item->done) {
        update_download(item);
        if (!item->done && GetTickCount64() - item->progressed >= 30000) cancel_download(item, "no_progress_timeout");
    }
    const auto download_count = downloads.size();
    while (downloads.size() > 32) {
        auto found = std::find_if(downloads.begin(), downloads.end(), [](auto& item) { return item->done; });
        if (found == downloads.end()) break;
        downloads.erase(found);
    }
    if (downloads.size() != download_count) refresh_downloads();
    while (permissions.size() > 16) {
        auto found = std::find_if(permissions.begin(), permissions.end(), [](auto& item) { return item->done; });
        if (found == permissions.end()) break;
        permissions.erase(found);
    }
}
