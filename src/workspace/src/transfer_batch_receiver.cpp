#include "redclaw/workspace/transfer_batch_receiver.h"
#include <limits>

namespace redclaw::workspace {
using protocol::TransferMessageV1;
using protocol::TransferMessageTypeV1;
using protocol::TransferDispositionV1;
namespace {
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
TransferConflict conflict(protocol::TransferConflictV1 value) {
    switch (value) {
    case protocol::TransferConflictV1::kOverwrite: return TransferConflict::kOverwrite;
    case protocol::TransferConflictV1::kSkip: return TransferConflict::kSkip;
    default: return TransferConflict::kKeepBoth;
    }
}
}
bool TransferBatchReceiver::begin(const std::filesystem::path& destination, std::string epoch,
    std::string operation, std::uint64_t entries, std::uint64_t bytes, std::string* error) {
    if (progress_.state != TransferBatchState::kIdle || cancelled_.load(std::memory_order_acquire)) {
        if (error) *error = "transfer_batch_already_started_or_cancelled";
        return false;
    }
    TransferMessageV1 identity;
    identity.type = TransferMessageTypeV1::kFinish; identity.sequence = 1;
    identity.session_epoch = epoch; identity.operation_id = operation;
    if (!protocol::validate_transfer_message_v1(identity, error) || (!entries && bytes)) {
        if (error && error->empty()) *error = "transfer_invalid_totals";
        return false;
    }
    if (!file_.begin_batch(destination, operation, error)) return false;
    epoch_ = std::move(epoch); operation_ = std::move(operation);
    progress_.state = TransferBatchState::kReceiving;
    progress_.total_entries = entries; progress_.total_bytes = bytes;
    if (error) error->clear();
    return true;
}
TransferMessageV1 TransferBatchReceiver::receipt(const TransferMessageV1& message,
    TransferDispositionV1 disposition, std::string error) {
    TransferMessageV1 result;
    result.type = message.type == TransferMessageTypeV1::kFinish
        ? TransferMessageTypeV1::kFinished : TransferMessageTypeV1::kReceipt;
    result.session_epoch = epoch_; result.operation_id = operation_;
    result.sequence = message.sequence; result.entry_id = message.entry_id;
    result.disposition = disposition; result.error_code = std::move(error);
    return result;
}
void TransferBatchReceiver::fail(std::string error) {
    std::string cleanup;
    if (!file_.end_batch(&cleanup)) error = std::move(cleanup);
    progress_.state = TransferBatchState::kFailed; progress_.error = std::move(error);
}
std::optional<TransferMessageV1> TransferBatchReceiver::accept(const TransferMessageV1& message) {
    if (cancelled_.load(std::memory_order_acquire)) finish_cancel();
    if (progress_.state != TransferBatchState::kReceiving || message.session_epoch != epoch_
        || message.operation_id != operation_) return std::nullopt;
    std::string error;
    // Stale identity/sequence never advances this batch or replays a commit.
    if (!protocol::validate_transfer_message_v1(message, &error)) { fail(std::move(error)); return std::nullopt; }
    if (message.sequence <= sequence_) return std::nullopt;
    if (sequence_ == std::numeric_limits<std::uint64_t>::max() || message.sequence != sequence_ + 1) {
        fail("transfer_sequence_gap");
        return receipt(message, TransferDispositionV1::kFailed, progress_.error);
    }
    sequence_ = message.sequence;
    const auto reject = [&](std::string reason) {
        fail(std::move(reason)); return receipt(message, TransferDispositionV1::kFailed, progress_.error);
    };
    switch (message.type) {
    case TransferMessageTypeV1::kEntry: {
        if (file_.receiving() || entry_id_ >= progress_.total_entries || message.entry_id != entry_id_ + 1
            || message.size > progress_.total_bytes - declared_bytes_) return reject("transfer_entry_out_of_order");
        ++entry_id_; declared_bytes_ += message.size; file_size_ = message.size;
        progress_.current_path = message.relative_path;
        if (message.directory) {
            if (!file_.directory(message.relative_path, &error)) return reject(std::move(error));
            ++progress_.completed_entries;
            last_result_ = TransferResultEntry{message.relative_path, 0, true, false};
            return receipt(message, TransferDispositionV1::kCommitted);
        }
        const auto opened = file_.begin_file(message.relative_path, message.size, conflict(message.conflict));
        if (opened.result == TransferEntryResult::kFailed) return reject(opened.error);
        if (opened.result == TransferEntryResult::kSkipped) {
            ++progress_.skipped_entries; ++progress_.completed_entries;
            ++progress_.completed_files;
            last_result_ = TransferResultEntry{message.relative_path, message.size, false, true};
            return receipt(message, TransferDispositionV1::kSkipped);
        }
        return receipt(message, TransferDispositionV1::kReceiving);
    }
    case TransferMessageTypeV1::kChunk: {
        if (!file_.receiving() || message.entry_id != entry_id_) return reject("transfer_chunk_without_entry");
        if (!file_.write(message.offset,
            {reinterpret_cast<const std::uint8_t*>(message.bytes.data()), message.bytes.size()}, &error))
            return reject(std::move(error));
        progress_.received_bytes += message.bytes.size();
        auto result = receipt(message, TransferDispositionV1::kReceiving);
        result.offset = file_.received_bytes(); return result;
    }
    case TransferMessageTypeV1::kCommit: {
        if (!file_.receiving() || message.entry_id != entry_id_) return reject("transfer_commit_without_entry");
        const auto committed = file_.commit(message.sha256);
        if (committed.result == TransferEntryResult::kFailed) return reject(committed.error);
        ++progress_.completed_entries;
        ++progress_.completed_files;
        const bool skipped = committed.result == TransferEntryResult::kSkipped;
        if (skipped) ++progress_.skipped_entries; else progress_.committed_bytes += file_size_;
        auto result = receipt(message, skipped ? TransferDispositionV1::kSkipped : TransferDispositionV1::kCommitted);
        result.relative_path = utf8(committed.relative_path); result.offset = committed.bytes;
        last_result_ = TransferResultEntry{result.relative_path.empty() ? progress_.current_path : result.relative_path,
            file_size_, false, skipped};
        return result;
    }
    case TransferMessageTypeV1::kFinish:
        if (file_.receiving() || progress_.completed_entries != progress_.total_entries
            || declared_bytes_ != progress_.total_bytes) return reject("transfer_incomplete_batch");
        if (!file_.end_batch(&error)) return reject(std::move(error));
        progress_.state = TransferBatchState::kComplete;
        return receipt(message, TransferDispositionV1::kCommitted);
    default: return reject("transfer_wrong_direction");
    }
}
void TransferBatchReceiver::request_cancel() noexcept { cancelled_.store(true, std::memory_order_release); }
void TransferBatchReceiver::finish_cancel() {
    cancelled_.store(true, std::memory_order_release);
    if (progress_.state == TransferBatchState::kComplete || progress_.state == TransferBatchState::kFailed
        || progress_.state == TransferBatchState::kCancelled) return;
    std::string error;
    if (!file_.end_batch(&error)) { progress_.state = TransferBatchState::kFailed; progress_.error = std::move(error); }
    else { progress_.state = TransferBatchState::kCancelled; progress_.error = "transfer_cancelled"; }
}
}
