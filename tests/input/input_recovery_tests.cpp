#include <gtest/gtest.h>

#include <vector>

#include "redclaw/input/input_recovery.h"

namespace {
using namespace redclaw::input;

class FakePlatform final : public IInputRecoveryPlatform {
public:
    InputRecoveryObservation observation{true, false, false, {10, 10}, 11, 12};
    unsigned reads = 0, attempts = 0, closes = 0;
    std::vector<std::uint64_t> activation_times;
    bool activate = true, window = false;
    InputRecoveryWindowClose next_close = InputRecoveryWindowClose::kNone;
    InputRecoveryObservation observe(DesktopPoint) override { ++reads; return observation; }
    bool activate_window(std::uintptr_t, DesktopPoint, std::uint64_t now_ms) override {
        activation_times.push_back(now_ms);
        ++attempts; window = activate; return activate;
    }
    InputRecoveryWindowClose pump_window(std::uint64_t) override {
        const auto result = next_close;
        next_close = InputRecoveryWindowClose::kNone;
        if (result != InputRecoveryWindowClose::kNone) window = false;
        return result;
    }
    void close_window() override { ++closes; window = false; }
};

InputEvent move_to(int x = 500) {
    InputEvent event;
    event.type = InputEventType::kMouseMove;
    event.absolute_coordinates = true;
    event.x = x; event.y = 200;
    return event;
}

class InputRecoveryTests : public testing::Test {
protected:
    InMemoryInputInjectorBackend native;
    FakePlatform* platform = nullptr;
    std::unique_ptr<RecoveringInputInjectorBackend> recovery;
    void SetUp() override {
        auto fake = std::make_unique<FakePlatform>(); platform = fake.get();
        recovery = std::make_unique<RecoveringInputInjectorBackend>(native, std::move(fake));
        recovery->poll(true, 1000, 1);
    }
    void stationary(std::uint64_t start) {
        recovery->poll(true, start, 1);
        ASSERT_TRUE(recovery->inject(move_to()));
        recovery->poll(true, start + 200, 1);
    }
};

TEST_F(InputRecoveryTests, ThreeDelayedGapsActivateOnceWithoutExtraOrReplayedInput) {
    stationary(1000); stationary(1300);
    EXPECT_EQ(platform->attempts, 0U);
    stationary(1600);
    EXPECT_EQ(recovery->stats().stationary_samples, 3U);
    EXPECT_EQ(platform->attempts, 1U);
    EXPECT_EQ(recovery->stats().foreground_activations, 1U);
    EXPECT_EQ(native.injected_events().size(), 3U);
    stationary(34000); stationary(34300); stationary(34600);
    EXPECT_EQ(platform->attempts, 2U); // Fresh real movement can retry after the cooldown.
}

TEST_F(InputRecoveryTests, AsynchronousProgressDoesNotTriggerAndSamplingIsBounded) {
    ASSERT_TRUE(recovery->inject(move_to()));
    for (unsigned i = 0; i < 100; ++i) ASSERT_TRUE(recovery->inject(move_to(600)));
    EXPECT_EQ(platform->reads, 1U);
    recovery->poll(true, 1199, 1);
    EXPECT_EQ(platform->reads, 1U);
    platform->observation.cursor = {600, 200}; // Coalesced later move, not first target.
    recovery->poll(true, 1200, 1);
    EXPECT_EQ(recovery->stats().stationary_samples, 0U);
    EXPECT_EQ(platform->attempts, 0U);
    EXPECT_EQ(native.injected_events().size(), 101U);
}

TEST_F(InputRecoveryTests, CoalescedProgressRearmsSameForegroundWithoutOldBackoff) {
    stationary(1000); stationary(1300); stationary(1600);
    ASSERT_EQ(platform->attempts, 1U);
    recovery->poll(true, 2000, 1);
    ASSERT_TRUE(recovery->inject(move_to(500)));
    ASSERT_TRUE(recovery->inject(move_to(700)));
    platform->observation.cursor = {700, 200};
    recovery->poll(true, 2200, 1);
    EXPECT_EQ(recovery->stats().cursor_progress_samples, 1U);
    stationary(2300); stationary(2550);
    EXPECT_EQ(platform->attempts, 1U);
    stationary(2800); // New episode qualifies after 700 ms, before old backoff expires.
    EXPECT_EQ(platform->attempts, 2U);
    EXPECT_EQ(recovery->stats().cooldown_stalls, 0U);
    EXPECT_EQ(native.injected_events().size(), 8U);
    EXPECT_EQ(platform->activation_times, (std::vector<std::uint64_t>{1800, 3000}));
}

TEST_F(InputRecoveryTests, ProgressWhileRecoveryWindowOwnsForegroundRearmsLaterStall) {
    stationary(1000); stationary(1300); stationary(1600);
    ASSERT_EQ(platform->attempts, 1U);
    platform->observation.foreground = 31;
    platform->observation.recovery_foreground = true;
    recovery->poll(true, 2000, 1);
    ASSERT_TRUE(recovery->inject(move_to()));
    platform->observation.cursor = {500, 200};
    recovery->poll(true, 2200, 1);
    EXPECT_EQ(recovery->stats().cursor_progress_samples, 1U);
    platform->observation.foreground = 11;
    platform->observation.recovery_foreground = false;
    platform->observation.cursor = {10, 10};
    platform->next_close = InputRecoveryWindowClose::kForegroundLost;
    stationary(2300); stationary(2550);
    EXPECT_EQ(platform->attempts, 1U); // Every new episode still needs three gaps.
    stationary(2800);
    ASSERT_EQ(platform->attempts, 2U);

    platform->observation.foreground = 41;
    platform->observation.recovery_foreground = true;
    recovery->poll(true, 3200, 1);
    ASSERT_TRUE(recovery->inject(move_to(700)));
    platform->observation.cursor = {700, 200};
    recovery->poll(true, 3400, 1);
    platform->observation.foreground = 11;
    platform->observation.recovery_foreground = false;
    platform->observation.cursor = {10, 10};
    platform->next_close = InputRecoveryWindowClose::kForegroundLost;
    stationary(3500); stationary(3750);
    EXPECT_EQ(platform->attempts, 2U);
    stationary(4000);
    EXPECT_EQ(platform->attempts, 3U);
    EXPECT_EQ(recovery->stats().cursor_progress_samples, 2U);
    EXPECT_EQ(recovery->stats().cooldown_stalls, 0U);
    EXPECT_EQ(native.injected_events().size(), 11U);
    EXPECT_EQ(platform->activation_times, (std::vector<std::uint64_t>{1800, 3000, 4200}));
}

TEST_F(InputRecoveryTests, WindowClosureAndContextChangesDoNotClearFailureBackoff) {
    stationary(1000); stationary(1300); stationary(1600);
    ASSERT_EQ(platform->attempts, 1U);
    platform->next_close = InputRecoveryWindowClose::kForegroundLost;
    recovery->poll(true, 1900, 1);
    stationary(2000); stationary(2250); stationary(2500);
    EXPECT_EQ(platform->attempts, 1U);
    EXPECT_EQ(recovery->stats().cooldown_stalls, 1U);

    recovery->poll(false, 2800, 2);
    recovery->poll(true, 2900, 2);
    for (const std::uint64_t start : {3000U, 3250U, 3500U}) {
        recovery->poll(true, start, 2);
        ASSERT_TRUE(recovery->inject(move_to()));
        recovery->poll(true, start + 200, 2);
    }
    EXPECT_EQ(platform->attempts, 1U);
    EXPECT_EQ(recovery->stats().cooldown_stalls, 2U);
    EXPECT_EQ(recovery->stats().cursor_progress_samples, 0U);

    stationary(32000); stationary(32250); stationary(32500);
    EXPECT_EQ(platform->attempts, 2U);
    EXPECT_EQ(native.injected_events().size(), 12U);
}

TEST_F(InputRecoveryTests, StationaryRecoveryWindowNeverTriggersAnotherActivation) {
    platform->observation.recovery_foreground = true;
    stationary(1000); stationary(1300); stationary(1600);
    EXPECT_EQ(platform->attempts, 0U);
}

TEST_F(InputRecoveryTests, UserClosingWindowDoesNotResetTheBoundedRetryBudget) {
    stationary(1000); stationary(1300); stationary(1600);
    platform->next_close = InputRecoveryWindowClose::kUser;
    recovery->poll(true, 11800, 1);
    EXPECT_EQ(recovery->stats().window_user_closes, 1U);
    EXPECT_EQ(recovery->stats().last_window_lifetime_ms, 10000U);
    EXPECT_EQ(recovery->stats().cursor_progress_samples, 0U);
    stationary(33000); stationary(33300); stationary(33600);
    EXPECT_EQ(platform->attempts, 2U);
    stationary(65000); stationary(65300); stationary(65600);
    EXPECT_EQ(platform->attempts, 3U);
    stationary(97000); stationary(97300); stationary(97600);
    EXPECT_EQ(platform->attempts, 3U);
    EXPECT_EQ(recovery->stats().budget_suppressed, 1U);
}

TEST_F(InputRecoveryTests, RealClickDoesNotDestroyWindowBeforeButtonUpButStopDoes) {
    InputPolicyGate gate;
    RemoteInputSession session(gate, *recovery);
    session.set_authorized(true); ASSERT_TRUE(session.request_active(1000));
    auto poll_session = [&](std::uint64_t now) {
        recovery->poll(session.ordinary_input_recovery_context_valid(), now,
            session.eligibility_revision(), session.ordinary_input_recovery_eligible());
    };
    poll_session(1000);
    for (unsigned i = 0; i < 3; ++i) {
        const auto now = 1000 + i * 300;
        poll_session(now);
        ASSERT_TRUE(session.enqueue_batch(i + 1, {move_to()}, now));
        ASSERT_TRUE(session.drain(now));
        poll_session(now + 200);
    }
    ASSERT_TRUE(platform->window);
    const auto closes = platform->closes;
    InputEvent click; click.type = InputEventType::kMouseButtonDown; click.mouse_button = MouseButton::kLeft;
    ASSERT_TRUE(session.enqueue_batch(4, {click}, 1900)); ASSERT_TRUE(session.drain(1900));
    ASSERT_FALSE(session.ordinary_input_recovery_eligible());
    ASSERT_TRUE(session.ordinary_input_recovery_context_valid());
    poll_session(1900);
    EXPECT_TRUE(platform->window);
    EXPECT_EQ(platform->closes, closes);
    click.type = InputEventType::kMouseButtonUp;
    ASSERT_TRUE(session.enqueue_batch(5, {click}, 2000)); ASSERT_TRUE(session.drain(2000));
    poll_session(2000);
    EXPECT_TRUE(platform->window);
    session.pause(RemoteInputPauseReason::kLocalPause); poll_session(2100);
    EXPECT_FALSE(platform->window);
    EXPECT_EQ(recovery->stats().window_gate_closes, 1U);
    EXPECT_EQ(native.injected_events().size(), 5U);
}

TEST_F(InputRecoveryTests, HeldInputCancelsDetectionWithoutStartingAWindow) {
    stationary(1000); stationary(1300);
    recovery->poll(true, 1600, 1, false);
    ASSERT_TRUE(recovery->inject(move_to()));
    recovery->poll(true, 1800, 1, false);
    EXPECT_EQ(platform->attempts, 0U);
    stationary(1900); stationary(2200);
    EXPECT_EQ(platform->attempts, 0U);
    stationary(2500);
    EXPECT_EQ(platform->attempts, 1U);
}

TEST_F(InputRecoveryTests, NoRecoveryFromApiFailureRelativeMotionOrKeyboard) {
    native.set_fail_mode(true);
    EXPECT_FALSE(recovery->inject(move_to()));
    recovery->poll(true, 1200, 1);
    native.set_fail_mode(false);
    auto relative = move_to(); relative.absolute_coordinates = false;
    InputEvent key; key.type = InputEventType::kKeyDown; key.scan_code = 8;
    ASSERT_TRUE(recovery->inject_batch({relative, key}));
    recovery->poll(true, 2000, 1);
    EXPECT_EQ(platform->attempts, 0U);
    EXPECT_EQ(recovery->stats().stationary_samples, 0U);
}

TEST_F(InputRecoveryTests, InvalidDesktopClippingHeldInputAndOwnForegroundFailClosed) {
    for (unsigned gate = 0; gate < 4; ++gate) {
        platform->observation = {true, false, false, {10, 10}, 11, 12};
        if (gate == 0) platform->observation.valid = false;
        if (gate == 1) platform->observation.held_input = true;
        if (gate == 2) platform->observation.recovery_foreground = true;
        if (gate == 3) platform->observation.target_window = 0;
        for (unsigned i = 0; i < 3; ++i) stationary(1000 + gate * 2000 + i * 300);
    }
    EXPECT_EQ(platform->attempts, 0U);
    EXPECT_EQ(recovery->stats().stationary_samples, 0U);
}

TEST_F(InputRecoveryTests, ChangedContextOrForegroundAndOldSamplesCannotActivate) {
    stationary(1000); stationary(1300);
    recovery->poll(true, 1600, 1); ASSERT_TRUE(recovery->inject(move_to()));
    platform->observation.foreground = 21;
    recovery->poll(true, 1800, 1);
    EXPECT_EQ(platform->attempts, 0U);
    stationary(1900); stationary(2200);
    recovery->poll(true, 2500, 2); // Geometry/control epoch invalidates observations.
    ASSERT_TRUE(recovery->inject(move_to()));
    recovery->poll(true, 2700, 2);
    EXPECT_EQ(platform->attempts, 0U);
    recovery->poll(true, 3000, 2); ASSERT_TRUE(recovery->inject(move_to()));
    recovery->poll(true, 5000, 2); // Runtime stall must not act on an old input.
    EXPECT_EQ(platform->attempts, 0U);
}

TEST_F(InputRecoveryTests, TargetChangeAndInterveningClickDiscardTheStreak) {
    stationary(1000); stationary(1300);
    recovery->poll(true, 1600, 1); ASSERT_TRUE(recovery->inject(move_to()));
    platform->observation.target_window = 22;
    recovery->poll(true, 1800, 1);
    stationary(1900); stationary(2200);
    InputEvent click; click.type = InputEventType::kMouseButtonDown; click.mouse_button = MouseButton::kLeft;
    ASSERT_TRUE(recovery->inject(click));
    stationary(2500);
    EXPECT_EQ(platform->attempts, 0U);
}

TEST_F(InputRecoveryTests, LostEligibilityCancelsPendingAndClosesOwnedWindow) {
    stationary(1000); stationary(1300);
    recovery->poll(true, 1600, 1); ASSERT_TRUE(recovery->inject(move_to()));
    recovery->poll(false, 1800, 1);
    EXPECT_EQ(platform->attempts, 0U);
    recovery->poll(true, 2000, 1);
    stationary(2000); stationary(2300); stationary(2600);
    ASSERT_TRUE(platform->window);
    recovery->poll(false, 2850, 1);
    EXPECT_FALSE(platform->window);
}

TEST_F(InputRecoveryTests, ForegroundDenialIsBoundedAndDistinctFromSuccess) {
    platform->activate = false;
    stationary(1000); stationary(1300); stationary(1600);
    EXPECT_EQ(recovery->stats().activation_attempts, 1U);
    EXPECT_EQ(recovery->stats().foreground_activations, 0U);
    platform->observation.foreground = 21;
    stationary(2000); stationary(2300); stationary(2600);
    EXPECT_EQ(platform->attempts, 1U); // No rapid loop on another foreground.
    stationary(33000); stationary(33300); stationary(33600);
    EXPECT_EQ(platform->attempts, 2U);
}

TEST_F(InputRecoveryTests, SessionHoldsTransferPauseAndLeaseSuppressRecoveryEligibility) {
    InputPolicyGate gate; gate.set_permission_policy(InputPermissionPolicy::kStandardDesktopOnly);
    RemoteInputSession session(gate, *recovery);
    EXPECT_FALSE(session.ordinary_input_recovery_eligible());
    session.set_authorized(true); ASSERT_TRUE(session.request_active(1000));
    ASSERT_TRUE(session.ordinary_input_recovery_eligible());
    InputEvent key; key.type = InputEventType::kKeyDown; key.scan_code = 8;
    ASSERT_TRUE(session.enqueue_batch(1, {key}, 1100)); ASSERT_TRUE(session.drain(1100));
    EXPECT_FALSE(session.ordinary_input_recovery_eligible());
    session.release_all(); EXPECT_TRUE(session.ordinary_input_recovery_eligible());
    key.type = InputEventType::kMouseButtonDown; key.mouse_button = MouseButton::kLeft;
    ASSERT_TRUE(session.enqueue_batch(2, {key}, 1200)); ASSERT_TRUE(session.drain(1200));
    EXPECT_FALSE(session.ordinary_input_recovery_eligible());
    session.release_all(); session.set_transfer_blocked(true, 1200);
    EXPECT_FALSE(session.ordinary_input_recovery_eligible());
    EXPECT_FALSE(session.ordinary_input_recovery_context_valid());
    session.set_transfer_blocked(false, 1300); EXPECT_TRUE(session.ordinary_input_recovery_eligible());
    ASSERT_TRUE(session.expire_lease(5000)); EXPECT_FALSE(session.ordinary_input_recovery_eligible());
    ASSERT_TRUE(session.request_active(6000));
    session.pause(RemoteInputPauseReason::kNoVideo); EXPECT_FALSE(session.ordinary_input_recovery_eligible());
}
} // namespace
