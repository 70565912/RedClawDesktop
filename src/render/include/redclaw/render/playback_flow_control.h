#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Platform-neutral policy for the Controller playback path. The Windows D3D11
// presenter, the decode receiver and the decode probe feed these with real
// observations; tests feed them with fakes.
namespace redclaw::render {

struct GpuAdapterCandidate {
    std::uint32_t enum_index = 0;
    std::uint64_t luid = 0;
    std::string name;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t output_count = 0;
    // The adapter drives the monitor that shows the playback window.
    bool owns_target_monitor = false;
    bool software = false;
};

struct GpuAdapterChoice {
    // Index into the candidate span; empty means "let D3D pick the default".
    std::optional<std::size_t> index;
    std::string_view reason;
};

// Prefers the adapter that owns the target monitor, then the first hardware
// adapter with outputs. An adapter without outputs (an Optimus dGPU) is never
// chosen implicitly: presenting from it needs a cross-adapter copy.
[[nodiscard]] GpuAdapterChoice choose_gpu_adapter(std::span<const GpuAdapterCandidate> candidates) noexcept;
[[nodiscard]] std::string describe_gpu_adapter(const GpuAdapterCandidate& adapter);

// Hardware decode of one codec on the adapter that presents the window.
// Only that adapter is probed. Another adapter, even one that can decode the
// codec, is not used: copying frames across adapters costs more than falling
// back to H.264.
struct DecodeAdapterAttempt {
    std::size_t index = 0;
    bool hardware_decode = false;
};

struct DecodeAdapterChoice {
    // Index into the candidate span; empty means the display adapter cannot
    // hardware-decode the codec.
    std::optional<std::size_t> index;
    // `display_adapter`, `display_adapter_no_<codec>` or
    // `present_adapter_not_enumerated`.
    std::string reason;
    // The display adapter, when it was probed. Empty when it is missing or
    // a software adapter.
    std::vector<DecodeAdapterAttempt> attempts;
};

// `probe` reports whether the adapter can hardware-decode the codec (profile
// present and a real device opens). It is called for the display adapter
// only, and never for a software adapter.
using DecodeAdapterProbe = std::function<bool(const GpuAdapterCandidate&)>;
[[nodiscard]] DecodeAdapterChoice choose_decode_adapter(
    std::span<const GpuAdapterCandidate> candidates,
    std::uint64_t present_adapter_luid,
    std::string_view codec_name,
    const DecodeAdapterProbe& probe);

// The presenter device is created once and outlives sessions. At session
// start it is recreated when the window's monitor is now driven by another
// adapter. Unknown LUIDs (0) never force a rebuild.
[[nodiscard]] bool presenter_adapter_changed(
    std::uint64_t device_adapter_luid, std::uint64_t window_monitor_adapter_luid) noexcept;

enum class DecoderHwBackend {
    kD3D11va,
    kQsv,
    kCuda,
    kDxva2,
};

// Hardware decode backends to try, in order. With a decode device pinned to
// the presenter's adapter only D3D11VA can use it; QSV, CUDA and DXVA2 would
// open FFmpeg's default device, which may sit on another GPU, so they are
// left out and decode falls back to software instead.
[[nodiscard]] std::vector<DecoderHwBackend> decoder_hw_backend_order(bool pinned_to_presenter_adapter);

// Coalesces keyframe requests from the playback receiver. The first request
// is sent immediately; requests inside the minimum interval become one
// pending request that poll() emits once the interval elapses, unless a
// keyframe satisfied it first.
class KeyframeRequestLimiter final {
public:
    explicit KeyframeRequestLimiter(std::uint64_t min_interval_ms = 500) noexcept
        : min_interval_ms_(min_interval_ms) {}

    // Return the reason to send when due. A pending resume notification
    // takes priority over a dependency request.
    std::optional<std::string> request(std::uint64_t now_ms, std::string reason = {});
    std::optional<std::string> poll(std::uint64_t now_ms) noexcept;
    void satisfied() noexcept;
    void cancel_pending() noexcept { pending_reason_.reset(); }
    void reset() noexcept;

    [[nodiscard]] bool pending() const noexcept { return pending_reason_.has_value(); }
    [[nodiscard]] std::uint64_t sent() const noexcept { return sent_; }
    [[nodiscard]] std::uint64_t suppressed() const noexcept { return suppressed_; }

private:
    bool due(std::uint64_t now_ms) const noexcept;

    std::uint64_t min_interval_ms_;
    bool has_sent_ = false;
    std::optional<std::string> pending_reason_;
    std::uint64_t last_sent_ms_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t suppressed_ = 0;
};

// Decode-worker reference state. Successful IDR submission and displayable
// output are separate events: buffered decoders must receive following P AUs.
class DecoderReferenceRecovery final {
public:
    void invalidate() noexcept { needs_idr_ = true; accepted_idr_ = false; }
    void clear() noexcept { needs_idr_ = false; accepted_idr_ = false; }
    [[nodiscard]] bool needs_idr() const noexcept { return needs_idr_; }
    [[nodiscard]] bool awaiting_output() const noexcept { return accepted_idr_; }
    bool prepare(std::uint32_t codec, std::uint32_t width, std::uint32_t height,
        std::uint64_t geometry, bool idr) noexcept {
        if (codec != codec_ || width != width_ || height != height_ || geometry != geometry_) invalidate();
        codec_ = codec; width_ = width; height_ = height; geometry_ = geometry;
        return !needs_idr_ || idr;
    }
    void submitted(bool idr) noexcept {
        if (idr) { needs_idr_ = false; accepted_idr_ = true; }
    }
    bool take_recovery_output() noexcept {
        const bool accepted = accepted_idr_;
        accepted_idr_ = false;
        return accepted;
    }
private:
    bool needs_idr_ = false, accepted_idr_ = false;
    std::uint32_t codec_ = 0, width_ = 0, height_ = 0;
    std::uint64_t geometry_ = 0;
};

// Decides whether the presenter may touch D3D for a frame. Hidden windows
// and a powered-off display skip every D3D call, as does explicit window
// coverage. DXGI occlusion is probed separately at a low rate.
class PresentationGate final {
public:
    enum class Decision {
        kPresent,
        kTestOcclusion,
        kSkip,
    };
    explicit PresentationGate(std::uint64_t occlusion_probe_interval_ms = 250) noexcept
        : occlusion_probe_interval_ms_(occlusion_probe_interval_ms) {}

    void set_hidden(bool hidden) noexcept;
    void set_display_on(bool on) noexcept;
    void set_window_occluded(bool occluded) noexcept;
    // Report the result of a real or test Present.
    void on_present_result(bool occluded, std::uint64_t now_ms) noexcept;
    Decision decide(std::uint64_t now_ms) noexcept;
    // True once after all pause reasons clear, even without frame delivery.
    bool take_resume() noexcept;

    [[nodiscard]] bool hidden() const noexcept { return hidden_; }
    [[nodiscard]] bool display_on() const noexcept { return display_on_; }
    [[nodiscard]] bool occluded() const noexcept { return window_occluded_ || swap_chain_occluded_; }
    [[nodiscard]] bool needs_occlusion_probe() const noexcept {
        return swap_chain_occluded_ && !window_occluded_ && !hidden_ && display_on_;
    }
    [[nodiscard]] bool paused() const noexcept { return hidden_ || !display_on_ || occluded(); }
    [[nodiscard]] std::uint64_t skipped() const noexcept { return skipped_; }
    [[nodiscard]] std::string_view state_name() const noexcept;

private:
    void note_state_change() noexcept;

    std::uint64_t occlusion_probe_interval_ms_;
    bool hidden_ = false;
    bool display_on_ = true;
    bool window_occluded_ = false;
    bool swap_chain_occluded_ = false;
    bool resume_pending_ = false;
    bool was_paused_ = false;
    std::uint64_t last_occlusion_probe_ms_ = 0;
    std::uint64_t skipped_ = 0;
};

// Rate-limited warning for individual D3D calls on the GUI thread.
class SlowCallReporter final {
public:
    struct Report {
        std::string_view call;
        std::uint64_t elapsed_us = 0;
        // Slow calls swallowed by the rate limit since the previous report.
        std::uint64_t suppressed = 0;
    };
    explicit SlowCallReporter(std::uint64_t threshold_us = 50000, std::uint64_t min_report_interval_ms = 1000) noexcept
        : threshold_us_(threshold_us), min_report_interval_ms_(min_report_interval_ms) {}

    // `call` must outlive the returned report (use string literals).
    std::optional<Report> observe(std::string_view call, std::uint64_t elapsed_us, std::uint64_t now_ms) noexcept;

    [[nodiscard]] std::uint64_t slow_calls() const noexcept { return slow_calls_; }
    [[nodiscard]] std::uint64_t max_elapsed_us() const noexcept { return max_elapsed_us_; }
    [[nodiscard]] std::uint64_t threshold_us() const noexcept { return threshold_us_; }

private:
    std::uint64_t threshold_us_;
    std::uint64_t min_report_interval_ms_;
    bool has_reported_ = false;
    std::uint64_t last_report_ms_ = 0;
    std::uint64_t suppressed_ = 0;
    std::uint64_t slow_calls_ = 0;
    std::uint64_t max_elapsed_us_ = 0;
};

}  // namespace redclaw::render
