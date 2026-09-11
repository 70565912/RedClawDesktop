#pragma once

#include "redclaw/service/dht_rendezvous.h"
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

namespace redclaw::service::detail {

struct DhtPublishLease {
    DhtPublishResult result{DhtPublishState::kPending};
    bool submit = false;
    std::uint64_t attempt = 0;
};

// The native signing callback owns only this bounded registry, never the store
// or runtime. No payload or routing secret is retained here.
class DhtPublishRegistry final {
    struct Entry {
        std::string public_key, salt, completion_key;
        std::uint64_t attempt = 0, enqueued_ms = 0, completed_ms = 0, expires_ms = 0;
        DhtPublishState state = DhtPublishState::kPending;
        std::uint64_t retry_at_ms = 0;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> records_;
    std::unordered_map<std::string, std::string> completions_;
    std::uint64_t next_attempt_ = 0, late_events_ = 0, backpressure_ = 0;
    std::size_t peak_ = 0;
    static std::string completion_key(const std::string& public_key, const std::string& salt, std::int64_t sequence) {
        return public_key + ":" + std::to_string(salt.size()) + ":" + salt + ":" + std::to_string(sequence);
    }
    void remove_completion(const Entry& entry) {
        if (!entry.completion_key.empty()) completions_.erase(entry.completion_key);
    }
    void prune_locked(std::uint64_t now) {
        for (auto it = records_.begin(); it != records_.end();) {
            auto& entry = it->second;
            if (entry.state == DhtPublishState::kPending && now >= entry.retry_at_ms) {
                remove_completion(entry);
                entry.completion_key.clear();
                entry.state = DhtPublishState::kRetryableFailure;
                entry.completed_ms = now;
            }
            if (now >= entry.expires_ms || (entry.state != DhtPublishState::kPending
                    && now >= entry.completed_ms && now - entry.completed_ms >= 60000)) {
                remove_completion(entry);
                it = records_.erase(it);
            } else ++it;
        }
    }
public:
    struct Stats { std::size_t records = 0, in_flight = 0, peak = 0; std::uint64_t late_events = 0, backpressure = 0; };
    DhtPublishLease begin(const std::string& key, const std::string& public_key, const std::string& salt,
                         std::uint64_t now, std::uint64_t expires, std::uint64_t retry_ms,
                         std::uint64_t republish_ms) {
        std::lock_guard lock(mutex_);
        prune_locked(now);
        if (expires <= now) return {DhtPublishState::kPermanentFailure};
        auto existing = records_.find(key);
        if (existing != records_.end()) {
            auto& entry = existing->second;
            if (entry.state == DhtPublishState::kSucceeded && now - entry.completed_ms < republish_ms) {
                return {DhtPublishState::kSucceeded};
            }
            if (entry.state == DhtPublishState::kPending && now - entry.enqueued_ms < retry_ms) {
                return {DhtPublishState::kPending};
            }
        }
        const auto active = std::count_if(records_.begin(), records_.end(), [&](const auto& pair) {
            return pair.first != key && pair.second.state == DhtPublishState::kPending;
        });
        if (active >= 64) { ++backpressure_; return {DhtPublishState::kRetryableFailure}; }
        if (existing == records_.end() && records_.size() >= 256) {
            auto oldest = records_.end();
            for (auto it = records_.begin(); it != records_.end(); ++it) {
                if (it->second.state == DhtPublishState::kPending) continue;
                if (oldest == records_.end() || it->second.completed_ms < oldest->second.completed_ms) oldest = it;
            }
            if (oldest == records_.end()) { ++backpressure_; return {DhtPublishState::kRetryableFailure}; }
            remove_completion(oldest->second);
            records_.erase(oldest);
        }
        auto& entry = records_[key];
        remove_completion(entry);
        entry = {public_key, salt, {}, ++next_attempt_, now, 0, expires, DhtPublishState::kPending};
        entry.retry_at_ms = now + std::max<std::uint64_t>(retry_ms, 1);
        peak_ = std::max(peak_, records_.size());
        return {DhtPublishState::kPending, true, entry.attempt};
    }
    bool signed_sequence(const std::string& key, std::uint64_t attempt, std::int64_t sequence) {
        std::lock_guard lock(mutex_);
        const auto it = records_.find(key);
        if (it == records_.end() || it->second.attempt != attempt || it->second.state != DhtPublishState::kPending) {
            ++late_events_; return false;
        }
        auto& entry = it->second;
        remove_completion(entry);
        entry.completion_key = completion_key(entry.public_key, entry.salt, sequence);
        completions_[entry.completion_key] = key;
        return true;
    }
    bool complete(const std::string& public_key, const std::string& salt, std::int64_t sequence,
                  bool success, std::uint64_t now) {
        std::lock_guard lock(mutex_);
        prune_locked(now);
        const auto index = completions_.find(completion_key(public_key, salt, sequence));
        if (index == completions_.end()) { ++late_events_; return false; }
        auto& entry = records_.at(index->second);
        if (entry.state != DhtPublishState::kPending) { ++late_events_; return false; }
        entry.state = success ? DhtPublishState::kSucceeded : DhtPublishState::kRetryableFailure;
        entry.completed_ms = now;
        completions_.erase(index);
        entry.completion_key.clear();
        return true;
    }
    [[nodiscard]] Stats stats() const {
        std::lock_guard lock(mutex_);
        return {records_.size(), static_cast<std::size_t>(std::count_if(records_.begin(), records_.end(),
            [](const auto& pair) { return pair.second.state == DhtPublishState::kPending; })), peak_, late_events_, backpressure_};
    }
};
} // namespace redclaw::service::detail
