#pragma once

#include <cstdint>
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
// can force the rollback path.
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
    };

    explicit HostVideoCodecNegotiatorV1(VideoCodecPreference preference) noexcept;

    // Returns true when the peer report changed.
    bool observe_peer(std::uint32_t version, std::uint32_t decode_mask, std::uint32_t hardware_decode_mask);
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

}  // namespace redclaw::protocol
