#include <gtest/gtest.h>
#include "redclaw/protocol/transfer_protocol.h"
#include "redclaw/protocol/compressed_protobuf.h"
#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw_wire.pb.h"
#include <random>

namespace {
using namespace redclaw::protocol;
TEST(TransferProtocol, IncompressibleChunkFitsTransportAndKeeps64BitOffset) {
    TransferMessageV1 message;
    message.type = TransferMessageTypeV1::kChunk;
    message.session_epoch = "session-1"; message.operation_id = "transfer-1";
    message.sequence = 99; message.entry_id = 2; message.offset = 5ULL * 1024 * 1024 * 1024;
    std::mt19937 random(20260915);
    message.bytes.resize(kMaxTransferChunkBytes);
    for (auto& byte : message.bytes) byte = static_cast<char>(random() & 255);
    const auto frame = serialize_transfer_message_v1(message);
    ASSERT_FALSE(frame.empty());
    EXPECT_LE(frame.size(), kMaxProtobufWireBytes);
    const auto parsed = parse_transfer_message_v1(frame);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.bytes, message.bytes); EXPECT_EQ(parsed.value.offset, message.offset);
    message.bytes += 'x';
    EXPECT_TRUE(serialize_transfer_message_v1(message).empty());
    EXPECT_FALSE(parse_transfer_message_v1(frame + "trailing").ok);
    auto corrupt = frame; corrupt.back() ^= 1;
    EXPECT_FALSE(parse_transfer_message_v1(corrupt).ok);
}
TEST(TransferProtocol, FutureOptionalFieldsAreToleratedAndVersionErrorsAreExplicit) {
    wire::TransferMessageV1 message;
    message.set_schema_version(1); message.set_type(static_cast<unsigned>(TransferMessageTypeV1::kEntry));
    message.set_sequence(1); message.set_entry_id(1); message.set_session_epoch("session");
    message.set_operation_id("operation"); message.set_relative_path("folder/file.txt");
    message.set_size(6ULL * 1024 * 1024 * 1024);
    message.GetReflection()->MutableUnknownFields(&message)->AddLengthDelimited(100, "optional");
    auto parsed = parse_transfer_message_v1(compress_protobuf(message, ProtobufWireKind::kTransfer));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.size, 6ULL * 1024 * 1024 * 1024);
    message.set_schema_version(2);
    parsed = parse_transfer_message_v1(compress_protobuf(message, ProtobufWireKind::kTransfer));
    EXPECT_FALSE(parsed.ok); EXPECT_EQ(parsed.error, "protocol_version_incompatible");
    message.set_schema_version(1); message.set_conflict(999);
    EXPECT_FALSE(parse_transfer_message_v1(compress_protobuf(message, ProtobufWireKind::kTransfer)).ok);
    message.set_conflict(0); message.set_data("smuggled");
    EXPECT_FALSE(parse_transfer_message_v1(compress_protobuf(message, ProtobufWireKind::kTransfer)).ok);
    message.clear_data();
    EXPECT_FALSE(parse_transfer_message_v1(compress_protobuf(message, ProtobufWireKind::kTerminal)).ok);
}

TEST(TransferProtocol, PriorityCommandsUseControlEpochGuardAndMissingCapabilityIsDisabled) {
    StreamControlMessageV1 control;
    control.type = StreamControlMessageTypeV1::kCapabilities;
    control.session_epoch = "session"; control.message_id = 1; control.sent_at_ms = 1;
    auto parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(control));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.file_transfer_version, 0U);
    control.file_transfer_version = 1;
    parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(control));
    ASSERT_TRUE(parsed.ok); EXPECT_EQ(parsed.value.file_transfer_version, 1U);
    control.type = StreamControlMessageTypeV1::kWorkspace;
    control.request_id = "operation"; control.workspace.emplace();
    control.workspace->action = WorkspaceActionV1::kCancel;
    parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(control));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_TRUE(parsed.value.workspace); EXPECT_EQ(parsed.value.workspace->action, WorkspaceActionV1::kCancel);
    StreamControlEpochGuardV1 guard;
    auto hello = control; hello.type = StreamControlMessageTypeV1::kHello; hello.workspace.reset();
    ASSERT_TRUE(guard.accept(hello));
    parsed.value.message_id = 2;
    ASSERT_TRUE(guard.accept(parsed.value));
    EXPECT_FALSE(guard.accept(parsed.value));
    control.workspace->schema_version = 2;
    parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(control));
    EXPECT_FALSE(parsed.ok); EXPECT_EQ(parsed.error, "protocol_version_incompatible");
    control.workspace->schema_version = 1; control.workspace->path = "must not accompany cancel";
    EXPECT_FALSE(parse_stream_control_message_v1(serialize_stream_control_message_v1(control)).ok);
    control.workspace->path.clear(); control.payload = "file data does not belong on this command";
    EXPECT_FALSE(parse_stream_control_message_v1(serialize_stream_control_message_v1(control)).ok);
}
TEST(TransferProtocol, ProgressSeparatesReceivedBytesFromVerifiedFiles) {
    StreamControlMessageV1 control;
    control.type = StreamControlMessageTypeV1::kWorkspace; control.session_epoch = "session";
    control.message_id = 1; control.sent_at_ms = 1; control.request_id = "operation";
    auto& progress = control.workspace.emplace(); progress.action = WorkspaceActionV1::kProgress;
    progress.path = "folder/file.bin"; progress.entries = 3; progress.files = 2;
    progress.bytes = 8ULL * 1024 * 1024 * 1024; progress.completed_bytes = progress.bytes - 123;
    progress.committed_bytes = 1024; progress.completed_entries = 2; progress.completed_files = 1;
    const auto parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(control));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.workspace->path, progress.path);
    EXPECT_EQ(parsed.value.workspace->completed_bytes, progress.completed_bytes);
    EXPECT_EQ(parsed.value.workspace->committed_bytes, 1024U);
    EXPECT_EQ(parsed.value.workspace->completed_files, 1U);
    progress.committed_bytes = progress.completed_bytes + 1;
    EXPECT_FALSE(validate_workspace_control_v1(progress));
    progress.committed_bytes = 0; progress.completed_files = 3;
    EXPECT_FALSE(validate_workspace_control_v1(progress));
}
TEST(TransferProtocol, LocalFinishedRecordCarriesResultJournalOnlyInTheFinishedAction) {
    StreamControlMessageV1 control;
    control.type = StreamControlMessageTypeV1::kWorkspace; control.session_epoch = "session";
    control.message_id = 1; control.sent_at_ms = 1; control.request_id = "operation";
    auto& result = control.workspace.emplace(); result.action = WorkspaceActionV1::kFinished;
    result.results_path = "C:/owned/transfer-results-fixture.rctr";
    const auto parsed = parse_local_runtime_control_frame_v2(serialize_local_runtime_control_frame_v2(control));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.workspace->results_path, result.results_path);
    result.action = WorkspaceActionV1::kPrepare;
    EXPECT_FALSE(validate_workspace_control_v1(result));
}
TEST(TransferProtocol, ClipboardCapabilityAndOwnerSequenceAreOptionalAndPasteReceiptIsTyped) {
    StreamControlMessageV1 control;
    control.type = StreamControlMessageTypeV1::kWorkspace; control.session_epoch = "session";
    control.message_id = 1; control.sent_at_ms = 1; control.request_id = "paste";
    control.clipboard_version = 1;
    auto& value = control.workspace.emplace(); value.purpose = WorkspaceTransferPurposeV1::kClipboard;
    value.clipboard_sequence = 42;
    auto parsed = parse_local_runtime_control_frame_v2(serialize_local_runtime_control_frame_v2(control));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.clipboard_version, 1U);
    EXPECT_EQ(parsed.value.workspace->clipboard_sequence, 42U);
    EXPECT_EQ(parsed.value.workspace->purpose, WorkspaceTransferPurposeV1::kClipboard);
    value.path = "C:/peer-supplied"; EXPECT_FALSE(validate_workspace_control_v1(value)); value.path.clear();
    value.direction = TransferDirectionV1::kToController; EXPECT_FALSE(validate_workspace_control_v1(value));
    value.direction = TransferDirectionV1::kToHost; value.action = WorkspaceActionV1::kFinished;
    EXPECT_FALSE(validate_workspace_control_v1(value)); value.clipboard_sequence = 0; value.paste_submitted = true;
    parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(control));
    ASSERT_TRUE(parsed.ok) << parsed.error; EXPECT_TRUE(parsed.value.workspace->paste_submitted);
    value.purpose = WorkspaceTransferPurposeV1::kFiles; EXPECT_FALSE(validate_workspace_control_v1(value));
}
TEST(TransferProtocol, ClipboardCopyListingAndOpaqueActionsRoundTripAndRejectPaths) {
    StreamControlMessageV1 control;
    control.type = StreamControlMessageTypeV1::kWorkspace; control.session_epoch = "session";
    control.message_id = 1; control.sent_at_ms = 1; control.request_id = "copies";
    control.clipboard_version = 2; auto& value = control.workspace.emplace();
    value.action = WorkspaceActionV1::kBrowseClipboardCopies;
    auto parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(control));
    ASSERT_TRUE(parsed.ok) << parsed.error; EXPECT_EQ(parsed.value.workspace->action, value.action);
    value.action = WorkspaceActionV1::kBrowseEntry; value.path = std::string(32, 'a'); value.created_at_ms = 1700000000000;
    parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(control));
    ASSERT_TRUE(parsed.ok) << parsed.error; EXPECT_EQ(parsed.value.workspace->created_at_ms, value.created_at_ms);
    value.created_at_ms = 0; value.action = WorkspaceActionV1::kPrepare;
    for (const auto purpose : {WorkspaceTransferPurposeV1::kClipboardCleanup, WorkspaceTransferPurposeV1::kClipboardOpenCopy}) {
        value.purpose = purpose;
        ASSERT_TRUE(parse_stream_control_message_v1(serialize_stream_control_message_v1(control)).ok);
        for (const auto path : {"../outside", "C:/Windows", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}) {
            value.path = path; EXPECT_FALSE(validate_workspace_control_v1(value));
        }
        value.path = std::string(32, 'a');
    }
}
}
