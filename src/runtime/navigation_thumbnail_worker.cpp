#include "runtime/navigation_thumbnail_worker.h"
#include "redclaw/net/video_frame_transport.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace redclaw::runtime {
namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t elapsed_us(Clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}
}
class NavigationThumbnailWorker::Impl {
public:
    struct Job {
        capture::NavigationThumbnailImage image;
        std::string display_id;
        std::uint64_t generation = 0, catalog_revision = 0, revision = 0;
        Sender sender;
    };
    mutable std::mutex mutex;
    std::condition_variable ready;
    bool running;
    std::uint64_t generation = 1, revision = 0;
    std::optional<std::uint64_t> last_prepare_ms;
    std::optional<Job> pending;
    Sender sender;
    NavigationThumbnailStats counters;
    std::thread worker;

    Impl(bool enabled, Encoder encode) : running(enabled) {
        if (enabled) worker = std::thread([this, encode = std::move(encode)] { run(encode); });
    }
    bool due(std::uint64_t now_ms) const {
        return running && sender && (!last_prepare_ms || now_ms >= *last_prepare_ms + 1000);
    }
    void invalidate_locked() {
        ++generation;
        last_prepare_ms.reset();
        if (pending) { ++counters.discarded; pending.reset(); }
        counters.pending_bytes = 0;
    }
    bool current(const Job& job) const {
        std::lock_guard lock(mutex);
        return running && generation == job.generation;
    }
    void run(const Encoder& custom_encode) {
        capture::NavigationThumbnailEncoder encoder;
        std::vector<std::uint8_t> jpeg, packet;
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return !running || pending.has_value(); });
                if (!running) return;
                job = std::move(*pending);
                pending.reset();
                counters.pending_bytes = 0;
                counters.active = true;
            }
            std::string error;
            auto started = Clock::now();
            bool ok = false, sent = false;
            std::uint64_t encoding_us = 0, sending_us = 0;
            try {
                if (current(job)) {
                    ok = custom_encode ? custom_encode(job.image, &jpeg, &error) : encoder.encode(job.image, &jpeg, &error);
                    if (ok) {
                        net::NavigationThumbnailView view;
                        view.catalog_revision = job.catalog_revision;
                        view.thumbnail_revision = job.revision;
                        view.width = job.image.width;
                        view.height = job.image.height;
                        view.display_id = job.display_id;
                        view.jpeg = jpeg;
                        ok = net::serialize_navigation_thumbnail(view, &packet, &error);
                    }
                    encoding_us = elapsed_us(started);
                    if (ok && current(job)) {
                        started = Clock::now();
                        // Never hold the queue mutex across transport calls.
                        sent = job.sender(packet);
                        sending_us = elapsed_us(started);
                    }
                }
            } catch (...) { ok = false; }
            std::lock_guard lock(mutex);
            counters.active = false;
            counters.encode_us += encoding_us;
            counters.send_us += sending_us;
            if (generation != job.generation || !running) ++counters.discarded;
            else if (ok && sent) ++counters.sent;
            else ++counters.failed;
        }
    }
};
NavigationThumbnailWorker::NavigationThumbnailWorker(bool enabled, Encoder encoder)
    : impl_(std::make_unique<Impl>(enabled, std::move(encoder))) {}
NavigationThumbnailWorker::~NavigationThumbnailWorker() { stop(); }
void NavigationThumbnailWorker::set_sender(Sender sender) {
    std::lock_guard lock(impl_->mutex);
    impl_->invalidate_locked();
    impl_->sender = std::move(sender);
}
void NavigationThumbnailWorker::invalidate() {
    std::lock_guard lock(impl_->mutex);
    impl_->invalidate_locked();
}
bool NavigationThumbnailWorker::wants_frame(std::uint64_t now_ms) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->due(now_ms);
}
void NavigationThumbnailWorker::submit(const capture::CapturedFrame& frame, const std::string& display_id,
                                        std::uint64_t catalog_revision, std::uint64_t now_ms) {
    Impl::Job job;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->due(now_ms)) return;
        impl_->last_prepare_ms = now_ms;
        job.generation = impl_->generation;
        job.revision = ++impl_->revision;
        job.sender = impl_->sender;
    }
    const auto started = Clock::now();
    job.display_id = display_id;
    job.catalog_revision = catalog_revision;
    bool prepared = false;
    try { prepared = capture::prepare_navigation_thumbnail(frame, 320, &job.image); }
    catch (...) { prepared = false; }
    const auto prepare_us = elapsed_us(started);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->counters.prepare_us += prepare_us;
        if (!impl_->running || impl_->generation != job.generation) { ++impl_->counters.discarded; return; }
        if (!prepared) { ++impl_->counters.failed; return; }
        if (impl_->pending) ++impl_->counters.replaced;
        ++impl_->counters.submitted;
        impl_->counters.pending_bytes = job.image.bgra.size();
        impl_->pending = std::move(job);
    }
    impl_->ready.notify_one();
}
NavigationThumbnailStats NavigationThumbnailWorker::stats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->counters;
}
void NavigationThumbnailWorker::stop() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->running = false;
        impl_->invalidate_locked();
        impl_->sender = {};
    }
    impl_->ready.notify_one();
    if (impl_->worker.joinable()) impl_->worker.join();
}
} // namespace redclaw::runtime
