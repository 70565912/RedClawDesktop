#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "redclaw/render/d3d11_video_device.h"

#include <array>
#include <cstdio>

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace redclaw::render {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kMicrosoftBasicRenderVendor = 0x1414;

std::uint64_t pack_luid(const LUID& luid) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32U)
        | static_cast<std::uint64_t>(luid.LowPart);
}

std::string utf8_from_wide(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string out(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string hresult_text(const char* what, HRESULT hr) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s failed: 0x%08lx", what, static_cast<unsigned long>(hr));
    return buffer;
}

ComPtr<IDXGIAdapter1> find_adapter(std::uint64_t luid) {
    ComPtr<IDXGIFactory1> factory;
    if (luid == 0 || FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) {
        return {};
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && pack_luid(desc.AdapterLuid) == luid) return adapter;
    }
    return {};
}

}  // namespace

std::vector<GpuAdapterCandidate> enumerate_gpu_adapters(void* target_monitor) {
    HMONITOR monitor = static_cast<HMONITOR>(target_monitor);
    if (monitor == nullptr) monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    std::vector<GpuAdapterCandidate> candidates;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) {
        return candidates;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        GpuAdapterCandidate candidate;
        candidate.enum_index = index;
        candidate.luid = pack_luid(desc.AdapterLuid);
        candidate.name = utf8_from_wide(desc.Description);
        candidate.vendor_id = desc.VendorId;
        candidate.device_id = desc.DeviceId;
        candidate.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0
            || desc.VendorId == kMicrosoftBasicRenderVendor;
        ComPtr<IDXGIOutput> output;
        for (UINT output_index = 0; adapter->EnumOutputs(output_index, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
             ++output_index) {
            ++candidate.output_count;
            DXGI_OUTPUT_DESC output_desc{};
            if (monitor != nullptr && SUCCEEDED(output->GetDesc(&output_desc)) && output_desc.Monitor == monitor) {
                candidate.owns_target_monitor = true;
            }
        }
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

SelectedGpuAdapter select_gpu_adapter_for_monitor(void* target_monitor) {
    const auto candidates = enumerate_gpu_adapters(target_monitor);
    const auto choice = choose_gpu_adapter(candidates);
    if (!choice.index) return {std::nullopt, choice.reason};
    return {candidates[*choice.index], choice.reason};
}

ID3D11Device* create_d3d11_video_device(std::uint64_t adapter_luid, std::string* error_detail, bool debug_layer) {
    const auto adapter = find_adapter(adapter_luid);
    if (adapter_luid != 0 && !adapter) {
        if (error_detail) *error_detail = "requested D3D11 adapter LUID is no longer present";
        return nullptr;
    }
    constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT
        | (debug_layer ? static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG) : 0U);
    HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags,
        kFeatureLevels, static_cast<UINT>(std::size(kFeatureLevels)), D3D11_SDK_VERSION,
        &device, &level, &context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, flags,
            kFeatureLevels + 1, static_cast<UINT>(std::size(kFeatureLevels) - 1), D3D11_SDK_VERSION,
            &device, &level, &context);
    }
    if (FAILED(hr) || !device || !context) {
        if (error_detail) *error_detail = hresult_text("D3D11CreateDevice(decode)", hr);
        return nullptr;
    }
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context.As(&multithread)) && multithread) multithread->SetMultithreadProtected(TRUE);
    return device.Detach();
}

std::uint64_t d3d11_device_adapter_luid(ID3D11Device* device) {
    if (device == nullptr) return 0;
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi_device.GetAddressOf())))
        || FAILED(dxgi_device->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc))) {
        return 0;
    }
    return pack_luid(desc.AdapterLuid);
}

namespace {

// Owned jointly by the bridge and every published frame so a presenter can
// finish with a slot after the bridge has moved to a new format or size.
struct SharedSlotSet {
    std::array<ComPtr<ID3D11Texture2D>, D3D11SharedSurfaceBridge::kSlotCount> textures;
    std::array<ComPtr<IDXGIKeyedMutex>, D3D11SharedSurfaceBridge::kSlotCount> mutexes;
    std::array<HANDLE, D3D11SharedSurfaceBridge::kSlotCount> handles{};
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t generation = 0;
};

}  // namespace

class D3D11SharedSurfaceBridge::Impl {
public:
    explicit Impl(ID3D11Device* device) : device_(device) {
        if (device_) device_->GetImmediateContext(&context_);
    }

    PublishStatus publish(ID3D11Texture2D* source, std::uint32_t array_slice,
        D3D11DecodedSurface* output, std::string* error_detail) {
        if (!device_ || !context_ || source == nullptr || output == nullptr) {
            if (error_detail) *error_detail = "shared surface bridge has no device or source";
            return PublishStatus::kFailed;
        }
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        if (array_slice >= source_desc.ArraySize) {
            if (error_detail) *error_detail = "decoded array slice is out of range";
            return PublishStatus::kFailed;
        }
        if (!slots_ || slots_->width != source_desc.Width || slots_->height != source_desc.Height
            || slots_->format != source_desc.Format) {
            if (!create_slots(source_desc, error_detail)) return PublishStatus::kFailed;
        }
        for (std::uint32_t attempt = 0; attempt < kSlotCount; ++attempt) {
            const std::uint32_t slot = (next_slot_ + attempt) % kSlotCount;
            const HRESULT acquire = slots_->mutexes[slot]->AcquireSync(0, 0);
            if (acquire == static_cast<HRESULT>(WAIT_TIMEOUT)) continue;
            if (acquire != S_OK) {
                if (error_detail) *error_detail = hresult_text("IDXGIKeyedMutex::AcquireSync(decode)", acquire);
                return PublishStatus::kFailed;
            }
            context_->CopySubresourceRegion(slots_->textures[slot].Get(), 0, 0, 0, 0, source,
                D3D11CalcSubresource(0, array_slice, source_desc.MipLevels), nullptr);
            const HRESULT release = slots_->mutexes[slot]->ReleaseSync(0);
            if (FAILED(release)) {
                if (error_detail) *error_detail = hresult_text("IDXGIKeyedMutex::ReleaseSync(decode)", release);
                return PublishStatus::kFailed;
            }
            next_slot_ = (slot + 1) % kSlotCount;
            output->device = nullptr;
            output->texture = nullptr;
            output->array_slice = 0;
            output->dxgi_format = static_cast<std::uint32_t>(slots_->format);
            output->shared_handle = slots_->handles[slot];
            output->shared_generation = slots_->generation;
            output->shared_slot = slot;
            output->frame_lifetime = slots_;
            return PublishStatus::kPublished;
        }
        return PublishStatus::kAllSlotsBusy;
    }

private:
    bool create_slots(const D3D11_TEXTURE2D_DESC& source_desc, std::string* error_detail) {
        auto slots = std::make_shared<SharedSlotSet>();
        slots->width = source_desc.Width;
        slots->height = source_desc.Height;
        slots->format = source_desc.Format;
        slots->generation = ++generation_;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = source_desc.Width;
        desc.Height = source_desc.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = source_desc.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &slots->textures[slot]);
            if (FAILED(hr)) {
                // Some drivers reject shader-resource binding for YUV formats.
                desc.BindFlags = 0;
                hr = device_->CreateTexture2D(&desc, nullptr, &slots->textures[slot]);
            }
            if (FAILED(hr)) {
                if (error_detail) *error_detail = hresult_text("CreateTexture2D(shared keyed mutex)", hr)
                    + " format=" + std::to_string(static_cast<unsigned>(desc.Format));
                return false;
            }
            ComPtr<IDXGIResource> resource;
            hr = slots->textures[slot].As(&slots->mutexes[slot]);
            if (SUCCEEDED(hr)) hr = slots->textures[slot].As(&resource);
            if (SUCCEEDED(hr)) hr = resource->GetSharedHandle(&slots->handles[slot]);
            if (FAILED(hr) || slots->handles[slot] == nullptr) {
                if (error_detail) *error_detail = hresult_text("IDXGIResource::GetSharedHandle", hr);
                return false;
            }
        }
        slots_ = std::move(slots);
        next_slot_ = 0;
        return true;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    std::shared_ptr<SharedSlotSet> slots_;
    std::uint32_t next_slot_ = 0;
    std::uint64_t generation_ = 0;
};

D3D11SharedSurfaceBridge::D3D11SharedSurfaceBridge(ID3D11Device* producer_device)
    : impl_(std::make_unique<Impl>(producer_device)) {}

D3D11SharedSurfaceBridge::~D3D11SharedSurfaceBridge() = default;

D3D11SharedSurfaceBridge::PublishStatus D3D11SharedSurfaceBridge::publish(
    ID3D11Texture2D* source, std::uint32_t array_slice, D3D11DecodedSurface* output, std::string* error_detail) {
    return impl_->publish(source, array_slice, output, error_detail);
}

}  // namespace redclaw::render

#endif
