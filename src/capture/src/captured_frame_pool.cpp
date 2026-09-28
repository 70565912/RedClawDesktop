#include "redclaw/capture/captured_frame_pool.h"
#include <utility>

namespace redclaw::capture {

CapturedFramePool::CapturedFramePool() {
    for (auto& slot : slots_) slot = std::make_shared<CapturedFrame>();
}

std::shared_ptr<CapturedFrame> CapturedFramePool::acquire() {
    for (auto& slot : slots_) {
        if (slot.use_count() != 1) continue;
        // Preserve both size and capacity: resize on an unchanged desktop must
        // neither allocate nor zero the entire image before capture overwrites it.
        auto pixels = std::move(slot->data);
        *slot = CapturedFrame{};
        slot->data = std::move(pixels);
        ++stats_.acquisitions;
        return slot;
    }
    ++stats_.exhausted;
    return {};
}

void CapturedFramePool::release_unused() {
    for (auto& slot : slots_) {
        if (slot.use_count() == 1) *slot = CapturedFrame{};
    }
}

CapturedFramePoolStats CapturedFramePool::stats() const {
    auto result = stats_;
    for (const auto& slot : slots_) result.retained_cpu_bytes += slot->data.capacity();
    return result;
}

} // namespace redclaw::capture
