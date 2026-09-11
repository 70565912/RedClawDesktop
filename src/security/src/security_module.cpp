#include "redclaw/security/security_module.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <utility>

namespace redclaw::security {

namespace {

std::uint64_t default_now_ms() {
    return 0;
}

HandshakeReplayCheckResult make_result(
    bool accepted,
    HandshakeReplayDecision decision,
    std::string error = {}) {
    HandshakeReplayCheckResult result;
    result.accepted = accepted;
    result.decision = decision;
    result.error = std::move(error);
    return result;
}

}

InMemoryHandshakeReplayGuard::InMemoryHandshakeReplayGuard(
    HandshakeReplayGuardConfig config,
    NowProvider now_provider)
    : config_(config),
      now_provider_(std::move(now_provider)) {
    if (!now_provider_) {
        now_provider_ = default_now_ms;
    }
}

HandshakeReplayCheckResult InMemoryHandshakeReplayGuard::check(const HandshakeReplayCheckRequest& request) {
    if (request.session_id.empty() || request.nonce.empty() || request.created_at_ms == 0) {
        return make_result(false, HandshakeReplayDecision::rejected_invalid_input, "session_id, nonce, and created_at_ms are required");
    }
    if (config_.max_age_ms == 0 || config_.max_nonces_per_session == 0) {
        return make_result(false, HandshakeReplayDecision::rejected_invalid_input, "replay guard config is invalid");
    }

    const std::uint64_t now_ms = now_provider_();
    if (request.created_at_ms > now_ms + config_.max_future_skew_ms) {
        return make_result(false, HandshakeReplayDecision::rejected_future_timestamp, "handshake timestamp is too far in the future");
    }
    if (now_ms > request.created_at_ms && (now_ms - request.created_at_ms) > config_.max_age_ms) {
        return make_result(false, HandshakeReplayDecision::rejected_expired_timestamp, "handshake timestamp is expired");
    }

    auto& cache = caches_[request.session_id];
    prune_expired_locked(cache, now_ms);

    if (cache.seen_nonces.contains(request.nonce)) {
        return make_result(false, HandshakeReplayDecision::rejected_duplicate_nonce, "nonce replay detected for session");
    }

    cache.arrival_order.emplace_back(request.nonce, request.created_at_ms);
    cache.seen_nonces.insert(request.nonce);

    while (cache.arrival_order.size() > config_.max_nonces_per_session) {
        const auto evicted = cache.arrival_order.front().first;
        cache.arrival_order.pop_front();
        cache.seen_nonces.erase(evicted);
    }

    return make_result(true, HandshakeReplayDecision::accepted);
}

void InMemoryHandshakeReplayGuard::reset_session(std::string_view session_id) {
    if (session_id.empty()) {
        return;
    }
    caches_.erase(std::string(session_id));
}

void InMemoryHandshakeReplayGuard::prune_expired_locked(SessionCache& cache, std::uint64_t now_ms) {
    while (!cache.arrival_order.empty()) {
        const auto& entry = cache.arrival_order.front();
        const std::uint64_t created_at_ms = entry.second;
        if (now_ms <= created_at_ms || (now_ms - created_at_ms) <= config_.max_age_ms) {
            break;
        }
        cache.seen_nonces.erase(entry.first);
        cache.arrival_order.pop_front();
    }
}

InMemoryPeerFingerprintVerifier::InMemoryPeerFingerprintVerifier(const std::vector<std::string>& trusted_fingerprints) {
    for (const auto& fingerprint : trusted_fingerprints) {
        trust_fingerprint(fingerprint);
    }
}

PeerFingerprintCheckResult InMemoryPeerFingerprintVerifier::verify(std::string_view fingerprint) const {
    PeerFingerprintCheckResult result;
    const std::string normalized = normalize_fingerprint_for_policy(fingerprint);

    if (!is_well_formed_fingerprint(normalized)) {
        result.accepted = false;
        result.decision = PeerFingerprintDecision::rejected_invalid_input;
        result.error = "fingerprint format is invalid";
        return result;
    }

    if (!trusted_fingerprints_.contains(normalized)) {
        result.accepted = false;
        result.decision = PeerFingerprintDecision::rejected_untrusted;
        result.error = "fingerprint is not trusted";
        return result;
    }

    result.accepted = true;
    result.decision = PeerFingerprintDecision::accepted;
    return result;
}

void InMemoryPeerFingerprintVerifier::trust_fingerprint(std::string_view fingerprint) {
    const std::string normalized = normalize_fingerprint_for_policy(fingerprint);
    if (!is_well_formed_fingerprint(normalized)) {
        return;
    }
    trusted_fingerprints_.insert(normalized);
}

void InMemoryPeerFingerprintVerifier::revoke_fingerprint(std::string_view fingerprint) {
    const std::string normalized = normalize_fingerprint_for_policy(fingerprint);
    trusted_fingerprints_.erase(normalized);
}

void InMemoryPeerFingerprintVerifier::set_trusted_fingerprints(const std::vector<std::string>& fingerprints) {
    trusted_fingerprints_.clear();
    for (const auto& fingerprint : fingerprints) {
        trust_fingerprint(fingerprint);
    }
}

std::vector<std::string> InMemoryPeerFingerprintVerifier::trusted_fingerprints() const {
    std::vector<std::string> out;
    out.reserve(trusted_fingerprints_.size());
    for (const auto& fingerprint : trusted_fingerprints_) {
        out.push_back(fingerprint);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string InMemoryPeerFingerprintVerifier::normalize_fingerprint_for_policy(std::string_view fingerprint) {
    std::size_t start = 0;
    std::size_t end = fingerprint.size();
    while (start < end && std::isspace(static_cast<unsigned char>(fingerprint[start])) != 0) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(fingerprint[end - 1])) != 0) {
        --end;
    }

    std::string normalized;
    normalized.reserve(end - start);
    for (std::size_t i = start; i < end; ++i) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(fingerprint[i]))));
    }
    return normalized;
}

bool InMemoryPeerFingerprintVerifier::is_well_formed_fingerprint(std::string_view fingerprint) {
    constexpr std::string_view kSha256Prefix = "sha256:";
    if (fingerprint.size() <= kSha256Prefix.size()) {
        return false;
    }
    if (!fingerprint.starts_with(kSha256Prefix)) {
        return false;
    }

    for (std::size_t i = kSha256Prefix.size(); i < fingerprint.size(); ++i) {
        const char ch = fingerprint[i];
        const bool is_hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!is_hex && ch != ':' && ch != '=') {
            return false;
        }
    }
    return true;
}

FileBackedPeerFingerprintStore::FileBackedPeerFingerprintStore(std::string file_path)
    : file_path_(std::move(file_path)) {}

bool FileBackedPeerFingerprintStore::load(std::vector<std::string>* fingerprints, std::string* error) const {
    if (fingerprints == nullptr) {
        if (error != nullptr) {
            *error = "fingerprints output pointer is required";
        }
        return false;
    }

    std::ifstream in(file_path_);
    if (!in.is_open()) {
        if (error != nullptr) {
            *error = "failed to open trust store file for read";
        }
        return false;
    }

    std::vector<std::string> loaded;
    std::string line;
    int line_no = 0;

    while (std::getline(in, line)) {
        ++line_no;

        const std::string normalized = InMemoryPeerFingerprintVerifier::normalize_fingerprint_for_policy(line);
        if (normalized.empty() || normalized.starts_with("#")) {
            continue;
        }
        if (!InMemoryPeerFingerprintVerifier::is_well_formed_fingerprint(normalized)) {
            if (error != nullptr) {
                *error = "invalid fingerprint at line " + std::to_string(line_no);
            }
            return false;
        }
        loaded.push_back(normalized);
    }

    std::sort(loaded.begin(), loaded.end());
    loaded.erase(std::unique(loaded.begin(), loaded.end()), loaded.end());
    *fingerprints = std::move(loaded);
    return true;
}

bool FileBackedPeerFingerprintStore::save(const std::vector<std::string>& fingerprints, std::string* error) const {
    std::vector<std::string> normalized;
    normalized.reserve(fingerprints.size());

    for (const auto& fingerprint : fingerprints) {
        const std::string value = InMemoryPeerFingerprintVerifier::normalize_fingerprint_for_policy(fingerprint);
        if (!InMemoryPeerFingerprintVerifier::is_well_formed_fingerprint(value)) {
            if (error != nullptr) {
                *error = "invalid fingerprint in save set";
            }
            return false;
        }
        normalized.push_back(value);
    }

    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());

    std::ofstream out(file_path_, std::ios::trunc);
    if (!out.is_open()) {
        if (error != nullptr) {
            *error = "failed to open trust store file for write";
        }
        return false;
    }

    for (const auto& fingerprint : normalized) {
        out << fingerprint << '\n';
    }
    return true;
}

std::string_view module_name() {
    return "security";
}
}
