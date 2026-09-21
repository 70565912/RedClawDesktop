#include <gtest/gtest.h>

#include "redclaw/service/connection_negotiation.h"
#include "redclaw/service/dht_rendezvous.h"
#include "redclaw/service/dht_publish_registry.h"

#include <limits>
#include <array>

namespace redclaw::service {
namespace {

TEST(DhtPublication, PublicTraversalCompletionIsNotExpiredByFastPolling) {
    // Native public-DHT evidence: signing/put completions arrive tens of seconds
    // after submission. Polling an immutable revision must not replace its lease.
    for (const std::uint64_t completion_ms : {30000U, 45000U, 90000U}) {
        detail::DhtPublishRegistry registry;
        const auto timeout = LibtorrentDhtRendezvousStoreOptions{}.publish_timeout_milliseconds;
        const auto first = registry.begin("revision", "key", "salt", 1, 300000, timeout, 15000);
        ASSERT_TRUE(first.submit);
        ASSERT_TRUE(registry.signed_sequence("revision", first.attempt, 41));
        for (std::uint64_t now = 33; now < completion_ms; now += 32) {
            const auto poll = registry.begin("revision", "key", "salt", now, 300000, timeout, 15000);
            ASSERT_FALSE(poll.submit);
            ASSERT_EQ(poll.result.state, DhtPublishState::kPending);
        }
        ASSERT_TRUE(registry.complete("key", "salt", 41, true, completion_ms));
        EXPECT_EQ(registry.begin("revision", "key", "salt", completion_ms + 1,
            300000, timeout, 15000).result.state, DhtPublishState::kSucceeded);
        EXPECT_EQ(registry.stats().late_events, 0U);
        EXPECT_EQ(registry.stats().records, 1U);
    }
}

constexpr std::string_view kHostInstanceA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kHostInstanceB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

TEST(DhtPublishLifecycle, TenThousandRevisionsRetireHistoryAndIgnoreLateActualSequences) {
    detail::DhtPublishRegistry registry;
    for (std::uint64_t revision = 1; revision <= 10000; ++revision) {
        const auto now = revision * 10;
        const auto key = std::to_string(revision);
        const auto lease = registry.begin(key, "public", "salt", now, now + 300000, 8000, 15000);
        ASSERT_TRUE(lease.submit);
        ASSERT_TRUE(registry.signed_sequence(key, lease.attempt, static_cast<std::int64_t>(revision + 500)));
        EXPECT_FALSE(registry.complete("public", "salt", static_cast<std::int64_t>(revision), true, now + 1));
        ASSERT_TRUE(registry.complete("public", "salt", static_cast<std::int64_t>(revision + 500), true, now + 2));
        EXPECT_FALSE(registry.complete("public", "salt", static_cast<std::int64_t>(revision + 500), true, now + 3));
        EXPECT_EQ(registry.begin(key, "public", "salt", now + 4, now + 300000, 8000, 15000).result.state,
            DhtPublishState::kSucceeded);
        ASSERT_LE(registry.stats().records, 256U);
        ASSERT_LE(registry.stats().in_flight, 64U);
    }
    EXPECT_EQ(registry.stats().peak, 256U);
    EXPECT_EQ(registry.stats().late_events, 20000U);
}

TEST(DhtPublishLifecycle, CapacityRetryExpiryAndOldSigningCallbackAreBounded) {
    detail::DhtPublishRegistry registry;
    std::uint64_t first_attempt = 0;
    for (unsigned i = 0; i < 64; ++i) {
        const auto lease = registry.begin(std::to_string(i), "public", std::to_string(i), 1000, 301000, 8000, 15000);
        ASSERT_TRUE(lease.submit);
        if (i == 0) first_attempt = lease.attempt;
    }
    EXPECT_EQ(registry.begin("extra", "public", "extra", 1001, 301000, 8000, 15000).result.state,
        DhtPublishState::kRetryableFailure);
    const auto retry = registry.begin("0", "public", "0", 10000, 301000, 8000, 15000);
    ASSERT_TRUE(retry.submit);
    EXPECT_FALSE(registry.signed_sequence("0", first_attempt, 123));
    EXPECT_TRUE(registry.signed_sequence("0", retry.attempt, 456));
    EXPECT_FALSE(registry.complete("public", "0", 123, true, 10001));
    EXPECT_TRUE(registry.complete("public", "0", 456, false, 10001));
    EXPECT_EQ(registry.stats().in_flight, 0U);
    (void)registry.begin("new", "public", "new", 400000, 401000, 8000, 15000);
    EXPECT_EQ(registry.stats().records, 1U);
}

TEST(DhtPublishLifecycle, IndirectPublicationYieldsAndPublishesSummaryOnlyAfterChunks) {
    class CountingStore final : public IDhtRendezvousStore {
    public:
        unsigned calls = 0, summaries = 0;
        bool acknowledge = false;
        DhtPublishResult publish(const DhtEncryptedRecord& record, std::string_view, std::string*) override {
            ++calls;
            summaries += record.lane == "host";
            return acknowledge ? DhtPublishState::kSucceeded : DhtPublishState::kPending;
        }
        std::optional<DhtEncryptedRecord> fetch(std::string_view, std::string_view, std::uint64_t,
            std::string_view, std::string*) override { return {}; }
    };
    auto primary = std::make_unique<CountingStore>();
    auto* observed = primary.get();
    IndirectDhtRendezvousStore store(std::move(primary), nullptr);
    DhtEncryptedRecord record{std::string(64, 'a'), "host", std::string(12000, 'b'), 1, 9999999999ULL};
    std::string error;
    EXPECT_EQ(store.publish(record, "fixture", &error).state, DhtPublishState::kPending);
    EXPECT_LE(observed->calls, 8U);
    EXPECT_EQ(observed->summaries, 0U);
    observed->acknowledge = true;
    bool done = false;
    for (int turn = 0; turn < 32 && !done; ++turn) {
        const auto before = observed->calls;
        done = store.publish(record, "fixture", &error);
        EXPECT_LE(observed->calls - before, 8U);
    }
    EXPECT_TRUE(done) << error;
    EXPECT_EQ(observed->summaries, 1U);
}

// The record is readable by the peer before the local put confirmation arrives.
// This models the public-DHT ordering without sockets, sleeps or wall clocks.
class DelayedConfirmationStore final : public IDhtRendezvousStore {
public:
    std::uint64_t now_ms = 0;
    std::uint64_t confirm_at_ms = 0;
    std::string first_controller_blob;
    bool reused_controller_blob = true;

    DhtPublishResult publish(const DhtEncryptedRecord& record, std::string_view token,
        std::string* error) override {
        if (!records_.publish(record, token, error)) {
            return false;
        }
        if (record.lane == "controller") {
            if (first_controller_blob.empty()) {
                first_controller_blob = record.encrypted_blob;
            }
            reused_controller_blob &= first_controller_blob == record.encrypted_blob;
            if (now_ms < confirm_at_ms) {
                if (error != nullptr) *error = "fixture: local put confirmation pending";
                return DhtPublishState::kPending;
            }
        }
        return true;
    }

    std::optional<DhtEncryptedRecord> fetch(std::string_view topic, std::string_view lane,
        std::uint64_t after, std::string_view token, std::string* error) override {
        return records_.fetch(topic, lane, after, token, error);
    }
private:
    InMemoryDhtRendezvousStore records_;
};

DhtSignalSnapshot make_answer_fixture() {
    DhtSignalSnapshot snapshot;
    snapshot.role = "controller";
    snapshot.description_type = "answer";
    snapshot.description_sdp = "fixture-sdp";
    snapshot.publisher_instance_id = kHostInstanceB;
    snapshot.generation = 21;
    snapshot.connection_request_tag = "request-00000021";
    snapshot.answered_description_tag = "offer-0000000021";
    snapshot.revision = 100;
    snapshot.candidates_complete = true;
    snapshot.candidate_lines = {
        "0\tcandidate:1 1 udp 2122260223 192.0.2.10 5000 typ host",
        "0\tcandidate:2 1 udp 1686052607 203.0.113.8 62000 typ srflx"};
    return snapshot;
}

TEST(ConnectionNegotiationTest, ControllerTracksMatchingAnswerAppliedAcknowledgement) {
    ConnectionNegotiationCoordinator coordinator(ConnectionNegotiationRole::kController);

    ASSERT_TRUE(coordinator.begin_controller_request("request-7").accepted());
    ASSERT_TRUE(coordinator.mark_controller_request_published());
    const auto offer = coordinator.observe_host_offer(7, "request-7", "offer-7");
    ASSERT_TRUE(offer.accepted());
    EXPECT_TRUE(offer.generation_changed);
    EXPECT_TRUE(coordinator.mark_remote_offer_applied());
    EXPECT_TRUE(coordinator.mark_local_answer_ready("answer-7"));
    EXPECT_TRUE(coordinator.mark_local_answer_published());
    EXPECT_TRUE(coordinator.local_answer_published());
    EXPECT_FALSE(coordinator.answer_acknowledged());

    EXPECT_EQ(
        coordinator.observe_answer_applied(6, "request-7", "offer-6", "answer-6").observation,
        ConnectionNegotiationObservation::kRejectedStaleGeneration);
    EXPECT_EQ(
        coordinator.observe_answer_applied(7, "request-7", "offer-7", "wrong-answer").observation,
        ConnectionNegotiationObservation::kRejectedAnswerMismatch);
    EXPECT_FALSE(coordinator.answer_acknowledged());

    const auto acknowledged = coordinator.observe_answer_applied(
        7, "request-7", "offer-7", "answer-7");
    EXPECT_EQ(acknowledged.observation, ConnectionNegotiationObservation::kAccepted);
    EXPECT_TRUE(coordinator.answer_acknowledged());

    const auto duplicate = coordinator.observe_answer_applied(
        7, "request-7", "offer-7", "answer-7");
    EXPECT_EQ(duplicate.observation, ConnectionNegotiationObservation::kDuplicate);
}

TEST(ConnectionNegotiationTest, SynchronousLocalAnswerDoesNotMoveBackToOfferApplied) {
    ConnectionNegotiationCoordinator coordinator(ConnectionNegotiationRole::kController);

    ASSERT_TRUE(coordinator.begin_controller_request("request-sync").accepted());
    ASSERT_TRUE(coordinator.observe_host_offer(
        1, "request-sync", "offer-sync").accepted());

    // This is the callback order observed from libdatachannel: applying the
    // remote offer emits the answer before applyRemoteDescription returns.
    ASSERT_TRUE(coordinator.mark_local_answer_ready("answer-sync"));
    EXPECT_EQ(coordinator.phase(), ConnectionNegotiationPhase::kAnswerReady);
    ASSERT_TRUE(coordinator.mark_remote_offer_applied());
    EXPECT_EQ(coordinator.phase(), ConnectionNegotiationPhase::kAnswerReady);
    EXPECT_TRUE(coordinator.mark_local_answer_published());
    EXPECT_TRUE(coordinator.local_answer_published());
}

TEST(ConnectionNegotiationTest, OnlyHostCanAdvanceGeneration) {
    ConnectionNegotiationCoordinator host(ConnectionNegotiationRole::kHost);
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);

    EXPECT_TRUE(host.local_timer_may_advance_generation());
    EXPECT_FALSE(controller.local_timer_may_advance_generation());

    ASSERT_TRUE(host.observe_controller_request("request-1").accepted());
    ASSERT_TRUE(host.begin_host_generation(1, "offer-1").accepted());
    EXPECT_EQ(
        host.begin_host_generation(1, "different-offer").observation,
        ConnectionNegotiationObservation::kRejectedGenerationConflict);
    EXPECT_EQ(
        host.begin_host_generation(0, "offer-0").observation,
        ConnectionNegotiationObservation::kRejectedInvalid);
    EXPECT_TRUE(host.begin_host_generation(2, "offer-2").accepted());

    ASSERT_TRUE(controller.begin_controller_request("request-1").accepted());
    ASSERT_TRUE(controller.observe_host_offer(1, "request-1", "offer-1").accepted());
    ASSERT_TRUE(controller.observe_host_offer(2, "request-1", "offer-2").accepted());
    EXPECT_EQ(
        controller.observe_host_offer(1, "request-1", "offer-1").observation,
        ConnectionNegotiationObservation::kRejectedStaleGeneration);
}

TEST(ConnectionNegotiationTest, HostAcknowledgesOnlyTheAnswerForItsCurrentOffer) {
    ConnectionNegotiationCoordinator host(ConnectionNegotiationRole::kHost);
    ASSERT_TRUE(host.observe_controller_request("request-9").accepted());
    ASSERT_TRUE(host.begin_host_generation(9, "offer-9").accepted());

    EXPECT_EQ(
        host.observe_controller_answer(8, "request-9", "offer-8", "answer-8").observation,
        ConnectionNegotiationObservation::kRejectedStaleGeneration);
    EXPECT_EQ(
        host.observe_controller_answer(9, "request-9", "other-offer", "answer-9").observation,
        ConnectionNegotiationObservation::kRejectedOfferMismatch);

    const auto accepted = host.observe_controller_answer(
        9, "request-9", "offer-9", "answer-9");
    ASSERT_EQ(accepted.observation, ConnectionNegotiationObservation::kAccepted);
    EXPECT_TRUE(host.mark_remote_answer_applied("answer-9"));
    EXPECT_TRUE(host.answer_acknowledged());
    EXPECT_EQ(host.acknowledged_answer_description_tag(), "answer-9");
}

TEST(ConnectionNegotiationTest, DeliveryDelayNeverChangesGeneration) {
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);
    ASSERT_TRUE(controller.begin_controller_request("request-3").accepted());
    ASSERT_TRUE(controller.observe_host_offer(3, "request-3", "offer-3").accepted());
    ASSERT_TRUE(controller.mark_remote_offer_applied());
    ASSERT_TRUE(controller.mark_local_answer_ready("answer-3"));
    ASSERT_TRUE(controller.mark_local_answer_published());

    for (int elapsed_seconds = 0; elapsed_seconds < 120; ++elapsed_seconds) {
        const auto timeout = controller.observe_delivery_timeout();
        EXPECT_EQ(timeout.observation, ConnectionNegotiationObservation::kDuplicate);
        EXPECT_FALSE(timeout.generation_changed);
    }

    EXPECT_EQ(controller.generation(), 3U);
    EXPECT_EQ(controller.phase(), ConnectionNegotiationPhase::kAnswerPublished);
    EXPECT_FALSE(controller.answer_acknowledged());
}

TEST(ConnectionNegotiationTest, ControllerAdoptsPersistentHostOfferWithoutTimingWindow) {
    ConnectionNegotiationCoordinator host(ConnectionNegotiationRole::kHost);
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);

    ASSERT_TRUE(host.observe_controller_request("host-standby-lease").accepted());
    ASSERT_TRUE(host.begin_host_generation(1, "persistent-offer").accepted());

    ASSERT_TRUE(controller.begin_controller_request("controller-wake-record").accepted());
    ASSERT_TRUE(controller.mark_controller_request_published());
    const auto offer = controller.observe_host_offer(
        1,
        "host-standby-lease",
        "persistent-offer",
        PersistentHostOfferAdoption::kFreshController);
    EXPECT_TRUE(offer.accepted());
    EXPECT_TRUE(offer.generation_changed);
    EXPECT_EQ(controller.connection_request_tag(), "host-standby-lease");
    EXPECT_EQ(controller.offer_description_tag(), "persistent-offer");
}

TEST(ConnectionNegotiationTest, NewControllerRequestSupersedesUnconnectedAttempt) {
    ConnectionNegotiationCoordinator host(ConnectionNegotiationRole::kHost);

    ASSERT_TRUE(host.observe_controller_request("controller-old").accepted());
    ASSERT_TRUE(host.begin_host_generation(1, "offer-old").accepted());
    ASSERT_TRUE(host.observe_controller_answer(
        1, "controller-old", "offer-old", "answer-old").accepted());
    ASSERT_TRUE(host.mark_failed());

    const auto replacement = host.observe_controller_request("controller-new");
    EXPECT_EQ(replacement.observation, ConnectionNegotiationObservation::kAccepted);
    EXPECT_TRUE(replacement.generation_changed);
    EXPECT_TRUE(replacement.state_changed);
    EXPECT_EQ(host.phase(), ConnectionNegotiationPhase::kRequestReceived);
    EXPECT_EQ(host.generation(), 0U);
    EXPECT_EQ(host.connection_request_tag(), "controller-new");
    EXPECT_TRUE(host.offer_description_tag().empty());
    EXPECT_TRUE(host.answer_description_tag().empty());
    EXPECT_TRUE(host.acknowledged_answer_description_tag().empty());

    ASSERT_TRUE(host.begin_host_generation(2, "offer-new").accepted());
    EXPECT_EQ(host.generation(), 2U);
    EXPECT_EQ(host.offer_description_tag(), "offer-new");
}

TEST(ConnectionNegotiationTest, ConnectedHostDoesNotReplaceItsControllerLease) {
    ConnectionNegotiationCoordinator host(ConnectionNegotiationRole::kHost);

    ASSERT_TRUE(host.observe_controller_request("controller-active").accepted());
    ASSERT_TRUE(host.begin_host_generation(5, "offer-active").accepted());
    ASSERT_TRUE(host.observe_controller_answer(
        5, "controller-active", "offer-active", "answer-active").accepted());
    ASSERT_TRUE(host.mark_remote_answer_applied("answer-active"));
    ASSERT_TRUE(host.mark_connected());

    const auto competing = host.observe_controller_request("controller-other");
    EXPECT_EQ(
        competing.observation,
        ConnectionNegotiationObservation::kRejectedGenerationConflict);
    EXPECT_EQ(host.phase(), ConnectionNegotiationPhase::kConnected);
    EXPECT_EQ(host.connection_request_tag(), "controller-active");
    EXPECT_EQ(host.generation(), 5U);
}

TEST(ConnectionNegotiationTest, DuplicateAndReorderedDeliveryConvergesOnOneGeneration) {
    ConnectionNegotiationCoordinator host(ConnectionNegotiationRole::kHost);
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);

    ASSERT_TRUE(host.observe_controller_request("request-12").accepted());
    ASSERT_TRUE(host.begin_host_generation(12, "offer-12").accepted());
    ASSERT_TRUE(controller.begin_controller_request("request-12").accepted());
    ASSERT_TRUE(controller.observe_host_offer(12, "request-12", "offer-12").accepted());
    EXPECT_EQ(
        controller.observe_host_offer(12, "request-12", "offer-12").observation,
        ConnectionNegotiationObservation::kDuplicate);
    ASSERT_TRUE(controller.mark_remote_offer_applied());
    ASSERT_TRUE(controller.mark_local_answer_ready("answer-12"));
    ASSERT_TRUE(controller.mark_local_answer_published());

    EXPECT_EQ(
        host.observe_controller_answer(11, "request-12", "offer-11", "answer-11").observation,
        ConnectionNegotiationObservation::kRejectedStaleGeneration);
    ASSERT_TRUE(host.observe_controller_answer(
        12, "request-12", "offer-12", "answer-12").accepted());
    EXPECT_EQ(
        host.observe_controller_answer(12, "request-12", "offer-12", "answer-12").observation,
        ConnectionNegotiationObservation::kDuplicate);
    ASSERT_TRUE(host.mark_remote_answer_applied("answer-12"));

    EXPECT_EQ(
        controller.observe_answer_applied(
            12, "request-12", "wrong-offer", "answer-12").observation,
        ConnectionNegotiationObservation::kRejectedOfferMismatch);
    ASSERT_TRUE(controller.observe_answer_applied(
        12, "request-12", "offer-12", "answer-12").accepted());
    EXPECT_TRUE(controller.answer_acknowledged());
    EXPECT_EQ(host.generation(), controller.generation());
}

TEST(ConnectionNegotiationTest, ControllerFollowsOnlyANewerHostGeneration) {
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);
    ASSERT_TRUE(controller.begin_controller_request("request-20").accepted());
    ASSERT_TRUE(controller.observe_host_offer(20, "request-20", "offer-20").accepted());
    ASSERT_TRUE(controller.mark_remote_offer_applied());
    ASSERT_TRUE(controller.mark_local_answer_ready("answer-20"));
    ASSERT_TRUE(controller.mark_local_answer_published());

    EXPECT_EQ(
        controller.observe_host_offer(19, "request-20", "offer-19").observation,
        ConnectionNegotiationObservation::kRejectedStaleGeneration);
    const auto next = controller.observe_host_offer(21, "request-20", "offer-21");
    EXPECT_TRUE(next.accepted());
    EXPECT_TRUE(next.generation_changed);
    EXPECT_EQ(controller.phase(), ConnectionNegotiationPhase::kOfferReady);
    EXPECT_FALSE(controller.answer_acknowledged());
}

TEST(ConnectionNegotiationTest, ExplicitReconnectClearsConnectedLease) {
    ConnectionNegotiationCoordinator host(ConnectionNegotiationRole::kHost);
    ASSERT_TRUE(host.observe_controller_request("request-old").accepted());
    ASSERT_TRUE(host.begin_host_generation(3, "offer-old").accepted());
    ASSERT_TRUE(host.observe_controller_answer(
        3, "request-old", "offer-old", "answer-old").accepted());
    ASSERT_TRUE(host.mark_remote_answer_applied("answer-old"));
    ASSERT_TRUE(host.mark_connected());

    EXPECT_TRUE(host.reset_for_reconnect());
    EXPECT_EQ(host.phase(), ConnectionNegotiationPhase::kIdle);
    EXPECT_TRUE(host.observe_controller_request("request-new").accepted());

    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);
    ASSERT_TRUE(controller.begin_controller_request("request-old").accepted());
    EXPECT_FALSE(controller.reset_for_reconnect());
    EXPECT_TRUE(controller.reset_for_reconnect("request-new"));
    EXPECT_EQ(controller.phase(), ConnectionNegotiationPhase::kRequestReady);
    EXPECT_EQ(controller.connection_request_tag(), "request-new");
}

TEST(ConnectionNegotiationTest, AutomaticReconnectUsesBoundedExponentialBackoff) {
    EXPECT_EQ(automatic_reconnect_backoff_ms(0), 1000U);
    EXPECT_EQ(automatic_reconnect_backoff_ms(1), 2000U);
    EXPECT_EQ(automatic_reconnect_backoff_ms(2), 4000U);
    EXPECT_EQ(automatic_reconnect_backoff_ms(3), 8000U);
    EXPECT_EQ(automatic_reconnect_backoff_ms(4), 16000U);
    EXPECT_EQ(automatic_reconnect_backoff_ms(5), 16000U);
    EXPECT_EQ(
        automatic_reconnect_backoff_ms(std::numeric_limits<std::uint32_t>::max()),
        16000U);
}

TEST(ConnectionNegotiationTest, DirectConnectedToFailedTransitionIsOwnedByRecovery) {
    EstablishedSessionRecoveryInput input;
    input.connected_once = true;
    input.transport_failed = true;
    input.transport_failure_observed_at_ms = 10'000;
    input.now_ms = 10'000;
    input.grace_ms = 3'000;

    EXPECT_EQ(
        decide_established_session_recovery(input),
        EstablishedSessionRecoveryAction::kWaitGrace);
    input.now_ms = 12'999;
    EXPECT_EQ(
        decide_established_session_recovery(input),
        EstablishedSessionRecoveryAction::kWaitGrace);
    input.now_ms = 13'000;
    EXPECT_EQ(
        decide_established_session_recovery(input),
        EstablishedSessionRecoveryAction::kRecover);
}

TEST(ConnectionNegotiationTest, FailedControllerBackoffDoesNotWaitForCompleteSignaling) {
    DhtIceFailureRecoveryInput failure;
    failure.role = ConnectionNegotiationRole::kController;
    failure.transport_failed = true;
    failure.remote_description_applied = true;

    const auto priority_candidates_only = decide_dht_ice_failure_recovery(failure);
    EXPECT_EQ(
        priority_candidates_only.action,
        DhtIceFailureRecoveryAction::kWaitBackoff);
    EXPECT_FALSE(priority_candidates_only.may_apply_remote_candidates);

    // No publication/ACK or complete remote candidates are needed to abandon
    // a terminal transport. The first backoff expires after one second.
    failure.failure_elapsed_ms = 999;
    EXPECT_EQ(decide_dht_ice_failure_recovery(failure).action,
        DhtIceFailureRecoveryAction::kWaitBackoff);
    failure.failure_elapsed_ms = 1000;
    const auto complete_candidates = decide_dht_ice_failure_recovery(failure);
    EXPECT_EQ(
        complete_candidates.action,
        DhtIceFailureRecoveryAction::kRebuildControllerRequest);
    EXPECT_FALSE(complete_candidates.may_apply_remote_candidates);
}

TEST(ConnectionNegotiationTest, FreshControllerRequestConvergesAfterLateCandidateFailure) {
    ConnectionNegotiationCoordinator host(ConnectionNegotiationRole::kHost);
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);

    ASSERT_TRUE(controller.begin_controller_request("request-old").accepted());
    ASSERT_TRUE(controller.mark_controller_request_published());
    ASSERT_TRUE(host.observe_controller_request("request-old").accepted());
    ASSERT_TRUE(host.begin_host_generation(1, "offer-old").accepted());
    ASSERT_TRUE(controller.observe_host_offer(
        1, "request-old", "offer-old").accepted());
    ASSERT_TRUE(controller.mark_remote_offer_applied());
    ASSERT_TRUE(controller.mark_local_answer_ready("answer-old"));
    ASSERT_TRUE(controller.mark_local_answer_published());
    ASSERT_TRUE(controller.mark_failed());

    DhtIceFailureRecoveryInput failure;
    failure.role = ConnectionNegotiationRole::kController;
    failure.transport_failed = true;
    failure.remote_description_applied = true;
    failure.failure_elapsed_ms = 1000;
    ASSERT_EQ(
        decide_dht_ice_failure_recovery(failure).action,
        DhtIceFailureRecoveryAction::kRebuildControllerRequest);

    const std::uint64_t failed_generation = controller.generation();
    ASSERT_TRUE(controller.reset_for_reconnect("request-new"));
    EXPECT_TRUE(should_skip_failed_dht_host_offer(
        1, failed_generation, "offer-old", "offer-old"));
    EXPECT_FALSE(should_skip_failed_dht_host_offer(
        2, failed_generation, "offer-new", "offer-old"));
    // A restarted Host owns a fresh SDP/ICE identity even if its process-local
    // generation counter starts over at one.
    EXPECT_FALSE(should_skip_failed_dht_host_offer(
        1, failed_generation, "offer-restarted", "offer-old"));
    EXPECT_TRUE(should_defer_dht_controller_request_for_persistent_offer(
        true, false, false, false, true));
    EXPECT_FALSE(should_defer_dht_controller_request_for_persistent_offer(
        true, false, true, true, true));
    EXPECT_FALSE(should_defer_dht_controller_request_for_persistent_offer(
        true, true, false, false, true));

    // The old offer may remain visible while the fresh request propagates. It
    // must not be fed back into the coordinator, otherwise the Controller
    // creates a second answer for the already-dead generation.
    EXPECT_EQ(controller.generation(), 0U);
    EXPECT_EQ(controller.connection_request_tag(), "request-new");
    const auto replacement = host.observe_controller_request("request-new");
    ASSERT_TRUE(replacement.accepted());
    EXPECT_TRUE(replacement.generation_changed);
    ASSERT_TRUE(host.begin_host_generation(2, "offer-new").accepted());
    const auto new_offer = controller.observe_host_offer(
        2, "request-new", "offer-new");
    EXPECT_TRUE(new_offer.accepted());
    EXPECT_TRUE(new_offer.generation_changed);
    EXPECT_EQ(controller.generation(), 2U);
}

TEST(ConnectionNegotiationTest, FreshControllerSkipsAlreadyAnsweredPersistentOffer) {
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);

    ASSERT_TRUE(controller.begin_controller_request("request-fresh").accepted());
    ASSERT_TRUE(controller.mark_controller_request_published());
    ASSERT_EQ(controller.generation(), 0U);

    EXPECT_TRUE(should_skip_completed_dht_host_offer_on_fresh_controller(
        controller.generation(), "answer-from-prior-controller"));
    EXPECT_FALSE(should_skip_completed_dht_host_offer_on_fresh_controller(
        controller.generation(), ""));
    EXPECT_FALSE(should_skip_completed_dht_host_offer_on_fresh_controller(
        controller.generation(), "none"));

    const auto current = controller.observe_host_offer(
        12, "request-fresh", "offer-current");
    ASSERT_TRUE(current.accepted());
    EXPECT_FALSE(should_skip_completed_dht_host_offer_on_fresh_controller(
        controller.generation(), "answer-from-current-controller"));
}

TEST(ConnectionNegotiationTest, FreshRequestReplacesStandbyAfterEstablishedSessionDisconnects) {
    ConnectionNegotiationCoordinator host(ConnectionNegotiationRole::kHost);

    ASSERT_TRUE(host.observe_controller_request("controller-old").accepted());
    ASSERT_TRUE(host.begin_host_generation(6, "offer-old").accepted());
    ASSERT_TRUE(host.observe_controller_answer(
        6, "controller-old", "offer-old", "answer-old").accepted());
    ASSERT_TRUE(host.mark_remote_answer_applied("answer-old"));
    ASSERT_TRUE(host.mark_connected());

    // A healthy connected session keeps its lease and rejects a competing
    // Controller. This prevents a DHT record from stealing a live session.
    EXPECT_EQ(
        host.observe_controller_request("controller-new").observation,
        ConnectionNegotiationObservation::kRejectedGenerationConflict);

    // Channel/ICE loss returns the persistent Host to a fresh standby offer.
    // The process-wide history remains true even though the current standby
    // has not applied a description yet; that is what distinguishes it from
    // the initial unattended wait.
    ASSERT_TRUE(host.reset_for_reconnect());
    ASSERT_TRUE(host.observe_controller_request("host-standby-after-disconnect").accepted());
    ASSERT_TRUE(host.begin_host_generation(7, "standby-offer").accepted());
    EXPECT_FALSE(should_defer_dht_controller_request_for_persistent_offer(
        true, false, false, true, true));

    const auto replacement = host.observe_controller_request("controller-new");
    ASSERT_TRUE(replacement.accepted());
    EXPECT_TRUE(replacement.generation_changed);
    EXPECT_TRUE(replacement.state_changed);
    EXPECT_EQ(host.phase(), ConnectionNegotiationPhase::kRequestReceived);
    EXPECT_EQ(host.connection_request_tag(), "controller-new");
    EXPECT_EQ(host.generation(), 0U);

    ASSERT_TRUE(host.begin_host_generation(8, "offer-new").accepted());
    EXPECT_EQ(host.generation(), 8U);
    EXPECT_EQ(host.offer_description_tag(), "offer-new");
}

TEST(ConnectionNegotiationTest, HostRebuildPreservesAcceptedControllerRequestTag) {
    ConnectionNegotiationCoordinator host(ConnectionNegotiationRole::kHost);
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);

    ASSERT_TRUE(host.observe_controller_request("request-old").accepted());
    ASSERT_TRUE(host.begin_host_generation(1, "offer-old").accepted());
    ASSERT_TRUE(controller.begin_controller_request("request-new").accepted());
    ASSERT_TRUE(controller.mark_controller_request_published());

    const auto replacement = host.observe_controller_request("request-new");
    ASSERT_TRUE(replacement.accepted());
    ASSERT_TRUE(replacement.generation_changed);
    const auto preserved_tag = select_dht_host_rebuild_request_tag(
        true,
        host.connection_request_tag(),
        "host-standby-after-failure");
    EXPECT_EQ(preserved_tag, "request-new");

    ASSERT_TRUE(host.reset_for_reconnect());
    ASSERT_TRUE(host.observe_controller_request(preserved_tag).accepted());
    ASSERT_TRUE(host.begin_host_generation(2, "offer-new").accepted());
    const auto new_offer = controller.observe_host_offer(
        2,
        host.connection_request_tag(),
        host.offer_description_tag());
    EXPECT_TRUE(new_offer.accepted());
    EXPECT_EQ(controller.connection_request_tag(), "request-new");
    EXPECT_EQ(controller.generation(), 2U);
}

TEST(ConnectionNegotiationTest, HostRebuildUsesStandbyTagWithoutAcceptedRequest) {
    EXPECT_EQ(
        select_dht_host_rebuild_request_tag(false, "request-ignored", "host-standby"),
        "host-standby");
    EXPECT_EQ(
        select_dht_host_rebuild_request_tag(true, "", "host-standby"),
        "host-standby");
}

TEST(ConnectionNegotiationTest, ConnectedHostRecoveryWaitsForControllerRequest) {
    EXPECT_TRUE(should_defer_dht_host_rebuild_until_controller_request(true, ""));
    EXPECT_FALSE(should_defer_dht_host_rebuild_until_controller_request(
        true, "controller-request"));
    EXPECT_FALSE(should_defer_dht_host_rebuild_until_controller_request(false, ""));
}

TEST(ConnectionNegotiationTest, AutomaticRecoveryOwnsTimeoutAndTwoFourSecondBackoff) {
    AutomaticReconnectSchedule schedule;
    schedule.active = true;
    EXPECT_EQ(schedule.tick(1000, 90000, true), AutomaticReconnectAction::kScheduled);
    EXPECT_EQ(schedule.tick(2000, 90000, true), AutomaticReconnectAction::kRebuild);
    EXPECT_EQ(schedule.tick(92000, 90000, false), AutomaticReconnectAction::kAttemptFailed);
    DhtIceFailureRecoveryInput failure;
    failure.role = ConnectionNegotiationRole::kController;
    failure.transport_failed = true;
    failure.automatic_recovery_owned = schedule.active;
    failure.remote_description_applied = true;
    EXPECT_EQ(decide_dht_ice_failure_recovery(failure).action, DhtIceFailureRecoveryAction::kNone);
    EXPECT_EQ(schedule.tick(93999, 90000, true), AutomaticReconnectAction::kNone);
    EXPECT_EQ(schedule.tick(94000, 90000, true), AutomaticReconnectAction::kRebuild);
    EXPECT_EQ(schedule.attempt_count, 2U);
    EXPECT_EQ(schedule.tick(184000, 90000, false), AutomaticReconnectAction::kAttemptFailed);
    EXPECT_EQ(schedule.tick(187999, 90000, true), AutomaticReconnectAction::kNone);
    EXPECT_EQ(schedule.tick(188000, 90000, true), AutomaticReconnectAction::kRebuild);
    EXPECT_EQ(schedule.tick(188000, 90000, false), AutomaticReconnectAction::kNone);
    EXPECT_EQ(schedule.attempt_count, 3U);
}

TEST(ConnectionNegotiationTest, OnlyNewCurrentPhaseRenewsBoundedDeadline) {
    AutomaticReconnectSchedule schedule;
    schedule.active = true;
    (void)schedule.tick(1000, 90000, false);
    (void)schedule.tick(2000, 90000, false);
    ASSERT_TRUE(schedule.observe_progress(ConnectionNegotiationPhase::kOfferApplied, 7, 91000, 90000));
    EXPECT_EQ(schedule.deadline_ms, 181000U);
    EXPECT_FALSE(schedule.observe_progress(ConnectionNegotiationPhase::kOfferApplied, 7, 180000, 90000));
    EXPECT_FALSE(schedule.observe_progress(ConnectionNegotiationPhase::kAnswerApplied, 6, 180000, 90000));
    ASSERT_TRUE(schedule.observe_progress(ConnectionNegotiationPhase::kAnswerApplied, 7, 180000, 90000));
    ASSERT_TRUE(schedule.observe_progress(ConnectionNegotiationPhase::kIceConnecting, 7, 269999, 90000));
    EXPECT_LE(schedule.deadline_ms, schedule.total_deadline_ms);
    EXPECT_EQ(schedule.tick(270000, 90000, true), AutomaticReconnectAction::kAttemptFailed);
    EXPECT_FALSE(schedule.observe_progress(ConnectionNegotiationPhase::kIceConnecting, 7, 270001, 90000));
}

TEST(ConnectionNegotiationTest, EstablishedControllerRecoveryWaitsForMatchingNewOffer) {
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);

    ASSERT_TRUE(controller.begin_controller_request("controller-old").accepted());
    ASSERT_TRUE(controller.observe_host_offer(
        6, "controller-old", "offer-old").accepted());
    ASSERT_TRUE(controller.mark_remote_offer_applied());
    ASSERT_TRUE(controller.mark_local_answer_ready("answer-old"));
    ASSERT_TRUE(controller.mark_local_answer_published());
    ASSERT_TRUE(controller.observe_answer_applied(
        6, "controller-old", "offer-old", "answer-old").accepted());
    ASSERT_TRUE(controller.mark_connected());

    const std::uint64_t disconnected_generation = controller.generation();
    ASSERT_TRUE(controller.reset_for_reconnect("controller-new"));
    EXPECT_TRUE(should_skip_failed_dht_host_offer(
        6, disconnected_generation, "offer-old", "offer-old"));
    EXPECT_FALSE(should_skip_failed_dht_host_offer(
        7, disconnected_generation, "offer-new", "offer-old"));

    // The Host may publish a standby offer before it observes the new request.
    // Adopting that mismatched lease races the Host's next generation and
    // leaves the peers answering different offers.
    const auto standby = controller.observe_host_offer(
        7,
        "host-standby-after-disconnect",
        "offer-standby",
        PersistentHostOfferAdoption::kRejected);
    EXPECT_EQ(standby.observation, ConnectionNegotiationObservation::kRejectedOfferMismatch);
    EXPECT_EQ(controller.generation(), 0U);
    EXPECT_EQ(controller.connection_request_tag(), "controller-new");

    const auto replacement = controller.observe_host_offer(
        8,
        "controller-new",
        "offer-new",
        PersistentHostOfferAdoption::kRejected);
    ASSERT_TRUE(replacement.accepted());
    EXPECT_TRUE(replacement.generation_changed);
    EXPECT_EQ(controller.connection_request_tag(), "controller-new");
    EXPECT_EQ(controller.generation(), 8U);
}

TEST(ConnectionNegotiationTest, UnauthenticatedControllerCanLeaveRejectedStaleHostOffer) {
    PersistentHostOfferAdoptionInput input;
    input.remote_description_ever_applied = true;
    input.last_applied_host_instance_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    input.offered_host_instance_id = input.last_applied_host_instance_id;
    input.authenticated_once = false;
    input.current_generation = 1; input.offered_generation = 2;
    const auto adoption = decide_persistent_host_offer_adoption(input);
    EXPECT_EQ(adoption, PersistentHostOfferAdoption::kNewerUnauthenticatedHost);
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);
    ASSERT_TRUE(controller.begin_controller_request("first").accepted());
    ASSERT_TRUE(controller.observe_host_offer(1, "first", "old-offer").accepted());
    ASSERT_TRUE(controller.mark_remote_offer_applied());
    ASSERT_TRUE(controller.mark_local_answer_ready("old-answer"));
    const auto replacement = controller.observe_host_offer(2, "standby-new", "new-offer", adoption);
    EXPECT_TRUE(replacement.accepted()); EXPECT_TRUE(replacement.generation_changed);
    EXPECT_EQ(controller.connection_request_tag(), "standby-new");
    EXPECT_FALSE(controller.observe_host_offer(1, "first", "old-offer", adoption).accepted());
    input.authenticated_once = true;
    EXPECT_EQ(decide_persistent_host_offer_adoption(input), PersistentHostOfferAdoption::kRejected);
    input.authenticated_once = false; input.offered_generation = 1;
    EXPECT_EQ(decide_persistent_host_offer_adoption(input), PersistentHostOfferAdoption::kRejected);
    input.offered_generation = 2; input.exact_failed_offer = true;
    EXPECT_EQ(decide_persistent_host_offer_adoption(input), PersistentHostOfferAdoption::kRejected);
}

TEST(ConnectionNegotiationTest, PersistentOfferAdoptionRequiresFreshOrRestartedHost) {
    EXPECT_EQ(
        decide_persistent_host_offer_adoption({
            .remote_description_ever_applied = false,
            .last_applied_host_instance_id = {},
            .offered_host_instance_id = {},
        }),
        PersistentHostOfferAdoption::kFreshController);
    EXPECT_EQ(
        decide_persistent_host_offer_adoption({
            .remote_description_ever_applied = true,
            .last_applied_host_instance_id = kHostInstanceA,
            .offered_host_instance_id = kHostInstanceA,
        }),
        PersistentHostOfferAdoption::kRejected);
    EXPECT_EQ(
        decide_persistent_host_offer_adoption({
            .remote_description_ever_applied = true,
            .last_applied_host_instance_id = kHostInstanceA,
            .offered_host_instance_id = kHostInstanceB,
        }),
        PersistentHostOfferAdoption::kRestartedHost);
    EXPECT_EQ(
        decide_persistent_host_offer_adoption({
            .remote_description_ever_applied = true,
            .last_applied_host_instance_id = kHostInstanceA,
            .offered_host_instance_id = kHostInstanceB,
            .exact_failed_offer = true,
        }),
        PersistentHostOfferAdoption::kRejected);
    EXPECT_EQ(
        decide_persistent_host_offer_adoption({
            .remote_description_ever_applied = true,
            .last_applied_host_instance_id = kHostInstanceA,
            .offered_host_instance_id = kHostInstanceB,
            .completed_offer = true,
        }),
        PersistentHostOfferAdoption::kRejected);
    EXPECT_EQ(
        decide_persistent_host_offer_adoption({
            .remote_description_ever_applied = true,
            .last_applied_host_instance_id = {},
            .offered_host_instance_id = kHostInstanceB,
        }),
        PersistentHostOfferAdoption::kRejected);
}

TEST(ConnectionNegotiationTest, LongRunningControllerAdoptsFreshlyRestartedHostStandby) {
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);

    ASSERT_TRUE(controller.begin_controller_request("controller-old").accepted());
    ASSERT_TRUE(controller.mark_controller_request_published());
    ASSERT_TRUE(controller.observe_host_offer(
        6, "controller-old", "offer-host-a").accepted());
    ASSERT_TRUE(controller.mark_remote_offer_applied());
    ASSERT_TRUE(controller.mark_local_answer_ready("answer-old"));
    ASSERT_TRUE(controller.mark_local_answer_published());
    ASSERT_TRUE(controller.observe_answer_applied(
        6, "controller-old", "offer-host-a", "answer-old").accepted());
    ASSERT_TRUE(controller.mark_connected());

    ASSERT_TRUE(controller.reset_for_reconnect("controller-new"));
    ASSERT_TRUE(controller.mark_controller_request_published());
    const auto adoption = decide_persistent_host_offer_adoption({
        .remote_description_ever_applied = true,
        .last_applied_host_instance_id = kHostInstanceA,
        .offered_host_instance_id = kHostInstanceB,
    });
    ASSERT_EQ(adoption, PersistentHostOfferAdoption::kRestartedHost);

    const auto restarted_offer = controller.observe_host_offer(
        1,
        "host-b-standby",
        "offer-host-b",
        adoption);
    ASSERT_TRUE(restarted_offer.accepted());
    EXPECT_TRUE(restarted_offer.generation_changed);
    EXPECT_EQ(controller.generation(), 1U);
    EXPECT_EQ(controller.connection_request_tag(), "host-b-standby");
    ASSERT_TRUE(controller.mark_remote_offer_applied());
    ASSERT_TRUE(controller.mark_local_answer_ready("answer-host-b"));
    ASSERT_TRUE(controller.mark_local_answer_published());
    ASSERT_TRUE(controller.observe_answer_applied(
        1, "host-b-standby", "offer-host-b", "answer-host-b").accepted());
    EXPECT_TRUE(controller.mark_connected());

    const auto duplicate = controller.observe_host_offer(
        1,
        "host-b-standby",
        "offer-host-b",
        adoption);
    EXPECT_EQ(duplicate.observation, ConnectionNegotiationObservation::kDuplicate);
    EXPECT_FALSE(duplicate.generation_changed);
}

TEST(ConnectionNegotiationTest, LiveTransportStillAcceptsCumulativeCandidates) {
    DhtIceFailureRecoveryInput healthy;
    healthy.role = ConnectionNegotiationRole::kController;
    healthy.remote_description_applied = true;

    const auto decision = decide_dht_ice_failure_recovery(healthy);
    EXPECT_EQ(decision.action, DhtIceFailureRecoveryAction::kNone);
    EXPECT_TRUE(decision.may_apply_remote_candidates);

    healthy.transport_failed = true;
    healthy.repair_attempts = 20;
    const auto waiting = decide_dht_ice_failure_recovery(healthy);
    EXPECT_EQ(waiting.action, DhtIceFailureRecoveryAction::kWaitBackoff);
    EXPECT_FALSE(waiting.may_apply_remote_candidates);
}

TEST(ConnectionNegotiationTest, CandidateAdmissionNeedsAppliedDescriptionNotStoreConfirmation) {
    DhtIceFailureRecoveryInput input;
    input.role = ConnectionNegotiationRole::kController;
    EXPECT_FALSE(decide_dht_ice_failure_recovery(input).may_apply_remote_candidates);
    input.remote_description_applied = true;
    // Publication confirmation, local gathering and Host ACK are deliberately
    // absent from the admission contract; cumulative trickle may begin now.
    EXPECT_TRUE(decide_dht_ice_failure_recovery(input).may_apply_remote_candidates);
    input.transport_failed = true;
    EXPECT_FALSE(decide_dht_ice_failure_recovery(input).may_apply_remote_candidates);
}

TEST(ConnectionNegotiationTest, InitialFailureBackoffIsBoundedAndDoesNotStealHostRecovery) {
    DhtIceFailureRecoveryInput input;
    input.role = ConnectionNegotiationRole::kController;
    input.transport_failed = true;
    for (std::uint32_t attempt = 0; attempt < 1000; ++attempt) {
        input.repair_attempts = attempt;
        const auto delay = automatic_reconnect_backoff_ms(attempt);
        EXPECT_LE(delay, 16000U);
        input.failure_elapsed_ms = delay - 1;
        EXPECT_EQ(decide_dht_ice_failure_recovery(input).action,
            DhtIceFailureRecoveryAction::kWaitBackoff);
        input.failure_elapsed_ms = delay;
        EXPECT_EQ(decide_dht_ice_failure_recovery(input).action,
            DhtIceFailureRecoveryAction::kRebuildControllerRequest);
    }
    input.repair_attempts = std::numeric_limits<std::uint32_t>::max();
    EXPECT_EQ(decide_dht_ice_failure_recovery(input).action,
        DhtIceFailureRecoveryAction::kRebuildControllerRequest);
    input.role = ConnectionNegotiationRole::kHost;
    EXPECT_EQ(decide_dht_ice_failure_recovery(input).action,
        DhtIceFailureRecoveryAction::kWaitForHostRepair);
    input.connected_once = true;
    EXPECT_EQ(decide_dht_ice_failure_recovery(input).action,
        DhtIceFailureRecoveryAction::kNone);
    EXPECT_FALSE(decide_dht_ice_failure_recovery(input).may_apply_remote_candidates);
}

TEST(ConnectionNegotiationTest, LateAckAndPublicationCannotReviveFailedAttempt) {
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);
    ASSERT_TRUE(controller.begin_controller_request("request").accepted());
    ASSERT_TRUE(controller.observe_host_offer(1, "request", "offer").accepted());
    ASSERT_TRUE(controller.mark_local_answer_ready("answer"));
    ASSERT_TRUE(controller.mark_failed());
    EXPECT_EQ(controller.observe_answer_applied(1, "request", "offer", "answer").observation,
        ConnectionNegotiationObservation::kRejectedTerminalState);
    EXPECT_FALSE(controller.mark_local_answer_published());
    EXPECT_FALSE(controller.mark_local_answer_ready("answer"));
    EXPECT_FALSE(controller.mark_ice_connecting());
    EXPECT_FALSE(controller.mark_connected());
    EXPECT_EQ(controller.phase(), ConnectionNegotiationPhase::kFailed);
    EXPECT_FALSE(controller.answer_acknowledged());

    ASSERT_TRUE(controller.reset_for_reconnect("replacement"));
    EXPECT_FALSE(controller.observe_answer_applied(1, "request", "offer", "answer").accepted());
    EXPECT_FALSE(controller.observe_host_offer(1, "request", "offer").accepted());
    EXPECT_EQ(controller.generation(), 0U);
}

TEST(ConnectionNegotiationTest, InitialAutomaticRetriesReachLateHostWithoutManualRestart) {
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);
    ASSERT_TRUE(controller.begin_controller_request("request-0").accepted());
    DhtIceFailureRecoveryInput failure;
    failure.role = ConnectionNegotiationRole::kController;
    failure.transport_failed = true;
    // Exercise the production decision and coordinator past the old 20-repair
    // cap. A fresh request is generated once per failed transport, after delay.
    for (std::uint32_t attempt = 0; attempt < 30; ++attempt) {
        const auto request = "request-" + std::to_string(attempt);
        const auto offer = "offer-" + std::to_string(attempt);
        ASSERT_TRUE(controller.mark_controller_request_published());
        ASSERT_TRUE(controller.observe_host_offer(attempt + 1, request, offer).accepted());
        ASSERT_TRUE(controller.mark_remote_offer_applied());
        ASSERT_TRUE(controller.mark_failed());
        failure.repair_attempts = attempt;
        const auto delay = automatic_reconnect_backoff_ms(attempt);
        for (std::uint64_t elapsed = 0; elapsed < delay; elapsed += 250) {
            failure.failure_elapsed_ms = elapsed;
            ASSERT_EQ(decide_dht_ice_failure_recovery(failure).action,
                DhtIceFailureRecoveryAction::kWaitBackoff);
        }
        failure.failure_elapsed_ms = delay;
        ASSERT_EQ(decide_dht_ice_failure_recovery(failure).action,
            DhtIceFailureRecoveryAction::kRebuildControllerRequest);
        ASSERT_TRUE(controller.reset_for_reconnect("request-" + std::to_string(attempt + 1)));
        EXPECT_FALSE(controller.observe_host_offer(attempt + 1, request, offer).accepted());
    }
    ASSERT_TRUE(controller.mark_controller_request_published());
    ASSERT_TRUE(controller.observe_host_offer(31, "request-30", "working-offer").accepted());
    ASSERT_TRUE(controller.mark_remote_offer_applied());
    ASSERT_TRUE(controller.mark_local_answer_ready("working-answer"));
    ASSERT_TRUE(controller.mark_local_answer_published());
    ASSERT_TRUE(controller.observe_answer_applied(31,
        "request-30", "working-offer", "working-answer").accepted());
    EXPECT_TRUE(controller.mark_connected());
    failure.connected_once = true;
    EXPECT_EQ(decide_dht_ice_failure_recovery(failure).action, DhtIceFailureRecoveryAction::kNone);
}

TEST(ConnectionNegotiationTest, HostAckAfterConnectedDoesNotRegressState) {
    ConnectionNegotiationCoordinator host(ConnectionNegotiationRole::kHost);
    ASSERT_TRUE(host.observe_controller_request("request").accepted());
    ASSERT_TRUE(host.begin_host_generation(1, "offer").accepted());
    ASSERT_TRUE(host.observe_controller_answer(1, "request", "offer", "answer").accepted());
    ASSERT_TRUE(host.mark_connected());
    ASSERT_TRUE(host.mark_remote_answer_applied("answer"));
    EXPECT_EQ(host.phase(), ConnectionNegotiationPhase::kConnected);
    EXPECT_EQ(host.outbound_answer_acknowledgement(), "answer");
    ASSERT_TRUE(host.mark_failed());
    EXPECT_FALSE(host.mark_remote_answer_applied("answer"));
    EXPECT_FALSE(host.observe_controller_answer(1, "request", "offer", "answer").accepted());
    EXPECT_EQ(host.phase(), ConnectionNegotiationPhase::kFailed);
}

TEST(ConnectionNegotiationTest, DelayedDhtConfirmationDoesNotGateCandidatesOrEchoAck) {
    auto boundary_answer = make_answer_fixture();
    std::uint32_t random = 0x491a32f7U;
    bool found_boundary = false;
    std::size_t direct_bytes = 0;
    std::size_t echoed_bytes = 0;
    // Compression invalidates the old assumption that N repeated characters
    // add exactly N wire bytes. Find a real production-code boundary using
    // deterministic incompressible padding; keep the original direct -> chunked
    // ACK-mutation regression, not obsolete 697/713 textual-format constants.
    for (int i = 0; i < 4000; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        boundary_answer.description_sdp.push_back(static_cast<char>('!' + random % 90));
        const auto size = estimate_dht_encrypted_blob_bytes(boundary_answer);
        ASSERT_TRUE(size.has_value());
        auto echoed = boundary_answer;
        echoed.acknowledged_answer_tag = derive_dht_description_tag(echoed.description_sdp);
        const auto echo_size = estimate_dht_encrypted_blob_bytes(echoed);
        ASSERT_TRUE(echo_size.has_value());
        if (*size <= kDirectDhtEncryptedBlobBudgetBytes && *echo_size > kDirectDhtEncryptedBlobBudgetBytes) {
            direct_bytes = *size; echoed_bytes = *echo_size; found_boundary = true; break;
        }
    }
    ASSERT_TRUE(found_boundary);
    for (const auto confirmation_ms : std::array<std::uint64_t, 4>{0, 30000, 45000, 90000}) {
        SCOPED_TRACE(confirmation_ms);
        DelayedConfirmationStore store;
        store.confirm_at_ms = confirmation_ms;
        DhtRendezvousClient publisher(store);
        DhtRendezvousClient peer(store);
        DhtRendezvousConfig config;
        config.session_code = "TEST0001";
        config.pairing_secret = "fixture-only";
        config.now_unix = 1000;
        auto answer = boundary_answer;
        ASSERT_EQ(estimate_dht_encrypted_blob_bytes(answer), direct_bytes);

        ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);
        ASSERT_TRUE(controller.begin_controller_request(answer.connection_request_tag).accepted());
        const auto adopted = controller.observe_host_offer(answer.generation,
            answer.connection_request_tag, answer.answered_description_tag);
        ASSERT_TRUE(adopted.generation_changed);
        ASSERT_TRUE(controller.mark_remote_offer_applied());
        const auto answer_tag = derive_dht_description_tag(answer.description_sdp);
        ASSERT_TRUE(controller.mark_local_answer_ready(answer_tag));
        const auto pending_key = make_dht_signal_publication_key(answer);
        std::string error;
        const bool confirmed = publisher.publish_signal_snapshot(config, answer, &error);
        EXPECT_EQ(confirmed, confirmation_ms == 0);
        if (confirmed) ASSERT_TRUE(controller.mark_local_answer_published());
        EXPECT_EQ(controller.local_answer_published(), confirmed);
        DhtIceFailureRecoveryInput admission;
        admission.remote_description_applied = true;
        EXPECT_TRUE(decide_dht_ice_failure_recovery(admission).may_apply_remote_candidates);

        // Host reads the real encrypted record and applies exactly this Answer,
        // even though our store has not yet confirmed the Controller's put.
        store.now_ms = 100;
        auto fetched = peer.fetch_signal_snapshot(config, "controller", 0, &error);
        ASSERT_TRUE(fetched.has_value()) << error;
        EXPECT_EQ(fetched->description_sdp, answer.description_sdp);
        ConnectionNegotiationCoordinator host(ConnectionNegotiationRole::kHost);
        ASSERT_TRUE(host.observe_controller_request(answer.connection_request_tag).accepted());
        ASSERT_TRUE(host.begin_host_generation(answer.generation, answer.answered_description_tag).accepted());
        ASSERT_TRUE(host.observe_controller_answer(fetched->generation, fetched->connection_request_tag,
            fetched->answered_description_tag, answer_tag).accepted());
        ASSERT_TRUE(host.mark_remote_answer_applied(answer_tag));
        ASSERT_TRUE(controller.mark_ice_connecting());
        const auto ack = controller.observe_answer_applied(answer.generation,
            answer.connection_request_tag, answer.answered_description_tag,
            host.outbound_answer_acknowledgement());
        ASSERT_TRUE(ack.accepted());
        EXPECT_EQ(controller.phase(), ConnectionNegotiationPhase::kIceConnecting);
        EXPECT_EQ(controller.local_answer_published(), confirmed);
        answer.acknowledged_answer_tag = controller.outbound_answer_acknowledgement();
        EXPECT_TRUE(answer.acknowledged_answer_tag.empty());
        EXPECT_EQ(make_dht_signal_publication_key(answer), pending_key);
        EXPECT_EQ(estimate_dht_encrypted_blob_bytes(answer), direct_bytes);
        auto old_echo = answer;
        old_echo.acknowledged_answer_tag = host.outbound_answer_acknowledgement();
        EXPECT_EQ(estimate_dht_encrypted_blob_bytes(old_echo), echoed_bytes);
        EXPECT_GT(*estimate_dht_encrypted_blob_bytes(old_echo), kDirectDhtEncryptedBlobBudgetBytes);

        store.now_ms = 200;
        ASSERT_TRUE(controller.mark_connected());
        // Repeated cumulative records do not create another Answer or regress
        // connection state. A late local confirmation only marks delivery.
        EXPECT_FALSE(controller.observe_host_offer(answer.generation,
            answer.connection_request_tag, answer.answered_description_tag).generation_changed);
        EXPECT_TRUE(controller.mark_local_answer_ready(answer_tag));
        EXPECT_EQ(controller.phase(), ConnectionNegotiationPhase::kConnected);
        EXPECT_FALSE(controller.mark_local_answer_ready("different-answer"));
        store.now_ms = confirmation_ms + 200;
        ASSERT_TRUE(publisher.publish_signal_snapshot(config, answer, &error)) << error;
        ASSERT_TRUE(controller.mark_local_answer_published());
        EXPECT_TRUE(controller.local_answer_published());
        EXPECT_TRUE(controller.answer_acknowledged());
        EXPECT_EQ(controller.phase(), ConnectionNegotiationPhase::kConnected);
        EXPECT_TRUE(store.reused_controller_blob);
        EXPECT_EQ(answer.revision, 100U);
        EXPECT_EQ(controller.observe_answer_applied(answer.generation, answer.connection_request_tag,
            answer.answered_description_tag, answer_tag).observation,
            ConnectionNegotiationObservation::kDuplicate);
    }
}

TEST(ConnectionNegotiationTest, ConnectionBeforeAckAndPublicationRemainsConnected) {
    ConnectionNegotiationCoordinator controller(ConnectionNegotiationRole::kController);
    ASSERT_TRUE(controller.begin_controller_request("request").accepted());
    ASSERT_TRUE(controller.observe_host_offer(1, "request", "offer").accepted());
    ASSERT_TRUE(controller.mark_local_answer_ready("answer"));
    ASSERT_TRUE(controller.mark_connected());
    EXPECT_FALSE(controller.answer_acknowledged());
    EXPECT_FALSE(controller.local_answer_published());
    ASSERT_TRUE(controller.observe_answer_applied(1, "request", "offer", "answer").accepted());
    ASSERT_TRUE(controller.mark_local_answer_published());
    EXPECT_EQ(controller.phase(), ConnectionNegotiationPhase::kConnected);
}

}  // namespace
}  // namespace redclaw::service
