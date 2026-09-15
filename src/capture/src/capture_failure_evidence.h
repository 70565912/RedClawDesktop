#pragma once
#include "redclaw/capture/capture_module.h"
#include <filesystem>

namespace redclaw::capture {
// Capture-thread owned, failure-only evidence. It is deliberately separate
// from the runtime log exported through Control and contains no credentials.
class CaptureFailureEvidence {
public:
    void configure(const std::string& directory);
    void record(CaptureFailure, const CaptureDesktopContext&, CaptureBackendType,
        const CaptureSessionConfig&) noexcept;
private:
    std::filesystem::path path_;
    CaptureFailure previous_;
    CaptureDesktopContext previous_context_;
    CaptureBackendType previous_backend_ = CaptureBackendType::kUnknown;
    unsigned records_ = 0;
};
}
