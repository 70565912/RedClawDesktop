#include "redclaw/render/playback_flow_control.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using namespace redclaw::render;

GpuAdapterCandidate adapter(std::uint32_t index, const char* name, std::uint32_t vendor,
    std::uint32_t outputs, bool owns_monitor = false, bool software = false) {
    GpuAdapterCandidate candidate;
    candidate.enum_index = index;
    candidate.luid = 0x1000U + index;
    candidate.name = name;
    candidate.vendor_id = vendor;
    candidate.output_count = outputs;
    candidate.owns_target_monitor = owns_monitor;
    candidate.software = software;
    return candidate;
}

TEST(GpuAdapterChoice, OptimusLaptopPicksTheIntegratedAdapterThatOwnsTheDisplay) {
    // DXGI may list the output-less dGPU first when it is the preferred GPU.
    const std::vector<GpuAdapterCandidate> adapters{
        adapter(0, "NVIDIA GeForce GTX 960M", 0x10de, 0),
        adapter(1, "Intel(R) HD Graphics 530", 0x8086, 1, true),
        adapter(2, "Microsoft Basic Render Driver", 0x1414, 0, false, true),
    };
    const auto choice = choose_gpu_adapter(adapters);
    ASSERT_TRUE(choice.index.has_value());
    EXPECT_EQ(*choice.index, 1U);
    EXPECT_EQ(choice.reason, "window_monitor_adapter");
}

TEST(GpuAdapterChoice, WindowOnSecondAdapterMonitorFollowsThatAdapter) {
    const std::vector<GpuAdapterCandidate> adapters{
        adapter(0, "Primary", 0x10de, 1),
        adapter(1, "Secondary", 0x1002, 2, true),
    };
    EXPECT_EQ(choose_gpu_adapter(adapters).index, std::optional<std::size_t>(1U));
}

TEST(GpuAdapterChoice, WithoutMonitorMatchFallsBackToFirstAdapterWithOutputs) {
    const std::vector<GpuAdapterCandidate> adapters{
        adapter(0, "Headless dGPU", 0x10de, 0),
        adapter(1, "iGPU", 0x8086, 1),
        adapter(2, "Other", 0x1002, 1),
    };
    const auto choice = choose_gpu_adapter(adapters);
    EXPECT_EQ(choice.index, std::optional<std::size_t>(1U));
    EXPECT_EQ(choice.reason, "first_adapter_with_outputs");
}

TEST(GpuAdapterChoice, NeverPicksOutputLessOrSoftwareAdaptersImplicitly) {
    const std::vector<GpuAdapterCandidate> adapters{
        adapter(0, "Headless dGPU", 0x10de, 0),
        adapter(1, "Microsoft Basic Render Driver", 0x1414, 1, true, true),
    };
    const auto choice = choose_gpu_adapter(adapters);
    EXPECT_FALSE(choice.index.has_value());
    EXPECT_EQ(choice.reason, "system_default_no_adapter_with_outputs");
    EXPECT_FALSE(choose_gpu_adapter({}).index.has_value());
}

TEST(GpuAdapterChoice, DescriptionCarriesNameVendorAndLuidWithoutQuotesOrNewlines) {
    auto candidate = adapter(3, "Bad\"Name\n", 0x8086, 1, true);
    candidate.device_id = 0x191b;
    candidate.luid = (std::uint64_t{0x1} << 32U) | 0xabcdU;
    const auto text = describe_gpu_adapter(candidate);
    EXPECT_NE(text.find("name=\"Bad_Name_\""), std::string::npos) << text;
    EXPECT_NE(text.find("vendor=0x8086"), std::string::npos) << text;
    EXPECT_NE(text.find("device=0x191b"), std::string::npos) << text;
    EXPECT_NE(text.find("luid=0x00000001:0000abcd"), std::string::npos) << text;
    EXPECT_NE(text.find("index=3"), std::string::npos) << text;
}

TEST(KeyframeRequestLimiter, FirstRequestIsImmediateAndBurstsCoalesce) {
    KeyframeRequestLimiter limiter(500);
    EXPECT_TRUE(limiter.request(1000));
    // A gap marks every following dependent frame; all of them collapse.
    for (std::uint64_t now = 1016; now < 1500; now += 16) EXPECT_FALSE(limiter.request(now));
    EXPECT_TRUE(limiter.pending());
    EXPECT_FALSE(limiter.poll(1499));
    EXPECT_TRUE(limiter.poll(1500));
    EXPECT_FALSE(limiter.poll(1600));
    EXPECT_EQ(limiter.sent(), 2U);
    EXPECT_EQ(limiter.suppressed(), 31U);
}

TEST(KeyframeRequestLimiter, KeyframeArrivalCancelsPendingRequest) {
    KeyframeRequestLimiter limiter(500);
    EXPECT_TRUE(limiter.request(0));
    EXPECT_FALSE(limiter.request(100));
    limiter.satisfied();
    EXPECT_FALSE(limiter.poll(600));
    // A later gap still asks again, but never faster than the interval.
    EXPECT_TRUE(limiter.request(700));
    EXPECT_FALSE(limiter.request(800));
    EXPECT_EQ(limiter.sent(), 2U);
}

TEST(KeyframeRequestLimiter, LostKeyframeIsRequestedAgainAtTheIntervalWhileStillWaiting) {
    KeyframeRequestLimiter limiter(400);
    std::uint64_t sent = 0;
    for (std::uint64_t now = 0; now <= 2000; now += 20) {
        sent += limiter.request(now) ? 1U : 0U;
        sent += limiter.poll(now) ? 1U : 0U;
    }
    EXPECT_EQ(sent, 6U);
    EXPECT_EQ(limiter.sent(), 6U);
}

TEST(KeyframeRequestLimiter, ResetAllowsImmediateRequestForNewSession) {
    KeyframeRequestLimiter limiter(500);
    EXPECT_TRUE(limiter.request(100));
    EXPECT_FALSE(limiter.request(110));
    limiter.reset();
    EXPECT_FALSE(limiter.pending());
    EXPECT_TRUE(limiter.request(120));
}

TEST(PresentationGate, HiddenOrDisplayOffSkipsEveryPresent) {
    PresentationGate gate(250);
    EXPECT_EQ(gate.decide(0), PresentationGate::Decision::kPresent);
    gate.set_hidden(true);
    EXPECT_EQ(gate.decide(10), PresentationGate::Decision::kSkip);
    EXPECT_EQ(gate.state_name(), "hidden");
    gate.set_hidden(false);
    gate.set_display_on(false);
    EXPECT_EQ(gate.decide(1000), PresentationGate::Decision::kSkip);
    EXPECT_EQ(gate.state_name(), "display_off");
    gate.set_display_on(true);
    EXPECT_EQ(gate.decide(1010), PresentationGate::Decision::kPresent);
    EXPECT_EQ(gate.skipped(), 2U);
}

TEST(PresentationGate, OccludedSwapChainIsProbedAtLowRateUntilVisible) {
    PresentationGate gate(250);
    gate.on_present_result(true, 1000);
    EXPECT_EQ(gate.state_name(), "occluded");
    int probes = 0;
    for (std::uint64_t now = 1016; now < 2000; now += 16) {
        if (gate.decide(now) == PresentationGate::Decision::kTestOcclusion) {
            ++probes;
            gate.on_present_result(true, now);
        }
    }
    EXPECT_EQ(probes, 3);
    EXPECT_EQ(gate.decide(2300), PresentationGate::Decision::kTestOcclusion);
    gate.on_present_result(false, 2300);
    EXPECT_EQ(gate.decide(2316), PresentationGate::Decision::kPresent);
}

TEST(SlowCallReporter, ReportsCallsAboveThresholdWithRateLimit) {
    SlowCallReporter reporter(50000, 1000);
    EXPECT_FALSE(reporter.observe("Present", 50000, 0).has_value());
    auto report = reporter.observe("ResizeBuffers", 460000, 10);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->call, "ResizeBuffers");
    EXPECT_EQ(report->elapsed_us, 460000U);
    EXPECT_EQ(report->suppressed, 0U);
    EXPECT_FALSE(reporter.observe("GetBuffer", 470000, 500).has_value());
    EXPECT_FALSE(reporter.observe("Present", 60000, 900).has_value());
    report = reporter.observe("CreateSwapChainForHwnd", 80000, 1010);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->suppressed, 2U);
    EXPECT_EQ(reporter.slow_calls(), 4U);
    EXPECT_EQ(reporter.max_elapsed_us(), 470000U);
}

}  // namespace
