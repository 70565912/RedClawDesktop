#pragma once
#include "redclaw/capture/capture_module.h"
#include "redclaw/render/render_module.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <thread>

namespace {

redclaw::capture::CapturedFrame make_test_bgra_frame(std::uint32_t width, std::uint32_t height) {
    redclaw::capture::CapturedFrame frame;
    frame.width = width;
    frame.height = height;
    frame.bgra = true;
    frame.row_pitch = width * 4;
    frame.data.resize(static_cast<std::size_t>(frame.row_pitch) * height, 0);

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * frame.row_pitch + static_cast<std::size_t>(x) * 4;
            frame.data[index + 0] = static_cast<std::uint8_t>((x * 3U) % 255U);
            frame.data[index + 1] = static_cast<std::uint8_t>((y * 5U) % 255U);
            frame.data[index + 2] = static_cast<std::uint8_t>(((x + y) * 7U) % 255U);
            frame.data[index + 3] = 255;
        }
    }

    return frame;
}

redclaw::render::EncodedVideoCodec to_render_codec(redclaw::capture::EncoderCodec codec) {
    switch (codec) {
    case redclaw::capture::EncoderCodec::kH264:
        return redclaw::render::EncodedVideoCodec::kH264;
    case redclaw::capture::EncoderCodec::kHevc:
        return redclaw::render::EncodedVideoCodec::kHevc;
    }

    return redclaw::render::EncodedVideoCodec::kUnknown;
}

void check_desktop_budget_after_low_cadence_resize(redclaw::capture::EncoderBackendType backend) {
    using namespace redclaw::capture;
    struct Step { std::uint32_t width, height, fps, kbps; };
    EncoderExecutionSession encoder;
    EncoderBackendBridgePlan plan;
    plan.selected_backend = backend;
    plan.allow_hardware_frame_input = false;
    std::string error;
    std::int64_t previous_pts = -1;
    std::uint64_t timestamp_ms = 1000;
    for (const auto [width, height, fps, kbps] : {
             Step{1184, 666, 30, 1892}, Step{1778, 1000, 5, 4267},
             Step{1184, 666, 1, 1892}, Step{1184, 666, 5, 1892}}) {
        SCOPED_TRACE(::testing::Message() << width << 'x' << height << " cadence=" << fps);
        EncoderProfileRequest request;
        request.width = width;
        request.height = height;
        request.fps = fps;
        EncoderConfigProfile profile;
        ASSERT_TRUE(build_desktop_encoder_profile(request, &profile, &error)) << error;
        ASSERT_TRUE(encoder.start(profile, plan, &error)) << error;
        ASSERT_EQ(encoder.diagnostics().backend, backend);
        ASSERT_EQ(encoder.diagnostics().configured_bitrate_kbps, kbps);
        redclaw::render::FfmpegVideoFrameDecoder decoder;
        const auto captured = make_test_bgra_frame(width, height);
        std::uint32_t frame_count = 0;
        // Resume motion then return to low cadence without a codec restart or
        // rate update. The final stage also covers a same-size reconnect.
        for (const auto interval_ms : {0U, 200U, 34U, 34U, 1000U}) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            timestamp_ms += interval_ms;
            EncodedFramePacket packet;
            ASSERT_TRUE(encoder.encode_bgra_frame(captured, timestamp_ms, &packet, &error)) << error;
            ASSERT_FALSE(packet.payload.empty());
            EXPECT_EQ(packet.keyframe, frame_count == 0);
            const auto diagnostics = encoder.diagnostics();
            EXPECT_EQ(diagnostics.configured_fps, 30U);
            EXPECT_EQ(diagnostics.configured_time_base_num, 1U);
            EXPECT_EQ(diagnostics.configured_time_base_den, 30U);
            EXPECT_EQ(diagnostics.configured_gop_frames, 60U);
            EXPECT_EQ(diagnostics.configured_bitrate_kbps, kbps);
            EXPECT_EQ(diagnostics.active_target_bitrate_kbps, kbps);
            EXPECT_EQ(diagnostics.active_max_bitrate_kbps, kbps + kbps / 5);
            EXPECT_EQ(diagnostics.rate_control_update_attempt_count, 0U);
            EXPECT_EQ(diagnostics.encoded_frame_count, ++frame_count);
            EXPECT_GT(diagnostics.last_submitted_pts, previous_pts);
            EXPECT_EQ(diagnostics.last_output_pts, diagnostics.last_submitted_pts);
            previous_pts = diagnostics.last_submitted_pts;
            redclaw::render::EncodedVideoFrame encoded;
            encoded.codec = to_render_codec(packet.codec);
            encoded.width = width;
            encoded.height = height;
            encoded.timestamp_ms = timestamp_ms;
            encoded.keyframe = packet.keyframe;
            encoded.payload = std::move(packet.payload);
            redclaw::render::DecodedVideoFrame decoded;
            ASSERT_TRUE(decoder.decode_frame(encoded, &decoded, &error)) << error;
            EXPECT_EQ(decoded.width, width);
            EXPECT_EQ(decoded.height, height);
            EXPECT_EQ(decoded.timestamp_ms, timestamp_ms);
            EXPECT_FALSE(decoded.pixels.empty());
        }
        encoder.stop();
        ++timestamp_ms;
    }
}
}  // namespace
