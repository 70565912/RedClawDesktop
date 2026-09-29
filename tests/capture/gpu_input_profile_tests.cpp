#include <gtest/gtest.h>
#include "redclaw/capture/capture_module.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <algorithm>

#ifdef _WIN32
#include "capture_d3d11.h"
#include <d3d11_4.h>
using namespace redclaw::capture;
namespace {
struct ProfileCase { bool concurrent; std::uint32_t acquire_timeout_ms; };
class GpuInputProfile : public ::testing::TestWithParam<ProfileCase> {};
}

// Developer-invoked diagnostic, excluded from CTest. Run with a moving desktop.
// No network, main-app replacement, timing threshold or changed encoder quality.
TEST_P(GpuInputProfile, RealDesktopStageTimings) {
    using Clock = std::chrono::steady_clock;
    WindowsCaptureSession capture;
    CaptureSessionConfig config;
    config.preferred_backend = CaptureBackendType::kDesktopDuplication;
    config.fallback_enabled = false;
    config.frame_delivery = CaptureFrameDelivery::kCpuAndGpu;
    config.frame_acquire_timeout_ms = GetParam().acquire_timeout_ms;
    std::string error;
    ASSERT_TRUE(capture.start(config, &error)) << error;
    std::mutex mutex;
    std::condition_variable ready;
    std::shared_ptr<CapturedFrame> latest;
    std::atomic<std::uint64_t> capture_failures = 0, captured = 0, timeouts = 0;
    auto acquire = [&]() -> std::shared_ptr<CapturedFrame> {
        auto frame = capture.acquireFrame();
        if (!frame) return {};
        std::string capture_error;
        if (capture.captureFrame(frame.get(), &capture_error)) { ++captured; return frame; }
        if (capture_error == "timeout waiting for desktop frame") ++timeouts;
        else ++capture_failures;
        return {};
    };
    std::jthread producer;
    if (GetParam().concurrent) producer = std::jthread([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            auto frame = acquire();
            if (frame) {
                { std::lock_guard lock(mutex); latest = std::move(frame); }
                ready.notify_one();
            } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    EncoderExecutionSession encoder;
    bool started = false, measuring = false;
    EncoderExecutionDiagnostics before;
    std::uint64_t attempts = 0, outputs = 0;
    const auto start = Clock::now();
    auto measured_start = start;
    while (Clock::now() - start < std::chrono::seconds(11)) {
        std::shared_ptr<CapturedFrame> frame;
        if (GetParam().concurrent) {
            std::unique_lock lock(mutex);
            ready.wait_for(lock, std::chrono::milliseconds(100), [&] { return latest != nullptr; });
            frame = std::move(latest);
        } else frame = acquire();
        if (!frame) continue;
        ASSERT_EQ(frame->width, 1920U); ASSERT_EQ(frame->height, 1080U);
        if (!started) {
            EncoderProfileRequest request;
            request.width = 1778; request.height = 1000; request.fps = 30;
            EncoderConfigProfile profile;
            ASSERT_TRUE(build_desktop_encoder_profile(request, &profile, &error)) << error;
            profile.target_bitrate_kbps = 4267; profile.max_bitrate_kbps = 5120;
            EncoderBackendBridgePlan plan;
            plan.selected_backend = EncoderBackendType::kNvenc;
            plan.allow_hardware_frame_input = true;
            ASSERT_TRUE(encoder.start(profile, plan, &error)) << error;
            started = true;
        }
        if (!measuring && Clock::now() - start >= std::chrono::seconds(3)) {
            before = encoder.diagnostics(); measured_start = Clock::now(); measuring = true;
        }
        EncodedFramePacket packet;
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count();
        const bool output = encoder.encode_bgra_frame(*frame, static_cast<std::uint64_t>(timestamp), &packet, &error);
        ASSERT_TRUE(output || encoder.diagnostics().last_failure == EncoderExecutionFailureCategory::kOutputNotReady) << error;
        ASSERT_EQ(encoder.diagnostics().hardware_input_fallback_count, 0U) << encoder.diagnostics().hardware_input_block_reason;
        capture.configureFrameDelivery(encoder.diagnostics().requested_capture_delivery);
        if (measuring) { ++attempts; if (output) ++outputs; }
    }
    const double seconds = std::chrono::duration<double>(Clock::now()-measured_start).count();
    producer.request_stop(); if (producer.joinable()) producer.join();
    ASSERT_TRUE(measuring); ASSERT_GT(attempts, 0U);
    const auto after = encoder.diagnostics();
    EXPECT_TRUE(after.hardware_frame_input_confirmed);
    EXPECT_EQ(capture_failures.load(), 0U);
    const auto& a = after.gpu_input_timing;
    const auto& b = before.gpu_input_timing;
    RecordProperty("schema", "redclaw.gpu-input-profile.v1");
    RecordProperty("concurrent", GetParam().concurrent ? 1 : 0);
    RecordProperty("acquire_timeout_ms", GetParam().acquire_timeout_ms);
    RecordProperty("attempts", std::to_string(attempts));
    RecordProperty("sent_to_codec_fps", std::to_string(outputs/seconds));
    RecordProperty("captured", std::to_string(captured.load()));
    RecordProperty("timeouts", std::to_string(timeouts.load()));
    auto record = [&](const char* name, std::uint64_t end, std::uint64_t begin) {
        RecordProperty(name, std::to_string(static_cast<double>(end-begin)/attempts/1000.0));
    };
    record("prepare_ms", after.total_input_prepare_us, before.total_input_prepare_us);
    record("send_ms", after.total_send_frame_us, before.total_send_frame_us);
    record("receive_ms", after.total_receive_packet_us, before.total_receive_packet_us);
    record("encode_ms", after.total_encode_us, before.total_encode_us);
    record("scale_ms", a.scale_us, b.scale_us);
    record("scale_lock_ms", a.scale_lock_wait_us, b.scale_lock_wait_us);
    record("scale_setup_ms", a.scale_setup_us, b.scale_setup_us);
    record("scale_view_ms", a.scale_input_view_us, b.scale_input_view_us);
    record("scale_state_ms", a.scale_state_us, b.scale_state_us);
    record("scale_blt_ms", a.scale_blt_us, b.scale_blt_us);
    record("release_ms", a.frame_release_us, b.frame_release_us);
    record("pool_ms", a.frame_pool_us, b.frame_pool_us);
    record("map_ms", a.frame_map_us, b.frame_map_us);
    record("copy_lock_ms", a.copy_lock_wait_us, b.copy_lock_wait_us);
    record("copy_submit_ms", a.copy_submit_us, b.copy_submit_us);
    encoder.stop(); capture.stop();
}

INSTANTIATE_TEST_SUITE_P(LocalDiagnostic, GpuInputProfile,
    ::testing::Values(ProfileCase{false, 50}, ProfileCase{true, 50}, ProfileCase{true, 0}));

class DdaDeviceWaitProfile : public ::testing::TestWithParam<std::uint32_t> {};
TEST_P(DdaDeviceWaitProfile, ProtectedDeviceEntryWhileAcquiring) {
    using Clock = std::chrono::steady_clock;
    WindowsCaptureSession capture;
    CaptureSessionConfig config;
    config.preferred_backend = CaptureBackendType::kDesktopDuplication;
    config.fallback_enabled = false; config.frame_delivery = CaptureFrameDelivery::kGpu;
    config.frame_acquire_timeout_ms = GetParam();
    std::string error;
    ASSERT_TRUE(capture.start(config, &error)) << error;
    auto first = capture.acquireFrame();
    ASSERT_TRUE(first);
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!capture.captureFrame(first.get(), &error) && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(first->native_handle) << error;
    auto owner = first->native_handle->owner;
    Microsoft::WRL::ComPtr<ID3D11Multithread> protection;
    ASSERT_TRUE(SUCCEEDED(owner->context.As(&protection)));
    ASSERT_TRUE(protection->GetMultithreadProtected());
    first.reset();
    std::atomic<unsigned> captured = 0, failed = 0;
    std::jthread producer([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            auto frame = capture.acquireFrame();
            std::string detail;
            if (frame && capture.captureFrame(frame.get(), &detail)) ++captured;
            else {
                if (!detail.empty() && detail != "timeout waiting for desktop frame") ++failed;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });
    std::vector<double> waits;
    const auto start = Clock::now();
    while (Clock::now() - start < std::chrono::seconds(5)) {
        {
            // Exclude our mutex wait. This measures the protected D3D11 entry itself.
            std::lock_guard lock(owner->mutex);
            const auto entry = Clock::now();
            protection->Enter();
            const auto acquired = Clock::now();
            protection->Leave();
            waits.push_back(std::chrono::duration<double, std::milli>(acquired-entry).count());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    producer.request_stop(); producer.join();
    EXPECT_EQ(failed.load(), 0U); EXPECT_GT(captured.load(), 0U);
    ASSERT_FALSE(waits.empty());
    std::sort(waits.begin(), waits.end());
    double sum = 0; for (double value : waits) sum += value;
    RecordProperty("schema", "redclaw.d3d11-entry-profile.v1");
    RecordProperty("acquire_timeout_ms", GetParam());
    RecordProperty("samples", std::to_string(waits.size()));
    RecordProperty("entry_mean_ms", std::to_string(sum/waits.size()));
    RecordProperty("entry_p95_ms", std::to_string(waits[(waits.size()*95+99)/100-1]));
    RecordProperty("entry_max_ms", std::to_string(waits.back()));
    capture.stop();
}
INSTANTIATE_TEST_SUITE_P(LocalDiagnostic, DdaDeviceWaitProfile, ::testing::Values(50U, 0U));
#endif
