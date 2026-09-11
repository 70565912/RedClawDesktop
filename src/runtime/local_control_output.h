#pragma once
#include "redclaw/protocol/stream_control_protocol.h"
#include <atomic>
#include <iostream>
#include <mutex>

namespace redclaw::runtime {

// Construct before process log capture. In dedicated mode the saved stdout
// stream has only a control lock and never passes through the diagnostic tee.
class LocalControlOutput {
public:
    explicit LocalControlOutput(bool dedicated, std::streambuf* output = std::cout.rdbuf())
        : dedicated_(dedicated), output_(output) {}
    bool send(const redclaw::protocol::StreamControlMessageV1& message, std::uint64_t received_us = 0) {
        auto frame = redclaw::protocol::serialize_local_runtime_control_frame_v2(message, received_us);
        if (frame.empty() || frame.size() > redclaw::protocol::kMaxLocalRuntimeControlFrameBytes) {
            failed_ = true; return false;
        }
        frame.push_back('\n');
        std::lock_guard lock(mutex_);
        if (!dedicated_) { std::cout << frame; return std::cout.good(); }
        const bool ok = !failed_ && output_
            && output_->sputn(frame.data(), static_cast<std::streamsize>(frame.size())) == static_cast<std::streamsize>(frame.size())
            && output_->pubsync() == 0;
        if (!ok) failed_ = true;
        return ok;
    }
    bool failed() const { return failed_.load(); }
private:
    bool dedicated_;
    std::streambuf* output_;
    std::mutex mutex_;
    std::atomic_bool failed_{false};
};

}  // namespace redclaw::runtime
