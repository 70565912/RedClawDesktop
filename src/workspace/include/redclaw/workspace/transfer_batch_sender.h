#pragma once
#include "redclaw/protocol/transfer_protocol.h"
#include "redclaw/workspace/transfer_source_manifest.h"
#include "redclaw/workspace/transfer_file_source.h"
#include "redclaw/workspace/transfer_result_journal.h"
#include <optional>

namespace redclaw::workspace {
enum class TransferSenderState { kIdle, kScanning, kPrepared, kSending, kComplete, kCancelled, kFailed };
struct TransferSendProgress {
    TransferSenderState state = TransferSenderState::kIdle;
    TransferScanProgress totals;
    std::uint64_t submitted_bytes = 0, acknowledged_bytes = 0, committed_bytes = 0;
    std::uint64_t completed_entries = 0, skipped_entries = 0;
    std::uint64_t completed_files = 0;
    std::string current_path;
    std::string error;
};
// Worker-owned producer. Reliable transport owns accepted frames; this object
// retains only one unsent chunk and at most 1 MiB of unacknowledged file bytes.
// The send callback must not call receive reentrantly. Cancellation is reported
// to the priority coordinator; it does not enqueue an abort behind file bytes.
class TransferBatchSender final {
public:
    using Send = std::function<bool(const protocol::TransferMessageV1&)>;
    bool begin_prepare(const std::filesystem::path& spool_directory);
    bool add_source(const std::filesystem::path& source,
        const std::function<bool(const TransferScanProgress&)>& progress = {}, std::string_view relative_root = {});
    bool finish_prepare();
    bool prepare(std::span<const std::filesystem::path> selected, const std::filesystem::path& spool_directory,
        const std::function<bool(const TransferScanProgress&)>& progress = {});
    bool begin(std::string epoch, std::string operation, protocol::TransferConflictV1 conflict);
    void pump(const Send& send);
    bool receive(const protocol::TransferMessageV1& receipt);
    void request_cancel() noexcept;
    [[nodiscard]] const TransferSendProgress& progress() const { return progress_; }
    [[nodiscard]] const std::optional<TransferResultEntry>& last_result() const { return last_result_; }
    static constexpr std::uint64_t kMaxInFlightBytes = 1024U * 1024U;
private:
    enum class Phase { kEntry, kWaitEntry, kData, kWaitData, kCommit, kWaitCommit, kFinish, kWaitFinish };
    protocol::TransferMessageV1 message(protocol::TransferMessageTypeV1 type) const;
    void fail(std::string error);
    void cancel_if_requested();
    TransferSourceManifest manifest_;
    TransferFileSource source_;
    TransferSourceEntry entry_;
    TransferSendProgress progress_;
    std::optional<TransferResultEntry> last_result_;
    Phase phase_ = Phase::kEntry;
    std::atomic_bool cancelled_{false};
    std::string epoch_, operation_, hash_;
    protocol::TransferConflictV1 conflict_ = protocol::TransferConflictV1::kKeepBoth;
    std::optional<protocol::TransferMessageV1> pending_;
    std::uint64_t sequence_ = 0, acknowledged_sequence_ = 0, entry_id_ = 0;
    std::uint64_t sent_offset_ = 0, acknowledged_offset_ = 0, first_data_sequence_ = 0;
};
}
