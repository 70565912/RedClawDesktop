#pragma once
#include "redclaw/capture/navigation_thumbnail.h"
#include <functional>
#include <span>

namespace redclaw::runtime {
struct NavigationThumbnailStats {
    std::uint64_t submitted = 0, replaced = 0, sent = 0, failed = 0, discarded = 0;
    std::uint64_t prepare_us = 0, encode_us = 0, send_us = 0;
    std::uint64_t gpu_readbacks = 0, gpu_readback_bytes = 0;
    std::size_t pending_bytes = 0;
    bool active = false;
};

// One active job and one replaceable pending image. No full desktop frame lease
// crosses into this worker. The sender must remain bound to its channel instance.
class NavigationThumbnailWorker {
public:
    using Sender = std::function<bool(std::span<const std::uint8_t>)>;
    using Encoder = std::function<bool(const capture::NavigationThumbnailImage&,
                                      std::vector<std::uint8_t>*, std::string*)>;
    explicit NavigationThumbnailWorker(bool enabled, Encoder encoder = {});
    ~NavigationThumbnailWorker();
    NavigationThumbnailWorker(const NavigationThumbnailWorker&) = delete;
    NavigationThumbnailWorker& operator=(const NavigationThumbnailWorker&) = delete;
    void set_sender(Sender sender);
    void invalidate();
    [[nodiscard]] bool wants_frame(std::uint64_t now_ms) const;
    void submit(const capture::CapturedFrame& frame, const std::string& display_id,
                std::uint64_t catalog_revision, std::uint64_t now_ms);
    [[nodiscard]] NavigationThumbnailStats stats() const;
    void stop();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace redclaw::runtime
