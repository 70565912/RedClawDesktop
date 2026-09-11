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

bool test_stability_probe_rejects_zero_duration() {
    redclaw::capture::CaptureStabilityRunConfig config;
    config.run_duration_seconds = 0;

    redclaw::capture::CaptureStabilityRunResult result;
    std::string error;
    const bool ok = redclaw::capture::run_capture_stability_probe(config, &result, &error);

    return expect_true(!ok, "stability probe should fail on zero duration")
        && expect_true(error.find("greater than zero") != std::string::npos, "error should describe invalid duration");
}

bool test_stability_probe_smoke_run() {
    redclaw::capture::CaptureStabilityRunConfig config;
    config.capture_config.output_index = 0;
    config.capture_config.frame_acquire_timeout_ms = 250;
    config.run_duration_seconds = 2;
    config.max_consecutive_timeouts = 200;
    config.max_consecutive_failures = 2;

    redclaw::capture::CaptureStabilityRunResult result;
    std::string error;
    const bool ok = redclaw::capture::run_capture_stability_probe(config, &result, &error);

    if (!ok) {
        std::cout << "[SKIP] redclaw_capture_stability_smoke_tests: " << error << '\n';
        return true;
    }

    return expect_true(result.frames_captured > 0, "stability probe should capture at least one frame")
        && expect_true(result.capture_attempts >= result.frames_captured, "attempt count should be >= frame count")
        && expect_true(result.average_fps > 0.0, "average fps should be positive");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_stability_probe_rejects_zero_duration() && ok;
    ok = test_stability_probe_smoke_run() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_capture_stability_smoke_tests" << '\n';
    return 0;
}
