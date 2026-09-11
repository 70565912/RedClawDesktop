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

bool test_bridge_plan_auto_prefers_hardware_when_available() {
    redclaw::capture::EncoderBackendBridgeRequest request;
    request.preferred_backend = redclaw::capture::EncoderBackendType::kAuto;

    redclaw::capture::EncoderBackendCapabilities capabilities;
    capabilities.ffmpeg_runtime_available = true;
    capabilities.nvenc_available = true;
    capabilities.quicksync_available = true;
    capabilities.software_available = true;

    redclaw::capture::EncoderBackendBridgePlan plan;
    std::string error;
    const bool ok = redclaw::capture::build_encoder_backend_bridge_plan(request, capabilities, &plan, &error);

    return expect_true(ok, std::string("auto bridge plan should succeed: ") + error)
        && expect_true(plan.selected_backend == redclaw::capture::EncoderBackendType::kNvenc,
            "auto bridge plan should prioritize NVENC")
        && expect_true(!plan.fallback_applied, "auto hardware selection should not be marked as fallback");
}

bool test_bridge_plan_auto_prefers_capture_adapter_backend_when_hint_available() {
    redclaw::capture::EncoderBackendBridgeRequest request;
    request.preferred_backend = redclaw::capture::EncoderBackendType::kAuto;
    request.capture_adapter_vendor = redclaw::capture::CaptureAdapterVendor::kIntel;

    redclaw::capture::EncoderBackendCapabilities capabilities;
    capabilities.ffmpeg_runtime_available = true;
    capabilities.nvenc_available = true;
    capabilities.quicksync_available = true;
    capabilities.amf_available = true;
    capabilities.software_available = true;

    redclaw::capture::EncoderBackendBridgePlan plan;
    std::string error;
    const bool ok = redclaw::capture::build_encoder_backend_bridge_plan(request, capabilities, &plan, &error);

    return expect_true(ok, std::string("adapter-aware auto bridge plan should succeed: ") + error)
        && expect_true(plan.selected_backend == redclaw::capture::EncoderBackendType::kQuickSync,
            "auto bridge plan should prioritize QuickSync for Intel capture adapters")
        && expect_true(plan.reason.find("Intel capture adapter priority") != std::string::npos,
            "bridge plan reason should mention Intel capture adapter priority");
}

bool test_bridge_plan_falls_back_to_software_when_hardware_unavailable() {
    redclaw::capture::EncoderBackendBridgeRequest request;
    request.preferred_backend = redclaw::capture::EncoderBackendType::kNvenc;
    request.allow_hardware_fallback = true;

    redclaw::capture::EncoderBackendCapabilities capabilities;
    capabilities.ffmpeg_runtime_available = false;
    capabilities.nvenc_available = false;
    capabilities.quicksync_available = false;
    capabilities.software_available = true;

    redclaw::capture::EncoderBackendBridgePlan plan;
    std::string error;
    const bool ok = redclaw::capture::build_encoder_backend_bridge_plan(request, capabilities, &plan, &error);

    return expect_true(ok, std::string("software fallback bridge plan should succeed: ") + error)
        && expect_true(plan.selected_backend == redclaw::capture::EncoderBackendType::kSoftware,
            "bridge plan should fallback to software")
        && expect_true(plan.fallback_applied, "software fallback should be flagged");
}

bool test_bridge_plan_prefers_alternate_hardware_before_software() {
    redclaw::capture::EncoderBackendBridgeRequest request;
    request.preferred_backend = redclaw::capture::EncoderBackendType::kNvenc;
    request.allow_hardware_fallback = true;

    redclaw::capture::EncoderBackendCapabilities capabilities;
    capabilities.ffmpeg_runtime_available = true;
    capabilities.nvenc_available = false;
    capabilities.quicksync_available = false;
    capabilities.amf_available = true;
    capabilities.software_available = true;

    redclaw::capture::EncoderBackendBridgePlan plan;
    std::string error;
    const bool ok = redclaw::capture::build_encoder_backend_bridge_plan(request, capabilities, &plan, &error);

    return expect_true(ok, std::string("alternate hardware bridge plan should succeed: ") + error)
        && expect_true(plan.selected_backend == redclaw::capture::EncoderBackendType::kAmf,
            "bridge plan should prefer alternate hardware before software")
        && expect_true(plan.fallback_applied, "alternate hardware fallback should be flagged");
}

bool test_bridge_plan_prefers_capture_adapter_hardware_during_fallback() {
    redclaw::capture::EncoderBackendBridgeRequest request;
    request.preferred_backend = redclaw::capture::EncoderBackendType::kNvenc;
    request.allow_hardware_fallback = true;
    request.capture_adapter_vendor = redclaw::capture::CaptureAdapterVendor::kIntel;

    redclaw::capture::EncoderBackendCapabilities capabilities;
    capabilities.ffmpeg_runtime_available = true;
    capabilities.nvenc_available = false;
    capabilities.quicksync_available = true;
    capabilities.amf_available = true;
    capabilities.software_available = true;

    redclaw::capture::EncoderBackendBridgePlan plan;
    std::string error;
    const bool ok = redclaw::capture::build_encoder_backend_bridge_plan(request, capabilities, &plan, &error);

    return expect_true(ok, std::string("adapter-aware fallback bridge plan should succeed: ") + error)
        && expect_true(plan.selected_backend == redclaw::capture::EncoderBackendType::kQuickSync,
            "hardware fallback should prioritize QuickSync for Intel capture adapters")
        && expect_true(plan.fallback_applied, "alternate hardware fallback should be flagged");
}

bool test_bridge_plan_excludes_live_reconfiguration_backend() {
    redclaw::capture::EncoderBackendBridgeRequest request;
    request.preferred_backend = redclaw::capture::EncoderBackendType::kAuto;
    request.excluded_hardware_backend = redclaw::capture::EncoderBackendType::kQuickSync;
    request.capture_adapter_vendor = redclaw::capture::CaptureAdapterVendor::kIntel;

    redclaw::capture::EncoderBackendCapabilities capabilities;
    capabilities.ffmpeg_runtime_available = true;
    capabilities.nvenc_available = true;
    capabilities.quicksync_available = true;
    capabilities.software_available = true;

    redclaw::capture::EncoderBackendBridgePlan plan;
    std::string error;
    const bool ok = redclaw::capture::build_encoder_backend_bridge_plan(
        request, capabilities, &plan, &error);

    return expect_true(ok, std::string("excluded-backend bridge plan should succeed: ") + error)
        && expect_true(plan.selected_backend == redclaw::capture::EncoderBackendType::kNvenc,
            "auto bridge plan should skip excluded QuickSync and use the next hardware backend");
}

bool test_bridge_plan_exclusion_can_fall_back_to_software() {
    redclaw::capture::EncoderBackendBridgeRequest request;
    request.preferred_backend = redclaw::capture::EncoderBackendType::kAuto;
    request.excluded_hardware_backend = redclaw::capture::EncoderBackendType::kQuickSync;
    request.capture_adapter_vendor = redclaw::capture::CaptureAdapterVendor::kIntel;

    redclaw::capture::EncoderBackendCapabilities capabilities;
    capabilities.ffmpeg_runtime_available = true;
    capabilities.quicksync_available = true;
    capabilities.software_available = true;

    redclaw::capture::EncoderBackendBridgePlan plan;
    std::string error;
    const bool ok = redclaw::capture::build_encoder_backend_bridge_plan(
        request, capabilities, &plan, &error);

    return expect_true(ok, std::string("software exclusion fallback should succeed: ") + error)
        && expect_true(plan.selected_backend == redclaw::capture::EncoderBackendType::kSoftware,
            "auto bridge plan should use software when excluded QuickSync is the only hardware backend");
}

bool test_bridge_plan_hard_failure_when_fallback_disabled() {
    redclaw::capture::EncoderBackendBridgeRequest request;
    request.preferred_backend = redclaw::capture::EncoderBackendType::kQuickSync;
    request.allow_hardware_fallback = false;

    redclaw::capture::EncoderBackendCapabilities capabilities;
    capabilities.ffmpeg_runtime_available = true;
    capabilities.nvenc_available = false;
    capabilities.quicksync_available = false;
    capabilities.software_available = true;

    redclaw::capture::EncoderBackendBridgePlan plan;
    std::string error;
    const bool ok = redclaw::capture::build_encoder_backend_bridge_plan(request, capabilities, &plan, &error);

    return expect_true(!ok, "bridge plan should fail when fallback is disabled")
        && expect_true(error.find("fallback is disabled") != std::string::npos,
            "bridge plan should return fallback-disabled error");
}

bool test_bridge_plan_propagates_hardware_frame_input_policy() {
    redclaw::capture::EncoderBackendBridgeRequest request;
    request.preferred_backend = redclaw::capture::EncoderBackendType::kAuto;
    request.allow_hardware_frame_input = false;

    redclaw::capture::EncoderBackendCapabilities capabilities;
    capabilities.ffmpeg_runtime_available = true;
    capabilities.nvenc_available = true;
    capabilities.software_available = true;

    redclaw::capture::EncoderBackendBridgePlan plan;
    std::string error;
    const bool ok = redclaw::capture::build_encoder_backend_bridge_plan(
        request, capabilities, &plan, &error);

    return expect_true(ok, std::string("hardware-frame policy bridge plan should succeed: ") + error)
        && expect_true(plan.selected_backend == redclaw::capture::EncoderBackendType::kNvenc,
            "disabling hardware-frame input must preserve the NVENC codec backend")
        && expect_true(!plan.allow_hardware_frame_input,
            "bridge plan must propagate the disabled hardware-frame input policy");
}

bool test_initial_encoder_start_waits_boundedly_for_viewport() {
    using redclaw::capture::should_defer_initial_encoder_start_for_viewport;

    return expect_true(
               should_defer_initial_encoder_start_for_viewport(
                   false, false, 1000, 3999, 3000),
               "initial encoder should wait within the viewport grace")
        && expect_true(
            !should_defer_initial_encoder_start_for_viewport(
                false, true, 1000, 1001, 3000),
            "a received viewport should release initial encoder startup")
        && expect_true(
            !should_defer_initial_encoder_start_for_viewport(
                false, false, 1000, 4000, 3000),
            "headless startup should proceed when the viewport grace expires")
        && expect_true(
            !should_defer_initial_encoder_start_for_viewport(
                true, false, 1000, 1001, 3000),
            "an active encoder should never enter the initial viewport gate");
}

bool test_live_hardware_encoder_preserves_rate_session_but_applies_geometry() {
    using redclaw::capture::EncoderBackendType;
    using redclaw::capture::should_preserve_encoder_during_live_reconfiguration;

    return expect_true(
               !should_preserve_encoder_during_live_reconfiguration(
                   EncoderBackendType::kNvenc, true, true, false),
               "NVENC geometry changes must rebuild so Host applies the requested dimensions")
        && expect_true(
            should_preserve_encoder_during_live_reconfiguration(
                EncoderBackendType::kQuickSync, true, false, true),
            "QSV rate restart fallback must preserve the active driver session")
        && expect_true(
            should_preserve_encoder_during_live_reconfiguration(
                EncoderBackendType::kAmf, true, true, true),
            "AMF rate restart fallback may preserve the active driver session")
        && expect_true(
            !should_preserve_encoder_during_live_reconfiguration(
                EncoderBackendType::kSoftware, true, true, false),
            "software encoder geometry changes may rebuild safely")
        && expect_true(
            !should_preserve_encoder_during_live_reconfiguration(
                EncoderBackendType::kNvenc, false, true, true),
            "an inactive hardware encoder does not need preservation");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_bridge_plan_auto_prefers_hardware_when_available() && ok;
    ok = test_bridge_plan_auto_prefers_capture_adapter_backend_when_hint_available() && ok;
    ok = test_bridge_plan_falls_back_to_software_when_hardware_unavailable() && ok;
    ok = test_bridge_plan_prefers_alternate_hardware_before_software() && ok;
    ok = test_bridge_plan_prefers_capture_adapter_hardware_during_fallback() && ok;
    ok = test_bridge_plan_excludes_live_reconfiguration_backend() && ok;
    ok = test_bridge_plan_exclusion_can_fall_back_to_software() && ok;
    ok = test_bridge_plan_hard_failure_when_fallback_disabled() && ok;
    ok = test_bridge_plan_propagates_hardware_frame_input_policy() && ok;
    ok = test_initial_encoder_start_waits_boundedly_for_viewport() && ok;
    ok = test_live_hardware_encoder_preserves_rate_session_but_applies_geometry() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_capture_encoder_backend_bridge_tests" << '\n';
    return 0;
}
