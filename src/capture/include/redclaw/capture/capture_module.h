#pragma once

#include <cstdint>
#include "redclaw/capture/capture_recovery.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace redclaw::capture {

enum class CaptureBackendType {
	kUnknown,
	kDesktopDuplication,
	kWindowsGraphicsCapture,
	kGdiBitBlt,
};

enum class EncoderCodec {
	kH264,
	kHevc,
};

enum class EncoderRateControl {
	kCbr,
	kVbr,
};

enum class EncoderBackendType {
	kAuto,
	kNvenc,
	kQuickSync,
	kAmf,
	kSoftware,
};

enum class CaptureAdapterVendor {
	kUnknown,
	kIntel,
	kNvidia,
	kAmd,
};

enum class EncoderWorkload {
	kInteractiveDesktop,
	kFastAction,
};

struct EncoderProfileRequest {
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t fps = 0;
	EncoderWorkload workload = EncoderWorkload::kInteractiveDesktop;
	EncoderCodec preferred_codec = EncoderCodec::kH264;
};

struct EncoderConfigProfile {
	EncoderCodec codec = EncoderCodec::kH264;
	EncoderWorkload workload = EncoderWorkload::kInteractiveDesktop;
	EncoderRateControl rate_control = EncoderRateControl::kCbr;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t fps = 0;
	std::uint32_t target_bitrate_kbps = 0;
	std::uint32_t max_bitrate_kbps = 0;
	std::uint32_t gop_length_frames = 0;
	std::uint32_t b_frames = 0;
	bool lookahead_enabled = false;
	bool repeat_headers = true;
	bool zero_latency_tuning = true;
};

struct EncoderBackendBridgeRequest {
	EncoderCodec codec = EncoderCodec::kH264;
	EncoderBackendType preferred_backend = EncoderBackendType::kAuto;
	EncoderBackendType excluded_hardware_backend = EncoderBackendType::kAuto;
	bool allow_hardware_fallback = true;
	bool allow_hardware_frame_input = true;
	CaptureAdapterVendor capture_adapter_vendor = CaptureAdapterVendor::kUnknown;
};

struct EncoderBackendCapabilities {
	bool ffmpeg_runtime_available = false;
	bool nvenc_available = false;
	bool quicksync_available = false;
	bool amf_available = false;
	bool software_available = true;
};

struct EncoderBackendBridgePlan {
	EncoderBackendType selected_backend = EncoderBackendType::kSoftware;
	bool fallback_applied = false;
	bool allow_hardware_frame_input = true;
	std::string reason;
};

enum class EncoderExecutionFailureCategory {
	kNone,
	kLibavcodecUnavailable,
	kInvalidConfig,
	kEncoderNotFound,
	kEncoderInitFailed,
	kFrameFormatUnsupported,
	kFrameSizeMismatch,
	kOutputNotReady,
	kEncodeFailed,
	kOutputResourceLimit,
};

enum class EncoderRateControlUpdateStatus {
	kApplied,
	kUnsupported,
	kFailed,
};

struct CapturedFrameNativeHandle;
struct CapturedFrame;

struct CaptureRegion {
	std::uint32_t x = 0;
	std::uint32_t y = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint64_t revision = 1;
};

struct EncoderContentRect {
	std::uint32_t x = 0;
	std::uint32_t y = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
};

struct CaptureDisplayDescriptor {
	std::string id;
	std::string name;
	std::string device_path;
	std::uint32_t adapter_index = 0;
	std::uint32_t output_index = 0;
	std::int64_t adapter_luid = 0;
	std::int32_t desktop_origin_x = 0;
	std::int32_t desktop_origin_y = 0;
	std::uint32_t pixel_width = 0;
	std::uint32_t pixel_height = 0;
	std::uint32_t rotation = 0;
	bool primary = false;
};

[[nodiscard]] std::vector<CaptureDisplayDescriptor> enumerate_capture_displays(
	std::string* error_detail = nullptr);
[[nodiscard]] CaptureRegion normalize_capture_region(
	CaptureRegion requested,
	std::uint32_t source_width,
	std::uint32_t source_height);
[[nodiscard]] CaptureRegion capture_region_from_normalized_bounds(
	std::uint16_t left,
	std::uint16_t top,
	std::uint16_t right,
	std::uint16_t bottom,
	std::uint32_t source_width,
	std::uint32_t source_height,
	std::uint64_t revision);
[[nodiscard]] EncoderContentRect resolve_capture_region_content_rect(
	const CaptureRegion& region,
	std::uint32_t canvas_width,
	std::uint32_t canvas_height);

// Preserve selection intent across catalog/frame size differences and recovery.
// Pixel bounds are derived from the actual source, never cached from a catalog.
struct CaptureRegionSelection {
    std::uint16_t left = 0;
    std::uint16_t top = 0;
    std::uint16_t right = 65535;
    std::uint16_t bottom = 65535;
    std::uint64_t revision = 1;

    [[nodiscard]] CaptureRegion resolve(std::uint32_t width, std::uint32_t height) const {
        return capture_region_from_normalized_bounds(
            left, top, right, bottom, width, height, revision);
    }
};

bool encode_navigation_thumbnail_jpeg(
	const CapturedFrame& frame,
	std::uint32_t max_edge,
	std::vector<std::uint8_t>* jpeg,
	std::uint32_t* output_width,
	std::uint32_t* output_height,
	std::string* error_detail = nullptr);

enum class CapturedFrameNativeHandleType {
	kNone,
	kD3D11Texture2D,
};

struct EncodedFramePacket {
	EncoderCodec codec = EncoderCodec::kH264;
	bool keyframe = false;
	std::uint64_t timestamp_ms = 0;
	std::vector<std::uint8_t> payload;
	// Local sender reservation, zero for callers without a bounded output pool.
	std::size_t payload_limit_bytes = 0;
};

struct EncoderExecutionDiagnostics {
	bool initialized = false;
	bool qsv_low_delay_brc = false;
	bool qsv_low_delay_brc_verified = false;
	std::uint32_t configured_fps = 0;
	std::uint32_t configured_bitrate_kbps = 0;
	std::int64_t last_submitted_pts = 0;
	EncoderBackendType backend = EncoderBackendType::kSoftware;
	std::string encoder_name;
	std::string input_mode;
	std::string capture_adapter_summary;
	bool hardware_frame_input_active = false;
	bool d3d11_video_processor_scaling_active = false;
	bool gpu_to_cpu_readback_active = false;
	std::string hardware_input_block_reason;
	std::uint32_t encoded_frame_count = 0;
	std::uint32_t encoded_keyframe_count = 0;
	std::uint32_t keyframe_request_count = 0;
	std::uint32_t rate_control_update_attempt_count = 0;
	std::uint32_t rate_control_update_success_count = 0;
	std::uint32_t rate_control_update_unsupported_count = 0;
	std::uint32_t rate_control_update_failure_count = 0;
	bool rate_control_hot_update_supported = false;
	std::uint32_t active_target_bitrate_kbps = 0;
	std::uint32_t active_max_bitrate_kbps = 0;
	std::uint32_t dropped_frame_count = 0;
	std::uint32_t backpressure_event_count = 0;
	std::uint64_t total_input_prepare_us = 0;
	std::uint64_t total_bgra_to_yuv_us = 0;
	std::uint64_t total_send_frame_us = 0;
	std::uint64_t total_receive_packet_us = 0;
	std::uint64_t total_payload_copy_us = 0;
	std::uint64_t total_encode_us = 0;
	EncoderExecutionFailureCategory last_failure = EncoderExecutionFailureCategory::kNone;
	std::string last_error_detail;
};

CaptureAdapterVendor detect_captured_frame_adapter_vendor(
	const CapturedFrame& frame,
	std::string* adapter_summary = nullptr);

class EncoderExecutionSession {
public:
	EncoderExecutionSession();
	~EncoderExecutionSession();

	bool start(
		const EncoderConfigProfile& profile,
		const EncoderBackendBridgePlan& bridge_plan,
		std::string* error_detail = nullptr);

	bool encode_bgra_frame(
		const CapturedFrame& frame,
		std::uint64_t timestamp_ms,
		EncodedFramePacket* packet,
		std::string* error_detail = nullptr);

	void request_keyframe();

	EncoderRateControlUpdateStatus update_rate_control(
		std::uint32_t target_bitrate_kbps,
		std::uint32_t max_bitrate_kbps,
		std::string* error_detail = nullptr);

	void stop();
	[[nodiscard]] bool is_running() const;
	[[nodiscard]] EncoderExecutionDiagnostics diagnostics() const;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

bool build_low_latency_encoder_profile(
	const EncoderProfileRequest& request,
	EncoderConfigProfile* profile,
	std::string* error_detail = nullptr);

[[nodiscard]] std::uint32_t resolve_interactive_desktop_bitrate_floor_kbps(
	std::uint32_t width,
	std::uint32_t height,
	std::uint32_t fps);

bool resolve_viewport_encode_dimensions(
	std::uint32_t source_width,
	std::uint32_t source_height,
	std::uint32_t viewport_width,
	std::uint32_t viewport_height,
	std::uint32_t configured_max_width,
	std::uint32_t* encode_width,
	std::uint32_t* encode_height,
	std::string* error_detail = nullptr);

bool detect_encoder_backend_capabilities(
	EncoderBackendCapabilities* capabilities,
	std::string* error_detail = nullptr);

bool build_encoder_backend_bridge_plan(
	const EncoderBackendBridgeRequest& request,
	const EncoderBackendCapabilities& capabilities,
	EncoderBackendBridgePlan* plan,
	std::string* error_detail = nullptr);

// Give the Controller's first viewport request a bounded opportunity to arrive
// before opening a hardware encoder. This avoids an immediate full-size ->
// viewport-size teardown/reopen while still supporting headless Controllers.
[[nodiscard]] bool should_defer_initial_encoder_start_for_viewport(
	bool encoder_started,
	bool viewport_ready,
	std::uint64_t required_channels_ready_at_ms,
	std::uint64_t now_ms,
	std::uint64_t grace_ms) noexcept;

// Hardware encoder/driver teardown cannot be assumed reliable while a process
// remains live across the supported Windows backends. Preserve the active
// session and let the Controller scale its output; software encoders may still
// be rebuilt.
[[nodiscard]] bool should_preserve_encoder_during_live_reconfiguration(
	EncoderBackendType active_backend,
	bool encoder_started,
	bool resolution_change_requested,
	bool rate_restart_requested) noexcept;

bool start_encoder_execution_from_bridge(
	const EncoderProfileRequest& profile_request,
	const EncoderBackendBridgeRequest& bridge_request,
	EncoderExecutionSession* session,
	EncoderBackendBridgePlan* resolved_plan,
	std::string* error_detail = nullptr);

bool start_encoder_execution_from_bridge(
	const EncoderConfigProfile& profile,
	const EncoderBackendBridgeRequest& bridge_request,
	EncoderExecutionSession* session,
	EncoderBackendBridgePlan* resolved_plan,
	std::string* error_detail = nullptr);

struct CaptureSessionConfig {
	// Local evidence only, not a runtime-profile or wire setting.
	std::string local_evidence_directory;
	std::uint32_t adapter_index = 0;
	std::uint32_t output_index = 0;
	std::string display_id;
	std::uint32_t frame_acquire_timeout_ms = 250;
	CaptureBackendType preferred_backend = CaptureBackendType::kDesktopDuplication;
	bool fallback_enabled = true;
	bool capture_native_d3d11_textures = false;
	bool skip_cpu_readback_when_native_texture_available = false;
	std::uint32_t max_consecutive_failures_before_fallback = 5;
};

struct CaptureHealthSignals {
	std::uint32_t consecutive_failures = 0;
	CaptureBackendType backend = CaptureBackendType::kUnknown;
};

struct CaptureFallbackDecision {
	bool should_switch_backend = false;
	std::string reason;
};

CaptureFallbackDecision evaluate_capture_fallback_decision(
	const CaptureHealthSignals& signals,
	const CaptureSessionConfig& config);

struct CaptureGeometryUpdateDecision {
	bool valid = false;
	bool changed = false;
	std::uint64_t next_revision = 0;
};

CaptureGeometryUpdateDecision evaluate_capture_geometry_update(
	std::uint32_t current_width,
	std::uint32_t current_height,
	std::uint32_t next_width,
	std::uint32_t next_height,
	std::uint64_t current_revision);

struct CaptureBackendTelemetry {
	CaptureAvailability availability = CaptureAvailability::kStopped;
	CaptureFailure last_failure;
	CaptureDesktopContext failure_desktop_context;
	std::uint64_t generation = 0;
	std::uint32_t dda_rebuild_count = 0;
	CaptureBackendType active_backend = CaptureBackendType::kUnknown;
	std::uint32_t backend_switch_count = 0;
	std::uint32_t fallback_attempt_count = 0;
	std::string last_fallback_reason;
	std::uint32_t consecutive_timeouts = 0;
	std::uint32_t consecutive_failures = 0;
	std::uint32_t stale_frame_count = 0;
	double black_frame_ratio = 0.0;
	double frame_present_delta_ms = 0.0;
	std::uint32_t total_capture_attempt_count = 0;
	std::uint32_t total_timeout_count = 0;
	std::uint64_t total_accumulated_frames = 0;
	std::uint32_t last_accumulated_frames = 0;
	std::uint64_t total_capture_wait_us = 0;
	std::uint64_t total_capture_copy_us = 0;
	std::uint64_t native_texture_pool_create_count = 0;
	std::uint64_t native_texture_pool_reuse_count = 0;
	std::uint64_t native_texture_pool_exhaustion_count = 0;
	std::uint64_t frame_pool_recreate_count = 0;
	std::uint64_t total_frame_pool_recreate_us = 0;
	double last_frame_pool_recreate_ms = 0.0;
	double last_capture_wait_ms = 0.0;
	double last_capture_copy_ms = 0.0;
	std::uint32_t total_frames_observed = 0;
};

struct CaptureStabilityRunConfig {
	CaptureSessionConfig capture_config;
	std::uint32_t run_duration_seconds = 1800;
	std::uint32_t max_consecutive_timeouts = 120;
	std::uint32_t max_consecutive_failures = 5;
};

struct CaptureStabilityRunResult {
	bool ok = false;
	std::string error;
	CaptureBackendType backend = CaptureBackendType::kUnknown;
	std::uint32_t backend_switch_count = 0;
	std::uint32_t fallback_attempt_count = 0;
	std::uint32_t frames_captured = 0;
	std::uint32_t capture_attempts = 0;
	std::uint32_t timeout_count = 0;
	std::uint32_t failure_count = 0;
	std::uint32_t max_consecutive_timeouts_seen = 0;
	std::uint32_t max_consecutive_failures_seen = 0;
	std::uint32_t stale_frame_count = 0;
	double black_frame_ratio = 0.0;
	double frame_present_delta_ms = 0.0;
	double average_fps = 0.0;
	std::uint64_t run_duration_ms = 0;
};

bool run_capture_stability_probe(
	const CaptureStabilityRunConfig& config,
	CaptureStabilityRunResult* result,
	std::string* error_detail = nullptr);

struct CapturedFrame {
    // Optional process-local trace stamps; not part of the captured image or wire format.
    std::uint64_t local_capture_begin_us = 0, local_capture_end_us = 0, local_capture_ready_us = 0;
	std::uint64_t capture_generation = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t row_pitch = 0;
	bool bgra = false;
	std::int32_t desktop_origin_x = 0;
	std::int32_t desktop_origin_y = 0;
	std::uint32_t desktop_width = 0;
	std::uint32_t desktop_height = 0;
	std::uint32_t desktop_rotation = 0;
	std::uint64_t desktop_geometry_revision = 0;
	CaptureRegion source_region;
	std::vector<std::uint8_t> data;
	CapturedFrameNativeHandleType native_handle_type = CapturedFrameNativeHandleType::kNone;
	std::shared_ptr<CapturedFrameNativeHandle> native_handle;
};

class WindowsCaptureSession {
public:
	WindowsCaptureSession();
	~WindowsCaptureSession();

	bool start(const CaptureSessionConfig& config, std::string* error_detail = nullptr);
	bool captureFrame(CapturedFrame* frame, std::string* error_detail = nullptr);
	void retryCapture();
	void configureNativeFrameDelivery(
		bool capture_native_d3d11_textures,
		bool skip_cpu_readback_when_native_texture_available);
	void stop();
	bool isRunning() const;
	CaptureBackendType activeBackend() const;
	CaptureBackendTelemetry telemetry() const;

private:
	friend struct CaptureSessionTestAccess;
	class Impl;
	std::unique_ptr<Impl> impl_;
};

struct DdaCapturePocResult {
	bool ok = false;
	std::string error;
	std::string output_file_path;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	bool bgra_frame = false;
};

DdaCapturePocResult run_dda_min_capture_poc(const std::string& output_file_path, std::uint32_t timeout_ms = 3000);

std::string_view module_name();
}
