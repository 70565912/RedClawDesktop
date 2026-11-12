#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace redclaw::protocol {

// Optional Hello/Capabilities fields 92-94. Zero means an older peer, which
// is only guaranteed to decode H.264.
inline constexpr std::uint32_t kVideoCodecCapabilityVersion = 1;
inline constexpr std::uint32_t kVideoCodecMaskH264 = 1U << 0U;
inline constexpr std::uint32_t kVideoCodecMaskHevc = 1U << 1U;
inline constexpr std::uint32_t kKnownVideoCodecMask = kVideoCodecMaskH264 | kVideoCodecMaskHevc;

// Runtime setting `stream_video_codec` / `--stream-video-codec`.
// kAuto prefers HEVC only on hardware paths on both sides; kH264 is the
// rollback switch and also makes a Controller advertise H.264 only.
enum class VideoCodecPreference {
    kAuto,
    kH264,
    kHevc,
};

enum class NegotiatedVideoCodec {
    kH264,
    kHevc,
};

struct PeerVideoDecodeCapabilities {
    std::uint32_t version = 0;
    std::uint32_t decode_mask = 0;
    std::uint32_t hardware_decode_mask = 0;
};

struct HostVideoEncodeCapabilities {
    bool hevc_hardware_encoder_available = false;
    bool hevc_software_encoder_available = false;
    // Sticky for the Host process after a real HEVC open failure.
    bool hevc_encoder_open_failed = false;
};

struct VideoCodecSelection {
    NegotiatedVideoCodec codec = NegotiatedVideoCodec::kH264;
    // Only an explicit `hevc` preference may use the software HEVC encoder.
    bool allow_software_encoder = true;
    std::string_view reason;
};

// Local Controller decode report before preference is applied.
struct LocalVideoDecodeSupport {
    bool h264_decoder = false;
    bool hevc_decoder = false;
    bool h264_hardware = false;
    bool hevc_hardware = false;
};

[[nodiscard]] std::optional<VideoCodecPreference> parse_video_codec_preference(std::string_view value);
[[nodiscard]] std::string_view to_string(VideoCodecPreference preference);
[[nodiscard]] std::string_view to_string(NegotiatedVideoCodec codec);
[[nodiscard]] std::string describe_video_codec_mask(std::uint32_t mask);

// Wire byte in the video fragment header: 1 = H.264, 2 = HEVC.
[[nodiscard]] std::uint32_t video_codec_mask_bit(NegotiatedVideoCodec codec) noexcept;

// What a Controller advertises. A `h264` preference hides HEVC so either side
// can force the rollback path. HEVC is advertised only when this machine can
// hardware-decode it; a software decoder is not offered.
[[nodiscard]] PeerVideoDecodeCapabilities build_local_video_decode_capabilities(
    const LocalVideoDecodeSupport& support,
    VideoCodecPreference preference) noexcept;

// H.264 is the mandatory baseline, so it is always accepted. HEVC needs an
// explicit report from this session's peer.
[[nodiscard]] bool peer_accepts_video_codec(
    const PeerVideoDecodeCapabilities& peer,
    NegotiatedVideoCodec codec) noexcept;

[[nodiscard]] VideoCodecSelection select_video_codec(
    VideoCodecPreference preference,
    const PeerVideoDecodeCapabilities& peer,
    const HostVideoEncodeCapabilities& host) noexcept;

// Hold the first encode of a session (bounded by grace_ms) until the peer's
// Hello/Capabilities arrives, so the Host does not start with a codec it must
// immediately tear down.
[[nodiscard]] bool should_wait_for_peer_video_codec_report(
    bool peer_report_received,
    std::uint64_t required_channels_ready_at_ms,
    std::uint64_t now_ms,
    std::uint64_t grace_ms) noexcept;

// A running H.264 encoder is rebuilt as HEVC only when the negotiation changed
// since it started (late or reconnect peer report) and the current selection
// is HEVC. Each revision yields at most one rebuild, and a failed HEVC open
// bumps the revision while selecting H.264, so this cannot loop. The rebuilt
// encoder starts on an IDR.
[[nodiscard]] bool should_upgrade_running_encoder_to_hevc(
    NegotiatedVideoCodec running_codec,
    std::uint64_t negotiation_revision_at_start,
    std::uint64_t current_negotiation_revision,
    const VideoCodecSelection& current_selection) noexcept;

// Host-side owner of the per-session peer report and the sticky HEVC failure.
// Control-channel and encoder threads share it.
class HostVideoCodecNegotiatorV1 final {
public:
    struct Snapshot {
        bool peer_report_received = false;
        PeerVideoDecodeCapabilities peer;
        bool hevc_encoder_open_failed = false;
        std::uint64_t revision = 0;
        // Sanitized wire field 95 from the latest peer report; empty if absent.
        std::string peer_change_reason;
    };

    explicit HostVideoCodecNegotiatorV1(VideoCodecPreference preference) noexcept;

    // Returns true when the peer report changed.
    bool observe_peer(
        std::uint32_t version,
        std::uint32_t decode_mask,
        std::uint32_t hardware_decode_mask,
        std::string_view change_reason = {});
    void reset_peer();
    void record_hevc_encoder_open_failure();

    [[nodiscard]] VideoCodecPreference preference() const noexcept { return preference_; }
    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] VideoCodecSelection select(HostVideoEncodeCapabilities host) const;

private:
    const VideoCodecPreference preference_;
    mutable std::mutex mutex_;
    Snapshot state_;
};

// Accepts only short lowercase tokens ([a-z0-9_], at most 64 chars) from the
// peer so a diagnostics field cannot inject log content. Returns empty
// otherwise.
[[nodiscard]] std::string sanitize_video_codec_change_reason(std::string_view reason);

inline constexpr std::string_view kControllerHevcDecodeOverloadReason = "controller_hevc_decode_overload";

// Frames lost on the network, for ControllerHevcDecodeOverloadMonitor.
// `dependency_frames_dropped` counts every dropped dependent frame;
// `local_dependency_frames_dropped` is the part discarded locally while
// waiting for an IDR after a ring gap or busy drop. Those are a consequence
// of decode overload, not network loss, so they are excluded; otherwise each
// overrun would reset the clean-network window and the fallback never fires.
[[nodiscard]] std::uint64_t controller_network_loss_total(
    std::uint64_t incomplete_frames_dropped,
    std::uint64_t dependency_frames_dropped,
    std::uint64_t local_dependency_frames_dropped,
    std::uint64_t dropped_keyframes,
    std::uint64_t reassembly_timeouts) noexcept;

// Controller-side safety net: HEVC frames that the local decoder cannot keep
// up with show as shared-memory ring overruns (the reader is a full ring
// behind). When that happens repeatedly while the network is clean, the
// Controller withdraws HEVC so the Host renegotiates to H.264. A separate
// bounded trial policy may explicitly rearm the monitor. Ring overruns while frames are
// being lost on the network are attributed to the network and ignored.
// Not thread-safe; the owner serializes calls.
class ControllerHevcDecodeOverloadMonitor final {
public:
    struct Config {
        std::uint32_t overrun_episodes = 5;
        std::uint64_t window_ms = 30000;
        // Network loss within this interval before an overrun disqualifies it.
        std::uint64_t clean_network_ms = 5000;
    };
    struct Observation {
        std::uint64_t now_ms = 0;
        NegotiatedVideoCodec codec = NegotiatedVideoCodec::kH264;
        // latest written sequence minus reader sequence after the write.
        std::uint64_t ring_backlog = 0;
        std::uint32_t ring_slots = 0;
        // Monotonic count of frames lost on the network; see
        // controller_network_loss_total.
        std::uint64_t network_loss_total = 0;
        bool playback_eligible = true;
        std::uint64_t playback_revision = 0;
    };

    ControllerHevcDecodeOverloadMonitor() noexcept : ControllerHevcDecodeOverloadMonitor(Config{}) {}
    explicit ControllerHevcDecodeOverloadMonitor(Config config) noexcept;

    // Returns true exactly once, on the observation that trips the fallback.
    bool observe(const Observation& observation);
    // Preserve real loss history and an already latched fallback on pause.
    void clear_overrun_history() noexcept;
    void rearm() noexcept;

    [[nodiscard]] bool triggered() const noexcept { return triggered_; }
    [[nodiscard]] std::uint64_t counted_episodes() const noexcept { return total_counted_; }
    [[nodiscard]] std::uint64_t ignored_lossy_episodes() const noexcept { return ignored_lossy_; }
    [[nodiscard]] std::size_t episodes_in_window() const noexcept { return episodes_.size(); }
    [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
    Config config_;
    std::deque<std::uint64_t> episodes_;
    bool in_overrun_ = false;
    bool triggered_ = false;
    bool loss_seen_ = false;
    bool loss_changed_ = false;
    std::uint64_t last_loss_total_ = 0;
    std::uint64_t last_loss_change_ms_ = 0;
    std::uint64_t total_counted_ = 0;
    std::uint64_t ignored_lossy_ = 0;
    std::uint64_t playback_revision_ = 0;
};

// One automatic trial per runtime. Publication is an attempt; only actual
// HEVC output followed by 30 seconds of healthy playback completes a trial.
class ControllerHevcTrialPolicy final {
public:
    struct Sample {
        std::uint64_t now_ms = 0;
        NegotiatedVideoCodec codec = NegotiatedVideoCodec::kH264;
        bool visible = true;
        bool feedback_fresh = false;
        bool backend_healthy = false;
        bool recovering = false;
        std::uint64_t backlog = 0;
        std::uint32_t ring_slots = 6;
        std::uint64_t progress = 0;
        std::uint64_t hevc_progress = 0;
        std::uint64_t failures = 0;
        std::uint64_t recovery_breaks = 0;
        std::uint64_t playback_revision = 0;
    };
    void on_fallback(std::uint64_t now_ms) noexcept;
    // True once when the original eligible report may be restored.
    bool observe(const Sample& sample) noexcept;
    void cancel_unpublished_attempt() noexcept {
        attempted_ = false;
        withdrawn_ = true;
        stable_since_ms_ = trial_since_ms_ = 0;
    }
    [[nodiscard]] bool attempted() const noexcept { return attempted_; }
    [[nodiscard]] bool trial_succeeded() const noexcept { return trial_succeeded_; }
private:
    bool withdrawn_ = false, attempted_ = false, trial_succeeded_ = false;
    std::uint64_t fallback_ms_ = 0, stable_since_ms_ = 0, trial_since_ms_ = 0;
    std::uint64_t last_sample_ms_ = 0, last_progress_ms_ = 0;
    std::uint64_t progress_ = 0, failures_ = 0, breaks_ = 0;
    std::uint64_t playback_revision_ = 0;
    std::uint64_t attempt_hevc_progress_ = 0;
    std::uint64_t hevc_progress_ = 0, last_hevc_progress_ms_ = 0;
};

}  // namespace redclaw::protocol
