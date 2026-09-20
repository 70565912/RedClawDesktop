#pragma once
#include <algorithm>
#include <cstdint>

namespace redclaw::session {

// Monotonic rational cadence. Low precision waits may be late but do not move
// every subsequent deadline. A long suspension skips history, never replays it.
class FrameCadence final {
public:
    [[nodiscard]] std::uint64_t due_ms() const { return (next_us_ + 999) / 1000; }
    void advance(std::uint64_t now_ms, std::uint32_t fps) {
        fps = std::clamp(fps, 1U, 1000000U);
        const auto now_us = now_ms * 1000;
        const auto period = std::max<std::uint64_t>(1, 1000000ULL / fps);
        if (fps_ != fps || next_us_ == 0 || now_us > next_us_ + 2 * period) {
            next_us_ = now_us;
            remainder_ = 0;
            fps_ = fps;
        }
        do {
            const auto numerator = 1000000ULL + remainder_;
            next_us_ += numerator / fps;
            remainder_ = numerator % fps;
        } while (next_us_ <= now_us);
    }
    void reset() { *this = {}; }
private:
    std::uint64_t next_us_ = 0;
    std::uint64_t remainder_ = 0;
    std::uint32_t fps_ = 0;
};
}  // namespace redclaw::session
