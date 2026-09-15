#include <gtest/gtest.h>
#include "redclaw/workspace/terminal_controller.h"
#include <deque>
#include <algorithm>

namespace {
using namespace redclaw::protocol;
TEST(TerminalController, CloseBeforeReadyWaitsForCleanupAndNewDesktopUsesNewClientIdentity) {
    std::vector<TerminalMessageV1> sent;
    redclaw::workspace::TerminalController controller([&](const auto& message) {
        EXPECT_FALSE(serialize_terminal_message_v1(message).empty()); sent.push_back(message); return true;
    }, {});
    controller.surface_ready(true); controller.request_open();
    TerminalMessageV1 available; available.type = TerminalMessageTypeV1::kAvailability;
    available.session_epoch = "host"; available.input_enabled = true;
    ASSERT_TRUE(controller.receive(available)); ASSERT_EQ(sent.size(), 1U);
    const auto client = sent[0].client_session_id; EXPECT_FALSE(client.empty());
    controller.request_end();
    ASSERT_EQ(sent.size(), 2U); EXPECT_EQ(sent.back().type, TerminalMessageTypeV1::kEnd);
    EXPECT_TRUE(sent.back().terminal_id.empty()); EXPECT_FALSE(controller.end_complete());
    EXPECT_FALSE(controller.input("must_not_execute"));
    auto ended = sent.back(); ended.type = TerminalMessageTypeV1::kEnded; ended.terminal_id = "started-before-ready";
    ended.client_session_id = "other-client"; EXPECT_FALSE(controller.receive(ended));
    ended.client_session_id = client; ASSERT_TRUE(controller.receive(ended));
    EXPECT_TRUE(controller.end_complete()); EXPECT_TRUE(controller.end_acknowledged());
    controller.request_open();
    EXPECT_EQ(sent.back().type, TerminalMessageTypeV1::kOpen);
    EXPECT_NE(sent.back().client_session_id, client);
}
TEST(TerminalController, InputIsBoundedAcknowledgedAndNeverReplayedAcrossGenerations) {
    std::deque<TerminalMessageV1> sent;
    std::string displayed;
    unsigned reset_count = 0;
    redclaw::workspace::TerminalController controller([&](const auto& message) {
        const auto parsed = parse_terminal_message_v1(serialize_terminal_message_v1(message));
        EXPECT_TRUE(parsed.ok) << parsed.error;
        if (!parsed.ok) return false;
        sent.push_back(parsed.value); return true;
    }, {[&](auto) { ++reset_count; }, [&](auto bytes) { displayed += bytes; return true; }, {}});
    controller.surface_ready(true);
    controller.request_open();
    EXPECT_TRUE(sent.empty());
    TerminalMessageV1 available;
    available.type = TerminalMessageTypeV1::kAvailability; available.session_epoch = "host-1";
    available.input_enabled = true;
    ASSERT_TRUE(controller.receive(available));
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent.front().type, TerminalMessageTypeV1::kOpen);
    sent.clear();
    auto state = available;
    state.type = TerminalMessageTypeV1::kReady; state.terminal_id = "shell-1";
    state.input_generation = 1;
    ASSERT_TRUE(controller.receive(state));
    EXPECT_EQ(reset_count, 1U);
    EXPECT_FALSE(controller.input_enabled()); // wait for the surface reset ACK
    controller.surface_ready(true);
    sent.clear();
    ASSERT_TRUE(controller.input(std::string(40 * 1024, 'x')));
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent.front().bytes.size(), kMaxTerminalChunkBytes);
    EXPECT_EQ(sent.front().sequence, 1U);
    controller.pump();
    EXPECT_EQ(sent.size(), 1U); // host has not acknowledged the first chunk
    EXPECT_FALSE(controller.input(std::string(64 * 1024, 'y')));
    state.type = TerminalMessageTypeV1::kState; state.input_sequence = 1;
    ASSERT_TRUE(controller.receive(state));
    EXPECT_EQ(sent[1].sequence, 2U);
    EXPECT_EQ(controller.pending_input_bytes(), 8U * 1024U);
    state.input_enabled = false;
    ASSERT_TRUE(controller.receive(state));
    EXPECT_EQ(controller.pending_input_bytes(), 0U);
    EXPECT_FALSE(controller.input("should not execute\r"));
    state.input_enabled = true; state.input_generation = 2; state.input_sequence = 1;
    sent.clear();
    ASSERT_TRUE(controller.receive(state));
    ASSERT_TRUE(controller.input("new input"));
    const auto input = std::find_if(sent.begin(), sent.end(), [](const auto& m) { return m.type == TerminalMessageTypeV1::kInput; });
    ASSERT_NE(input, sent.end());
    EXPECT_EQ(input->bytes, "new input"); EXPECT_EQ(input->sequence, 2U); EXPECT_EQ(input->input_generation, 2U);
    controller.disconnected();
    EXPECT_FALSE(controller.input("disconnected"));
    available.session_epoch = "host-2";
    sent.clear();
    ASSERT_TRUE(controller.receive(available));
    ASSERT_EQ(sent.size(), 1U); EXPECT_EQ(sent.front().type, TerminalMessageTypeV1::kOpen);
    state.type = TerminalMessageTypeV1::kReady; state.session_epoch = "host-2"; state.input_generation = 3;
    ASSERT_TRUE(controller.receive(state));
    EXPECT_EQ(reset_count, 1U); // same shell survives a transport epoch
    EXPECT_EQ(controller.pending_input_bytes(), 0U);

    auto output = state;
    output.type = TerminalMessageTypeV1::kOutput; output.bytes = "visible"; output.sequence = 1;
    sent.clear();
    ASSERT_TRUE(controller.receive(output));
    EXPECT_EQ(displayed, "visible");
    EXPECT_TRUE(sent.empty()); // receiving bytes is not a presentation ACK
    controller.output_parsed();
    ASSERT_EQ(sent.size(), 1U); EXPECT_EQ(sent.back().type, TerminalMessageTypeV1::kOutputAck);
    ASSERT_TRUE(controller.receive(output));
    EXPECT_EQ(displayed, "visible"); EXPECT_EQ(sent.size(), 2U); // duplicate re-ACK, no duplicate output
    output.session_epoch = "host-1";
    EXPECT_FALSE(controller.receive(output));
    state.terminal_id = "new-shell"; state.sequence = 0;
    ASSERT_TRUE(controller.receive(state));
    EXPECT_EQ(reset_count, 2U);
    EXPECT_FALSE(controller.input_enabled());
}

TEST(TerminalController, LocalTransferGatePreservesOutputAndComposesWithHostPause) {
    std::vector<TerminalMessageV1> sent;
    std::string displayed;
    unsigned resets = 0;
    redclaw::workspace::TerminalController controller([&](const auto& message) {
        sent.push_back(message); return true;
    }, {[&](auto) { ++resets; }, [&](auto bytes) { displayed += bytes; return true; }, {}});
    controller.surface_ready(true); controller.request_open();
    TerminalMessageV1 available;
    available.type = TerminalMessageTypeV1::kAvailability; available.session_epoch = "host"; available.input_enabled = true;
    ASSERT_TRUE(controller.receive(available));
    auto state = available; state.type = TerminalMessageTypeV1::kReady; state.terminal_id = "shell"; state.input_generation = 1;
    ASSERT_TRUE(controller.receive(state)); controller.surface_ready(true); sent.clear();
    ASSERT_TRUE(controller.input(std::string(20 * 1024, 'x')));
    EXPECT_GT(controller.pending_input_bytes(), 0U);
    available.local_input_paused = true;
    ASSERT_TRUE(controller.receive(available));
    EXPECT_EQ(controller.pending_input_bytes(), 0U);
    EXPECT_FALSE(controller.input("must not execute\r"));
    sent.clear();
    auto output = state; output.type = TerminalMessageTypeV1::kOutput; output.sequence = 1; output.bytes = "background output";
    ASSERT_TRUE(controller.receive(output)); controller.output_parsed();
    EXPECT_EQ(displayed, "background output");
    ASSERT_EQ(sent.size(), 1U); EXPECT_EQ(sent.front().type, TerminalMessageTypeV1::kOutputAck);
    state.type = TerminalMessageTypeV1::kState; state.input_generation = 2;
    ASSERT_TRUE(controller.receive(state)); // a Host-ready message cannot clear the local transfer gate
    EXPECT_FALSE(controller.input_enabled());
    state.input_enabled = false;
    ASSERT_TRUE(controller.receive(state));
    available.local_input_paused = false;
    ASSERT_TRUE(controller.receive(available)); // completing transfer cannot clear Host's independent pause
    EXPECT_FALSE(controller.input_enabled());
    state.input_enabled = true; state.input_generation = 3;
    ASSERT_TRUE(controller.receive(state));
    EXPECT_TRUE(controller.input_enabled()); EXPECT_EQ(resets, 1U);
    EXPECT_EQ(controller.pending_input_bytes(), 0U);
}
}
