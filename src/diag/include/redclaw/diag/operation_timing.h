#pragma once
#include <cstdint>

namespace redclaw::diag {
enum class DiagnosticOperation { kLogLock, kOutputWrite, kFileWrite, kFlush, kDhtListenPort };
struct OperationTimingSample {
    std::uint64_t begin_us = 0, end_us = 0, cpu_us = 0;
};
using OperationTimingObserver = void (*)(void*, DiagnosticOperation, const OperationTimingSample&);
struct ThreadTimingObserver { void* context = nullptr; OperationTimingObserver observe = nullptr; };
// Observer belongs to the calling thread; no process-wide callback or lock.
ThreadTimingObserver set_thread_timing_observer(ThreadTimingObserver observer);
std::uint64_t monotonic_time_us();
std::uint64_t current_thread_cpu_us();

class DiagnosticTimingScope {
public:
    explicit DiagnosticTimingScope(DiagnosticOperation operation);
    ~DiagnosticTimingScope() { finish(); }
    void finish();
private:
    ThreadTimingObserver observer_;
    DiagnosticOperation operation_;
    std::uint64_t begin_us_ = 0, cpu_us_ = 0;
};
}
