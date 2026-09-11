#include <cstdint>
#include <iostream>
#include <string>

#include "redclaw/capture/capture_module.h"
#include "redclaw/render/render_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

class IntegrationTestDecoder final : public redclaw::render::IVideoFrameDecoder {
public:
    bool decode_frame(
        const redclaw::render::EncodedVideoFrame& input,
        redclaw::render::DecodedVideoFrame* output,
        std::string* error_detail) override {
        if (output == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "decoded output pointer is null";
            }
            return false;
        }

        if (input.width == 0 || input.height == 0 || input.payload.empty()) {
            if (error_detail != nullptr) {
                *error_detail = "encoded frame metadata is invalid";
            }
            return false;
        }

        output->width = input.width;
        output->height = input.height;
        output->row_pitch = input.width * 4;
        output->timestamp_ms = input.timestamp_ms;
        output->bgra = true;
        output->pixels.assign(output->row_pitch * output->height, 0x4F);
        return true;
    }

    void reset() override {
        ++reset_count_;
    }

    [[nodiscard]] std::uint32_t reset_count() const {
        return reset_count_;
    }

private:
    std::uint32_t reset_count_ = 0;
};

redclaw::render::EncodedVideoFrame make_encoded_frame(
    std::uint32_t width,
    std::uint32_t height,
    std::uint64_t timestamp_ms,
    bool keyframe) {
    redclaw::render::EncodedVideoFrame frame;
    frame.codec = redclaw::render::EncodedVideoCodec::kH264;
    frame.width = width;
    frame.height = height;
    frame.timestamp_ms = timestamp_ms;
    frame.keyframe = keyframe;
    frame.payload.assign(1024, static_cast<std::uint8_t>(keyframe ? 0xAA : 0x55));
    return frame;
}

bool test_resolution_switch_recomputes_layout_and_keeps_queue_stable() {
    redclaw::capture::EncoderProfileRequest profile_request_a;
    profile_request_a.width = 1920;
    profile_request_a.height = 1080;
    profile_request_a.fps = 60;

    redclaw::capture::EncoderProfileRequest profile_request_b;
    profile_request_b.width = 1280;
    profile_request_b.height = 720;
    profile_request_b.fps = 60;

    redclaw::capture::EncoderConfigProfile profile_a;
    redclaw::capture::EncoderConfigProfile profile_b;
    std::string profile_error;

    const bool profile_ok_a = redclaw::capture::build_low_latency_encoder_profile(
        profile_request_a,
        &profile_a,
        &profile_error);
    const bool profile_ok_b = redclaw::capture::build_low_latency_encoder_profile(
        profile_request_b,
        &profile_b,
        &profile_error);

    if (!expect_true(profile_ok_a && profile_ok_b, "encoder profile generation should succeed")) {
        return false;
    }

    redclaw::render::DecodedFrameQueueConfig queue_config;
    queue_config.max_frames = 4;
    queue_config.drop_oldest_on_overflow = true;
    redclaw::render::DecodedFrameQueue queue(queue_config);

    IntegrationTestDecoder decoder;

    redclaw::render::RenderViewportConfig viewport_config;
    viewport_config.viewport_width = 1366;
    viewport_config.viewport_height = 768;
    viewport_config.scale_mode = redclaw::render::ViewportScaleMode::kFit;

    redclaw::render::RenderViewportLayout initial_layout;
    redclaw::render::RenderViewportLayout switched_layout;

    bool saw_resolution_switch = false;
    std::uint32_t processed_frames = 0;

    const redclaw::render::EncodedVideoFrame encoded_sequence[] = {
        make_encoded_frame(profile_a.width, profile_a.height, 1000, true),
        make_encoded_frame(profile_a.width, profile_a.height, 1016, false),
        make_encoded_frame(profile_b.width, profile_b.height, 1033, true),
        make_encoded_frame(profile_b.width, profile_b.height, 1050, false),
    };

    std::uint32_t last_width = 0;
    std::uint32_t last_height = 0;

    for (const auto& encoded : encoded_sequence) {
        redclaw::render::DecodedVideoFrame decoded;
        std::string decode_error;
        if (!expect_true(decoder.decode_frame(encoded, &decoded, &decode_error), "decode should succeed for integration sequence")) {
            return false;
        }

        if (last_width != 0 && (decoded.width != last_width || decoded.height != last_height)) {
            saw_resolution_switch = true;
            decoder.reset();
        }

        const auto push_status = queue.push(decoded);
        if (!expect_true(
                push_status == redclaw::render::QueuePushStatus::kAccepted
                    || push_status == redclaw::render::QueuePushStatus::kDroppedOldest,
                "queue push should stay within accepted/drop-oldest policy")) {
            return false;
        }

        redclaw::render::DecodedVideoFrame consumed;
        if (!expect_true(queue.pop(&consumed), "decoded queue pop should succeed")) {
            return false;
        }

        redclaw::render::RenderViewportLayout layout;
        std::string layout_error;
        if (!expect_true(
                redclaw::render::compute_render_viewport_layout(
                    consumed.width,
                    consumed.height,
                    viewport_config,
                    &layout,
                    &layout_error),
                "viewport layout should succeed")) {
            return false;
        }

        if (processed_frames < 2) {
            initial_layout = layout;
        } else {
            switched_layout = layout;
        }

        last_width = decoded.width;
        last_height = decoded.height;
        ++processed_frames;
    }

    return expect_true(processed_frames == 4, "integration sequence should process all frames")
        && expect_true(saw_resolution_switch, "integration sequence should detect a resolution switch")
        && expect_true(decoder.reset_count() == 1, "decoder reset should be called exactly once on switch")
        && expect_true(queue.empty(), "queue should remain drained after integration sequence")
        && expect_true(initial_layout.source.width != switched_layout.source.width, "source crop width should change after resolution switch")
        && expect_true(initial_layout.source.height != switched_layout.source.height, "source crop height should change after resolution switch")
        && expect_true(switched_layout.destination.width <= viewport_config.viewport_width, "switched destination width should remain in viewport bounds")
        && expect_true(switched_layout.destination.height <= viewport_config.viewport_height, "switched destination height should remain in viewport bounds");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_resolution_switch_recomputes_layout_and_keeps_queue_stable() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_m05_resolution_switch_integration_tests" << '\n';
    return 0;
}
