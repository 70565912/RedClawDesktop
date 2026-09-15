#pragma once
#include "redclaw/protocol/transfer_protocol.h"
#include "redclaw/workspace/transfer_file_receiver.h"
#include "redclaw/workspace/transfer_result_journal.h"
#include <atomic>
#include <optional>

namespace redclaw::workspace {
enum class TransferBatchState { kIdle, kReceiving, kComplete, kCancelled, kFailed };
struct TransferBatchProgress {
    TransferBatchState state = TransferBatchState::kIdle;
    std::uint64_t total_entries = 0, total_bytes = 0;
    std::uint64_t completed_entries = 0, skipped_entries = 0;
    std::uint64_t received_bytes = 0, committed_bytes = 0;
    std::uint64_t completed_files = 0;
    std::string current_path;
    std::string error;
};
// One disk worker owns a batch for its entire lifetime. request_cancel is the
// sole cross-thread operation: priority cancellation takes effect before the
// next disk command, including queued commits. A commit already in progress
// may finish; its completed file is retained. The owner clears the UI/input
// gate only after finish_cancel/accept returns the final worker state.
class TransferBatchReceiver final {
public:
    bool begin(const std::filesystem::path& destination, std::string epoch, std::string operation,
        std::uint64_t entries, std::uint64_t bytes, std::string* error = nullptr);
    std::optional<protocol::TransferMessageV1> accept(const protocol::TransferMessageV1& message);
    void request_cancel() noexcept;
    void finish_cancel();
    [[nodiscard]] const TransferBatchProgress& progress() const { return progress_; }
    [[nodiscard]] const std::optional<TransferResultEntry>& last_result() const { return last_result_; }
private:
    protocol::TransferMessageV1 receipt(const protocol::TransferMessageV1& message,
        protocol::TransferDispositionV1 disposition, std::string error = {});
    void fail(std::string error);
    TransferFileReceiver file_;
    TransferBatchProgress progress_;
    std::optional<TransferResultEntry> last_result_;
    std::atomic_bool cancelled_{false};
    std::string epoch_, operation_;
    std::uint64_t sequence_ = 0, entry_id_ = 0, declared_bytes_ = 0, file_size_ = 0;
};
}
