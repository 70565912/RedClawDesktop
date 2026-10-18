#include "redclaw/input/input_recovery.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace redclaw::input {
namespace {
constexpr std::uint64_t kSampleIntervalMs = 250;
constexpr std::uint64_t kSettleMs = 200;
constexpr std::uint64_t kMaxSampleAgeMs = 1000;
constexpr std::uint64_t kMaxGapIntervalMs = 2000;
constexpr std::uint64_t kFailureRetryCooldownMs = 30000;
constexpr unsigned kRequiredGaps = 3;
constexpr unsigned kMaxAttemptsWithoutProgress = 3;

bool near(DesktopPoint a, DesktopPoint b, std::int64_t tolerance) {
    return std::abs(static_cast<std::int64_t>(a.x) - b.x) <= tolerance &&
        std::abs(static_cast<std::int64_t>(a.y) - b.y) <= tolerance;
}
} // namespace

RecoveringInputInjectorBackend::RecoveringInputInjectorBackend(
    IInputInjectorBackend& backend, std::unique_ptr<IInputRecoveryPlatform> platform)
    : backend_(backend), platform_(std::move(platform)) {}

RecoveringInputInjectorBackend::~RecoveringInputInjectorBackend() {
    if (platform_) platform_->close_window();
}

void RecoveringInputInjectorBackend::reset_observation() {
    pending_.reset();
    consecutive_gaps_ = 0;
    gap_foreground_ = 0;
}

bool RecoveringInputInjectorBackend::inject(const InputEvent& event) {
    return inject_batch({event});
}

bool RecoveringInputInjectorBackend::inject_batch(const std::vector<InputEvent>& events) {
    if (events.empty()) return backend_.inject_batch(events);
    const bool movement_only = std::all_of(events.begin(), events.end(), [](const InputEvent& event) {
        return event.target == InputTarget::kUserDesktop && event.type == InputEventType::kMouseMove &&
            event.absolute_coordinates;
    });
    // A click, drag, chord or state-sync release invalidates the observation.
    // Recovery never uses those events as probes or replays them after activation.
    if (!movement_only) {
        if (pending_ || consecutive_gaps_) ++stats_.mixed_input_resets;
        reset_observation();
    }
    std::optional<Pending> candidate;
    if (eligible_ && platform_ && movement_only && !pending_ && now_ms_ >= next_sample_ms_) {
        next_sample_ms_ = now_ms_ + kSampleIntervalMs;
        const DesktopPoint target{events.back().x, events.back().y};
        const auto before = platform_->observe(target);
        if (before.valid && !before.held_input && before.foreground && before.target_window &&
            !near(before.cursor, target, 16)) {
            ++stats_.observations_started;
            candidate = Pending{target, before, now_ms_};
        } else { ++stats_.observation_gated; reset_observation(); }
    }
    const bool inserted = backend_.inject_batch(events);
    if (!inserted) reset_observation();
    else if (candidate) pending_ = candidate;
    return inserted;
}

void RecoveringInputInjectorBackend::poll(bool context_valid, std::uint64_t now_ms,
                                         std::uint64_t context_revision, bool input_released) {
    if (!platform_) return;
    const bool changed = context_revision_ != context_revision || now_ms < now_ms_;
    now_ms_ = now_ms;
    context_revision_ = context_revision;
    eligible_ = context_valid && input_released;
    if (!context_valid || changed) {
        if (pending_ || consecutive_gaps_) ++stats_.context_resets;
        reset_observation();
        platform_->close_window();
        if (window_started_ms_) {
            ++stats_.window_gate_closes;
            stats_.last_window_lifetime_ms = now_ms >= *window_started_ms_ ? now_ms - *window_started_ms_ : 0;
            window_started_ms_.reset();
        }
        next_sample_ms_ = now_ms;
        // Preserve the attempt budget/cooldown across authorization toggles.
        return;
    }
    const auto window_close = platform_->pump_window(now_ms);
    if (window_close != InputRecoveryWindowClose::kNone) {
        if (window_started_ms_) stats_.last_window_lifetime_ms = now_ms - *window_started_ms_;
        window_started_ms_.reset();
        switch (window_close) {
        case InputRecoveryWindowClose::kUser: ++stats_.window_user_closes; break;
        case InputRecoveryWindowClose::kForegroundLost: ++stats_.window_focus_losses; break;
        case InputRecoveryWindowClose::kDesktopUnavailable: ++stats_.window_gate_closes; break;
        case InputRecoveryWindowClose::kNone: break;
        }
    }
    if (!input_released) {
        if (pending_ || consecutive_gaps_) ++stats_.held_input_resets;
        reset_observation();
        next_sample_ms_ = now_ms;
        return;
    }
    if (!pending_ || now_ms - pending_->since_ms < kSettleMs) return;
    const auto sample = *pending_;
    pending_.reset();
    if (now_ms - sample.since_ms > kMaxSampleAgeMs) { ++stats_.stale_samples; reset_observation(); return; }
    const auto after = platform_->observe(sample.target);
    if (!after.valid || after.held_input) {
        ++stats_.observation_gated;
        reset_observation();
        return;
    }
    if (after.foreground != sample.before.foreground || after.target_window != sample.before.target_window) {
        ++stats_.changed_target_samples;
        reset_observation();
        return;
    }
    if (!near(after.cursor, sample.before.cursor, 2)) {
        // Cursor progress is only detector evidence, not application consumption.
        ++stats_.cursor_progress_samples;
        reset_observation();
        // A later real move can be coalesced past the sampled target. Progress
        // ends this failure episode; a fresh stall must not inherit its backoff.
        attempts_without_progress_ = 0;
        next_attempt_ms_ = 0;
        return;
    }
    // Real movement while our window owns foreground can establish progress,
    // but a stalled owned window must never cause recursive activation.
    if (after.recovery_foreground) { reset_observation(); return; }
    ++stats_.stationary_samples;
    consecutive_gaps_ = gap_foreground_ == after.foreground && now_ms - last_gap_ms_ <= kMaxGapIntervalMs
        ? consecutive_gaps_ + 1 : 1;
    gap_foreground_ = after.foreground;
    last_gap_ms_ = now_ms;
    if (consecutive_gaps_ < kRequiredGaps) return;
    if (now_ms < next_attempt_ms_) { ++stats_.cooldown_stalls; return; }
    if (attempts_without_progress_ >= kMaxAttemptsWithoutProgress) { ++stats_.budget_suppressed; return; }
    ++attempts_without_progress_;
    next_attempt_ms_ = now_ms + kFailureRetryCooldownMs;
    ++stats_.activation_attempts;
    if (platform_->activate_window(after.foreground, sample.target, now_ms)) {
        ++stats_.foreground_activations;
        window_started_ms_ = now_ms;
    }
    reset_observation();
}
} // namespace redclaw::input
