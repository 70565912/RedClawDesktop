#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace redclaw::runtime {
// Notification only: the existing owner retains and bounds all control data.
class RuntimeLoopWake final {
public:
    void notify() {
        { std::lock_guard lock(mutex_); pending_ = true; }
        changed_.notify_one();
    }
    void wait_for(std::chrono::milliseconds idle_interval) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, idle_interval, [this] { return pending_; });
        pending_ = false;
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool pending_ = false;
};
}
