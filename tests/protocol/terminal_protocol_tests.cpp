#include <gtest/gtest.h>
#include "redclaw/protocol/terminal_protocol.h"
#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/protocol/compressed_protobuf.h"
#include "redclaw_wire.pb.h"

namespace {
using namespace redclaw::protocol;
TEST(TerminalProtocol, PreservesControlBytesAndRejectsOversizedPayloads) {
    TerminalMessageV1 message;
    message.type = TerminalMessageTypeV1::kOutput;
    message.session_epoch = "desktop-session-1";
    message.terminal_id = "terminal-1";
    message.sequence = 9;
    message.bytes = std::string("\x1b[32mhello\0\x1b[0m", 15);
    const auto parsed = parse_terminal_message_v1(serialize_terminal_message_v1(message));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.bytes, message.bytes);
    EXPECT_EQ(parsed.value.sequence, 9U);
    message.bytes.assign(kMaxTerminalChunkBytes + 1, 'x');
    EXPECT_TRUE(serialize_terminal_message_v1(message).empty());
    wire::TerminalMessageV1 hostile;
    hostile.set_schema_version(1); hostile.set_session_epoch("epoch"); hostile.set_terminal_id("terminal");
    hostile.set_type(static_cast<std::uint32_t>(TerminalMessageTypeV1::kOutput)); hostile.set_sequence(1);
    hostile.set_data(message.bytes);
    const auto rejected = parse_terminal_message_v1(compress_protobuf(hostile, ProtobufWireKind::kTerminal));
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error, "terminal_invalid_payload");
}
TEST(TerminalProtocol, ExplicitVersionAndIdentityBoundaries) {
    wire::TerminalMessageV1 future;
    future.set_schema_version(2); future.set_session_epoch("epoch"); future.set_columns(80); future.set_rows(25);
    auto parsed = parse_terminal_message_v1(compress_protobuf(future, ProtobufWireKind::kTerminal));
    EXPECT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.error, "protocol_version_incompatible");
    future.set_schema_version(1);
    future.set_client_session_id("client-desktop");
    future.GetReflection()->MutableUnknownFields(&future)->AddLengthDelimited(100, "optional future metadata");
    EXPECT_TRUE(parse_terminal_message_v1(compress_protobuf(future, ProtobufWireKind::kTerminal)).ok);
    EXPECT_FALSE(parse_terminal_message_v1(compress_protobuf(future, ProtobufWireKind::kAgent)).ok);
    future.set_type(999);
    EXPECT_FALSE(parse_terminal_message_v1(compress_protobuf(future, ProtobufWireKind::kTerminal)).ok);
    future.set_type(static_cast<std::uint32_t>(TerminalMessageTypeV1::kInput));
    future.set_data("whoami\r"); future.set_sequence(1); future.set_input_generation(1);
    EXPECT_FALSE(parse_terminal_message_v1(compress_protobuf(future, ProtobufWireKind::kTerminal)).ok);
    future.set_terminal_id("terminal");
    EXPECT_TRUE(parse_terminal_message_v1(compress_protobuf(future, ProtobufWireKind::kTerminal)).ok);
}
TEST(TerminalProtocol, MissingCapabilityIsDisabledAndUnknownFieldsRemainOptional) {
    StreamControlMessageV1 capabilities;
    capabilities.type = StreamControlMessageTypeV1::kCapabilities;
    capabilities.session_epoch = "epoch";
    capabilities.message_id = 1;
    capabilities.sent_at_ms = 1;
    auto parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(capabilities));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.terminal_version, 0U);
    capabilities.terminal_version = 1;
    parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(capabilities));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.terminal_version, 1U);
    EXPECT_EQ(parsed.value.capture_status_version, 0U);
}
TEST(TerminalProtocol, ExecutionReceiptRequiresOperationAndKnownState) {
    TerminalMessageV1 receipt;
    receipt.type = TerminalMessageTypeV1::kExecState; receipt.session_epoch = "session"; receipt.terminal_id = "shell";
    receipt.operation_id = "operation"; receipt.execution_state = "succeeded";
    receipt.powershell_success = true; receipt.has_native_exit_code = true; receipt.last_native_exit_code = 7;
    const auto parsed = parse_terminal_message_v1(serialize_terminal_message_v1(receipt));
    ASSERT_TRUE(parsed.ok); EXPECT_TRUE(parsed.value.powershell_success); EXPECT_EQ(parsed.value.last_native_exit_code, 7);
    receipt.operation_id.clear(); EXPECT_FALSE(validate_terminal_message_v1(receipt));
    receipt.operation_id = "operation"; receipt.execution_state = "not-a-state"; EXPECT_FALSE(validate_terminal_message_v1(receipt));
    receipt.execution_state = "output"; receipt.sequence = 1; receipt.bytes = "merged output";
    EXPECT_TRUE(parse_terminal_message_v1(serialize_terminal_message_v1(receipt)).ok);
}
}
