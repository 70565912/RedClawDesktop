#include "redclaw/workspace/clipboard_demand_session.h"
#include <algorithm>

namespace redclaw::workspace {
namespace {
using Direction = protocol::TransferDirectionV1;
using Purpose = protocol::WorkspaceTransferPurposeV1;
ClipboardDemandOffer offer_of(const protocol::WorkspaceControlV1& value) {
    return {value.clipboard_token, value.clipboard_formats, value.clipboard_revision, value.clipboard_move};
}
constexpr std::size_t kPageBytes = 32U * 1024U;
}
ClipboardDemandSession::ClipboardDemandSession(bool host, std::string epoch, std::filesystem::path spool,
    std::shared_ptr<ClipboardDemandPort> port, ClipboardHostActions actions, Send peer, Send gui,
    Send start, std::function<void(std::string)> cancel, std::function<bool(std::string_view)> available)
    : host_(host), epoch_(std::move(epoch)), spool_(std::move(spool)), port_(std::move(port)), actions_(std::move(actions)),
      shortcut_paste_(actions_), peer_(std::move(peer)), gui_(std::move(gui)), start_(std::move(start)),
      cancel_(std::move(cancel)), available_(std::move(available)) { native_failed_ = !port_; }
ClipboardDemandSession::Control ClipboardDemandSession::message(Action action, std::string operation,
    ClipboardDemandOffer offer, Direction direction) const {
    Control result; result.type = protocol::StreamControlMessageTypeV1::kWorkspace; result.session_epoch = epoch_;
    result.request_id = operation.empty() ? "clipboard-offer" : std::move(operation);
    auto& value = result.workspace.emplace(); value.action = action; value.purpose = Purpose::kClipboard;
    value.direction = direction; value.clipboard_mode = 6; value.clipboard_token = std::move(offer.token);
    value.clipboard_formats = offer.formats; value.clipboard_move = offer.move; value.clipboard_revision = offer.sequence;
    return result;
}
bool ClipboardDemandSession::pending(std::string operation, ClipboardDemandOffer offer, Direction direction) {
    if (pending_) return pending_->operation == operation && pending_->offer.token == offer.token;
    if (available_ && !available_(operation)) {
        if (port_) port_->cancel(operation);
        if (peer_) { auto rejected = message(Action::kCancel, operation, offer, direction);
            rejected.workspace->error_code = "workspace_transfer_busy"; (void)peer_(rejected); }
        return false;
    }
    pending_ = Pending{}; pending_->operation = std::move(operation); pending_->offer = std::move(offer); pending_->direction = direction;
    return true;
}
bool ClipboardDemandSession::reserved_for_other(std::string_view operation) const {
    return pending_ && pending_->operation != operation;
}
bool ClipboardDemandSession::accepts_transfer(const Control& value) const {
    if (!enabled_ || !port_ || !value.workspace) return false;
    const bool source = host_ == (value.workspace->direction == Direction::kToController);
    const auto& offer = source ? local_offer_ : remote_offer_;
    return !offer.token.empty() && offer.token == value.workspace->clipboard_token
        && offer.formats == value.workspace->clipboard_formats && offer.move == value.workspace->clipboard_move;
}
std::uint32_t ClipboardDemandSession::source_sequence(std::string_view token) const {
    std::uint32_t sequence = 0; std::vector<ClipboardFileSelection> files;
    return port_ && port_->source(token, sequence, files) ? sequence : 0;
}
ClipboardCapture ClipboardDemandSession::checked_capture(std::string_view token, ClipboardCapture fallback) const {
    const auto offer = local_offer_;
    auto description = preview_token_ == token ? source_description_ : std::shared_ptr<ClipboardDescription>{};
    return [port = port_, offer, description, fallback = std::move(fallback)](ClipboardSnapshot& snapshot,
        const std::filesystem::path& spool, std::uint32_t expected, const std::atomic_bool& cancelled, std::string* error) {
        std::uint32_t sequence = 0; std::vector<ClipboardFileSelection> files;
        if (!port || !port->source(offer.token, sequence, files) || sequence != expected) {
            if (error) *error = "clipboard_source_changed"; return false;
        }
        if ((offer.formats & kClipboardFilesFormat) && (!description || !description->sources_unchanged(cancelled, error))) return false;
        return fallback ? fallback(snapshot, spool, expected, cancelled, error) : snapshot.capture(spool, expected, cancelled, error);
    };
}
void ClipboardDemandSession::local_state(Action action, std::string status) {
    if (!pending_ || host_ || !gui_) return;
    auto result = message(action, pending_->operation, pending_->offer, pending_->direction);
    result.workspace->error_code = std::move(status);
    if (pending_->description) {
        const auto& totals = pending_->description->totals;
        result.workspace->entries = totals.entries; result.workspace->files = totals.files; result.workspace->bytes = totals.bytes;
    }
    if (pending_->bulk_done) {
        const auto& bulk = pending_->bulk; auto& value = *result.workspace;
        value.entries = bulk.entries; value.files = bulk.files; value.bytes = bulk.bytes;
        value.completed_entries = bulk.completed_entries; value.completed_files = bulk.completed_files;
        value.completed_bytes = bulk.completed_bytes; value.committed_bytes = bulk.committed_bytes; value.skipped_entries = bulk.skipped_entries;
    }
    presentation(result); (void)gui_(result);
}
void ClipboardDemandSession::presentation(Control& message) const {
    if (!matches(message)) return;
    const auto description = pending_->description ? pending_->description
        : preview_token_ == pending_->offer.token ? source_description_ : std::shared_ptr<ClipboardDescription>{};
    if (!description) return;
    auto& value = *message.workspace; const auto& totals = description->totals;
    if (value.entries < totals.entries || value.files < totals.files || value.bytes < totals.bytes) return;
    const auto subtract = [](std::uint64_t n, std::uint64_t overhead) { return n > overhead ? n - overhead : 0; };
    // The snapshot's format metadata precedes the selected file roots. Keep
    // its envelope counts inside the transport, outside user file progress.
    value.completed_bytes = subtract(value.completed_bytes, value.bytes - totals.bytes);
    value.committed_bytes = subtract(value.committed_bytes, value.bytes - totals.bytes);
    value.completed_files = subtract(value.completed_files, value.files - totals.files);
    value.completed_entries = subtract(value.completed_entries, value.entries - totals.entries);
    value.skipped_entries = (std::min)(value.skipped_entries, value.completed_entries);
    value.entries = totals.entries; value.files = totals.files; value.bytes = totals.bytes;
    if (value.path.starts_with("metadata")) value.path.clear();
    else if (value.path.starts_with("files/")) {
        const auto end = value.path.find('/', 6); if (end != std::string::npos) value.path.erase(0, end + 1);
    }
}
void ClipboardDemandSession::describe(std::string operation, const ClipboardDemandOffer& offer) {
    if (!port_ || offer.token != local_offer_.token) {
        auto rejected = message(Action::kClipboardDescription, operation, offer, host_ ? Direction::kToController : Direction::kToHost);
        rejected.workspace->error_code = "clipboard_source_changed"; if (peer_) (void)peer_(rejected); return;
    }
    if (preview_) {
        if (preview_operation_ == operation) return;
        auto rejected = message(Action::kClipboardDescription, operation, offer, host_ ? Direction::kToController : Direction::kToHost);
        rejected.workspace->error_code = "workspace_transfer_busy"; if (peer_) (void)peer_(rejected); return;
    }
    std::uint32_t sequence = 0; std::vector<ClipboardFileSelection> files;
    if (!port_->source(offer.token, sequence, files) || files.empty()) {
        auto rejected = message(Action::kClipboardDescription, operation, offer, host_ ? Direction::kToController : Direction::kToHost);
        rejected.workspace->error_code = "clipboard_source_changed"; if (peer_) (void)peer_(rejected); return;
    }
    preview_operation_ = std::move(operation); preview_token_ = offer.token; source_description_.reset();
    TransferWorkerConfig config; config.source = true; config.directory = spool_;
    config.clipboard_description_only = true; config.clipboard_files = std::move(files);
    preview_ = std::make_unique<TransferWorker>(std::move(config));
}
void ClipboardDemandSession::demand(std::string operation, ClipboardDemandOffer offer, Direction direction) {
    if (!pending(std::move(operation), std::move(offer), direction)) return;
    pending_->demanded = true;
    if (pending_->started || pending_->asked) return;
    if (pending_->offer.formats & kClipboardFilesFormat) {
        if (!pending_->description) {
            if (pending_->offer.token == local_offer_.token && source_description_ && preview_token_ == local_offer_.token)
                pending_->description = source_description_;
            else return;
        }
        pending_->asked = true; local_state(Action::kClipboardConfirm); return;
    }
    approve();
}
void ClipboardDemandSession::approve() {
    if (!pending_ || pending_->started || host_) return;
    const bool local_source = pending_->direction == Direction::kToHost;
    const auto sequence = local_source ? source_sequence(pending_->offer.token) : pending_->offer.sequence;
    if (!sequence) { finish("clipboard_source_changed", true); return; }
    if (local_source) {
        auto approved = message(Action::kClipboardApprove, pending_->operation, pending_->offer, pending_->direction);
        approved.workspace->clipboard_complete = true;
        if (!peer_ || !peer_(approved)) { finish("workspace_send_unconfirmed"); return; }
    } else if (port_) port_->approve(pending_->operation);
    pending_->confirmed = true;
    if ((pending_->offer.formats & kClipboardFilesFormat) && !pending_->read_requested) {
        local_state(Action::kClipboardNativePending, "clipboard_waiting_for_target"); return;
    }
    auto prepare = message(Action::kPrepare, pending_->operation, pending_->offer, pending_->direction);
    if (local_source) prepare.workspace->clipboard_sequence = sequence;
    pending_->started = true; local_state(Action::kClipboardNativePending, "clipboard_transferring");
    if (!start_ || !start_(prepare)) finish("workspace_transfer_busy", true);
}
bool ClipboardDemandSession::matches(const Control& value) const {
    return pending_ && value.workspace && value.request_id == pending_->operation
        && value.workspace->clipboard_token == pending_->offer.token && value.workspace->direction == pending_->direction;
}
bool ClipboardDemandSession::from_gui(const Control& value) {
    if (!value.workspace || value.workspace->clipboard_token.empty()) return false;
    const auto& data = *value.workspace;
    if (data.action == Action::kClipboardShortcut) {
        if (!enabled_ || host_) return true;
        if (native_failed_) {
            if (pending(value.request_id, {"local-shortcut", 0, 0, false}, Direction::kToHost)) {
                local_state(Action::kClipboardNativePending); finish("clipboard_native_unavailable");
            }
            return true;
        }
        if (pending_ || !shortcut_waiting_.empty()) return true;
        shortcut_waiting_ = value.request_id;
        port_->refresh_for_shortcut(value.request_id, data.clipboard_sequence); return true;
    }
    if (data.action == Action::kClipboardApprove) {
        if (matches(value) && pending_->asked && !pending_->started) {
            if (data.clipboard_complete) approve(); else finish("transfer_cancelled", true);
        }
        return true;
    }
    if (data.action == Action::kCancel && data.clipboard_mode == 6) {
        if (matches(value)) finish("transfer_cancelled", true);
        return true;
    }
    return data.action >= Action::kClipboardOffer;
}
bool ClipboardDemandSession::from_peer(const Control& value) {
    if (!value.workspace) return false;
    if (value.workspace->action == Action::kCancel && value.workspace->clipboard_mode == 6 && matches(value)) {
        finish(value.workspace->error_code.empty() ? "transfer_cancelled" : value.workspace->error_code); return true;
    }
    if (value.workspace->action < Action::kClipboardOffer) return false;
    if (!enabled_ || !port_ || value.session_epoch != peer_epoch_) return true;
    const auto& data = *value.workspace; const auto offer = offer_of(data);
    switch (data.action) {
    case Action::kClipboardOffer:
        if (data.clipboard_token != remote_offer_.token) {
            if (pending_ && pending_->offer.token == remote_offer_.token) finish("clipboard_source_changed", true);
            remote_offer_ = offer; port_->remote_offer(offer);
        }
        return true;
    case Action::kClipboardDescribe: describe(value.request_id, offer); return true;
    case Action::kClipboardDescription:
        if (!matches(value) || pending_->offer.token != remote_offer_.token) return true;
        if (!data.error_code.empty()) { finish(data.error_code, true); return true; }
        if (!data.clipboard_pages || data.clipboard_page > receiving_page_
            || (receiving_pages_ && receiving_pages_ != data.clipboard_pages)) { finish("clipboard_description_order_invalid", true); return true; }
        if (data.clipboard_page < receiving_page_) return true;
        if (receiving_metadata_.size() + data.clipboard_metadata.size() > kMaxClipboardDescriptionBytes) { finish("clipboard_description_too_large", true); return true; }
        receiving_pages_ = data.clipboard_pages; ++receiving_page_; receiving_metadata_ += data.clipboard_metadata;
        if (receiving_page_ == receiving_pages_) {
            std::string error; pending_->description = ClipboardDescription::parse(receiving_metadata_, &error);
            receiving_metadata_.clear(); receiving_page_ = receiving_pages_ = 0;
            if (!pending_->description) { finish(error, true); return true; }
            port_->description(pending_->operation, pending_->description, {});
            if (pending_->demanded && !host_) demand(pending_->operation, pending_->offer, pending_->direction);
        }
        return true;
    case Action::kClipboardDemand:
        if (!host_ && offer.token == local_offer_.token && data.direction == Direction::kToHost) {
            if (data.clipboard_complete && pending(value.request_id, offer, data.direction)) pending_->read_requested = true;
            demand(value.request_id, offer, data.direction);
            if (matches(value) && pending_->confirmed && pending_->read_requested && !pending_->started) approve();
        }
        return true;
    case Action::kClipboardApprove:
        if (host_ && offer.token == remote_offer_.token && data.direction == Direction::kToHost) {
            if (data.clipboard_complete) port_->approve(value.request_id);
            else if (matches(value)) finish("transfer_cancelled");
        }
        return true;
    case Action::kClipboardInvokePaste:
        if (pending_ && pending_->operation == value.request_id) return true;
        if (host_ && offer.token == remote_offer_.token && pending(value.request_id, offer, data.direction)) {
            std::string error;
            if (!shortcut_paste_.begin(peer_epoch_, value.request_id, &error)) finish(error, true);
            else port_->remote_offer(offer, value.request_id);
        }
        return true;
    case Action::kClipboardNativeResult:
        if (!matches(value)) return true;
        pending_->native_received = true; pending_->native_complete = data.clipboard_complete;
        pending_->effect = data.clipboard_effect; pending_->native_error = data.error_code;
        if (!data.error_code.empty() && !pending_->started) finish(data.error_code);
        else settle();
        return true;
    case Action::kClipboardMoveCommit:
        if (host_ && matches(value) && pending_->offer.move) { pending_->move_requested = true; settle(); }
        return true;
    case Action::kClipboardMoveReceipt:
        if (host_ && data.direction == Direction::kToHost && matches(value) && pending_->committing) {
            finish(data.clipboard_complete && data.clipboard_effect == 2 ? "" : "clipboard_source_retained");
            return true;
        }
        if (!host_ && matches(value) && pending_->committing)
            finish(data.clipboard_complete && data.clipboard_effect == 2 ? "" : data.error_code.empty() ? "clipboard_source_retained" : data.error_code);
        return true;
    default: return true;
    }
}
void ClipboardDemandSession::transfer_started(const Control& value) {
    if (!value.workspace || value.workspace->clipboard_mode != 6) return;
    if (!pending(value.request_id, offer_of(*value.workspace), value.workspace->direction)) return;
    pending_->started = pending_->demanded = true;
}
void ClipboardDemandSession::received(std::string_view operation, std::shared_ptr<PreparedClipboardPayload> payload,
    std::filesystem::path directory, bool verified) {
    if (!pending_ || pending_->operation != operation || !port_) return;
    if (!verified || !payload) { finish("clipboard_data_not_verified", true); return; }
    port_->payload(pending_->operation, std::move(payload), std::move(directory), {});
}
void ClipboardDemandSession::bulk_finished(const Control& result, bool) {
    if (!matches(result)) return;
    if (!result.workspace->error_code.empty()) { finish(result.workspace->error_code, true); return; }
    pending_->bulk_done = true;
    pending_->bulk = *result.workspace;
    local_state(Action::kClipboardNativePending, "clipboard_awaiting_native_paste"); settle();
}
void ClipboardDemandSession::settle() {
    if (!pending_ || !pending_->bulk_done || !pending_->native_received) return;
    const bool destination = host_ == (pending_->direction == Direction::kToHost);
    if (destination) {
        auto result = message(Action::kClipboardNativeResult, pending_->operation, pending_->offer, pending_->direction);
        result.workspace->clipboard_complete = pending_->native_complete;
        result.workspace->clipboard_effect = pending_->effect; result.workspace->error_code = pending_->native_error;
        if (peer_ && !peer_(result)) { finish("workspace_send_unconfirmed"); return; }
    }
    if (!pending_->native_complete || !pending_->native_error.empty()) { finish("clipboard_native_paste_incomplete"); return; }
    if (!pending_->offer.move) { finish(); return; }
    if (pending_->effect != 2) { finish("clipboard_source_retained"); return; }
    if (host_ && destination) { pending_->committing = true; return; }
    if (pending_->committing) return;
    if (host_) {
        if (pending_->move_requested && source_description_ && preview_token_ == pending_->offer.token) {
            pending_->committing = true;
            port_->commit_source(pending_->operation, pending_->offer.token, source_description_);
        }
        return;
    }
    pending_->committing = true; local_state(Action::kClipboardNativePending, "clipboard_completing_move");
    if (destination) {
        auto commit = message(Action::kClipboardMoveCommit, pending_->operation, pending_->offer, pending_->direction);
        if (!peer_ || !peer_(commit)) finish("clipboard_move_result_unknown");
    } else if (port_ && source_description_ && preview_token_ == pending_->offer.token)
        port_->commit_source(pending_->operation, pending_->offer.token, source_description_);
    else finish("clipboard_source_retained");
}
void ClipboardDemandSession::finish(std::string error, bool notify_peer) {
    if (!pending_) return;
    if (pending_->committing && pending_->offer.move && (error == "transfer_cancelled" || error == "workspace_disconnected"
        || error == "clipboard_session_changed" || error == "workspace_send_unconfirmed"))
        error = "clipboard_move_result_unknown";
    const auto operation = pending_->operation;
    shortcut_paste_.cancel();
    if (notify_peer && peer_) {
        auto cancel = message(Action::kCancel, operation, pending_->offer, pending_->direction);
        cancel.workspace->error_code = error; (void)peer_(cancel);
    }
    if (!error.empty()) {
        if (cancel_) cancel_(operation);
        if (port_) port_->cancel(operation);
    }
    if (port_) port_->finished(operation, error.empty() && pending_->offer.move && pending_->committing);
    local_state(Action::kFinished, std::move(error)); pending_.reset();
    receiving_metadata_.clear(); receiving_page_ = receiving_pages_ = 0;
}
void ClipboardDemandSession::native(ClipboardDemandEvent event) {
    const auto direction = host_ ? Direction::kToHost : Direction::kToController;
    switch (event.kind) {
    case ClipboardDemandEventKind::kOffer:
        if (pending_ && event.offer.token != local_offer_.token) finish("clipboard_local_changed", true);
        if (event.offer.token != local_offer_.token) {
            if (preview_) preview_->cancel();
            preview_token_.clear(); source_description_.reset(); sending_metadata_.clear();
        }
        local_offer_ = event.offer;
        if (peer_) (void)peer_(message(Action::kClipboardOffer, {}, event.offer, host_ ? Direction::kToController : Direction::kToHost));
        break;
    case ClipboardDemandEventKind::kShortcutReady:
        if (event.operation != shortcut_waiting_) break;
        shortcut_waiting_.clear();
        if (!event.error.empty()) {
            if (pending(event.operation, {"local-shortcut", 0, 0, false}, Direction::kToHost)) {
                local_state(Action::kClipboardNativePending); finish(event.error);
            }
            break;
        }
        if (!host_ && !event.offer.token.empty() && event.error.empty() && peer_
            && pending(event.operation, event.offer, Direction::kToHost)) {
            local_state(Action::kClipboardNativePending, "clipboard_preparing");
            (void)peer_(message(Action::kClipboardInvokePaste, event.operation, event.offer, Direction::kToHost));
        }
        break;
    case ClipboardDemandEventKind::kInstalled:
        if (host_ && pending_ && pending_->operation == event.operation && pending_->offer.token == event.offer.token) {
            std::string error;
            if (!shortcut_paste_.paste(&error)) finish(error, true);
        }
        break;
    case ClipboardDemandEventKind::kDescribe:
        if (pending(event.operation, event.offer, direction) && peer_) {
            local_state(Action::kClipboardNativePending, "clipboard_scanning");
            (void)peer_(message(Action::kClipboardDescribe, event.operation, event.offer, direction));
        }
        break;
    case ClipboardDemandEventKind::kDemand:
        if (host_) {
            if (pending(event.operation, event.offer, direction)) {
                pending_->demanded = true;
                if (peer_) (void)peer_(message(Action::kClipboardDemand, event.operation, event.offer, direction));
            }
        } else demand(event.operation, event.offer, direction);
        break;
    case ClipboardDemandEventKind::kRead:
        if (host_) {
            if (pending(event.operation, event.offer, direction) && peer_) {
                auto read = message(Action::kClipboardDemand, event.operation, event.offer, direction);
                read.workspace->clipboard_complete = true; (void)peer_(read);
            }
        } else if (pending(event.operation, event.offer, direction)) {
            pending_->read_requested = true;
            if (pending_->confirmed) approve(); else demand(event.operation, event.offer, direction);
        }
        break;
    case ClipboardDemandEventKind::kNativeResult:
        if (pending_ && pending_->operation == event.operation && pending_->offer.token == event.offer.token) {
            pending_->native_received = true; pending_->native_complete = event.complete;
            pending_->effect = event.effect; pending_->native_error = event.error; settle();
            if (pending_ && !pending_->started && !event.error.empty()) finish(event.error, true);
        }
        break;
    case ClipboardDemandEventKind::kMoveReceipt:
        if (pending_ && pending_->operation == event.operation && pending_->offer.token == event.offer.token && pending_->committing) {
            if (peer_) {
                auto result = message(Action::kClipboardMoveReceipt, event.operation, pending_->offer, pending_->direction);
                result.workspace->clipboard_complete = event.complete; result.workspace->clipboard_effect = event.effect;
                result.workspace->error_code = event.error; (void)peer_(result);
            }
            finish(event.complete ? "" : event.error.empty() ? "clipboard_source_retained" : event.error);
        }
        break;
    case ClipboardDemandEventKind::kUnavailable:
        native_failed_ = true;
        if (pending_) finish("clipboard_native_unavailable", true);
        local_offer_ = {}; remote_offer_ = {};
        break;
    }
}
void ClipboardDemandSession::pump(std::uint64_t, bool enabled, std::string peer_epoch) {
    if (enabled_ != enabled || peer_epoch_ != peer_epoch) {
        if (pending_) finish("clipboard_session_changed");
        enabled_ = enabled; peer_epoch_ = std::move(peer_epoch);
        if (port_) port_->connected(enabled_, epoch_ + ":" + peer_epoch_);
    }
    if (!enabled_ || !port_) return;
    for (unsigned i = 0; i < 8; ++i) { auto event = port_->take_event(); if (!event) break; native(std::move(*event)); }
    if (preview_ && preview_->progress().finished()) {
        const auto progress = preview_->progress();
        const bool current = !preview_token_.empty() && preview_token_ == local_offer_.token;
        source_description_ = current ? preview_->clipboard_description() : std::shared_ptr<ClipboardDescription>{};
        preview_.reset(); sending_page_ = 0;
        if (source_description_) sending_metadata_ = source_description_->serialize();
        if (current && (!source_description_ || sending_metadata_.empty())) {
            auto error = message(Action::kClipboardDescription, preview_operation_, local_offer_, host_ ? Direction::kToController : Direction::kToHost);
            error.workspace->error_code = progress.error.empty() ? "clipboard_description_unavailable" : progress.error;
            if (peer_) (void)peer_(error);
        }
        if (pending_ && pending_->offer.token == preview_token_ && source_description_) {
            pending_->description = source_description_;
            if (pending_->demanded && !host_) demand(pending_->operation, pending_->offer, pending_->direction);
        }
    }
    if (!sending_metadata_.empty() && peer_) {
        auto part = message(Action::kClipboardDescription, preview_operation_, local_offer_, host_ ? Direction::kToController : Direction::kToHost);
        part.workspace->clipboard_pages = static_cast<std::uint32_t>((sending_metadata_.size() + kPageBytes - 1) / kPageBytes);
        part.workspace->clipboard_page = sending_page_;
        part.workspace->clipboard_metadata = sending_metadata_.substr(sending_page_ * kPageBytes, kPageBytes);
        if (peer_(part) && ++sending_page_ == part.workspace->clipboard_pages) sending_metadata_.clear();
    }
    // Native extraction and conflict dialogs have no fixed deadline. Retain
    // the single reservation until native completion, explicit cancel or loss
    // of the session; elapsed time alone never aborts paste or commits a move.
}
void ClipboardDemandSession::disconnected() {
    if (pending_) finish("workspace_disconnected");
    if (preview_) { preview_->cancel(); if (preview_->progress().finished()) preview_.reset(); }
    local_offer_ = {}; remote_offer_ = {}; source_description_.reset(); sending_metadata_.clear(); receiving_metadata_.clear();
    preview_token_.clear(); preview_operation_.clear(); receiving_page_ = receiving_pages_ = 0;
    enabled_ = false; peer_epoch_.clear(); shortcut_waiting_.clear(); if (port_) port_->connected(false, {});
}
void ClipboardDemandSession::reset_epoch(std::string epoch) { disconnected(); epoch_ = std::move(epoch); }
}
