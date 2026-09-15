#include <gtest/gtest.h>
#include "redclaw/workspace/terminal_host.h"
#include <chrono>
#include <algorithm>
#include <deque>
#include <thread>

namespace {
using namespace redclaw::protocol;
using redclaw::workspace::TerminalHost;
using namespace std::chrono_literals;

TEST(TerminalHostIntegration, ReconnectRetainsOutputAndTransferGateNeverReplaysInput) {
#ifndef _WIN32
    GTEST_SKIP() << "ConPTY requires Windows.";
#else
    std::deque<TerminalMessageV1> outgoing;
    TerminalHost host([&](const TerminalMessageV1& message) {
        const auto decoded = parse_terminal_message_v1(serialize_terminal_message_v1(message));
        EXPECT_TRUE(decoded.ok) << decoded.error;
        if (!decoded.ok) return false;
        outgoing.push_back(decoded.value);
        return true;
    });
    host.begin_session("desktop-epoch", std::filesystem::current_path());
    host.set_connection(true, false);
    TerminalMessageV1 command;
    command.session_epoch = "desktop-epoch"; command.columns = 100; command.rows = 30;
    command.client_session_id = "client-desktop-1";
    std::string error;
    EXPECT_FALSE(host.receive(command, &error));
    EXPECT_EQ(error, "terminal_input_unavailable");
    EXPECT_FALSE(host.running());
    host.set_connection(true, true);
    ASSERT_TRUE(host.receive(command, &error)) << error;
    ASSERT_TRUE(host.running());
    const std::string identity(host.terminal_id());
    ASSERT_FALSE(identity.empty());
    ASSERT_TRUE(host.receive(command, &error));
    EXPECT_EQ(host.terminal_id(), identity);
    command.terminal_id = identity;
    command.input_generation = outgoing.back().input_generation;

    std::string output;
    std::uint64_t input_sequence = 0;
    auto consume = [&](bool acknowledge) {
        host.pump();
        std::optional<TerminalMessageV1> retained;
        while (!outgoing.empty()) {
            auto message = std::move(outgoing.front()); outgoing.pop_front();
            if (message.type == TerminalMessageTypeV1::kState || message.type == TerminalMessageTypeV1::kReady) {
                command.input_generation = message.input_generation;
            }
            if (message.type != TerminalMessageTypeV1::kOutput) continue;
            if (!acknowledge) { retained = message; continue; }
            const auto previous = output.size();
            output += message.bytes;
            auto ack = command;
            ack.type = TerminalMessageTypeV1::kOutputAck; ack.sequence = message.sequence;
            EXPECT_TRUE(host.receive(ack, &error)) << error;
            if (output.find("\x1b[6n", previous > 3 ? previous - 3 : 0) != std::string::npos) {
                auto response = command;
                response.type = TerminalMessageTypeV1::kInput; response.sequence = ++input_sequence;
                response.bytes = "\x1b[1;1R";
                EXPECT_TRUE(host.receive(response, &error)) << error;
            }
        }
        return retained;
    };
    const auto until_output = [&](std::string_view marker) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        do { consume(true); if (output.find(marker) != std::string::npos) return true;
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    ASSERT_TRUE(until_output("PS "));
    auto input = command;
    input.type = TerminalMessageTypeV1::kInput; input.sequence = ++input_sequence;
    input.bytes = "$retained=41; Write-Output ('RETAINED_'+'BEFORE='+$retained)\r";
    ASSERT_TRUE(host.receive(input, &error)) << error;
    ASSERT_TRUE(until_output("RETAINED_BEFORE=41"));

    input.sequence = ++input_sequence;
    input.bytes = "Write-Output ('OUTPUT_'+'IN_FLIGHT')\r";
    ASSERT_TRUE(host.receive(input, &error)) << error;
    std::optional<TerminalMessageV1> unacknowledged;
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!unacknowledged && std::chrono::steady_clock::now() < deadline) {
        unacknowledged = consume(false); std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(unacknowledged.has_value());
    host.set_connection(false, false);
    EXPECT_TRUE(host.running());
    EXPECT_FALSE(host.receive(input, &error));
    host.set_connection(true, true);
    EXPECT_FALSE(consume(false).has_value()); // no output before the new surface opens
    ASSERT_TRUE(host.receive(command, &error)) << error;
    const auto replay = consume(false);
    ASSERT_TRUE(replay.has_value());
    EXPECT_EQ(replay->sequence, unacknowledged->sequence);
    EXPECT_EQ(replay->bytes, unacknowledged->bytes);
    auto ack = command;
    ack.type = TerminalMessageTypeV1::kOutputAck; ack.sequence = replay->sequence + 1000;
    EXPECT_FALSE(host.receive(ack, &error));
    ack.sequence = replay->sequence;
    ASSERT_TRUE(host.receive(ack, &error)) << error;
    output += replay->bytes;
    ASSERT_TRUE(until_output("OUTPUT_IN_FLIGHT"));

    host.set_connection(true, false);
    consume(true);
    input.input_generation = command.input_generation;
    input.sequence = ++input_sequence;
    input.bytes = "$blockedWrite=1\r";
    EXPECT_FALSE(host.receive(input, &error));
    host.set_connection(true, true);
    EXPECT_FALSE(host.receive(input, &error)); // old input generation cannot replay
    EXPECT_EQ(error, "terminal_stale_input_generation");
    consume(true);
    input.input_generation = command.input_generation;
    input.sequence = ++input_sequence;
    input.bytes = "Write-Output ('BLOCKED_'+'EMPTY='+($null -eq $blockedWrite)); Write-Output ('RETAINED_'+'AFTER='+$retained)\r";
    ASSERT_TRUE(host.receive(input, &error)) << error;
    ASSERT_TRUE(until_output("BLOCKED_EMPTY=True"));
    ASSERT_TRUE(until_output("RETAINED_AFTER=41"));
    EXPECT_EQ(host.terminal_id(), identity);
    auto stale = input; stale.session_epoch = "different-epoch";
    EXPECT_FALSE(host.receive(stale, &error));
    EXPECT_LE(host.buffered_output_bytes(), redclaw::workspace::TerminalSession::kMaxBufferedOutputBytes + kMaxTerminalChunkBytes);
    auto end = command; end.type = TerminalMessageTypeV1::kEnd;
    ASSERT_TRUE(host.receive(end, &error)) << error;
    EXPECT_FALSE(host.running()); host.pump();
    EXPECT_TRUE(std::any_of(outgoing.begin(), outgoing.end(), [](const auto& message) { return message.type == TerminalMessageTypeV1::kEnded; }));
    ASSERT_TRUE(host.receive(end, &error)); // idempotent, including after cleanup
    EXPECT_FALSE(host.receive(command, &error)); // the ended desktop cannot reopen its shell
    command.terminal_id.clear(); command.client_session_id = "client-desktop-2";
    ASSERT_TRUE(host.receive(command, &error)) << error;
    EXPECT_TRUE(host.running()); EXPECT_NE(host.terminal_id(), identity);
    EXPECT_FALSE(host.receive(end, &error)); // late close cannot kill a new desktop's shell
    host.end_session();
    EXPECT_FALSE(host.running());
    EXPECT_TRUE(host.terminal_id().empty());
    EXPECT_FALSE(host.receive(input, &error));
#endif
}
}
