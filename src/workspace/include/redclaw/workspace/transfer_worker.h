#pragma once
#include "redclaw/workspace/transfer_batch_sender.h"
#include "redclaw/workspace/transfer_batch_receiver.h"
#include "redclaw/workspace/clipboard_payload.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace redclaw::workspace {
enum class ClipboardCopyAction { kNone, kCleanup, kOpen };
enum class TransferWorkerState { kStarting, kSelecting, kScanning, kPrepared, kTransferring, kAwaitingPaste, kComplete, kCancelled, kFailed };
struct TransferWorkerProgress {
    TransferWorkerState state = TransferWorkerState::kStarting;
    TransferScanProgress totals;
    std::uint64_t accepted_sources = 0, completed_entries = 0, transferred_bytes = 0, committed_bytes = 0;
    std::uint64_t completed_files = 0, skipped_entries = 0;
    std::string current_path;
    std::filesystem::path results_path;
    std::string error;
    [[nodiscard]] bool finished() const {
        return state == TransferWorkerState::kComplete || state == TransferWorkerState::kCancelled
            || state == TransferWorkerState::kFailed;
    }
};
struct TransferWorkerConfig {
    bool source = false;
    std::string epoch, operation;
    std::filesystem::path directory; // local spool directory or receiver destination
    TransferScanProgress totals;
    protocol::TransferConflictV1 conflict = protocol::TransferConflictV1::kKeepBoth;
    std::filesystem::path results_directory;
    bool clipboard = false;
    std::uint32_t clipboard_sequence = 0;
    ClipboardCopyAction copy_action = ClipboardCopyAction::kNone;
    std::string clipboard_batch_id;
};
// One operation owns one disk thread. Network callbacks enqueue bounded frames;
// the runtime owner drains output and samples progress. No file operation runs
// on a GUI, media, or network callback thread. Source selection is acknowledged
// one path at a time, independently of how large each selected directory is.
class TransferWorker final {
public:
    explicit TransferWorker(TransferWorkerConfig config);
    ~TransferWorker();
    TransferWorker(const TransferWorker&) = delete;
    TransferWorker& operator=(const TransferWorker&) = delete;
    bool select_source(std::filesystem::path path, std::string relative_root = {});
    bool finish_selection();
    bool start_sending();
    std::unique_ptr<PreparedClipboardPayload> take_prepared_clipboard();
    bool complete_clipboard();
    bool receive(std::string& frame); // moves on success; backpressure leaves the caller's frame intact
    std::optional<std::string> take_output();
    [[nodiscard]] bool output_empty() const;
    [[nodiscard]] TransferWorkerProgress progress() const;
    void cancel() noexcept;
private:
    enum class CommandType { kSelect, kSelectionComplete, kStart, kPasteComplete };
    struct Command { CommandType type; std::filesystem::path path; std::string relative_root; };
    bool command(Command value);
    void run();
    void run_copy_action();
    void publish();
    void fail(std::string error);
    bool emit(const protocol::TransferMessageV1& value);
    TransferWorkerConfig config_;
    TransferBatchSender sender_;
    TransferBatchReceiver receiver_;
    TransferResultJournal results_;
    std::unique_ptr<ClipboardSnapshot> snapshot_;
    std::unique_ptr<PreparedClipboardPayload> clipboard_payload_;
    bool awaiting_paste_ = false, paste_complete_ = false;
    bool clipboard_batch_created_ = false, clipboard_files_seen_ = false;
    std::uint64_t recorded_entries_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> incoming_, outgoing_;
    std::optional<Command> command_;
    TransferWorkerProgress progress_;
    std::uint64_t generation_ = 0, accepted_sources_ = 0;
    bool selection_pending_ = false, source_started_ = false;
    std::string encoding_error_;
    std::atomic_bool cancelled_{false};
    std::thread thread_;
};
}
