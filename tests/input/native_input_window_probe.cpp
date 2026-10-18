// Developer-invoked native-window observation; intentionally absent from CTest.
#include "redclaw/input/input_module.h"
#include "redclaw/input/input_recovery.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>

namespace {
using namespace redclaw::input;
using Clock = std::chrono::steady_clock;

struct Counts {
    unsigned moves = 0, down = 0, up = 0, key_down = 0, key_up = 0, characters = 0;
};
struct Probe {
    Counts marked;
    unsigned text_7 = 0;
    HWND window = nullptr;
    bool closed = false;
    bool operator_started = false;
    RecoveringInputInjectorBackend* automatic_recovery = nullptr;
    std::wstring phase = L"Baseline: keep this window foreground briefly.";
};

struct MoveResult {
    bool delivered = false;
    bool qualifying_gap = false;
    HWND foreground = nullptr;
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* probe = reinterpret_cast<Probe*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        probe = static_cast<Probe*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(probe));
    }
    // TranslateMessage's WM_CHAR need not retain the injected extra-info marker.
    // Count actual text only in this owned target, separately from marked keys.
    if (probe && message == WM_CHAR && wparam == '7') ++probe->text_7;
    if (probe && message == WM_COMMAND && LOWORD(wparam) == 1001 && HIWORD(wparam) == BN_CLICKED) {
        probe->operator_started = true;
        EnableWindow(reinterpret_cast<HWND>(lparam), FALSE);
        SetFocus(window);
        return 0;
    }
    if (probe && static_cast<std::uint32_t>(GetMessageExtraInfo()) ==
                     static_cast<std::uint32_t>(kRedClawInputExtraInfo)) {
        switch (message) {
        case WM_MOUSEMOVE: ++probe->marked.moves; break;
        case WM_LBUTTONDOWN: ++probe->marked.down; break;
        case WM_LBUTTONUP: ++probe->marked.up; break;
        case WM_KEYDOWN: if (wparam == '7') ++probe->marked.key_down; break;
        case WM_KEYUP: if (wparam == '7') ++probe->marked.key_up; break;
        case WM_CHAR: if (wparam == '7') ++probe->marked.characters; break;
        default: break;
        }
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT rect{}; GetClientRect(window, &rect);
        const std::wstring text = L"RedClaw local input probe\n\n"
            L"Baseline accepts one 7 and one click here only.\n"
            L"Observation sends bounded mouse movement only.\n"
            L"No security settings or remote input are changed.\n\n"
            L"Text received: " + std::wstring(probe && probe->text_7 ? L"7" : L"-") +
            L"\n\n" + (probe ? probe->phase : L"");
        DrawTextW(dc, text.c_str(), -1, &rect, DT_CENTER | DT_WORDBREAK);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DESTROY && probe) { probe->closed = true; return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

void pump(Probe& probe, int milliseconds) {
    const auto until = Clock::now() + std::chrono::milliseconds(milliseconds);
    while (!probe.closed && Clock::now() < until) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (probe.automatic_recovery) probe.automatic_recovery->poll(!probe.closed,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now().time_since_epoch()).count()), 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool modifiers_down() {
    return ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) |
             GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_LWIN) |
             GetAsyncKeyState(VK_RWIN) | GetAsyncKeyState(VK_LBUTTON) |
             GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0;
}

const char* foreground_kind(HWND window, HWND own, const std::set<DWORD>& watched) {
    if (window == own) return "probe";
    DWORD process = 0; GetWindowThreadProcessId(window, &process);
    return watched.contains(process) ? "watched_product" : "other";
}

long long unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void flush_line(std::ofstream& output, const std::string& line) {
    output << line << '\n'; output.flush(); std::cout << line << std::endl;
}

MoveResult move_sample(Probe& probe, IInputInjectorBackend& backend,
                 std::ofstream& output, unsigned index, const std::set<DWORD>& watched,
                 const char* phase = "observe") {
    POINT target{(index % 2) ? 150L : 330L, 165};
    ClientToScreen(probe.window, &target);
    const HWND foreground = GetForegroundWindow();
    POINT before{}; const bool before_valid = GetCursorPos(&before) != FALSE;
    const Counts counts_before = probe.marked;
    RECT before_rect{}, after_rect{}, clip{};
    const bool rect_valid = GetWindowRect(probe.window, &before_rect) != FALSE;
    const bool clip_valid = GetClipCursor(&clip) != FALSE;
    const bool clip_contains_target = clip_valid && PtInRect(&clip, target);
    const bool held_input = modifiers_down();
    const HWND point_window = WindowFromPoint(target);
    if (probe.closed || held_input || point_window != probe.window) {
        flush_line(output, "{\"stage\":\"move\",\"unix_ms\":" + std::to_string(unix_ms()) +
            ",\"index\":" + std::to_string(index) + ",\"sent\":false,\"reason\":\"target_occluded_or_user_input\""+
            ",\"held_input\":"+(held_input?"true":"false")+
            ",\"point_window_available\":"+(point_window?"true":"false")+
            ",\"point_is_own_window\":"+(point_window==probe.window?"true":"false")+
            ",\"own_window_visible\":"+(IsWindowVisible(probe.window)?"true":"false")+"}");
        return {};
    }
    InputEvent event; event.type = InputEventType::kMouseMove;
    event.x = target.x; event.y = target.y; event.absolute_coordinates = true;
    const bool accepted = backend.inject(event);
    POINT immediate{};
    const bool immediate_valid = GetCursorPos(&immediate) != FALSE;
    pump(probe, 200);
    POINT after{}; const bool after_valid = GetCursorPos(&after) != FALSE;
    const bool reached = after_valid && std::abs(after.x-target.x) <= 2 && std::abs(after.y-target.y) <= 2;
    const bool changed = before_valid && after_valid && (before.x != after.x || before.y != after.y);
    const bool foreground_same = foreground == GetForegroundWindow();
    const bool target_same = rect_valid && GetWindowRect(probe.window, &after_rect) &&
        EqualRect(&before_rect, &after_rect) && WindowFromPoint(target) == probe.window;
    const bool held_after = modifiers_down();
    const bool immediate_reached = immediate_valid && std::abs(immediate.x-target.x) <= 2 &&
        std::abs(immediate.y-target.y) <= 2;
    const bool native_delivered = probe.marked.moves > counts_before.moves;
    DWORD foreground_owner = 0;
    GetWindowThreadProcessId(foreground, &foreground_owner);
    const bool qualifying_gap = accepted && !reached && !native_delivered && foreground_same &&
        target_same && !held_after && clip_contains_target && watched.contains(foreground_owner);
    flush_line(output, "{\"stage\":\"move\",\"unix_ms\":" + std::to_string(unix_ms()) +
        ",\"phase\":\"" + phase + "\"" +
        ",\"index\":" + std::to_string(index) + ",\"sent\":true,\"api_success\":" +
        (accepted ? "true" : "false") + ",\"cursor_changed\":" + (changed ? "true" : "false") +
        ",\"cursor_reached_target\":" + (reached ? "true" : "false") +
        ",\"native_move_delta\":" + std::to_string(probe.marked.moves-counts_before.moves) +
        ",\"foreground\":\"" + foreground_kind(foreground, probe.window, watched) +
        "\",\"foreground_unchanged\":" + (foreground_same ? "true" : "false") +
        ",\"target_unchanged\":" + (target_same ? "true" : "false") +
        ",\"clip_contains_target\":" + (clip_contains_target ? "true" : "false") +
        ",\"immediate_cursor_reached\":" + (immediate_reached ? "true" : "false") +
        ",\"held_input_after\":" + (held_after ? "true" : "false") +
        ",\"qualifying_gap\":" + (qualifying_gap ? "true" : "false") + "}");
    return {accepted && reached && native_delivered && foreground_same && target_same,
        qualifying_gap, foreground};
}

bool owned_input_round(Probe& probe, IInputInjectorBackend& backend,
                      std::ofstream& output, const std::set<DWORD>& watched,
                      const char* phase, HWND allowed_foreground) {
    const Counts before = probe.marked;
    const unsigned text_before = probe.text_7;
    bool mouse_api = false, key_api = false;
    const bool focused_before = GetForegroundWindow() == allowed_foreground;
    const bool held_before = modifiers_down();
    if (!probe.closed && focused_before && !held_before) {
        const auto movement = move_sample(probe, backend, output, 0, watched, phase);
        POINT cursor{};
        if (movement.delivered && GetCursorPos(&cursor) && WindowFromPoint(cursor) == probe.window &&
            GetForegroundWindow() == allowed_foreground && !modifiers_down()) {
            InputEvent down;
            down.type = InputEventType::kMouseButtonDown; down.mouse_button = MouseButton::kLeft;
            InputEvent up = down; up.type = InputEventType::kMouseButtonUp;
            mouse_api = backend.inject_batch({down, up});
            pump(probe, 150);
            if (!probe.closed && GetForegroundWindow() == probe.window && !modifiers_down()) {
                InputEvent key;
                key.type = InputEventType::kKeyDown; key.scan_code = 0x08; key.virtual_key = '7';
                InputEvent release = key; release.type = InputEventType::kKeyUp;
                key_api = backend.inject_batch({key, release});
                pump(probe, 150);
            }
        }
    }
    const bool passed = mouse_api && key_api && probe.marked.down-before.down == 1 &&
        probe.marked.up-before.up == 1 && probe.marked.key_down-before.key_down == 1 &&
        probe.marked.key_up-before.key_up == 1 && probe.text_7-text_before == 1;
    flush_line(output, "{\"stage\":\"" + std::string(phase) + "\",\"unix_ms\":" +
        std::to_string(unix_ms()) + ",\"passed\":" + (passed ? "true" : "false") +
        ",\"foreground_gate\":" + (focused_before ? "true" : "false") +
        ",\"held_input_before\":" + (held_before ? "true" : "false") +
        ",\"mouse_api\":" + (mouse_api ? "true" : "false") +
        ",\"key_api\":" + (key_api ? "true" : "false") +
        ",\"mouse_down\":" + std::to_string(probe.marked.down-before.down) +
        ",\"mouse_up\":" + std::to_string(probe.marked.up-before.up) +
        ",\"key_down\":" + std::to_string(probe.marked.key_down-before.key_down) +
        ",\"key_up\":" + std::to_string(probe.marked.key_up-before.key_up) +
        ",\"text_7\":" + std::to_string(probe.text_7-text_before) + "}");
    InvalidateRect(probe.window, nullptr, TRUE);
    pump(probe, 100);
    return passed;
}

bool attempt_window_recovery(Probe& original, IInputInjectorBackend& backend,
                             std::ofstream& output, const std::set<DWORD>& watched,
                             HINSTANCE instance, const wchar_t* class_name, HWND failing_foreground) {
    if (original.closed || GetForegroundWindow() != failing_foreground || modifiers_down()) {
        flush_line(output, "{\"stage\":\"recovery_activation\",\"attempted\":false,\"reason\":\"context_changed\"}");
        return false;
    }
    Probe recovery;
    recovery.phase = L"Recovery candidate: normal window activation only.";
    recovery.window = CreateWindowExW(0, class_name, L"RedClaw recovery candidate",
        WS_OVERLAPPEDWINDOW, 580, 80, 520, 300, nullptr, nullptr, instance, &recovery);
    if (!recovery.window) {
        flush_line(output, "{\"stage\":\"recovery_activation\",\"attempted\":false,\"reason\":\"window_creation_failed\"}");
        return false;
    }
    // No injected shortcut, thread-input attachment, foreground-lock change or elevation.
    ShowWindow(recovery.window, SW_SHOWNORMAL);
    const bool activation_return = SetForegroundWindow(recovery.window) != FALSE;
    pump(original, 500);
    const bool activated = !recovery.closed && GetForegroundWindow() == recovery.window;
    flush_line(output, "{\"stage\":\"recovery_activation\",\"unix_ms\":" +
        std::to_string(unix_ms()) + ",\"attempted\":true,\"api_return\":" +
        (activation_return ? "true" : "false") + ",\"foreground_verified\":" +
        (activated ? "true" : "false") + ",\"same_process\":true,\"same_backend\":true}");
    bool recovered = false;
    if (activated) {
        original.phase = L"Verify recovery in the original target and injector instance.";
        recovered = owned_input_round(original, backend, output, watched,
            "recovery_original_target", recovery.window);
    }
    if (!recovery.closed) DestroyWindow(recovery.window);
    return recovered;
}
}

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path output_path, watched_path, start_signal;
    int observe_seconds = 0;
    bool recover_on_gap = false;
    bool automatic_recovery = false;
    bool manual_start = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--output" && i+1 < argc) output_path = argv[++i];
        else if (arg == L"--start-signal" && i+1 < argc) start_signal = argv[++i];
        else if (arg == L"--recover-on-delivery-gap") recover_on_gap = true;
        else if (arg == L"--automatic-recovery") automatic_recovery = true;
        else if (arg == L"--manual-start") manual_start = true;
        else if (arg == L"--watched-process-file" && i+1 < argc) watched_path = argv[++i];
        else if (arg == L"--observe-seconds" && i+1 < argc) {
            try { observe_seconds = std::stoi(argv[++i]); } catch (...) { return 2; }
        } else return 2;
    }
    if (output_path.empty() || std::filesystem::exists(output_path) || observe_seconds < 0 || observe_seconds > 300) return 2;
    if (!start_signal.empty() && std::filesystem::exists(start_signal)) return 2;
    if (manual_start && !start_signal.empty()) return 2;
    std::ofstream output(output_path); if (!output) return 2;
    std::set<DWORD> watched;
    if (!watched_path.empty()) {
        std::ifstream input(watched_path);
        DWORD value = 0; while (input >> value) watched.insert(value);
    }
    if ((recover_on_gap && automatic_recovery) ||
        ((recover_on_gap || automatic_recovery) && (observe_seconds == 0 || watched.empty()))) return 2;
    // Only this probe's GUI thread opts into physical coordinates.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HWND previous_foreground = GetForegroundWindow();
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    const HMONITOR placement_monitor = MonitorFromWindow(previous_foreground, MONITOR_DEFAULTTOPRIMARY);
    const bool monitor_available = GetMonitorInfoW(placement_monitor, &monitor) != FALSE;
    const int window_x = monitor_available ? monitor.rcWork.left + 40 : 40;
    const int window_y = monitor_available ? monitor.rcWork.top + 80 : 80;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW klass{}; klass.lpfnWndProc=window_proc; klass.hInstance=instance;
    klass.lpszClassName=L"RedClawNativeInputWindowProbe";
    klass.hCursor=LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    klass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    if (!RegisterClassW(&klass)) return 2;
    Probe probe;
    probe.window=CreateWindowExW(0, klass.lpszClassName, L"RedClaw local input probe",
        WS_OVERLAPPEDWINDOW, window_x, window_y, 520, 300, nullptr, nullptr, instance, &probe);
    if (!probe.window) return 2;
    ShowWindow(probe.window, SW_SHOW); SetForegroundWindow(probe.window); SetFocus(probe.window);
    bool preparation_ready = true;
    if (manual_start || !start_signal.empty()) {
        probe.phase = manual_start
            ? L"WAITING - no test input is running.\nClick Start local test when ready (within 30 minutes)."
            : L"WAITING - no test input is running.\nPlease confirm you can see this window in the chat.";
        HWND start_button = nullptr;
        if (manual_start) {
            SetWindowPos(probe.window, nullptr, 0, 0, 600, 380, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            start_button = CreateWindowExW(0, L"BUTTON", L"Start local test", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                195, 280, 200, 36, probe.window, reinterpret_cast<HMENU>(1001), instance, nullptr);
            if (!start_button) { DestroyWindow(probe.window); return 2; }
        }
        SetWindowPos(probe.window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(probe.window, nullptr, TRUE);
        const int preparation_seconds = manual_start ? 1800 : 300;
        flush_line(output, "{\"stage\":\"await_operator\",\"unix_ms\":" + std::to_string(unix_ms()) +
            ",\"timeout_seconds\":" + std::to_string(preparation_seconds) +
            ",\"manual_start\":" + (manual_start ? "true" : "false") +
            ",\"input_sent\":false,\"placed_on_previous_foreground_monitor\":" +
            (monitor_available ? "true" : "false") + "}");
        const auto preparation_deadline = Clock::now() + std::chrono::seconds(preparation_seconds);
        const auto start_requested = [&] {
            return manual_start ? probe.operator_started : std::filesystem::exists(start_signal);
        };
        while (!probe.closed && Clock::now() < preparation_deadline && !start_requested())
            pump(probe, 100);
        preparation_ready = !probe.closed && start_requested();
        if (start_button && IsWindow(start_button)) DestroyWindow(start_button);
        if (preparation_ready) {
            probe.phase = L"Click this test window, then release mouse and keyboard.\nBaseline starts after one quiet second.";
            SetForegroundWindow(probe.window);
            SetFocus(probe.window);
            InvalidateRect(probe.window, nullptr, TRUE);
        }
    }
    // A click gives foreground on button-down. Do not fail the baseline before
    // the operator has released that click and the owned target is stable.
    bool baseline_ready = false;
    if (preparation_ready) {
        probe.phase = L"Click this test window, then release mouse and keyboard.\nBaseline starts after one quiet second.";
        InvalidateRect(probe.window, nullptr, TRUE);
        const auto focus_deadline = Clock::now() + std::chrono::seconds(120);
        auto stable_since = Clock::now();
        while (!probe.closed && Clock::now() < focus_deadline) {
            pump(probe, 50);
            if (GetForegroundWindow() != probe.window || modifiers_down()) stable_since = Clock::now();
            else if (Clock::now() - stable_since >= std::chrono::seconds(1)) {
                baseline_ready = true;
                break;
            }
        }
    }
    flush_line(output, "{\"stage\":\"baseline_readiness\",\"unix_ms\":" + std::to_string(unix_ms()) +
        ",\"preparation_ready\":" + (preparation_ready ? "true" : "false") +
        ",\"stable_focus_and_release\":" + (baseline_ready ? "true" : "false") + "}");
    WindowsSendInputInjectorBackend native_backend;
    RecoveringInputInjectorBackend recovering_backend(native_backend);
    IInputInjectorBackend& backend = automatic_recovery ? static_cast<IInputInjectorBackend&>(recovering_backend)
                                                       : static_cast<IInputInjectorBackend&>(native_backend);
    if (automatic_recovery) {
        probe.automatic_recovery = &recovering_backend;
        pump(probe, 10);
    }
    // No diagnostic observer: this tool never queries another process's token or desktop.
    const bool baseline_pass = baseline_ready &&
        owned_input_round(probe, backend, output, watched, "baseline", probe.window);
    bool gap_observed = false, recovery_attempted = false, recovery_pass = false;
    if (baseline_pass && observe_seconds) {
        if (recover_on_gap || automatic_recovery) {
            // Keep only this harmless owned target unobscured, without changing foreground.
            SetWindowPos(probe.window, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        probe.phase = L"Now activate PC Manager, then leave mouse/keyboard untouched for 25 seconds.";
        InvalidateRect(probe.window, nullptr, TRUE);
        flush_line(output,"{\"stage\":\"ready\",\"unix_ms\":"+std::to_string(unix_ms())+
            ",\"observe_seconds\":"+std::to_string(observe_seconds)+",\"interval_seconds\":"+
            std::to_string(automatic_recovery ? 1 : 5)+"}");
        const auto deadline=Clock::now()+std::chrono::seconds(observe_seconds);
        unsigned index=1;
        unsigned consecutive_gaps = 0;
        HWND gap_foreground = nullptr;
        while (!probe.closed && Clock::now()<deadline) {
            pump(probe,automatic_recovery ? 1000 : 5000);
            if (probe.closed || Clock::now() >= deadline) break;
            const auto sample = move_sample(probe, backend, output, index++, watched);
            if (automatic_recovery && recovering_backend.stats().activation_attempts) {
                const auto& stats = recovering_backend.stats();
                gap_observed = stats.stationary_samples >= 3;
                recovery_attempted = true;
                const HWND recovery_foreground = GetForegroundWindow();
                DWORD recovery_owner = 0;
                GetWindowThreadProcessId(recovery_foreground, &recovery_owner);
                const bool own_recovery_foreground = stats.foreground_activations == 1 &&
                    recovery_foreground != probe.window && recovery_owner == GetCurrentProcessId();
                flush_line(output, "{\"stage\":\"automatic_recovery\",\"unix_ms\":" + std::to_string(unix_ms()) +
                    ",\"stationary_samples\":" + std::to_string(stats.stationary_samples) +
                    ",\"attempts\":" + std::to_string(stats.activation_attempts) +
                    ",\"activations\":" + std::to_string(stats.foreground_activations) +
                    ",\"own_foreground_verified\":" + (own_recovery_foreground ? "true" : "false") + "}");
                if (own_recovery_foreground) recovery_pass = owned_input_round(probe, backend, output, watched,
                    "recovery_original_target", recovery_foreground);
                break;
            }
            if (sample.qualifying_gap) {
                consecutive_gaps = gap_foreground == sample.foreground ? consecutive_gaps+1 : 1;
                gap_foreground = sample.foreground;
            } else { consecutive_gaps = 0; gap_foreground = nullptr; }
            if (recover_on_gap && consecutive_gaps >= 3) {
                gap_observed = true;
                recovery_attempted = true;
                recovery_pass = attempt_window_recovery(probe, backend, output, watched,
                    instance, klass.lpszClassName, gap_foreground);
                break;
            }
        }
    }
    if (recover_on_gap || automatic_recovery) {
        flush_line(output, "{\"stage\":\"recovery_result\",\"failure_observed\":" +
            std::string(gap_observed ? "true" : "false") + ",\"attempted\":" +
            (recovery_attempted ? "true" : "false") + ",\"passed\":" +
            (recovery_pass ? "true" : "false") + ",\"same_original_target\":true,\"same_backend\":true}");
    }
    const bool restore_requested=!probe.closed && GetForegroundWindow()==probe.window;
    recovering_backend.poll(false, 0, 1);
    probe.automatic_recovery = nullptr;
    if (!probe.closed) DestroyWindow(probe.window);
    bool restored=false;
    if (restore_requested && IsWindow(previous_foreground)) {
        restored=SetForegroundWindow(previous_foreground)!=FALSE;
    }
    flush_line(output,"{\"stage\":\"cleanup\",\"own_window_closed\":true,\"focus_restore_requested\":"+
        std::string(restore_requested?"true":"false")+",\"focus_restored\":"+(restored?"true":"false")+"}");
    return !baseline_pass ? 3 : ((recover_on_gap || automatic_recovery) && !recovery_pass ? 4 : 0);
}
#else
int main() { return 2; }
#endif
