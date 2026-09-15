#pragma once
#include <cstdint>
#include <string>

namespace redclaw::capture {
enum class CaptureFailureStage { kNone, kDesktopAccess, kEnumerateOutput, kCreateDevice,
    kDuplicateOutput, kAcquireFrame, kReadback, kCursor, kStartBackend };
enum class CaptureFailureKind { kNone, kTimeout, kAccessLost, kDeviceLost, kAccessDenied,
    kSessionDisconnected, kOther };
enum class CaptureAvailability { kStopped, kRecovering, kRunning, kPaused };
enum class CaptureDesktopAccess { kUnknown, kOrdinary, kDenied, kDisconnected };
enum class CaptureRecoveryAction { kWait, kRebuildDda, kFallback, kPause };
struct CaptureFailure {
    CaptureFailureStage stage = CaptureFailureStage::kNone;
    CaptureFailureKind kind = CaptureFailureKind::kNone;
    std::uint32_t hresult = 0;
};
CaptureFailure make_capture_failure(CaptureFailureStage stage, std::uint32_t hresult);

// Local evidence only; never serialize desktop or token details into Control.
struct CaptureDesktopContext {
    CaptureDesktopAccess access = CaptureDesktopAccess::kUnknown;
    std::uint32_t session_id = 0;
    std::uint32_t win32_error = 0;
    bool token_elevated = false;
    bool token_restricted = false;
    bool token_app_container = false;
    std::string thread_desktop;
    std::string input_desktop;
    std::string window_station;
    std::uint64_t display_signature = 0;
    bool operator==(const CaptureDesktopContext&) const = default;
};
enum class CaptureProbeDetail { kAccessOnly, kWithDisplayState };
CaptureDesktopContext probe_capture_desktop(CaptureProbeDetail detail = CaptureProbeDetail::kWithDisplayState);

// One rebuild per incident. Only a real new frame ends the incident.
class CaptureRecoveryPolicy {
public:
    CaptureRecoveryAction failure(CaptureFailure failure, CaptureDesktopAccess access,
        bool dda, std::uint32_t consecutive_failures, std::uint32_t threshold);
    void frame_captured();
    void reset();
private:
    bool dda_rebuild_attempted_ = false;
};
} // namespace redclaw::capture
