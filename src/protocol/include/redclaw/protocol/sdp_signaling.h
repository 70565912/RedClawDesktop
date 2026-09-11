#pragma once

#include <string>
#include <string_view>

namespace redclaw::protocol {

struct SdpSignalingAttributes {
    std::string ice_ufrag;
    std::string ice_pwd;
    std::string fingerprint;

    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] std::string_view first_missing_reason() const noexcept;
};

[[nodiscard]] SdpSignalingAttributes parse_sdp_signaling_attributes(
    std::string_view sdp);

}  // namespace redclaw::protocol
