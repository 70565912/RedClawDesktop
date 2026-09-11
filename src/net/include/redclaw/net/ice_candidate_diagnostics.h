#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace redclaw::net {

struct IceCandidateObservation {
    bool local = false;
    std::string identity;
    std::string family;
    std::string type;
    std::uint64_t first_seen_steady_ms = 0;
    [[nodiscard]] std::string diagnostic_reason() const;
};

// Owner-serialized, connection-generation scoped. Only the owning wrapper sees
// pending candidate text and private credentials. No raw tuple leaves this type.
class IceCandidateDiagnostics {
public:
    ~IceCandidateDiagnostics() { reset(); }
    static constexpr std::size_t kCapacity = 64;
    std::vector<IceCandidateObservation> set_description(bool local, std::string_view sdp);
    std::vector<IceCandidateObservation> observe(bool local, std::string_view candidate,
                                               std::string_view mid, std::uint64_t steady_ms);
    void reset();
private:
    struct Pending { bool local; std::string candidate, mid; std::uint64_t steady_ms; };
    std::string local_key_, remote_key_;
    std::vector<Pending> pending_;
    std::vector<std::string> seen_;
    std::vector<IceCandidateObservation> consume(Pending pending);
};

[[nodiscard]] bool is_valid_candidate_diagnostic_reason(std::string_view reason);
} // namespace redclaw::net
