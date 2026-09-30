#include "media_pacer_wait.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace redclaw::net {

struct MediaPacerWait::NativeState {
#ifdef _WIN32
    HANDLE notification = nullptr;
    HANDLE timer = nullptr;
    ~NativeState() {
        if (timer) CloseHandle(timer);
        if (notification) CloseHandle(notification);
    }
#endif
};

MediaPacerWait::MediaPacerWait(Mode mode) {
#ifdef _WIN32
    if (mode == Mode::kAutomatic) {
        auto native = std::make_unique<NativeState>();
        native->notification = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        native->timer = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (native->notification && native->timer) {
            native_ = std::move(native);
            high_resolution_ = true;
        }
    }
#else
    (void)mode;
#endif
}

MediaPacerWait::~MediaPacerWait() = default;

void MediaPacerWait::notify() {
#ifdef _WIN32
    // Handles stay immutable/alive until the worker has joined, including after
    // a timer failure. No notifier races a CloseHandle or backend switch.
    if (native_) SetEvent(native_->notification);
#endif
    fallback_.notify_all();
}

void MediaPacerWait::wait_until(std::unique_lock<std::mutex>& lock,
                              Clock::time_point deadline) {
#ifdef _WIN32
    if (high_resolution_) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) return;
        LARGE_INTEGER due;
        using TimerTicks = std::chrono::duration<LONGLONG, std::ratio<1, 10000000>>;
        due.QuadPart = -std::chrono::ceil<TimerTicks>(remaining).count();
        // Reset/arm under the predicate mutex, then unlock before blocking. A
        // producer cannot change the predicate in the reset-to-wait interval
        // without also leaving the manual-reset event signalled.
        if (ResetEvent(native_->notification)
            && SetWaitableTimerEx(native_->timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
            const HANDLE handles[]{native_->notification, native_->timer};
            lock.unlock();
            const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            lock.lock();
            CancelWaitableTimer(native_->timer);
            if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1) return;
        }
        // Latch failure; the caller rechecks its predicate/deadline before the
        // next fallback wait. Do not retry a broken timer on every fragment.
        high_resolution_ = false;
        return;
    }
#endif
    fallback_.wait_until(lock, deadline);
}

} // namespace redclaw::net
