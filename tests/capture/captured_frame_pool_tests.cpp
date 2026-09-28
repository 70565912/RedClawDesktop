#include <gtest/gtest.h>
#include "redclaw/capture/captured_frame_pool.h"
#include <chrono>
#include <thread>
#ifdef _WIN32
#include "capture_backend.h"
#endif

using namespace redclaw::capture;

TEST(CapturedFramePool, ExhaustionWaitsForLastConsumerAndReusesStorage) {
    CapturedFramePool pool;
    auto first = pool.acquire(), pending = pool.acquire(), encoding = pool.acquire();
    ASSERT_TRUE(first); ASSERT_TRUE(pending); ASSERT_TRUE(encoding);
    first->data.resize(1920 * 1080 * 4, 0x5a);
    auto* pixels = first->data.data();
    const auto capacity = first->data.capacity();
    first->capture_generation = 17;
    first->source_region = {.x = 2, .y = 2, .width = 64, .height = 64, .revision = 9};
    std::shared_ptr<const CapturedFrame> consumer = first;
    first.reset();
    EXPECT_FALSE(pool.acquire());
    // Last reader may finish on the encoding thread.
    std::thread([held = std::move(consumer)] {}).join();
    auto reused = pool.acquire();
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused->data.data(), pixels);
    EXPECT_EQ(reused->data.capacity(), capacity);
    EXPECT_EQ(reused->data.size(), capacity);
    EXPECT_EQ(reused->data.front(), 0x5a);
    EXPECT_EQ(reused->capture_generation, 0U);
    EXPECT_EQ(reused->source_region.revision, CaptureRegion{}.revision);
    EXPECT_EQ(pool.stats().exhausted, 1U);
}

TEST(CapturedFramePool, ResizeAndStopDoNotInvalidateOldGeneration) {
    std::shared_ptr<const CapturedFrame> old;
    {
        CapturedFramePool pool;
        auto slot = pool.acquire();
        slot->width = 64; slot->height = 64; slot->capture_generation = 1;
        slot->data.assign(64 * 64 * 4, 0x5a);
        old = slot;
        slot.reset();
        pool.release_unused();
        auto resized = pool.acquire();
        resized->width = 128; resized->height = 64; resized->capture_generation = 2;
        resized->data.assign(128 * 64 * 4, 0xa5);
        EXPECT_NE(resized.get(), old.get());
        EXPECT_EQ(old->data.front(), 0x5a);
        EXPECT_EQ(old->capture_generation, 1U);
        auto third = pool.acquire();
        EXPECT_FALSE(pool.acquire());
        EXPECT_EQ(pool.stats().retained_cpu_bytes, (64U * 64 + 128U * 64) * 4);
    }
    EXPECT_EQ(old->data.size(), 64U * 64 * 4);
    EXPECT_EQ(old->data.back(), 0x5a);
}

TEST(CapturedFramePool, AdapterIdentityDoesNotRequireImageTexture) {
    CapturedFrame frame;
    frame.adapter_identity = std::make_shared<const CaptureAdapterIdentity>(
        CaptureAdapterIdentity{CaptureAdapterVendor::kIntel, "test-adapter"});
    std::string summary;
    EXPECT_EQ(detect_captured_frame_adapter_vendor(frame, &summary), CaptureAdapterVendor::kIntel);
    EXPECT_EQ(summary, "test-adapter");
    EXPECT_FALSE(frame.native_handle);
}

#ifdef _WIN32
namespace {
class DeliveryBackend : public ICaptureBackend {
public:
    explicit DeliveryBackend(std::vector<CaptureFrameDelivery>& observed) : observed_(observed) {}
    bool start(const CaptureSessionConfig&, std::string*) override { return true; }
    bool capture_frame(CapturedFrame* frame, CaptureFrameStageTelemetry*, std::string*) override {
        observed_.push_back(delivery_);
        frame->width = 2; frame->height = 2; frame->row_pitch = 8; frame->bgra = true;
        frame->data.assign(16, 0xa5);
        return true;
    }
    void configure_frame_delivery(CaptureFrameDelivery value) override { delivery_ = value; }
    void stop() override {}
    bool is_running() const override { return true; }
    CaptureBackendType backend_type() const override { return CaptureBackendType::kDesktopDuplication; }
private:
    std::vector<CaptureFrameDelivery>& observed_;
    CaptureFrameDelivery delivery_ = CaptureFrameDelivery::kCpu;
};
}

TEST(CapturedFramePool, DeliveryOverrideIsPerFrameAndPoolSurvivesRestart) {
    std::vector<CaptureFrameDelivery> observed;
    WindowsCaptureSession session;
    CaptureSessionTestAccess::install(session, {
        .backend = [&](CaptureBackendType) { return std::make_unique<DeliveryBackend>(observed); },
        .desktop = [] { return CaptureDesktopContext{.access = CaptureDesktopAccess::kOrdinary}; }
    });
    ASSERT_TRUE(session.start({}));
    auto old = session.acquireFrame();
    ASSERT_TRUE(session.captureFrame(old.get()));
    const auto generation = old->capture_generation;
    session.configureFrameDelivery(CaptureFrameDelivery::kGpu);
    auto second = session.acquireFrame();
    ASSERT_TRUE(session.captureFrame(second.get(), nullptr, true));
    auto third = session.acquireFrame();
    ASSERT_TRUE(session.captureFrame(third.get()));
    EXPECT_EQ(observed, (std::vector<CaptureFrameDelivery>{CaptureFrameDelivery::kCpu,
        CaptureFrameDelivery::kCpuAndGpu, CaptureFrameDelivery::kGpu}));
    session.stop();
    ASSERT_TRUE(session.start({}));
    EXPECT_FALSE(session.acquireFrame());
    second.reset(); third.reset();
    auto next = session.acquireFrame();
    ASSERT_TRUE(next);
    ASSERT_TRUE(session.captureFrame(next.get()));
    EXPECT_GT(next->capture_generation, generation);
    EXPECT_EQ(old->capture_generation, generation);
    EXPECT_EQ(observed.back(), CaptureFrameDelivery::kCpu);
}

// Developer-invoked real desktop gate. Not part of unattended CTest.
class CapturedFramePoolHardware : public ::testing::TestWithParam<CaptureBackendType> {};
TEST_P(CapturedFramePoolHardware, CpuDeliveryReusesThreeSlotsWithoutNativeCopies) {
    WindowsCaptureSession session;
    CaptureSessionConfig config;
    config.preferred_backend = GetParam(); config.fallback_enabled = false;
    config.frame_acquire_timeout_ms = 100;
    std::string error;
    ASSERT_TRUE(session.start(config, &error)) << error;
    std::shared_ptr<CapturedFrame> pending, encoding;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    unsigned captured = 0;
    std::uint64_t warm_allocations = 0;
    while (captured < 60 && std::chrono::steady_clock::now() < deadline) {
        auto frame = session.acquireFrame();
        ASSERT_TRUE(frame);
        if (!session.captureFrame(frame.get(), &error)) {
            ASSERT_EQ(error, "timeout waiting for desktop frame");
            continue;
        }
        ASSERT_FALSE(frame->data.empty()); ASSERT_FALSE(frame->native_handle);
        ASSERT_NE(detect_captured_frame_adapter_vendor(*frame), CaptureAdapterVendor::kUnknown);
        encoding = std::move(pending); pending = std::move(frame);
        ++captured;
        if (captured == 3) warm_allocations = session.telemetry().cpu_buffer_allocation_count;
    }
    ASSERT_EQ(captured, 60U) << "Run a moving real desktop scene for this gate";
    const auto stats = session.telemetry();
    EXPECT_EQ(warm_allocations, 3U);
    EXPECT_EQ(stats.cpu_buffer_allocation_count, warm_allocations);
    EXPECT_EQ(stats.gpu_readback_count, captured);
    EXPECT_EQ(stats.cpu_frame_copy_count, captured);
    EXPECT_EQ(stats.native_frame_copy_count, 0U);
    EXPECT_EQ(stats.native_texture_pool_create_count, 0U);
    EXPECT_EQ(stats.cpu_frame_pool_retained_bytes,
        3ULL * pending->row_pitch * pending->height);
    RecordProperty("frames", captured);
    RecordProperty("cpu_allocations", stats.cpu_buffer_allocation_count);
    RecordProperty("gpu_readbacks", stats.gpu_readback_count);
    RecordProperty("native_copies", stats.native_frame_copy_count);
    RecordProperty("retained_bytes", stats.cpu_frame_pool_retained_bytes);
    for (const auto delivery : {CaptureFrameDelivery::kCpuAndGpu,
                               CaptureFrameDelivery::kGpu, CaptureFrameDelivery::kCpu}) {
        session.configureFrameDelivery(delivery);
        auto frame = session.acquireFrame();
        ASSERT_TRUE(frame);
        bool ready = false;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!ready && std::chrono::steady_clock::now() < until) {
            ready = session.captureFrame(frame.get(), &error);
            if (!ready) ASSERT_EQ(error, "timeout waiting for desktop frame");
        }
        ASSERT_TRUE(ready) << error;
        EXPECT_EQ(frame->data.empty(), delivery == CaptureFrameDelivery::kGpu);
        EXPECT_EQ(frame->native_handle != nullptr, delivery != CaptureFrameDelivery::kCpu);
        encoding = std::move(pending); pending = std::move(frame);
    }
    EXPECT_EQ(session.telemetry().cpu_buffer_allocation_count, warm_allocations);
    EXPECT_EQ(session.telemetry().gpu_readback_count, captured + 2);
    EXPECT_EQ(session.telemetry().native_frame_copy_count, 2U);
}
INSTANTIATE_TEST_SUITE_P(RealDesktop, CapturedFramePoolHardware,
    ::testing::Values(CaptureBackendType::kDesktopDuplication, CaptureBackendType::kWindowsGraphicsCapture));
#endif
