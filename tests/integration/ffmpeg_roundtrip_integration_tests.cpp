#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include "redclaw/helper/direct_frame_shared_memory.h"
#endif

#include "redclaw/capture/capture_module.h"
#include "redclaw/render/render_module.h"

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

}  // namespace

TEST(FfmpegRoundtripIntegration, EncodesAndDecodesSingleFrame) {
    redclaw::capture::EncoderProfileRequest profile_request;
    profile_request.width = 320;
    profile_request.height = 180;
    profile_request.fps = 10;
    profile_request.preferred_codec = redclaw::capture::EncoderCodec::kH264;

    redclaw::capture::EncoderBackendBridgeRequest bridge_request;
    bridge_request.codec = redclaw::capture::EncoderCodec::kH264;
    bridge_request.preferred_backend = redclaw::capture::EncoderBackendType::kSoftware;
    bridge_request.allow_hardware_fallback = true;

    redclaw::capture::EncoderExecutionSession encoder_session;
    redclaw::capture::EncoderBackendBridgePlan resolved_plan;
    std::string start_error;
    ASSERT_TRUE(redclaw::capture::start_encoder_execution_from_bridge(
        profile_request,
        bridge_request,
        &encoder_session,
        &resolved_plan,
        &start_error)) << start_error;
    ASSERT_TRUE(encoder_session.is_running());

    const redclaw::capture::CapturedFrame captured_frame = make_test_bgra_frame(
        profile_request.width,
        profile_request.height);

    redclaw::capture::EncodedFramePacket encoded_packet;
    std::string encode_error;
    ASSERT_TRUE(encoder_session.encode_bgra_frame(
        captured_frame,
        1000,
        &encoded_packet,
        &encode_error)) << encode_error;
    ASSERT_FALSE(encoded_packet.payload.empty());

    redclaw::render::EncodedVideoFrame encoded_frame;
    encoded_frame.codec = to_render_codec(encoded_packet.codec);
    encoded_frame.width = captured_frame.width;
    encoded_frame.height = captured_frame.height;
    encoded_frame.timestamp_ms = encoded_packet.timestamp_ms;
    encoded_frame.keyframe = encoded_packet.keyframe;
    encoded_frame.payload = encoded_packet.payload;

    redclaw::render::FfmpegVideoFrameDecoder decoder;
    redclaw::render::DecodedVideoFrame decoded_frame;
    std::string decode_error;
    ASSERT_TRUE(decoder.decode_frame(encoded_frame, &decoded_frame, &decode_error)) << decode_error;

    EXPECT_EQ(decoded_frame.width, captured_frame.width);
    EXPECT_EQ(decoded_frame.height, captured_frame.height);
    EXPECT_EQ(decoded_frame.row_pitch, captured_frame.width * 4U);
    EXPECT_EQ(decoded_frame.timestamp_ms, encoded_packet.timestamp_ms);
    EXPECT_TRUE(decoded_frame.bgra);
    EXPECT_FALSE(decoded_frame.pixels.empty());

#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_context;
    D3D_FEATURE_LEVEL feature_level{};
    const HRESULT device_result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &d3d11_device,
        &feature_level,
        &d3d11_context);
    if (FAILED(device_result)) {
        GTEST_SKIP() << "D3D11 hardware video device is unavailable";
    }

    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (FAILED(d3d11_context.As(&multithread))) {
        GTEST_SKIP() << "D3D11 multithread protection is unavailable";
    }
    multithread->SetMultithreadProtected(TRUE);

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device;
    if (FAILED(d3d11_device.As(&video_device))) {
        GTEST_SKIP() << "D3D11 video decode interface is unavailable";
    }
    bool supports_h264_nv12 = false;
    for (UINT index = 0; index < video_device->GetVideoDecoderProfileCount(); ++index) {
        GUID profile{};
        BOOL format_supported = FALSE;
        if (SUCCEEDED(video_device->GetVideoDecoderProfile(index, &profile))
            && IsEqualGUID(profile, D3D11_DECODER_PROFILE_H264_VLD_NOFGT)
            && SUCCEEDED(video_device->CheckVideoDecoderFormat(
                &profile, DXGI_FORMAT_NV12, &format_supported))
            && format_supported) {
            supports_h264_nv12 = true;
            break;
        }
    }
    if (!supports_h264_nv12) {
        GTEST_SKIP() << "D3D11 H.264 NV12 decode is unavailable";
    }

    redclaw::render::FfmpegVideoFrameDecoder surface_decoder;
    surface_decoder.configure_d3d11_surface_output(d3d11_device.Get());
    redclaw::render::DecodedVideoFrame surface_frame;
    ASSERT_TRUE(surface_decoder.decode_frame(encoded_frame, &surface_frame, &decode_error)) << decode_error;
    EXPECT_EQ(
        surface_frame.output_path,
        redclaw::render::DecodedVideoFramePath::kD3D11DecodeSurface);
    EXPECT_FALSE(surface_frame.bgra);
    EXPECT_TRUE(surface_frame.pixels.empty());
    ASSERT_NE(surface_frame.d3d11_surface, nullptr);
    EXPECT_EQ(surface_frame.d3d11_surface->device, d3d11_device.Get());
    EXPECT_NE(surface_frame.d3d11_surface->texture, nullptr);
    EXPECT_NE(surface_frame.d3d11_surface->frame_lifetime, nullptr);

    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context;
    ASSERT_TRUE(SUCCEEDED(d3d11_context.As(&video_context)));

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc{};
    content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content_desc.InputFrameRate = {10, 1};
    content_desc.InputWidth = surface_frame.width;
    content_desc.InputHeight = surface_frame.height;
    content_desc.OutputFrameRate = {10, 1};
    content_desc.OutputWidth = surface_frame.width;
    content_desc.OutputHeight = surface_frame.height;
    content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
    ASSERT_TRUE(SUCCEEDED(video_device->CreateVideoProcessorEnumerator(&content_desc, &enumerator)));
    ASSERT_TRUE(SUCCEEDED(video_device->CreateVideoProcessor(enumerator.Get(), 0, &processor)));

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.ArraySlice = surface_frame.d3d11_surface->array_slice;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
    ASSERT_TRUE(SUCCEEDED(video_device->CreateVideoProcessorInputView(
        surface_frame.d3d11_surface->texture,
        enumerator.Get(),
        &input_desc,
        &input_view)));

    D3D11_TEXTURE2D_DESC output_texture_desc{};
    output_texture_desc.Width = surface_frame.width;
    output_texture_desc.Height = surface_frame.height;
    output_texture_desc.MipLevels = 1;
    output_texture_desc.ArraySize = 1;
    output_texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    output_texture_desc.SampleDesc.Count = 1;
    output_texture_desc.Usage = D3D11_USAGE_DEFAULT;
    output_texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_texture;
    ASSERT_TRUE(SUCCEEDED(d3d11_device->CreateTexture2D(
        &output_texture_desc, nullptr, &output_texture)));

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view;
    ASSERT_TRUE(SUCCEEDED(video_device->CreateVideoProcessorOutputView(
        output_texture.Get(), enumerator.Get(), &output_desc, &output_view)));

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    EXPECT_TRUE(SUCCEEDED(video_context->VideoProcessorBlt(
        processor.Get(), output_view.Get(), 0, 1, &stream)));
#endif

    encoder_session.stop();
}

// Developer-selected hardware check; not part of the portable CTest/release gate.
TEST(FfmpegNvencRecoveryIntegration, RequestProducesStandaloneIdr) {
    redclaw::capture::EncoderProfileRequest request;
    request.width = 320;
    request.height = 180;
    request.fps = 30;
    redclaw::capture::EncoderConfigProfile profile;
    std::string error;
    ASSERT_TRUE(redclaw::capture::build_low_latency_encoder_profile(request, &profile, &error)) << error;
    profile.gop_length_frames = 60;
    redclaw::capture::EncoderBackendBridgePlan plan;
    plan.selected_backend = redclaw::capture::EncoderBackendType::kNvenc;
    redclaw::capture::EncoderExecutionSession encoder;
    ASSERT_TRUE(encoder.start(profile, plan, &error)) << error;
    const auto captured = make_test_bgra_frame(request.width, request.height);
    for (std::uint64_t index = 0; index < 5; ++index) {
        const bool recovery = index == 2 || index == 4;
        if (recovery) encoder.request_keyframe();
        redclaw::capture::EncodedFramePacket packet;
        ASSERT_TRUE(encoder.encode_bgra_frame(captured, 1000 + index * 34, &packet, &error)) << error;
        ASSERT_FALSE(packet.payload.empty());
        if (!recovery) {
            EXPECT_EQ(packet.keyframe, index == 0);
            continue;
        }
        ASSERT_TRUE(packet.keyframe);
        bool contains_idr = false;
        for (std::size_t offset = 0; offset + 3 < packet.payload.size(); ++offset) {
            if (packet.payload[offset] == 0 && packet.payload[offset + 1] == 0
                && packet.payload[offset + 2] == 1
                && (packet.payload[offset + 3] & 0x1f) == 5) {
                contains_idr = true;
            }
        }
        ASSERT_TRUE(contains_idr) << "Requested recovery must contain H.264 IDR, not only an intra slice";
        redclaw::render::EncodedVideoFrame encoded;
        encoded.codec = to_render_codec(packet.codec);
        encoded.width = captured.width;
        encoded.height = captured.height;
        encoded.timestamp_ms = packet.timestamp_ms;
        encoded.keyframe = packet.keyframe;
        encoded.payload = std::move(packet.payload);
        // A fresh decoder has neither the initial headers nor prior references.
        redclaw::render::FfmpegVideoFrameDecoder decoder;
        redclaw::render::DecodedVideoFrame decoded;
        ASSERT_TRUE(decoder.decode_frame(encoded, &decoded, &error)) << error;
        EXPECT_EQ(decoded.width, captured.width);
        EXPECT_EQ(decoded.height, captured.height);
        EXPECT_FALSE(decoded.pixels.empty());
    }
}

TEST(FfmpegRoundtripIntegration, DecoderCanAdvanceWithoutBgraOutput) {
    redclaw::capture::EncoderProfileRequest profile_request;
    profile_request.width = 320;
    profile_request.height = 180;
    profile_request.fps = 10;
    profile_request.preferred_codec = redclaw::capture::EncoderCodec::kH264;

    redclaw::capture::EncoderBackendBridgeRequest bridge_request;
    bridge_request.codec = redclaw::capture::EncoderCodec::kH264;
    bridge_request.preferred_backend = redclaw::capture::EncoderBackendType::kSoftware;
    bridge_request.allow_hardware_fallback = true;

    redclaw::capture::EncoderExecutionSession encoder_session;
    redclaw::capture::EncoderBackendBridgePlan resolved_plan;
    std::string start_error;
    ASSERT_TRUE(redclaw::capture::start_encoder_execution_from_bridge(
        profile_request,
        bridge_request,
        &encoder_session,
        &resolved_plan,
        &start_error)) << start_error;
    ASSERT_TRUE(encoder_session.is_running());

    const redclaw::capture::CapturedFrame captured_frame = make_test_bgra_frame(
        profile_request.width,
        profile_request.height);

    redclaw::capture::EncodedFramePacket encoded_packet;
    std::string encode_error;
    ASSERT_TRUE(encoder_session.encode_bgra_frame(
        captured_frame,
        1000,
        &encoded_packet,
        &encode_error)) << encode_error;
    ASSERT_FALSE(encoded_packet.payload.empty());

    redclaw::render::FfmpegVideoFrameDecoder decoder;
    bool frame_ready = false;
    std::string decode_error;
    ASSERT_TRUE(decoder.decode_frame_view(
        to_render_codec(encoded_packet.codec),
        captured_frame.width,
        captured_frame.height,
        encoded_packet.timestamp_ms,
        encoded_packet.keyframe,
        encoded_packet.payload.data(),
        encoded_packet.payload.size(),
        &frame_ready,
        nullptr,
        &decode_error)) << decode_error;
    EXPECT_TRUE(frame_ready);
    EXPECT_TRUE(decode_error.empty());

    encoder_session.stop();
}

#if defined(_WIN32)
TEST(FfmpegRoundtripIntegration, TwoSlotBurstsPreservePredictiveChainWithoutExtraIdr) {
    redclaw::capture::EncoderProfileRequest profile;
    profile.width = 320;
    profile.height = 180;
    profile.fps = 24;
    profile.preferred_codec = redclaw::capture::EncoderCodec::kH264;
    redclaw::capture::EncoderBackendBridgeRequest bridge;
    bridge.codec = profile.preferred_codec;
    bridge.preferred_backend = redclaw::capture::EncoderBackendType::kSoftware;
    redclaw::capture::EncoderExecutionSession encoder;
    redclaw::capture::EncoderBackendBridgePlan resolved;
    std::string error;
    ASSERT_TRUE(redclaw::capture::start_encoder_execution_from_bridge(
        profile, bridge, &encoder, &resolved, &error)) << error;

    std::vector<std::uint64_t> mapping((redclaw::helper::direct_frame_shared_mapping_size() + 7) / 8);
    auto* header = reinterpret_cast<redclaw::helper::DirectFrameSharedMemoryHeader*>(mapping.data());
    redclaw::helper::initialize_direct_frame_shared_memory(header);
    redclaw::helper::DirectFrameSharedSnapshot copy;
    ASSERT_TRUE(redclaw::helper::prepare_direct_frame_shared_snapshot(&copy));
    redclaw::render::FfmpegVideoFrameDecoder decoder;
    redclaw::render::FfmpegVideoFrameDecoder sequential_baseline;
    redclaw::render::DecodedVideoFrame expected;
    auto capture = make_test_bgra_frame(profile.width, profile.height);
    std::uint64_t observed = 0;
    unsigned catchups = 0, predictive = 0, decoded = 0;
    unsigned dependency_gaps = 0, old_latest_only_gaps = 0;
    // Frame 1 establishes the reference. Each following pair arrives before
    // the consumer wakes, reproducing event coalescing without a larger queue.
    for (std::uint64_t sequence = 1; sequence <= 13; ++sequence) {
        capture.data[(sequence * 131) % capture.data.size()] ^= 127;
        redclaw::capture::EncodedFramePacket packet;
        ASSERT_TRUE(encoder.encode_bgra_frame(capture, sequence * 42, &packet, &error)) << error;
        if (!packet.keyframe) ++predictive;
        redclaw::render::EncodedVideoFrame encoded;
        encoded.codec = to_render_codec(packet.codec);
        encoded.width = profile.width;
        encoded.height = profile.height;
        encoded.timestamp_ms = packet.timestamp_ms;
        encoded.keyframe = packet.keyframe;
        encoded.payload = packet.payload;
        ASSERT_TRUE(sequential_baseline.decode_frame(encoded, &expected, &error)) << error;
        auto* slot = redclaw::helper::direct_frame_shared_slot_header(mapping.data(),
            static_cast<std::uint32_t>((sequence - 1) % 2));
        redclaw::helper::direct_frame_atomic_store_i64(&slot->committed_sequence, 0);
        slot->frame = {};
        slot->frame.width = profile.width;
        slot->frame.height = profile.height;
        slot->frame.format = redclaw::helper::kDirectFrameFormatH264;
        slot->frame.capture_region_revision = 1;
        slot->frame.timestamp_ms = packet.timestamp_ms;
        slot->frame.flags = packet.keyframe ? 1U : 0U;
        slot->frame.payload_size = static_cast<std::uint32_t>(packet.payload.size());
        std::memcpy(redclaw::helper::direct_frame_shared_slot_payload(slot),
            packet.payload.data(), packet.payload.size());
        redclaw::helper::direct_frame_atomic_store_i64(&slot->committed_sequence, sequence);
        redclaw::helper::direct_frame_atomic_store_i64(&header->latest_sequence, sequence);
        if (sequence % 2 == 0) continue;
        if (sequence > observed + 1 && !packet.keyframe) ++old_latest_only_gaps;
        redclaw::render::DecodedVideoFrame latest;
        while (observed < sequence) {
            const auto plan = redclaw::helper::plan_direct_frame_shared_read(
                mapping.data(), header, observed, observed == 0);
            ASSERT_EQ(plan.sequence, observed + 1); // No dependency skipped.
            if (plan.sequence > observed + 1) ++dependency_gaps;
            catchups += plan.dependency_catchup ? 1U : 0U;
            ASSERT_EQ(redclaw::helper::copy_direct_frame_shared_sequence(
                mapping.data(), header, plan.sequence, &copy),
                redclaw::helper::DirectFrameSharedSnapshotStatus::kCopied);
            ASSERT_EQ(header->reader_active_sequence, 0);
            bool ready = false;
            ASSERT_TRUE(decoder.decode_frame_view(to_render_codec(packet.codec),
                copy.frame.width, copy.frame.height, copy.frame.timestamp_ms,
                (copy.frame.flags & 1U) != 0, copy.payload.data(), copy.payload.size(),
                &ready, &latest, &error)) << error;
            ASSERT_TRUE(ready);
            observed = plan.sequence;
            ++decoded;
        }
        EXPECT_EQ(latest.pixels, expected.pixels); // Same decoded result as no coalescing.
        EXPECT_EQ(latest.timestamp_ms, expected.timestamp_ms);
    }
    EXPECT_EQ(predictive, 12U);
    EXPECT_EQ(decoded, 13U);
    EXPECT_EQ(catchups, 6U);
    EXPECT_EQ(header->slot_count, 2U);
    EXPECT_EQ(old_latest_only_gaps, 6U);
    EXPECT_EQ(dependency_gaps, 0U);
    RecordProperty("dependency_catchups", catchups);
    RecordProperty("decoded_frames", decoded);
    RecordProperty("old_latest_only_dependency_gaps", old_latest_only_gaps);
    RecordProperty("dependency_gaps", dependency_gaps);
}
#endif

TEST(FfmpegRoundtripIntegration, RebuildsEncoderAndResetsDecoderAcrossThreeResolutions) {
    struct Resolution {
        std::uint32_t width;
        std::uint32_t height;
    };
    const Resolution resolutions[] = {
        {320, 180},
        {480, 270},
        {256, 144},
    };

    redclaw::render::FfmpegVideoFrameDecoder decoder;
    std::uint64_t total_encoded_bytes = 0;
    for (std::size_t index = 0; index < std::size(resolutions); ++index) {
        const Resolution resolution = resolutions[index];
        redclaw::capture::EncoderProfileRequest profile_request;
        profile_request.width = resolution.width;
        profile_request.height = resolution.height;
        profile_request.fps = 15;
        profile_request.preferred_codec = redclaw::capture::EncoderCodec::kH264;

        redclaw::capture::EncoderBackendBridgeRequest bridge_request;
        bridge_request.codec = redclaw::capture::EncoderCodec::kH264;
        bridge_request.preferred_backend = redclaw::capture::EncoderBackendType::kSoftware;
        bridge_request.allow_hardware_fallback = true;

        redclaw::capture::EncoderExecutionSession encoder_session;
        redclaw::capture::EncoderBackendBridgePlan resolved_plan;
        std::string start_error;
        ASSERT_TRUE(redclaw::capture::start_encoder_execution_from_bridge(
            profile_request,
            bridge_request,
            &encoder_session,
            &resolved_plan,
            &start_error)) << start_error;

        const auto captured_frame = make_test_bgra_frame(resolution.width, resolution.height);
        redclaw::capture::EncodedFramePacket encoded_packet;
        std::string encode_error;
        ASSERT_TRUE(encoder_session.encode_bgra_frame(
            captured_frame,
            1000 + index * 100,
            &encoded_packet,
            &encode_error)) << encode_error;
        ASSERT_FALSE(encoded_packet.payload.empty());
        total_encoded_bytes += encoded_packet.payload.size();

        if (index != 0) {
            decoder.reset();
        }
        redclaw::render::EncodedVideoFrame encoded_frame;
        encoded_frame.codec = to_render_codec(encoded_packet.codec);
        encoded_frame.width = resolution.width;
        encoded_frame.height = resolution.height;
        encoded_frame.timestamp_ms = encoded_packet.timestamp_ms;
        encoded_frame.keyframe = encoded_packet.keyframe;
        encoded_frame.payload = std::move(encoded_packet.payload);

        redclaw::render::DecodedVideoFrame decoded_frame;
        std::string decode_error;
        ASSERT_TRUE(decoder.decode_frame(encoded_frame, &decoded_frame, &decode_error))
            << "resolution=" << resolution.width << 'x' << resolution.height
            << " error=" << decode_error;
        EXPECT_EQ(decoded_frame.width, resolution.width);
        EXPECT_EQ(decoded_frame.height, resolution.height);
        EXPECT_FALSE(decoded_frame.pixels.empty());
        encoder_session.stop();
    }

    EXPECT_GT(total_encoded_bytes, 0U);
}
