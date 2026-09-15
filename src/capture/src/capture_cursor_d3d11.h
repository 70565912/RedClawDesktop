#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "redclaw/capture/capture_cursor.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace redclaw::capture {
// Scratch textures cover only the pointer rectangle. The encoder's existing
// frame texture retains BindFlags == 0; no full-frame readback or extra copy.
class CaptureCursorD3D11 {
public:
    HRESULT composite(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame,
        const CaptureCursorShape& shape, CaptureCursorPlacement placement);
    void reset();
private:
    HRESULT prepare(ID3D11Device* device, const CaptureCursorShape& shape, std::uint32_t rotation);
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cursor_, background_, output_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursor_view_, background_view_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_view_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    std::uint64_t revision_ = 0;
    std::uint32_t rotation_ = 0;
};
}
#endif
