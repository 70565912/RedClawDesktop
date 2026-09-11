#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace redclaw::protocol {

inline constexpr int kSchemaVersionV1 = 1;

enum class SessionStateV1 {
	idle,
	offering,
	connecting,
	established,
	recovering,
	terminated,
};

struct OfferBlobV1 {
	int schema_version = kSchemaVersionV1;
	std::string session_id;
	std::string host_peer_id;
	std::string controller_peer_id;
	std::string nonce;
	std::uint64_t created_at_ms = 0;
	std::string description_sdp;
	std::string ice_ufrag;
	std::string ice_pwd;
	std::string host_fingerprint;
	bool trickle_ice = false;
	std::vector<std::string> candidates;
};

struct HandshakeMessageV1 {
	int schema_version = kSchemaVersionV1;
	std::string session_id;
	std::string nonce;
	std::uint64_t created_at_ms = 0;
	std::string initiator_peer_id;
	std::string responder_peer_id;
	std::string responder_fingerprint;
};

struct SessionStateSnapshotV1 {
	int schema_version = kSchemaVersionV1;
	std::string session_id;
	SessionStateV1 state = SessionStateV1::idle;
	std::uint64_t revision = 0;
	bool controller_attached = false;
};

enum class UacConsentDecisionV1 {
	allow,
	deny,
	timeout,
};

struct UacConsentEnvelopeV1 {
	int schema_version = kSchemaVersionV1;
	std::string session_id;
	std::string token_id;
	std::string uac_prompt_id;
	UacConsentDecisionV1 decision = UacConsentDecisionV1::deny;
};

struct EncryptedOfferBlobV1 {
	int schema_version = kSchemaVersionV1;
	std::string kdf = "pbkdf2-sha256";
	int pbkdf2_iterations = 120000;
	std::string salt_b64;
	std::string iv_b64;
	std::string tag_b64;
	std::string ciphertext_b64;
};

struct QrPayloadV1 {
	int schema_version = kSchemaVersionV1;
	std::string purpose = "offer";
	int frame_index = 0;
	int total_frames = 1;
	std::string payload_b64;
	std::string checksum_hex;
};

template <typename TValue>
struct ParseResult {
	bool ok = false;
	TValue value {};
	std::string error;
};

bool validate_offer_blob_v1(const OfferBlobV1& offer, std::string* error = nullptr);
bool validate_handshake_message_v1(const HandshakeMessageV1& message, std::string* error = nullptr);
bool validate_session_state_snapshot_v1(const SessionStateSnapshotV1& snapshot, std::string* error = nullptr);
bool validate_uac_consent_envelope_v1(const UacConsentEnvelopeV1& envelope, std::string* error = nullptr);

std::string serialize_offer_blob_v1(const OfferBlobV1& offer);
std::string serialize_handshake_message_v1(const HandshakeMessageV1& message);
std::string serialize_session_state_snapshot_v1(const SessionStateSnapshotV1& snapshot);
std::string serialize_uac_consent_envelope_v1(const UacConsentEnvelopeV1& envelope);

ParseResult<OfferBlobV1> parse_offer_blob_v1(std::string_view serialized);
ParseResult<HandshakeMessageV1> parse_handshake_message_v1(std::string_view serialized);
ParseResult<SessionStateSnapshotV1> parse_session_state_snapshot_v1(std::string_view serialized);
ParseResult<UacConsentEnvelopeV1> parse_uac_consent_envelope_v1(std::string_view serialized);

std::string serialize_encrypted_offer_blob_v1(const EncryptedOfferBlobV1& encrypted);
ParseResult<EncryptedOfferBlobV1> parse_encrypted_offer_blob_v1(std::string_view serialized);

ParseResult<std::string> encrypt_offer_blob_v1(const OfferBlobV1& offer, std::string_view passphrase);
ParseResult<OfferBlobV1> decrypt_offer_blob_v1(std::string_view encrypted_blob, std::string_view passphrase);

std::string serialize_qr_payload_v1(const QrPayloadV1& payload);
ParseResult<QrPayloadV1> parse_qr_payload_v1(std::string_view serialized);

ParseResult<std::string> encode_offer_for_qr_v1(std::string_view encrypted_offer_blob);
ParseResult<std::string> decode_offer_from_qr_v1(std::string_view qr_payload);

std::string to_string(SessionStateV1 state);
ParseResult<SessionStateV1> parse_session_state_v1(std::string_view value);
std::string to_string(UacConsentDecisionV1 decision);
ParseResult<UacConsentDecisionV1> parse_uac_consent_decision_v1(std::string_view value);

std::string_view module_name();

}
