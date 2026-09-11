#pragma once

#include "redclaw/service/dht_rendezvous.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace redclaw::service::detail {

// Internal codec seam used by deterministic compatibility tests. Runtime
// callers must continue through DhtRendezvousClient so encryption, expiry and
// lane validation remain mandatory.
[[nodiscard]] std::optional<DhtSignalSnapshot> parse_dht_signal_plaintext(
    std::string_view plaintext,
    std::uint64_t revision,
    std::uint64_t expires_at_unix,
    std::string* error_detail = nullptr);

}  // namespace redclaw::service::detail
