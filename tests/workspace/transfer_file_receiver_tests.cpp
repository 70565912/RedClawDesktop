#include <gtest/gtest.h>
#include "redclaw/workspace/transfer_file_receiver.h"
#include "redclaw/workspace/transfer_file_source.h"
#include "redclaw/workspace/transfer_batch_receiver.h"
#include "redclaw/workspace/transfer_source_manifest.h"
#include "redclaw/workspace/transfer_batch_sender.h"
#include <array>
#include <deque>
#include <fstream>
#include <openssl/evp.h>
#include <openssl/rand.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
using namespace redclaw::workspace;
class TemporaryTree {
public:
    std::filesystem::path root;
    TemporaryTree() {
        unsigned char random[8]{};
        if (RAND_bytes(random, sizeof(random)) != 1) throw std::runtime_error("fixture_random_failed");
        std::string name = "redclaw-transfer-test-";
        for (auto byte : random) name += std::to_string(byte);
        root = std::filesystem::canonical(std::filesystem::temp_directory_path()) / name;
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("fixture_directory_failed");
    }
    ~TemporaryTree() {
        std::error_code error;
        const auto resolved = std::filesystem::canonical(root, error);
        if (!error && resolved == root && root.filename().string().starts_with("redclaw-transfer-test-"))
            std::filesystem::remove_all(resolved, error);
    }
};
std::string sha256(std::string_view bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned length = 0;
    EXPECT_EQ(EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr), 1);
    std::string hash;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < length; ++i) { hash += hex[digest[i] >> 4]; hash += hex[digest[i] & 15]; }
    return hash;
}
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
bool write(TransferFileReceiver& receiver, std::string_view bytes) {
    std::string error;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = std::min<std::size_t>(64 * 1024, bytes.size() - offset);
        if (!receiver.write(offset, {reinterpret_cast<const std::uint8_t*>(bytes.data() + offset), count}, &error)) {
            ADD_FAILURE() << error; return false;
        }
        offset += count;
    }
    return true;
}
std::size_t partials(const std::filesystem::path& root) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        if (entry.path().extension() == ".partial") ++count;
    return count;
}
TEST(TransferFileReceiver, PathsExcludeTraversalDevicesAndAlternateStreams) {
    std::filesystem::path path;
    for (const auto* text : {"../escape", "a/../b", "C:/Windows/file", "/absolute", "\\\\server\\share", "a//b",
        "a\\..\\b", "NUL", "aux.txt", "COM1.log", "CONIN$", "CONOUT$.log", "COM\xc2\xb9.txt", "LPT\xc2\xb3",
        "a:file", "trailing.", "trailing ", "a?b", "a/./b"}) {
        EXPECT_FALSE(validate_transfer_relative_path(text, &path)) << text;
    }
    EXPECT_TRUE(validate_transfer_relative_path("folder/empty.txt", &path));
    EXPECT_EQ(path, std::filesystem::path("folder") / "empty.txt");
    EXPECT_TRUE(validate_transfer_relative_path("\xe4\xb8\xad\xe6\x96\x87/hello.txt", &path));
}
TEST(TransferFileReceiver, HashCommitConflictAndCancellationPreserveCompleteFiles) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows receiver.";
#else
    TemporaryTree tree;
    TransferFileReceiver receiver;
    std::string error;
    ASSERT_TRUE(receiver.begin_batch(tree.root, "owned-batch", &error)) << error;
    ASSERT_TRUE(receiver.directory("folder/empty", &error)) << error;
    ASSERT_TRUE(std::filesystem::is_directory(tree.root / "folder/empty"));
    std::string bytes(1024 * 1024 + 17, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i % 251);
    ASSERT_EQ(receiver.begin_file("folder/data.bin", bytes.size(), TransferConflict::kKeepBoth).result, TransferEntryResult::kReceiving);
    ASSERT_TRUE(write(receiver, bytes));
    EXPECT_FALSE(std::filesystem::exists(tree.root / "folder/data.bin"));
    auto committed = receiver.commit(sha256(bytes));
    ASSERT_EQ(committed.result, TransferEntryResult::kCommitted) << committed.error;
    EXPECT_EQ(read(tree.root / committed.relative_path), bytes);
    EXPECT_EQ(partials(tree.root), 0U);
    ASSERT_EQ(receiver.begin_file("folder/data.bin", 3, TransferConflict::kOverwrite).result, TransferEntryResult::kReceiving);
    ASSERT_TRUE(write(receiver, "new"));
    EXPECT_EQ(receiver.commit(sha256("bad")).error, "transfer_sha256_mismatch");
    EXPECT_EQ(read(tree.root / "folder/data.bin"), bytes);
    EXPECT_EQ(partials(tree.root), 0U);
    ASSERT_EQ(receiver.begin_file("folder/data.bin", 3, TransferConflict::kOverwrite).result, TransferEntryResult::kReceiving);
    ASSERT_TRUE(write(receiver, "new")); receiver.cancel_file();
    EXPECT_EQ(read(tree.root / "folder/data.bin"), bytes);
    EXPECT_EQ(partials(tree.root), 0U);
    ASSERT_EQ(receiver.begin_file("folder/data.bin", 3, TransferConflict::kOverwrite).result, TransferEntryResult::kReceiving);
    ASSERT_TRUE(write(receiver, "new"));
    ASSERT_EQ(receiver.commit(sha256("new")).result, TransferEntryResult::kCommitted);
    EXPECT_EQ(read(tree.root / "folder/data.bin"), "new");
    EXPECT_EQ(receiver.begin_file("folder/data.bin", 3, TransferConflict::kSkip).result, TransferEntryResult::kSkipped);
    EXPECT_FALSE(receiver.receiving());
    ASSERT_EQ(receiver.begin_file("folder/data.bin", 4, TransferConflict::kKeepBoth).result, TransferEntryResult::kReceiving);
    ASSERT_TRUE(write(receiver, "copy"));
    committed = receiver.commit(sha256("copy"));
    ASSERT_EQ(committed.result, TransferEntryResult::kCommitted) << committed.error;
    EXPECT_NE(committed.relative_path, std::filesystem::path("folder/data.bin"));
    EXPECT_EQ(read(tree.root / committed.relative_path), "copy");
    EXPECT_EQ(read(tree.root / "folder/data.bin"), "new");
    ASSERT_EQ(receiver.begin_file("folder/empty.bin", 0, TransferConflict::kKeepBoth).result, TransferEntryResult::kReceiving);
    ASSERT_EQ(receiver.commit(sha256("")).result, TransferEntryResult::kCommitted);
    EXPECT_EQ(std::filesystem::file_size(tree.root / "folder/empty.bin"), 0U);
#endif
}
TEST(TransferFileReceiver, LargeOffsetsAndOccupiedTargetsFailWithoutReplacingOriginal) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows receiver.";
#else
    TemporaryTree tree;
    TransferFileReceiver receiver;
    ASSERT_TRUE(receiver.begin_batch(tree.root, "large-and-occupied"));
    const auto started = receiver.begin_file("large.bin", 5ULL * 1024 * 1024 * 1024, TransferConflict::kKeepBoth);
    ASSERT_EQ(started.result, TransferEntryResult::kReceiving) << started.error;
    std::uint8_t byte = 1;
    EXPECT_FALSE(receiver.write(1ULL << 32, {&byte, 1}));
    EXPECT_TRUE(receiver.write(0, {&byte, 1}));
    EXPECT_EQ(receiver.commit(sha256(std::string(1, '\1'))).error, "transfer_size_mismatch");
    EXPECT_FALSE(std::filesystem::exists(tree.root / "large.bin"));
    EXPECT_EQ(partials(tree.root), 0U);
    { std::ofstream original(tree.root / "occupied.txt"); original << "original"; }
    const auto occupied = CreateFileW((tree.root / "occupied.txt").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(occupied, INVALID_HANDLE_VALUE);
    const auto pending = receiver.begin_file("occupied.txt", 3, TransferConflict::kOverwrite);
    EXPECT_EQ(pending.result, TransferEntryResult::kReceiving);
    if (pending.result == TransferEntryResult::kReceiving && write(receiver, "new")) {
        const auto result = receiver.commit(sha256("new"));
        EXPECT_EQ(result.result, TransferEntryResult::kFailed);
        EXPECT_TRUE(result.error.starts_with("transfer_commit_failed:"));
    }
    CloseHandle(occupied);
    EXPECT_EQ(read(tree.root / "occupied.txt"), "original");
    EXPECT_EQ(partials(tree.root), 0U);
#endif
}

TEST(TransferFileSource, UnicodeFileStreamsToVerifiedDestinationAndExcludesOrdinaryWriters) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows source.";
#else
    TemporaryTree tree;
    const auto source_path = tree.root / L"\u6e90\u6570\u636e.bin";
    std::string bytes(3 * 64 * 1024 + 117, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i % 239);
    { std::ofstream source(source_path, std::ios::binary); source.write(bytes.data(), bytes.size()); }
    TransferFileSource source;
    TransferFileReceiver destination;
    std::string error;
    ASSERT_TRUE(source.open(source_path, &error)) << error;
    EXPECT_EQ(source.size(), bytes.size());
    const auto writer = CreateFileW(source_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    ASSERT_TRUE(destination.begin_batch(tree.root, "source-to-destination"));
    const auto entry = destination.begin_file("\xe4\xb8\xad\xe6\x96\x87/copy.bin", source.size(), TransferConflict::kKeepBoth);
    ASSERT_EQ(entry.result, TransferEntryResult::kReceiving) << entry.error;
    const std::uint8_t* buffer = nullptr;
    TransferFileResult committed;
    while (true) {
        const auto chunk = source.read();
        ASSERT_NE(chunk.state, TransferReadState::kFailed) << chunk.error;
        if (chunk.state == TransferReadState::kComplete) {
            EXPECT_EQ(chunk.sha256, sha256(bytes));
            committed = destination.commit(chunk.sha256); break;
        }
        if (!buffer) buffer = chunk.bytes.data();
        EXPECT_EQ(buffer, chunk.bytes.data());
        EXPECT_LE(chunk.bytes.size(), 64U * 1024U);
        ASSERT_TRUE(destination.write(chunk.offset, chunk.bytes, &error)) << error;
    }
    ASSERT_EQ(committed.result, TransferEntryResult::kCommitted) << committed.error;
    EXPECT_EQ(read(tree.root / committed.relative_path), bytes);
    source.close();
    const auto active_writer = CreateFileW(source_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(active_writer, INVALID_HANDLE_VALUE);
    EXPECT_FALSE(source.open(source_path, &error));
    EXPECT_TRUE(error.starts_with("transfer_source_open:"));
    CloseHandle(active_writer);
    EXPECT_EQ(partials(tree.root), 0U);
#endif
}

TEST(TransferBatchReceiver, WireRoundTripCommitsFilesAndFoldersAndNeverReplaysCommit) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows receiver.";
#else
    using namespace redclaw::protocol;
    TemporaryTree tree;
    std::string bytes(9 * kMaxTransferChunkBytes + 157, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>((i * 113) % 251);
    const auto source_path = tree.root / L"source.bin";
    { std::ofstream source(source_path, std::ios::binary); source.write(bytes.data(), bytes.size()); }
    { std::ofstream existing(tree.root / "skip.txt"); existing << "original"; }
    TransferBatchReceiver batch;
    ASSERT_TRUE(batch.begin(tree.root, "session-1", "operation-1", 4, bytes.size() + 4));
    TransferMessageV1 message;
    message.session_epoch = "session-1"; message.operation_id = "operation-1";
    std::uint64_t sequence = 0;
    const auto deliver = [&]() {
        message.sequence = ++sequence;
        const auto parsed = parse_transfer_message_v1(serialize_transfer_message_v1(message));
        EXPECT_TRUE(parsed.ok) << parsed.error;
        const auto receipt = batch.accept(parsed.value);
        EXPECT_TRUE(receipt.has_value());
        if (!receipt) return TransferMessageV1{};
        const auto round_trip = parse_transfer_message_v1(serialize_transfer_message_v1(*receipt));
        EXPECT_TRUE(round_trip.ok) << round_trip.error;
        return round_trip.value;
    };
    message.entry_id = 1; message.relative_path = "empty/folder"; message.directory = true;
    EXPECT_EQ(deliver().disposition, TransferDispositionV1::kCommitted);
    message.entry_id = 2; message.relative_path = "\xe4\xb8\xad\xe6\x96\x87/data.bin";
    message.directory = false; message.size = bytes.size();
    EXPECT_EQ(deliver().disposition, TransferDispositionV1::kReceiving);
    message.relative_path.clear(); message.size = 0;
    TransferFileSource source;
    ASSERT_TRUE(source.open(source_path));
    while (true) {
        const auto data = source.read(kMaxTransferChunkBytes);
        ASSERT_NE(data.state, TransferReadState::kFailed) << data.error;
        if (data.state == TransferReadState::kComplete) {
            message.type = TransferMessageTypeV1::kCommit; message.bytes.clear();
            message.sha256 = data.sha256; message.offset = 0;
            const auto receipt = deliver();
            ASSERT_EQ(receipt.disposition, TransferDispositionV1::kCommitted) << receipt.error_code;
            EXPECT_EQ(read(tree.root / std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(receipt.relative_path.data()), receipt.relative_path.size()))), bytes);
            EXPECT_FALSE(batch.accept(message).has_value());
            EXPECT_EQ(batch.progress().completed_entries, 2U); break;
        }
        message.type = TransferMessageTypeV1::kChunk; message.offset = data.offset;
        message.bytes.assign(reinterpret_cast<const char*>(data.bytes.data()), data.bytes.size());
        EXPECT_EQ(deliver().offset, data.offset + data.bytes.size());
    }
    source.close();
    message.type = TransferMessageTypeV1::kEntry; message.sha256.clear();
    message.entry_id = 3; message.relative_path = "empty.bin";
    EXPECT_EQ(deliver().disposition, TransferDispositionV1::kReceiving);
    message.type = TransferMessageTypeV1::kCommit; message.relative_path.clear(); message.sha256 = sha256("");
    EXPECT_EQ(deliver().disposition, TransferDispositionV1::kCommitted);
    message.type = TransferMessageTypeV1::kEntry; message.sha256.clear();
    message.entry_id = 4; message.relative_path = "skip.txt"; message.size = 4;
    message.conflict = TransferConflictV1::kSkip;
    EXPECT_EQ(deliver().disposition, TransferDispositionV1::kSkipped);
    message.type = TransferMessageTypeV1::kFinish; message.entry_id = 0; message.relative_path.clear(); message.size = 0;
    EXPECT_EQ(deliver().type, TransferMessageTypeV1::kFinished);
    EXPECT_EQ(batch.progress().state, TransferBatchState::kComplete);
    EXPECT_EQ(batch.progress().completed_entries, 4U); EXPECT_EQ(batch.progress().skipped_entries, 1U);
    EXPECT_EQ(batch.progress().received_bytes, bytes.size()); EXPECT_EQ(batch.progress().committed_bytes, bytes.size());
    EXPECT_EQ(read(tree.root / "skip.txt"), "original");
    EXPECT_TRUE(std::filesystem::is_directory(tree.root / "empty/folder"));
    EXPECT_EQ(partials(tree.root), 0U);
#endif
}

TEST(TransferBatchReceiver, PriorityCancelInvalidatesQueuedCommitAndRetainsCompletedFiles) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows receiver.";
#else
    using namespace redclaw::protocol;
    TemporaryTree tree;
    TransferBatchReceiver batch;
    ASSERT_TRUE(batch.begin(tree.root, "session", "cancel-operation", 2, 3));
    TransferMessageV1 m;
    m.session_epoch = "session"; m.operation_id = "cancel-operation"; m.sequence = 1;
    m.entry_id = 1; m.relative_path = "completed.bin";
    ASSERT_TRUE(batch.accept(m).has_value());
    m.type = TransferMessageTypeV1::kCommit; m.sequence++; m.relative_path.clear(); m.sha256 = sha256("");
    ASSERT_EQ(batch.accept(m)->disposition, TransferDispositionV1::kCommitted);
    m.type = TransferMessageTypeV1::kEntry; m.sequence++; m.entry_id = 2;
    m.relative_path = "partial.bin"; m.sha256.clear(); m.size = 3;
    ASSERT_EQ(batch.accept(m)->disposition, TransferDispositionV1::kReceiving);
    auto stale = m; stale.session_epoch = "old-session"; stale.sequence = 999;
    EXPECT_FALSE(batch.accept(stale).has_value());
    m.type = TransferMessageTypeV1::kChunk; m.relative_path.clear(); m.size = 0; m.sequence++; m.bytes = "abc";
    ASSERT_EQ(batch.accept(m)->offset, 3U);
    batch.request_cancel();
    m.type = TransferMessageTypeV1::kCommit; m.sequence++; m.bytes.clear(); m.sha256 = sha256("abc");
    EXPECT_FALSE(batch.accept(m).has_value());
    EXPECT_EQ(batch.progress().state, TransferBatchState::kCancelled);
    EXPECT_TRUE(std::filesystem::exists(tree.root / "completed.bin"));
    EXPECT_FALSE(std::filesystem::exists(tree.root / "partial.bin"));
    EXPECT_EQ(partials(tree.root), 0U);
    EXPECT_FALSE(batch.begin(tree.root, "new-session", "cancel-operation", 1, 0));
#endif
}

TEST(TransferBatchReceiver, SequenceGapAndIncompleteFinishDoNotCommitPartialData) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows receiver.";
#else
    using namespace redclaw::protocol;
    TemporaryTree tree;
    TransferBatchReceiver batch;
    ASSERT_TRUE(batch.begin(tree.root, "session", "gap-operation", 1, 3));
    TransferMessageV1 m;
    m.session_epoch = "session"; m.operation_id = "gap-operation"; m.sequence = 1;
    m.entry_id = 1; m.relative_path = "partial.bin"; m.size = 3;
    ASSERT_EQ(batch.accept(m)->disposition, TransferDispositionV1::kReceiving);
    m.type = TransferMessageTypeV1::kChunk; m.sequence = 3; m.relative_path.clear(); m.size = 0; m.bytes = "abc";
    EXPECT_EQ(batch.accept(m)->error_code, "transfer_sequence_gap");
    EXPECT_EQ(batch.progress().state, TransferBatchState::kFailed);
    EXPECT_EQ(partials(tree.root), 0U);
    TransferBatchReceiver incomplete;
    ASSERT_TRUE(incomplete.begin(tree.root, "session", "incomplete-operation", 1, 0));
    m.operation_id = "incomplete-operation"; m.sequence = 1; m.type = TransferMessageTypeV1::kFinish;
    m.entry_id = 0; m.bytes.clear();
    EXPECT_EQ(incomplete.accept(m)->error_code, "transfer_incomplete_batch");
    EXPECT_EQ(incomplete.progress().state, TransferBatchState::kFailed);
#endif
}

TEST(TransferSourceManifest, ScansUnicodeAndEmptyFoldersWithoutIncludingItsOwnedSpool) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows source manifest.";
#else
    TemporaryTree tree;
    std::filesystem::create_directories(tree.root / L"\u6587\u4ef6/empty");
    { std::ofstream source(tree.root / L"\u6587\u4ef6/data.bin", std::ios::binary); source << "abc"; }
    const std::array selected{tree.root};
    const auto fixture_name = tree.root.filename().generic_u8string();
    std::string prefix(reinterpret_cast<const char*>(fixture_name.data()), fixture_name.size());
    std::uint64_t last_entries = 0;
    {
        TransferSourceManifest manifest;
        std::string error;
        ASSERT_TRUE(manifest.scan(selected, tree.root, [&](auto progress) {
            EXPECT_GT(progress.entries, last_entries); last_entries = progress.entries; return true;
        }, &error)) << error;
        EXPECT_EQ(manifest.totals().entries, 4U); EXPECT_EQ(manifest.totals().files, 1U);
        EXPECT_EQ(manifest.totals().bytes, 3U);
        TransferSourceEntry entry;
        std::uint64_t entries = 0, bytes = 0;
        bool empty_directory = false;
        while (true) {
            const auto state = manifest.next(&entry, &error);
            ASSERT_NE(state, TransferManifestRead::kFailed) << error;
            if (state == TransferManifestRead::kEnd) break;
            ++entries; bytes += entry.size;
            EXPECT_TRUE(entry.source.is_absolute());
            EXPECT_TRUE(entry.relative_path.starts_with(prefix));
            EXPECT_EQ(entry.relative_path.find(".redclaw-manifest-"), std::string::npos);
            if (entry.directory && entry.relative_path.ends_with("/empty")) empty_directory = true;
        }
        EXPECT_TRUE(empty_directory); EXPECT_EQ(entries, 4U); EXPECT_EQ(bytes, 3U);
        EXPECT_EQ(manifest.next(&entry), TransferManifestRead::kEnd);
        std::uint64_t spool_files = 0;
        for (const auto& item : std::filesystem::directory_iterator(tree.root))
            if (item.path().filename().string().starts_with(".redclaw-manifest-")) ++spool_files;
        EXPECT_EQ(spool_files, 1U);
    }
    for (const auto& item : std::filesystem::directory_iterator(tree.root))
        EXPECT_FALSE(item.path().filename().wstring().starts_with(L".redclaw-manifest-"));
#endif
}

TEST(TransferSourceManifest, ScanCancellationRemovesSpoolAndCannotBeReplayed) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows source manifest.";
#else
    TemporaryTree tree;
    std::filesystem::create_directories(tree.root / "folder/empty");
    { std::ofstream source(tree.root / "folder/file"); source << "original"; }
    TransferSourceManifest manifest;
    const std::array selected{tree.root / "folder"};
    std::string error;
    EXPECT_FALSE(manifest.scan(selected, tree.root, [](auto progress) { return progress.entries < 2; }, &error));
    EXPECT_EQ(error, "transfer_cancelled");
    EXPECT_EQ(read(tree.root / "folder/file"), "original");
    for (const auto& item : std::filesystem::directory_iterator(tree.root))
        EXPECT_FALSE(item.path().filename().string().starts_with(".redclaw-manifest-"));
    TransferSourceEntry entry;
    EXPECT_EQ(manifest.next(&entry, &error), TransferManifestRead::kFailed);
    EXPECT_FALSE(manifest.scan(selected, tree.root, {}, &error));
#endif
}

TEST(TransferBatchSender, StreamsManifestWithBoundedWindowAndReceiverCommitReceipts) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows file transfer.";
#else
    using namespace redclaw::protocol;
    TemporaryTree tree;
    const auto source_root = tree.root / L"\u6e90\u6587\u4ef6";
    const auto destination = tree.root / "destination";
    std::filesystem::create_directories(source_root / "empty");
    std::filesystem::create_directory(destination);
    std::string bytes(3 * 1024 * 1024 + 17, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>((i * 179) % 253);
    { std::ofstream file(source_root / "data.bin", std::ios::binary); file.write(bytes.data(), bytes.size()); }
    { std::ofstream file(source_root / "zero.bin", std::ios::binary); }
    const std::array selected{source_root};
    TransferBatchSender sender;
    ASSERT_TRUE(sender.prepare(selected, tree.root)) << sender.progress().error;
    const auto totals = sender.progress().totals;
    ASSERT_EQ(totals.entries, 4U); EXPECT_EQ(totals.bytes, bytes.size());
    ASSERT_TRUE(sender.begin("epoch", "windowed-operation", TransferConflictV1::kKeepBoth));
    TransferBatchReceiver receiver;
    ASSERT_TRUE(receiver.begin(destination, "epoch", "windowed-operation", totals.entries, totals.bytes));
    std::deque<std::string> network;
    std::uint64_t peak_unacknowledged = 0;
    bool blocked_one_chunk = false;
    std::string blocked_frame;
    const auto send = [&](const TransferMessageV1& message) {
        const auto frame = serialize_transfer_message_v1(message);
        EXPECT_FALSE(frame.empty());
        if (message.type == TransferMessageTypeV1::kChunk && !blocked_one_chunk) {
            blocked_one_chunk = true; blocked_frame = frame; return false;
        }
        if (!blocked_frame.empty()) { EXPECT_EQ(frame, blocked_frame); blocked_frame.clear(); }
        network.push_back(frame); return true;
    };
    for (unsigned step = 0; step < 200 && sender.progress().state == TransferSenderState::kSending; ++step) {
        // Repeated pumps without receipts must stop at the bounded byte window.
        sender.pump(send); sender.pump(send); sender.pump(send);
        const auto& progress = sender.progress();
        const auto unacknowledged = progress.submitted_bytes - progress.acknowledged_bytes;
        peak_unacknowledged = std::max(peak_unacknowledged, unacknowledged);
        EXPECT_LE(unacknowledged, TransferBatchSender::kMaxInFlightBytes);
        EXPECT_LE(network.size(), 22U);
        while (!network.empty()) {
            const auto parsed = parse_transfer_message_v1(network.front()); network.pop_front();
            ASSERT_TRUE(parsed.ok) << parsed.error;
            const auto reply = receiver.accept(parsed.value);
            ASSERT_TRUE(reply.has_value());
            const auto wire_reply = parse_transfer_message_v1(serialize_transfer_message_v1(*reply));
            ASSERT_TRUE(wire_reply.ok) << wire_reply.error;
            ASSERT_TRUE(sender.receive(wire_reply.value)) << sender.progress().error;
        }
    }
    EXPECT_EQ(sender.progress().state, TransferSenderState::kComplete) << sender.progress().error;
    EXPECT_EQ(receiver.progress().state, TransferBatchState::kComplete) << receiver.progress().error;
    EXPECT_GT(peak_unacknowledged, kMaxTransferChunkBytes);
    RecordProperty("peak_unacknowledged_bytes", std::to_string(peak_unacknowledged));
    RecordProperty("verified_file_bytes", std::to_string(bytes.size()));
    EXPECT_EQ(sender.progress().submitted_bytes, bytes.size());
    EXPECT_EQ(sender.progress().acknowledged_bytes, bytes.size());
    EXPECT_EQ(sender.progress().committed_bytes, bytes.size());
    EXPECT_EQ(sha256(read(destination / source_root.filename() / "data.bin")), sha256(bytes));
    EXPECT_TRUE(std::filesystem::is_directory(destination / source_root.filename() / "empty"));
    EXPECT_EQ(partials(destination), 0U);
    for (const auto& item : std::filesystem::directory_iterator(tree.root))
        EXPECT_FALSE(item.path().filename().wstring().starts_with(L".redclaw-manifest-"));
#endif
}

TEST(TransferBatchSender, ChangedSourceAndCancellationNeverReportCommittedBytes) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows file transfer.";
#else
    using namespace redclaw::protocol;
    TemporaryTree tree;
    const auto source = tree.root / "source.bin";
    { std::ofstream file(source); file << "before"; }
    const std::array selected{source};
    TransferBatchSender changed;
    ASSERT_TRUE(changed.prepare(selected, tree.root));
    { std::ofstream file(source); file << "changed length"; }
    ASSERT_TRUE(changed.begin("epoch", "changed-source", TransferConflictV1::kKeepBoth));
    TransferMessageV1 receipt;
    changed.pump([&](const auto& message) {
        receipt = message; receipt.type = TransferMessageTypeV1::kReceipt;
        receipt.relative_path.clear(); receipt.size = 0; return true;
    });
    EXPECT_FALSE(changed.receive(receipt));
    EXPECT_EQ(changed.progress().error, "transfer_source_changed_after_scan");
    EXPECT_EQ(changed.progress().committed_bytes, 0U);
    TransferBatchSender cancelled;
    ASSERT_TRUE(cancelled.prepare(selected, tree.root));
    ASSERT_TRUE(cancelled.begin("epoch", "cancelled-source", TransferConflictV1::kKeepBoth));
    unsigned sends = 0;
    cancelled.pump([&](const auto&) { ++sends; return false; });
    ASSERT_EQ(sends, 1U);
    cancelled.request_cancel();
    cancelled.pump([&](const auto&) { ++sends; return true; });
    EXPECT_EQ(sends, 1U);
    EXPECT_EQ(cancelled.progress().state, TransferSenderState::kCancelled);
    EXPECT_EQ(cancelled.progress().committed_bytes, 0U);
    EXPECT_FALSE(cancelled.begin("new-epoch", "cancelled-source", TransferConflictV1::kKeepBoth));
    for (const auto& item : std::filesystem::directory_iterator(tree.root))
        EXPECT_FALSE(item.path().filename().wstring().starts_with(L".redclaw-manifest-"));
#endif
}
}
