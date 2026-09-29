#include <gtest/gtest.h>
#include "redclaw/workspace/transfer_runtime_bridge.h"
#include "redclaw/workspace/clipboard_host_paste.h"
#include "redclaw_wire.pb.h"
#include "ui/transfer_coordinator.h"
#include <QCoreApplication>
#include <QThreadPool>
#include <QTemporaryDir>
#include <QThread>
#include <QElapsedTimer>
#include <fstream>

namespace {
using namespace redclaw::workspace;
using namespace redclaw::protocol;
using Action = WorkspaceActionV1;
using Control = StreamControlMessageV1;
struct ClipboardControlFixture : testing::Test {
    QTemporaryDir temporary;
    std::filesystem::path root;
    TransferOperationGate hg, cg;
    std::unique_ptr<TransferRuntimeBridge> host, controller;
    std::deque<Control> hc, ch;
    std::deque<std::string> hb, cb;
    std::vector<Control> results;
    std::uint64_t now = 0, sequence = 0;
    int host_publications = 0, local_publications = 0, pastes = 0;
    std::atomic_uint32_t host_clipboard_sequence{100}, local_clipboard_sequence{200};
    std::unique_ptr<redclaw::ui::TransferCoordinator> coordinator;
    std::string automatic_operation;
    ClipboardFocusToken focus{1, 2, 3, 4, 5, 6, 7};
    std::string descriptor() {
        wire::ClipboardSourceV1 source; source.set_schema_version(1);
        auto* text = source.add_formats(); text->set_kind(1); text->set_data("f\0i\0x\0t\0u\0r\0e\0\0\0", 16);
        auto* html = source.add_formats(); html->set_kind(2); html->set_data("<b>fixture</b>\0", 15);
        auto* rtf = source.add_formats(); rtf->set_kind(3); rtf->set_data("{\\rtf1 fixture}\0", 16);
        auto* png = source.add_formats(); png->set_kind(4); png->set_data("PNG-test-format-bytes");
        auto* dib5 = source.add_formats(); dib5->set_kind(5); dib5->set_data(std::string(128, '\0'));
        auto* dib = source.add_formats(); dib->set_kind(6); dib->set_data(std::string(44, '\0'));
        const auto path = (root / "source").generic_u8string();
        source.add_files(reinterpret_cast<const char*>(path.data()), path.size());
        return source.SerializeAsString();
    }
    ClipboardHostActions actions(bool remote) {
        ClipboardHostActions value;
        value.eligibility = [] { return ClipboardInputEligibility{true, 1}; };
        value.publish_eligible = [] { return true; };
        value.submit_paste = [this](auto, auto*) { ++pastes; return true; };
        value.focus_snapshot = [this] { return std::optional(focus); };
        value.publish = [this, remote](auto&, const auto& guard, auto*) {
            if (!guard()) return false;
            if (remote) { ++host_publications; ++host_clipboard_sequence; }
            else { ++local_publications; ++local_clipboard_sequence; }
            return true;
        };
        value.sequence = [this, remote] { return remote ? host_clipboard_sequence.load() : local_clipboard_sequence.load(); };
        value.capture = [this, remote](auto& snapshot, const auto& spool, auto expected, const auto& cancelled, auto* error) {
            const auto current = remote ? host_clipboard_sequence.load() : local_clipboard_sequence.load();
            if (expected && expected != current) { if (error) *error = "clipboard_changed_before_snapshot"; return false; }
            return snapshot.create(spool, descriptor(), cancelled, error);
        };
        return value;
    }
    bool send(Control value, std::deque<Control>& queue) {
        EXPECT_TRUE(value.workspace->clipboard_source.empty()); EXPECT_TRUE(value.workspace->snapshot_path.empty());
        value.message_id = ++sequence; value.sent_at_ms = now + 1;
        const auto bytes = serialize_stream_control_message_v1(value);
        const auto parsed = parse_stream_control_message_v1(bytes);
        EXPECT_TRUE(parsed.ok) << parsed.error;
        if (!parsed.ok) return false;
        queue.push_back(parsed.value); return true;
    }
    void SetUp() override {
        ASSERT_TRUE(temporary.isValid()); root = temporary.path().toStdWString();
        std::filesystem::create_directories(root / "source/empty");
        std::filesystem::create_directories(root / "host");
        std::filesystem::create_directories(root / "controller");
        { std::ofstream file(root / "source/data", std::ios::binary); file << std::string(65539, 'C'); }
        host = std::make_unique<TransferRuntimeBridge>(true, "host", root / "host", hg,
            [&](auto value) { return send(value, hc); }, [](auto) { return true; },
            [&](auto bytes) { hb.emplace_back(bytes); return true; }, [] { return true; }, actions(true));
        controller = std::make_unique<TransferRuntimeBridge>(false, "controller", root / "controller", cg,
            [&](auto value) { return send(value, ch); }, [&](auto value) {
                results.push_back(value); if (coordinator) coordinator->receive(value); return true;
            },
            [&](auto bytes) { cb.emplace_back(bytes); return true; }, [] { return true; }, actions(false));
        host->peer_capability(1, "controller", 3); controller->peer_capability(1, "host", 3);
        host->channel_open(true); controller->channel_open(true); step();
    }
    void TearDown() override {
        coordinator.reset(); controller.reset(); host.reset();
        EXPECT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
    }
    void step() {
        now += 10;
        while (!ch.empty()) { EXPECT_TRUE(host->receive_control(ch.front())); ch.pop_front(); }
        while (!hc.empty()) { EXPECT_TRUE(controller->receive_control(hc.front())); hc.pop_front(); }
        while (!cb.empty()) { EXPECT_TRUE(host->receive_bulk(cb.front())); cb.pop_front(); }
        while (!hb.empty()) { EXPECT_TRUE(controller->receive_bulk(hb.front())); hb.pop_front(); }
        host->pump(now, true); controller->pump(now, true);
        QCoreApplication::processEvents();
    }
    void enable_return() {
        coordinator = std::make_unique<redclaw::ui::TransferCoordinator>(
            [&](const auto& message, auto*) { controller->from_gui(message); return true; }, nullptr,
            [&] { return local_clipboard_sequence.load(); });
        coordinator->started = [&](const auto& message) { automatic_operation = message.request_id; };
        host->peer_capability(1, "controller", 4); controller->peer_capability(1, "host", 4); step();
    }
    void remote_copy() {
        ++host_clipboard_sequence;
        for (int tick = 0; tick < 8 && automatic_operation.empty(); ++tick) step();
    }
    Control command(std::uint32_t mode, std::string id) {
        Control value; value.type = StreamControlMessageTypeV1::kWorkspace; value.request_id = id;
        value.workspace.emplace(); value.workspace->action = Action::kPrepare;
        value.workspace->purpose = WorkspaceTransferPurposeV1::kClipboard;
        value.workspace->clipboard_mode = mode;
        value.workspace->direction = mode == 1 || mode >= 4 ? TransferDirectionV1::kToController : TransferDirectionV1::kToHost;
        if (mode == 2 || mode == 5) value.workspace->clipboard_source = descriptor();
        return value;
    }
    std::optional<Control> finish(const std::string& id) {
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 15000) {
            step();
            for (const auto& result : results) if (result.request_id == id && result.workspace->action == Action::kFinished
                && !hg.blocks_mutation() && !cg.blocks_mutation()) return result;
            QThread::msleep(2);
        }
        return {};
    }
};
TEST_F(ClipboardControlFixture, ReadsExportBothEndsWithoutPublishingAndWritesPublishOnlyOnce) {
    for (const auto mode : {1U, 4U, 2U, 5U}) {
        const auto id = "operation-" + std::to_string(mode);
        controller->from_gui(command(mode, id));
        ASSERT_TRUE(cg.blocks_mutation());
        const auto result = finish(id);
        ASSERT_TRUE(result) << mode;
        ASSERT_TRUE(result->workspace->error_code.empty()) << result->workspace->error_code;
        EXPECT_FALSE(cg.blocks_mutation()); EXPECT_FALSE(hg.blocks_mutation());
        EXPECT_EQ(pastes, 0);
        if (mode == 1 || mode == 4) {
            EXPECT_EQ(host_publications + local_publications, 0);
            const auto& text = result->workspace->snapshot_path;
            const auto path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
            ASSERT_TRUE(std::filesystem::exists(path / "metadata/manifest.pb"));
            PreparedClipboardPayload verified; const std::atomic_bool cancelled{false}; std::string error;
            EXPECT_TRUE(verified.load(path, cancelled, &error)) << error;
            EXPECT_TRUE(std::filesystem::exists(path / "files/0/source/empty"));
        }
    }
    EXPECT_EQ(host_publications, 1); EXPECT_EQ(local_publications, 1);
    controller->from_gui(command(3, "paste"));
    const auto pasted = finish("paste");
    ASSERT_TRUE(pasted); EXPECT_TRUE(pasted->workspace->error_code.empty()) << pasted->workspace->error_code;
    EXPECT_TRUE(pasted->workspace->paste_submitted); EXPECT_EQ(pastes, 1);
    EXPECT_EQ(host_publications + local_publications, 2);
}
TEST_F(ClipboardControlFixture, ChangedFocusRejectsSeparatePasteAndOldPeerRejectsNewSemantics) {
    ClipboardHostPaste paste(actions(true)); std::string error;
    ASSERT_TRUE(paste.begin("epoch", "operation", &error));
    ++focus.generation;
    EXPECT_FALSE(paste.paste(&error)); EXPECT_EQ(error, "clipboard_focus_changed"); EXPECT_EQ(pastes, 0);
    host->peer_capability(1, "controller", 2); controller->peer_capability(1, "host", 2); step();
    controller->from_gui(command(1, "unsupported")); step();
    EXPECT_FALSE(cg.blocks_mutation()); EXPECT_FALSE(hg.blocks_mutation());
    ASSERT_FALSE(results.empty());
    const auto found = std::find_if(results.begin(), results.end(), [](const auto& value) {
        return value.request_id == "unsupported" && value.workspace->action == Action::kError;
    });
    ASSERT_NE(found, results.end()); EXPECT_EQ(found->workspace->error_code, "clipboard_peer_unsupported");
}
TEST_F(ClipboardControlFixture, CommonVersionTwoKeepsLegacyPasteWithInjectedAdapters) {
    host->peer_capability(1, "controller", 2); controller->peer_capability(1, "host", 2); step();
    controller->from_gui(command(0, "legacy-paste"));
    const auto result = finish("legacy-paste"); ASSERT_TRUE(result);
    EXPECT_TRUE(result->workspace->error_code.empty()) << result->workspace->error_code;
    EXPECT_TRUE(result->workspace->paste_submitted);
    EXPECT_EQ(host_publications, 1); EXPECT_EQ(pastes, 1); EXPECT_EQ(local_publications, 0);
}
TEST_F(ClipboardControlFixture, LocalSnapshotCopyCanBeListedAndCleanedByOpaqueId) {
    controller->from_gui(command(4, "export-copy"));
    const auto result = finish("export-copy"); ASSERT_TRUE(result);
    ASSERT_TRUE(result->workspace->error_code.empty()) << result->workspace->error_code;
    const auto path = std::filesystem::u8path(result->workspace->snapshot_path);
    ASSERT_TRUE(std::filesystem::exists(path));
    Control list; list.type = StreamControlMessageTypeV1::kWorkspace; list.request_id = "copies";
    list.workspace.emplace(); list.workspace->action = Action::kBrowseClipboardCopies;
    list.workspace->direction = TransferDirectionV1::kToController;
    controller->from_gui(list);
    QElapsedTimer timer; timer.start(); bool listed = false, ended = false;
    while (!ended && timer.elapsed() < 5000) {
        step();
        for (const auto& item : results) if (item.request_id == "copies") {
            if (item.workspace->action == Action::kBrowseEntry && item.workspace->path == path.filename().string()) listed = true;
            if (item.workspace->action == Action::kBrowseEnd) ended = true;
        }
        QThread::msleep(2);
    }
    EXPECT_TRUE(ended); EXPECT_TRUE(listed);
    auto cleanup = command(0, "clean-copy");
    cleanup.workspace->purpose = WorkspaceTransferPurposeV1::kClipboardCleanup;
    cleanup.workspace->direction = TransferDirectionV1::kToController;
    cleanup.workspace->path = path.filename().string();
    controller->from_gui(cleanup);
    const auto cleaned = finish("clean-copy"); ASSERT_TRUE(cleaned);
    EXPECT_TRUE(cleaned->workspace->error_code.empty()) << cleaned->workspace->error_code;
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(host_publications + local_publications + pastes, 0);
}
TEST_F(ClipboardControlFixture, RemoteCopyAutomaticallyReturnsVerifiedFormatsAndFilesWithoutInjectingPaste) {
    enable_return();
    EXPECT_TRUE(automatic_operation.empty()); // Connecting does not export an old clipboard.
    remote_copy(); ASSERT_FALSE(automatic_operation.empty());
    const auto result = finish(automatic_operation); ASSERT_TRUE(result);
    ASSERT_TRUE(result->workspace->error_code.empty()) << result->workspace->error_code;
    EXPECT_EQ(local_publications, 1); EXPECT_EQ(host_publications, 0); EXPECT_EQ(pastes, 0);
    const auto& snapshot = result->workspace->snapshot_path;
    const auto path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(snapshot.data()), snapshot.size()));
    PreparedClipboardPayload verified; const std::atomic_bool cancelled{false}; std::string error;
    EXPECT_TRUE(verified.load(path, cancelled, &error)) << error;
    wire::ClipboardPayloadManifestV1 manifest;
    std::ifstream metadata(path / "metadata/manifest.pb", std::ios::binary);
    ASSERT_TRUE(manifest.ParseFromIstream(&metadata)); EXPECT_EQ(manifest.formats_size(), 6);
    std::ifstream copied(path / "files/0/source/data", std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(copied), std::istreambuf_iterator<char>()};
    EXPECT_EQ(bytes, std::string(65539, 'C'));
    EXPECT_TRUE(std::filesystem::is_directory(path / "files/0/source/empty"));
    const auto completed_id = automatic_operation;
    for (int tick = 0; tick < 20; ++tick) step();
    EXPECT_EQ(automatic_operation, completed_id); EXPECT_EQ(local_publications, 1);
}
TEST_F(ClipboardControlFixture, LocalCopyDuringReturnPreventsRemotePublication) {
    enable_return(); remote_copy(); ASSERT_FALSE(automatic_operation.empty());
    ++local_clipboard_sequence;
    const auto result = finish(automatic_operation); ASSERT_TRUE(result);
    EXPECT_EQ(result->workspace->error_code, "clipboard_local_changed");
    EXPECT_EQ(local_publications, 0); EXPECT_EQ(pastes, 0);
}
TEST_F(ClipboardControlFixture, ANewerRemoteCopyCannotBeCapturedAsTheRequestedRevision) {
    host->peer_capability(1, "controller", 4); controller->peer_capability(1, "host", 4); step();
    auto copy = command(2, "stale-copy"); copy.workspace->clipboard_source.clear();
    copy.workspace->direction = TransferDirectionV1::kToController;
    copy.workspace->clipboard_sequence = local_clipboard_sequence;
    copy.workspace->clipboard_revision = host_clipboard_sequence.load() - 1;
    controller->from_gui(copy);
    const auto result = finish("stale-copy"); ASSERT_TRUE(result);
    EXPECT_EQ(result->workspace->error_code, "clipboard_changed_before_snapshot"); EXPECT_EQ(local_publications, 0);
}
TEST_F(ClipboardControlFixture, BusyHostRejectionPreservesReturnIdentityAndReleasesController) {
    host->peer_capability(1, "controller", 4); controller->peer_capability(1, "host", 4); step();
    ASSERT_TRUE(hg.begin("host", "other-operation"));
    auto copy = command(2, "busy-return"); copy.workspace->clipboard_source.clear();
    copy.workspace->direction = TransferDirectionV1::kToController;
    copy.workspace->clipboard_sequence = local_clipboard_sequence;
    copy.workspace->clipboard_revision = host_clipboard_sequence;
    controller->from_gui(copy);
    for (int tick = 0; tick < 12; ++tick) step();
    EXPECT_FALSE(cg.blocks_mutation()); EXPECT_TRUE(hg.blocks_mutation());
    const auto found = std::find_if(results.begin(), results.end(), [](const auto& message) {
        return message.request_id == "busy-return" && message.workspace->action == Action::kFinished;
    });
    ASSERT_NE(found, results.end()); EXPECT_EQ(found->workspace->error_code, "workspace_transfer_busy");
    EXPECT_EQ(found->workspace->clipboard_mode, 2U); EXPECT_EQ(found->workspace->clipboard_revision, 100U);
    EXPECT_EQ(local_publications, 0); hg.disconnected();
}
TEST_F(ClipboardControlFixture, PublishingToHostDoesNotEchoAndReconnectDoesNotReplayClipboard) {
    enable_return();
    controller->from_gui(command(2, "local-write"));
    const auto result = finish("local-write"); ASSERT_TRUE(result); EXPECT_TRUE(result->workspace->error_code.empty());
    for (int tick = 0; tick < 10; ++tick) step();
    EXPECT_EQ(host_publications, 1); EXPECT_EQ(local_publications, 0); EXPECT_TRUE(automatic_operation.empty());
    host->channel_open(false); controller->channel_open(false); step(); coordinator->disconnected();
    ++host_clipboard_sequence;
    host->peer_capability(1, "controller", 3); controller->peer_capability(1, "host", 3);
    host->channel_open(true); controller->channel_open(true);
    for (int tick = 0; tick < 10; ++tick) step();
    EXPECT_TRUE(automatic_operation.empty());
    host->channel_open(false); controller->channel_open(false); step(); coordinator->disconnected();
    ++host_clipboard_sequence;
    host->peer_capability(1, "controller", 4); controller->peer_capability(1, "host", 4);
    host->channel_open(true); controller->channel_open(true);
    for (int tick = 0; tick < 10; ++tick) step();
    EXPECT_TRUE(automatic_operation.empty()); // Upgrade enables new copies, not replay.
    remote_copy(); ASSERT_FALSE(automatic_operation.empty());
    ASSERT_TRUE(finish(automatic_operation)); EXPECT_EQ(local_publications, 1);
}
TEST_F(ClipboardControlFixture, VersionThreePeersKeepBothDirectionsWithoutAutomaticNotifications) {
    ++host_clipboard_sequence;
    for (int tick = 0; tick < 8; ++tick) step();
    EXPECT_TRUE(std::none_of(results.begin(), results.end(), [](const auto& message) {
        return message.workspace->action == Action::kClipboardChanged;
    }));
    auto incoming = command(2, "v3-remote-copy"); incoming.workspace->clipboard_source.clear();
    incoming.workspace->direction = TransferDirectionV1::kToController;
    controller->from_gui(incoming); auto result = finish("v3-remote-copy"); ASSERT_TRUE(result);
    EXPECT_TRUE(result->workspace->error_code.empty()); EXPECT_EQ(local_publications, 1);
    controller->from_gui(command(2, "v3-local-copy")); result = finish("v3-local-copy"); ASSERT_TRUE(result);
    EXPECT_TRUE(result->workspace->error_code.empty()); EXPECT_EQ(host_publications, 1);
    incoming.workspace->clipboard_revision = host_clipboard_sequence;
    incoming.session_epoch = "host";
    EXPECT_FALSE(controller->receive_control(incoming));
    incoming.session_epoch = "controller"; EXPECT_FALSE(host->receive_control(incoming));
}
}
