#pragma once

#include <limits>
#include <optional>

#include "redclaw/protocol/stream_control_protocol.h"

namespace redclaw::protocol {

inline constexpr std::uint32_t kCaptureRegionCommandVersion = 1;

// Legacy peers use the requested revision as both correlation and geometry.
// Negotiated commands keep correlation independent of Host-owned geometry.
inline std::uint64_t capture_region_request_token(const StreamControlMessageV1& message) {
    return message.capture_region_command_version == kCaptureRegionCommandVersion
        ? message.capture_region_request_id : message.capture_region_revision;
}

inline void set_capture_region_request_identity(
    StreamControlMessageV1& message, std::uint64_t token, std::uint32_t peer_version) {
    message.capture_region_command_version = peer_version >= kCaptureRegionCommandVersion
        ? kCaptureRegionCommandVersion : 0;
    message.capture_region_request_id = message.capture_region_command_version ? token : 0;
    message.capture_region_revision = message.capture_region_command_version ? 0 : token;
}

inline std::optional<std::uint64_t> assign_capture_region_revision(
    const StreamControlMessageV1& request, std::uint64_t current_revision) {
    if (request.capture_region_command_version == kCaptureRegionCommandVersion) {
        if (request.capture_region_request_id == 0
            || current_revision == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
        return current_revision + 1;
    }
    if (request.capture_region_revision <= current_revision) return std::nullopt;
    return request.capture_region_revision;
}

inline void copy_capture_region_reply(
    const StreamControlMessageV1& request, StreamControlMessageV1& reply) {
    reply.display_id = request.display_id;
    reply.region_left = request.region_left;
    reply.region_top = request.region_top;
    reply.region_right = request.region_right;
    reply.region_bottom = request.region_bottom;
    reply.capture_region_command_version = request.capture_region_command_version;
    reply.capture_region_request_id = request.capture_region_request_id;
    reply.capture_region_revision = request.capture_region_revision;
}

}  // namespace redclaw::protocol
