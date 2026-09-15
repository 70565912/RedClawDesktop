#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace redclaw::workspace {
enum class TransferGatePhase { kIdle, kPreparing, kTransferring, kCancelling, kFinishing };
// The runtime owner updates this single transfer reason. Existing capture,
// consent, geometry and lifecycle gates remain independently authoritative.
// Agent workers may read blocks_mutation without acquiring runtime locks.
class TransferOperationGate final {
public:
    bool begin(std::string epoch, std::string operation);
    bool start_transfer(std::string_view epoch, std::string_view operation);
    bool cancel(std::string_view epoch, std::string_view operation);
    bool local_finished(std::string_view epoch, std::string_view operation);
    bool peer_finished(std::string_view epoch, std::string_view operation);
    bool completion_sent(std::string_view epoch, std::string_view operation);
    void disconnected();
    [[nodiscard]] bool blocks_mutation() const noexcept { return busy_.load(std::memory_order_acquire); }
    [[nodiscard]] TransferGatePhase phase() const { return phase_; }
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
private:
    bool matches(std::string_view epoch, std::string_view operation) const;
    void settle();
    std::atomic_bool busy_{false};
    std::string epoch_, operation_;
    TransferGatePhase phase_ = TransferGatePhase::kIdle;
    bool local_finished_ = false, peer_finished_ = false, completion_sent_ = false;
    std::uint64_t revision_ = 0;
};
}
