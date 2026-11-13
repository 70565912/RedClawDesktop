#include <cstdlib>
#include <iostream>

#include "redclaw/session/session_module.h"
#include "redclaw/protocol/video_codec_negotiation.h"

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

void test_keyframe_request_reopens_settled_static_reference() {
    using namespace redclaw::session;
    DesktopSourceActivityTracker tracker({.static_quiet_ms = 500, .minimum_timeout_polls = 1});
    tracker.on_capture_frame(10, 1, 1);
    const auto pending = tracker.on_capture_timeout(510);
    tracker.on_reference_submitted(1, 1, 510, 1);
    tracker.on_displayable_ack(pending.snapshot.revision, 1);
    const auto quiet = tracker.on_capture_timeout(520);
    require(quiet.snapshot.state == DesktopSourceActivityState::kStatic
        && !quiet.request_keyframe && !quiet.request_reference_frame,
        "settled static desktop must stay quiet without a request");

    const auto refresh = tracker.on_keyframe_request();
    require(refresh.snapshot.state == DesktopSourceActivityState::kStaticPending
        && refresh.state_changed && refresh.request_keyframe && refresh.request_reference_frame,
        "static source must request an IDR from its retained capture");
    require(refresh.snapshot.last_frame_ms == quiet.snapshot.last_frame_ms,
        "the reference must be requested without new capture");
    tracker.on_reference_submitted(2, 2, 530, 1);
    const auto replacement = tracker.on_keyframe_request();
    require(replacement.request_reference_frame && replacement.snapshot.reference_keyframe_id == 0
        && replacement.snapshot.revision > refresh.snapshot.revision,
        "a new request must supersede an older pending reference");
    require(tracker.on_displayable_ack(refresh.snapshot.revision, 2).snapshot.state
        == DesktopSourceActivityState::kStaticPending, "a stale ACK must not settle the source");
    tracker.on_reference_submitted(3, 3, 540, 1);
    require(tracker.on_displayable_ack(replacement.snapshot.revision, 3).snapshot.state
        == DesktopSourceActivityState::kStatic, "the replacement IDR must settle the source");
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

void test_playback_pause_suspends_and_restarts_one_recovery_generation() {
    using namespace redclaw::session;
    ControllerDecoderRecoveryCoordinator coordinator;
    auto first=coordinator.on_dependency_break(ControllerDecoderBreakReason::kSharedSequenceGap, 1, 1000, 0);
    require(first.request_keyframe, "initial dependency break requests IDR");
    coordinator.on_keyframe_submitted(2, 1001, 0);
    coordinator.set_playback_state(1, 1, true);
    for (std::uint64_t now=2000; now<20000; now+=500) {
        require(!coordinator.tick(now, 0).request_keyframe, "pause cancels runtime retries");
        require(!coordinator.on_dependency_break(ControllerDecoderBreakReason::kSharedSequenceGap, 3, now, 0).request_keyframe,
            "pause suppresses gaps even when draining stalls");
        coordinator.on_keyframe_submitted(4, now, 0);
        require(coordinator.should_accept_frame(false), "network and shared ring continue while paused");
    }
    coordinator.set_playback_state(1, 2, false);
    require(!coordinator.tick(21000, 0).request_keyframe, "receiver owns initial resume request");
    auto resumed=coordinator.on_dependency_break(ControllerDecoderBreakReason::kPresentationResumed, 0, 21000, 0);
    require(resumed.request_keyframe && resumed.snapshot.generation == first.snapshot.generation+1,
        "resume starts exactly one generation");
    require(!coordinator.on_dependency_break(ControllerDecoderBreakReason::kDependencyFrame, 5, 21001, 0).request_keyframe,
        "duplicate initial request coalesces");
    require(!coordinator.tick(21499, 0).request_keyframe, "retry observes minimum interval");
    require(coordinator.tick(21500, 0).request_keyframe, "failed delivery gets bounded-delay retry");
    coordinator.on_keyframe_submitted(6, 21501, 0);
    require(coordinator.should_accept_frame(false), "submitted IDR permits dependencies awaiting output");
    coordinator.on_displayable_ack(6);
    require(!coordinator.tick(30000, 0).request_keyframe, "displayable output stops retries");
    coordinator.set_playback_state(1, 3, true);
    coordinator.set_playback_state(2, 4, true);
    coordinator.reset();
    require(!coordinator.tick(40000, 0).request_keyframe, "reset cannot revive a paused session");
}

void test_resume_notification_timeout_arms_recovery() {
    using namespace redclaw::session;
    ControllerDecoderRecoveryCoordinator coordinator;
    coordinator.set_playback_state(1, 1, true);
    coordinator.set_playback_state(1, 2, false);
    require(!coordinator.tick(1000, 0).request_keyframe, "give the coalescer the first opportunity");
    coordinator.on_dependency_break(ControllerDecoderBreakReason::kSharedSequenceGap, 5, 1100, 0);
    require(!coordinator.tick(1499, 0).request_keyframe, "honor the resume notification grace");
    const auto first = coordinator.tick(1500, 0);
    require(first.request_keyframe && first.snapshot.state == ControllerDecoderRecoveryState::kAwaitingIdr,
        "a missing dedicated resume notification must still arm recovery");
    require(!coordinator.on_dependency_break(
        ControllerDecoderBreakReason::kPresentationResumed, 0, 1600, 0).request_keyframe,
        "a delayed notification must coalesce into the existing recovery");
    require(coordinator.tick(2000, 0).request_keyframe, "an unanswered request must retry");
    coordinator.on_keyframe_submitted(6, 2001, 0);
    require(!coordinator.on_dependency_break(
        ControllerDecoderBreakReason::kPresentationResumed, 0, 2002, 0).request_keyframe,
        "a delayed notification must preserve the submitted IDR");
    coordinator.on_displayable_ack(6);
    require(!coordinator.tick(3000, 0).request_keyframe, "displayable IDR must end retries");
}

void test_codec_change_refreshes_a_settled_source_at_the_restart_deadline() {
    using namespace redclaw::session;
    using namespace redclaw::protocol;
    DesktopSourceActivityTracker tracker({.static_quiet_ms = 500, .minimum_timeout_polls = 1});
    tracker.on_capture_frame(10, 1, 1);
    const auto pending = tracker.on_capture_timeout(510);
    tracker.on_reference_submitted(1, 1, 510, 1);
    tracker.on_displayable_ack(pending.snapshot.revision, 1);
    require(tracker.snapshot().state == DesktopSourceActivityState::kStatic, "test starts on a settled desktop");

    HostVideoCodecNegotiatorV1 negotiator(VideoCodecPreference::kAuto);
    const HostVideoEncodeCapabilities host{.hevc_hardware_encoder_available = true};
    const auto h264_revision = negotiator.snapshot().revision;
    const auto mask = kVideoCodecMaskH264 | kVideoCodecMaskHevc;
    negotiator.observe_peer(1, mask, mask);
    const auto hevc = negotiator.snapshot();
    const auto upgrade = running_video_codec_change_due_ms(NegotiatedVideoCodec::kH264,
        h264_revision, hevc.revision, negotiator.select(host, hevc), 500, 2000);
    require(upgrade == 2500, "late HEVC upgrade must wake at the encoder age deadline");
    auto refresh = tracker.on_keyframe_request();
    require(refresh.snapshot.state == DesktopSourceActivityState::kStaticPending && refresh.request_reference_frame,
        "eligible HEVC upgrade requires one retained reference");
    tracker.on_reference_submitted(2, 2, *upgrade, 2);
    tracker.on_displayable_ack(refresh.snapshot.revision, 2);
    require(!running_video_codec_change_due_ms(NegotiatedVideoCodec::kHevc, hevc.revision,
        hevc.revision, negotiator.select(host, hevc), *upgrade, 2000),
        "settled source needs no more encodes after the codec change");

    negotiator.observe_peer(1, kVideoCodecMaskH264, kVideoCodecMaskH264);
    const auto h264 = negotiator.snapshot();
    require(running_video_codec_change_due_ms(NegotiatedVideoCodec::kHevc, hevc.revision,
        h264.revision, negotiator.select(host, h264), *upgrade, 2000) == 0,
        "withdrawal must be eligible immediately");
    refresh = tracker.on_keyframe_request();
    require(refresh.snapshot.state == DesktopSourceActivityState::kStaticPending && refresh.request_reference_frame,
        "withdrawal requires one H.264 reference even without new capture");
}

void test_geometry_preserves_resume_retry_until_displayable() {
    using namespace redclaw::session;
    ControllerDecoderRecoveryCoordinator coordinator;
    coordinator.set_playback_state(1, 1, true);
    coordinator.set_playback_state(1, 2, false);
    coordinator.on_geometry_request();
    require(coordinator.on_dependency_break(ControllerDecoderBreakReason::kPresentationResumed, 0, 1000, 0)
        .request_keyframe, "viewport before resume notice must preserve its initial request slot");
    coordinator.on_geometry_request();
    require(coordinator.tick(1500, 0).request_keyframe, "duplicate viewport cannot stop IDR retries");
    coordinator.on_keyframe_submitted(7, 1501, 0);
    coordinator.on_geometry_request();
    require(coordinator.tick(3500, 0).request_keyframe, "capture-region request cannot stop displayable ACK retries");
    require(coordinator.on_displayable_ack(7).state_changed, "geometry IDR can still complete recovery");
    require(!coordinator.tick(6000, 0).request_keyframe, "displayable geometry IDR ends retries");
}

void test_stale_resume_and_ack_cannot_mutate_newer_recovery() {
    using namespace redclaw::session;
    ControllerDecoderRecoveryCoordinator coordinator;
    const ControllerPlaybackIdentity old_resume{1, 2, false};
    const ControllerPlaybackIdentity new_resume{1, 4, false};
    coordinator.set_playback_state(1, 2, false);
    coordinator.set_playback_state(1, 3, true);
    coordinator.set_playback_state(1, 4, false);
    require(!coordinator.on_dependency_break(ControllerDecoderBreakReason::kPresentationResumed, 0, 1000, 0,
        old_resume).request_keyframe, "queued stale resume cannot consume the new request slot");
    require(coordinator.on_dependency_break(ControllerDecoderBreakReason::kPresentationResumed, 0, 1001, 0,
        new_resume).request_keyframe, "the new resume retains its own request slot");
    require(!coordinator.on_keyframe_submitted(10, 1002, 0, old_resume).state_changed,
        "an old IDR submission cannot become the new reference");
    require(coordinator.on_keyframe_submitted(11, 1003, 0, new_resume).state_changed,
        "current IDR is accepted");
    require(!coordinator.on_displayable_ack(11, ControllerPlaybackIdentity{2, 4, false}).state_changed,
        "old session ACK is rejected even if frame ID and revision match");
    require(!coordinator.on_displayable_ack(11, old_resume).state_changed,
        "old revision ACK is rejected even if frame ID matches");
    require(coordinator.on_displayable_ack(11, new_resume).state_changed,
        "current displayable ACK completes recovery");
}

int main() {
    test_codec_change_refreshes_a_settled_source_at_the_restart_deadline();
    test_geometry_preserves_resume_retry_until_displayable();
    test_stale_resume_and_ack_cannot_mutate_newer_recovery();
    test_playback_pause_suspends_and_restarts_one_recovery_generation();
    test_sparse_updates_preserve_predictive_chain();
    test_static_requires_healthy_timeouts_and_displayable_keyframe();
    test_stall_is_unknown_and_frame_change_reactivates();
    test_static_viewport_change_and_bounded_retry();
    test_keyframe_request_reopens_settled_static_reference();
    test_resume_notification_timeout_arms_recovery();
    test_work_coordinator_coalesces_typed_reasons();
    test_decoder_recovery_coalesces_breaks_and_waits_for_displayable_idr();
    test_decoder_recovery_retries_invalidates_and_resets();
    return 0;
}
