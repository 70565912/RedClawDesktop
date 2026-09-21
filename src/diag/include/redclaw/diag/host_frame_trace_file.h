#pragma once
#include "redclaw/net/media_frame_trace.h"
#include <filesystem>
#include <ostream>

namespace redclaw::diag {
void write_host_frame_trace_csv(std::ostream& output, const net::MediaFrameTraceBatch& batch);

// Called only by the periodic diagnostic thread. An exact sidecar request
// arms one window: "redclaw.host-frame-trace.v1 <seconds 1..120>".
// Consumed requests remain as receipts; no hot-path file I/O or re-arming loop.
class HostFrameTraceFile final {
public:
    HostFrameTraceFile(net::MediaFrameTraceRecorder& recorder, std::filesystem::path runtime_log);
    void poll(std::uint64_t now_us);
private:
    net::MediaFrameTraceRecorder& recorder_;
    std::filesystem::path runtime_log_;
    bool recording_ = false;
};
} // namespace redclaw::diag
