#include "redclaw/protocol/debug_bridge_protocol.h"

#include <gtest/gtest.h>

#include <string>

namespace {

redclaw::protocol::DebugBridgeEnvelopeV1 valid_message() {
    redclaw::protocol::DebugBridgeEnvelopeV1 message;
    message.bridge_id = "bridge-a";
    message.session_epoch = "epoch-a";
    message.message_id = 1;
    message.sequence = 1;
    message.sent_at_ms = 1234;
    message.type = redclaw::protocol::DebugBridgeMessageTypeV1::kCommandRequest;
    message.request_id = "request-a";
    message.action = "status";
    message.payload = "line=one\nline=two\\three";
    return message;
}

TEST(DebugBridgeProtocol, RoundTripsEscapedCommandPayload) {
    const auto input = valid_message();
    const auto parsed = redclaw::protocol::parse_debug_bridge_message_v1(
        redclaw::protocol::serialize_debug_bridge_message_v1(input));

    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.bridge_id, input.bridge_id);
    EXPECT_EQ(parsed.value.session_epoch, input.session_epoch);
    EXPECT_EQ(parsed.value.request_id, input.request_id);
    EXPECT_EQ(parsed.value.action, input.action);
    EXPECT_EQ(parsed.value.payload, input.payload);
}

TEST(DebugBridgeProtocol, RejectsOversizedPayloadAndMessage) {
    auto message = valid_message();
    message.payload.assign(redclaw::protocol::kMaxDebugBridgePayloadBytes + 1U, 'x');
    std::string error;
    EXPECT_FALSE(redclaw::protocol::validate_debug_bridge_message_v1(message, &error));
    EXPECT_FALSE(error.empty());

    const std::string oversized(redclaw::protocol::kMaxDebugBridgeMessageBytes + 1U, 'x');
    EXPECT_FALSE(redclaw::protocol::parse_debug_bridge_message_v1(oversized).ok);
}

TEST(DebugBridgeProtocol, RejectsMissingOrMalformedCommandIdentity) {
    auto message = valid_message();
    message.request_id.clear();
    std::string error;
    EXPECT_FALSE(redclaw::protocol::validate_debug_bridge_message_v1(message, &error));

    message = valid_message();
    message.action = "run shell";
    EXPECT_FALSE(redclaw::protocol::validate_debug_bridge_message_v1(message, &error));
}

TEST(DebugBridgeProtocol, EpochGuardRejectsReplayAndEpochChanges) {
    redclaw::protocol::DebugBridgeEpochGuardV1 guard;
    auto message = valid_message();
    std::string error;
    EXPECT_TRUE(guard.accept(message, &error)) << error;
    EXPECT_FALSE(guard.accept(message, &error));

    message.message_id = 2;
    message.sequence = 2;
    message.session_epoch = "epoch-b";
    EXPECT_FALSE(guard.accept(message, &error));

    guard.reset();
    EXPECT_TRUE(guard.accept(message, &error)) << error;
}

TEST(DebugBridgeProtocol, AgentFrameMustContainTypedAgentPayload) {
    auto message = valid_message();
    message.type = redclaw::protocol::DebugBridgeMessageTypeV1::kAgentFrame;
    message.action.clear();
    message.request_id.clear();
    message.payload.clear();
    std::string error;
    EXPECT_FALSE(redclaw::protocol::validate_debug_bridge_message_v1(message, &error));

    message.payload = "RCD-AGENT-V1\n";
    EXPECT_TRUE(redclaw::protocol::validate_debug_bridge_message_v1(message, &error)) << error;
}

}  // namespace
