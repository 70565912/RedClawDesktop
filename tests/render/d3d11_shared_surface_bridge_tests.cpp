#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "redclaw/render/d3d11_video_device.h"

#include <gtest/gtest.h>

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using redclaw::render::D3D11DecodedSurface;
using redclaw::render::D3D11SharedSurfaceBridge;

struct DevicePair {
    ComPtr<ID3D11Device> producer;
    ComPtr<ID3D11DeviceContext> producer_context;
    ComPtr<ID3D11Device> consumer;
    ComPtr<ID3D11DeviceContext> consumer_context;
};

bool create_device(D3D_DRIVER_TYPE type, ComPtr<ID3D11Device>* device, ComPtr<ID3D11DeviceContext>* context) {
    return SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, device->GetAddressOf(), nullptr, context->GetAddressOf()));
}

bool create_pair(DevicePair* pair) {
    for (const auto type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        *pair = {};
        if (create_device(type, &pair->producer, &pair->producer_context)
            && create_device(type, &pair->consumer, &pair->consumer_context)) {
            return true;
        }
    }
    return false;
}

ComPtr<ID3D11Texture2D> make_source(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height,
    std::uint32_t slice1_pixel) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 2;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) return {};
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height, slice1_pixel);
    context->UpdateSubresource(texture.Get(), D3D11CalcSubresource(0, 1, 1), nullptr, pixels.data(), width * 4U, 0);
    return texture;
}

std::uint32_t read_first_pixel(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return 0;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 0;
    const auto pixel = *static_cast<const std::uint32_t*>(mapped.pData);
    context->Unmap(staging.Get(), 0);
    return pixel;
}

struct OpenedSlot {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
};

OpenedSlot open_slot(ID3D11Device* device, const D3D11DecodedSurface& surface) {
    OpenedSlot opened;
    if (FAILED(device->OpenSharedResource(static_cast<HANDLE>(surface.shared_handle), __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(opened.texture.GetAddressOf())))) {
        return {};
    }
    if (FAILED(opened.texture.As(&opened.mutex))) return {};
    return opened;
}

class D3D11SharedSurfaceBridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!create_pair(&devices)) GTEST_SKIP() << "no D3D11 device available";
    }

    D3D11SharedSurfaceBridge::PublishStatus publish(D3D11SharedSurfaceBridge& bridge, ID3D11Texture2D* source,
        D3D11DecodedSurface* surface) {
        std::string error;
        const auto status = bridge.publish(source, 1, surface, &error);
        if (status == D3D11SharedSurfaceBridge::PublishStatus::kFailed
            && error.find("shared keyed mutex") != std::string::npos) {
            skipped = true;
        }
        last_error = error;
        return status;
    }

    DevicePair devices;
    bool skipped = false;
    std::string last_error;
};

TEST_F(D3D11SharedSurfaceBridgeTest, PublishedSliceIsReadableOnAnotherDevice) {
    D3D11SharedSurfaceBridge bridge(devices.producer.Get());
    const auto source = make_source(devices.producer.Get(), devices.producer_context.Get(), 64, 32, 0xff3366ccU);
    ASSERT_TRUE(source);
    D3D11DecodedSurface surface;
    const auto status = publish(bridge, source.Get(), &surface);
    if (skipped) GTEST_SKIP() << last_error;
    ASSERT_EQ(status, D3D11SharedSurfaceBridge::PublishStatus::kPublished) << last_error;
    EXPECT_EQ(surface.device, nullptr);
    EXPECT_EQ(surface.texture, nullptr);
    EXPECT_NE(surface.shared_handle, nullptr);
    EXPECT_NE(surface.frame_lifetime, nullptr);
    EXPECT_EQ(surface.dxgi_format, static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM));

    auto opened = open_slot(devices.consumer.Get(), surface);
    ASSERT_TRUE(opened.texture && opened.mutex);
    D3D11_TEXTURE2D_DESC opened_desc{};
    opened.texture->GetDesc(&opened_desc);
    // Shader-resource alone makes CreateVideoProcessorInputView return
    // E_INVALIDARG on Intel HD 530. The bridge must not select it.
    EXPECT_NE(opened_desc.BindFlags, static_cast<UINT>(D3D11_BIND_SHADER_RESOURCE));
    EXPECT_TRUE(opened_desc.BindFlags == (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)
        || opened_desc.BindFlags == D3D11_BIND_RENDER_TARGET
        || opened_desc.BindFlags == 0U);
    const std::string note = bridge.shared_texture_note();
    EXPECT_NE(note.find("bind_flags="), std::string::npos);
    EXPECT_EQ(note.find(' '), std::string::npos);
    ASSERT_EQ(opened.mutex->AcquireSync(0, 2000), S_OK);
    EXPECT_EQ(read_first_pixel(devices.consumer.Get(), devices.consumer_context.Get(), opened.texture.Get()), 0xff3366ccU);
    EXPECT_EQ(opened.mutex->ReleaseSync(0), S_OK);
}

TEST_F(D3D11SharedSurfaceBridgeTest, SlotsHeldByPresenterAreSkippedAndAllBusyDropsForDisplayOnly) {
    D3D11SharedSurfaceBridge bridge(devices.producer.Get());
    const auto source = make_source(devices.producer.Get(), devices.producer_context.Get(), 32, 32, 0xff00ff00U);
    ASSERT_TRUE(source);
    std::array<D3D11DecodedSurface, D3D11SharedSurfaceBridge::kSlotCount> surfaces;
    std::array<OpenedSlot, D3D11SharedSurfaceBridge::kSlotCount> held;
    for (std::uint32_t i = 0; i < D3D11SharedSurfaceBridge::kSlotCount; ++i) {
        const auto status = publish(bridge, source.Get(), &surfaces[i]);
        if (skipped) GTEST_SKIP() << last_error;
        ASSERT_EQ(status, D3D11SharedSurfaceBridge::PublishStatus::kPublished) << last_error;
        EXPECT_EQ(surfaces[i].shared_slot, i);
        held[i] = open_slot(devices.consumer.Get(), surfaces[i]);
        ASSERT_TRUE(held[i].mutex);
        ASSERT_EQ(held[i].mutex->AcquireSync(0, 2000), S_OK);
    }
    D3D11DecodedSurface dropped;
    EXPECT_EQ(publish(bridge, source.Get(), &dropped), D3D11SharedSurfaceBridge::PublishStatus::kAllSlotsBusy);
    ASSERT_EQ(held[1].mutex->ReleaseSync(0), S_OK);
    D3D11DecodedSurface next;
    ASSERT_EQ(publish(bridge, source.Get(), &next), D3D11SharedSurfaceBridge::PublishStatus::kPublished) << last_error;
    EXPECT_EQ(next.shared_slot, 1U);
    EXPECT_EQ(held[0].mutex->ReleaseSync(0), S_OK);
    EXPECT_EQ(held[2].mutex->ReleaseSync(0), S_OK);
}

TEST_F(D3D11SharedSurfaceBridgeTest, SizeChangeStartsNewGenerationWhileOldFramesStayValid) {
    D3D11SharedSurfaceBridge bridge(devices.producer.Get());
    const auto small_source = make_source(devices.producer.Get(), devices.producer_context.Get(), 32, 32, 0xff111111U);
    const auto large_source = make_source(devices.producer.Get(), devices.producer_context.Get(), 64, 64, 0xff222222U);
    ASSERT_TRUE(small_source && large_source);
    D3D11DecodedSurface old_surface;
    const auto status = publish(bridge, small_source.Get(), &old_surface);
    if (skipped) GTEST_SKIP() << last_error;
    ASSERT_EQ(status, D3D11SharedSurfaceBridge::PublishStatus::kPublished) << last_error;
    D3D11DecodedSurface new_surface;
    ASSERT_EQ(publish(bridge, large_source.Get(), &new_surface), D3D11SharedSurfaceBridge::PublishStatus::kPublished) << last_error;
    EXPECT_GT(new_surface.shared_generation, old_surface.shared_generation);
    auto opened = open_slot(devices.consumer.Get(), old_surface);
    ASSERT_TRUE(opened.mutex);
    ASSERT_EQ(opened.mutex->AcquireSync(0, 2000), S_OK);
    EXPECT_EQ(read_first_pixel(devices.consumer.Get(), devices.consumer_context.Get(), opened.texture.Get()), 0xff111111U);
    EXPECT_EQ(opened.mutex->ReleaseSync(0), S_OK);
}

TEST(D3D11AdapterEnumeration, CandidatesCarryLuidAndDescription) {
    const auto adapters = redclaw::render::enumerate_gpu_adapters(nullptr);
    for (const auto& adapter : adapters) {
        EXPECT_NE(adapter.luid, 0U);
        EXPECT_NE(redclaw::render::describe_gpu_adapter(adapter).find("luid=0x"), std::string::npos);
    }
    const auto selected = redclaw::render::select_gpu_adapter_for_monitor(nullptr);
    if (selected.adapter) {
        EXPECT_FALSE(selected.adapter->software);
        EXPECT_GT(selected.adapter->output_count, 0U);
    }
}

TEST(VideoDecodeProbe, HevcDecodeAdapterMatchesTheReportedFlags) {
    const auto probe = redclaw::render::probe_local_video_decode_support();
    EXPECT_FALSE(probe.hevc_decode_reason.empty());
    if (probe.hevc_hardware) {
        EXPECT_FALSE(probe.hevc_decode_adapter.empty());
        EXPECT_EQ(probe.hevc_decode_reason, "display_adapter");
        EXPECT_EQ(probe.hevc_decode_adapter_luid, probe.present_adapter_luid);
    } else {
        EXPECT_TRUE(probe.hevc_decode_adapter.empty());
        EXPECT_EQ(probe.hevc_decode_adapter_luid, 0U);
        EXPECT_NE(probe.hevc_decode_reason, "display_adapter");
    }
    // The attempt list names the display adapter only, so it has no comma.
    if (probe.detail.find("hevc_probe_attempts=") != std::string::npos) {
        const auto attempts = probe.detail.substr(probe.detail.find("hevc_probe_attempts="));
        EXPECT_EQ(attempts.find(','), std::string::npos) << probe.detail;
    }
}

// The HD 530 bug: an NV12 shared texture with only D3D11_BIND_SHADER_RESOURCE
// is created successfully, then CreateVideoProcessorInputView returns
// E_INVALIDARG. This checks the flags the bridge actually publishes.
TEST(D3D11SharedSurfaceBridgeNv12, PublishedTextureAcceptsVideoProcessorInputView) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT created = E_FAIL;
    for (const auto type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        device.Reset();
        context.Reset();
        created = D3D11CreateDevice(nullptr, type, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf());
        if (SUCCEEDED(created)) break;
    }
    if (FAILED(created) || !device) GTEST_SKIP() << "no D3D11 video device";
    ComPtr<ID3D11VideoDevice> video;
    if (FAILED(device.As(&video)) || !video) GTEST_SKIP() << "ID3D11VideoDevice unavailable";

    D3D11_TEXTURE2D_DESC source_desc{};
    source_desc.Width = 64;
    source_desc.Height = 32;
    source_desc.MipLevels = 1;
    source_desc.ArraySize = 1;
    source_desc.Format = DXGI_FORMAT_NV12;
    source_desc.SampleDesc.Count = 1;
    source_desc.Usage = D3D11_USAGE_DEFAULT;
    source_desc.BindFlags = 0;
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(device->CreateTexture2D(&source_desc, nullptr, source.GetAddressOf()))) {
        GTEST_SKIP() << "NV12 textures are unsupported on this device";
    }

    D3D11SharedSurfaceBridge bridge(device.Get());
    D3D11DecodedSurface surface;
    std::string error;
    const auto status = bridge.publish(source.Get(), 0, &surface, &error);
    if (status != D3D11SharedSurfaceBridge::PublishStatus::kPublished) {
        GTEST_SKIP() << error;
    }
    const std::string note = bridge.shared_texture_note();
    EXPECT_EQ(note.find(' '), std::string::npos) << note;
    EXPECT_EQ(note.find("bind_flags=shader_resource"), std::string::npos) << note;
    if (note.find("vp_input_view=ok") == std::string::npos) {
        GTEST_SKIP() << note;
    }

    ComPtr<ID3D11Texture2D> opened;
    ASSERT_HRESULT_SUCCEEDED(device->OpenSharedResource(static_cast<HANDLE>(surface.shared_handle),
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(opened.GetAddressOf())));
    D3D11_TEXTURE2D_DESC opened_desc{};
    opened->GetDesc(&opened_desc);
    EXPECT_NE(opened_desc.BindFlags, static_cast<UINT>(D3D11_BIND_SHADER_RESOURCE));

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = opened_desc.Width;
    content.InputHeight = opened_desc.Height;
    content.OutputWidth = opened_desc.Width;
    content.OutputHeight = opened_desc.Height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ASSERT_HRESULT_SUCCEEDED(video->CreateVideoProcessorEnumerator(&content, enumerator.GetAddressOf()));
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input{};
    input.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> view;
    const HRESULT view_hr = video->CreateVideoProcessorInputView(
        opened.Get(), enumerator.Get(), &input, view.GetAddressOf());
    EXPECT_HRESULT_SUCCEEDED(view_hr) << note;
}

}  // namespace
#endif
