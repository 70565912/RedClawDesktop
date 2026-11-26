#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace redclaw::runtime {

struct DesktopStreamCounters {
    std::uint32_t captured_frames = 0;
    std::uint32_t synthetic_frames = 0;
    std::uint32_t capture_failures = 0;
    std::uint32_t encoded_frames = 0;
    std::uint32_t encode_failures = 0;
    std::uint32_t transmitted_frames = 0;
    std::uint32_t transmit_failures = 0;
    std::uint32_t transmit_backpressure_drops = 0;
    std::uint32_t transmit_local_pacing_drops = 0;
    std::uint32_t received_frames = 0;
    std::uint32_t decode_failures = 0;
    std::uint32_t decoded_frames = 0;
    std::uint32_t render_failures = 0;
    std::uint32_t rendered_frames = 0;
    std::string last_error;
    std::string last_preview_path;
};

inline std::string format_auth_rejected_stream_stats(
    std::string_view role, const DesktopStreamCounters& counters,
    bool media_channel_open, bool control_channel_open,
    std::uint64_t media_fragments_sent, std::uint64_t media_fragments_received) {
    std::ostringstream line;
    line << "Runtime desktop stream stats role=" << role << std::boolalpha
         << " channel_open=" << (media_channel_open && control_channel_open)
         << " media_channel_open=" << media_channel_open
         << " captured=" << counters.captured_frames
         << " synthetic=" << counters.synthetic_frames
         << " capture_failures=" << counters.capture_failures
         << " encoded=" << counters.encoded_frames
         << " encode_failures=" << counters.encode_failures
         << " transmitted=" << counters.transmitted_frames
         << " transmit_failures=" << counters.transmit_failures
         << " received=" << counters.received_frames
         << " decoded=" << counters.decoded_frames
         << " media_fragments_sent=" << media_fragments_sent
         << " media_fragments_received=" << media_fragments_received
         << " event=connection_auth_rejected";
    return line.str();
}

}  // namespace redclaw::runtime
