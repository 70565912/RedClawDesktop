// Capability-5 clipboard liveness: every shortcut or native demand either
// completes or ends with a visible, logged error. No payload contents here.
#include "redclaw/workspace/transfer_runtime_bridge.h"
#include "redclaw/input/input_module.h"
#include <gtest/gtest.h>
#include <algorithm>

namespace {
using namespace redclaw::workspace;
using namespace redclaw::protocol;
using Action = WorkspaceActionV1;
using Control = StreamControlMessageV1;
constexpr std::uint32_t kTextFormat = 1;

class FakePort final : public ClipboardDemandPort {
public:
    ClipboardDemandOffer local, remote;
    std::deque<ClipboardDemandEvent> events;
    int cancelled = 0, installed = 0, restarts = 0;
    bool restartable = true;
    void connected(bool, std::string) override {}
    std::optional<ClipboardDemandEvent> take_event() override {
        if (events.empty()) return {}; auto result = events.front(); events.pop_front(); return result;
    }
    void remote_offer(ClipboardDemandOffer offer, std::string invoke) override {
        remote = offer; if (!invoke.empty()) { ++installed; events.push_back({ClipboardDemandEventKind::kInstalled, invoke, offer}); }
    }
    void refresh_for_shortcut(std::string operation, std::uint32_t sequence) override {
        if (sequence != local.sequence) return; // simulates a native capture that never answers
        events.push_back({ClipboardDemandEventKind::kOffer, {}, local});
        events.push_back({ClipboardDemandEventKind::kShortcutReady, operation, local});
    }
    bool source(std::string_view token, std::uint32_t& sequence, std::vector<ClipboardFileSelection>&) override {
        if (token != local.token) return false; sequence = local.sequence; return true;
    }
    void description(std::string, std::shared_ptr<ClipboardDescription>, std::string) override {}
    void approve(std::string operation) override { events.push_back({ClipboardDemandEventKind::kRead, operation, remote}); }
    void payload(std::string, std::shared_ptr<PreparedClipboardPayload>, std::filesystem::path, std::string) override {}
    void cancel(std::string) override { ++cancelled; }
    void commit_source(std::string, std::string, std::shared_ptr<ClipboardDescription>) override {}
    bool restart() override { if (!restartable) return false; ++restarts; return true; }
};

class Liveness : public testing::Test {
protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("redclaw-liveness-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::shared_ptr<FakePort> hp = std::make_shared<FakePort>(), cp = std::make_shared<FakePort>();
    TransferOperationGate hg, cg;
    redclaw::input::InputPolicyGate input_gate;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession input{input_gate, backend};
    std::unique_ptr<TransferRuntimeBridge> host, controller;
    std::deque<Control> hc, ch;
    std::vector<Control> gui;
    std::vector<std::string> log;
    std::uint64_t now = 1000, sequence = 0;
    bool drop_to_controller = false, drop_to_host = false, legacy_host_wiring = false;
    std::function<void(Control&)> tamper_to_host;
    bool send(Control value, std::deque<Control>& queue, bool drop) {
        if (drop) return true;
        value.message_id = ++sequence; value.sent_at_ms = 1;
        const auto parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(value));
        if (!parsed.ok) { ADD_FAILURE() << parsed.error; return false; }
        queue.push_back(parsed.value); return true;
    }
    ClipboardHostActions actions(std::shared_ptr<FakePort> port, bool is_host) {
        ClipboardHostActions result; result.demand_port = std::move(port);
        result.publish_eligible = [] { return true; };
        result.focus_snapshot = [] { return std::optional<ClipboardFocusToken>(ClipboardFocusToken{1, 2, 3, 4, 5, 6, 7}); };
        result.diagnostic = [this](std::string_view line) { log.emplace_back(line); };
        if (!is_host) return result;
        // Production Host wiring: the real RemoteInputSession decides injection.
        result.eligibility = [this] { return ClipboardInputEligibility{input.clipboard_paste_eligible(), input.eligibility_revision()}; };
        result.submit_paste = [this](std::uint64_t revision, std::string* error) { return input.paste_verified_clipboard(revision, error); };
        if (!legacy_host_wiring)
            result.submit_shortcut_paste = [this](std::uint64_t revision, std::string* error) { return input.paste_shortcut_clipboard(revision, error); };
        return result;
    }
    void build() {
        std::filesystem::create_directories(root / "host"); std::filesystem::create_directories(root / "controller");
        host = std::make_unique<TransferRuntimeBridge>(true, "host", root / "host", hg,
            [&](auto value) { return send(value, hc, drop_to_controller); }, [](auto) { return true; },
            [](auto) { return true; }, [] { return true; }, actions(hp, true));
        controller = std::make_unique<TransferRuntimeBridge>(false, "controller", root / "controller", cg,
            [&](auto value) { return send(value, ch, drop_to_host); }, [&](auto value) { gui.push_back(value); return true; },
            [](auto) { return true; }, [] { return true; }, actions(cp, false));
        host->peer_capability(1, "controller", 5); controller->peer_capability(1, "host", 5);
        host->channel_open(true); controller->channel_open(true); step();
    }
    void SetUp() override {
        input.set_authorized(true); ASSERT_TRUE(input.request_active(1000));
    }
    void TearDown() override {
        controller.reset(); host.reset();
        std::error_code ec; std::filesystem::remove_all(root, ec);
    }
    void step(std::uint64_t elapsed = 10) {
        now += elapsed;
        while (!ch.empty()) { auto value = ch.front(); ch.pop_front(); if (tamper_to_host) tamper_to_host(value); EXPECT_TRUE(host->receive_control(value)); }
        while (!hc.empty()) { EXPECT_TRUE(controller->receive_control(hc.front())); hc.pop_front(); }
        host->pump(now, true); controller->pump(now, true);
    }
    void steps(int count) { for (int i = 0; i < count; ++i) step(); }
    void offer_text_to_host() {
        cp->local = {"controller-text", kTextFormat, 42, false};
        cp->events.push_back({ClipboardDemandEventKind::kOffer, {}, cp->local});
        steps(5);
    }
    void shortcut(const std::string& id, std::uint32_t clipboard_sequence = 42) {
        Control value; value.type = StreamControlMessageTypeV1::kWorkspace; value.request_id = id;
        auto& data = value.workspace.emplace(); data.action = Action::kClipboardShortcut;
        data.purpose = WorkspaceTransferPurposeV1::kClipboard; data.clipboard_mode = 6;
        data.clipboard_token = "local-shortcut"; data.clipboard_sequence = clipboard_sequence;
        controller->from_gui(value);
    }
    std::optional<std::string> finished(const std::string& id) {
        for (auto it = gui.rbegin(); it != gui.rend(); ++it)
            if (it->request_id == id && it->workspace && it->workspace->action == Action::kFinished) return it->workspace->error_code;
        return {};
    }
    bool logged(std::string_view needle) const {
        return std::any_of(log.begin(), log.end(), [&](const std::string& line) { return line.find(needle) != std::string::npos; });
    }
};

TEST_F(Liveness, CopyPublishesAFreshOfferToTheHostAndLogsIt) {
    build(); offer_text_to_host();
    EXPECT_EQ(hp->remote.token, "controller-text");
    EXPECT_TRUE(logged("clipboard role=controller event=offer_published"));
    EXPECT_TRUE(logged("clipboard role=host event=offer_received"));
}

TEST_F(Liveness, HostShortcutPasteInjectsWithoutATransferGate) {
    build(); offer_text_to_host();
    shortcut("shortcut"); steps(6);
    EXPECT_EQ(hp->installed, 1);
    ASSERT_EQ(backend.injected_events().size(), 4U);
    EXPECT_EQ(backend.injected_events()[0].key_code, 0x11); EXPECT_EQ(backend.injected_events()[1].key_code, 0x56);
    EXPECT_TRUE(logged("event=paste_inject")); EXPECT_TRUE(logged("result=ok"));
    EXPECT_FALSE(finished("shortcut"));
}

TEST_F(Liveness, LegacyGateOnlyPasteFailsVisiblyInsteadOfSilently) {
    legacy_host_wiring = true; build(); offer_text_to_host();
    shortcut("shortcut"); steps(8);
    EXPECT_TRUE(backend.injected_events().empty());
    ASSERT_TRUE(finished("shortcut"));
    EXPECT_EQ(*finished("shortcut"), "clipboard_input_revoked");
}

TEST_F(Liveness, ControllerCancelsAHostDemandForAStaleToken) {
    build(); offer_text_to_host();
    hp->events.push_back({ClipboardDemandEventKind::kDemand, "paste", {"replaced-text", kTextFormat, 41, false}});
    steps(6);
    EXPECT_GE(hp->cancelled, 1);
    EXPECT_TRUE(logged("clipboard role=controller event=reject op=paste token=replaced error=clipboard_source_changed"));
    EXPECT_TRUE(logged("clipboard role=host event=finish op=paste"));
}

TEST_F(Liveness, UnansweredHostDemandIsReleasedAndFreesTheSlot) {
    build(); offer_text_to_host();
    drop_to_controller = true;
    hp->events.push_back({ClipboardDemandEventKind::kDemand, "paste", hp->remote});
    steps(3); EXPECT_EQ(hp->cancelled, 0);
    step(16000);
    EXPECT_GE(hp->cancelled, 1);
    EXPECT_TRUE(logged("clipboard role=host event=reply_timeout op=paste"));
    EXPECT_TRUE(logged("error=clipboard_peer_timeout"));
    drop_to_controller = false; log.clear();
    hp->events.push_back({ClipboardDemandEventKind::kDemand, "paste-2", hp->remote});
    steps(4);
    EXPECT_TRUE(logged("clipboard role=controller event=demand_received op=paste-2"));
    EXPECT_FALSE(logged("workspace_transfer_busy"));
}

TEST_F(Liveness, ShortcutOnADeadPortIsVisibleAndThePortRestarts) {
    build(); offer_text_to_host();
    cp->events.push_back({ClipboardDemandEventKind::kUnavailable, {}, {}, false, 0, "listener_failed"});
    step();
    EXPECT_TRUE(logged("event=port_unavailable reason=listener_failed"));
    shortcut("shortcut"); step();
    ASSERT_TRUE(finished("shortcut"));
    EXPECT_EQ(*finished("shortcut"), "clipboard_native_unavailable");
    step(1500);
    EXPECT_EQ(cp->restarts, 1); EXPECT_TRUE(logged("event=port_restarted"));
    cp->events.push_back({ClipboardDemandEventKind::kOffer, {}, cp->local});
    shortcut("shortcut-2"); steps(6);
    EXPECT_EQ(hp->installed, 1); EXPECT_EQ(backend.injected_events().size(), 4U);
}

TEST_F(Liveness, PortRestartBacksOffWhileTheOldOwnerIsStillExiting) {
    build();
    cp->restartable = false;
    cp->events.push_back({ClipboardDemandEventKind::kUnavailable, {}, {}, false, 0, "port_stopped"});
    step(); step(1500);
    EXPECT_TRUE(logged("event=port_restart_deferred retry_ms=2000"));
    cp->restartable = true; step(1000); EXPECT_EQ(cp->restarts, 0);
    step(1100); EXPECT_EQ(cp->restarts, 1);
}

TEST_F(Liveness, NativeCaptureThatNeverAnswersTimesOutVisibly) {
    build(); offer_text_to_host();
    shortcut("shortcut", 7); steps(3);
    EXPECT_FALSE(finished("shortcut"));
    step(6000);
    ASSERT_TRUE(finished("shortcut"));
    EXPECT_EQ(*finished("shortcut"), "clipboard_shortcut_timeout");
}

TEST_F(Liveness, SecondShortcutWhileOneIsWaitingIsReportedBusy) {
    build(); offer_text_to_host();
    shortcut("first", 7); step();
    shortcut("second"); step();
    ASSERT_TRUE(finished("second"));
    EXPECT_EQ(*finished("second"), "workspace_transfer_busy");
}

TEST_F(Liveness, HostCancelsAnInvokeForAnOfferItDoesNotHoldAndTheControllerReportsIt) {
    // The Host holds a different offer than the one the Controller invokes.
    tamper_to_host = [](Control& value) {
        if (value.workspace && value.workspace->action == Action::kClipboardOffer) value.workspace->clipboard_token = "stale-offer";
    };
    build(); offer_text_to_host();
    shortcut("shortcut"); steps(6);
    EXPECT_TRUE(backend.injected_events().empty());
    EXPECT_TRUE(logged("clipboard role=host event=reject op=shortcut token=controll error=clipboard_source_changed"));
    ASSERT_TRUE(finished("shortcut"));
    EXPECT_EQ(*finished("shortcut"), "clipboard_source_changed");
}

TEST_F(Liveness, UnansweredControllerInvokeIsReleased) {
    build(); offer_text_to_host();
    drop_to_host = true;
    shortcut("shortcut"); steps(3);
    EXPECT_FALSE(finished("shortcut"));
    step(16000);
    ASSERT_TRUE(finished("shortcut"));
    EXPECT_EQ(*finished("shortcut"), "clipboard_peer_timeout");
}
}
