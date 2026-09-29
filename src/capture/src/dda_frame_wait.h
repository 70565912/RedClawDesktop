#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi.h>
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace redclaw::capture {
struct DdaFrameWaitResult {
    HRESULT status = DXGI_ERROR_WAIT_TIMEOUT;
    std::uint32_t polls = 0, waits = 0;
};

// AcquireNextFrame can retain the device's internal multithread lock while
// waiting. Poll without a driver wait; back off outside every device/context lock.
// Keep one deadline for the caller's whole budget, including OS wake lateness.
template<class Acquire, class Now, class WaitUntil>
DdaFrameWaitResult wait_for_dda_frame(std::uint32_t timeout_ms,
    Acquire acquire, Now now, WaitUntil wait_until) {
    const auto deadline = now() + std::chrono::milliseconds(timeout_ms);
    DdaFrameWaitResult result;
    for (;;) {
        ++result.polls;
        result.status = acquire(0U);
        if (result.status != DXGI_ERROR_WAIT_TIMEOUT) return result;
        const auto current = now();
        if (current >= deadline) return result;
        ++result.waits;
        wait_until((std::min)(deadline, current + std::chrono::milliseconds(8)));
        if (now() >= deadline) return result;
    }
}
}
#endif
