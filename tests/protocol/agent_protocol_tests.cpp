#include <gtest/gtest.h>

#include <string>
#include <chrono>

#include "redclaw/protocol/agent_protocol.h"
#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/protocol/compressed_protobuf.h"
#include "redclaw_wire.pb.h"
#include <zstd.h>

namespace {

TEST(CompressedProtobuf, RejectsWrongKindCorruptionTruncationAndTrailingFrames) {
    using namespace redclaw::protocol;
    wire::AgentMessageEnvelopeV1 message;
    message.set_schema_version(1);
    message.set_session_epoch("wire-test");
    message.set_message_id(1);
    const auto frame = compress_protobuf(message, ProtobufWireKind::kAgent);
    ASSERT_FALSE(frame.empty());
    EXPECT_TRUE(parse_agent_message_v1(frame).ok);
    EXPECT_FALSE(parse_stream_control_message_v1(frame).ok);
    EXPECT_FALSE(parse_agent_message_v1("RCD-AGENT-V1\nschema_version=1\n").ok);
    for (std::size_t i = 0; i < frame.size(); ++i) {
        EXPECT_FALSE(parse_agent_message_v1(std::string_view(frame).substr(0, i)).ok);
    }
    auto corrupt = frame;
    corrupt.back() ^= 1;
    EXPECT_FALSE(parse_agent_message_v1(corrupt).ok);
    EXPECT_FALSE(parse_agent_message_v1(frame + frame.substr(5)).ok);
    EXPECT_FALSE(parse_agent_message_v1(frame + "trailing").ok);
    message.set_type(static_cast<wire::AgentMessageTypeV1>(999));
    EXPECT_FALSE(parse_agent_message_v1(compress_protobuf(message, ProtobufWireKind::kAgent)).ok);
}

TEST(CompressedProtobuf, EnforcesExpandedLimitBeforeParsing) {
    using namespace redclaw::protocol;
    // A tiny wire payload must not bypass the original 64 KiB expanded limit.
    std::string expanded(kMaxProtobufWireBytes + 1, 'x');
    std::string bomb(5 + ZSTD_compressBound(expanded.size()), '\0');
    bomb.replace(0, 4, "RCP1");
    bomb[4] = static_cast<char>(ProtobufWireKind::kAgent);
    const auto compressed = ZSTD_compress(bomb.data() + 5, bomb.size() - 5,
        expanded.data(), expanded.size(), 1);
    ASSERT_FALSE(ZSTD_isError(compressed));
    bomb.resize(5 + compressed);
    ASSERT_LT(bomb.size(), 1024U);
    EXPECT_FALSE(parse_agent_message_v1(bomb).ok);
    wire::AgentMessageEnvelopeV1 oversized;
    oversized.set_text(expanded);
    EXPECT_TRUE(compress_protobuf(oversized, ProtobufWireKind::kAgent).empty());
}

TEST(CompressedProtobuf, RejectsRepeatedOverflowAndNarrowingBeforeInputInjection) {
    using namespace redclaw::protocol;
    wire::StreamControlMessageV1 message;
    message.set_schema_version(1);
    message.set_session_epoch("wire-test");
    message.set_message_id(1);
    message.set_type(wire::StreamControlMessageTypeV1_kInputBatch);
    message.set_sent_at_ms(1);
    message.set_input_sequence(1);
    message.set_desktop_geometry_revision(1);
    auto* event = message.add_input_events();
    event->set_type(wire::RemoteInputEventTypeV1_kMouseMove);
    event->set_normalized_x(65536);
    EXPECT_FALSE(parse_stream_control_message_v1(compress_protobuf(message, ProtobufWireKind::kControl)).ok);
    event->set_normalized_x(65535);
    ASSERT_TRUE(parse_stream_control_message_v1(compress_protobuf(message, ProtobufWireKind::kControl)).ok);
    for (std::size_t i = 1; i <= kMaxRemoteInputEventsPerBatch; ++i) {
        message.add_input_events()->CopyFrom(message.input_events(0));
    }
    EXPECT_FALSE(parse_stream_control_message_v1(compress_protobuf(message, ProtobufWireKind::kControl)).ok);
}

TEST(ProtocolWireCost, RepresentativeMessages) {
    using namespace redclaw::protocol;
    StreamControlMessageV1 ping;
    ping.type = StreamControlMessageTypeV1::kPing;
    ping.session_epoch = "fixture-epoch";
    ping.message_id = 42;
    ping.sent_at_ms = 1788933600000ULL;
    AgentMessageEnvelopeV1 event;
    event.type = AgentMessageTypeV1::kEvent;
    event.session_epoch = ping.session_epoch;
    event.message_id = 43;
    event.sent_at_ms = ping.sent_at_ms;
    event.task_id = "task-fixture";
    event.event_sequence = 17;
    event.event_kind = "text_delta";
    for (int i = 0; i < 60; ++i) {
        event.text += "[test " + std::to_string(i)
            + "] parser accepted a bounded message; queue_depth=2; elapsed_us="
            + std::to_string(30 + i * 7) + "; all assertions passed.\n";
    }
    const auto ping_wire = serialize_stream_control_message_v1(ping);
    const auto event_wire = serialize_agent_message_v1(event);
    ASSERT_TRUE(parse_stream_control_message_v1(ping_wire).ok);
    ASSERT_TRUE(parse_agent_message_v1(event_wire).ok);
    RecordProperty("ping_wire_bytes", static_cast<int>(ping_wire.size()));
    RecordProperty("event_text_bytes", static_cast<int>(event.text.size()));
    RecordProperty("event_wire_bytes", static_cast<int>(event_wire.size()));
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(parse_agent_message_v1(serialize_agent_message_v1(event)).ok);
    }
    RecordProperty("event_roundtrip_mean_us", static_cast<int>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count() / 1000));
}

TEST(CompressedProtobuf, IndependentMessagesPreserveBinaryChunksAndStrictLocalFraming) {
    using namespace redclaw::protocol;
    AgentMessageEnvelopeV1 event;
    event.type = AgentMessageTypeV1::kEvent;
    event.session_epoch = "independent-frames";
    event.message_id = 1;
    event.task_id = "opaque-task";
    std::uint32_t random = 0x81237abcU;
    for (std::size_t i = 0; i < 8192; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        event.text.push_back(static_cast<char>(random & 255));
    }
    const auto first = serialize_agent_message_v1(event);
    const auto parsed = parse_agent_message_v1(first);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.text, event.text);
    RecordProperty("incompressible_text_bytes", event.text.size());
    RecordProperty("incompressible_wire_bytes", first.size());
    event.text.assign(8192, 'x');
    (void)serialize_agent_message_v1(event);
    EXPECT_EQ(parse_agent_message_v1(first).value.text, parsed.value.text);
    auto local = serialize_local_runtime_agent_frame_v1(event);
    ASSERT_TRUE(parse_local_runtime_agent_frame_v1(local).ok);
    EXPECT_FALSE(parse_local_runtime_agent_frame_v1(local + "garbage").ok);
    EXPECT_FALSE(parse_local_runtime_agent_frame_v1(local + "=").ok);
    EXPECT_FALSE(parse_local_runtime_agent_frame_v1(local.substr(0, local.size() - 1)).ok);
}

redclaw::protocol::AgentMessageEnvelopeV1 make_task_message() {
    redclaw::protocol::AgentMessageEnvelopeV1 message;
    message.session_epoch = "epoch-2";
    message.message_id = 7;
    message.sent_at_ms = 1000;
    message.task_id = "task-1";
    message.request_id = "request-1";
    message.type = redclaw::protocol::AgentMessageTypeV1::kTaskCreate;
    message.provider = redclaw::protocol::AgentProviderKindV1::kCodex;
    message.provider_readiness = redclaw::protocol::AgentProviderReadinessV1::kReady;
    message.work_directory_mode =
        redclaw::protocol::AgentWorkDirectoryModeV1::kIsolatedWorktree;
    message.git_repository = true;
    message.task_state = redclaw::protocol::AgentTaskStateV1::kQueued;
    message.model = "gpt-5.6-sol";
    message.project_id = "project-opaque-1";
    message.text = "Change the parser.\nRun focused tests.";
    return message;
}

TEST(AgentProtocolV1, RoundTripsTaskRequestAndLocalFrame) {
    const auto message = make_task_message();
    const auto parsed = redclaw::protocol::parse_agent_message_v1(
        redclaw::protocol::serialize_agent_message_v1(message));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.type, message.type);
    EXPECT_EQ(parsed.value.task_id, message.task_id);
    EXPECT_EQ(parsed.value.project_id, message.project_id);
    EXPECT_EQ(parsed.value.provider_readiness, message.provider_readiness);
    EXPECT_EQ(parsed.value.text, message.text);
    EXPECT_TRUE(parsed.value.git_repository);

    const auto local = redclaw::protocol::parse_local_runtime_agent_frame_v1(
        redclaw::protocol::serialize_local_runtime_agent_frame_v1(message));
    ASSERT_TRUE(local.ok) << local.error;
    EXPECT_EQ(local.value.message_id, message.message_id);
    EXPECT_EQ(local.value.text, message.text);
}

TEST(AgentProtocolV1, RejectsOversizedInstructionAndInvalidApproval) {
    auto message = make_task_message();
    message.text.assign(redclaw::protocol::kMaxAgentInstructionBytes + 1U, 'x');
    std::string error;
    EXPECT_FALSE(redclaw::protocol::validate_agent_message_v1(message, &error));
    EXPECT_NE(error.find("exceeds"), std::string::npos);

    message = make_task_message();
    message.type = redclaw::protocol::AgentMessageTypeV1::kApprovalDecision;
    message.request_id.clear();
    message.text.clear();
    EXPECT_FALSE(redclaw::protocol::validate_agent_message_v1(message, &error));
}

TEST(AgentProtocolV1, CompleteInstructionsRequireUtf8ButStreamChunksRemainOpaque) {
    auto message = make_task_message();
    for (const auto& malformed : {std::string("\xE4\xB8"), std::string("\xC0\x80"),
            std::string("\xED\xA0\x80"), std::string("\xF4\x90\x80\x80")}) {
        message.text = malformed;
        EXPECT_FALSE(redclaw::protocol::parse_agent_message_v1(
            redclaw::protocol::serialize_agent_message_v1(message)).ok);
        auto chunk = message;
        chunk.type = redclaw::protocol::AgentMessageTypeV1::kEvent;
        const auto parsed = redclaw::protocol::parse_agent_message_v1(
            redclaw::protocol::serialize_agent_message_v1(chunk));
        ASSERT_TRUE(parsed.ok);
        EXPECT_EQ(parsed.value.text, malformed);
    }
    message.text = "UTF-8: \xE4\xB8\xAD \xF0\x9F\x9A\x80";
    EXPECT_TRUE(redclaw::protocol::parse_agent_message_v1(
        redclaw::protocol::serialize_agent_message_v1(message)).ok);
}

TEST(AgentProtocolV1, EpochGuardRejectsReplayAndOldEpoch) {
    redclaw::protocol::AgentEpochGuardV1 guard;
    auto message = make_task_message();
    std::string error;
    ASSERT_TRUE(guard.accept(message, &error)) << error;
    EXPECT_FALSE(guard.accept(message, &error));

    ++message.message_id;
    message.session_epoch = "old-epoch";
    EXPECT_FALSE(guard.accept(message, &error));

    guard.reset();
    EXPECT_TRUE(guard.accept(message, &error)) << error;
}

TEST(AgentProtocolV1, CatalogCarriesOpaqueIdWithoutFilesystemPath) {
    redclaw::protocol::AgentMessageEnvelopeV1 message;
    message.session_epoch = "epoch-3";
    message.message_id = 1;
    message.type = redclaw::protocol::AgentMessageTypeV1::kProjectCatalog;
    message.project_id = "p-a751efe6";
    message.display_name = "RedClaw Desktop";
    message.complete = true;
    std::string error;
    EXPECT_TRUE(redclaw::protocol::validate_agent_message_v1(message, &error)) << error;
    const std::string serialized = redclaw::protocol::serialize_agent_message_v1(message);
    EXPECT_EQ(serialized.find("F:\\"), std::string::npos);
}

TEST(AgentProtocolV1, CompleteCapabilitiesUpdateRemoteAuthorization) {
    redclaw::protocol::AgentMessageEnvelopeV1 message;
    message.type = redclaw::protocol::AgentMessageTypeV1::kCapabilities;

    EXPECT_FALSE(
        redclaw::protocol::agent_capabilities_authorization_update_v1(message).has_value());

    message.complete = true;
    message.available = true;
    const auto allowed =
        redclaw::protocol::agent_capabilities_authorization_update_v1(message);
    ASSERT_TRUE(allowed.has_value());
    EXPECT_TRUE(*allowed);

    message.available = false;
    message.error_code = "not_authorized";
    const auto denied =
        redclaw::protocol::agent_capabilities_authorization_update_v1(message);
    ASSERT_TRUE(denied.has_value());
    EXPECT_FALSE(*denied);

    message.type = redclaw::protocol::AgentMessageTypeV1::kProjectCatalog;
    EXPECT_FALSE(
        redclaw::protocol::agent_capabilities_authorization_update_v1(message).has_value());
}

TEST(AgentProtocolV1, EvidenceReferenceIsTypedBoundedAndBackwardCompatible) {
    auto message = make_task_message();
    message.type = redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest;
    message.text.clear();
    message.event_kind = "coordination_evidence";
    message.evidence_manifest_name = "agent-evidence-v1.json";
    message.evidence_sha256 = std::string(64, 'a');

    const auto parsed = redclaw::protocol::parse_agent_message_v1(
        redclaw::protocol::serialize_agent_message_v1(message));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.evidence_manifest_name, message.evidence_manifest_name);
    EXPECT_EQ(parsed.value.evidence_sha256, message.evidence_sha256);

    message.evidence_manifest_name = "C:\\private\\evidence.json";
    std::string error;
    EXPECT_FALSE(redclaw::protocol::validate_agent_message_v1(message, &error));
}

}  // namespace
