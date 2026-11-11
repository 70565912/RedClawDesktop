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
    const auto small = make_source(devices.producer.Get(), devices.producer_context.Get(), 32, 32, 0xff111111U);
    const auto large = make_source(devices.producer.Get(), devices.producer_context.Get(), 64, 64, 0xff222222U);
    ASSERT_TRUE(small && large);
    D3D11DecodedSurface old_surface;
    const auto status = publish(bridge, small.Get(), &old_surface);
    if (skipped) GTEST_SKIP() << last_error;
    ASSERT_EQ(status, D3D11SharedSurfaceBridge::PublishStatus::kPublished) << last_error;
    D3D11DecodedSurface new_surface;
    ASSERT_EQ(publish(bridge, large.Get(), &new_surface), D3D11SharedSurfaceBridge::PublishStatus::kPublished) << last_error;
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

}  // namespace
#endif
