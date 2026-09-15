#include "redclaw/workspace/transfer_worker.h"
#include "redclaw/workspace/clipboard_copy_store.h"
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace redclaw::workspace {
namespace {
constexpr std::size_t kInboxFrames = 32; // exceeds the sender's bounded 1 MiB byte window
constexpr std::size_t kOutboxFrames = 8;
constexpr std::size_t kMaxWireBytes = 64U * 1024U;
bool clipboard_entry(const protocol::TransferMessageV1& entry) {
    const auto& path = entry.relative_path;
    if (path == "metadata") return entry.directory;
    if (path == "metadata/manifest.pb") return !entry.directory;
    for (int kind = 1; kind <= 6; ++kind)
        if (path == "metadata/format-" + std::to_string(kind) + ".bin") return !entry.directory;
    return path == "files" ? entry.directory : path.starts_with("files/");
}
}
TransferWorker::TransferWorker(TransferWorkerConfig config) : config_(std::move(config)), thread_([this] { run(); }) {}
TransferWorker::~TransferWorker() { cancel(); if (thread_.joinable()) thread_.join(); }
void TransferWorker::cancel() noexcept {
    cancelled_.store(true, std::memory_order_release);
    sender_.request_cancel(); receiver_.request_cancel();
    wake_.notify_one();
#ifdef _WIN32
    // The cancellation flag prevents new work. Interrupt any current Windows
    // read/write/directory operation without terminating the thread or process.
    if (thread_.joinable()) (void)CancelSynchronousIo(thread_.native_handle());
#endif
}
bool TransferWorker::command(Command value) {
    std::lock_guard lock(mutex_);
    if (cancelled_.load() || progress_.finished() || command_ || selection_pending_) return false;
    if (value.type == CommandType::kPasteComplete) {
        if (progress_.state != TransferWorkerState::kAwaitingPaste || clipboard_payload_) return false;
    } else if (value.type == CommandType::kStart) {
        if (progress_.state != TransferWorkerState::kPrepared) return false;
    } else if (progress_.state != TransferWorkerState::kSelecting) return false;
    selection_pending_ = true;
    command_ = std::move(value); ++generation_; wake_.notify_one(); return true;
}
bool TransferWorker::select_source(std::filesystem::path path, std::string relative_root) {
    return command({CommandType::kSelect, std::move(path), std::move(relative_root)});
}
bool TransferWorker::finish_selection() { return command({CommandType::kSelectionComplete, {}}); }
bool TransferWorker::start_sending() { return command({CommandType::kStart, {}}); }
bool TransferWorker::complete_clipboard() { return command({CommandType::kPasteComplete, {}}); }
std::unique_ptr<PreparedClipboardPayload> TransferWorker::take_prepared_clipboard() {
    std::lock_guard lock(mutex_);
    if (progress_.state != TransferWorkerState::kAwaitingPaste || cancelled_.load()) return {};
    return std::move(clipboard_payload_);
}
bool TransferWorker::receive(std::string& frame) {
    if (frame.empty() || frame.size() > kMaxWireBytes) return false;
    std::lock_guard lock(mutex_);
    if (cancelled_.load() || progress_.finished() || incoming_.size() >= kInboxFrames) return false;
    incoming_.push_back(std::move(frame)); ++generation_; wake_.notify_one(); return true;
}
std::optional<std::string> TransferWorker::take_output() {
    std::lock_guard lock(mutex_);
    if (cancelled_.load() || outgoing_.empty()) return {};
    auto result = std::move(outgoing_.front()); outgoing_.pop_front(); ++generation_; wake_.notify_one(); return result;
}
bool TransferWorker::output_empty() const { std::lock_guard lock(mutex_); return outgoing_.empty(); }
TransferWorkerProgress TransferWorker::progress() const { std::lock_guard lock(mutex_); return progress_; }
bool TransferWorker::emit(const protocol::TransferMessageV1& message) {
    {
        std::lock_guard lock(mutex_);
        if (cancelled_.load() || outgoing_.size() >= kOutboxFrames) return false;
    }
    auto frame = protocol::serialize_transfer_message_v1(message);
    if (frame.empty()) { encoding_error_ = "transfer_encoding_failed"; return false; }
    std::lock_guard lock(mutex_);
    if (cancelled_.load()) return false;
    outgoing_.push_back(std::move(frame)); return true;
}
void TransferWorker::fail(std::string error) {
    sender_.request_cancel(); sender_.pump({}); receiver_.request_cancel(); receiver_.finish_cancel();
    results_.finish(); publish();
    std::lock_guard lock(mutex_);
    progress_.state = cancelled_.load() ? TransferWorkerState::kCancelled : TransferWorkerState::kFailed;
    progress_.error = cancelled_.load() ? "transfer_cancelled" : std::move(error);
    incoming_.clear(); outgoing_.clear(); command_.reset(); selection_pending_ = false;
}
void TransferWorker::publish() {
    TransferWorkerProgress value;
    value.accepted_sources = accepted_sources_;
    if (config_.source) {
        const auto& source = sender_.progress();
        value.totals = source.totals; value.completed_entries = source.completed_entries;
        value.transferred_bytes = source.acknowledged_bytes; value.committed_bytes = source.committed_bytes; value.error = source.error;
        value.current_path = source.current_path; value.completed_files = source.completed_files; value.skipped_entries = source.skipped_entries;
        switch (source.state) {
        case TransferSenderState::kIdle: value.state = TransferWorkerState::kStarting; break;
        case TransferSenderState::kScanning: value.state = TransferWorkerState::kSelecting; break;
        case TransferSenderState::kPrepared: value.state = TransferWorkerState::kPrepared; break;
        case TransferSenderState::kSending: value.state = TransferWorkerState::kTransferring; break;
        case TransferSenderState::kComplete: value.state = TransferWorkerState::kComplete; break;
        case TransferSenderState::kCancelled: value.state = TransferWorkerState::kCancelled; break;
        case TransferSenderState::kFailed: value.state = TransferWorkerState::kFailed; break;
        }
    } else {
        const auto& target = receiver_.progress();
        value.totals = config_.totals; value.completed_entries = target.completed_entries;
        value.transferred_bytes = target.received_bytes; value.committed_bytes = target.committed_bytes; value.error = target.error;
        value.current_path = target.current_path; value.completed_files = target.completed_files; value.skipped_entries = target.skipped_entries;
        switch (target.state) {
        case TransferBatchState::kIdle: value.state = TransferWorkerState::kStarting; break;
        case TransferBatchState::kReceiving: value.state = TransferWorkerState::kTransferring; break;
        case TransferBatchState::kComplete:
            value.state = config_.clipboard && !paste_complete_ ? TransferWorkerState::kAwaitingPaste : TransferWorkerState::kComplete;
            break;
        case TransferBatchState::kCancelled: value.state = TransferWorkerState::kCancelled; break;
        case TransferBatchState::kFailed: value.state = TransferWorkerState::kFailed; break;
        }
    }
    if (value.finished()) {
        snapshot_.reset(); // source handles are closed; cleanup stays on the disk owner
        results_.finish(); value.results_path = results_.path();
        if (clipboard_batch_created_ && !clipboard_files_seen_) {
            std::string error;
            const std::atomic_bool cleanup_cancelled{false};
            if (!ClipboardCopyStore(config_.results_directory).cleanup_batch(config_.clipboard_batch_id, cleanup_cancelled, {}, &error)
                && value.error.empty()) { value.state = TransferWorkerState::kFailed; value.error = std::move(error); }
            clipboard_batch_created_ = false;
        }
    }
    std::lock_guard lock(mutex_); progress_ = std::move(value); selection_pending_ = false;
}
void TransferWorker::run() {
    try {
        if (config_.copy_action != ClipboardCopyAction::kNone) { run_copy_action(); return; }
        if (config_.source) {
            if (!sender_.begin_prepare(config_.directory)) { publish(); return; }
            if (config_.clipboard) {
                std::string error;
                snapshot_ = std::make_unique<ClipboardSnapshot>();
                if (!snapshot_->capture(config_.directory, config_.clipboard_sequence, cancelled_, &error)) {
                    fail(std::move(error)); return;
                }
                const auto scan = [this](const TransferScanProgress& totals) {
                    std::lock_guard lock(mutex_);
                    progress_.state = TransferWorkerState::kScanning; progress_.totals = totals;
                    return !cancelled_.load();
                };
                if (!sender_.add_source(snapshot_->metadata_directory(), scan, "metadata")) { fail(sender_.progress().error); return; }
                for (const auto& file : snapshot_->files()) {
                    if (!sender_.add_source(file.source, scan, file.relative_root)) { fail(sender_.progress().error); return; }
                }
                if (!sender_.finish_prepare()) { fail(sender_.progress().error); return; }
                if (!results_.begin(config_.results_directory, &error)) { fail(std::move(error)); return; }
            }
        } else {
            std::string error;
            if (config_.clipboard) {
                ClipboardCopyStore copies(config_.results_directory);
                if (config_.directory != copies.batch_directory(config_.clipboard_batch_id)
                    || !copies.create_batch(config_.clipboard_batch_id, &error)) {
                    fail(error.empty() ? "clipboard_batch_destination_invalid" : std::move(error)); return;
                }
                clipboard_batch_created_ = true;
            }
            if (!results_.begin(config_.clipboard ? config_.directory : config_.results_directory, &error)) { fail(std::move(error)); return; }
            if (!receiver_.begin(config_.directory, config_.epoch, config_.operation,
                    config_.totals.entries, config_.totals.bytes, &error)) { fail(std::move(error)); return; }
        }
        publish();
        std::optional<protocol::TransferMessageV1> receipt;
        for (;;) {
            std::optional<Command> current;
            std::optional<std::string> wire;
            std::uint64_t observed;
            {
                std::lock_guard lock(mutex_);
                observed = generation_;
                current = std::exchange(command_, {});
                if (!receipt && !incoming_.empty()) { wire = std::move(incoming_.front()); incoming_.pop_front(); }
            }
            if (cancelled_.load()) { fail("transfer_cancelled"); return; }
            if (current) {
                bool ok = false;
                switch (current->type) {
                case CommandType::kSelect:
                    ok = sender_.add_source(current->path, [this](const auto& totals) {
                        std::lock_guard lock(mutex_);
                        progress_.state = TransferWorkerState::kScanning; progress_.totals = totals;
                        return !cancelled_.load();
                    }, current->relative_root);
                    if (ok) ++accepted_sources_;
                    break;
                case CommandType::kSelectionComplete: {
                    ok = sender_.finish_prepare();
                    // Create only after scanning, so selecting the spool tree
                    // cannot include this operation's live results file.
                    std::string error;
                    if (ok && !results_.begin(config_.results_directory, &error)) { fail(std::move(error)); return; }
                    break;
                }
                case CommandType::kStart:
                    ok = sender_.begin(config_.epoch, config_.operation, config_.conflict); source_started_ = ok; break;
                case CommandType::kPasteComplete:
                    paste_complete_ = true; ok = true; break;
                }
                publish(); if (!ok) return;
            }
            if (wire) {
                const auto parsed = protocol::parse_transfer_message_v1(*wire);
                if (!parsed.ok) { fail("transfer_invalid_frame"); return; }
                // Old operation frames can still drain after priority cancel.
                if (parsed.value.session_epoch == config_.epoch && parsed.value.operation_id == config_.operation) {
                    if (config_.clipboard && !config_.source && parsed.value.type == protocol::TransferMessageTypeV1::kEntry) {
                        if (!clipboard_entry(parsed.value)) { fail("clipboard_entry_namespace_invalid"); return; }
                        if (parsed.value.relative_path == "files" || parsed.value.relative_path.starts_with("files/")) clipboard_files_seen_ = true;
                    }
                    if (config_.source) {
                        if (!source_started_ || !sender_.receive(parsed.value)) { fail("transfer_invalid_receipt"); return; }
                    } else receipt = receiver_.accept(parsed.value);
                    const auto completed = config_.source ? sender_.progress().completed_entries : receiver_.progress().completed_entries;
                    if (completed != recorded_entries_) {
                        const auto& entry = config_.source ? sender_.last_result() : receiver_.last_result();
                        std::string error;
                        if (completed != recorded_entries_ + 1 || !entry || !results_.append(*entry, &error)) {
                            fail(error.empty() ? "transfer_results_missing_entry" : std::move(error)); return;
                        }
                        recorded_entries_ = completed;
                    }
                }
            }
            if (receipt && emit(*receipt)) receipt.reset();
            if (config_.source && source_started_) sender_.pump([this](const auto& message) { return emit(message); });
            if (!encoding_error_.empty()) { fail(std::move(encoding_error_)); return; }
            if (config_.clipboard && !config_.source && !awaiting_paste_
                && receiver_.progress().state == TransferBatchState::kComplete) {
                auto payload = std::make_unique<PreparedClipboardPayload>();
                std::string error;
                if (!payload->load(config_.directory, cancelled_, &error)) { fail(std::move(error)); return; }
                awaiting_paste_ = true;
                std::lock_guard lock(mutex_); clipboard_payload_ = std::move(payload);
            }
            publish();
            if (progress().finished() && !receipt) return;
            std::unique_lock lock(mutex_);
            // Incoming frames are consumed only when a pending receipt has
            // entered the bounded outbox. Writable notification wakes us.
            wake_.wait(lock, [&] {
                return cancelled_.load() || generation_ != observed || command_
                    || (!receipt && !incoming_.empty());
            });
        }
    } catch (const std::exception&) { fail("transfer_worker_exception"); }
}
void TransferWorker::run_copy_action() {
    { std::lock_guard lock(mutex_); progress_.state = TransferWorkerState::kTransferring; }
    ClipboardCopyStore copies(config_.results_directory);
    std::string error;
    bool ok = false;
    if (!cancelled_.load()) {
        if (config_.copy_action == ClipboardCopyAction::kCleanup) {
            ok = copies.cleanup_batch(config_.clipboard_batch_id, cancelled_, [this](const auto& count) {
                std::lock_guard lock(mutex_);
                progress_.totals.entries = progress_.completed_entries = count.files + count.directories;
                progress_.totals.files = progress_.completed_files = count.files;
            }, &error);
        } else ok = copies.open_batch(config_.clipboard_batch_id, &error);
    }
    std::lock_guard lock(mutex_);
    // Once ShellExecute accepted the folder open, cancellation cannot undo it.
    progress_.state = ok ? TransferWorkerState::kComplete : cancelled_.load() ? TransferWorkerState::kCancelled : TransferWorkerState::kFailed;
    progress_.error = ok ? std::string{} : cancelled_.load() ? "clipboard_copy_action_cancelled" : std::move(error);
}
}
