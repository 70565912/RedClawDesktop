#include "redclaw/capture/capture_module.h"

#include <chrono>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>
#endif

#if __has_include(<libavcodec/avcodec.h>) && __has_include(<libavutil/buffer.h>) && __has_include(<libavutil/error.h>) && __has_include(<libavutil/frame.h>) && __has_include(<libavutil/hwcontext.h>) && __has_include(<libavutil/opt.h>)
#define REDCLAW_CAPTURE_HAS_LIBAVCODEC 1
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}
#else
#define REDCLAW_CAPTURE_HAS_LIBAVCODEC 0
#endif

#if defined(_WIN32) && REDCLAW_CAPTURE_HAS_LIBAVCODEC && __has_include(<libavutil/hwcontext_d3d11va.h>)
#define REDCLAW_CAPTURE_HAS_D3D11_HWCONTEXT 1
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#else
#define REDCLAW_CAPTURE_HAS_D3D11_HWCONTEXT 0
#endif

#if __has_include(<libswscale/swscale.h>)
#define REDCLAW_CAPTURE_HAS_LIBSWSCALE 1
extern "C" {
#include <libswscale/swscale.h>
}
#else
#define REDCLAW_CAPTURE_HAS_LIBSWSCALE 0
#endif

namespace redclaw::capture {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

struct CapturedFrameNativeHandle {
    ComPtr<ID3D11Device> d3d11_device;
    ComPtr<ID3D11Texture2D> d3d11_texture;
    DXGI_FORMAT d3d11_format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t d3d11_subresource_index = 0;
};
#else
struct CapturedFrameNativeHandle {};
#endif

namespace {

void assign_error(std::string value, std::string* error_detail) {
    if (error_detail != nullptr) {
        *error_detail = std::move(value);
    }
}

std::uint32_t clamp_u32(std::uint32_t value, std::uint32_t min_value, std::uint32_t max_value) {
    return std::max(min_value, std::min(value, max_value));
}

bool is_hardware_encoder_backend(EncoderBackendType backend) {
    return backend == EncoderBackendType::kNvenc
        || backend == EncoderBackendType::kQuickSync
        || backend == EncoderBackendType::kAmf;
}

bool captured_frame_has_cpu_bgra_pixels(const CapturedFrame& frame) {
    if (!frame.bgra || frame.width == 0 || frame.height == 0) {
        return false;
    }

    const std::size_t min_row_bytes = static_cast<std::size_t>(frame.width) * 4U;
    if (frame.row_pitch < min_row_bytes) {
        return false;
    }

    const std::size_t min_frame_bytes = static_cast<std::size_t>(frame.row_pitch)
        * static_cast<std::size_t>(frame.height);
    return frame.data.size() >= min_frame_bytes;
}

#ifdef _WIN32
CapturedFrameNativeHandle* unwrap_d3d11_native_handle(const CapturedFrame& frame) {
    if (frame.native_handle_type != CapturedFrameNativeHandleType::kD3D11Texture2D
        || frame.native_handle == nullptr) {
        return nullptr;
    }

    return frame.native_handle.get();
}

std::shared_ptr<CapturedFrameNativeHandle> make_d3d11_native_handle(
    ID3D11Device* device,
    ID3D11Texture2D* texture,
    DXGI_FORMAT format,
    std::uint32_t subresource_index) {
    auto handle = std::make_shared<CapturedFrameNativeHandle>();
    handle->d3d11_device = device;
    handle->d3d11_texture = texture;
    handle->d3d11_format = format;
    handle->d3d11_subresource_index = subresource_index;
    return handle;
}

std::string describe_encoder_backend(EncoderBackendType backend);

constexpr std::uint32_t kDxgiVendorIdIntel = 0x8086U;
constexpr std::uint32_t kDxgiVendorIdNvidia = 0x10DEU;
constexpr std::uint32_t kDxgiVendorIdAmd = 0x1002U;
constexpr int kD3D11HardwareFramePoolSize = 32;
constexpr std::size_t kDdaNativeTexturePoolSize = 4;
constexpr std::size_t kWgcNativeTexturePoolSize = 2;
constexpr std::int32_t kWgcFramePoolSize = 2;
constexpr std::uint32_t kMaxWgcFramePoolRecreatesPerCapture = 4;

struct DxgiAdapterIdentity {
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    LUID luid{};
    std::string description;
};

std::string wide_to_utf8(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }

    std::vector<char> buffer(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1, buffer.data(), length, nullptr, nullptr) <= 0) {
        return {};
    }

    return std::string(buffer.data());
}

std::string format_hex_u32(std::uint32_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

std::string format_luid(const LUID& luid) {
    std::ostringstream stream;
    stream << "0x"
           << std::hex
           << std::uppercase
           << static_cast<std::uint32_t>(luid.HighPart)
           << ":"
           << static_cast<std::uint32_t>(luid.LowPart);
    return stream.str();
}

CaptureAdapterVendor classify_dxgi_vendor(std::uint32_t vendor_id) {
    switch (vendor_id) {
    case kDxgiVendorIdIntel:
        return CaptureAdapterVendor::kIntel;
    case kDxgiVendorIdNvidia:
        return CaptureAdapterVendor::kNvidia;
    case kDxgiVendorIdAmd:
        return CaptureAdapterVendor::kAmd;
    default:
        return CaptureAdapterVendor::kUnknown;
    }
}

std::string describe_capture_adapter_vendor(CaptureAdapterVendor vendor) {
    switch (vendor) {
    case CaptureAdapterVendor::kIntel:
        return "Intel";
    case CaptureAdapterVendor::kNvidia:
        return "NVIDIA";
    case CaptureAdapterVendor::kAmd:
        return "AMD";
    case CaptureAdapterVendor::kUnknown:
        return "unknown";
    }

    return "unknown";
}

std::string describe_dxgi_vendor(std::uint32_t vendor_id) {
    return describe_capture_adapter_vendor(classify_dxgi_vendor(vendor_id));
}

std::optional<std::uint32_t> expected_dxgi_vendor_for_backend(EncoderBackendType backend) {
    switch (backend) {
    case EncoderBackendType::kNvenc:
        return kDxgiVendorIdNvidia;
    case EncoderBackendType::kQuickSync:
        return kDxgiVendorIdIntel;
    case EncoderBackendType::kAmf:
        return kDxgiVendorIdAmd;
    case EncoderBackendType::kAuto:
    case EncoderBackendType::kSoftware:
        return std::nullopt;
    }

    return std::nullopt;
}

bool query_d3d11_device_adapter_identity(
    ID3D11Device* device,
    DxgiAdapterIdentity* identity,
    std::string* error_detail) {
    if (device == nullptr || identity == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "D3D11 device adapter query requires non-null inputs";
        }
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) || dxgi_device == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "failed to query IDXGIDevice from D3D11 device";
        }
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter)) || adapter == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "failed to query DXGI adapter from D3D11 device";
        }
        return false;
    }

    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) {
        if (error_detail != nullptr) {
            *error_detail = "failed to read DXGI adapter description";
        }
        return false;
    }

    identity->vendor_id = desc.VendorId;
    identity->device_id = desc.DeviceId;
    identity->luid = desc.AdapterLuid;
    identity->description = wide_to_utf8(desc.Description);
    if (error_detail != nullptr) {
        error_detail->clear();
    }
    return true;
}

std::string format_dxgi_adapter_identity(const DxgiAdapterIdentity& identity) {
    std::ostringstream stream;
    stream << "description=" << (identity.description.empty() ? std::string("unknown") : identity.description)
           << ",vendor=" << describe_dxgi_vendor(identity.vendor_id)
           << "(" << format_hex_u32(identity.vendor_id) << ")"
           << ",device=" << format_hex_u32(identity.device_id)
           << ",luid=" << format_luid(identity.luid);
    return stream.str();
}

bool is_native_frame_backend_vendor_compatible(
    const CapturedFrame& frame,
    EncoderBackendType backend,
    std::string* adapter_summary,
    std::string* block_reason) {
    auto* native = unwrap_d3d11_native_handle(frame);
    if (native == nullptr || native->d3d11_device == nullptr) {
        return true;
    }

    DxgiAdapterIdentity identity;
    std::string adapter_error;
    if (!query_d3d11_device_adapter_identity(native->d3d11_device.Get(), &identity, &adapter_error)) {
        if (adapter_summary != nullptr) {
            *adapter_summary = adapter_error;
        }
        return true;
    }

    if (adapter_summary != nullptr) {
        *adapter_summary = format_dxgi_adapter_identity(identity);
    }

    const auto expected_vendor = expected_dxgi_vendor_for_backend(backend);
    if (!expected_vendor.has_value() || identity.vendor_id == 0 || identity.vendor_id == expected_vendor.value()) {
        return true;
    }

    if (block_reason != nullptr) {
        *block_reason = "skipping D3D11 hardware-frame input for "
            + describe_encoder_backend(backend)
            + " because capture adapter "
            + format_dxgi_adapter_identity(identity)
            + " does not match expected vendor "
            + describe_dxgi_vendor(expected_vendor.value())
            + " (" + format_hex_u32(expected_vendor.value()) + ")";
    }
    return false;
}

class D3D11VideoProcessorScaler final {
public:
    bool scale(
        const CapturedFrame& source,
        std::uint32_t output_width,
        std::uint32_t output_height,
        CapturedFrame* output,
        std::string* error_detail) {
        auto* native = unwrap_d3d11_native_handle(source);
        if (native == nullptr || native->d3d11_device == nullptr
            || native->d3d11_texture == nullptr || output == nullptr) {
            assign_error("D3D11 video processor scaling requires a native texture", error_detail);
            return false;
        }
        if (output_width < 2 || output_height < 2) {
            assign_error("D3D11 video processor output dimensions are invalid", error_detail);
            return false;
        }

        D3D11_TEXTURE2D_DESC source_desc{};
        native->d3d11_texture->GetDesc(&source_desc);
        if (!ensure_pipeline(
                native->d3d11_device.Get(),
                source_desc,
                output_width,
                output_height,
                error_detail)) {
            return false;
        }

        ComPtr<ID3D11VideoProcessorInputView> input_view;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
        input_desc.FourCC = 0;
        input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_desc.Texture2D.MipSlice = 0;
        input_desc.Texture2D.ArraySlice = 0;
        const HRESULT input_view_hr = video_device_->CreateVideoProcessorInputView(
            native->d3d11_texture.Get(),
            enumerator_.Get(),
            &input_desc,
            &input_view);
        if (FAILED(input_view_hr) || input_view == nullptr) {
            assign_error(
                "CreateVideoProcessorInputView failed hr="
                    + format_hex_u32(static_cast<std::uint32_t>(input_view_hr)),
                error_detail);
            return false;
        }

        const CaptureRegion source_region = normalize_capture_region(
            source.source_region,
            source.width,
            source.height);
        const EncoderContentRect content_rect = resolve_capture_region_content_rect(
            source_region,
            output_width,
            output_height);
        if (source_region.width == 0 || source_region.height == 0
            || content_rect.width == 0 || content_rect.height == 0) {
            assign_error("D3D11 video processor crop geometry is invalid", error_detail);
            return false;
        }
        const RECT source_rect{
            static_cast<LONG>(source_region.x),
            static_cast<LONG>(source_region.y),
            static_cast<LONG>(source_region.x + source_region.width),
            static_cast<LONG>(source_region.y + source_region.height)};
        const RECT output_rect{
            0, 0, static_cast<LONG>(output_width), static_cast<LONG>(output_height)};
        const RECT destination_rect{
            static_cast<LONG>(content_rect.x),
            static_cast<LONG>(content_rect.y),
            static_cast<LONG>(content_rect.x + content_rect.width),
            static_cast<LONG>(content_rect.y + content_rect.height)};
        D3D11_VIDEO_COLOR background{};
        background.RGBA.A = 1.0F;
        video_context_->VideoProcessorSetOutputBackgroundColor(
            processor_.Get(), FALSE, &background);
        video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &output_rect);
        video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &source_rect);
        video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &destination_rect);
        video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input_view.Get();
        const HRESULT blit_hr = video_context_->VideoProcessorBlt(
            processor_.Get(), output_view_.Get(), 0, 1, &stream);
        if (FAILED(blit_hr)) {
            assign_error(
                "VideoProcessorBlt failed hr="
                    + format_hex_u32(static_cast<std::uint32_t>(blit_hr)),
                error_detail);
            return false;
        }

        output->width = output_width;
        output->height = output_height;
        output->row_pitch = 0;
        output->bgra = true;
        output->data.clear();
        output->source_region = CaptureRegion{
            .x = 0,
            .y = 0,
            .width = output_width,
            .height = output_height,
            .revision = source_region.revision,
        };
        output->desktop_origin_x = source.desktop_origin_x;
        output->desktop_origin_y = source.desktop_origin_y;
        output->desktop_width = source.desktop_width;
        output->desktop_height = source.desktop_height;
        output->desktop_rotation = source.desktop_rotation;
        output->native_handle_type = CapturedFrameNativeHandleType::kD3D11Texture2D;
        output->native_handle = make_d3d11_native_handle(
            native->d3d11_device.Get(),
            output_texture_.Get(),
            source_desc.Format,
            0);
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    void reset() {
        output_view_.Reset();
        output_texture_.Reset();
        processor_.Reset();
        enumerator_.Reset();
        video_context_.Reset();
        video_device_.Reset();
        device_.Reset();
        source_width_ = 0;
        source_height_ = 0;
        output_width_ = 0;
        output_height_ = 0;
        format_ = DXGI_FORMAT_UNKNOWN;
    }

private:
    bool ensure_pipeline(
        ID3D11Device* device,
        const D3D11_TEXTURE2D_DESC& source_desc,
        std::uint32_t output_width,
        std::uint32_t output_height,
        std::string* error_detail) {
        if (device_.Get() == device
            && source_width_ == source_desc.Width
            && source_height_ == source_desc.Height
            && output_width_ == output_width
            && output_height_ == output_height
            && format_ == source_desc.Format
            && processor_ != nullptr && output_view_ != nullptr) {
            return true;
        }

        reset();
        device_ = device;
        if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&video_device_)))
            || video_device_ == nullptr) {
            assign_error("ID3D11VideoDevice is unavailable", error_detail);
            reset();
            return false;
        }
        ComPtr<ID3D11DeviceContext> immediate_context;
        device_->GetImmediateContext(&immediate_context);
        if (immediate_context == nullptr
            || FAILED(immediate_context.As(&video_context_))
            || video_context_ == nullptr) {
            assign_error("ID3D11VideoContext is unavailable", error_detail);
            reset();
            return false;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {30, 1};
        content.InputWidth = source_desc.Width;
        content.InputHeight = source_desc.Height;
        content.OutputFrameRate = {30, 1};
        content.OutputWidth = output_width;
        content.OutputHeight = output_height;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&content, &enumerator_);
        if (FAILED(hr) || enumerator_ == nullptr) {
            assign_error(
                "CreateVideoProcessorEnumerator failed hr="
                    + format_hex_u32(static_cast<std::uint32_t>(hr)),
                error_detail);
            reset();
            return false;
        }
        hr = video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_);
        if (FAILED(hr) || processor_ == nullptr) {
            assign_error(
                "CreateVideoProcessor failed hr="
                    + format_hex_u32(static_cast<std::uint32_t>(hr)),
                error_detail);
            reset();
            return false;
        }

        D3D11_TEXTURE2D_DESC output_desc{};
        output_desc.Width = output_width;
        output_desc.Height = output_height;
        output_desc.MipLevels = 1;
        output_desc.ArraySize = 1;
        output_desc.Format = source_desc.Format;
        output_desc.SampleDesc.Count = 1;
        output_desc.Usage = D3D11_USAGE_DEFAULT;
        output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = device_->CreateTexture2D(&output_desc, nullptr, &output_texture_);
        if (FAILED(hr) || output_texture_ == nullptr) {
            assign_error(
                "D3D11 video processor output texture creation failed hr="
                    + format_hex_u32(static_cast<std::uint32_t>(hr)),
                error_detail);
            reset();
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc{};
        output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        output_view_desc.Texture2D.MipSlice = 0;
        hr = video_device_->CreateVideoProcessorOutputView(
            output_texture_.Get(), enumerator_.Get(), &output_view_desc, &output_view_);
        if (FAILED(hr) || output_view_ == nullptr) {
            assign_error(
                "CreateVideoProcessorOutputView failed hr="
                    + format_hex_u32(static_cast<std::uint32_t>(hr)),
                error_detail);
            reset();
            return false;
        }

        source_width_ = source_desc.Width;
        source_height_ = source_desc.Height;
        output_width_ = output_width;
        output_height_ = output_height;
        format_ = source_desc.Format;
        return true;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11Texture2D> output_texture_;
    ComPtr<ID3D11VideoProcessorOutputView> output_view_;
    std::uint32_t source_width_ = 0;
    std::uint32_t source_height_ = 0;
    std::uint32_t output_width_ = 0;
    std::uint32_t output_height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};
#else
CapturedFrameNativeHandle* unwrap_d3d11_native_handle(const CapturedFrame&) {
    return nullptr;
}
#endif

bool validate_encoder_request(const EncoderProfileRequest& request, std::string* error_detail) {
    if (request.width == 0 || request.height == 0 || request.fps == 0) {
        if (error_detail != nullptr) {
            *error_detail = "encoder profile request requires non-zero width/height/fps";
        }
        return false;
    }

    if (request.fps > 240) {
        if (error_detail != nullptr) {
            *error_detail = "fps out of supported range (max 240)";
        }
        return false;
    }

    return true;
}

std::optional<std::string> read_env_value(const char* key) {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, key) != 0 || buffer == nullptr) {
        return std::nullopt;
    }

    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

bool is_env_truthy(const char* key) {
    const auto value = read_env_value(key);
    if (!value.has_value()) {
        return false;
    }

    return value.value() == "1"
        || value.value() == "true"
        || value.value() == "TRUE"
        || value.value() == "on"
        || value.value() == "ON";
}

bool is_env_falsy(const char* key) {
    const auto value = read_env_value(key);
    if (!value.has_value()) {
        return false;
    }

    return value.value() == "0"
        || value.value() == "false"
        || value.value() == "FALSE"
        || value.value() == "off"
        || value.value() == "OFF";
}

bool is_backend_usable(EncoderBackendType backend, const EncoderBackendCapabilities& capabilities) {
    if (backend == EncoderBackendType::kNvenc) {
        return capabilities.ffmpeg_runtime_available && capabilities.nvenc_available;
    }
    if (backend == EncoderBackendType::kQuickSync) {
        return capabilities.ffmpeg_runtime_available && capabilities.quicksync_available;
    }
    if (backend == EncoderBackendType::kAmf) {
        return capabilities.ffmpeg_runtime_available && capabilities.amf_available;
    }
    if (backend == EncoderBackendType::kSoftware) {
        return capabilities.software_available;
    }
    return false;
}

bool is_hardware_backend(EncoderBackendType backend) {
    return backend == EncoderBackendType::kNvenc
        || backend == EncoderBackendType::kQuickSync
        || backend == EncoderBackendType::kAmf;
}

std::optional<EncoderBackendType> preferred_backend_for_capture_adapter_vendor(
    CaptureAdapterVendor capture_adapter_vendor) {
    switch (capture_adapter_vendor) {
    case CaptureAdapterVendor::kIntel:
        return EncoderBackendType::kQuickSync;
    case CaptureAdapterVendor::kNvidia:
        return EncoderBackendType::kNvenc;
    case CaptureAdapterVendor::kAmd:
        return EncoderBackendType::kAmf;
    case CaptureAdapterVendor::kUnknown:
        return std::nullopt;
    }

    return std::nullopt;
}

std::vector<EncoderBackendType> resolve_hardware_backend_priority_order(
    CaptureAdapterVendor capture_adapter_vendor) {
    std::vector<EncoderBackendType> ordered_backends;
    ordered_backends.reserve(3);

    const auto preferred_backend = preferred_backend_for_capture_adapter_vendor(capture_adapter_vendor);
    if (preferred_backend.has_value()) {
        ordered_backends.push_back(preferred_backend.value());
    }

    for (const EncoderBackendType backend : {
             EncoderBackendType::kNvenc,
             EncoderBackendType::kQuickSync,
             EncoderBackendType::kAmf,
         }) {
        if (preferred_backend.has_value() && preferred_backend.value() == backend) {
            continue;
        }

        ordered_backends.push_back(backend);
    }

    return ordered_backends;
}

std::string describe_encoder_backend(EncoderBackendType backend) {
    switch (backend) {
    case EncoderBackendType::kAuto:
        return "auto";
    case EncoderBackendType::kNvenc:
        return "NVENC";
    case EncoderBackendType::kQuickSync:
        return "QuickSync";
    case EncoderBackendType::kAmf:
        return "AMF";
    case EncoderBackendType::kSoftware:
        return "software";
    }

    return "unknown";
}

void disable_backend_capability(EncoderBackendType backend, EncoderBackendCapabilities* capabilities) {
    if (capabilities == nullptr) {
        return;
    }

    switch (backend) {
    case EncoderBackendType::kNvenc:
        capabilities->nvenc_available = false;
        break;
    case EncoderBackendType::kQuickSync:
        capabilities->quicksync_available = false;
        break;
    case EncoderBackendType::kAmf:
        capabilities->amf_available = false;
        break;
    case EncoderBackendType::kAuto:
    case EncoderBackendType::kSoftware:
        break;
    }
}

#if REDCLAW_CAPTURE_HAS_LIBAVCODEC

std::string ffmpeg_error_to_string(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, error_code);
    return std::string(buffer);
}

std::size_t find_annex_b_start_code(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t offset,
    std::size_t* prefix_size) {
    if (data == nullptr || offset >= size) {
        return size;
    }

    for (std::size_t index = offset; index + 3 < size; ++index) {
        if (data[index] != 0 || data[index + 1] != 0) {
            continue;
        }
        if (data[index + 2] == 1) {
            if (prefix_size != nullptr) {
                *prefix_size = 3;
            }
            return index;
        }
        if (index + 4 < size && data[index + 2] == 0 && data[index + 3] == 1) {
            if (prefix_size != nullptr) {
                *prefix_size = 4;
            }
            return index;
        }
    }

    return size;
}

bool payload_contains_h264_keyframe(const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        std::size_t prefix_size = 0;
        const std::size_t start = find_annex_b_start_code(data, size, offset, &prefix_size);
        if (start == size) {
            break;
        }

        const std::size_t nal_index = start + prefix_size;
        if (nal_index >= size) {
            break;
        }

        const std::uint8_t nal_type = data[nal_index] & 0x1FU;
        if (nal_type == 5U) {
            return true;
        }

        offset = nal_index + 1;
    }

    return false;
}

bool payload_contains_hevc_keyframe(const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        std::size_t prefix_size = 0;
        const std::size_t start = find_annex_b_start_code(data, size, offset, &prefix_size);
        if (start == size) {
            break;
        }

        const std::size_t nal_index = start + prefix_size;
        if (nal_index >= size) {
            break;
        }

        const std::uint8_t nal_type = static_cast<std::uint8_t>((data[nal_index] >> 1U) & 0x3FU);
        if (nal_type >= 16U && nal_type <= 21U) {
            return true;
        }

        offset = nal_index + 1;
    }

    return false;
}

bool packet_contains_keyframe(EncoderCodec codec, const AVPacket* packet) {
    if (packet == nullptr || packet->data == nullptr || packet->size <= 0) {
        return false;
    }
    if ((packet->flags & AV_PKT_FLAG_KEY) != 0) {
        return true;
    }

    const auto* payload = reinterpret_cast<const std::uint8_t*>(packet->data);
    const std::size_t payload_size = static_cast<std::size_t>(packet->size);
    if (codec == EncoderCodec::kHevc) {
        return payload_contains_hevc_keyframe(payload, payload_size);
    }
    return payload_contains_h264_keyframe(payload, payload_size);
}

AVCodecID resolve_ffmpeg_codec_id(EncoderCodec codec) {
    if (codec == EncoderCodec::kHevc) {
        return AV_CODEC_ID_HEVC;
    }
    return AV_CODEC_ID_H264;
}

std::vector<std::string> resolve_encoder_candidates(EncoderCodec codec, EncoderBackendType backend) {
    if (codec == EncoderCodec::kHevc) {
        if (backend == EncoderBackendType::kNvenc) {
            return {"hevc_nvenc"};
        }
        if (backend == EncoderBackendType::kQuickSync) {
            return {"hevc_qsv"};
        }
        if (backend == EncoderBackendType::kAmf) {
            return {"hevc_amf"};
        }
        return {"libx265"};
    }

    if (backend == EncoderBackendType::kNvenc) {
        return {"h264_nvenc"};
    }
    if (backend == EncoderBackendType::kQuickSync) {
        return {"h264_qsv"};
    }
    if (backend == EncoderBackendType::kAmf) {
        return {"h264_amf"};
    }
    return {"libx264", "libopenh264"};
}

bool has_any_encoder_named(const std::vector<std::string>& encoder_names) {
    for (const std::string& encoder_name : encoder_names) {
        if (avcodec_find_encoder_by_name(encoder_name.c_str()) != nullptr) {
            return true;
        }
    }

    return false;
}

bool encoder_supports_pixel_format(const AVCodec* codec, AVPixelFormat pixel_format) {
    if (codec == nullptr || codec->pix_fmts == nullptr) {
        return false;
    }

    for (const AVPixelFormat* supported_format = codec->pix_fmts;
         *supported_format != AV_PIX_FMT_NONE;
         ++supported_format) {
        if (*supported_format == pixel_format) {
            return true;
        }
    }

    return false;
}

AVPixelFormat resolve_encoder_cpu_pixel_format(
    const AVCodec* codec,
    EncoderBackendType backend) {
    if (encoder_supports_pixel_format(codec, AV_PIX_FMT_BGRA)) {
        return AV_PIX_FMT_BGRA;
    }
    if (is_hardware_encoder_backend(backend)
        && encoder_supports_pixel_format(codec, AV_PIX_FMT_NV12)) {
        return AV_PIX_FMT_NV12;
    }
    if (encoder_supports_pixel_format(codec, AV_PIX_FMT_YUV420P)) {
        return AV_PIX_FMT_YUV420P;
    }
    if (encoder_supports_pixel_format(codec, AV_PIX_FMT_NV12)) {
        return AV_PIX_FMT_NV12;
    }
    return AV_PIX_FMT_YUV420P;
}

std::string describe_encoder_cpu_input_mode(AVPixelFormat format, bool scaled) {
    std::string mode;
    if (format == AV_PIX_FMT_BGRA || format == AV_PIX_FMT_BGR0) {
        mode = "bgra-direct";
    } else if (format == AV_PIX_FMT_NV12) {
        mode = "nv12-convert";
    } else {
        mode = "yuv420p-convert";
    }
    if (!scaled) {
        return mode;
    }
    return format == AV_PIX_FMT_BGRA || format == AV_PIX_FMT_BGR0
        ? "bgra-scale-direct"
        : "scale-" + mode;
}

#if REDCLAW_CAPTURE_HAS_LIBAVCODEC
std::string hw_device_type_to_string(AVHWDeviceType device_type) {
    const char* name = av_hwdevice_get_type_name(device_type);
    if (name == nullptr) {
        return "unknown";
    }

    return std::string(name);
}

const AVCodecHWConfig* find_encoder_hw_config(
    const AVCodec* codec,
    AVHWDeviceType device_type) {
    if (codec == nullptr) {
        return nullptr;
    }

    for (int index = 0;; ++index) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, index);
        if (config == nullptr) {
            break;
        }

        if ((config->methods & (AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX | AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) != 0
            && config->device_type == device_type) {
            return config;
        }
    }

    return nullptr;
}

struct EncoderHardwareInputPlan {
    AVHWDeviceType device_type = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat pixel_format = AV_PIX_FMT_NONE;
    std::string input_mode;
};

std::vector<AVHWDeviceType> resolve_encoder_hw_device_priority_order(EncoderBackendType backend) {
    switch (backend) {
    case EncoderBackendType::kNvenc:
        return {AV_HWDEVICE_TYPE_D3D11VA};
    case EncoderBackendType::kQuickSync:
        return {AV_HWDEVICE_TYPE_QSV, AV_HWDEVICE_TYPE_D3D11VA};
    case EncoderBackendType::kAmf:
        {
            std::vector<AVHWDeviceType> device_types;
            const AVHWDeviceType amf_device_type = av_hwdevice_find_type_by_name("amf");
            if (amf_device_type != AV_HWDEVICE_TYPE_NONE) {
                device_types.push_back(amf_device_type);
            }
            device_types.push_back(AV_HWDEVICE_TYPE_D3D11VA);
            return device_types;
        }
    default:
        return {};
    }
}

std::optional<EncoderHardwareInputPlan> resolve_encoder_hardware_input_plan(
    const AVCodec* codec,
    EncoderBackendType backend) {
    if (codec == nullptr || !is_hardware_encoder_backend(backend)) {
        return std::nullopt;
    }

    for (const AVHWDeviceType device_type : resolve_encoder_hw_device_priority_order(backend)) {
        if (const AVCodecHWConfig* config = find_encoder_hw_config(codec, device_type)) {
            EncoderHardwareInputPlan plan;
            plan.device_type = config->device_type;
            plan.pixel_format = config->pix_fmt;
            plan.input_mode = hw_device_type_to_string(plan.device_type) + "-hwframe";
            return plan;
        }

#if REDCLAW_CAPTURE_HAS_D3D11_HWCONTEXT && defined(_WIN32)
        if (device_type == AV_HWDEVICE_TYPE_D3D11VA && encoder_supports_pixel_format(codec, AV_PIX_FMT_D3D11)) {
            EncoderHardwareInputPlan plan;
            plan.device_type = AV_HWDEVICE_TYPE_D3D11VA;
            plan.pixel_format = AV_PIX_FMT_D3D11;
            plan.input_mode = "d3d11va-hwframe";
            return plan;
        }
#endif
    }

    return std::nullopt;
}
#endif

std::uint8_t clamp_to_u8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<std::uint8_t>(value);
}

bool convert_bgra_to_encoder_frame(
    const CapturedFrame& frame,
    AVFrame* output_frame,
#if REDCLAW_CAPTURE_HAS_LIBSWSCALE
    SwsContext** sws_context,
#endif
    std::string* error_detail) {
    if (output_frame == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "encoder output frame must be non-null";
        }
        return false;
    }

    const CaptureRegion source_region = normalize_capture_region(
        frame.source_region,
        frame.width,
        frame.height);
    const std::uint32_t source_width = source_region.width;
    const std::uint32_t source_height = source_region.height;
    const std::uint32_t output_width = static_cast<std::uint32_t>(output_frame->width);
    const std::uint32_t output_height = static_cast<std::uint32_t>(output_frame->height);
    const AVPixelFormat output_format = static_cast<AVPixelFormat>(output_frame->format);
    const EncoderContentRect content = resolve_capture_region_content_rect(
        source_region,
        output_width,
        output_height);
    const bool same_dimensions = source_width == output_width
        && source_height == output_height
        && content.x == 0 && content.y == 0;

    if (output_width == 0 || output_height == 0
        || source_width == 0 || source_height == 0
        || content.width == 0 || content.height == 0) {
        if (error_detail != nullptr) {
            *error_detail = "encoder output frame dimensions must be non-zero";
        }
        return false;
    }

    if (output_format == AV_PIX_FMT_BGRA || output_format == AV_PIX_FMT_BGR0) {
        for (std::uint32_t y = 0; y < output_height; ++y) {
            std::memset(
                output_frame->data[0] + static_cast<std::size_t>(y) * output_frame->linesize[0],
                0,
                static_cast<std::size_t>(output_width) * 4U);
        }
    } else if (output_format == AV_PIX_FMT_YUV420P) {
        for (std::uint32_t y = 0; y < output_height; ++y) {
            std::memset(output_frame->data[0] + static_cast<std::size_t>(y) * output_frame->linesize[0], 16, output_width);
        }
        for (std::uint32_t y = 0; y < output_height / 2U; ++y) {
            std::memset(output_frame->data[1] + static_cast<std::size_t>(y) * output_frame->linesize[1], 128, output_width / 2U);
            std::memset(output_frame->data[2] + static_cast<std::size_t>(y) * output_frame->linesize[2], 128, output_width / 2U);
        }
    } else if (output_format == AV_PIX_FMT_NV12) {
        for (std::uint32_t y = 0; y < output_height; ++y) {
            std::memset(output_frame->data[0] + static_cast<std::size_t>(y) * output_frame->linesize[0], 16, output_width);
        }
        for (std::uint32_t y = 0; y < output_height / 2U; ++y) {
            std::memset(output_frame->data[1] + static_cast<std::size_t>(y) * output_frame->linesize[1], 128, output_width);
        }
    }

    if ((output_format == AV_PIX_FMT_BGRA || output_format == AV_PIX_FMT_BGR0) && same_dimensions) {
        for (std::uint32_t y = 0; y < source_height; ++y) {
            const auto* src_row = frame.data.data()
                + static_cast<std::size_t>(source_region.y + y) * frame.row_pitch
                + static_cast<std::size_t>(source_region.x) * 4U;
            auto* dst_row = output_frame->data[0] + static_cast<std::size_t>(y) * output_frame->linesize[0];
            std::memcpy(dst_row, src_row, static_cast<std::size_t>(source_width) * 4);
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

#if REDCLAW_CAPTURE_HAS_LIBSWSCALE
    if (sws_context == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "sws_context storage must be non-null";
        }
        return false;
    }

    *sws_context = sws_getCachedContext(
        *sws_context,
        static_cast<int>(source_width),
        static_cast<int>(source_height),
        AV_PIX_FMT_BGRA,
        static_cast<int>(content.width),
        static_cast<int>(content.height),
        output_format,
        SWS_FAST_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (*sws_context == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "sws_getCachedContext returned null";
        }
        return false;
    }

    std::uint8_t* src_data[4] = {
        const_cast<std::uint8_t*>(frame.data.data()
            + static_cast<std::size_t>(source_region.y) * frame.row_pitch
            + static_cast<std::size_t>(source_region.x) * 4U),
        nullptr,
        nullptr,
        nullptr};
    int src_linesize[4] = {static_cast<int>(frame.row_pitch), 0, 0, 0};

    std::uint8_t* destination_data[4] = {
        output_frame->data[0], output_frame->data[1], output_frame->data[2], output_frame->data[3]};
    if (output_format == AV_PIX_FMT_BGRA || output_format == AV_PIX_FMT_BGR0) {
        destination_data[0] += static_cast<std::size_t>(content.y) * output_frame->linesize[0]
            + static_cast<std::size_t>(content.x) * 4U;
    } else if (output_format == AV_PIX_FMT_YUV420P) {
        destination_data[0] += static_cast<std::size_t>(content.y) * output_frame->linesize[0] + content.x;
        destination_data[1] += static_cast<std::size_t>(content.y / 2U) * output_frame->linesize[1] + content.x / 2U;
        destination_data[2] += static_cast<std::size_t>(content.y / 2U) * output_frame->linesize[2] + content.x / 2U;
    } else if (output_format == AV_PIX_FMT_NV12) {
        destination_data[0] += static_cast<std::size_t>(content.y) * output_frame->linesize[0] + content.x;
        destination_data[1] += static_cast<std::size_t>(content.y / 2U) * output_frame->linesize[1] + content.x;
    }

    const int scaled_rows = sws_scale(
        *sws_context,
        src_data,
        src_linesize,
        0,
        static_cast<int>(source_height),
        destination_data,
        output_frame->linesize);
    if (scaled_rows != static_cast<int>(content.height)) {
        if (error_detail != nullptr) {
            *error_detail = "sws_scale did not convert the full frame";
        }
        return false;
    }

    if (error_detail != nullptr) {
        error_detail->clear();
    }
    return true;
#else
    if (!same_dimensions) {
        if (error_detail != nullptr) {
            *error_detail = "input frame scaling requires libswscale";
        }
        return false;
    }
    if (output_format != AV_PIX_FMT_YUV420P) {
        if (error_detail != nullptr) {
            *error_detail = "encoder output pixel format requires libswscale";
        }
        return false;
    }

    for (std::uint32_t y = 0; y < source_height; ++y) {
        const auto* src_row = frame.data.data() + static_cast<std::size_t>(y) * frame.row_pitch;
        auto* y_row = output_frame->data[0] + static_cast<std::size_t>(y) * output_frame->linesize[0];

        for (std::uint32_t x = 0; x < source_width; ++x) {
            const std::size_t src_index = static_cast<std::size_t>(x) * 4;
            const int b = src_row[src_index + 0];
            const int g = src_row[src_index + 1];
            const int r = src_row[src_index + 2];

            const int y_value = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            y_row[x] = clamp_to_u8(y_value);
        }
    }

    for (std::uint32_t y = 0; y < source_height; y += 2) {
        const auto* src_row0 = frame.data.data() + static_cast<std::size_t>(y) * frame.row_pitch;
        const auto* src_row1 =
            frame.data.data() + static_cast<std::size_t>(std::min(y + 1, source_height - 1)) * frame.row_pitch;

        auto* u_row = output_frame->data[1] + static_cast<std::size_t>(y / 2) * output_frame->linesize[1];
        auto* v_row = output_frame->data[2] + static_cast<std::size_t>(y / 2) * output_frame->linesize[2];

        for (std::uint32_t x = 0; x < source_width; x += 2) {
            int r_sum = 0;
            int g_sum = 0;
            int b_sum = 0;

            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                const auto* src_row = (dy == 0) ? src_row0 : src_row1;
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::uint32_t sample_x = std::min(x + dx, source_width - 1);
                    const std::size_t src_index = static_cast<std::size_t>(sample_x) * 4;
                    b_sum += src_row[src_index + 0];
                    g_sum += src_row[src_index + 1];
                    r_sum += src_row[src_index + 2];
                }
            }

            const int r = r_sum / 4;
            const int g = g_sum / 4;
            const int b = b_sum / 4;

            const int u_value = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            const int v_value = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

            u_row[x / 2] = clamp_to_u8(u_value);
            v_row[x / 2] = clamp_to_u8(v_value);
        }
    }

    if (error_detail != nullptr) {
        error_detail->clear();
    }
    return true;
#endif
}

#endif

}  // namespace

CaptureRegion normalize_capture_region(
    CaptureRegion requested,
    std::uint32_t source_width,
    std::uint32_t source_height) {
    CaptureRegion normalized;
    normalized.revision = requested.revision == 0 ? 1 : requested.revision;
    if (source_width < 64 || source_height < 64) {
        return normalized;
    }
    if (requested.width == 0 || requested.height == 0) {
        requested = CaptureRegion{
            .x = 0,
            .y = 0,
            .width = source_width,
            .height = source_height,
            .revision = normalized.revision,
        };
    }
    normalized.x = (std::min)(requested.x, source_width - 64U) & ~1U;
    normalized.y = (std::min)(requested.y, source_height - 64U) & ~1U;
    const std::uint32_t available_width = source_width - normalized.x;
    const std::uint32_t available_height = source_height - normalized.y;
    normalized.width = std::clamp(requested.width, 64U, available_width) & ~1U;
    normalized.height = std::clamp(requested.height, 64U, available_height) & ~1U;
    normalized.width = (std::max)(64U, normalized.width);
    normalized.height = (std::max)(64U, normalized.height);
    return normalized;
}

CaptureRegion capture_region_from_normalized_bounds(
    std::uint16_t left,
    std::uint16_t top,
    std::uint16_t right,
    std::uint16_t bottom,
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint64_t revision) {
    if (left >= right || top >= bottom) {
        return {};
    }
    const auto scale_floor = [](std::uint16_t value, std::uint32_t extent) {
        return static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(value) * extent / 65535ULL);
    };
    const auto scale_ceil = [](std::uint16_t value, std::uint32_t extent) {
        return static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(value) * extent + 65534ULL) / 65535ULL);
    };
    const std::uint32_t x = scale_floor(left, source_width);
    const std::uint32_t y = scale_floor(top, source_height);
    const std::uint32_t right_px = (std::min)(source_width, scale_ceil(right, source_width));
    const std::uint32_t bottom_px = (std::min)(source_height, scale_ceil(bottom, source_height));
    return normalize_capture_region(
        CaptureRegion{
            .x = x,
            .y = y,
            .width = right_px > x ? right_px - x : 0,
            .height = bottom_px > y ? bottom_px - y : 0,
            .revision = revision,
        },
        source_width,
        source_height);
}

EncoderContentRect resolve_capture_region_content_rect(
    const CaptureRegion& region,
    std::uint32_t canvas_width,
    std::uint32_t canvas_height) {
    EncoderContentRect rect;
    if (region.width == 0 || region.height == 0
        || canvas_width < 2 || canvas_height < 2) {
        return rect;
    }
    const double scale = (std::min)(
        static_cast<double>(canvas_width) / region.width,
        static_cast<double>(canvas_height) / region.height);
    rect.width = (std::max)(2U,
        static_cast<std::uint32_t>(static_cast<double>(region.width) * scale) & ~1U);
    rect.height = (std::max)(2U,
        static_cast<std::uint32_t>(static_cast<double>(region.height) * scale) & ~1U);
    rect.width = (std::min)(rect.width, canvas_width & ~1U);
    rect.height = (std::min)(rect.height, canvas_height & ~1U);
    rect.x = ((canvas_width - rect.width) / 2U) & ~1U;
    rect.y = ((canvas_height - rect.height) / 2U) & ~1U;
    return rect;
}

bool encode_navigation_thumbnail_jpeg(
    const CapturedFrame& frame,
    std::uint32_t max_edge,
    std::vector<std::uint8_t>* jpeg,
    std::uint32_t* output_width,
    std::uint32_t* output_height,
    std::string* error_detail) {
    if (jpeg == nullptr || output_width == nullptr || output_height == nullptr) {
        assign_error("navigation thumbnail output is null", error_detail);
        return false;
    }
    jpeg->clear();
    *output_width = 0;
    *output_height = 0;
    if (!frame.bgra || frame.width == 0 || frame.height == 0
        || frame.row_pitch < frame.width * 4U
        || frame.data.size() < static_cast<std::size_t>(frame.row_pitch) * frame.height
        || max_edge < 64 || max_edge > 320) {
        assign_error("navigation thumbnail requires a bounded CPU BGRA frame", error_detail);
        return false;
    }
#ifdef _WIN32
    const double scale = (std::min)(
        1.0,
        static_cast<double>(max_edge) / (std::max)(frame.width, frame.height));
    const auto width = (std::max)(1U, static_cast<std::uint32_t>(frame.width * scale));
    const auto height = (std::max)(1U, static_cast<std::uint32_t>(frame.height * scale));
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_hr);
    if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE) {
        assign_error("COM initialization failed for navigation thumbnail", error_detail);
        return false;
    }
    struct ComApartmentGuard final {
        bool active = false;
        ~ComApartmentGuard() {
            if (active) {
                CoUninitialize();
            }
        }
    } com_guard{uninitialize_com};
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    ComPtr<IWICBitmap> source;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateBitmapFromMemory(
            frame.width,
            frame.height,
            GUID_WICPixelFormat32bppBGRA,
            frame.row_pitch,
            static_cast<UINT>(frame.data.size()),
            const_cast<BYTE*>(frame.data.data()),
            &source);
    }
    ComPtr<IWICBitmapScaler> scaler;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateBitmapScaler(&scaler);
    }
    if (SUCCEEDED(hr)) {
        hr = scaler->Initialize(source.Get(), width, height, WICBitmapInterpolationModeFant);
    }
    ComPtr<IStream> stream;
    if (SUCCEEDED(hr)) {
        hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }
    ComPtr<IWICBitmapFrameEncode> frame_encoder;
    ComPtr<IPropertyBag2> options;
    if (SUCCEEDED(hr)) {
        hr = encoder->CreateNewFrame(&frame_encoder, &options);
    }
    if (SUCCEEDED(hr)) {
        hr = frame_encoder->Initialize(options.Get());
    }
    if (SUCCEEDED(hr)) {
        hr = frame_encoder->SetSize(width, height);
    }
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) {
        hr = frame_encoder->SetPixelFormat(&pixel_format);
    }
    if (SUCCEEDED(hr)) {
        hr = frame_encoder->WriteSource(scaler.Get(), nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = frame_encoder->Commit();
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
    }
    HGLOBAL global = nullptr;
    if (SUCCEEDED(hr)) {
        hr = GetHGlobalFromStream(stream.Get(), &global);
    }
    const SIZE_T jpeg_size = global != nullptr ? GlobalSize(global) : 0;
    const void* bytes = global != nullptr ? GlobalLock(global) : nullptr;
    if (FAILED(hr) || bytes == nullptr || jpeg_size == 0
        || jpeg_size > 512U * 1024U) {
        if (bytes != nullptr) {
            GlobalUnlock(global);
        }
        assign_error("WIC JPEG encoding failed for navigation thumbnail", error_detail);
        return false;
    }
    jpeg->assign(static_cast<const std::uint8_t*>(bytes),
                 static_cast<const std::uint8_t*>(bytes) + jpeg_size);
    GlobalUnlock(global);
    *output_width = width;
    *output_height = height;
    assign_error({}, error_detail);
    return true;
#else
    (void)max_edge;
    assign_error("navigation thumbnails are only available on Windows", error_detail);
    return false;
#endif
}

std::vector<CaptureDisplayDescriptor> enumerate_capture_displays(
    std::string* error_detail) {
    std::vector<CaptureDisplayDescriptor> displays;
#ifdef _WIN32
    ComPtr<IDXGIFactory1> factory;
    const HRESULT factory_hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(factory_hr) || factory == nullptr) {
        assign_error("CreateDXGIFactory1 failed while enumerating displays", error_detail);
        return displays;
    }
    for (std::uint32_t adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (adapter == nullptr) {
            continue;
        }
        DXGI_ADAPTER_DESC1 adapter_desc{};
        adapter->GetDesc1(&adapter_desc);
        const std::int64_t luid =
            (static_cast<std::int64_t>(adapter_desc.AdapterLuid.HighPart) << 32)
            | adapter_desc.AdapterLuid.LowPart;
        for (std::uint32_t output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            const HRESULT output_hr = adapter->EnumOutputs(output_index, &output);
            if (output_hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(output_hr) || output == nullptr) {
                continue;
            }
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) {
                continue;
            }
            const auto width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            const auto height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            if (width < 64 || height < 64) {
                continue;
            }
            CaptureDisplayDescriptor display;
            display.adapter_index = adapter_index;
            display.output_index = output_index;
            display.adapter_luid = luid;
            display.device_path = wide_to_utf8(desc.DeviceName);
            display.name = display.device_path.empty()
                ? "Display " + std::to_string(displays.size() + 1)
                : display.device_path;
            std::ostringstream id;
            id << std::hex << static_cast<std::uint64_t>(luid) << '-'
               << output_index;
            display.id = id.str();
            display.desktop_origin_x = desc.DesktopCoordinates.left;
            display.desktop_origin_y = desc.DesktopCoordinates.top;
            display.pixel_width = static_cast<std::uint32_t>(width);
            display.pixel_height = static_cast<std::uint32_t>(height);
            display.rotation = desc.Rotation == DXGI_MODE_ROTATION_ROTATE90 ? 90U
                : desc.Rotation == DXGI_MODE_ROTATION_ROTATE180 ? 180U
                : desc.Rotation == DXGI_MODE_ROTATION_ROTATE270 ? 270U
                : 0U;
            display.primary = desc.DesktopCoordinates.left == 0
                && desc.DesktopCoordinates.top == 0;
            displays.push_back(std::move(display));
        }
    }
#else
    (void)error_detail;
#endif
    if (displays.empty()) {
        assign_error("no attached desktop displays were found", error_detail);
    } else if (error_detail != nullptr) {
        error_detail->clear();
    }
    return displays;
}

std::uint32_t resolve_interactive_desktop_bitrate_floor_kbps(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t fps) {
    constexpr std::uint32_t kInteractiveBitrateFloorKbps = 400;
    constexpr std::uint32_t kInteractiveBitrateCeilingKbps = 20000;
    if (width == 0 || height == 0 || fps == 0) {
        return kInteractiveBitrateFloorKbps;
    }

    // Preserve the initial interactive-desktop quality budget for every frame
    // that Host still elects to send. Network adaptation must lower cadence
    // before it can lower this bitrate. One-FPS pacing requests IDR for every
    // selected latest frame, so it receives a higher independently-decodable
    // frame budget.
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    const std::uint64_t quality_floor_kbps = fps == 1
        ? (((pixels * 3ULL + 1ULL) / 2ULL) + 999ULL) / 1000ULL
        : (pixels * static_cast<std::uint64_t>(fps) * 8ULL) / 100000ULL;
    return clamp_u32(
        static_cast<std::uint32_t>(std::min<std::uint64_t>(
            quality_floor_kbps,
            std::numeric_limits<std::uint32_t>::max())),
        kInteractiveBitrateFloorKbps,
        kInteractiveBitrateCeilingKbps);
}

bool build_low_latency_encoder_profile(
    const EncoderProfileRequest& request,
    EncoderConfigProfile* profile,
    std::string* error_detail) {
    if (profile == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "profile output must be non-null";
        }
        return false;
    }

    if (!validate_encoder_request(request, error_detail)) {
        return false;
    }

    const std::uint64_t pixels_per_second =
        static_cast<std::uint64_t>(request.width) * static_cast<std::uint64_t>(request.height) * request.fps;

    std::uint32_t target_bitrate_kbps = 0;
    if (request.workload == EncoderWorkload::kFastAction) {
        target_bitrate_kbps = static_cast<std::uint32_t>(pixels_per_second / 180ULL);
        target_bitrate_kbps = clamp_u32(target_bitrate_kbps, 6000, 35000);
    } else {
        // Interactive desktop starts at approximately 0.08 bits per pixel.
        target_bitrate_kbps = static_cast<std::uint32_t>((pixels_per_second * 8ULL) / 100000ULL);
        target_bitrate_kbps = clamp_u32(
            target_bitrate_kbps,
            resolve_interactive_desktop_bitrate_floor_kbps(
                request.width,
                request.height,
                request.fps),
            20000);
    }

    profile->codec = request.preferred_codec;
    profile->workload = request.workload;
    profile->rate_control = EncoderRateControl::kCbr;
    profile->width = request.width;
    profile->height = request.height;
    profile->fps = request.fps;
    profile->target_bitrate_kbps = target_bitrate_kbps;
    profile->max_bitrate_kbps = target_bitrate_kbps + (target_bitrate_kbps / 5);
    profile->gop_length_frames = std::max(2U,
        (request.workload == EncoderWorkload::kFastAction) ? request.fps : request.fps * 2);
    profile->b_frames = 0;
    profile->lookahead_enabled = false;
    profile->repeat_headers = true;
    profile->zero_latency_tuning = true;

    if (error_detail != nullptr) {
        error_detail->clear();
    }
    return true;
}

bool resolve_viewport_encode_dimensions(
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint32_t viewport_width,
    std::uint32_t viewport_height,
    std::uint32_t configured_max_width,
    std::uint32_t* encode_width,
    std::uint32_t* encode_height,
    std::string* error_detail) {
    if (encode_width == nullptr || encode_height == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "encode dimension outputs must be non-null";
        }
        return false;
    }
    if (source_width < 2 || source_height < 2
        || viewport_width < 2 || viewport_height < 2) {
        if (error_detail != nullptr) {
            *error_detail = "source and viewport dimensions must be at least 2 pixels";
        }
        return false;
    }

    double scale = (std::min)(
        1.0,
        (std::min)(
            static_cast<double>(viewport_width) / source_width,
            static_cast<double>(viewport_height) / source_height));
    if (configured_max_width > 0) {
        scale = (std::min)(
            scale,
            static_cast<double>(configured_max_width) / source_width);
    }
    auto even_floor = [](double value) {
        const auto integral = static_cast<std::uint32_t>(value);
        return (std::max)(2U, integral - (integral % 2U));
    };
    *encode_width = even_floor(static_cast<double>(source_width) * scale);
    *encode_height = even_floor(static_cast<double>(source_height) * scale);
    if (error_detail != nullptr) {
        error_detail->clear();
    }
    return true;
}

bool detect_encoder_backend_capabilities(
    EncoderBackendCapabilities* capabilities,
    std::string* error_detail) {
    if (capabilities == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "capabilities output must be non-null";
        }
        return false;
    }

    *capabilities = EncoderBackendCapabilities{};

#if REDCLAW_CAPTURE_HAS_LIBAVCODEC
    capabilities->ffmpeg_runtime_available = true;
#endif

#if defined(_WIN32) && REDCLAW_CAPTURE_HAS_LIBAVCODEC
    HMODULE avcodec = LoadLibraryA("avcodec-61.dll");
    if (avcodec == nullptr) {
        avcodec = LoadLibraryA("avcodec-60.dll");
    }
    if (avcodec == nullptr) {
        avcodec = LoadLibraryA("avcodec-59.dll");
    }

    if (avcodec != nullptr) {
        capabilities->ffmpeg_runtime_available = true;
        FreeLibrary(avcodec);
    }
#endif

    if (is_env_truthy("REDCLAW_DISABLE_FFMPEG") || is_env_falsy("REDCLAW_ENABLE_FFMPEG")) {
        capabilities->ffmpeg_runtime_available = false;
    }

    if (capabilities->ffmpeg_runtime_available) {
        capabilities->nvenc_available = has_any_encoder_named({"h264_nvenc", "hevc_nvenc"});
        capabilities->quicksync_available = has_any_encoder_named({"h264_qsv", "hevc_qsv"});
        capabilities->amf_available = has_any_encoder_named({"h264_amf", "hevc_amf"});
        capabilities->software_available = has_any_encoder_named({"libx264", "libopenh264", "libx265"});
    }

    if (is_env_truthy("REDCLAW_DISABLE_NVENC") || is_env_falsy("REDCLAW_ENABLE_NVENC")) {
        capabilities->nvenc_available = false;
    }
    if (is_env_truthy("REDCLAW_DISABLE_QSV") || is_env_falsy("REDCLAW_ENABLE_QSV")) {
        capabilities->quicksync_available = false;
    }
    if (is_env_truthy("REDCLAW_DISABLE_AMF") || is_env_falsy("REDCLAW_ENABLE_AMF")) {
        capabilities->amf_available = false;
    }
    if (is_env_truthy("REDCLAW_DISABLE_SOFTWARE_ENCODER")) {
        capabilities->software_available = false;
    }

    if (error_detail != nullptr) {
        error_detail->clear();
    }
    return true;
}

CaptureAdapterVendor detect_captured_frame_adapter_vendor(
    const CapturedFrame& frame,
    std::string* adapter_summary) {
#ifdef _WIN32
    auto* native = unwrap_d3d11_native_handle(frame);
    if (native == nullptr || native->d3d11_device == nullptr) {
        if (adapter_summary != nullptr) {
            adapter_summary->clear();
        }
        return CaptureAdapterVendor::kUnknown;
    }

    DxgiAdapterIdentity identity;
    std::string error_detail;
    if (!query_d3d11_device_adapter_identity(native->d3d11_device.Get(), &identity, &error_detail)) {
        if (adapter_summary != nullptr) {
            *adapter_summary = error_detail;
        }
        return CaptureAdapterVendor::kUnknown;
    }

    if (adapter_summary != nullptr) {
        *adapter_summary = format_dxgi_adapter_identity(identity);
    }
    return classify_dxgi_vendor(identity.vendor_id);
#else
    (void)frame;
    if (adapter_summary != nullptr) {
        adapter_summary->clear();
    }
    return CaptureAdapterVendor::kUnknown;
#endif
}

bool build_encoder_backend_bridge_plan(
    const EncoderBackendBridgeRequest& request,
    const EncoderBackendCapabilities& capabilities,
    EncoderBackendBridgePlan* plan,
    std::string* error_detail) {
    if (plan == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "plan output must be non-null";
        }
        return false;
    }

    *plan = EncoderBackendBridgePlan{};
    plan->allow_hardware_frame_input = request.allow_hardware_frame_input;

    auto finalize_backend = [&](EncoderBackendType backend, std::string reason, bool fallback_applied) -> bool {
        plan->selected_backend = backend;
        plan->fallback_applied = fallback_applied;
        plan->reason = std::move(reason);
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    };

    auto find_available_hardware_backend = [&](std::optional<EncoderBackendType> exclude) -> std::optional<EncoderBackendType> {
        for (const EncoderBackendType backend : resolve_hardware_backend_priority_order(request.capture_adapter_vendor)) {
            if ((exclude.has_value() && exclude.value() == backend)
                || request.excluded_hardware_backend == backend) {
                continue;
            }

            if (is_backend_usable(backend, capabilities)) {
                return backend;
            }
        }

        return std::nullopt;
    };

    auto finalize_software = [&](std::string reason, bool fallback_applied) -> bool {
        if (!capabilities.software_available) {
            if (error_detail != nullptr) {
                *error_detail = "software encoder backend is unavailable";
            }
            return false;
        }

        return finalize_backend(EncoderBackendType::kSoftware, std::move(reason), fallback_applied);
    };

    if (request.preferred_backend == EncoderBackendType::kAuto) {
        const auto preferred_hardware = find_available_hardware_backend(std::nullopt);
        if (preferred_hardware.has_value()) {
            std::string reason = "auto-selected " + describe_encoder_backend(preferred_hardware.value()) + " backend";
            if (request.capture_adapter_vendor != CaptureAdapterVendor::kUnknown) {
                reason += " using " + describe_capture_adapter_vendor(request.capture_adapter_vendor)
                    + " capture adapter priority";
            }
            if (request.excluded_hardware_backend != EncoderBackendType::kAuto) {
                reason += "; excluded " + describe_encoder_backend(request.excluded_hardware_backend)
                    + " for this live reconfiguration";
            }
            return finalize_backend(
                preferred_hardware.value(),
                std::move(reason),
                false);
        }
        std::string reason =
            "auto mode fell back to software backend because no usable hardware backend is available";
        if (request.excluded_hardware_backend != EncoderBackendType::kAuto) {
            reason += "; excluded " + describe_encoder_backend(request.excluded_hardware_backend)
                + " for this live reconfiguration";
        }
        return finalize_software(std::move(reason), true);
    }

    if (request.preferred_backend == EncoderBackendType::kSoftware) {
        if (!capabilities.software_available) {
            if (error_detail != nullptr) {
                *error_detail = "software encoder backend is unavailable";
            }
            return false;
        }

        return finalize_backend(EncoderBackendType::kSoftware, "preferred backend is available", false);
    }

    if (is_backend_usable(request.preferred_backend, capabilities)) {
        return finalize_backend(request.preferred_backend, "preferred backend is available", false);
    }

    if (!request.allow_hardware_fallback) {
        if (error_detail != nullptr) {
            *error_detail = "preferred hardware backend is unavailable and fallback is disabled";
        }
        return false;
    }

    const auto alternate_hardware = find_available_hardware_backend(request.preferred_backend);
    if (alternate_hardware.has_value()) {
        return finalize_backend(
            alternate_hardware.value(),
            "preferred " + describe_encoder_backend(request.preferred_backend)
                + " backend is unavailable; fallback to "
                + describe_encoder_backend(alternate_hardware.value()) + " backend",
            true);
    }

    return finalize_software(
        "preferred hardware backend is unavailable; fallback to software backend",
        true);
}

bool should_defer_initial_encoder_start_for_viewport(
    bool encoder_started,
    bool viewport_ready,
    std::uint64_t required_channels_ready_at_ms,
    std::uint64_t now_ms,
    std::uint64_t grace_ms) noexcept {
    return !encoder_started
        && !viewport_ready
        && required_channels_ready_at_ms != 0
        && now_ms >= required_channels_ready_at_ms
        && now_ms - required_channels_ready_at_ms < grace_ms;
}

bool should_preserve_encoder_during_live_reconfiguration(
    EncoderBackendType active_backend,
    bool encoder_started,
    bool resolution_change_requested,
    bool rate_restart_requested) noexcept {
    (void)resolution_change_requested;
    return encoder_started
        && is_hardware_backend(active_backend)
        && rate_restart_requested;
}

class EncoderExecutionSession::Impl {
public:
    bool start(
        const EncoderConfigProfile& profile,
        const EncoderBackendBridgePlan& bridge_plan,
        std::string* error_detail) {
        stop();

        diagnostics_ = EncoderExecutionDiagnostics{};
        diagnostics_.backend = bridge_plan.selected_backend;
        profile_ = profile;
        selected_backend_ = bridge_plan.selected_backend;
        hardware_frame_input_allowed_ = bridge_plan.allow_hardware_frame_input;
        hardware_frame_input_activation_failed_ = !hardware_frame_input_allowed_;

        if (profile.width == 0 || profile.height == 0 || profile.fps == 0) {
            return fail(
                EncoderExecutionFailureCategory::kInvalidConfig,
                "encoder execution profile requires non-zero width/height/fps",
                error_detail);
        }
        if (profile.b_frames != 0 || profile.lookahead_enabled || !profile.zero_latency_tuning) {
            return fail(
                EncoderExecutionFailureCategory::kInvalidConfig,
                "desktop encoder requires zero B-frames, disabled lookahead, and zero-latency tuning",
                error_detail);
        }

#if REDCLAW_CAPTURE_HAS_LIBAVCODEC
        const auto encoder_candidates = resolve_encoder_candidates(profile.codec, bridge_plan.selected_backend);

        for (const std::string& encoder_name : encoder_candidates) {
            codec_ = avcodec_find_encoder_by_name(encoder_name.c_str());
            if (codec_ != nullptr) {
                selected_encoder_name_ = encoder_name;
                break;
            }
        }

        if (codec_ == nullptr) {
            return fail(
                EncoderExecutionFailureCategory::kEncoderNotFound,
                "no libavcodec encoder found for selected bridge backend",
                error_detail);
        }
        hardware_input_plan_ = resolve_encoder_hardware_input_plan(codec_, selected_backend_);
        const bool opened = open_runtime_context(false, nullptr, error_detail);
        if (opened && !hardware_frame_input_allowed_) {
            diagnostics_.hardware_input_block_reason =
                "D3D11 hardware-frame input disabled by runtime stability policy";
        }
        return opened;
#else
        (void)bridge_plan;
        return fail(
            EncoderExecutionFailureCategory::kLibavcodecUnavailable,
            "libavcodec headers are unavailable at build time; encoder execution path is disabled",
            error_detail);
#endif
    }

    bool encode_bgra_frame(
        const CapturedFrame& frame,
        std::uint64_t timestamp_ms,
        EncodedFramePacket* packet,
        std::string* error_detail) {
        if (packet == nullptr) {
            return fail(
                EncoderExecutionFailureCategory::kInvalidConfig,
                "encoded packet output must be non-null",
                error_detail);
        }

#if REDCLAW_CAPTURE_HAS_LIBAVCODEC
        if (!is_running()) {
            return fail(
                EncoderExecutionFailureCategory::kInvalidConfig,
                "encoder execution session is not running",
                error_detail);
        }

        if (!frame.bgra) {
            ++diagnostics_.dropped_frame_count;
            return fail(
                EncoderExecutionFailureCategory::kFrameFormatUnsupported,
                "encoder execution only accepts BGRA frames",
                error_detail);
        }

        const auto encode_start = std::chrono::steady_clock::now();
        const CaptureRegion source_region = normalize_capture_region(
            frame.source_region,
            frame.width,
            frame.height);
        const bool source_cropped = source_region.x != 0 || source_region.y != 0
            || source_region.width != frame.width || source_region.height != frame.height;
        const bool input_scaled = source_region.width != profile_.width
            || source_region.height != profile_.height;
        const bool has_cpu_pixels = captured_frame_has_cpu_bgra_pixels(frame);
        const bool has_native_texture = unwrap_d3d11_native_handle(frame) != nullptr;
        if (!hardware_frame_input_active_
            && !hardware_frame_input_activation_failed_
            && has_native_texture
            && hardware_input_plan_.has_value()) {
            std::string capture_adapter_summary;
            std::string hardware_input_block_reason;
            if (!is_native_frame_backend_vendor_compatible(
                    frame,
                    selected_backend_,
                    &capture_adapter_summary,
                    &hardware_input_block_reason)) {
                hardware_frame_input_activation_failed_ = true;
                diagnostics_.hardware_input_block_reason = hardware_input_block_reason;
            }
            if (!capture_adapter_summary.empty()) {
                diagnostics_.capture_adapter_summary = capture_adapter_summary;
            }
        }
        CapturedFrame gpu_scaled_frame;
        bool gpu_scaled = false;
        std::string gpu_scale_error;
#ifdef _WIN32
        if ((input_scaled || source_cropped)
            && has_native_texture
            && !hardware_frame_input_activation_failed_
            && hardware_input_plan_.has_value()) {
            gpu_scaled = d3d11_scaler_.scale(
                frame,
                profile_.width,
                profile_.height,
                &gpu_scaled_frame,
                &gpu_scale_error);
            if (!gpu_scaled && !gpu_scale_error.empty()) {
                diagnostics_.hardware_input_block_reason =
                    "D3D11 video processor scaling unavailable; CPU swscale/readback fallback: "
                    + gpu_scale_error;
            }
        }
#endif
        const CapturedFrame& hardware_candidate = gpu_scaled ? gpu_scaled_frame : frame;
        const bool hardware_candidate_scaled =
            hardware_candidate.width != profile_.width
            || hardware_candidate.height != profile_.height
            || (source_cropped && !gpu_scaled);

        if (!hardware_frame_input_active_
            && !hardware_frame_input_activation_failed_
            && !hardware_candidate_scaled
            && unwrap_d3d11_native_handle(hardware_candidate) != nullptr
            && hardware_input_plan_.has_value()) {
            diagnostics_.hardware_input_block_reason.clear();
            const std::uint64_t saved_pts = next_pts_;
            std::string hardware_error;
            if (open_runtime_context(true, &hardware_candidate, &hardware_error)) {
                next_pts_ = saved_pts;
            } else {
                const std::string reopen_error = hardware_error;
                if (!open_runtime_context(false, nullptr, &hardware_error)) {
                    ++diagnostics_.dropped_frame_count;
                    diagnostics_.total_encode_us += static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - encode_start)
                            .count());
                    return fail(
                        EncoderExecutionFailureCategory::kEncoderInitFailed,
                        "failed to activate D3D11 hardware-frame input and CPU fallback reopen also failed: "
                            + reopen_error + "; " + hardware_error,
                        error_detail);
                }
                next_pts_ = saved_pts;
                hardware_frame_input_activation_failed_ = true;
                diagnostics_.hardware_input_block_reason =
                    "D3D11 video-processor output could not activate hardware-frame encoding; "
                    "CPU swscale/readback fallback: " + reopen_error;
                diagnostics_.last_error_detail = reopen_error;
            }
        }

        if (hardware_frame_input_active_ && (input_scaled || source_cropped) && !gpu_scaled) {
            if (reopen_cpu_fallback_after_hardware_failure(
                    gpu_scale_error.empty()
                        ? "D3D11 video processor scaling became unavailable"
                        : gpu_scale_error,
                    error_detail)) {
                return encode_bgra_frame(frame, timestamp_ms, packet, error_detail);
            }
            return false;
        }

        diagnostics_.d3d11_video_processor_scaling_active =
            hardware_frame_input_active_ && gpu_scaled;
        diagnostics_.gpu_to_cpu_readback_active =
            !hardware_frame_input_active_
            && input_scaled
            && has_cpu_pixels
            && unwrap_d3d11_native_handle(frame) != nullptr;

        std::string convert_error;
        const auto input_prepare_start = std::chrono::steady_clock::now();
        bool prepared_frame = false;
        if (hardware_frame_input_active_) {
            prepared_frame = prepare_hardware_input_frame(hardware_candidate, &convert_error);
            diagnostics_.input_mode = gpu_scaled
                ? (hardware_frame_transfer_required_
                    ? "d3d11-video-processor-scale-transfer"
                    : "d3d11-video-processor-scale-direct")
                : (hardware_frame_transfer_required_
                    ? "hwframe-transfer"
                    : "d3d11-hwframe-direct");
        } else {
            if (!has_cpu_pixels) {
                ++diagnostics_.dropped_frame_count;
                diagnostics_.total_encode_us += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - encode_start)
                        .count());
                return fail(
                    EncoderExecutionFailureCategory::kInvalidConfig,
                    "input BGRA frame buffer is unavailable for CPU encode path",
                    error_detail);
            }

            const int writable_result = av_frame_make_writable(frame_);
            if (writable_result < 0) {
                return fail(
                    EncoderExecutionFailureCategory::kEncodeFailed,
                    "av_frame_make_writable failed: " + ffmpeg_error_to_string(writable_result),
                    error_detail);
            }

            diagnostics_.input_mode = describe_encoder_cpu_input_mode(
                static_cast<AVPixelFormat>(frame_->format),
                input_scaled || source_cropped);
            prepared_frame = convert_bgra_to_encoder_frame(
                frame,
                frame_,
#if REDCLAW_CAPTURE_HAS_LIBSWSCALE
                &sws_context_,
#endif
                &convert_error);
        }
        diagnostics_.total_input_prepare_us += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - input_prepare_start)
                .count());
        if (!hardware_frame_input_active_) {
            diagnostics_.total_bgra_to_yuv_us += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - input_prepare_start)
                    .count());
        }

        if (!prepared_frame) {
            diagnostics_.total_encode_us += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - encode_start)
                    .count());
            const std::string failure_detail = convert_error.empty() ? "input frame preparation failed" : convert_error;
            if (hardware_frame_input_active_) {
                if (reopen_cpu_fallback_after_hardware_failure(failure_detail, error_detail)) {
                    return encode_bgra_frame(frame, timestamp_ms, packet, error_detail);
                }
                ++diagnostics_.dropped_frame_count;
                return false;
            }
            ++diagnostics_.dropped_frame_count;
            return fail(
                EncoderExecutionFailureCategory::kEncodeFailed,
                failure_detail,
                error_detail);
        }
        const bool force_keyframe = keyframe_requested_.exchange(false);
        frame_->pict_type = force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
#if defined(AV_FRAME_FLAG_KEY)
        if (force_keyframe) {
            frame_->flags |= AV_FRAME_FLAG_KEY;
        } else {
            frame_->flags &= ~AV_FRAME_FLAG_KEY;
        }
#endif
        const auto submission_time = std::chrono::steady_clock::now();
        if (next_pts_ == 0) {
            pts_origin_ = submission_time;
        }
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            submission_time - pts_origin_).count();
        frame_->pts = std::max(static_cast<std::int64_t>(next_pts_),
            av_rescale_q(elapsed_us, AVRational{1, 1000000}, context_->time_base));
        next_pts_ = static_cast<std::uint64_t>(frame_->pts + 1);
        diagnostics_.last_submitted_pts = frame_->pts;

        const auto send_start = std::chrono::steady_clock::now();
        const int send_result = avcodec_send_frame(context_, frame_);
        diagnostics_.total_send_frame_us += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - send_start)
                .count());
        if (send_result == AVERROR(EAGAIN)) {
            const ReceivePacketResult blocked_receive_result = try_receive_encoded_packet(
                packet,
                timestamp_ms,
                error_detail);
            diagnostics_.total_encode_us += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - encode_start)
                    .count());
            if (blocked_receive_result == ReceivePacketResult::kPacketReady) {
                const auto retry_send_start = std::chrono::steady_clock::now();
                const int retry_send_result = avcodec_send_frame(context_, frame_);
                diagnostics_.total_send_frame_us += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - retry_send_start)
                        .count());
                if (retry_send_result >= 0) {
                    submitted_frame_timestamps_.emplace_back(frame_->pts, timestamp_ms);
                } else {
                    ++diagnostics_.dropped_frame_count;
                    if (hardware_frame_input_active_) {
                        const std::string failure_detail = "avcodec_send_frame retry failed: "
                            + ffmpeg_error_to_string(retry_send_result);
                        std::string fallback_error;
                        reopen_cpu_fallback_after_hardware_failure(failure_detail, &fallback_error);
                    }
                }
                if (hardware_frame_input_active_) {
                    av_frame_unref(frame_);
                }
                return true;
            }
            if (blocked_receive_result == ReceivePacketResult::kNeedInput) {
                if (hardware_frame_input_active_) {
                    av_frame_unref(frame_);
                }
                return fail(
                    EncoderExecutionFailureCategory::kOutputNotReady,
                    "avcodec_send_frame input not accepted before output drain (EAGAIN)",
                    error_detail);
            }
            if (hardware_frame_input_active_) {
                av_frame_unref(frame_);
            }
            return false;
        }
        if (send_result < 0) {
            diagnostics_.total_encode_us += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - encode_start)
                    .count());
            const std::string failure_detail = "avcodec_send_frame failed: " + ffmpeg_error_to_string(send_result);
            if (hardware_frame_input_active_) {
                av_frame_unref(frame_);
                if (reopen_cpu_fallback_after_hardware_failure(failure_detail, error_detail)) {
                    return encode_bgra_frame(frame, timestamp_ms, packet, error_detail);
                }
                ++diagnostics_.dropped_frame_count;
                return false;
            }
            ++diagnostics_.dropped_frame_count;
            return fail(
                EncoderExecutionFailureCategory::kEncodeFailed,
                failure_detail,
                error_detail);
        }
        submitted_frame_timestamps_.emplace_back(frame_->pts, timestamp_ms);
        if (hardware_frame_input_active_) {
            av_frame_unref(frame_);
        }

        const bool hardware_input_was_active = hardware_frame_input_active_;
        const ReceivePacketResult receive_result = try_receive_encoded_packet(
            packet,
            timestamp_ms,
            error_detail);
        diagnostics_.total_encode_us += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - encode_start)
                .count());
        if (receive_result == ReceivePacketResult::kPacketReady) {
            return true;
        }
        if (receive_result == ReceivePacketResult::kNeedInput) {
            return false;
        }
        if (hardware_input_was_active && !hardware_frame_input_active_) {
            return encode_bgra_frame(frame, timestamp_ms, packet, error_detail);
        }
        return false;
#else
        (void)frame;
        (void)timestamp_ms;
        return fail(
            EncoderExecutionFailureCategory::kLibavcodecUnavailable,
            "libavcodec headers are unavailable at build time; encode path is disabled",
            error_detail);
#endif
    }

    void stop() {
#ifdef _WIN32
        d3d11_scaler_.reset();
#endif
#if REDCLAW_CAPTURE_HAS_LIBAVCODEC
        if (packet_ != nullptr) {
            av_packet_free(&packet_);
            packet_ = nullptr;
        }
        if (source_hw_frame_ != nullptr) {
            av_frame_free(&source_hw_frame_);
            source_hw_frame_ = nullptr;
        }
        if (frame_ != nullptr) {
            av_frame_free(&frame_);
            frame_ = nullptr;
        }
        if (context_ != nullptr) {
            avcodec_free_context(&context_);
            context_ = nullptr;
        }
        if (encoder_hw_frames_context_ != nullptr) {
            av_buffer_unref(&encoder_hw_frames_context_);
            encoder_hw_frames_context_ = nullptr;
        }
        if (encoder_hw_device_context_ != nullptr) {
            av_buffer_unref(&encoder_hw_device_context_);
            encoder_hw_device_context_ = nullptr;
        }
        if (base_d3d11_frames_context_ != nullptr) {
            av_buffer_unref(&base_d3d11_frames_context_);
            base_d3d11_frames_context_ = nullptr;
        }
        if (base_d3d11_device_context_ != nullptr) {
            av_buffer_unref(&base_d3d11_device_context_);
            base_d3d11_device_context_ = nullptr;
        }
#if REDCLAW_CAPTURE_HAS_LIBSWSCALE
        if (sws_context_ != nullptr) {
            sws_freeContext(sws_context_);
            sws_context_ = nullptr;
        }
#endif
        codec_ = nullptr;
#endif
        selected_encoder_name_.clear();
        diagnostics_.encoder_name.clear();
        diagnostics_.input_mode.clear();
        diagnostics_.capture_adapter_summary.clear();
        diagnostics_.hardware_frame_input_active = false;
        diagnostics_.hardware_input_block_reason.clear();
        direct_bgra_input_ = false;
        hardware_frame_input_active_ = false;
        hardware_frame_input_allowed_ = true;
        hardware_frame_input_activation_failed_ = false;
        hardware_frame_transfer_required_ = false;
        hardware_input_plan_.reset();
        selected_backend_ = EncoderBackendType::kSoftware;
        next_pts_ = 0;
        keyframe_requested_.store(false);
        diagnostics_.initialized = false;
    }

    [[nodiscard]] bool is_running() const {
#if REDCLAW_CAPTURE_HAS_LIBAVCODEC
        return context_ != nullptr && frame_ != nullptr && packet_ != nullptr;
#else
        return false;
#endif
    }

    [[nodiscard]] EncoderExecutionDiagnostics diagnostics() const {
        EncoderExecutionDiagnostics snapshot = diagnostics_;
        snapshot.keyframe_request_count = keyframe_request_count_.load();
        return snapshot;
    }

    void request_keyframe() {
        keyframe_requested_.store(true);
        ++keyframe_request_count_;
    }

    EncoderRateControlUpdateStatus update_rate_control(
        std::uint32_t target_bitrate_kbps,
        std::uint32_t max_bitrate_kbps,
        std::string* error_detail) {
        ++diagnostics_.rate_control_update_attempt_count;
        if (target_bitrate_kbps == 0 || max_bitrate_kbps < target_bitrate_kbps) {
            ++diagnostics_.rate_control_update_failure_count;
            if (error_detail != nullptr) {
                *error_detail = "rate-control update requires a non-zero target and max >= target";
            }
            return EncoderRateControlUpdateStatus::kFailed;
        }
#if REDCLAW_CAPTURE_HAS_LIBAVCODEC
        if (!is_running() || codec_ == nullptr) {
            ++diagnostics_.rate_control_update_failure_count;
            if (error_detail != nullptr) {
                *error_detail = "rate-control update requires a running encoder";
            }
            return EncoderRateControlUpdateStatus::kFailed;
        }
        if ((codec_->capabilities & AV_CODEC_CAP_PARAM_CHANGE) == 0) {
            ++diagnostics_.rate_control_update_unsupported_count;
            diagnostics_.rate_control_hot_update_supported = false;
            if (error_detail != nullptr) {
                *error_detail = "active FFmpeg encoder does not advertise AV_CODEC_CAP_PARAM_CHANGE";
            }
            return EncoderRateControlUpdateStatus::kUnsupported;
        }

        const std::int64_t previous_bitrate = context_->bit_rate;
        const std::int64_t previous_max_rate = context_->rc_max_rate;
        const std::int64_t target_bitrate =
            static_cast<std::int64_t>(target_bitrate_kbps) * 1000LL;
        const std::int64_t max_bitrate =
            static_cast<std::int64_t>(max_bitrate_kbps) * 1000LL;
        const int target_result = av_opt_set_int(context_, "b", target_bitrate, 0);
        const int max_result = av_opt_set_int(context_, "maxrate", max_bitrate, 0);
        if (target_result < 0 || max_result < 0) {
            (void)av_opt_set_int(context_, "b", previous_bitrate, 0);
            (void)av_opt_set_int(context_, "maxrate", previous_max_rate, 0);
            context_->bit_rate = previous_bitrate;
            context_->rc_max_rate = previous_max_rate;
            ++diagnostics_.rate_control_update_failure_count;
            if (error_detail != nullptr) {
                *error_detail = "FFmpeg rate-control option update failed target="
                    + ffmpeg_error_to_string(target_result)
                    + " max=" + ffmpeg_error_to_string(max_result);
            }
            return EncoderRateControlUpdateStatus::kFailed;
        }

        context_->bit_rate = target_bitrate;
        context_->rc_max_rate = max_bitrate;
        profile_.target_bitrate_kbps = target_bitrate_kbps;
        profile_.max_bitrate_kbps = max_bitrate_kbps;
        diagnostics_.rate_control_hot_update_supported = true;
        diagnostics_.active_target_bitrate_kbps = target_bitrate_kbps;
        diagnostics_.active_max_bitrate_kbps = max_bitrate_kbps;
        ++diagnostics_.rate_control_update_success_count;
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return EncoderRateControlUpdateStatus::kApplied;
#else
        ++diagnostics_.rate_control_update_unsupported_count;
        diagnostics_.rate_control_hot_update_supported = false;
        if (error_detail != nullptr) {
            *error_detail = "libavcodec is unavailable";
        }
        return EncoderRateControlUpdateStatus::kUnsupported;
#endif
    }

private:
    enum class ReceivePacketResult {
        kPacketReady,
        kNeedInput,
        kFailed,
    };

    bool fail(EncoderExecutionFailureCategory category, std::string detail, std::string* error_detail) {
        diagnostics_.last_failure = category;
        diagnostics_.last_error_detail = detail;
        if (error_detail != nullptr) {
            *error_detail = diagnostics_.last_error_detail;
        }
        return false;
    }

#if REDCLAW_CAPTURE_HAS_LIBAVCODEC
    std::uint64_t consume_submitted_frame_timestamp(std::int64_t packet_pts, std::uint64_t fallback_timestamp_ms) {
        if (submitted_frame_timestamps_.empty()) {
            return fallback_timestamp_ms;
        }

        if (packet_pts != AV_NOPTS_VALUE) {
            for (auto iter = submitted_frame_timestamps_.begin(); iter != submitted_frame_timestamps_.end(); ++iter) {
                if (iter->first == packet_pts) {
                    const std::uint64_t timestamp_ms = iter->second;
                    auto erase_end = iter;
                    ++erase_end;
                    submitted_frame_timestamps_.erase(submitted_frame_timestamps_.begin(), erase_end);
                    return timestamp_ms;
                }
            }
        }

        const std::uint64_t timestamp_ms = submitted_frame_timestamps_.front().second;
        submitted_frame_timestamps_.pop_front();
        return timestamp_ms;
    }

    ReceivePacketResult try_receive_encoded_packet(
        EncodedFramePacket* packet,
        std::uint64_t fallback_timestamp_ms,
        std::string* error_detail) {
        const auto receive_start = std::chrono::steady_clock::now();
        const int receive_result = avcodec_receive_packet(context_, packet_);
        diagnostics_.total_receive_packet_us += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - receive_start)
                .count());

        if (receive_result == AVERROR(EAGAIN)) {
            ++diagnostics_.backpressure_event_count;
            fail(
                EncoderExecutionFailureCategory::kOutputNotReady,
                "avcodec_receive_packet output not ready (EAGAIN)",
                error_detail);
            return ReceivePacketResult::kNeedInput;
        }

        if (receive_result == AVERROR_EOF) {
            const std::string failure_detail = "avcodec_receive_packet reached EOF";
            if (hardware_frame_input_active_) {
                if (!reopen_cpu_fallback_after_hardware_failure(failure_detail, error_detail)) {
                    ++diagnostics_.dropped_frame_count;
                }
                return ReceivePacketResult::kFailed;
            }
            ++diagnostics_.dropped_frame_count;
            fail(
                EncoderExecutionFailureCategory::kEncodeFailed,
                failure_detail,
                error_detail);
            return ReceivePacketResult::kFailed;
        }

        if (receive_result < 0) {
            const std::string failure_detail = "avcodec_receive_packet failed: "
                + ffmpeg_error_to_string(receive_result);
            if (hardware_frame_input_active_) {
                if (!reopen_cpu_fallback_after_hardware_failure(failure_detail, error_detail)) {
                    ++diagnostics_.dropped_frame_count;
                }
                return ReceivePacketResult::kFailed;
            }
            ++diagnostics_.dropped_frame_count;
            fail(
                EncoderExecutionFailureCategory::kEncodeFailed,
                failure_detail,
                error_detail);
            return ReceivePacketResult::kFailed;
        }

        packet->codec = profile_.codec;
        packet->keyframe = packet_contains_keyframe(profile_.codec, packet_);
        packet->timestamp_ms = consume_submitted_frame_timestamp(packet_->pts, fallback_timestamp_ms);
        const auto payload_copy_start = std::chrono::steady_clock::now();
        packet->payload.assign(packet_->data, packet_->data + packet_->size);
        diagnostics_.total_payload_copy_us += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - payload_copy_start)
                .count());

        av_packet_unref(packet_);

        ++diagnostics_.encoded_frame_count;
        if (packet->keyframe) {
            ++diagnostics_.encoded_keyframe_count;
        }
        diagnostics_.last_failure = EncoderExecutionFailureCategory::kNone;
        diagnostics_.last_error_detail.clear();
        if (hardware_frame_input_active_) {
            diagnostics_.hardware_input_block_reason.clear();
        }
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return ReceivePacketResult::kPacketReady;
    }

    bool reopen_cpu_fallback_after_hardware_failure(
        const std::string& failure_detail,
        std::string* error_detail) {
        if (!hardware_frame_input_active_) {
            return fail(EncoderExecutionFailureCategory::kEncodeFailed, failure_detail, error_detail);
        }

        const std::uint64_t saved_pts = next_pts_;
        hardware_frame_input_activation_failed_ = true;
        std::string fallback_error;
        if (!open_runtime_context(false, nullptr, &fallback_error)) {
            next_pts_ = saved_pts;
            return fail(
                EncoderExecutionFailureCategory::kEncodeFailed,
                "hardware-frame input failed and CPU fallback reopen failed: "
                    + failure_detail + "; " + fallback_error,
                error_detail);
        }

        next_pts_ = saved_pts;
        diagnostics_.hardware_input_block_reason = "hardware-frame input disabled after encode failure: " + failure_detail;
        diagnostics_.last_failure = EncoderExecutionFailureCategory::kNone;
        diagnostics_.last_error_detail.clear();
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool open_runtime_context(
        bool prefer_hardware_frame_input,
        const CapturedFrame* native_frame,
        std::string* error_detail) {
        if (codec_ == nullptr) {
            return fail(
                EncoderExecutionFailureCategory::kEncoderInitFailed,
                "encoder codec is unavailable",
                error_detail);
        }

        if (prefer_hardware_frame_input && native_frame == nullptr) {
            return fail(
                EncoderExecutionFailureCategory::kInvalidConfig,
                "native-frame encoder initialization requires a captured frame",
                error_detail);
        }

        stop_runtime_context_only();

        context_ = avcodec_alloc_context3(codec_);
        if (context_ == nullptr) {
            return fail(
                EncoderExecutionFailureCategory::kEncoderInitFailed,
                "avcodec_alloc_context3 returned null",
                error_detail);
        }

        context_->width = static_cast<int>(profile_.width);
        context_->height = static_cast<int>(profile_.height);
        context_->time_base = AVRational{1, static_cast<int>(profile_.fps)};
        context_->framerate = AVRational{static_cast<int>(profile_.fps), 1};
        context_->gop_size = static_cast<int>(profile_.gop_length_frames);
        context_->max_b_frames = static_cast<int>(profile_.b_frames);
        context_->bit_rate = static_cast<std::int64_t>(profile_.target_bitrate_kbps) * 1000LL;
        context_->rc_max_rate = static_cast<std::int64_t>(profile_.max_bitrate_kbps) * 1000LL;
        context_->thread_count = 1;
        context_->flags |= AV_CODEC_FLAG_LOW_DELAY;

        direct_bgra_input_ = false;
        hardware_frame_input_active_ = false;
        hardware_frame_transfer_required_ = false;

        if (prefer_hardware_frame_input) {
            if (!initialize_hardware_frame_bridge(*native_frame, error_detail)) {
                stop_runtime_context_only();
                return false;
            }

            context_->pix_fmt = hardware_input_plan_.value().pixel_format;
            context_->hw_frames_ctx = av_buffer_ref(encoder_hw_frames_context_);
            if (context_->hw_frames_ctx == nullptr) {
                stop_runtime_context_only();
                return fail(
                    EncoderExecutionFailureCategory::kEncoderInitFailed,
                    "av_buffer_ref returned null for encoder hw_frames_ctx",
                    error_detail);
            }
            if (encoder_hw_device_context_ != nullptr) {
                context_->hw_device_ctx = av_buffer_ref(encoder_hw_device_context_);
                if (context_->hw_device_ctx == nullptr) {
                    stop_runtime_context_only();
                    return fail(
                        EncoderExecutionFailureCategory::kEncoderInitFailed,
                        "av_buffer_ref returned null for encoder hw_device_ctx",
                        error_detail);
                }
            }
        } else {
            context_->pix_fmt = resolve_encoder_cpu_pixel_format(codec_, selected_backend_);
            direct_bgra_input_ = context_->pix_fmt == AV_PIX_FMT_BGRA
                || context_->pix_fmt == AV_PIX_FMT_BGR0;
        }

        const bool supports_x26x_tuning = selected_encoder_name_ == "libx264" || selected_encoder_name_ == "libx265";
        if (profile_.zero_latency_tuning && supports_x26x_tuning && context_->priv_data != nullptr) {
            av_opt_set(context_->priv_data, "tune", "zerolatency", 0);
            av_opt_set(context_->priv_data, "preset", "ultrafast", 0);
            av_opt_set(context_->priv_data, "rc-lookahead", "0", 0);
        }
        const bool supports_nvenc_tuning = selected_encoder_name_ == "h264_nvenc" || selected_encoder_name_ == "hevc_nvenc";
        if (profile_.zero_latency_tuning && supports_nvenc_tuning && context_->priv_data != nullptr) {
            av_opt_set(context_->priv_data, "preset", "p1", 0);
            av_opt_set(context_->priv_data, "tune", "ull", 0);
            av_opt_set(context_->priv_data, "zerolatency", "1", 0);
            av_opt_set(context_->priv_data, "delay", "0", 0);
            av_opt_set(context_->priv_data, "rc-lookahead", "0", 0);
            av_opt_set(context_->priv_data, "rc", "cbr", 0);
        }
        const bool supports_qsv_tuning = selected_encoder_name_ == "h264_qsv" || selected_encoder_name_ == "hevc_qsv";
        if (profile_.zero_latency_tuning && supports_qsv_tuning && context_->priv_data != nullptr) {
            av_opt_set(context_->priv_data, "async_depth", "1", 0);
            av_opt_set(context_->priv_data, "look_ahead", "0", 0);
            const auto low_delay_brc = profile_.workload == EncoderWorkload::kInteractiveDesktop ? 0 : 1;
            if (av_opt_set_int(context_->priv_data, "low_delay_brc", low_delay_brc, 0) < 0) {
                stop_runtime_context_only();
                return fail(EncoderExecutionFailureCategory::kEncoderInitFailed,
                    "QSV low_delay_brc option could not be applied", error_detail);
            }
            av_opt_set(context_->priv_data, "forced_idr", "1", 0);
        }
        const bool supports_amf_tuning = selected_encoder_name_ == "h264_amf" || selected_encoder_name_ == "hevc_amf";
        if (profile_.zero_latency_tuning && supports_amf_tuning && context_->priv_data != nullptr) {
            av_opt_set(context_->priv_data, "usage", "ultralowlatency", 0);
            av_opt_set(context_->priv_data, "preanalysis", "false", 0);
        }
        if (profile_.repeat_headers && context_->priv_data != nullptr) {
            av_opt_set(context_->priv_data, "repeat_headers", "1", 0);
            av_opt_set(context_->priv_data, "repeat-headers", "1", 0);
            av_opt_set(context_->priv_data, "annexb", "1", 0);
        }

        const int open_result = avcodec_open2(context_, codec_, nullptr);
        if (open_result < 0) {
            const std::string error = "avcodec_open2 failed backend="
                + describe_encoder_backend(selected_backend_)
                + " encoder="
                + (selected_encoder_name_.empty() ? std::string("n/a") : selected_encoder_name_)
                + ": "
                + ffmpeg_error_to_string(open_result);
            stop_runtime_context_only();
            return fail(EncoderExecutionFailureCategory::kEncoderInitFailed, error, error_detail);
        }
        diagnostics_.configured_fps = profile_.fps;
        diagnostics_.configured_bitrate_kbps = static_cast<std::uint32_t>(context_->bit_rate / 1000);
        if (supports_qsv_tuning) {
            std::int64_t value = -1;
            diagnostics_.qsv_low_delay_brc_verified =
                av_opt_get_int(context_->priv_data, "low_delay_brc", 0, &value) >= 0;
            diagnostics_.qsv_low_delay_brc = value == 1;
        }
        if (context_->max_b_frames != 0) {
            stop_runtime_context_only();
            return fail(
                EncoderExecutionFailureCategory::kEncoderInitFailed,
                "encoder backend did not honor the zero B-frame requirement",
                error_detail);
        }
        diagnostics_.rate_control_hot_update_supported =
            (codec_->capabilities & AV_CODEC_CAP_PARAM_CHANGE) != 0;
        diagnostics_.active_target_bitrate_kbps = profile_.target_bitrate_kbps;
        diagnostics_.active_max_bitrate_kbps = profile_.max_bitrate_kbps;

        frame_ = av_frame_alloc();
        if (frame_ == nullptr) {
            stop_runtime_context_only();
            return fail(
                EncoderExecutionFailureCategory::kEncoderInitFailed,
                "av_frame_alloc returned null",
                error_detail);
        }

        frame_->format = context_->pix_fmt;
        frame_->width = context_->width;
        frame_->height = context_->height;

        if (prefer_hardware_frame_input && hardware_frame_transfer_required_) {
            source_hw_frame_ = av_frame_alloc();
            if (source_hw_frame_ == nullptr) {
                stop_runtime_context_only();
                return fail(
                    EncoderExecutionFailureCategory::kEncoderInitFailed,
                    "av_frame_alloc returned null for source hardware frame",
                    error_detail);
            }
        } else if (!prefer_hardware_frame_input) {
            const int frame_buffer_result = av_frame_get_buffer(frame_, 32);
            if (frame_buffer_result < 0) {
                stop_runtime_context_only();
                return fail(
                    EncoderExecutionFailureCategory::kEncoderInitFailed,
                    "av_frame_get_buffer failed: " + ffmpeg_error_to_string(frame_buffer_result),
                    error_detail);
            }
        }

        packet_ = av_packet_alloc();
        if (packet_ == nullptr) {
            stop_runtime_context_only();
            return fail(
                EncoderExecutionFailureCategory::kEncoderInitFailed,
                "av_packet_alloc returned null",
                error_detail);
        }

        diagnostics_.encoder_name = selected_encoder_name_;
        diagnostics_.input_mode = prefer_hardware_frame_input
            ? (hardware_frame_transfer_required_
                ? hardware_input_plan_.value().input_mode + "-transfer"
                : hardware_input_plan_.value().input_mode + "-direct")
            : describe_encoder_cpu_input_mode(context_->pix_fmt, false);
        diagnostics_.hardware_frame_input_active = prefer_hardware_frame_input;
        if (prefer_hardware_frame_input) {
            diagnostics_.hardware_input_block_reason.clear();
        }
        diagnostics_.initialized = true;
        diagnostics_.last_failure = EncoderExecutionFailureCategory::kNone;
        diagnostics_.last_error_detail.clear();
        hardware_frame_input_active_ = prefer_hardware_frame_input;
        keyframe_requested_.store(true);

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    void stop_runtime_context_only() {
        drain_encoder_before_close();
        submitted_frame_timestamps_.clear();
        if (packet_ != nullptr) {
            av_packet_free(&packet_);
            packet_ = nullptr;
        }
        if (source_hw_frame_ != nullptr) {
            av_frame_free(&source_hw_frame_);
            source_hw_frame_ = nullptr;
        }
        if (frame_ != nullptr) {
            av_frame_free(&frame_);
            frame_ = nullptr;
        }
        if (context_ != nullptr) {
            avcodec_free_context(&context_);
            context_ = nullptr;
        }
        if (encoder_hw_frames_context_ != nullptr) {
            av_buffer_unref(&encoder_hw_frames_context_);
            encoder_hw_frames_context_ = nullptr;
        }
        if (encoder_hw_device_context_ != nullptr) {
            av_buffer_unref(&encoder_hw_device_context_);
            encoder_hw_device_context_ = nullptr;
        }
        if (base_d3d11_frames_context_ != nullptr) {
            av_buffer_unref(&base_d3d11_frames_context_);
            base_d3d11_frames_context_ = nullptr;
        }
        if (base_d3d11_device_context_ != nullptr) {
            av_buffer_unref(&base_d3d11_device_context_);
            base_d3d11_device_context_ = nullptr;
        }
#if REDCLAW_CAPTURE_HAS_LIBSWSCALE
        if (sws_context_ != nullptr) {
            sws_freeContext(sws_context_);
            sws_context_ = nullptr;
        }
#endif
        direct_bgra_input_ = false;
        hardware_frame_input_active_ = false;
        hardware_frame_transfer_required_ = false;
    }

    void drain_encoder_before_close() {
        if (context_ == nullptr || packet_ == nullptr) {
            return;
        }

        const int send_result = avcodec_send_frame(context_, nullptr);
        if (send_result < 0 && send_result != AVERROR_EOF && send_result != AVERROR(EAGAIN)) {
            return;
        }

        for (int drain_count = 0; drain_count < 128; ++drain_count) {
            const int receive_result = avcodec_receive_packet(context_, packet_);
            if (receive_result < 0) {
                break;
            }
            av_packet_unref(packet_);
        }
    }

    bool initialize_hardware_frame_bridge(const CapturedFrame& frame, std::string* error_detail) {
#if !defined(_WIN32) || !REDCLAW_CAPTURE_HAS_D3D11_HWCONTEXT
        (void)frame;
        return fail(
            EncoderExecutionFailureCategory::kEncoderInitFailed,
            "D3D11 hardware-frame bridge is unavailable in this build",
            error_detail);
#else
        if (!hardware_input_plan_.has_value()) {
            return fail(
                EncoderExecutionFailureCategory::kEncoderInitFailed,
                "selected encoder does not expose a hardware-frame input plan",
                error_detail);
        }

        auto* native = unwrap_d3d11_native_handle(frame);
        if (native == nullptr || native->d3d11_device == nullptr || native->d3d11_texture == nullptr) {
            return fail(
                EncoderExecutionFailureCategory::kEncoderInitFailed,
                "captured frame does not carry a D3D11 texture handle",
                error_detail);
        }

        if (frame.width != profile_.width || frame.height != profile_.height) {
            return fail(
                EncoderExecutionFailureCategory::kEncoderInitFailed,
                "D3D11 hardware-frame input requires native capture dimensions",
                error_detail);
        }

        base_d3d11_device_context_ = create_d3d11_device_context_from_existing_device(
            native->d3d11_device.Get(),
            error_detail);
        if (base_d3d11_device_context_ == nullptr) {
            return false;
        }

        base_d3d11_frames_context_ = create_d3d11_frames_context(
            base_d3d11_device_context_,
            profile_.width,
            profile_.height,
            AV_PIX_FMT_D3D11,
            AV_PIX_FMT_BGRA,
            error_detail);
        if (base_d3d11_frames_context_ == nullptr) {
            return false;
        }

        if (hardware_input_plan_.value().device_type == AV_HWDEVICE_TYPE_D3D11VA
            && hardware_input_plan_.value().pixel_format == AV_PIX_FMT_D3D11) {
            encoder_hw_device_context_ = av_buffer_ref(base_d3d11_device_context_);
            encoder_hw_frames_context_ = av_buffer_ref(base_d3d11_frames_context_);
            hardware_frame_transfer_required_ = false;
        } else {
            const int derive_device_result = av_hwdevice_ctx_create_derived(
                &encoder_hw_device_context_,
                hardware_input_plan_.value().device_type,
                base_d3d11_device_context_,
                0);
            if (derive_device_result < 0) {
                return fail(
                    EncoderExecutionFailureCategory::kEncoderInitFailed,
                    "av_hwdevice_ctx_create_derived failed for "
                        + hw_device_type_to_string(hardware_input_plan_.value().device_type)
                        + ": "
                        + ffmpeg_error_to_string(derive_device_result),
                    error_detail);
            }

            const int derive_frames_result = av_hwframe_ctx_create_derived(
                &encoder_hw_frames_context_,
                hardware_input_plan_.value().pixel_format,
                encoder_hw_device_context_,
                base_d3d11_frames_context_,
                0);
            if (derive_frames_result < 0) {
                return fail(
                    EncoderExecutionFailureCategory::kEncoderInitFailed,
                    "av_hwframe_ctx_create_derived failed for "
                        + hw_device_type_to_string(hardware_input_plan_.value().device_type)
                        + ": "
                        + ffmpeg_error_to_string(derive_frames_result),
                    error_detail);
            }

            hardware_frame_transfer_required_ = true;
        }

        if (encoder_hw_device_context_ == nullptr || encoder_hw_frames_context_ == nullptr) {
            return fail(
                EncoderExecutionFailureCategory::kEncoderInitFailed,
                "hardware frame bridge initialization returned null contexts",
                error_detail);
        }

        return true;
#endif
    }

    bool prepare_hardware_input_frame(const CapturedFrame& frame, std::string* error_detail) {
#if !defined(_WIN32) || !REDCLAW_CAPTURE_HAS_D3D11_HWCONTEXT
        (void)frame;
        return fail(
            EncoderExecutionFailureCategory::kEncodeFailed,
            "D3D11 hardware-frame input is unavailable in this build",
            error_detail);
#else
        if (!hardware_frame_input_active_ || frame_ == nullptr || encoder_hw_frames_context_ == nullptr) {
            return fail(
                EncoderExecutionFailureCategory::kEncodeFailed,
                "hardware-frame encoder input is not initialized",
                error_detail);
        }

        if (hardware_frame_transfer_required_) {
            if (source_hw_frame_ == nullptr || base_d3d11_frames_context_ == nullptr) {
                return fail(
                    EncoderExecutionFailureCategory::kEncodeFailed,
                    "source D3D11 frame context is unavailable",
                    error_detail);
            }

            av_frame_unref(source_hw_frame_);
            const int source_buffer_result = av_hwframe_get_buffer(base_d3d11_frames_context_, source_hw_frame_, 0);
            if (source_buffer_result < 0) {
                return fail(
                    EncoderExecutionFailureCategory::kEncodeFailed,
                    "av_hwframe_get_buffer failed for D3D11 source frame: "
                        + ffmpeg_error_to_string(source_buffer_result),
                    error_detail);
            }

            if (!copy_d3d11_native_texture_to_frame(frame, source_hw_frame_, error_detail)) {
                return false;
            }

            av_frame_unref(frame_);
            const int encoder_buffer_result = av_hwframe_get_buffer(encoder_hw_frames_context_, frame_, 0);
            if (encoder_buffer_result < 0) {
                return fail(
                    EncoderExecutionFailureCategory::kEncodeFailed,
                    "av_hwframe_get_buffer failed for encoder hardware frame: "
                        + ffmpeg_error_to_string(encoder_buffer_result),
                    error_detail);
            }

            const int transfer_result = av_hwframe_transfer_data(frame_, source_hw_frame_, 0);
            if (transfer_result < 0) {
                return fail(
                    EncoderExecutionFailureCategory::kEncodeFailed,
                    "av_hwframe_transfer_data failed: " + ffmpeg_error_to_string(transfer_result),
                    error_detail);
            }
        } else {
            av_frame_unref(frame_);
            const int encoder_buffer_result = av_hwframe_get_buffer(encoder_hw_frames_context_, frame_, 0);
            if (encoder_buffer_result < 0) {
                return fail(
                    EncoderExecutionFailureCategory::kEncodeFailed,
                    "av_hwframe_get_buffer failed for D3D11 encoder frame: "
                        + ffmpeg_error_to_string(encoder_buffer_result),
                    error_detail);
            }

            if (!copy_d3d11_native_texture_to_frame(frame, frame_, error_detail)) {
                return false;
            }
        }

        frame_->width = context_->width;
        frame_->height = context_->height;
        frame_->format = context_->pix_fmt;
        return true;
#endif
    }

#if defined(_WIN32) && REDCLAW_CAPTURE_HAS_D3D11_HWCONTEXT
    AVBufferRef* create_d3d11_device_context_from_existing_device(
        ID3D11Device* device,
        std::string* error_detail) {
        if (device == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "D3D11 hardware-frame bridge requires a non-null device";
            }
            return nullptr;
        }

        AVBufferRef* device_context = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (device_context == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "av_hwdevice_ctx_alloc returned null for D3D11VA";
            }
            return nullptr;
        }

        auto* hw_device_context = reinterpret_cast<AVHWDeviceContext*>(device_context->data);
        auto* d3d11_hw_context = reinterpret_cast<AVD3D11VADeviceContext*>(hw_device_context->hwctx);
        device->AddRef();
        d3d11_hw_context->device = device;

        ComPtr<ID3D11DeviceContext> immediate_context;
        device->GetImmediateContext(&immediate_context);
        if (immediate_context == nullptr) {
            av_buffer_unref(&device_context);
            if (error_detail != nullptr) {
                *error_detail = "failed to query D3D11 immediate context";
            }
            return nullptr;
        }

        immediate_context.Get()->AddRef();
        d3d11_hw_context->device_context = immediate_context.Get();
        const int init_result = av_hwdevice_ctx_init(device_context);
        if (init_result < 0) {
            av_buffer_unref(&device_context);
            if (error_detail != nullptr) {
                *error_detail = "av_hwdevice_ctx_init failed for D3D11VA: "
                    + ffmpeg_error_to_string(init_result);
            }
            return nullptr;
        }

        return device_context;
    }

    AVBufferRef* create_d3d11_frames_context(
        AVBufferRef* device_context,
        std::uint32_t width,
        std::uint32_t height,
        AVPixelFormat hardware_pixel_format,
        AVPixelFormat software_pixel_format,
        std::string* error_detail) {
        if (device_context == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "hardware frames context requires a non-null device context";
            }
            return nullptr;
        }

        AVBufferRef* frames_context = av_hwframe_ctx_alloc(device_context);
        if (frames_context == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "av_hwframe_ctx_alloc returned null";
            }
            return nullptr;
        }

        auto* hw_frames_context = reinterpret_cast<AVHWFramesContext*>(frames_context->data);
        hw_frames_context->format = hardware_pixel_format;
        hw_frames_context->sw_format = software_pixel_format;
        hw_frames_context->width = static_cast<int>(width);
        hw_frames_context->height = static_cast<int>(height);
        hw_frames_context->initial_pool_size = kD3D11HardwareFramePoolSize;

        const int init_result = av_hwframe_ctx_init(frames_context);
        if (init_result < 0) {
            av_buffer_unref(&frames_context);
            if (error_detail != nullptr) {
                *error_detail = "av_hwframe_ctx_init failed for D3D11 frames: "
                    + ffmpeg_error_to_string(init_result);
            }
            return nullptr;
        }

        return frames_context;
    }

    bool copy_d3d11_native_texture_to_frame(
        const CapturedFrame& frame,
        AVFrame* destination_frame,
        std::string* error_detail) {
        auto* native = unwrap_d3d11_native_handle(frame);
        if (native == nullptr || native->d3d11_device == nullptr || native->d3d11_texture == nullptr) {
            return fail(
                EncoderExecutionFailureCategory::kEncodeFailed,
                "captured frame does not provide a D3D11 texture",
                error_detail);
        }

        if (destination_frame == nullptr || destination_frame->data[0] == nullptr) {
            return fail(
                EncoderExecutionFailureCategory::kEncodeFailed,
                "destination hardware frame does not contain a D3D11 texture",
                error_detail);
        }

        auto* destination_texture = reinterpret_cast<ID3D11Texture2D*>(destination_frame->data[0]);
        const std::uint32_t destination_subresource = destination_frame->data[1] == nullptr
            ? 0U
            : static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(destination_frame->data[1]));

        D3D11_TEXTURE2D_DESC source_desc{};
        native->d3d11_texture->GetDesc(&source_desc);
        D3D11_TEXTURE2D_DESC destination_desc{};
        destination_texture->GetDesc(&destination_desc);
        if (source_desc.Width != destination_desc.Width || source_desc.Height != destination_desc.Height) {
            return fail(
                EncoderExecutionFailureCategory::kEncodeFailed,
                "source and destination D3D11 texture dimensions differ",
                error_detail);
        }

        ComPtr<ID3D11DeviceContext> immediate_context;
        native->d3d11_device->GetImmediateContext(&immediate_context);
        if (immediate_context == nullptr) {
            return fail(
                EncoderExecutionFailureCategory::kEncodeFailed,
                "failed to query D3D11 immediate context for texture upload",
                error_detail);
        }

        immediate_context->CopySubresourceRegion(
            destination_texture,
            destination_subresource,
            0,
            0,
            0,
            native->d3d11_texture.Get(),
            native->d3d11_subresource_index,
            nullptr);

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }
#endif
#endif

    EncoderConfigProfile profile_{};
    EncoderExecutionDiagnostics diagnostics_{};
    std::string selected_encoder_name_;
    EncoderBackendType selected_backend_ = EncoderBackendType::kSoftware;
    bool direct_bgra_input_ = false;
    bool hardware_frame_input_active_ = false;
    bool hardware_frame_input_allowed_ = true;
    bool hardware_frame_input_activation_failed_ = false;
    bool hardware_frame_transfer_required_ = false;
#ifdef _WIN32
    D3D11VideoProcessorScaler d3d11_scaler_;
#endif
    std::uint64_t next_pts_ = 0;
    std::chrono::steady_clock::time_point pts_origin_{};
    std::atomic_bool keyframe_requested_{false};
    std::atomic<std::uint32_t> keyframe_request_count_{0};
    std::deque<std::pair<std::int64_t, std::uint64_t>> submitted_frame_timestamps_;

#if REDCLAW_CAPTURE_HAS_LIBAVCODEC
    const AVCodec* codec_ = nullptr;
    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* source_hw_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    std::optional<EncoderHardwareInputPlan> hardware_input_plan_;
    AVBufferRef* base_d3d11_device_context_ = nullptr;
    AVBufferRef* base_d3d11_frames_context_ = nullptr;
    AVBufferRef* encoder_hw_device_context_ = nullptr;
    AVBufferRef* encoder_hw_frames_context_ = nullptr;
#if REDCLAW_CAPTURE_HAS_LIBSWSCALE
    SwsContext* sws_context_ = nullptr;
#endif
#endif
};

EncoderExecutionSession::EncoderExecutionSession()
    : impl_(std::make_unique<Impl>()) {}

EncoderExecutionSession::~EncoderExecutionSession() = default;

bool EncoderExecutionSession::start(
    const EncoderConfigProfile& profile,
    const EncoderBackendBridgePlan& bridge_plan,
    std::string* error_detail) {
    return impl_->start(profile, bridge_plan, error_detail);
}

bool EncoderExecutionSession::encode_bgra_frame(
    const CapturedFrame& frame,
    std::uint64_t timestamp_ms,
    EncodedFramePacket* packet,
    std::string* error_detail) {
    return impl_->encode_bgra_frame(frame, timestamp_ms, packet, error_detail);
}

void EncoderExecutionSession::request_keyframe() {
    impl_->request_keyframe();
}

EncoderRateControlUpdateStatus EncoderExecutionSession::update_rate_control(
    std::uint32_t target_bitrate_kbps,
    std::uint32_t max_bitrate_kbps,
    std::string* error_detail) {
    return impl_->update_rate_control(
        target_bitrate_kbps,
        max_bitrate_kbps,
        error_detail);
}

void EncoderExecutionSession::stop() {
    impl_->stop();
}

bool EncoderExecutionSession::is_running() const {
    return impl_->is_running();
}

EncoderExecutionDiagnostics EncoderExecutionSession::diagnostics() const {
    return impl_->diagnostics();
}

bool start_encoder_execution_from_bridge(
    const EncoderProfileRequest& profile_request,
    const EncoderBackendBridgeRequest& bridge_request,
    EncoderExecutionSession* session,
    EncoderBackendBridgePlan* resolved_plan,
    std::string* error_detail) {
    if (session == nullptr || resolved_plan == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "encoder execution bridge start requires non-null session and resolved_plan";
        }
        return false;
    }

    EncoderConfigProfile profile;
    if (!build_low_latency_encoder_profile(profile_request, &profile, error_detail)) {
        return false;
    }

    return start_encoder_execution_from_bridge(
        profile,
        bridge_request,
        session,
        resolved_plan,
        error_detail);
}

bool start_encoder_execution_from_bridge(
    const EncoderConfigProfile& profile,
    const EncoderBackendBridgeRequest& bridge_request,
    EncoderExecutionSession* session,
    EncoderBackendBridgePlan* resolved_plan,
    std::string* error_detail) {
    if (session == nullptr || resolved_plan == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "encoder execution bridge start requires non-null session and resolved_plan";
        }
        return false;
    }

    EncoderBackendCapabilities capabilities;
    if (!detect_encoder_backend_capabilities(&capabilities, error_detail)) {
        return false;
    }

    EncoderBackendCapabilities remaining_capabilities = capabilities;
    std::string combined_error;
    std::size_t attempt_count = 0;

    while (true) {
        EncoderBackendBridgePlan attempt_plan;
        std::string plan_error;
        if (!build_encoder_backend_bridge_plan(bridge_request, remaining_capabilities, &attempt_plan, &plan_error)) {
            if (error_detail != nullptr) {
                if (combined_error.empty()) {
                    *error_detail = plan_error;
                } else if (plan_error.empty()) {
                    *error_detail = combined_error;
                } else {
                    *error_detail = combined_error + "; " + plan_error;
                }
            }
            return false;
        }

        std::string start_error;
        if (session->start(profile, attempt_plan, &start_error)) {
            *resolved_plan = attempt_plan;
            if (attempt_count > 0) {
                resolved_plan->fallback_applied = true;
                resolved_plan->reason += " after runtime start fallback: " + combined_error;
            }
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

        if (!combined_error.empty()) {
            combined_error += "; ";
        }
        combined_error += describe_encoder_backend(attempt_plan.selected_backend) + " encoder start failed";
        if (!start_error.empty()) {
            combined_error += ": " + start_error;
        }

        if (!is_hardware_backend(attempt_plan.selected_backend)
            || (!bridge_request.allow_hardware_fallback && bridge_request.preferred_backend != EncoderBackendType::kAuto)) {
            if (error_detail != nullptr) {
                *error_detail = combined_error;
            }
            return false;
        }

        disable_backend_capability(attempt_plan.selected_backend, &remaining_capabilities);
        ++attempt_count;
    }
}

CaptureFallbackDecision evaluate_capture_fallback_decision(
    const CaptureHealthSignals& signals,
    const CaptureSessionConfig& config) {
    CaptureFallbackDecision decision;

    if (!config.fallback_enabled) {
        return decision;
    }

    if (signals.backend != CaptureBackendType::kDesktopDuplication) {
        return decision;
    }

    if (signals.consecutive_failures >= config.max_consecutive_failures_before_fallback) {
        decision.should_switch_backend = true;
        decision.reason = "consecutive capture failure threshold exceeded";
        return decision;
    }

    return decision;
}

CaptureGeometryUpdateDecision evaluate_capture_geometry_update(
    std::uint32_t current_width,
    std::uint32_t current_height,
    std::uint32_t next_width,
    std::uint32_t next_height,
    std::uint64_t current_revision) {
    CaptureGeometryUpdateDecision decision;
    decision.next_revision = current_revision;
    if (next_width == 0 || next_height == 0) {
        return decision;
    }

    decision.valid = true;
    decision.changed = current_width != next_width || current_height != next_height;
    if (!decision.changed) {
        return decision;
    }

    if (current_revision == 0) {
        decision.next_revision = 1;
    } else if (current_revision < std::numeric_limits<std::uint64_t>::max()) {
        decision.next_revision = current_revision + 1;
    }
    return decision;
}

bool run_capture_stability_probe(
    const CaptureStabilityRunConfig& config,
    CaptureStabilityRunResult* result,
    std::string* error_detail) {
    if (result == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "result output must be non-null";
        }
        return false;
    }

    *result = CaptureStabilityRunResult{};

    if (config.run_duration_seconds == 0) {
        result->error = "run_duration_seconds must be greater than zero";
        if (error_detail != nullptr) {
            *error_detail = result->error;
        }
        return false;
    }

    WindowsCaptureSession session;
    std::string start_error;
    if (!session.start(config.capture_config, &start_error)) {
        result->error = start_error;
        if (error_detail != nullptr) {
            *error_detail = start_error;
        }
        return false;
    }

    result->backend = session.activeBackend();

    const auto begin = std::chrono::steady_clock::now();
    const auto deadline = begin + std::chrono::seconds(config.run_duration_seconds);

    std::uint32_t consecutive_timeouts = 0;
    std::uint32_t consecutive_failures = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        ++result->capture_attempts;

        CapturedFrame frame;
        std::string capture_error;
        if (session.captureFrame(&frame, &capture_error)) {
            ++result->frames_captured;
            consecutive_timeouts = 0;
            consecutive_failures = 0;
            continue;
        }

        if (capture_error == "timeout waiting for desktop frame") {
            ++result->timeout_count;
            ++consecutive_timeouts;
            if (consecutive_timeouts > result->max_consecutive_timeouts_seen) {
                result->max_consecutive_timeouts_seen = consecutive_timeouts;
            }

            if (consecutive_timeouts > config.max_consecutive_timeouts) {
                result->error = "exceeded max consecutive capture timeouts";
                break;
            }
            continue;
        }

        ++result->failure_count;
        ++consecutive_failures;
        if (consecutive_failures > result->max_consecutive_failures_seen) {
            result->max_consecutive_failures_seen = consecutive_failures;
        }

        if (consecutive_failures > config.max_consecutive_failures) {
            result->error = "exceeded max consecutive capture failures: " + capture_error;
            break;
        }
    }

    const auto end = std::chrono::steady_clock::now();
    result->run_duration_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());

    if (result->run_duration_ms > 0) {
        result->average_fps =
            static_cast<double>(result->frames_captured) * 1000.0 / static_cast<double>(result->run_duration_ms);
    }

    const auto telemetry = session.telemetry();
    result->backend = telemetry.active_backend;
    result->backend_switch_count = telemetry.backend_switch_count;
    result->fallback_attempt_count = telemetry.fallback_attempt_count;
    result->stale_frame_count = telemetry.stale_frame_count;
    result->black_frame_ratio = telemetry.black_frame_ratio;
    result->frame_present_delta_ms = telemetry.frame_present_delta_ms;

    session.stop();

    if (result->frames_captured == 0 && result->error.empty()) {
        result->error = "stability run finished without captured frames";
    }

    result->ok = result->error.empty();

    if (error_detail != nullptr) {
        *error_detail = result->error;
    }

    return result->ok;
}

#ifdef _WIN32

namespace {

using Microsoft::WRL::ComPtr;

struct CaptureFrameStageTelemetry {
    std::uint64_t wait_us = 0;
    std::uint64_t copy_us = 0;
    std::uint32_t accumulated_frames = 0;
    bool native_texture_pool_created = false;
    bool native_texture_pool_reused = false;
    bool native_texture_pool_exhausted = false;
    std::uint64_t frame_pool_recreate_us = 0;
    std::uint32_t frame_pool_recreate_count = 0;
    bool timeout = false;
};

class ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;
    virtual bool start(const CaptureSessionConfig& config, std::string* error_detail) = 0;
    virtual bool capture_frame(
        CapturedFrame* frame,
        CaptureFrameStageTelemetry* stage_telemetry,
        std::string* error_detail) = 0;
    virtual void configure_native_frame_delivery(
        bool capture_native_d3d11_textures,
        bool skip_cpu_readback_when_native_texture_available) {
        (void)capture_native_d3d11_textures;
        (void)skip_cpu_readback_when_native_texture_available;
    }
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
    virtual CaptureBackendType backend_type() const = 0;
};

class DdaCaptureBackend final : public ICaptureBackend {
public:
    bool start(const CaptureSessionConfig& config, std::string* error_detail) override {
        stop();

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
        constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };

        ComPtr<IDXGIFactory1> factory;
        const HRESULT factory_hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter1> selected_adapter;
        const HRESULT adapter_enum_hr = SUCCEEDED(factory_hr)
            ? factory->EnumAdapters1(config.adapter_index, &selected_adapter)
            : factory_hr;
        if (FAILED(adapter_enum_hr) || selected_adapter == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "failed to enumerate DXGI adapter index="
                    + std::to_string(config.adapter_index);
            }
            return false;
        }
        ComPtr<IDXGIAdapter> adapter;
        selected_adapter.As(&adapter);

        const HRESULT create_hr = D3D11CreateDevice(
            adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            0,
            kFeatureLevels,
            ARRAYSIZE(kFeatureLevels),
            D3D11_SDK_VERSION,
            &device,
            &feature_level,
            &context);
        if (FAILED(create_hr)) {
            if (error_detail != nullptr) {
                *error_detail = "D3D11CreateDevice failed hr="
                    + format_hex_u32(static_cast<std::uint32_t>(create_hr));
            }
            return false;
        }

        DXGI_ADAPTER_DESC adapter_desc{};
        const HRESULT adapter_desc_hr = adapter->GetDesc(&adapter_desc);
        const std::string adapter_summary = SUCCEEDED(adapter_desc_hr)
            ? wide_to_utf8(adapter_desc.Description)
            : "unknown-adapter";

        ComPtr<IDXGIOutput> output;
        const HRESULT output_hr = adapter->EnumOutputs(config.output_index, &output);
        if (FAILED(output_hr) || output == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "failed to enumerate DXGI output index="
                    + std::to_string(config.output_index)
                    + " adapter=\"" + adapter_summary + "\" hr="
                    + format_hex_u32(static_cast<std::uint32_t>(output_hr));
            }
            return false;
        }

        DXGI_OUTPUT_DESC output_desc{};
        const HRESULT output_desc_hr = output->GetDesc(&output_desc);
        const std::string output_summary = SUCCEEDED(output_desc_hr)
            ? wide_to_utf8(output_desc.DeviceName)
            : "unknown-output";

        ComPtr<IDXGIOutput1> output1;
        const HRESULT output1_hr = output.As(&output1);
        if (FAILED(output1_hr) || output1 == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "failed to query IDXGIOutput1 output=\""
                    + output_summary + "\" hr="
                    + format_hex_u32(static_cast<std::uint32_t>(output1_hr));
            }
            return false;
        }

        ComPtr<IDXGIOutputDuplication> duplication;
        const HRESULT dup_hr = output1->DuplicateOutput(device.Get(), &duplication);
        if (FAILED(dup_hr) || duplication == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "DuplicateOutput failed adapter=\"" + adapter_summary
                    + "\" output=\"" + output_summary + "\" hr="
                    + format_hex_u32(static_cast<std::uint32_t>(dup_hr));
            }
            return false;
        }

        device_ = std::move(device);
        context_ = std::move(context);
        duplication_ = std::move(duplication);
        if (SUCCEEDED(output_desc_hr)) {
            desktop_origin_x_ = output_desc.DesktopCoordinates.left;
            desktop_origin_y_ = output_desc.DesktopCoordinates.top;
            desktop_width_ = static_cast<std::uint32_t>(
                output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left);
            desktop_height_ = static_cast<std::uint32_t>(
                output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top);
            switch (output_desc.Rotation) {
            case DXGI_MODE_ROTATION_ROTATE90: desktop_rotation_ = 90; break;
            case DXGI_MODE_ROTATION_ROTATE180: desktop_rotation_ = 180; break;
            case DXGI_MODE_ROTATION_ROTATE270: desktop_rotation_ = 270; break;
            default: desktop_rotation_ = 0; break;
            }
            desktop_geometry_revision_ = 1;
        }
        frame_acquire_timeout_ms_ = config.frame_acquire_timeout_ms;
        capture_native_d3d11_textures_.store(config.capture_native_d3d11_textures);
        skip_cpu_readback_when_native_texture_available_.store(
            config.skip_cpu_readback_when_native_texture_available);
        return true;
    }

    bool capture_frame(
        CapturedFrame* frame,
        CaptureFrameStageTelemetry* stage_telemetry,
        std::string* error_detail) override {
        if (stage_telemetry != nullptr) {
            *stage_telemetry = CaptureFrameStageTelemetry{};
        }
        if (frame == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "frame pointer must be non-null";
            }
            return false;
        }

        if (!is_running()) {
            if (error_detail != nullptr) {
                *error_detail = "capture backend is not running";
            }
            return false;
        }

        DXGI_OUTDUPL_FRAME_INFO frame_info{};
        ComPtr<IDXGIResource> desktop_resource;
        const auto acquire_start = std::chrono::steady_clock::now();
        const HRESULT acquire_hr = duplication_->AcquireNextFrame(frame_acquire_timeout_ms_, &frame_info, &desktop_resource);
        const auto wait_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - acquire_start)
                .count());
        if (stage_telemetry != nullptr) {
            stage_telemetry->wait_us = wait_us;
            stage_telemetry->accumulated_frames = frame_info.AccumulatedFrames;
        }

        if (acquire_hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (stage_telemetry != nullptr) {
                stage_telemetry->timeout = true;
            }
            if (error_detail != nullptr) {
                *error_detail = "timeout waiting for desktop frame";
            }
            return false;
        }
        if (FAILED(acquire_hr) || desktop_resource == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "AcquireNextFrame failed";
            }
            return false;
        }

        const auto copy_start = std::chrono::steady_clock::now();
        auto store_copy_us = [&]() {
            if (stage_telemetry != nullptr) {
                stage_telemetry->copy_us = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - copy_start)
                        .count());
            }
        };

        ComPtr<ID3D11Texture2D> frame_texture;
        if (FAILED(desktop_resource.As(&frame_texture)) || frame_texture == nullptr) {
            duplication_->ReleaseFrame();
            store_copy_us();
            if (error_detail != nullptr) {
                *error_detail = "failed to query frame texture";
            }
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        frame_texture->GetDesc(&desc);

        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            duplication_->ReleaseFrame();
            store_copy_us();
            if (error_detail != nullptr) {
                *error_detail = "captured frame is not BGRA (DXGI_FORMAT_B8G8R8A8_UNORM)";
            }
            return false;
        }

        frame->width = desc.Width;
        frame->height = desc.Height;
        frame->bgra = true;
        frame->row_pitch = desc.Width * 4;
        frame->desktop_origin_x = desktop_origin_x_;
        frame->desktop_origin_y = desktop_origin_y_;
        frame->desktop_width = desktop_width_ == 0 ? desc.Width : desktop_width_;
        frame->desktop_height = desktop_height_ == 0 ? desc.Height : desktop_height_;
        frame->desktop_rotation = desktop_rotation_;
        frame->desktop_geometry_revision = desktop_geometry_revision_ == 0 ? 1 : desktop_geometry_revision_;
        frame->native_handle_type = CapturedFrameNativeHandleType::kNone;
        frame->native_handle.reset();

        bool native_frame_ready = false;
        if (capture_native_d3d11_textures_.load()) {
            D3D11_TEXTURE2D_DESC native_desc = desc;
            // A video-processor input view does not accept a texture whose only
            // bind flag is D3D11_BIND_SHADER_RESOURCE.  BindFlags == 0 is
            // explicitly supported and the texture is only used as a
            // CopyResource destination before VideoProcessorBlt/encoder copy.
            native_desc.BindFlags = 0;
            native_desc.CPUAccessFlags = 0;
            native_desc.Usage = D3D11_USAGE_DEFAULT;
            native_desc.MiscFlags = 0;

            auto native_handle = acquire_native_frame_handle(native_desc, stage_telemetry);
            if (native_handle != nullptr) {
                context_->CopyResource(native_handle->d3d11_texture.Get(), frame_texture.Get());
                frame->native_handle_type = CapturedFrameNativeHandleType::kD3D11Texture2D;
                frame->native_handle = std::move(native_handle);
                native_frame_ready = true;
            }
        }

        if (skip_cpu_readback_when_native_texture_available_.load() && native_frame_ready) {
            frame->row_pitch = 0;
            frame->data.clear();
            duplication_->ReleaseFrame();
            store_copy_us();
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

        auto reset_staging_texture = [&]() {
            staging_texture_.Reset();
            staging_texture_width_ = 0;
            staging_texture_height_ = 0;
            staging_texture_format_ = DXGI_FORMAT_UNKNOWN;
        };

        auto ensure_staging_texture = [&]() -> bool {
            if (staging_texture_ != nullptr
                && staging_texture_width_ == desc.Width
                && staging_texture_height_ == desc.Height
                && staging_texture_format_ == desc.Format) {
                return true;
            }

            D3D11_TEXTURE2D_DESC staging_desc = desc;
            staging_desc.BindFlags = 0;
            staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging_desc.Usage = D3D11_USAGE_STAGING;
            staging_desc.MiscFlags = 0;

            ComPtr<ID3D11Texture2D> staging_texture;
            if (FAILED(device_->CreateTexture2D(&staging_desc, nullptr, &staging_texture)) || staging_texture == nullptr) {
                return false;
            }

            staging_texture_ = std::move(staging_texture);
            staging_texture_width_ = desc.Width;
            staging_texture_height_ = desc.Height;
            staging_texture_format_ = desc.Format;
            return true;
        };

        auto map_staging_texture = [&](D3D11_MAPPED_SUBRESOURCE* mapped) -> bool {
            if (mapped == nullptr || staging_texture_ == nullptr) {
                return false;
            }

            context_->CopyResource(staging_texture_.Get(), frame_texture.Get());
            const HRESULT map_hr = context_->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, mapped);
            return SUCCEEDED(map_hr) && mapped->pData != nullptr;
        };

        if (!ensure_staging_texture()) {
            duplication_->ReleaseFrame();
            store_copy_us();
            if (error_detail != nullptr) {
                *error_detail = "failed to create staging texture";
            }
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!map_staging_texture(&mapped)) {
            reset_staging_texture();
            duplication_->ReleaseFrame();
            store_copy_us();
            if (error_detail != nullptr) {
                *error_detail = "failed to map staging texture";
            }
            return false;
        }

        {
            frame->data.resize(static_cast<std::size_t>(frame->row_pitch) * frame->height);

            const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
            for (std::uint32_t y = 0; y < frame->height; ++y) {
                const auto* src_row = src + static_cast<std::size_t>(y) * mapped.RowPitch;
                auto* dst_row = frame->data.data() + static_cast<std::size_t>(y) * frame->row_pitch;
                std::memcpy(dst_row, src_row, frame->row_pitch);
            }
        }

        context_->Unmap(staging_texture_.Get(), 0);
        duplication_->ReleaseFrame();
        store_copy_us();

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    void configure_native_frame_delivery(
        bool capture_native_d3d11_textures,
        bool skip_cpu_readback_when_native_texture_available) override {
        capture_native_d3d11_textures_.store(capture_native_d3d11_textures);
        skip_cpu_readback_when_native_texture_available_.store(
            skip_cpu_readback_when_native_texture_available);
    }

    void stop() override {
        staging_texture_.Reset();
        native_frame_pool_.clear();
        duplication_.Reset();
        context_.Reset();
        device_.Reset();
        staging_texture_width_ = 0;
        staging_texture_height_ = 0;
        staging_texture_format_ = DXGI_FORMAT_UNKNOWN;
        native_texture_width_ = 0;
        native_texture_height_ = 0;
        native_texture_format_ = DXGI_FORMAT_UNKNOWN;
        desktop_origin_x_ = 0;
        desktop_origin_y_ = 0;
        desktop_width_ = 0;
        desktop_height_ = 0;
        desktop_rotation_ = 0;
        desktop_geometry_revision_ = 0;
    }

    bool is_running() const override {
        return duplication_ != nullptr && context_ != nullptr && device_ != nullptr;
    }

    CaptureBackendType backend_type() const override {
        return CaptureBackendType::kDesktopDuplication;
    }

private:
    std::shared_ptr<CapturedFrameNativeHandle> acquire_native_frame_handle(
        const D3D11_TEXTURE2D_DESC& desc,
        CaptureFrameStageTelemetry* stage_telemetry) {
        if (native_texture_width_ != desc.Width
            || native_texture_height_ != desc.Height
            || native_texture_format_ != desc.Format) {
            native_frame_pool_.clear();
            native_texture_width_ = desc.Width;
            native_texture_height_ = desc.Height;
            native_texture_format_ = desc.Format;
        }

        for (const auto& handle : native_frame_pool_) {
            if (handle.use_count() == 1) {
                if (stage_telemetry != nullptr) {
                    stage_telemetry->native_texture_pool_reused = true;
                }
                return handle;
            }
        }

        if (native_frame_pool_.size() >= kDdaNativeTexturePoolSize) {
            if (stage_telemetry != nullptr) {
                stage_telemetry->native_texture_pool_exhausted = true;
            }
            return nullptr;
        }

        ComPtr<ID3D11Texture2D> native_texture;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &native_texture)) || native_texture == nullptr) {
            return nullptr;
        }

        auto handle = make_d3d11_native_handle(
            device_.Get(), native_texture.Get(), desc.Format, 0);
        native_frame_pool_.push_back(handle);
        if (stage_telemetry != nullptr) {
            stage_telemetry->native_texture_pool_created = true;
        }
        return handle;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> staging_texture_;
    std::vector<std::shared_ptr<CapturedFrameNativeHandle>> native_frame_pool_;
    std::uint32_t frame_acquire_timeout_ms_ = 250;
    std::uint32_t staging_texture_width_ = 0;
    std::uint32_t staging_texture_height_ = 0;
    DXGI_FORMAT staging_texture_format_ = DXGI_FORMAT_UNKNOWN;
    std::uint32_t native_texture_width_ = 0;
    std::uint32_t native_texture_height_ = 0;
    DXGI_FORMAT native_texture_format_ = DXGI_FORMAT_UNKNOWN;
    std::int32_t desktop_origin_x_ = 0;
    std::int32_t desktop_origin_y_ = 0;
    std::uint32_t desktop_width_ = 0;
    std::uint32_t desktop_height_ = 0;
    std::uint32_t desktop_rotation_ = 0;
    std::uint64_t desktop_geometry_revision_ = 0;
    std::atomic_bool capture_native_d3d11_textures_ = false;
    std::atomic_bool skip_cpu_readback_when_native_texture_available_ = false;
};

class WgcThreadApartment final {
public:
    WgcThreadApartment() {
        result_ = RoInitialize(RO_INIT_MULTITHREADED);
        owns_initialization_ = SUCCEEDED(result_);
    }

    ~WgcThreadApartment() {
        if (owns_initialization_) {
            winrt::clear_factory_cache();
            RoUninitialize();
        }
    }

    bool usable() const {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

    HRESULT result() const { return result_; }

private:
    HRESULT result_ = E_FAIL;
    bool owns_initialization_ = false;
};

WgcThreadApartment& current_wgc_thread_apartment() {
    thread_local WgcThreadApartment apartment;
    return apartment;
}

class WgcCaptureBackend final : public ICaptureBackend {
public:
    bool start(const CaptureSessionConfig& config, std::string* error_detail) override {
        stop();

        const auto& apartment = current_wgc_thread_apartment();
        if (!apartment.usable()) {
            if (error_detail != nullptr) {
                *error_detail = "RoInitialize failed for Windows Graphics Capture hr="
                    + format_hex_u32(static_cast<std::uint32_t>(apartment.result()));
            }
            return false;
        }

        winrt::hresult_error capture_session_factory_error{E_FAIL};
        const auto capture_session_factory = winrt::try_get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureSession,
            winrt::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics>(
            capture_session_factory_error);
        if (capture_session_factory == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "Windows Graphics Capture support query failed hr="
                    + format_hex_u32(static_cast<std::uint32_t>(capture_session_factory_error.code()));
            }
            return false;
        }
        bool capture_supported = false;
        using CaptureSessionStaticsAbi = winrt::impl::abi_t<
            winrt::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics>;
        auto* capture_session_factory_abi = reinterpret_cast<CaptureSessionStaticsAbi*>(
            winrt::get_abi(capture_session_factory));
        const HRESULT capture_supported_hr = capture_session_factory_abi->IsSupported(&capture_supported);
        if (FAILED(capture_supported_hr) || !capture_supported) {
            if (error_detail != nullptr) {
                *error_detail = FAILED(capture_supported_hr)
                    ? "Windows Graphics Capture support query failed hr="
                        + format_hex_u32(static_cast<std::uint32_t>(capture_supported_hr))
                    : "Windows Graphics Capture is not supported";
            }
            return false;
        }

        try {

            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
            constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0,
            };
            ComPtr<IDXGIFactory1> factory;
            winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            ComPtr<IDXGIAdapter1> selected_adapter;
            winrt::check_hresult(factory->EnumAdapters1(
                config.adapter_index, &selected_adapter));
            ComPtr<IDXGIAdapter> adapter;
            winrt::check_hresult(selected_adapter.As(&adapter));
            winrt::check_hresult(D3D11CreateDevice(
                adapter.Get(),
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                kFeatureLevels,
                ARRAYSIZE(kFeatureLevels),
                D3D11_SDK_VERSION,
                &device,
                &feature_level,
                &context));
            ComPtr<IDXGIDevice> dxgi_device;
            winrt::check_hresult(device.As(&dxgi_device));

            ComPtr<IDXGIOutput> output;
            winrt::check_hresult(adapter->EnumOutputs(config.output_index, &output));

            DXGI_OUTPUT_DESC output_desc{};
            winrt::check_hresult(output->GetDesc(&output_desc));
            if (output_desc.Monitor == nullptr || !output_desc.AttachedToDesktop) {
                throw winrt::hresult_error(E_INVALIDARG, L"DXGI output is not attached to the desktop");
            }

            const auto item_factory = winrt::get_activation_factory<
                winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                IGraphicsCaptureItemInterop>();
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem capture_item{nullptr};
            winrt::check_hresult(item_factory->CreateForMonitor(
                output_desc.Monitor,
                winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(capture_item)));

            winrt::com_ptr<IInspectable> inspectable_device;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
                dxgi_device.Get(), inspectable_device.put()));
            auto direct3d_device = inspectable_device.as<
                winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

            const auto item_size = capture_item.Size();
            if (item_size.Width <= 0 || item_size.Height <= 0) {
                throw winrt::hresult_error(E_INVALIDARG, L"capture item has invalid dimensions");
            }

            const auto frame_pool_factory = winrt::get_activation_factory<
                winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool,
                winrt::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2>();
            auto frame_pool = frame_pool_factory.CreateFreeThreaded(
                direct3d_device,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                kWgcFramePoolSize,
                item_size);
            auto capture_session = frame_pool.CreateCaptureSession(capture_item);

            device_ = std::move(device);
            context_ = std::move(context);
            direct3d_device_ = std::move(direct3d_device);
            output_ = std::move(output);
            capture_item_ = std::move(capture_item);
            frame_pool_ = std::move(frame_pool);
            capture_session_ = std::move(capture_session);
            frame_acquire_timeout_ms_ = config.frame_acquire_timeout_ms;
            capture_native_d3d11_textures_.store(config.capture_native_d3d11_textures);
            skip_cpu_readback_when_native_texture_available_.store(
                config.skip_cpu_readback_when_native_texture_available);
            stopping_ = false;
            recreating_frame_pool_ = false;
            callback_hresult_ = S_OK;
            frame_pool_width_ = static_cast<std::uint32_t>(item_size.Width);
            frame_pool_height_ = static_cast<std::uint32_t>(item_size.Height);

            desktop_origin_x_ = output_desc.DesktopCoordinates.left;
            desktop_origin_y_ = output_desc.DesktopCoordinates.top;
            desktop_width_ = static_cast<std::uint32_t>(item_size.Width);
            desktop_height_ = static_cast<std::uint32_t>(item_size.Height);
            switch (output_desc.Rotation) {
            case DXGI_MODE_ROTATION_ROTATE90: desktop_rotation_ = 90; break;
            case DXGI_MODE_ROTATION_ROTATE180: desktop_rotation_ = 180; break;
            case DXGI_MODE_ROTATION_ROTATE270: desktop_rotation_ = 270; break;
            default: desktop_rotation_ = 0; break;
            }
            desktop_geometry_revision_ = 1;

            frame_arrived_token_ = frame_pool_.FrameArrived(
                [this](
                    const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& sender,
                    const winrt::Windows::Foundation::IInspectable&) noexcept {
                    on_frame_arrived(sender);
                });
            capture_session_.StartCapture();
        } catch (const winrt::hresult_error& exception) {
            const HRESULT start_hr = exception.code();
            const std::string message = wide_to_utf8(exception.message().c_str());
            stop();
            if (error_detail != nullptr) {
                *error_detail = "Windows Graphics Capture start failed hr="
                    + format_hex_u32(static_cast<std::uint32_t>(start_hr))
                    + (message.empty() ? std::string{} : " detail=\"" + message + "\"");
            }
            return false;
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool capture_frame(
        CapturedFrame* frame,
        CaptureFrameStageTelemetry* stage_telemetry,
        std::string* error_detail) override {
        if (stage_telemetry != nullptr) {
            *stage_telemetry = CaptureFrameStageTelemetry{};
        }
        if (frame == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "frame pointer must be non-null";
            }
            return false;
        }
        if (!is_running()) {
            if (error_detail != nullptr) {
                *error_detail = "Windows Graphics Capture backend not running";
            }
            return false;
        }

        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame captured_frame{nullptr};
        std::uint32_t content_width = 0;
        std::uint32_t content_height = 0;
        std::uint32_t recreate_attempts = 0;
        const auto acquire_start = std::chrono::steady_clock::now();
        auto acquire_deadline = acquire_start + std::chrono::milliseconds(frame_acquire_timeout_ms_);
        while (true) {
            std::uint32_t accumulated_frames = 0;
            {
                std::unique_lock lock(frame_mutex_);
                if (!frame_ready_.wait_until(
                        lock,
                        acquire_deadline,
                        [this]() {
                            return stopping_ || pending_frame_ != nullptr || FAILED(callback_hresult_);
                        })) {
                    if (stage_telemetry != nullptr) {
                        stage_telemetry->timeout = true;
                        stage_telemetry->wait_us = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - acquire_start)
                                .count());
                    }
                    if (error_detail != nullptr) {
                        *error_detail = "timeout waiting for desktop frame";
                    }
                    return false;
                }

                if (FAILED(callback_hresult_)) {
                    const HRESULT callback_hr = callback_hresult_;
                    callback_hresult_ = S_OK;
                    if (error_detail != nullptr) {
                        *error_detail = "Windows Graphics Capture frame callback failed hr="
                            + format_hex_u32(static_cast<std::uint32_t>(callback_hr));
                    }
                    return false;
                }
                if (stopping_ || pending_frame_ == nullptr) {
                    if (error_detail != nullptr) {
                        *error_detail = "Windows Graphics Capture stopped while waiting for a frame";
                    }
                    return false;
                }

                captured_frame = std::move(pending_frame_);
                pending_frame_ = nullptr;
                accumulated_frames = pending_frame_count_;
                pending_frame_count_ = 0;
            }

            if (stage_telemetry != nullptr) {
                stage_telemetry->accumulated_frames += accumulated_frames;
            }

            winrt::Windows::Graphics::SizeInt32 content_size{};
            try {
                content_size = captured_frame.ContentSize();
            } catch (const winrt::hresult_error& exception) {
                captured_frame.Close();
                captured_frame = nullptr;
                if (error_detail != nullptr) {
                    *error_detail = "Windows Graphics Capture content-size query failed hr="
                        + format_hex_u32(static_cast<std::uint32_t>(exception.code()));
                }
                return false;
            }

            content_width = content_size.Width > 0
                ? static_cast<std::uint32_t>(content_size.Width)
                : 0;
            content_height = content_size.Height > 0
                ? static_cast<std::uint32_t>(content_size.Height)
                : 0;
            const auto geometry_update = evaluate_capture_geometry_update(
                frame_pool_width_,
                frame_pool_height_,
                content_width,
                content_height,
                desktop_geometry_revision_);
            if (!geometry_update.valid) {
                captured_frame.Close();
                captured_frame = nullptr;
                if (error_detail != nullptr) {
                    *error_detail = "Windows Graphics Capture reported invalid content dimensions";
                }
                return false;
            }
            if (!geometry_update.changed) {
                break;
            }

            captured_frame.Close();
            captured_frame = nullptr;
            if (recreate_attempts >= kMaxWgcFramePoolRecreatesPerCapture) {
                if (error_detail != nullptr) {
                    *error_detail = "Windows Graphics Capture dimensions did not stabilize";
                }
                return false;
            }

            const auto recreate_start = std::chrono::steady_clock::now();
            if (!recreate_frame_pool(
                    content_width,
                    content_height,
                    geometry_update.next_revision,
                    error_detail)) {
                return false;
            }
            const auto recreate_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - recreate_start)
                    .count());
            if (stage_telemetry != nullptr) {
                ++stage_telemetry->frame_pool_recreate_count;
                stage_telemetry->frame_pool_recreate_us += recreate_us;
            }
            ++recreate_attempts;
            acquire_deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(frame_acquire_timeout_ms_);
        }

        if (stage_telemetry != nullptr) {
            stage_telemetry->wait_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - acquire_start)
                    .count());
        }

        const auto copy_start = std::chrono::steady_clock::now();
        auto close_frame = [&]() noexcept {
            if (captured_frame != nullptr) {
                try {
                    captured_frame.Close();
                } catch (...) {
                }
                captured_frame = nullptr;
            }
        };
        auto store_copy_us = [&]() {
            if (stage_telemetry != nullptr) {
                stage_telemetry->copy_us = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - copy_start)
                        .count());
            }
        };

        try {
            const auto surface = captured_frame.Surface();
            const auto surface_access = surface.as<
                ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> frame_texture;
            winrt::check_hresult(surface_access->GetInterface(IID_PPV_ARGS(&frame_texture)));

            D3D11_TEXTURE2D_DESC desc{};
            frame_texture->GetDesc(&desc);
            if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                throw winrt::hresult_error(E_INVALIDARG, L"captured frame is not BGRA");
            }

            if (content_width > desc.Width || content_height > desc.Height) {
                throw winrt::hresult_error(E_BOUNDS, L"capture content exceeds its backing texture");
            }

            frame->width = content_width;
            frame->height = content_height;
            frame->bgra = true;
            frame->row_pitch = content_width * 4;
            frame->desktop_origin_x = desktop_origin_x_;
            frame->desktop_origin_y = desktop_origin_y_;
            frame->desktop_width = desktop_width_ == 0 ? content_width : desktop_width_;
            frame->desktop_height = desktop_height_ == 0 ? content_height : desktop_height_;
            frame->desktop_rotation = desktop_rotation_;
            frame->desktop_geometry_revision = desktop_geometry_revision_;
            frame->native_handle_type = CapturedFrameNativeHandleType::kNone;
            frame->native_handle.reset();

            bool native_frame_ready = false;
            if (capture_native_d3d11_textures_.load()) {
                D3D11_TEXTURE2D_DESC native_desc = desc;
                native_desc.BindFlags = 0;
                native_desc.CPUAccessFlags = 0;
                native_desc.Usage = D3D11_USAGE_DEFAULT;
                native_desc.MiscFlags = 0;
                auto native_handle = acquire_native_frame_handle(native_desc, stage_telemetry);
                if (native_handle != nullptr) {
                    context_->CopyResource(native_handle->d3d11_texture.Get(), frame_texture.Get());
                    frame->native_handle_type = CapturedFrameNativeHandleType::kD3D11Texture2D;
                    frame->native_handle = std::move(native_handle);
                    native_frame_ready = true;
                }
            }

            if (skip_cpu_readback_when_native_texture_available_.load() && native_frame_ready) {
                frame->row_pitch = 0;
                frame->data.clear();
                close_frame();
                store_copy_us();
                if (error_detail != nullptr) {
                    error_detail->clear();
                }
                return true;
            }

            if (!ensure_staging_texture(desc)) {
                throw winrt::hresult_error(E_OUTOFMEMORY, L"failed to create WGC staging texture");
            }

            context_->CopyResource(staging_texture_.Get(), frame_texture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT map_hr = context_->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(map_hr) || mapped.pData == nullptr) {
                reset_staging_texture();
                throw winrt::hresult_error(map_hr, L"failed to map WGC staging texture");
            }

            frame->data.resize(static_cast<std::size_t>(frame->row_pitch) * frame->height);
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
            for (std::uint32_t y = 0; y < frame->height; ++y) {
                std::memcpy(
                    frame->data.data() + static_cast<std::size_t>(y) * frame->row_pitch,
                    source + static_cast<std::size_t>(y) * mapped.RowPitch,
                    frame->row_pitch);
            }
            context_->Unmap(staging_texture_.Get(), 0);
        } catch (const winrt::hresult_error& exception) {
            close_frame();
            store_copy_us();
            if (error_detail != nullptr) {
                *error_detail = "Windows Graphics Capture frame copy failed hr="
                    + format_hex_u32(static_cast<std::uint32_t>(exception.code()));
            }
            return false;
        }

        close_frame();
        store_copy_us();
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    void configure_native_frame_delivery(
        bool capture_native_d3d11_textures,
        bool skip_cpu_readback_when_native_texture_available) override {
        capture_native_d3d11_textures_.store(capture_native_d3d11_textures);
        skip_cpu_readback_when_native_texture_available_.store(
            skip_cpu_readback_when_native_texture_available);
    }

    void stop() override {
        {
            std::lock_guard lock(frame_mutex_);
            stopping_ = true;
        }
        frame_ready_.notify_all();

        try {
            if (capture_session_ != nullptr) {
                capture_session_.Close();
            }
        } catch (...) {
        }

        if (frame_pool_ != nullptr && frame_arrived_token_.value != 0) {
            try {
                frame_pool_.FrameArrived(frame_arrived_token_);
            } catch (...) {
            }
        }
        frame_arrived_token_ = {};

        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame pending_frame_to_close{nullptr};
        {
            std::lock_guard lock(frame_mutex_);
            pending_frame_to_close = std::move(pending_frame_);
            pending_frame_count_ = 0;
            callback_hresult_ = S_OK;
            recreating_frame_pool_ = false;
        }
        if (pending_frame_to_close != nullptr) {
            try {
                pending_frame_to_close.Close();
            } catch (...) {
            }
            pending_frame_to_close = nullptr;
        }

        {
            std::lock_guard pool_lock(frame_pool_operation_mutex_);
            try {
                if (frame_pool_ != nullptr) {
                    frame_pool_.Close();
                }
            } catch (...) {
            }

            capture_session_ = nullptr;
            frame_pool_ = nullptr;
            capture_item_ = nullptr;
            direct3d_device_ = nullptr;
            output_.Reset();
            reset_staging_texture();
            native_frame_pool_.clear();
            context_.Reset();
            device_.Reset();
        }
        frame_pool_width_ = 0;
        frame_pool_height_ = 0;
        native_texture_width_ = 0;
        native_texture_height_ = 0;
        native_texture_format_ = DXGI_FORMAT_UNKNOWN;
        desktop_origin_x_ = 0;
        desktop_origin_y_ = 0;
        desktop_width_ = 0;
        desktop_height_ = 0;
        desktop_rotation_ = 0;
        desktop_geometry_revision_ = 0;
    }

    bool is_running() const override {
        return device_ != nullptr
            && context_ != nullptr
            && frame_pool_ != nullptr
            && capture_session_ != nullptr;
    }
    CaptureBackendType backend_type() const override { return CaptureBackendType::kWindowsGraphicsCapture; }

private:
    bool recreate_frame_pool(
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t geometry_revision,
        std::string* error_detail) {
        if (width == 0 || height == 0
            || width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            || height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
            if (error_detail != nullptr) {
                *error_detail = "Windows Graphics Capture cannot recreate an invalid frame-pool size";
            }
            return false;
        }

        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame pending_frame_to_close{nullptr};
        {
            std::lock_guard lock(frame_mutex_);
            if (stopping_) {
                if (error_detail != nullptr) {
                    *error_detail = "Windows Graphics Capture stopped before frame-pool recreation";
                }
                return false;
            }
            recreating_frame_pool_ = true;
            pending_frame_to_close = std::move(pending_frame_);
            pending_frame_count_ = 0;
            callback_hresult_ = S_OK;
        }
        if (pending_frame_to_close != nullptr) {
            try {
                pending_frame_to_close.Close();
            } catch (...) {
            }
            pending_frame_to_close = nullptr;
        }

        try {
            std::lock_guard pool_lock(frame_pool_operation_mutex_);
            if (frame_pool_ == nullptr || direct3d_device_ == nullptr) {
                throw winrt::hresult_error(RO_E_CLOSED, L"WGC frame pool is closed");
            }

            frame_pool_.Recreate(
                direct3d_device_,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                kWgcFramePoolSize,
                winrt::Windows::Graphics::SizeInt32{
                    static_cast<std::int32_t>(width),
                    static_cast<std::int32_t>(height)});
            reset_staging_texture();
            native_frame_pool_.clear();
            native_texture_width_ = 0;
            native_texture_height_ = 0;
            native_texture_format_ = DXGI_FORMAT_UNKNOWN;

            DXGI_OUTPUT_DESC output_desc{};
            if (output_ != nullptr && SUCCEEDED(output_->GetDesc(&output_desc))) {
                desktop_origin_x_ = output_desc.DesktopCoordinates.left;
                desktop_origin_y_ = output_desc.DesktopCoordinates.top;
                switch (output_desc.Rotation) {
                case DXGI_MODE_ROTATION_ROTATE90: desktop_rotation_ = 90; break;
                case DXGI_MODE_ROTATION_ROTATE180: desktop_rotation_ = 180; break;
                case DXGI_MODE_ROTATION_ROTATE270: desktop_rotation_ = 270; break;
                default: desktop_rotation_ = 0; break;
                }
            }
            desktop_width_ = width;
            desktop_height_ = height;
            desktop_geometry_revision_ = geometry_revision;
            frame_pool_width_ = width;
            frame_pool_height_ = height;

            {
                std::lock_guard lock(frame_mutex_);
                recreating_frame_pool_ = false;
            }
        } catch (const winrt::hresult_error& exception) {
            {
                std::lock_guard lock(frame_mutex_);
                recreating_frame_pool_ = false;
            }
            frame_ready_.notify_all();
            if (error_detail != nullptr) {
                *error_detail = "Windows Graphics Capture frame-pool recreation failed hr="
                    + format_hex_u32(static_cast<std::uint32_t>(exception.code()));
            }
            return false;
        }

        frame_ready_.notify_all();
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    void on_frame_arrived(
        const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& sender) noexcept {
        try {
            std::lock_guard pool_lock(frame_pool_operation_mutex_);
            auto newest_frame = sender.TryGetNextFrame();
            if (newest_frame == nullptr) {
                return;
            }

            std::uint32_t accumulated_frames = 1;
            while (accumulated_frames < static_cast<std::uint32_t>(kWgcFramePoolSize)) {
                auto next_frame = sender.TryGetNextFrame();
                if (next_frame == nullptr) {
                    break;
                }
                newest_frame.Close();
                newest_frame = std::move(next_frame);
                ++accumulated_frames;
            }

            {
                std::lock_guard lock(frame_mutex_);
                if (stopping_ || recreating_frame_pool_) {
                    newest_frame.Close();
                    return;
                }
                if (pending_frame_ != nullptr) {
                    pending_frame_.Close();
                }
                pending_frame_ = std::move(newest_frame);
                pending_frame_count_ += accumulated_frames;
            }
            frame_ready_.notify_one();
        } catch (const winrt::hresult_error& exception) {
            {
                std::lock_guard lock(frame_mutex_);
                if (!stopping_ && !recreating_frame_pool_) {
                    callback_hresult_ = exception.code();
                }
            }
            frame_ready_.notify_all();
        } catch (...) {
            {
                std::lock_guard lock(frame_mutex_);
                if (!stopping_ && !recreating_frame_pool_) {
                    callback_hresult_ = E_FAIL;
                }
            }
            frame_ready_.notify_all();
        }
    }

    std::shared_ptr<CapturedFrameNativeHandle> acquire_native_frame_handle(
        const D3D11_TEXTURE2D_DESC& desc,
        CaptureFrameStageTelemetry* stage_telemetry) {
        if (native_texture_width_ != desc.Width
            || native_texture_height_ != desc.Height
            || native_texture_format_ != desc.Format) {
            native_frame_pool_.clear();
            native_texture_width_ = desc.Width;
            native_texture_height_ = desc.Height;
            native_texture_format_ = desc.Format;
        }

        for (const auto& handle : native_frame_pool_) {
            if (handle.use_count() == 1) {
                if (stage_telemetry != nullptr) {
                    stage_telemetry->native_texture_pool_reused = true;
                }
                return handle;
            }
        }
        if (native_frame_pool_.size() >= kWgcNativeTexturePoolSize) {
            if (stage_telemetry != nullptr) {
                stage_telemetry->native_texture_pool_exhausted = true;
            }
            return nullptr;
        }

        ComPtr<ID3D11Texture2D> native_texture;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &native_texture)) || native_texture == nullptr) {
            return nullptr;
        }
        auto handle = make_d3d11_native_handle(
            device_.Get(), native_texture.Get(), desc.Format, 0);
        native_frame_pool_.push_back(handle);
        if (stage_telemetry != nullptr) {
            stage_telemetry->native_texture_pool_created = true;
        }
        return handle;
    }

    bool ensure_staging_texture(const D3D11_TEXTURE2D_DESC& desc) {
        if (staging_texture_ != nullptr
            && staging_texture_width_ == desc.Width
            && staging_texture_height_ == desc.Height
            && staging_texture_format_ == desc.Format) {
            return true;
        }

        reset_staging_texture();
        D3D11_TEXTURE2D_DESC staging_desc = desc;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.MiscFlags = 0;
        if (FAILED(device_->CreateTexture2D(&staging_desc, nullptr, &staging_texture_))
            || staging_texture_ == nullptr) {
            return false;
        }
        staging_texture_width_ = desc.Width;
        staging_texture_height_ = desc.Height;
        staging_texture_format_ = desc.Format;
        return true;
    }

    void reset_staging_texture() {
        staging_texture_.Reset();
        staging_texture_width_ = 0;
        staging_texture_height_ = 0;
        staging_texture_format_ = DXGI_FORMAT_UNKNOWN;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutput> output_;
    ComPtr<ID3D11Texture2D> staging_texture_;
    std::vector<std::shared_ptr<CapturedFrameNativeHandle>> native_frame_pool_;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice direct3d_device_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem capture_item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession capture_session_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame pending_frame_{nullptr};
    winrt::event_token frame_arrived_token_{};
    mutable std::mutex frame_mutex_;
    std::mutex frame_pool_operation_mutex_;
    std::condition_variable frame_ready_;
    std::uint32_t pending_frame_count_ = 0;
    std::uint32_t frame_acquire_timeout_ms_ = 250;
    std::uint32_t frame_pool_width_ = 0;
    std::uint32_t frame_pool_height_ = 0;
    std::uint32_t staging_texture_width_ = 0;
    std::uint32_t staging_texture_height_ = 0;
    DXGI_FORMAT staging_texture_format_ = DXGI_FORMAT_UNKNOWN;
    std::uint32_t native_texture_width_ = 0;
    std::uint32_t native_texture_height_ = 0;
    DXGI_FORMAT native_texture_format_ = DXGI_FORMAT_UNKNOWN;
    std::int32_t desktop_origin_x_ = 0;
    std::int32_t desktop_origin_y_ = 0;
    std::uint32_t desktop_width_ = 0;
    std::uint32_t desktop_height_ = 0;
    std::uint32_t desktop_rotation_ = 0;
    std::uint64_t desktop_geometry_revision_ = 0;
    HRESULT callback_hresult_ = S_OK;
    bool stopping_ = true;
    bool recreating_frame_pool_ = false;
    std::atomic_bool capture_native_d3d11_textures_ = false;
    std::atomic_bool skip_cpu_readback_when_native_texture_available_ = false;
};

class GdiCaptureBackend final : public ICaptureBackend {
public:
    bool start(const CaptureSessionConfig& config, std::string* error_detail) override {
        stop();

        std::string display_error;
        const auto displays = enumerate_capture_displays(&display_error);
        const auto selected = std::find_if(
            displays.begin(),
            displays.end(),
            [&](const CaptureDisplayDescriptor& display) {
                return !config.display_id.empty()
                    ? display.id == config.display_id
                    : (display.adapter_index == config.adapter_index
                        && display.output_index == config.output_index);
            });
        if (selected == displays.end()) {
            if (error_detail != nullptr) {
                *error_detail = "selected display is unavailable for GDI capture: " + display_error;
            }
            return false;
        }
        origin_x_ = selected->desktop_origin_x;
        origin_y_ = selected->desktop_origin_y;
        width_ = static_cast<int>(selected->pixel_width);
        height_ = static_cast<int>(selected->pixel_height);
        primary_width_ = width_;
        primary_height_ = height_;
        if (width_ <= 0 || height_ <= 0) {
            if (error_detail != nullptr) {
                *error_detail = "GDI virtual screen dimensions are invalid";
            }
            return false;
        }

        screen_dc_ = GetDC(nullptr);
        if (screen_dc_ == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "GetDC(nullptr) failed for GDI capture";
            }
            return false;
        }

        memory_dc_ = CreateCompatibleDC(screen_dc_);
        if (memory_dc_ == nullptr) {
            stop();
            if (error_detail != nullptr) {
                *error_detail = "CreateCompatibleDC failed for GDI capture";
            }
            return false;
        }

        bitmap_ = CreateCompatibleBitmap(screen_dc_, width_, height_);
        if (bitmap_ == nullptr) {
            stop();
            if (error_detail != nullptr) {
                *error_detail = "CreateCompatibleBitmap failed for GDI capture";
            }
            return false;
        }

        old_bitmap_ = SelectObject(memory_dc_, bitmap_);
        if (old_bitmap_ == nullptr) {
            stop();
            if (error_detail != nullptr) {
                *error_detail = "SelectObject failed for GDI capture";
            }
            return false;
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool capture_frame(
        CapturedFrame* frame,
        CaptureFrameStageTelemetry* stage_telemetry,
        std::string* error_detail) override {
        if (stage_telemetry != nullptr) {
            *stage_telemetry = CaptureFrameStageTelemetry{};
        }
        if (frame == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "frame pointer must be non-null";
            }
            return false;
        }

        if (!is_running()) {
            if (error_detail != nullptr) {
                *error_detail = "GDI capture backend is not running";
            }
            return false;
        }

        const auto copy_start = std::chrono::steady_clock::now();
        auto store_copy_us = [&]() {
            if (stage_telemetry != nullptr) {
                stage_telemetry->copy_us = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - copy_start)
                        .count());
            }
        };

        DWORD captureblt_error = ERROR_SUCCESS;
        DWORD srccopy_error = ERROR_SUCCESS;
        bool copied = bit_blt_from_screen(
            origin_x_,
            origin_y_,
            width_,
            height_,
            SRCCOPY | CAPTUREBLT,
            &captureblt_error);
        if (!copied) {
            copied = bit_blt_from_screen(origin_x_, origin_y_, width_, height_, SRCCOPY, &srccopy_error);
        }

        DWORD primary_captureblt_error = ERROR_SUCCESS;
        DWORD primary_srccopy_error = ERROR_SUCCESS;
        const bool can_retry_primary =
            primary_width_ > 0
            && primary_height_ > 0
            && (origin_x_ != 0
                || origin_y_ != 0
                || width_ > primary_width_
                || height_ > primary_height_);
        if (!copied && can_retry_primary) {
            PatBlt(memory_dc_, 0, 0, width_, height_, BLACKNESS);
            const int retry_width = std::min(width_, primary_width_);
            const int retry_height = std::min(height_, primary_height_);
            copied = bit_blt_from_screen(
                0,
                0,
                retry_width,
                retry_height,
                SRCCOPY | CAPTUREBLT,
                &primary_captureblt_error);
            if (!copied) {
                copied = bit_blt_from_screen(0, 0, retry_width, retry_height, SRCCOPY, &primary_srccopy_error);
            }
        }

        if (!copied) {
            if (error_detail != nullptr) {
                *error_detail = "BitBlt failed for GDI capture (captureblt_error="
                    + std::to_string(captureblt_error)
                    + ", srccopy_error=" + std::to_string(srccopy_error)
                    + ", primary_captureblt_error=" + std::to_string(primary_captureblt_error)
                    + ", primary_srccopy_error=" + std::to_string(primary_srccopy_error)
                    + ", source_origin=" + std::to_string(origin_x_) + "," + std::to_string(origin_y_)
                    + ", source_size=" + std::to_string(width_) + "x" + std::to_string(height_) + ")";
            }
                    store_copy_us();
            return false;
        }

        BITMAPINFO bitmap_info{};
        bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap_info.bmiHeader.biWidth = width_;
        bitmap_info.bmiHeader.biHeight = -height_;
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;

        frame->width = static_cast<std::uint32_t>(width_);
        frame->height = static_cast<std::uint32_t>(height_);
        frame->row_pitch = frame->width * 4;
        frame->bgra = true;
        frame->desktop_origin_x = origin_x_;
        frame->desktop_origin_y = origin_y_;
        frame->desktop_width = frame->width;
        frame->desktop_height = frame->height;
        frame->desktop_rotation = 0;
        frame->desktop_geometry_revision = 1;
        frame->data.resize(static_cast<std::size_t>(frame->row_pitch) * frame->height);

        const int scan_lines = GetDIBits(
            memory_dc_,
            bitmap_,
            0,
            frame->height,
            frame->data.data(),
            &bitmap_info,
            DIB_RGB_COLORS);
        if (scan_lines == 0 || static_cast<std::uint32_t>(scan_lines) != frame->height) {
            if (error_detail != nullptr) {
                *error_detail = "GetDIBits failed for GDI capture";
            }
            store_copy_us();
            return false;
        }

        store_copy_us();

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    void stop() override {
        if (memory_dc_ != nullptr && old_bitmap_ != nullptr) {
            SelectObject(memory_dc_, old_bitmap_);
        }
        old_bitmap_ = nullptr;

        if (bitmap_ != nullptr) {
            DeleteObject(bitmap_);
            bitmap_ = nullptr;
        }
        if (memory_dc_ != nullptr) {
            DeleteDC(memory_dc_);
            memory_dc_ = nullptr;
        }
        if (screen_dc_ != nullptr) {
            ReleaseDC(nullptr, screen_dc_);
            screen_dc_ = nullptr;
        }
        origin_x_ = 0;
        origin_y_ = 0;
        width_ = 0;
        height_ = 0;
        primary_width_ = 0;
        primary_height_ = 0;
    }

    bool is_running() const override {
        return screen_dc_ != nullptr && memory_dc_ != nullptr && bitmap_ != nullptr;
    }

    CaptureBackendType backend_type() const override { return CaptureBackendType::kGdiBitBlt; }

private:
    bool bit_blt_from_screen(
        int source_x,
        int source_y,
        int width,
        int height,
        DWORD raster_op,
        DWORD* last_error) {
        SetLastError(ERROR_SUCCESS);
        const BOOL copied = BitBlt(memory_dc_, 0, 0, width, height, screen_dc_, source_x, source_y, raster_op);
        if (last_error != nullptr) {
            *last_error = copied == 0 ? GetLastError() : ERROR_SUCCESS;
        }
        return copied != 0;
    }

    HDC screen_dc_ = nullptr;
    HDC memory_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    int origin_x_ = 0;
    int origin_y_ = 0;
    int width_ = 0;
    int height_ = 0;
    int primary_width_ = 0;
    int primary_height_ = 0;
};

std::uint64_t hash_frame_sample(const CapturedFrame& frame) {
    constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

    std::uint64_t hash = kFnvOffset;
    if (frame.data.empty()) {
        return hash;
    }

    const std::size_t sample_count = std::min<std::size_t>(frame.data.size(), 1024);
    const std::size_t stride = std::max<std::size_t>(frame.data.size() / sample_count, 1);
    std::size_t sampled = 0;
    for (std::size_t index = 0; index < frame.data.size() && sampled < sample_count; index += stride, ++sampled) {
        hash ^= static_cast<std::uint64_t>(frame.data[index]);
        hash *= kFnvPrime;
    }
    return hash;
}

bool is_black_frame(const CapturedFrame& frame) {
    if (frame.data.empty()) {
        return true;
    }

    const std::size_t sample_count = std::min<std::size_t>(frame.data.size(), 4096);
    const std::size_t stride = std::max<std::size_t>(frame.data.size() / sample_count, 1);
    std::size_t sampled = 0;
    for (std::size_t index = 0; index < frame.data.size() && sampled < sample_count; index += stride, ++sampled) {
        if (frame.data[index] != 0) {
            return false;
        }
    }
    return true;
}

bool write_bgra_bmp(
    const std::string& output_file_path,
    const std::uint8_t* data,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t row_pitch,
    std::string* error) {
    if (data == nullptr || width == 0 || height == 0) {
        if (error != nullptr) {
            *error = "invalid frame buffer";
        }
        return false;
    }

    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info_header{};

    info_header.biSize = sizeof(BITMAPINFOHEADER);
    info_header.biWidth = static_cast<LONG>(width);
    info_header.biHeight = -static_cast<LONG>(height);
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = width * height * 4;

    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + info_header.biSizeImage;

    std::ofstream out(output_file_path, std::ios::binary);
    if (!out.is_open()) {
        if (error != nullptr) {
            *error = "failed to open output file";
        }
        return false;
    }

    out.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
    out.write(reinterpret_cast<const char*>(&info_header), sizeof(info_header));

    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* row = data + static_cast<std::size_t>(y) * row_pitch;
        out.write(reinterpret_cast<const char*>(row), static_cast<std::streamsize>(width * 4));
    }

    if (!out.good()) {
        if (error != nullptr) {
            *error = "failed while writing output file";
        }
        return false;
    }

    return true;
}

}  // namespace

class WindowsCaptureSession::Impl {
public:
    bool start(const CaptureSessionConfig& config, std::string* error_detail) {
        stop();

        config_ = config;
        if (config_.preferred_backend == CaptureBackendType::kWindowsGraphicsCapture) {
            std::string wgc_error;
            if (switch_to_backend(std::make_unique<WgcCaptureBackend>(), "preferred-wgc", &wgc_error)) {
                if (error_detail != nullptr) {
                    error_detail->clear();
                }
                return true;
            }
            set_last_fallback_reason("Windows Graphics Capture start failed: " + wgc_error);
            if (!config_.fallback_enabled) {
                if (error_detail != nullptr) {
                    *error_detail = "Windows Graphics Capture start failed: " + wgc_error;
                }
                return false;
            }
            ++telemetry_.fallback_attempt_count;
            std::string gdi_error;
            if (!switch_to_backend(std::make_unique<GdiCaptureBackend>(), "preferred-wgc-gdi-fallback", &gdi_error)) {
                if (error_detail != nullptr) {
                    *error_detail = "Windows Graphics Capture start failed: " + wgc_error
                        + "; GDI BitBlt fallback failed: " + gdi_error;
                }
                return false;
            }
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

        if (config_.preferred_backend == CaptureBackendType::kGdiBitBlt) {
            std::string gdi_error;
            if (!switch_to_backend(std::make_unique<GdiCaptureBackend>(), "preferred-gdi", &gdi_error)) {
                if (error_detail != nullptr) {
                    *error_detail = "GDI BitBlt start failed: " + gdi_error;
                }
                return false;
            }
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

        std::string dda_error;
        if (!switch_to_backend(std::make_unique<DdaCaptureBackend>(), "initial-start", &dda_error)) {
            set_last_fallback_reason("Desktop Duplication start failed: " + dda_error);
            if (!config_.fallback_enabled) {
                if (error_detail != nullptr) {
                    *error_detail = "Desktop Duplication start failed: " + dda_error;
                }
                return false;
            }
            ++telemetry_.fallback_attempt_count;

            std::string wgc_error;
            if (switch_to_backend(std::make_unique<WgcCaptureBackend>(), "initial-wgc-fallback", &wgc_error)) {
                if (error_detail != nullptr) {
                    error_detail->clear();
                }
                return true;
            }

            std::string gdi_error;
            if (!switch_to_backend(std::make_unique<GdiCaptureBackend>(), "initial-gdi-fallback", &gdi_error)) {
                if (error_detail != nullptr) {
                    *error_detail = "Desktop Duplication start failed: " + dda_error
                        + "; Windows Graphics Capture fallback failed: " + wgc_error
                        + "; GDI BitBlt fallback failed: " + gdi_error;
                }
                return false;
            }
            set_last_fallback_reason(
                "Desktop Duplication start failed: " + dda_error
                + "; Windows Graphics Capture fallback failed: " + wgc_error);
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool capture_frame(CapturedFrame* frame, std::string* error_detail) {
        if (backend_ == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "capture session not started";
            }
            return false;
        }

        std::string backend_error;
        CaptureFrameStageTelemetry stage_telemetry;
        ++telemetry_.total_capture_attempt_count;
        if (!backend_->capture_frame(frame, &stage_telemetry, &backend_error)) {
            update_stage_telemetry(stage_telemetry);
            if (stage_telemetry.timeout) {
                ++telemetry_.total_timeout_count;
            }
            if (backend_error == "timeout waiting for desktop frame") {
                ++telemetry_.consecutive_timeouts;
                telemetry_.consecutive_failures = 0;
            } else {
                telemetry_.consecutive_timeouts = 0;
                ++telemetry_.consecutive_failures;
            }

            const CaptureHealthSignals signals = current_health_signals();
            const auto decision = evaluate_capture_fallback_decision(signals, config_);
            if (decision.should_switch_backend) {
                std::string fallback_error;
                try_fallback_to_wgc(decision.reason, &fallback_error);
            }

            if (error_detail != nullptr) {
                *error_detail = backend_error;
            }
            return false;
        }

        update_stage_telemetry(stage_telemetry);
        telemetry_.consecutive_timeouts = 0;
        telemetry_.consecutive_failures = 0;
        update_success_telemetry(*frame);

        const CaptureHealthSignals signals = current_health_signals();
        const auto decision = evaluate_capture_fallback_decision(signals, config_);
        if (decision.should_switch_backend) {
            std::string fallback_error;
            try_fallback_to_wgc(decision.reason, &fallback_error);
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    void configure_native_frame_delivery(
        bool capture_native_d3d11_textures,
        bool skip_cpu_readback_when_native_texture_available) {
        config_.capture_native_d3d11_textures = capture_native_d3d11_textures;
        config_.skip_cpu_readback_when_native_texture_available =
            skip_cpu_readback_when_native_texture_available;
        if (backend_ != nullptr) {
            backend_->configure_native_frame_delivery(
                capture_native_d3d11_textures,
                skip_cpu_readback_when_native_texture_available);
        }
    }

    void stop() {
        if (backend_ != nullptr) {
            backend_->stop();
            backend_.reset();
        }
        config_ = CaptureSessionConfig{};
        active_backend_ = CaptureBackendType::kUnknown;
        telemetry_ = CaptureBackendTelemetry{};
        black_frame_count_ = 0;
        last_frame_hash_ = 0;
        has_last_frame_hash_ = false;
        last_frame_timestamp_valid_ = false;
    }

    bool is_running() const {
        return backend_ != nullptr && backend_->is_running();
    }

    CaptureBackendType active_backend() const {
        return active_backend_;
    }

    CaptureBackendTelemetry telemetry() const {
        return telemetry_;
    }

private:
    void update_stage_telemetry(const CaptureFrameStageTelemetry& stage_telemetry) {
        telemetry_.total_capture_wait_us += stage_telemetry.wait_us;
        telemetry_.total_capture_copy_us += stage_telemetry.copy_us;
        telemetry_.last_capture_wait_ms = static_cast<double>(stage_telemetry.wait_us) / 1000.0;
        telemetry_.last_capture_copy_ms = static_cast<double>(stage_telemetry.copy_us) / 1000.0;
        telemetry_.total_accumulated_frames += stage_telemetry.accumulated_frames;
        telemetry_.last_accumulated_frames = stage_telemetry.accumulated_frames;
        if (stage_telemetry.native_texture_pool_created) {
            ++telemetry_.native_texture_pool_create_count;
        }
        if (stage_telemetry.native_texture_pool_reused) {
            ++telemetry_.native_texture_pool_reuse_count;
        }
        if (stage_telemetry.native_texture_pool_exhausted) {
            ++telemetry_.native_texture_pool_exhaustion_count;
        }
        telemetry_.frame_pool_recreate_count += stage_telemetry.frame_pool_recreate_count;
        telemetry_.total_frame_pool_recreate_us += stage_telemetry.frame_pool_recreate_us;
        if (stage_telemetry.frame_pool_recreate_count > 0) {
            telemetry_.last_frame_pool_recreate_ms =
                static_cast<double>(stage_telemetry.frame_pool_recreate_us) / 1000.0;
        }
    }

    bool switch_to_backend(
        std::unique_ptr<ICaptureBackend> candidate,
        const std::string&,
        std::string* error_detail) {
        if (candidate == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "invalid capture backend candidate";
            }
            return false;
        }

        std::string start_error;
        if (!candidate->start(config_, &start_error)) {
            if (error_detail != nullptr) {
                *error_detail = start_error;
            }
            return false;
        }

        if (backend_ != nullptr) {
            backend_->stop();
        }

        const CaptureBackendType previous_backend = active_backend_;
        backend_ = std::move(candidate);
        active_backend_ = backend_->backend_type();
        telemetry_.active_backend = active_backend_;
        if (previous_backend != CaptureBackendType::kUnknown && previous_backend != active_backend_) {
            ++telemetry_.backend_switch_count;
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    void try_fallback_to_wgc(const std::string& reason, std::string* error_detail) {
        ++telemetry_.fallback_attempt_count;
        std::string wgc_error;
        if (switch_to_backend(std::make_unique<WgcCaptureBackend>(), "fallback-wgc", &wgc_error)) {
            set_last_fallback_reason(reason);
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return;
        }

        std::string gdi_error;
        if (!switch_to_backend(std::make_unique<GdiCaptureBackend>(), "fallback-gdi", &gdi_error)) {
            set_last_fallback_reason(
                reason + "; Windows Graphics Capture fallback failed: " + wgc_error
                + "; GDI fallback failed: " + gdi_error);
            if (error_detail != nullptr) {
                *error_detail = "Windows Graphics Capture fallback failed: " + wgc_error
                    + "; GDI fallback failed: " + gdi_error;
            }
            return;
        }

        set_last_fallback_reason(
            reason + "; Windows Graphics Capture fallback failed: " + wgc_error);

        if (error_detail != nullptr) {
            error_detail->clear();
        }
    }

    CaptureHealthSignals current_health_signals() const {
        CaptureHealthSignals signals;
        signals.consecutive_failures = telemetry_.consecutive_failures;
        signals.backend = active_backend_;
        return signals;
    }

    void set_last_fallback_reason(std::string reason) {
        constexpr std::size_t kMaxFallbackReasonLength = 512;
        for (char& value : reason) {
            if (value == '\r' || value == '\n' || value == '\t') {
                value = ' ';
            }
        }
        if (reason.size() > kMaxFallbackReasonLength) {
            reason.resize(kMaxFallbackReasonLength);
        }
        telemetry_.last_fallback_reason = std::move(reason);
    }

    void update_success_telemetry(const CapturedFrame& frame) {
        ++telemetry_.total_frames_observed;

        if (!captured_frame_has_cpu_bgra_pixels(frame)) {
            telemetry_.black_frame_ratio = 0.0;
            telemetry_.stale_frame_count = 0;
            has_last_frame_hash_ = false;

            const auto now = std::chrono::steady_clock::now();
            if (last_frame_timestamp_valid_) {
                telemetry_.frame_present_delta_ms = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame_timestamp_).count());
            }
            last_frame_timestamp_ = now;
            last_frame_timestamp_valid_ = true;
            return;
        }

        if (is_black_frame(frame)) {
            ++black_frame_count_;
        }
        telemetry_.black_frame_ratio = static_cast<double>(black_frame_count_)
            / static_cast<double>(std::max<std::uint32_t>(telemetry_.total_frames_observed, 1));

        const std::uint64_t hash = hash_frame_sample(frame);
        if (has_last_frame_hash_ && hash == last_frame_hash_) {
            ++telemetry_.stale_frame_count;
        } else {
            telemetry_.stale_frame_count = 0;
        }
        last_frame_hash_ = hash;
        has_last_frame_hash_ = true;

        const auto now = std::chrono::steady_clock::now();
        if (last_frame_timestamp_valid_) {
            telemetry_.frame_present_delta_ms = static_cast<double>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame_timestamp_).count());
        }
        last_frame_timestamp_ = now;
        last_frame_timestamp_valid_ = true;
    }

    std::unique_ptr<ICaptureBackend> backend_;
    CaptureSessionConfig config_;
    CaptureBackendType active_backend_ = CaptureBackendType::kUnknown;
    CaptureBackendTelemetry telemetry_;
    std::uint32_t black_frame_count_ = 0;
    std::uint64_t last_frame_hash_ = 0;
    bool has_last_frame_hash_ = false;
    std::chrono::steady_clock::time_point last_frame_timestamp_{};
    bool last_frame_timestamp_valid_ = false;
};

WindowsCaptureSession::WindowsCaptureSession()
    : impl_(std::make_unique<Impl>()) {}

WindowsCaptureSession::~WindowsCaptureSession() = default;

bool WindowsCaptureSession::start(const CaptureSessionConfig& config, std::string* error_detail) {
    return impl_->start(config, error_detail);
}

bool WindowsCaptureSession::captureFrame(CapturedFrame* frame, std::string* error_detail) {
    return impl_->capture_frame(frame, error_detail);
}

void WindowsCaptureSession::configureNativeFrameDelivery(
    bool capture_native_d3d11_textures,
    bool skip_cpu_readback_when_native_texture_available) {
    impl_->configure_native_frame_delivery(
        capture_native_d3d11_textures,
        skip_cpu_readback_when_native_texture_available);
}

void WindowsCaptureSession::stop() {
    impl_->stop();
}

bool WindowsCaptureSession::isRunning() const {
    return impl_->is_running();
}

CaptureBackendType WindowsCaptureSession::activeBackend() const {
    return impl_->active_backend();
}

CaptureBackendTelemetry WindowsCaptureSession::telemetry() const {
    return impl_->telemetry();
}

DdaCapturePocResult run_dda_min_capture_poc(const std::string& output_file_path, std::uint32_t timeout_ms) {
    DdaCapturePocResult result;
    result.output_file_path = output_file_path;

    if (output_file_path.empty()) {
        result.error = "output_file_path must be non-empty";
        return result;
    }

    WindowsCaptureSession session;
    CaptureSessionConfig config;
    config.output_index = 0;
    config.frame_acquire_timeout_ms = 250;
    config.fallback_enabled = false;

    if (!session.start(config, &result.error)) {
        return result;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        CapturedFrame frame;
        std::string capture_error;
        if (!session.captureFrame(&frame, &capture_error)) {
            if (capture_error == "timeout waiting for desktop frame") {
                continue;
            }
            result.error = capture_error;
            return result;
        }

        result.width = frame.width;
        result.height = frame.height;
        result.bgra_frame = frame.bgra;

        if (!frame.bgra) {
            result.error = "captured frame is not BGRA (DXGI_FORMAT_B8G8R8A8_UNORM)";
            continue;
        }

        std::string write_error;
        const bool write_ok = write_bgra_bmp(
            output_file_path,
            frame.data.data(),
            frame.width,
            frame.height,
            frame.row_pitch,
            &write_error);

        if (!write_ok) {
            result.error = std::string("failed to write BGRA frame: ") + write_error;
            return result;
        }

        result.ok = true;
        return result;
    }

    result.error = "timeout waiting for desktop frame";
    return result;
}

#else

class WindowsCaptureSession::Impl {
public:
    bool start(const CaptureSessionConfig&, std::string* error_detail) {
        if (error_detail != nullptr) {
            *error_detail = "Windows capture session is only supported on Windows";
        }
        return false;
    }

    bool capture_frame(
        CapturedFrame*,
        CaptureFrameStageTelemetry*,
        std::string* error_detail) {
        if (error_detail != nullptr) {
            *error_detail = "Windows capture session is only supported on Windows";
        }
        return false;
    }

    void stop() {}
    void configure_native_frame_delivery(bool, bool) {}
    bool is_running() const { return false; }
    CaptureBackendType active_backend() const { return CaptureBackendType::kUnknown; }
    CaptureBackendTelemetry telemetry() const { return CaptureBackendTelemetry{}; }
};

WindowsCaptureSession::WindowsCaptureSession()
    : impl_(std::make_unique<Impl>()) {}

WindowsCaptureSession::~WindowsCaptureSession() = default;

bool WindowsCaptureSession::start(const CaptureSessionConfig& config, std::string* error_detail) {
    return impl_->start(config, error_detail);
}

bool WindowsCaptureSession::captureFrame(CapturedFrame* frame, std::string* error_detail) {
    return impl_->capture_frame(frame, error_detail);
}

void WindowsCaptureSession::configureNativeFrameDelivery(
    bool capture_native_d3d11_textures,
    bool skip_cpu_readback_when_native_texture_available) {
    impl_->configure_native_frame_delivery(
        capture_native_d3d11_textures,
        skip_cpu_readback_when_native_texture_available);
}

void WindowsCaptureSession::stop() {
    impl_->stop();
}

bool WindowsCaptureSession::isRunning() const {
    return impl_->is_running();
}

CaptureBackendType WindowsCaptureSession::activeBackend() const {
    return impl_->active_backend();
}

CaptureBackendTelemetry WindowsCaptureSession::telemetry() const {
    return impl_->telemetry();
}

DdaCapturePocResult run_dda_min_capture_poc(const std::string& output_file_path, std::uint32_t) {
    DdaCapturePocResult result;
    result.output_file_path = output_file_path;
    result.error = "DDA capture PoC is only supported on Windows";
    return result;
}

#endif

std::string_view module_name() {
    return "capture";
}
}
