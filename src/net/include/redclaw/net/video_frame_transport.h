#pragma once
#include "redclaw/net/media_frame_trace.h"
#include "redclaw/net/media_sample_window.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace redclaw::net {

struct EncodedVideoFrameView {
    std::uint64_t frame_id = 0;
    std::uint64_t rate_revision = 0;
    std::uint64_t capture_region_revision = 1;
    std::uint8_t codec = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t timestamp_ms = 0;
    bool keyframe = false;
    std::uint32_t content_rect_x = 0;
    std::uint32_t content_rect_y = 0;
    std::uint32_t content_rect_width = 0;
    std::uint32_t content_rect_height = 0;
    std::span<const std::uint8_t> payload;
};

struct EncodedVideoFragmentPlan {
    std::size_t max_fragment_payload_bytes = 0;
    std::uint16_t fragment_count = 0;
    std::size_t wire_bytes = 0;
};

struct EncodedVideoFragmentView {
    std::uint64_t transport_sequence = 0;
    std::uint64_t frame_id = 0;
    std::uint64_t rate_revision = 0;
    std::uint64_t capture_region_revision = 0;
    std::uint8_t codec = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t timestamp_ms = 0;
    bool keyframe = false;
    std::uint32_t content_rect_x = 0;
    std::uint32_t content_rect_y = 0;
    std::uint32_t content_rect_width = 0;
    std::uint32_t content_rect_height = 0;
    std::uint16_t fragment_index = 0;
    std::uint16_t fragment_count = 0;
    std::uint32_t total_payload_bytes = 0;
    std::span<const std::uint8_t> payload;
};

struct ReassembledEncodedVideoFrame {
    std::uint64_t frame_id = 0;
    std::uint64_t rate_revision = 0;
    std::uint64_t capture_region_revision = 0;
    std::uint8_t codec = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t timestamp_ms = 0;
    bool keyframe = false;
    std::uint32_t content_rect_x = 0;
    std::uint32_t content_rect_y = 0;
    std::uint32_t content_rect_width = 0;
    std::uint32_t content_rect_height = 0;
    std::vector<std::uint8_t> payload;
};

enum class EncodedVideoReassemblyStatus {
    kIncomplete,
    kComplete,
    kDiscarded,
};

struct EncodedVideoReassemblyResult {
    EncodedVideoReassemblyStatus status = EncodedVideoReassemblyStatus::kIncomplete;
    bool abandoned_incomplete_frame = false;
    bool started_frame = false;
    std::uint32_t dropped_incomplete_frames = 0;
    std::uint32_t dropped_dependency_frames = 0;
    std::uint32_t dropped_keyframes = 0;
    std::uint64_t dropped_frame_id = 0;
    bool keyframe_required = false;
    ReassembledEncodedVideoFrame frame;
    std::string error;
};

struct EncodedVideoDropSummary {
    std::uint32_t dropped_incomplete_frames = 0;
    std::uint32_t dropped_keyframes = 0;
    std::uint64_t dropped_frame_id = 0;
};

enum class ReceiverAssemblyPressure {
    kStable,
    kMild,
    kDegraded,
    kSevere,
};

enum class AvailableEncodeBudgetPressure {
    kStable,
    kPressure,
    kSevere,
};

struct ReceiverAssemblyQualityWindow {
    std::uint64_t completed_frames = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t dropped_keyframes = 0;
    std::uint64_t reassembly_timeouts = 0;
};

struct ReceiverAssemblyQuality {
    ReceiverAssemblyPressure pressure = ReceiverAssemblyPressure::kStable;
    std::uint32_t loss_per_mille = 0;
    std::uint32_t fps_scale_percent = 100;
};

struct ReceiverDecodeCapacitySample {
    std::uint64_t reassembled_frames = 0;
    std::uint64_t decoded_frames = 0;
    std::uint64_t window_ms = 0;
    std::uint32_t current_target_fps = 1;
    std::uint32_t maximum_target_fps = 1;
    bool revision_consistent = false;
    bool source_active = false;
    bool network_stable = false;
    bool hard_pressure = false;
    std::uint64_t rate_revision = 0;
    std::uint64_t source_revision = 0;
    std::optional<std::uint64_t> pending_frames;
};

struct ReceiverDecodeCapacityDecision {
    bool valid = false;
    bool pressure = false;
    bool stable = false;
    bool target_changed = false;
    std::uint32_t observed_decode_fps = 0;
    std::uint32_t target_fps = 1;
    std::uint32_t pressure_windows = 0;
    std::uint32_t stable_windows = 0;
    std::uint64_t decrease_total = 0;
    std::uint64_t increase_total = 0;
};

class ReceiverDecodeCapacityController final {
public:
    [[nodiscard]] ReceiverDecodeCapacityDecision update(
        const ReceiverDecodeCapacitySample& sample);
    void reset();

private:
    std::uint32_t pressure_windows_ = 0;
    std::uint32_t stable_windows_ = 0;
    std::uint64_t decrease_total_ = 0;
    std::uint64_t increase_total_ = 0;
    std::uint64_t window_ms_ = 0;
    std::uint64_t reassembled_ = 0;
    std::uint64_t decoded_ = 0;
    std::uint64_t healthy_ms_ = 0;
    std::uint64_t rate_revision_ = 0;
    std::uint64_t source_revision_ = 0;
    std::uint64_t previous_backlog_ = 0;
};

struct StreamRttSignal {
    bool valid = false;
    bool relief = false;
    bool high_pressure = false;
    bool severe_pressure = false;
    std::uint32_t baseline_rtt_ms = 0;
    std::uint32_t queue_delay_ms = 0;
};

enum class SentVideoFrameState { kSending, kCompleted, kDropped };
struct SentVideoFrameMetadata {
    std::uint64_t frame_id = 0;
    std::uint64_t steady_send_time_ms = 0;
    std::uint64_t rate_revision = 0;
    std::uint32_t fps = 0;
    std::uint32_t bitrate_kbps = 0;
    bool keyframe = false;
    SentVideoFrameState state = SentVideoFrameState::kSending;
    std::uint16_t sent_fragments = 0;
};

class SentVideoFrameMetadataRing final {
public:
    explicit SentVideoFrameMetadataRing(std::size_t capacity = 256);

    bool record(const SentVideoFrameMetadata& metadata);
    bool finish(std::uint64_t frame_id, SentVideoFrameState state, std::uint16_t fragments);
    [[nodiscard]] std::optional<SentVideoFrameMetadata> find(std::uint64_t frame_id) const;
    void clear();

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    std::size_t capacity_ = 0;
    std::deque<SentVideoFrameMetadata> entries_;
};

enum class ReceiverFeedbackFreshness {
    kFresh,
    kDelayed,
    kExpired,
    kStaleRevision,
    kUnknownFrame,
};

struct ReceiverFeedbackAssessment {
    ReceiverFeedbackFreshness freshness = ReceiverFeedbackFreshness::kUnknownFrame;
    std::uint64_t feedback_age_ms = 0;
    std::uint64_t fresh_limit_ms = 250;
    std::uint64_t expired_limit_ms = 1000;
};

struct MediaTransportArrival {
    std::uint64_t transport_sequence = 0;
    std::uint64_t receiver_steady_us = 0;
};

struct MediaTransportFeedbackBatch {
    std::uint64_t feedback_id = 0;
    std::uint64_t observed_rate_revision = 0;
    std::vector<MediaTransportArrival> arrivals;
};

class MediaTransportFeedbackRecorder final {
public:
    bool record(
        std::uint64_t transport_sequence,
        std::uint64_t receiver_steady_us,
        std::uint64_t observed_rate_revision);
    [[nodiscard]] bool feedback_due(std::uint64_t receiver_steady_us) const;
    [[nodiscard]] std::optional<MediaTransportFeedbackBatch> take_feedback(
        std::uint64_t receiver_steady_us);
    void reset();

    [[nodiscard]] std::size_t pending_arrivals() const noexcept;

private:
    std::uint64_t next_feedback_id_ = 1;
    std::uint64_t last_transport_sequence_ = 0;
    std::uint64_t next_feedback_due_us_ = 0;
    std::uint64_t observed_rate_revision_ = 0;
    std::deque<MediaTransportArrival> pending_;
};

struct SentMediaTransportPacket {
    std::uint64_t transport_sequence = 0;
    std::uint64_t frame_id = 0;
    std::uint64_t steady_send_us = 0;
    std::uint64_t rate_revision = 0;
    std::size_t wire_bytes = 0;
    bool keyframe = false;
    bool first_packet = false;
    std::uint32_t target_fps = 0;
    std::uint32_t target_bitrate_kbps = 0;
    bool application_limited = false;
    std::uint64_t probe_generation = 0;
};

struct MediaTransportEstimate {
    bool feedback_fresh = false;
    std::uint64_t feedback_sample_id = 0;
    std::uint64_t rate_revision = 0;
    std::uint32_t acknowledged_bitrate_kbps = 0;
    std::uint32_t loss_per_mille = 0;
    std::uint32_t queue_delay_ms = 0;
    std::uint64_t feedback_age_ms = 0;
    std::size_t in_flight_bytes = 0;
    std::uint64_t acknowledged_packets = 0;
    std::uint64_t lost_packets = 0;
    std::uint64_t expired_in_flight_packets = 0;
    std::uint64_t expired_in_flight_bytes = 0;
    std::uint64_t ignored_feedback = 0;
    bool delivery_rate_valid = false;
    bool application_limited = true;
    std::uint32_t delivery_bitrate_kbps = 0;
    std::uint64_t feedback_interval_us = 0;
    std::uint64_t feedback_jitter_us = 0;
    std::uint64_t feedback_round_trip_us = 0;
    std::uint64_t latest_acknowledged_sequence = 0;
    std::size_t acknowledged_batch_bytes = 0;
    std::uint64_t probe_generation = 0;
    std::uint32_t probe_delivery_bitrate_kbps = 0;
    std::size_t probe_acknowledged_bytes = 0;
    bool probe_rate_valid = false;
};

// Diagnostic only: one latest applied packet, not a packet history or a
// cross-clock one-way latency measurement. Sampling never changes adaptation.
struct MediaTransportTimingSample {
    std::uint64_t sample_count = 0;
    std::uint64_t feedback_id = 0;
    std::uint64_t transport_sequence = 0;
    std::uint64_t frame_id = 0;
    std::uint64_t sent_rate_revision = 0;
    std::uint64_t feedback_rate_revision = 0;
    std::uint64_t feedback_host_us = 0;
    std::uint64_t steady_send_us = 0;
    std::uint64_t receiver_steady_us = 0;
    std::uint64_t anchor_send_us = 0;
    std::uint64_t anchor_arrival_us = 0;
    double relative_transit_us = 0.0;
    double smoothed_relative_transit_us = 0.0;
    // Lower baseline projected by the learned clock rate, not a lifetime min.
    double minimum_smoothed_relative_transit_us = 0.0;
    std::uint32_t queue_delay_ms = 0;
    double clock_rate_ppm = 0.0;
    bool clock_rate_ready = false;
};

// Call only on the periodic diagnostic path, never for each packet. The RTT
// value is the latest independent RTT snapshot, not this packet's RTT.
[[nodiscard]] std::string format_media_transport_timing_sample(
    const MediaTransportTimingSample& sample,
    std::uint64_t log_host_us,
    std::uint32_t rtt_queue_latest_ms);

class MediaTransportEstimator final {
public:
    explicit MediaTransportEstimator(std::size_t sent_capacity = 4096);
    ~MediaTransportEstimator();

    MediaTransportEstimator(const MediaTransportEstimator&) = delete;
    MediaTransportEstimator& operator=(const MediaTransportEstimator&) = delete;

    bool record_sent(const SentMediaTransportPacket& packet);
    bool apply_feedback(
        const MediaTransportFeedbackBatch& feedback,
        std::uint64_t host_steady_us,
        std::uint64_t current_rate_revision);
    [[nodiscard]] MediaTransportEstimate snapshot(
        std::uint64_t host_steady_us,
        std::uint32_t smoothed_rtt_ms) const;
    [[nodiscard]] std::optional<MediaTransportTimingSample> timing_snapshot() const;
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class MediaNetworkPressure {
    kStable,
    kMild,
    kSevere,
};

struct MediaSendDemand {
    std::size_t pending_bytes = 0;
    std::uint32_t target_fps = 0;
    std::uint64_t receiver_frame_period_us = 0;
    bool token_limited = false;
    bool resource_limited = false;
    std::uint64_t probe_end_sequence = 0;
};

struct MediaCongestionSample {
    std::uint64_t now_steady_ms = 0;
    std::uint32_t encoder_target_bitrate_kbps = 0;
    std::uint32_t smoothed_rtt_ms = 0;
    std::uint32_t rtt_queue_delay_ms = 0;
    std::uint64_t rtt_sample_id = 0;
    bool rtt_fresh = false;
    bool local_backpressure = false;
    MediaTransportEstimate transport;
    bool recovery_budget_blocked = false;
    bool media_channel_open = false;
    std::size_t buffered_amount = 0;
    MediaSendDemand demand;
};

enum class MediaRecoveryProbePhase { kNone, kProbing, kConfirmed, kCancelled };
struct MediaRecoveryProbe {
    std::uint64_t generation = 0;
    MediaRecoveryProbePhase phase = MediaRecoveryProbePhase::kNone;
    std::size_t wire_budget_bytes = 0;
    std::uint32_t baseline_rate_kbps = 0;
    bool recovery = true;
};

struct MediaCongestionDecision {
    std::uint64_t decision_revision = 0;
    MediaNetworkPressure pressure = MediaNetworkPressure::kStable;
    std::uint32_t pacing_bitrate_kbps = 0;
    bool probe = false;
    bool backoff = false;
    bool isolated_loss = false;
    bool reduce_fps = false;
    std::uint64_t probe_count = 0;
    std::uint64_t backoff_count = 0;
    MediaRecoveryProbe recovery_probe;
    std::size_t in_flight_limit_bytes = 0;
    std::uint64_t feedback_horizon_us = 0;
    std::uint64_t queue_allowance_us = 0;
    std::uint64_t receiver_frame_period_us = 0;
    bool delivery_rate_valid = false;
    std::string reason;
};

class MediaCongestionController final {
public:
    [[nodiscard]] MediaCongestionDecision update(const MediaCongestionSample& sample);
    [[nodiscard]] MediaCongestionDecision on_feedback(
        const MediaTransportEstimate& estimate, const MediaSendDemand& demand,
        std::uint64_t now_ms);
    void reset();

private:
    MediaCongestionDecision update_locked(const MediaCongestionSample& sample);
    std::mutex mutex_;
    MediaCongestionSample latest_;
    MediaCongestionDecision decision_;
    MediaSampleWindow delivery_samples_;
    MediaSampleWindow queue_noise_;
    std::uint64_t next_probe_ms_ = 0;
    std::uint64_t queue_allowance_us_ = 0;
    std::uint32_t confirmed_rate_kbps_ = 0;
    std::uint32_t bootstrap_rate_kbps_ = 0;
    std::uint32_t rtt_pressure_rounds_ = 0;
    std::uint32_t pacing_bitrate_kbps_ = 0;
    std::uint32_t consecutive_loss_windows_ = 0;
    std::uint64_t probe_count_ = 0;
    std::uint64_t backoff_count_ = 0;
    std::uint64_t last_transport_feedback_sample_id_ = 0;
    std::uint64_t last_rtt_sample_id_ = 0;
    MediaRecoveryProbe recovery_probe_;
    std::uint64_t recovery_probe_started_ms_ = 0;
    std::uint32_t recovery_probe_original_rate_ = 0;
    std::uint64_t last_backoff_sample_ms_ = 0;
};

// Independent constraints use the same previous cadence, never sequential
// subtraction. Recovery cannot override a pressure cap in the same tick.
[[nodiscard]] std::uint32_t select_stream_target_fps(
    std::uint32_t current_fps, bool encoder_pressure,
    const MediaCongestionDecision& network,
    const ReceiverDecodeCapacityDecision& capacity);

struct PacedEncodedVideoFrame {
    MediaFrameTrace trace;
    std::uint64_t frame_id = 0;
    std::uint64_t rate_revision = 0;
    std::uint64_t capture_region_revision = 1;
    std::uint8_t codec = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t timestamp_ms = 0;
    std::uint32_t target_fps = 0;
    std::uint32_t target_bitrate_kbps = 0;
    std::uint64_t keyframe_refresh_generation = 0;
    // Local-only dependency lease, assigned by the pacer on admission.
    std::uint64_t recovery_generation = 0;
    bool recovery_frame = false;
    bool keyframe = false;
    std::uint32_t content_rect_x = 0;
    std::uint32_t content_rect_y = 0;
    std::uint32_t content_rect_width = 0;
    std::uint32_t content_rect_height = 0;
    std::vector<std::uint8_t> payload;
    std::uint64_t enqueued_us = 0;
};

struct MediaPacerSendResult {
    bool accepted = false;
    bool queued = false;
    std::size_t buffered_amount = 0;
};

struct MediaPacerTransportState {
    bool open = false;
    std::size_t buffered_amount = 0;
};

enum class MediaPacerFrameEventType {
    kSent,
    kDropped,
    kKeyframeRequired,
};

struct MediaPacerFrameEvent {
    MediaPacerFrameEventType type = MediaPacerFrameEventType::kDropped;
    std::uint64_t frame_id = 0;
    std::uint64_t rate_revision = 0;
    std::uint32_t target_fps = 0;
    std::uint32_t target_bitrate_kbps = 0;
    std::uint64_t keyframe_refresh_generation = 0;
    bool keyframe = false;
    std::uint16_t sent_fragments = 0;
    std::uint16_t fragment_count = 0;
    std::size_t wire_bytes = 0;
    std::uint32_t pacing_bitrate_kbps = 0;
    std::uint64_t pacing_duration_ms = 0;
    std::uint64_t deadline_ms = 0;
    std::uint64_t elapsed_us = 0;
    std::string reason;
};

enum class MediaBufferTargetReason { kBootstrap, kMeasuredService, kCongestion, kResourceLimit };

struct MediaPacerTelemetry {
    std::uint32_t pacing_bitrate_kbps = 0;
    std::uint32_t smoothed_rtt_ms = 0;
    std::size_t in_flight_bytes = 0;
    std::size_t in_flight_limit_bytes = 0;
    std::size_t buffered_limit_bytes = 0;
    std::size_t active_depth = 0;
    std::size_t pending_depth = 0;
    std::uint64_t packets_sent = 0;
    std::uint64_t frames_sent = 0;
    std::uint64_t deadline_drops = 0;
    std::uint64_t frame_budget_rejections = 0;
    std::uint64_t pacing_token_deadline_drops = 0;
    std::uint64_t in_flight_deadline_drops = 0;
    std::uint64_t buffered_deadline_drops = 0;
    std::uint64_t channel_deadline_drops = 0;
    std::uint64_t send_failures = 0;
    std::uint64_t in_flight_timeouts = 0;
    bool keyframe_required = false;
    std::uint64_t recovery_generation = 1;
    std::uint64_t dependency_pending_drops = 0;
    std::uint64_t suppressed_keyframe_requests = 0;
    std::uint64_t budget_revision = 0;
    std::uint64_t next_admission_ms = 0;
    std::uint64_t rejected_wire_bytes = 0;
    std::uint64_t probe_wire_bytes = 0;
    std::uint64_t budget_retry_attempt = 0;
    std::size_t pending_bytes = 0;
    std::size_t active_bytes = 0;
    std::size_t retained_bytes = 0;
    std::size_t reserved_bytes = 0;
    std::size_t queue_target_frames = 1;
    std::size_t queue_target_bytes = 0;
    std::uint64_t queue_target_us = 0;
    std::uint64_t oldest_pending_us = 0;
    std::uint64_t timer_lateness_us = 0;
    std::uint64_t token_wait_total_us = 0;
    bool frame_token_limited = false;
    std::uint64_t probe_end_sequence = 0;
    std::uint32_t target_fps = 0;
    bool resource_limited = false;
    bool congested = false;
    MediaBufferTargetReason buffer_target_reason = MediaBufferTargetReason::kBootstrap;
    [[nodiscard]] MediaSendDemand demand() const;
};

inline constexpr std::uint64_t kMediaPacerMaximumFrameDeadlineMs = 5000;
inline constexpr std::uint64_t kMediaPacerRecoveryFrameDeadlineMs = 10000;
inline constexpr std::uint64_t kEncodedVideoReassemblyTimeoutMs =
    kMediaPacerMaximumFrameDeadlineMs + 500;
inline constexpr std::uint64_t kEncodedKeyframeReassemblyTimeoutMs =
    kMediaPacerRecoveryFrameDeadlineMs + 500;

enum class MediaAdmissionState {
    kReady, kWaitCapacity, kWaitRecovery, kBudgetInfeasible, kStopped, kInvalidFrame
};
struct MediaAdmissionResult {
    MediaAdmissionState state = MediaAdmissionState::kStopped;
    std::uint64_t budget_revision = 0;
    std::uint64_t next_check_ms = 0;
    std::string detail;
    [[nodiscard]] bool ready() const { return state == MediaAdmissionState::kReady; }
};

struct MediaPacerFrameBudget {
    std::uint64_t pacing_duration_ms = 0;
    std::uint64_t guard_duration_ms = 0;
    std::uint64_t deadline_ms = 0;
    bool feasible = false;
};

[[nodiscard]] MediaPacerFrameBudget resolve_media_pacer_frame_budget(
    std::size_t wire_bytes,
    std::uint32_t pacing_bitrate_kbps,
    std::uint32_t smoothed_rtt_ms,
    bool recovery_frame = false);

constexpr std::size_t kMaxNavigationThumbnailJpegBytes = 512U * 1024U;

struct NavigationThumbnailView {
    std::uint64_t catalog_revision = 0;
    std::uint64_t thumbnail_revision = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string_view display_id;
    std::span<const std::uint8_t> jpeg;
};

struct NavigationThumbnail {
    std::uint64_t catalog_revision = 0;
    std::uint64_t thumbnail_revision = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string display_id;
    std::vector<std::uint8_t> jpeg;
};

bool serialize_navigation_thumbnail(
    const NavigationThumbnailView& thumbnail,
    std::vector<std::uint8_t>* packet,
    std::string* error_detail);

bool parse_navigation_thumbnail(
    std::span<const std::uint8_t> packet,
    NavigationThumbnail* thumbnail,
    std::string* error_detail);

class MediaPacingBudget final {
public:
    void update_rate(std::uint32_t pacing_bitrate_kbps, std::uint64_t now_steady_us);
    [[nodiscard]] std::uint64_t delay_until_available_us(
        std::size_t wire_bytes,
        std::uint64_t now_steady_us);
    bool consume(std::size_t wire_bytes, std::uint64_t now_steady_us);
    void observe_wait(std::uint64_t requested_us, std::uint64_t elapsed_us);
    void suspend(std::uint64_t now_steady_us);
    void set_window_limit(std::size_t bytes);
    [[nodiscard]] std::uint64_t lateness_us() const;
    void reset();

private:
    void refill(std::uint64_t now_steady_us);

    std::uint32_t pacing_bitrate_kbps_ = 0;
    std::uint64_t last_refill_us_ = 0;
    double tokens_bytes_ = 0.0;
    double burst_bytes_ = 0.0;
    bool initialized_ = false;
    std::size_t window_limit_bytes_ = 1024 * 1024;
    MediaSampleWindow lateness_;
};

using MediaPacerSendCallback =
    std::function<MediaPacerSendResult(std::span<const std::uint8_t>)>;
using MediaPacerTransportStateCallback = std::function<MediaPacerTransportState()>;
using MediaPacerPacketSentCallback = std::function<void(const SentMediaTransportPacket&)>;
using MediaPacerFrameEventCallback = std::function<void(const MediaPacerFrameEvent&)>;
using MediaPacerCapacityCallback = std::function<void()>;

class DesktopMediaSendPacer final {
public:
    class EncodeReservation final {
    public:
        EncodeReservation() = default;
        EncodeReservation(EncodeReservation&& other) noexcept;
        EncodeReservation& operator=(EncodeReservation&& other) noexcept;
        ~EncodeReservation();
        explicit operator bool() const { return owner_ != nullptr; }
        [[nodiscard]] std::size_t payload_limit_bytes() const;
        std::vector<std::uint8_t> payload;
        MediaAdmissionResult submit(PacedEncodedVideoFrame frame);
    private:
        friend class DesktopMediaSendPacer;
        DesktopMediaSendPacer* owner_ = nullptr;
        std::uint64_t generation_ = 0;
        void release();
    };
    DesktopMediaSendPacer();
    ~DesktopMediaSendPacer();

    DesktopMediaSendPacer(const DesktopMediaSendPacer&) = delete;
    DesktopMediaSendPacer& operator=(const DesktopMediaSendPacer&) = delete;

    bool start(
        MediaPacerSendCallback send_callback,
        MediaPacerTransportStateCallback transport_state_callback,
        MediaPacerPacketSentCallback packet_sent_callback,
        MediaPacerFrameEventCallback frame_event_callback,
        MediaPacerCapacityCallback capacity_callback = {},
        MediaFrameTraceRecorder* trace_recorder = nullptr);
    void stop();
    // Source/capture recovery stays in the current feedback sequence space.
    // Pass true only when both transport feedback endpoints start a new epoch.
    void reset(bool reset_transport_sequence = false);
    void update_budget(
        std::uint32_t pacing_bitrate_kbps,
        std::uint32_t smoothed_rtt_ms,
        std::size_t in_flight_bytes,
        const MediaRecoveryProbe* recovery_probe = nullptr,
        const MediaCongestionDecision* policy = nullptr);
    void update_policy(const MediaCongestionDecision& decision,
        std::uint32_t smoothed_rtt_ms, std::size_t in_flight_bytes);
    [[nodiscard]] EncodeReservation reserve_encode();
    void observe_ack_progress(std::uint64_t acknowledged_packets);
    void notify_writable();

    [[nodiscard]] MediaAdmissionResult admission() const;
    [[nodiscard]] MediaAdmissionResult submit_frame(PacedEncodedVideoFrame frame);
    [[nodiscard]] bool can_accept_frame() const;
    bool submit(PacedEncodedVideoFrame frame, std::string* error_detail = nullptr);
    [[nodiscard]] MediaPacerTelemetry telemetry() const;

private:
    MediaAdmissionResult submit_reserved(PacedEncodedVideoFrame frame, std::uint64_t generation);
    void cancel_reservation(std::uint64_t generation, std::vector<std::uint8_t> payload);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class DesktopStreamEndpointRole {
    kHost,
    kController,
};

enum class DesktopStreamHealth {
    kIdle,
    kCaptureStartupStalled,
    kEncoderStartupStalled,
    kTransmitStartupStalled,
    kReceiveStartupStalled,
    kPlaybackStartupStalled,
    kSendFailures,
    kSendBackpressure,
    kDecodeFailures,
    kRenderFailures,
    kEncodeBackpressure,
    kDirectPipeWriteFailures,
    kNetworkLoss,
    kRelayLoss,
    kPlaybackBacklog,
    kRelayPath,
    kDirectNatPath,
    kHealthy,
};

struct DesktopStreamHealthSample {
    DesktopStreamEndpointRole role = DesktopStreamEndpointRole::kHost;
    bool startup_tracking = false;
    std::uint64_t startup_elapsed_ms = 0;
    std::uint64_t startup_timeout_ms = 0;
    std::uint32_t captured_frames_total = 0;
    std::uint32_t encode_submit_attempts_total = 0;
    std::uint32_t encoded_frames_total = 0;
    std::uint32_t transmitted_frames_total = 0;
    std::uint32_t received_frames_total = 0;
    std::uint32_t reassembled_frames_total = 0;
    std::uint32_t delivered_frames_total = 0;
    bool relay_path = false;
    bool direct_nat_path = false;
    bool encode_backpressure = false;
    std::uint32_t encode_failures = 0;
    std::uint32_t transmit_failures = 0;
    std::uint32_t transmit_backpressure = 0;
    std::uint32_t decode_failures = 0;
    std::uint32_t render_failures = 0;
    std::uint32_t receiver_loss_per_mille = 0;
    std::uint32_t transmitted_frames = 0;
    std::uint32_t received_frames = 0;
    std::uint32_t rendered_frames = 0;
    bool direct_pipe_connected = false;
    std::uint32_t direct_pipe_write_failures = 0;
    std::uint64_t direct_pipe_writer_frames = 0;
    std::uint64_t direct_pipe_reader_frames = 0;
    std::uint64_t direct_pipe_backlog = 0;
};

[[nodiscard]] DesktopStreamHealth classify_desktop_stream_health(
    const DesktopStreamHealthSample& sample);
[[nodiscard]] std::string_view desktop_stream_health_name(
    DesktopStreamHealth health) noexcept;

[[nodiscard]] ReceiverFeedbackAssessment assess_receiver_feedback(
    const std::optional<SentVideoFrameMetadata>& sent_frame,
    std::uint64_t now_steady_ms,
    std::uint32_t smoothed_rtt_ms,
    std::uint64_t observed_rate_revision,
    std::uint64_t current_rate_revision);

class EncodedVideoFrameReassembler {
public:
    [[nodiscard]] EncodedVideoReassemblyResult push(
        const EncodedVideoFragmentView& fragment,
        std::uint64_t now_ms);
    [[nodiscard]] EncodedVideoDropSummary require_keyframe();
    void reset();

    [[nodiscard]] bool has_incomplete_frame() const;
    [[nodiscard]] bool requires_keyframe() const;
    [[nodiscard]] std::uint64_t started_at_ms() const;
    [[nodiscard]] std::uint64_t incomplete_frame_id() const;
    [[nodiscard]] bool expired(std::uint64_t now_steady_ms) const;
    [[nodiscard]] bool keyframe_progressing(std::uint64_t now_steady_ms, std::uint32_t srtt_ms) const;

private:
    void clear_current_frame();

    ReassembledEncodedVideoFrame frame_;
    std::uint16_t next_fragment_index_ = 0;
    std::uint16_t fragment_count_ = 0;
    std::uint32_t total_payload_bytes_ = 0;
    std::uint64_t started_at_ms_ = 0;
    std::uint64_t last_progress_ms_ = 0;
    std::uint64_t discarded_frame_id_ = 0;
    bool awaiting_keyframe_ = true;
};

[[nodiscard]] ReceiverAssemblyQuality evaluate_receiver_assembly_quality(
    const ReceiverAssemblyQualityWindow& window);
[[nodiscard]] std::uint32_t saturating_reduce_stream_target_fps(
    std::uint32_t current_fps,
    std::uint32_t decrement);
[[nodiscard]] StreamRttSignal evaluate_stream_rtt_signal(
    std::uint32_t latest_rtt_ms,
    std::uint32_t minimum_rtt_ms,
    std::uint32_t relief_queue_delay_ms,
    std::uint32_t high_queue_delay_ms,
    std::uint32_t severe_queue_delay_ms);
[[nodiscard]] std::uint32_t resolve_available_encode_budget_gap(
    std::uint32_t captured_frames,
    std::uint32_t encoded_frames,
    std::uint32_t target_budget_frames);
[[nodiscard]] AvailableEncodeBudgetPressure evaluate_available_encode_budget_pressure(
    std::uint32_t captured_frames,
    std::uint32_t encoded_frames,
    std::uint32_t target_budget_frames);

bool build_encoded_video_fragment_plan(
    const EncodedVideoFrameView& frame,
    std::size_t max_packet_bytes,
    EncodedVideoFragmentPlan* plan,
    std::string* error_detail = nullptr);

bool serialize_encoded_video_fragment(
    const EncodedVideoFrameView& frame,
    const EncodedVideoFragmentPlan& plan,
    std::uint16_t fragment_index,
    std::uint64_t transport_sequence,
    std::vector<std::uint8_t>* packet,
    std::string* error_detail = nullptr);

bool parse_encoded_video_fragment(
    std::span<const std::uint8_t> packet,
    EncodedVideoFragmentView* fragment,
    std::string* error_detail = nullptr);

}  // namespace redclaw::net
