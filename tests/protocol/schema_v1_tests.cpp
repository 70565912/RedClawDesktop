#include <iostream>
#include <string>

#include "redclaw/protocol/protocol_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

redclaw::protocol::OfferBlobV1 sample_offer();

bool test_offer_roundtrip() {
    redclaw::protocol::OfferBlobV1 offer;
    offer.session_id = "session-001";
    offer.host_peer_id = "host-alpha";
    offer.controller_peer_id = "controller-beta";
    offer.nonce = "nonce-1234567890ab";
    offer.created_at_ms = 1710000000123ULL;
    offer.description_sdp = "v=0\na=setup:actpass\n";
    offer.ice_ufrag = "ufrag";
    offer.ice_pwd = "pwd-xyz";
    offer.host_fingerprint = "sha256:aa=bb";
    offer.candidates = {
        "candidate:1 1 udp 2122260223 192.168.0.1 50000 typ host",
        "candidate:2 1 tcp 1019216383 10.0.0.10 9 typ host tcptype active"
    };

    const std::string serialized = redclaw::protocol::serialize_offer_blob_v1(offer);
    const auto parsed = redclaw::protocol::parse_offer_blob_v1(serialized);

    return expect_true(parsed.ok, "offer should parse after serialize")
        && expect_true(parsed.value.session_id == offer.session_id, "offer session_id mismatch")
        && expect_true(parsed.value.candidates.size() == offer.candidates.size(), "offer candidates size mismatch")
        && expect_true(parsed.value.host_fingerprint == offer.host_fingerprint, "offer fingerprint mismatch");
}

bool test_trickle_offer_allows_description_before_candidates() {
    auto offer = sample_offer();
    offer.trickle_ice = true;
    offer.candidates.clear();

    const auto encrypted = redclaw::protocol::encrypt_offer_blob_v1(
        offer, "trickle-description-passphrase");
    if (!expect_true(
            encrypted.ok,
            std::string("candidate-free trickle offer should encrypt: ") + encrypted.error)) {
        return false;
    }

    const auto decrypted = redclaw::protocol::decrypt_offer_blob_v1(
        encrypted.value, "trickle-description-passphrase");
    return expect_true(decrypted.ok, "candidate-free trickle offer should decrypt")
        && expect_true(decrypted.value.trickle_ice, "trickle_ice should roundtrip")
        && expect_true(decrypted.value.candidates.empty(), "initial trickle description should have no candidates");
}

bool test_non_trickle_offer_still_requires_candidate() {
    auto offer = sample_offer();
    offer.candidates.clear();

    std::string error;
    return expect_true(
            !redclaw::protocol::validate_offer_blob_v1(offer, &error),
            "legacy non-trickle offer must still reject an empty candidate list")
        && expect_true(error.find("candidate") != std::string::npos,
                       "non-trickle rejection should identify the missing candidate");
}

bool test_trickle_offer_rejects_invalid_boolean() {
    std::string serialized = redclaw::protocol::serialize_offer_blob_v1(sample_offer());
    const std::string valid = "trickle_ice=0";
    const auto position = serialized.find(valid);
    if (!expect_true(position != std::string::npos, "serialized offer should contain trickle_ice")) {
        return false;
    }
    serialized.replace(position, valid.size(), "trickle_ice=2");
    const auto parsed = redclaw::protocol::parse_offer_blob_v1(serialized);
    return expect_true(!parsed.ok, "invalid trickle_ice value must fail parsing");
}

bool test_legacy_offer_without_trickle_field_still_parses() {
    std::string serialized = redclaw::protocol::serialize_offer_blob_v1(sample_offer());
    const std::string field = "trickle_ice=0\n";
    const auto position = serialized.find(field);
    if (!expect_true(position != std::string::npos, "serialized offer should contain optional trickle marker")) {
        return false;
    }
    serialized.erase(position, field.size());
    const auto parsed = redclaw::protocol::parse_offer_blob_v1(serialized);
    return expect_true(parsed.ok, "legacy bundled-candidate offer should remain parseable")
        && expect_true(!parsed.value.trickle_ice, "missing legacy marker should default to bundled candidates");
}

redclaw::protocol::OfferBlobV1 sample_offer() {
    redclaw::protocol::OfferBlobV1 offer;
    offer.session_id = "session-001";
    offer.host_peer_id = "host-alpha";
    offer.controller_peer_id = "controller-beta";
    offer.nonce = "nonce-1234567890ab";
    offer.created_at_ms = 1710000000123ULL;
    offer.description_sdp = "v=0\na=setup:actpass\n";
    offer.ice_ufrag = "ufrag";
    offer.ice_pwd = "pwd-xyz";
    offer.host_fingerprint = "sha256:aa=bb";
    offer.candidates = {
        "candidate:1 1 udp 2122260223 192.168.0.1 50000 typ host",
        "candidate:2 1 tcp 1019216383 10.0.0.10 9 typ host tcptype active"
    };
    return offer;
}

bool test_offer_encryption_roundtrip() {
    const auto offer = sample_offer();
    const std::string passphrase = "correct horse battery staple";

    const auto encrypted = redclaw::protocol::encrypt_offer_blob_v1(offer, passphrase);
    if (!expect_true(encrypted.ok, "offer encryption should succeed")) {
        return false;
    }

    const auto decrypted = redclaw::protocol::decrypt_offer_blob_v1(encrypted.value, passphrase);
    return expect_true(decrypted.ok, "offer decryption should succeed")
        && expect_true(decrypted.value.session_id == offer.session_id, "decrypted session_id mismatch")
        && expect_true(decrypted.value.ice_pwd == offer.ice_pwd, "decrypted ice_pwd mismatch")
        && expect_true(decrypted.value.candidates.size() == offer.candidates.size(), "decrypted candidates mismatch");
}

bool test_offer_encryption_wrong_passphrase() {
    const auto offer = sample_offer();
    const auto encrypted = redclaw::protocol::encrypt_offer_blob_v1(offer, "good-passphrase");
    if (!expect_true(encrypted.ok, "encryption setup should succeed")) {
        return false;
    }

    const auto decrypted = redclaw::protocol::decrypt_offer_blob_v1(encrypted.value, "wrong-passphrase");
    return expect_true(!decrypted.ok, "decryption with wrong passphrase must fail");
}

bool test_offer_encryption_tamper_rejected() {
    const auto offer = sample_offer();
    const auto encrypted = redclaw::protocol::encrypt_offer_blob_v1(offer, "tamper-test-passphrase");
    if (!expect_true(encrypted.ok, "encryption setup should succeed")) {
        return false;
    }

    std::string tampered = encrypted.value;
    const std::size_t pos = tampered.find("ciphertext_b64=");
    if (!expect_true(pos != std::string::npos, "ciphertext field should exist")) {
        return false;
    }

    const std::size_t value_pos = pos + std::string("ciphertext_b64=").size();
    if (value_pos >= tampered.size()) {
        return expect_true(false, "ciphertext value position out of range");
    }

    tampered[value_pos] = (tampered[value_pos] == 'A') ? 'B' : 'A';
    const auto decrypted = redclaw::protocol::decrypt_offer_blob_v1(tampered, "tamper-test-passphrase");
    return expect_true(!decrypted.ok, "tampered ciphertext must fail authentication");
}

bool test_qr_offer_roundtrip() {
    const auto offer = sample_offer();
    const auto encrypted = redclaw::protocol::encrypt_offer_blob_v1(offer, "qr-passphrase");
    if (!expect_true(encrypted.ok, "encrypted offer for qr should be generated")) {
        return false;
    }

    const auto qr = redclaw::protocol::encode_offer_for_qr_v1(encrypted.value);
    if (!expect_true(qr.ok, "qr encoding should succeed")) {
        return false;
    }

    const auto decoded_offer_blob = redclaw::protocol::decode_offer_from_qr_v1(qr.value);
    if (!expect_true(decoded_offer_blob.ok, "qr decoding should succeed")) {
        return false;
    }

    const auto decrypted = redclaw::protocol::decrypt_offer_blob_v1(decoded_offer_blob.value, "qr-passphrase");
    return expect_true(decrypted.ok, "decrypted offer from qr should succeed")
        && expect_true(decrypted.value.session_id == offer.session_id, "qr decrypted session mismatch");
}

bool test_qr_offer_tamper_rejected() {
    const auto offer = sample_offer();
    const auto encrypted = redclaw::protocol::encrypt_offer_blob_v1(offer, "qr-passphrase");
    if (!expect_true(encrypted.ok, "encrypted offer for tamper test should be generated")) {
        return false;
    }

    const auto qr = redclaw::protocol::encode_offer_for_qr_v1(encrypted.value);
    if (!expect_true(qr.ok, "qr encoding should succeed")) {
        return false;
    }

    std::string tampered = qr.value;
    const std::size_t pos = tampered.find("payload_b64=");
    if (!expect_true(pos != std::string::npos, "payload_b64 should exist")) {
        return false;
    }

    const std::size_t value_pos = pos + std::string("payload_b64=").size();
    if (!expect_true(value_pos < tampered.size(), "payload value position should be valid")) {
        return false;
    }

    tampered[value_pos] = (tampered[value_pos] == 'A') ? 'B' : 'A';

    const auto decoded = redclaw::protocol::decode_offer_from_qr_v1(tampered);
    return expect_true(!decoded.ok, "tampered qr payload should fail checksum validation");
}

bool test_offer_version_rejection() {
    const std::string payload =
        "RCD-OFFER-V1\n"
        "schema_version=999\n"
        "session_id=s\n"
        "host_peer_id=h\n"
        "controller_peer_id=c\n"
        "nonce=nonce-123456\n"
        "created_at_ms=1\n"
        "description_sdp=v=0\n"
        "ice_ufrag=u\n"
        "ice_pwd=p\n"
        "host_fingerprint=f\n"
        "candidate_count=1\n"
        "candidate_0=x\n";

    const auto parsed = redclaw::protocol::parse_offer_blob_v1(payload);
    return expect_true(!parsed.ok, "unsupported offer schema version must fail parsing");
}

bool test_handshake_roundtrip() {
    redclaw::protocol::HandshakeMessageV1 message;
    message.session_id = "session-001";
    message.nonce = "nonce-abcdef123456";
    message.created_at_ms = 1710000000999ULL;
    message.initiator_peer_id = "host-alpha";
    message.responder_peer_id = "controller-beta";
    message.responder_fingerprint = "sha256:deadbeef";

    const std::string serialized = redclaw::protocol::serialize_handshake_message_v1(message);
    const auto parsed = redclaw::protocol::parse_handshake_message_v1(serialized);

    return expect_true(parsed.ok, "handshake should parse after serialize")
        && expect_true(parsed.value.nonce == message.nonce, "handshake nonce mismatch");
}

bool test_handshake_nonce_policy_rejects_short_nonce() {
    redclaw::protocol::HandshakeMessageV1 message;
    message.session_id = "session-001";
    message.nonce = "nonce-1";
    message.created_at_ms = 1710000000999ULL;
    message.initiator_peer_id = "host-alpha";
    message.responder_peer_id = "controller-beta";
    message.responder_fingerprint = "sha256:deadbeef";

    std::string error;
    const bool ok = redclaw::protocol::validate_handshake_message_v1(message, &error);
    return expect_true(!ok, "short handshake nonce must be rejected")
        && expect_true(error.find("nonce") != std::string::npos, "short nonce rejection should mention nonce");
}

bool test_handshake_nonce_policy_rejects_invalid_character() {
    redclaw::protocol::HandshakeMessageV1 message;
    message.session_id = "session-001";
    message.nonce = "nonce bad12345678";
    message.created_at_ms = 1710000000999ULL;
    message.initiator_peer_id = "host-alpha";
    message.responder_peer_id = "controller-beta";
    message.responder_fingerprint = "sha256:deadbeef";

    std::string error;
    const bool ok = redclaw::protocol::validate_handshake_message_v1(message, &error);
    return expect_true(!ok, "nonce with unsupported characters must be rejected")
        && expect_true(error.find("nonce") != std::string::npos, "invalid nonce rejection should mention nonce");
}

bool test_handshake_nonce_policy_rejects_too_long_nonce() {
    redclaw::protocol::HandshakeMessageV1 message;
    message.session_id = "session-001";
    message.nonce = std::string(129, 'a');
    message.created_at_ms = 1710000000999ULL;
    message.initiator_peer_id = "host-alpha";
    message.responder_peer_id = "controller-beta";
    message.responder_fingerprint = "sha256:deadbeef";

    std::string error;
    const bool ok = redclaw::protocol::validate_handshake_message_v1(message, &error);
    return expect_true(!ok, "overlong nonce must be rejected")
        && expect_true(error.find("nonce") != std::string::npos, "overlong nonce rejection should mention nonce");
}

bool test_session_state_roundtrip() {
    redclaw::protocol::SessionStateSnapshotV1 snapshot;
    snapshot.session_id = "session-001";
    snapshot.state = redclaw::protocol::SessionStateV1::connecting;
    snapshot.revision = 7;
    snapshot.controller_attached = true;

    const std::string serialized = redclaw::protocol::serialize_session_state_snapshot_v1(snapshot);
    const auto parsed = redclaw::protocol::parse_session_state_snapshot_v1(serialized);

    return expect_true(parsed.ok, "session snapshot should parse after serialize")
        && expect_true(parsed.value.revision == snapshot.revision, "session revision mismatch")
        && expect_true(parsed.value.controller_attached, "session controller_attached mismatch");
}

bool test_uac_consent_envelope_roundtrip() {
    redclaw::protocol::UacConsentEnvelopeV1 message;
    message.session_id = "session-001";
    message.token_id = "grant-123";
    message.uac_prompt_id = "uac-prompt-abc";
    message.decision = redclaw::protocol::UacConsentDecisionV1::allow;

    const std::string serialized = redclaw::protocol::serialize_uac_consent_envelope_v1(message);
    const auto parsed = redclaw::protocol::parse_uac_consent_envelope_v1(serialized);

    return expect_true(parsed.ok, "uac consent envelope should parse after serialize")
        && expect_true(parsed.value.session_id == message.session_id, "uac consent session_id mismatch")
        && expect_true(parsed.value.token_id == message.token_id, "uac consent token_id mismatch")
        && expect_true(parsed.value.uac_prompt_id == message.uac_prompt_id, "uac consent prompt id mismatch")
        && expect_true(
            parsed.value.decision == redclaw::protocol::UacConsentDecisionV1::allow,
            "uac consent decision mismatch");
}

bool test_uac_consent_envelope_rejects_invalid_decision() {
    const std::string payload =
        "RCD-UAC-CONSENT-V1\n"
        "schema_version=1\n"
        "session_id=s\n"
        "token_id=t\n"
        "uac_prompt_id=p\n"
        "decision=unsupported\n";

    const auto parsed = redclaw::protocol::parse_uac_consent_envelope_v1(payload);
    return expect_true(!parsed.ok, "invalid uac consent decision must fail parsing");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_offer_roundtrip() && ok;
    ok = test_trickle_offer_allows_description_before_candidates() && ok;
    ok = test_non_trickle_offer_still_requires_candidate() && ok;
    ok = test_trickle_offer_rejects_invalid_boolean() && ok;
    ok = test_legacy_offer_without_trickle_field_still_parses() && ok;
    ok = test_offer_encryption_roundtrip() && ok;
    ok = test_offer_encryption_wrong_passphrase() && ok;
    ok = test_offer_encryption_tamper_rejected() && ok;
    ok = test_qr_offer_roundtrip() && ok;
    ok = test_qr_offer_tamper_rejected() && ok;
    ok = test_offer_version_rejection() && ok;
    ok = test_handshake_roundtrip() && ok;
    ok = test_handshake_nonce_policy_rejects_short_nonce() && ok;
    ok = test_handshake_nonce_policy_rejects_invalid_character() && ok;
    ok = test_handshake_nonce_policy_rejects_too_long_nonce() && ok;
    ok = test_session_state_roundtrip() && ok;
    ok = test_uac_consent_envelope_roundtrip() && ok;
    ok = test_uac_consent_envelope_rejects_invalid_decision() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_protocol_schema_v1_tests" << '\n';
    return 0;
}
