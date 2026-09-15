#include "redclaw/workspace/transfer_batch_sender.h"
#include <algorithm>
#include <limits>

namespace redclaw::workspace {
using protocol::TransferMessageTypeV1;
using protocol::TransferDispositionV1;
bool TransferBatchSender::prepare(std::span<const std::filesystem::path> selected,
    const std::filesystem::path& spool_directory, const std::function<bool(const TransferScanProgress&)>& progress) {
    if (!begin_prepare(spool_directory)) return false;
    for (const auto& source : selected) if (!add_source(source, progress)) return false;
    return finish_prepare();
}
bool TransferBatchSender::begin_prepare(const std::filesystem::path& spool_directory) {
    cancel_if_requested();
    if (progress_.state != TransferSenderState::kIdle) return false;
    if (!manifest_.begin_scan(spool_directory, &progress_.error)) { fail(progress_.error); return false; }
    progress_.state = TransferSenderState::kScanning;
    return true;
}
bool TransferBatchSender::add_source(const std::filesystem::path& source,
    const std::function<bool(const TransferScanProgress&)>& progress, std::string_view relative_root) {
    cancel_if_requested();
    if (progress_.state != TransferSenderState::kScanning) return false;
    if (!manifest_.add_source(source, progress, &progress_.error, relative_root)) {
        fail(progress_.error); return false;
    }
    progress_.totals = manifest_.totals();
    return true;
}
bool TransferBatchSender::finish_prepare() {
    cancel_if_requested();
    if (progress_.state != TransferSenderState::kScanning) return false;
    if (!manifest_.finish_scan(&progress_.error)) { fail(progress_.error); return false; }
    progress_.totals = manifest_.totals(); progress_.state = TransferSenderState::kPrepared;
    return true;
}
bool TransferBatchSender::begin(std::string epoch, std::string operation, protocol::TransferConflictV1 conflict) {
    cancel_if_requested();
    if (progress_.state != TransferSenderState::kPrepared) return false;
    epoch_ = std::move(epoch); operation_ = std::move(operation); conflict_ = conflict;
    auto identity = message(TransferMessageTypeV1::kFinish); identity.entry_id = 0;
    if (!protocol::validate_transfer_message_v1(identity, &progress_.error)) { fail(progress_.error); return false; }
    progress_.state = TransferSenderState::kSending;
    return true;
}
protocol::TransferMessageV1 TransferBatchSender::message(TransferMessageTypeV1 type) const {
    protocol::TransferMessageV1 result;
    result.type = type; result.session_epoch = epoch_; result.operation_id = operation_;
    result.sequence = sequence_ + 1; result.entry_id = entry_id_; result.conflict = conflict_;
    return result;
}
void TransferBatchSender::fail(std::string error) {
    source_.close(); manifest_.close(); pending_.reset();
    progress_.state = error == "transfer_cancelled" ? TransferSenderState::kCancelled : TransferSenderState::kFailed;
    progress_.error = std::move(error);
}
void TransferBatchSender::cancel_if_requested() {
    if (!cancelled_.load(std::memory_order_acquire) || progress_.state == TransferSenderState::kComplete
        || progress_.state == TransferSenderState::kFailed || progress_.state == TransferSenderState::kCancelled) return;
    source_.close(); pending_.reset(); manifest_.close();
    progress_.state = TransferSenderState::kCancelled; progress_.error = "transfer_cancelled";
}
void TransferBatchSender::pump(const Send& send) {
    cancel_if_requested();
    if (progress_.state != TransferSenderState::kSending || !send) return;
    for (unsigned count = 0; count < 16; ++count) {
        cancel_if_requested();
        if (progress_.state != TransferSenderState::kSending) return;
        if (!pending_) {
            if (sequence_ == std::numeric_limits<std::uint64_t>::max()) { fail("transfer_sequence_exhausted"); return; }
            switch (phase_) {
            case Phase::kEntry: {
                std::string error;
                const auto state = manifest_.next(&entry_, &error);
                if (state == TransferManifestRead::kFailed) { fail(std::move(error)); return; }
                if (state == TransferManifestRead::kEnd) { phase_ = Phase::kFinish; continue; }
                ++entry_id_; sent_offset_ = acknowledged_offset_ = first_data_sequence_ = 0; hash_.clear();
                progress_.current_path = entry_.relative_path;
                pending_ = message(TransferMessageTypeV1::kEntry);
                pending_->relative_path = entry_.relative_path; pending_->size = entry_.size; pending_->directory = entry_.directory;
                break;
            }
            case Phase::kData: {
                const auto wanted = std::min<std::uint64_t>(protocol::kMaxTransferChunkBytes, entry_.size - sent_offset_);
                if (sent_offset_ - acknowledged_offset_ + wanted > kMaxInFlightBytes) return;
                const auto data = source_.read(protocol::kMaxTransferChunkBytes);
                if (data.state == TransferReadState::kFailed) { fail(data.error); return; }
                if (data.state == TransferReadState::kComplete) {
                    hash_ = data.sha256; source_.close();
                    phase_ = acknowledged_offset_ == entry_.size ? Phase::kCommit : Phase::kWaitData;
                    continue;
                }
                pending_ = message(TransferMessageTypeV1::kChunk); pending_->offset = data.offset;
                pending_->bytes.assign(reinterpret_cast<const char*>(data.bytes.data()), data.bytes.size());
                break;
            }
            case Phase::kCommit:
                pending_ = message(TransferMessageTypeV1::kCommit); pending_->sha256 = hash_; break;
            case Phase::kFinish:
                pending_ = message(TransferMessageTypeV1::kFinish); pending_->entry_id = 0; break;
            default: return;
            }
        }
        if (!send(*pending_)) return; // keep exactly the prepared bytes for writable notification
        sequence_ = pending_->sequence;
        switch (pending_->type) {
        case TransferMessageTypeV1::kEntry: phase_ = Phase::kWaitEntry; break;
        case TransferMessageTypeV1::kChunk:
            if (!first_data_sequence_) first_data_sequence_ = sequence_;
            sent_offset_ += pending_->bytes.size(); progress_.submitted_bytes += pending_->bytes.size(); break;
        case TransferMessageTypeV1::kCommit: phase_ = Phase::kWaitCommit; break;
        case TransferMessageTypeV1::kFinish: phase_ = Phase::kWaitFinish; break;
        default: break;
        }
        pending_.reset();
    }
}
bool TransferBatchSender::receive(const protocol::TransferMessageV1& receipt) {
    cancel_if_requested();
    if (progress_.state != TransferSenderState::kSending || receipt.session_epoch != epoch_
        || receipt.operation_id != operation_) return false;
    std::string error;
    if (!protocol::validate_transfer_message_v1(receipt, &error)) { fail(std::move(error)); return false; }
    if (receipt.sequence <= acknowledged_sequence_) return true;
    const bool finished = phase_ == Phase::kWaitFinish;
    if (receipt.sequence > sequence_ || receipt.entry_id != (finished ? 0 : entry_id_)
        || receipt.type != (finished ? TransferMessageTypeV1::kFinished : TransferMessageTypeV1::kReceipt)) {
        fail("transfer_invalid_receipt"); return false;
    }
    if (receipt.disposition == TransferDispositionV1::kFailed) {
        fail(receipt.error_code.empty() ? "transfer_peer_failed" : receipt.error_code); return true;
    }
    switch (phase_) {
    case Phase::kWaitEntry:
        if (receipt.sequence != sequence_) { fail("transfer_invalid_entry_receipt"); return false; }
        if (receipt.disposition == TransferDispositionV1::kSkipped
            || (entry_.directory && receipt.disposition == TransferDispositionV1::kCommitted)) {
            ++progress_.completed_entries;
            if (!entry_.directory) ++progress_.completed_files;
            if (receipt.disposition == TransferDispositionV1::kSkipped) ++progress_.skipped_entries;
            last_result_ = TransferResultEntry{entry_.relative_path, entry_.size, entry_.directory,
                receipt.disposition == TransferDispositionV1::kSkipped};
            phase_ = Phase::kEntry;
        } else if (!entry_.directory && receipt.disposition == TransferDispositionV1::kReceiving) {
            if (!source_.open(entry_.source, &error)) { fail(std::move(error)); return false; }
            if (source_.size() != entry_.size) { fail("transfer_source_changed_after_scan"); return false; }
            phase_ = Phase::kData;
        } else { fail("transfer_invalid_entry_disposition"); return false; }
        break;
    case Phase::kData:
    case Phase::kWaitData: {
        if (!first_data_sequence_ || receipt.sequence < first_data_sequence_
            || receipt.disposition != TransferDispositionV1::kReceiving) { fail("transfer_invalid_chunk_receipt"); return false; }
        const auto chunks = receipt.sequence - first_data_sequence_ + 1;
        if (chunks > std::numeric_limits<std::uint64_t>::max() / protocol::kMaxTransferChunkBytes) {
            fail("transfer_invalid_chunk_receipt"); return false;
        }
        const auto expected = std::min<std::uint64_t>(entry_.size, chunks * protocol::kMaxTransferChunkBytes);
        if (receipt.offset != expected || receipt.offset > sent_offset_ || receipt.offset < acknowledged_offset_) {
            fail("transfer_invalid_chunk_receipt"); return false;
        }
        progress_.acknowledged_bytes += receipt.offset - acknowledged_offset_; acknowledged_offset_ = receipt.offset;
        if (phase_ == Phase::kWaitData && acknowledged_offset_ == entry_.size) phase_ = Phase::kCommit;
        break;
    }
    case Phase::kWaitCommit:
        if (receipt.sequence != sequence_ || (receipt.disposition != TransferDispositionV1::kCommitted
            && receipt.disposition != TransferDispositionV1::kSkipped)) { fail("transfer_invalid_commit_receipt"); return false; }
        ++progress_.completed_entries;
        ++progress_.completed_files;
        if (receipt.disposition == TransferDispositionV1::kSkipped) ++progress_.skipped_entries;
        else progress_.committed_bytes += entry_.size;
        last_result_ = TransferResultEntry{receipt.relative_path.empty() ? entry_.relative_path : receipt.relative_path,
            entry_.size, false, receipt.disposition == TransferDispositionV1::kSkipped};
        phase_ = Phase::kEntry; break;
    case Phase::kWaitFinish:
        if (receipt.sequence != sequence_ || receipt.disposition != TransferDispositionV1::kCommitted
            || progress_.completed_entries != progress_.totals.entries) { fail("transfer_invalid_finish_receipt"); return false; }
        manifest_.close(); progress_.state = TransferSenderState::kComplete; break;
    default: fail("transfer_unexpected_receipt"); return false;
    }
    acknowledged_sequence_ = receipt.sequence;
    return true;
}
void TransferBatchSender::request_cancel() noexcept {
    cancelled_.store(true, std::memory_order_release); manifest_.request_cancel();
}
}
