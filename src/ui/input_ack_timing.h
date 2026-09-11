#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>

namespace redclaw::ui {
enum class InputAckKind { kBatch, kEmptySync, kStateSync };

struct InputAckTiming {
    std::uint64_t sequence, sent_us, received_us, consumed_us;
    InputAckKind kind;
    bool valid() const {
        return sequence && sent_us && sent_us <= received_us && received_us <= consumed_us;
    }
};

// The capture owner keeps the same bounded pending-ACK lifetime. A cumulative
// status completes each pending sequence once; repeated status is not a new ACK.
class InputAckTracker final {
public:
    static constexpr std::size_t kCapacity = 256;
    bool remember(std::uint64_t sequence, std::uint64_t sent_us, InputAckKind kind) {
        if (pending_.size() == kCapacity) return false;
        pending_.push_back({sequence, sent_us, kind});
        return true;
    }
    template<class Consumer>
    std::size_t acknowledge(std::uint64_t sequence, std::uint64_t received_us,
                            std::uint64_t consumed_us, Consumer consume) {
        std::size_t count = 0;
        while (!pending_.empty() && pending_.front().sequence <= sequence) {
            const auto pending = pending_.front(); pending_.pop_front();
            consume(InputAckTiming{pending.sequence, pending.sent_us, received_us, consumed_us, pending.kind});
            ++count;
        }
        return count;
    }
    void clear() { pending_.clear(); }
private:
    struct Pending { std::uint64_t sequence, sent_us; InputAckKind kind; };
    std::deque<Pending> pending_;
};
}  // namespace redclaw::ui
