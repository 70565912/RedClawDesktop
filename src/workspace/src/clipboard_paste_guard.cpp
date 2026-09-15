#include "redclaw/workspace/clipboard_paste_guard.h"
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace redclaw::workspace {
namespace {
bool valid(const ClipboardFocusToken& value) {
    return value.foreground && value.focus && value.process && value.thread && value.process_created;
}
}
bool ClipboardPasteGuard::begin(std::string epoch, std::string operation, ClipboardFocusToken focus,
    std::uint64_t revision, bool eligible) {
    if (expected_ || epoch.empty() || operation.empty() || !eligible || !valid(focus)) return false;
    expected_ = Expected{std::move(epoch), std::move(operation), focus, revision, false}; return true;
}
bool ClipboardPasteGuard::verified(std::string_view epoch, std::string_view operation) {
    if (!expected_ || expected_->epoch != epoch || expected_->operation != operation) return false;
    expected_->verified = true; return true;
}
ClipboardPasteDecision ClipboardPasteGuard::take(std::string_view epoch, std::string_view operation,
    const std::optional<ClipboardFocusToken>& focus, std::uint64_t revision, bool eligible) {
    if (!expected_) return ClipboardPasteDecision::kNoPendingPaste;
    if (expected_->epoch != epoch || expected_->operation != operation) return ClipboardPasteDecision::kStaleOperation;
    const auto expected = std::exchange(expected_, {});
    if (!eligible || revision != expected->input_revision) return ClipboardPasteDecision::kInputRevoked;
    if (!focus || *focus != expected->focus) return ClipboardPasteDecision::kFocusChanged;
    if (!expected->verified) return ClipboardPasteDecision::kDataNotVerified;
    // Consume before clipboard publication/input submission. A failed OS call
    // is reported and never retried as an automatic second paste.
    return ClipboardPasteDecision::kSubmitOnce;
}
void ClipboardPasteGuard::cancel() { expected_.reset(); }

struct ClipboardFocusTracker::Impl {
#ifdef _WIN32
    DWORD owner_thread = GetCurrentThreadId();
    HWINEVENTHOOK foreground_hook = nullptr, focus_hook = nullptr, destroy_hook = nullptr, desktop_hook = nullptr;
    std::uint64_t generation = 0, initial_generation = 0;
    HWND observed_foreground = nullptr, observed_focus = nullptr;
    DWORD observed_thread = 0;
    bool uncertain = false;
    static thread_local Impl* active;
    static void CALLBACK event(HWINEVENTHOOK, DWORD event_id, HWND window, LONG, LONG, DWORD thread, DWORD) {
        if (!active) return;
        if (event_id == EVENT_SYSTEM_FOREGROUND || event_id == EVENT_SYSTEM_DESKTOPSWITCH
            || (event_id == EVENT_OBJECT_FOCUS && thread == active->observed_thread)
            || (event_id == EVENT_OBJECT_DESTROY && (window == active->observed_foreground || window == active->observed_focus))) {
            if (++active->generation == 0) active->uncertain = true;
        }
    }
    Impl() {
        if (active) { uncertain = true; return; }
        active = this;
        // OUTOFCONTEXT keeps all callbacks on this owner thread and injects no
        // code into another application. Do not skip our own process in tests.
        foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, event, 0, 0, WINEVENT_OUTOFCONTEXT);
        focus_hook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, event, 0, 0, WINEVENT_OUTOFCONTEXT);
        destroy_hook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr, event, 0, 0, WINEVENT_OUTOFCONTEXT);
        desktop_hook = SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH, nullptr, event, 0, 0, WINEVENT_OUTOFCONTEXT);
        uncertain = !foreground_hook || !focus_hook || !destroy_hook || !desktop_hook;
    }
    ~Impl() {
        for (auto hook : {foreground_hook, focus_hook, destroy_hook, desktop_hook}) if (hook) UnhookWinEvent(hook);
        if (active == this) active = nullptr;
    }
    bool drain() {
        if (GetCurrentThreadId() != owner_thread || uncertain) return false;
        MSG message{};
        // A notification flood is ambiguous, so fail closed instead of making
        // the capture/runtime owner wait for an unbounded message drain.
        for (unsigned count = 0; count < 256; ++count) {
            if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) return true;
            if (message.message == WM_QUIT) { uncertain = true; return false; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        uncertain = true; return false;
    }
#endif
};
#ifdef _WIN32
thread_local ClipboardFocusTracker::Impl* ClipboardFocusTracker::Impl::active = nullptr;
#endif
ClipboardFocusTracker::ClipboardFocusTracker() : impl_(std::make_unique<Impl>()) {}
ClipboardFocusTracker::~ClipboardFocusTracker() = default;
bool ClipboardFocusTracker::pump_events() {
#ifdef _WIN32
    return impl_->drain() && (!impl_->observed_foreground
        || (impl_->generation == impl_->initial_generation && GetForegroundWindow() == impl_->observed_foreground));
#else
    return false;
#endif
}
std::optional<ClipboardFocusToken> ClipboardFocusTracker::snapshot(std::string* error) {
    const auto reject = [&](const char* code) -> std::optional<ClipboardFocusToken> { if (error) *error = code; return {}; };
#ifdef _WIN32
    if (!impl_->drain()) return reject("clipboard_focus_tracking_unavailable");
    const auto foreground = GetForegroundWindow();
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(foreground, &process);
    GUITHREADINFO information{}; information.cbSize = sizeof(information);
    if (!foreground || !thread || !GetGUIThreadInfo(thread, &information) || !information.hwndFocus
        || information.hwndActive != foreground || information.flags & (GUI_INMENUMODE | GUI_INMOVESIZE))
        return reject("clipboard_focus_unavailable");
    HANDLE owner = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (!owner) return reject("clipboard_target_identity_unavailable");
    FILETIME created{}, exited{}, kernel{}, user{};
    DWORD session = 0, local_session = 0;
    const bool identity = GetProcessTimes(owner, &created, &exited, &kernel, &user)
        && ProcessIdToSessionId(process, &session) && ProcessIdToSessionId(GetCurrentProcessId(), &local_session);
    CloseHandle(owner);
    if (!identity || session != local_session) return reject("clipboard_target_identity_unavailable");
    const auto generation = impl_->generation;
    if (!impl_->observed_foreground) impl_->initial_generation = generation;
    impl_->observed_foreground = foreground; impl_->observed_focus = information.hwndFocus; impl_->observed_thread = thread;
    if (!impl_->drain() || generation != impl_->generation || GetForegroundWindow() != foreground)
        return reject("clipboard_focus_changed");
    ClipboardFocusToken result{reinterpret_cast<std::uintptr_t>(foreground), reinterpret_cast<std::uintptr_t>(information.hwndFocus),
        process, thread, session, (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime, generation};
    if (error) error->clear(); return result;
#else
    return reject("clipboard_platform_unsupported");
#endif
}
}
