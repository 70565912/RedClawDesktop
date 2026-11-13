#include "redclaw/input/input_recovery.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
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
constexpr int kClosePromptId = 1;
constexpr int kCloseTargetId = 2;
constexpr int kPromptWidth = 480;
constexpr int kPromptHeight = 316;
constexpr std::uint64_t kCloseObservationMs = 2000;
// This window is pumped by the Host runtime. A system menu, caption drag or
// caption button could enter a modal loop while the next remote event is queued.
constexpr DWORD kRecoveryStyle = WS_POPUP | WS_BORDER | WS_CLIPCHILDREN;
constexpr DWORD kRecoveryExStyle = WS_EX_TOOLWINDOW;

enum class TargetCloseState {
    kUnavailable, kReady, kRequested, kPending, kClosed, kDenied, kFailed, kUnconfirmed,
};

struct RecoveryPrompt {
    HWND window = nullptr;
    HWND target = nullptr;
    DWORD target_thread = 0;
    DWORD target_process = 0;
    wchar_t title[128]{};
    TargetCloseState close_state = TargetCloseState::kUnavailable;
    std::uint64_t close_requested_ms = 0;

    [[nodiscard]] bool target_matches() const {
        DWORD process = 0;
        return target_thread && GetWindowThreadProcessId(target, &process) == target_thread &&
            process == target_process;
    }

    void set_close_state(TargetCloseState state) {
        close_state = state;
        InvalidateRect(window, nullptr, FALSE);
    }

    void update_target_close(std::uint64_t now_ms) {
        if (close_state == TargetCloseState::kRequested) {
            if (!target_matches()) {
                set_close_state(TargetCloseState::kClosed);
            } else if (!IsWindowVisible(target) || GetAncestor(target, GA_ROOT) != target) {
                set_close_state(TargetCloseState::kUnavailable);
            } else if (PostMessageW(target, WM_CLOSE, 0, 0)) {
                close_requested_ms = now_ms;
                set_close_state(TargetCloseState::kPending);
            } else {
                set_close_state(GetLastError() == ERROR_ACCESS_DENIED ?
                    TargetCloseState::kDenied : TargetCloseState::kFailed);
            }
        } else if (close_state == TargetCloseState::kPending) {
            if (!target_matches()) set_close_state(TargetCloseState::kClosed);
            else if (now_ms - close_requested_ms >= kCloseObservationMs)
                set_close_state(TargetCloseState::kUnconfirmed);
        }
    }
};

const wchar_t* target_close_text(TargetCloseState state) {
    switch (state) {
    case TargetCloseState::kUnavailable: return L"\u539f\u7a97\u53e3\u4e0d\u53ef\u5173\u95ed\u3002";
    case TargetCloseState::kReady: return L"";
    case TargetCloseState::kRequested:
    case TargetCloseState::kPending: return L"\u5df2\u8bf7\u6c42\u5173\u95ed\uff0c\u7b49\u5f85\u539f\u7a97\u53e3\u54cd\u5e94\u3002";
    case TargetCloseState::kClosed: return L"\u539f\u7a97\u53e3\u5df2\u5173\u95ed\u3002";
    case TargetCloseState::kDenied: return L"Windows \u62d2\u7edd\u5173\u95ed\u539f\u7a97\u53e3\u3002";
    case TargetCloseState::kFailed: return L"\u65e0\u6cd5\u53d1\u9001\u5173\u95ed\u8bf7\u6c42\u3002";
    case TargetCloseState::kUnconfirmed: return L"\u672a\u786e\u8ba4\u539f\u7a97\u53e3\u5173\u95ed\u3002";
    }
    return L"";
}

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

void paint_recovery_content(HDC dc, const RECT& client, UINT dpi, const RecoveryPrompt& prompt) {
    const auto px = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const int saved = SaveDC(dc);
    SetBkMode(dc, TRANSPARENT);
    SetDCBrushColor(dc, kRecoveryBackground);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    draw_recovery_text(dc, {px(24), px(20), client.right - px(120), px(54)},
        L"RedClaw \u8f93\u5165\u6062\u590d\u63d0\u793a", dpi, 14, FW_SEMIBOLD, kRecoveryText);

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
    draw_recovery_text(dc, {px(24), px(178), client.right - px(24), px(204)},
        prompt.title, dpi, 10, FW_SEMIBOLD, kRecoveryText);
    draw_recovery_text(dc, {px(24), px(207), client.right - px(24), px(263)},
        L"\u8bf7\u79fb\u52a8\u9f20\u6807\uff0c\u5207\u6362\u5230\u5176\u4ed6\u7a97\u53e3\u7ee7\u7eed\u64cd\u4f5c\u3002\n"
        L"\u5207\u6362\u7a97\u53e3\u540e\uff0c\u6b64\u63d0\u793a\u4f1a\u81ea\u52a8\u5173\u95ed\u3002",
        dpi, 10, FW_NORMAL, kRecoveryMuted);
    draw_recovery_text(dc, {px(24), px(278), client.right - px(160), client.bottom - px(10)},
        target_close_text(prompt.close_state), dpi, 10, FW_NORMAL, kRecoveryAccent);
    if (saved) RestoreDC(dc, saved);
}

LRESULT CALLBACK recovery_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
            reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    }
    auto* prompt = reinterpret_cast<RecoveryPrompt*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCDESTROY) {
        if (prompt && prompt->window == window) prompt->window = nullptr;
    }
    if (message == WM_CREATE) {
        const auto& create = *reinterpret_cast<const CREATESTRUCTW*>(lparam);
        const auto button = [&](int id, const wchar_t* label) {
            return CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                create.hInstance, nullptr);
        };
        const HWND close_target = button(kCloseTargetId, L"\u5173\u95ed\u539f\u7a97\u53e3");
        if (!close_target || !button(kClosePromptId, L"\u5173\u95ed\u63d0\u793a")) return -1;
        EnableWindow(close_target, prompt->close_state == TargetCloseState::kReady);
        return 0;
    }
    if (message == WM_SIZE) {
        const UINT dpi = GetDpiForWindow(window);
        const auto px = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        RECT client{};
        GetClientRect(window, &client);
        SetWindowPos(GetDlgItem(window, kClosePromptId), nullptr,
            client.right - px(104), px(16), px(80), px(30), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(window, kCloseTargetId), nullptr,
            client.right - px(152), px(268), px(128), px(32), SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (message == WM_SYSCOMMAND) {
        const auto command = wparam & 0xfff0;
        if (command == SC_KEYMENU || command == SC_MOUSEMENU || command == SC_MOVE || command == SC_SIZE)
            return 0;
    }
    if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED) {
        const auto id = LOWORD(wparam);
        const auto button = reinterpret_cast<HWND>(lparam);
        if (id == kClosePromptId && button && button == GetDlgItem(window, id)) {
            DestroyWindow(window);
            return 0;
        }
        if (id == kCloseTargetId && button && button == GetDlgItem(window, id) &&
            prompt->close_state == TargetCloseState::kReady) {
            prompt->set_close_state(TargetCloseState::kRequested);
            EnableWindow(button, FALSE);
            return 0;
        }
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
        paint_recovery_content(dc, client, GetDpiForWindow(window), *prompt);
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
        result.recovery_foreground = prompt_.window && foreground == prompt_.window;
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
        const HWND target_window = reinterpret_cast<HWND>(expected_foreground);
        prompt_.target = target_window;
        prompt_.target_thread = GetWindowThreadProcessId(target_window, &prompt_.target_process);
        if (prompt_.target_thread && prompt_.target_process != GetCurrentProcessId() &&
            target_window != GetShellWindow() && target_window != GetDesktopWindow() &&
            IsWindowVisible(target_window) && GetAncestor(target_window, GA_ROOT) == target_window) {
            GetWindowTextW(target_window, prompt_.title, static_cast<int>(std::size(prompt_.title)));
            prompt_.close_state = TargetCloseState::kReady;
        }
        const int width = std::min(static_cast<LONG>(kPromptWidth), monitor.rcWork.right - monitor.rcWork.left);
        auto& window = prompt_.window;
        window = CreateWindowExW(kRecoveryExStyle, class_name, L"RedClaw \u8f93\u5165\u6062\u590d",
            kRecoveryStyle, monitor.rcWork.right - width - 12,
            monitor.rcWork.top + 12, width, kPromptHeight, nullptr, nullptr, instance, &prompt_);
        if (!window) { close_window(); return false; }
        const UINT dpi = GetDpiForWindow(window);
        RECT bounds{0, 0, MulDiv(kPromptWidth, static_cast<int>(dpi), 96),
            MulDiv(kPromptHeight, static_cast<int>(dpi), 96)};
        AdjustWindowRectExForDpi(&bounds, kRecoveryStyle, FALSE, kRecoveryExStyle, dpi);
        const int margin = MulDiv(12, static_cast<int>(dpi), 96);
        const int outer_width = std::min(bounds.right - bounds.left, monitor.rcWork.right - monitor.rcWork.left);
        const int outer_height = std::min(bounds.bottom - bounds.top, monitor.rcWork.bottom - monitor.rcWork.top);
        SetWindowPos(window, nullptr, std::max(monitor.rcWork.left, monitor.rcWork.right - outer_width - margin),
            std::min(monitor.rcWork.top + margin, monitor.rcWork.bottom - outer_height), outer_width, outer_height,
            SWP_NOZORDER | SWP_NOACTIVATE);
        // No synthetic Alt, AttachThreadInput, lock timeout changes or escalation.
        if (GetForegroundWindow() != target_window) { close_window(); return false; }
        ShowWindow(window, SW_SHOWNORMAL);
        (void)SetForegroundWindow(window);
        if (GetForegroundWindow() != window) { close_window(); return false; }
        UpdateWindow(window);
        return true;
    }

    InputRecoveryWindowClose pump_window(std::uint64_t now_ms) override {
        auto& window = prompt_.window;
        if (!window) return InputRecoveryWindowClose::kNone;
        if (!IsWindow(window)) { prompt_ = {}; return InputRecoveryWindowClose::kUser; }
        auto reason = InputRecoveryWindowClose::kNone;
        if (!ordinary_input_desktop()) reason = InputRecoveryWindowClose::kDesktopUnavailable;
        else if (GetForegroundWindow() != window) reason = InputRecoveryWindowClose::kForegroundLost;
        // A timed close while we still own foreground can reactivate the same
        // blocking application. Let the user choose the next foreground instead.
        if (reason != InputRecoveryWindowClose::kNone) {
            close_window(); return reason;
        }
        MSG message{};
        // The filter includes the buttons and excludes the caller's other UI.
        for (unsigned count = 0; count < 32 && window &&
            PeekMessageW(&message, window, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!window) { prompt_ = {}; return InputRecoveryWindowClose::kUser; }
        if (prompt_.close_state == TargetCloseState::kRequested &&
            (!ordinary_input_desktop() || GetForegroundWindow() != window)) {
            prompt_.set_close_state(TargetCloseState::kUnavailable);
        }
        prompt_.update_target_close(now_ms);
        return InputRecoveryWindowClose::kNone;
    }

    void close_window() override {
        if (prompt_.window && IsWindow(prompt_.window)) DestroyWindow(prompt_.window);
        prompt_ = {};
    }
private:
    RecoveryPrompt prompt_;
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
