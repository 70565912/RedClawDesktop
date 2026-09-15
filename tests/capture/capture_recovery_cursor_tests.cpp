#include <gtest/gtest.h>
#include "redclaw/capture/capture_recovery.h"
#include "redclaw/capture/capture_cursor.h"
#include "redclaw/capture/capture_stream_gate.h"
#include "playback/capture_playback_state.h"
#include <deque>
#include <chrono>
#ifdef _WIN32
#include "capture_cursor_d3d11.h"
#include "capture_backend.h"
#endif
using namespace redclaw::capture;

TEST(CaptureDesktopProbe, AccessOnlyKeepsTheSamePermissionChecksWithoutEnumeratingDisplays) {
    const auto full = probe_capture_desktop();
    const auto access = probe_capture_desktop(CaptureProbeDetail::kAccessOnly);
    EXPECT_EQ(full.access, access.access);
    EXPECT_EQ(full.session_id, access.session_id);
    EXPECT_EQ(full.token_restricted, access.token_restricted);
    EXPECT_EQ(full.token_app_container, access.token_app_container);
    EXPECT_EQ(access.display_signature, 0U);
    const auto measure = [](CaptureProbeDetail detail) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) (void)probe_capture_desktop(detail);
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 20.0;
    };
    RecordProperty("full_probe_mean_us", std::to_string(measure(CaptureProbeDetail::kWithDisplayState)));
    RecordProperty("access_probe_mean_us", std::to_string(measure(CaptureProbeDetail::kAccessOnly)));
}

#ifdef _WIN32
TEST(CaptureHardwareRecovery, NativeEncoderSurvivesCaptureDeviceRecreation) {
    if (probe_capture_desktop().access != CaptureDesktopAccess::kOrdinary) {
        GTEST_SKIP() << "An ordinary interactive desktop is required";
    }
    WindowsCaptureSession capture;
    CaptureSessionConfig config;
    config.preferred_backend = CaptureBackendType::kDesktopDuplication;
    std::string error;
    if (!capture.start(config, &error)) { GTEST_SKIP() << error; }
    capture.configureNativeFrameDelivery(true, false);
    auto next_frame = [&capture, &error](CapturedFrame* frame) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (capture.captureFrame(frame, &error)) { return true; }
        }
        return false;
    };
    CapturedFrame first;
    ASSERT_TRUE(next_frame(&first)) << error;
    if (!first.native_handle) { GTEST_SKIP() << "Native D3D11 capture unavailable"; }
    EncoderProfileRequest profile;
    profile.width = first.width; profile.height = first.height; profile.fps = 30;
    EncoderBackendBridgeRequest bridge;
    bridge.allow_hardware_frame_input = true;
    bridge.capture_adapter_vendor = detect_captured_frame_adapter_vendor(first);
    EncoderExecutionSession encoder;
    EncoderBackendBridgePlan plan;
    if (!start_encoder_execution_from_bridge(profile, bridge, &encoder, &plan, &error)) {
        GTEST_SKIP() << error;
    }
    EncodedFramePacket packet;
    ASSERT_TRUE(encoder.encode_bgra_frame(first, 1000, &packet, &error)) << error;
    if (!encoder.diagnostics().hardware_frame_input_active) {
        GTEST_SKIP() << "Native encoder input unavailable: " << encoder.diagnostics().hardware_input_block_reason;
    }
    ASSERT_FALSE(packet.payload.empty());
    first = {};
    // Keep the encoder alive while the capture backend and its D3D11 device
    // are destroyed. This is the resource ownership boundary used by recovery.
    capture.stop();
    ASSERT_TRUE(capture.start(config, &error)) << error;
    capture.configureNativeFrameDelivery(true, false);
    CapturedFrame recovered;
    ASSERT_TRUE(next_frame(&recovered)) << error;
    ASSERT_TRUE(recovered.native_handle);
    ASSERT_TRUE(encoder.encode_bgra_frame(recovered, 2000, &packet, &error)) << error;
    EXPECT_TRUE(encoder.diagnostics().hardware_frame_input_active);
    EXPECT_FALSE(packet.payload.empty());
    EXPECT_TRUE(packet.keyframe);
}
#endif
TEST(CaptureRecovery, TimeoutNeverTriggersRebuild) {
    CaptureRecoveryPolicy policy;
    EXPECT_EQ(policy.failure(make_capture_failure(CaptureFailureStage::kAcquireFrame, 0x887A0027U),
        CaptureDesktopAccess::kOrdinary, true, 999, 5), CaptureRecoveryAction::kWait);
}
TEST(CaptureRecovery, AccessLostRebuildIsBoundedUntilRealFrame) {
    CaptureRecoveryPolicy policy;
    const auto lost = make_capture_failure(CaptureFailureStage::kAcquireFrame, 0x887A0026U);
    EXPECT_EQ(policy.failure(lost, CaptureDesktopAccess::kOrdinary, true, 1, 5), CaptureRecoveryAction::kRebuildDda);
    EXPECT_EQ(policy.failure(lost, CaptureDesktopAccess::kOrdinary, true, 1, 5), CaptureRecoveryAction::kFallback);
    policy.frame_captured();
    EXPECT_EQ(policy.failure(lost, CaptureDesktopAccess::kOrdinary, true, 1, 5), CaptureRecoveryAction::kRebuildDda);
}
TEST(CaptureRecovery, DeviceLostAndRawHresultArePreserved) {
    CaptureRecoveryPolicy policy;
    const auto removed = make_capture_failure(CaptureFailureStage::kReadback, 0x887A0005U);
    EXPECT_EQ(removed.hresult, 0x887A0005U);
    EXPECT_EQ(removed.kind, CaptureFailureKind::kDeviceLost);
    EXPECT_EQ(policy.failure(removed, CaptureDesktopAccess::kOrdinary, true, 1, 5), CaptureRecoveryAction::kRebuildDda);
}
TEST(CaptureRecovery, UnknownSecureAndDisconnectedNeverFallback) {
    for (auto access : {CaptureDesktopAccess::kUnknown, CaptureDesktopAccess::kDenied, CaptureDesktopAccess::kDisconnected}) {
        CaptureRecoveryPolicy policy;
        EXPECT_EQ(policy.failure(make_capture_failure(CaptureFailureStage::kDuplicateOutput, 0x80070005U),
            access, true, 5, 5), CaptureRecoveryAction::kPause);
    }
}
TEST(CaptureRecovery, OtherErrorsRetainThresholdAndWgcDoesNotSelectDda) {
    CaptureRecoveryPolicy policy;
    const auto failure = make_capture_failure(CaptureFailureStage::kAcquireFrame, 0x80004005U);
    EXPECT_EQ(policy.failure(failure, CaptureDesktopAccess::kOrdinary, false, 4, 5), CaptureRecoveryAction::kWait);
    EXPECT_EQ(policy.failure(failure, CaptureDesktopAccess::kOrdinary, false, 5, 5), CaptureRecoveryAction::kFallback);
}
TEST(CaptureCursor, MonochromePreserveBlackWhiteAndInvert) {
    CaptureCursorShape shape;
    const std::uint8_t masks[] = {0x90, 0x30}; // AND 1001; XOR 0011.
    ASSERT_TRUE(shape.update(CaptureCursorKind::kMonochrome, 4, 2, 1, masks));
    std::vector<std::uint8_t> pixels(16, 0x35);
    composite_capture_cursor(shape, {0, 0, true, 0}, pixels, 4, 1, 16);
    EXPECT_EQ(pixels[0], 0x35); EXPECT_EQ(pixels[4], 0); EXPECT_EQ(pixels[8], 255); EXPECT_EQ(pixels[12], 0xCA);
}
TEST(CaptureCursor, MaskedColorAndNegativeOriginClipping) {
    CaptureCursorShape shape;
    const std::uint8_t cursor[] = {0xFF, 0, 0, 255, 1, 2, 3, 0};
    ASSERT_TRUE(shape.update(CaptureCursorKind::kMaskedColor, 2, 1, 8, cursor));
    std::vector<std::uint8_t> pixels(8, 0x35);
    composite_capture_cursor(shape, {-1, 0, true, 0}, pixels, 2, 1, 8);
    EXPECT_EQ(pixels[0], 1); EXPECT_EQ(pixels[1], 2); EXPECT_EQ(pixels[2], 3); EXPECT_EQ(pixels[4], 0x35);
    composite_capture_cursor(shape, {0, 0, true, 0}, pixels, 2, 1, 8);
    EXPECT_EQ(pixels[0], 254);
}
TEST(CaptureCursor, ColorAlphaHiddenAndRotated) {
    CaptureCursorShape shape;
    const std::uint8_t cursor[] = {100, 0, 0, 255, 200, 0, 0, 255};
    ASSERT_TRUE(shape.update(CaptureCursorKind::kColor, 2, 1, 8, cursor));
    std::vector<std::uint8_t> pixels(16, 0);
    composite_capture_cursor(shape, {0, 0, false, 0}, pixels, 2, 2, 8);
    EXPECT_EQ(pixels[0], 0);
    composite_capture_cursor(shape, {0, 0, true, 90}, pixels, 2, 2, 8);
    EXPECT_EQ(pixels[0], 200); EXPECT_EQ(pixels[8], 100);
    const auto revision = shape.revision;
    EXPECT_FALSE(shape.update(CaptureCursorKind::kMonochrome, 8, 3, 1, cursor));
    EXPECT_EQ(shape.revision, revision);
}

TEST(CaptureRecovery, OldFramesAreRejectedAndRecoveryRequiresPresentation) {
    CaptureStreamGate gate;
    EXPECT_TRUE(gate.update(CaptureAvailability::kRunning, 1));
    EXPECT_TRUE(gate.submitted_keyframe(1, 10));
    EXPECT_FALSE(gate.presented(9));
    EXPECT_TRUE(gate.presented(10));
    EXPECT_TRUE(gate.update(CaptureAvailability::kPaused, 1));
    EXPECT_FALSE(gate.accepts(1));
    EXPECT_FALSE(gate.presented(100));
    EXPECT_TRUE(gate.update(CaptureAvailability::kRecovering, 2));
    EXPECT_FALSE(gate.accepts(1));
    EXPECT_FALSE(gate.update(CaptureAvailability::kRunning, 2));
    EXPECT_FALSE(gate.snapshot().presented);
    EXPECT_FALSE(gate.submitted_keyframe(1, 101));
    EXPECT_TRUE(gate.submitted_keyframe(2, 102));
    EXPECT_FALSE(gate.presented(101));
    EXPECT_TRUE(gate.presented(102));
}

TEST(CaptureRecovery, FastRecoveryLatchesInputPauseAndRejectsInflightSubmission) {
    CaptureStreamGate gate;
    gate.update(CaptureAvailability::kRunning, 1);
    const auto consumed = gate.snapshot().input_pause_revision;
    gate.update(CaptureAvailability::kRecovering, 2);
    gate.update(CaptureAvailability::kRunning, 2);
    EXPECT_GT(gate.snapshot().input_pause_revision, consumed);
    bool submitted = false;
    EXPECT_FALSE(gate.submit(1, [&] { submitted = true; }));
    EXPECT_FALSE(submitted);
    EXPECT_TRUE(gate.submit(2, [&] { submitted = true; }));
    EXPECT_TRUE(submitted);
}

TEST(CapturePlayback, LegacyAndUnknownCapabilitiesKeepExistingBehavior) {
    redclaw::ui::CapturePlaybackState state;
    redclaw::protocol::StreamControlMessageV1 message;
    state.observe(message);
    EXPECT_FALSE(state.waiting());
    EXPECT_TRUE(state.control_allowed());
    message.capture_status_version = 2;
    message.capture_status = 3;
    message.capture_generation = 1;
    state.observe(message);
    EXPECT_FALSE(state.waiting());
}

TEST(CapturePlayback, OldFramesAndDelayedActiveStatusCannotRearmAfterRecovery) {
    redclaw::ui::CapturePlaybackState state;
    redclaw::protocol::StreamControlMessageV1 message;
    message.capture_status_version = 1;
    message.capture_status = 3;
    message.capture_generation = 2;
    state.observe(message);
    state.presented(15);
    EXPECT_TRUE(state.waiting());
    EXPECT_FALSE(state.request_control());
    message.capture_status = 1;
    message.capture_first_frame_id = 20;
    state.observe(message);
    state.presented(19);
    EXPECT_TRUE(state.waiting());
    state.presented(20);
    EXPECT_FALSE(state.waiting());
    EXPECT_FALSE(state.control_allowed());
    EXPECT_TRUE(state.request_control());
    EXPECT_TRUE(state.control_allowed());
    message.capture_status = 3;
    message.capture_generation = 1;
    state.observe(message);
    EXPECT_FALSE(state.waiting());
}

#ifdef _WIN32
namespace {
struct BackendStep {
    CaptureBackendType type;
    std::uint32_t start_hr = 0;
    std::deque<std::uint32_t> frames;
};
class ScriptedCaptureBackend final : public ICaptureBackend {
public:
    ScriptedCaptureBackend(BackendStep step, int& alive) : step_(std::move(step)), alive_(alive) { ++alive_; }
    ~ScriptedCaptureBackend() override { --alive_; }
    bool start(const CaptureSessionConfig&, std::string* error) override {
        record_failure(CaptureFailureStage::kStartBackend, step_.start_hr);
        if (step_.start_hr && error) { *error = "controlled start failure"; }
        running_ = step_.start_hr == 0;
        return running_;
    }
    bool capture_frame(CapturedFrame* frame, CaptureFrameStageTelemetry* stage, std::string* error) override {
        const auto hr = step_.frames.empty() ? 0U : step_.frames.front();
        if (!step_.frames.empty()) { step_.frames.pop_front(); }
        record_failure(CaptureFailureStage::kAcquireFrame, hr);
        stage->timeout = hr == 0x887A0027U;
        if (hr) { if (error) { *error = "controlled capture failure"; } return false; }
        frame->width = 2; frame->height = 2; frame->row_pitch = 8;
        frame->data.assign(16, 0); frame->bgra = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool is_running() const override { return running_; }
    CaptureBackendType backend_type() const override { return step_.type; }
private:
    BackendStep step_;
    int& alive_;
    bool running_ = false;
};
class CaptureSessionRecovery : public ::testing::Test {
protected:
    std::deque<BackendStep> steps;
    std::vector<CaptureBackendType> attempts;
    int alive = 0;
    CaptureDesktopContext desktop{.access = CaptureDesktopAccess::kOrdinary};
    std::chrono::steady_clock::time_point now{};
    WindowsCaptureSession session;
    CapturedFrame frame;
    static constexpr auto dda = CaptureBackendType::kDesktopDuplication;
    static constexpr auto wgc = CaptureBackendType::kWindowsGraphicsCapture;
    static constexpr auto gdi = CaptureBackendType::kGdiBitBlt;
    void SetUp() override {
        CaptureSessionTestAccess::install(session, {
            .backend = [&](CaptureBackendType type) -> std::unique_ptr<ICaptureBackend> {
                EXPECT_EQ(alive, 0) << "Old backend must be released before constructing the next";
                attempts.push_back(type);
                if (steps.empty()) { ADD_FAILURE() << "Unexpected backend retry"; return std::make_unique<ScriptedCaptureBackend>(BackendStep{type, 0x80004005U}, alive); }
                auto step = std::move(steps.front()); steps.pop_front();
                EXPECT_EQ(type, step.type);
                return std::make_unique<ScriptedCaptureBackend>(std::move(step), alive);
            },
            .desktop = [&] { return desktop; },
            .clock = [&] { return now; }
        });
    }
    bool capture() { now += std::chrono::milliseconds(100); return session.captureFrame(&frame); }
};
}

TEST_F(CaptureSessionRecovery, TimeoutsStaticAndBlackFramesDoNotRecover) {
    steps = {{dda, 0, {0x887A0027U, 0x887A0027U, 0}}};
    ASSERT_TRUE(session.start({}));
    EXPECT_FALSE(capture()); EXPECT_FALSE(capture());
    for (int i = 0; i < 20; ++i) { EXPECT_TRUE(capture()); }
    EXPECT_EQ(attempts.size(), 1U);
    EXPECT_EQ(session.telemetry().dda_rebuild_count, 0U);
    EXPECT_EQ(session.telemetry().total_timeout_count, 2U);
}
TEST_F(CaptureSessionRecovery, AccessLostRebuildsAndCompletesOnNewFrame) {
    steps = {{dda, 0, {0, 0x887A0026U}}, {dda, 0, {0}}};
    ASSERT_TRUE(session.start({}));
    ASSERT_TRUE(capture()); const auto old_generation = frame.capture_generation;
    EXPECT_FALSE(capture());
    EXPECT_EQ(session.telemetry().availability, CaptureAvailability::kRecovering);
    EXPECT_EQ(session.telemetry().dda_rebuild_count, 1U);
    ASSERT_TRUE(capture());
    EXPECT_GT(frame.capture_generation, old_generation);
    EXPECT_EQ(session.telemetry().availability, CaptureAvailability::kRunning);
}
TEST_F(CaptureSessionRecovery, FailedDdaRebuildFallsBackAndStableWgcStaysSelected) {
    steps = {{dda, 0, {0x887A0005U}}, {dda, 0x80070005U}, {wgc, 0, {0}}};
    ASSERT_TRUE(session.start({})); EXPECT_FALSE(capture());
    EXPECT_EQ(session.activeBackend(), wgc);
    for (int i = 0; i < 100; ++i) { EXPECT_TRUE(capture()); }
    EXPECT_EQ(attempts, (std::vector{dda, dda, wgc}));
}
TEST_F(CaptureSessionRecovery, TransientFailureAlsoInvalidatesOldFrameGeneration) {
    steps = {{dda, 0, {0, 0x80004005U, 0}}};
    ASSERT_TRUE(session.start({})); ASSERT_TRUE(capture());
    const auto old_generation = frame.capture_generation;
    EXPECT_FALSE(capture());
    EXPECT_EQ(session.telemetry().dda_rebuild_count, 0U);
    ASSERT_TRUE(capture());
    EXPECT_GT(frame.capture_generation, old_generation);
    EXPECT_EQ(attempts.size(), 1U);
}
TEST_F(CaptureSessionRecovery, WgcFailureTriesGdi) {
    steps = {{dda, 0x80070005U}, {wgc, 0, {0x80004005U}}, {gdi, 0, {0}}};
    CaptureSessionConfig config; config.max_consecutive_failures_before_fallback = 1;
    ASSERT_TRUE(session.start(config)); EXPECT_FALSE(capture());
    EXPECT_EQ(session.activeBackend(), gdi); EXPECT_TRUE(capture());
    EXPECT_EQ(attempts, (std::vector{dda, wgc, gdi}));
}
TEST_F(CaptureSessionRecovery, AllFailedPauseUntilStateChangeOrExplicitRetry) {
    steps = {{dda, 0x80070005U}, {wgc, 0x80004005U}, {gdi, 0x80004005U}};
    EXPECT_FALSE(session.start({}));
    for (int i = 0; i < 100; ++i) { EXPECT_FALSE(capture()); }
    EXPECT_EQ(attempts.size(), 3U);
    EXPECT_EQ(session.telemetry().total_capture_attempt_count, 0U);
    EXPECT_EQ(session.telemetry().availability, CaptureAvailability::kPaused);
    steps = {{dda, 0, {0}}}; session.retryCapture();
    EXPECT_TRUE(capture());
    desktop.access = CaptureDesktopAccess::kDisconnected;
    now += std::chrono::seconds(1); EXPECT_FALSE(capture());
    EXPECT_EQ(alive, 0);
    steps = {{dda, 0, {0}}}; desktop.access = CaptureDesktopAccess::kOrdinary;
    now += std::chrono::seconds(1); EXPECT_TRUE(capture());
}
TEST_F(CaptureSessionRecovery, SecureAndUnknownDesktopRefuseAllBackendsIncludingRetry) {
    for (const auto access : {CaptureDesktopAccess::kDenied, CaptureDesktopAccess::kUnknown, CaptureDesktopAccess::kDisconnected}) {
        desktop.access = access;
        EXPECT_FALSE(session.start({})); session.retryCapture(); EXPECT_FALSE(capture());
        EXPECT_TRUE(attempts.empty());
    }
}

TEST(CaptureCursor, D3D11MatchesCpuForShapesClippingAndRotation) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const auto created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(created)) { GTEST_SKIP() << "Hardware D3D11 device unavailable hr=" << created; }
    for (auto kind : {CaptureCursorKind::kColor, CaptureCursorKind::kMaskedColor, CaptureCursorKind::kMonochrome}) {
        CaptureCursorShape shape;
        const std::uint8_t color[] = {255, 0, 128, 255, 100, 200, 50, 0, 64, 10, 200, 128, 255, 255, 255, 255};
        const std::uint8_t mono[] = {0x80, 0xC0, 0x40, 0xC0};
        ASSERT_TRUE(shape.update(kind, 2, kind == CaptureCursorKind::kMonochrome ? 4 : 2,
            kind == CaptureCursorKind::kMonochrome ? 1 : 8,
            kind == CaptureCursorKind::kMonochrome ? std::span<const std::uint8_t>(mono) : std::span<const std::uint8_t>(color)));
        CaptureCursorD3D11 gpu;
        for (std::uint32_t rotation : {0U, 90U, 180U, 270U}) {
            for (int position : {-1, 0, 3, 5}) {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = 4; desc.Height = 4; desc.MipLevels = 1; desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                std::vector<std::uint8_t> expected(64, 53);
                D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = expected.data(); initial.SysMemPitch = 16;
                ComPtr<ID3D11Texture2D> frame, readback;
                ASSERT_HRESULT_SUCCEEDED(device->CreateTexture2D(&desc, &initial, &frame));
                CaptureCursorPlacement placement{position, position, true, rotation};
                ASSERT_HRESULT_SUCCEEDED(gpu.composite(device.Get(), context.Get(), frame.Get(), shape, placement));
                composite_capture_cursor(shape, placement, expected, 4, 4, 16);
                desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ASSERT_HRESULT_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &readback));
                context->CopyResource(readback.Get(), frame.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                ASSERT_HRESULT_SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                for (std::size_t y = 0; y < 4; ++y) {
                    const auto* actual = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                    for (std::size_t x = 0; x < 16; ++x) {
                        EXPECT_NEAR(actual[x], expected[y * 16 + x], 1)
                            << "kind=" << static_cast<int>(kind) << " rotation=" << rotation << " position=" << position;
                    }
                }
                context->Unmap(readback.Get(), 0);
            }
        }
    }
}
#endif
