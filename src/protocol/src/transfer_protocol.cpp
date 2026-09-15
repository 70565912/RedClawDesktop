#include "redclaw/protocol/transfer_protocol.h"
#include "redclaw/protocol/compressed_protobuf.h"
#include "redclaw_wire.pb.h"
#include <algorithm>
#include <limits>

namespace redclaw::protocol {
namespace {
bool token(std::string_view value) {
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
            || ch == '-' || ch == '_' || ch == '.' || ch == ':';
    });
}
bool reject(std::string* error, const char* code) { if (error) *error = code; return false; }
}
bool validate_workspace_control_v1(const WorkspaceControlV1& m, std::string* error) {
    if (m.schema_version != 1) return reject(error, "protocol_version_incompatible");
    if (m.action < WorkspaceActionV1::kPrepare || m.action > WorkspaceActionV1::kBrowseClipboardCopies
        || m.direction < TransferDirectionV1::kToHost || m.direction > TransferDirectionV1::kToController
        || m.purpose < WorkspaceTransferPurposeV1::kFiles || m.purpose > WorkspaceTransferPurposeV1::kClipboardOpenCopy
        || m.conflict < TransferConflictV1::kKeepBoth || m.conflict > TransferConflictV1::kSkip)
        return reject(error, "workspace_unknown_value");
    const bool clipboard = m.purpose == WorkspaceTransferPurposeV1::kClipboard;
    const bool copy_action = m.purpose == WorkspaceTransferPurposeV1::kClipboardCleanup || m.purpose == WorkspaceTransferPurposeV1::kClipboardOpenCopy;
    if (((clipboard || copy_action) && m.direction != TransferDirectionV1::kToHost)
        || (m.clipboard_sequence && (!clipboard || m.action != WorkspaceActionV1::kPrepare))
        || (m.paste_submitted && (!clipboard || m.action != WorkspaceActionV1::kFinished))
        || (clipboard && m.action == WorkspaceActionV1::kPrepare && !m.path.empty()))
        return reject(error, "workspace_invalid_clipboard_fields");
    if (copy_action && m.action == WorkspaceActionV1::kPrepare
        && (m.path.size() != 32 || !std::all_of(m.path.begin(), m.path.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        }))) return reject(error, "clipboard_copy_id_invalid");
    if (m.created_at_ms && (m.action != WorkspaceActionV1::kBrowseEntry
        || m.created_at_ms > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        return reject(error, "workspace_invalid_copy_timestamp");
    if (m.action != WorkspaceActionV1::kAvailability && (m.active || m.operation_revision))
        return reject(error, "workspace_local_status_fields");
    if ((!m.results_path.empty() && m.action != WorkspaceActionV1::kFinished) || m.results_path.size() > 32760
        || std::any_of(m.results_path.begin(), m.results_path.end(), [](unsigned char ch) { return ch < 32; }))
        return reject(error, "workspace_invalid_results_path");
    const bool path_action = m.action == WorkspaceActionV1::kPrepare || m.action == WorkspaceActionV1::kSelectSource
        || m.action == WorkspaceActionV1::kBrowse || m.action == WorkspaceActionV1::kBrowseEntry
        || m.action == WorkspaceActionV1::kProgress;
    if (m.path.size() > 32760 || (!path_action && !m.path.empty())
        || std::any_of(m.path.begin(), m.path.end(), [](unsigned char ch) { return ch < 32; })
        || ((m.action == WorkspaceActionV1::kSelectSource || m.action == WorkspaceActionV1::kBrowseEntry) && m.path.empty()))
        return reject(error, "workspace_invalid_path");
    if (m.files > m.entries || m.completed_entries > m.entries || m.completed_bytes > m.bytes
        || m.committed_bytes > m.completed_bytes || m.completed_files > m.files || m.skipped_entries > m.completed_entries)
        return reject(error, "workspace_invalid_progress");
    if ((!m.error_code.empty() && !token(m.error_code))
        || (m.action == WorkspaceActionV1::kError && m.error_code.empty()))
        return reject(error, "workspace_invalid_error");
    if (error) error->clear();
    return true;
}
bool validate_transfer_message_v1(const TransferMessageV1& m, std::string* error) {
    if (m.schema_version != 1) return reject(error, "protocol_version_incompatible");
    if (!token(m.session_epoch) || !token(m.operation_id) || !m.sequence)
        return reject(error, "transfer_invalid_identity");
    if (m.type < TransferMessageTypeV1::kEntry || m.type > TransferMessageTypeV1::kFinished
        || m.conflict < TransferConflictV1::kKeepBoth || m.conflict > TransferConflictV1::kSkip
        || m.disposition < TransferDispositionV1::kReceiving || m.disposition > TransferDispositionV1::kFailed)
        return reject(error, "transfer_unknown_value");
    const bool end = m.type == TransferMessageTypeV1::kFinish || m.type == TransferMessageTypeV1::kFinished;
    if (end ? m.entry_id != 0 : m.entry_id == 0) return reject(error, "transfer_invalid_entry");
    const bool path = m.type == TransferMessageTypeV1::kEntry || m.type == TransferMessageTypeV1::kReceipt;
    if ((!path && !m.relative_path.empty()) || m.relative_path.size() > 32760
        || m.relative_path.find('\0') != std::string::npos
        || (m.type == TransferMessageTypeV1::kEntry && m.relative_path.empty()))
        return reject(error, "transfer_invalid_path");
    if (m.size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        || (m.directory && (m.type != TransferMessageTypeV1::kEntry || m.size)))
        return reject(error, "transfer_invalid_size");
    if (m.type == TransferMessageTypeV1::kChunk) {
        if (m.bytes.empty() || m.bytes.size() > kMaxTransferChunkBytes
            || m.offset > std::numeric_limits<std::uint64_t>::max() - m.bytes.size())
            return reject(error, "transfer_invalid_chunk");
    } else if (!m.bytes.empty()) return reject(error, "transfer_unexpected_data");
    if (m.type == TransferMessageTypeV1::kCommit) {
        if (m.sha256.size() != 64 || !std::all_of(m.sha256.begin(), m.sha256.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        })) return reject(error, "transfer_invalid_sha256");
    } else if (!m.sha256.empty()) return reject(error, "transfer_unexpected_sha256");
    if (!m.error_code.empty() && (!token(m.error_code)
        || (m.type != TransferMessageTypeV1::kReceipt && m.type != TransferMessageTypeV1::kFinished)))
        return reject(error, "transfer_invalid_error");
    if (error) error->clear();
    return true;
}
std::string serialize_transfer_message_v1(const TransferMessageV1& m) {
    if (!validate_transfer_message_v1(m)) return {};
    wire::TransferMessageV1 encoded;
    encoded.set_schema_version(m.schema_version); encoded.set_type(static_cast<std::uint32_t>(m.type));
    encoded.set_session_epoch(m.session_epoch); encoded.set_operation_id(m.operation_id);
    encoded.set_sequence(m.sequence); encoded.set_entry_id(m.entry_id);
    encoded.set_relative_path(m.relative_path); encoded.set_directory(m.directory);
    encoded.set_size(m.size); encoded.set_offset(m.offset);
    encoded.set_conflict(static_cast<std::uint32_t>(m.conflict));
    encoded.set_disposition(static_cast<std::uint32_t>(m.disposition));
    encoded.set_sha256(m.sha256); encoded.set_data(m.bytes); encoded.set_error_code(m.error_code);
    return compress_protobuf(encoded, ProtobufWireKind::kTransfer);
}
ParseResult<TransferMessageV1> parse_transfer_message_v1(std::string_view frame) {
    ParseResult<TransferMessageV1> result;
    wire::TransferMessageV1 encoded;
    if (!decompress_protobuf(frame, ProtobufWireKind::kTransfer, encoded, &result.error)) return result;
    if (encoded.schema_version() != 1) { result.error = "protocol_version_incompatible"; return result; }
    if (encoded.type() > static_cast<unsigned>(TransferMessageTypeV1::kFinished)
        || encoded.conflict() > static_cast<unsigned>(TransferConflictV1::kSkip)
        || encoded.disposition() > static_cast<unsigned>(TransferDispositionV1::kFailed)) {
        result.error = "transfer_unknown_value"; return result;
    }
    auto& m = result.value;
    m.schema_version = encoded.schema_version(); m.type = static_cast<TransferMessageTypeV1>(encoded.type());
    m.session_epoch = encoded.session_epoch(); m.operation_id = encoded.operation_id();
    m.sequence = encoded.sequence(); m.entry_id = encoded.entry_id();
    m.relative_path = encoded.relative_path(); m.directory = encoded.directory();
    m.size = encoded.size(); m.offset = encoded.offset();
    m.conflict = static_cast<TransferConflictV1>(encoded.conflict());
    m.disposition = static_cast<TransferDispositionV1>(encoded.disposition());
    m.sha256 = encoded.sha256(); m.bytes = encoded.data(); m.error_code = encoded.error_code();
    result.ok = validate_transfer_message_v1(m, &result.error);
    return result;
}
}
