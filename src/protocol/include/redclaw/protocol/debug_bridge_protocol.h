#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "redclaw/protocol/protocol_module.h"

namespace redclaw::protocol {

inline constexpr std::size_t kMaxDebugBridgeMessageBytes = 64U * 1024U;
inline constexpr std::size_t kMaxDebugBridgePayloadBytes = 48U * 1024U;
inline constexpr std::size_t kMaxDebugBridgeAgentInstructionBytes = 16U * 1024U;
inline constexpr std::size_t kMaxDebugBridgeTextChunkBytes = 8U * 1024U;

enum class DebugBridgeMessageTypeV1 {
    kHello,
    kHeartbeat,
    kStatusRequest,
    kStatusResponse,
    kCommandRequest,
    kCommandResult,
    kAgentFrame,
    kError,
};

struct DebugBridgeEnvelopeV1 {
    int schema_version = kSchemaVersionV1;
    std::string bridge_id;
    std::string session_epoch;
    std::uint64_t message_id = 0;
    std::uint64_t sequence = 0;
    std::uint64_t acknowledged_sequence = 0;
    std::uint64_t sent_at_ms = 0;
    DebugBridgeMessageTypeV1 type = DebugBridgeMessageTypeV1::kHello;
    std::string request_id;
    std::string action;
    std::string payload;
    bool ok = false;
    std::string error_code;
};

class DebugBridgeEpochGuardV1 final {
public:
    [[nodiscard]] bool accept(
        const DebugBridgeEnvelopeV1& message,
        std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] const std::string& epoch() const noexcept;
    [[nodiscard]] std::uint64_t last_message_id() const noexcept;
    [[nodiscard]] std::uint64_t last_sequence() const noexcept;

private:
    std::string epoch_;
    std::uint64_t last_message_id_ = 0;
    std::uint64_t last_sequence_ = 0;
};

[[nodiscard]] bool validate_debug_bridge_message_v1(
    const DebugBridgeEnvelopeV1& message,
    std::string* error = nullptr);
[[nodiscard]] std::string serialize_debug_bridge_message_v1(
    const DebugBridgeEnvelopeV1& message);
[[nodiscard]] ParseResult<DebugBridgeEnvelopeV1> parse_debug_bridge_message_v1(
    std::string_view serialized);
[[nodiscard]] std::string_view to_string(DebugBridgeMessageTypeV1 type);

}  // namespace redclaw::protocol
