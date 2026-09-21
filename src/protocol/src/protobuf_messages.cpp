#include "redclaw/protocol/agent_protocol.h"
#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/protocol/compressed_protobuf.h"
#include "redclaw_wire.pb.h"

#include <limits>

namespace redclaw::protocol {
namespace {
wire::AgentMessageEnvelopeV1 to_wire(const AgentMessageEnvelopeV1& message);
bool from_wire(const wire::AgentMessageEnvelopeV1& encoded, AgentMessageEnvelopeV1& message);
wire::RemoteInputEventV1 to_wire(const RemoteInputEventV1& message);
bool from_wire(const wire::RemoteInputEventV1& encoded, RemoteInputEventV1& message);
wire::MediaTransportArrivalV1 to_wire(const MediaTransportArrivalV1& message);
bool from_wire(const wire::MediaTransportArrivalV1& encoded, MediaTransportArrivalV1& message);
wire::DesktopDisplayV1 to_wire(const DesktopDisplayV1& message);
bool from_wire(const wire::DesktopDisplayV1& encoded, DesktopDisplayV1& message);
wire::StreamControlMessageV1 to_wire(const StreamControlMessageV1& message);
bool from_wire(const wire::StreamControlMessageV1& encoded, StreamControlMessageV1& message);

wire::AgentMessageEnvelopeV1 to_wire(const AgentMessageEnvelopeV1& message) {
    wire::AgentMessageEnvelopeV1 encoded;
    encoded.set_schema_version(message.schema_version);
    encoded.set_session_epoch(message.session_epoch);
    encoded.set_message_id(message.message_id);
    encoded.set_sent_at_ms(message.sent_at_ms);
    encoded.set_task_id(message.task_id);
    encoded.set_request_id(message.request_id);
    encoded.set_supersedes_request_id(message.supersedes_request_id);
    encoded.set_event_sequence(message.event_sequence);
    encoded.set_type(static_cast<wire::AgentMessageTypeV1>(message.type));
    encoded.set_provider(static_cast<wire::AgentProviderKindV1>(message.provider));
    encoded.set_provider_readiness(static_cast<wire::AgentProviderReadinessV1>(message.provider_readiness));
    encoded.set_work_directory_mode(static_cast<wire::AgentWorkDirectoryModeV1>(message.work_directory_mode));
    encoded.set_task_state(static_cast<wire::AgentTaskStateV1>(message.task_state));
    encoded.set_approval_decision(static_cast<wire::AgentApprovalDecisionV1>(message.approval_decision));
    encoded.set_model(message.model);
    encoded.set_project_id(message.project_id);
    encoded.set_display_name(message.display_name);
    encoded.set_event_kind(message.event_kind);
    encoded.set_error_code(message.error_code);
    encoded.set_evidence_manifest_name(message.evidence_manifest_name);
    encoded.set_evidence_sha256(message.evidence_sha256);
    encoded.set_text(message.text);
    encoded.set_acknowledged_event_sequence(message.acknowledged_event_sequence);
    encoded.set_available(message.available);
    encoded.set_complete(message.complete);
    encoded.set_gap(message.gap);
    encoded.set_git_repository(message.git_repository);
    encoded.set_requires_turn_approval(message.requires_turn_approval);
    encoded.set_supports_structured_approval(message.supports_structured_approval);
    return encoded;
}

bool from_wire(const wire::AgentMessageEnvelopeV1& encoded, AgentMessageEnvelopeV1& message) {
    message.schema_version = encoded.schema_version();
    message.session_epoch = encoded.session_epoch();
    message.message_id = encoded.message_id();
    message.sent_at_ms = encoded.sent_at_ms();
    message.task_id = encoded.task_id();
    message.request_id = encoded.request_id();
    message.supersedes_request_id = encoded.supersedes_request_id();
    message.event_sequence = encoded.event_sequence();
    if (!wire::AgentMessageTypeV1_IsValid(static_cast<int>(encoded.type()))) return false;
    message.type = static_cast<AgentMessageTypeV1>(encoded.type());
    if (!wire::AgentProviderKindV1_IsValid(static_cast<int>(encoded.provider()))) return false;
    message.provider = static_cast<AgentProviderKindV1>(encoded.provider());
    if (!wire::AgentProviderReadinessV1_IsValid(static_cast<int>(encoded.provider_readiness()))) return false;
    message.provider_readiness = static_cast<AgentProviderReadinessV1>(encoded.provider_readiness());
    if (!wire::AgentWorkDirectoryModeV1_IsValid(static_cast<int>(encoded.work_directory_mode()))) return false;
    message.work_directory_mode = static_cast<AgentWorkDirectoryModeV1>(encoded.work_directory_mode());
    if (!wire::AgentTaskStateV1_IsValid(static_cast<int>(encoded.task_state()))) return false;
    message.task_state = static_cast<AgentTaskStateV1>(encoded.task_state());
    if (!wire::AgentApprovalDecisionV1_IsValid(static_cast<int>(encoded.approval_decision()))) return false;
    message.approval_decision = static_cast<AgentApprovalDecisionV1>(encoded.approval_decision());
    message.model = encoded.model();
    message.project_id = encoded.project_id();
    message.display_name = encoded.display_name();
    message.event_kind = encoded.event_kind();
    message.error_code = encoded.error_code();
    message.evidence_manifest_name = encoded.evidence_manifest_name();
    message.evidence_sha256 = encoded.evidence_sha256();
    message.text = encoded.text();
    message.acknowledged_event_sequence = encoded.acknowledged_event_sequence();
    message.available = encoded.available();
    message.complete = encoded.complete();
    message.gap = encoded.gap();
    message.git_repository = encoded.git_repository();
    message.requires_turn_approval = encoded.requires_turn_approval();
    message.supports_structured_approval = encoded.supports_structured_approval();
    return true;
}

wire::RemoteInputEventV1 to_wire(const RemoteInputEventV1& message) {
    wire::RemoteInputEventV1 encoded;
    encoded.set_type(static_cast<wire::RemoteInputEventTypeV1>(message.type));
    encoded.set_scan_code(message.scan_code);
    encoded.set_virtual_key(message.virtual_key);
    encoded.set_extended(message.extended);
    encoded.set_repeat(message.repeat);
    encoded.set_normalized_x(message.normalized_x);
    encoded.set_normalized_y(message.normalized_y);
    encoded.set_mouse_button(static_cast<wire::RemoteInputMouseButtonV1>(message.mouse_button));
    encoded.set_wheel_delta(message.wheel_delta);
    return encoded;
}

bool from_wire(const wire::RemoteInputEventV1& encoded, RemoteInputEventV1& message) {
    if (!wire::RemoteInputEventTypeV1_IsValid(static_cast<int>(encoded.type()))) return false;
    message.type = static_cast<RemoteInputEventTypeV1>(encoded.type());
    if (encoded.scan_code() > std::numeric_limits<std::uint16_t>::max()) return false;
    message.scan_code = static_cast<std::uint16_t>(encoded.scan_code());
    if (encoded.virtual_key() > std::numeric_limits<std::uint16_t>::max()) return false;
    message.virtual_key = static_cast<std::uint16_t>(encoded.virtual_key());
    message.extended = encoded.extended();
    message.repeat = encoded.repeat();
    if (encoded.normalized_x() > std::numeric_limits<std::uint16_t>::max()) return false;
    message.normalized_x = static_cast<std::uint16_t>(encoded.normalized_x());
    if (encoded.normalized_y() > std::numeric_limits<std::uint16_t>::max()) return false;
    message.normalized_y = static_cast<std::uint16_t>(encoded.normalized_y());
    if (!wire::RemoteInputMouseButtonV1_IsValid(static_cast<int>(encoded.mouse_button()))) return false;
    message.mouse_button = static_cast<RemoteInputMouseButtonV1>(encoded.mouse_button());
    message.wheel_delta = encoded.wheel_delta();
    return true;
}

wire::MediaTransportArrivalV1 to_wire(const MediaTransportArrivalV1& message) {
    wire::MediaTransportArrivalV1 encoded;
    encoded.set_transport_sequence(message.transport_sequence);
    encoded.set_receiver_steady_us(message.receiver_steady_us);
    return encoded;
}

bool from_wire(const wire::MediaTransportArrivalV1& encoded, MediaTransportArrivalV1& message) {
    message.transport_sequence = encoded.transport_sequence();
    message.receiver_steady_us = encoded.receiver_steady_us();
    return true;
}

wire::DesktopDisplayV1 to_wire(const DesktopDisplayV1& message) {
    wire::DesktopDisplayV1 encoded;
    encoded.set_display_id(message.display_id);
    encoded.set_display_name(message.display_name);
    encoded.set_desktop_origin_x(message.desktop_origin_x);
    encoded.set_desktop_origin_y(message.desktop_origin_y);
    encoded.set_pixel_width(message.pixel_width);
    encoded.set_pixel_height(message.pixel_height);
    encoded.set_rotation(message.rotation);
    encoded.set_primary(message.primary);
    return encoded;
}

bool from_wire(const wire::DesktopDisplayV1& encoded, DesktopDisplayV1& message) {
    message.display_id = encoded.display_id();
    message.display_name = encoded.display_name();
    message.desktop_origin_x = encoded.desktop_origin_x();
    message.desktop_origin_y = encoded.desktop_origin_y();
    message.pixel_width = encoded.pixel_width();
    message.pixel_height = encoded.pixel_height();
    message.rotation = encoded.rotation();
    message.primary = encoded.primary();
    return true;
}

wire::StreamControlMessageV1 to_wire(const StreamControlMessageV1& message) {
    wire::StreamControlMessageV1 encoded;
    encoded.set_schema_version(message.schema_version);
    encoded.set_type(static_cast<wire::StreamControlMessageTypeV1>(message.type));
    encoded.set_session_epoch(message.session_epoch);
    encoded.set_message_id(message.message_id);
    encoded.set_sent_at_ms(message.sent_at_ms);
    encoded.set_viewport_width(message.viewport_width);
    encoded.set_viewport_height(message.viewport_height);
    encoded.set_encoded_width(message.encoded_width);
    encoded.set_encoded_height(message.encoded_height);
    encoded.set_target_fps(message.target_fps);
    encoded.set_target_bitrate_kbps(message.target_bitrate_kbps);
    encoded.set_received_bytes(message.received_bytes);
    encoded.set_received_frames(message.received_frames);
    encoded.set_decoded_frames(message.decoded_frames);
    encoded.set_rendered_frames(message.rendered_frames);
    encoded.set_dropped_frames(message.dropped_frames);
    encoded.set_reassembly_timeouts(message.reassembly_timeouts);
    encoded.set_received_fragments(message.received_fragments);
    encoded.set_reassembled_frames(message.reassembled_frames);
    encoded.set_completed_keyframes(message.completed_keyframes);
    encoded.set_dropped_keyframes(message.dropped_keyframes);
    encoded.set_playback_starvation_count(message.playback_starvation_count);
    encoded.set_incomplete_frame_id(message.incomplete_frame_id);
    encoded.set_latest_received_frame_id(message.latest_received_frame_id);
    encoded.set_latest_complete_frame_id(message.latest_complete_frame_id);
    encoded.set_latest_complete_keyframe_id(message.latest_complete_keyframe_id);
    encoded.set_observed_rate_revision(message.observed_rate_revision);
    encoded.set_source_activity_revision(message.source_activity_revision);
    encoded.set_source_activity_state(static_cast<wire::DesktopSourceActivityStateV1>(message.source_activity_state));
    encoded.set_media_budget_waiting(message.media_budget_waiting);
    encoded.set_capture_status_version(message.capture_status_version);
    encoded.set_terminal_version(message.terminal_version);
    encoded.set_file_transfer_version(message.file_transfer_version);
    encoded.set_clipboard_version(message.clipboard_version);
    encoded.set_connection_auth_version(message.connection_auth_version);
    encoded.set_auth_step(message.auth_step);
    encoded.set_auth_data(message.auth_data);
    if (message.workspace) {
        const auto& data = *message.workspace;
        auto* nested = encoded.mutable_workspace();
        nested->set_schema_version(data.schema_version); nested->set_action(static_cast<std::uint32_t>(data.action));
        nested->set_direction(static_cast<std::uint32_t>(data.direction)); nested->set_path(data.path);
        nested->set_error_code(data.error_code); nested->set_entries(data.entries); nested->set_files(data.files);
        nested->set_bytes(data.bytes); nested->set_completed_entries(data.completed_entries);
        nested->set_completed_bytes(data.completed_bytes); nested->set_directory(data.directory);
        nested->set_conflict(static_cast<std::uint32_t>(data.conflict));
        nested->set_accepted_sources(data.accepted_sources);
        nested->set_committed_bytes(data.committed_bytes); nested->set_completed_files(data.completed_files);
        nested->set_skipped_entries(data.skipped_entries);
        nested->set_results_path(data.results_path);
        nested->set_purpose(static_cast<std::uint32_t>(data.purpose));
        nested->set_clipboard_sequence(data.clipboard_sequence); nested->set_paste_submitted(data.paste_submitted);
        nested->set_clipboard_mode(data.clipboard_mode); nested->set_clipboard_source(data.clipboard_source);
        nested->set_snapshot_path(data.snapshot_path);
        nested->set_created_at_ms(data.created_at_ms);
        nested->set_active(data.active); nested->set_operation_revision(data.operation_revision);
    }
    encoded.set_capture_status(message.capture_status);
    encoded.set_capture_generation(message.capture_generation);
    encoded.set_capture_first_frame_id(message.capture_first_frame_id);
    encoded.set_capture_retry_requested(message.capture_retry_requested);
    encoded.set_reference_frame_id(message.reference_frame_id);
    encoded.set_reference_keyframe_id(message.reference_keyframe_id);
    encoded.set_stream_geometry_revision(message.stream_geometry_revision);
    encoded.set_rate_revision(message.rate_revision);
    encoded.set_observed_source_activity_revision(message.observed_source_activity_revision);
    encoded.set_latest_displayable_frame_id(message.latest_displayable_frame_id);
    encoded.set_latest_displayable_keyframe_id(message.latest_displayable_keyframe_id);
    encoded.set_latest_presented_frame_id(message.latest_presented_frame_id);
    encoded.set_rtt_ms(message.rtt_ms);
    encoded.set_transport_feedback_id(message.transport_feedback_id);
    for (const auto& item : message.media_transport_arrivals) {
        *encoded.add_media_transport_arrivals() = to_wire(item);
    }
    encoded.set_request_id(message.request_id);
    encoded.set_log_mode(static_cast<wire::RemoteLogModeV1>(message.log_mode));
    encoded.set_log_cursor(message.log_cursor);
    encoded.set_chunk_index(message.chunk_index);
    encoded.set_complete(message.complete);
    encoded.set_gap(message.gap);
    encoded.set_payload(message.payload);
    encoded.set_input_supported(message.input_supported);
    encoded.set_input_authorized(message.input_authorized);
    encoded.set_input_requested_active(message.input_requested_active);
    encoded.set_input_state(static_cast<wire::RemoteInputControlStateV1>(message.input_state));
    encoded.set_input_reason(static_cast<wire::RemoteInputStatusReasonV1>(message.input_reason));
    encoded.set_input_sequence(message.input_sequence);
    encoded.set_desktop_origin_x(message.desktop_origin_x);
    encoded.set_desktop_origin_y(message.desktop_origin_y);
    encoded.set_desktop_width(message.desktop_width);
    encoded.set_desktop_height(message.desktop_height);
    encoded.set_desktop_rotation(message.desktop_rotation);
    encoded.set_desktop_geometry_revision(message.desktop_geometry_revision);
    encoded.set_pressed_mouse_buttons(message.pressed_mouse_buttons);
    for (const auto& item : message.pressed_scan_codes) {
        encoded.add_pressed_scan_codes(item);
    }
    for (const auto& item : message.input_events) {
        *encoded.add_input_events() = to_wire(item);
    }
    for (const auto& item : message.desktop_displays) {
        *encoded.add_desktop_displays() = to_wire(item);
    }
    encoded.set_display_catalog_revision(message.display_catalog_revision);
    encoded.set_display_id(message.display_id);
    encoded.set_region_left(message.region_left);
    encoded.set_region_top(message.region_top);
    encoded.set_region_right(message.region_right);
    encoded.set_region_bottom(message.region_bottom);
    encoded.set_capture_region_revision(message.capture_region_revision);
    encoded.set_content_rect_x(message.content_rect_x);
    encoded.set_content_rect_y(message.content_rect_y);
    encoded.set_content_rect_width(message.content_rect_width);
    encoded.set_content_rect_height(message.content_rect_height);
    return encoded;
}

bool from_wire(const wire::StreamControlMessageV1& encoded, StreamControlMessageV1& message) {
    message.schema_version = encoded.schema_version();
    if (!wire::StreamControlMessageTypeV1_IsValid(static_cast<int>(encoded.type()))) return false;
    message.type = static_cast<StreamControlMessageTypeV1>(encoded.type());
    message.session_epoch = encoded.session_epoch();
    message.message_id = encoded.message_id();
    message.sent_at_ms = encoded.sent_at_ms();
    message.viewport_width = encoded.viewport_width();
    message.viewport_height = encoded.viewport_height();
    message.encoded_width = encoded.encoded_width();
    message.encoded_height = encoded.encoded_height();
    message.target_fps = encoded.target_fps();
    message.target_bitrate_kbps = encoded.target_bitrate_kbps();
    message.received_bytes = encoded.received_bytes();
    message.received_frames = encoded.received_frames();
    message.decoded_frames = encoded.decoded_frames();
    message.rendered_frames = encoded.rendered_frames();
    message.dropped_frames = encoded.dropped_frames();
    message.reassembly_timeouts = encoded.reassembly_timeouts();
    message.received_fragments = encoded.received_fragments();
    message.reassembled_frames = encoded.reassembled_frames();
    message.completed_keyframes = encoded.completed_keyframes();
    message.dropped_keyframes = encoded.dropped_keyframes();
    message.playback_starvation_count = encoded.playback_starvation_count();
    message.incomplete_frame_id = encoded.incomplete_frame_id();
    message.latest_received_frame_id = encoded.latest_received_frame_id();
    message.latest_complete_frame_id = encoded.latest_complete_frame_id();
    message.latest_complete_keyframe_id = encoded.latest_complete_keyframe_id();
    message.observed_rate_revision = encoded.observed_rate_revision();
    message.source_activity_revision = encoded.source_activity_revision();
    if (!wire::DesktopSourceActivityStateV1_IsValid(static_cast<int>(encoded.source_activity_state()))) return false;
    message.source_activity_state = static_cast<DesktopSourceActivityStateV1>(encoded.source_activity_state());
    message.media_budget_waiting = encoded.media_budget_waiting();
    message.capture_status_version = encoded.capture_status_version();
    message.terminal_version = encoded.terminal_version();
    message.file_transfer_version = encoded.file_transfer_version();
    message.clipboard_version = encoded.clipboard_version();
    message.connection_auth_version = encoded.connection_auth_version();
    message.auth_step = encoded.auth_step();
    message.auth_data = encoded.auth_data();
    if (encoded.has_workspace()) {
        const auto& nested = encoded.workspace();
        if (nested.action() > static_cast<std::uint32_t>(WorkspaceActionV1::kBrowseClipboardCopies)
            || nested.purpose() > static_cast<std::uint32_t>(WorkspaceTransferPurposeV1::kClipboardOpenCopy)
            || nested.direction() > static_cast<std::uint32_t>(TransferDirectionV1::kToController)
            || nested.conflict() > static_cast<std::uint32_t>(TransferConflictV1::kSkip)) return false;
        auto& data = message.workspace.emplace();
        data.schema_version = nested.schema_version(); data.action = static_cast<WorkspaceActionV1>(nested.action());
        data.direction = static_cast<TransferDirectionV1>(nested.direction()); data.path = nested.path();
        data.error_code = nested.error_code(); data.entries = nested.entries(); data.files = nested.files();
        data.bytes = nested.bytes(); data.completed_entries = nested.completed_entries();
        data.completed_bytes = nested.completed_bytes(); data.directory = nested.directory();
        data.conflict = static_cast<TransferConflictV1>(nested.conflict());
        data.accepted_sources = nested.accepted_sources();
        data.committed_bytes = nested.committed_bytes(); data.completed_files = nested.completed_files();
        data.results_path = nested.results_path();
        data.purpose = static_cast<WorkspaceTransferPurposeV1>(nested.purpose());
        data.clipboard_sequence = nested.clipboard_sequence(); data.paste_submitted = nested.paste_submitted();
        data.clipboard_mode = nested.clipboard_mode(); data.clipboard_source = nested.clipboard_source();
        data.snapshot_path = nested.snapshot_path();
        data.created_at_ms = nested.created_at_ms();
        data.skipped_entries = nested.skipped_entries();
        data.active = nested.active(); data.operation_revision = nested.operation_revision();
    }
    message.capture_status = encoded.capture_status();
    message.capture_generation = encoded.capture_generation();
    message.capture_first_frame_id = encoded.capture_first_frame_id();
    message.capture_retry_requested = encoded.capture_retry_requested();
    message.reference_frame_id = encoded.reference_frame_id();
    message.reference_keyframe_id = encoded.reference_keyframe_id();
    message.stream_geometry_revision = encoded.stream_geometry_revision();
    message.rate_revision = encoded.rate_revision();
    message.observed_source_activity_revision = encoded.observed_source_activity_revision();
    message.latest_displayable_frame_id = encoded.latest_displayable_frame_id();
    message.latest_displayable_keyframe_id = encoded.latest_displayable_keyframe_id();
    message.latest_presented_frame_id = encoded.latest_presented_frame_id();
    message.rtt_ms = encoded.rtt_ms();
    message.transport_feedback_id = encoded.transport_feedback_id();
    if (encoded.media_transport_arrivals_size() > static_cast<int>(kMaxMediaTransportArrivalsPerFeedback)) return false;
    message.media_transport_arrivals.reserve(static_cast<std::size_t>(encoded.media_transport_arrivals_size()));
    for (const auto& item : encoded.media_transport_arrivals()) {
        MediaTransportArrivalV1 decoded;
        if (!from_wire(item, decoded)) return false;
        message.media_transport_arrivals.push_back(std::move(decoded));
    }
    message.request_id = encoded.request_id();
    if (!wire::RemoteLogModeV1_IsValid(static_cast<int>(encoded.log_mode()))) return false;
    message.log_mode = static_cast<RemoteLogModeV1>(encoded.log_mode());
    message.log_cursor = encoded.log_cursor();
    message.chunk_index = encoded.chunk_index();
    message.complete = encoded.complete();
    message.gap = encoded.gap();
    message.payload = encoded.payload();
    message.input_supported = encoded.input_supported();
    message.input_authorized = encoded.input_authorized();
    message.input_requested_active = encoded.input_requested_active();
    if (!wire::RemoteInputControlStateV1_IsValid(static_cast<int>(encoded.input_state()))) return false;
    message.input_state = static_cast<RemoteInputControlStateV1>(encoded.input_state());
    if (!wire::RemoteInputStatusReasonV1_IsValid(static_cast<int>(encoded.input_reason()))) return false;
    message.input_reason = static_cast<RemoteInputStatusReasonV1>(encoded.input_reason());
    message.input_sequence = encoded.input_sequence();
    message.desktop_origin_x = encoded.desktop_origin_x();
    message.desktop_origin_y = encoded.desktop_origin_y();
    message.desktop_width = encoded.desktop_width();
    message.desktop_height = encoded.desktop_height();
    message.desktop_rotation = encoded.desktop_rotation();
    message.desktop_geometry_revision = encoded.desktop_geometry_revision();
    message.pressed_mouse_buttons = encoded.pressed_mouse_buttons();
    if (encoded.pressed_scan_codes_size() > static_cast<int>(kMaxRemoteInputPressedKeys)) return false;
    message.pressed_scan_codes.reserve(static_cast<std::size_t>(encoded.pressed_scan_codes_size()));
    for (const auto& item : encoded.pressed_scan_codes()) {
        if (item > std::numeric_limits<std::uint16_t>::max()) return false;
        message.pressed_scan_codes.push_back(static_cast<std::uint16_t>(item));
    }
    if (encoded.input_events_size() > static_cast<int>(kMaxRemoteInputEventsPerBatch)) return false;
    message.input_events.reserve(static_cast<std::size_t>(encoded.input_events_size()));
    for (const auto& item : encoded.input_events()) {
        RemoteInputEventV1 decoded;
        if (!from_wire(item, decoded)) return false;
        message.input_events.push_back(std::move(decoded));
    }
    if (encoded.desktop_displays_size() > static_cast<int>(kMaxDesktopDisplaysPerCatalog)) return false;
    message.desktop_displays.reserve(static_cast<std::size_t>(encoded.desktop_displays_size()));
    for (const auto& item : encoded.desktop_displays()) {
        DesktopDisplayV1 decoded;
        if (!from_wire(item, decoded)) return false;
        message.desktop_displays.push_back(std::move(decoded));
    }
    message.display_catalog_revision = encoded.display_catalog_revision();
    message.display_id = encoded.display_id();
    if (encoded.region_left() > std::numeric_limits<std::uint16_t>::max()) return false;
    message.region_left = static_cast<std::uint16_t>(encoded.region_left());
    if (encoded.region_top() > std::numeric_limits<std::uint16_t>::max()) return false;
    message.region_top = static_cast<std::uint16_t>(encoded.region_top());
    if (encoded.region_right() > std::numeric_limits<std::uint16_t>::max()) return false;
    message.region_right = static_cast<std::uint16_t>(encoded.region_right());
    if (encoded.region_bottom() > std::numeric_limits<std::uint16_t>::max()) return false;
    message.region_bottom = static_cast<std::uint16_t>(encoded.region_bottom());
    message.capture_region_revision = encoded.capture_region_revision();
    message.content_rect_x = encoded.content_rect_x();
    message.content_rect_y = encoded.content_rect_y();
    message.content_rect_width = encoded.content_rect_width();
    message.content_rect_height = encoded.content_rect_height();
    return true;
}
}  // namespace

std::size_t agent_message_protobuf_size_v1(const AgentMessageEnvelopeV1& message) {
    return to_wire(message).ByteSizeLong();
}

std::string serialize_agent_message_v1(const AgentMessageEnvelopeV1& message) {
    return compress_protobuf(to_wire(message), ProtobufWireKind::kAgent);
}

ParseResult<AgentMessageEnvelopeV1> parse_agent_message_v1(std::string_view serialized) {
    ParseResult<AgentMessageEnvelopeV1> result;
    wire::AgentMessageEnvelopeV1 encoded;
    if (!decompress_protobuf(serialized, ProtobufWireKind::kAgent, encoded, &result.error)) return result;
    if (!from_wire(encoded, result.value)) {
        result.error = "invalid Protobuf enum, integer range or repeated-field count";
        return result;
    }
    result.ok = validate_agent_message_v1(result.value, &result.error);
    return result;
}

std::size_t stream_control_message_protobuf_size_v1(const StreamControlMessageV1& message) {
    return to_wire(message).ByteSizeLong();
}

std::string serialize_stream_control_message_v1(const StreamControlMessageV1& message) {
    return compress_protobuf(to_wire(message), ProtobufWireKind::kControl);
}

ParseResult<StreamControlMessageV1> parse_stream_control_message_v1(std::string_view serialized) {
    ParseResult<StreamControlMessageV1> result;
    wire::StreamControlMessageV1 encoded;
    if (!decompress_protobuf(serialized, ProtobufWireKind::kControl, encoded, &result.error)) return result;
    if (encoded.has_workspace() && encoded.workspace().schema_version() != 1) {
        result.error = "protocol_version_incompatible"; return result;
    }
    if (encoded.type() == wire::StreamControlMessageTypeV1_kInputBatch
        && encoded.ByteSizeLong() > kMaxRemoteInputBatchBytes) {
        result.error = "remote input batch exceeds 4 KiB";
        return result;
    }
    if (!from_wire(encoded, result.value)) {
        result.error = "invalid Protobuf enum, integer range or repeated-field count";
        return result;
    }
    result.ok = validate_stream_control_message_v1(result.value, &result.error);
    return result;
}

}  // namespace redclaw::protocol
