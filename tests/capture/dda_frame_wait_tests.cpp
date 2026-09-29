#include <gtest/gtest.h>
#ifdef _WIN32
#include "dda_frame_wait.h"
#include <vector>
using namespace redclaw::capture;
using namespace std::chrono_literals;

TEST(DdaFrameWait, ZeroBudgetMakesOneNonblockingAttempt) {
    const auto start = std::chrono::steady_clock::time_point{};
    const auto result = wait_for_dda_frame(0, [](std::uint32_t timeout) {
        EXPECT_EQ(timeout, 0U); return DXGI_ERROR_WAIT_TIMEOUT;
    }, [&] { return start; }, [](auto) { ADD_FAILURE() << "Zero budget must not sleep"; });
    EXPECT_EQ(result.status, DXGI_ERROR_WAIT_TIMEOUT);
    EXPECT_EQ(result.polls, 1U); EXPECT_EQ(result.waits, 0U);
}

TEST(DdaFrameWait, OneBudgetLimitsEmptyPollsAndDoesNotRestartAtEachWake) {
    auto clock = std::chrono::steady_clock::time_point{};
    std::vector<decltype(clock)> wakes;
    const auto result = wait_for_dda_frame(50, [](std::uint32_t timeout) {
        EXPECT_EQ(timeout, 0U); return DXGI_ERROR_WAIT_TIMEOUT;
    }, [&] { return clock; }, [&](auto deadline) { wakes.push_back(deadline); clock = deadline; });
    EXPECT_EQ(result.status, DXGI_ERROR_WAIT_TIMEOUT);
    EXPECT_EQ(result.polls, 7U); EXPECT_EQ(result.waits, 7U);
    EXPECT_EQ(clock.time_since_epoch(), 50ms);
    EXPECT_EQ(wakes.front().time_since_epoch(), 8ms);
    EXPECT_EQ(wakes.back().time_since_epoch(), 50ms);
}

TEST(DdaFrameWait, SuccessAndDeviceLossReturnImmediately) {
    for (HRESULT final_result : {S_OK, DXGI_ERROR_ACCESS_LOST}) {
        auto clock = std::chrono::steady_clock::time_point{};
        unsigned polls = 0;
        const auto result = wait_for_dda_frame(50, [&](std::uint32_t timeout) {
            EXPECT_EQ(timeout, 0U); return ++polls == 2 ? final_result : DXGI_ERROR_WAIT_TIMEOUT;
        }, [&] { return clock; }, [&](auto deadline) { clock = deadline; });
        EXPECT_EQ(result.status, final_result);
        EXPECT_EQ(result.polls, 2U); EXPECT_EQ(result.waits, 1U);
        EXPECT_EQ(clock.time_since_epoch(), 8ms);
    }
}

TEST(DdaFrameWait, LateWakeAndSlowDriverDoNotExtendBudget) {
    auto clock = std::chrono::steady_clock::time_point{};
    const auto result = wait_for_dda_frame(50, [&](std::uint32_t) {
        clock += 45ms; return DXGI_ERROR_WAIT_TIMEOUT;
    }, [&] { return clock; }, [&](auto deadline) {
        EXPECT_EQ(deadline.time_since_epoch(), 50ms); clock = deadline + 20ms;
    });
    EXPECT_EQ(result.polls, 1U); EXPECT_EQ(result.waits, 1U);
    EXPECT_EQ(result.status, DXGI_ERROR_WAIT_TIMEOUT);
}
#endif
