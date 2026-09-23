#pragma once

#include "redclaw/protocol/protocol_module.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace redclaw::protocol {

inline constexpr std::uint32_t kAudioStreamCapabilityVersion = 1;
inline constexpr std::uint8_t kAudioPacketVersion = 1;
inline constexpr std::uint32_t kAudioSampleRate = 48000;
inline constexpr std::uint16_t kAudioChannels = 2;
inline constexpr std::uint16_t kAudioFrameSamples = 960;
inline constexpr int kAudioBitrateBps = 96000;
inline constexpr std::size_t kAudioPacketHeaderBytes = 20;
inline constexpr std::size_t kMaxAudioPacketPayloadBytes = 1275;

struct AudioPacketV1 {
    std::uint8_t version = kAudioPacketVersion;
    std::uint8_t channels = static_cast<std::uint8_t>(kAudioChannels);
    std::uint32_t sample_rate = kAudioSampleRate;
    std::uint16_t frame_samples = kAudioFrameSamples;
    std::uint32_t sequence = 0;
    std::uint64_t timestamp_us = 0;
    std::vector<std::uint8_t> payload;
};

// Controller playback plus a supporting peer opens the audio channel.
// A secure desktop on the deciding endpoint keeps the stream closed.
[[nodiscard]] inline bool audio_stream_wanted(
    std::uint32_t peer_audio_version,
    bool playback_requested,
    bool secure_desktop) noexcept {
    return !secure_desktop
        && playback_requested
        && peer_audio_version >= 1
        && kAudioStreamCapabilityVersion >= 1;
}

[[nodiscard]] std::vector<std::uint8_t> serialize_audio_packet_v1(const AudioPacketV1& packet);
[[nodiscard]] ParseResult<AudioPacketV1> parse_audio_packet_v1(std::span<const std::uint8_t> bytes);

}  // namespace redclaw::protocol
