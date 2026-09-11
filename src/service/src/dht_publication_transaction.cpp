#include "redclaw/service/dht_publication_transaction.h"

#include <limits>

namespace redclaw::service {

DhtPublicationTransaction::DhtPublicationTransaction(std::uint64_t revision_floor)
    : high_water_(revision_floor) {}

bool DhtPublicationTransaction::alive(std::uint64_t expiry, std::uint64_t started,
    std::uint64_t ttl, std::uint64_t now_unix, std::uint64_t now_steady_ms) const {
    return expiry > now_unix && now_steady_ms >= started && now_steady_ms - started < ttl;
}

void DhtPublicationTransaction::retire_expired() {
    ++expired_total_;
    revision_ = 0;
    pending_ = false;
    key_.clear();
}

std::optional<std::uint64_t> DhtPublicationTransaction::prepare(
    const DhtSignalSnapshot& snapshot, bool refresh, std::uint64_t now_unix,
    std::uint64_t now_steady_ms, std::uint64_t ttl_seconds) {
    const auto key = make_dht_signal_publication_key(snapshot);
    if (revision_ && !alive(expires_at_unix_, started_ms_, ttl_ms_, now_unix, now_steady_ms)) {
        retire_expired();
    }
    if (revision_ && key == key_ && (pending_ || !refresh)) return revision_;
    if (high_water_ == std::numeric_limits<std::uint64_t>::max() || ttl_seconds == 0
        || ttl_seconds > std::numeric_limits<std::uint64_t>::max() / 1000
        || now_unix > std::numeric_limits<std::uint64_t>::max() - ttl_seconds) return std::nullopt;
    revision_ = ++high_water_;
    key_ = key;
    started_ms_ = now_steady_ms;
    ttl_ms_ = ttl_seconds * 1000;
    expires_at_unix_ = now_unix + ttl_seconds;
    pending_ = true;
    return revision_;
}

bool DhtPublicationTransaction::observe(std::uint64_t revision, const DhtPublishResult& result) {
    if (revision == 0 || revision != revision_) { ++late_result_total_; return false; }
    if (result.failure == DhtPublishFailure::kExpired) {
        retire_expired();
        return true;
    }
    if (result.expires_at_unix) expires_at_unix_ = result.expires_at_unix;
    if (result.state == DhtPublishState::kSucceeded) {
        pending_ = false;
        success_expiry_ = expires_at_unix_;
        success_started_ms_ = started_ms_;
        success_ttl_ms_ = ttl_ms_;
    }
    return true;
}

void DhtPublicationTransaction::reset() {
    key_.clear();
    revision_ = expires_at_unix_ = success_expiry_ = 0;
    pending_ = false;
}

bool DhtPublicationTransaction::standby_viable(std::uint64_t now_unix, std::uint64_t now_steady_ms) const {
    return alive(success_expiry_, success_started_ms_, success_ttl_ms_, now_unix, now_steady_ms)
        || (pending_ && revision_ && alive(expires_at_unix_, started_ms_, ttl_ms_, now_unix, now_steady_ms));
}

} // namespace redclaw::service
