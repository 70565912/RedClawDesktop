#include "runtime/runtime_loop_wake.h"
#include <gtest/gtest.h>
#include <future>

using namespace std::chrono_literals;
using redclaw::runtime::RuntimeLoopWake;

TEST(RuntimeLoopWake, PendingSignalsCoalesceWithoutLosingTheNextIdleWait) {
    RuntimeLoopWake wake;
    for (unsigned i = 0; i < 100; ++i) wake.notify();
    const auto start = std::chrono::steady_clock::now();
    wake.wait_for(1s);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 100ms);
    const auto next = std::chrono::steady_clock::now();
    wake.wait_for(20ms);
    EXPECT_GE(std::chrono::steady_clock::now() - next, 15ms);
}

TEST(RuntimeLoopWake, InputOrDisconnectNotificationInterruptsIdleWait) {
    RuntimeLoopWake wake;
    auto waited = std::async(std::launch::async, [&] { wake.wait_for(1s); });
    ASSERT_EQ(waited.wait_for(20ms), std::future_status::timeout);
    wake.notify();
    EXPECT_EQ(waited.wait_for(100ms), std::future_status::ready);
    waited.get();
}
