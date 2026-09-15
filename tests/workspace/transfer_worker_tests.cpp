#include "redclaw/workspace/transfer_worker.h"
#include "redclaw/workspace/clipboard_copy_store.h"
#include "redclaw_wire.pb.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
using namespace redclaw::workspace;
struct Tree {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("redclaw-worker-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Tree() { if (!std::filesystem::create_directory(path)) throw std::runtime_error("fixture_directory_failed"); }
    ~Tree() {
        std::error_code ec;
        const auto resolved = std::filesystem::canonical(path, ec);
        const auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        if (!ec && resolved.parent_path() == parent && resolved.filename().string().starts_with("redclaw-worker-"))
            std::filesystem::remove_all(resolved, ec);
    }
};
template<class Predicate> bool until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
TEST(TransferWorker, IncrementalSelectionRoundTripsOnIndependentDiskThreads) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows transfer worker.";
#else
    Tree tree;
    std::filesystem::create_directories(tree.path / "source/empty");
    std::filesystem::create_directories(tree.path / "target");
    std::string data(4 * 1024 * 1024 + 13, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 197) % 251);
    { std::ofstream file(tree.path / "source/payload.bin", std::ios::binary); file.write(data.data(), data.size()); }
    { std::ofstream file(tree.path / "separate.txt"); file << "second selection"; }
    TransferWorker source({true, "epoch", "worker-batch", tree.path, {}, {}, tree.path});
    ASSERT_TRUE(until([&] { return source.progress().state == TransferWorkerState::kSelecting; }));
    ASSERT_TRUE(source.select_source(tree.path / "source"));
    ASSERT_TRUE(until([&] { return source.progress().accepted_sources == 1; }));
    ASSERT_TRUE(source.select_source(tree.path / "separate.txt"));
    ASSERT_TRUE(until([&] { return source.progress().accepted_sources == 2; }));
    ASSERT_TRUE(source.finish_selection());
    ASSERT_TRUE(until([&] { return source.progress().state == TransferWorkerState::kPrepared; }));
    const auto totals = source.progress().totals;
    ASSERT_EQ(totals.entries, 4U); ASSERT_EQ(totals.files, 2U);
    TransferWorker target({false, "epoch", "worker-batch", tree.path / "target", totals, {}, tree.path});
    ASSERT_TRUE(until([&] { return target.progress().state == TransferWorkerState::kTransferring; }));
    ASSERT_TRUE(source.start_sending());
    std::optional<std::string> source_frame, target_frame;
    ASSERT_TRUE(until([&] {
        for (unsigned batch = 0; batch < 16; ++batch) {
            if (!source_frame) source_frame = source.take_output();
            if (source_frame && target.receive(*source_frame)) source_frame.reset();
            if (!target_frame) target_frame = target.take_output();
            if (target_frame && source.receive(*target_frame)) target_frame.reset();
        }
        return source.progress().finished() && target.progress().finished();
    }));
    EXPECT_EQ(source.progress().state, TransferWorkerState::kComplete) << source.progress().error;
    EXPECT_EQ(target.progress().state, TransferWorkerState::kComplete) << target.progress().error;
    EXPECT_EQ(source.progress().committed_bytes, totals.bytes);
    EXPECT_EQ(target.progress().committed_bytes, totals.bytes);
    const auto source_results = read_transfer_result_page(source.progress().results_path);
    const auto target_results = read_transfer_result_page(target.progress().results_path);
    ASSERT_TRUE(source_results.error.empty()) << source_results.error;
    ASSERT_TRUE(target_results.error.empty()) << target_results.error;
    ASSERT_EQ(source_results.entries.size(), 4U); ASSERT_EQ(target_results.entries.size(), 4U);
    for (std::size_t i = 0; i < source_results.entries.size(); ++i) {
        EXPECT_EQ(source_results.entries[i].relative_path, target_results.entries[i].relative_path);
        EXPECT_EQ(source_results.entries[i].size, target_results.entries[i].size);
    }
    // Completion must release even the batch root lock before the coordinator
    // re-enables Agent/file operations, while the worker object still exists.
    const auto directory_handle = CreateFileW((tree.path / "target").c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    EXPECT_NE(directory_handle, INVALID_HANDLE_VALUE) << GetLastError();
    if (directory_handle != INVALID_HANDLE_VALUE) CloseHandle(directory_handle);
    EXPECT_TRUE(std::filesystem::is_directory(tree.path / "target/source/empty"));
    std::ifstream result(tree.path / "target/source/payload.bin", std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(result), {}), data);
    EXPECT_TRUE(source.output_empty()); EXPECT_TRUE(target.output_empty());
    RecordProperty("verified_bytes", std::to_string(totals.bytes));
#endif
}
TEST(TransferWorker, CancelInterruptsSelectionAndNeverSendsQueuedFileBytes) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows transfer worker.";
#else
    Tree tree;
    std::filesystem::create_directories(tree.path / "source");
    for (unsigned i = 0; i < 500; ++i) { std::ofstream file(tree.path / "source" / std::to_string(i)); file << i; }
    TransferWorker source({true, "epoch", "cancel-scan", tree.path, {}, {}, tree.path});
    ASSERT_TRUE(until([&] { return source.progress().state == TransferWorkerState::kSelecting; }));
    ASSERT_TRUE(source.select_source(tree.path / "source"));
    source.cancel();
    ASSERT_TRUE(until([&] { return source.progress().finished(); }));
    EXPECT_EQ(source.progress().state, TransferWorkerState::kCancelled) << source.progress().error;
    EXPECT_FALSE(source.take_output()); EXPECT_FALSE(source.finish_selection()); EXPECT_FALSE(source.start_sending());
    EXPECT_TRUE(source.output_empty());
    for (const auto& entry : std::filesystem::directory_iterator(tree.path))
        EXPECT_FALSE(entry.path().filename().string().starts_with(".redclaw-manifest-"));
#endif
}
TEST(TransferWorker, ClipboardReceiverRetainsGateUntilPreparedObjectsAreConsumedOrCancelled) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows clipboard object preparation.";
#else
    for (const bool cancel : {false, true}) {
        Tree tree;
        std::filesystem::create_directories(tree.path / "metadata");
        std::filesystem::create_directories(tree.path / "source/empty");
        { std::ofstream file(tree.path / "source/file.txt"); file << "copy survives cancellation"; }
        redclaw::protocol::wire::ClipboardPayloadManifestV1 manifest;
        manifest.set_schema_version(1); manifest.set_file_roots(1);
        { std::ofstream file(tree.path / "metadata/manifest.pb", std::ios::binary); ASSERT_TRUE(manifest.SerializeToOstream(&file)); }
        TransferWorker source({true, "epoch", "clipboard-worker", tree.path, {}, {}, tree.path});
        ASSERT_TRUE(until([&] { return source.progress().state == TransferWorkerState::kSelecting; }));
        ASSERT_TRUE(source.select_source(tree.path / "metadata", "metadata"));
        ASSERT_TRUE(until([&] { return source.progress().accepted_sources == 1; }));
        ASSERT_TRUE(source.select_source(tree.path / "source", "files/0/source"));
        ASSERT_TRUE(until([&] { return source.progress().accepted_sources == 2; }));
        ASSERT_TRUE(source.finish_selection());
        ASSERT_TRUE(until([&] { return source.progress().state == TransferWorkerState::kPrepared; }));
        ClipboardCopyStore copies(tree.path); const auto id = ClipboardCopyStore::new_id();
        const auto received = copies.batch_directory(id);
        TransferWorker target({false, "epoch", "clipboard-worker", received, source.progress().totals, {}, tree.path, true, 0,
            ClipboardCopyAction::kNone, id});
        ASSERT_TRUE(until([&] { return target.progress().state == TransferWorkerState::kTransferring; }));
        ASSERT_TRUE(source.start_sending());
        std::optional<std::string> source_frame, target_frame;
        ASSERT_TRUE(until([&] {
            for (unsigned count = 0; count < 16; ++count) {
                if (!source_frame) source_frame = source.take_output();
                if (source_frame && target.receive(*source_frame)) source_frame.reset();
                if (!target_frame) target_frame = target.take_output();
                if (target_frame && source.receive(*target_frame)) target_frame.reset();
            }
            return source.progress().finished() && target.progress().state == TransferWorkerState::kAwaitingPaste;
        })) << target.progress().error;
        EXPECT_FALSE(target.progress().finished()); EXPECT_FALSE(target.complete_clipboard());
        if (cancel) {
            target.cancel();
            ASSERT_TRUE(until([&] { return target.progress().finished(); }));
            EXPECT_EQ(target.progress().state, TransferWorkerState::kCancelled);
            EXPECT_FALSE(target.take_prepared_clipboard()); EXPECT_FALSE(target.complete_clipboard());
        } else {
            auto payload = target.take_prepared_clipboard(); ASSERT_TRUE(payload);
            EXPECT_FALSE(target.take_prepared_clipboard());
            ASSERT_TRUE(target.complete_clipboard());
            ASSERT_TRUE(until([&] { return target.progress().finished(); }));
            EXPECT_EQ(target.progress().state, TransferWorkerState::kComplete);
            EXPECT_FALSE(target.complete_clipboard());
        }
        EXPECT_TRUE(std::filesystem::exists(received / "files/0/source/file.txt"));
        EXPECT_TRUE(std::filesystem::is_directory(received / "files/0/source/empty"));
        EXPECT_TRUE(copies.contains_batch(id));
        // No publication or injection: these tests never touch the interactive clipboard.
    }
#endif
}
TEST(TransferWorker, ClipboardRejectsWritesOutsidePayloadAndRemovesTextOnlyStagingOnCancel) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows owned clipboard staging.";
#else
    for (const bool invalid_entry : {false, true}) {
        Tree tree; ClipboardCopyStore copies(tree.path); const auto id = ClipboardCopyStore::new_id();
        TransferWorker target({false, "epoch", "namespace", copies.batch_directory(id), {1, 1, 0}, {}, tree.path, true, 0,
            ClipboardCopyAction::kNone, id});
        ASSERT_TRUE(until([&] { return target.progress().state == TransferWorkerState::kTransferring; }));
        if (invalid_entry) {
            redclaw::protocol::TransferMessageV1 entry;
            entry.type = redclaw::protocol::TransferMessageTypeV1::kEntry; entry.session_epoch = "epoch";
            entry.operation_id = "namespace"; entry.sequence = entry.entry_id = 1;
            entry.relative_path = ".redclaw-clipboard-batch";
            auto wire = redclaw::protocol::serialize_transfer_message_v1(entry); ASSERT_FALSE(wire.empty());
            ASSERT_TRUE(target.receive(wire));
        } else target.cancel();
        ASSERT_TRUE(until([&] { return target.progress().finished(); }));
        EXPECT_EQ(target.progress().state, invalid_entry ? TransferWorkerState::kFailed : TransferWorkerState::kCancelled);
        if (invalid_entry) EXPECT_EQ(target.progress().error, "clipboard_entry_namespace_invalid");
        EXPECT_FALSE(std::filesystem::exists(copies.batch_directory(id)));
    }
#endif
}
TEST(TransferResults, BoundedBidirectionalPagesAndIncompleteTailRetainCompletedRows) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows result journal.";
#else
    Tree tree;
    TransferResultJournal journal;
    std::string error;
    ASSERT_TRUE(journal.begin(tree.path, &error)) << error;
    EXPECT_FALSE(journal.append({"../escape", 1, false, false}, &error));
    for (unsigned i = 0; i < 300; ++i) {
        ASSERT_TRUE(journal.append({"folder/" + std::to_string(i), i, false, i % 2 == 0}, &error)) << error;
    }
    journal.finish();
    const auto first = read_transfer_result_page(journal.path());
    ASSERT_TRUE(first.error.empty()) << first.error;
    ASSERT_EQ(first.entries.size(), 128U); EXPECT_FALSE(first.has_previous); EXPECT_TRUE(first.has_next);
    const auto second = read_transfer_result_page(journal.path(), first.end);
    ASSERT_TRUE(second.error.empty()); ASSERT_EQ(second.entries.size(), 128U);
    const auto last = read_transfer_result_page(journal.path(), second.end);
    ASSERT_TRUE(last.error.empty()); ASSERT_EQ(last.entries.size(), 44U); EXPECT_FALSE(last.has_next);
    EXPECT_EQ(last.entries.back().relative_path, "folder/299");
    const auto back = read_transfer_result_page(journal.path(), last.begin, true);
    ASSERT_TRUE(back.error.empty()); ASSERT_EQ(back.entries.size(), 128U);
    EXPECT_EQ(back.begin, second.begin); EXPECT_EQ(back.end, second.end);
    for (std::size_t i = 0; i < back.entries.size(); ++i) {
        EXPECT_EQ(back.entries[i].relative_path, second.entries[i].relative_path);
        EXPECT_EQ(back.entries[i].size, second.entries[i].size);
        EXPECT_EQ(back.entries[i].skipped, second.entries[i].skipped);
    }
    EXPECT_FALSE(read_transfer_result_page(journal.path(), 1).error.empty());
    { std::ofstream tail(journal.path(), std::ios::binary | std::ios::app); tail.put('\xff'); }
    const auto damaged = read_transfer_result_page(journal.path(), second.end);
    EXPECT_EQ(damaged.entries.size(), 44U); EXPECT_EQ(damaged.error, "transfer_results_truncated");
#endif
}
TEST(TransferWorker, ClipboardRootAliasesPreserveBasenamesWithoutCopyingSourceTrees) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows transfer worker.";
#else
    Tree tree; std::filesystem::create_directories(tree.path / "source/empty");
    { std::ofstream file(tree.path / "source/payload"); file << "clipboard-file-fixture"; }
    std::filesystem::create_directory(tree.path / "target");
    TransferWorker source({true, "epoch", "aliased-roots", tree.path, {}, {}, tree.path});
    ASSERT_TRUE(until([&] { return source.progress().state == TransferWorkerState::kSelecting; }));
    ASSERT_TRUE(source.select_source(tree.path / "source", "files/0/source"));
    ASSERT_TRUE(until([&] { return source.progress().accepted_sources == 1; }));
    ASSERT_TRUE(source.finish_selection()); ASSERT_TRUE(until([&] { return source.progress().state == TransferWorkerState::kPrepared; }));
    const auto totals = source.progress().totals;
    TransferWorker target({false, "epoch", "aliased-roots", tree.path / "target", totals, {}, tree.path});
    ASSERT_TRUE(until([&] { return target.progress().state == TransferWorkerState::kTransferring; }));
    ASSERT_TRUE(source.start_sending());
    std::optional<std::string> forward, backward;
    ASSERT_TRUE(until([&] {
        if (!forward) forward = source.take_output(); if (forward && target.receive(*forward)) forward.reset();
        if (!backward) backward = target.take_output(); if (backward && source.receive(*backward)) backward.reset();
        return source.progress().finished() && target.progress().finished();
    }));
    ASSERT_EQ(source.progress().state, TransferWorkerState::kComplete) << source.progress().error;
    ASSERT_EQ(target.progress().state, TransferWorkerState::kComplete) << target.progress().error;
    EXPECT_TRUE(std::filesystem::exists(tree.path / "target/files/0/source/payload"));
    EXPECT_TRUE(std::filesystem::is_directory(tree.path / "target/files/0/source/empty"));
    EXPECT_TRUE(std::filesystem::exists(tree.path / "source/payload"));
#endif
}
}
