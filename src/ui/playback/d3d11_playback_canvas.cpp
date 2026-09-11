#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "playback_backends.h"
#include "ui/gui_latency_probe.h"
#include <QDateTime>
#include <QPaintEngine>
#include <QResizeEvent>
#include <QShowEvent>
#include <algorithm>
#include <cstring>
#include <vector>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <Windows.h>
namespace redclaw::ui {
PresentOutcome classify_dxgi_present_result(std::int32_t result) {
  if (result == DXGI_ERROR_WAS_STILL_DRAWING || (SUCCEEDED(result) && result != S_OK))
    return PresentOutcome::kBusyDrop;
  return FAILED(result) ? PresentOutcome::kFailed : PresentOutcome::kPresented;
}
namespace {
using Microsoft::WRL::ComPtr;
using D3DCompileProc = HRESULT(WINAPI*)(
    LPCVOID,
    SIZE_T,
    LPCSTR,
    const D3D_SHADER_MACRO*,
    ID3DInclude*,
    LPCSTR,
    LPCSTR,
    UINT,
    UINT,
    ID3DBlob**,
    ID3DBlob**);

D3DCompileProc load_d3d_compile() {
  static HMODULE compiler_module = []() {
    HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
    if (module == nullptr) {
      module = LoadLibraryW(L"d3dcompiler_46.dll");
    }
    return module;
  }();
  if (compiler_module == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<D3DCompileProc>(GetProcAddress(compiler_module, "D3DCompile"));
}

class D3D11PlaybackWidget final : public QWidget, public PlaybackCanvas {
 public:
  explicit D3D11PlaybackWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAutoFillBackground(false);
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }

  QPaintEngine* paintEngine() const override {
    return nullptr;
  }

  [[nodiscard]] QString backend_name() const override { return "d3d11"; }
  QJsonObject diagnostic_snapshot() const override {
    D3D11_TEXTURE2D_DESC description{};
    if (video_input_texture_) video_input_texture_->GetDesc(&description);
    return {{"backend", "d3d11"}, {"last_presented_surface", latest_frame_is_surface_},
      {"encoded_visible_width", int(video_source_width_)}, {"encoded_visible_height", int(video_source_height_)},
      {"texture_width", int(description.Width)}, {"texture_height", int(description.Height)},
      {"texture_format", int(description.Format)}, {"texture_array_size", int(description.ArraySize)},
      {"output_buffer_width", swap_chain_phys_size_.width()}, {"output_buffer_height", swap_chain_phys_size_.height()},
      {"geometry_transaction_active", geometry_transaction_active_}, {"resize_count", qint64(swap_chain_resize_count_)}};
  }

  void clear_frame() override {
    frame_texture_.Reset();
    frame_srv_.Reset();
    video_input_texture_.Reset();
    video_input_views_.clear();
    latest_frame_is_surface_ = false;
    frame_width_ = 0;
    frame_height_ = 0;
    if (context_ && render_target_ && swap_chain_) {
      const FLOAT clear_color[4] = {0.008F, 0.024F, 0.090F, 1.0F};
      context_->ClearRenderTargetView(render_target_.Get(), clear_color);
      (void)present_swap_chain(0, nullptr, true);
    }
  }

  void set_geometry_transaction_active(bool active) override {
    geometry_transaction_active_ = active;
  }

  void commit_geometry(std::uint64_t transaction_id) override {
    Q_UNUSED(transaction_id);
    geometry_transaction_active_ = false;
    const QSize phys = current_phys_size();
    if (phys != last_phys_size_) {
      last_phys_size_ = phys;
      sync_swap_chain_size();
    }
  }

  [[nodiscard]] std::uint64_t swap_chain_resize_count() const override {
    return swap_chain_resize_count_;
  }

  ID3D11Device* d3d11_decode_device(QString* error_detail) override {
    if (!ensure_device(error_detail)) {
      return nullptr;
    }
    if ((FAILED(device_.As(&video_device_)) || !video_device_)
        || (FAILED(context_.As(&video_context_)) || !video_context_)) {
      if (error_detail != nullptr) {
        *error_detail = "D3D11 video processing interfaces are unavailable.";
      }
      return nullptr;
    }
    return device_.Get();
  }

  PresentOutcome present_frame(const DirectFrameData& frame, QString* error_detail) override {
    GuiLatencyScope phase(frame_has_presented_ ? GuiStage::kPresentSteady : GuiStage::kPresentInitialize);
    GuiLatencyScope preparation(GuiStage::kPresentPrepare);
    if (frame.d3d11_surface != nullptr) {
      if (!ensure_device(error_detail) || !ensure_swap_chain(error_detail)) {
        return PresentOutcome::kFailed;
      }
      sync_swap_chain_size();
      preparation.finish();
      const auto outcome = present_d3d11_surface(frame, error_detail);
      if (outcome == PresentOutcome::kPresented) {
        latest_frame_is_surface_ = true;
        frame_has_presented_ = true;
      }
      return outcome;
    }

    if (frame.width == 0 || frame.height == 0 || frame.row_pitch < frame.width * 4U || frame.pixels.empty()) {
      if (error_detail != nullptr) {
        *error_detail = "Direct frame is empty.";
      }
      return PresentOutcome::kFailed;
    }
    if (!ensure_device(error_detail) || !ensure_swap_chain(error_detail) || !ensure_shaders(error_detail)) {
      return PresentOutcome::kFailed;
    }
    sync_swap_chain_size();
    const quint32 content_x = frame.content_rect_x;
    const quint32 content_y = frame.content_rect_y;
    const quint32 content_width = frame.content_rect_width == 0
        ? frame.width : frame.content_rect_width;
    const quint32 content_height = frame.content_rect_height == 0
        ? frame.height : frame.content_rect_height;
    if (content_x + content_width > frame.width
        || content_y + content_height > frame.height
        || !ensure_frame_texture(content_width, content_height, error_detail)) {
      return PresentOutcome::kFailed;
    }

    context_->UpdateSubresource(
        frame_texture_.Get(),
        0,
        nullptr,
        frame.pixels.data()
            + static_cast<std::size_t>(content_y) * frame.row_pitch
            + static_cast<std::size_t>(content_x) * 4U,
        frame.row_pitch,
        0);
    latest_frame_is_surface_ = false;
    preparation.finish();
    const auto outcome = render(error_detail);
    frame_has_presented_ |= outcome == PresentOutcome::kPresented;
    return outcome;
  }

  PresentOutcome present_image(const QImage& image, QString* error_detail) override {
    if (image.isNull()) {
      if (error_detail != nullptr) {
        *error_detail = "Playback image is empty.";
      }
      return PresentOutcome::kFailed;
    }

    const QImage bgra = image.convertToFormat(QImage::Format_RGB32);
    DirectFrameData frame;
    frame.width = static_cast<quint32>(bgra.width());
    frame.height = static_cast<quint32>(bgra.height());
    frame.row_pitch = static_cast<quint32>(bgra.bytesPerLine());
    frame.timestamp_ms = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    frame.pixels.assign(
        bgra.constBits(),
        bgra.constBits() + static_cast<std::size_t>(bgra.sizeInBytes()));
    return present_frame(frame, error_detail);
  }

 protected:
  void resizeEvent(QResizeEvent*) override {
    // Only rebuild the swap chain when the physical pixel size actually changed.
    // Qt may deliver resize events with the same logical size (e.g. from layout
    // re-passes triggered by sibling widget text changes), which would cause
    // ResizeBuffers to briefly invalidate the DXGI surface and produce flicker.
    const QSize phys = current_phys_size();
    if (!geometry_transaction_active_ && phys != last_phys_size_) {
      last_phys_size_ = phys;
      sync_swap_chain_size();
    }
    if (frame_texture_ && !latest_frame_is_surface_) {
      (void)render(nullptr, true);
    }
  }

  void paintEvent(QPaintEvent*) override {
    if (frame_texture_ && !latest_frame_is_surface_) {
      (void)render(nullptr, true);
    }
  }

  void showEvent(QShowEvent* event) override {
    QWidget::showEvent(event);
    const QSize phys = current_phys_size();
    if (!geometry_transaction_active_ && phys != last_phys_size_) {
      last_phys_size_ = phys;
      sync_swap_chain_size();
    }
    if (frame_texture_ && !latest_frame_is_surface_) {
      (void)render(nullptr, true);
    }
  }

 private:
  struct Vertex {
    float x;
    float y;
    float u;
    float v;
  };

  bool ensure_device(QString* error_detail) {
    if (device_) {
      return true;
    }

    constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL created_level = D3D_FEATURE_LEVEL_11_0;
    const HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT
            | (qEnvironmentVariableIsSet("REDCLAW_D3D11_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0),
        kFeatureLevels,
        static_cast<UINT>(sizeof(kFeatureLevels) / sizeof(kFeatureLevels[0])),
        D3D11_SDK_VERSION,
        &device_,
        &created_level,
        &context_);
    if (FAILED(hr) || !device_ || !context_) {
      if (error_detail != nullptr) {
        *error_detail = QString("D3D11CreateDevice failed: 0x%1").arg(static_cast<qulonglong>(hr), 0, 16);
      }
      return false;
    }

    ComPtr<ID3D11Multithread> multithread;
    if (FAILED(context_.As(&multithread)) || !multithread) {
      if (error_detail != nullptr) {
        *error_detail = "D3D11 multithread protection is unavailable.";
      }
      device_.Reset();
      context_.Reset();
      return false;
    }
    multithread->SetMultithreadProtected(TRUE);
    return true;
  }

  bool ensure_swap_chain(QString* error_detail) {
    if (swap_chain_) {
      return true;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device_.As(&dxgi_device)) || !dxgi_device) {
      if (error_detail != nullptr) {
        *error_detail = "Failed to query IDXGIDevice.";
      }
      return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter)) || !adapter) {
      if (error_detail != nullptr) {
        *error_detail = "Failed to query DXGI adapter.";
      }
      return false;
    }

    // Prefer DXGI 1.2 flip-model swap chain (Windows 8+).
    // DXGI_SWAP_EFFECT_FLIP_DISCARD ensures DWM always composites the last
    // presented frame, eliminating checkerboard artifacts from undefined
    // back-buffer content between frames.
    ComPtr<IDXGIFactory2> factory2;
    if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2),
                                     reinterpret_cast<void**>(factory2.GetAddressOf())))
        && factory2) {
      DXGI_SWAP_CHAIN_DESC1 desc1{};
      desc1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc1.SampleDesc.Count = 1;
      desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc1.BufferCount = 2;
      desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
      desc1.Scaling = DXGI_SCALING_STRETCH;

      ComPtr<IDXGISwapChain1> swap_chain1;
      const HRESULT flip_hr = factory2->CreateSwapChainForHwnd(
          device_.Get(),
          reinterpret_cast<HWND>(winId()),
          &desc1, nullptr, nullptr,
          &swap_chain1);
      if (SUCCEEDED(flip_hr) && swap_chain1) {
        swap_chain1.As(&swap_chain_);
      }
    }

    if (!swap_chain_) {
      // Fallback: legacy blt-model swap chain (Windows 7 or IDXGIFactory2 unavailable).
      ComPtr<IDXGIFactory> factory;
      if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory),
                                    reinterpret_cast<void**>(factory.GetAddressOf())))
          || !factory) {
        if (error_detail != nullptr) {
          *error_detail = "Failed to query DXGI factory.";
        }
        return false;
      }

      DXGI_SWAP_CHAIN_DESC desc{};
      desc.BufferCount = 2;
      desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.OutputWindow = reinterpret_cast<HWND>(winId());
      desc.SampleDesc.Count = 1;
      desc.Windowed = TRUE;
      desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

      const HRESULT hr = factory->CreateSwapChain(device_.Get(), &desc, &swap_chain_);
      if (FAILED(hr) || !swap_chain_) {
        if (error_detail != nullptr) {
          *error_detail = QString("CreateSwapChain failed: 0x%1").arg(static_cast<qulonglong>(hr), 0, 16);
        }
        return false;
      }
    }

    return create_render_target(error_detail);
  }

  bool create_render_target(QString* error_detail) {
    render_target_.Reset();
    ComPtr<ID3D11Texture2D> back_buffer;
    const HRESULT buffer_hr = swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back_buffer.GetAddressOf()));
    if (FAILED(buffer_hr) || !back_buffer) {
      if (error_detail != nullptr) {
        *error_detail = QString("GetBuffer failed: 0x%1").arg(static_cast<qulonglong>(buffer_hr), 0, 16);
      }
      return false;
    }
    const HRESULT target_hr = device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &render_target_);
    if (FAILED(target_hr) || !render_target_) {
      if (error_detail != nullptr) {
        *error_detail = QString("CreateRenderTargetView failed: 0x%1").arg(static_cast<qulonglong>(target_hr), 0, 16);
      }
      return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    back_buffer->GetDesc(&description);
    swap_chain_phys_size_ = QSize(static_cast<int>(description.Width), static_cast<int>(description.Height));
    return true;
  }

  bool ensure_shaders(QString* error_detail) {
    if (vertex_shader_ && pixel_shader_ && input_layout_ && sampler_ && vertex_buffer_) {
      return true;
    }

    constexpr char kVertexShader[] =
        "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };"
        "struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };"
        "PSIn main(VSIn input) { PSIn output; output.pos = float4(input.pos, 0.0, 1.0); output.uv = input.uv; return output; }";
    constexpr char kPixelShader[] =
        "Texture2D frameTex : register(t0);"
        "SamplerState frameSampler : register(s0);"
        "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET { return frameTex.Sample(frameSampler, uv); }";

    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    if (!compile_shader(kVertexShader, "vs_4_0", vs_blob.GetAddressOf(), error_detail)
        || !compile_shader(kPixelShader, "ps_4_0", ps_blob.GetAddressOf(), error_detail)) {
      return false;
    }

    HRESULT hr = device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vertex_shader_);
    if (FAILED(hr)) {
      if (error_detail != nullptr) {
        *error_detail = QString("CreateVertexShader failed: 0x%1").arg(static_cast<qulonglong>(hr), 0, 16);
      }
      return false;
    }
    hr = device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &pixel_shader_);
    if (FAILED(hr)) {
      if (error_detail != nullptr) {
        *error_detail = QString("CreatePixelShader failed: 0x%1").arg(static_cast<qulonglong>(hr), 0, 16);
      }
      return false;
    }

    constexpr D3D11_INPUT_ELEMENT_DESC kLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof(float) * 2, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device_->CreateInputLayout(
        kLayout,
        static_cast<UINT>(sizeof(kLayout) / sizeof(kLayout[0])),
        vs_blob->GetBufferPointer(),
        vs_blob->GetBufferSize(),
        &input_layout_);
    if (FAILED(hr)) {
      if (error_detail != nullptr) {
        *error_detail = QString("CreateInputLayout failed: 0x%1").arg(static_cast<qulonglong>(hr), 0, 16);
      }
      return false;
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sampler_desc, &sampler_);
    if (FAILED(hr)) {
      if (error_detail != nullptr) {
        *error_detail = QString("CreateSamplerState failed: 0x%1").arg(static_cast<qulonglong>(hr), 0, 16);
      }
      return false;
    }

    D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.ByteWidth = sizeof(Vertex) * 6;
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&buffer_desc, nullptr, &vertex_buffer_);
    if (FAILED(hr)) {
      if (error_detail != nullptr) {
        *error_detail = QString("CreateBuffer failed: 0x%1").arg(static_cast<qulonglong>(hr), 0, 16);
      }
      return false;
    }

    return true;
  }

  bool compile_shader(const char* source, const char* profile, ID3DBlob** blob, QString* error_detail) {
    D3DCompileProc d3d_compile = load_d3d_compile();
    if (d3d_compile == nullptr) {
      if (error_detail != nullptr) {
        *error_detail = "D3DCompile is unavailable; d3dcompiler_47.dll was not found.";
      }
      return false;
    }

    ComPtr<ID3DBlob> errors;
    const HRESULT hr = d3d_compile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        "main",
        profile,
        0,
        0,
        blob,
        &errors);
    if (FAILED(hr)) {
      if (error_detail != nullptr) {
        const QString compile_error = errors
            ? QString::fromLocal8Bit(static_cast<const char*>(errors->GetBufferPointer()), static_cast<int>(errors->GetBufferSize()))
            : QString();
        *error_detail = QString("D3DCompile failed: 0x%1 %2").arg(static_cast<qulonglong>(hr), 0, 16).arg(compile_error);
      }
      return false;
    }
    return true;
  }

  bool ensure_video_processor(quint32 width, quint32 height, QString* error_detail) {
    // Geometry transactions defer ResizeBuffers. Until commit, new frames must
    // target the existing buffer; the QWidget may already have its next size.
    const QSize output_size = swap_chain_phys_size_;
    if (width == 0 || height == 0 || output_size.width() <= 0 || output_size.height() <= 0) {
      if (error_detail != nullptr) {
        *error_detail = "D3D11 video processor received invalid dimensions.";
      }
      return false;
    }
    if (video_processor_ && video_source_width_ == width && video_source_height_ == height
        && video_output_size_ == output_size) {
      return true;
    }

    video_processor_.Reset();
    video_processor_enumerator_.Reset();
    video_output_views_.clear();
    video_input_views_.clear();
    video_input_texture_.Reset();

    if (!video_device_ && (FAILED(device_.As(&video_device_)) || !video_device_)) {
      if (error_detail != nullptr) {
        *error_detail = "D3D11 video device is unavailable.";
      }
      return false;
    }
    if (!video_context_ && (FAILED(context_.As(&video_context_)) || !video_context_)) {
      if (error_detail != nullptr) {
        *error_detail = "D3D11 video context is unavailable.";
      }
      return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc{};
    content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content_desc.InputFrameRate.Numerator = 30;
    content_desc.InputFrameRate.Denominator = 1;
    content_desc.InputWidth = width;
    content_desc.InputHeight = height;
    content_desc.OutputFrameRate.Numerator = 30;
    content_desc.OutputFrameRate.Denominator = 1;
    content_desc.OutputWidth = static_cast<UINT>(output_size.width());
    content_desc.OutputHeight = static_cast<UINT>(output_size.height());
    content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = video_device_->CreateVideoProcessorEnumerator(
        &content_desc, &video_processor_enumerator_);
    if (FAILED(hr) || !video_processor_enumerator_) {
      if (error_detail != nullptr) {
        *error_detail = QString("CreateVideoProcessorEnumerator failed: 0x%1")
                            .arg(static_cast<qulonglong>(hr), 0, 16);
      }
      return false;
    }
    hr = video_device_->CreateVideoProcessor(video_processor_enumerator_.Get(), 0, &video_processor_);
    if (FAILED(hr) || !video_processor_) {
      if (error_detail != nullptr) {
        *error_detail = QString("CreateVideoProcessor failed: 0x%1")
                            .arg(static_cast<qulonglong>(hr), 0, 16);
      }
      return false;
    }

    D3D11_VIDEO_COLOR background{};
    background.RGBA.A = 1.0F;
    video_context_->VideoProcessorSetOutputBackgroundColor(video_processor_.Get(), FALSE, &background);
    video_context_->VideoProcessorSetStreamFrameFormat(
        video_processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context_->VideoProcessorSetStreamAutoProcessingMode(video_processor_.Get(), 0, FALSE);

    video_source_width_ = width;
    video_source_height_ = height;
    video_output_size_ = output_size;
    return true;
  }

  ID3D11VideoProcessorOutputView* video_output_view(QString* error_detail) {
    UINT buffer_index = 0;
    UINT buffer_count = 1;
    if (!swap_chain3_) {
      swap_chain_.As(&swap_chain3_);
    }
    if (swap_chain3_) {
      buffer_index = swap_chain3_->GetCurrentBackBufferIndex();
      DXGI_SWAP_CHAIN_DESC swap_desc{};
      if (SUCCEEDED(swap_chain_->GetDesc(&swap_desc))) {
        buffer_count = (std::max)(1U, swap_desc.BufferCount);
      }
    }
    if (video_output_views_.size() != buffer_count) {
      video_output_views_.clear();
      video_output_views_.resize(buffer_count);
    }
    if (buffer_index >= video_output_views_.size()) {
      if (error_detail != nullptr) {
        *error_detail = "Current DXGI back-buffer index is out of range.";
      }
      return nullptr;
    }

    auto& output_view = video_output_views_[buffer_index];
    if (output_view) {
      return output_view.Get();
    }
    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = swap_chain_->GetBuffer(
        buffer_index,
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(back_buffer.GetAddressOf()));
    if (FAILED(hr) || !back_buffer) {
      if (error_detail != nullptr) {
        *error_detail = QString("GetBuffer for video output failed: 0x%1")
                            .arg(static_cast<qulonglong>(hr), 0, 16);
      }
      return nullptr;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_desc.Texture2D.MipSlice = 0;
    hr = video_device_->CreateVideoProcessorOutputView(
        back_buffer.Get(), video_processor_enumerator_.Get(), &output_desc, &output_view);
    if (FAILED(hr) || !output_view) {
      if (error_detail != nullptr) {
        *error_detail = QString("CreateVideoProcessorOutputView failed: 0x%1")
                            .arg(static_cast<qulonglong>(hr), 0, 16);
      }
      return nullptr;
    }
    return output_view.Get();
  }

  ID3D11VideoProcessorInputView* video_input_view(
      const redclaw::render::D3D11DecodedSurface& surface,
      QString* error_detail) {
    if (video_input_texture_.Get() != surface.texture) {
      video_input_views_.clear();
      video_input_texture_ = surface.texture;
      D3D11_TEXTURE2D_DESC texture_desc{};
      surface.texture->GetDesc(&texture_desc);
      video_input_views_.resize(texture_desc.ArraySize);
    }
    if (surface.array_slice >= video_input_views_.size()) {
      if (error_detail != nullptr) {
        *error_detail = "D3D11 decoded surface array slice is out of range.";
      }
      return nullptr;
    }

    auto& input_view = video_input_views_[surface.array_slice];
    if (!input_view) {
      D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
      input_desc.FourCC = 0;
      input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
      input_desc.Texture2D.MipSlice = 0;
      input_desc.Texture2D.ArraySlice = surface.array_slice;
      const HRESULT hr = video_device_->CreateVideoProcessorInputView(
          surface.texture, video_processor_enumerator_.Get(), &input_desc, &input_view);
      if (FAILED(hr) || !input_view) {
        if (error_detail != nullptr) {
          *error_detail = QString("CreateVideoProcessorInputView failed: 0x%1")
                              .arg(static_cast<qulonglong>(hr), 0, 16);
        }
        return nullptr;
      }
    }
    return input_view.Get();
  }

  PresentOutcome present_d3d11_surface(const DirectFrameData& frame, QString* error_detail) {
    GuiLatencyScope preparation(GuiStage::kPresentPrepare);
    const auto& surface = frame.d3d11_surface;
    if (!surface || surface->device != device_.Get() || surface->texture == nullptr) {
      if (error_detail != nullptr) {
        *error_detail = "D3D11 decoded surface belongs to a different or invalid device.";
      }
      return PresentOutcome::kFailed;
    }
    if (!ensure_video_processor(frame.width, frame.height, error_detail)) {
      return PresentOutcome::kFailed;
    }
    ID3D11VideoProcessorOutputView* output_view = video_output_view(error_detail);
    if (output_view == nullptr) {
      return PresentOutcome::kFailed;
    }
    ID3D11VideoProcessorInputView* input_view = video_input_view(*surface, error_detail);
    if (input_view == nullptr) {
      return PresentOutcome::kFailed;
    }

    const LONG source_x = static_cast<LONG>(frame.content_rect_x);
    const LONG source_y = static_cast<LONG>(frame.content_rect_y);
    const LONG source_width = static_cast<LONG>(frame.content_rect_width == 0
        ? frame.width : frame.content_rect_width);
    const LONG source_height = static_cast<LONG>(frame.content_rect_height == 0
        ? frame.height : frame.content_rect_height);
    const RECT source_rect{
        source_x, source_y, source_x + source_width, source_y + source_height};
    const int output_width = std::max(1, video_output_size_.width());
    const int output_height = std::max(1, video_output_size_.height());
    const double scale = std::min(
        static_cast<double>(output_width) / source_width,
        static_cast<double>(output_height) / source_height);
    const LONG destination_width = static_cast<LONG>(source_width * scale);
    const LONG destination_height = static_cast<LONG>(source_height * scale);
    const LONG destination_x = (output_width - destination_width) / 2;
    const LONG destination_y = (output_height - destination_height) / 2;
    const RECT destination_rect{
        destination_x,
        destination_y,
        destination_x + destination_width,
        destination_y + destination_height};
    const RECT output_rect{0, 0, output_width, output_height};

    video_context_->VideoProcessorSetOutputTargetRect(video_processor_.Get(), TRUE, &output_rect);
    video_context_->VideoProcessorSetStreamSourceRect(video_processor_.Get(), 0, TRUE, &source_rect);
    video_context_->VideoProcessorSetStreamDestRect(video_processor_.Get(), 0, TRUE, &destination_rect);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view;
    preparation.finish();
    GuiLatencyScope blit_timing(GuiStage::kVideoBlt);
    const HRESULT blit_hr = video_context_->VideoProcessorBlt(
        video_processor_.Get(), output_view, 0, 1, &stream);
    blit_timing.finish();
    if (FAILED(blit_hr)) {
      D3D11_TEXTURE2D_DESC input_desc{};
      surface->texture->GetDesc(&input_desc);
      UINT input_support = 0, output_support = 0;
      const HRESULT input_check = video_processor_enumerator_->CheckVideoProcessorFormat(input_desc.Format, &input_support);
      const HRESULT output_check = video_processor_enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &output_support);
      QString detail = QString("VideoProcessorBlt failed: 0x%1 visible=%2x%3 texture=%4x%5 format=%6 array=%7 slice=%8 bind=%9 mips=%10 samples=%11 source=(%12,%13,%14,%15) output=%16x%17 dest=(%18,%19,%20,%21) input_check=%22/%23 output_check=%24/%25")
          .arg(quint32(blit_hr), 8, 16, QLatin1Char('0')).arg(frame.width).arg(frame.height)
          .arg(input_desc.Width).arg(input_desc.Height).arg(input_desc.Format).arg(input_desc.ArraySize)
          .arg(surface->array_slice).arg(input_desc.BindFlags).arg(input_desc.MipLevels).arg(input_desc.SampleDesc.Count)
          .arg(source_rect.left).arg(source_rect.top).arg(source_rect.right).arg(source_rect.bottom)
          .arg(output_width).arg(output_height).arg(destination_rect.left).arg(destination_rect.top)
          .arg(destination_rect.right).arg(destination_rect.bottom).arg(quint32(input_check)).arg(input_support)
          .arg(quint32(output_check)).arg(output_support);
      ComPtr<ID3D11InfoQueue> messages;
      if (SUCCEEDED(device_.As(&messages))) {
        const auto count = messages->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = count > 16 ? count - 16 : 0; i < count; ++i) {
          SIZE_T bytes = 0;
          if (FAILED(messages->GetMessage(i, nullptr, &bytes)) || bytes > 8192) continue;
          std::vector<unsigned char> storage(bytes);
          auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
          if (SUCCEEDED(messages->GetMessage(i, message, &bytes)))
            detail += QString("\nD3D11 id=%1 %2").arg(message->ID).arg(QString::fromLatin1(message->pDescription).left(512));
        }
      }
      if (auto* probe = gui_latency_probe()) {
        probe->present_failure(detail);
        probe->record(GuiStage::kPresentFailure, 0);
      }
      if (error_detail != nullptr) {
        *error_detail = detail;
      }
      return PresentOutcome::kFailed;
    }

    return present_swap_chain(DXGI_PRESENT_DO_NOT_WAIT, error_detail, false);
  }

  PresentOutcome present_swap_chain(UINT flags, QString* error_detail, bool redraw) {
    GuiLatencyScope present_timing(GuiStage::kPresent);
    const HRESULT present_hr = swap_chain_->Present(0, flags);
    const auto outcome = classify_dxgi_present_result(present_hr);
    if (outcome == PresentOutcome::kBusyDrop) {
      if (error_detail != nullptr) {
        error_detail->clear();
      }
      return PresentOutcome::kBusyDrop;
    }
    if (outcome == PresentOutcome::kFailed) {
      const auto detail = QString("Present failed: 0x%1").arg(quint32(present_hr), 8, 16, QLatin1Char('0'));
      if (auto* probe = gui_latency_probe()) {
        probe->present_failure(detail);
        probe->record(GuiStage::kPresentFailure, 0);
      }
      if (error_detail != nullptr) {
        *error_detail = detail;
      }
      return PresentOutcome::kFailed;
    }
    if (redraw) if (auto* probe = gui_latency_probe()) probe->record(GuiStage::kRedraw, 0);
    return PresentOutcome::kPresented;
  }

  bool ensure_frame_texture(quint32 width, quint32 height, QString* error_detail) {
    if (frame_texture_ && frame_width_ == width && frame_height_ == height) {
      return true;
    }

    frame_texture_.Reset();
    frame_srv_.Reset();
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->CreateTexture2D(&texture_desc, nullptr, &frame_texture_);
    if (FAILED(hr) || !frame_texture_) {
      if (error_detail != nullptr) {
        *error_detail = QString("CreateTexture2D failed: 0x%1").arg(static_cast<qulonglong>(hr), 0, 16);
      }
      return false;
    }
    hr = device_->CreateShaderResourceView(frame_texture_.Get(), nullptr, &frame_srv_);
    if (FAILED(hr) || !frame_srv_) {
      if (error_detail != nullptr) {
        *error_detail = QString("CreateShaderResourceView failed: 0x%1").arg(static_cast<qulonglong>(hr), 0, 16);
      }
      return false;
    }

    frame_width_ = width;
    frame_height_ = height;
    return true;
  }

  QSize current_phys_size() const {
    return QSize(qRound(width() * devicePixelRatioF()),
                 qRound(height() * devicePixelRatioF()));
  }

  void sync_swap_chain_size() {
    if (geometry_transaction_active_ || !swap_chain_ || !context_) {
      return;
    }

    const QSize phys = current_phys_size();
    if (phys.width() <= 0 || phys.height() <= 0 || phys == swap_chain_phys_size_) {
      return;
    }

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    render_target_.Reset();
    video_processor_.Reset();
    video_processor_enumerator_.Reset();
    video_output_views_.clear();
    video_input_views_.clear();
    video_input_texture_.Reset();
    video_output_size_ = {};

    const HRESULT resize_hr = swap_chain_->ResizeBuffers(
        0,
        static_cast<UINT>(phys.width()),
        static_cast<UINT>(phys.height()),
        DXGI_FORMAT_UNKNOWN,
        0);
    if (FAILED(resize_hr)) {
      return;
    }

    QString ignored;
    if (create_render_target(&ignored)) {
      swap_chain_phys_size_ = phys;
      ++swap_chain_resize_count_;
    }
  }

  void update_vertices() {
    if (!vertex_buffer_ || frame_width_ == 0 || frame_height_ == 0 || width() <= 0 || height() <= 0) {
      return;
    }

    // Use physical pixels so the letterbox/pillarbox geometry is correct on
    // high-DPI displays where logical and physical dimensions diverge.
    const int phys_w = qRound(width()  * devicePixelRatioF());
    const int phys_h = qRound(height() * devicePixelRatioF());
    const float widget_aspect = static_cast<float>(phys_w) / static_cast<float>(phys_h);
    const float frame_aspect  = static_cast<float>(frame_width_) / static_cast<float>(frame_height_);
    float x = 1.0F;
    float y = 1.0F;
    if (frame_aspect > widget_aspect) {
      y = widget_aspect / frame_aspect;
    } else {
      x = frame_aspect / widget_aspect;
    }

    const Vertex vertices[] = {
        {-x, -y, 0.0F, 1.0F},
        {-x,  y, 0.0F, 0.0F},
        { x, -y, 1.0F, 1.0F},
        { x, -y, 1.0F, 1.0F},
        {-x,  y, 0.0F, 0.0F},
        { x,  y, 1.0F, 0.0F},
    };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context_->Map(vertex_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      std::memcpy(mapped.pData, vertices, sizeof(vertices));
      context_->Unmap(vertex_buffer_.Get(), 0);
    }
  }

  PresentOutcome render(QString* error_detail = nullptr, bool redraw = false) {
    GuiLatencyScope preparation(GuiStage::kPresentPrepare);
    if (!context_ || !swap_chain_ || !render_target_ || !frame_srv_) {
      if (error_detail) *error_detail = "CPU-transfer presentation resources are unavailable";
      return PresentOutcome::kFailed;
    }

    update_vertices();
    const FLOAT clear_color[4] = {0.008F, 0.024F, 0.090F, 1.0F};
    context_->ClearRenderTargetView(render_target_.Get(), clear_color);

    // Use physical pixel dimensions so the viewport matches the actual DXGI
    // back-buffer size on high-DPI displays (e.g. 125 % / 150 % scaling).
    const int phys_w = qRound(width()  * devicePixelRatioF());
    const int phys_h = qRound(height() * devicePixelRatioF());
    D3D11_VIEWPORT viewport{};
    viewport.Width    = static_cast<FLOAT>(std::max(1, phys_w));
    viewport.Height   = static_cast<FLOAT>(std::max(1, phys_h));
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;
    context_->RSSetViewports(1, &viewport);
    context_->OMSetRenderTargets(1, render_target_.GetAddressOf(), nullptr);

    constexpr UINT stride = sizeof(Vertex);
    constexpr UINT offset = 0;
    context_->IASetInputLayout(input_layout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetVertexBuffers(0, 1, vertex_buffer_.GetAddressOf(), &stride, &offset);
    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->PSSetShaderResources(0, 1, frame_srv_.GetAddressOf());
    context_->Draw(6, 0);

    ID3D11ShaderResourceView* null_srv = nullptr;
    context_->PSSetShaderResources(0, 1, &null_srv);
    // Present(0, …): no vsync wait – frame delivery rate drives the refresh.
    preparation.finish();
    return present_swap_chain(0, error_detail, redraw);
  }

  bool frame_has_presented_ = false;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGISwapChain> swap_chain_;
  ComPtr<IDXGISwapChain3> swap_chain3_;
  ComPtr<ID3D11RenderTargetView> render_target_;
  ComPtr<ID3D11VertexShader> vertex_shader_;
  ComPtr<ID3D11PixelShader> pixel_shader_;
  ComPtr<ID3D11InputLayout> input_layout_;
  ComPtr<ID3D11SamplerState> sampler_;
  ComPtr<ID3D11Buffer> vertex_buffer_;
  ComPtr<ID3D11Texture2D> frame_texture_;
  ComPtr<ID3D11ShaderResourceView> frame_srv_;
  ComPtr<ID3D11VideoDevice> video_device_;
  ComPtr<ID3D11VideoContext> video_context_;
  ComPtr<ID3D11VideoProcessorEnumerator> video_processor_enumerator_;
  ComPtr<ID3D11VideoProcessor> video_processor_;
  std::vector<ComPtr<ID3D11VideoProcessorOutputView>> video_output_views_;
  ComPtr<ID3D11Texture2D> video_input_texture_;
  std::vector<ComPtr<ID3D11VideoProcessorInputView>> video_input_views_;
  quint32 frame_width_ = 0;
  quint32 frame_height_ = 0;
  quint32 video_source_width_ = 0;
  quint32 video_source_height_ = 0;
  QSize video_output_size_;
  bool latest_frame_is_surface_ = false;
  bool geometry_transaction_active_ = false;
  std::uint64_t swap_chain_resize_count_ = 0;
  QSize last_phys_size_;
  QSize swap_chain_phys_size_;
};

// Quick probe: create a D3D11 hardware device and immediately release it.
[[nodiscard]] static bool probe_d3d11_available() {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL level{};
  const HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
      nullptr, 0, D3D11_SDK_VERSION, &device, &level, &ctx);
  if (device != nullptr) { device->Release(); }
  if (ctx    != nullptr) { ctx->Release();    }
  return SUCCEEDED(hr);
}
}  // namespace
PlaybackWidgetResult try_create_d3d11_playback_renderer(QWidget* parent) {
    if (!probe_d3d11_available()) return {};
    auto* widget = new D3D11PlaybackWidget(parent);
    return {widget, widget};
}
}  // namespace redclaw::ui
#endif
