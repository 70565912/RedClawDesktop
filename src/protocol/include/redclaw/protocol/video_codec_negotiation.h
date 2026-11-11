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

// Controller-side safety net: HEVC frames that the local decoder cannot keep
// up with show as shared-memory ring overruns (the reader is a full ring
// behind). When that happens repeatedly while the network is clean, the
// Controller withdraws HEVC from its decode report for the rest of the
// session so the Host renegotiates to H.264. Ring overruns while frames are
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
        // Monotonic count of frames lost before reaching the decoder.
        std::uint64_t network_loss_total = 0;
    };

    ControllerHevcDecodeOverloadMonitor() noexcept : ControllerHevcDecodeOverloadMonitor(Config{}) {}
    explicit ControllerHevcDecodeOverloadMonitor(Config config) noexcept;

    // Returns true exactly once, on the observation that trips the fallback.
    bool observe(const Observation& observation);

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
};

}  // namespace redclaw::protocol
