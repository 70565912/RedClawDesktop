#include "redclaw/render/render_module.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#endif

#if __has_include(<libavcodec/avcodec.h>) && __has_include(<libavutil/error.h>) && __has_include(<libavutil/hwcontext.h>) && __has_include(<libavutil/imgutils.h>) && __has_include(<libswscale/swscale.h>)
#define REDCLAW_RENDER_HAS_LIBAVCODEC 1
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#if defined(_WIN32)
#include <libavutil/hwcontext_d3d11va.h>
#endif
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#else
#define REDCLAW_RENDER_HAS_LIBAVCODEC 0
#endif

namespace redclaw::render {

namespace {

double resolve_scale(
    std::uint32_t frame_width,
    std::uint32_t frame_height,
    const RenderViewportConfig& config) {
    const double viewport_w = static_cast<double>(config.viewport_width);
    const double viewport_h = static_cast<double>(config.viewport_height);
    const double frame_w = static_cast<double>(frame_width);
    const double frame_h = static_cast<double>(frame_height);

    const double fit_scale = std::min(viewport_w / frame_w, viewport_h / frame_h);
    const double fill_scale = std::max(viewport_w / frame_w, viewport_h / frame_h);

    switch (config.scale_mode) {
    case ViewportScaleMode::kFit:
        return fit_scale;
    case ViewportScaleMode::kFill:
        return fill_scale;
    case ViewportScaleMode::kOneToOne:
        return 1.0;
    }

    return fit_scale;
}

#if REDCLAW_RENDER_HAS_LIBAVCODEC

std::string ffmpeg_error_to_string(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, error_code);
    return std::string(buffer);
}

AVCodecID resolve_decoder_codec_id(EncodedVideoCodec codec) {
    if (codec == EncodedVideoCodec::kHevc) {
        return AV_CODEC_ID_HEVC;
    }
    return AV_CODEC_ID_H264;
}

std::string hw_device_type_to_string(AVHWDeviceType device_type) {
    const char* name = av_hwdevice_get_type_name(device_type);
    if (name == nullptr) {
        return "unknown";
    }

    return std::string(name);
}

const AVCodecHWConfig* find_decoder_hw_config(const AVCodec* decoder, AVHWDeviceType device_type) {
    for (int index = 0;; ++index) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, index);
        if (config == nullptr) {
            break;
        }

        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0
            && config->device_type == device_type) {
            return config;
        }
    }

    return nullptr;
}

std::vector<AVHWDeviceType> resolve_decoder_hw_device_priority_order() {
    return {
        AV_HWDEVICE_TYPE_D3D11VA,
        AV_HWDEVICE_TYPE_QSV,
        AV_HWDEVICE_TYPE_CUDA,
        AV_HWDEVICE_TYPE_DXVA2,
    };
}

#endif

}  // namespace

std::string_view decoded_video_frame_path_name(DecodedVideoFramePath path) {
    switch (path) {
    case DecodedVideoFramePath::kSharedMemoryBgra:
        return "shared-memory-bgra";
    case DecodedVideoFramePath::kSoftwareDecodeBgra:
        return "software-decode-bgra";
    case DecodedVideoFramePath::kHardwareDecodeCpuTransferBgra:
        return "hardware-decode-cpu-transfer-bgra";
    case DecodedVideoFramePath::kD3D11DecodeSurface:
        return "d3d11-decode-surface";
    case DecodedVideoFramePath::kUnknown:
        break;
    }
    return "unknown";
}

class DecodedFrameBufferPool::Impl {
public:
    explicit Impl(DecodedFrameBufferPoolConfig config)
        : config_(config) {}

    DecodedVideoFrame acquire(std::size_t required_pixel_bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++telemetry_.acquire_requests;

        auto best = buffers_.end();
        for (auto candidate = buffers_.begin(); candidate != buffers_.end(); ++candidate) {
            if (candidate->pixels.capacity() < required_pixel_bytes) {
                continue;
            }
            if (best == buffers_.end()
                || candidate->pixels.capacity() < best->pixels.capacity()) {
                best = candidate;
            }
        }

        if (best == buffers_.end()) {
            update_retained_telemetry();
            return {};
        }

        DecodedVideoFrame frame = std::move(*best);
        buffers_.erase(best);
        ++telemetry_.reuse_hits;
        reset_frame_metadata(&frame);
        update_retained_telemetry();
        return frame;
    }

    void release(DecodedVideoFrame frame) {
        reset_frame_metadata(&frame);
        const std::size_t buffer_bytes = frame.pixels.capacity();

        std::lock_guard<std::mutex> lock(mutex_);
        const bool individually_valid = buffer_bytes > 0
            && buffer_bytes <= config_.max_buffer_bytes
            && buffer_bytes <= config_.max_total_bytes;
        if (individually_valid) {
            while (!buffers_.empty()) {
                const std::size_t retained_bytes = calculate_retained_bytes();
                const bool count_fits = buffers_.size() < config_.max_buffers;
                const bool bytes_fit = retained_bytes <= config_.max_total_bytes
                    && buffer_bytes <= config_.max_total_bytes - retained_bytes;
                if (count_fits && bytes_fit) {
                    break;
                }

                const auto smallest = std::min_element(
                    buffers_.begin(),
                    buffers_.end(),
                    [](const DecodedVideoFrame& lhs, const DecodedVideoFrame& rhs) {
                        return lhs.pixels.capacity() < rhs.pixels.capacity();
                    });
                if (smallest == buffers_.end()
                    || smallest->pixels.capacity() >= buffer_bytes) {
                    break;
                }
                buffers_.erase(smallest);
                ++telemetry_.evicted_buffers;
            }
        }

        const std::size_t retained_bytes = calculate_retained_bytes();
        const bool retain = individually_valid
            && buffers_.size() < config_.max_buffers
            && retained_bytes <= config_.max_total_bytes
            && buffer_bytes <= config_.max_total_bytes - retained_bytes;
        if (retain) {
            buffers_.push_back(std::move(frame));
            ++telemetry_.accepted_releases;
        } else if (buffer_bytes > 0) {
            ++telemetry_.discarded_releases;
        }
        update_retained_telemetry();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        buffers_.clear();
        telemetry_ = DecodedFrameBufferPoolTelemetry{};
    }

    DecodedFrameBufferPoolTelemetry telemetry() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return telemetry_;
    }

private:
    static void reset_frame_metadata(DecodedVideoFrame* frame) {
        frame->width = 0;
        frame->height = 0;
        frame->row_pitch = 0;
        frame->timestamp_ms = 0;
        frame->bgra = true;
        frame->output_path = DecodedVideoFramePath::kUnknown;
        frame->pixels.clear();
#if defined(_WIN32)
        frame->d3d11_surface.reset();
#endif
    }

    std::size_t calculate_retained_bytes() const {
        std::size_t total = 0;
        for (const auto& frame : buffers_) {
            total += frame.pixels.capacity();
        }
        return total;
    }

    void update_retained_telemetry() {
        telemetry_.retained_buffers = buffers_.size();
        telemetry_.retained_bytes = calculate_retained_bytes();
    }

    DecodedFrameBufferPoolConfig config_;
    mutable std::mutex mutex_;
    std::vector<DecodedVideoFrame> buffers_;
    DecodedFrameBufferPoolTelemetry telemetry_;
};

DecodedFrameBufferPool::DecodedFrameBufferPool(DecodedFrameBufferPoolConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

DecodedFrameBufferPool::~DecodedFrameBufferPool() = default;

DecodedVideoFrame DecodedFrameBufferPool::acquire(std::size_t required_pixel_bytes) {
    return impl_->acquire(required_pixel_bytes);
}

void DecodedFrameBufferPool::release(DecodedVideoFrame frame) {
    impl_->release(std::move(frame));
}

void DecodedFrameBufferPool::clear() {
    impl_->clear();
}

DecodedFrameBufferPoolTelemetry DecodedFrameBufferPool::telemetry() const {
    return impl_->telemetry();
}

RuntimeStatusTimeline::RuntimeStatusTimeline(RuntimeStatusTimelineConfig config)
    : config_(config) {}

void RuntimeStatusTimeline::append(RuntimeStatusEvent event) {
    if (config_.max_events == 0) {
        return;
    }

    if (events_.size() >= config_.max_events) {
        events_.pop_front();
    }
    events_.push_back(std::move(event));
}

void RuntimeStatusTimeline::clear() {
    events_.clear();
}

std::size_t RuntimeStatusTimeline::size() const {
    return events_.size();
}

std::vector<RuntimeStatusEvent> RuntimeStatusTimeline::latest(std::size_t max_events) const {
    if (max_events == 0 || events_.empty()) {
        return {};
    }

    const std::size_t count = std::min(max_events, events_.size());
    return std::vector<RuntimeStatusEvent>(events_.end() - static_cast<std::ptrdiff_t>(count), events_.end());
}

RuntimeStatusTimelineUiModel::RuntimeStatusTimelineUiModel(RuntimeStatusTimelineUiModelConfig config)
    : config_(std::move(config)) {}

void RuntimeStatusTimelineUiModel::bind(const RuntimeStatusTimeline* timeline) {
    timeline_ = timeline;
}

void RuntimeStatusTimelineUiModel::refresh() {
    items_.clear();
    if (timeline_ == nullptr || config_.max_items == 0) {
        return;
    }

    const auto events = timeline_->latest(config_.max_items);
    items_.reserve(events.size());
    for (const auto& event : events) {
        RuntimeStatusTimelineUiItem item;
        item.timestamp_ms = event.timestamp_ms;
        item.severity = event.severity;
        item.title = event.category;
        item.subtitle = event.message;
        items_.push_back(std::move(item));
    }
}

void RuntimeStatusTimelineUiModel::clear() {
    items_.clear();
}

const std::string& RuntimeStatusTimelineUiModel::widget_title() const {
    return config_.widget_title;
}

const std::vector<RuntimeStatusTimelineUiItem>& RuntimeStatusTimelineUiModel::items() const {
    return items_;
}

void RuntimeStatusTimelineWidgetComponent::bind(const RuntimeStatusTimelineUiModel* model) {
    model_ = model;
}

void RuntimeStatusTimelineWidgetComponent::refresh() {
    state_ = RuntimeStatusTimelineWidgetState{};
    if (model_ == nullptr) {
        return;
    }

    state_.title = model_->widget_title();
    state_.items = model_->items();
    for (const auto& item : state_.items) {
        if (item.severity == RuntimeStatusSeverity::kError) {
            state_.has_error = true;
        }
        if (item.severity == RuntimeStatusSeverity::kWarning || item.severity == RuntimeStatusSeverity::kError) {
            state_.has_warning = true;
        }
    }
}

void RuntimeStatusTimelineWidgetComponent::clear() {
    state_ = RuntimeStatusTimelineWidgetState{};
}

const RuntimeStatusTimelineWidgetState& RuntimeStatusTimelineWidgetComponent::state() const {
    return state_;
}

class FfmpegVideoFrameDecoder::Impl {
public:
    bool decode_frame_view(
        EncodedVideoCodec codec,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t timestamp_ms,
        const std::uint8_t* payload_data,
        std::size_t payload_size,
        bool* frame_ready,
        DecodedVideoFrame* output,
        std::string* error_detail) {
        if (frame_ready != nullptr) {
            *frame_ready = false;
        }
        const bool output_pixels = output != nullptr;
        if (!output_pixels && frame_ready == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "decoded output pointer is null";
            }
            return false;
        }

        if (payload_data == nullptr || payload_size == 0 || width == 0 || height == 0) {
            if (error_detail != nullptr) {
                *error_detail = "encoded frame payload and dimensions are required";
            }
            return false;
        }

#if REDCLAW_RENDER_HAS_LIBAVCODEC
        if (!ensure_decoder(codec, error_detail)) {
            return false;
        }

        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "av_packet_alloc returned null";
            }
            return false;
        }

        const int new_packet_result = av_new_packet(packet, static_cast<int>(payload_size));
        if (new_packet_result < 0) {
            av_packet_free(&packet);
            if (error_detail != nullptr) {
                *error_detail = "av_new_packet failed: " + ffmpeg_error_to_string(new_packet_result);
            }
            return false;
        }

        std::memcpy(packet->data, payload_data, payload_size);
        packet->pts = static_cast<std::int64_t>(timestamp_ms);

        const int send_result = avcodec_send_packet(context_, packet);
        av_packet_free(&packet);
        if (send_result < 0) {
            if (error_detail != nullptr) {
                *error_detail = "avcodec_send_packet failed: " + ffmpeg_error_to_string(send_result);
            }
            return false;
        }

        const int receive_result = avcodec_receive_frame(context_, frame_);
        if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
            if (frame_ready != nullptr) {
                if (error_detail != nullptr) {
                    error_detail->clear();
                }
                return true;
            }
            if (error_detail != nullptr) {
                *error_detail = "decoder produced no output frame";
            }
            return false;
        }
        if (receive_result < 0) {
            if (error_detail != nullptr) {
                *error_detail = "avcodec_receive_frame failed: " + ffmpeg_error_to_string(receive_result);
            }
            return false;
        }

        if (!output_pixels) {
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            if (frame_ready != nullptr) {
                *frame_ready = true;
            }
            return true;
        }

#if defined(_WIN32)
        if (using_hardware_decode_
            && hw_device_type_ == AV_HWDEVICE_TYPE_D3D11VA
            && hw_pixel_format_ == AV_PIX_FMT_D3D11
            && frame_->format == AV_PIX_FMT_D3D11
            && d3d11_surface_output_device_ != nullptr
            && output_d3d11_surface(frame_, timestamp_ms, output)) {
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            if (frame_ready != nullptr) {
                *frame_ready = true;
            }
            return true;
        }
#endif

        AVFrame* source_frame = frame_;
        bool transferred_from_hardware = false;
        if (using_hardware_decode_ && hw_pixel_format_ != AV_PIX_FMT_NONE && frame_->format == hw_pixel_format_) {
            if (transfer_frame_ == nullptr) {
                if (error_detail != nullptr) {
                    *error_detail = "hardware decoder transfer frame is unavailable";
                }
                return false;
            }

            av_frame_unref(transfer_frame_);
            const int transfer_result = av_hwframe_transfer_data(transfer_frame_, frame_, 0);
            if (transfer_result < 0) {
                if (error_detail != nullptr) {
                    *error_detail = "av_hwframe_transfer_data failed: "
                        + ffmpeg_error_to_string(transfer_result);
                }
                return false;
            }

            source_frame = transfer_frame_;
            transferred_from_hardware = true;
        }

        const int source_width = source_frame->width;
        const int source_height = source_frame->height;
        if (source_width <= 0 || source_height <= 0) {
            if (error_detail != nullptr) {
                *error_detail = "decoder returned invalid frame dimensions";
            }
            return false;
        }

        sws_context_ = sws_getCachedContext(
            sws_context_,
            source_width,
            source_height,
            static_cast<AVPixelFormat>(source_frame->format),
            source_width,
            source_height,
            AV_PIX_FMT_BGRA,
            SWS_FAST_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (sws_context_ == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "sws_getCachedContext returned null";
            }
            return false;
        }

        output->width = static_cast<std::uint32_t>(source_width);
        output->height = static_cast<std::uint32_t>(source_height);
        output->row_pitch = output->width * 4;
        output->timestamp_ms = timestamp_ms;
        output->bgra = true;
        output->output_path = transferred_from_hardware
            ? DecodedVideoFramePath::kHardwareDecodeCpuTransferBgra
            : DecodedVideoFramePath::kSoftwareDecodeBgra;
#if defined(_WIN32)
        output->d3d11_surface.reset();
#endif
        output->pixels.resize(static_cast<std::size_t>(output->row_pitch) * output->height);

        std::uint8_t* dst_data[4] = {output->pixels.data(), nullptr, nullptr, nullptr};
        int dst_linesize[4] = {static_cast<int>(output->row_pitch), 0, 0, 0};
        const int scaled_rows = sws_scale(
            sws_context_,
            source_frame->data,
            source_frame->linesize,
            0,
            source_height,
            dst_data,
            dst_linesize);
        if (scaled_rows != source_height) {
            if (error_detail != nullptr) {
                *error_detail = "sws_scale did not convert the full frame";
            }
            return false;
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        if (frame_ready != nullptr) {
            *frame_ready = true;
        }
        return true;
#else
        (void)codec;
        (void)width;
        (void)height;
        (void)timestamp_ms;
        (void)payload_data;
        (void)payload_size;
        (void)frame_ready;
        if (error_detail != nullptr) {
            *error_detail = "libavcodec/libswscale headers are unavailable at build time; decoder path is disabled";
        }
        return false;
#endif
    }

    bool decode_frame(const EncodedVideoFrame& input, DecodedVideoFrame* output, std::string* error_detail) {
        return decode_frame_view(
            input.codec,
            input.width,
            input.height,
            input.timestamp_ms,
            input.payload.data(),
            input.payload.size(),
            nullptr,
            output,
            error_detail);
    }

    void reset() {
#if REDCLAW_RENDER_HAS_LIBAVCODEC
        if (sws_context_ != nullptr) {
            sws_freeContext(sws_context_);
            sws_context_ = nullptr;
        }
        if (transfer_frame_ != nullptr) {
            av_frame_free(&transfer_frame_);
            transfer_frame_ = nullptr;
        }
        if (frame_ != nullptr) {
            av_frame_free(&frame_);
            frame_ = nullptr;
        }
        if (context_ != nullptr) {
            avcodec_free_context(&context_);
            context_ = nullptr;
        }
        if (hw_device_context_ != nullptr) {
            av_buffer_unref(&hw_device_context_);
            hw_device_context_ = nullptr;
        }
        codec_id_ = AV_CODEC_ID_NONE;
        hw_device_type_ = AV_HWDEVICE_TYPE_NONE;
        hw_pixel_format_ = AV_PIX_FMT_NONE;
        using_hardware_decode_ = false;
        active_decoder_name_.clear();
#endif
    }

    void force_software_decode() {
        force_software_decode_ = true;
        reset();
    }

    [[nodiscard]] bool using_hardware_decode() const {
#if REDCLAW_RENDER_HAS_LIBAVCODEC
        return using_hardware_decode_;
#else
        return false;
#endif
    }

    [[nodiscard]] bool software_decode_forced() const {
        return force_software_decode_;
    }

    [[nodiscard]] std::string active_decoder_name() const {
        if (!active_decoder_name_.empty()) {
            return active_decoder_name_;
        }
        return force_software_decode_ ? "software-pending" : "unopened";
    }

#if defined(_WIN32)
    void configure_d3d11_surface_output(ID3D11Device* device) {
        force_software_decode_ = false;
        reset();
        if (d3d11_surface_output_device_ == device) {
            return;
        }
        if (d3d11_surface_output_device_ != nullptr) {
            d3d11_surface_output_device_->Release();
        }
        d3d11_surface_output_device_ = device;
        if (d3d11_surface_output_device_ != nullptr) {
            d3d11_surface_output_device_->AddRef();
        }
    }
#endif

    ~Impl() {
        reset();
#if defined(_WIN32)
        if (d3d11_surface_output_device_ != nullptr) {
            d3d11_surface_output_device_->Release();
            d3d11_surface_output_device_ = nullptr;
        }
#endif
    }

private:
#if REDCLAW_RENDER_HAS_LIBAVCODEC
#if defined(_WIN32)
    bool output_d3d11_surface(
        AVFrame* decoded_frame,
        std::uint64_t timestamp_ms,
        DecodedVideoFrame* output) const {
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(decoded_frame->data[0]);
        if (texture == nullptr || output == nullptr) {
            return false;
        }

        AVFrame* retained_frame = av_frame_clone(decoded_frame);
        if (retained_frame == nullptr) {
            return false;
        }

        D3D11_TEXTURE2D_DESC texture_desc{};
        texture->GetDesc(&texture_desc);

        auto surface = std::make_shared<D3D11DecodedSurface>();
        surface->device = d3d11_surface_output_device_;
        surface->texture = texture;
        surface->array_slice = static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(decoded_frame->data[1]));
        surface->dxgi_format = static_cast<std::uint32_t>(texture_desc.Format);
        surface->frame_lifetime = std::shared_ptr<void>(
            retained_frame,
            [](void* pointer) {
                auto* frame = static_cast<AVFrame*>(pointer);
                av_frame_free(&frame);
            });

        output->width = static_cast<std::uint32_t>(decoded_frame->width);
        output->height = static_cast<std::uint32_t>(decoded_frame->height);
        output->row_pitch = 0;
        output->timestamp_ms = timestamp_ms;
        output->bgra = false;
        output->output_path = DecodedVideoFramePath::kD3D11DecodeSurface;
        output->pixels.clear();
        output->d3d11_surface = std::move(surface);
        return true;
    }
#endif

    static AVPixelFormat select_hardware_pixel_format(AVCodecContext* context, const AVPixelFormat* pixel_formats) {
        const auto* impl = static_cast<const Impl*>(context->opaque);
        if (impl != nullptr && impl->hw_pixel_format_ != AV_PIX_FMT_NONE) {
            for (const AVPixelFormat* pixel_format = pixel_formats;
                 pixel_formats != nullptr && *pixel_format != AV_PIX_FMT_NONE;
                 ++pixel_format) {
                if (*pixel_format == impl->hw_pixel_format_) {
                    return *pixel_format;
                }
            }
        }

        return (pixel_formats != nullptr) ? pixel_formats[0] : AV_PIX_FMT_NONE;
    }

    bool try_open_decoder_software(const AVCodec* decoder, std::string* error_detail) {
        reset();

        context_ = avcodec_alloc_context3(decoder);
        if (context_ == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "avcodec_alloc_context3 returned null";
            }
            return false;
        }

        const int open_result = avcodec_open2(context_, decoder, nullptr);
        if (open_result < 0) {
            if (error_detail != nullptr) {
                *error_detail = "avcodec_open2 failed: " + ffmpeg_error_to_string(open_result);
            }
            reset();
            return false;
        }

        frame_ = av_frame_alloc();
        if (frame_ == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "av_frame_alloc returned null";
            }
            reset();
            return false;
        }

        using_hardware_decode_ = false;
        hw_device_type_ = AV_HWDEVICE_TYPE_NONE;
        hw_pixel_format_ = AV_PIX_FMT_NONE;
        active_decoder_name_ = std::string("software:")
            + (decoder->name == nullptr ? "unknown" : decoder->name);
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool try_open_decoder_hardware(
        const AVCodec* decoder,
        const AVCodecHWConfig* hw_config,
        AVHWDeviceType device_type,
        std::string* error_detail) {
        reset();

        context_ = avcodec_alloc_context3(decoder);
        if (context_ == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "avcodec_alloc_context3 returned null";
            }
            return false;
        }

        hw_pixel_format_ = hw_config->pix_fmt;
        hw_device_type_ = device_type;
        context_->opaque = this;
        context_->get_format = &Impl::select_hardware_pixel_format;

        int hw_device_result = 0;
#if defined(_WIN32)
        if (device_type == AV_HWDEVICE_TYPE_D3D11VA && d3d11_surface_output_device_ != nullptr) {
            hw_device_context_ = av_hwdevice_ctx_alloc(device_type);
            if (hw_device_context_ == nullptr) {
                hw_device_result = AVERROR(ENOMEM);
            } else {
                auto* generic_context = reinterpret_cast<AVHWDeviceContext*>(hw_device_context_->data);
                auto* d3d11_context = static_cast<AVD3D11VADeviceContext*>(generic_context->hwctx);
                d3d11_surface_output_device_->AddRef();
                d3d11_context->device = d3d11_surface_output_device_;
                hw_device_result = av_hwdevice_ctx_init(hw_device_context_);
            }
        } else
#endif
        {
            hw_device_result = av_hwdevice_ctx_create(&hw_device_context_, device_type, nullptr, nullptr, 0);
        }
        if (hw_device_result < 0) {
            if (error_detail != nullptr) {
                *error_detail = "av_hwdevice_ctx_create failed for "
                    + hw_device_type_to_string(device_type) + ": "
                    + ffmpeg_error_to_string(hw_device_result);
            }
            reset();
            return false;
        }

        context_->hw_device_ctx = av_buffer_ref(hw_device_context_);
        if (context_->hw_device_ctx == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "av_buffer_ref returned null for decoder hw_device_ctx";
            }
            reset();
            return false;
        }

        const int open_result = avcodec_open2(context_, decoder, nullptr);
        if (open_result < 0) {
            if (error_detail != nullptr) {
                *error_detail = "avcodec_open2 failed for "
                    + hw_device_type_to_string(device_type) + ": "
                    + ffmpeg_error_to_string(open_result);
            }
            reset();
            return false;
        }

        frame_ = av_frame_alloc();
        transfer_frame_ = av_frame_alloc();
        if (frame_ == nullptr || transfer_frame_ == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "av_frame_alloc returned null";
            }
            reset();
            return false;
        }

        using_hardware_decode_ = true;
        active_decoder_name_ = hw_device_type_to_string(device_type)
            + ":"
            + (decoder->name == nullptr ? "unknown" : decoder->name);
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool ensure_decoder(EncodedVideoCodec codec, std::string* error_detail) {
        const AVCodecID requested_codec_id = resolve_decoder_codec_id(codec);
        if (context_ != nullptr && codec_id_ == requested_codec_id) {
            return true;
        }

        reset();

        const AVCodec* decoder = avcodec_find_decoder(requested_codec_id);
        if (decoder == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "no libavcodec decoder found";
            }
            return false;
        }

        std::string hardware_errors;
        if (!force_software_decode_) {
            for (const AVHWDeviceType device_type : resolve_decoder_hw_device_priority_order()) {
                const AVCodecHWConfig* hw_config = find_decoder_hw_config(decoder, device_type);
                if (hw_config == nullptr) {
                    continue;
                }

                std::string hardware_error;
                if (try_open_decoder_hardware(decoder, hw_config, device_type, &hardware_error)) {
                    codec_id_ = requested_codec_id;
                    if (error_detail != nullptr) {
                        error_detail->clear();
                    }
                    return true;
                }

                if (!hardware_errors.empty()) {
                    hardware_errors += "; ";
                }
                hardware_errors += hardware_error;
            }
        }

        std::string software_error;
        if (!try_open_decoder_software(decoder, &software_error)) {
            if (error_detail != nullptr) {
                *error_detail = hardware_errors;
                if (!error_detail->empty() && !software_error.empty()) {
                    *error_detail += "; ";
                }
                *error_detail += software_error;
            }
            return false;
        }

        codec_id_ = requested_codec_id;
        return true;
    }

    AVCodecID codec_id_ = AV_CODEC_ID_NONE;
    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* transfer_frame_ = nullptr;
    AVBufferRef* hw_device_context_ = nullptr;
    AVHWDeviceType hw_device_type_ = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat hw_pixel_format_ = AV_PIX_FMT_NONE;
    bool using_hardware_decode_ = false;
    SwsContext* sws_context_ = nullptr;
#endif
    bool force_software_decode_ = false;
    std::string active_decoder_name_;
#if defined(_WIN32)
    ID3D11Device* d3d11_surface_output_device_ = nullptr;
#endif
};

FfmpegVideoFrameDecoder::FfmpegVideoFrameDecoder()
    : impl_(std::make_unique<Impl>()) {}

FfmpegVideoFrameDecoder::~FfmpegVideoFrameDecoder() = default;

bool FfmpegVideoFrameDecoder::decode_frame_view(
    EncodedVideoCodec codec,
    std::uint32_t width,
    std::uint32_t height,
    std::uint64_t timestamp_ms,
    bool,
    const std::uint8_t* payload_data,
    std::size_t payload_size,
    bool* frame_ready,
    DecodedVideoFrame* output,
    std::string* error_detail) {
    return impl_->decode_frame_view(
        codec,
        width,
        height,
        timestamp_ms,
        payload_data,
        payload_size,
        frame_ready,
        output,
        error_detail);
}

bool FfmpegVideoFrameDecoder::decode_frame(
    const EncodedVideoFrame& input,
    DecodedVideoFrame* output,
    std::string* error_detail) {
    return impl_->decode_frame(input, output, error_detail);
}

#if defined(_WIN32)
void FfmpegVideoFrameDecoder::configure_d3d11_surface_output(ID3D11Device* device) {
    impl_->configure_d3d11_surface_output(device);
}
#endif

void FfmpegVideoFrameDecoder::reset() {
    impl_->reset();
}

void FfmpegVideoFrameDecoder::force_software_decode() {
    impl_->force_software_decode();
}

bool FfmpegVideoFrameDecoder::using_hardware_decode() const {
    return impl_->using_hardware_decode();
}

bool FfmpegVideoFrameDecoder::software_decode_forced() const {
    return impl_->software_decode_forced();
}

std::string FfmpegVideoFrameDecoder::active_decoder_name() const {
    return impl_->active_decoder_name();
}

bool should_fallback_to_software_decode(
    bool using_hardware_decode,
    std::uint32_t consecutive_decode_failures,
    std::uint64_t decoded_frames) {
    return using_hardware_decode
        && decoded_frames == 0
        && consecutive_decode_failures >= 1;
}

std::string render_runtime_status_timeline_text(
    const RuntimeStatusTimeline& timeline,
    std::size_t max_events) {
    const auto events = timeline.latest(max_events);
    if (events.empty()) {
        return "";
    }

    std::ostringstream out;
    for (const auto& event : events) {
        std::string severity = "INFO";
        if (event.severity == RuntimeStatusSeverity::kWarning) {
            severity = "WARN";
        } else if (event.severity == RuntimeStatusSeverity::kError) {
            severity = "ERROR";
        }

        out << "[" << event.timestamp_ms << "]"
            << "[" << severity << "]"
            << "[" << event.category << "] "
            << event.message
            << '\n';
    }

    return out.str();
}

std::string render_runtime_status_timeline_widget_text(
    const RuntimeStatusTimelineUiModel& model) {
    if (model.items().empty()) {
        return "";
    }

    std::ostringstream out;
    out << model.widget_title() << '\n';
    for (const auto& item : model.items()) {
        std::string severity = "INFO";
        if (item.severity == RuntimeStatusSeverity::kWarning) {
            severity = "WARN";
        } else if (item.severity == RuntimeStatusSeverity::kError) {
            severity = "ERROR";
        }

        out << "[" << item.timestamp_ms << "]"
            << "[" << severity << "]"
            << "[" << item.title << "] "
            << item.subtitle
            << '\n';
    }

    return out.str();
}

bool compute_render_viewport_layout(
    std::uint32_t frame_width,
    std::uint32_t frame_height,
    const RenderViewportConfig& config,
    RenderViewportLayout* layout,
    std::string* error_detail) {
    if (layout == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "viewport layout output is required";
        }
        return false;
    }

    if (frame_width == 0 || frame_height == 0) {
        if (error_detail != nullptr) {
            *error_detail = "frame dimensions must be non-zero";
        }
        return false;
    }

    if (config.viewport_width == 0 || config.viewport_height == 0) {
        if (error_detail != nullptr) {
            *error_detail = "viewport dimensions must be non-zero";
        }
        return false;
    }

    const double scale = resolve_scale(frame_width, frame_height, config);
    if (scale <= 0.0) {
        if (error_detail != nullptr) {
            *error_detail = "computed viewport scale is invalid";
        }
        return false;
    }

    const double scaled_width = static_cast<double>(frame_width) * scale;
    const double scaled_height = static_cast<double>(frame_height) * scale;

    const double dest_x = (static_cast<double>(config.viewport_width) - scaled_width) * 0.5
        + static_cast<double>(config.pan_x);
    const double dest_y = (static_cast<double>(config.viewport_height) - scaled_height) * 0.5
        + static_cast<double>(config.pan_y);

    const double visible_left = std::max(0.0, dest_x);
    const double visible_top = std::max(0.0, dest_y);
    const double visible_right = std::min(static_cast<double>(config.viewport_width), dest_x + scaled_width);
    const double visible_bottom = std::min(static_cast<double>(config.viewport_height), dest_y + scaled_height);

    if (visible_right <= visible_left || visible_bottom <= visible_top) {
        if (error_detail != nullptr) {
            *error_detail = "panned frame is outside viewport bounds";
        }
        return false;
    }

    const double source_left = (visible_left - dest_x) / scale;
    const double source_top = (visible_top - dest_y) / scale;
    const double source_right = (visible_right - dest_x) / scale;
    const double source_bottom = (visible_bottom - dest_y) / scale;

    const auto source_x = static_cast<std::int32_t>(std::floor(source_left));
    const auto source_y = static_cast<std::int32_t>(std::floor(source_top));
    const auto source_w = static_cast<std::int32_t>(std::ceil(source_right) - std::floor(source_left));
    const auto source_h = static_cast<std::int32_t>(std::ceil(source_bottom) - std::floor(source_top));

    const auto destination_x = static_cast<std::int32_t>(std::floor(visible_left));
    const auto destination_y = static_cast<std::int32_t>(std::floor(visible_top));
    const auto destination_w = static_cast<std::int32_t>(std::ceil(visible_right) - std::floor(visible_left));
    const auto destination_h = static_cast<std::int32_t>(std::ceil(visible_bottom) - std::floor(visible_top));

    layout->source.x = std::max(0, source_x);
    layout->source.y = std::max(0, source_y);
    layout->source.width = static_cast<std::uint32_t>(std::max(1, std::min(source_w, static_cast<std::int32_t>(frame_width))));
    layout->source.height = static_cast<std::uint32_t>(std::max(1, std::min(source_h, static_cast<std::int32_t>(frame_height))));

    layout->destination.x = std::max(0, destination_x);
    layout->destination.y = std::max(0, destination_y);
    layout->destination.width = static_cast<std::uint32_t>(std::max(1, std::min(destination_w, static_cast<std::int32_t>(config.viewport_width))));
    layout->destination.height = static_cast<std::uint32_t>(std::max(1, std::min(destination_h, static_cast<std::int32_t>(config.viewport_height))));
    layout->scale = scale;
    return true;
}

DecodedFrameQueue::DecodedFrameQueue(DecodedFrameQueueConfig config)
    : config_(config) {}

QueuePushStatus DecodedFrameQueue::push(DecodedVideoFrame frame) {
    if (config_.max_frames == 0) {
        return QueuePushStatus::kRejectedFull;
    }

    if (queue_.size() >= config_.max_frames) {
        if (!config_.drop_oldest_on_overflow) {
            return QueuePushStatus::kRejectedFull;
        }

        queue_.pop_front();
        ++dropped_frame_count_;
        queue_.push_back(std::move(frame));
        return QueuePushStatus::kDroppedOldest;
    }

    queue_.push_back(std::move(frame));
    return QueuePushStatus::kAccepted;
}

bool DecodedFrameQueue::pop(DecodedVideoFrame* frame) {
    if (frame == nullptr || queue_.empty()) {
        return false;
    }

    *frame = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void DecodedFrameQueue::clear() {
    queue_.clear();
}

std::size_t DecodedFrameQueue::size() const {
    return queue_.size();
}

bool DecodedFrameQueue::empty() const {
    return queue_.empty();
}

std::size_t DecodedFrameQueue::dropped_frame_count() const {
    return dropped_frame_count_;
}

std::string_view module_name() {
    return "render";
}
}
