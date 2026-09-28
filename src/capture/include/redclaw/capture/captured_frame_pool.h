#pragma once

#include "redclaw/capture/capture_module.h"
#include <array>

namespace redclaw::capture {

struct CapturedFramePoolStats {
    std::uint64_t acquisitions = 0;
    std::uint64_t exhausted = 0;
    std::size_t retained_cpu_bytes = 0;
};

// One capture-thread owner; consumers retain strong references and read only.
// Do not create weak references: only acquire() may make an idle slot live.
// The three slots cover capture, latest pending and the encoder's current frame.
class CapturedFramePool {
public:
    static constexpr std::size_t kCapacity = 3;
    CapturedFramePool();
    CapturedFramePool(const CapturedFramePool&) = delete;
    CapturedFramePool& operator=(const CapturedFramePool&) = delete;
    std::shared_ptr<CapturedFrame> acquire();
    void release_unused();
    CapturedFramePoolStats stats() const;

private:
    std::array<std::shared_ptr<CapturedFrame>, kCapacity> slots_;
    CapturedFramePoolStats stats_;
};

} // namespace redclaw::capture
