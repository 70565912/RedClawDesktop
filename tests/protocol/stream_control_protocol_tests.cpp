#include <gtest/gtest.h>

#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/protocol/compressed_protobuf.h"
#include "redclaw_wire.pb.h"

namespace {

redclaw::protocol::StreamControlMessageV1 make_message(
    redclaw::protocol::StreamControlMessageTypeV1 type) {
    redclaw::protocol::StreamControlMessageV1 message;
    message.type = type;
    message.session_epoch = "epoch-17";
    message.message_id = 9;
    message.sent_at_ms = 123456;
    return message;
}

TEST(StreamControlProtocolTests, ViewportRequestRoundTrips) {
    auto message = make_message(redclaw::protocol::StreamControlMessageTypeV1::kViewportRequest);
    message.viewport_width = 1537;
    message.viewport_height = 863;

    const auto parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(message));

    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.type, redclaw::protocol::StreamControlMessageTypeV1::kViewportRequest);
    EXPECT_EQ(parsed.value.viewport_width, 1537U);
    EXPECT_EQ(parsed.value.viewport_height, 863U);
    EXPECT_EQ(parsed.value.session_epoch, "epoch-17");
}

TEST(StreamControlProtocolTests, KeyframeRequestRoundTripsWithBoundedReason) {
    auto message = make_message(redclaw::protocol::StreamControlMessageTypeV1::kKeyframeRequest);
    message.payload = "gui_decode_failure";

    const auto parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(message));

    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.type, redclaw::protocol::StreamControlMessageTypeV1::kKeyframeRequest);
    EXPECT_EQ(parsed.value.payload, message.payload);

    message.payload.assign(129, 'x');
    std::string error;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(message, &error));
}

TEST(StreamControlProtocolTests, RemoteLogChunkPreservesEscapedText) {
    auto message = make_message(redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogChunk);
    message.request_id = "request-2";
    message.chunk_index = 3;
    message.payload = "first=value\nsecond\\value\r\n";

    const auto parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(message));

    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.payload, message.payload);
    EXPECT_EQ(parsed.value.chunk_index, 3U);
}

TEST(StreamControlProtocolTests, LocalRuntimeFrameIsSingleLineAndRoundTrips) {
    auto message = make_message(redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogChunk);
    message.request_id = "request-5";
    message.payload = "one\ntwo=three";
    const std::string frame = redclaw::protocol::serialize_local_runtime_control_frame_v2(message, 1234567);

    EXPECT_EQ(frame.find('\n'), std::string::npos);
    std::uint64_t emitted = 0;
    const auto parsed = redclaw::protocol::parse_local_runtime_control_frame_v2(frame, &emitted);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(emitted, 1234567U);
    EXPECT_EQ(parsed.value.payload, message.payload);
}

TEST(StreamControlProtocolTests, LocalTimingRejectsMalformedOrOldEnvelope) {
    for (const auto line : {"RCD-LOCAL-CONTROL-V1 eA==", "RCD-LOCAL-CONTROL-V2 0 eA==",
        "RCD-LOCAL-CONTROL-V2 -1 eA==", "RCD-LOCAL-CONTROL-V2 123x eA==",
        "RCD-LOCAL-CONTROL-V2 99999999999999999999999 eA=="}) {
        std::uint64_t emitted = 42;
        EXPECT_FALSE(redclaw::protocol::parse_local_runtime_control_frame_v2(line, &emitted).ok);
        EXPECT_EQ(emitted, 0U);
    }
}

TEST(StreamControlProtocolTests, RejectsInvalidEpochAndDimensions) {
    auto message = make_message(redclaw::protocol::StreamControlMessageTypeV1::kViewportRequest);
    message.session_epoch = "bad epoch";
    message.viewport_width = 32;
    message.viewport_height = 32;
    std::string error;

    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(message, &error));
    EXPECT_FALSE(error.empty());
}

TEST(StreamControlProtocolTests, RejectsOversizedControlFrame) {
    const std::string oversized(redclaw::protocol::kMaxStreamControlMessageBytes + 1, 'x');
    const auto parsed = redclaw::protocol::parse_stream_control_message_v1(oversized);
    EXPECT_FALSE(parsed.ok);
    EXPECT_NE(parsed.error.find("size"), std::string::npos);
}

TEST(StreamControlProtocolTests, EpochGuardRejectsPreHelloReplayAndOldEpoch) {
    redclaw::protocol::StreamControlEpochGuardV1 guard;
    auto capabilities = make_message(
        redclaw::protocol::StreamControlMessageTypeV1::kCapabilities);
    std::string error;
    EXPECT_FALSE(guard.accept(capabilities, &error));

    auto hello = make_message(redclaw::protocol::StreamControlMessageTypeV1::kHello);
    ASSERT_TRUE(guard.accept(hello, &error)) << error;
    capabilities.message_id = 10;
    EXPECT_TRUE(guard.accept(capabilities, &error)) << error;
    EXPECT_FALSE(guard.accept(capabilities, &error));

    guard.reset();
    hello.session_epoch = "epoch-18";
    hello.message_id = 1;
    ASSERT_TRUE(guard.accept(hello, &error)) << error;
    capabilities.session_epoch = "epoch-17";
    capabilities.message_id = 11;
    EXPECT_FALSE(guard.accept(capabilities, &error));
}

TEST(StreamControlProtocolTests, RejectsInconsistentReceiverStatistics) {
    auto stats = make_message(
        redclaw::protocol::StreamControlMessageTypeV1::kReceiverNetworkStats);
    stats.received_frames = 10;
    stats.decoded_frames = 11;
    std::string error;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(stats, &error));
    EXPECT_NE(error.find("statistics"), std::string::npos);
}

TEST(StreamControlProtocolTests, PlaybackStarvationFeedbackRequiresDamagedFrameEvidence) {
    auto message = make_message(redclaw::protocol::StreamControlMessageTypeV1::kPlaybackStarvation);
    message.playback_starvation_count = 7;
    message.incomplete_frame_id = 42;
    message.latest_received_frame_id = 45;
    message.latest_complete_frame_id = 41;
    message.latest_complete_keyframe_id = 36;
    message.observed_rate_revision = 9;
    message.payload = "incomplete_frame_abandoned";

    const auto parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(message));

    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.type, redclaw::protocol::StreamControlMessageTypeV1::kPlaybackStarvation);
    EXPECT_EQ(parsed.value.playback_starvation_count, 7U);
    EXPECT_EQ(parsed.value.incomplete_frame_id, 42U);
    EXPECT_EQ(parsed.value.latest_received_frame_id, 45U);
    EXPECT_EQ(parsed.value.latest_complete_frame_id, 41U);
    EXPECT_EQ(parsed.value.latest_complete_keyframe_id, 36U);
    EXPECT_EQ(parsed.value.observed_rate_revision, 9U);
    EXPECT_EQ(parsed.value.payload, "incomplete_frame_abandoned");

    message.incomplete_frame_id = 0;
    std::string error;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(message, &error));
}

TEST(StreamControlProtocolTests, MediaTransportFeedbackRoundTripsAndRequiresMonotonicArrivals) {
    auto message = make_message(
        redclaw::protocol::StreamControlMessageTypeV1::kMediaTransportFeedback);
    message.transport_feedback_id = 3;
    message.observed_rate_revision = 7;
    message.media_transport_arrivals = {
        {.transport_sequence = 41, .receiver_steady_us = 1000000},
        {.transport_sequence = 43, .receiver_steady_us = 1003500},
    };

    const auto parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(message));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.value.media_transport_arrivals.size(), 2U);
    EXPECT_EQ(parsed.value.transport_feedback_id, 3U);
    EXPECT_EQ(parsed.value.media_transport_arrivals[1].transport_sequence, 43U);
    EXPECT_EQ(parsed.value.media_transport_arrivals[1].receiver_steady_us, 1003500U);

    std::string error;
    message.media_transport_arrivals[1].transport_sequence = 41;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(message, &error));
    message.media_transport_arrivals[1].transport_sequence = 43;
    message.media_transport_arrivals[1].receiver_steady_us = 999999;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(message, &error));
    message.media_transport_arrivals.resize(
        redclaw::protocol::kMaxMediaTransportArrivalsPerFeedback + 1U,
        {.transport_sequence = 44, .receiver_steady_us = 1004000});
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(message, &error));
}

TEST(StreamControlProtocolTests, ReceiverAssemblyQualityRoundTrips) {
    auto stats = make_message(
        redclaw::protocol::StreamControlMessageTypeV1::kReceiverNetworkStats);
    stats.received_frames = 12;
    stats.dropped_frames = 2;
    stats.received_fragments = 38;
    stats.reassembled_frames = 10;
    stats.completed_keyframes = 2;
    stats.dropped_keyframes = 1;
    stats.reassembly_timeouts = 1;
    stats.latest_received_frame_id = 18;
    stats.latest_complete_frame_id = 17;
    stats.latest_complete_keyframe_id = 12;
    stats.observed_rate_revision = 5;

    const auto parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(stats));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.received_fragments, 38U);
    EXPECT_EQ(parsed.value.reassembled_frames, 10U);
    EXPECT_EQ(parsed.value.completed_keyframes, 2U);
    EXPECT_EQ(parsed.value.dropped_keyframes, 1U);
    EXPECT_EQ(parsed.value.latest_received_frame_id, 18U);
    EXPECT_EQ(parsed.value.latest_complete_frame_id, 17U);
    EXPECT_EQ(parsed.value.latest_complete_keyframe_id, 12U);
    EXPECT_EQ(parsed.value.observed_rate_revision, 5U);

    stats.dropped_keyframes = 3;
    std::string error;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(stats, &error));
}

TEST(StreamControlProtocolTests, SourceActivityAndDisplayableAckRoundTrip) {
    auto state = make_message(
        redclaw::protocol::StreamControlMessageTypeV1::kSourceActivityState);
    state.source_activity_revision = 4;
    state.source_activity_state =
        redclaw::protocol::DesktopSourceActivityStateV1::kStaticPending;
    state.reference_frame_id = 81;
    state.reference_keyframe_id = 81;
    state.stream_geometry_revision = 7;
    state.rate_revision = 9;
    state.media_budget_waiting = true;

    auto parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(state));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.source_activity_revision, 4U);
    EXPECT_EQ(
        parsed.value.source_activity_state,
        redclaw::protocol::DesktopSourceActivityStateV1::kStaticPending);
    EXPECT_EQ(parsed.value.reference_keyframe_id, 81U);
    EXPECT_TRUE(parsed.value.media_budget_waiting);
    redclaw::protocol::wire::StreamControlMessageV1 wire;
    ASSERT_TRUE(redclaw::protocol::decompress_protobuf(
        redclaw::protocol::serialize_stream_control_message_v1(state),
        redclaw::protocol::ProtobufWireKind::kControl, wire));
    wire.clear_media_budget_waiting();
    auto encoded = redclaw::protocol::compress_protobuf(
        wire, redclaw::protocol::ProtobufWireKind::kControl);
    parsed = redclaw::protocol::parse_stream_control_message_v1(encoded);
    ASSERT_TRUE(parsed.ok);
    EXPECT_FALSE(parsed.value.media_budget_waiting);
    encoded += "media_budget_waiting=invalid\n";
    EXPECT_FALSE(redclaw::protocol::parse_stream_control_message_v1(encoded).ok);

    auto stats = make_message(
        redclaw::protocol::StreamControlMessageTypeV1::kReceiverNetworkStats);
    stats.observed_source_activity_revision = 4;
    stats.latest_displayable_frame_id = 82;
    stats.latest_displayable_keyframe_id = 81;
    stats.latest_presented_frame_id = 79;
    parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(stats));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.latest_displayable_keyframe_id, 81U);
    EXPECT_EQ(parsed.value.latest_presented_frame_id, 79U);

    state.source_activity_state =
        redclaw::protocol::DesktopSourceActivityStateV1::kActive;
    std::string error;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(state, &error));
    stats.observed_source_activity_revision = 0;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(stats, &error));
}

TEST(StreamControlProtocolTests, LocalRuntimeFrameHasHard64KiBLimit) {
    auto message = make_message(redclaw::protocol::StreamControlMessageTypeV1::kCapabilities);
    message.payload.assign(65U * 1024U, 'x');
    EXPECT_TRUE(redclaw::protocol::serialize_local_runtime_control_frame_v2(message).empty());

    const std::string oversized(redclaw::protocol::kMaxLocalRuntimeControlFrameBytes + 1, 'x');
    const auto parsed = redclaw::protocol::parse_local_runtime_control_frame_v2(oversized);
    EXPECT_FALSE(parsed.ok);
}

TEST(StreamControlProtocolTests, RemoteInputBatchRoundTripsTypedEvents) {
    auto message = make_message(redclaw::protocol::StreamControlMessageTypeV1::kInputBatch);
    message.input_sequence = 17;
    message.desktop_geometry_revision = 4;
    redclaw::protocol::RemoteInputEventV1 key;
    key.type = redclaw::protocol::RemoteInputEventTypeV1::kKeyDown;
    key.scan_code = 0x38;
    key.virtual_key = 0xA5;
    key.extended = true;
    key.repeat = false;
    message.input_events.push_back(key);
    redclaw::protocol::RemoteInputEventV1 mouse;
    mouse.type = redclaw::protocol::RemoteInputEventTypeV1::kMouseButtonDown;
    mouse.normalized_x = 65535;
    mouse.normalized_y = 32768;
    mouse.mouse_button = redclaw::protocol::RemoteInputMouseButtonV1::kX1;
    message.input_events.push_back(mouse);

    const std::string wire = redclaw::protocol::serialize_stream_control_message_v1(message);
    ASSERT_LE(wire.size(), redclaw::protocol::kMaxRemoteInputBatchBytes);
    const auto parsed = redclaw::protocol::parse_stream_control_message_v1(wire);

    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.value.input_events.size(), 2U);
    EXPECT_EQ(parsed.value.input_sequence, 17U);
    EXPECT_TRUE(parsed.value.input_events[0].extended);
    EXPECT_EQ(parsed.value.input_events[1].normalized_x, 65535U);
    EXPECT_EQ(
        parsed.value.input_events[1].mouse_button,
        redclaw::protocol::RemoteInputMouseButtonV1::kX1);
}

TEST(StreamControlProtocolTests, RemoteInputCapabilitiesAndStateSyncRoundTrip) {
    auto capabilities = make_message(
        redclaw::protocol::StreamControlMessageTypeV1::kInputCapabilities);
    capabilities.input_supported = true;
    capabilities.input_authorized = true;
    capabilities.input_state = redclaw::protocol::RemoteInputControlStateV1::kAvailable;
    capabilities.desktop_origin_x = -1920;
    capabilities.desktop_origin_y = 0;
    capabilities.desktop_width = 3840;
    capabilities.desktop_height = 1080;
    capabilities.desktop_rotation = 90;
    capabilities.desktop_geometry_revision = 4;
    auto parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(capabilities));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.desktop_origin_x, -1920);
    EXPECT_EQ(parsed.value.desktop_rotation, 90U);

    auto sync = make_message(redclaw::protocol::StreamControlMessageTypeV1::kInputStateSync);
    sync.input_sequence = 22;
    sync.desktop_geometry_revision = 4;
    sync.pressed_scan_codes = {0x1D, static_cast<std::uint16_t>(0x8000U | 0x38U)};
    sync.pressed_mouse_buttons = 0x11;
    parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(sync));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.pressed_scan_codes, sync.pressed_scan_codes);
    EXPECT_EQ(parsed.value.pressed_mouse_buttons, 0x11U);
}

TEST(StreamControlProtocolTests, RejectsInvalidRemoteInputBoundaries) {
    auto batch = make_message(redclaw::protocol::StreamControlMessageTypeV1::kInputBatch);
    batch.input_sequence = 1;
    batch.desktop_geometry_revision = 1;
    batch.input_events.resize(redclaw::protocol::kMaxRemoteInputEventsPerBatch + 1U);
    for (auto& event : batch.input_events) {
        event.type = redclaw::protocol::RemoteInputEventTypeV1::kKeyDown;
        event.scan_code = 1;
    }
    std::string error;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(batch, &error));

    batch.input_events.resize(1);
    batch.input_events[0].scan_code = 1;
    batch.desktop_geometry_revision = 0;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(batch, &error));
    batch.desktop_geometry_revision = 1;

    batch.input_events[0].scan_code = 0;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(batch, &error));

    batch.input_events[0].type = redclaw::protocol::RemoteInputEventTypeV1::kMouseWheel;
    batch.input_events[0].wheel_delta = 12001;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(batch, &error));

    batch.input_events[0] = {};
    batch.input_events[0].type = redclaw::protocol::RemoteInputEventTypeV1::kKeyDown;
    batch.input_events[0].scan_code = 0x100;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(batch, &error));

    batch.input_events[0].type = redclaw::protocol::RemoteInputEventTypeV1::kKeyUp;
    batch.input_events[0].scan_code = 0x1D;
    batch.input_events[0].repeat = true;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(batch, &error));

    auto oversized = make_message(redclaw::protocol::StreamControlMessageTypeV1::kInputBatch);
    oversized.input_sequence = 2;
    oversized.desktop_geometry_revision = 1;
    oversized.input_events.resize(redclaw::protocol::kMaxRemoteInputEventsPerBatch);
    for (auto& event : oversized.input_events) {
        event.type = redclaw::protocol::RemoteInputEventTypeV1::kMouseHorizontalWheel;
        event.normalized_x = 65535;
        event.normalized_y = 65535;
        event.wheel_delta = 12000;
    }
    EXPECT_LT(
        redclaw::protocol::stream_control_message_protobuf_size_v1(oversized),
        redclaw::protocol::kMaxRemoteInputBatchBytes);
    EXPECT_TRUE(redclaw::protocol::validate_stream_control_message_v1(oversized, &error));
    oversized.payload.assign(4096, 'x');
    EXPECT_GT(redclaw::protocol::stream_control_message_protobuf_size_v1(oversized),
        redclaw::protocol::kMaxRemoteInputBatchBytes);
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(oversized, &error));
    EXPECT_FALSE(redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(oversized)).ok);

    auto sync = make_message(redclaw::protocol::StreamControlMessageTypeV1::kInputStateSync);
    sync.input_sequence = 3;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(sync, &error));
}

TEST(StreamControlProtocolTests, DesktopDisplayCatalogPreservesVirtualDesktopGeometry) {
    auto catalog = make_message(
        redclaw::protocol::StreamControlMessageTypeV1::kDesktopDisplayCatalog);
    catalog.display_catalog_revision = 7;
    catalog.desktop_displays = {
        {
            .display_id = "luid-1-output-0",
            .display_name = "Primary display",
            .desktop_origin_x = 0,
            .desktop_origin_y = 0,
            .pixel_width = 2560,
            .pixel_height = 1440,
            .rotation = 0,
            .primary = true,
        },
        {
            .display_id = "luid-2-output-1",
            .display_name = "Portrait display",
            .desktop_origin_x = -1080,
            .desktop_origin_y = -240,
            .pixel_width = 1080,
            .pixel_height = 1920,
            .rotation = 90,
            .primary = false,
        },
    };

    const auto parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(catalog));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.value.desktop_displays.size(), 2U);
    EXPECT_EQ(parsed.value.display_catalog_revision, 7U);
    EXPECT_EQ(parsed.value.desktop_displays[1].desktop_origin_x, -1080);
    EXPECT_EQ(parsed.value.desktop_displays[1].desktop_origin_y, -240);
    EXPECT_EQ(parsed.value.desktop_displays[1].rotation, 90U);
}

TEST(StreamControlProtocolTests, CaptureRegionMessagesAreBoundedAndVersioned) {
    auto request = make_message(
        redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRequest);
    request.display_id = "luid-1-output-0";
    request.region_left = 1024;
    request.region_top = 2048;
    request.region_right = 60000;
    request.region_bottom = 62000;
    request.capture_region_revision = 11;

    auto parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(request));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.display_id, request.display_id);
    EXPECT_EQ(parsed.value.region_left, 1024U);
    EXPECT_EQ(parsed.value.capture_region_revision, 11U);

    auto applied = request;
    applied.type = redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionApplied;
    applied.encoded_width = 1920;
    applied.encoded_height = 1080;
    applied.content_rect_x = 240;
    applied.content_rect_y = 0;
    applied.content_rect_width = 1440;
    applied.content_rect_height = 1080;
    parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(applied));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.content_rect_x, 240U);
    EXPECT_EQ(parsed.value.content_rect_width, 1440U);

    auto rejected = request;
    rejected.type = redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRejected;
    rejected.payload = "capture_switch_failed";
    parsed = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(rejected));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.payload, "capture_switch_failed");

    std::string error;
    request.region_right = request.region_left;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(request, &error));
    applied.content_rect_x = 1000;
    applied.content_rect_width = 1000;
    EXPECT_FALSE(redclaw::protocol::validate_stream_control_message_v1(applied, &error));
}

}  // namespace
