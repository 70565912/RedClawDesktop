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

// Where hardware decode of one codec runs relative to the presenting adapter.
// The display adapter is preferred whenever it can decode the codec: it
// avoids a cross-adapter copy and was measured faster on the Optimus laptop
// (Intel HD 530 HEVC 0.9 ms/frame against GTX 960M about 4 ms). Another
// hardware adapter is used only when the display adapter cannot decode.
struct DecodeAdapterAttempt {
    std::size_t index = 0;
    bool hardware_decode = false;
};

struct DecodeAdapterChoice {
    // Index into the candidate span; empty means no adapter can decode.
    std::optional<std::size_t> index;
    // The decode adapter differs from the presenting adapter, so decoded
    // frames must cross adapters.
    bool cross_adapter = false;
    // `display_adapter`, `display_adapter_no_<codec>`,
    // `present_adapter_not_enumerated` or `no_adapter_<codec>`.
    std::string reason;
    // Adapters that were actually probed, in order. The display adapter is
    // probed first and the others only when it fails.
    std::vector<DecodeAdapterAttempt> attempts;
};

// `probe` reports whether the adapter can hardware-decode the codec (profile
// present and a real device opens). Software adapters are never probed.
using DecodeAdapterProbe = std::function<bool(const GpuAdapterCandidate&)>;
[[nodiscard]] DecodeAdapterChoice choose_decode_adapter(
    std::span<const GpuAdapterCandidate> candidates,
    std::uint64_t present_adapter_luid,
    std::string_view codec_name,
    const DecodeAdapterProbe& probe);

// Coalesces keyframe requests from the playback receiver. The first request
// is sent immediately; requests inside the minimum interval become one
// pending request that poll() emits once the interval elapses, unless a
// keyframe satisfied it first.
class KeyframeRequestLimiter final {
public:
    explicit KeyframeRequestLimiter(std::uint64_t min_interval_ms = 500) noexcept
        : min_interval_ms_(min_interval_ms) {}

    // Returns true when the caller should send a request now.
    bool request(std::uint64_t now_ms) noexcept;
    // Returns true when a coalesced request is due now.
    bool poll(std::uint64_t now_ms) noexcept;
    void satisfied() noexcept { pending_ = false; }
    void reset() noexcept;

    [[nodiscard]] bool pending() const noexcept { return pending_; }
    [[nodiscard]] std::uint64_t sent() const noexcept { return sent_; }
    [[nodiscard]] std::uint64_t suppressed() const noexcept { return suppressed_; }

private:
    bool due(std::uint64_t now_ms) const noexcept;

    std::uint64_t min_interval_ms_;
    bool has_sent_ = false;
    bool pending_ = false;
    std::uint64_t last_sent_ms_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t suppressed_ = 0;
};

// Decides whether the presenter may touch D3D for a frame. Hidden windows
// and a powered-off display skip every D3D call; an occluded swap chain is
// probed with DXGI_PRESENT_TEST at a low rate. Decode is not affected.
class PresentationGate final {
public:
    enum class Decision {
        kPresent,
        kTestOcclusion,
        kSkip,
    };
    explicit PresentationGate(std::uint64_t occlusion_probe_interval_ms = 250) noexcept
        : occlusion_probe_interval_ms_(occlusion_probe_interval_ms) {}

    void set_hidden(bool hidden) noexcept { hidden_ = hidden; }
    void set_display_on(bool on) noexcept { display_on_ = on; }
    // Report the result of a real or test Present.
    void on_present_result(bool occluded, std::uint64_t now_ms) noexcept;
    Decision decide(std::uint64_t now_ms) noexcept;

    [[nodiscard]] bool hidden() const noexcept { return hidden_; }
    [[nodiscard]] bool display_on() const noexcept { return display_on_; }
    [[nodiscard]] bool occluded() const noexcept { return occluded_; }
    [[nodiscard]] std::uint64_t skipped() const noexcept { return skipped_; }
    [[nodiscard]] std::string_view state_name() const noexcept;

private:
    std::uint64_t occlusion_probe_interval_ms_;
    bool hidden_ = false;
    bool display_on_ = true;
    bool occluded_ = false;
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
