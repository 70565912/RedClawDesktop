#pragma once
#include "redclaw/input/input_module.h"
#if defined(_WIN32)
#include <Windows.h>
#include <array>

namespace redclaw::input::detail {
inline std::uint32_t desktop_kind(HDESK desktop, std::uint32_t* error) {
    if (!desktop) { if (error) *error = GetLastError(); return 0; }
    wchar_t name[256]{}; DWORD needed = 0;
    if (!GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed)) {
        if (error) *error = GetLastError(); return 0;
    }
    return lstrcmpiW(name, L"Default") == 0 ? 1U : lstrcmpiW(name, L"Winlogon") == 0 ? 2U : 3U;
}
inline std::uint32_t integrity(DWORD pid, std::uint32_t* error) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) { *error = GetLastError(); return 0; }
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        *error = GetLastError(); CloseHandle(process); return 0;
    }
    alignas(TOKEN_MANDATORY_LABEL) std::array<std::byte, 256> buffer{};
    DWORD needed = 0; std::uint32_t value = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), DWORD(buffer.size()), &needed)) {
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
        if (IsValidSid(label->Label.Sid) && *GetSidSubAuthorityCount(label->Label.Sid) > 0)
            value = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
        else *error = ERROR_INVALID_SID;
    } else *error = GetLastError();
    CloseHandle(token); CloseHandle(process); return value;
}
inline void sample_context(SendInputDiagnostic& result) {
    result.context_sampled = true;
    result.process_id = GetCurrentProcessId(); result.thread_id = GetCurrentThreadId();
    DWORD session = MAXDWORD;
    if (ProcessIdToSessionId(result.process_id, &session)) result.session_id = session;
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    result.input_desktop = desktop_kind(input, &result.desktop_error);
    if (input) CloseDesktop(input);
    result.thread_desktop = desktop_kind(GetThreadDesktop(result.thread_id), nullptr);
    DWORD foreground = 0;
    const DWORD thread = GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    result.foreground_pid = foreground;
    if (foreground && ProcessIdToSessionId(foreground, &session)) result.foreground_session = session;
    GUITHREADINFO gui{}; gui.cbSize = sizeof(gui);
    if (thread && GetGUIThreadInfo(thread, &gui)) {
        DWORD focus = 0; GetWindowThreadProcessId(gui.hwndFocus, &focus); result.focus_pid = focus;
    }
    result.process_integrity = integrity(result.process_id, &result.process_integrity_error);
    result.foreground_integrity = integrity(foreground, &result.foreground_integrity_error);
    POINT cursor{};
    result.cursor_before_valid = GetCursorPos(&cursor) != FALSE;
    result.cursor_before_x = cursor.x; result.cursor_before_y = cursor.y;
}
}
#endif
