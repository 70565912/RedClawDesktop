#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace redclaw::protocol {

// Desktop HEVC access units are Annex B. Only IDR_W_RADL (19) and IDR_N_LP
// (20) reset the reference chain. Stop at the first VCL NAL, before scanning
// the slice payload on the per-frame path.
[[nodiscard]] inline bool hevc_annex_b_contains_idr(std::span<const std::uint8_t> payload) noexcept {
    for (std::size_t i = 0; i + 4 < payload.size(); ++i) {
        if (payload[i] != 0 || payload[i + 1] != 0 || payload[i + 2] != 1) continue;
        const auto header = payload[i + 3];
        const auto type = (header >> 1U) & 0x3fU;
        if (type < 32) {
            return (header & 0x80U) == 0 && (payload[i + 4] & 7U) != 0
                && (type == 19 || type == 20);
        }
        i += 4;
    }
    return false;
}

}  // namespace redclaw::protocol
