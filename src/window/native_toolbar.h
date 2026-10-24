#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl.h>
#include <functional>
#include <string>
#include "../control.h"

namespace distinct {
enum class ShellCommand { takeover, address_focus, navigate, back, forward, reload_stop, reload, focus_page, focus_page_next, focus_page_previous };
struct ShellState {
    Mode mode = Mode::available;
    bool ready = false, connected = true, busy = false, loading = true;
    bool can_back = false, can_forward = false;
    std::wstring notice;
};

// Owns the native shell, never the page or its control lease. All user intent
// goes through the owner callback before any WebView2 navigation is attempted.
class NativeToolbar {
public:
    static constexpr int height_dip = 64;
    static constexpr int takeover_id = 101, address_id = 102;
    static constexpr int back_id = 104, forward_id = 105, reload_id = 106, status_id = 107;
    NativeToolbar() = default;
    ~NativeToolbar();
    NativeToolbar(const NativeToolbar&) = delete;
    NativeToolbar& operator=(const NativeToolbar&) = delete;
    HRESULT create(HWND parent, std::function<void(ShellCommand)> callback);
    void layout();
    void update(const ShellState& state);
    void set_url(const std::wstring& url);
    std::wstring input_url() const;
    void focus_address();
    void focus_from_page(bool previous);
    void theme_changed();
    bool shortcut(UINT key, bool ctrl, bool alt);
    bool page_shortcut(UINT key, bool ctrl, bool alt) const;
    bool pre_translate(const MSG& message);
    bool parent_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);
    int height_pixels() const;
    HWND address() const { return address_; }
private:
    HWND parent_ = nullptr, background_ = nullptr, address_ = nullptr, status_ = nullptr;
    HWND back_ = nullptr, forward_ = nullptr, reload_ = nullptr, takeover_ = nullptr, tooltip_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    std::function<void(ShellCommand)> callback_;
    ShellState state_;
    std::wstring url_ = L"about:blank", status_text_;
    RECT address_box_{};
    UINT dpi_ = 96;
    int hover_id_ = 0;
    bool dark_ = false, high_contrast_ = false, animate_ = true, compact_ = false, setting_text_ = false, ime_composing_ = false;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> write_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dc_target_;
    int px(int dip) const;
    COLORREF edit_color() const;
    D2D1_COLOR_F color(unsigned light, unsigned dark) const;
    void paint_background(ID2D1RenderTarget* target);
    void draw_item(const DRAWITEMSTRUCT& item);
    void paint();
    void invalidate();
    void apply_state();
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK control_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
};
}
