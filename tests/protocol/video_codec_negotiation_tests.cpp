#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/protocol/video_codec_negotiation.h"

#include <gtest/gtest.h>

namespace {

using namespace redclaw::protocol;

PeerVideoDecodeCapabilities hevc_hardware_peer() {
    return {kVideoCodecCapabilityVersion, kVideoCodecMaskH264 | kVideoCodecMaskHevc,
        kVideoCodecMaskH264 | kVideoCodecMaskHevc};
}

HostVideoEncodeCapabilities hevc_hardware_host() {
    HostVideoEncodeCapabilities host;
    host.hevc_hardware_encoder_available = true;
    host.hevc_software_encoder_available = true;
    return host;
}

TEST(VideoCodecNegotiation, AutoPrefersHevcWhenBothSidesHaveHardware) {
    const auto selection = select_video_codec(VideoCodecPreference::kAuto, hevc_hardware_peer(), hevc_hardware_host());
    EXPECT_EQ(selection.codec, NegotiatedVideoCodec::kHevc);
    EXPECT_FALSE(selection.allow_software_encoder);
    EXPECT_EQ(selection.reason, "auto_hevc_hardware_on_both_sides");
}

TEST(VideoCodecNegotiation, OldPeerWithoutReportGetsH264) {
    const auto selection = select_video_codec(VideoCodecPreference::kAuto, {}, hevc_hardware_host());
    EXPECT_EQ(selection.codec, NegotiatedVideoCodec::kH264);
    EXPECT_EQ(selection.reason, "peer_no_video_codec_report");
    // Forcing HEVC on the Host never sends HEVC to a peer that did not ask for it.
    EXPECT_EQ(select_video_codec(VideoCodecPreference::kHevc, {}, hevc_hardware_host()).codec,
        NegotiatedVideoCodec::kH264);
}

TEST(VideoCodecNegotiation, PeerWithoutHevcDecoderGetsH264) {
    const PeerVideoDecodeCapabilities peer{1, kVideoCodecMaskH264, kVideoCodecMaskH264};
    const auto selection = select_video_codec(VideoCodecPreference::kAuto, peer, hevc_hardware_host());
    EXPECT_EQ(selection.codec, NegotiatedVideoCodec::kH264);
    EXPECT_EQ(selection.reason, "peer_no_hevc_decoder");
}

TEST(VideoCodecNegotiation, AutoRequiresPeerHardwareHevcDecode) {
    const PeerVideoDecodeCapabilities peer{1, kVideoCodecMaskH264 | kVideoCodecMaskHevc, kVideoCodecMaskH264};
    const auto selection = select_video_codec(VideoCodecPreference::kAuto, peer, hevc_hardware_host());
    EXPECT_EQ(selection.codec, NegotiatedVideoCodec::kH264);
    EXPECT_EQ(selection.reason, "peer_hevc_decode_not_hardware");
    EXPECT_EQ(select_video_codec(VideoCodecPreference::kHevc, peer, hevc_hardware_host()).codec,
        NegotiatedVideoCodec::kHevc);
}

TEST(VideoCodecNegotiation, AutoNeverFallsBackToSoftwareHevcEncoder) {
    HostVideoEncodeCapabilities host;
    host.hevc_software_encoder_available = true;
    const auto selection = select_video_codec(VideoCodecPreference::kAuto, hevc_hardware_peer(), host);
    EXPECT_EQ(selection.codec, NegotiatedVideoCodec::kH264);
    EXPECT_EQ(selection.reason, "host_no_hevc_hardware_encoder");

    const auto forced = select_video_codec(VideoCodecPreference::kHevc, hevc_hardware_peer(), host);
    EXPECT_EQ(forced.codec, NegotiatedVideoCodec::kHevc);
    EXPECT_TRUE(forced.allow_software_encoder);
    EXPECT_EQ(forced.reason, "forced_hevc_by_setting_software_encoder");

    EXPECT_EQ(select_video_codec(VideoCodecPreference::kHevc, hevc_hardware_peer(), {}).reason,
        "host_no_hevc_encoder");
}

TEST(VideoCodecNegotiation, ForcedH264WinsOverEverything) {
    const auto selection = select_video_codec(VideoCodecPreference::kH264, hevc_hardware_peer(), hevc_hardware_host());
    EXPECT_EQ(selection.codec, NegotiatedVideoCodec::kH264);
    EXPECT_EQ(selection.reason, "forced_h264_by_setting");
}

TEST(VideoCodecNegotiation, ControllerH264PreferenceHidesHevc) {
    const LocalVideoDecodeSupport support{true, true, true, true};
    const auto advertised = build_local_video_decode_capabilities(support, VideoCodecPreference::kH264);
    EXPECT_EQ(advertised.version, kVideoCodecCapabilityVersion);
    EXPECT_EQ(advertised.decode_mask, kVideoCodecMaskH264);
    EXPECT_EQ(advertised.hardware_decode_mask, kVideoCodecMaskH264);
    EXPECT_EQ(select_video_codec(VideoCodecPreference::kAuto, advertised, hevc_hardware_host()).codec,
        NegotiatedVideoCodec::kH264);

    const auto automatic = build_local_video_decode_capabilities(support, VideoCodecPreference::kAuto);
    EXPECT_EQ(automatic.decode_mask, kVideoCodecMaskH264 | kVideoCodecMaskHevc);
    EXPECT_EQ(automatic.hardware_decode_mask, kVideoCodecMaskH264 | kVideoCodecMaskHevc);

    const LocalVideoDecodeSupport software_hevc{true, true, true, false};
    const auto software = build_local_video_decode_capabilities(software_hevc, VideoCodecPreference::kAuto);
    EXPECT_EQ(software.decode_mask, kVideoCodecMaskH264);
    EXPECT_EQ(software.hardware_decode_mask, kVideoCodecMaskH264);
    EXPECT_EQ(select_video_codec(VideoCodecPreference::kAuto, software, hevc_hardware_host()).reason,
        "peer_no_hevc_decoder");
    // An explicit hevc preference does not advertise a software decoder either.
    EXPECT_EQ(build_local_video_decode_capabilities(software_hevc, VideoCodecPreference::kHevc).decode_mask,
        kVideoCodecMaskH264);
}

TEST(VideoCodecNegotiation, PreferenceParserAcceptsOnlyKnownValues) {
    EXPECT_EQ(parse_video_codec_preference("auto"), VideoCodecPreference::kAuto);
    EXPECT_EQ(parse_video_codec_preference("h264"), VideoCodecPreference::kH264);
    EXPECT_EQ(parse_video_codec_preference("hevc"), VideoCodecPreference::kHevc);
    EXPECT_FALSE(parse_video_codec_preference("h265").has_value());
    EXPECT_FALSE(parse_video_codec_preference("").has_value());
    EXPECT_FALSE(parse_video_codec_preference("HEVC").has_value());
    EXPECT_EQ(to_string(VideoCodecPreference::kHevc), "hevc");
    EXPECT_EQ(describe_video_codec_mask(kVideoCodecMaskH264 | kVideoCodecMaskHevc), "h264,hevc");
    EXPECT_EQ(describe_video_codec_mask(0), "none");
}

TEST(VideoCodecNegotiation, H264IsAlwaysAcceptedAndHevcNeedsAReport) {
    EXPECT_TRUE(peer_accepts_video_codec({}, NegotiatedVideoCodec::kH264));
    EXPECT_FALSE(peer_accepts_video_codec({}, NegotiatedVideoCodec::kHevc));
    EXPECT_FALSE(peer_accepts_video_codec({1, kVideoCodecMaskH264, 0}, NegotiatedVideoCodec::kHevc));
    EXPECT_TRUE(peer_accepts_video_codec(hevc_hardware_peer(), NegotiatedVideoCodec::kHevc));
}

TEST(VideoCodecNegotiation, WaitForPeerReportIsBounded) {
    EXPECT_TRUE(should_wait_for_peer_video_codec_report(false, 1000, 1500, 3000));
    EXPECT_FALSE(should_wait_for_peer_video_codec_report(false, 1000, 4000, 3000));
    EXPECT_FALSE(should_wait_for_peer_video_codec_report(true, 1000, 1500, 3000));
    EXPECT_FALSE(should_wait_for_peer_video_codec_report(false, 0, 1500, 3000));
}

TEST(HostVideoCodecNegotiator, TracksSessionReportAndStickyEncoderFailure) {
    HostVideoCodecNegotiatorV1 negotiator(VideoCodecPreference::kAuto);
    EXPECT_FALSE(negotiator.snapshot().peer_report_received);
    EXPECT_EQ(negotiator.select(hevc_hardware_host()).reason, "peer_no_video_codec_report");

    // Old peer: Hello without the optional fields.
    EXPECT_TRUE(negotiator.observe_peer(0, 0, 0));
    EXPECT_TRUE(negotiator.snapshot().peer_report_received);
    EXPECT_EQ(negotiator.select(hevc_hardware_host()).codec, NegotiatedVideoCodec::kH264);
    EXPECT_FALSE(negotiator.observe_peer(0, 0, 0));

    // Unknown future bits are dropped; hardware bits imply decode bits.
    EXPECT_TRUE(negotiator.observe_peer(2, 0xFFU, 0xFFU));
    EXPECT_EQ(negotiator.snapshot().peer.decode_mask, kKnownVideoCodecMask);
    EXPECT_EQ(negotiator.select(hevc_hardware_host()).codec, NegotiatedVideoCodec::kHevc);

    const auto before = negotiator.snapshot().revision;
    negotiator.record_hevc_encoder_open_failure();
    EXPECT_GT(negotiator.snapshot().revision, before);
    EXPECT_EQ(negotiator.select(hevc_hardware_host()).reason, "host_hevc_encoder_open_failed");

    // A new session forgets the peer but keeps the Host failure.
    negotiator.reset_peer();
    EXPECT_FALSE(negotiator.snapshot().peer_report_received);
    EXPECT_TRUE(negotiator.observe_peer(1, kKnownVideoCodecMask, kKnownVideoCodecMask));
    EXPECT_EQ(negotiator.select(hevc_hardware_host()).codec, NegotiatedVideoCodec::kH264);
}

TEST(VideoCodecWire, OptionalFieldsRoundTripAndDefaultToOldPeer) {
    StreamControlMessageV1 hello;
    hello.type = StreamControlMessageTypeV1::kHello;
    hello.session_epoch = "epoch-1";
    hello.message_id = 1;
    hello.sent_at_ms = 1;
    auto parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(hello));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.video_codec_version, 0U);
    EXPECT_EQ(parsed.value.video_decode_codecs, 0U);

    hello.video_codec_version = kVideoCodecCapabilityVersion;
    hello.video_decode_codecs = kVideoCodecMaskH264 | kVideoCodecMaskHevc;
    hello.video_hardware_decode_codecs = kVideoCodecMaskHevc;
    parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(hello));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.video_codec_version, kVideoCodecCapabilityVersion);
    EXPECT_EQ(parsed.value.video_decode_codecs, kVideoCodecMaskH264 | kVideoCodecMaskHevc);
    EXPECT_EQ(parsed.value.video_hardware_decode_codecs, kVideoCodecMaskHevc);

    // A future version with unknown bits must still parse and validate.
    hello.video_codec_version = 7;
    hello.video_decode_codecs = 0xF3U;
    parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(hello));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_TRUE(validate_stream_control_message_v1(parsed.value));
}

TEST(VideoCodecWire, ChangeReasonRoundTripsAndDefaultsEmpty) {
    StreamControlMessageV1 caps;
    caps.type = StreamControlMessageTypeV1::kCapabilities;
    caps.session_epoch = "epoch-1";
    caps.message_id = 2;
    caps.sent_at_ms = 2;
    auto parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(caps));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_TRUE(parsed.value.video_codec_change_reason.empty());
    caps.video_codec_version = kVideoCodecCapabilityVersion;
    caps.video_decode_codecs = kVideoCodecMaskH264;
    caps.video_codec_change_reason = std::string(kControllerHevcDecodeOverloadReason);
    parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(caps));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.video_codec_change_reason, kControllerHevcDecodeOverloadReason);
}

TEST(VideoCodecNegotiator, KeepsOnlySanitizedPeerChangeReason) {
    HostVideoCodecNegotiatorV1 negotiator(VideoCodecPreference::kAuto);
    EXPECT_TRUE(negotiator.observe_peer(1, kVideoCodecMaskH264 | kVideoCodecMaskHevc, kVideoCodecMaskHevc));
    EXPECT_TRUE(negotiator.snapshot().peer_change_reason.empty());
    EXPECT_TRUE(negotiator.observe_peer(1, kVideoCodecMaskH264, 0, kControllerHevcDecodeOverloadReason));
    EXPECT_EQ(negotiator.snapshot().peer_change_reason, kControllerHevcDecodeOverloadReason);
    EXPECT_FALSE(peer_accepts_video_codec(negotiator.snapshot().peer, NegotiatedVideoCodec::kHevc));
    negotiator.observe_peer(1, kVideoCodecMaskH264, 0, "bad reason\nInjected");
    EXPECT_TRUE(negotiator.snapshot().peer_change_reason.empty());
    negotiator.observe_peer(1, kVideoCodecMaskH264, 0, std::string(65, 'a'));
    EXPECT_TRUE(negotiator.snapshot().peer_change_reason.empty());
    negotiator.observe_peer(1, kVideoCodecMaskH264, 0, kControllerHevcDecodeOverloadReason);
    negotiator.reset_peer();
    EXPECT_TRUE(negotiator.snapshot().peer_change_reason.empty());
}

ControllerHevcDecodeOverloadMonitor::Observation overload_sample(
    std::uint64_t now_ms, std::uint64_t backlog, std::uint64_t loss = 0,
    NegotiatedVideoCodec codec = NegotiatedVideoCodec::kHevc) {
    ControllerHevcDecodeOverloadMonitor::Observation sample;
    sample.now_ms = now_ms;
    sample.codec = codec;
    sample.ring_backlog = backlog;
    sample.ring_slots = 6;
    sample.network_loss_total = loss;
    return sample;
}

TEST(ControllerHevcDecodeOverload, TripsOnceAfterRepeatedOverrunEpisodesOnCleanNetwork) {
    ControllerHevcDecodeOverloadMonitor monitor({3, 10000, 2000});
    std::uint64_t now = 1000;
    int trips = 0;
    for (int episode = 0; episode < 3; ++episode) {
        // A sustained overrun is one episode, not one per frame.
        for (int frame = 0; frame < 10; ++frame) trips += monitor.observe(overload_sample(now += 16, 6 + frame));
        trips += monitor.observe(overload_sample(now += 16, 1));
    }
    EXPECT_EQ(trips, 1);
    EXPECT_TRUE(monitor.triggered());
    EXPECT_EQ(monitor.counted_episodes(), 3U);
    for (int frame = 0; frame < 3; ++frame) {
        EXPECT_FALSE(monitor.observe(overload_sample(now += 1000, 0)));
        EXPECT_FALSE(monitor.observe(overload_sample(now += 16, 9)));
    }
}

TEST(ControllerHevcDecodeOverload, BacklogBelowRingSizeIsNotAnOverrun) {
    ControllerHevcDecodeOverloadMonitor monitor({2, 10000, 2000});
    std::uint64_t now = 0;
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(monitor.observe(overload_sample(now += 16, 5)));
        EXPECT_FALSE(monitor.observe(overload_sample(now += 16, 0)));
    }
    EXPECT_EQ(monitor.counted_episodes(), 0U);
}

TEST(ControllerHevcDecodeOverload, EpisodesOutsideWindowExpire) {
    ControllerHevcDecodeOverloadMonitor monitor({3, 10000, 2000});
    std::uint64_t now = 0;
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(monitor.observe(overload_sample(now += 6000, 7)));
        EXPECT_FALSE(monitor.observe(overload_sample(now += 16, 0)));
    }
    EXPECT_FALSE(monitor.triggered());
    EXPECT_LE(monitor.episodes_in_window(), 2U);
}

TEST(ControllerHevcDecodeOverload, NetworkLossDisqualifiesNearbyOverruns) {
    ControllerHevcDecodeOverloadMonitor monitor({2, 30000, 2000});
    std::uint64_t now = 0;
    std::uint64_t loss = 0;
    EXPECT_FALSE(monitor.observe(overload_sample(now += 16, 0, loss)));
    for (int i = 0; i < 5; ++i) {
        ++loss;
        EXPECT_FALSE(monitor.observe(overload_sample(now += 100, 8, loss)));
        EXPECT_FALSE(monitor.observe(overload_sample(now += 16, 0, loss)));
    }
    EXPECT_FALSE(monitor.triggered());
    EXPECT_EQ(monitor.ignored_lossy_episodes(), 5U);
    now += 2500;
    EXPECT_FALSE(monitor.observe(overload_sample(now, 8, loss)));
    EXPECT_FALSE(monitor.observe(overload_sample(now += 16, 0, loss)));
    EXPECT_TRUE(monitor.observe(overload_sample(now += 16, 8, loss)));
}

TEST(ControllerHevcDecodeOverload, H264OverrunsNeverTrip) {
    ControllerHevcDecodeOverloadMonitor monitor({1, 30000, 2000});
    for (std::uint64_t now = 0; now < 2000; now += 32) {
        EXPECT_FALSE(monitor.observe(overload_sample(now, 9, 0, NegotiatedVideoCodec::kH264)));
        EXPECT_FALSE(monitor.observe(overload_sample(now + 16, 0, 0, NegotiatedVideoCodec::kH264)));
    }
    EXPECT_TRUE(monitor.observe(overload_sample(5000, 9)));
}

}  // namespace
