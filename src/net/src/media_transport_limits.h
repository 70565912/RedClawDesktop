#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace redclaw::net::transport_detail {
// Wire overhead and in-flight expiry are shared, immutable contracts, not
// another congestion authority. Keep all queue ownership in the existing pacer.
inline constexpr std::size_t kFragmentHeaderBytes = 80;
inline constexpr std::size_t kMinimumPacerWindowBytes = 16 * 1024;
inline constexpr std::size_t kMaximumPacerWindowBytes = 1024 * 1024;
// Resource safety, not adaptive working points. One legal output is reserved
// before encode; active, queued, idle and reserved storage share this limit.
inline constexpr std::size_t kMaximumEncodedFrameBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kPacerPayloadMemoryLimitBytes = 3 * kMaximumEncodedFrameBytes;
inline constexpr std::size_t kPacerMaximumQueueFrames = 32;
inline std::size_t resolve_pacer_window_bytes(
    std::uint32_t pacing_bitrate_kbps,
    std::uint32_t smoothed_rtt_ms) {
    const std::uint64_t window_ms = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(smoothed_rtt_ms) * 2ULL,
        1ULL);
    const std::uint64_t bytes = static_cast<std::uint64_t>(pacing_bitrate_kbps)
        * 1000ULL * window_ms / 8000ULL;
    return static_cast<std::size_t>(std::clamp<std::uint64_t>(
        bytes,
        kMinimumPacerWindowBytes,
        kMaximumPacerWindowBytes));
}

inline std::uint64_t resolve_transport_in_flight_expiry_us(std::uint32_t smoothed_rtt_ms) {
    return std::clamp<std::uint64_t>(
        static_cast<std::uint64_t>(smoothed_rtt_ms) * 2ULL * 1000ULL + 500000ULL,
        750000ULL,
        5000000ULL);
}

inline void assign_error(std::string detail, std::string* error_detail) {
    if (error_detail != nullptr) {
        *error_detail = std::move(detail);
    }
}
}  // namespace redclaw::net::transport_detail
