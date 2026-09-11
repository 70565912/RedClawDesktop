#include "redclaw/protocol/stream_control_protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>

namespace redclaw::protocol {
namespace {

constexpr std::string_view kLocalFramePrefix = "RCD-LOCAL-CONTROL-V2 ";
constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void assign_error(std::string value, std::string* error) {
    if (error != nullptr) {
        *error = std::move(value);
    }
}

bool valid_token(std::string_view value, std::size_t max_size) {
    if (value.empty() || value.size() > max_size) {
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

std::string base64_encode(std::string_view value) {
    std::string output;
    output.reserve(((value.size() + 2U) / 3U) * 4U);
    std::uint32_t accumulator = 0;
    int bits = -6;
    for (const unsigned char ch : value) {
        accumulator = (accumulator << 8U) | ch;
        bits += 8;
        while (bits >= 0) {
            output.push_back(kBase64Alphabet[(accumulator >> bits) & 0x3FU]);
            bits -= 6;
        }
    }
    if (bits > -6) {
        output.push_back(kBase64Alphabet[(accumulator << static_cast<unsigned>(-bits)) & 0x3FU]);
    }
    while (output.size() % 4U != 0) {
        output.push_back('=');
    }
    return output;
}

bool base64_decode(std::string_view value, std::string* output) {
    output->clear();
    output->reserve((value.size() / 4U) * 3U);
    std::uint32_t accumulator = 0;
    int bits = -8;
    for (const char ch : value) {
        if (ch == '=') {
            break;
        }
        const std::size_t decoded = kBase64Alphabet.find(ch);
        if (decoded == std::string_view::npos) {
            return false;
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(decoded);
        bits += 6;
        if (bits >= 0) {
            output->push_back(static_cast<char>((accumulator >> bits) & 0xFFU));
            bits -= 8;
        }
    }
    // Reject noncanonical padding, trailing garbage and discarded low bits.
    return base64_encode(*output) == value;
}

}  // namespace

std::string_view to_string(StreamControlMessageTypeV1 type) {
    switch (type) {
    case StreamControlMessageTypeV1::kHello: return "hello";
    case StreamControlMessageTypeV1::kCapabilities: return "capabilities";
    case StreamControlMessageTypeV1::kViewportRequest: return "viewport_request";
    case StreamControlMessageTypeV1::kStreamTargetApplied: return "stream_target_applied";
    case StreamControlMessageTypeV1::kSourceActivityState: return "source_activity_state";
    case StreamControlMessageTypeV1::kReceiverNetworkStats: return "receiver_network_stats";
    case StreamControlMessageTypeV1::kMediaTransportFeedback: return "media_transport_feedback";
    case StreamControlMessageTypeV1::kPlaybackStarvation: return "playback_starvation";
    case StreamControlMessageTypeV1::kKeyframeRequest: return "keyframe_request";
    case StreamControlMessageTypeV1::kPing: return "ping";
    case StreamControlMessageTypeV1::kPong: return "pong";
    case StreamControlMessageTypeV1::kRemoteLogRequest: return "remote_log_request";
    case StreamControlMessageTypeV1::kRemoteLogChunk: return "remote_log_chunk";
    case StreamControlMessageTypeV1::kRemoteLogComplete: return "remote_log_complete";
    case StreamControlMessageTypeV1::kRemoteLogError: return "remote_log_error";
    case StreamControlMessageTypeV1::kInputCapabilities: return "input_capabilities";
    case StreamControlMessageTypeV1::kInputControlRequest: return "input_control_request";
    case StreamControlMessageTypeV1::kInputControlStatus: return "input_control_status";
    case StreamControlMessageTypeV1::kInputBatch: return "input_batch";
    case StreamControlMessageTypeV1::kInputStateSync: return "input_state_sync";
    case StreamControlMessageTypeV1::kInputReleaseAll: return "input_release_all";
    case StreamControlMessageTypeV1::kDesktopDisplayCatalog: return "desktop_display_catalog";
    case StreamControlMessageTypeV1::kCaptureRegionRequest: return "capture_region_request";
    case StreamControlMessageTypeV1::kCaptureRegionApplied: return "capture_region_applied";
    case StreamControlMessageTypeV1::kCaptureRegionRejected: return "capture_region_rejected";
    }
    return "unknown";
}

std::string_view to_string(DesktopSourceActivityStateV1 state) {
    switch (state) {
    case DesktopSourceActivityStateV1::kUnknown: return "unknown";
    case DesktopSourceActivityStateV1::kActive: return "active";
    case DesktopSourceActivityStateV1::kStaticPending: return "static_pending";
    case DesktopSourceActivityStateV1::kStatic: return "static";
    }
    return "unknown";
}

std::string_view to_string(RemoteInputEventTypeV1 type) {
    switch (type) {
    case RemoteInputEventTypeV1::kKeyDown: return "key_down";
    case RemoteInputEventTypeV1::kKeyUp: return "key_up";
    case RemoteInputEventTypeV1::kMouseMove: return "mouse_move";
    case RemoteInputEventTypeV1::kMouseButtonDown: return "mouse_button_down";
    case RemoteInputEventTypeV1::kMouseButtonUp: return "mouse_button_up";
    case RemoteInputEventTypeV1::kMouseWheel: return "mouse_wheel";
    case RemoteInputEventTypeV1::kMouseHorizontalWheel: return "mouse_horizontal_wheel";
    }
    return "key_down";
}

std::string_view to_string(RemoteInputMouseButtonV1 button) {
    switch (button) {
    case RemoteInputMouseButtonV1::kNone: return "none";
    case RemoteInputMouseButtonV1::kLeft: return "left";
    case RemoteInputMouseButtonV1::kRight: return "right";
    case RemoteInputMouseButtonV1::kMiddle: return "middle";
    case RemoteInputMouseButtonV1::kX1: return "x1";
    case RemoteInputMouseButtonV1::kX2: return "x2";
    }
    return "none";
}

std::string_view to_string(RemoteInputControlStateV1 state) {
    switch (state) {
    case RemoteInputControlStateV1::kUnavailable: return "unavailable";
    case RemoteInputControlStateV1::kAvailable: return "available";
    case RemoteInputControlStateV1::kActive: return "active";
    case RemoteInputControlStateV1::kPaused: return "paused";
    case RemoteInputControlStateV1::kDenied: return "denied";
    }
    return "unavailable";
}

std::string_view to_string(RemoteInputStatusReasonV1 reason) {
    switch (reason) {
    case RemoteInputStatusReasonV1::kNone: return "none";
    case RemoteInputStatusReasonV1::kUnsupported: return "unsupported";
    case RemoteInputStatusReasonV1::kNotAuthorized: return "not_authorized";
    case RemoteInputStatusReasonV1::kNoVideo: return "no_video";
    case RemoteInputStatusReasonV1::kLocalPause: return "local_pause";
    case RemoteInputStatusReasonV1::kLeaseExpired: return "lease_expired";
    case RemoteInputStatusReasonV1::kQueueOverflow: return "queue_overflow";
    case RemoteInputStatusReasonV1::kInjectionFailed: return "injection_failed";
    case RemoteInputStatusReasonV1::kDisconnected: return "disconnected";
    case RemoteInputStatusReasonV1::kGeometryChanged: return "geometry_changed";
    case RemoteInputStatusReasonV1::kStaleSequence: return "stale_sequence";
    }
    return "none";
}

std::string_view to_string(RemoteLogModeV1 mode) {
    switch (mode) {
    case RemoteLogModeV1::kSnapshot: return "snapshot";
    case RemoteLogModeV1::kFollow: return "follow";
    case RemoteLogModeV1::kStop: return "stop";
    }
    return "snapshot";
}

bool validate_stream_control_message_v1(const StreamControlMessageV1& message, std::string* error) {
    if (message.schema_version != kSchemaVersionV1) {
        assign_error("stream control schema_version must be 1", error);
        return false;
    }
    if (!valid_token(message.session_epoch, 128)) {
        assign_error("stream control session_epoch is invalid", error);
        return false;
    }
    if (message.message_id == 0 || message.sent_at_ms == 0) {
        assign_error("stream control message_id and sent_at_ms must be non-zero", error);
        return false;
    }
    if (message.payload.size() > kMaxRemoteLogChunkBytes
        && message.type != StreamControlMessageTypeV1::kHello
        && message.type != StreamControlMessageTypeV1::kCapabilities) {
        assign_error("stream control payload exceeds message limit", error);
        return false;
    }
    if ((message.type == StreamControlMessageTypeV1::kViewportRequest
         && (message.viewport_width < 64 || message.viewport_width > 16384
             || message.viewport_height < 64 || message.viewport_height > 16384))
        || (message.type == StreamControlMessageTypeV1::kStreamTargetApplied
            && (message.encoded_width < 2 || message.encoded_width > 16384
                || message.encoded_height < 2 || message.encoded_height > 16384))) {
        assign_error("stream control dimensions are out of range", error);
        return false;
    }
    if (message.type == StreamControlMessageTypeV1::kStreamTargetApplied
        && (message.target_fps < 1 || message.target_fps > 240
            || message.target_bitrate_kbps < 400 || message.target_bitrate_kbps > 20000)) {
        assign_error("applied stream target is out of range", error);
        return false;
    }
    if (message.type == StreamControlMessageTypeV1::kSourceActivityState) {
        const bool reference_state =
            message.source_activity_state == DesktopSourceActivityStateV1::kStaticPending
            || message.source_activity_state == DesktopSourceActivityStateV1::kStatic;
        if (message.source_activity_revision == 0
            || message.stream_geometry_revision == 0
            || message.rate_revision == 0
            || (reference_state
                && (message.reference_frame_id == 0
                    || message.reference_keyframe_id == 0
                    || message.reference_keyframe_id < message.reference_frame_id))
            || (!reference_state
                && (message.reference_frame_id != 0
                    || message.reference_keyframe_id != 0))) {
            assign_error("source activity state is out of range", error);
            return false;
        }
    }
    if (message.type == StreamControlMessageTypeV1::kReceiverNetworkStats
        && (message.rtt_ms > 120000
            || message.decoded_frames > message.received_frames
            || message.rendered_frames > message.decoded_frames
            || message.dropped_frames > message.received_frames
            || message.reassembled_frames > message.received_frames
            || message.reassembled_frames > message.received_frames - message.dropped_frames
            || message.completed_keyframes > message.reassembled_frames
            || message.dropped_keyframes > message.dropped_frames
            || message.latest_complete_frame_id > message.latest_received_frame_id
            || message.latest_complete_keyframe_id > message.latest_complete_frame_id
            || (message.latest_received_frame_id > 0 && message.observed_rate_revision == 0)
            || message.latest_displayable_keyframe_id > message.latest_displayable_frame_id
            || message.latest_presented_frame_id > message.latest_displayable_frame_id
            || ((message.latest_displayable_frame_id > 0
                 || message.latest_presented_frame_id > 0)
                && message.observed_source_activity_revision == 0))) {
        assign_error("receiver network statistics are out of range", error);
        return false;
    }
    if (message.type == StreamControlMessageTypeV1::kKeyframeRequest
        && message.payload.size() > 128) {
        assign_error("keyframe request reason exceeds 128 bytes", error);
        return false;
    }
    if (message.type == StreamControlMessageTypeV1::kMediaTransportFeedback) {
        if (message.transport_feedback_id == 0
            || message.observed_rate_revision == 0
            || message.media_transport_arrivals.empty()
            || message.media_transport_arrivals.size()
                > kMaxMediaTransportArrivalsPerFeedback) {
            assign_error("media transport feedback is out of range", error);
            return false;
        }
        std::uint64_t previous_sequence = 0;
        std::uint64_t previous_time = 0;
        for (const auto& arrival : message.media_transport_arrivals) {
            if (arrival.transport_sequence == 0 || arrival.receiver_steady_us == 0
                || arrival.transport_sequence <= previous_sequence
                || arrival.receiver_steady_us < previous_time) {
                assign_error("media transport feedback arrivals are not monotonic", error);
                return false;
            }
            previous_sequence = arrival.transport_sequence;
            previous_time = arrival.receiver_steady_us;
        }
    }
    if (message.type == StreamControlMessageTypeV1::kPlaybackStarvation
        && (message.playback_starvation_count == 0
            || message.incomplete_frame_id == 0
            || message.latest_received_frame_id < message.incomplete_frame_id
            || message.latest_complete_frame_id > message.latest_received_frame_id
            || message.latest_complete_keyframe_id > message.latest_complete_frame_id
            || message.observed_rate_revision == 0
            || message.payload.empty()
            || message.payload.size() > 128)) {
        assign_error("playback starvation feedback is out of range", error);
        return false;
    }
    if ((message.type == StreamControlMessageTypeV1::kRemoteLogRequest
         || message.type == StreamControlMessageTypeV1::kRemoteLogChunk
         || message.type == StreamControlMessageTypeV1::kRemoteLogComplete
         || message.type == StreamControlMessageTypeV1::kRemoteLogError)
        && !valid_token(message.request_id, 128)) {
        assign_error("remote log request_id is invalid", error);
        return false;
    }
    if (message.type == StreamControlMessageTypeV1::kInputCapabilities
        && message.input_supported
        && (message.desktop_width == 0 || message.desktop_height == 0
            || message.desktop_rotation > 270 || message.desktop_rotation % 90 != 0
            || message.desktop_geometry_revision == 0)) {
        assign_error("remote input desktop geometry is invalid", error);
        return false;
    }
    if (message.type == StreamControlMessageTypeV1::kInputBatch) {
        if (message.input_sequence == 0 || message.desktop_geometry_revision == 0
            || message.input_events.empty()
            || message.input_events.size() > kMaxRemoteInputEventsPerBatch) {
            assign_error("remote input batch size or sequence is invalid", error);
            return false;
        }
        for (const auto& event : message.input_events) {
            const bool key_event = event.type == RemoteInputEventTypeV1::kKeyDown
                || event.type == RemoteInputEventTypeV1::kKeyUp;
            const bool move_event = event.type == RemoteInputEventTypeV1::kMouseMove;
            const bool button_event = event.type == RemoteInputEventTypeV1::kMouseButtonDown
                || event.type == RemoteInputEventTypeV1::kMouseButtonUp;
            const bool wheel_event = event.type == RemoteInputEventTypeV1::kMouseWheel
                || event.type == RemoteInputEventTypeV1::kMouseHorizontalWheel;
            if ((key_event && (event.scan_code == 0 || event.scan_code > 0xFFU
                    || event.virtual_key > 0xFFU || event.mouse_button != RemoteInputMouseButtonV1::kNone
                    || event.wheel_delta != 0
                    || (event.type == RemoteInputEventTypeV1::kKeyUp && event.repeat)))
                || (move_event && (event.scan_code != 0 || event.virtual_key != 0
                    || event.mouse_button != RemoteInputMouseButtonV1::kNone
                    || event.wheel_delta != 0 || event.repeat))
                || (button_event && (event.mouse_button == RemoteInputMouseButtonV1::kNone
                    || event.scan_code != 0 || event.virtual_key != 0
                    || event.wheel_delta != 0 || event.repeat))
                || (wheel_event && (event.wheel_delta == 0
                    || event.wheel_delta < -12000 || event.wheel_delta > 12000
                    || event.scan_code != 0 || event.virtual_key != 0
                    || event.mouse_button != RemoteInputMouseButtonV1::kNone
                    || event.repeat))
                || (!key_event && !move_event && !button_event && !wheel_event)) {
                assign_error("remote input event is invalid", error);
                return false;
            }
        }
        if (stream_control_message_protobuf_size_v1(message) > kMaxRemoteInputBatchBytes) {
            assign_error("remote input batch exceeds 4 KiB", error);
            return false;
        }
    }
    if (message.type == StreamControlMessageTypeV1::kInputStateSync
        && (message.input_sequence == 0 || message.desktop_geometry_revision == 0
            || message.pressed_scan_codes.size() > kMaxRemoteInputPressedKeys
            || message.pressed_mouse_buttons > 0x1FU
            || std::any_of(
                message.pressed_scan_codes.begin(),
                message.pressed_scan_codes.end(),
                [](std::uint16_t value) {
                    const std::uint16_t scan_code = value & 0x7FFFU;
                    return scan_code == 0 || scan_code > 0xFFU;
                }))) {
        assign_error("remote input state sync is invalid", error);
        return false;
    }
    if (message.type == StreamControlMessageTypeV1::kDesktopDisplayCatalog) {
        if (message.display_catalog_revision == 0
            || message.desktop_displays.empty()
            || message.desktop_displays.size() > kMaxDesktopDisplaysPerCatalog) {
            assign_error("desktop display catalog size is invalid", error);
            return false;
        }
        for (const auto& display : message.desktop_displays) {
            if (!valid_token(display.display_id, kMaxDesktopDisplayIdBytes)
                || display.display_name.empty()
                || display.display_name.size() > kMaxDesktopDisplayNameBytes
                || display.pixel_width < 64 || display.pixel_width > 32768
                || display.pixel_height < 64 || display.pixel_height > 32768
                || display.rotation > 270 || display.rotation % 90 != 0) {
                assign_error("desktop display descriptor is invalid", error);
                return false;
            }
        }
    }
    const bool capture_region_message =
        message.type == StreamControlMessageTypeV1::kCaptureRegionRequest
        || message.type == StreamControlMessageTypeV1::kCaptureRegionApplied
        || message.type == StreamControlMessageTypeV1::kCaptureRegionRejected;
    if (capture_region_message
        && (!valid_token(message.display_id, kMaxDesktopDisplayIdBytes)
            || message.capture_region_revision == 0
            || message.region_left >= message.region_right
            || message.region_top >= message.region_bottom)) {
        assign_error("capture region request metadata is invalid", error);
        return false;
    }
    if (message.type == StreamControlMessageTypeV1::kCaptureRegionApplied
        && (message.content_rect_width == 0 || message.content_rect_height == 0
            || message.content_rect_x + message.content_rect_width > message.encoded_width
            || message.content_rect_y + message.content_rect_height > message.encoded_height)) {
        assign_error("capture region applied content rect is invalid", error);
        return false;
    }
    if (message.type == StreamControlMessageTypeV1::kCaptureRegionRejected
        && (message.payload.empty() || message.payload.size() > 256)) {
        assign_error("capture region rejection reason is invalid", error);
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool StreamControlEpochGuardV1::accept(
    const StreamControlMessageV1& message,
    std::string* error) {
    if (message.type == StreamControlMessageTypeV1::kHello) {
        if (!epoch_.empty() && message.session_epoch != epoch_) {
            assign_error("hello belongs to a different active session epoch", error);
            return false;
        }
        if (epoch_.empty()) {
            epoch_ = message.session_epoch;
            last_message_id_ = message.message_id;
            if (error != nullptr) {
                error->clear();
            }
            return true;
        }
    } else if (epoch_.empty()) {
        assign_error("stream control hello is required before session messages", error);
        return false;
    }

    if (message.session_epoch != epoch_) {
        assign_error("stream control message belongs to a stale session epoch", error);
        return false;
    }
    if (message.message_id <= last_message_id_) {
        assign_error("stream control message id is stale or duplicated", error);
        return false;
    }
    last_message_id_ = message.message_id;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void StreamControlEpochGuardV1::reset() noexcept {
    epoch_.clear();
    last_message_id_ = 0;
}

const std::string& StreamControlEpochGuardV1::epoch() const noexcept {
    return epoch_;
}

std::uint64_t StreamControlEpochGuardV1::last_message_id() const noexcept {
    return last_message_id_;
}

std::string serialize_local_runtime_control_frame_v2(
    const StreamControlMessageV1& message, std::uint64_t observed_monotonic_us) {
    if (!observed_monotonic_us) observed_monotonic_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto wire = serialize_stream_control_message_v1(message);
    if (wire.empty()) return {};
    std::string frame = std::string(kLocalFramePrefix)
        + std::to_string(observed_monotonic_us) + ' '
        + base64_encode(wire);
    if (frame.size() > kMaxLocalRuntimeControlFrameBytes) {
        return {};
    }
    return frame;
}

ParseResult<StreamControlMessageV1> parse_local_runtime_control_frame_v2(
    std::string_view line, std::uint64_t* emitted_monotonic_us) {
    if (emitted_monotonic_us) *emitted_monotonic_us = 0;
    ParseResult<StreamControlMessageV1> result;
    if (!line.starts_with(kLocalFramePrefix)
        || line.size() > kMaxLocalRuntimeControlFrameBytes) {
        result.error = "unexpected local runtime control frame";
        return result;
    }
    std::string decoded;
    const auto payload = line.substr(kLocalFramePrefix.size());
    const auto separator = payload.find(' ');
    std::uint64_t emitted = 0;
    if (separator == std::string_view::npos || separator == 0) {
        result.error = "missing local emission timestamp";
        return result;
    }
    const auto parsed_time = std::from_chars(payload.data(), payload.data() + separator, emitted);
    if (parsed_time.ec != std::errc{} || parsed_time.ptr != payload.data() + separator || emitted == 0) {
        result.error = "invalid local emission timestamp";
        return result;
    }
    if (!base64_decode(payload.substr(separator + 1), &decoded)) {
        result.error = "invalid local runtime control base64";
        return result;
    }
    result = parse_stream_control_message_v1(decoded);
    if (result.ok && emitted_monotonic_us) *emitted_monotonic_us = emitted;
    return result;
}

}  // namespace redclaw::protocol
