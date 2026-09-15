#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "redclaw/protocol/protocol_module.h"

namespace redclaw::protocol {

inline constexpr std::size_t kMaxStreamControlMessageBytes = 64U * 1024U;
inline constexpr std::size_t kMaxLocalRuntimeControlFrameBytes = 64U * 1024U;
inline constexpr std::size_t kMaxRemoteLogChunkBytes = 8U * 1024U;
inline constexpr std::size_t kMaxRemoteInputBatchBytes = 4U * 1024U;
inline constexpr std::size_t kMaxRemoteInputEventsPerBatch = 64U;
inline constexpr std::size_t kMaxRemoteInputPressedKeys = 64U;
inline constexpr std::size_t kMaxMediaTransportArrivalsPerFeedback = 64U;
inline constexpr std::size_t kMaxDesktopDisplaysPerCatalog = 32U;
inline constexpr std::size_t kMaxDesktopDisplayIdBytes = 128U;
inline constexpr std::size_t kMaxDesktopDisplayNameBytes = 256U;

enum class StreamControlMessageTypeV1 {
    kHello,
    kCapabilities,
    kViewportRequest,
    kStreamTargetApplied,
    kSourceActivityState,
    kReceiverNetworkStats,
    kMediaTransportFeedback,
    kPlaybackStarvation,
    kKeyframeRequest,
    kPing,
    kPong,
    kRemoteLogRequest,
    kRemoteLogChunk,
    kRemoteLogComplete,
    kRemoteLogError,
    kInputCapabilities,
    kInputControlRequest,
    kInputControlStatus,
    kInputBatch,
    kInputStateSync,
    kInputReleaseAll,
    kDesktopDisplayCatalog,
    kCaptureRegionRequest,
    kCaptureRegionApplied,
    kCaptureRegionRejected,
};

enum class RemoteLogModeV1 {
    kSnapshot,
    kFollow,
    kStop,
};

enum class RemoteInputEventTypeV1 {
    kKeyDown,
    kKeyUp,
    kMouseMove,
    kMouseButtonDown,
    kMouseButtonUp,
    kMouseWheel,
    kMouseHorizontalWheel,
};

enum class RemoteInputMouseButtonV1 {
    kNone,
    kLeft,
    kRight,
    kMiddle,
    kX1,
    kX2,
};

enum class RemoteInputControlStateV1 {
    kUnavailable,
    kAvailable,
    kActive,
    kPaused,
    kDenied,
};

enum class RemoteInputStatusReasonV1 {
    kNone,
    kUnsupported,
    kNotAuthorized,
    kNoVideo,
    kLocalPause,
    kLeaseExpired,
    kQueueOverflow,
    kInjectionFailed,
    kDisconnected,
    kGeometryChanged,
    kStaleSequence,
};

struct RemoteInputEventV1 {
    RemoteInputEventTypeV1 type = RemoteInputEventTypeV1::kKeyDown;
    std::uint16_t scan_code = 0;
    std::uint16_t virtual_key = 0;
    bool extended = false;
    bool repeat = false;
    std::uint16_t normalized_x = 0;
    std::uint16_t normalized_y = 0;
    RemoteInputMouseButtonV1 mouse_button = RemoteInputMouseButtonV1::kNone;
    std::int32_t wheel_delta = 0;
};

enum class DesktopSourceActivityStateV1 {
    kUnknown,
    kActive,
    kStaticPending,
    kStatic,
};

struct MediaTransportArrivalV1 {
    std::uint64_t transport_sequence = 0;
    std::uint64_t receiver_steady_us = 0;
};

struct DesktopDisplayV1 {
    std::string display_id;
    std::string display_name;
    std::int32_t desktop_origin_x = 0;
    std::int32_t desktop_origin_y = 0;
    std::uint32_t pixel_width = 0;
    std::uint32_t pixel_height = 0;
    std::uint32_t rotation = 0;
    bool primary = false;
};

struct StreamControlMessageV1 {
    int schema_version = kSchemaVersionV1;
    StreamControlMessageTypeV1 type = StreamControlMessageTypeV1::kHello;
    std::string session_epoch;
    std::uint64_t message_id = 0;
    std::uint64_t sent_at_ms = 0;

    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
    std::uint32_t encoded_width = 0;
    std::uint32_t encoded_height = 0;
    std::uint32_t target_fps = 0;
    std::uint32_t target_bitrate_kbps = 0;

    std::uint64_t received_bytes = 0;
    std::uint64_t received_frames = 0;
    std::uint64_t decoded_frames = 0;
    std::uint64_t rendered_frames = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t reassembly_timeouts = 0;
    std::uint64_t received_fragments = 0;
    std::uint64_t reassembled_frames = 0;
    std::uint64_t completed_keyframes = 0;
    std::uint64_t dropped_keyframes = 0;
    std::uint64_t playback_starvation_count = 0;
    std::uint64_t incomplete_frame_id = 0;
    std::uint64_t latest_received_frame_id = 0;
    std::uint64_t latest_complete_frame_id = 0;
    std::uint64_t latest_complete_keyframe_id = 0;
    std::uint64_t observed_rate_revision = 0;
    std::uint64_t source_activity_revision = 0;
    DesktopSourceActivityStateV1 source_activity_state =
        DesktopSourceActivityStateV1::kUnknown;
    // Orthogonal to source motion: Host is retaining the last clear frame
    // while its bounded recovery admission is waiting for media capacity.
    bool media_budget_waiting = false;
    // Optional capture-status capability v1. Zero means an older peer.
    std::uint32_t capture_status_version = 0;
    // 0 unspecified, 1 capturing, 2 recovering, 3 paused. Future values ignored.
    std::uint32_t capture_status = 0;
    std::uint64_t capture_generation = 0;
    std::uint64_t capture_first_frame_id = 0;
    bool capture_retry_requested = false;
    std::uint64_t reference_frame_id = 0;
    std::uint64_t reference_keyframe_id = 0;
    std::uint64_t stream_geometry_revision = 0;
    std::uint64_t rate_revision = 0;
    std::uint64_t observed_source_activity_revision = 0;
    std::uint64_t latest_displayable_frame_id = 0;
    std::uint64_t latest_displayable_keyframe_id = 0;
    std::uint64_t latest_presented_frame_id = 0;
    std::uint32_t rtt_ms = 0;
    std::uint64_t transport_feedback_id = 0;
    std::vector<MediaTransportArrivalV1> media_transport_arrivals;

    std::string request_id;
    RemoteLogModeV1 log_mode = RemoteLogModeV1::kSnapshot;
    std::uint64_t log_cursor = 0;
    std::uint32_t chunk_index = 0;
    bool complete = false;
    bool gap = false;
    std::string payload;

    bool input_supported = false;
    bool input_authorized = false;
    bool input_requested_active = false;
    RemoteInputControlStateV1 input_state = RemoteInputControlStateV1::kUnavailable;
    RemoteInputStatusReasonV1 input_reason = RemoteInputStatusReasonV1::kNone;
    std::uint64_t input_sequence = 0;
    std::int32_t desktop_origin_x = 0;
    std::int32_t desktop_origin_y = 0;
    std::uint32_t desktop_width = 0;
    std::uint32_t desktop_height = 0;
    std::uint32_t desktop_rotation = 0;
    std::uint64_t desktop_geometry_revision = 0;
    std::uint32_t pressed_mouse_buttons = 0;
    std::vector<std::uint16_t> pressed_scan_codes;
    std::vector<RemoteInputEventV1> input_events;

    std::vector<DesktopDisplayV1> desktop_displays;
    std::uint64_t display_catalog_revision = 0;
    std::string display_id;
    std::uint16_t region_left = 0;
    std::uint16_t region_top = 0;
    std::uint16_t region_right = 0;
    std::uint16_t region_bottom = 0;
    std::uint64_t capture_region_revision = 0;
    std::uint32_t content_rect_x = 0;
    std::uint32_t content_rect_y = 0;
    std::uint32_t content_rect_width = 0;
    std::uint32_t content_rect_height = 0;
};

class StreamControlEpochGuardV1 final {
public:
    [[nodiscard]] bool accept(
        const StreamControlMessageV1& message,
        std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] const std::string& epoch() const noexcept;
    [[nodiscard]] std::uint64_t last_message_id() const noexcept;

private:
    std::string epoch_;
    std::uint64_t last_message_id_ = 0;
};

[[nodiscard]] bool validate_stream_control_message_v1(
    const StreamControlMessageV1& message,
    std::string* error = nullptr);
[[nodiscard]] std::string serialize_stream_control_message_v1(
    const StreamControlMessageV1& message);
[[nodiscard]] std::size_t stream_control_message_protobuf_size_v1(
    const StreamControlMessageV1& message);
[[nodiscard]] ParseResult<StreamControlMessageV1> parse_stream_control_message_v1(
    std::string_view serialized);
[[nodiscard]] std::string serialize_local_runtime_control_frame_v2(
    const StreamControlMessageV1& message, std::uint64_t observed_monotonic_us = 0);
[[nodiscard]] ParseResult<StreamControlMessageV1> parse_local_runtime_control_frame_v2(
    std::string_view line, std::uint64_t* emitted_monotonic_us = nullptr);

[[nodiscard]] std::string_view to_string(StreamControlMessageTypeV1 type);
[[nodiscard]] std::string_view to_string(DesktopSourceActivityStateV1 state);
[[nodiscard]] std::string_view to_string(RemoteLogModeV1 mode);
[[nodiscard]] std::string_view to_string(RemoteInputEventTypeV1 type);
[[nodiscard]] std::string_view to_string(RemoteInputMouseButtonV1 button);
[[nodiscard]] std::string_view to_string(RemoteInputControlStateV1 state);
[[nodiscard]] std::string_view to_string(RemoteInputStatusReasonV1 reason);

}  // namespace redclaw::protocol
