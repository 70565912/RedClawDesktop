#include <filesystem>
#include <iostream>
#include <limits>
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

bool test_capture_frame_requires_started_session() {
    redclaw::capture::WindowsCaptureSession session;

    redclaw::capture::CapturedFrame frame;
    std::string error;
    const bool ok = session.captureFrame(&frame, &error);

    return expect_true(!ok, "captureFrame should fail before start")
        && expect_true(error.find("not started") != std::string::npos, "error should indicate session not started");
}

bool test_start_exposes_active_backend_when_available() {
    redclaw::capture::WindowsCaptureSession session;

    redclaw::capture::CaptureSessionConfig config;
    config.output_index = 0;
    config.frame_acquire_timeout_ms = 1000;

    std::string error;
    const bool started = session.start(config, &error);
    if (!started) {
        std::cout << "[SKIP] redclaw_capture_windows_capture_abstraction_skeleton_tests: " << error << '\n';
        return true;
    }

    const auto active_backend = session.activeBackend();
    const bool backend_ok =
        active_backend == redclaw::capture::CaptureBackendType::kDesktopDuplication
        || active_backend == redclaw::capture::CaptureBackendType::kWindowsGraphicsCapture
        || active_backend == redclaw::capture::CaptureBackendType::kGdiBitBlt;

    redclaw::capture::CapturedFrame frame;
    const bool captured = session.captureFrame(&frame, &error);

    session.stop();

    if (!captured) {
        std::cout << "[SKIP] redclaw_capture_windows_capture_abstraction_skeleton_tests: " << error << '\n';
        return true;
    }

    return expect_true(backend_ok, "active backend should be Desktop Duplication, Windows Graphics Capture, or GDI BitBlt")
        && expect_true(frame.bgra, "captured frame should be BGRA")
        && expect_true(frame.width > 0 && frame.height > 0, "captured frame should have valid dimensions")
        && expect_true(!frame.data.empty(), "captured frame data should be non-empty");
}

bool test_capture_geometry_update_advances_only_for_a_valid_size_change() {
    const auto unchanged = redclaw::capture::evaluate_capture_geometry_update(
        1920, 1080, 1920, 1080, 7);
    const auto changed = redclaw::capture::evaluate_capture_geometry_update(
        1920, 1080, 2560, 1440, 7);
    const auto invalid = redclaw::capture::evaluate_capture_geometry_update(
        1920, 1080, 0, 1440, 7);
    const auto first = redclaw::capture::evaluate_capture_geometry_update(
        0, 0, 1280, 720, 0);
    const auto saturated = redclaw::capture::evaluate_capture_geometry_update(
        1920,
        1080,
        2560,
        1440,
        std::numeric_limits<std::uint64_t>::max());

    return expect_true(unchanged.valid, "an unchanged non-zero capture size should be valid")
        && expect_true(!unchanged.changed, "an unchanged capture size must not advance geometry")
        && expect_true(unchanged.next_revision == 7, "an unchanged capture size must keep its revision")
        && expect_true(changed.valid && changed.changed, "a new non-zero capture size should require reconfiguration")
        && expect_true(changed.next_revision == 8, "a changed capture size should advance geometry exactly once")
        && expect_true(!invalid.valid && !invalid.changed, "a zero dimension must be rejected without reconfiguration")
        && expect_true(invalid.next_revision == 7, "an invalid size must keep its prior revision")
        && expect_true(first.valid && first.changed && first.next_revision == 1,
                       "the first valid geometry should start at revision one")
        && expect_true(saturated.changed, "a changed size remains observable at a saturated revision")
        && expect_true(saturated.next_revision == std::numeric_limits<std::uint64_t>::max(),
                       "geometry revision must not wrap");
}

bool test_capture_region_normalization_and_letterbox_are_bounded() {
    const auto normalized = redclaw::capture::normalize_capture_region(
        {.x = 1919, .y = 1079, .width = 1, .height = 1, .revision = 9},
        1920,
        1080);
    const auto from_wire = redclaw::capture::capture_region_from_normalized_bounds(
        32768,
        16384,
        65535,
        65535,
        1920,
        1080,
        12);
    const auto content = redclaw::capture::resolve_capture_region_content_rect(
        {.x = 0, .y = 0, .width = 800, .height = 800, .revision = 12},
        1920,
        1080);

    return expect_true(normalized.x % 2 == 0 && normalized.y % 2 == 0,
                       "capture region origin must be even")
        && expect_true(normalized.width >= 64 && normalized.height >= 64,
                       "capture region must preserve the 64x64 minimum")
        && expect_true(normalized.x + normalized.width <= 1920
                           && normalized.y + normalized.height <= 1080,
                       "capture region must stay inside the selected display")
        && expect_true(from_wire.revision == 12 && from_wire.x % 2 == 0
                           && from_wire.y % 2 == 0,
                       "wire bounds must preserve revision and even alignment")
        && expect_true(from_wire.x + from_wire.width <= 1920
                           && from_wire.y + from_wire.height <= 1080,
                       "wire bounds must be clamped to the display")
        && expect_true(content.width == 1080 && content.height == 1080,
                       "a square crop should scale to the canvas height")
        && expect_true(content.x == 420 && content.y == 0,
                       "letterboxed content should be centered on even coordinates");
}

bool test_navigation_thumbnail_encodes_bounded_jpeg() {
    redclaw::capture::CapturedFrame frame;
    frame.width = 640;
    frame.height = 360;
    frame.row_pitch = frame.width * 4U;
    frame.bgra = true;
    frame.data.resize(static_cast<std::size_t>(frame.row_pitch) * frame.height);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y) * frame.row_pitch
                + static_cast<std::size_t>(x) * 4U;
            frame.data[offset + 0] = static_cast<std::uint8_t>(x & 0xFFU);
            frame.data[offset + 1] = static_cast<std::uint8_t>(y & 0xFFU);
            frame.data[offset + 2] = 0x80;
            frame.data[offset + 3] = 0xFF;
        }
    }
    std::vector<std::uint8_t> jpeg;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string error;
    const bool encoded = redclaw::capture::encode_navigation_thumbnail_jpeg(
        frame, 320, &jpeg, &width, &height, &error);
    return expect_true(encoded, "navigation JPEG should encode: " + error)
        && expect_true(width == 320 && height == 180,
                       "navigation JPEG should preserve aspect within 320 pixels")
        && expect_true(jpeg.size() >= 4 && jpeg.size() <= 512U * 1024U,
                       "navigation JPEG should remain within protocol bounds")
        && expect_true(jpeg[0] == 0xFF && jpeg[1] == 0xD8,
                       "navigation payload should have a JPEG SOI marker");
}

bool test_wgc_backend_captures_a_real_frame_when_available() {
    redclaw::capture::WindowsCaptureSession session;
    redclaw::capture::CaptureSessionConfig config;
    config.output_index = 0;
    config.frame_acquire_timeout_ms = 2000;
    config.preferred_backend = redclaw::capture::CaptureBackendType::kWindowsGraphicsCapture;
    config.fallback_enabled = false;

    std::string error;
    std::cout << "[WGC] starting forced backend" << std::endl;
    if (!session.start(config, &error)) {
        std::cout << "[SKIP] WGC capture unavailable: " << error << '\n';
        return true;
    }
    std::cout << "[WGC] backend started" << std::endl;

    redclaw::capture::CapturedFrame frame;
    const bool captured = session.captureFrame(&frame, &error);
    std::cout << "[WGC] capture returned captured=" << std::boolalpha << captured << std::endl;
    const auto active_backend = session.activeBackend();
    session.stop();
    std::cout << "[WGC] backend stopped" << std::endl;

    bool ok = expect_true(
               active_backend == redclaw::capture::CaptureBackendType::kWindowsGraphicsCapture,
               "forced WGC session must not silently use another backend")
        && expect_true(captured, std::string("WGC should capture a real frame: ") + error)
        && expect_true(frame.bgra, "WGC frame should be BGRA")
        && expect_true(frame.width > 0 && frame.height > 0, "WGC frame should have valid dimensions")
        && expect_true(!frame.data.empty(), "WGC CPU-readback test frame should contain pixels");
    if (!ok) {
        return false;
    }

    redclaw::capture::EncoderProfileRequest profile_request;
    profile_request.width = ((frame.width < 580U ? frame.width : 580U) & ~1U);
    profile_request.height = ((frame.height < 362U ? frame.height : 362U) & ~1U);
    profile_request.fps = 30;
    profile_request.preferred_codec = redclaw::capture::EncoderCodec::kH264;

    redclaw::capture::EncoderBackendBridgeRequest bridge_request;
    bridge_request.preferred_backend = redclaw::capture::EncoderBackendType::kAuto;
    bridge_request.allow_hardware_fallback = true;
    bridge_request.capture_adapter_vendor =
        redclaw::capture::detect_captured_frame_adapter_vendor(frame);

    redclaw::capture::EncoderExecutionSession encoder_session;
    redclaw::capture::EncoderBackendBridgePlan bridge_plan;
    if (!redclaw::capture::start_encoder_execution_from_bridge(
            profile_request,
            bridge_request,
            &encoder_session,
            &bridge_plan,
            &error)) {
        return expect_true(false, std::string("WGC frame encoder should start: ") + error);
    }

    redclaw::capture::EncodedFramePacket packet;
    const bool encoded = encoder_session.encode_bgra_frame(frame, 1000, &packet, &error);
    const auto diagnostics = encoder_session.diagnostics();
    encoder_session.stop();

    ok = expect_true(encoded, std::string("WGC frame should encode after any native-input fallback: ") + error) && ok;
    ok = expect_true(!packet.payload.empty(), "WGC frame encode should produce a payload") && ok;
    ok = expect_true(
        diagnostics.last_failure == redclaw::capture::EncoderExecutionFailureCategory::kNone,
        "a recovered WGC native-input attempt must not remain an encode failure") && ok;
    ok = expect_true(
        diagnostics.dropped_frame_count == 0,
        "a recovered WGC native-input attempt must retry the current frame instead of dropping it") && ok;
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_capture_frame_requires_started_session() && ok;
    ok = test_capture_geometry_update_advances_only_for_a_valid_size_change() && ok;
    ok = test_capture_region_normalization_and_letterbox_are_bounded() && ok;
    ok = test_navigation_thumbnail_encodes_bounded_jpeg() && ok;
    ok = test_start_exposes_active_backend_when_available() && ok;
    ok = test_wgc_backend_captures_a_real_frame_when_available() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_capture_windows_capture_abstraction_skeleton_tests" << '\n';
    return 0;
}
