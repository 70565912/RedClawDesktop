#include "redclaw/workspace/transfer_operation_gate.h"
#include <algorithm>

namespace redclaw::workspace {
namespace {
bool token(std::string_view text) {
    return !text.empty() && text.size() <= 128 && std::all_of(text.begin(), text.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
            || ch == '_' || ch == '-' || ch == '.';
    });
}
}
bool TransferOperationGate::begin(std::string epoch, std::string operation) {
    if (blocks_mutation() || !token(epoch) || !token(operation)) return false;
    epoch_ = std::move(epoch); operation_ = std::move(operation);
    local_finished_ = peer_finished_ = completion_sent_ = false;
    phase_ = TransferGatePhase::kPreparing;
    ++revision_;
    busy_.store(true, std::memory_order_release);
    return true;
}
bool TransferOperationGate::matches(std::string_view epoch, std::string_view operation) const {
    return blocks_mutation() && epoch == epoch_ && operation == operation_;
}
bool TransferOperationGate::start_transfer(std::string_view epoch, std::string_view operation) {
    if (!matches(epoch, operation) || phase_ != TransferGatePhase::kPreparing) return false;
    phase_ = TransferGatePhase::kTransferring; return true;
}
bool TransferOperationGate::cancel(std::string_view epoch, std::string_view operation) {
    if (!matches(epoch, operation)) return false;
    phase_ = TransferGatePhase::kCancelling; return true;
}
void TransferOperationGate::settle() {
    if (!local_finished_ || !peer_finished_ || !completion_sent_) return;
    epoch_.clear(); operation_.clear(); phase_ = TransferGatePhase::kIdle;
    busy_.store(false, std::memory_order_release);
}
bool TransferOperationGate::local_finished(std::string_view epoch, std::string_view operation) {
    if (!matches(epoch, operation)) return false;
    local_finished_ = true;
    if (phase_ != TransferGatePhase::kCancelling) phase_ = TransferGatePhase::kFinishing;
    settle(); return true;
}
bool TransferOperationGate::peer_finished(std::string_view epoch, std::string_view operation) {
    if (!matches(epoch, operation)) return false;
    peer_finished_ = true; settle(); return true;
}
bool TransferOperationGate::completion_sent(std::string_view epoch, std::string_view operation) {
    if (!matches(epoch, operation)) return false;
    completion_sent_ = true; settle(); return true;
}
void TransferOperationGate::disconnected() {
    if (!blocks_mutation()) return;
    phase_ = TransferGatePhase::kCancelling;
    peer_finished_ = completion_sent_ = true;
    settle(); // still waits for the local disk worker to release its handles
}
}
