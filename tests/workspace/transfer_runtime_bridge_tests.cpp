#include "redclaw/workspace/transfer_runtime_bridge.h"
#include "redclaw/workspace/clipboard_copy_store.h"
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

namespace {
using namespace redclaw::workspace;
using namespace redclaw::protocol;
using Action = WorkspaceActionV1;
using Control = StreamControlMessageV1;
std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string(); return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
struct TransferRuntimeFixture : testing::Test {
    std::filesystem::path root;
    TransferOperationGate host_gate, controller_gate;
    std::deque<Control> to_host, to_controller;
    std::deque<std::string> bulk_to_host, bulk_to_controller;
    std::vector<Control> local_events;
    std::unique_ptr<TransferRuntimeBridge> host, controller;
    std::uint64_t now = 0, sequence = 0;
    bool block_bulk = false;
    bool block_finished = false;
    void SetUp() override {
#ifndef _WIN32
        GTEST_SKIP() << "Windows file transfer runtime.";
#else
        root = std::filesystem::canonical(std::filesystem::temp_directory_path())
            / ("redclaw-transfer-runtime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(root));
        std::filesystem::create_directories(root / "destination");
        host = std::make_unique<TransferRuntimeBridge>(true, "host-epoch", root, host_gate,
            [&](auto message) { return send(message, to_controller); }, [](const auto&) { return true; },
            [&](auto frame) { if (block_bulk) return false; bulk_to_controller.emplace_back(frame); return true; }, [] { return true; });
        controller = std::make_unique<TransferRuntimeBridge>(false, "controller-epoch", root, controller_gate,
            [&](auto message) { return send(message, to_host); },
            [&](const auto& message) { local_events.push_back(message); return true; },
            [&](auto frame) { if (block_bulk) return false; bulk_to_host.emplace_back(frame); return true; }, [] { return true; });
        host->peer_capability(1, "controller-epoch"); controller->peer_capability(1, "host-epoch");
        host->channel_open(true); controller->channel_open(true); step();
#endif
    }
    void TearDown() override {
        controller.reset(); host.reset();
        if (root.empty()) return;
        std::error_code ec;
        const auto checked = std::filesystem::canonical(root, ec);
        const auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        if (!ec && checked == root && checked.parent_path() == parent && checked.filename().string().starts_with("redclaw-transfer-runtime-"))
            std::filesystem::remove_all(checked, ec);
    }
    bool send(Control message, std::deque<Control>& target) {
        EXPECT_TRUE(message.workspace->results_path.empty()); // local journal never enters the peer channel
        EXPECT_EQ(message.workspace->clipboard_sequence, 0U);
        if (block_finished && message.workspace->action == Action::kFinished) return false;
        message.message_id = ++sequence; message.sent_at_ms = now + 1;
        const auto wire = serialize_stream_control_message_v1(message);
        EXPECT_FALSE(wire.empty()); if (wire.empty()) return false;
        const auto parsed = parse_stream_control_message_v1(wire);
        EXPECT_TRUE(parsed.ok) << parsed.error; if (!parsed.ok) return false;
        target.push_back(parsed.value); return true;
    }
    Control command(Action action, TransferDirectionV1 direction, std::string path = {}, std::string id = "batch") {
        Control result; result.type = StreamControlMessageTypeV1::kWorkspace; result.request_id = std::move(id);
        result.workspace.emplace(); result.workspace->action = action; result.workspace->direction = direction;
        result.workspace->path = std::move(path); return result;
    }
    void step() {
        now += 10;
        while (!to_host.empty()) { EXPECT_TRUE(host->receive_control(to_host.front())); to_host.pop_front(); }
        while (!to_controller.empty()) { EXPECT_TRUE(controller->receive_control(to_controller.front())); to_controller.pop_front(); }
        while (!bulk_to_host.empty()) { EXPECT_TRUE(host->receive_bulk(bulk_to_host.front())); bulk_to_host.pop_front(); }
        while (!bulk_to_controller.empty()) { EXPECT_TRUE(controller->receive_bulk(bulk_to_controller.front())); bulk_to_controller.pop_front(); }
        host->pump(now, true); controller->pump(now, true);
    }
    template<class Predicate> bool until(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        do { step(); if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
    bool seen(Action action, std::string id = "batch") const {
        return std::any_of(local_events.begin(), local_events.end(), [&](const auto& item) {
            return item.request_id == id && item.workspace && item.workspace->action == action;
        });
    }
    void begin(TransferDirectionV1 direction) {
        controller->from_gui(command(Action::kPrepare, direction, utf8(root / "destination")));
        ASSERT_TRUE(controller_gate.blocks_mutation());
        ASSERT_TRUE(until([&] { return seen(Action::kPrepared); }));
        ASSERT_TRUE(host_gate.blocks_mutation());
        controller->from_gui(command(Action::kSelectSource, direction, utf8(root / "source")));
        ASSERT_TRUE(until([&] { return seen(Action::kSourceAccepted); }));
        controller->from_gui(command(Action::kSelectionComplete, direction));
        ASSERT_TRUE(until([&] { return seen(Action::kReady); }));
    }
};
TEST_F(TransferRuntimeFixture, UploadAndDownloadCommitBeforeUnlockingBothEndpoints) {
    for (const auto direction : {TransferDirectionV1::kToHost, TransferDirectionV1::kToController}) {
        local_events.clear();
        std::filesystem::create_directories(root / "source/empty");
        std::string bytes(2 * 1024 * 1024 + 5, 'r');
        bytes.back() = direction == TransferDirectionV1::kToHost ? 'H' : 'C';
        { std::ofstream file(root / "source/data", std::ios::binary); file.write(bytes.data(), bytes.size()); }
        begin(direction);
        ASSERT_TRUE(until([&] { return !host_gate.blocks_mutation() && !controller_gate.blocks_mutation() && seen(Action::kFinished); }));
        bool exact_file_found = false;
        for (const auto& item : std::filesystem::directory_iterator(root / "destination/source")) {
            if (!item.is_regular_file()) continue;
            std::ifstream file(item.path(), std::ios::binary);
            const std::string actual(std::istreambuf_iterator<char>(file), {});
            exact_file_found = exact_file_found || actual == bytes;
        }
        EXPECT_TRUE(exact_file_found);
        const auto finished = std::find_if(local_events.begin(), local_events.end(), [](const auto& item) {
            return item.workspace->action == Action::kFinished;
        });
        ASSERT_NE(finished, local_events.end()); ASSERT_FALSE(finished->workspace->results_path.empty());
        const auto path_bytes = finished->workspace->results_path;
        const auto results = read_transfer_result_page(std::filesystem::u8path(path_bytes));
        ASSERT_TRUE(results.error.empty()) << results.error;
        ASSERT_EQ(results.entries.size(), 3U);
        bool result_file_exists = false;
        for (const auto& entry : results.entries) {
            if (!entry.directory) result_file_exists = std::filesystem::exists(root / "destination" / std::filesystem::u8path(entry.relative_path));
        }
        EXPECT_TRUE(result_file_exists); // includes the renamed keep-both destination
        EXPECT_EQ(host_gate.revision(), controller_gate.revision());
    }
}
TEST_F(TransferRuntimeFixture, PriorityCancelBypassesBlockedBulkAndWaitsForBothWorkers) {
    std::filesystem::create_directories(root / "source");
    { std::ofstream file(root / "source/data", std::ios::binary); file << std::string(3 * 1024 * 1024, 'x'); }
    block_bulk = true;
    begin(TransferDirectionV1::kToHost);
    controller->from_gui(command(Action::kCancel, TransferDirectionV1::kToHost));
    EXPECT_TRUE(controller_gate.blocks_mutation());
    EXPECT_TRUE(host_gate.blocks_mutation());
    ASSERT_TRUE(until([&] { return !host_gate.blocks_mutation() && !controller_gate.blocks_mutation(); }));
    EXPECT_TRUE(seen(Action::kFinished));
    EXPECT_FALSE(std::filesystem::exists(root / "destination/source/data"));
    EXPECT_TRUE(bulk_to_host.empty()); EXPECT_TRUE(bulk_to_controller.empty());
}
TEST_F(TransferRuntimeFixture, FastChannelFlapAbortsWithoutReplayAndRejectsOldCapability) {
    std::filesystem::create_directories(root / "source");
    { std::ofstream file(root / "source/data"); file << "no replay"; }
    block_bulk = true;
    begin(TransferDirectionV1::kToController);
    host->channel_open(false); controller->channel_open(false);
    host->channel_open(true); controller->channel_open(true);
    ASSERT_TRUE(until([&] { return !host_gate.blocks_mutation() && !controller_gate.blocks_mutation(); }));
    EXPECT_FALSE(std::filesystem::exists(root / "destination/source/data"));
    const auto revision = controller_gate.revision();
    step(); // drain the prior connection's final receipt before changing capability
    controller->peer_capability(0, "host-epoch"); step();
    controller->from_gui(command(Action::kPrepare, TransferDirectionV1::kToHost, utf8(root / "destination"), "old-peer"));
    EXPECT_TRUE(seen(Action::kError, "old-peer")); EXPECT_EQ(controller_gate.revision(), revision);
}
TEST_F(TransferRuntimeFixture, CancelDuringUnsentCompletionRetainsCommittedFilesAndReleasesBothGates) {
    std::filesystem::create_directories(root / "source");
    { std::ofstream file(root / "source/data"); file << "already committed"; }
    block_finished = true;
    begin(TransferDirectionV1::kToHost);
    ASSERT_TRUE(until([&] {
        return host_gate.phase() == TransferGatePhase::kFinishing && controller_gate.phase() == TransferGatePhase::kFinishing;
    }));
    controller->from_gui(command(Action::kCancel, TransferDirectionV1::kToHost));
    for (unsigned i = 0; i < 4; ++i) step();
    EXPECT_TRUE(host_gate.blocks_mutation()); EXPECT_TRUE(controller_gate.blocks_mutation());
    block_finished = false;
    ASSERT_TRUE(until([&] { return !host_gate.blocks_mutation() && !controller_gate.blocks_mutation(); }));
    EXPECT_TRUE(std::filesystem::exists(root / "destination/source/data"));
    const auto finished = std::find_if(local_events.begin(), local_events.end(), [](const auto& item) {
        return item.workspace->action == Action::kFinished;
    });
    ASSERT_NE(finished, local_events.end());
    const auto results = read_transfer_result_page(std::filesystem::u8path(finished->workspace->results_path));
    ASSERT_TRUE(results.error.empty()) << results.error;
    EXPECT_EQ(results.entries.size(), 2U);
}
TEST_F(TransferRuntimeFixture, PeerCannotSupplyOwnerLocalResultJournal) {
    auto forged = command(Action::kFinished, TransferDirectionV1::kToHost);
    forged.session_epoch = "host-epoch"; forged.workspace->results_path = utf8(root / "untrusted.rctr");
    EXPECT_FALSE(controller->receive_control(forged));
    forged.session_epoch = "controller-epoch";
    EXPECT_FALSE(host->receive_control(forged));
}
TEST_F(TransferRuntimeFixture, ClipboardRequiresItsOwnCapabilityAndHostInputBeforeSnapshot) {
    auto paste = command(Action::kPrepare, TransferDirectionV1::kToHost, {}, "paste");
    paste.workspace->purpose = WorkspaceTransferPurposeV1::kClipboard; paste.workspace->clipboard_sequence = 42;
    controller->from_gui(paste);
    ASSERT_TRUE(seen(Action::kError, "paste")); EXPECT_FALSE(controller_gate.blocks_mutation());
    EXPECT_EQ(local_events.back().workspace->error_code, "clipboard_peer_unsupported");
    EXPECT_TRUE(to_host.empty());
    local_events.clear();
    host->peer_capability(1, "controller-epoch", 1); controller->peer_capability(1, "host-epoch", 1); step();
    controller->from_gui(paste); EXPECT_TRUE(controller_gate.blocks_mutation());
    ASSERT_TRUE(until([&] { return seen(Action::kFinished, "paste") && !controller_gate.blocks_mutation() && !host_gate.blocks_mutation(); }));
    const auto result = std::find_if(local_events.begin(), local_events.end(), [](const auto& message) {
        return message.request_id == "paste" && message.workspace->action == Action::kFinished;
    });
    ASSERT_NE(result, local_events.end()); EXPECT_EQ(result->workspace->error_code, "clipboard_input_unavailable");
    EXPECT_FALSE(result->workspace->paste_submitted); EXPECT_TRUE(result->workspace->results_path.empty());
    EXPECT_TRUE(bulk_to_host.empty()); EXPECT_TRUE(bulk_to_controller.empty());
}
TEST_F(TransferRuntimeFixture, PeerCannotForgeClipboardSequenceOrHostPasteReceipt) {
    auto request = command(Action::kPrepare, TransferDirectionV1::kToHost);
    request.workspace->purpose = WorkspaceTransferPurposeV1::kClipboard;
    request.workspace->clipboard_sequence = 17; request.session_epoch = "controller-epoch";
    EXPECT_FALSE(host->receive_control(request));
    request.workspace->clipboard_sequence = 0; request.workspace->action = Action::kFinished;
    request.workspace->paste_submitted = true;
    EXPECT_FALSE(host->receive_control(request));
}
TEST_F(TransferRuntimeFixture, RemoteDirectoryEnumerationStreamsAcrossControlWithoutTakingTransferGate) {
    std::filesystem::create_directories(root / "listing/empty");
    for (unsigned index = 0; index < 80; ++index) { std::ofstream file(root / "listing" / (std::to_string(index) + ".txt")); file << index; }
    controller->from_gui(command(Action::kBrowse, TransferDirectionV1::kToHost, utf8(root / "listing"), "browse-fixture"));
    ASSERT_TRUE(until([&] { return seen(Action::kBrowseEnd, "browse-fixture"); }));
    unsigned entries = 0;
    for (const auto& message : local_events) {
        if (message.request_id != "browse-fixture") continue;
        if (message.workspace->action == Action::kBrowseEntry) ++entries;
        if (message.workspace->action == Action::kBrowseEnd) EXPECT_TRUE(message.workspace->error_code.empty());
    }
    EXPECT_EQ(entries, 81U); EXPECT_FALSE(host_gate.blocks_mutation()); EXPECT_FALSE(controller_gate.blocks_mutation());
}
TEST_F(TransferRuntimeFixture, ClipboardCopiesNegotiateVersionTwoAndCleanupWaitsForBothReceipts) {
    ClipboardCopyStore copies(root); const auto id = ClipboardCopyStore::new_id(); std::string error;
    ASSERT_TRUE(copies.create_batch(id, &error)) << error;
    std::filesystem::create_directories(copies.batch_directory(id) / "files/empty");
    { std::ofstream file(copies.batch_directory(id) / "files/copied.txt"); file << "owned copy"; }
    auto cleanup = command(Action::kPrepare, TransferDirectionV1::kToHost, id, "cleanup");
    cleanup.workspace->purpose = WorkspaceTransferPurposeV1::kClipboardCleanup;
    host->peer_capability(1, "controller-epoch", 1); controller->peer_capability(1, "host-epoch", 1); step();
    controller->from_gui(cleanup);
    EXPECT_TRUE(seen(Action::kError, "cleanup")); EXPECT_TRUE(copies.contains_batch(id));
    EXPECT_EQ(local_events.back().workspace->error_code, "clipboard_peer_unsupported");
    EXPECT_FALSE(controller_gate.blocks_mutation()); EXPECT_TRUE(to_host.empty());
    host->peer_capability(1, "controller-epoch", 2); controller->peer_capability(1, "host-epoch", 9); step();
    EXPECT_EQ(local_events.back().clipboard_version, 2U);
    controller->from_gui(command(Action::kBrowseClipboardCopies, TransferDirectionV1::kToHost, {}, "copies"));
    ASSERT_TRUE(until([&] { return seen(Action::kBrowseEnd, "copies"); }));
    unsigned count = 0;
    for (const auto& event : local_events) if (event.request_id == "copies" && event.workspace->action == Action::kBrowseEntry) {
        ++count; EXPECT_EQ(event.workspace->path, id); EXPECT_GT(event.workspace->created_at_ms, 0U);
    }
    EXPECT_EQ(count, 1U); EXPECT_FALSE(controller_gate.blocks_mutation());
    block_finished = true;
    controller->from_gui(cleanup); EXPECT_TRUE(controller_gate.blocks_mutation());
    ASSERT_TRUE(until([&] { return !std::filesystem::exists(copies.batch_directory(id)); }));
    EXPECT_TRUE(host_gate.blocks_mutation()); EXPECT_TRUE(controller_gate.blocks_mutation());
    EXPECT_TRUE(bulk_to_host.empty()); EXPECT_TRUE(bulk_to_controller.empty());
    block_finished = false;
    ASSERT_TRUE(until([&] { return seen(Action::kFinished, "cleanup") && !host_gate.blocks_mutation() && !controller_gate.blocks_mutation(); }));
    for (const auto& event : local_events) if (event.request_id == "cleanup" && event.workspace->action == Action::kFinished)
        EXPECT_TRUE(event.workspace->error_code.empty()) << event.workspace->error_code;
    // A subsequent ordinary browse must not inherit the previous cleanup purpose.
    controller->from_gui(command(Action::kBrowse, TransferDirectionV1::kToHost, utf8(root), "after-cleanup"));
    ASSERT_TRUE(until([&] { return seen(Action::kBrowseEnd, "after-cleanup"); }));
}
TEST_F(TransferRuntimeFixture, ClipboardCopyCleanupRefusesForeignDirectoryAndReturnsFailure) {
    host->peer_capability(1, "controller-epoch", 2); controller->peer_capability(1, "host-epoch", 2); step();
    ClipboardCopyStore copies(root); const auto id = ClipboardCopyStore::new_id();
    std::filesystem::create_directories(copies.batch_directory(id) / "files");
    { std::ofstream file(copies.batch_directory(id) / "files/foreign"); file << "do not remove"; }
    auto cleanup = command(Action::kPrepare, TransferDirectionV1::kToHost, id, "foreign");
    cleanup.workspace->purpose = WorkspaceTransferPurposeV1::kClipboardCleanup;
    controller->from_gui(cleanup);
    ASSERT_TRUE(until([&] { return seen(Action::kFinished, "foreign") && !host_gate.blocks_mutation() && !controller_gate.blocks_mutation(); }));
    EXPECT_TRUE(std::filesystem::exists(copies.batch_directory(id) / "files/foreign"));
    EXPECT_FALSE(seen(Action::kReady, "foreign"));
    for (const auto& event : local_events) if (event.request_id == "foreign" && event.workspace->action == Action::kFinished)
        EXPECT_EQ(event.workspace->error_code, "clipboard_copy_batch_not_owned");
}
}
