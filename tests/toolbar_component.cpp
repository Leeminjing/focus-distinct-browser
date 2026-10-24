// Uses a process-local HKCU mapping, so theme tests never change the user's
// Windows theme. This is the production toolbar module in a small native host.
#include "../src/window/native_toolbar.h"
#include <memory>
#include <string>
using namespace distinct;
static std::unique_ptr<NativeToolbar> toolbar;
static HKEY preferences = nullptr;
static ShellState state;
static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    LRESULT result = 0;
    if (toolbar && toolbar->parent_message(message, wparam, lparam, result)) return result;
    if (message == WM_SIZE && toolbar) { toolbar->layout(); return 0; }
    if (message == WM_APP + 100 && toolbar) {
        DWORD light = wparam == 0 ? 1 : 0;
        RegSetValueEx(preferences, L"AppsUseLightTheme", 0, REG_DWORD, reinterpret_cast<BYTE*>(&light), sizeof(light));
        state.mode = wparam == 3 ? Mode::faulted : wparam == 2 ? Mode::manual : Mode::automatic;
        toolbar->theme_changed(); toolbar->update(state);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        return GetDpiForWindow(window);
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(window, message, wparam, lparam);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int) {
    if (std::wstring(arguments).find(L"--unaware") != std::wstring::npos)
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    GUID id{}; CoCreateGuid(&id); wchar_t guid[40]{}; StringFromGUID2(id, guid, 40);
    std::wstring key = std::wstring(L"Software\\DistinctBrowser\\ToolbarTests\\") + guid;
    HKEY sandbox = nullptr;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &sandbox, nullptr) != ERROR_SUCCESS)
        return 1;
    if (RegCreateKeyEx(sandbox, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, nullptr,
        REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &preferences, nullptr) != ERROR_SUCCESS) return 2;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, sandbox) != ERROR_SUCCESS) return 3;
    WNDCLASS type{}; type.lpszClassName = L"DistinctToolbarComponentTest"; type.hInstance = instance;
    type.lpfnWndProc = WindowProc; type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClass(&type);
    HWND window = CreateWindowEx(0, type.lpszClassName, L"Native toolbar component", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 220, nullptr, nullptr, instance, nullptr);
    toolbar = std::make_unique<NativeToolbar>();
    HRESULT hr = toolbar->create(window, [](ShellCommand) {});
    if (FAILED(hr)) return 4;
    state.ready = true; state.loading = false; state.can_back = true; state.can_forward = true;
    toolbar->set_url(L"https://example.org/native-toolbar");
    toolbar->update(state); toolbar->layout();
    ShowWindow(window, SW_SHOWNOACTIVATE);
    std::string ready = std::to_string(reinterpret_cast<uintptr_t>(window)) + "\n";
    DWORD written = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), ready.data(), static_cast<DWORD>(ready.size()), &written, nullptr);
    MSG message{};
    while (GetMessage(&message, nullptr, 0, 0) > 0) {
        if (!toolbar->pre_translate(message)) { TranslateMessage(&message); DispatchMessage(&message); }
    }
    toolbar.reset();
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(preferences); RegCloseKey(sandbox);
    RegDeleteTree(HKEY_CURRENT_USER, key.c_str());
    return 0;
}
