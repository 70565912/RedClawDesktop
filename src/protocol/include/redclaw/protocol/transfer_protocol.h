#pragma once
#include "redclaw/protocol/protocol_module.h"
#include <cstddef>

namespace redclaw::protocol {
// Leave room for the envelope and incompressible data within the existing
// 64 KiB transport-frame boundary. File offsets and totals remain 64 bit.
inline constexpr std::size_t kMaxTransferChunkBytes = 48U * 1024U;
enum class TransferConflictV1 { kKeepBoth, kOverwrite, kSkip };
// Carried in Control v1, sharing its epoch, message ID and replay guard. These
// commands/receipts have no file bytes and never queue on the bulk channel.
enum class WorkspaceActionV1 {
    kPrepare, kPrepared, kSelectSource, kSelectionComplete, kScanProgress,
    kOffer, kReady, kCancel, kFinished, kError, kBrowse, kBrowseEntry, kBrowseEnd,
    kSourceAccepted, kProgress, kAvailability, kBrowseClipboardCopies
};
enum class TransferDirectionV1 { kToHost, kToController };
enum class WorkspaceTransferPurposeV1 { kFiles, kClipboard, kClipboardCleanup, kClipboardOpenCopy };
struct WorkspaceControlV1 {
    int schema_version = 1;
    WorkspaceActionV1 action = WorkspaceActionV1::kPrepare;
    TransferDirectionV1 direction = TransferDirectionV1::kToHost;
    WorkspaceTransferPurposeV1 purpose = WorkspaceTransferPurposeV1::kFiles;
    std::string path;
    std::string error_code;
    std::uint64_t entries = 0, files = 0, bytes = 0;
    std::uint64_t completed_entries = 0, completed_bytes = 0;
    std::uint64_t committed_bytes = 0, completed_files = 0, skipped_entries = 0;
    bool directory = false;
    std::uint64_t accepted_sources = 0;
    // Local runtime -> GUI status only; never accepted as a peer instruction.
    bool active = false;
    std::uint64_t operation_revision = 0;
    std::string results_path;
    std::uint32_t clipboard_sequence = 0; // Controller GUI -> its runtime only
    bool paste_submitted = false; // Host's final clipboard result
    std::uint64_t created_at_ms = 0; // clipboard-copy listing only
    TransferConflictV1 conflict = TransferConflictV1::kKeepBoth;
    std::uint32_t clipboard_mode = 0; // 0 legacy, 1 export, 2 publish, 3 paste only
    std::string clipboard_source; // owner-local protobuf descriptor, never sent to peer
    std::string snapshot_path; // owner-local verified result reference
};
[[nodiscard]] bool validate_workspace_control_v1(const WorkspaceControlV1&, std::string* error = nullptr);
enum class TransferMessageTypeV1 { kEntry, kChunk, kCommit, kReceipt, kFinish, kFinished };
enum class TransferDispositionV1 { kReceiving, kSkipped, kCommitted, kFailed };
struct TransferMessageV1 {
    int schema_version = 1;
    TransferMessageTypeV1 type = TransferMessageTypeV1::kEntry;
    std::string session_epoch;
    std::string operation_id;
    std::uint64_t sequence = 0;
    std::uint64_t entry_id = 0;
    std::string relative_path;
    bool directory = false;
    std::uint64_t size = 0;
    std::uint64_t offset = 0;
    TransferConflictV1 conflict = TransferConflictV1::kKeepBoth;
    TransferDispositionV1 disposition = TransferDispositionV1::kReceiving;
    std::string sha256;
    std::string bytes;
    std::string error_code;
};
[[nodiscard]] bool validate_transfer_message_v1(const TransferMessageV1&, std::string* error = nullptr);
[[nodiscard]] std::string serialize_transfer_message_v1(const TransferMessageV1&);
[[nodiscard]] ParseResult<TransferMessageV1> parse_transfer_message_v1(std::string_view);
}
