#pragma once

#if defined(_WIN32)

#include "redclaw/render/playback_flow_control.h"
#include "redclaw/render/render_module.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace redclaw::render {

// `target_monitor` is an HMONITOR; nullptr means the primary monitor.
[[nodiscard]] std::vector<GpuAdapterCandidate> enumerate_gpu_adapters(void* target_monitor);

struct SelectedGpuAdapter {
    std::optional<GpuAdapterCandidate> adapter;
    std::string_view reason;
};
[[nodiscard]] SelectedGpuAdapter select_gpu_adapter_for_monitor(void* target_monitor);

// Hardware D3D11 device with BGRA and video support on the adapter with
// `adapter_luid` (0 = system default), multithread protected. The caller owns
// the returned reference.
[[nodiscard]] ID3D11Device* create_d3d11_video_device(
    std::uint64_t adapter_luid, std::string* error_detail, bool debug_layer = false);
[[nodiscard]] std::uint64_t d3d11_device_adapter_luid(ID3D11Device* device);

// Hands decoded frames from the decode device to a presenter on another
// device through a small ring of keyed-mutex shared textures. publish() runs
// on the decode thread, copies the decoder's array slice into a free slot on
// the GPU (a GPU-local copy, not zero-copy), and lets the decoder's surface
// pool slice go immediately, so a stalled presenter cannot hold decoder
// surfaces or the decode device lock. A slot the presenter is reading is
// skipped; with every slot busy the frame is dropped for display only.
class D3D11SharedSurfaceBridge final {
public:
    static constexpr std::uint32_t kSlotCount = 3;
    enum class PublishStatus {
        kPublished,
        kAllSlotsBusy,
        kFailed,
    };

    explicit D3D11SharedSurfaceBridge(ID3D11Device* producer_device);
    ~D3D11SharedSurfaceBridge();
    D3D11SharedSurfaceBridge(const D3D11SharedSurfaceBridge&) = delete;
    D3D11SharedSurfaceBridge& operator=(const D3D11SharedSurfaceBridge&) = delete;

    PublishStatus publish(
        ID3D11Texture2D* source,
        std::uint32_t array_slice,
        D3D11DecodedSurface* output,
        std::string* error_detail);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::render

#endif
