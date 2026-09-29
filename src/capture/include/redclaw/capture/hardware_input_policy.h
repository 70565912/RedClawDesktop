#pragma once
#include <cstdint>
#include <optional>

namespace redclaw::capture {
enum class CaptureFrameDelivery { kCpu, kGpu, kCpuAndGpu };

// Encoder-thread owned. A resize may retry initialization after success, but a
// failed device generation stays on CPU until capture reports a new generation.
class HardwareInputPolicy {
public:
    void configure(bool enabled) { enabled_ = enabled; confirmed_ = false; }
    bool observe_generation(std::uint64_t generation) {
        if (!generation_ || *generation_ != generation) {
            generation_ = generation;
            failed_ = confirmed_ = false;
            return true;
        }
        return false;
    }
    void begin_attempt() { ++attempts_; }
    void confirm_output() { if (!blocked()) confirmed_ = true; }
    void fail_generation() { if (!failed_) ++fallbacks_; failed_ = true; confirmed_ = false; }
    void invalidate_context() { confirmed_ = false; }
    bool blocked() const { return !enabled_ || failed_; }
    bool confirmed() const { return confirmed_; }
    std::uint64_t attempts() const { return attempts_; }
    std::uint64_t fallbacks() const { return fallbacks_; }
    CaptureFrameDelivery delivery() const {
        if (blocked()) return CaptureFrameDelivery::kCpu;
        return confirmed_ ? CaptureFrameDelivery::kGpu : CaptureFrameDelivery::kCpuAndGpu;
    }
private:
    std::optional<std::uint64_t> generation_;
    bool enabled_ = false, failed_ = false, confirmed_ = false;
    std::uint64_t attempts_ = 0, fallbacks_ = 0;
};
}
