#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <ostream>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>

#include "redclaw/net/net_module.h"
#include "redclaw/net/video_frame_transport.h"
#include "redclaw/protocol/protocol_module.h"

namespace redclaw::diag {

enum class LogLevel {
	kDebug,
	kInfo,
	kWarn,
	kError,
};

struct StructuredLogEvent {
	std::string component;
	std::string event;
	LogLevel level = LogLevel::kInfo;
	std::unordered_map<std::string, std::string> fields;
	std::uint64_t timestamp_unix = 0;
};

struct SupportBundleRequest {
	std::string bundle_id;
	std::string module = "diag";
	std::uint64_t generated_at_unix = 0;
	std::unordered_map<std::string, std::string> metadata;
	std::unordered_map<std::string, std::string> metrics;
	std::vector<StructuredLogEvent> structured_logs;
	std::vector<std::string> raw_log_lines;
};

struct ConnectivityDiagnosticsSnapshot {
	std::string session_id;
	std::string peer_id;
	net::IceConnectionState ice_state = net::IceConnectionState::kNew;
	protocol::SessionStateV1 session_state = protocol::SessionStateV1::idle;
	std::uint64_t collected_at_unix = 0;
	std::uint32_t local_candidate_count = 0;
	std::uint32_t remote_candidate_count = 0;
	std::uint32_t reconnect_attempt_count = 0;
	std::uint32_t recent_error_count = 0;
	std::string last_error;
	std::unordered_map<std::string, std::string> details;
};

struct ProcessFileLoggerConfig {
	std::filesystem::path directory;
	std::string file_stem = "redclaw-desktop";
	std::string role = "app";
	std::uint64_t max_file_bytes = 10ULL * 1024ULL * 1024ULL;
	std::size_t max_file_count = 5;
	std::uint32_t retention_days = 7;
	std::uint64_t max_directory_bytes = 250ULL * 1024ULL * 1024ULL;
	std::size_t memory_ring_max_lines = 4096;
	std::size_t memory_ring_max_bytes = 1024ULL * 1024ULL;
};

struct ProcessLogSnapshot {
	std::uint64_t first_cursor = 0;
	std::uint64_t next_cursor = 0;
	bool gap = false;
	std::vector<std::string> lines;
};

struct DebugRuntimeStatus {
    redclaw::net::TransportDiagnostics transport_diagnostics;
    redclaw::net::MediaPacerTelemetry media_pacer;
	std::string schema = "redclaw.debug.status.v1";
	std::string run_id;
	std::string role;
	std::string phase = "idle";
	std::string negotiation_phase = "idle";
	std::string ice_state = "new";
	std::string transport_path = "unknown";
	std::string stream_health = "idle";
	std::string network_bind_address = "auto";
	std::string ice_port_mapping_status = "disabled";
	std::string ice_mapped_external_ip;
	std::string local_description_tag;
	std::string remote_description_tag;
	std::string dht_publisher_instance_summary;
	std::string dht_last_host_instance_summary;
	std::string dht_last_adoption_reason = "none";
	std::string last_error;
	std::string process_log_path;
	std::string status_path;
	std::string evidence_manifest_path;
	std::uint64_t app_pid = 0;
	std::uint64_t runtime_pid = 0;
	std::uint64_t started_at_unix_ms = 0;
	std::uint64_t updated_at_unix_ms = 0;
	std::uint16_t ice_udp_port = 55000;
	std::uint16_t ice_mapped_internal_port = 0;
	std::uint16_t ice_mapped_external_port = 0;
	bool runtime_running = false;
	bool dht_reachable = false;
    bool dht_listener_available = false;
    bool dht_listener_ready = false;
    bool dht_listener_startup_failed = false;
    std::uint64_t dht_listener_probe_attempts = 0;
	bool answer_acknowledged = false;
	bool full_candidates_published = false;
	bool remote_description_applied = false;
	bool connected = false;
	bool channel_open = false;
	std::uint64_t dht_publish_success = 0;
	std::uint64_t dht_generation_publish_success = 0;
	std::uint64_t dht_publish_revision = 0;
	std::uint64_t dht_publish_expiry = 0;
	std::uint64_t dht_publish_expired_total = 0;
	std::uint64_t dht_publish_late_results = 0;
	std::uint64_t dht_fetch_hits = 0;
	std::uint64_t dht_generation = 0;
	std::uint64_t local_revision = 0;
	std::uint64_t remote_revision = 0;
	std::uint64_t persistent_offer_adopted_initial_total = 0;
	std::uint64_t persistent_offer_adopted_host_restart_total = 0;
	std::uint64_t persistent_offer_same_instance_rejected_total = 0;
	std::uint64_t peer_instance_missing_total = 0;
	std::uint64_t duplicate_offer_application_suppressed_total = 0;
	std::uint64_t local_dht_direct_published = 0;
	std::uint64_t remote_dht_latest_candidates = 0;
	std::uint64_t local_host = 0;
	std::uint64_t local_srflx = 0;
	std::uint64_t local_relay = 0;
	std::uint64_t remote_host = 0;
	std::uint64_t remote_srflx = 0;
	std::uint64_t remote_relay = 0;
	std::uint64_t captured = 0;
	std::uint64_t synthetic = 0;
	std::uint64_t capture_failures = 0;
	std::uint64_t encoded = 0;
	std::uint64_t encode_failures = 0;
	std::uint64_t transmitted = 0;
	std::uint64_t transmit_failures = 0;
	std::uint64_t received = 0;
	std::uint64_t media_fragments_received = 0;
	std::uint64_t encoded_frames_reassembled = 0;
	std::uint64_t direct_pipe_written = 0;
	std::uint64_t decoded = 0;
	std::uint64_t decode_failures = 0;
	std::uint64_t rendered = 0;
	std::uint64_t render_failures = 0;
	std::uint64_t gui_decode_success = 0;
	std::uint64_t gui_decode_failures = 0;
	std::uint64_t gui_presented = 0;
	std::uint64_t gui_present_failures = 0;
	std::uint64_t direct_pipe_writer_busy_drops = 0;
	std::uint64_t shared_snapshot_copies = 0;
	std::uint64_t shared_snapshot_failures = 0;
	std::uint64_t shared_snapshot_sequence_changes = 0;
	std::uint64_t shared_snapshot_max_us = 0;
	std::uint64_t shared_dependency_catchups = 0;
	std::uint64_t shared_independent_skips = 0;
	std::uint64_t shared_dependency_gaps = 0;
	std::uint64_t decoder_recovery_state = 0;
	std::uint64_t decoder_recovery_generation = 0;
	std::uint64_t decoder_recovery_breaks_total = 0;
	std::uint64_t decoder_recovery_requests_total = 0;
	std::uint64_t decoder_recovery_retries_total = 0;
	std::uint64_t decoder_recovery_suppressed_total = 0;
	std::uint64_t decoder_recovery_displayable_acks_total = 0;
	std::uint64_t decoder_recovery_invalidated_total = 0;
	std::uint64_t receiver_decode_fps = 0;
	std::uint64_t receiver_decode_pressure_windows = 0;
	std::uint64_t receiver_decode_stable_windows = 0;
	std::uint64_t receiver_decode_fps_decrease_total = 0;
	std::uint64_t receiver_decode_fps_increase_total = 0;
	std::uint64_t hardware_decode_runtime_fallbacks = 0;
	std::string decoder_backend;
	std::string last_decode_error;
	std::string last_hardware_decode_error;
	bool agent_authorized = false;
    bool agent_local_authorized = false;
    std::uint64_t agent_local_execution_state = 0;
    std::uint64_t agent_local_execution_queued = 0;
    std::string agent_local_execution_task;
    std::uint64_t agent_request_queue_depth = 0;
    std::uint64_t agent_result_queue_depth = 0;
    std::uint64_t agent_request_queue_peak = 0;
    std::uint64_t agent_result_queue_peak = 0;
    std::uint64_t agent_sent_requests = 0;
    std::uint64_t agent_sent_results = 0;
    std::uint64_t agent_received_commands = 0;
    std::uint64_t agent_received_results = 0;
    std::uint64_t agent_peer_rejected_total = 0;
    std::uint64_t agent_peer_stale_total = 0;
    std::uint64_t agent_send_pump_max_us = 0;
	bool agent_channel_open = false;
	bool agent_channel_unavailable = false;
	std::uint64_t agent_channel_open_total = 0;
	std::uint64_t agent_channel_close_total = 0;
	std::uint64_t agent_channel_rebuild_attempt_total = 0;
	std::uint64_t agent_channel_rebuild_success_total = 0;
	std::uint64_t agent_capability_refresh_total = 0;
	std::uint64_t agent_task_create_total = 0;
	std::uint64_t agent_task_complete_total = 0;
	std::uint64_t agent_task_failed_total = 0;
	std::uint64_t agent_duplicate_task_rejected_total = 0;
	std::uint64_t agent_event_total = 0;
	std::uint64_t agent_event_ack_total = 0;
	std::uint64_t agent_replayed_event_total = 0;
	std::uint64_t agent_gap_total = 0;
	std::uint64_t agent_approval_request_total = 0;
	std::uint64_t agent_approval_accept_total = 0;
	std::uint64_t agent_approval_reject_total = 0;
	std::uint64_t agent_approval_timeout_total = 0;
	std::uint64_t agent_queue_peak = 0;
	std::uint64_t agent_cached_event_bytes = 0;
	std::uint64_t agent_cached_event_bytes_peak = 0;
	std::uint64_t agent_outbound_queue_current = 0;
	std::uint64_t agent_outbound_queue_peak = 0;
};

class ProcessFileLogger {
public:
	ProcessFileLogger();
	~ProcessFileLogger();

	ProcessFileLogger(const ProcessFileLogger&) = delete;
	ProcessFileLogger& operator=(const ProcessFileLogger&) = delete;

	[[nodiscard]] bool start(const ProcessFileLoggerConfig& config, std::string* error_detail = nullptr);
	void stop();
	void write_line(std::string_view source, std::string_view line, bool flush_immediately = false);
	void flush();

	[[nodiscard]] bool is_open() const;
	[[nodiscard]] bool is_healthy() const;
	[[nodiscard]] std::filesystem::path log_path() const;
	[[nodiscard]] ProcessLogSnapshot snapshot(
		std::uint64_t after_cursor,
		std::size_t max_lines,
		std::size_t max_bytes) const;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

class ScopedProcessStreamCapture {
public:
	ScopedProcessStreamCapture();
	~ScopedProcessStreamCapture();

	ScopedProcessStreamCapture(const ScopedProcessStreamCapture&) = delete;
	ScopedProcessStreamCapture& operator=(const ScopedProcessStreamCapture&) = delete;

	[[nodiscard]] bool start(ProcessFileLogger* logger, std::string* error_detail = nullptr, bool diagnostics_to_stderr = false);
	void stop();

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool is_sensitive_field_name(std::string_view field_name);
[[nodiscard]] std::string redact_field_value(std::string_view field_name, std::string_view value);
[[nodiscard]] std::unordered_map<std::string, std::string> redact_fields(
	const std::unordered_map<std::string, std::string>& fields);
[[nodiscard]] std::string format_structured_log_line(const StructuredLogEvent& event);
[[nodiscard]] std::string export_support_bundle(const SupportBundleRequest& request);
[[nodiscard]] std::string export_connectivity_diagnostics_snapshot(const ConnectivityDiagnosticsSnapshot& snapshot);
[[nodiscard]] std::string redact_log_text(std::string_view line);
[[nodiscard]] bool remote_diagnostics_allowed_by_policy(
	bool release_build,
	bool local_opt_in) noexcept;
[[nodiscard]] bool update_debug_runtime_status_from_line(
	std::string_view line,
	DebugRuntimeStatus* status);

std::string_view module_name();
}
