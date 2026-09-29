#include "redclaw/capture/navigation_thumbnail.h"
#include "capture_d3d11.h"
#include <algorithm>
#include <array>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace redclaw::capture {
namespace {
bool fail(std::string* error, const char* detail) {
    if (error) *error = detail;
    return false;
}
struct SampleAxis { std::uint32_t first, second, weight; };
SampleAxis sample_axis(std::uint32_t index, std::uint32_t source, std::uint32_t target) {
    // Pixel centers, with 8-bit interpolation weights and clamped edge samples.
    const auto position = (static_cast<std::uint64_t>(index) * 2 + 1) * source * 128 / target;
    const auto fixed = position > 128 ? position - 128 : 0;
    const auto first = (std::min)(static_cast<std::uint32_t>(fixed / 256), source - 1);
    return {first, (std::min)(first + 1, source - 1), static_cast<std::uint32_t>(fixed % 256)};
}
}

bool prepare_navigation_thumbnail(const CapturedFrame& source, std::uint32_t max_edge,
                                  NavigationThumbnailImage* image, std::string* error) {
    if (!image || max_edge < 64 || max_edge > 320 || !source.bgra || !source.width || !source.height
        || source.row_pitch < static_cast<std::uint64_t>(source.width) * 4
        || source.data.size() < static_cast<std::uint64_t>(source.row_pitch) * source.height) {
        return fail(error, "navigation thumbnail requires CPU BGRA pixels and a 64..320 bound");
    }
    const auto longest = (std::max)(source.width, source.height);
    image->gpu_readback_bytes = 0;
    image->width = longest <= max_edge ? source.width
        : (std::max)(1U, static_cast<std::uint32_t>(static_cast<std::uint64_t>(source.width) * max_edge / longest));
    image->height = longest <= max_edge ? source.height
        : (std::max)(1U, static_cast<std::uint32_t>(static_cast<std::uint64_t>(source.height) * max_edge / longest));
    image->bgra.resize(static_cast<std::size_t>(image->width) * image->height * 4);
    std::array<SampleAxis, 320> horizontal{};
    for (std::uint32_t x = 0; x < image->width; ++x) horizontal[x] = sample_axis(x, source.width, image->width);
    for (std::uint32_t y = 0; y < image->height; ++y) {
        const auto sy = sample_axis(y, source.height, image->height);
        const auto* top = source.data.data() + static_cast<std::size_t>(sy.first) * source.row_pitch;
        const auto* bottom = source.data.data() + static_cast<std::size_t>(sy.second) * source.row_pitch;
        for (std::uint32_t x = 0; x < image->width; ++x) {
            const auto sx = horizontal[x];
            for (std::uint32_t c = 0; c < 4; ++c) {
                const auto a = top[sx.first * 4 + c] * (256 - sx.weight) + top[sx.second * 4 + c] * sx.weight;
                const auto b = bottom[sx.first * 4 + c] * (256 - sx.weight) + bottom[sx.second * 4 + c] * sx.weight;
                image->bgra[(static_cast<std::size_t>(y) * image->width + x) * 4 + c] =
                    static_cast<std::uint8_t>((a * (256 - sy.weight) + b * sy.weight + 32768) >> 16);
            }
        }
    }
    if (error) error->clear();
    return true;
}

class NavigationThumbnailPreparer::Impl {
public:
#ifdef _WIN32
    D3D11VideoProcessorScaler scaler;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    std::shared_ptr<D3D11CaptureDevice> device;
    std::uint32_t width = 0, height = 0;
#endif
    bool prepare(const CapturedFrame& source, std::uint32_t max_edge,
                 NavigationThumbnailImage* image, std::string* error) {
        if (!source.data.empty()) return prepare_navigation_thumbnail(source, max_edge, image, error);
#ifdef _WIN32
        const auto* native = unwrap_d3d11_native_handle(source);
        if (!image || !native || !native->owner || !source.width || !source.height
            || max_edge < 64 || max_edge > 320) return fail(error, "invalid GPU thumbnail input");
        const auto longest = (std::max)(source.width, source.height);
        const auto w = (std::max)(2U, static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(source.width) * (std::min)(longest, max_edge) / longest));
        const auto h = (std::max)(2U, static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(source.height) * (std::min)(longest, max_edge) / longest));
        // Navigation shows the complete display, independent of the video crop.
        CapturedFrame entire_display;
        entire_display.width = source.width; entire_display.height = source.height;
        entire_display.bgra = true;
        entire_display.native_handle_type = source.native_handle_type;
        entire_display.native_handle = source.native_handle;
        CapturedFrame reduced;
        if (!scaler.scale(entire_display, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, &reduced, error)) return false;
        std::lock_guard lock(native->owner->mutex);
        if (device != native->owner || width != w || height != h) {
            staging.Reset(); device = native->owner; width = w; height = h;
        }
        if (!staging) {
            D3D11_TEXTURE2D_DESC desc{};
            reduced.native_handle->d3d11_texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
            if (FAILED(device->device->CreateTexture2D(&desc, nullptr, &staging))) {
                return fail(error, "small thumbnail staging allocation failed");
            }
        }
        image->width = w; image->height = h;
        image->gpu_readback_bytes = 0;
        // Allocate before Map so an exception cannot leave the resource mapped.
        image->bgra.resize(static_cast<std::size_t>(w) * h * 4);
        device->context->CopyResource(staging.Get(), reduced.native_handle->d3d11_texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(device->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return fail(error, "small thumbnail readback failed");
        }
        for (std::uint32_t y = 0; y < h; ++y) {
            std::memcpy(image->bgra.data() + static_cast<std::size_t>(y) * w * 4,
                static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, w * 4);
        }
        device->context->Unmap(staging.Get(), 0);
        image->gpu_readback_bytes = image->bgra.size();
        if (error) error->clear();
        return true;
#else
        return fail(error, "GPU thumbnail preparation unavailable");
#endif
    }
};
NavigationThumbnailPreparer::NavigationThumbnailPreparer() : impl_(std::make_unique<Impl>()) {}
NavigationThumbnailPreparer::~NavigationThumbnailPreparer() = default;
bool NavigationThumbnailPreparer::prepare(const CapturedFrame& frame, std::uint32_t max_edge,
                                          NavigationThumbnailImage* image, std::string* error) {
    return impl_->prepare(frame, max_edge, image, error);
}

class NavigationThumbnailEncoder::Impl {
public:
#ifdef _WIN32
    using Factory = Microsoft::WRL::ComPtr<IWICImagingFactory>;
    bool com_initialized = false;
    Factory factory;
    Microsoft::WRL::ComPtr<IStream> stream;
    ~Impl() {
        stream.Reset(); factory.Reset();
        if (com_initialized) CoUninitialize();
    }
    bool initialize(std::string* error) {
        if (!factory) {
            if (!com_initialized) {
                const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                com_initialized = SUCCEEDED(hr);
                if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return fail(error, "thumbnail COM initialization failed");
            }
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory)))) return fail(error, "thumbnail WIC factory failed");
        }
        if (!stream && FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
            return fail(error, "thumbnail stream creation failed");
        }
        return true;
    }
#endif
    bool encode(const NavigationThumbnailImage& image, std::vector<std::uint8_t>* jpeg, std::string* error) {
        if (!jpeg || !image.width || !image.height || image.width > 320 || image.height > 320
            || image.bgra.size() != static_cast<std::size_t>(image.width) * image.height * 4) {
            return fail(error, "thumbnail input is outside its 320-pixel bound");
        }
        jpeg->clear();
#ifdef _WIN32
        using Microsoft::WRL::ComPtr;
        if (!initialize(error)) return false;
        HRESULT hr = stream->SetSize(ULARGE_INTEGER{});
        if (SUCCEEDED(hr)) hr = stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr);
        ComPtr<IWICBitmap> bitmap;
        if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromMemory(image.width, image.height,
            GUID_WICPixelFormat32bppBGRA, image.width * 4, static_cast<UINT>(image.bgra.size()),
            const_cast<BYTE*>(image.bgra.data()), &bitmap);
        ComPtr<IWICBitmapEncoder> encoder;
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
        if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
        if (SUCCEEDED(hr)) hr = frame->SetSize(image.width, image.height);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr)) hr = frame->WriteSource(bitmap.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        STATSTG stat{};
        if (SUCCEEDED(hr)) hr = stream->Stat(&stat, STATFLAG_NONAME);
        if (FAILED(hr) || stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > 512U * 1024U) {
            return fail(error, "thumbnail WIC JPEG encoding failed");
        }
        jpeg->resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
        hr = stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr);
        ULONG read = 0;
        if (SUCCEEDED(hr)) hr = stream->Read(jpeg->data(), static_cast<ULONG>(jpeg->size()), &read);
        if (FAILED(hr) || read != jpeg->size()) return fail(error, "thumbnail JPEG stream read failed");
        if (error) error->clear();
        return true;
#else
        return fail(error, "navigation thumbnails require Windows");
#endif
    }
};
NavigationThumbnailEncoder::NavigationThumbnailEncoder() : impl_(std::make_unique<Impl>()) {}
NavigationThumbnailEncoder::~NavigationThumbnailEncoder() = default;
bool NavigationThumbnailEncoder::encode(const NavigationThumbnailImage& image,
                                        std::vector<std::uint8_t>* jpeg, std::string* error) {
    return impl_->encode(image, jpeg, error);
}
} // namespace redclaw::capture
