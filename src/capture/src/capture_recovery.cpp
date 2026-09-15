#include "redclaw/capture/capture_recovery.h"
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wtsapi32.h>
#endif

namespace redclaw::capture {
CaptureFailure make_capture_failure(CaptureFailureStage stage, std::uint32_t hr) {
    CaptureFailureKind kind = CaptureFailureKind::kOther;
    switch (hr) {
    case 0: kind = CaptureFailureKind::kNone; break;
    case 0x887A0027U: kind = CaptureFailureKind::kTimeout; break;
    case 0x887A0026U: kind = CaptureFailureKind::kAccessLost; break;
    case 0x887A0005U: case 0x887A0006U: case 0x887A0007U:
        kind = CaptureFailureKind::kDeviceLost; break;
    case 0x80070005U: kind = CaptureFailureKind::kAccessDenied; break;
    case 0x887A0028U: kind = CaptureFailureKind::kSessionDisconnected; break;
    default: break;
    }
    return {stage, kind, hr};
}
CaptureRecoveryAction CaptureRecoveryPolicy::failure(CaptureFailure failure,
    CaptureDesktopAccess access, bool dda, std::uint32_t count, std::uint32_t threshold) {
    if (access != CaptureDesktopAccess::kOrdinary
        || failure.kind == CaptureFailureKind::kSessionDisconnected) {
        return CaptureRecoveryAction::kPause;
    }
    if (failure.kind == CaptureFailureKind::kTimeout || failure.kind == CaptureFailureKind::kNone) {
        return CaptureRecoveryAction::kWait;
    }
    const bool immediate = failure.kind == CaptureFailureKind::kAccessLost
        || failure.kind == CaptureFailureKind::kDeviceLost;
    if (!immediate && count < std::max(1U, threshold)) { return CaptureRecoveryAction::kWait; }
    if (dda && !dda_rebuild_attempted_) {
        dda_rebuild_attempted_ = true;
        return CaptureRecoveryAction::kRebuildDda;
    }
    return CaptureRecoveryAction::kFallback;
}
void CaptureRecoveryPolicy::frame_captured() { reset(); }
void CaptureRecoveryPolicy::reset() { dda_rebuild_attempted_ = false; }

CaptureDesktopContext probe_capture_desktop() {
    CaptureDesktopContext result;
#ifdef _WIN32
    auto object_name = [](HANDLE object) -> std::string {
        wchar_t name[256]{};
        DWORD needed = 0;
        if (!object || !GetUserObjectInformationW(object, UOI_NAME, name, sizeof(name), &needed)) { return {}; }
        char utf8[1024]{};
        const int size = WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
        return size > 0 ? std::string(utf8) : std::string{};
    };
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        result.win32_error = GetLastError(); return result;
    }
    result.session_id = session;
    LPWSTR session_info = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSConnectState,
            &session_info, &bytes)) {
        result.win32_error = GetLastError(); return result;
    }
    const bool connected = bytes >= sizeof(WTS_CONNECTSTATE_CLASS)
        && *reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(session_info) == WTSActive;
    WTSFreeMemory(session_info);
    if (!connected) { result.access = CaptureDesktopAccess::kDisconnected; return result; }
    result.thread_desktop = object_name(GetThreadDesktop(GetCurrentThreadId()));
    result.window_station = object_name(GetProcessWindowStation());
    const HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) {
        result.win32_error = GetLastError();
        result.access = result.win32_error == ERROR_ACCESS_DENIED
            ? CaptureDesktopAccess::kDenied : CaptureDesktopAccess::kUnknown;
        return result;
    }
    result.input_desktop = object_name(input);
    CloseDesktop(input);
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        result.win32_error = GetLastError(); return result;
    }
    TOKEN_ELEVATION elevation{};
    DWORD app_container = 0;
    const bool token_ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes)
        && GetTokenInformation(token, TokenIsAppContainer, &app_container, sizeof(app_container), &bytes);
    result.token_restricted = IsTokenRestricted(token) != FALSE;
    result.token_elevated = elevation.TokenIsElevated != 0;
    result.token_app_container = app_container != 0;
    if (!token_ok) { result.win32_error = GetLastError(); }
    CloseHandle(token);
    if (!token_ok || result.input_desktop.empty() || result.thread_desktop.empty()
        || result.window_station.empty()) { return result; }
    result.access = _stricmp(result.window_station.c_str(), "WinSta0") == 0
        && !result.token_restricted && !result.token_app_container
        && _stricmp(result.input_desktop.c_str(), "Default") == 0
        && _stricmp(result.thread_desktop.c_str(), "Default") == 0
        ? CaptureDesktopAccess::kOrdinary : CaptureDesktopAccess::kDenied;
    std::uint64_t signature = 1469598103934665603ULL;
    auto mix = [&](std::uint32_t value) { signature = (signature ^ value) * 1099511628211ULL; };
    DISPLAY_DEVICEW display{};
    display.cb = sizeof(display);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &display, 0); ++index) {
        if (display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            DEVMODEW mode{}; mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(display.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                for (const wchar_t ch : display.DeviceName) { mix(ch); }
                mix(mode.dmPelsWidth); mix(mode.dmPelsHeight); mix(mode.dmDisplayOrientation);
                mix(mode.dmPosition.x); mix(mode.dmPosition.y); mix(mode.dmDisplayFrequency);
            }
        }
        display = {}; display.cb = sizeof(display);
    }
    result.display_signature = signature;
#endif
    return result;
}
} // namespace redclaw::capture
