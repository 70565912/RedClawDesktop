#include "capture_d3d11.h"
#ifdef _WIN32
#include <d3d11_4.h>
#include <sstream>
namespace redclaw::capture {
using Microsoft::WRL::ComPtr;
namespace {
void assign_error(std::string value, std::string* error) { if (error) *error = std::move(value); }
std::string format_hex_u32(std::uint32_t value) { std::ostringstream s; s << "0x" << std::hex << value; return s.str(); }
}
std::shared_ptr<D3D11CaptureDevice> D3D11CaptureDevice::create(ID3D11Device* device) {
    if (!device) return {};
    auto owner = std::make_shared<D3D11CaptureDevice>();
    owner->device = device;
    device->GetImmediateContext(&owner->context);
    ComPtr<ID3D11Multithread> protection;
    if (!owner->context || FAILED(owner->context.As(&protection))) return {};
    protection->SetMultithreadProtected(TRUE);
    if (!protection->GetMultithreadProtected()) return {};
    return owner;
}
CapturedFrameNativeHandle* unwrap_d3d11_native_handle(const CapturedFrame& frame) {
    if (frame.native_handle_type != CapturedFrameNativeHandleType::kD3D11Texture2D
        || frame.native_handle == nullptr) {
        return nullptr;
    }

    return frame.native_handle.get();
}

std::shared_ptr<CapturedFrameNativeHandle> make_d3d11_native_handle(
    std::shared_ptr<D3D11CaptureDevice> owner,
    ID3D11Texture2D* texture,
    DXGI_FORMAT format,
    std::uint32_t subresource_index) {
    auto handle = std::make_shared<CapturedFrameNativeHandle>();
    handle->owner = std::move(owner);
    handle->d3d11_device = handle->owner->device;
    handle->d3d11_texture = texture;
    handle->d3d11_format = format;
    handle->d3d11_subresource_index = subresource_index;
    return handle;
}

class D3D11VideoProcessorScaler::Impl {
public:
    bool scale(
        const CapturedFrame& source,
        std::uint32_t output_width,
        std::uint32_t output_height,
        DXGI_FORMAT output_format,
        CapturedFrame* output,
        std::string* error_detail) {
        auto* native = unwrap_d3d11_native_handle(source);
        if (native == nullptr || !native->owner || native->d3d11_device == nullptr
            || native->d3d11_texture == nullptr || output == nullptr) {
            assign_error("D3D11 video processor scaling requires a native texture", error_detail);
            return false;
        }
        if (output_width < 2 || output_height < 2
            || (output_format == DXGI_FORMAT_NV12 && ((output_width | output_height) & 1))) {
            assign_error("D3D11 video processor output dimensions are invalid", error_detail);
            return false;
        }

        std::lock_guard lock(native->owner->mutex);
        D3D11_TEXTURE2D_DESC source_desc{};
        native->d3d11_texture->GetDesc(&source_desc);
        if (!ensure_pipeline(
                native->d3d11_device.Get(),
                source_desc,
                output_width,
                output_height,
                output_format,
                error_detail)) {
            return false;
        }

        ComPtr<ID3D11VideoProcessorInputView> input_view;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
        input_desc.FourCC = 0;
        input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_desc.Texture2D.MipSlice = 0;
        input_desc.Texture2D.ArraySlice = native->d3d11_subresource_index;
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
        // Match the existing swscale default: full-range RGB -> limited BT.601.
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_color{}, output_color{};
        input_color.RGB_Range = 0;
        output_color.YCbCr_Matrix = 0;
        output_color.Nominal_Range = output_format == DXGI_FORMAT_NV12 ? 1 : 2;
        video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &input_color);
        video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &output_color);
        D3D11_VIDEO_COLOR background{};
        if (output_format == DXGI_FORMAT_NV12) {
            background.YCbCr.Y = 16.0F / 255.0F;
            background.YCbCr.Cb = background.YCbCr.Cr = 128.0F / 255.0F;
            background.YCbCr.A = 1.0F;
        } else {
            background.RGBA.A = 1.0F;
        }
        video_context_->VideoProcessorSetOutputBackgroundColor(
            processor_.Get(), output_format == DXGI_FORMAT_NV12, &background);
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
        output->bgra = output_format == DXGI_FORMAT_B8G8R8A8_UNORM;
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
        if (!output_handle_) {
            output_handle_ = make_d3d11_native_handle(native->owner, output_texture_.Get(), output_format, 0);
        }
        output->native_handle = output_handle_;
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
        output_handle_.reset();
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
        DXGI_FORMAT output_format,
        std::string* error_detail) {
        if (device_.Get() == device
            && source_width_ == source_desc.Width
            && source_height_ == source_desc.Height
            && output_width_ == output_width
            && output_height_ == output_height
            && format_ == source_desc.Format
            && output_format_ == output_format
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
        UINT input_support = 0, output_support = 0;
        if (FAILED(enumerator_->CheckVideoProcessorFormat(source_desc.Format, &input_support))
            || FAILED(enumerator_->CheckVideoProcessorFormat(output_format, &output_support))
            || !(input_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)
            || !(output_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            assign_error("D3D11 video processor input/output format unsupported", error_detail);
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
        output_desc.Format = output_format;
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
        output_format_ = output_format;
        return true;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11Texture2D> output_texture_;
    ComPtr<ID3D11VideoProcessorOutputView> output_view_;
    std::shared_ptr<CapturedFrameNativeHandle> output_handle_;
    std::uint32_t source_width_ = 0;
    std::uint32_t source_height_ = 0;
    std::uint32_t output_width_ = 0;
    std::uint32_t output_height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT output_format_ = DXGI_FORMAT_UNKNOWN;
};
D3D11VideoProcessorScaler::D3D11VideoProcessorScaler() : impl_(std::make_unique<Impl>()) {}
D3D11VideoProcessorScaler::~D3D11VideoProcessorScaler() = default;
bool D3D11VideoProcessorScaler::scale(const CapturedFrame& frame, std::uint32_t width, std::uint32_t height,
                                    DXGI_FORMAT format, CapturedFrame* output, std::string* error) {
    return impl_->scale(frame, width, height, format, output, error);
}
void D3D11VideoProcessorScaler::reset() { impl_->reset(); }
} // namespace redclaw::capture
#endif
