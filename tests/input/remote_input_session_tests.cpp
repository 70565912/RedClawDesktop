#include <gtest/gtest.h>

#include "redclaw/input/input_module.h"

namespace {

redclaw::input::InputEvent key_event(
    redclaw::input::InputEventType type,
    std::uint16_t scan_code,
    bool repeat = false) {
    redclaw::input::InputEvent event;
    event.type = type;
    event.scan_code = scan_code;
    event.repeat = repeat;
    return event;
}

TEST(RemoteInputGeometryTests, MapsVirtualDesktopAndRotations) {
    redclaw::input::DesktopGeometry geometry;
    geometry.origin_x = -1920;
    geometry.origin_y = -100;
    geometry.width = 3840;
    geometry.height = 1080;
    geometry.revision = 1;
    redclaw::input::DesktopPoint point;

    ASSERT_TRUE(redclaw::input::map_normalized_desktop_point(0, 0, geometry, &point));
    EXPECT_EQ(point.x, -1920);
    EXPECT_EQ(point.y, -100);
    ASSERT_TRUE(redclaw::input::map_normalized_desktop_point(65535, 65535, geometry, &point));
    EXPECT_EQ(point.x, 1919);
    EXPECT_EQ(point.y, 979);

    geometry.rotation = 90;
    ASSERT_TRUE(redclaw::input::map_normalized_desktop_point(0, 0, geometry, &point));
    EXPECT_EQ(point.x, -1920);
    EXPECT_EQ(point.y, 979);
    geometry.rotation = 180;
    ASSERT_TRUE(redclaw::input::map_normalized_desktop_point(0, 0, geometry, &point));
    EXPECT_EQ(point.x, 1919);
    EXPECT_EQ(point.y, 979);
    geometry.rotation = 270;
    ASSERT_TRUE(redclaw::input::map_normalized_desktop_point(0, 0, geometry, &point));
    EXPECT_EQ(point.x, 1919);
    EXPECT_EQ(point.y, -100);
}

TEST(RemoteInputGeometryTests, MapsSelectedRegionBeforeDisplayRotation) {
    redclaw::input::DesktopGeometry geometry;
    geometry.origin_x = -1920;
    geometry.origin_y = -100;
    geometry.width = 1920;
    geometry.height = 1080;
    geometry.revision = 8;
    const redclaw::input::DesktopCaptureRegion region{
        .x = 320,
        .y = 180,
        .width = 1280,
        .height = 720,
    };
    redclaw::input::DesktopPoint point;

    ASSERT_TRUE(redclaw::input::map_normalized_capture_region_point(
        0, 0, geometry, region, &point));
    EXPECT_EQ(point.x, -1600);
    EXPECT_EQ(point.y, 80);
    ASSERT_TRUE(redclaw::input::map_normalized_capture_region_point(
        65535, 65535, geometry, region, &point));
    EXPECT_EQ(point.x, -321);
    EXPECT_EQ(point.y, 799);

    geometry.rotation = 90;
    ASSERT_TRUE(redclaw::input::map_normalized_capture_region_point(
        0, 0, geometry, region, &point));
    EXPECT_EQ(point.x, -1600);
    EXPECT_EQ(point.y, 799);
    geometry.rotation = 180;
    ASSERT_TRUE(redclaw::input::map_normalized_capture_region_point(
        0, 0, geometry, region, &point));
    EXPECT_EQ(point.x, -321);
    EXPECT_EQ(point.y, 799);
    geometry.rotation = 270;
    ASSERT_TRUE(redclaw::input::map_normalized_capture_region_point(
        0, 0, geometry, region, &point));
    EXPECT_EQ(point.x, -321);
    EXPECT_EQ(point.y, 80);
}

TEST(RemoteInputSessionTests, DeniedSessionNeverInvokesBackend) {
    redclaw::input::InputPolicyGate gate;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession session(gate, backend);
    std::string error;

    EXPECT_FALSE(session.request_active(1000, &error));
    EXPECT_TRUE(backend.injected_events().empty());
    EXPECT_EQ(session.state(), redclaw::input::RemoteInputSessionState::kDenied);
}

TEST(RemoteInputSessionTests, AppliesBatchSuppressesDuplicateDownAndReleases) {
    redclaw::input::InputPolicyGate gate;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession session(gate, backend);
    session.set_authorized(true);
    ASSERT_TRUE(session.request_active(1000));

    std::vector<redclaw::input::InputEvent> events;
    events.push_back(key_event(redclaw::input::InputEventType::kKeyDown, 0x1E));
    events.push_back(key_event(redclaw::input::InputEventType::kKeyDown, 0x1E));
    events.push_back(key_event(redclaw::input::InputEventType::kKeyDown, 0x1E, true));
    ASSERT_TRUE(session.enqueue_batch(1, std::move(events), 1100));
    ASSERT_TRUE(session.drain(1100));
    ASSERT_EQ(backend.injected_events().size(), 2U);

    session.pause(redclaw::input::RemoteInputPauseReason::kLocalPause);
    ASSERT_EQ(backend.injected_events().size(), 3U);
    EXPECT_EQ(backend.injected_events().back().type, redclaw::input::InputEventType::kKeyUp);
    EXPECT_EQ(session.state(), redclaw::input::RemoteInputSessionState::kPaused);
}

TEST(RemoteInputSessionTests, QaReceiptsTrackOnlyActuallyInjectedEventsWithTheirBatchSequence) {
    using namespace redclaw::input;
    InputPolicyGate gate; InMemoryInputInjectorBackend backend; RemoteInputSession session(gate, backend);
    std::vector<InputInjectionReceipt> receipts;
    session.set_injection_observer([&](const auto& receipt) { receipts.push_back(receipt); });
    session.set_authorized(true); ASSERT_TRUE(session.request_active(1000));
    ASSERT_TRUE(session.enqueue_batch(11, {key_event(InputEventType::kKeyDown, 30), key_event(InputEventType::kKeyDown, 30)}, 1100));
    ASSERT_TRUE(session.enqueue_batch(12, {key_event(InputEventType::kKeyUp, 30)}, 1100));
    ASSERT_TRUE(session.drain(1100));
    ASSERT_EQ(receipts.size(), 2); EXPECT_EQ(receipts[0].sequence, 11); EXPECT_EQ(receipts[1].sequence, 12);
    EXPECT_EQ(receipts[0].type, InputEventType::kKeyDown); EXPECT_EQ(receipts[1].type, InputEventType::kKeyUp);
    EXPECT_TRUE(receipts[0].injected); EXPECT_GT(receipts[0].begin_us, 0); EXPECT_GE(receipts[0].end_us, receipts[0].begin_us);
}

TEST(RemoteInputSessionTests, StateSyncAndLeasePreventStuckKeys) {
    redclaw::input::InputPolicyGate gate;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession session(gate, backend);
    session.set_authorized(true);
    ASSERT_TRUE(session.request_active(1000));
    ASSERT_TRUE(session.enqueue_batch(
        1,
        {key_event(redclaw::input::InputEventType::kKeyDown, 0x1D)},
        1100));
    ASSERT_TRUE(session.drain(1100));
    ASSERT_TRUE(session.synchronize_state(2, {}, 0, 1500));
    ASSERT_EQ(backend.injected_events().size(), 2U);
    EXPECT_EQ(backend.injected_events().back().type, redclaw::input::InputEventType::kKeyUp);
    EXPECT_EQ(session.state(), redclaw::input::RemoteInputSessionState::kActive);

    EXPECT_TRUE(session.expire_lease(4500));
    EXPECT_EQ(session.pause_reason(), redclaw::input::RemoteInputPauseReason::kLeaseExpired);
}

TEST(RemoteInputSessionTests, ReleaseAllPreservesActiveControlIntent) {
    redclaw::input::InputPolicyGate gate;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession session(gate, backend);
    session.set_authorized(true);
    ASSERT_TRUE(session.request_active(1000));
    ASSERT_TRUE(session.enqueue_batch(
        1,
        {key_event(redclaw::input::InputEventType::kKeyDown, 0x1D)},
        1100));
    ASSERT_TRUE(session.drain(1100));

    session.release_all();

    ASSERT_EQ(backend.injected_events().size(), 2U);
    EXPECT_EQ(backend.injected_events().back().type, redclaw::input::InputEventType::kKeyUp);
    EXPECT_EQ(session.state(), redclaw::input::RemoteInputSessionState::kActive);
    EXPECT_EQ(session.pause_reason(), redclaw::input::RemoteInputPauseReason::kNone);
}

TEST(RemoteInputSessionTests, StateSyncNeverInventsPressedKeys) {
    redclaw::input::InputPolicyGate gate;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession session(gate, backend);
    session.set_authorized(true);
    ASSERT_TRUE(session.request_active(1000));

    ASSERT_TRUE(session.synchronize_state(1, {0x1D}, 0, 1100));
    EXPECT_TRUE(backend.injected_events().empty());
    session.pause(redclaw::input::RemoteInputPauseReason::kLocalPause);
    EXPECT_TRUE(backend.injected_events().empty());
}

TEST(RemoteInputSessionTests, DisconnectStartsFreshInputSequenceEpoch) {
    redclaw::input::InputPolicyGate gate;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession session(gate, backend);
    session.set_authorized(true);
    ASSERT_TRUE(session.request_active(1000));
    ASSERT_TRUE(session.enqueue_batch(
        50,
        {key_event(redclaw::input::InputEventType::kKeyDown, 0x1E)},
        1100));
    ASSERT_TRUE(session.drain(1100));

    session.pause(redclaw::input::RemoteInputPauseReason::kDisconnected);
    ASSERT_TRUE(session.request_active(2000));
    EXPECT_TRUE(session.enqueue_batch(
        1,
        {key_event(redclaw::input::InputEventType::kKeyDown, 0x30)},
        2100));
}

TEST(RemoteInputSessionTests, ReauthorizedChannelClearsDisconnectedPause) {
    redclaw::input::InputPolicyGate gate;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession session(gate, backend);
    session.set_authorized(true);
    ASSERT_TRUE(session.request_active(1000));

    session.pause(redclaw::input::RemoteInputPauseReason::kDisconnected);
    ASSERT_EQ(session.state(), redclaw::input::RemoteInputSessionState::kPaused);
    ASSERT_EQ(
        session.pause_reason(),
        redclaw::input::RemoteInputPauseReason::kDisconnected);

    session.set_authorized(true);
    EXPECT_EQ(session.state(), redclaw::input::RemoteInputSessionState::kAvailable);
    EXPECT_EQ(session.pause_reason(), redclaw::input::RemoteInputPauseReason::kNone);
}

TEST(RemoteInputSessionTests, QueueOverflowAndInjectionFailureFailClosed) {
    redclaw::input::InputPolicyGate gate;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession session(gate, backend);
    session.set_authorized(true);
    ASSERT_TRUE(session.request_active(1000));
    std::vector<redclaw::input::InputEvent> overflow(
        redclaw::input::RemoteInputSession::kQueueCapacity + 1U,
        key_event(redclaw::input::InputEventType::kKeyDown, 0x20));
    EXPECT_FALSE(session.enqueue_batch(1, std::move(overflow), 1100));
    EXPECT_EQ(session.pause_reason(), redclaw::input::RemoteInputPauseReason::kQueueOverflow);

    session.set_authorized(true);
    ASSERT_TRUE(session.request_active(2000));
    backend.set_fail_mode(true);
    ASSERT_TRUE(session.enqueue_batch(
        2,
        {key_event(redclaw::input::InputEventType::kKeyDown, 0x21)},
        2100));
    EXPECT_FALSE(session.drain(2100));
    EXPECT_EQ(session.pause_reason(), redclaw::input::RemoteInputPauseReason::kInjectionFailed);
}

TEST(RemoteInputSessionTests, TransferReleasesPressedKeysDropsPendingInputAndPreservesControlIntent) {
    using namespace redclaw::input;
    InputPolicyGate gate; InMemoryInputInjectorBackend backend; RemoteInputSession session(gate, backend);
    session.set_authorized(true); ASSERT_TRUE(session.request_active(1000));
    ASSERT_TRUE(session.enqueue_batch(1, {key_event(InputEventType::kKeyDown, 0x1D)}, 1100));
    ASSERT_TRUE(session.drain(1100));
    ASSERT_TRUE(session.enqueue_batch(2, {key_event(InputEventType::kKeyDown, 0x1E)}, 1200));
    session.set_transfer_blocked(true, 1300);
    EXPECT_EQ(session.queued_event_count(), 0U);
    ASSERT_EQ(backend.injected_events().size(), 2U);
    EXPECT_EQ(backend.injected_events().back().type, InputEventType::kKeyUp);
    EXPECT_EQ(backend.injected_events().back().scan_code, 0x1D);
    std::string error;
    EXPECT_FALSE(session.enqueue_batch(3, {key_event(InputEventType::kKeyDown, 0x20)}, 1400, &error));
    EXPECT_EQ(error, "workspace_transfer_busy");
    EXPECT_FALSE(session.synchronize_state(4, {0x20}, 0, 1400, &error));
    EXPECT_FALSE(session.request_active(1400));
    EXPECT_TRUE(session.drain(60000)); EXPECT_FALSE(session.expire_lease(60000));
    EXPECT_EQ(backend.injected_events().size(), 2U);
    session.set_transfer_blocked(false, 60000);
    EXPECT_EQ(session.state(), RemoteInputSessionState::kActive);
    EXPECT_FALSE(session.enqueue_batch(3, {key_event(InputEventType::kKeyDown, 0x20)}, 60001));
    ASSERT_TRUE(session.enqueue_batch(5, {key_event(InputEventType::kKeyDown, 0x21)}, 60001));
    ASSERT_TRUE(session.drain(60001));
    ASSERT_EQ(backend.injected_events().size(), 3U); EXPECT_EQ(backend.injected_events().back().scan_code, 0x21);
}
TEST(RemoteInputSessionTests, VerifiedClipboardPasteIsFixedAndCannotReopenOrReplayGeneralInput) {
    using namespace redclaw::input;
    InputPolicyGate gate; InMemoryInputInjectorBackend backend; RemoteInputSession session(gate, backend);
    session.set_authorized(true); ASSERT_TRUE(session.request_active(1000));
    const auto revision = session.eligibility_revision();
    EXPECT_FALSE(session.paste_verified_clipboard(revision));
    session.set_transfer_blocked(true, 1100);
    EXPECT_EQ(session.eligibility_revision(), revision); ASSERT_TRUE(session.clipboard_paste_eligible());
    ASSERT_TRUE(session.paste_verified_clipboard(revision));
    ASSERT_EQ(backend.injected_events().size(), 4U);
    EXPECT_EQ(backend.injected_events()[0].key_code, 0x11); EXPECT_EQ(backend.injected_events()[1].key_code, 0x56);
    EXPECT_EQ(backend.injected_events()[2].type, InputEventType::kKeyUp); EXPECT_EQ(backend.injected_events()[3].type, InputEventType::kKeyUp);
    EXPECT_FALSE(session.paste_verified_clipboard(revision));
    EXPECT_FALSE(session.enqueue_batch(1, {key_event(InputEventType::kKeyDown, 0x20)}, 1200));
    EXPECT_EQ(backend.injected_events().size(), 4U);
    session.pause(RemoteInputPauseReason::kGeometryChanged);
    EXPECT_FALSE(session.clipboard_paste_eligible());
    EXPECT_FALSE(session.paste_verified_clipboard(session.eligibility_revision()));
    session.set_transfer_blocked(false, 1300);
    EXPECT_EQ(session.pause_reason(), RemoteInputPauseReason::kGeometryChanged);
}

TEST(RemoteInputSessionTests, TransferCompletionCannotClearOtherPauseReasonsOrReviveExpiredConsent) {
    using namespace redclaw::input;
    for (const auto reason : {RemoteInputPauseReason::kNoVideo, RemoteInputPauseReason::kGeometryChanged,
        RemoteInputPauseReason::kDisconnected, RemoteInputPauseReason::kLocalPause}) {
        InputPolicyGate gate; InMemoryInputInjectorBackend backend; RemoteInputSession session(gate, backend);
        session.set_authorized(true); ASSERT_TRUE(session.request_active(1000));
        session.set_transfer_blocked(true, 1100);
        session.pause(reason);
        session.set_transfer_blocked(false, 60000);
        EXPECT_EQ(session.state(), RemoteInputSessionState::kPaused); EXPECT_EQ(session.pause_reason(), reason);
        EXPECT_FALSE(session.enqueue_batch(1, {key_event(InputEventType::kKeyDown, 0x21)}, 60001));
        EXPECT_TRUE(backend.injected_events().empty());
    }
    InputPolicyGate gate; InMemoryInputInjectorBackend backend; RemoteInputSession session(gate, backend);
    session.set_authorized(true); ASSERT_TRUE(session.request_active(1000));
    session.set_transfer_blocked(true, 5000); session.set_transfer_blocked(false, 6000);
    EXPECT_EQ(session.pause_reason(), RemoteInputPauseReason::kLeaseExpired);
    ASSERT_TRUE(session.request_active(7000)); session.set_transfer_blocked(true, 7100);
    session.set_authorized(false); session.set_transfer_blocked(false, 7200);
    EXPECT_EQ(session.state(), RemoteInputSessionState::kDenied);
    EXPECT_EQ(session.pause_reason(), RemoteInputPauseReason::kNotAuthorized);
}

}  // namespace
