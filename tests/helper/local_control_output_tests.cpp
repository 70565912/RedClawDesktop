#include "runtime/local_control_output.h"
#include "redclaw/diag/diag_module.h"
#include <gtest/gtest.h>
#include <sstream>
#include <thread>
#include <chrono>

namespace {
class SlowDiagnosticBuffer final : public std::stringbuf {
public:
    std::atomic_bool entered{false};
    std::streamsize xsputn(const char* text, std::streamsize count) override {
        entered = true; std::this_thread::sleep_for(std::chrono::milliseconds(350));
        return std::stringbuf::xsputn(text, count);
    }
};
}

TEST(LocalControlOutput, AckBypassesSlowDiagnosticOutputAndPreservesControlEnvelope) {
    std::stringbuf control_buffer;
    SlowDiagnosticBuffer diagnostic_buffer;
    redclaw::runtime::LocalControlOutput control(true, &control_buffer);
    redclaw::protocol::StreamControlMessageV1 message;
    message.type = redclaw::protocol::StreamControlMessageTypeV1::kInputControlStatus;
    message.session_epoch = "local"; message.message_id = 1; message.input_sequence = 42;
    message.sent_at_ms = 1;
    std::thread diagnostic([&] { diagnostic_buffer.sputn("ordinary diagnostic", 19); });
    while (!diagnostic_buffer.entered) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    const bool sent = control.send(message, 12345);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    diagnostic.join();
    ASSERT_TRUE(sent);
    EXPECT_LT(elapsed, std::chrono::milliseconds(250));
    std::uint64_t received = 0;
    auto wire = control_buffer.str();
    ASSERT_EQ(wire.back(), '\n'); wire.pop_back();
    const auto parsed = redclaw::protocol::parse_local_runtime_control_frame_v2(wire, &received);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.value.input_sequence, 42); EXPECT_EQ(received, 12345);
    EXPECT_EQ(control_buffer.str().find("ordinary diagnostic"), std::string::npos);
}

TEST(LocalControlOutput, OutputFailureIsLatched) {
    class FailedBuffer final : public std::streambuf {
        std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
    } failed;
    redclaw::runtime::LocalControlOutput output(true, &failed);
    redclaw::protocol::StreamControlMessageV1 message;
    message.type = redclaw::protocol::StreamControlMessageTypeV1::kInputControlStatus;
    message.session_epoch = "local"; message.message_id = 1;
    EXPECT_FALSE(output.send(message)); EXPECT_TRUE(output.failed());
}
