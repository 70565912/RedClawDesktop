#include "redclaw/diag/operation_timing.h"
#include <chrono>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace redclaw::diag {
namespace { thread_local ThreadTimingObserver thread_observer; }
ThreadTimingObserver set_thread_timing_observer(ThreadTimingObserver observer) {
    const auto previous = thread_observer;
    thread_observer = observer;
    return previous;
}
std::uint64_t monotonic_time_us() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::uint64_t current_thread_cpu_us() {
#if defined(_WIN32)
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
        const auto value = [](FILETIME t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        return (value(kernel) + value(user)) / 10;
    }
#endif
    return 0;
}
DiagnosticTimingScope::DiagnosticTimingScope(DiagnosticOperation operation)
    : observer_(thread_observer), operation_(operation) {
    if (observer_.observe) { begin_us_ = monotonic_time_us(); cpu_us_ = current_thread_cpu_us(); }
}
void DiagnosticTimingScope::finish() {
    if (!observer_.observe) return;
    const auto observer = observer_;
    observer_.observe = nullptr;
    const auto end = monotonic_time_us();
    const auto cpu = current_thread_cpu_us();
    observer.observe(observer.context, operation_, {begin_us_, end, cpu >= cpu_us_ ? cpu - cpu_us_ : 0});
}
}
