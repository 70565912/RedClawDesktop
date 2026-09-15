#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace google::protobuf { class MessageLite; }

namespace redclaw::protocol {

// This is a wire-format version, independent of the domain envelope's schema.
// Every frame contains one complete, independently compressed Protobuf message.
enum class ProtobufWireKind : std::uint8_t {
    kControl = 1, kAgent = 2, kDht = 3, kDebugBridge = 4, kTerminal = 5, kTransfer = 6
};
inline constexpr std::size_t kMaxProtobufWireBytes = 64U * 1024U;

[[nodiscard]] std::string compress_protobuf(
    const google::protobuf::MessageLite& message, ProtobufWireKind kind);
[[nodiscard]] bool decompress_protobuf(
    std::string_view frame, ProtobufWireKind expected_kind,
    google::protobuf::MessageLite& message, std::string* error = nullptr);

}  // namespace redclaw::protocol
