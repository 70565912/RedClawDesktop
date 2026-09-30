#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace redclaw::net {

// One timed waiter, owned by the pacer and destroyed only after its worker joins.
// Predicate state and wait_until() use the same external mutex. Producers mutate
// that state before notify(); notifications alone do not grant sending credit.
class MediaPacerWait final {
public:
    using Clock = std::chrono::steady_clock;
    enum class Mode { kAutomatic, kConditionVariable };

    explicit MediaPacerWait(Mode mode = Mode::kAutomatic);
    ~MediaPacerWait();
    MediaPacerWait(const MediaPacerWait&) = delete;
    MediaPacerWait& operator=(const MediaPacerWait&) = delete;

    void notify();
    void wait_until(std::unique_lock<std::mutex>& lock, Clock::time_point deadline);
    // Query with the external mutex held (or before starting the worker).
    [[nodiscard]] bool high_resolution() const { return high_resolution_; }

    template<class Predicate>
    bool wait_for(std::unique_lock<std::mutex>& lock,
                  std::chrono::microseconds duration, Predicate ready) {
        const auto deadline = Clock::now() + duration;
        while (!ready()) {
            if (Clock::now() >= deadline) return false;
            wait_until(lock, deadline);
        }
        return true;
    }

private:
    struct NativeState;
    std::unique_ptr<NativeState> native_;
    std::condition_variable fallback_;
    bool high_resolution_ = false;
};

} // namespace redclaw::net
