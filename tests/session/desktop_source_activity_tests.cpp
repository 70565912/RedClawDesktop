#include <cstdlib>
#include <iostream>

#include "redclaw/session/session_module.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void test_static_requires_healthy_timeouts_and_displayable_keyframe() {
    redclaw::session::DesktopSourceActivityTracker tracker({.static_quiet_ms = 500});
    tracker.reset(4, 7);
    tracker.on_capture_poll_started(100);
    auto update = tracker.on_capture_frame(110, 4, 7);
    require(update.snapshot.state == redclaw::session::DesktopSourceActivityState::kActive,
        "first frame must activate source");
    require(update.request_keyframe, "first frame must request IDR");

    for (std::uint64_t now : {300ULL, 500ULL}) {
        tracker.on_capture_poll_started(now - 10);
        update = tracker.on_capture_timeout(now);
        require(update.snapshot.state == redclaw::session::DesktopSourceActivityState::kActive,
            "fewer than three timeouts must not declare static");
    }
    tracker.on_capture_poll_started(690);
    update = tracker.on_capture_timeout(700);
    require(update.snapshot.state == redclaw::session::DesktopSourceActivityState::kStaticPending,
        "quiet source with three healthy polls must enter static_pending");
    require(update.request_reference_frame, "static_pending must request a reference IDR");
    const auto revision = update.snapshot.revision;

    update = tracker.on_reference_submitted(42, 42, 700, 3);
    require(update.snapshot.reference_keyframe_id == 42, "reference IDR must be recorded");
    update = tracker.on_displayable_ack(revision - 1, 42);
    require(update.snapshot.state == redclaw::session::DesktopSourceActivityState::kStaticPending,
        "old revision ACK must not complete static transition");
    update = tracker.on_displayable_ack(revision, 41);
    require(update.snapshot.state == redclaw::session::DesktopSourceActivityState::kStaticPending,
        "ACK before reference IDR must not complete static transition");
    update = tracker.on_displayable_ack(revision, 42);
    require(update.snapshot.state == redclaw::session::DesktopSourceActivityState::kStatic,
        "displayable reference IDR must complete static transition");
}

void test_stall_is_unknown_and_frame_change_reactivates() {
    redclaw::session::DesktopSourceActivityTracker tracker;
    tracker.reset(1, 1);
    tracker.on_capture_poll_started(10);
    tracker.on_capture_frame(20, 1, 1);
    tracker.on_capture_poll_started(30);
    auto update = tracker.tick(1031);
    require(update.snapshot.state == redclaw::session::DesktopSourceActivityState::kUnknown,
        "stopped polling must be unknown, not static");
    require(update.snapshot.capture_stall_total == 1, "capture stall must be counted");

    tracker.on_capture_poll_started(1040);
    update = tracker.on_capture_frame(1050, 1, 1);
    require(update.snapshot.state == redclaw::session::DesktopSourceActivityState::kActive,
        "first frame after a stall must reactivate source");
    require(update.request_keyframe, "reactivation must start from IDR");
}

void test_static_viewport_change_and_bounded_retry() {
    redclaw::session::DesktopSourceActivityTracker tracker({.static_quiet_ms = 500});
    tracker.reset(2, 3);
    tracker.on_capture_poll_started(1);
    tracker.on_capture_frame(10, 2, 3);
    for (std::uint64_t now : {510ULL, 520ULL, 530ULL}) {
        tracker.on_capture_poll_started(now - 1);
        tracker.on_capture_timeout(now);
    }
    auto snapshot = tracker.snapshot();
    tracker.on_reference_submitted(100, 100, 530, 4);
    tracker.on_displayable_ack(snapshot.revision, 100);

    auto update = tracker.on_viewport_change(600, 4, 5);
    require(update.snapshot.state == redclaw::session::DesktopSourceActivityState::kStaticPending,
        "static viewport change must re-enter static_pending");
    require(update.snapshot.stream_geometry_revision == 4, "new geometry revision must be retained");
    tracker.on_reference_submitted(101, 101, 600, 4);
    require(!tracker.reference_retry_due(1599, 0), "first retry must wait one second");
    require(tracker.reference_retry_due(1600, 0), "first retry must become due at one second");
    update = tracker.mark_reference_retry(1600);
    require(update.request_reference_frame && update.snapshot.reference_retry_total == 1,
        "retry must request a bounded replacement reference");
}

void test_work_coordinator_coalesces_typed_reasons() {
    redclaw::session::HostStreamWorkCoordinator coordinator;
    const auto initial = coordinator.snapshot();
    coordinator.post(redclaw::session::HostStreamWorkReason::kViewport);
    coordinator.post(redclaw::session::HostStreamWorkReason::kSourceReference);
    const auto work = coordinator.consume(initial.generation);
    require(work.contains(redclaw::session::HostStreamWorkReason::kViewport),
        "viewport wake reason missing");
    require(work.contains(redclaw::session::HostStreamWorkReason::kSourceReference),
        "source-reference wake reason missing");
    const auto empty = coordinator.consume(work.generation);
    require(empty.reasons == 0, "consumed work must not repeat without a new generation");
}

void test_decoder_recovery_coalesces_breaks_and_waits_for_displayable_idr() {
    using redclaw::session::ControllerDecoderBreakReason;
    using redclaw::session::ControllerDecoderRecoveryCoordinator;
    using redclaw::session::ControllerDecoderRecoveryState;

    ControllerDecoderRecoveryCoordinator coordinator;
    auto update = coordinator.on_dependency_break(
        ControllerDecoderBreakReason::kDirectPipeBusy,
        40,
        1000,
        20);
    require(update.request_keyframe, "first dependency break must request one IDR");
    require(update.snapshot.generation == 1, "first break must start generation one");

    update = coordinator.on_dependency_break(
        ControllerDecoderBreakReason::kSharedSequenceGap,
        41,
        1010,
        20);
    require(!update.request_keyframe, "same recovery generation must suppress duplicate IDR");
    require(update.snapshot.request_total == 1, "duplicate break must not increase IDR sends");
    require(update.snapshot.suppressed_request_total == 1,
        "duplicate break suppression must be observable");
    require(!coordinator.should_accept_frame(false),
        "dependent P frames must be rejected before an IDR arrives");
    require(coordinator.should_accept_frame(true), "replacement IDR must remain acceptable");

    update = coordinator.on_keyframe_submitted(50, 1100, 20);
    require(update.snapshot.state == ControllerDecoderRecoveryState::kAwaitingDisplayable,
        "written IDR must wait for GUI displayable acknowledgement");
    require(coordinator.should_accept_frame(false),
        "ordered P frames after the replacement IDR may continue to the GUI");
    update = coordinator.on_displayable_ack(49);
    require(update.snapshot.state == ControllerDecoderRecoveryState::kAwaitingDisplayable,
        "older displayable keyframe cannot complete recovery");
    update = coordinator.on_displayable_ack(50);
    require(update.snapshot.state == ControllerDecoderRecoveryState::kSynchronized,
        "matching displayable IDR must complete recovery");
}

void test_decoder_recovery_retries_invalidates_and_resets() {
    using redclaw::session::ControllerDecoderBreakReason;
    using redclaw::session::ControllerDecoderRecoveryCoordinator;
    using redclaw::session::ControllerDecoderRecoveryState;

    ControllerDecoderRecoveryCoordinator coordinator;
    auto update = coordinator.on_dependency_break(
        ControllerDecoderBreakReason::kFragmentReassembly,
        60,
        2000,
        100);
    require(update.snapshot.retry_delay_ms == 500,
        "retry delay must clamp 2*SRTT+250ms to the 500ms minimum");
    require(!coordinator.tick(2499, 100).request_keyframe,
        "retry must not fire before its deadline");
    update = coordinator.tick(2500, 100);
    require(update.request_keyframe && update.snapshot.retry_total == 1,
        "lost IDR must retry at the bounded deadline");
    require(update.snapshot.retry_delay_ms == 1000,
        "retry delay must back off exponentially");
    update = coordinator.tick(3500, 100);
    require(update.request_keyframe && update.snapshot.retry_delay_ms == 2000,
        "retry delay must back off to the two-second ceiling");

    coordinator.on_keyframe_submitted(70, 3600, 100);
    update = coordinator.on_dependency_break(
        ControllerDecoderBreakReason::kSharedSequenceGap,
        71,
        3700,
        100);
    require(update.request_keyframe && update.snapshot.generation == 2,
        "a break after replacement IDR must start a new generation");
    require(update.snapshot.invalidated_keyframe_total == 1,
        "the unusable replacement IDR must be counted as invalidated");

    coordinator.reset();
    const auto snapshot = coordinator.snapshot();
    require(snapshot.state == ControllerDecoderRecoveryState::kSynchronized
            && snapshot.generation == 0 && snapshot.request_total == 0,
        "epoch reset must clear recovery state and counters");
}

} // namespace

void test_sparse_updates_preserve_predictive_chain() {
    redclaw::session::DesktopSourceActivityTracker tracker;
    tracker.reset(1, 1);
    unsigned idrs = 0;
    for (std::uint64_t frame = 0; frame < 60; ++frame) {
        const auto now = 100 + frame * 1000;
        tracker.on_capture_poll_started(now - 1);
        idrs += tracker.on_capture_frame(now, 1, 1).request_keyframe;
        for (std::uint64_t delay : {200ULL, 500ULL, 900ULL}) {
            tracker.on_capture_poll_started(now + delay - 1);
            const auto update = tracker.on_capture_timeout(now + delay);
            idrs += update.request_keyframe;
            require(!update.request_reference_frame, "1 Hz source must not generate static refreshes");
        }
    }
    require(idrs == 1, "60 sparse frames must require only the initial IDR, not 120");
    tracker.on_capture_poll_started(61199);
    auto update = tracker.on_capture_timeout(61200);
    require(update.request_reference_frame, "truly quiet desktop must retain final reference refresh");
    tracker.on_reference_submitted(61, 61, 61200, 1);
    tracker.on_displayable_ack(update.snapshot.revision, 61);
    tracker.on_capture_poll_started(61299);
    update = tracker.on_capture_frame(61300, 1, 1);
    require(!update.request_keyframe, "ordinary static exit must preserve prediction");
    tracker.on_capture_failure(61400);
    require(tracker.on_capture_frame(61500, 1, 1).request_keyframe, "real recovery still requires IDR");
}

int main() {
    test_sparse_updates_preserve_predictive_chain();
    test_static_requires_healthy_timeouts_and_displayable_keyframe();
    test_stall_is_unknown_and_frame_change_reactivates();
    test_static_viewport_change_and_bounded_retry();
    test_work_coordinator_coalesces_typed_reasons();
    test_decoder_recovery_coalesces_breaks_and_waits_for_displayable_idr();
    test_decoder_recovery_retries_invalidates_and_resets();
    return 0;
}
