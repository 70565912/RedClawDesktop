#include "capture_cursor_d3d11.h"
#ifdef _WIN32
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace redclaw::capture {
using Microsoft::WRL::ComPtr;
namespace {
constexpr char kCursorShader[] = R"hlsl(
Texture2D<uint4> pointerPixels : register(t0);
Texture2D<float4> desktopPixels : register(t1);
cbuffer Parameters : register(b0) { uint shapeWidth; uint shapeHeight; uint kind; uint rotation; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);
}
float4 ps(float4 position : SV_Position) : SV_Target {
    uint2 xy = uint2(position.xy);
    uint2 source = xy;
    if (rotation == 90) source = uint2(shapeWidth - 1 - xy.y, xy.x);
    else if (rotation == 180) source = uint2(shapeWidth - 1 - xy.x, shapeHeight - 1 - xy.y);
    else if (rotation == 270) source = uint2(xy.y, shapeHeight - 1 - xy.x);
    uint4 pointer = pointerPixels.Load(int3(source, 0));
    float4 background = desktopPixels.Load(int3(xy, 0));
    uint3 original = uint3(round(saturate(background.rgb) * 255.0));
    if (kind == 1) return float4(float3((original & pointer.x) ^ pointer.y) / 255.0, background.a);
    uint3 color = pointer.zyx;
    if (kind == 4) return float4(float3(pointer.w != 0 ? original ^ color : color) / 255.0, background.a);
    return float4((float3(color) * pointer.w + float3(original) * (255 - pointer.w)) / 65025.0, background.a);
}
)hlsl";
}
void CaptureCursorD3D11::reset() { *this = CaptureCursorD3D11{}; }

HRESULT CaptureCursorD3D11::prepare(ID3D11Device* device, const CaptureCursorShape& shape, std::uint32_t rotation) {
    if (!vertex_shader_) {
        ComPtr<ID3DBlob> vs, ps, errors;
        HRESULT hr = D3DCompile(kCursorShader, sizeof(kCursorShader), nullptr, nullptr, nullptr,
            "vs", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vs, &errors);
        if (FAILED(hr)) { return hr; }
        hr = D3DCompile(kCursorShader, sizeof(kCursorShader), nullptr, nullptr, nullptr,
            "ps", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &ps, &errors);
        if (FAILED(hr)) { return hr; }
        hr = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_shader_);
        if (FAILED(hr)) { return hr; }
        hr = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_shader_);
        if (FAILED(hr)) { reset(); return hr; }
    }
    if (revision_ == shape.revision && rotation_ == rotation && cursor_) { return S_OK; }
    cursor_.Reset(); cursor_view_.Reset(); background_.Reset(); background_view_.Reset();
    output_.Reset(); output_view_.Reset(); constants_.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = shape.width; desc.Height = shape.height;
    desc.MipLevels = 1; desc.ArraySize = 1; desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = shape.pixels.data(); initial.SysMemPitch = shape.width * 4;
    HRESULT hr = device->CreateTexture2D(&desc, &initial, &cursor_);
    if (FAILED(hr)) { return hr; }
    hr = device->CreateShaderResourceView(cursor_.Get(), nullptr, &cursor_view_);
    if (FAILED(hr)) { return hr; }
    if (rotation == 90 || rotation == 270) { std::swap(desc.Width, desc.Height); }
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.Usage = D3D11_USAGE_DEFAULT;
    hr = device->CreateTexture2D(&desc, nullptr, &background_);
    if (FAILED(hr)) { return hr; }
    hr = device->CreateShaderResourceView(background_.Get(), nullptr, &background_view_);
    if (FAILED(hr)) { return hr; }
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = device->CreateTexture2D(&desc, nullptr, &output_);
    if (FAILED(hr)) { return hr; }
    hr = device->CreateRenderTargetView(output_.Get(), nullptr, &output_view_);
    if (FAILED(hr)) { return hr; }
    const std::uint32_t parameters[] = {shape.width, shape.height, static_cast<std::uint32_t>(shape.kind), rotation};
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(parameters); buffer.Usage = D3D11_USAGE_IMMUTABLE;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    initial = {}; initial.pSysMem = parameters;
    hr = device->CreateBuffer(&buffer, &initial, &constants_);
    if (FAILED(hr)) { return hr; }
    revision_ = shape.revision; rotation_ = rotation;
    return S_OK;
}

HRESULT CaptureCursorD3D11::composite(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* frame, const CaptureCursorShape& shape, CaptureCursorPlacement p) {
    if (!p.visible || !shape.width || !shape.height) { return S_OK; }
    D3D11_TEXTURE2D_DESC desc{}; frame->GetDesc(&desc);
    const auto rect = capture_cursor_rect(shape, p, desc.Width, desc.Height);
    const auto left = std::max<std::int64_t>(0, rect.x);
    const auto top = std::max<std::int64_t>(0, rect.y);
    const auto right = std::min<std::int64_t>(desc.Width, static_cast<std::int64_t>(rect.x) + rect.width);
    const auto bottom = std::min<std::int64_t>(desc.Height, static_cast<std::int64_t>(rect.y) + rect.height);
    if (right <= left || bottom <= top) { return S_OK; }
    HRESULT hr = prepare(device, shape, p.rotation);
    if (FAILED(hr)) { return hr; }
    const auto dx = static_cast<UINT>(left - rect.x), dy = static_cast<UINT>(top - rect.y);
    D3D11_BOX box{static_cast<UINT>(left), static_cast<UINT>(top), 0,
        static_cast<UINT>(right), static_cast<UINT>(bottom), 1};
    context->CopySubresourceRegion(background_.Get(), 0, dx, dy, 0, frame, 0, &box);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(rect.width), static_cast<float>(rect.height), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    ID3D11ShaderResourceView* views[] = {cursor_view_.Get(), background_view_.Get()};
    context->PSSetShaderResources(0, 2, views);
    context->OMSetRenderTargets(1, output_view_.GetAddressOf(), nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
    context->OMSetDepthStencilState(nullptr, 0);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* unbound[] = {nullptr, nullptr};
    context->PSSetShaderResources(0, 2, unbound);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    box = {dx, dy, 0, dx + static_cast<UINT>(right - left), dy + static_cast<UINT>(bottom - top), 1};
    context->CopySubresourceRegion(frame, 0, static_cast<UINT>(left), static_cast<UINT>(top), 0, output_.Get(), 0, &box);
    return device->GetDeviceRemovedReason();
}
} // namespace redclaw::capture
#endif
