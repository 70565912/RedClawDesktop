#include "redclaw/net/ice_candidate_diagnostics.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace redclaw::net {
namespace {
std::string description_key(std::string_view sdp) {
    if (sdp.size() > 65536) return {};
    std::string key;
    std::istringstream input{std::string(sdp)};
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.starts_with("a=ice-pwd:")) continue;
        const auto candidate = line.substr(10);
        if (candidate.size() < 22 || candidate.size() > 256
            || !std::all_of(candidate.begin(), candidate.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '+' || c == '/';
            }) || (!key.empty() && key != candidate)) return {};
        key = candidate;
    }
    return key;
}
bool hex_identity(std::string_view text) {
    return text.size() == 32 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
} // namespace

std::string IceCandidateObservation::diagnostic_reason() const {
    return std::string(local ? "candidate_local:" : "candidate_remote:") + identity + ":"
        + family + ":" + type + ":" + std::to_string(first_seen_steady_ms);
}

bool is_valid_candidate_diagnostic_reason(std::string_view reason) {
    if (reason.size() > 112) return false;
    std::array<std::string_view, 5> fields;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto end = reason.find(':');
        if ((i < 4) != (end != std::string_view::npos)) return false;
        fields[i] = reason.substr(0, end);
        if (i < 4) reason.remove_prefix(end + 1);
    }
    std::uint64_t timestamp = 0;
    const auto parsed = std::from_chars(fields[4].data(), fields[4].data() + fields[4].size(), timestamp);
    return (fields[0] == "candidate_local" || fields[0] == "candidate_remote")
        && hex_identity(fields[1]) && (fields[2] == "v4" || fields[2] == "v6")
        && (fields[3] == "host" || fields[3] == "srflx" || fields[3] == "relay" || fields[3] == "prflx")
        && parsed.ec == std::errc{} && parsed.ptr == fields[4].data() + fields[4].size();
}

std::vector<IceCandidateObservation> IceCandidateDiagnostics::consume(Pending pending) {
    const auto& key = pending.local ? local_key_ : remote_key_;
    if (key.empty() || seen_.size() >= kCapacity) return {};
    std::istringstream input(pending.candidate);
    std::array<std::string, 8> fields;
    for (auto& field : fields) if (!(input >> field)) return {};
    if ((!fields[0].starts_with("candidate:") && !fields[0].starts_with("a=candidate:"))
        || fields[1] != "1" || (fields[2] != "udp" && fields[2] != "UDP") || fields[6] != "typ"
        || (fields[7] != "host" && fields[7] != "srflx" && fields[7] != "relay" && fields[7] != "prflx")) return {};
    unsigned port = 0;
    const auto parsed = std::from_chars(fields[5].data(), fields[5].data() + fields[5].size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != fields[5].data() + fields[5].size() || !port || port > 65535) return {};
    std::array<unsigned char, 16> address{};
    const bool v4 = inet_pton(AF_INET, fields[4].c_str(), address.data()) == 1;
    if (!v4 && inet_pton(AF_INET6, fields[4].c_str(), address.data()) != 1) return {};
    std::string canonical = "redclaw-ice-candidate-v1|" + pending.mid + "|udp|" + fields[7] + (v4 ? "|v4|" : "|v6|");
    canonical.append(reinterpret_cast<const char*>(address.data()), v4 ? 4 : 16);
    canonical.push_back(static_cast<char>(port >> 8));
    canonical.push_back(static_cast<char>(port & 255));
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned length = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size(), digest.data(), &length)
        || length < 16) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string identity;
    for (std::size_t i = 0; i < 16; ++i) { identity += hex[digest[i] >> 4]; identity += hex[digest[i] & 15]; }
    OPENSSL_cleanse(digest.data(), digest.size());
    const auto dedup = std::string(pending.local ? "L" : "R") + identity;
    if (std::find(seen_.begin(), seen_.end(), dedup) != seen_.end()) return {};
    seen_.push_back(dedup);
    return {{pending.local, std::move(identity), v4 ? "v4" : "v6", fields[7], pending.steady_ms}};
}

std::vector<IceCandidateObservation> IceCandidateDiagnostics::set_description(bool local, std::string_view sdp) {
    auto& key = local ? local_key_ : remote_key_;
    auto parsed = description_key(sdp);
    if (!key.empty()) return {}; // no key switching within one native generation
    key = std::move(parsed);
    if (key.empty()) return {};
    std::vector<IceCandidateObservation> result;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->local != local) { ++it; continue; }
        auto rows = consume(std::move(*it));
        result.insert(result.end(), rows.begin(), rows.end());
        it = pending_.erase(it);
    }
    return result;
}

std::vector<IceCandidateObservation> IceCandidateDiagnostics::observe(
    bool local, std::string_view candidate, std::string_view mid, std::uint64_t steady_ms) {
    if (candidate.empty() || candidate.size() > 4096 || mid.empty() || mid.size() > 64
        || std::any_of(candidate.begin(), candidate.end(), [](unsigned char c) { return c < 32 || c > 126; })
        || std::any_of(mid.begin(), mid.end(), [](unsigned char c) { return c < 33 || c > 126 || c == '|'; })) return {};
    Pending row{local, std::string(candidate), std::string(mid), steady_ms};
    if ((local ? local_key_ : remote_key_).empty()) {
        if (pending_.size() < kCapacity) pending_.push_back(std::move(row));
        return {};
    }
    return consume(std::move(row));
}

void IceCandidateDiagnostics::reset() {
    OPENSSL_cleanse(local_key_.data(), local_key_.size());
    OPENSSL_cleanse(remote_key_.data(), remote_key_.size());
    local_key_.clear(); remote_key_.clear(); pending_.clear(); seen_.clear();
}
} // namespace redclaw::net
