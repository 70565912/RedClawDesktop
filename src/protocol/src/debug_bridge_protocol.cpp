#include "redclaw/protocol/debug_bridge_protocol.h"

#include <cctype>
#include <utility>

#include "redclaw/protocol/compressed_protobuf.h"
#include "redclaw_wire.pb.h"

namespace redclaw::protocol {
namespace {

void assign_error(std::string value, std::string* error) {
    if (error != nullptr) {
        *error = std::move(value);
    }
}

bool valid_token(std::string_view value, std::size_t max_size, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > max_size) {
        return false;
    }
    for (const char ch : value) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) == 0 && ch != '-' && ch != '_' && ch != '.') {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string_view to_string(DebugBridgeMessageTypeV1 type) {
    switch (type) {
        case DebugBridgeMessageTypeV1::kHello: return "hello";
        case DebugBridgeMessageTypeV1::kHeartbeat: return "heartbeat";
        case DebugBridgeMessageTypeV1::kStatusRequest: return "status_request";
        case DebugBridgeMessageTypeV1::kStatusResponse: return "status_response";
        case DebugBridgeMessageTypeV1::kCommandRequest: return "command_request";
        case DebugBridgeMessageTypeV1::kCommandResult: return "command_result";
        case DebugBridgeMessageTypeV1::kAgentFrame: return "agent_frame";
        case DebugBridgeMessageTypeV1::kError: return "error";
    }
    return "error";
}

bool validate_debug_bridge_message_v1(
    const DebugBridgeEnvelopeV1& message,
    std::string* error) {
    if (message.schema_version != kSchemaVersionV1) {
        assign_error("unsupported debug bridge schema version", error);
        return false;
    }
    if (!valid_token(message.bridge_id, 128)
        || !valid_token(message.session_epoch, 128)
        || message.message_id == 0
        || message.sequence == 0) {
        assign_error("invalid debug bridge envelope identity", error);
        return false;
    }
    if (!valid_token(message.request_id, 128, true)
        || !valid_token(message.action, 64, true)
        || !valid_token(message.error_code, 64, true)) {
        assign_error("invalid debug bridge token field", error);
        return false;
    }
    if (message.payload.size() > kMaxDebugBridgePayloadBytes) {
        assign_error("debug bridge payload exceeds limit", error);
        return false;
    }
    if ((message.type == DebugBridgeMessageTypeV1::kCommandRequest
         || message.type == DebugBridgeMessageTypeV1::kCommandResult)
        && (message.request_id.empty() || message.action.empty())) {
        assign_error("debug bridge command envelope is incomplete", error);
        return false;
    }
    if (message.type == DebugBridgeMessageTypeV1::kAgentFrame
        && message.payload.empty()) {
        assign_error("debug bridge Agent frame is empty", error);
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::string serialize_debug_bridge_message_v1(const DebugBridgeEnvelopeV1& message) {
    wire::DebugBridgeEnvelopeV1 packed;
    packed.set_schema_version(message.schema_version);
    packed.set_bridge_id(message.bridge_id);
    packed.set_session_epoch(message.session_epoch);
    packed.set_message_id(message.message_id);
    packed.set_sequence(message.sequence);
    packed.set_acknowledged_sequence(message.acknowledged_sequence);
    packed.set_sent_at_ms(message.sent_at_ms);
    packed.set_type(static_cast<wire::DebugBridgeMessageTypeV1>(message.type));
    packed.set_request_id(message.request_id);
    packed.set_action(message.action);
    packed.set_payload(message.payload);
    packed.set_ok(message.ok);
    packed.set_error_code(message.error_code);
    return compress_protobuf(packed, ProtobufWireKind::kDebugBridge);
}

ParseResult<DebugBridgeEnvelopeV1> parse_debug_bridge_message_v1(
    std::string_view serialized) {
    ParseResult<DebugBridgeEnvelopeV1> result;
    wire::DebugBridgeEnvelopeV1 packed;
    if (!decompress_protobuf(serialized, ProtobufWireKind::kDebugBridge, packed, &result.error)) {
        return result;
    }
    if (!wire::DebugBridgeMessageTypeV1_IsValid(packed.type())) {
        result.error = "invalid debug bridge message type";
        return result;
    }
    auto& message = result.value;
    message.schema_version = packed.schema_version();
    message.bridge_id = packed.bridge_id();
    message.session_epoch = packed.session_epoch();
    message.message_id = packed.message_id();
    message.sequence = packed.sequence();
    message.acknowledged_sequence = packed.acknowledged_sequence();
    message.sent_at_ms = packed.sent_at_ms();
    message.type = static_cast<DebugBridgeMessageTypeV1>(packed.type());
    message.request_id = packed.request_id();
    message.action = packed.action();
    message.payload = packed.payload();
    message.ok = packed.ok();
    message.error_code = packed.error_code();
    result.ok = validate_debug_bridge_message_v1(message, &result.error);
    return result;
}

bool DebugBridgeEpochGuardV1::accept(
    const DebugBridgeEnvelopeV1& message,
    std::string* error) {
    if (!validate_debug_bridge_message_v1(message, error)) {
        return false;
    }
    if (epoch_.empty()) {
        epoch_ = message.session_epoch;
    } else if (message.session_epoch != epoch_) {
        assign_error("debug bridge epoch mismatch", error);
        return false;
    }
    if (message.message_id <= last_message_id_ || message.sequence <= last_sequence_) {
        assign_error("debug bridge replay or out-of-order message", error);
        return false;
    }
    last_message_id_ = message.message_id;
    last_sequence_ = message.sequence;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void DebugBridgeEpochGuardV1::reset() noexcept {
    epoch_.clear();
    last_message_id_ = 0;
    last_sequence_ = 0;
}

const std::string& DebugBridgeEpochGuardV1::epoch() const noexcept {
    return epoch_;
}

std::uint64_t DebugBridgeEpochGuardV1::last_message_id() const noexcept {
    return last_message_id_;
}

std::uint64_t DebugBridgeEpochGuardV1::last_sequence() const noexcept {
    return last_sequence_;
}

}  // namespace redclaw::protocol
