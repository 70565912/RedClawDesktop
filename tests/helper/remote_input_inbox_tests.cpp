#include "runtime/remote_input_inbox.h"
#include <gtest/gtest.h>

namespace {
using Inbox = redclaw::runtime::RemoteInputInbox;
using Type = redclaw::protocol::StreamControlMessageTypeV1;
using Event = redclaw::protocol::RemoteInputEventV1;
using EventType = redclaw::protocol::RemoteInputEventTypeV1;
Inbox::Message move(std::uint64_t sequence, std::uint64_t geometry = 1) {
    Inbox::Message message;
    message.type = Type::kInputBatch; message.session_epoch = "peer";
    message.input_sequence = sequence; message.desktop_geometry_revision = geometry;
    Event event; event.type = EventType::kMouseMove;
    event.normalized_x = static_cast<std::uint16_t>(sequence);
    message.input_events.push_back(event);
    return message;
}
Inbox::Message key(std::uint64_t sequence) {
    auto message = move(sequence);
    message.input_events.front().type = EventType::kKeyDown;
    message.input_events.front().scan_code = 0x1e;
    return message;
}
}

TEST(RemoteInputInbox, DelayedConsumerKeepsLatestMotionWithoutOverflow) {
    Inbox inbox; std::size_t accepted = 0;
    for (std::uint64_t sequence = 1; sequence <= 1024; ++sequence)
        accepted += inbox.enqueue(move(sequence), 100 + sequence) ? 1U : 0U;
    EXPECT_EQ(accepted, 1024U);
    EXPECT_EQ(inbox.stats().peak, 1U);
    EXPECT_EQ(inbox.stats().coalesced, 1023U);
    EXPECT_EQ(inbox.stats().overflows, 0U);
    auto batch = inbox.take(1200);
    ASSERT_EQ(batch.messages.size(), 1U);
    EXPECT_EQ(batch.messages.front().input_sequence, 1024U);
    EXPECT_EQ(batch.messages.front().input_events.front().normalized_x, 1024U);
    EXPECT_FALSE(batch.overflow);
    EXPECT_EQ(inbox.stats().max_pending_ms, 1099U);
}

TEST(RemoteInputInbox, MotionNeverCrossesKeyboardReleaseOrGeometryBoundaries) {
    Inbox inbox;
    ASSERT_TRUE(inbox.enqueue(move(1), 1));
    ASSERT_TRUE(inbox.enqueue(key(2), 2));
    ASSERT_TRUE(inbox.enqueue(move(3), 3));
    auto release = move(4); release.type = Type::kInputStateSync; release.input_events.clear();
    ASSERT_TRUE(inbox.enqueue(release, 4));
    ASSERT_TRUE(inbox.enqueue(move(5), 5));
    ASSERT_TRUE(inbox.enqueue(move(6, 2), 6));
    auto stop = move(7); stop.type = Type::kInputReleaseAll; stop.input_events.clear();
    ASSERT_TRUE(inbox.enqueue(stop, 7));
    ASSERT_TRUE(inbox.enqueue(move(8, 2), 8));
    auto batch = inbox.take(10);
    ASSERT_EQ(batch.messages.size(), 8U);
    for (std::size_t i = 0; i < batch.messages.size(); ++i)
        EXPECT_EQ(batch.messages[i].input_sequence, i + 1U);
}

TEST(RemoteInputInbox, DoesNotCoalesceStaleSequencesOrDifferentPeerEpochs) {
    Inbox inbox;
    ASSERT_TRUE(inbox.enqueue(move(2), 1));
    ASSERT_TRUE(inbox.enqueue(move(1), 2));
    auto different = move(3); different.session_epoch = "new-peer";
    ASSERT_TRUE(inbox.enqueue(different, 3));
    EXPECT_EQ(inbox.take(4).messages.size(), 3U);
}

TEST(RemoteInputInbox, AFullCriticalQueueStillFailsClosedAndRetainsStop) {
    Inbox inbox;
    auto stop = key(1); stop.type = Type::kInputReleaseAll; stop.input_events.clear();
    ASSERT_TRUE(inbox.enqueue(stop, 1));
    for (std::uint64_t i = 2; i <= Inbox::kCapacity; ++i)
        ASSERT_TRUE(inbox.enqueue(key(i), i));
    EXPECT_FALSE(inbox.enqueue(key(65), 65));
    EXPECT_TRUE(inbox.overflow_pending()); EXPECT_TRUE(inbox.has_stop());
    EXPECT_EQ(inbox.stats().peak, Inbox::kCapacity);
    auto batch = inbox.take(100);
    EXPECT_TRUE(batch.overflow); EXPECT_EQ(batch.messages.size(), Inbox::kCapacity);
    EXPECT_FALSE(inbox.overflow_pending()); EXPECT_TRUE(inbox.empty());
}

TEST(RemoteInputInbox, GeometryBoundaryDiscardsOrdinaryInputButKeepsStop) {
    Inbox inbox;
    ASSERT_TRUE(inbox.enqueue(move(1), 1));
    auto stop = move(2); stop.type = Type::kInputControlRequest; stop.input_requested_active = false;
    ASSERT_TRUE(inbox.enqueue(stop, 2));
    inbox.discard_obsolete();
    auto batch = inbox.take(3);
    ASSERT_EQ(batch.messages.size(), 1U);
    EXPECT_EQ(batch.messages.front().type, Type::kInputControlRequest);
    EXPECT_FALSE(batch.messages.front().input_requested_active);
}

TEST(RemoteInputInbox, EmptySyncBurstsCoalesceButHeldStateRemainsOrdered) {
    Inbox inbox;
    for (std::uint64_t i = 1; i <= 100; ++i) {
        auto message = move(i); message.type = Type::kInputStateSync; message.input_events.clear();
        ASSERT_TRUE(inbox.enqueue(message, i));
    }
    auto held = move(101); held.type = Type::kInputStateSync; held.input_events.clear(); held.pressed_scan_codes = {0x1e};
    ASSERT_TRUE(inbox.enqueue(held, 101));
    held.input_sequence = 102; held.pressed_scan_codes.clear();
    ASSERT_TRUE(inbox.enqueue(held, 102));
    auto batch = inbox.take(103);
    ASSERT_EQ(batch.messages.size(), 3U);
    EXPECT_EQ(batch.messages[0].input_sequence, 100U);
    EXPECT_EQ(batch.messages[1].pressed_scan_codes.size(), 1U);
    EXPECT_TRUE(batch.messages[2].pressed_scan_codes.empty());
}

TEST(RemoteInputCaptureBoundary, PreservesAnExistingPauseAndAuthorizationFailure) {
    redclaw::input::InputPolicyGate policy;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession session(policy, backend);
    session.set_authorized(true); ASSERT_TRUE(session.request_active(1000));
    session.pause(redclaw::input::RemoteInputPauseReason::kLocalPause);
    std::deque<Inbox::Message> pending{move(1)};
    EXPECT_FALSE(redclaw::runtime::pause_remote_input_for_capture(session, pending));
    EXPECT_EQ(session.pause_reason(), redclaw::input::RemoteInputPauseReason::kLocalPause);
    EXPECT_TRUE(pending.empty());
    session.set_authorized(false);
    auto start = move(2); start.type = Type::kInputControlRequest; start.input_requested_active = true;
    pending.push_back(start);
    EXPECT_FALSE(redclaw::runtime::pause_remote_input_for_capture(session, pending));
    EXPECT_EQ(session.pause_reason(), redclaw::input::RemoteInputPauseReason::kNotAuthorized);
}

TEST(RemoteInputCaptureBoundary, ActiveCaptureLossStopsInputAndRetainsExplicitStop) {
    redclaw::input::InputPolicyGate policy;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession session(policy, backend);
    session.set_authorized(true); ASSERT_TRUE(session.request_active(1000));
    auto stop = move(2); stop.type = Type::kInputReleaseAll;
    std::deque<Inbox::Message> pending{move(1), stop};
    EXPECT_TRUE(redclaw::runtime::pause_remote_input_for_capture(session, pending));
    EXPECT_EQ(session.state(), redclaw::input::RemoteInputSessionState::kPaused);
    EXPECT_EQ(session.pause_reason(), redclaw::input::RemoteInputPauseReason::kNoVideo);
    ASSERT_EQ(pending.size(), 1U); EXPECT_EQ(pending.front().type, Type::kInputReleaseAll);
}

TEST(RemoteInputCaptureBoundary, ANewStartWithoutVideoIsStillRejected) {
    redclaw::input::InputPolicyGate policy;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession session(policy, backend); session.set_authorized(true);
    auto start = move(1); start.type = Type::kInputControlRequest; start.input_requested_active = true;
    std::deque<Inbox::Message> pending{start};
    EXPECT_TRUE(redclaw::runtime::pause_remote_input_for_capture(session, pending));
    EXPECT_EQ(session.pause_reason(), redclaw::input::RemoteInputPauseReason::kNoVideo);
    EXPECT_TRUE(pending.empty()); EXPECT_TRUE(backend.injected_events().empty());
}
