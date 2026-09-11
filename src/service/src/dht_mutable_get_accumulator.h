#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "redclaw/service/dht_rendezvous.h"

namespace redclaw::service::detail {

class DhtMutableGetAccumulator final {
public:
    void observe(
        std::int64_t sequence,
        bool authoritative,
        std::optional<DhtEncryptedRecord> record) {
        ++response_count_;
        if (authoritative) {
            authoritative_received_ = true;
            authoritative_sequence_ = sequence;
            authoritative_record_ = record;
            latest_verified_sequence_ = sequence;
            latest_verified_record_ = std::move(record);
        } else {
            ++non_authoritative_count_;
            if (record.has_value()
                && (!latest_verified_sequence_.has_value()
                    || sequence >= *latest_verified_sequence_)) {
                latest_verified_sequence_ = sequence;
                latest_verified_record_ = std::move(record);
            }
        }
        if (!highest_observed_sequence_.has_value() || sequence >= *highest_observed_sequence_) {
            highest_observed_sequence_ = sequence;
        }
    }

    [[nodiscard]] bool authoritative_received() const {
        return authoritative_received_;
    }

    [[nodiscard]] const std::optional<DhtEncryptedRecord>& authoritative_record() const {
        return authoritative_record_;
    }

    [[nodiscard]] std::optional<std::int64_t> authoritative_sequence() const {
        return authoritative_sequence_;
    }

    // libtorrent only emits mutable item callbacks after the BEP 44 key and
    // signature checks pass. Intermediate callbacks are not final, but they
    // are the highest verified sequence observed so far and are deliberately
    // exposed by libtorrent so callers do not have to wait for the traversal
    // timeout before using the value.
    [[nodiscard]] const std::optional<DhtEncryptedRecord>& latest_verified_record() const {
        return latest_verified_record_;
    }

    [[nodiscard]] std::optional<std::int64_t> latest_verified_sequence() const {
        return latest_verified_sequence_;
    }

    [[nodiscard]] std::optional<std::int64_t> highest_observed_sequence() const {
        return highest_observed_sequence_;
    }

    [[nodiscard]] std::size_t response_count() const {
        return response_count_;
    }

    [[nodiscard]] std::size_t non_authoritative_count() const {
        return non_authoritative_count_;
    }

private:
    bool authoritative_received_ = false;
    std::optional<std::int64_t> authoritative_sequence_;
    std::optional<std::int64_t> latest_verified_sequence_;
    std::optional<std::int64_t> highest_observed_sequence_;
    std::optional<DhtEncryptedRecord> authoritative_record_;
    std::optional<DhtEncryptedRecord> latest_verified_record_;
    std::size_t response_count_ = 0;
    std::size_t non_authoritative_count_ = 0;
};

}  // namespace redclaw::service::detail
