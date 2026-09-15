#include "capture_failure_evidence.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#ifdef _WIN32
#include <windows.h>
#endif

namespace redclaw::capture {
void CaptureFailureEvidence::configure(const std::string& directory) {
    if (directory.empty()) { path_.clear(); return; }
    // Keep one bounded file for this recorder across backend/session rebuilds.
    if (!path_.empty()) { return; }
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = 0;
#endif
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::path(directory) / ("capture-failure-" + std::to_string(pid)
        + "-" + std::to_string(stamp) + ".jsonl");
}
void CaptureFailureEvidence::record(CaptureFailure failure, const CaptureDesktopContext& context,
    CaptureBackendType backend, const CaptureSessionConfig& config) noexcept {
    if (path_.empty() || records_ >= 64 || failure.kind == CaptureFailureKind::kNone
        || failure.kind == CaptureFailureKind::kTimeout) { return; }
    if (records_ && previous_.stage == failure.stage && previous_.hresult == failure.hresult
        && previous_backend_ == backend && previous_context_ == context) { return; }
    try {
        std::error_code error;
        std::filesystem::create_directories(path_.parent_path(), error);
        if (error) { return; }
        std::ofstream output(path_, std::ios::app);
        if (!output) { return; }
        auto safe = [](std::string text) {
            for (auto& ch : text) { if (static_cast<unsigned char>(ch) < 32) { ch = '?'; } }
            return text;
        };
        output << "{\"schema\":\"redclaw.capture-failure.v1\",\"unix_ms\":"
            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()
            << ",\"stage\":" << static_cast<int>(failure.stage)
            << ",\"kind\":" << static_cast<int>(failure.kind)
            << ",\"hresult\":" << failure.hresult
            << ",\"backend\":" << static_cast<int>(backend)
            << ",\"adapter_index\":" << config.adapter_index << ",\"output_index\":" << config.output_index
            << ",\"display_id\":" << std::quoted(safe(config.display_id))
            << ",\"desktop_access\":" << static_cast<int>(context.access)
            << ",\"session_id\":" << context.session_id << ",\"win32_error\":" << context.win32_error
            << ",\"thread_desktop\":" << std::quoted(safe(context.thread_desktop))
            << ",\"input_desktop\":" << std::quoted(safe(context.input_desktop))
            << ",\"window_station\":" << std::quoted(safe(context.window_station))
            << ",\"elevated\":" << context.token_elevated << ",\"restricted\":" << context.token_restricted
            << ",\"app_container\":" << context.token_app_container
            << ",\"display_signature\":" << context.display_signature << "}\n";
        if (output) {
            previous_ = failure; previous_context_ = context; previous_backend_ = backend; ++records_;
        }
    } catch (...) { /* Evidence failure must not change the permission/recovery decision. */ }
}
}
