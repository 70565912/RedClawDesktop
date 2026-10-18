#pragma once

#include "redclaw/input/input_module.h"

#include <memory>
#include <optional>

namespace redclaw::input {

// Machine-local observation only. Identities are never serialized or logged.
struct InputRecoveryObservation {
    bool valid = false;
    bool held_input = false;
    bool recovery_foreground = false;
    DesktopPoint cursor;
    std::uintptr_t foreground = 0;
    std::uintptr_t target_window = 0;
};

enum class InputRecoveryWindowClose {
    kNone, kUser, kForegroundLost, kDesktopUnavailable,
};

class IInputRecoveryPlatform {
public:
    virtual ~IInputRecoveryPlatform() = default;
    // Valid only on this thread's ordinary input desktop, with an unclipped,
    // visible target point. No desktop switch or permission change is allowed.
    virtual InputRecoveryObservation observe(DesktopPoint target) = 0;
    virtual bool activate_window(std::uintptr_t expected_foreground, DesktopPoint target,
                                 std::uint64_t now_ms) = 0;
    virtual InputRecoveryWindowClose pump_window(std::uint64_t now_ms) = 0;
    virtual void close_window() = 0;
};

[[nodiscard]] std::unique_ptr<IInputRecoveryPlatform> make_windows_input_recovery_platform();

struct InputRecoveryStats {
    std::uint64_t stationary_samples = 0;
    std::uint64_t cursor_progress_samples = 0;
    std::uint64_t activation_attempts = 0;
    std::uint64_t foreground_activations = 0;
    std::uint64_t observations_started = 0;
    std::uint64_t observation_gated = 0;
    std::uint64_t context_resets = 0;
    std::uint64_t mixed_input_resets = 0;
    std::uint64_t stale_samples = 0;
    std::uint64_t changed_target_samples = 0;
    std::uint64_t budget_suppressed = 0;
    std::uint64_t cooldown_stalls = 0;
    std::uint64_t held_input_resets = 0;
    std::uint64_t window_focus_losses = 0;
    std::uint64_t window_gate_closes = 0;
    std::uint64_t window_user_closes = 0;
    std::uint64_t last_window_lifetime_ms = 0;
};

// Owns recovery lifecycle on the injector/runtime thread. Calls the underlying
// backend exactly once; never synthesizes or replays application input.
class RecoveringInputInjectorBackend final : public IInputInjectorBackend {
public:
    explicit RecoveringInputInjectorBackend(IInputInjectorBackend& backend,
        std::unique_ptr<IInputRecoveryPlatform> platform = make_windows_input_recovery_platform());
    ~RecoveringInputInjectorBackend() override;

    bool inject(const InputEvent& event) override;
    bool inject_batch(const std::vector<InputEvent>& events) override;
    // Caller supplies a monotonic clock and rechecks all ordinary-input gates
    // after consuming stops, disconnects, geometry changes and lease expiry.
    // Held input blocks detection/activation, but must not destroy the window
    // between a real ButtonDown and its matching ButtonUp.
    void poll(bool context_valid, std::uint64_t now_ms, std::uint64_t context_revision,
              bool input_released = true);
    [[nodiscard]] const InputRecoveryStats& stats() const { return stats_; }

private:
    void reset_observation();
    struct Pending {
        DesktopPoint target;
        InputRecoveryObservation before;
        std::uint64_t since_ms = 0;
    };
    IInputInjectorBackend& backend_;
    std::unique_ptr<IInputRecoveryPlatform> platform_;
    std::optional<Pending> pending_;
    InputRecoveryStats stats_;
    bool eligible_ = false;
    std::uint64_t now_ms_ = 0, context_revision_ = 0, next_sample_ms_ = 0;
    std::uint64_t last_gap_ms_ = 0, next_attempt_ms_ = 0;
    std::uintptr_t gap_foreground_ = 0;
    unsigned consecutive_gaps_ = 0;
    unsigned attempts_without_progress_ = 0;
    std::optional<std::uint64_t> window_started_ms_;
};

} // namespace redclaw::input
