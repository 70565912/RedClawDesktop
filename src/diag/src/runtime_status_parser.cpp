#include "redclaw/diag/diag_module.h"
#include "redclaw/net/ice_candidate_diagnostics.h"
#include <algorithm>
#include <charconv>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace redclaw::diag {
namespace {
std::unordered_map<std::string, std::string> parse_runtime_fields(std::string_view line) {
    std::unordered_map<std::string, std::string> fields;
    std::istringstream input{std::string(line)};
    std::string token;
    while (input >> token) {
        const auto split = token.find('=');
        if (split == std::string::npos || split == 0 || split + 1 >= token.size()) {
            continue;
        }
        fields[token.substr(0, split)] = token.substr(split + 1);
    }
    return fields;
}

bool field_bool(const std::unordered_map<std::string, std::string>& fields, std::string_view key, bool fallback) {
    const auto found = fields.find(std::string(key));
    if (found == fields.end()) {
        return fallback;
    }
    return found->second == "true" || found->second == "1" || found->second == "yes";
}

std::uint64_t field_u64(
    const std::unordered_map<std::string, std::string>& fields,
    std::string_view key,
    std::uint64_t fallback) {
    const auto found = fields.find(std::string(key));
    if (found == fields.end()) {
        return fallback;
    }
    std::uint64_t value = 0;
    const char* begin = found->second.data();
    const char* end = begin + found->second.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end ? value : fallback;
}

std::string field_string(
    const std::unordered_map<std::string, std::string>& fields,
    std::string_view key,
    std::string fallback = {}) {
    const auto found = fields.find(std::string(key));
    return found == fields.end() ? std::move(fallback) : found->second;
}

std::string ice_state_from_number(std::string_view value) {
    if (value == "0") return "new";
    if (value == "1") return "gathering";
    if (value == "2") return "connecting";
    if (value == "3") return "connected";
    if (value == "4") return "disconnected";
    if (value == "5") return "failed";
    if (value == "6") return "closed";
    return "unknown";
}
}  // namespace

bool update_debug_runtime_status_from_line(std::string_view line, DebugRuntimeStatus* status) {
    if (status == nullptr || line.empty()) {
        return false;
    }
    const auto fields = parse_runtime_fields(line);
    bool changed = false;
    const auto update_role = [&]() {
        const std::string role = field_string(fields, "role");
        if (!role.empty() && role != status->role) {
            status->role = role;
            changed = true;
        }
    };

    if (line.rfind("Runtime transport diagnostic ", 0) == 0) {
        const auto sequence = field_u64(fields, "sequence", 0);
        const auto layer = field_u64(fields, "layer", 99);
        const auto kind = field_u64(fields, "channel", 99);
        auto& diagnostics = status->transport_diagnostics;
        if (sequence == 0 || sequence <= diagnostics.event_sequence || layer > 5) return false;
        if (layer == 5 && !redclaw::net::is_valid_candidate_diagnostic_reason(field_string(fields, "reason"))) return false;
        if (layer == 4 && field_u64(fields, "native_state", 99) >= redclaw::net::kIceCheckKindCount) return false;
        redclaw::net::TransportDiagnosticEvent event;
        event.sequence = sequence;
        event.steady_ms = field_u64(fields, "steady_ms", 0);
        event.peer_generation = field_u64(fields, "peer_generation", 0);
        event.channel_generation = field_u64(fields, "channel_generation", 0);
        event.layer = static_cast<redclaw::net::TransportDiagnosticLayer>(layer);
        if (kind < 5) event.channel = static_cast<redclaw::net::DataChannelKind>(kind);
        event.native_state = static_cast<int>(std::min<std::uint64_t>(field_u64(fields, "native_state", 0), 255));
        event.failure = field_bool(fields, "failure", false);
        const auto pair_text = field_string(fields, "pair_id");
        event.pair_id = pair_text == "-1" || pair_text.empty() ? -1
            : static_cast<int>(std::min<std::uint64_t>(field_u64(fields, "pair_id", 0), 4096));
        event.local_candidate_type = static_cast<int>(std::min<std::uint64_t>(field_u64(fields, "local_type", 0), 4));
        event.remote_candidate_type = static_cast<int>(std::min<std::uint64_t>(field_u64(fields, "remote_type", 0), 4));
        event.occurrences = field_u64(fields, "occurrences", 0);
        event.reason = redact_log_text(field_string(fields, "reason").substr(0, 128));
        if (event.peer_generation < diagnostics.ice_check_generation) return false;
        if (!diagnostics.recent_events.empty()
            && diagnostics.recent_events.back().peer_generation != event.peer_generation) {
            diagnostics.first_failure.reset();
            diagnostics.candidate_events.clear();
        }
        if (layer == 5 && diagnostics.candidate_events.size() < 64) diagnostics.candidate_events.push_back(event);
        if (event.failure && !diagnostics.first_failure) diagnostics.first_failure = event;
        if (diagnostics.recent_events.size() == 64) diagnostics.recent_events.erase(diagnostics.recent_events.begin());
        diagnostics.recent_events.push_back(std::move(event));
        diagnostics.event_sequence = sequence;
        return true;
    } else if (line.rfind("Runtime ICE check stats ", 0) == 0) {
        auto& diagnostics = status->transport_diagnostics;
        const auto generation = field_u64(fields, "peer_generation", 0);
        if (generation < diagnostics.ice_check_generation) return false;
        if (generation != diagnostics.ice_check_generation) {
            diagnostics.ice_check_totals.fill(0);
            if (!diagnostics.candidate_events.empty() && diagnostics.candidate_events.front().peer_generation < generation)
                diagnostics.candidate_events.clear();
        }
        diagnostics.ice_check_generation = generation;
        for (std::size_t index = 0; index < redclaw::net::kIceCheckKindCount; ++index) {
            const auto key = redclaw::net::ice_check_kind_name(static_cast<redclaw::net::IceCheckKind>(index));
            diagnostics.ice_check_totals[index] = (std::max)(diagnostics.ice_check_totals[index], field_u64(fields, key, 0));
        }
        return true;
    } else if (line.rfind("Runtime media transport feedback stats ", 0) == 0) {
        status->media_pacer.recovery_generation = field_u64(fields, "pacer_recovery_generation", status->media_pacer.recovery_generation);
        status->media_pacer.dependency_pending_drops = field_u64(fields, "pacer_dependency_pending_drops", status->media_pacer.dependency_pending_drops);
        status->media_pacer.suppressed_keyframe_requests = field_u64(fields, "pacer_suppressed_keyframe_requests", status->media_pacer.suppressed_keyframe_requests);
        status->media_pacer.budget_revision = field_u64(fields, "pacer_budget_revision", status->media_pacer.budget_revision);
        status->media_pacer.budget_retry_attempt = field_u64(fields, "pacer_budget_retry_attempt", status->media_pacer.budget_retry_attempt);
        status->media_pacer.next_admission_ms = field_u64(fields, "pacer_next_admission_ms", status->media_pacer.next_admission_ms);
        status->media_pacer.probe_wire_bytes = field_u64(fields, "pacer_probe_wire_bytes", status->media_pacer.probe_wire_bytes);
        status->media_pacer.frame_budget_rejections = field_u64(fields, "pacer_frame_budget_rejections", status->media_pacer.frame_budget_rejections);
        status->media_pacer.deadline_drops = field_u64(fields, "pacer_deadline_drops", status->media_pacer.deadline_drops);
        status->media_pacer.active_depth = field_u64(fields, "pacer_active_depth", status->media_pacer.active_depth);
        status->media_pacer.pending_depth = field_u64(fields, "pacer_pending_depth", status->media_pacer.pending_depth);
        return true;
    } else if (line.rfind("Runtime ICE config ", 0) == 0) {
        const std::string bind_address = field_string(fields, "network_bind_address");
        if (!bind_address.empty() && bind_address != status->network_bind_address) {
            status->network_bind_address = bind_address;
            changed = true;
        }
        const auto ice_udp_port = field_u64(fields, "ice_udp_port", status->ice_udp_port);
        if (ice_udp_port >= 1 && ice_udp_port <= 65535
            && ice_udp_port != status->ice_udp_port) {
            status->ice_udp_port = static_cast<std::uint16_t>(ice_udp_port);
            changed = true;
        }
    } else if (line.rfind("Runtime ICE port mapping role=", 0) == 0) {
        update_role();
        status->ice_port_mapping_status = field_string(fields, "status", "disabled");
        status->ice_mapped_internal_port = static_cast<std::uint16_t>(
            (std::min<std::uint64_t>)(field_u64(fields, "internal_port", 0), 65535));
        status->ice_mapped_external_port = static_cast<std::uint16_t>(
            (std::min<std::uint64_t>)(field_u64(fields, "external_port", 0), 65535));
        status->ice_mapped_external_ip = redact_log_text(
            field_string(fields, "external_ip").substr(0, 64));
        changed = true;
    } else if (line.rfind("Runtime state role=", 0) == 0) {
        update_role();
        const std::string state = field_string(fields, "state");
        const std::string ice_state = ice_state_from_number(state);
        if (ice_state != status->ice_state) {
            status->ice_state = ice_state;
            changed = true;
        }
        status->remote_description_applied = field_bool(
            fields, "remote_description_applied", status->remote_description_applied);
        status->connected = field_bool(fields, "connected", status->connected);
        const bool preserve_media_state = status->connected
            && ((status->role == "controller" && status->phase == "streaming")
                || (status->phase == "failed"
                    && status->stream_health.find("_startup_stalled")
                        != std::string::npos));
        if (!preserve_media_state) {
            status->phase = status->connected
                ? "connected"
                : (status->remote_description_applied ? "ice_connecting" : "dht_waiting");
        }
        changed = true;
    } else if (line.rfind("Runtime DHT backend diagnostics role=", 0) == 0) {
        status->dht_listener_available = fields.contains("listen_ready");
        status->dht_listener_ready = field_bool(fields, "listen_ready", false);
        status->dht_listener_startup_failed = field_bool(fields, "listen_startup_failed", false);
        status->dht_listener_probe_attempts = (std::min<std::uint64_t>)(
            field_u64(fields, "listen_port_probe_attempts", 0), 16);
        changed = true;
    } else if (line.rfind("Runtime DHT stats role=", 0) == 0) {
        update_role();
        status->dht_reachable = field_bool(fields, "reachable", status->dht_reachable);
        status->dht_publish_success = field_u64(fields, "publish_success", status->dht_publish_success);
        status->dht_generation_publish_success = field_u64(
            fields,
            "generation_publish_success",
            status->dht_generation_publish_success);
        status->dht_fetch_hits = field_u64(fields, "fetch_hits", status->dht_fetch_hits);
        status->dht_publish_revision = field_u64(fields, "publish_revision", status->dht_publish_revision);
        status->dht_publish_expiry = field_u64(fields, "publish_expiry", status->dht_publish_expiry);
        status->dht_publish_expired_total = field_u64(fields, "publish_expired_total", status->dht_publish_expired_total);
        status->dht_publish_late_results = field_u64(fields, "publish_late_results", status->dht_publish_late_results);
        status->dht_generation = field_u64(fields, "generation", status->dht_generation);
        status->answer_acknowledged = field_bool(
            fields,
            "answer_acknowledged",
            status->answer_acknowledged);
        const std::string negotiation_phase = field_string(fields, "negotiation_phase");
        if (!negotiation_phase.empty()) {
            status->negotiation_phase = negotiation_phase;
        }
        status->local_revision = field_u64(fields, "local_revision", status->local_revision);
        status->remote_revision = field_u64(fields, "remote_revision", status->remote_revision);
        status->dht_publisher_instance_summary = field_string(fields, "publisher_instance");
        status->dht_last_host_instance_summary = field_string(fields, "last_host_instance");
        status->persistent_offer_adopted_initial_total = field_u64(
            fields,
            "persistent_offer_adopted_initial_total",
            status->persistent_offer_adopted_initial_total);
        status->persistent_offer_adopted_host_restart_total = field_u64(
            fields,
            "persistent_offer_adopted_host_restart_total",
            status->persistent_offer_adopted_host_restart_total);
        status->persistent_offer_same_instance_rejected_total = field_u64(
            fields,
            "persistent_offer_same_instance_rejected_total",
            status->persistent_offer_same_instance_rejected_total);
        status->peer_instance_missing_total = field_u64(
            fields,
            "peer_instance_missing_total",
            status->peer_instance_missing_total);
        status->duplicate_offer_application_suppressed_total = field_u64(
            fields,
            "duplicate_offer_application_suppressed_total",
            status->duplicate_offer_application_suppressed_total);
        if (!status->connected && !status->remote_description_applied) {
            status->phase = "dht_waiting";
        }
        changed = true;
    } else if (line.rfind("Runtime candidate stats role=", 0) == 0) {
        update_role();
        status->local_host = field_u64(fields, "local_host", status->local_host);
        status->local_srflx = field_u64(fields, "local_srflx", status->local_srflx);
        status->local_relay = field_u64(fields, "local_relay", status->local_relay);
        status->remote_host = field_u64(fields, "remote_host", status->remote_host);
        status->remote_srflx = field_u64(fields, "remote_srflx", status->remote_srflx);
        status->remote_relay = field_u64(fields, "remote_relay", status->remote_relay);
        status->local_dht_direct_published = field_u64(
            fields, "local_dht_direct_published", status->local_dht_direct_published);
        status->remote_dht_latest_candidates = field_u64(
            fields, "remote_dht_latest_candidates", status->remote_dht_latest_candidates);
        status->full_candidates_published = field_bool(
            fields, "local_dht_full_published", status->full_candidates_published);
        changed = true;
    } else if (line.rfind("Runtime DHT signal ", 0) == 0) {
        update_role();
        const std::uint64_t generation = field_u64(fields, "generation", status->dht_generation);
        if (generation != status->dht_generation) {
            status->dht_generation = generation;
            changed = true;
        }
        const std::string acknowledged_answer_tag = field_string(fields, "acknowledged_answer_tag");
        if (!acknowledged_answer_tag.empty()
            && acknowledged_answer_tag != "none"
            && !status->answer_acknowledged) {
            status->answer_acknowledged = true;
            changed = true;
        }
        const std::string tag = field_string(fields, "description_tag");
        if (!tag.empty() && tag != status->local_description_tag) {
            status->local_description_tag = tag;
            changed = true;
        }
    } else if (line.rfind("Runtime Controller adopted persistent Host offer role=", 0) == 0) {
        update_role();
        const std::string adoption_reason = field_string(fields, "adoption_reason");
        if (!adoption_reason.empty()) {
            status->dht_last_adoption_reason = adoption_reason;
        }
        const std::string host_instance = field_string(fields, "publisher_instance");
        if (!host_instance.empty()) {
            status->dht_last_host_instance_summary = host_instance;
        }
        changed = true;
    } else if (line.rfind("Runtime remote description applied role=", 0) == 0) {
        update_role();
        status->dht_generation = field_u64(fields, "generation", status->dht_generation);
        status->remote_description_applied = true;
        status->phase = "remote_description";
        const std::string tag = field_string(fields, "remote_description_tag");
        if (!tag.empty()) {
            status->remote_description_tag = tag;
        }
        status->remote_revision = field_u64(fields, "remote_revision", status->remote_revision);
        changed = true;
    } else if (line.rfind("Runtime Host DHT answer acknowledgement accepted role=", 0) == 0) {
        update_role();
        status->dht_generation = field_u64(fields, "generation", status->dht_generation);
        status->answer_acknowledged = true;
        status->negotiation_phase = "answer_applied";
        changed = true;
    } else if (line.rfind("Runtime desktop stream rates role=", 0) == 0) {
        update_role();
        const std::string stream_health = field_string(fields, "health");
        if (!stream_health.empty()) {
            status->stream_health = stream_health;
        }
        const bool startup_stalled = status->stream_health == "capture_startup_stalled"
            || status->stream_health == "encoder_startup_stalled"
            || status->stream_health == "transmit_startup_stalled"
            || status->stream_health == "receive_startup_stalled"
            || status->stream_health == "playback_startup_stalled";
        if (startup_stalled) {
            status->phase = "failed";
            status->last_error = "desktop stream startup stalled: " + status->stream_health;
        }
        changed = true;
    } else if (line.rfind("Runtime desktop stream stats role=", 0) == 0) {
        update_role();
        status->channel_open = field_bool(fields, "channel_open", status->channel_open);
        status->captured = field_u64(fields, "captured", status->captured);
        status->synthetic = field_u64(fields, "synthetic", status->synthetic);
        status->capture_failures = field_u64(fields, "capture_failures", status->capture_failures);
        status->encoded = field_u64(fields, "encoded", status->encoded);
        status->encode_failures = field_u64(fields, "encode_failures", status->encode_failures);
        status->transmitted = field_u64(fields, "transmitted", status->transmitted);
        status->transmit_failures = field_u64(fields, "transmit_failures", status->transmit_failures);
        status->received = field_u64(fields, "received", status->received);
        status->media_fragments_received = field_u64(
            fields, "media_fragments_received", status->media_fragments_received);
        status->encoded_frames_reassembled = field_u64(
            fields, "encoded_frames_reassembled", status->encoded_frames_reassembled);
        status->direct_pipe_written = field_u64(
            fields, "direct_pipe_written", status->direct_pipe_written);
        status->decoded = field_u64(fields, "decoded", status->decoded);
        status->decode_failures = field_u64(fields, "decode_failures", status->decode_failures);
        status->rendered = field_u64(fields, "rendered", status->rendered);
        status->render_failures = field_u64(fields, "render_failures", status->render_failures);
        const bool host_streaming = status->role == "host"
            && status->connected && status->channel_open
            && status->captured > 0 && status->encoded > 0 && status->transmitted > 0;
        if (host_streaming) {
            status->phase = "streaming";
            status->last_error.clear();
        } else if (status->role != "controller" || status->phase != "streaming") {
            status->phase = status->channel_open ? "channel_open" : status->phase;
        }
        changed = true;
    } else if (line.rfind("Runtime stream adaptation stats role=", 0) == 0) {
        update_role();
        status->receiver_decode_fps = field_u64(
            fields, "receiver_decode_fps", status->receiver_decode_fps);
        status->receiver_decode_pressure_windows = field_u64(
            fields,
            "receiver_decode_pressure_windows",
            status->receiver_decode_pressure_windows);
        status->receiver_decode_stable_windows = field_u64(
            fields,
            "receiver_decode_stable_windows",
            status->receiver_decode_stable_windows);
        status->receiver_decode_fps_decrease_total = field_u64(
            fields,
            "receiver_decode_fps_decrease_total",
            status->receiver_decode_fps_decrease_total);
        status->receiver_decode_fps_increase_total = field_u64(
            fields,
            "receiver_decode_fps_increase_total",
            status->receiver_decode_fps_increase_total);
        changed = true;
    } else if (line.rfind("Runtime controller stream consumption stats role=", 0) == 0) {
        update_role();
        status->decoder_recovery_state = field_u64(
            fields, "decoder_recovery_state", status->decoder_recovery_state);
        status->decoder_recovery_generation = field_u64(
            fields, "decoder_recovery_generation", status->decoder_recovery_generation);
        status->decoder_recovery_breaks_total = field_u64(
            fields, "decoder_recovery_breaks_total", status->decoder_recovery_breaks_total);
        status->decoder_recovery_requests_total = field_u64(
            fields, "decoder_recovery_requests_total", status->decoder_recovery_requests_total);
        status->decoder_recovery_retries_total = field_u64(
            fields, "decoder_recovery_retries_total", status->decoder_recovery_retries_total);
        status->decoder_recovery_suppressed_total = field_u64(
            fields, "decoder_recovery_suppressed_total", status->decoder_recovery_suppressed_total);
        status->decoder_recovery_displayable_acks_total = field_u64(
            fields,
            "decoder_recovery_displayable_acks_total",
            status->decoder_recovery_displayable_acks_total);
        status->decoder_recovery_invalidated_total = field_u64(
            fields,
            "decoder_recovery_invalidated_total",
            status->decoder_recovery_invalidated_total);
        changed = true;
    } else if (line.rfind("Runtime remote agent stats role=", 0) == 0) {
        update_role();
        status->agent_local_authorized = field_bool(fields, "local_authorized", status->agent_local_authorized);
        status->agent_local_execution_state = field_u64(fields, "local_execution_state", status->agent_local_execution_state);
        status->agent_local_execution_queued = field_u64(fields, "local_execution_queued", status->agent_local_execution_queued);
        if (const auto task = fields.find("local_execution_task"); task != fields.end())
            status->agent_local_execution_task = task->second;
        status->agent_request_queue_depth = field_u64(fields, "request_queue_depth", status->agent_request_queue_depth);
        status->agent_result_queue_depth = field_u64(fields, "result_queue_depth", status->agent_result_queue_depth);
        status->agent_request_queue_peak = field_u64(fields, "request_queue_peak", status->agent_request_queue_peak);
        status->agent_result_queue_peak = field_u64(fields, "result_queue_peak", status->agent_result_queue_peak);
        status->agent_sent_requests = field_u64(fields, "sent_requests", status->agent_sent_requests);
        status->agent_sent_results = field_u64(fields, "sent_results", status->agent_sent_results);
        status->agent_received_commands = field_u64(fields, "received_commands", status->agent_received_commands);
        status->agent_received_results = field_u64(fields, "received_results", status->agent_received_results);
        status->agent_peer_rejected_total = field_u64(fields, "peer_rejected_total", status->agent_peer_rejected_total);
        status->agent_peer_stale_total = field_u64(fields, "peer_stale_total", status->agent_peer_stale_total);
        status->agent_send_pump_max_us = field_u64(fields, "send_pump_max_us", status->agent_send_pump_max_us);
        status->agent_authorized = field_bool(
            fields, "authorized", status->agent_authorized);
        status->agent_channel_open = field_bool(
            fields, "channel_open", status->agent_channel_open);
        status->agent_channel_unavailable = field_bool(
            fields, "channel_unavailable", status->agent_channel_unavailable);
        status->agent_channel_open_total = field_u64(
            fields, "channel_open_total", status->agent_channel_open_total);
        status->agent_channel_close_total = field_u64(
            fields, "channel_close_total", status->agent_channel_close_total);
        status->agent_channel_rebuild_attempt_total = field_u64(
            fields, "channel_rebuild_attempt_total",
            status->agent_channel_rebuild_attempt_total);
        status->agent_channel_rebuild_success_total = field_u64(
            fields, "channel_rebuild_success_total",
            status->agent_channel_rebuild_success_total);
        status->agent_capability_refresh_total = field_u64(
            fields, "capability_refresh_total", status->agent_capability_refresh_total);
        status->agent_task_create_total = field_u64(
            fields, "task_create_total", status->agent_task_create_total);
        status->agent_task_complete_total = field_u64(
            fields, "task_complete_total", status->agent_task_complete_total);
        status->agent_task_failed_total = field_u64(
            fields, "task_failed_total", status->agent_task_failed_total);
        status->agent_duplicate_task_rejected_total = field_u64(
            fields, "duplicate_task_rejected_total",
            status->agent_duplicate_task_rejected_total);
        status->agent_event_total = field_u64(
            fields, "event_total", status->agent_event_total);
        status->agent_event_ack_total = field_u64(
            fields, "event_ack_total", status->agent_event_ack_total);
        status->agent_replayed_event_total = field_u64(
            fields, "replayed_event_total", status->agent_replayed_event_total);
        status->agent_gap_total = field_u64(fields, "gap_total", status->agent_gap_total);
        status->agent_approval_request_total = field_u64(
            fields, "approval_request_total", status->agent_approval_request_total);
        status->agent_approval_accept_total = field_u64(
            fields, "approval_accept_total", status->agent_approval_accept_total);
        status->agent_approval_reject_total = field_u64(
            fields, "approval_reject_total", status->agent_approval_reject_total);
        status->agent_approval_timeout_total = field_u64(
            fields, "approval_timeout_total", status->agent_approval_timeout_total);
        status->agent_queue_peak = field_u64(
            fields, "queue_peak", status->agent_queue_peak);
        status->agent_cached_event_bytes = field_u64(
            fields, "cached_event_bytes", status->agent_cached_event_bytes);
        status->agent_cached_event_bytes_peak = field_u64(
            fields, "cached_event_bytes_peak", status->agent_cached_event_bytes_peak);
        status->agent_outbound_queue_current = field_u64(
            fields, "outbound_queue_current", status->agent_outbound_queue_current);
        status->agent_outbound_queue_peak = field_u64(
            fields, "outbound_queue_peak", status->agent_outbound_queue_peak);
        changed = true;
    } else if (line.find("desktop stream data channel opened") != std::string_view::npos) {
        status->channel_open = true;
        if (status->phase != "streaming") {
            status->phase = "channel_open";
        }
        changed = true;
    } else if (line.rfind("Runtime rebuilding signaling session role=", 0) == 0) {
        update_role();
        status->phase = "dht_waiting";
        status->stream_health = "idle";
        status->remote_description_applied = false;
        status->connected = false;
        status->channel_open = false;
        status->ice_state = "new";
        status->answer_acknowledged = false;
        status->negotiation_phase = "offer_ready";
        status->last_error.clear();
        changed = true;
    } else if (line.find("Runtime ICE state failed before DHT signaling completed; continuing")
            != std::string_view::npos
        || line.find("Runtime ICE state failed; DHT retry remains active")
            != std::string_view::npos) {
        status->phase = "dht_waiting";
        status->connected = false;
        status->channel_open = false;
        status->ice_state = "failed";
        status->last_error = redact_log_text(line);
        changed = true;
    } else if (line.find("Runtime signaling failed") != std::string_view::npos
        || line.find("Runtime ICE state failed") != std::string_view::npos
        || line.find("Runtime signaling timeout") != std::string_view::npos) {
        status->phase = "failed";
        status->last_error = redact_log_text(line);
        changed = true;
    }
    return changed;
}

std::string_view module_name() {
    return "diag";
}

}  // namespace redclaw::diag
