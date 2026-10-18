#include "redclaw/input/input_recovery.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dwmapi.h>
#include <algorithm>

namespace redclaw::input {
namespace {
class PhysicalCoordinates {
public:
    PhysicalCoordinates() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalCoordinates() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
private:
    DPI_AWARENESS_CONTEXT previous_;
};

bool ordinary_input_desktop() {
    const HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) return false;
    wchar_t input_name[128]{}, thread_name[128]{};
    DWORD required = 0;
    USEROBJECTFLAGS station{};
    const bool ordinary = GetUserObjectInformationW(input, UOI_NAME, input_name, sizeof(input_name), &required) &&
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
            thread_name, sizeof(thread_name), &required) &&
        GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &station, sizeof(station), &required) &&
        (station.dwFlags & WSF_VISIBLE) && lstrcmpiW(input_name, L"Default") == 0 &&
        lstrcmpiW(input_name, thread_name) == 0;
    CloseDesktop(input);
    return ordinary;
}

bool held_key_or_button() {
    // Full keyboard scan only at activation, never in the normal mouse path.
    for (int key = 1; key < 256; ++key) if (GetAsyncKeyState(key) & 0x8000) return true;
    return false;
}

bool held_drag_or_modifier() {
    for (const int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2,
                         VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key) & 0x8000) return true;
    return false;
}

// Match the GUI shell palette without making the runtime depend on Qt.
constexpr COLORREF kRecoveryBackground = RGB(8, 17, 31);
constexpr COLORREF kRecoveryCard = RGB(15, 23, 42);
constexpr COLORREF kRecoveryBorder = RGB(36, 50, 68);
constexpr COLORREF kRecoveryText = RGB(226, 232, 240);
constexpr COLORREF kRecoveryMuted = RGB(148, 163, 184);
constexpr COLORREF kRecoveryAccent = RGB(125, 211, 252);

void draw_recovery_text(HDC dc, RECT rect, const wchar_t* text, UINT dpi,
                        int points, int weight, COLORREF color) {
    const HFONT font = CreateFontW(-MulDiv(points, static_cast<int>(dpi), 72), 0, 0, 0, weight,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const auto previous = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, previous);
    if (font) DeleteObject(font);
}

void paint_recovery_content(HDC dc, const RECT& client, UINT dpi) {
    const auto px = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const int saved = SaveDC(dc);
    SetBkMode(dc, TRANSPARENT);
    SetDCBrushColor(dc, kRecoveryBackground);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    draw_recovery_text(dc, {px(24), px(20), client.right - px(24), px(54)},
        L"\u8f93\u5165\u6062\u590d\u63d0\u793a", dpi, 14, FW_SEMIBOLD, kRecoveryText);

    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, kRecoveryCard);
    SetDCPenColor(dc, kRecoveryBorder);
    RoundRect(dc, px(24), px(66), client.right - px(24), px(166), px(16), px(16));
    draw_recovery_text(dc, {px(40), px(82), client.right - px(40), px(126)},
        L"\u5f53\u524d\u6fc0\u6d3b\u7a97\u53e3\u7981\u6b62\u952e\u76d8\u9f20\u6807\u6d88\u606f\u6ce8\u5165\uff0c",
        dpi, 10, FW_NORMAL, kRecoveryText);
    draw_recovery_text(dc, {px(40), px(128), client.right - px(40), px(154)},
        L"\u8bf7\u5173\u95ed\u6b64\u8f6f\u4ef6\u7a97\u53e3\u3002", dpi, 11, FW_SEMIBOLD, kRecoveryAccent);
    draw_recovery_text(dc, {px(24), px(186), client.right - px(24), client.bottom - px(20)},
        L"\u8bf7\u79fb\u52a8\u9f20\u6807\uff0c\u5207\u6362\u5230\u5176\u4ed6\u7a97\u53e3\u7ee7\u7eed\u64cd\u4f5c\u3002\n"
        L"\u5207\u6362\u7a97\u53e3\u540e\uff0c\u6b64\u63d0\u793a\u4f1a\u81ea\u52a8\u5173\u95ed\u3002",
        dpi, 10, FW_NORMAL, kRecoveryMuted);
    if (saved) RestoreDC(dc, saved);
}

LRESULT CALLBACK recovery_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
            reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    }
    if (message == WM_NCDESTROY) {
        auto* owned_window = reinterpret_cast<HWND*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (owned_window && *owned_window == window) *owned_window = nullptr;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_DPICHANGED) {
        const auto& rect = *reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT client{}; GetClientRect(window, &client);
        paint_recovery_content(dc, client, GetDpiForWindow(window));
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

class WindowsInputRecoveryPlatform final : public IInputRecoveryPlatform {
public:
    ~WindowsInputRecoveryPlatform() override { close_window(); }

    InputRecoveryObservation observe(DesktopPoint target) override {
        InputRecoveryObservation result;
        const PhysicalCoordinates physical;
        if (!ordinary_input_desktop()) return result;
        result.held_input = held_drag_or_modifier();
        if (result.held_input) return result;
        POINT cursor{}; RECT clip{};
        const POINT point{target.x, target.y};
        if (!GetCursorPos(&cursor) || !GetClipCursor(&clip) || !PtInRect(&clip, point) ||
            !MonitorFromPoint(point, MONITOR_DEFAULTTONULL)) return result;
        const HWND foreground = GetForegroundWindow();
        const HWND target_window = WindowFromPoint(point);
        if (!foreground || !target_window || !IsWindowVisible(target_window)) return result;
        result.valid = true;
        result.cursor = {cursor.x, cursor.y};
        result.foreground = reinterpret_cast<std::uintptr_t>(foreground);
        result.target_window = reinterpret_cast<std::uintptr_t>(target_window);
        result.recovery_foreground = window_ && foreground == window_;
        return result;
    }

    bool activate_window(std::uintptr_t expected_foreground, DesktopPoint target, std::uint64_t) override {
        const auto check = observe(target);
        if (!check.valid || check.held_input || check.foreground != expected_foreground || check.recovery_foreground ||
            held_key_or_button())
            return false;
        close_window();
        const PhysicalCoordinates physical;
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        constexpr auto class_name = L"RedClawOrdinaryInputRecovery";
        WNDCLASSW klass{};
        klass.lpfnWndProc = recovery_window_proc;
        klass.hInstance = instance;
        klass.lpszClassName = class_name;
        klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (!RegisterClassW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromPoint({target.x, target.y}, MONITOR_DEFAULTTONULL), &monitor)) return false;
        const int width = std::min(480L, monitor.rcWork.right - monitor.rcWork.left);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"RedClaw \u8f93\u5165\u6062\u590d",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, monitor.rcWork.right - width - 12,
            monitor.rcWork.top + 12, width, 252, nullptr, nullptr, instance, &window_);
        if (!window_) return false;
        const UINT dpi = GetDpiForWindow(window_);
        RECT bounds{0, 0, MulDiv(480, static_cast<int>(dpi), 96), MulDiv(252, static_cast<int>(dpi), 96)};
        AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW, dpi);
        const int margin = MulDiv(12, static_cast<int>(dpi), 96);
        const int outer_width = std::min(bounds.right - bounds.left, monitor.rcWork.right - monitor.rcWork.left);
        const int outer_height = std::min(bounds.bottom - bounds.top, monitor.rcWork.bottom - monitor.rcWork.top);
        SetWindowPos(window_, nullptr, std::max(monitor.rcWork.left, monitor.rcWork.right - outer_width - margin),
            std::min(monitor.rcWork.top + margin, monitor.rcWork.bottom - outer_height), outer_width, outer_height,
            SWP_NOZORDER | SWP_NOACTIVATE);
        const BOOL dark = TRUE;
        (void)DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        (void)DwmSetWindowAttribute(window_, DWMWA_CAPTION_COLOR, &kRecoveryBackground, sizeof(kRecoveryBackground));
        (void)DwmSetWindowAttribute(window_, DWMWA_TEXT_COLOR, &kRecoveryText, sizeof(kRecoveryText));
        (void)DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &kRecoveryBorder, sizeof(kRecoveryBorder));
        // No synthetic Alt, AttachThreadInput, lock timeout changes or escalation.
        ShowWindow(window_, SW_SHOWNORMAL);
        (void)SetForegroundWindow(window_);
        if (GetForegroundWindow() != window_) { close_window(); return false; }
        UpdateWindow(window_);
        return true;
    }

    InputRecoveryWindowClose pump_window(std::uint64_t) override {
        if (!window_) return InputRecoveryWindowClose::kNone;
        if (!IsWindow(window_)) { window_ = nullptr; return InputRecoveryWindowClose::kUser; }
        auto reason = InputRecoveryWindowClose::kNone;
        if (!ordinary_input_desktop()) reason = InputRecoveryWindowClose::kDesktopUnavailable;
        else if (GetForegroundWindow() != window_) reason = InputRecoveryWindowClose::kForegroundLost;
        // A timed close while we still own foreground can reactivate the same
        // blocking application. Let the user choose the next foreground instead.
        if (reason != InputRecoveryWindowClose::kNone) {
            close_window(); return reason;
        }
        MSG message{};
        // Only this owned window's queue; never consume the caller's other UI.
        for (unsigned count = 0; count < 32 && window_ &&
            PeekMessageW(&message, window_, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return window_ ? InputRecoveryWindowClose::kNone : InputRecoveryWindowClose::kUser;
    }

    void close_window() override {
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        window_ = nullptr;
    }
private:
    HWND window_ = nullptr;
};
} // namespace

std::unique_ptr<IInputRecoveryPlatform> make_windows_input_recovery_platform() {
    return std::make_unique<WindowsInputRecoveryPlatform>();
}
} // namespace redclaw::input
#else
namespace redclaw::input {
std::unique_ptr<IInputRecoveryPlatform> make_windows_input_recovery_platform() { return {}; }
} // namespace redclaw::input
#endif
