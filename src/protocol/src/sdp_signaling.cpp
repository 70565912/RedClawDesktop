#include "redclaw/protocol/sdp_signaling.h"

#include <cctype>

namespace redclaw::protocol {
namespace {

std::string_view trim_ascii_whitespace(std::string_view value) {
    while (!value.empty()
        && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty()
        && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

bool starts_with_ascii_case_insensitive(
    std::string_view value,
    std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        const auto left = static_cast<unsigned char>(value[index]);
        const auto right = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

void assign_attribute_if_present(
    std::string_view line,
    std::string_view prefix,
    std::string* destination) {
    if (!destination->empty() || !starts_with_ascii_case_insensitive(line, prefix)) {
        return;
    }
    const std::string_view value = trim_ascii_whitespace(line.substr(prefix.size()));
    if (!value.empty()) {
        destination->assign(value);
    }
}

}  // namespace

bool SdpSignalingAttributes::complete() const noexcept {
    return !ice_ufrag.empty() && !ice_pwd.empty() && !fingerprint.empty();
}

std::string_view SdpSignalingAttributes::first_missing_reason() const noexcept {
    if (ice_ufrag.empty()) {
        return "missing_ice_ufrag";
    }
    if (ice_pwd.empty()) {
        return "missing_ice_pwd";
    }
    if (fingerprint.empty()) {
        return "missing_fingerprint";
    }
    return {};
}

SdpSignalingAttributes parse_sdp_signaling_attributes(std::string_view sdp) {
    SdpSignalingAttributes attributes;
    std::size_t begin = 0;
    while (begin < sdp.size()) {
        const std::size_t end = sdp.find('\n', begin);
        std::string_view line = sdp.substr(
            begin,
            end == std::string_view::npos ? sdp.size() - begin : end - begin);
        line = trim_ascii_whitespace(line);
        assign_attribute_if_present(line, "a=ice-ufrag:", &attributes.ice_ufrag);
        assign_attribute_if_present(line, "a=ice-pwd:", &attributes.ice_pwd);
        assign_attribute_if_present(line, "a=fingerprint:", &attributes.fingerprint);
        if (attributes.complete() || end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return attributes;
}

}  // namespace redclaw::protocol
