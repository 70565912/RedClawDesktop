#include "redclaw/workspace/transfer_runtime_bridge.h"
#include "redclaw/workspace/clipboard_copy_store.h"
#include "redclaw_wire.pb.h"
#include <gtest/gtest.h>
#include <fstream>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include <Windows.h>
#include <ole2.h>
#include <shldisp.h>
#include <shlobj.h>
#endif

namespace {
using namespace redclaw::workspace;
using namespace redclaw::protocol;
using Action = WorkspaceActionV1;
using Control = StreamControlMessageV1;
class FakePort final : public ClipboardDemandPort {
public:
    ClipboardDemandOffer local, remote;
    std::vector<ClipboardFileSelection> files;
    std::deque<ClipboardDemandEvent> events;
    std::shared_ptr<ClipboardDescription> described;
    std::shared_ptr<PreparedClipboardPayload> prepared;
    std::filesystem::path received, owned_root;
    int approved = 0, delivered = 0, cancelled = 0, commits = 0, source_queries = 0, installed = 0;
    void connected(bool, std::string) override {}
    std::optional<ClipboardDemandEvent> take_event() override {
        if (events.empty()) return {}; auto result = events.front(); events.pop_front(); return result;
    }
    void remote_offer(ClipboardDemandOffer offer, std::string invoke) override {
        remote = offer; if (!invoke.empty()) { ++installed; events.push_back({ClipboardDemandEventKind::kInstalled, invoke, offer}); }
    }
    void refresh_for_shortcut(std::string operation, std::uint32_t sequence) override {
        if (sequence == local.sequence) {
            events.push_back({ClipboardDemandEventKind::kOffer, {}, local});
            events.push_back({ClipboardDemandEventKind::kShortcutReady, operation, local});
        }
    }
    bool source(std::string_view token, std::uint32_t& sequence, std::vector<ClipboardFileSelection>& selected) override {
        ++source_queries; if (token != local.token) return false;
        sequence = local.sequence; selected = files; return true;
    }
    void description(std::string, std::shared_ptr<ClipboardDescription> value, std::string) override { described = value; }
    void approve(std::string operation) override {
        ++approved;
        events.push_back({ClipboardDemandEventKind::kRead, operation, remote});
    }
    void payload(std::string, std::shared_ptr<PreparedClipboardPayload> value, std::filesystem::path directory, std::string) override {
        prepared = std::move(value); received = std::move(directory); ++delivered;
    }
    void cancel(std::string) override { ++cancelled; }
    void commit_source(std::string operation, std::string token, std::shared_ptr<ClipboardDescription> description) override {
        ++commits; std::string error; const std::atomic_bool stop{false};
        bool ok = token == local.token && description && description->sources_unchanged(stop, &error);
        // This fake stands in for the original application's delete-on-paste
        // callback. Every exact file was created by this fixture, never a user file.
        if (ok) for (auto item = description->identities.rbegin(); item != description->identities.rend(); ++item) {
            const auto path = item->path.lexically_normal();
            if (!path.string().starts_with(owned_root.string() + std::string(1, std::filesystem::path::preferred_separator))) {
                ok = false; break;
            }
            std::error_code ec; if (!std::filesystem::remove(path, ec) || ec) { ok = false; break; }
        }
        events.push_back({ClipboardDemandEventKind::kMoveReceipt, operation, local, ok, ok ? 2U : 0U,
            ok ? "" : error.empty() ? "clipboard_source_retained" : error});
    }
};
class DemandPair : public testing::Test {
protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("redclaw-demand-" + ClipboardCopyStore::new_id());
    std::shared_ptr<FakePort> hp = std::make_shared<FakePort>(), cp = std::make_shared<FakePort>();
    TransferOperationGate hg, cg;
    std::unique_ptr<TransferRuntimeBridge> host, controller;
    std::deque<Control> hc, ch;
    std::deque<std::string> hb, cb;
    std::vector<Control> gui;
    int captures = 0;
    ClipboardFocusToken focus{1, 2, 3, 4, 5, 6, 7};
    int injected = 0;
    int published = 0;
    std::string formats_descriptor;
    std::uint64_t now = 0, sequence = 0;
    bool send(Control value, std::deque<Control>& queue) {
        value.message_id = ++sequence; value.sent_at_ms = 1;
        const auto bytes = serialize_stream_control_message_v1(value);
        if (bytes.empty()) { ADD_FAILURE() << "unserializable action=" << int(value.workspace->action); return false; }
        const auto parsed = parse_stream_control_message_v1(bytes);
        if (!parsed.ok) { ADD_FAILURE() << parsed.error; return false; }
        queue.push_back(parsed.value); return true;
    }
    ClipboardHostActions actions(std::shared_ptr<FakePort> port) {
        ClipboardHostActions result; result.demand_port = std::move(port);
        result.publish_eligible = [] { return true; }; result.eligibility = [] { return ClipboardInputEligibility{true, 1}; };
        result.focus_snapshot = [this] { return std::optional<ClipboardFocusToken>(focus); };
        result.submit_paste = [this](auto, auto*) { ++injected; return true; };
        result.publish = [this](auto&, const auto& guard, auto*) { if (!guard || !guard()) return false; ++published; return true; };
        result.capture = [this](ClipboardSnapshot& snapshot, const auto& spool, auto, const auto& cancelled, auto* error) {
            ++captures;
            if (!formats_descriptor.empty()) return snapshot.create(spool, formats_descriptor, cancelled, error);
            redclaw::protocol::wire::ClipboardSourceV1 source; source.set_schema_version(1);
            const auto path = (root / "source").generic_u8string();
            source.add_files(std::string(reinterpret_cast<const char*>(path.data()), path.size()));
            return snapshot.create(spool, source.SerializeAsString(), cancelled, error);
        };
        return result;
    }
    void SetUp() override {
        std::filesystem::create_directories(root / "host"); std::filesystem::create_directories(root / "controller");
        std::filesystem::create_directories(root / "source" / "empty");
        std::ofstream(root / "source" / "data", std::ios::binary) << std::string(65539, 'D');
        hp->owned_root = cp->owned_root = root;
        host = std::make_unique<TransferRuntimeBridge>(true, "host", root / "host", hg,
            [&](auto value) { return send(value, hc); }, [](auto) { return true; },
            [&](auto bytes) { hb.emplace_back(bytes); return true; }, [] { return true; }, actions(hp));
        controller = std::make_unique<TransferRuntimeBridge>(false, "controller", root / "controller", cg,
            [&](auto value) { return send(value, ch); }, [&](auto value) { gui.push_back(value); return true; },
            [&](auto bytes) { cb.emplace_back(bytes); return true; }, [] { return true; }, actions(cp));
        host->peer_capability(1, "controller", 5); controller->peer_capability(1, "host", 5);
        host->channel_open(true); controller->channel_open(true); step();
    }
    void TearDown() override {
        controller.reset(); host.reset();
        std::error_code ec; std::filesystem::remove_all(root, ec);
    }
    void step() {
        now += 10;
        while (!ch.empty()) { EXPECT_TRUE(host->receive_control(ch.front())); ch.pop_front(); }
        while (!hc.empty()) { EXPECT_TRUE(controller->receive_control(hc.front())); hc.pop_front(); }
        while (!cb.empty()) { EXPECT_TRUE(host->receive_bulk(cb.front())); cb.pop_front(); }
        while (!hb.empty()) { EXPECT_TRUE(controller->receive_bulk(hb.front())); hb.pop_front(); }
        host->pump(now, true); controller->pump(now, true);
    }
    bool wait(const std::function<bool()>& ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) { step(); if (ready()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        return false;
    }
    std::optional<Control> event(Action action, std::string id = "paste") {
        for (auto it = gui.rbegin(); it != gui.rend(); ++it)
            if (it->request_id == id && it->workspace->action == action) return *it;
        return {};
    }
    void offer(bool to_host, bool move) {
        auto source = to_host ? cp : hp;
        source->local = {"selected-source", kClipboardFilesFormat, 42, move};
        source->files = {{root / "source", "files/0/source"}};
        source->events.push_back({ClipboardDemandEventKind::kOffer, {}, source->local});
        for (int i = 0; i < 5; ++i) step();
    }
    bool describe(bool to_host) {
        auto destination = to_host ? hp : cp;
        destination->events.push_back({ClipboardDemandEventKind::kDescribe, "paste", destination->remote});
        return wait([&] { return bool(destination->described); });
    }
    bool demand(bool to_host) {
        auto destination = to_host ? hp : cp;
        destination->events.push_back({ClipboardDemandEventKind::kDemand, "paste", destination->remote});
        return wait([&] { return bool(event(Action::kClipboardConfirm)); });
    }
    void approve() {
        auto confirm = event(Action::kClipboardConfirm); ASSERT_TRUE(confirm);
        auto command = *confirm; command.workspace->action = Action::kClipboardApprove; command.workspace->clipboard_complete = true;
        controller->from_gui(command);
    }
};
TEST_F(DemandPair, CopyAndCutOffersNeverReadPayloadOrAcquireTheInputGateInEitherDirection) {
    for (const bool direction : {false, true}) for (const bool move : {false, true}) {
        offer(direction, move); for (int i = 0; i < 20; ++i) step();
        EXPECT_EQ(captures, 0); EXPECT_FALSE(hg.blocks_mutation()); EXPECT_FALSE(cg.blocks_mutation());
        EXPECT_FALSE(event(Action::kClipboardConfirm)); EXPECT_EQ(hp->delivered + cp->delivered, 0);
    }
}
TEST_F(DemandPair, MetadataAndRejectedConfirmationDoNotReadFileContents) {
    offer(false, true); ASSERT_TRUE(describe(false)); ASSERT_TRUE(demand(false));
    EXPECT_EQ(cp->described->totals.bytes, 65539U); EXPECT_EQ(captures, 0);
    EXPECT_FALSE(hg.blocks_mutation()); EXPECT_FALSE(cg.blocks_mutation());
    auto cancel = *event(Action::kClipboardConfirm); cancel.workspace->action = Action::kClipboardApprove;
    cancel.workspace->clipboard_complete = false; controller->from_gui(cancel);
    EXPECT_TRUE(wait([&] { return bool(event(Action::kFinished)); }));
    EXPECT_EQ(captures, 0); EXPECT_EQ(hp->commits, 0); EXPECT_TRUE(std::filesystem::exists(root / "source" / "data"));
}
TEST_F(DemandPair, VerifiedReturnWaitsForNativePasteAndDuplicateReceiptDoesNotRepeatIt) {
    offer(false, false); ASSERT_TRUE(describe(false)); ASSERT_TRUE(demand(false)); approve();
    ASSERT_TRUE(wait([&] { return cp->delivered == 1 && !hg.blocks_mutation() && !cg.blocks_mutation(); }));
    EXPECT_EQ(captures, 1); EXPECT_FALSE(event(Action::kFinished));
    std::ifstream input(cp->received / "files/0/source/data", std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), std::string(65539, 'D'));
    cp->events.push_back({ClipboardDemandEventKind::kNativeResult, "paste", cp->remote, true, 1});
    ASSERT_TRUE(wait([&] { return bool(event(Action::kFinished)); }));
    EXPECT_TRUE(event(Action::kFinished)->workspace->error_code.empty());
    EXPECT_EQ(event(Action::kFinished)->workspace->bytes, 65539U);
    EXPECT_EQ(event(Action::kFinished)->workspace->committed_bytes, 65539U);
    EXPECT_EQ(event(Action::kFinished)->workspace->files, 1U);
    cp->events.push_back({ClipboardDemandEventKind::kNativeResult, "paste", cp->remote, true, 1});
    for (int i = 0; i < 10; ++i) step();
    EXPECT_EQ(captures, 1); EXPECT_EQ(hp->commits, 0);
}
TEST_F(DemandPair, UploadUsesTheSameConfirmationAndNativeCompletion) {
    offer(true, false); ASSERT_TRUE(describe(true)); ASSERT_TRUE(demand(true)); approve();
    ASSERT_TRUE(wait([&] { return hp->delivered == 1 && !hg.blocks_mutation() && !cg.blocks_mutation(); }));
    EXPECT_EQ(captures, 1); EXPECT_FALSE(event(Action::kFinished));
    hp->events.push_back({ClipboardDemandEventKind::kNativeResult, "paste", hp->remote, true, 1});
    ASSERT_TRUE(wait([&] { return bool(event(Action::kFinished)); }));
    EXPECT_TRUE(event(Action::kFinished)->workspace->error_code.empty());
}
TEST_F(DemandPair, CutRemovesOnlyFixtureSourcesAfterVerifiedNativeMoveReceipt) {
    offer(false, true); ASSERT_TRUE(describe(false)); ASSERT_TRUE(demand(false)); approve();
    ASSERT_TRUE(wait([&] { return cp->delivered == 1 && !hg.blocks_mutation() && !cg.blocks_mutation(); }));
    EXPECT_EQ(hp->commits, 0); ASSERT_TRUE(std::filesystem::exists(root / "source" / "data"));
    auto stale = cp->remote; stale.token = "obsolete-version";
    cp->events.push_back({ClipboardDemandEventKind::kNativeResult, "paste", stale, true, 2});
    for (int i = 0; i < 5; ++i) step(); EXPECT_EQ(hp->commits, 0); EXPECT_FALSE(event(Action::kFinished));
    cp->events.push_back({ClipboardDemandEventKind::kNativeResult, "paste", cp->remote, true, 2});
    ASSERT_TRUE(wait([&] { return bool(event(Action::kFinished)); }));
    EXPECT_EQ(hp->commits, 1); EXPECT_TRUE(event(Action::kFinished)->workspace->error_code.empty());
    EXPECT_FALSE(std::filesystem::exists(root / "source"));
    cp->events.push_back({ClipboardDemandEventKind::kNativeResult, "paste", cp->remote, true, 2});
    for (int i = 0; i < 10; ++i) step(); EXPECT_EQ(hp->commits, 1);
}
TEST_F(DemandPair, UploadCutWaitsForDestinationMoveFeedbackAndOriginalSourceCompletion) {
    offer(true, true); ASSERT_TRUE(describe(true)); ASSERT_TRUE(demand(true)); approve();
    ASSERT_TRUE(wait([&] { return hp->delivered == 1 && !hg.blocks_mutation() && !cg.blocks_mutation(); }));
    EXPECT_EQ(cp->commits, 0); EXPECT_TRUE(std::filesystem::exists(root / "source/data"));
    hp->events.push_back({ClipboardDemandEventKind::kNativeResult, "paste", hp->remote, true, 2});
    ASSERT_TRUE(wait([&] { return bool(event(Action::kFinished)); }));
    EXPECT_EQ(cp->commits, 1); EXPECT_FALSE(std::filesystem::exists(root / "source"));
    EXPECT_TRUE(event(Action::kFinished)->workspace->error_code.empty());
    for (int i = 0; i < 8; ++i) step(); EXPECT_EQ(cp->commits, 1);
}
TEST_F(DemandPair, ChangedSourceAndMissingNativeReceiptKeepSourceFiles) {
    offer(false, true); ASSERT_TRUE(describe(false)); ASSERT_TRUE(demand(false)); approve();
    ASSERT_TRUE(wait([&] { return cp->delivered == 1 && !hg.blocks_mutation() && !cg.blocks_mutation(); }));
    std::ofstream(root / "source" / "data", std::ios::app) << "changed";
    cp->events.push_back({ClipboardDemandEventKind::kNativeResult, "paste", cp->remote, true, 2});
    ASSERT_TRUE(wait([&] { return bool(event(Action::kFinished)); }));
    EXPECT_FALSE(event(Action::kFinished)->workspace->error_code.empty());
    EXPECT_TRUE(std::filesystem::exists(root / "source" / "data"));
}
TEST_F(DemandPair, ElapsedTimeNeverCommitsOrAbortsNativePasteAndExplicitCancelKeepsSources) {
    offer(true, true); ASSERT_TRUE(describe(true)); ASSERT_TRUE(demand(true)); approve();
    ASSERT_TRUE(wait([&] { return hp->delivered == 1 && !hg.blocks_mutation() && !cg.blocks_mutation(); }));
    now += 31000; step(); EXPECT_FALSE(event(Action::kFinished)); EXPECT_EQ(cp->commits, 0);
    auto cancel = *event(Action::kClipboardConfirm); cancel.workspace->action = Action::kCancel;
    controller->from_gui(cancel);
    ASSERT_TRUE(event(Action::kFinished)); EXPECT_EQ(cp->commits, 0);
    EXPECT_TRUE(std::filesystem::exists(root / "source" / "data"));
}
TEST_F(DemandPair, PartialNativePasteKeepsEverySourceAndDisconnectDiscardsLateReceipts) {
    offer(false, true); ASSERT_TRUE(describe(false)); ASSERT_TRUE(demand(false)); approve();
    ASSERT_TRUE(wait([&] { return cp->delivered == 1 && !hg.blocks_mutation() && !cg.blocks_mutation(); }));
    cp->events.push_back({ClipboardDemandEventKind::kNativeResult, "paste", cp->remote, false, 2, "clipboard_native_paste_incomplete"});
    ASSERT_TRUE(wait([&] { return bool(event(Action::kFinished)); }));
    EXPECT_EQ(hp->commits, 0); EXPECT_TRUE(std::filesystem::exists(root / "source/data"));
    controller->channel_open(false); step();
    cp->events.push_back({ClipboardDemandEventKind::kNativeResult, "paste", cp->remote, true, 2});
    for (int i = 0; i < 5; ++i) step();
    EXPECT_EQ(hp->commits, 0); EXPECT_EQ(injected, 0);
}
TEST_F(DemandPair, ANewCopyCancelsConfirmationWithoutReadingAndCannotReplayOldApproval) {
    offer(false, true); ASSERT_TRUE(describe(false)); ASSERT_TRUE(demand(false));
    auto old = *event(Action::kClipboardConfirm); hp->local.token = "replacement-source";
    hp->events.push_back({ClipboardDemandEventKind::kOffer, {}, hp->local});
    ASSERT_TRUE(wait([&] { return bool(event(Action::kFinished)); }));
    old.workspace->action = Action::kClipboardApprove; old.workspace->clipboard_complete = true;
    controller->from_gui(old); for (int i = 0; i < 5; ++i) step();
    EXPECT_EQ(captures, 0); EXPECT_EQ(cp->delivered, 0); EXPECT_EQ(hp->commits, 0);
}
TEST_F(DemandPair, ShortcutInstallsOnceAndRejectsChangedOriginalFocus) {
    offer(true, false);
    Control shortcut; shortcut.type = StreamControlMessageTypeV1::kWorkspace; shortcut.request_id = "shortcut";
    auto& value = shortcut.workspace.emplace(); value.action = Action::kClipboardShortcut;
    value.purpose = WorkspaceTransferPurposeV1::kClipboard; value.clipboard_mode = 6;
    value.clipboard_token = "local-shortcut"; value.clipboard_sequence = 42;
    controller->from_gui(shortcut);
    step(); step(); // Host captures the focus before installing its OLE proxy.
    EXPECT_EQ(hp->installed, 1);
    ++focus.generation;
    step(); EXPECT_EQ(injected, 0); EXPECT_EQ(captures, 0);
}
TEST_F(DemandPair, OldShortcutCannotSilentlyConvertAFileCutIntoACopyAtTheNewHost) {
    host->peer_capability(1, "controller", 4); controller->peer_capability(1, "host", 4); step();
    Control request; request.type = StreamControlMessageTypeV1::kWorkspace; request.request_id = "legacy-file-paste";
    auto& value = request.workspace.emplace(); value.action = Action::kPrepare;
    value.purpose = WorkspaceTransferPurposeV1::kClipboard; value.clipboard_sequence = 42;
    controller->from_gui(request);
    ASSERT_TRUE(wait([&] { return bool(event(Action::kFinished, request.request_id)); }));
    EXPECT_EQ(event(Action::kFinished, request.request_id)->workspace->error_code, "clipboard_native_peer_unsupported");
    EXPECT_EQ(injected, 0); EXPECT_EQ(hp->delivered + cp->delivered, 0);
    EXPECT_TRUE(std::filesystem::exists(root / "source/data"));
}
TEST(ClipboardDescription, SimulatedThirteenGigabytesHasOnlyBoundedMetadata) {
    ClipboardDescription description; description.roots = 31;
    constexpr std::uint64_t bytes = 13ULL * 1024 * 1024 * 1024;
    for (unsigned i = 0; i < 31; ++i) description.entries.push_back({"file" + std::to_string(i),
        "files/" + std::to_string(i) + "/file" + std::to_string(i), i ? 0 : bytes, false});
    const auto wire = description.serialize(); EXPECT_LT(wire.size(), 4096U);
    std::string error; const auto parsed = ClipboardDescription::parse(wire, &error);
    ASSERT_TRUE(parsed) << error; EXPECT_EQ(parsed->totals.bytes, bytes); EXPECT_EQ(parsed->totals.files, 31U);
}
#ifdef _WIN32
TEST_F(DemandPair, CommonOldCapabilitiesRetainExplicitTextPasteWithoutPublishingTheInteractiveClipboard) {
    redclaw::protocol::wire::ClipboardSourceV1 descriptor; descriptor.set_schema_version(1);
    auto* format = descriptor.add_formats(); format->set_kind(1);
    const std::wstring text = L"old/new fixture";
    format->set_data(reinterpret_cast<const char*>(text.c_str()), (text.size() + 1) * sizeof(wchar_t));
    formats_descriptor = descriptor.SerializeAsString();
    for (const unsigned version : {2U, 3U, 4U}) {
        host->peer_capability(1, "controller", version); controller->peer_capability(1, "host", version); step();
        Control request; request.type = StreamControlMessageTypeV1::kWorkspace;
        request.request_id = "legacy-text-" + std::to_string(version); auto& value = request.workspace.emplace();
        value.action = Action::kPrepare; value.purpose = WorkspaceTransferPurposeV1::kClipboard; value.clipboard_sequence = 42;
        controller->from_gui(request);
        ASSERT_TRUE(wait([&] { return bool(event(Action::kFinished, request.request_id)); }));
        EXPECT_TRUE(event(Action::kFinished, request.request_id)->workspace->error_code.empty());
        for (int i = 0; i < 5; ++i) step();
    }
    EXPECT_EQ(published, 3); EXPECT_EQ(injected, 3);
}
TEST_F(DemandPair, BothDirectionsPreserveEveryExistingTextRichTextAndImageFormatOnDemand) {
    redclaw::protocol::wire::ClipboardSourceV1 descriptor; descriptor.set_schema_version(1);
    const std::wstring text = L"RedClaw \u4e2d\u6587\r\n";
    const auto bitmap = [](bool v5) {
        BITMAPV5HEADER header{}; header.bV5Size = v5 ? sizeof(header) : sizeof(BITMAPINFOHEADER);
        header.bV5Width = header.bV5Height = 1; header.bV5Planes = 1; header.bV5BitCount = 32; header.bV5SizeImage = 4;
        std::string bytes(reinterpret_cast<const char*>(&header), header.bV5Size);
        return bytes + std::string("\x71\x29\xde\xff", 4);
    };
    std::string png;
    const std::string hex = "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000b49444154789c636000020000050001a5f645400000000049454e44ae426082";
    for (std::size_t i = 0; i < hex.size(); i += 2) png += static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16));
    const std::vector<std::string> expected = {
        std::string(reinterpret_cast<const char*>(text.c_str()), (text.size() + 1) * sizeof(wchar_t)),
        std::string("Version:1.0\r\n<html><b>fixture</b></html>") + '\0',
        std::string("{\\rtf1 fixture}") + '\0',
        png, bitmap(true), bitmap(false)};
    const std::vector<UINT> formats = {CF_UNICODETEXT, RegisterClipboardFormatW(L"HTML Format"),
        RegisterClipboardFormatW(L"Rich Text Format"), RegisterClipboardFormatW(L"PNG"), CF_DIBV5, CF_DIB};
    for (unsigned i = 0; i < 6; ++i) { auto* value = descriptor.add_formats(); value->set_kind(i + 1); value->set_data(expected[i]); }
    formats_descriptor = descriptor.SerializeAsString(); ASSERT_TRUE(SUCCEEDED(OleInitialize(nullptr)));
    for (const bool to_host : {false, true}) {
        auto source = to_host ? cp : hp; auto destination = to_host ? hp : cp;
        gui.clear(); source->local = {to_host ? "plain-upload" : "plain-return", 63, 42, false};
        source->files.clear(); source->events.push_back({ClipboardDemandEventKind::kOffer, {}, source->local});
        for (int i = 0; i < 6; ++i) step();
        destination->events.push_back({ClipboardDemandEventKind::kDemand, "plain", destination->remote});
        ASSERT_TRUE(wait([&] { return destination->prepared && !hg.blocks_mutation() && !cg.blocks_mutation(); }));
        EXPECT_FALSE(event(Action::kClipboardConfirm, "plain"));
        std::vector<ClipboardDemandEvent> events;
        std::function<void(std::shared_ptr<PreparedClipboardPayload>, std::shared_ptr<ClipboardDescription>, std::filesystem::path)> supply;
        auto* object = static_cast<IDataObject*>(make_clipboard_data_object_for_test(destination->remote,
            [&](auto event) { events.push_back(event); }, &supply));
        supply(destination->prepared, {}, {});
        for (unsigned i = 0; i < 6; ++i) {
            FORMATETC request{static_cast<CLIPFORMAT>(formats[i]), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{}; ASSERT_EQ(object->GetData(&request, &medium), S_OK);
            ASSERT_GE(GlobalSize(medium.hGlobal), expected[i].size());
            const auto* bytes = static_cast<const char*>(GlobalLock(medium.hGlobal));
            EXPECT_EQ(std::string(bytes, expected[i].size()), expected[i]);
            GlobalUnlock(medium.hGlobal); ReleaseStgMedium(&medium);
        }
        EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) { return event.kind == ClipboardDemandEventKind::kNativeResult; }), 1);
        object->Release();
        destination->events.push_back({ClipboardDemandEventKind::kNativeResult, "plain", destination->remote, true, 1});
        ASSERT_TRUE(wait([&] { return bool(event(Action::kFinished, "plain")); }));
        for (int i = 0; i < 5; ++i) step();
    }
    EXPECT_EQ(captures, 2); OleUninitialize();
}
TEST_F(DemandPair, DetachedNativeMenuConsumerReadsVerifiedStreamAndWaitsForBothMoveFeedbackFormats) {
    offer(false, true); ASSERT_TRUE(describe(false)); ASSERT_TRUE(demand(false)); approve();
    ASSERT_TRUE(wait([&] { return cp->delivered == 1 && !hg.blocks_mutation() && !cg.blocks_mutation(); }));
    ASSERT_TRUE(SUCCEEDED(OleInitialize(nullptr)));
    std::vector<ClipboardDemandEvent> events;
    std::function<void(std::shared_ptr<PreparedClipboardPayload>, std::shared_ptr<ClipboardDescription>, std::filesystem::path)> supply;
    auto* object = static_cast<IDataObject*>(make_clipboard_data_object_for_test(cp->remote,
        [&](auto event) { events.push_back(std::move(event)); }, &supply));
    supply(cp->prepared, cp->described, cp->received);
    FORMATETC descriptor{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW")), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{}; ASSERT_EQ(object->GetData(&descriptor, &medium), S_OK);
    auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal)); ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->cItems, cp->described->entries.size());
    GlobalUnlock(medium.hGlobal); ReleaseStgMedium(&medium);
    std::size_t index = 0; while (cp->described->entries[index].directory) ++index;
    FORMATETC content{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents")), nullptr, DVASPECT_CONTENT, static_cast<LONG>(index), TYMED_ISTREAM};
    ASSERT_EQ(object->GetData(&content, &medium), S_OK);
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) { return event.kind == ClipboardDemandEventKind::kRead; }), 0);
    STATSTG stat{}; EXPECT_EQ(medium.pstm->Stat(&stat, STATFLAG_NONAME), S_OK); EXPECT_EQ(stat.cbSize.QuadPart, 65539U);
    std::string bytes(65539, '\0'); ULONG read = 0;
    EXPECT_EQ(medium.pstm->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read), S_OK);
    EXPECT_EQ(bytes, std::string(65539, 'D')); EXPECT_EQ(read, 65539U); ReleaseStgMedium(&medium);
    IDataObjectAsyncCapability* async = nullptr;
    ASSERT_EQ(object->QueryInterface(__uuidof(IDataObjectAsyncCapability), reinterpret_cast<void**>(&async)), S_OK);
    EXPECT_EQ(async->EndOperation(S_OK, nullptr, DROPEFFECT_MOVE), S_OK);
    const auto results = [&] { return std::count_if(events.begin(), events.end(), [](const auto& event) { return event.kind == ClipboardDemandEventKind::kNativeResult; }); };
    EXPECT_EQ(results(), 0);
    for (const auto* name : {L"Performed DropEffect", L"Paste Succeeded"}) {
        FORMATETC feedback{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM value{}; value.tymed = TYMED_HGLOBAL; value.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        *static_cast<DWORD*>(GlobalLock(value.hGlobal)) = DROPEFFECT_MOVE; GlobalUnlock(value.hGlobal);
        EXPECT_EQ(object->SetData(&feedback, &value, TRUE), S_OK);
    }
    ASSERT_EQ(results(), 1); EXPECT_TRUE(events.back().complete); EXPECT_EQ(events.back().effect, 2U);
    EXPECT_EQ(hp->commits, 0); // Detached native success does not bypass the session's verified receipt.
    async->Release(); object->Release(); OleUninitialize();
}
TEST(ClipboardNativeObject, FormatQueriesAndReleaseDoNotRequestAnyPayload) {
    const auto initialized = OleInitialize(nullptr); ASSERT_TRUE(SUCCEEDED(initialized));
    int events = 0;
    auto* object = static_cast<IDataObject*>(make_clipboard_data_object_for_test({"large-selection", 64, 1, true},
        [&](auto) { ++events; }, nullptr));
    ASSERT_NE(object, nullptr);
    IEnumFORMATETC* formats = nullptr; ASSERT_EQ(object->EnumFormatEtc(DATADIR_GET, &formats), S_OK);
    FORMATETC item{}; ULONG done = 0;
    while (formats->Next(1, &item, &done) == S_OK) EXPECT_EQ(object->QueryGetData(&item), S_OK);
    formats->Release();
    IDataObjectAsyncCapability* async = nullptr;
    ASSERT_EQ(object->QueryInterface(__uuidof(IDataObjectAsyncCapability), reinterpret_cast<void**>(&async)), S_OK);
    BOOL enabled = FALSE; EXPECT_EQ(async->GetAsyncMode(&enabled), S_OK); EXPECT_TRUE(enabled);
    async->Release(); object->Release(); EXPECT_EQ(events, 0); OleUninitialize();
}
#endif
}
