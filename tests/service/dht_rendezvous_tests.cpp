#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "redclaw/service/dht_rendezvous.h"
#include "dht_listen_port_selection.h"
#include "dht_mutable_get_accumulator.h"
#include "dht_signal_codec_internal.h"

namespace {

// Deterministic, non-repeating text keeps chunk/budget regressions meaningful
// after whole-message compression; repeated 'x' no longer exceeds the budget.
std::string incompressible_text(std::size_t size) {
    std::uint32_t state = 0x7839a175U;
    std::string result;
    for (std::size_t i = 0; i < size; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        result.push_back(static_cast<char>('!' + state % 90));
    }
    return result;
}

constexpr std::string_view kPublisherInstanceId =
    "0123456789abcdef0123456789abcdef";

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

redclaw::service::DhtSignalSnapshot make_snapshot(
    std::string role,
    std::uint64_t revision = 1) {
    redclaw::service::DhtSignalSnapshot snapshot;
    snapshot.role = std::move(role);
    snapshot.description_type = snapshot.role == "host" ? "offer" : "answer";
    snapshot.description_sdp =
        "v=0\n"
        "a=ice-ufrag:test\n"
        "a=ice-pwd:test-password\n";
    snapshot.publisher_instance_id = kPublisherInstanceId;
    snapshot.generation = 4;
    snapshot.connection_request_tag = "request-tag-0004";
    if (snapshot.description_type == "answer") {
        snapshot.answered_description_tag =
            redclaw::service::derive_dht_description_tag("v=0\na=ice-ufrag:remote\n");
    }
    snapshot.candidate_lines = {
        "0\tcandidate:1 1 udp 2122260223 192.168.1.10 5000 typ host",
        "0\tcandidate:2 1 udp 1686052607 203.0.113.8 62000 typ srflx",
    };
    snapshot.candidates_complete = true;
    snapshot.revision = revision;
    return snapshot;
}

bool publish_until_complete(
    redclaw::service::DhtRendezvousClient& client,
    const redclaw::service::DhtRendezvousConfig& config,
    const redclaw::service::DhtSignalSnapshot& snapshot,
    std::string* error,
    int max_attempts = 64) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        if (client.publish_signal_snapshot(config, snapshot, error)) {
            return true;
        }
    }
    return false;
}

std::filesystem::path test_scratch_root(const std::string& name) {
    return std::filesystem::current_path() / "redclaw-dht-test-scratch" / name;
}

class CapturingPrimaryDhtStore final : public redclaw::service::IDhtRendezvousStore {
public:
    [[nodiscard]] redclaw::service::DhtPublishResult publish(
        const redclaw::service::DhtEncryptedRecord& record,
        std::string_view routing_token,
        std::string* error_detail) override {
        (void)routing_token;
        (void)error_detail;
        last_record = record;
        published_records.push_back(record);
        return true;
    }

    [[nodiscard]] std::optional<redclaw::service::DhtEncryptedRecord> fetch(
        std::string_view topic_hex,
        std::string_view lane,
        std::uint64_t after_revision,
        std::string_view routing_token,
        std::string* error_detail) override {
        (void)routing_token;
        (void)error_detail;
        if (!last_record.has_value()) {
            return std::nullopt;
        }
        if (last_record->topic_hex != topic_hex || last_record->lane != lane || last_record->revision <= after_revision) {
            return std::nullopt;
        }
        return last_record;
    }

    std::optional<redclaw::service::DhtEncryptedRecord> last_record;
    std::vector<redclaw::service::DhtEncryptedRecord> published_records;
};

class StickyPrimaryDhtStore final : public redclaw::service::IDhtRendezvousStore {
public:
    [[nodiscard]] redclaw::service::DhtPublishResult publish(
        const redclaw::service::DhtEncryptedRecord& record,
        std::string_view routing_token,
        std::string* error_detail) override {
        (void)routing_token;
        (void)error_detail;
        published_records.push_back(record);
        if (!first_record.has_value()) {
            first_record = record;
        }
        return true;
    }

    [[nodiscard]] std::optional<redclaw::service::DhtEncryptedRecord> fetch(
        std::string_view topic_hex,
        std::string_view lane,
        std::uint64_t after_revision,
        std::string_view routing_token,
        std::string* error_detail) override {
        (void)routing_token;
        (void)error_detail;
        if (!first_record.has_value()) {
            return std::nullopt;
        }
        if (first_record->topic_hex != topic_hex || first_record->lane != lane || first_record->revision <= after_revision) {
            return std::nullopt;
        }
        return first_record;
    }

    std::optional<redclaw::service::DhtEncryptedRecord> first_record;
    std::vector<redclaw::service::DhtEncryptedRecord> published_records;
};

class WriteOnlyPrimaryDhtStore final : public redclaw::service::IDhtRendezvousStore {
public:
    [[nodiscard]] redclaw::service::DhtPublishResult publish(
        const redclaw::service::DhtEncryptedRecord& record,
        std::string_view routing_token,
        std::string* error_detail) override {
        (void)routing_token;
        (void)error_detail;
        last_record = record;
        return true;
    }

    [[nodiscard]] std::optional<redclaw::service::DhtEncryptedRecord> fetch(
        std::string_view topic_hex,
        std::string_view lane,
        std::uint64_t after_revision,
        std::string_view routing_token,
        std::string* error_detail) override {
        (void)topic_hex;
        (void)lane;
        (void)after_revision;
        (void)routing_token;
        (void)error_detail;
        return std::nullopt;
    }

    std::optional<redclaw::service::DhtEncryptedRecord> last_record;
};

class FailingDhtStore final : public redclaw::service::IDhtRendezvousStore {
public:
    [[nodiscard]] redclaw::service::DhtPublishResult publish(
        const redclaw::service::DhtEncryptedRecord& record,
        std::string_view routing_token,
        std::string* error_detail) override {
        (void)record;
        (void)routing_token;
        if (error_detail != nullptr) {
            *error_detail = "intentional secondary failure";
        }
        return false;
    }

    [[nodiscard]] std::optional<redclaw::service::DhtEncryptedRecord> fetch(
        std::string_view topic_hex,
        std::string_view lane,
        std::uint64_t after_revision,
        std::string_view routing_token,
        std::string* error_detail) override {
        (void)topic_hex;
        (void)lane;
        (void)after_revision;
        (void)routing_token;
        if (error_detail != nullptr) {
            *error_detail = "intentional secondary failure";
        }
        return std::nullopt;
    }
};

bool test_topic_derivation_is_stable_and_redacted() {
    const std::string topic_a = redclaw::service::derive_dht_rendezvous_topic_hex("A1B2C3D4", "pairing-secret");
    const std::string topic_b = redclaw::service::derive_dht_rendezvous_topic_hex("A1B2C3D4", "pairing-secret");
    const std::string topic_c = redclaw::service::derive_dht_rendezvous_topic_hex("A1B2C3D4", "other-secret");

    return expect_true(topic_a == topic_b, "same pairing material should produce stable topic")
        && expect_true(topic_a != topic_c, "different pairing material should produce different topic")
        && expect_true(topic_a.find("A1B2C3D4") == std::string::npos, "topic must not leak session code")
        && expect_true(topic_a.size() == 64, "topic should be sha256 hex");
}

    bool test_routing_token_derivation_is_stable_and_lane_scoped() {
        const std::string token_a = redclaw::service::derive_dht_rendezvous_routing_token_hex("A1B2C3D4", "pairing-secret", "host");
        const std::string token_b = redclaw::service::derive_dht_rendezvous_routing_token_hex("A1B2C3D4", "pairing-secret", "host");
        const std::string token_c = redclaw::service::derive_dht_rendezvous_routing_token_hex("A1B2C3D4", "pairing-secret", "controller");

        return expect_true(token_a == token_b, "same lane should produce stable routing token")
        && expect_true(token_a != token_c, "different lanes should produce different routing tokens")
        && expect_true(token_a.find("A1B2C3D4") == std::string::npos, "routing token must not leak session code")
        && expect_true(token_a.find("pairing-secret") == std::string::npos, "routing token must not leak pairing secret")
        && expect_true(token_a.size() == 64, "routing token should be sha256 hex");
    }

bool test_in_memory_publish_fetch_roundtrip() {
    redclaw::service::InMemoryDhtRendezvousStore store;
    redclaw::service::DhtRendezvousClient client(store);

    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "A1B2C3D4";
    config.pairing_secret = "pairing-secret";
    config.now_unix = 1000;
    config.ttl_seconds = 300;

    std::string error;
    const auto host = make_snapshot("host", 7);
    if (!expect_true(client.publish_signal_snapshot(config, host, &error), "publish should succeed: " + error)) {
        return false;
    }

    const auto fetched = client.fetch_signal_snapshot(config, "host", 0, &error);
    bool ok = true;
    ok = expect_true(fetched.has_value(), "fetch should return snapshot") && ok;
    if (fetched.has_value()) {
        ok = expect_true(fetched->role == "host", "fetched role should match lane") && ok;
        ok = expect_true(fetched->description_type == "offer", "description type should roundtrip") && ok;
        ok = expect_true(fetched->description_sdp == host.description_sdp, "SDP should roundtrip") && ok;
        ok = expect_true(
                 fetched->publisher_instance_id == host.publisher_instance_id,
                 "publisher instance ID should roundtrip")
            && ok;
        ok = expect_true(fetched->generation == host.generation, "generation should roundtrip") && ok;
        ok = expect_true(fetched->candidates_complete, "candidate completion should roundtrip") && ok;
        ok = expect_true(fetched->candidate_lines.size() == host.candidate_lines.size(), "candidates should roundtrip") && ok;
        ok = expect_true(fetched->revision == 7, "revision should roundtrip") && ok;
    }

    const auto stale_fetch = client.fetch_signal_snapshot(config, "host", 7, &error);
    ok = expect_true(!stale_fetch.has_value(), "fetch after current revision should return nothing") && ok;

    const auto controller = make_snapshot("controller", 3);
    ok = expect_true(
             client.publish_signal_snapshot(config, controller, &error),
             "controller publish should succeed: " + error)
        && ok;
    const auto fetched_controller = client.fetch_signal_snapshot(config, "controller", 0, &error);
    ok = expect_true(fetched_controller.has_value(), "controller fetch should return snapshot") && ok;
    if (fetched_controller.has_value()) {
        ok = expect_true(
                 fetched_controller->answered_description_tag == controller.answered_description_tag
                     && !controller.answered_description_tag.empty(),
                 "answered description tag should roundtrip")
            && ok;
        ok = expect_true(
                 fetched_controller->generation == controller.generation,
                 "controller generation should roundtrip")
            && ok;
    }

    auto acknowledged_host = make_snapshot("host", 8);
    acknowledged_host.acknowledged_answer_tag = "answer-tag-0001";
    ok = expect_true(
             client.publish_signal_snapshot(config, acknowledged_host, &error),
             "acknowledged host publish should succeed: " + error)
        && ok;
    const auto fetched_acknowledged_host = client.fetch_signal_snapshot(config, "host", 7, &error);
    ok = expect_true(fetched_acknowledged_host.has_value(), "acknowledged host fetch should return snapshot") && ok;
    if (fetched_acknowledged_host.has_value()) {
        ok = expect_true(
                 fetched_acknowledged_host->acknowledged_answer_tag == acknowledged_host.acknowledged_answer_tag,
                 "acknowledged answer tag should roundtrip")
            && ok;
    }

    auto connect_request = make_snapshot("controller", 9);
    connect_request.description_type = "request";
    connect_request.description_sdp = "controller-request-nonce";
    connect_request.generation = 0;
    connect_request.answered_description_tag.clear();
    connect_request.acknowledged_answer_tag.clear();
    connect_request.candidate_lines.clear();
    connect_request.candidates_complete = false;
    ok = expect_true(
             client.publish_signal_snapshot(config, connect_request, &error),
             "connect request publish should succeed: " + error)
        && ok;
    const auto fetched_connect_request = client.fetch_signal_snapshot(config, "controller", 8, &error);
    ok = expect_true(fetched_connect_request.has_value(), "connect request should roundtrip") && ok;
    if (fetched_connect_request.has_value()) {
        ok = expect_true(
                 fetched_connect_request->description_type == "request"
                     && fetched_connect_request->generation == 0
                     && fetched_connect_request->connection_request_tag
                         == connect_request.connection_request_tag,
                 "connect request correlation should roundtrip")
            && ok;
    }
    return ok;
}

bool test_publisher_instance_id_generation_and_validation() {
    std::string error;
    const auto first = redclaw::service::make_dht_publisher_instance_id(&error);
    const auto second = redclaw::service::make_dht_publisher_instance_id(&error);
    bool ok = true;
    ok = expect_true(first.has_value(), "first publisher instance ID should be generated: " + error) && ok;
    ok = expect_true(second.has_value(), "second publisher instance ID should be generated: " + error) && ok;
    if (first.has_value() && second.has_value()) {
        ok = expect_true(
                 redclaw::service::is_valid_dht_publisher_instance_id(*first),
                 "generated publisher instance ID should validate")
            && ok;
        ok = expect_true(*first != *second, "separate publisher instance IDs should differ") && ok;
    }
    ok = expect_true(
             !redclaw::service::is_valid_dht_publisher_instance_id(""),
             "empty publisher instance ID should be invalid")
        && ok;
    ok = expect_true(
             !redclaw::service::is_valid_dht_publisher_instance_id(
                 "0123456789ABCDEF0123456789ABCDEF"),
             "uppercase publisher instance ID should be invalid")
        && ok;
    ok = expect_true(
             !redclaw::service::is_valid_dht_publisher_instance_id(
                 "0123456789abcdef0123456789abcdeg"),
             "non-hex publisher instance ID should be invalid")
        && ok;

    redclaw::service::InMemoryDhtRendezvousStore store;
    redclaw::service::DhtRendezvousClient client(store);
    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "A1B2C3D4";
    config.pairing_secret = "pairing-secret";
    config.now_unix = 1000;
    auto invalid = make_snapshot("host", 10);
    invalid.publisher_instance_id.clear();
    error.clear();
    ok = expect_true(
             !client.publish_signal_snapshot(config, invalid, &error)
                 && error.find("incomplete") != std::string::npos,
             "V4 publication should fail closed without a publisher instance ID")
        && ok;
    return ok;
}

bool test_v3_plaintext_remains_parseable_but_has_no_instance_identity() {
    const std::string legacy_plaintext =
        "RCD-DHT-SIGNAL-V3\n"
        "role=host\n"
        "description_type=offer\n"
        "description_sdp_hex=763d300a\n"
        "generation=1\n"
        "connection_request_tag=legacy-request\n"
        "candidates_complete=1\n"
        "candidate_count=0\n";
    std::string error;
    const auto legacy = redclaw::service::detail::parse_dht_signal_plaintext(
        legacy_plaintext,
        17,
        1300,
        &error);
    bool ok = expect_true(
        legacy.has_value(),
        "legacy V3 plaintext should remain parseable: " + error);
    if (legacy.has_value()) {
        ok = expect_true(
                 legacy->publisher_instance_id.empty(),
                 "legacy V3 record must not invent a publisher instance identity")
            && ok;
        ok = expect_true(
                 legacy->revision == 17 && legacy->generation == 1,
                 "legacy V3 revision and generation should remain intact")
            && ok;
    }

    const std::string invalid_v4_plaintext =
        "RCD-DHT-SIGNAL-V4\n"
        "role=host\n"
        "description_type=offer\n"
        "description_sdp_hex=763d300a\n"
        "publisher_instance_id=INVALID\n"
        "generation=1\n"
        "connection_request_tag=request-v4\n"
        "candidates_complete=1\n"
        "candidate_count=0\n";
    error.clear();
    const auto invalid_v4 = redclaw::service::detail::parse_dht_signal_plaintext(
        invalid_v4_plaintext,
        18,
        1300,
        &error);
    ok = expect_true(
             !invalid_v4.has_value()
                 && error.find("publisher instance ID is invalid") != std::string::npos,
             "V4 plaintext with an invalid instance identity should fail closed")
        && ok;
    return ok;
}

bool test_description_tag_distinguishes_descriptions() {
    const std::string offer = "v=0\na=ice-ufrag:first\n";
    const std::string other_offer = "v=0\na=ice-ufrag:second\n";
    const auto tag = redclaw::service::derive_dht_description_tag(offer);
    return expect_true(
               tag == redclaw::service::derive_dht_description_tag(offer),
               "same description should produce a stable tag")
        && expect_true(
               tag != redclaw::service::derive_dht_description_tag(other_offer),
               "different descriptions should produce different tags")
        && expect_true(
               redclaw::service::derive_dht_description_tag("").empty(),
               "empty description should produce an empty tag")
        && expect_true(tag.find("ice-ufrag") == std::string::npos, "tag must not leak description content");
}

bool test_wrong_secret_and_expired_payload_are_rejected() {
    redclaw::service::InMemoryDhtRendezvousStore store;
    redclaw::service::DhtRendezvousClient client(store);

    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "Z9Y8X7W6";
    config.pairing_secret = "good-secret";
    config.now_unix = 1000;
    config.ttl_seconds = 5;

    std::string error;
    if (!expect_true(client.publish_signal_snapshot(config, make_snapshot("controller"), &error), "publish should succeed")) {
        return false;
    }

    redclaw::service::DhtRendezvousConfig wrong_secret = config;
    wrong_secret.pairing_secret = "bad-secret";
    const auto wrong = client.fetch_signal_snapshot(wrong_secret, "controller", 0, &error);

    redclaw::service::DhtRendezvousConfig expired = config;
    expired.now_unix = 1006;
    const auto expired_result = client.fetch_signal_snapshot(expired, "controller", 0, &error);

    return expect_true(!wrong.has_value(), "wrong secret should not decrypt snapshot")
        && expect_true(!expired_result.has_value(), "expired snapshot should be rejected");
}

bool test_new_revision_refreshes_record_expiry() {
    CapturingPrimaryDhtStore store;
    redclaw::service::DhtRendezvousClient publisher(store);

    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "KEEP1234";
    config.pairing_secret = "keep-alive-secret";
    config.now_unix = 4000;
    config.ttl_seconds = 300;

    auto snapshot = make_snapshot("host", 1);
    std::string error;
    if (!expect_true(
            publisher.publish_signal_snapshot(config, snapshot, &error),
            "initial keep-alive publish should succeed: " + error)) {
        return false;
    }

    const auto first_record = store.last_record;
    if (!expect_true(first_record.has_value(), "initial keep-alive record should be captured")) {
        return false;
    }

    config.now_unix = 4301;
    snapshot.revision = 2;
    if (!expect_true(
            publisher.publish_signal_snapshot(config, snapshot, &error),
            "new-revision keep-alive publish should succeed: " + error)) {
        return false;
    }

    const auto refreshed_record = store.last_record;
    bool ok = expect_true(refreshed_record.has_value(), "refreshed keep-alive record should be captured");
    if (!refreshed_record.has_value()) {
        return false;
    }

    ok = expect_true(refreshed_record->revision == 2, "keep-alive refresh should advance the record revision") && ok;
    ok = expect_true(
            refreshed_record->expires_at_unix == 4601,
            "keep-alive refresh should seal a new expiry") && ok;
    ok = expect_true(
            refreshed_record->expires_at_unix > first_record->expires_at_unix,
            "keep-alive refresh should extend the record lifetime") && ok;
    ok = expect_true(
            refreshed_record->encrypted_blob != first_record->encrypted_blob,
            "keep-alive refresh should produce fresh ciphertext") && ok;

    redclaw::service::DhtRendezvousClient reader(store);
    const auto fetched = reader.fetch_signal_snapshot(config, "host", 1, &error);
    ok = expect_true(
            fetched.has_value(),
            "a peer should fetch the refreshed record after the original TTL: " + error) && ok;
    return ok;
}

bool test_file_backed_store_writes_encrypted_mailbox_artifact() {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto root = test_scratch_root("mailbox-" + std::to_string(stamp));

    redclaw::service::FileBackedDhtRendezvousStore store(root);
    redclaw::service::DhtRendezvousClient client(store);

    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "Q1W2E3R4";
    config.pairing_secret = "mailbox-secret";
    config.now_unix = 2000;
    config.ttl_seconds = 300;

    std::string error;
    const auto snapshot = make_snapshot("host", 3);
    if (!expect_true(client.publish_signal_snapshot(config, snapshot, &error), "file-backed publish should succeed: " + error)) {
        return false;
    }

    bool artifact_has_plaintext = false;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        artifact_has_plaintext = artifact_has_plaintext
            || contents.find(snapshot.description_sdp) != std::string::npos
            || contents.find(snapshot.candidate_lines.front()) != std::string::npos;
    }

    const auto fetched = client.fetch_signal_snapshot(config, "host", 0, &error);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    return expect_true(!artifact_has_plaintext, "mailbox artifact must not contain plaintext SDP/candidates")
        && expect_true(fetched.has_value(), "file-backed fetch should roundtrip encrypted snapshot");
}

bool test_indirect_store_compacts_large_primary_payload_and_roundtrips() {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto secondary_root = test_scratch_root("indirect-mailbox-" + std::to_string(stamp));

    auto primary_store = std::make_unique<CapturingPrimaryDhtStore>();
    auto* primary_capture = primary_store.get();
    auto secondary_store = std::make_unique<redclaw::service::FileBackedDhtRendezvousStore>(secondary_root);
    redclaw::service::IndirectDhtRendezvousStore store(std::move(primary_store), std::move(secondary_store));
    redclaw::service::DhtRendezvousClient client(store);

    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "B1C2D3E4";
    config.pairing_secret = "indirect-secret";
    config.now_unix = 3000;
    config.ttl_seconds = 300;

    auto snapshot = make_snapshot("host", 9);
    snapshot.description_sdp += incompressible_text(1200);
    snapshot.candidate_lines.push_back("0\tcandidate:3 1 udp 41885439 198.51.100.4 3478 typ relay generation 0 ufrag long-ufrag network-cost 999");
    snapshot.candidate_lines.push_back(std::string("0\tcandidate:4 1 udp 2122260223 2001:db8::feed 54000 typ host generation 0 ") + std::string(400, 'x'));

    std::string error;
    if (!expect_true(publish_until_complete(client, config, snapshot, &error), "indirect publish should succeed: " + error)) {
        return false;
    }

    bool ok = true;
    ok = expect_true(primary_capture->last_record.has_value(), "primary compact record should be captured") && ok;
    if (primary_capture->last_record.has_value()) {
        ok = expect_true(primary_capture->last_record->encrypted_blob.find("RCD-DHT-INDIRECT-V1") == 0, "primary record should carry indirect summary header") && ok;
        ok = expect_true(primary_capture->last_record->encrypted_blob.size() < 512, "primary compact summary should stay small") && ok;
        ok = expect_true(primary_capture->last_record->encrypted_blob.find(snapshot.description_sdp) == std::string::npos, "primary compact summary must not leak SDP") && ok;
        ok = expect_true(primary_capture->last_record->encrypted_blob.find(snapshot.candidate_lines.front()) == std::string::npos, "primary compact summary must not leak candidates") && ok;
    }

    const auto fetched = client.fetch_signal_snapshot(config, "host", 0, &error);
    ok = expect_true(fetched.has_value(), "indirect fetch should roundtrip encrypted snapshot: " + error) && ok;
    if (fetched.has_value()) {
        ok = expect_true(fetched->description_sdp == snapshot.description_sdp, "indirect fetch should preserve large SDP") && ok;
        ok = expect_true(fetched->candidate_lines == snapshot.candidate_lines, "indirect fetch should preserve candidate list") && ok;
        ok = expect_true(fetched->revision == snapshot.revision, "indirect fetch should preserve revision") && ok;
    }

    std::error_code ec;
    std::filesystem::remove_all(secondary_root, ec);
    return ok;
}

bool test_indirect_store_publishes_small_description_snapshot_directly() {
    auto primary_store = std::make_unique<CapturingPrimaryDhtStore>();
    auto* primary_capture = primary_store.get();
    auto secondary_store = std::make_unique<FailingDhtStore>();
    redclaw::service::IndirectDhtRendezvousStore store(std::move(primary_store), std::move(secondary_store));
    redclaw::service::DhtRendezvousClient client(store);

    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "SMOL1234";
    config.pairing_secret = "small-direct-secret";
    config.now_unix = 3500;
    config.ttl_seconds = 300;

    auto snapshot = make_snapshot("controller", 3);
    snapshot.candidate_lines.clear();

    std::string error;
    bool ok = true;
    ok = expect_true(
        client.publish_signal_snapshot(config, snapshot, &error),
        "small description-only snapshot should publish directly: " + error) && ok;
    ok = expect_true(primary_capture->last_record.has_value(), "direct primary record should be captured") && ok;
    if (primary_capture->last_record.has_value()) {
        ok = expect_true(
            primary_capture->last_record->encrypted_blob.find("RCD-DHT-INDIRECT-V1") != 0,
            "small description-only snapshot must not use an indirect summary") && ok;
    }

    const auto fetched = client.fetch_signal_snapshot(config, "controller", 0, &error);
    ok = expect_true(fetched.has_value(), "small direct fetch should roundtrip snapshot: " + error) && ok;
    if (fetched.has_value()) {
        ok = expect_true(fetched->description_sdp == snapshot.description_sdp, "small direct fetch should preserve SDP") && ok;
        ok = expect_true(fetched->candidate_lines.empty(), "small direct fetch should preserve empty candidates") && ok;
        ok = expect_true(fetched->revision == snapshot.revision, "small direct fetch should preserve revision") && ok;
    }
    return ok;
}

bool test_indirect_retry_reuses_same_blob_metadata_for_same_revision() {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto secondary_root = test_scratch_root("indirect-retry-" + std::to_string(stamp));

    auto primary_store = std::make_unique<StickyPrimaryDhtStore>();
    auto* sticky_primary = primary_store.get();
    auto secondary_store = std::make_unique<redclaw::service::FileBackedDhtRendezvousStore>(secondary_root);
    redclaw::service::IndirectDhtRendezvousStore store(std::move(primary_store), std::move(secondary_store));
    redclaw::service::DhtRendezvousClient client(store);

    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "RTRY1234";
    config.pairing_secret = "retry-secret";
    config.now_unix = 4000;
    config.ttl_seconds = 300;

    auto snapshot = make_snapshot("host", 1);
    snapshot.description_sdp += incompressible_text(1800);
    std::string error;
    if (!expect_true(publish_until_complete(client, config, snapshot, &error), "first retry publish should succeed: " + error)) {
        return false;
    }

    config.now_unix = 4015;
    if (!expect_true(publish_until_complete(client, config, snapshot, &error), "second retry publish should reuse metadata: " + error)) {
        return false;
    }

    bool ok = true;
    std::vector<redclaw::service::DhtEncryptedRecord> summary_records;
    for (const auto& record : sticky_primary->published_records) {
        if (record.lane == snapshot.role) {
            summary_records.push_back(record);
        }
    }
    ok = expect_true(summary_records.size() == 2, "sticky primary should capture both summary publish attempts") && ok;
    if (summary_records.size() == 2) {
        ok = expect_true(
            summary_records[0].encrypted_blob == summary_records[1].encrypted_blob,
            "same revision retries should reuse the same compact summary") && ok;
        ok = expect_true(
            summary_records[0].expires_at_unix == summary_records[1].expires_at_unix,
            "same revision retries should reuse the same expiry") && ok;
    }

    const auto fetched = client.fetch_signal_snapshot(config, "host", 0, &error);
    ok = expect_true(fetched.has_value(), "retry-stable indirect fetch should succeed: " + error) && ok;
    if (fetched.has_value()) {
        ok = expect_true(fetched->description_sdp == snapshot.description_sdp, "retry-stable fetch should preserve SDP") && ok;
        ok = expect_true(fetched->candidate_lines == snapshot.candidate_lines, "retry-stable fetch should preserve candidates") && ok;
    }

    std::error_code ec;
    std::filesystem::remove_all(secondary_root, ec);
    return ok;
}

bool test_indirect_fetch_falls_back_to_secondary_mailbox_when_primary_misses() {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto secondary_root = test_scratch_root("indirect-secondary-fallback-" + std::to_string(stamp));

    auto primary_store = std::make_unique<WriteOnlyPrimaryDhtStore>();
    auto secondary_store = std::make_unique<redclaw::service::FileBackedDhtRendezvousStore>(secondary_root);
    redclaw::service::IndirectDhtRendezvousStore store(std::move(primary_store), std::move(secondary_store));
    redclaw::service::DhtRendezvousClient client(store);

    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "FALLB123";
    config.pairing_secret = "secondary-fallback-secret";
    config.now_unix = 5000;
    config.ttl_seconds = 300;

    auto snapshot = make_snapshot("controller", 2);
    snapshot.description_sdp += incompressible_text(1800);
    snapshot.candidate_lines.push_back("0\tcandidate:5 1 udp 2122260223 127.0.0.1 54001 typ host generation 0");

    std::string error;
    if (!expect_true(publish_until_complete(client, config, snapshot, &error), "fallback publish should succeed: " + error)) {
        return false;
    }

    bool ok = true;
    const auto fetched = client.fetch_signal_snapshot(config, "controller", 0, &error);
    ok = expect_true(fetched.has_value(), "secondary fallback fetch should succeed: " + error) && ok;
    if (fetched.has_value()) {
        ok = expect_true(fetched->description_sdp == snapshot.description_sdp, "secondary fallback should preserve SDP") && ok;
        ok = expect_true(fetched->candidate_lines == snapshot.candidate_lines, "secondary fallback should preserve candidates") && ok;
        ok = expect_true(fetched->revision == snapshot.revision, "secondary fallback should preserve revision") && ok;
    }

    std::error_code ec;
    std::filesystem::remove_all(secondary_root, ec);
    return ok;
}

bool test_indirect_store_roundtrips_large_payload_without_secondary_mailbox() {
    auto primary_store = std::make_unique<redclaw::service::InMemoryDhtRendezvousStore>();
    auto secondary_store = std::make_unique<FailingDhtStore>();
    redclaw::service::IndirectDhtRendezvousStore store(std::move(primary_store), std::move(secondary_store));
    redclaw::service::DhtRendezvousClient client(store);

    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "CHNK1234";
    config.pairing_secret = "chunk-secret";
    config.now_unix = 6000;
    config.ttl_seconds = 300;

    auto snapshot = make_snapshot("host", 11);
    snapshot.description_sdp += incompressible_text(1800);
    snapshot.candidate_lines.push_back(
        std::string("0\tcandidate:6 1 udp 2122260223 10.0.0.8 55000 typ host generation 0 ")
        + std::string(500, 'c'));

    std::string error;
    bool ok = true;
    ok = expect_true(
        publish_until_complete(client, config, snapshot, &error),
        "chunked primary publish should not require secondary mailbox: " + error) && ok;

    const auto fetched = client.fetch_signal_snapshot(config, "host", 0, &error);
    ok = expect_true(fetched.has_value(), "chunked primary fetch should reconstruct snapshot: " + error) && ok;
    if (fetched.has_value()) {
        ok = expect_true(fetched->description_sdp == snapshot.description_sdp, "chunked fetch should preserve SDP") && ok;
        ok = expect_true(fetched->candidate_lines == snapshot.candidate_lines, "chunked fetch should preserve candidates") && ok;
        ok = expect_true(fetched->revision == snapshot.revision, "chunked fetch should preserve revision") && ok;
    }
    return ok;
}

bool test_indirect_chunks_are_content_addressed_for_same_revision() {
    auto primary_store = std::make_unique<CapturingPrimaryDhtStore>();
    auto* capture = primary_store.get();
    redclaw::service::IndirectDhtRendezvousStore store(std::move(primary_store), nullptr);

    redclaw::service::DhtEncryptedRecord record;
    record.topic_hex = std::string(64, 'a');
    record.lane = "host";
    record.revision = 7;
    record.expires_at_unix = 9000;
    record.encrypted_blob = std::string(2200, 'x');
    std::string error;
    if (!expect_true(store.publish(record, "routing", &error),
                     "first content-addressed publish should succeed: " + error)) {
        return false;
    }

    std::set<std::string> first_chunk_lanes;
    for (const auto& published : capture->published_records) {
        if (published.lane != record.lane) {
            first_chunk_lanes.insert(published.lane);
        }
    }
    capture->published_records.clear();
    record.encrypted_blob = std::string(2200, 'y');
    if (!expect_true(store.publish(record, "routing", &error),
                     "second same-revision publish should succeed: " + error)) {
        return false;
    }

    bool disjoint = true;
    for (const auto& published : capture->published_records) {
        if (published.lane != record.lane
            && first_chunk_lanes.contains(published.lane)) {
            disjoint = false;
        }
    }
    return expect_true(!first_chunk_lanes.empty(), "first publish should create indirect chunk lanes")
        && expect_true(disjoint, "different same-revision blobs must use disjoint content-addressed lanes");
}

bool test_bootstrap_nodes_prefer_cached_numeric_fallbacks() {
    const auto normalized = redclaw::service::normalize_dht_bootstrap_nodes({
        "router.bittorrent.com:6881",
        "dht.transmissionbt.com:6881",
        "dht.libtorrent.org:25401",
        "router.utorrent.com:6881",
    });

    bool ok = true;
    ok = expect_true(normalized.size() == 5, "default bootstrap should expand to five numeric nodes") && ok;
    if (normalized.size() >= 5) {
        ok = expect_true(normalized[0] == "67.215.246.10:6881", "router fallback should be first") && ok;
        ok = expect_true(normalized[1] == "87.98.162.88:6881", "transmission fallback should follow") && ok;
        ok = expect_true(normalized[2] == "212.129.33.59:6881", "transmission secondary fallback should follow") && ok;
        ok = expect_true(normalized[3] == "185.157.221.247:25401", "libtorrent fallback should follow") && ok;
        ok = expect_true(normalized[4] == "82.221.103.244:6881", "utorrent fallback should follow") && ok;
    }

    for (const std::string& node : normalized) {
        ok = expect_true(node.find("bittorrent.com") == std::string::npos, "hostname bootstrap should be omitted: " + node) && ok;
        ok = expect_true(node.find("transmissionbt.com") == std::string::npos, "hostname bootstrap should be omitted: " + node) && ok;
        ok = expect_true(node.find("libtorrent.org") == std::string::npos, "hostname bootstrap should be omitted: " + node) && ok;
        ok = expect_true(node.find("utorrent.com") == std::string::npos, "hostname bootstrap should be omitted: " + node) && ok;
    }

    const auto custom = redclaw::service::normalize_dht_bootstrap_nodes({"bootstrap.example.com:6881"});
    ok = expect_true(custom.size() == 1, "custom hostname without fallback should be preserved") && ok;
    if (!custom.empty()) {
        ok = expect_true(custom[0] == "bootstrap.example.com:6881", "custom bootstrap node should pass through") && ok;
    }
    return ok;
}

bool test_helper_role_and_path_probe() {
    redclaw::service::LocalHelperRole role = redclaw::service::LocalHelperRole::kOff;
    std::string error;
    bool ok = true;
    ok = expect_true(redclaw::service::parse_local_helper_role("lan-helper", &role, &error), "lan-helper role should parse") && ok;
    ok = expect_true(role == redclaw::service::LocalHelperRole::kLanHelper, "parsed role should be lan-helper") && ok;
    ok = expect_true(!redclaw::service::parse_local_helper_role("invalid", &role, &error), "invalid helper role should fail") && ok;

    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto root = test_scratch_root("helper-probe-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto probe = redclaw::service::probe_local_helper_path(root);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    ok = expect_true(probe.status == redclaw::service::LocalHelperPathStatus::kWritable, "temp helper path should be writable") && ok;
    ok = expect_true(probe.writable, "writable probe should set writable flag") && ok;
    return ok;
}

bool test_candidate_diagnostics_helpers() {
    const std::string ipv4 = "candidate:0 1 udp 2122260223 192.168.1.10 5000 typ host";
    const std::string ipv6 = "candidate:1 1 udp 2122260223 2001:db8::1 5000 typ host";
    const std::string srflx = "candidate:2 1 udp 1686052607 203.0.113.8 62000 typ srflx";
    const std::string relay = "candidate:3 1 udp 41885439 198.51.100.4 3478 typ relay";

    bool ok = true;
    ok = expect_true(!redclaw::service::candidate_looks_ipv6(ipv4), "IPv4 candidate should not be detected as IPv6") && ok;
    ok = expect_true(redclaw::service::candidate_looks_ipv6(ipv6), "IPv6 candidate should be detected") && ok;
    ok = expect_true(redclaw::service::candidate_looks_server_reflexive(srflx), "srflx candidate should be detected") && ok;
    ok = expect_true(redclaw::service::candidate_looks_relay(relay), "relay candidate should be detected") && ok;

    const auto prioritized = redclaw::service::prioritize_dht_candidate_lines({ipv4, srflx, relay});
    ok = expect_true(prioritized.size() == 3, "candidate prioritization should preserve every candidate") && ok;
    if (prioritized.size() == 3) {
        ok = expect_true(prioritized[0] == relay, "relay should be the first DHT candidate") && ok;
        ok = expect_true(prioritized[1] == srflx, "srflx should follow relay") && ok;
        ok = expect_true(prioritized[2] == ipv4, "host should follow public candidates") && ok;
    }

    const auto cumulative = redclaw::service::merge_dht_candidate_snapshots(
        {srflx, ipv4},
        {relay, srflx});
    ok = expect_true(
        cumulative.size() == 3,
        "cumulative DHT candidate snapshot should deduplicate candidates") && ok;
    if (cumulative.size() == 3) {
        ok = expect_true(
            cumulative[0] == relay,
            "cumulative DHT candidate snapshot should reprioritize relay first") && ok;
        ok = expect_true(
            cumulative[1] == srflx,
            "cumulative DHT candidate snapshot should retain the published srflx candidate") && ok;
        ok = expect_true(
            cumulative[2] == ipv4,
            "cumulative DHT candidate snapshot should retain the published host candidate") && ok;
    }

    redclaw::service::DhtSignalSnapshot compact_description;
    compact_description.role = "controller";
    compact_description.description_type = "answer";
    compact_description.description_sdp = incompressible_text(500);
    compact_description.publisher_instance_id = kPublisherInstanceId;
    compact_description.generation = 5;
    compact_description.connection_request_tag = "request-tag-0005";
    compact_description.answered_description_tag = "0123456789abcdef";

    std::string plan_error;
    for (std::size_t length = 100; length < 2000; ++length) {
        compact_description.description_sdp = incompressible_text(length);
        auto trial = compact_description;
        trial.candidate_lines = {relay, srflx, ipv4};
        if (!redclaw::service::dht_signal_snapshot_fits_direct_record(trial)) break;
    }
    const auto chunked_plan = redclaw::service::plan_dht_initial_snapshot(
        compact_description,
        {ipv4, srflx, relay},
        &plan_error);
    ok = expect_true(chunked_plan.has_value(), "large initial snapshot plan should be valid: " + plan_error) && ok;
    if (chunked_plan.has_value()) {
        ok = expect_true(
            !chunked_plan->full_snapshot_fits_direct,
            "full wire-size overflow should avoid a chunked initial publication") && ok;
        ok = expect_true(
            !chunked_plan->candidate_lines.empty()
                && chunked_plan->candidate_lines.size() < 3,
            "initial publication should carry the largest priority prefix that fits directly") && ok;
        if (!chunked_plan->candidate_lines.empty()) {
            ok = expect_true(
                chunked_plan->candidate_lines[0] == relay,
                "wire-size-gated initial prefix should keep the relay candidate first") && ok;
            redclaw::service::DhtSignalSnapshot selected_snapshot = compact_description;
            selected_snapshot.candidate_lines = chunked_plan->candidate_lines;
            ok = expect_true(
                redclaw::service::dht_signal_snapshot_fits_direct_record(selected_snapshot),
                "selected initial candidate prefix should fit the direct record") && ok;
        }
        ok = expect_true(
            chunked_plan->full_encrypted_blob_bytes > redclaw::service::kDirectDhtEncryptedBlobBudgetBytes,
            "chunked plan should report the encrypted wire size above the direct budget") && ok;
    }

    redclaw::service::DhtSignalSnapshot small_description = compact_description;
    small_description.description_sdp = "small-answer";
    const auto direct_plan = redclaw::service::plan_dht_initial_snapshot(
        small_description,
        {ipv4, srflx, relay},
        &plan_error);
    ok = expect_true(direct_plan.has_value(), "small initial snapshot plan should be valid: " + plan_error) && ok;
    if (direct_plan.has_value()) {
        ok = expect_true(
            direct_plan->full_snapshot_fits_direct,
            "small encrypted snapshot should remain a single direct publication") && ok;
        ok = expect_true(
            direct_plan->candidate_lines.size() == 3,
            "small direct snapshot should carry the full candidate set") && ok;
        if (direct_plan->candidate_lines.size() == 3) {
            ok = expect_true(direct_plan->candidate_lines[0] == relay, "direct plan should prioritize relay first") && ok;
            ok = expect_true(direct_plan->candidate_lines[1] == srflx, "direct plan should prioritize srflx second") && ok;
            ok = expect_true(direct_plan->candidate_lines[2] == ipv4, "direct plan should keep host last") && ok;
        }
    }

    redclaw::service::DhtSignalSnapshot priority_snapshot = compact_description;
    priority_snapshot.candidate_lines = {relay};
    std::size_t priority_wire_bytes = 0;
    ok = expect_true(
        redclaw::service::dht_signal_snapshot_fits_direct_record(
            priority_snapshot,
            &priority_wire_bytes,
            &plan_error),
        "one priority candidate should fit the direct record: " + plan_error) && ok;
    ok = expect_true(
        priority_wire_bytes <= redclaw::service::kDirectDhtEncryptedBlobBudgetBytes,
        "direct candidate estimate should stay within the direct budget") && ok;

    redclaw::service::DhtSignalSnapshot runtime_sized_snapshot = compact_description;
    runtime_sized_snapshot.description_sdp = std::string(468, 's');
    runtime_sized_snapshot.candidate_lines = {srflx, ipv4};
    runtime_sized_snapshot.candidates_complete = true;
    std::size_t runtime_sized_wire_bytes = 0;
    ok = expect_true(
             redclaw::service::dht_signal_snapshot_fits_direct_record(
                 runtime_sized_snapshot,
                 &runtime_sized_wire_bytes,
                 &plan_error),
             "representative host+srflx V4 snapshot should fit one direct DHT record: "
                 + plan_error)
        && ok;
    ok = expect_true(
             runtime_sized_wire_bytes <= redclaw::service::kDirectDhtEncryptedBlobBudgetBytes,
             "compact V4 instance identity must not force a representative two-candidate snapshot indirect")
        && ok;

    redclaw::service::DhtSignalSnapshot description_only_fallback = compact_description;
    description_only_fallback.description_sdp = incompressible_text(800);
    const auto description_only_plan = redclaw::service::plan_dht_initial_snapshot(
        description_only_fallback,
        {relay},
        &plan_error);
    ok = expect_true(
        description_only_plan.has_value(),
        "description-only fallback plan should be valid: " + plan_error) && ok;
    if (description_only_plan.has_value()) {
        ok = expect_true(
            description_only_plan->candidate_lines.empty(),
            "no candidate should be added when even one candidate exceeds the direct budget") && ok;
    }
    return ok;
}

bool test_mutable_get_exposes_verified_intermediate_response() {
    redclaw::service::detail::DhtMutableGetAccumulator accumulator;

    redclaw::service::DhtEncryptedRecord stale;
    stale.topic_hex = "topic";
    stale.lane = "host";
    stale.encrypted_blob = "old";
    stale.revision = 1;
    stale.expires_at_unix = 100;
    accumulator.observe(100, false, stale);

    bool ok = true;
    ok = expect_true(
        !accumulator.authoritative_received(),
        "intermediate mutable response must not finalize the lookup") && ok;
    ok = expect_true(
        accumulator.latest_verified_record().has_value()
            && accumulator.latest_verified_record()->revision == 1,
        "signature-verified intermediate response should be available immediately") && ok;

    redclaw::service::DhtEncryptedRecord fresh = stale;
    fresh.encrypted_blob = "new";
    fresh.revision = 2;
    fresh.expires_at_unix = 200;
    accumulator.observe(150, false, fresh);
    ok = expect_true(
        accumulator.latest_verified_record().has_value()
            && accumulator.latest_verified_record()->revision == 2,
        "higher-sequence verified intermediate response should replace the earlier value") && ok;
    ok = expect_true(
        accumulator.latest_verified_sequence().has_value()
            && *accumulator.latest_verified_sequence() == 150,
        "latest verified sequence should track the intermediate response") && ok;
    accumulator.observe(200, true, fresh);

    ok = expect_true(
        accumulator.authoritative_received(),
        "authoritative mutable response should complete the lookup") && ok;
    ok = expect_true(
        accumulator.authoritative_sequence().has_value() && *accumulator.authoritative_sequence() == 200,
        "lookup should retain the authoritative response sequence") && ok;
    ok = expect_true(
        accumulator.authoritative_record().has_value() && accumulator.authoritative_record()->revision == 2,
        "lookup should return only the authoritative response record") && ok;
    ok = expect_true(
        accumulator.latest_verified_sequence().has_value()
            && *accumulator.latest_verified_sequence() == 200,
        "authoritative response should become the latest verified sequence") && ok;
    ok = expect_true(accumulator.response_count() == 3, "all mutable responses should be counted") && ok;
    ok = expect_true(
        accumulator.non_authoritative_count() == 2,
        "non-authoritative responses should be counted separately") && ok;
    return ok;
}

bool test_mutable_get_authoritative_empty_response_completes_without_record() {
    redclaw::service::detail::DhtMutableGetAccumulator accumulator;
    redclaw::service::DhtEncryptedRecord candidate;
    candidate.topic_hex = "topic";
    candidate.lane = "host";
    candidate.encrypted_blob = "non-authoritative";
    candidate.revision = 1;
    candidate.expires_at_unix = 100;
    accumulator.observe(100, false, candidate);
    accumulator.observe(0, true, std::nullopt);

    return expect_true(
               accumulator.authoritative_received(),
               "authoritative empty mutable response should complete the lookup")
        && expect_true(
            !accumulator.authoritative_record().has_value(),
            "authoritative empty mutable response should replace any non-authoritative candidate")
        && expect_true(
            !accumulator.latest_verified_record().has_value(),
            "authoritative empty response should clear the intermediate value");
}

bool test_libtorrent_mutable_get_poll_is_non_blocking_by_default() {
    const redclaw::service::LibtorrentDhtRendezvousStoreOptions options;
    return expect_true(
        options.fetch_timeout_milliseconds == 0,
        "runtime mutable-get polling should not block the 20 ms orchestration loop");
}

bool test_publish_revision_floor_survives_runtime_restart() {
    constexpr std::uint64_t kPreviousProcessLastRevision = 103;
    constexpr std::uint64_t kFirstProcessStartUnixMs = 1'788'334'000'000ULL;
    constexpr std::uint64_t kSecondProcessStartUnixMs = kFirstProcessStartUnixMs + 2'000ULL;

    const std::uint64_t first_process_revision =
        redclaw::service::make_dht_publish_revision_floor(kFirstProcessStartUnixMs) + 1;
    const std::uint64_t second_process_revision =
        redclaw::service::make_dht_publish_revision_floor(kSecondProcessStartUnixMs) + 1;

    redclaw::service::InMemoryDhtRendezvousStore store;
    redclaw::service::DhtRendezvousClient previous_process(store);
    redclaw::service::DhtRendezvousClient restarted_process(store);
    redclaw::service::DhtRendezvousClient reader(store);
    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "A1B2C3D4";
    config.pairing_secret = "pairing-secret";
    config.now_unix = 1'788'334'000ULL;

    auto previous = make_snapshot("controller", kPreviousProcessLastRevision);
    previous.connection_request_tag = "previous-process";
    auto restarted = make_snapshot("controller", first_process_revision);
    restarted.connection_request_tag = "restarted-process";

    std::string error;
    bool ok = expect_true(
        previous_process.publish_signal_snapshot(config, previous, &error),
        "previous publisher should establish the retained revision: " + error);
    ok = expect_true(
             restarted_process.publish_signal_snapshot(config, restarted, &error),
             "restarted publisher should replace the retained record: " + error)
        && ok;
    const auto fetched = reader.fetch_signal_snapshot(
        config,
        "controller",
        kPreviousProcessLastRevision,
        &error);

    return expect_true(
               first_process_revision > kPreviousProcessLastRevision,
               "a restarted publisher must advance beyond a retained legacy revision")
        && expect_true(
            second_process_revision > first_process_revision,
            "a later publisher process must begin above the prior process revision range")
        && expect_true(
            fetched.has_value()
                && fetched->connection_request_tag == restarted.connection_request_tag,
            "a peer retaining the old revision must receive the restarted publisher record")
        && ok;
}

bool test_answer_refresh_preserves_offer_correlation() {
    constexpr std::string_view kOfferTag = "0123456789abcdef";
    const auto initial_tag = redclaw::service::select_dht_answered_description_tag(
        "answer",
        kOfferTag);
    const auto refresh_tag = redclaw::service::select_dht_answered_description_tag(
        "answer",
        kOfferTag);

    redclaw::service::InMemoryDhtRendezvousStore store;
    redclaw::service::DhtRendezvousClient publisher(store);
    redclaw::service::DhtRendezvousClient reader(store);
    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "A1B2C3D4";
    config.pairing_secret = "pairing-secret";
    config.now_unix = 1'788'334'000ULL;

    auto initial = make_snapshot("controller", 700);
    initial.answered_description_tag = initial_tag;
    auto refresh = initial;
    refresh.revision = 701;
    refresh.answered_description_tag = refresh_tag;

    std::string error;
    bool ok = expect_true(
        publisher.publish_signal_snapshot(config, initial, &error),
        "initial answer should publish: " + error);
    ok = expect_true(
             publisher.publish_signal_snapshot(config, refresh, &error),
             "answer keep-alive should publish: " + error)
        && ok;
    const auto fetched_refresh = reader.fetch_signal_snapshot(config, "controller", 700, &error);

    return expect_true(
               initial_tag == kOfferTag,
               "initial answer publication should carry the answered offer tag")
        && expect_true(
            refresh_tag == initial_tag,
            "answer keep-alive publication should preserve the answered offer tag")
        && expect_true(
            redclaw::service::select_dht_answered_description_tag("offer", kOfferTag).empty(),
            "offer publication must not carry an answered description tag")
        && expect_true(
            redclaw::service::select_dht_answered_description_tag("request", kOfferTag).empty(),
            "request publication must not carry an answered description tag")
        && expect_true(
            fetched_refresh.has_value()
                && fetched_refresh->answered_description_tag == kOfferTag,
            "the serialized keep-alive record must retain the answered offer tag")
        && ok;
}

bool test_dht_listen_port_selection_preserves_fixed_port() {
    const auto selection = redclaw::service::detail::select_dht_listen_port_bounded(
        45911, [](int port) { return redclaw::service::detail::DhtListenPortSelection{.selected_port = port}; });
    return expect_true(
               selection.selected_port == 45911,
               "fixed DHT port should be preserved")
        && expect_true(
            !selection.os_assigned,
            "fixed DHT port should not be reported as OS assigned")
        && expect_true(
            selection.error_detail.empty(),
            "fixed DHT port should not produce a selection error");
}

bool test_dht_listen_port_selection_rejects_out_of_range_port() {
    const auto negative = redclaw::service::detail::select_dht_listen_port(-1, {});
    const auto overflow = redclaw::service::detail::select_dht_listen_port(65536, {});
    return expect_true(
               negative.selected_port == 0 && !negative.error_detail.empty(),
               "negative DHT listen port should fail closed")
        && expect_true(
            overflow.selected_port == 0 && !overflow.error_detail.empty(),
            "overflow DHT listen port should fail closed");
}

bool test_dht_listen_port_selection_rejects_invalid_address() {
    const auto selection = redclaw::service::detail::select_dht_listen_port(
        0,
        "not-an-ipv4-address");
#if defined(_WIN32)
    return expect_true(
               selection.selected_port == 0,
               "invalid bind address should not produce an automatic DHT port")
        && expect_true(
            !selection.os_assigned,
            "failed automatic selection should not report an OS-assigned port")
        && expect_true(
            !selection.error_detail.empty(),
            "failed automatic selection should retain a diagnostic");
#else
    return expect_true(selection.selected_port == 0, "non-Windows auto selection should remain delegated");
#endif
}

bool test_dht_listen_port_selection_uses_windows_udp_assignment() {
    const auto selection = redclaw::service::detail::select_dht_listen_port(0, "127.0.0.1");
#if defined(_WIN32)
    return expect_true(
               selection.selected_port > 0 && selection.selected_port <= 65535,
               "Windows should assign a bindable UDP port for automatic DHT listening")
        && expect_true(
            selection.os_assigned,
            "automatic Windows DHT port should be marked as OS assigned")
        && expect_true(
            selection.error_detail.empty(),
            "successful Windows DHT port selection should not retain an error");
#else
    return expect_true(selection.selected_port == 0, "non-Windows auto selection should remain delegated");
#endif
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_topic_derivation_is_stable_and_redacted() && ok;
    ok = test_routing_token_derivation_is_stable_and_lane_scoped() && ok;
    ok = test_in_memory_publish_fetch_roundtrip() && ok;
    ok = test_publisher_instance_id_generation_and_validation() && ok;
    ok = test_v3_plaintext_remains_parseable_but_has_no_instance_identity() && ok;
    ok = test_description_tag_distinguishes_descriptions() && ok;
    ok = test_publish_revision_floor_survives_runtime_restart() && ok;
    ok = test_answer_refresh_preserves_offer_correlation() && ok;
    ok = test_wrong_secret_and_expired_payload_are_rejected() && ok;
    ok = test_new_revision_refreshes_record_expiry() && ok;
    ok = test_file_backed_store_writes_encrypted_mailbox_artifact() && ok;
    ok = test_indirect_store_compacts_large_primary_payload_and_roundtrips() && ok;
    ok = test_indirect_store_publishes_small_description_snapshot_directly() && ok;
    ok = test_indirect_retry_reuses_same_blob_metadata_for_same_revision() && ok;
    ok = test_indirect_fetch_falls_back_to_secondary_mailbox_when_primary_misses() && ok;
    ok = test_indirect_store_roundtrips_large_payload_without_secondary_mailbox() && ok;
    ok = test_indirect_chunks_are_content_addressed_for_same_revision() && ok;
    ok = test_bootstrap_nodes_prefer_cached_numeric_fallbacks() && ok;
    ok = test_helper_role_and_path_probe() && ok;
    ok = test_candidate_diagnostics_helpers() && ok;
    ok = test_mutable_get_exposes_verified_intermediate_response() && ok;
    ok = test_mutable_get_authoritative_empty_response_completes_without_record() && ok;
    ok = test_libtorrent_mutable_get_poll_is_non_blocking_by_default() && ok;
    ok = test_dht_listen_port_selection_preserves_fixed_port() && ok;
    ok = test_dht_listen_port_selection_rejects_out_of_range_port() && ok;
    ok = test_dht_listen_port_selection_rejects_invalid_address() && ok;
    ok = test_dht_listen_port_selection_uses_windows_udp_assignment() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_service_dht_rendezvous_tests" << '\n';
    return 0;
}
