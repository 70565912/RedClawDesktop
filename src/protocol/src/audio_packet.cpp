#include "redclaw/protocol/audio_packet.h"

#include <cstring>

namespace redclaw::protocol {
namespace {

void write_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void write_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

void write_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

template <typename T>
bool read_le(std::span<const std::uint8_t> bytes, std::size_t offset, T* value) {
    if (value == nullptr || offset + sizeof(T) > bytes.size()) {
        return false;
    }
    T assembled = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        assembled |= static_cast<T>(static_cast<std::uint64_t>(bytes[offset + index]) << (8 * index));
    }
    *value = assembled;
    return true;
}

}  // namespace

std::vector<std::uint8_t> serialize_audio_packet_v1(const AudioPacketV1& packet) {
    if (packet.version != kAudioPacketVersion
        || packet.channels != kAudioChannels
        || packet.sample_rate != kAudioSampleRate
        || packet.frame_samples != kAudioFrameSamples
        || packet.sequence == 0
        || packet.payload.empty()
        || packet.payload.size() > kMaxAudioPacketPayloadBytes) {
        return {};
    }
    std::vector<std::uint8_t> out;
    out.reserve(kAudioPacketHeaderBytes + packet.payload.size());
    out.push_back(packet.version);
    out.push_back(packet.channels);
    write_u32(out, packet.sample_rate);
    write_u16(out, packet.frame_samples);
    write_u32(out, packet.sequence);
    write_u64(out, packet.timestamp_us);
    out.insert(out.end(), packet.payload.begin(), packet.payload.end());
    return out;
}

ParseResult<AudioPacketV1> parse_audio_packet_v1(std::span<const std::uint8_t> bytes) {
    ParseResult<AudioPacketV1> result;
    if (bytes.size() < kAudioPacketHeaderBytes) {
        result.error = "audio packet is truncated";
        return result;
    }
    AudioPacketV1 packet;
    packet.version = bytes[0];
    packet.channels = bytes[1];
    if (!read_le(bytes, 2, &packet.sample_rate)
        || !read_le(bytes, 6, &packet.frame_samples)
        || !read_le(bytes, 8, &packet.sequence)
        || !read_le(bytes, 12, &packet.timestamp_us)) {
        result.error = "audio packet header is truncated";
        return result;
    }
    if (packet.version != kAudioPacketVersion
        || packet.channels != kAudioChannels
        || packet.sample_rate != kAudioSampleRate
        || packet.frame_samples != kAudioFrameSamples
        || packet.sequence == 0) {
        result.error = "audio packet header is not supported";
        return result;
    }
    const std::size_t payload_bytes = bytes.size() - kAudioPacketHeaderBytes;
    if (payload_bytes == 0 || payload_bytes > kMaxAudioPacketPayloadBytes) {
        result.error = "audio packet payload size is invalid";
        return result;
    }
    packet.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kAudioPacketHeaderBytes), bytes.end());
    result.ok = true;
    result.value = std::move(packet);
    return result;
}

}  // namespace redclaw::protocol
