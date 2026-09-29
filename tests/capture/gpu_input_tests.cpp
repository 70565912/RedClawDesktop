#include <gtest/gtest.h>
#include "redclaw/capture/hardware_input_policy.h"
#include "redclaw/capture/navigation_thumbnail.h"
#include "redclaw/render/render_module.h"
#include <atomic>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include "capture_d3d11.h"
#include <d3d11_4.h>
#endif
using namespace redclaw::capture;

TEST(HardwareInputPolicy, ConfirmationAndFailureHaveDifferentRestartSemantics) {
    HardwareInputPolicy policy;
    policy.configure(true); policy.observe_generation(7);
    EXPECT_EQ(policy.delivery(), CaptureFrameDelivery::kCpuAndGpu);
    policy.begin_attempt();
    EXPECT_EQ(policy.delivery(), CaptureFrameDelivery::kCpuAndGpu);
    policy.confirm_output();
    EXPECT_EQ(policy.delivery(), CaptureFrameDelivery::kGpu);
    policy.invalidate_context(); policy.configure(true); // Resize: prove output again.
    EXPECT_EQ(policy.delivery(), CaptureFrameDelivery::kCpuAndGpu);
    policy.fail_generation();
    for (int i = 0; i < 10; ++i) {
        policy.configure(true); policy.observe_generation(7); // Same device must not retry.
        EXPECT_EQ(policy.delivery(), CaptureFrameDelivery::kCpu);
    }
    EXPECT_EQ(policy.fallbacks(), 1U);
    policy.observe_generation(8);
    EXPECT_EQ(policy.delivery(), CaptureFrameDelivery::kCpuAndGpu);
    policy.configure(false); policy.observe_generation(9); policy.confirm_output();
    EXPECT_EQ(policy.delivery(), CaptureFrameDelivery::kCpu);
}

#ifdef _WIN32
namespace {
std::shared_ptr<D3D11CaptureDevice> create_device(D3D_DRIVER_TYPE driver) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr))) return {};
    return D3D11CaptureDevice::create(device.Get());
}
CapturedFrame make_frame(const std::shared_ptr<D3D11CaptureDevice>& owner, std::uint64_t generation = 1) {
    CapturedFrame frame;
    frame.width = 640; frame.height = 400; frame.row_pitch = frame.width * 4;
    frame.bgra = true; frame.capture_generation = generation;
    frame.data.resize(static_cast<std::size_t>(frame.row_pitch) * frame.height);
    for (std::uint32_t y = 0; y < frame.height; ++y) for (std::uint32_t x = 0; x < frame.width; ++x) {
        auto* p = frame.data.data() + static_cast<std::size_t>(y) * frame.row_pitch + x * 4;
        p[0] = 0; p[1] = 0; p[2] = 255; p[3] = 255;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = frame.width; desc.Height = frame.height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA pixels{frame.data.data(), frame.row_pitch, 0};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(owner->device->CreateTexture2D(&desc, &pixels, &texture))) return {};
    frame.native_handle_type = CapturedFrameNativeHandleType::kD3D11Texture2D;
    frame.native_handle = make_d3d11_native_handle(owner, texture.Get(), desc.Format, 0);
    return frame;
}
}
TEST(HardwareInputDevice, NativeLeaseRetainsProtectedRecursiveContext) {
    auto owner = create_device(D3D_DRIVER_TYPE_WARP);
    ASSERT_TRUE(owner);
    Microsoft::WRL::ComPtr<ID3D11Multithread> protection;
    ASSERT_TRUE(SUCCEEDED(owner->context.As(&protection)));
    EXPECT_TRUE(protection->GetMultithreadProtected());
    auto frame = make_frame(owner);
    ASSERT_TRUE(frame.native_handle);
    std::weak_ptr<D3D11CaptureDevice> lifetime = owner;
    owner.reset();
    ASSERT_FALSE(lifetime.expired());
    {
        std::lock_guard first(frame.native_handle->owner->mutex);
        std::lock_guard recursive(frame.native_handle->owner->mutex);
        frame.native_handle->owner->context->Flush();
    }
    frame = {};
    EXPECT_TRUE(lifetime.expired());
}

// Developer-invoked hardware cases: no desktop capture, network, or live session
// replacement. CTest registers only the portable policy and WARP ownership case.
TEST(GpuInputHardware, Nv12CropAndBoundedThumbnailPreserveColor) {
    auto owner = create_device(D3D_DRIVER_TYPE_HARDWARE);
    ASSERT_TRUE(owner);
    auto source = make_frame(owner);
    ASSERT_TRUE(source.native_handle);
    source.source_region = {.x = 0, .y = 0, .width = 400, .height = 400, .revision = 3};
    D3D11VideoProcessorScaler scaler;
    CapturedFrame nv12;
    std::string error;
    ASSERT_TRUE(scaler.scale(source, 320, 200, DXGI_FORMAT_NV12, &nv12, &error)) << error;
    D3D11_TEXTURE2D_DESC desc{};
    nv12.native_handle->d3d11_texture->GetDesc(&desc);
    EXPECT_EQ(desc.Format, DXGI_FORMAT_NV12);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    ASSERT_TRUE(SUCCEEDED(owner->device->CreateTexture2D(&desc, nullptr, &staging)));
    {
        std::lock_guard lock(owner->mutex);
        owner->context->CopyResource(staging.Get(), nv12.native_handle->d3d11_texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ASSERT_TRUE(SUCCEEDED(owner->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
        EXPECT_NEAR(bytes[100 * mapped.RowPitch + 160], 81, 4); // Limited BT.601 red.
        EXPECT_NEAR(bytes[100 * mapped.RowPitch + 10], 16, 4); // Letterbox stays black.
        owner->context->Unmap(staging.Get(), 0);
    }
    source.data.clear(); source.row_pitch = 0;
    NavigationThumbnailPreparer preparer;
    NavigationThumbnailImage image;
    ASSERT_TRUE(preparer.prepare(source, 320, &image, &error)) << error;
    EXPECT_EQ(image.width, 320U); EXPECT_EQ(image.height, 200U);
    EXPECT_EQ(image.gpu_readback_bytes, 320U * 200U * 4U);
    EXPECT_NEAR(image.bgra[(100 * 320 + 10) * 4 + 2], 255, 3); // Entire display, no video crop.
    EXPECT_NEAR(image.bgra[(100 * 320 + 10) * 4], 0, 3);
    EXPECT_FALSE(scaler.scale(source, 319, 200, DXGI_FORMAT_NV12, &nv12, &error));
}

TEST(GpuInputHardware, CodecUsesGpuOrLatchesCpuFallbackAcrossResizeAndRecovery) {
    auto owner = create_device(D3D_DRIVER_TYPE_HARDWARE);
    ASSERT_TRUE(owner);
    auto source = make_frame(owner);
    ASSERT_TRUE(source.native_handle);
    EncoderProfileRequest request;
    request.width = 320; request.height = 200; request.fps = 30;
    EncoderBackendBridgeRequest bridge;
    bridge.capture_adapter_vendor = detect_captured_frame_adapter_vendor(source);
    bridge.allow_hardware_frame_input = true;
    EncoderExecutionSession encoder;
    EncoderBackendBridgePlan plan;
    std::string error;
    ASSERT_TRUE(start_encoder_execution_from_bridge(request, bridge, &encoder, &plan, &error)) << error;
    redclaw::render::FfmpegVideoFrameDecoder decoder;
    decoder.force_software_decode();
    std::uint32_t outputs = 0;
    auto pattern = make_frame(owner);
    std::atomic<std::uint32_t> concurrent_copies = 0, concurrent_thumbnails = 0;
    CapturedFrame thumbnail_source = source;
    thumbnail_source.data.clear(); thumbnail_source.row_pitch = 0;
    auto exercise_context = [&, device = owner, target = source.native_handle,
                                      input = pattern.native_handle, thumbnail_source](std::stop_token stop) {
        NavigationThumbnailPreparer preparer;
        while (!stop.stop_requested()) {
            {
                std::lock_guard lock(device->mutex);
                device->context->CopyResource(target->d3d11_texture.Get(), input->d3d11_texture.Get());
                ++concurrent_copies;
            }
            NavigationThumbnailImage thumbnail;
            if (preparer.prepare(thumbnail_source, 320, &thumbnail)) ++concurrent_thumbnails;
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Never hold the device lock here.
        }
    };
    std::jthread concurrent_capture{exercise_context};
    auto encode_frames = [&] {
        for (int i = 0; i < 12; ++i) {
            EncodedFramePacket packet;
            if (!encoder.encode_bgra_frame(source, 1000 + outputs * 33, &packet, &error)) {
                EXPECT_EQ(encoder.diagnostics().last_failure, EncoderExecutionFailureCategory::kOutputNotReady) << error;
                continue;
            }
            bool ready = false;
            redclaw::render::DecodedVideoFrame decoded;
            EXPECT_TRUE(decoder.decode_frame_view(redclaw::render::EncodedVideoCodec::kH264,
                request.width, request.height, packet.timestamp_ms, packet.keyframe,
                packet.payload.data(), packet.payload.size(), &ready, &decoded, &error)) << error;
            EXPECT_TRUE(ready);
            ++outputs;
        }
    };
    encode_frames();
    ASSERT_GT(outputs, 0U);
    const auto first = encoder.diagnostics();
    EXPECT_TRUE(first.hardware_frame_input_confirmed || first.requested_capture_delivery == CaptureFrameDelivery::kCpu);
    if (first.hardware_frame_input_confirmed) {
        source.data.clear(); source.row_pitch = 0;
        const auto before_gpu_only = outputs;
        encode_frames();
        EXPECT_GT(outputs, before_gpu_only);
        EXPECT_TRUE(encoder.diagnostics().hardware_frame_input_confirmed);
    }
    concurrent_capture.request_stop(); concurrent_capture.join();
    EXPECT_GT(concurrent_copies.load(), 0U); EXPECT_GT(concurrent_thumbnails.load(), 0U);
    if (first.hardware_frame_input_confirmed) {
        auto broken = make_frame(owner);
        broken.native_handle->d3d11_texture.Reset();
        EncodedFramePacket rejected;
        EXPECT_FALSE(encoder.encode_bgra_frame(broken, 2500, &rejected, &error));
        EXPECT_EQ(encoder.diagnostics().last_failure, EncoderExecutionFailureCategory::kOutputNotReady);
        EXPECT_EQ(encoder.diagnostics().requested_capture_delivery, CaptureFrameDelivery::kCpu);
        source = make_frame(owner); // The next real frame provides the CPU fallback.
        decoder.reset(); encode_frames();
        EXPECT_FALSE(encoder.diagnostics().hardware_frame_input_confirmed);
        EXPECT_EQ(encoder.diagnostics().hardware_input_fallback_count, 1U);
    }
    const auto attempts = encoder.diagnostics().hardware_input_attempt_count;
    const auto blocked = encoder.diagnostics().hardware_input_fallback_count != 0;
    const auto blocked_reason = encoder.diagnostics().hardware_input_block_reason;
    request.width = 256; request.height = 160;
    source = make_frame(owner);
    ASSERT_TRUE(start_encoder_execution_from_bridge(request, bridge, &encoder, &plan, &error)) << error;
    decoder.reset(); encode_frames();
    if (blocked) {
        EXPECT_EQ(encoder.diagnostics().hardware_input_attempt_count, attempts);
        EXPECT_EQ(encoder.diagnostics().hardware_input_block_reason, blocked_reason);
        EXPECT_FALSE(blocked_reason.empty());
    }
    owner = create_device(D3D_DRIVER_TYPE_HARDWARE); // Old encoder still owns old device.
    ASSERT_TRUE(owner); source = make_frame(owner, 2);
    decoder.reset(); encode_frames();
    RecordProperty("gpu_confirmed", encoder.diagnostics().hardware_frame_input_confirmed);
    RecordProperty("gpu_attempts", std::to_string(encoder.diagnostics().hardware_input_attempt_count));
    RecordProperty("gpu_fallbacks", std::to_string(encoder.diagnostics().hardware_input_fallback_count));
    RecordProperty("concurrent_copies", concurrent_copies.load());
    RecordProperty("concurrent_thumbnails", concurrent_thumbnails.load());
    encoder.stop();
}

class GpuInputRealCapture : public ::testing::TestWithParam<CaptureBackendType> {};
TEST_P(GpuInputRealCapture, ConfirmedFramesAvoidDesktopReadbackAcrossRestart) {
    WindowsCaptureSession capture;
    CaptureSessionConfig config;
    config.preferred_backend = GetParam(); config.fallback_enabled = false;
    config.frame_delivery = CaptureFrameDelivery::kCpu;
    config.frame_acquire_timeout_ms = 100;
    EncoderExecutionSession encoder;
    NavigationThumbnailPreparer thumbnail;
    std::string error;
    std::uint64_t gpu_frames = 0, thumbnail_bytes = 0;
    std::uint64_t last_generation = 0;
    bool started = false;
    for (int reconnect = 0; reconnect < 3; ++reconnect) {
        ASSERT_TRUE(capture.start(config, &error)) << error;
        std::shared_ptr<CapturedFrame> pending, retained;
        auto readbacks_after_confirmation = capture.telemetry().gpu_readback_count;
        bool confirmed = false;
        unsigned frames = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (frames < 8 && std::chrono::steady_clock::now() < deadline) {
            auto frame = capture.acquireFrame();
            ASSERT_TRUE(frame);
            if (!capture.captureFrame(frame.get(), &error)) {
                ASSERT_EQ(error, "timeout waiting for desktop frame");
                continue;
            }
            if (!started) {
                EncoderProfileRequest request;
                request.width = 640; request.height = 360; request.fps = 30;
                EncoderBackendBridgeRequest bridge;
                bridge.capture_adapter_vendor = detect_captured_frame_adapter_vendor(*frame);
                EncoderBackendBridgePlan plan;
                ASSERT_TRUE(start_encoder_execution_from_bridge(request, bridge, &encoder, &plan, &error)) << error;
                started = true;
                RecordProperty("encoder", encoder.diagnostics().encoder_name);
            }
            EncodedFramePacket packet;
            const bool output = encoder.encode_bgra_frame(*frame, 1000 + frames * 33, &packet, &error);
            if (!output) ASSERT_EQ(encoder.diagnostics().last_failure, EncoderExecutionFailureCategory::kOutputNotReady) << error;
            if (confirmed) {
                ASSERT_TRUE(frame->native_handle);
                EXPECT_TRUE(frame->data.empty());
                EXPECT_EQ(capture.telemetry().gpu_readback_count, readbacks_after_confirmation);
                NavigationThumbnailImage image;
                ASSERT_TRUE(thumbnail.prepare(*frame, 320, &image, &error)) << error;
                EXPECT_LE(image.gpu_readback_bytes, 320U * 320U * 4U);
                EXPECT_GT(image.gpu_readback_bytes, 0U);
                thumbnail_bytes += image.gpu_readback_bytes; ++gpu_frames;
            }
            if (!confirmed && output && encoder.diagnostics().hardware_frame_input_confirmed) {
                EXPECT_TRUE(packet.keyframe);
                EXPECT_GT(frame->capture_generation, last_generation);
                last_generation = frame->capture_generation;
                confirmed = true;
                readbacks_after_confirmation = capture.telemetry().gpu_readback_count;
            }
            capture.configureFrameDelivery(encoder.diagnostics().requested_capture_delivery);
            retained = std::move(pending); pending = std::move(frame);
            ++frames;
        }
        ASSERT_TRUE(confirmed) << encoder.diagnostics().hardware_input_block_reason;
        ASSERT_GE(frames, 2U) << "A changed real desktop frame is required after confirmation";
        // Retained textures and the encoder outlive capture's device owner here.
        capture.stop();
    }
    EXPECT_GT(gpu_frames, 0U);
    RecordProperty("gpu_frames_without_main_readback", std::to_string(gpu_frames));
    RecordProperty("thumbnail_readback_bytes", std::to_string(thumbnail_bytes));
    RecordProperty("device_generations", 3);
    encoder.stop();
}
INSTANTIATE_TEST_SUITE_P(RealDesktop, GpuInputRealCapture,
    ::testing::Values(CaptureBackendType::kWindowsGraphicsCapture, CaptureBackendType::kDesktopDuplication));
#endif
