#include "redclaw/render/playback_flow_control.h"

#include <cstdio>

namespace redclaw::render {

GpuAdapterChoice choose_gpu_adapter(std::span<const GpuAdapterCandidate> candidates) noexcept {
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!candidates[i].software && candidates[i].owns_target_monitor) {
            return {i, "window_monitor_adapter"};
        }
    }
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!candidates[i].software && candidates[i].output_count > 0) {
            return {i, "first_adapter_with_outputs"};
        }
    }
    return {std::nullopt, "system_default_no_adapter_with_outputs"};
}

std::string describe_gpu_adapter(const GpuAdapterCandidate& adapter) {
    char ids[96];
    std::snprintf(ids, sizeof(ids), " vendor=0x%04x device=0x%04x luid=0x%08x:%08x",
        static_cast<unsigned>(adapter.vendor_id), static_cast<unsigned>(adapter.device_id),
        static_cast<unsigned>(adapter.luid >> 32U), static_cast<unsigned>(adapter.luid & 0xffffffffU));
    std::string name;
    name.reserve(adapter.name.size());
    for (const char c : adapter.name) name.push_back(c == '"' || c == '\n' || c == '\r' ? '_' : c);
    return "name=\"" + name + "\"" + ids + " index=" + std::to_string(adapter.enum_index)
        + " outputs=" + std::to_string(adapter.output_count)
        + (adapter.owns_target_monitor ? " owns_window_monitor=1" : " owns_window_monitor=0");
}

bool KeyframeRequestLimiter::due(std::uint64_t now_ms) const noexcept {
    return !has_sent_ || now_ms < last_sent_ms_ || now_ms - last_sent_ms_ >= min_interval_ms_;
}

bool KeyframeRequestLimiter::request(std::uint64_t now_ms) noexcept {
    if (due(now_ms)) {
        has_sent_ = true;
        last_sent_ms_ = now_ms;
        pending_ = false;
        ++sent_;
        return true;
    }
    pending_ = true;
    ++suppressed_;
    return false;
}

bool KeyframeRequestLimiter::poll(std::uint64_t now_ms) noexcept {
    if (!pending_ || !due(now_ms)) return false;
    has_sent_ = true;
    last_sent_ms_ = now_ms;
    pending_ = false;
    ++sent_;
    return true;
}

void KeyframeRequestLimiter::reset() noexcept {
    has_sent_ = false;
    pending_ = false;
    last_sent_ms_ = 0;
}

void PresentationGate::on_present_result(bool occluded, std::uint64_t now_ms) noexcept {
    occluded_ = occluded;
    last_occlusion_probe_ms_ = now_ms;
}

PresentationGate::Decision PresentationGate::decide(std::uint64_t now_ms) noexcept {
    if (hidden_ || !display_on_) {
        ++skipped_;
        return Decision::kSkip;
    }
    if (!occluded_) return Decision::kPresent;
    if (now_ms < last_occlusion_probe_ms_ || now_ms - last_occlusion_probe_ms_ >= occlusion_probe_interval_ms_) {
        last_occlusion_probe_ms_ = now_ms;
        return Decision::kTestOcclusion;
    }
    ++skipped_;
    return Decision::kSkip;
}

std::string_view PresentationGate::state_name() const noexcept {
    if (!display_on_) return "display_off";
    if (hidden_) return "hidden";
    if (occluded_) return "occluded";
    return "visible";
}

std::optional<SlowCallReporter::Report> SlowCallReporter::observe(
    std::string_view call, std::uint64_t elapsed_us, std::uint64_t now_ms) noexcept {
    if (elapsed_us <= threshold_us_) return std::nullopt;
    ++slow_calls_;
    if (elapsed_us > max_elapsed_us_) max_elapsed_us_ = elapsed_us;
    if (has_reported_ && now_ms >= last_report_ms_ && now_ms - last_report_ms_ < min_report_interval_ms_) {
        ++suppressed_;
        return std::nullopt;
    }
    has_reported_ = true;
    last_report_ms_ = now_ms;
    Report report{call, elapsed_us, suppressed_};
    suppressed_ = 0;
    return report;
}

}  // namespace redclaw::render
