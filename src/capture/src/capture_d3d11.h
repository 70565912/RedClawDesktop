#pragma once
#include "redclaw/capture/capture_module.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include <mutex>

namespace redclaw::capture {
// One owner per capture-device lifetime. Textures and FFmpeg retain this owner
// after capture recovery/stop; every immediate/video-context user shares mutex.
struct D3D11CaptureDevice {
    std::recursive_mutex mutex;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    static std::shared_ptr<D3D11CaptureDevice> create(ID3D11Device* device);
};
struct CapturedFrameNativeHandle {
    std::shared_ptr<D3D11CaptureDevice> owner;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11_texture;
    DXGI_FORMAT d3d11_format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t d3d11_subresource_index = 0;
};
CapturedFrameNativeHandle* unwrap_d3d11_native_handle(const CapturedFrame& frame);
std::shared_ptr<CapturedFrameNativeHandle> make_d3d11_native_handle(
    std::shared_ptr<D3D11CaptureDevice> owner, ID3D11Texture2D* texture,
    DXGI_FORMAT format, std::uint32_t subresource_index);

// Physical allocation only; QSV crop and codec dimensions remain visible-sized.
D3D11_TEXTURE2D_DESC describe_d3d11_encoder_pool(
    std::uint32_t width, std::uint32_t height, DXGI_FORMAT format, bool qsv);

// Owned by one caller thread. Context access is shared with capture and FFmpeg.
class D3D11VideoProcessorScaler {
public:
    D3D11VideoProcessorScaler();
    ~D3D11VideoProcessorScaler();
    bool scale(const CapturedFrame& source, std::uint32_t width, std::uint32_t height,
               DXGI_FORMAT format, CapturedFrame* output, std::string* error);
    void reset();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
