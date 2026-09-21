#include "redclaw/workspace/transfer_runtime_bridge.h"
#include <algorithm>
#include "redclaw/workspace/clipboard_copy_store.h"

namespace redclaw::workspace {
namespace {
std::filesystem::path path_from_utf8(std::string_view value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
bool copy_purpose(protocol::WorkspaceTransferPurposeV1 purpose) {
    return purpose == protocol::WorkspaceTransferPurposeV1::kClipboardCleanup
        || purpose == protocol::WorkspaceTransferPurposeV1::kClipboardOpenCopy;
}
}
TransferRuntimeBridge::TransferRuntimeBridge(bool host, std::string epoch, std::filesystem::path spool,
    TransferOperationGate& gate, ControlSend peer, ControlSend gui, BulkSend bulk, std::function<bool()> ensure,
    ClipboardHostActions clipboard_actions)
    : host_role_(host), local_epoch_(std::move(epoch)), spool_directory_(std::move(spool)), gate_(gate),
      peer_send_(std::move(peer)), gui_send_(std::move(gui)), bulk_send_(std::move(bulk)), ensure_(std::move(ensure)),
      clipboard_paste_(clipboard_actions), clipboard_capture_(std::move(clipboard_actions.capture)) {}
void TransferRuntimeBridge::peer_capability(std::uint32_t version, std::string epoch, std::uint32_t clipboard_version) {
    std::lock_guard lock(inbox_mutex_);
    peer_version_ = kFileTransferCapabilityVersion && version >= 1 ? 1 : 0;
    peer_clipboard_version_ = peer_version_ ? std::min(kClipboardCapabilityVersion, clipboard_version) : 0;
    advertised_peer_epoch_ = std::move(epoch);
}
void TransferRuntimeBridge::channel_open(bool open) {
    std::lock_guard lock(inbox_mutex_);
    open_ = open;
    if (!open) { bulk_.clear(); ++invalidation_; }
}
bool TransferRuntimeBridge::receive_control(const Control& message) {
    if (message.type != protocol::StreamControlMessageTypeV1::kWorkspace || !message.workspace
        || message.workspace->action == Action::kAvailability || !message.workspace->results_path.empty()
        || message.workspace->clipboard_sequence || !message.workspace->clipboard_source.empty()
        || !message.workspace->snapshot_path.empty() || (host_role_ && message.workspace->paste_submitted)) return false;
    std::lock_guard lock(inbox_mutex_);
    if (!peer_version_ || overflow_ || message.session_epoch != advertised_peer_epoch_) return false;
    if (message.workspace->clipboard_mode && peer_clipboard_version_ < 3) return false;
    if (copy_purpose(message.workspace->purpose) && message.workspace->direction == protocol::TransferDirectionV1::kToController
        && peer_clipboard_version_ < 3) return false;
    if (controls_.size() >= 32) { overflow_ = true; return false; }
    controls_.push_back(message); return true;
}
bool TransferRuntimeBridge::receive_bulk(std::string_view frame) {
    if (frame.empty() || frame.size() > 64U * 1024U) return false;
    std::lock_guard lock(inbox_mutex_);
    if (!open_ || !peer_version_ || overflow_) return false;
    if (bulk_.size() >= 32) { overflow_ = true; return false; }
    bulk_.emplace_back(frame); return true;
}
TransferRuntimeBridge::Control TransferRuntimeBridge::make(Action action, std::string operation) const {
    Control result;
    result.type = protocol::StreamControlMessageTypeV1::kWorkspace;
    result.session_epoch = local_epoch_; result.request_id = operation.empty() ? operation_ : std::move(operation);
    if (result.request_id.empty()) result.request_id = "workspace-status";
    auto& data = result.workspace.emplace();
    data.action = action; data.direction = direction_; data.conflict = conflict_; data.purpose = purpose_;
    data.clipboard_mode = purpose_ == protocol::WorkspaceTransferPurposeV1::kClipboard ? clipboard_mode_ : 0;
    return result;
}
void TransferRuntimeBridge::local(Control message) { if (gui_send_) (void)gui_send_(message); }
void TransferRuntimeBridge::queue(Control message) {
    if (outgoing_.size() >= 16) { cancel("workspace_control_overflow", false); return; }
    outgoing_.push_back(std::move(message));
}
bool TransferRuntimeBridge::source() const {
    return !copy_action() && clipboard_mode_ < 4 && host_role_ == (direction_ == protocol::TransferDirectionV1::kToController);
}
bool TransferRuntimeBridge::matches(const Control& message) const {
    return !operation_.empty() && message.request_id == operation_ && message.workspace
        && message.workspace->direction == direction_ && message.workspace->purpose == purpose_
        && message.workspace->clipboard_mode == clipboard_mode_;
}
bool TransferRuntimeBridge::begin(const Control& message) {
    if (!ready_ || gate_.blocks_mutation() || !message.workspace || !operation_.empty()) return false;
    if (message.workspace->purpose == protocol::WorkspaceTransferPurposeV1::kClipboard && !clipboard_ready_) return false;
    if (copy_purpose(message.workspace->purpose) && clipboard_version_ < 2) return false;
    if (copy_purpose(message.workspace->purpose) && message.workspace->direction == protocol::TransferDirectionV1::kToController
        && clipboard_version_ < 3) return false;
    if (message.workspace->clipboard_mode && clipboard_version_ < 3) return false;
    operation_epoch_ = host_role_ ? local_epoch_ : peer_epoch_;
    if (!gate_.begin(operation_epoch_, message.request_id)) return false;
    operation_ = message.request_id; direction_ = message.workspace->direction; conflict_ = message.workspace->conflict;
    purpose_ = message.workspace->purpose; clipboard_sequence_ = message.workspace->clipboard_sequence;
    clipboard_mode_ = message.workspace->clipboard_mode; clipboard_source_ = message.workspace->clipboard_source;
    paste_submitted_ = paste_requested_ = false;
    cancelled_ = prepared_ = offered_ = transfer_ready_ = local_finished_ = completed_notification_queued_ = false;
    accepted_sources_ = next_progress_ms_ = 0; error_.clear(); destination_.clear(); clipboard_batch_id_.clear();
    try {
        if (copy_action() || (clipboard() && !source())) {
            clipboard_batch_id_ = copy_action() ? message.workspace->path : ClipboardCopyStore::new_id();
            destination_ = ClipboardCopyStore(spool_directory_).batch_directory(clipboard_batch_id_);
        } else if (!source()) destination_ = path_from_utf8(message.workspace->path);
    } catch (const std::exception&) {
        cancel("transfer_invalid_destination", host_role_);
        if (!host_role_) (void)gate_.peer_finished(operation_epoch_, operation_);
        return true;
    }
    if (!source() && !destination_.is_absolute()) {
        cancel("transfer_destination_not_absolute", host_role_);
        if (!host_role_) (void)gate_.peer_finished(operation_epoch_, operation_);
        return true;
    }
    if (clipboard() && host_role_ && (clipboard_mode_ == 0 || clipboard_mode_ == 3)) {
        std::string error;
        if (!clipboard_paste_.begin(operation_epoch_, operation_, &error)) { cancel(std::move(error), true); return true; }
    }
    update_availability();
    return true;
}
void TransferRuntimeBridge::create_source() {
    auto config = TransferWorkerConfig{true, operation_epoch_, operation_, spool_directory_, {}, conflict_, spool_directory_, clipboard(), clipboard_sequence_};
    config.clipboard_source = clipboard_source_;
    config.clipboard_capture = clipboard_capture_;
    worker_ = std::make_unique<TransferWorker>(std::move(config));
}
void TransferRuntimeBridge::create_receiver(const protocol::WorkspaceControlV1& offer) {
    auto config = TransferWorkerConfig{false, operation_epoch_, operation_, destination_,
        {offer.entries, offer.files, offer.bytes}, conflict_, spool_directory_, clipboard(), 0,
        ClipboardCopyAction::kNone, clipboard_batch_id_};
    config.retain_clipboard_snapshot = clipboard_mode_ != 0;
    worker_ = std::make_unique<TransferWorker>(std::move(config));
}
void TransferRuntimeBridge::create_copy_action() {
    TransferWorkerConfig config{false, operation_epoch_, operation_, destination_, {}, conflict_, spool_directory_};
    config.copy_action = purpose_ == protocol::WorkspaceTransferPurposeV1::kClipboardCleanup
        ? ClipboardCopyAction::kCleanup : ClipboardCopyAction::kOpen;
    config.clipboard_batch_id = clipboard_batch_id_;
    worker_ = std::make_unique<TransferWorker>(std::move(config));
}
void TransferRuntimeBridge::from_gui(const Control& message) {
    if (!message.workspace || message.type != protocol::StreamControlMessageTypeV1::kWorkspace
        || !message.workspace->results_path.empty() || message.workspace->paste_submitted
        || !message.workspace->snapshot_path.empty()) return;
    const auto& data = *message.workspace;
    if (data.action == Action::kCancel && matches(message)) { cancel("transfer_cancelled", true); return; }
    if (host_role_) return; // the connected Controller owns file selection
    if (data.action == Action::kBrowse || data.action == Action::kBrowseClipboardCopies) {
        if (data.action == Action::kBrowseClipboardCopies && data.direction == protocol::TransferDirectionV1::kToController
            && ready_ && clipboard_version_ >= 3) {
            browser_local_ = true; browse(message); return;
        }
        if (ready_ && (data.action != Action::kBrowseClipboardCopies || clipboard_version_ >= 2)) {
            auto request = make(data.action, message.request_id);
            request.workspace->purpose = protocol::WorkspaceTransferPurposeV1::kFiles;
            request.workspace->direction = protocol::TransferDirectionV1::kToHost;
            request.workspace->path = data.path;
            queue(std::move(request));
        } else {
            auto reply = make(Action::kBrowseEnd, message.request_id);
            reply.workspace->purpose = protocol::WorkspaceTransferPurposeV1::kFiles;
            reply.workspace->direction = protocol::TransferDirectionV1::kToHost;
            reply.workspace->error_code = "clipboard_peer_unsupported"; local(std::move(reply));
        }
        return;
    }
    if (data.action == Action::kPrepare) {
        if (!begin(message)) {
            auto rejected = make(Action::kError, message.request_id);
            rejected.workspace->purpose = data.purpose; rejected.workspace->direction = data.direction;
            rejected.workspace->error_code = (data.purpose == protocol::WorkspaceTransferPurposeV1::kClipboard && !clipboard_ready_)
                || (copy_purpose(data.purpose) && clipboard_version_ < 2) || (data.clipboard_mode && clipboard_version_ < 3)
                ? "clipboard_peer_unsupported" : ready_ ? "workspace_transfer_busy" : "workspace_peer_unavailable";
            local(std::move(rejected)); return;
        }
        if (cancelled_) return;
        auto request = make(Action::kPrepare);
        // A download destination is local and is never disclosed to the Host.
        if (direction_ == protocol::TransferDirectionV1::kToHost || copy_action()) request.workspace->path = data.path;
        queue(std::move(request));
        if ((copy_action() && direction_ == protocol::TransferDirectionV1::kToHost) || clipboard_mode_ == 3) finish_local(); // retain gate until Host receipt
        return;
    }
    if (!matches(message) || !prepared_ || cancelled_ || clipboard() || copy_action()) return;
    if (data.action != Action::kSelectSource && data.action != Action::kSelectionComplete) return;
    if (!source()) {
        auto request = make(data.action); request.workspace->path = data.path;
        queue(std::move(request)); return;
    }
    if (!worker_) return;
    try {
        const bool accepted = data.action == Action::kSelectSource
            ? worker_->select_source(path_from_utf8(data.path)) : worker_->finish_selection();
        if (!accepted) cancel("transfer_selection_out_of_order", true);
    } catch (const std::exception&) { cancel("transfer_invalid_source_path", true); }
}
void TransferRuntimeBridge::cancel(std::string error, bool notify_peer) {
    if (operation_.empty() || cancelled_) return;
    cancelled_ = true; error_ = std::move(error);
    clipboard_paste_.cancel();
    (void)gate_.cancel(operation_epoch_, operation_);
    if (worker_) worker_->cancel();
    if (local_clipboard_source_) local_clipboard_source_->cancel();
    pending_bulk_.reset(); outgoing_.clear(); progress_message_.reset(); completed_notification_queued_ = false;
    {
        std::lock_guard lock(inbox_mutex_); bulk_.clear();
    }
    if (notify_peer && ready_) {
        auto request = make(Action::kCancel); request.workspace->error_code = error_;
        queue(std::move(request));
    }
    auto notification = make(Action::kCancel); notification.workspace->error_code = error_;
    local(std::move(notification));
}
void TransferRuntimeBridge::disconnect() {
    cancel("workspace_connection_lost", false);
    outgoing_.clear(); progress_message_.reset(); pending_bulk_.reset();
    gate_.disconnected();
    if (browser_) browser_->cancel();
    pending_browse_.reset(); browse_reply_.reset();
}
void TransferRuntimeBridge::reset_transport(std::string epoch) {
    ready_ = false; disconnect(); local_epoch_ = std::move(epoch); peer_epoch_.clear(); availability_known_ = false;
    std::lock_guard lock(inbox_mutex_);
    controls_.clear(); bulk_.clear(); peer_version_ = peer_clipboard_version_ = 0;
    clipboard_ready_ = false; clipboard_version_ = 0; advertised_peer_epoch_.clear(); open_ = overflow_ = false;
    ++invalidation_; observed_invalidation_ = invalidation_; ensure_attempts_ = 0; next_ensure_ms_ = 0;
}
void TransferRuntimeBridge::peer_message(const Control& message) {
    if (!message.workspace || message.session_epoch != peer_epoch_) return;
    const auto& data = *message.workspace;
    if (host_role_ && (data.action == Action::kBrowse || data.action == Action::kBrowseClipboardCopies)) {
        if (data.action == Action::kBrowseClipboardCopies && clipboard_version_ < 2) {
            auto reply = make(Action::kBrowseEnd, message.request_id);
            reply.workspace->purpose = protocol::WorkspaceTransferPurposeV1::kFiles;
            reply.workspace->direction = protocol::TransferDirectionV1::kToHost;
            reply.workspace->error_code = "clipboard_peer_unsupported"; queue(std::move(reply));
        } else browse(message);
        return;
    }
    if (!host_role_ && (data.action == Action::kBrowseEntry || data.action == Action::kBrowseEnd)) {
        auto reply = make(data.action, message.request_id); *reply.workspace = data; local(std::move(reply)); return;
    }
    if (host_role_ && data.action == Action::kPrepare) {
        if (!begin(message)) {
            auto rejected = make(Action::kError, message.request_id);
            rejected.workspace->purpose = data.purpose; rejected.workspace->direction = data.direction;
            rejected.workspace->error_code = (data.purpose == protocol::WorkspaceTransferPurposeV1::kClipboard && !clipboard_ready_)
                || (copy_purpose(data.purpose) && clipboard_version_ < 2) || (data.clipboard_mode && clipboard_version_ < 3)
                ? "clipboard_peer_unsupported" : "workspace_transfer_busy"; queue(std::move(rejected)); return;
        }
        if (cancelled_) return;
        if (clipboard_mode_ >= 4 || (copy_action() && direction_ == protocol::TransferDirectionV1::kToController)) {
            queue(make(Action::kPrepared)); finish_local();
        }
        else if (clipboard_mode_ == 3) {
            std::string error;
            if (!clipboard_paste_.paste(&error)) cancel(std::move(error), true);
            else paste_submitted_ = true;
            finish_local();
        } else if (copy_action()) create_copy_action();
        else if (source()) create_source();
        else { prepared_ = true; queue(make(Action::kPrepared)); }
        return;
    }
    if (!matches(message)) return;
    if (data.action == Action::kCancel || data.action == Action::kError) {
        cancel(data.error_code.empty() ? "transfer_cancelled" : data.error_code, false);
        // Rejection means the peer never acquired this operation's gate.
        if (data.action == Action::kError) (void)gate_.peer_finished(operation_epoch_, operation_);
        return;
    }
    if (data.action == Action::kFinished) {
        (void)gate_.peer_finished(operation_epoch_, operation_);
        if (!data.error_code.empty()) cancel(data.error_code, false);
        if (clipboard() && !host_role_ && !cancelled_ && (clipboard_mode_ == 0 || clipboard_mode_ == 3)) {
            paste_submitted_ = data.paste_submitted;
            if (!paste_submitted_) cancel("clipboard_paste_not_confirmed", false);
        }
        return;
    }
    if (cancelled_) return;
    if (copy_action()) {
        if (!host_role_ && direction_ == protocol::TransferDirectionV1::kToController
            && data.action == Action::kPrepared && !worker_) { prepared_ = true; create_copy_action(); }
        if (!host_role_ && data.action == Action::kProgress) {
            auto update = make(Action::kProgress); *update.workspace = data; local(std::move(update));
        }
        return;
    }
    if (!host_role_ && data.action == Action::kPrepared && !prepared_ && !worker_) {
        if (clipboard_mode_ >= 4) {
            prepared_ = true;
            auto config = TransferWorkerConfig{true, operation_epoch_, operation_, spool_directory_, {}, conflict_, spool_directory_, true};
            config.clipboard_source = clipboard_source_;
            config.clipboard_capture = clipboard_capture_;
            local_clipboard_source_ = std::make_unique<TransferWorker>(std::move(config));
        } else if (source()) create_source();
        else { prepared_ = true; local(make(Action::kPrepared)); }
        return;
    }
    if (host_role_ && source() && prepared_
        && (data.action == Action::kSelectSource || data.action == Action::kSelectionComplete)) {
        try {
            const bool accepted = data.action == Action::kSelectSource
                ? worker_->select_source(path_from_utf8(data.path)) : worker_->finish_selection();
            if (!accepted) cancel("transfer_selection_out_of_order", true);
        } catch (const std::exception&) { cancel("transfer_invalid_source_path", true); }
        return;
    }
    if (!host_role_ && !source() && (data.action == Action::kSourceAccepted || data.action == Action::kScanProgress)) {
        auto update = make(data.action); *update.workspace = data; local(std::move(update)); return;
    }
    if (!source() && prepared_ && data.action == Action::kOffer && !worker_) {
        create_receiver(data); local(make(Action::kOffer)); return;
    }
    if (source() && offered_ && data.action == Action::kReady && !transfer_ready_) {
        if (!worker_->start_sending()) { cancel("transfer_start_out_of_order", true); return; }
        transfer_ready_ = true; (void)gate_.start_transfer(operation_epoch_, operation_); local(make(Action::kReady));
    }
}
void TransferRuntimeBridge::finish_local() {
    if (!local_finished_) {
        local_finished_ = true; (void)gate_.local_finished(operation_epoch_, operation_);
    }
    if (ready_ && !completed_notification_queued_) {
        auto finished = make(Action::kFinished); finished.workspace->error_code = error_;
        if (host_role_) finished.workspace->paste_submitted = paste_submitted_;
        queue(std::move(finished)); completed_notification_queued_ = true;
    }
}
void TransferRuntimeBridge::update_worker(std::uint64_t now) {
    if (!worker_) { if (cancelled_) finish_local(); return; }
    const auto progress = worker_->progress();
    if (progress.state == TransferWorkerState::kAwaitingPaste && !paste_requested_ && !cancelled_) {
        paste_requested_ = true;
        auto payload = worker_->take_prepared_clipboard();
        std::string error;
        const bool applied = payload && clipboard() && (clipboard_mode_ == 1 || clipboard_mode_ == 4
            || (clipboard_mode_ == 2 || clipboard_mode_ == 5 ? clipboard_paste_.publish(*payload, &error)
                : host_role_ && clipboard_paste_.complete(*payload, &error)));
        if (!applied) {
            cancel(error.empty() ? "clipboard_payload_unavailable" : std::move(error), true); return;
        }
        paste_submitted_ = clipboard_mode_ == 0;
        if (!worker_->complete_clipboard()) { cancel("clipboard_completion_failed", true); return; }
    }
    if (progress.finished()) {
        if (progress.state != TransferWorkerState::kComplete && !cancelled_) cancel(progress.error, true);
        if (cancelled_ || (!pending_bulk_ && worker_->output_empty())) finish_local();
    }
    if (cancelled_) return;
    if (source() && !prepared_ && (progress.state == TransferWorkerState::kSelecting
        || (clipboard() && progress.state == TransferWorkerState::kPrepared))) {
        prepared_ = true;
        if (host_role_) queue(make(Action::kPrepared)); else local(make(Action::kPrepared));
    }
    if (source() && progress.accepted_sources != accepted_sources_) {
        accepted_sources_ = progress.accepted_sources;
        auto accepted = make(Action::kSourceAccepted); accepted.workspace->accepted_sources = accepted_sources_;
        if (host_role_) queue(std::move(accepted)); else local(std::move(accepted));
    }
    if (source() && !offered_ && progress.state == TransferWorkerState::kPrepared) {
        offered_ = true;
        auto offer = make(Action::kOffer); offer.workspace->entries = progress.totals.entries;
        offer.workspace->files = progress.totals.files; offer.workspace->bytes = progress.totals.bytes;
        queue(offer); local(std::move(offer));
    }
    if (!source() && !copy_action() && clipboard_mode_ < 4 && !transfer_ready_ && progress.state == TransferWorkerState::kTransferring) {
        transfer_ready_ = true; (void)gate_.start_transfer(operation_epoch_, operation_);
        queue(make(Action::kReady)); local(make(Action::kReady));
    }
    if (now >= next_progress_ms_) {
        next_progress_ms_ = now + 200;
        auto update = make(progress.state == TransferWorkerState::kScanning ? Action::kScanProgress : Action::kProgress);
        auto& data = *update.workspace;
        data.entries = progress.totals.entries; data.files = progress.totals.files; data.bytes = progress.totals.bytes;
        data.completed_entries = progress.completed_entries; data.completed_bytes = progress.transferred_bytes;
        data.committed_bytes = progress.committed_bytes; data.completed_files = progress.completed_files;
        data.skipped_entries = progress.skipped_entries;
        if (data.action == Action::kProgress && !clipboard()) data.path = progress.current_path;
        data.accepted_sources = progress.accepted_sources;
        if (host_role_ && ((source() && !offered_) || copy_action())) progress_message_ = update;
        local(std::move(update));
    }
}
void TransferRuntimeBridge::update_availability() {
    const bool busy = gate_.blocks_mutation();
    if (availability_known_ && notified_ready_ == ready_ && notified_busy_ == busy && notified_revision_ == gate_.revision()
        && notified_clipboard_version_ == clipboard_version_) return;
    auto status = make(Action::kAvailability);
    status.file_transfer_version = ready_ ? 1 : 0;
    status.clipboard_version = clipboard_version_;
    status.workspace->active = busy; status.workspace->operation_revision = gate_.revision();
    if (!ready_) status.workspace->error_code = "workspace_peer_unavailable";
    local(std::move(status)); availability_known_ = true;
    notified_ready_ = ready_; notified_busy_ = busy; notified_revision_ = gate_.revision();
    notified_clipboard_version_ = clipboard_version_;
}
void TransferRuntimeBridge::pump(std::uint64_t now, bool connected) {
    bool open, overflow; std::uint32_t version, clipboard_version; std::uint64_t invalidation; std::string epoch;
    std::deque<Control> incoming;
    {
        std::lock_guard lock(inbox_mutex_);
        open = open_; overflow = overflow_; version = peer_version_; invalidation = invalidation_;
        clipboard_version = peer_clipboard_version_;
        epoch = advertised_peer_epoch_; incoming.swap(controls_);
    }
    const bool changed = observed_invalidation_ != invalidation || (!peer_epoch_.empty() && peer_epoch_ != epoch);
    observed_invalidation_ = invalidation;
    const bool available = connected && version && open && !overflow;
    if ((ready_ && !available) || changed || overflow) { ready_ = false; disconnect(); }
    ready_ = available; peer_epoch_ = std::move(epoch);
    clipboard_version_ = ready_ ? clipboard_version : 0;
    clipboard_ready_ = clipboard_version_ >= 1;
    if (copy_action() && !operation_.empty() && !cancelled_ && clipboard_version_ < 2) cancel("clipboard_peer_unsupported", true);
    if (clipboard() && !operation_.empty() && !cancelled_ && (!clipboard_ready_ || !clipboard_paste_.pump()))
        cancel("clipboard_context_unavailable", true);
    if (open) ensure_attempts_ = 0;
    if (host_role_ && connected && version && !open && !overflow && ensure_attempts_ < 3 && now >= next_ensure_ms_) {
        ++ensure_attempts_; next_ensure_ms_ = now + 3000; if (ensure_) (void)ensure_();
    }
    if (ready_) for (const auto& message : incoming) peer_message(message);
    pump_local_clipboard();
    if (worker_ && !cancelled_ && !copy_action() && clipboard_mode_ < 4) {
        {
            std::lock_guard lock(inbox_mutex_);
            for (unsigned count = 0; count < 16 && !bulk_.empty(); ++count) {
                if (!worker_->receive(bulk_.front())) break;
                bulk_.pop_front();
            }
        }
        for (unsigned count = 0; count < 8 && ready_; ++count) {
            if (!pending_bulk_) pending_bulk_ = worker_->take_output();
            if (!pending_bulk_ || !bulk_send_ || !bulk_send_(*pending_bulk_)) break;
            pending_bulk_.reset();
        }
    }
    update_worker(now);
    for (unsigned count = 0; count < 8 && ready_ && !outgoing_.empty(); ++count) {
        if (!peer_send_ || !peer_send_(outgoing_.front())) break;
        if (outgoing_.front().workspace->action == Action::kFinished && outgoing_.front().request_id == operation_)
            (void)gate_.completion_sent(operation_epoch_, operation_);
        outgoing_.pop_front();
    }
    if (ready_ && outgoing_.empty() && progress_message_ && peer_send_ && peer_send_(*progress_message_)) progress_message_.reset();
    pump_browser();
    if (!operation_.empty() && local_finished_ && !gate_.blocks_mutation()) {
        auto finished = make(Action::kFinished); finished.workspace->error_code = error_;
        finished.workspace->paste_submitted = paste_submitted_;
        if (worker_) {
            const auto progress = worker_->progress();
            auto& data = *finished.workspace;
            data.entries = progress.totals.entries; data.files = progress.totals.files; data.bytes = progress.totals.bytes;
            data.completed_entries = progress.completed_entries; data.completed_bytes = progress.transferred_bytes;
            data.committed_bytes = progress.committed_bytes; data.completed_files = progress.completed_files;
            data.skipped_entries = progress.skipped_entries;
            const auto path = progress.results_path.generic_u8string();
            if (!clipboard() && !copy_action()) data.results_path.assign(reinterpret_cast<const char*>(path.data()), path.size());
            if (clipboard() && clipboard_mode_ && !source() && error_.empty()) {
                const auto snapshot = destination_.generic_u8string();
                data.snapshot_path.assign(reinterpret_cast<const char*>(snapshot.data()), snapshot.size());
                data.results_path.assign(reinterpret_cast<const char*>(path.data()), path.size());
            }
        }
        local(std::move(finished));
        worker_.reset(); local_clipboard_source_.reset(); local_clipboard_data_.reset(); local_clipboard_receipt_.reset();
        operation_.clear(); pending_bulk_.reset(); outgoing_.clear(); progress_message_.reset();
        clipboard_mode_ = 0; clipboard_source_.clear(); purpose_ = protocol::WorkspaceTransferPurposeV1::kFiles;
        std::lock_guard lock(inbox_mutex_); bulk_.clear();
    }
    update_availability();
}
void TransferRuntimeBridge::pump_local_clipboard() {
    if (!local_clipboard_source_ || cancelled_) return;
    const auto progress = local_clipboard_source_->progress();
    if (progress.finished() && progress.state != TransferWorkerState::kComplete) { cancel(progress.error, true); return; }
    if (!worker_ && progress.state == TransferWorkerState::kPrepared) {
        protocol::WorkspaceControlV1 offer;
        offer.entries = progress.totals.entries; offer.files = progress.totals.files; offer.bytes = progress.totals.bytes;
        create_receiver(offer);
        if (!local_clipboard_source_->start_sending()) { cancel("clipboard_local_start_failed", true); return; }
        (void)gate_.start_transfer(operation_epoch_, operation_);
    }
    if (!worker_) return;
    for (unsigned count = 0; count < 8; ++count) {
        if (!local_clipboard_data_) local_clipboard_data_ = local_clipboard_source_->take_output();
        if (local_clipboard_data_ && worker_->receive(*local_clipboard_data_)) local_clipboard_data_.reset();
        if (!local_clipboard_receipt_) local_clipboard_receipt_ = worker_->take_output();
        if (local_clipboard_receipt_ && local_clipboard_source_->receive(*local_clipboard_receipt_)) local_clipboard_receipt_.reset();
    }
}
void TransferRuntimeBridge::browse(const Control& request) {
    pending_browse_ = request; browse_reply_.reset();
    if (browser_) browser_->cancel();
}
void TransferRuntimeBridge::pump_browser() {
    if (!host_role_ && !browser_local_) return;
    if (browser_ && pending_browse_ && browser_->finished()) browser_.reset();
    if (ready_ && !browser_ && pending_browse_) {
        const auto request = std::move(*pending_browse_); pending_browse_.reset(); browse_operation_ = request.request_id;
        try {
            const bool copies = request.workspace->action == Action::kBrowseClipboardCopies;
            browser_ = std::make_unique<DirectoryBrowser>(copies ? ClipboardCopyStore(spool_directory_).directory()
                : path_from_utf8(request.workspace->path), copies);
        }
        catch (const std::exception&) {
            auto error = make(Action::kBrowseEnd, browse_operation_); error.workspace->error_code = "browse_invalid_path";
            browse_reply_ = std::move(error);
        }
    }
    // Keep browsing behind operation receipts and bounded to a few rows per
    // owner tick; directory size never expands the runtime control queue.
    for (unsigned count = 0; count < 4 && ready_ && outgoing_.empty() && !pending_browse_; ++count) {
        if (!browse_reply_ && browser_) {
            if (auto entry = browser_->take()) {
                browse_reply_ = make(entry->action, browse_operation_); *browse_reply_->workspace = std::move(*entry);
            }
        }
        if (!browse_reply_) break;
        if (browser_local_) local(*browse_reply_);
        else if (!peer_send_ || !peer_send_(*browse_reply_)) break;
        const bool end = browse_reply_->workspace->action == Action::kBrowseEnd;
        browse_reply_.reset();
        if (end && browser_ && browser_->finished()) { browser_.reset(); browser_local_ = false; }
    }
    if (!ready_ && browser_ && browser_->finished()) browser_.reset();
}
}
