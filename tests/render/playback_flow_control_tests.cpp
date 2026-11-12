#include "redclaw/render/playback_flow_control.h"
#include "redclaw/protocol/hevc_access_unit.h"

#include <array>

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
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

std::vector<GpuAdapterCandidate> optimus_adapters() {
    return {
        adapter(0, "NVIDIA GeForce GTX 960M", 0x10de, 0),
        adapter(1, "Intel(R) HD Graphics 530", 0x8086, 1, true),
        adapter(2, "Microsoft Basic Render Driver", 0x1414, 0, false, true),
    };
}

// Adapters whose index is in `capable` can hardware-decode; every probed index is recorded.
DecodeAdapterProbe fake_probe(std::set<std::uint32_t> capable, std::vector<std::uint32_t>* probed) {
    return [capable = std::move(capable), probed](const GpuAdapterCandidate& candidate) {
        probed->push_back(candidate.enum_index);
        return capable.count(candidate.enum_index) != 0;
    };
}

TEST(DecodeAdapterChoice, DisplayAdapterThatCanDecodeIsUsedAndNothingElseIsProbed) {
    const auto adapters = optimus_adapters();
    std::vector<std::uint32_t> probed;
    const auto choice = choose_decode_adapter(adapters, adapters[1].luid, "hevc", fake_probe({0, 1}, &probed));
    ASSERT_TRUE(choice.index.has_value());
    EXPECT_EQ(*choice.index, 1U);
    EXPECT_EQ(choice.reason, "display_adapter");
    EXPECT_EQ(probed, std::vector<std::uint32_t>({1}));
    ASSERT_EQ(choice.attempts.size(), 1U);
    EXPECT_TRUE(choice.attempts[0].hardware_decode);
}

TEST(DecodeAdapterChoice, DisplayWithoutCodecDoesNotUseAnotherAdapter) {
    const auto adapters = optimus_adapters();
    std::vector<std::uint32_t> probed;
    // The output-less dGPU could decode HEVC. It must not be probed or chosen.
    const auto choice = choose_decode_adapter(adapters, adapters[1].luid, "hevc", fake_probe({0}, &probed));
    EXPECT_FALSE(choice.index.has_value());
    EXPECT_EQ(choice.reason, "display_adapter_no_hevc");
    EXPECT_EQ(probed, std::vector<std::uint32_t>({1}));
    ASSERT_EQ(choice.attempts.size(), 1U);
    EXPECT_FALSE(choice.attempts[0].hardware_decode);
}

TEST(DecodeAdapterChoice, NoAdapterThatCanDecodeMeansNoCodec) {
    const auto adapters = optimus_adapters();
    std::vector<std::uint32_t> probed;
    const auto choice = choose_decode_adapter(adapters, adapters[1].luid, "hevc", fake_probe({}, &probed));
    EXPECT_FALSE(choice.index.has_value());
    EXPECT_EQ(choice.reason, "display_adapter_no_hevc");
    EXPECT_EQ(probed, std::vector<std::uint32_t>({1}));
}

TEST(DecodeAdapterChoice, SoftwareAdapterIsNeverAcceptedEvenIfItClaimsSupport) {
    const auto adapters = optimus_adapters();
    std::vector<std::uint32_t> probed;
    const auto choice = choose_decode_adapter(adapters, adapters[1].luid, "hevc", fake_probe({2}, &probed));
    EXPECT_FALSE(choice.index.has_value());
    EXPECT_EQ(std::count(probed.begin(), probed.end(), 2U), 0);
}

TEST(DecodeAdapterChoice, SoftwareDisplayAdapterIsNotReplacedByAnotherGpu) {
    std::vector<GpuAdapterCandidate> adapters{
        adapter(0, "Microsoft Basic Render Driver", 0x1414, 1, true, true),
        adapter(1, "NVIDIA GeForce GTX 960M", 0x10de, 0),
    };
    std::vector<std::uint32_t> probed;
    const auto choice = choose_decode_adapter(adapters, adapters[0].luid, "hevc", fake_probe({0, 1}, &probed));
    EXPECT_FALSE(choice.index.has_value());
    EXPECT_EQ(choice.reason, "display_adapter_no_hevc");
    EXPECT_TRUE(probed.empty());
}

TEST(DecodeAdapterChoice, UnknownPresentAdapterProbesNothing) {
    const auto adapters = optimus_adapters();
    std::vector<std::uint32_t> probed;
    const auto choice = choose_decode_adapter(adapters, 0, "hevc", fake_probe({0, 1}, &probed));
    EXPECT_FALSE(choice.index.has_value());
    EXPECT_TRUE(probed.empty());
    EXPECT_EQ(choice.reason, "present_adapter_not_enumerated");
}

TEST(DecodeAdapterChoice, PresenterFallbackCannotAuthorizeHevcOnAnOtherwiseCapableGpu) {
    std::vector<GpuAdapterCandidate> adapters{
        adapter(0, "Fallback", 0x8086, 1),
        adapter(1, "Other GPU", 0x10de, 0),
    };
    const auto fallback = choose_gpu_adapter(adapters);
    ASSERT_TRUE(fallback.index);
    const auto& presenter = adapters[*fallback.index];
    std::vector<std::uint32_t> probed;
    const auto probe = fake_probe({0, 1}, &probed);
    EXPECT_FALSE(choose_decode_adapter(adapters, monitor_owned_presenter_luid(&presenter, presenter.luid),
        "hevc", probe).index);
    EXPECT_FALSE(choose_decode_adapter(adapters, monitor_owned_presenter_luid(nullptr, presenter.luid),
        "hevc", probe).index);
    EXPECT_TRUE(probed.empty());

    adapters[0].owns_target_monitor = true;
    EXPECT_EQ(monitor_owned_presenter_luid(&presenter, adapters[1].luid), 0U);
    EXPECT_EQ(choose_decode_adapter(adapters, monitor_owned_presenter_luid(&presenter, presenter.luid),
        "hevc", probe).index, 0U);
    EXPECT_EQ(probed, (std::vector<std::uint32_t>{0}));
}

TEST(DecodeAdapterChoice, EmptyAdapterListAndMissingProbeDecodeNothing) {
    std::vector<std::uint32_t> probed;
    const auto missing_adapter = choose_decode_adapter({}, 0x1001U, "hevc", fake_probe({0}, &probed));
    EXPECT_FALSE(missing_adapter.index.has_value());
    EXPECT_EQ(missing_adapter.reason, "present_adapter_not_enumerated");
    EXPECT_TRUE(probed.empty());
    const auto adapters = optimus_adapters();
    const auto missing_probe = choose_decode_adapter(adapters, adapters[1].luid, "hevc", {});
    EXPECT_FALSE(missing_probe.index.has_value());
    EXPECT_EQ(missing_probe.reason, "display_adapter_no_hevc");
    EXPECT_TRUE(missing_probe.attempts.empty());
}

TEST(DecodeAdapterChoice, ReasonNamesTheCodec) {
    const auto adapters = optimus_adapters();
    std::vector<std::uint32_t> probed;
    EXPECT_EQ(choose_decode_adapter(adapters, adapters[1].luid, "h264", fake_probe({0}, &probed)).reason,
        "display_adapter_no_h264");
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

TEST(KeyframeRequestLimiter, ResumeReasonSurvivesDependencyRequestsUntilDue) {
    for (bool dependency_when_due : {false, true}) {
        KeyframeRequestLimiter limiter(500);
        ASSERT_TRUE(limiter.request(1000, "shared_memory_sequence_gap"));
        EXPECT_FALSE(limiter.request(1200, "presentation_resumed_visibility_timer"));
        EXPECT_FALSE(limiter.request(1400, "latest_frame_missing_keyframe_dependency"));
        limiter.satisfied();
        EXPECT_FALSE(limiter.poll(1499));
        const auto ready = dependency_when_due
            ? limiter.request(1500, "gui_decode_failure")
            : limiter.poll(1500);
        ASSERT_TRUE(ready);
        EXPECT_EQ(*ready, "presentation_resumed_visibility_timer");
        EXPECT_FALSE(limiter.poll(2000));
        EXPECT_EQ(limiter.sent(), 2U);
    }
}

TEST(PresentationGate, WindowCoveragePausesEvenWhenDxgiReportsVisible) {
    PresentationGate gate(250);
    gate.set_window_occluded(true);
    gate.on_present_result(false, 1000);
    EXPECT_EQ(gate.state_name(), "occluded");
    EXPECT_EQ(gate.decide(1250), PresentationGate::Decision::kSkip);
    EXPECT_FALSE(gate.needs_occlusion_probe());
    EXPECT_FALSE(gate.take_resume());

    gate.on_present_result(true, 1300);
    gate.set_window_occluded(false);
    EXPECT_TRUE(gate.needs_occlusion_probe());
    EXPECT_FALSE(gate.take_resume());
    EXPECT_EQ(gate.decide(1550), PresentationGate::Decision::kTestOcclusion);
    gate.on_present_result(false, 1550);
    EXPECT_TRUE(gate.take_resume());
    EXPECT_EQ(gate.decide(1551), PresentationGate::Decision::kPresent);
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

TEST(PresentationGate, RestoreAfterSkippedFramesAsksForOneKeyframe) {
    PresentationGate gate(250);
    EXPECT_FALSE(gate.take_resume());
    // Resume is independent of frame delivery.
    gate.set_hidden(true);
    gate.set_hidden(false);
    EXPECT_TRUE(gate.take_resume());

    gate.set_hidden(true);
    EXPECT_EQ(gate.decide(10), PresentationGate::Decision::kSkip);
    EXPECT_FALSE(gate.take_resume());
    gate.set_hidden(false);
    EXPECT_TRUE(gate.take_resume());
    EXPECT_FALSE(gate.take_resume());

    // Display off, then minimized while still off: resume only when both clear.
    gate.set_display_on(false);
    EXPECT_EQ(gate.decide(20), PresentationGate::Decision::kSkip);
    gate.set_hidden(true);
    gate.set_display_on(true);
    EXPECT_FALSE(gate.take_resume());
    gate.set_hidden(false);
    EXPECT_TRUE(gate.take_resume());
}

TEST(PresentationGate, OcclusionEndingAfterDroppedFramesAsksForKeyframe) {
    PresentationGate gate(250);
    gate.on_present_result(true, 1000);
    EXPECT_EQ(gate.decide(1016), PresentationGate::Decision::kSkip);
    gate.on_present_result(true, 1250);
    EXPECT_FALSE(gate.take_resume());
    gate.on_present_result(false, 1500);
    EXPECT_TRUE(gate.take_resume());
    gate.on_present_result(false, 1516);
    EXPECT_FALSE(gate.take_resume());
}

TEST(PresenterAdapter, SessionStartRebuildsOnlyForAKnownDifferentAdapter) {
    EXPECT_TRUE(presenter_adapter_changed(0x10, 0x20));
    EXPECT_FALSE(presenter_adapter_changed(0x10, 0x10));
    EXPECT_FALSE(presenter_adapter_changed(0, 0x20));
    EXPECT_FALSE(presenter_adapter_changed(0x10, 0));
}

TEST(DecoderHwBackendOrder, PinnedDecodeUsesOnlyThePresenterAdapter) {
    const auto pinned = decoder_hw_backend_order(true);
    ASSERT_EQ(pinned.size(), 1U);
    EXPECT_EQ(pinned.front(), DecoderHwBackend::kD3D11va);
    const auto unpinned = decoder_hw_backend_order(false);
    ASSERT_EQ(unpinned.size(), 4U);
    EXPECT_EQ(unpinned.front(), DecoderHwBackend::kD3D11va);
}

}  // namespace

TEST(PlaybackDecodeRecovery, HevcCraCannotSatisfyAnIdrBoundary) {
    using redclaw::protocol::hevc_annex_b_contains_idr;
    const std::array<std::uint8_t, 6> cra{0, 0, 1, 42, 1, 0x80};
    const std::array<std::uint8_t, 6> idr_n_lp{0, 0, 1, 40, 1, 0x80};
    const std::array<std::uint8_t, 13> idr_w_radl{0, 0, 0, 1, 64, 1, 0x80, 0, 0, 1, 38, 1, 0x80};
    DecoderReferenceRecovery recovery;
    recovery.invalidate();
    EXPECT_FALSE(recovery.prepare(3, 640, 360, 1, hevc_annex_b_contains_idr(cra)));
    EXPECT_TRUE(recovery.needs_idr());
    EXPECT_FALSE(recovery.awaiting_output());
    EXPECT_TRUE(hevc_annex_b_contains_idr(idr_n_lp));
    ASSERT_TRUE(recovery.prepare(3, 640, 360, 1, hevc_annex_b_contains_idr(idr_w_radl)));
    recovery.submitted(true);
    EXPECT_FALSE(recovery.needs_idr());
    EXPECT_TRUE(recovery.take_recovery_output());
}

TEST(PlaybackDecodeRecovery, BufferedIdrAcceptsDependenciesBeforeFirstOutput) {
    redclaw::render::DecoderReferenceRecovery recovery;
    recovery.invalidate();
    EXPECT_FALSE(recovery.prepare(3, 1920, 1080, 10, false));
    EXPECT_TRUE(recovery.prepare(3, 1920, 1080, 10, true));
    EXPECT_TRUE(recovery.needs_idr());
    recovery.submitted(true); // decode succeeded, no output yet
    EXPECT_FALSE(recovery.needs_idr());
    EXPECT_TRUE(recovery.prepare(3, 1920, 1080, 10, false));
    recovery.submitted(false);
    EXPECT_TRUE(recovery.awaiting_output()); // preserve ACK across a busy surface bridge
    EXPECT_TRUE(recovery.take_recovery_output());
    EXPECT_FALSE(recovery.take_recovery_output());
    recovery.invalidate();
    EXPECT_FALSE(recovery.prepare(2, 1280, 720, 11, false));
    EXPECT_TRUE(recovery.prepare(2, 1280, 720, 11, true));
    recovery.submitted(true);
    EXPECT_FALSE(recovery.prepare(2, 1280, 720, 12, false));
    EXPECT_FALSE(recovery.take_recovery_output());
}

TEST(PresentationGate, ZeroFrameOcclusionResumeWaitsForEveryReason) {
    redclaw::render::PresentationGate gate;
    gate.on_present_result(true, 1);
    gate.set_hidden(true);
    gate.set_display_on(false);
    EXPECT_EQ(gate.decide(1000), redclaw::render::PresentationGate::Decision::kSkip);
    gate.set_display_on(true);
    EXPECT_TRUE(gate.paused());
    gate.set_hidden(false);
    EXPECT_EQ(gate.decide(1250), redclaw::render::PresentationGate::Decision::kTestOcclusion);
    gate.on_present_result(false, 1250); // timer, no decoded frames
    EXPECT_FALSE(gate.paused());
    EXPECT_TRUE(gate.take_resume());
    EXPECT_FALSE(gate.take_resume());
}

TEST(PlaybackRecoveryRequest, PauseCancelsPendingButPreserves500msCoalescingAcrossRapidToggle) {
    redclaw::render::KeyframeRequestLimiter limiter;
    EXPECT_TRUE(limiter.request(1000));
    EXPECT_FALSE(limiter.request(1100));
    limiter.satisfied(); // entering pause
    EXPECT_FALSE(limiter.poll(2000));
    EXPECT_FALSE(limiter.request(1200)); // initial resume request coalesced
    EXPECT_FALSE(limiter.poll(1499));
    EXPECT_TRUE(limiter.poll(1500));
    EXPECT_FALSE(limiter.poll(2000));
}
