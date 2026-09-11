#include <gtest/gtest.h>
#include "redclaw/service/dht_publication_transaction.h"
#include "redclaw/service/connection_negotiation.h"
#include <limits>

namespace redclaw::service {
namespace {

class ControlledStore final : public IDhtRendezvousStore {
public:
    DhtEncryptedRecord last;
    DhtPublishState outcome = DhtPublishState::kPending;
    unsigned calls = 0;
    DhtPublishResult publish(const DhtEncryptedRecord& record, std::string_view, std::string*) override {
        last = record;
        ++calls;
        return outcome;
    }
    std::optional<DhtEncryptedRecord> fetch(std::string_view, std::string_view, std::uint64_t,
        std::string_view, std::string*) override { return std::nullopt; }
};

DhtSignalSnapshot offer() {
    DhtSignalSnapshot value;
    value.role = "host";
    value.description_type = "offer";
    value.description_sdp = "v=0\na=ice-ufrag:fixture\na=ice-pwd:fixture-only\n";
    value.publisher_instance_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    value.generation = 1;
    value.connection_request_tag = "fixture-request";
    return value;
}

TEST(DhtPublicationTransaction, PendingCrossesTtlThenRecoversWithoutRestart) {
    for (const bool refresh : {false, true}) {
        ControlledStore store;
        DhtRendezvousClient client(store);
        DhtPublicationTransaction publication(100);
        DhtRendezvousConfig config{"DIAG0001", "fixture-only", 1000, 300};
        auto snapshot = offer();
        snapshot.revision = *publication.prepare(snapshot, refresh, config.now_unix, 5000, 300);
        auto result = client.publish_signal_snapshot(config, snapshot);
        ASSERT_TRUE(publication.observe(snapshot.revision, result));
        const auto ciphertext = store.last.encrypted_blob;
        const auto original = snapshot.revision;
        for (unsigned elapsed = 1; elapsed < 300; ++elapsed) {
            config.now_unix = 1000 + elapsed;
            snapshot.revision = *publication.prepare(snapshot, refresh, config.now_unix, 5000 + elapsed * 1000, 300);
            EXPECT_EQ(snapshot.revision, original);
            result = client.publish_signal_snapshot(config, snapshot);
            ASSERT_TRUE(publication.observe(snapshot.revision, result));
            EXPECT_EQ(store.last.encrypted_blob, ciphertext);
        }
        config.now_unix = 1301;
        store.outcome = DhtPublishState::kSucceeded;
        snapshot.revision = *publication.prepare(snapshot, refresh, 1301, 306000, 300);
        EXPECT_GT(snapshot.revision, original);
        result = client.publish_signal_snapshot(config, snapshot);
        ASSERT_TRUE(publication.observe(snapshot.revision, result));
        EXPECT_EQ(result.state, DhtPublishState::kSucceeded);
        EXPECT_EQ(result.expires_at_unix, 1601U);
        EXPECT_NE(store.last.encrypted_blob, ciphertext);
        EXPECT_EQ(publication.expired_total(), 1U);
        EXPECT_TRUE(publication.standby_viable(1301, 306000));
    }
}

TEST(DhtPublicationTransaction, ClientReportsTypedExpiryAndDoesNotResealSameRevision) {
    ControlledStore store;
    DhtRendezvousClient client(store);
    DhtRendezvousConfig config{"DIAG0001", "fixture-only", 1000, 300};
    auto snapshot = offer();
    snapshot.revision = 1;
    EXPECT_EQ(client.publish_signal_snapshot(config, snapshot).state, DhtPublishState::kPending);
    config.now_unix = 1301;
    const auto expired = client.publish_signal_snapshot(config, snapshot);
    EXPECT_EQ(expired.failure, DhtPublishFailure::kExpired);
    EXPECT_EQ(expired.state, DhtPublishState::kPermanentFailure);
    EXPECT_EQ(expired.expires_at_unix, 1300U);
    EXPECT_EQ(store.calls, 1U);
}

TEST(DhtPublicationTransaction, NativeExpiryRetiresOnceAndLateCompletionCannotResurrectIt) {
    DhtPublicationTransaction publication(10);
    const auto snapshot = offer();
    const auto old = *publication.prepare(snapshot, false, 1000, 1, 300);
    const DhtPublishResult expired{DhtPublishState::kPermanentFailure, DhtPublishFailure::kExpired};
    EXPECT_TRUE(publication.observe(old, expired));
    EXPECT_FALSE(publication.observe(old, expired));
    const auto current = *publication.prepare(snapshot, true, 1001, 2, 300);
    EXPECT_GT(current, old);
    EXPECT_FALSE(publication.observe(old, DhtPublishState::kSucceeded));
    EXPECT_EQ(publication.revision(), current);
    EXPECT_EQ(publication.expired_total(), 1U);
    EXPECT_EQ(publication.late_result_total(), 2U);
}

TEST(DhtPublicationTransaction, SuccessRefreshAndDirtyChangeShareOneHighWaterMark) {
    DhtPublicationTransaction publication(100);
    auto snapshot = offer();
    auto revision = *publication.prepare(snapshot, false, 1000, 1, 300);
    EXPECT_EQ(revision, 101U);
    EXPECT_TRUE(publication.observe(revision, DhtPublishState::kSucceeded));
    revision = *publication.prepare(snapshot, true, 1030, 30001, 300);
    EXPECT_EQ(revision, 102U);
    EXPECT_EQ(*publication.prepare(snapshot, true, 1031, 31001, 300), revision);
    EXPECT_TRUE(publication.observe(revision, DhtPublishState::kSucceeded));
    snapshot.candidates_complete = true;
    EXPECT_EQ(*publication.prepare(snapshot, false, 1040, 40001, 300), 103U);
    publication.reset();
    EXPECT_FALSE(publication.observe(103, DhtPublishState::kSucceeded));
    EXPECT_EQ(*publication.prepare(snapshot, false, 1050, 50001, 300), 104U);
}

TEST(DhtPublicationTransaction, WallClockRollbackDoesNotExtendPendingForever) {
    DhtPublicationTransaction publication;
    const auto snapshot = offer();
    const auto old = *publication.prepare(snapshot, false, 1000, 100, 300);
    EXPECT_FALSE(publication.standby_viable(900, 300100));
    EXPECT_GT(*publication.prepare(snapshot, false, 900, 300100, 300), old);
    EXPECT_EQ(publication.expired_total(), 1U);
    EXPECT_GT(*publication.prepare(snapshot, false, 9000, 300101, 300), old + 1);
    EXPECT_EQ(publication.expired_total(), 2U);
}

TEST(DhtPublicationTransaction, ExpiredStandbyDoesNotIgnoreControllerRequest) {
    DhtPublicationTransaction publication;
    const auto snapshot = offer();
    const auto rev = *publication.prepare(snapshot, false, 1000, 1, 300);
    EXPECT_TRUE(publication.observe(rev, DhtPublishState::kSucceeded));
    EXPECT_TRUE(should_defer_dht_controller_request_for_persistent_offer(
        true, false, false, false, publication.standby_viable(1020, 20001)));
    EXPECT_FALSE(should_defer_dht_controller_request_for_persistent_offer(
        true, false, false, false, publication.standby_viable(1301, 301001)));
}

TEST(DhtPublicationTransaction, RevisionOverflowFailsClosed) {
    DhtPublicationTransaction publication(std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(publication.prepare(offer(), false, 1000, 1, 300).has_value());
}

TEST(DhtPublicationTransaction, ExpiredChunkedPublicationRestartsAsOneNewRevision) {
    auto primary = std::make_unique<ControlledStore>();
    auto* store = primary.get();
    IndirectDhtRendezvousStore indirect(std::move(primary), nullptr);
    DhtRendezvousClient client(indirect);
    DhtPublicationTransaction publication;
    DhtRendezvousConfig config{"DIAG0001", "fixture-only", 1000, 300};
    auto snapshot = offer();
    snapshot.description_sdp += std::string(4000, 'x');
    snapshot.revision = *publication.prepare(snapshot, false, 1000, 1, 300);
    const auto old = snapshot.revision;
    EXPECT_EQ(client.publish_signal_snapshot(config, snapshot).state, DhtPublishState::kPending);
    config.now_unix = 1301;
    store->outcome = DhtPublishState::kSucceeded;
    snapshot.revision = *publication.prepare(snapshot, false, 1301, 301001, 300);
    EXPECT_GT(snapshot.revision, old);
    DhtPublishResult result;
    for (int i = 0; i < 32; ++i) {
        result = client.publish_signal_snapshot(config, snapshot);
        ASSERT_TRUE(publication.observe(snapshot.revision, result));
        if (result) break;
    }
    EXPECT_TRUE(result);
    EXPECT_EQ(result.expires_at_unix, 1601U);
    EXPECT_EQ(publication.expired_total(), 1U);
}

} // namespace
} // namespace redclaw::service
