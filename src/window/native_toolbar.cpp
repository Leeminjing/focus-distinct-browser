#include "native_toolbar.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <iterator>

using Microsoft::WRL::ComPtr;
namespace distinct {
namespace {
D2D1_COLOR_F rgb(unsigned value) {
    return D2D1::ColorF(static_cast<float>((value >> 16) & 255) / 255,
        static_cast<float>((value >> 8) & 255) / 255, static_cast<float>(value & 255) / 255);
}
D2D1_COLOR_F system_color(int index) {
    auto value = GetSysColor(index);
    return D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f, GetBValue(value) / 255.0f);
}
void line(ID2D1RenderTarget* target, ID2D1Brush* brush, float x1, float y1, float x2, float y2) {
    target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, 1.6f);
}
}
NativeToolbar::~NativeToolbar() {
    if (font_) DeleteObject(font_);
    if (edit_brush_) DeleteObject(edit_brush_);
    if (tooltip_ && IsWindow(tooltip_)) DestroyWindow(tooltip_);
}
int NativeToolbar::px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }
int NativeToolbar::height_pixels() const { return px(height_dip); }
D2D1_COLOR_F NativeToolbar::color(unsigned light, unsigned dark) const { return rgb(dark_ ? dark : light); }
COLORREF NativeToolbar::edit_color() const {
    if (high_contrast_) return GetSysColor(COLOR_WINDOW);
    return dark_ ? RGB(35, 43, 54) : RGB(255, 255, 255);
}
HRESULT NativeToolbar::create(HWND parent, std::function<void(ShellCommand)> callback) {
    parent_ = parent; callback_ = std::move(callback);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    if (!InitCommonControlsEx(&controls)) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write_.GetAddressOf()));
    if (FAILED(hr)) return hr;
    hr = write_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"zh-CN", &text_);
    if (FAILED(hr)) return hr;
    text_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    text_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    text_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    WNDCLASS type{}; type.hInstance = GetModuleHandle(nullptr); type.lpfnWndProc = window_proc;
    type.lpszClassName = L"DistinctNativeToolbar"; type.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClass(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return HRESULT_FROM_WIN32(GetLastError());
    background_ = CreateWindowEx(0, type.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0, parent, nullptr, type.hInstance, this);
    if (!background_) return HRESULT_FROM_WIN32(GetLastError());
    auto button = [&](int id, const wchar_t* caption) {
        HWND result = CreateWindowEx(0, L"BUTTON", caption, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), type.hInstance, nullptr);
        if (result) SetWindowSubclass(result, control_proc, 1, reinterpret_cast<DWORD_PTR>(this));
        return result;
    };
    back_ = button(back_id, L"返回 (Alt+Left)");
    forward_ = button(forward_id, L"前进 (Alt+Right)");
    reload_ = button(reload_id, L"刷新 (Ctrl+R)");
    address_ = CreateWindowEx(0, L"EDIT", L"about:blank", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(address_id)), type.hInstance, nullptr);
    status_ = CreateWindowEx(0, L"STATIC", L"正在准备", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(status_id)), type.hInstance, nullptr);
    takeover_ = button(takeover_id, L"接管");
    if (!back_ || !forward_ || !reload_ || !address_ || !status_ || !takeover_) return HRESULT_FROM_WIN32(GetLastError());
    SetWindowSubclass(address_, control_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessage(address_, EM_SETLIMITTEXT, 8192, 0);
    SendMessage(address_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"输入网址"));
    tooltip_ = CreateWindowEx(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr, type.hInstance, nullptr);
    const std::pair<HWND, const wchar_t*> tips[] = {{back_, L"返回 (Alt+Left)"}, {forward_, L"前进 (Alt+Right)"},
        {reload_, L"刷新 / 停止 (Ctrl+R / Esc)"}, {address_, L"输入地址后按 Enter · Ctrl+L 选择地址"},
        {takeover_, L"接管 / 交还自动控制"}};
    for (const auto& tip : tips) {
        TOOLINFO info{sizeof(info)}; info.uFlags = TTF_IDISHWND | TTF_SUBCLASS; info.hwnd = parent;
        info.uId = reinterpret_cast<UINT_PTR>(tip.first); info.lpszText = const_cast<LPWSTR>(tip.second);
        SendMessage(tooltip_, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&info));
    }
    theme_changed(); layout(); apply_state();
    return S_OK;
}
void NativeToolbar::theme_changed() {
    HIGHCONTRAST contrast{sizeof(contrast)};
    SystemParametersInfo(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    high_contrast_ = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    DWORD light = 1, size = sizeof(light);
    RegGetValue(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    dark_ = !high_contrast_ && light == 0;
    BOOL animations = TRUE; SystemParametersInfo(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    animate_ = animations && !high_contrast_;
    if (state_.loading && animate_) SetTimer(background_, 1, 50, nullptr);
    else KillTimer(background_, 1);
    BOOL dark_caption = dark_; DwmSetWindowAttribute(parent_, 20, &dark_caption, sizeof(dark_caption));
    // Windows 11 caption colors keep inactive title text legible as well.
    COLORREF caption = dark_ ? RGB(23, 28, 36) : 0xFFFFFFFF;
    COLORREF caption_text = dark_ ? RGB(231, 237, 245) : 0xFFFFFFFF;
    DwmSetWindowAttribute(parent_, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(parent_, 36, &caption_text, sizeof(caption_text));
    if (edit_brush_) DeleteObject(edit_brush_);
    edit_brush_ = CreateSolidBrush(edit_color());
    target_.Reset(); dc_target_.Reset();
    invalidate();
}
void NativeToolbar::layout() {
    if (!background_) return;
    UINT new_dpi = GetDpiForWindow(parent_);
    if (new_dpi != dpi_ || !font_) {
        dpi_ = new_dpi;
        HFONT font = CreateFont(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SendMessage(address_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (font_) DeleteObject(font_);
        font_ = font; target_.Reset(); dc_target_.Reset();
    }
    RECT client{}; GetClientRect(parent_, &client);
    int width = client.right, right = width - px(12);
    compact_ = width < px(720);
    const int state_width = px(compact_ ? 86 : 128);
    const int button_width = px(70);
    MoveWindow(background_, 0, 0, width, height_pixels(), TRUE);
    SetWindowPos(background_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    MoveWindow(back_, px(10), px(14), px(32), px(36), TRUE);
    MoveWindow(forward_, px(44), px(14), px(32), px(36), TRUE);
    MoveWindow(reload_, px(78), px(14), px(32), px(36), TRUE);
    MoveWindow(takeover_, right - button_width, px(14), button_width, px(36), TRUE);
    right -= button_width + px(8);
    MoveWindow(status_, right - state_width, px(16), state_width, px(32), TRUE);
    right -= state_width + px(12);
    address_box_ = {px(120), px(13), std::max(px(236), right), px(51)};
    MoveWindow(address_, address_box_.left + px(33), px(22),
        std::max(1L, address_box_.right - address_box_.left - px(45)), px(23), TRUE);
    if (target_) target_->Resize(D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height_pixels())));
    apply_state(); invalidate();
}
void NativeToolbar::update(const ShellState& state) {
    bool was_loading = state_.loading;
    state_ = state;
    if (state.loading && !was_loading && animate_) SetTimer(background_, 1, 50, nullptr);
    if (!state.loading) KillTimer(background_, 1);
    apply_state(); invalidate();
}
void NativeToolbar::apply_state() {
    if (!background_) return;
    bool usable = state_.ready && state_.mode != Mode::faulted && state_.mode != Mode::closed;
    EnableWindow(back_, usable && state_.can_back);
    EnableWindow(forward_, usable && state_.can_forward);
    EnableWindow(reload_, usable);
    EnableWindow(address_, usable);
    bool handback = state_.mode == Mode::manual || state_.mode == Mode::paused;
    SetWindowText(takeover_, handback ? L"交还" : L"接管");
    SetWindowText(reload_, state_.loading ? L"停止加载 (Esc)" : L"刷新 (Ctrl+R)");
    EnableWindow(takeover_, state_.ready && state_.connected && state_.mode != Mode::faulted &&
        state_.mode != Mode::taking_over && !state_.busy);
    // Takeover itself must remain available while an automatic action is pending.
    if (usable && state_.connected && (state_.mode == Mode::automatic || state_.mode == Mode::available)) EnableWindow(takeover_, TRUE);
    if (state_.mode == Mode::faulted) status_text_ = compact_ ? L"页面故障" : L"页面故障 · 请关闭";
    else if (!state_.connected) status_text_ = L"连接已断开";
    else if (!state_.notice.empty()) status_text_ = state_.notice;
    else if (!state_.ready) status_text_ = L"正在准备";
    else switch (state_.mode) {
    case Mode::automatic: status_text_ = compact_ ? L"Agent 操作" : L"Agent 正在操作"; break;
    case Mode::taking_over: status_text_ = L"接管中"; break;
    case Mode::manual: status_text_ = L"人工控制"; break;
    case Mode::paused: status_text_ = L"已暂停"; break;
    default: status_text_ = state_.busy ? (compact_ ? L"收尾中" : L"等待操作收尾") : L"可自动控制"; break;
    }
    SetWindowText(status_, status_text_.c_str());
}
void NativeToolbar::set_url(const std::wstring& url) {
    url_ = url;
    if (GetFocus() != address_) {
        setting_text_ = true; SetWindowText(address_, url.c_str()); setting_text_ = false;
    }
}
std::wstring NativeToolbar::input_url() const {
    int count = GetWindowTextLength(address_);
    std::wstring text(static_cast<size_t>(count) + 1, L'\0');
    GetWindowText(address_, text.data(), count + 1); text.resize(count);
    return text;
}
void NativeToolbar::focus_address() {
    SetFocus(address_); SendMessage(address_, EM_SETSEL, 0, -1);
}
void NativeToolbar::focus_from_page(bool previous) {
    HWND order[] = {back_, forward_, reload_, address_, takeover_};
    for (int index = previous ? 4 : 0; index >= 0 && index < 5; index += previous ? -1 : 1) {
        if (IsWindowEnabled(order[index])) { SetFocus(order[index]); return; }
    }
}
bool NativeToolbar::shortcut(UINT key, bool ctrl, bool alt) {
    if ((ctrl && key == 'L') || (alt && key == 'D')) { focus_address(); return true; }
    if ((ctrl && key == 'R') || key == VK_F5) { callback_(ShellCommand::reload); return true; }
    if (alt && key == VK_LEFT) { callback_(ShellCommand::back); return true; }
    if (alt && key == VK_RIGHT) { callback_(ShellCommand::forward); return true; }
    if (key == VK_ESCAPE) {
        if (GetFocus() == address_) { SetWindowText(address_, url_.c_str()); callback_(ShellCommand::focus_page); }
        else if (state_.loading) callback_(ShellCommand::reload_stop);
        else return false;
        return true;
    }
    return false;
}
bool NativeToolbar::pre_translate(const MSG& message) {
    if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
        if (message.hwnd == address_ && ime_composing_) return false;
        if (shortcut(static_cast<UINT>(message.wParam), GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_MENU) < 0)) return true;
        if (message.wParam == VK_RETURN && message.hwnd == address_) { callback_(ShellCommand::navigate); return true; }
        if (message.wParam == VK_RETURN && (message.hwnd == back_ || message.hwnd == forward_ || message.hwnd == reload_ || message.hwnd == takeover_)) {
            if (IsWindowEnabled(message.hwnd)) SendMessage(message.hwnd, BM_CLICK, 0, 0);
            return true;
        }
        if (message.wParam == VK_TAB) {
            HWND order[] = {back_, forward_, reload_, address_, takeover_};
            for (int index = 0; index < 5; ++index) if (message.hwnd == order[index]) {
                int step = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
                for (int next = index + step; next >= 0 && next < 5; next += step) {
                    if (IsWindowEnabled(order[next])) { SetFocus(order[next]); return true; }
                }
                callback_(step > 0 ? ShellCommand::focus_page_next : ShellCommand::focus_page_previous); return true;
            }
        }
    }
    return false;
}
bool NativeToolbar::parent_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
    if (message == WM_DRAWITEM) {
        auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (item->CtlID == back_id || item->CtlID == forward_id || item->CtlID == reload_id || item->CtlID == takeover_id || item->CtlID == status_id) {
            draw_item(*item); result = TRUE; return true;
        }
    }
    if ((message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC) && reinterpret_cast<HWND>(lparam) == address_) {
        SetTextColor(reinterpret_cast<HDC>(wparam), !IsWindowEnabled(address_) ? GetSysColor(COLOR_GRAYTEXT) :
            high_contrast_ ? GetSysColor(COLOR_WINDOWTEXT) : dark_ ? RGB(231, 237, 245) : RGB(28, 39, 54));
        SetBkColor(reinterpret_cast<HDC>(wparam), edit_color());
        result = reinterpret_cast<LRESULT>(edit_brush_); return true;
    }
    if (message == WM_COMMAND) {
        int id = LOWORD(wparam), code = HIWORD(wparam);
        if (id == address_id && code == EN_SETFOCUS && !setting_text_) {
            callback_(ShellCommand::address_focus); invalidate(); result = 0; return true;
        }
        if (id == address_id && code == EN_KILLFOCUS) {
            ime_composing_ = false;
            SetWindowText(address_, url_.c_str());
            invalidate(); result = 0; return true;
        }
        if (code == BN_CLICKED) {
            if (id == takeover_id) callback_(ShellCommand::takeover);
            else if (id == back_id) callback_(ShellCommand::back);
            else if (id == forward_id) callback_(ShellCommand::forward);
            else if (id == reload_id) callback_(ShellCommand::reload_stop);
            else return false;
            result = 0; return true;
        }
    }
    return false;
}
void NativeToolbar::invalidate() {
    for (HWND item : {background_, address_, status_, back_, forward_, reload_, takeover_})
        if (item && IsWindow(item)) InvalidateRect(item, nullptr, FALSE);
}
void NativeToolbar::paint_background(ID2D1RenderTarget* target) {
    ComPtr<ID2D1SolidColorBrush> brush;
    auto base = high_contrast_ ? system_color(COLOR_WINDOW) : color(0xF3F5F7, 0x171C24);
    target->CreateSolidColorBrush(base, &brush);
    if (!brush) return;
    target->Clear(base);
    float scale = 96.0f / dpi_;
    auto rect = D2D1::RectF(address_box_.left * scale, address_box_.top * scale, address_box_.right * scale, address_box_.bottom * scale);
    brush->SetColor(high_contrast_ ? system_color(COLOR_WINDOW) : color(0xFFFFFF, 0x232B36));
    target->FillRoundedRectangle(D2D1::RoundedRect(rect, 8, 8), brush.Get());
    brush->SetColor(GetFocus() == address_ ? color(0x185DCE, 0x8BB8FF) : high_contrast_ ? system_color(COLOR_WINDOWTEXT) : color(0xD9E0E8, 0x3D4858));
    target->DrawRoundedRectangle(D2D1::RoundedRect(rect, 8, 8), brush.Get(), GetFocus() == address_ ? 1.5f : 1);
    brush->SetColor(high_contrast_ ? system_color(COLOR_WINDOWTEXT) : color(0x58677B, 0xA7B3C3));
    auto center = D2D1::Point2F(rect.left + 18, 32);
    target->DrawEllipse(D2D1::Ellipse(center, 7, 7), brush.Get(), 1.2f);
    target->DrawEllipse(D2D1::Ellipse(center, 3, 7), brush.Get(), 1);
    line(target, brush.Get(), center.x - 7, center.y, center.x + 7, center.y);
    auto size = target->GetSize();
    brush->SetColor(high_contrast_ ? system_color(COLOR_WINDOWTEXT) : color(0xD9E0E8, 0x3D4858));
    target->DrawLine(D2D1::Point2F(0, 63.5f), D2D1::Point2F(size.width, 63.5f), brush.Get(), 1);
    if (state_.loading) {
        brush->SetColor(high_contrast_ ? system_color(COLOR_HIGHLIGHT) : color(0x185DCE, 0x8BB8FF));
        float start = animate_ ? static_cast<float>(GetTickCount64() % 1800) / 1800 * (size.width + 120) - 120 : 0;
        target->FillRectangle(D2D1::RectF(std::max(0.0f, start), 62, animate_ ? start + 120 : size.width, 64), brush.Get());
    }
}
void NativeToolbar::draw_item(const DRAWITEMSTRUCT& item) {
    if (!dc_target_) {
        auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), static_cast<float>(dpi_), static_cast<float>(dpi_));
        if (FAILED(factory_->CreateDCRenderTarget(&properties, &dc_target_))) return;
    }
    if (FAILED(dc_target_->BindDC(item.hDC, &item.rcItem))) return;
    auto target = dc_target_.Get();
    target->BeginDraw();
    auto base = high_contrast_ ? system_color(COLOR_WINDOW) : color(0xF3F5F7, 0x171C24);
    target->Clear(base);
    ComPtr<ID2D1SolidColorBrush> brush; target->CreateSolidColorBrush(base, &brush);
    if (!brush) { target->EndDraw(); return; }
    auto size = target->GetSize();
    auto rect = D2D1::RectF(1, 1, size.width - 1, size.height - 1);
    bool disabled = (item.itemState & ODS_DISABLED) != 0, pressed = (item.itemState & ODS_SELECTED) != 0;
    bool primary = item.CtlID == takeover_id;
    bool status = item.CtlID == status_id;
    auto ink = high_contrast_ ? system_color(disabled ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT) :
        disabled ? color(0xA4AEBC, 0x606D7D) : color(0x1C2736, 0xE7EDF5);
    if (status) {
        auto accent = color(0x58677B, 0xA7B3C3), tint = color(0xE8ECF1, 0x242D39);
        if (state_.mode == Mode::automatic) { accent = color(0x185DCE, 0x8BB8FF); tint = color(0xE8F0FE, 0x203759); }
        if (state_.mode == Mode::manual) { accent = color(0x176B4D, 0x83D4B1); tint = color(0xE6F4ED, 0x203E34); }
        if (state_.mode == Mode::paused || state_.mode == Mode::taking_over || !state_.connected || !state_.notice.empty()) {
            accent = color(0x805018, 0xF0C782); tint = color(0xFFF2DA, 0x42361F);
        }
        if (state_.mode == Mode::faulted) { accent = color(0xAD3434, 0xFFABAB); tint = color(0xFCEAEA, 0x47292C); }
        brush->SetColor(high_contrast_ ? base : tint);
        target->FillRoundedRectangle(D2D1::RoundedRect(rect, 7, 7), brush.Get());
        brush->SetColor(high_contrast_ ? ink : accent);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(10, size.height / 2), 2.5f, 2.5f), brush.Get());
        auto label = D2D1::RectF(17, 0, size.width - 5, size.height);
        target->DrawText(status_text_.c_str(), static_cast<UINT32>(status_text_.size()), text_.Get(), label, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    } else {
        if (primary || pressed || hover_id_ == static_cast<int>(item.CtlID)) {
            brush->SetColor(high_contrast_ ? base : primary ? color(0x1C2736, 0xDCE6F4) : color(0xE2E7EE, 0x303C4B));
            target->FillRoundedRectangle(D2D1::RoundedRect(rect, 7, 7), brush.Get());
        }
        if (primary && !high_contrast_) ink = disabled ? color(0x919CAA, 0x667280) : color(0xFFFFFF, 0x172231);
        brush->SetColor(ink);
        float x = size.width / 2, y = size.height / 2;
        if (item.CtlID == back_id || item.CtlID == forward_id) {
            float direction = item.CtlID == back_id ? -1.0f : 1.0f;
            line(target, brush.Get(), x - direction * 6, y, x + direction * 6, y);
            line(target, brush.Get(), x + direction * 6, y, x, y - 6);
            line(target, brush.Get(), x + direction * 6, y, x, y + 6);
        } else if (item.CtlID == reload_id) {
            if (state_.loading) target->DrawRectangle(D2D1::RectF(x - 5, y - 5, x + 5, y + 5), brush.Get(), 1.7f);
            else {
                ComPtr<ID2D1PathGeometry> path; ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(factory_->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
                    sink->BeginFigure(D2D1::Point2F(x + 5, y - 4), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + 6, y + 3), D2D1::SizeF(7, 7), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN); sink->Close();
                    target->DrawGeometry(path.Get(), brush.Get(), 1.6f);
                }
                line(target, brush.Get(), x + 5, y - 4, x, y - 4);
                line(target, brush.Get(), x + 5, y - 4, x + 5, y - 9);
            }
        } else if (primary) {
            const wchar_t* caption = state_.mode == Mode::manual || state_.mode == Mode::paused ? L"交还" : L"接管";
            target->DrawText(caption, 2, text_.Get(), rect, brush.Get());
        }
        if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
            brush->SetColor(high_contrast_ ? system_color(COLOR_HIGHLIGHT) : color(0x185DCE, 0x8BB8FF));
            target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(3, 3, size.width - 3, size.height - 3), 5, 5), brush.Get(), 1.5f);
        }
    }
    if (target->EndDraw() == D2DERR_RECREATE_TARGET) dc_target_.Reset();
}
void NativeToolbar::paint() {
    PAINTSTRUCT paint{}; BeginPaint(background_, &paint);
    if (!target_) {
        RECT bounds{}; GetClientRect(background_, &bounds);
        factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(), static_cast<float>(dpi_), static_cast<float>(dpi_)),
            D2D1::HwndRenderTargetProperties(background_, D2D1::SizeU(bounds.right, bounds.bottom)), &target_);
    }
    if (target_) {
        target_->BeginDraw(); paint_background(target_.Get());
        if (target_->EndDraw() == D2DERR_RECREATE_TARGET) target_.Reset();
    }
    EndPaint(background_, &paint);
}
LRESULT CALLBACK NativeToolbar::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto self = reinterpret_cast<NativeToolbar*>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<NativeToolbar*>(reinterpret_cast<CREATESTRUCT*>(lparam)->lpCreateParams);
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->background_ = window;
    }
    if (self) {
        if (message == WM_PAINT) { self->paint(); return 0; }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_TIMER) { InvalidateRect(window, nullptr, FALSE); return 0; }
        if (message == WM_NCDESTROY) { KillTimer(window, 1); SetWindowLongPtr(window, GWLP_USERDATA, 0); }
    }
    return DefWindowProc(window, message, wparam, lparam);
}
LRESULT CALLBACK NativeToolbar::control_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto self = reinterpret_cast<NativeToolbar*>(data);
    if (message == WM_MOUSEMOVE && window != self->address_) {
        if (self->hover_id_ != GetDlgCtrlID(window)) {
            self->hover_id_ = GetDlgCtrlID(window);
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking); self->invalidate();
        }
    }
    if (message == WM_MOUSELEAVE) { self->hover_id_ = 0; self->invalidate(); }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) self->invalidate();
    if (window == self->address_ && message == WM_IME_STARTCOMPOSITION) self->ime_composing_ = true;
    if (window == self->address_ && message == WM_IME_ENDCOMPOSITION) self->ime_composing_ = false;
    if (message == WM_KEYDOWN && window == self->address_ && !self->ime_composing_) {
        if (wparam == VK_RETURN) { self->callback_(ShellCommand::navigate); return 0; }
        if (wparam == VK_ESCAPE) {
            SetWindowText(window, self->url_.c_str()); self->callback_(ShellCommand::focus_page); return 0;
        }
    }
    if (message == WM_CHAR && window == self->address_ && !self->ime_composing_ && (wparam == VK_RETURN || wparam == VK_ESCAPE)) return 0;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, control_proc, 1);
    return DefSubclassProc(window, message, wparam, lparam);
}
}
