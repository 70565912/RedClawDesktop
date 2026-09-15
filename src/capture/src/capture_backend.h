#pragma once
#include "redclaw/capture/capture_module.h"
#include <chrono>
#include <functional>

namespace redclaw::capture {
struct CaptureFrameStageTelemetry {
    std::uint64_t wait_us = 0, copy_us = 0;
    std::uint32_t accumulated_frames = 0;
    bool native_texture_pool_created = false, native_texture_pool_reused = false;
    bool native_texture_pool_exhausted = false;
    std::uint64_t frame_pool_recreate_us = 0;
    std::uint32_t frame_pool_recreate_count = 0;
    bool timeout = false;
};
class ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;
    virtual bool start(const CaptureSessionConfig&, std::string*) = 0;
    virtual bool capture_frame(CapturedFrame*, CaptureFrameStageTelemetry*, std::string*) = 0;
    virtual void configure_native_frame_delivery(bool, bool) {}
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
    virtual CaptureBackendType backend_type() const = 0;
    CaptureFailure failure() const { return failure_; }
    void clear_failure() { failure_ = {}; }
protected:
    void record_failure(CaptureFailureStage stage, std::uint32_t hr) {
        failure_ = make_capture_failure(stage, hr);
    }
private:
    CaptureFailure failure_;
};
// Internal dependency seam: tests exercise the actual session recovery loop.
// No runtime flag, environment variable or remote command can install hooks.
struct CaptureSessionTestHooks {
    std::function<std::unique_ptr<ICaptureBackend>(CaptureBackendType)> backend;
    std::function<CaptureDesktopContext()> desktop;
    std::function<std::chrono::steady_clock::time_point()> clock;
};
struct CaptureSessionTestAccess {
    static void install(WindowsCaptureSession&, CaptureSessionTestHooks);
};
}
