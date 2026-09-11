#pragma once

#include "redclaw/service/dht_rendezvous.h"

namespace redclaw::service {

// Runtime-owner only. No payload queue or worker: one publication identity,
// a monotonic revision high-water mark, and the last confirmed lease.
class DhtPublicationTransaction final {
public:
    explicit DhtPublicationTransaction(std::uint64_t revision_floor = 0);
    [[nodiscard]] std::optional<std::uint64_t> prepare(
        const DhtSignalSnapshot& snapshot, bool refresh,
        std::uint64_t now_unix, std::uint64_t now_steady_ms, std::uint64_t ttl_seconds);
    bool observe(std::uint64_t revision, const DhtPublishResult& result);
    // Retire old session/description work, but never reuse an allocated revision.
    void reset();
    [[nodiscard]] bool standby_viable(std::uint64_t now_unix, std::uint64_t now_steady_ms) const;
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
    [[nodiscard]] std::uint64_t expires_at_unix() const { return expires_at_unix_; }
    [[nodiscard]] std::uint64_t expired_total() const { return expired_total_; }
    [[nodiscard]] std::uint64_t late_result_total() const { return late_result_total_; }
private:
    [[nodiscard]] bool alive(std::uint64_t expiry, std::uint64_t started,
        std::uint64_t ttl, std::uint64_t now_unix, std::uint64_t now_steady_ms) const;
    void retire_expired();
    std::string key_;
    std::uint64_t high_water_ = 0, revision_ = 0, expires_at_unix_ = 0;
    std::uint64_t started_ms_ = 0, ttl_ms_ = 0;
    std::uint64_t success_expiry_ = 0, success_started_ms_ = 0, success_ttl_ms_ = 0;
    std::uint64_t expired_total_ = 0, late_result_total_ = 0;
    bool pending_ = false;
};

} // namespace redclaw::service
