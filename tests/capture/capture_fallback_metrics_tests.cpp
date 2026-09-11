#include <iostream>
#include <string>

#include "redclaw/capture/capture_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }
    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_fallback_decision_on_failure_threshold() {
    redclaw::capture::CaptureSessionConfig config;
    config.fallback_enabled = true;
    config.max_consecutive_failures_before_fallback = 3;

    redclaw::capture::CaptureHealthSignals signals;
    signals.backend = redclaw::capture::CaptureBackendType::kDesktopDuplication;
    signals.consecutive_failures = 3;

    const auto decision = redclaw::capture::evaluate_capture_fallback_decision(signals, config);
    return expect_true(decision.should_switch_backend, "fallback should trigger on capture failure threshold")
        && expect_true(decision.reason.find("failure") != std::string::npos, "fallback reason should mention failures");
}

bool test_valid_dda_frame_does_not_trigger_fallback() {
    redclaw::capture::CaptureSessionConfig config;
    config.fallback_enabled = true;

    redclaw::capture::CaptureHealthSignals signals;
    signals.backend = redclaw::capture::CaptureBackendType::kDesktopDuplication;

    const auto decision = redclaw::capture::evaluate_capture_fallback_decision(signals, config);
    return expect_true(!decision.should_switch_backend,
        "a valid DDA frame must not trigger fallback merely because desktop content is unchanged");
}

bool test_fallback_decision_respects_disable_switch() {
    redclaw::capture::CaptureSessionConfig config;
    config.fallback_enabled = false;

    redclaw::capture::CaptureHealthSignals signals;
    signals.backend = redclaw::capture::CaptureBackendType::kDesktopDuplication;
    signals.consecutive_failures = 100;

    const auto decision = redclaw::capture::evaluate_capture_fallback_decision(signals, config);
    return expect_true(!decision.should_switch_backend, "fallback should be disabled when config disables fallback");
}

bool test_session_telemetry_default_values() {
    redclaw::capture::WindowsCaptureSession session;
    const auto telemetry = session.telemetry();

    return expect_true(telemetry.active_backend == redclaw::capture::CaptureBackendType::kUnknown,
               "default active backend should be unknown")
        && expect_true(telemetry.backend_switch_count == 0, "default backend switch count should be zero")
        && expect_true(telemetry.fallback_attempt_count == 0, "default fallback attempt count should be zero");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_fallback_decision_on_failure_threshold() && ok;
    ok = test_valid_dda_frame_does_not_trigger_fallback() && ok;
    ok = test_fallback_decision_respects_disable_switch() && ok;
    ok = test_session_telemetry_default_values() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_capture_fallback_metrics_tests" << '\n';
    return 0;
}
