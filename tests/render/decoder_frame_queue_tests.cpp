#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

#include "redclaw/render/render_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

class FakeVideoFrameDecoder final : public redclaw::render::IVideoFrameDecoder {
public:
    bool decode_frame(
        const redclaw::render::EncodedVideoFrame& input,
        redclaw::render::DecodedVideoFrame* output,
        std::string* error_detail) override {
        if (output == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "output frame pointer is null";
            }
            return false;
        }

        if (input.payload.empty()) {
            if (error_detail != nullptr) {
                *error_detail = "encoded payload is empty";
            }
            return false;
        }

        output->width = input.width;
        output->height = input.height;
        output->row_pitch = input.width * 4;
        output->timestamp_ms = input.timestamp_ms;
        output->bgra = true;
        output->pixels.assign(output->row_pitch * output->height, 0x7F);
        return true;
    }

    void reset() override {}
};

bool test_decoder_abstraction_accepts_encoded_frames() {
    FakeVideoFrameDecoder decoder;

    redclaw::render::EncodedVideoFrame encoded;
    encoded.codec = redclaw::render::EncodedVideoCodec::kH264;
    encoded.width = 1280;
    encoded.height = 720;
    encoded.timestamp_ms = 42;
    encoded.payload.assign(256, 0xAB);

    redclaw::render::DecodedVideoFrame decoded;
    std::string error;
    const bool ok = decoder.decode_frame(encoded, &decoded, &error);

    return expect_true(ok, "decoder should accept non-empty encoded frame")
        && expect_true(error.empty(), "successful decode should not set error")
        && expect_true(decoded.width == encoded.width, "decoded width should match")
        && expect_true(decoded.height == encoded.height, "decoded height should match")
        && expect_true(decoded.timestamp_ms == encoded.timestamp_ms, "decoded timestamp should match")
        && expect_true(!decoded.pixels.empty(), "decoded pixels should be generated");
}

bool test_frame_queue_fifo_order() {
    redclaw::render::DecodedFrameQueue queue;

    redclaw::render::DecodedVideoFrame frame_a;
    frame_a.timestamp_ms = 100;
    redclaw::render::DecodedVideoFrame frame_b;
    frame_b.timestamp_ms = 200;

    const auto push_a = queue.push(frame_a);
    const auto push_b = queue.push(frame_b);

    redclaw::render::DecodedVideoFrame pop_a;
    redclaw::render::DecodedVideoFrame pop_b;

    return expect_true(push_a == redclaw::render::QueuePushStatus::kAccepted, "first push should be accepted")
        && expect_true(push_b == redclaw::render::QueuePushStatus::kAccepted, "second push should be accepted")
        && expect_true(queue.size() == 2, "queue size should be 2 before pop")
        && expect_true(queue.pop(&pop_a), "first pop should succeed")
        && expect_true(queue.pop(&pop_b), "second pop should succeed")
        && expect_true(pop_a.timestamp_ms == 100, "first pop should return oldest frame")
        && expect_true(pop_b.timestamp_ms == 200, "second pop should return next frame")
        && expect_true(queue.empty(), "queue should be empty after pops");
}

bool test_frame_queue_drops_oldest_when_full() {
    redclaw::render::DecodedFrameQueueConfig config;
    config.max_frames = 2;
    config.drop_oldest_on_overflow = true;

    redclaw::render::DecodedFrameQueue queue(config);

    redclaw::render::DecodedVideoFrame frame_a;
    frame_a.timestamp_ms = 1;
    redclaw::render::DecodedVideoFrame frame_b;
    frame_b.timestamp_ms = 2;
    redclaw::render::DecodedVideoFrame frame_c;
    frame_c.timestamp_ms = 3;

    queue.push(frame_a);
    queue.push(frame_b);
    const auto overflow = queue.push(frame_c);

    redclaw::render::DecodedVideoFrame first;
    redclaw::render::DecodedVideoFrame second;

    return expect_true(overflow == redclaw::render::QueuePushStatus::kDroppedOldest, "overflow push should drop oldest frame")
        && expect_true(queue.dropped_frame_count() == 1, "dropped counter should increment")
        && expect_true(queue.pop(&first), "pop after overflow should succeed")
        && expect_true(queue.pop(&second), "second pop after overflow should succeed")
        && expect_true(first.timestamp_ms == 2, "oldest frame should be dropped")
        && expect_true(second.timestamp_ms == 3, "new frame should be retained");
}

bool test_frame_queue_rejects_when_overflow_drop_disabled() {
    redclaw::render::DecodedFrameQueueConfig config;
    config.max_frames = 1;
    config.drop_oldest_on_overflow = false;

    redclaw::render::DecodedFrameQueue queue(config);

    redclaw::render::DecodedVideoFrame frame_a;
    frame_a.timestamp_ms = 10;
    redclaw::render::DecodedVideoFrame frame_b;
    frame_b.timestamp_ms = 20;

    queue.push(frame_a);
    const auto overflow = queue.push(frame_b);

    redclaw::render::DecodedVideoFrame popped;
    return expect_true(overflow == redclaw::render::QueuePushStatus::kRejectedFull, "overflow should be rejected when drop policy disabled")
        && expect_true(queue.dropped_frame_count() == 0, "reject path should not increment dropped counter")
        && expect_true(queue.pop(&popped), "existing frame should still be readable")
        && expect_true(popped.timestamp_ms == 10, "original frame should stay in queue");
}

bool test_decoded_frame_buffer_pool_reuses_and_bounds_capacity() {
    redclaw::render::DecodedFrameBufferPoolConfig config;
    config.max_buffers = 2;
    config.max_buffer_bytes = 1024;
    config.max_total_bytes = 1536;
    redclaw::render::DecodedFrameBufferPool pool(config);

    redclaw::render::DecodedVideoFrame reusable;
    reusable.width = 16;
    reusable.output_path = redclaw::render::DecodedVideoFramePath::kHardwareDecodeCpuTransferBgra;
    reusable.pixels.reserve(1024);
    reusable.pixels.resize(512);
    pool.release(std::move(reusable));

    auto acquired = pool.acquire(800);
    bool ok = true;
    ok = expect_true(acquired.pixels.capacity() >= 800, "pool should return a sufficiently large buffer") && ok;
    ok = expect_true(acquired.pixels.empty(), "acquired buffer should keep capacity but clear size") && ok;
    ok = expect_true(acquired.width == 0, "acquired buffer metadata should be reset") && ok;
    ok = expect_true(
        acquired.output_path == redclaw::render::DecodedVideoFramePath::kUnknown,
        "acquired buffer output path should be reset") && ok;

    acquired.pixels.resize(900);
    pool.release(std::move(acquired));

    redclaw::render::DecodedVideoFrame oversized;
    oversized.pixels.reserve(2048);
    oversized.pixels.resize(1);
    pool.release(std::move(oversized));

    const auto telemetry = pool.telemetry();
    ok = expect_true(telemetry.acquire_requests == 1, "pool should count acquire requests") && ok;
    ok = expect_true(telemetry.reuse_hits == 1, "pool should count reuse hits") && ok;
    ok = expect_true(telemetry.retained_buffers == 1, "pool should retain the reusable buffer") && ok;
    ok = expect_true(telemetry.retained_bytes <= config.max_total_bytes, "pool should respect total-byte bound") && ok;
    ok = expect_true(telemetry.discarded_releases == 1, "pool should discard an oversized buffer") && ok;
    return ok;
}

bool test_decoded_frame_path_names_are_explicit() {
    using redclaw::render::DecodedVideoFramePath;
    return expect_true(
               redclaw::render::decoded_video_frame_path_name(
                   DecodedVideoFramePath::kHardwareDecodeCpuTransferBgra)
                   == "hardware-decode-cpu-transfer-bgra",
               "hardware transfer path name should not claim zero copy")
        && expect_true(
            redclaw::render::decoded_video_frame_path_name(
                DecodedVideoFramePath::kSoftwareDecodeBgra)
                == "software-decode-bgra",
            "software decode path name should be explicit")
        && expect_true(
            redclaw::render::decoded_video_frame_path_name(
                DecodedVideoFramePath::kD3D11DecodeSurface)
                == "d3d11-decode-surface",
            "D3D11 surface path name should describe the real data flow");
}

bool test_runtime_hardware_decode_fallback_is_immediate_before_first_frame() {
    return expect_true(
               redclaw::render::should_fallback_to_software_decode(true, 1, 0),
               "first pre-frame hardware decode failure should select software decode")
        && expect_true(
            !redclaw::render::should_fallback_to_software_decode(false, 1, 0),
            "software failures must not recurse into another fallback")
        && expect_true(
            !redclaw::render::should_fallback_to_software_decode(true, 1, 1),
            "an established hardware decoder should not switch on a short isolated burst");
}

#if defined(_WIN32)
bool test_buffer_pool_releases_d3d11_surface_lifetime() {
    redclaw::render::DecodedFrameBufferPool pool;
    redclaw::render::DecodedVideoFrame frame;
    frame.d3d11_surface = std::make_shared<redclaw::render::D3D11DecodedSurface>();
    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<int> weak_lifetime = lifetime;
    frame.d3d11_surface->frame_lifetime = lifetime;
    lifetime.reset();

    pool.release(std::move(frame));
    return expect_true(
        weak_lifetime.expired(),
        "recycling a frame should release its decoded-surface lifetime before pooling CPU storage");
}
#endif

bool test_decoded_frame_buffer_pool_replaces_smaller_buffers() {
    redclaw::render::DecodedFrameBufferPoolConfig config;
    config.max_buffers = 2;
    config.max_buffer_bytes = 2048;
    config.max_total_bytes = 4096;
    redclaw::render::DecodedFrameBufferPool pool(config);

    for (int index = 0; index < 2; ++index) {
        redclaw::render::DecodedVideoFrame small;
        small.pixels.reserve(512);
        small.pixels.resize(1);
        pool.release(std::move(small));
    }

    redclaw::render::DecodedVideoFrame large;
    large.pixels.reserve(1536);
    large.pixels.resize(1);
    pool.release(std::move(large));

    auto acquired = pool.acquire(1400);
    const auto telemetry = pool.telemetry();
    return expect_true(acquired.pixels.capacity() >= 1400, "larger replacement buffer should be reusable")
        && expect_true(telemetry.evicted_buffers == 1, "one smaller buffer should be evicted")
        && expect_true(telemetry.retained_bytes <= config.max_total_bytes, "replacement should preserve total-byte bound");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_decoder_abstraction_accepts_encoded_frames() && ok;
    ok = test_frame_queue_fifo_order() && ok;
    ok = test_frame_queue_drops_oldest_when_full() && ok;
    ok = test_frame_queue_rejects_when_overflow_drop_disabled() && ok;
    ok = test_decoded_frame_buffer_pool_reuses_and_bounds_capacity() && ok;
    ok = test_decoded_frame_path_names_are_explicit() && ok;
    ok = test_runtime_hardware_decode_fallback_is_immediate_before_first_frame() && ok;
    ok = test_decoded_frame_buffer_pool_replaces_smaller_buffers() && ok;
#if defined(_WIN32)
    ok = test_buffer_pool_releases_d3d11_surface_lifetime() && ok;
#endif

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_render_decoder_frame_queue_tests" << '\n';
    return 0;
}
