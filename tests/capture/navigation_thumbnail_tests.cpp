#include "runtime/navigation_thumbnail_worker.h"
#include "redclaw/net/video_frame_transport.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace {
using namespace redclaw;
using namespace std::chrono_literals;
capture::CapturedFrame pixels(std::uint32_t width = 640, std::uint32_t height = 360) {
    capture::CapturedFrame f;
    f.width = width; f.height = height; f.row_pitch = width * 4 + 16; f.bgra = true;
    f.data.resize(static_cast<std::size_t>(f.row_pitch) * height, 0x77);
    return f;
}
template<class Predicate> bool wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}
struct SlowEncoder {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    bool encode(const capture::NavigationThumbnailImage&, std::vector<std::uint8_t>* jpeg, std::string*) {
        std::unique_lock lock(mutex);
        entered = true; changed.notify_all();
        changed.wait_for(lock, 3s, [&] { return released; });
        *jpeg = {0xff, 0xd8, 0xff, 0xd9};
        return true;
    }
    bool await_entry() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 3s, [&] { return entered; });
    }
    void release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
};
TEST(NavigationThumbnail, BoundedPreparationPreservesAspectPaddingAndSource) {
    auto frame = pixels(1080, 1920);
    const auto original = frame.data;
    capture::NavigationThumbnailImage image;
    ASSERT_TRUE(capture::prepare_navigation_thumbnail(frame, 320, &image));
    EXPECT_EQ(image.width, 180U); EXPECT_EQ(image.height, 320U);
    EXPECT_EQ(image.bgra, std::vector<std::uint8_t>(180 * 320 * 4, 0x77));
    EXPECT_EQ(frame.data, original);
    frame = pixels(20, 10);
    ASSERT_TRUE(capture::prepare_navigation_thumbnail(frame, 320, &image));
    EXPECT_EQ(image.width, 20U); EXPECT_EQ(image.height, 10U);
    frame.data.clear();
    EXPECT_FALSE(capture::prepare_navigation_thumbnail(frame, 320, &image));
}
#ifdef _WIN32
TEST(NavigationThumbnail, ReusedWicStreamHasExactJpegBoundaryAcrossSizes) {
    capture::NavigationThumbnailEncoder encoder;
    for (const auto width : {1920U, 80U, 640U}) {
        auto frame = pixels(width, width / 2);
        capture::NavigationThumbnailImage image;
        std::vector<std::uint8_t> jpeg;
        std::string error;
        ASSERT_TRUE(capture::prepare_navigation_thumbnail(frame, 320, &image));
        ASSERT_TRUE(encoder.encode(image, &jpeg, &error)) << error;
        ASSERT_GE(jpeg.size(), 4U); EXPECT_LE(jpeg.size(), 512U * 1024U);
        EXPECT_EQ(jpeg[0], 0xff); EXPECT_EQ(jpeg[1], 0xd8);
        EXPECT_EQ(jpeg[jpeg.size()-2], 0xff); EXPECT_EQ(jpeg.back(), 0xd9);
    }
}
#endif
TEST(NavigationThumbnailWorker, SlowConsumerKeepsOnlyLatestPendingAndCadence) {
    SlowEncoder slow;
    std::mutex sent_mutex;
    std::vector<std::uint64_t> revisions;
    runtime::NavigationThumbnailWorker worker(true, [&](const auto& image, auto* jpeg, auto* error) { return slow.encode(image, jpeg, error); });
    worker.set_sender([&](auto packet) {
        net::NavigationThumbnail thumbnail;
        if (!net::parse_navigation_thumbnail(packet, &thumbnail, nullptr)) return false;
        std::lock_guard lock(sent_mutex); revisions.push_back(thumbnail.catalog_revision); return true;
    });
    const auto frame = pixels();
    worker.submit(frame, "display", 1, 1000);
    ASSERT_TRUE(slow.await_entry());
    EXPECT_FALSE(worker.wants_frame(1999));
    worker.submit(frame, "display", 2, 2000);
    worker.submit(frame, "display", 3, 3000);
    EXPECT_EQ(worker.stats().submitted, 3U);
    EXPECT_EQ(worker.stats().replaced, 1U);
    EXPECT_EQ(worker.stats().pending_bytes, 320U * 180U * 4U);
    slow.release();
    ASSERT_TRUE(wait_for([&] { return worker.stats().sent == 2; }));
    worker.stop();
    EXPECT_EQ(revisions, (std::vector<std::uint64_t>{1, 3}));
}
TEST(NavigationThumbnailWorker, ReconnectDiscardsActiveAndPendingOldGeneration) {
    SlowEncoder slow;
    runtime::NavigationThumbnailWorker worker(true, [&](const auto& image, auto* jpeg, auto* error) { return slow.encode(image, jpeg, error); });
    std::atomic<int> old_sent{0}, new_sent{0};
    worker.set_sender([&](auto) { ++old_sent; return true; });
    const auto frame = pixels();
    worker.submit(frame, "old", 1, 1000);
    ASSERT_TRUE(slow.await_entry());
    worker.submit(frame, "old", 1, 2000);
    worker.set_sender({});
    worker.set_sender([&](auto) { ++new_sent; return true; });
    EXPECT_TRUE(worker.wants_frame(2001));
    worker.submit(frame, "new", 2, 2001);
    slow.release();
    ASSERT_TRUE(wait_for([&] { return worker.stats().sent == 1; }));
    worker.stop();
    EXPECT_EQ(old_sent.load(), 0); EXPECT_EQ(new_sent.load(), 1);
    EXPECT_EQ(worker.stats().discarded, 2U);
}
TEST(NavigationThumbnailWorker, StopDiscardsPendingAndWaitsOutsideQueueMutex) {
    SlowEncoder slow;
    std::atomic<int> sent{0};
    runtime::NavigationThumbnailWorker worker(true, [&](const auto& image, auto* jpeg, auto* error) { return slow.encode(image, jpeg, error); });
    worker.set_sender([&](auto) { ++sent; return true; });
    const auto frame = pixels();
    worker.submit(frame, "display", 1, 1000);
    ASSERT_TRUE(slow.await_entry());
    worker.submit(frame, "display", 1, 2000);
    auto stopped = std::async(std::launch::async, [&] { worker.stop(); });
    ASSERT_TRUE(wait_for([&] { return !worker.wants_frame(3000); }));
    EXPECT_EQ(worker.stats().pending_bytes, 0U);
    slow.release();
    ASSERT_EQ(stopped.wait_for(3s), std::future_status::ready);
    stopped.get();
    EXPECT_EQ(sent.load(), 0);
    EXPECT_EQ(worker.stats().discarded, 2U);
}
TEST(NavigationThumbnailWorker, FailureDoesNotBlockNewFrames) {
    runtime::NavigationThumbnailWorker worker(true, [](const auto&, auto*, auto*) { return false; });
    worker.set_sender([](auto) { return true; });
    auto frame = pixels();
    worker.submit(frame, "display", 1, 1000);
    ASSERT_TRUE(wait_for([&] { return worker.stats().failed == 1; }));
    worker.invalidate();
    EXPECT_TRUE(worker.wants_frame(1001));
    worker.stop();
    EXPECT_FALSE(worker.wants_frame(10000));
    worker.submit(frame, "display", 1, 10000);
    EXPECT_EQ(worker.stats().submitted, 1U);
}
} // namespace
