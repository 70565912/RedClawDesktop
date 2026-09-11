#include <cstdlib>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "redclaw/capture/capture_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

void set_environment_value(const std::string& name, const std::optional<std::string>& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.has_value() ? value->c_str() : "");
#else
    if (value.has_value()) {
        setenv(name.c_str(), value->c_str(), 1);
    } else {
        unsetenv(name.c_str());
    }
#endif
}

std::optional<std::string> get_environment_value(const std::string& name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name.c_str()) != 0 || value == nullptr) {
        return std::nullopt;
    }
    const std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name.c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
#endif
}

class ScopedEnvironmentValue {
public:
    ScopedEnvironmentValue(std::string name, std::string value)
        : name_(std::move(name)),
          original_value_(get_environment_value(name_)) {
        set_environment_value(name_, value);
    }

    ~ScopedEnvironmentValue() {
        set_environment_value(name_, original_value_);
    }

    ScopedEnvironmentValue(const ScopedEnvironmentValue&) = delete;
    ScopedEnvironmentValue& operator=(const ScopedEnvironmentValue&) = delete;

private:
    std::string name_;
    std::optional<std::string> original_value_;
};

redclaw::capture::CapturedFrame make_test_bgra_frame(std::uint32_t width, std::uint32_t height) {
    redclaw::capture::CapturedFrame frame;
    frame.width = width;
    frame.height = height;
    frame.bgra = true;
    frame.row_pitch = width * 4;
    frame.data.resize(static_cast<std::size_t>(frame.row_pitch) * height, 0);

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * frame.row_pitch + static_cast<std::size_t>(x) * 4;
            frame.data[index + 0] = static_cast<std::uint8_t>(x % 256);
            frame.data[index + 1] = static_cast<std::uint8_t>(y % 256);
            frame.data[index + 2] = static_cast<std::uint8_t>((x + y) % 256);
            frame.data[index + 3] = 255;
        }
    }

    return frame;
}

bool test_encoder_execution_bridge_start_flow() {
    redclaw::capture::EncoderProfileRequest profile_request;
    profile_request.width = 640;
    profile_request.height = 360;
    profile_request.fps = 30;
    profile_request.preferred_codec = redclaw::capture::EncoderCodec::kH264;

    redclaw::capture::EncoderBackendBridgeRequest bridge_request;
    bridge_request.preferred_backend = redclaw::capture::EncoderBackendType::kAuto;

    redclaw::capture::EncoderExecutionSession session;
    redclaw::capture::EncoderBackendBridgePlan resolved_plan;
    std::string error;

    const bool started = redclaw::capture::start_encoder_execution_from_bridge(
        profile_request,
        bridge_request,
        &session,
        &resolved_plan,
        &error);

    if (!started) {
        return expect_true(!error.empty(), "start failure should include error detail");
    }

    bool ok = true;
    ok = expect_true(session.is_running(), "session should be running after successful start") && ok;
    ok = expect_true(
        resolved_plan.selected_backend != redclaw::capture::EncoderBackendType::kAuto,
        "resolved backend should not remain auto") && ok;

    redclaw::capture::EncodedFramePacket packet;
    const redclaw::capture::CapturedFrame frame = make_test_bgra_frame(profile_request.width, profile_request.height);
    std::string encode_error;
    const bool encoded = session.encode_bgra_frame(frame, 1000, &packet, &encode_error);

    if (encoded) {
        ok = expect_true(!packet.payload.empty(), "encoded payload should be non-empty on success") && ok;
        ok = expect_true(packet.keyframe, "a newly opened low-latency encoder must emit an I-frame first") && ok;
    } else {
        const auto diagnostics = session.diagnostics();
        ok = expect_true(!encode_error.empty(), "encode failure should include detail") && ok;
        ok = expect_true(
            diagnostics.last_failure != redclaw::capture::EncoderExecutionFailureCategory::kNone,
            "encode failure should set non-none failure category") && ok;
    }

    const auto diagnostics = session.diagnostics();
    ok = expect_true(
        !diagnostics.hardware_frame_input_active,
        "CPU-backed test frame should not activate hardware-frame input") && ok;

    session.stop();
    return ok;
}

bool test_encoder_execution_rejects_b_frames() {
    redclaw::capture::EncoderProfileRequest profile_request;
    profile_request.width = 320;
    profile_request.height = 180;
    profile_request.fps = 15;

    redclaw::capture::EncoderConfigProfile profile;
    std::string error;
    if (!redclaw::capture::build_low_latency_encoder_profile(profile_request, &profile, &error)) {
        return expect_true(false, "low-latency profile should build before B-frame rejection test");
    }
    profile.b_frames = 1;

    redclaw::capture::EncoderBackendBridgeRequest bridge_request;
    bridge_request.preferred_backend = redclaw::capture::EncoderBackendType::kSoftware;
    redclaw::capture::EncoderExecutionSession session;
    redclaw::capture::EncoderBackendBridgePlan resolved_plan;
    const bool started = redclaw::capture::start_encoder_execution_from_bridge(
        profile,
        bridge_request,
        &session,
        &resolved_plan,
        &error);
    return expect_true(!started, "runtime encoder must reject any profile that enables B-frames")
        && expect_true(error.find("zero B-frames") != std::string::npos,
            "B-frame rejection should identify the low-latency invariant");
}

bool test_encoder_execution_reencodes_static_frame_as_requested_keyframe() {
    redclaw::capture::EncoderProfileRequest profile_request;
    profile_request.width = 320;
    profile_request.height = 180;
    profile_request.fps = 30;
    profile_request.preferred_codec = redclaw::capture::EncoderCodec::kH264;

    redclaw::capture::EncoderConfigProfile profile;
    std::string error;
    if (!redclaw::capture::build_low_latency_encoder_profile(profile_request, &profile, &error)) {
        return expect_true(false, "static-refresh profile should build: " + error);
    }

    redclaw::capture::EncoderBackendBridgeRequest bridge_request;
    bridge_request.preferred_backend = redclaw::capture::EncoderBackendType::kSoftware;
    redclaw::capture::EncoderExecutionSession session;
    redclaw::capture::EncoderBackendBridgePlan resolved_plan;
    if (!redclaw::capture::start_encoder_execution_from_bridge(
            profile,
            bridge_request,
            &session,
            &resolved_plan,
            &error)) {
        return expect_true(false, "static-refresh software encoder should start: " + error);
    }

    const redclaw::capture::CapturedFrame frame = make_test_bgra_frame(
        profile_request.width,
        profile_request.height);
    redclaw::capture::EncodedFramePacket initial_packet;
    redclaw::capture::EncodedFramePacket refresh_packet;
    std::string encode_error;
    const bool initial_encoded = session.encode_bgra_frame(
        frame,
        1000,
        &initial_packet,
        &encode_error);
    const auto initial_pts = session.diagnostics().last_submitted_pts;
    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    redclaw::capture::EncodedFramePacket predicted_packet;
    const bool predicted_encoded = session.encode_bgra_frame(
        frame, 1110, &predicted_packet, &encode_error);
    const auto paced_pts = session.diagnostics().last_submitted_pts;
    session.request_keyframe();
    const bool refresh_encoded = session.encode_bgra_frame(
        frame,
        1111,
        &refresh_packet,
        &encode_error);
    const auto diagnostics = session.diagnostics();
    session.stop();

    return expect_true(initial_encoded, "static-refresh initial frame should encode: " + encode_error)
        && expect_true(initial_packet.keyframe, "static-refresh initial frame should be an I-frame")
        && expect_true(predicted_encoded && !predicted_packet.keyframe,
            "a paced static frame must remain predictive without an IDR request")
        && expect_true(paced_pts >= initial_pts + 3,
            "encoder PTS must follow elapsed submission time, not frame count")
        && expect_true(diagnostics.last_submitted_pts > paced_pts,
            "back-to-back submissions must retain strictly monotonic PTS")
        && expect_true(diagnostics.configured_fps == 30,
            "pacing must not silently change the active encoder time base")
        && expect_true(refresh_encoded, "the retained static frame should encode again: " + encode_error)
        && expect_true(refresh_packet.keyframe, "a requested static refresh must be an I-frame")
        && expect_true(
            diagnostics.encoded_keyframe_count >= 2,
            "static refresh should be visible in encoder keyframe diagnostics");
}

bool test_encoder_execution_accepts_resized_source_frame() {
    redclaw::capture::EncoderProfileRequest profile_request;
    profile_request.width = 640;
    profile_request.height = 360;
    profile_request.fps = 30;
    profile_request.preferred_codec = redclaw::capture::EncoderCodec::kH264;

    redclaw::capture::EncoderBackendBridgeRequest bridge_request;
    bridge_request.preferred_backend = redclaw::capture::EncoderBackendType::kAuto;

    redclaw::capture::EncoderExecutionSession session;
    redclaw::capture::EncoderBackendBridgePlan resolved_plan;
    std::string error;

    const bool started = redclaw::capture::start_encoder_execution_from_bridge(
        profile_request,
        bridge_request,
        &session,
        &resolved_plan,
        &error);

    if (!started) {
        return expect_true(!error.empty(), "resized-source start failure should include error detail");
    }

    bool ok = true;
    redclaw::capture::EncodedFramePacket packet;
    const redclaw::capture::CapturedFrame frame = make_test_bgra_frame(1280, 720);
    std::string encode_error;
    const bool encoded = session.encode_bgra_frame(frame, 1000, &packet, &encode_error);
    const auto diagnostics = session.diagnostics();

    ok = expect_true(
        diagnostics.last_failure != redclaw::capture::EncoderExecutionFailureCategory::kFrameSizeMismatch,
        "resized source frame should be handled by the encoder conversion path") && ok;
    ok = expect_true(
        diagnostics.input_mode.find("scale") != std::string::npos,
        "resized source frame should report a scaling input mode") && ok;
    ok = expect_true(
        !diagnostics.hardware_frame_input_active,
        "resized CPU-backed test frame should stay on CPU input mode") && ok;

    if (encoded) {
        ok = expect_true(!packet.payload.empty(), "resized-source encoded payload should be non-empty") && ok;
    } else {
        ok = expect_true(!encode_error.empty(), "resized-source encode failure should include detail") && ok;
    }

    session.stop();
    return ok;
}

bool test_encoder_execution_bridge_falls_back_to_software() {
    const ScopedEnvironmentValue disable_nvenc("REDCLAW_DISABLE_NVENC", "1");
    const ScopedEnvironmentValue disable_quicksync("REDCLAW_DISABLE_QSV", "1");
    const ScopedEnvironmentValue disable_amf("REDCLAW_DISABLE_AMF", "1");

    redclaw::capture::EncoderProfileRequest profile_request;
    profile_request.width = 640;
    profile_request.height = 360;
    profile_request.fps = 24;
    profile_request.preferred_codec = redclaw::capture::EncoderCodec::kH264;

    redclaw::capture::EncoderConfigProfile profile;
    std::string error;
    if (!redclaw::capture::build_low_latency_encoder_profile(profile_request, &profile, &error)) {
        return expect_true(false, std::string("software fallback profile should be valid: ") + error);
    }

    redclaw::capture::EncoderBackendBridgeRequest bridge_request;
    bridge_request.preferred_backend = redclaw::capture::EncoderBackendType::kNvenc;
    bridge_request.allow_hardware_fallback = true;

    redclaw::capture::EncoderExecutionSession session;
    redclaw::capture::EncoderBackendBridgePlan resolved_plan;
    const bool started = redclaw::capture::start_encoder_execution_from_bridge(
        profile,
        bridge_request,
        &session,
        &resolved_plan,
        &error);

    bool ok = true;
    ok = expect_true(started, std::string("software fallback should start: ") + error) && ok;
    if (started) {
        const auto diagnostics = session.diagnostics();
        ok = expect_true(
            resolved_plan.selected_backend == redclaw::capture::EncoderBackendType::kSoftware,
            "disabled hardware backends should resolve to software") && ok;
        ok = expect_true(resolved_plan.fallback_applied, "software selection should be marked as fallback") && ok;
        ok = expect_true(
            diagnostics.backend == redclaw::capture::EncoderBackendType::kSoftware,
            "running encoder diagnostics should report software backend") && ok;
        ok = expect_true(!diagnostics.encoder_name.empty(), "software encoder name should be observable") && ok;
    }
    session.stop();
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_encoder_execution_bridge_start_flow() && ok;
    ok = test_encoder_execution_accepts_resized_source_frame() && ok;
    ok = test_encoder_execution_bridge_falls_back_to_software() && ok;
    ok = test_encoder_execution_rejects_b_frames() && ok;
    ok = test_encoder_execution_reencodes_static_frame_as_requested_keyframe() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_capture_encoder_execution_session_tests" << '\n';
    return 0;
}
