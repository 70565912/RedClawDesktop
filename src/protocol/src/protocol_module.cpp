#include "redclaw/protocol/protocol_module.h"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace redclaw::protocol {

namespace {

using FieldMap = std::unordered_map<std::string, std::string>;

constexpr std::string_view kHeaderOffer = "RCD-OFFER-V1";
constexpr std::string_view kHeaderHandshake = "RCD-HANDSHAKE-V1";
constexpr std::string_view kHeaderSession = "RCD-SESSION-V1";
constexpr std::string_view kHeaderUacConsent = "RCD-UAC-CONSENT-V1";
constexpr std::string_view kHeaderEncryptedOffer = "RCD-OFFER-ENC-V1";
constexpr std::string_view kHeaderQrPayload = "RCD-QR-V1";

constexpr int kPbkdf2Iterations = 120000;
constexpr int kSaltSize = 16;
constexpr int kAes256KeySize = 32;
constexpr int kAesGcmIvSize = 12;
constexpr int kAesGcmTagSize = 16;
constexpr std::size_t kNonceMinLength = 16;
constexpr std::size_t kNonceMaxLength = 128;

bool is_nonce_char_allowed(char ch) {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

bool validate_nonce_policy(std::string_view nonce, std::string* error) {
    if (nonce.size() < kNonceMinLength || nonce.size() > kNonceMaxLength) {
        if (error != nullptr) {
            *error = "nonce length must be within [16, 128]";
        }
        return false;
    }

    bool has_alpha = false;
    bool has_digit = false;

    for (char ch : nonce) {
        const auto uch = static_cast<unsigned char>(ch);
        if (!is_nonce_char_allowed(ch)) {
            if (error != nullptr) {
                *error = "nonce contains unsupported character";
            }
            return false;
        }

        has_alpha = has_alpha || (std::isalpha(uch) != 0);
        has_digit = has_digit || (std::isdigit(uch) != 0);
    }

    if (!has_alpha || !has_digit) {
        if (error != nullptr) {
            *error = "nonce must include at least one letter and one digit";
        }
        return false;
    }

    return true;
}

std::string trim(std::string_view value) {
    std::size_t start = 0;
    std::size_t end = value.size();

    while (start < end && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(start, end - start));
}

std::string escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());

    for (char ch : value) {
        if (ch == '\\' || ch == '=' || ch == '\n') {
            out.push_back('\\');
            if (ch == '\n') {
                out.push_back('n');
            } else {
                out.push_back(ch);
            }
            continue;
        }
        out.push_back(ch);
    }

    return out;
}

ParseResult<std::string> unescape(std::string_view value) {
    ParseResult<std::string> result;
    result.ok = true;
    result.value.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch != '\\') {
            result.value.push_back(ch);
            continue;
        }

        if (i + 1 >= value.size()) {
            result.ok = false;
            result.error = "dangling escape sequence";
            return result;
        }

        const char escaped = value[++i];
        if (escaped == 'n') {
            result.value.push_back('\n');
        } else if (escaped == '\\' || escaped == '=') {
            result.value.push_back(escaped);
        } else {
            result.ok = false;
            result.error = "unsupported escape sequence";
            return result;
        }
    }

    return result;
}

void append_field(std::ostringstream& out, std::string_view key, std::string_view value) {
    out << key << '=' << escape(value) << '\n';
}

void append_field(std::ostringstream& out, std::string_view key, std::uint64_t value) {
    out << key << '=' << value << '\n';
}

void append_field(std::ostringstream& out, std::string_view key, int value) {
    out << key << '=' << value << '\n';
}

ParseResult<FieldMap> parse_key_values(std::string_view serialized, std::string_view expected_header) {
    ParseResult<FieldMap> result;

    std::istringstream stream{std::string(serialized)};
    std::string line;

    if (!std::getline(stream, line)) {
        result.error = "missing header";
        return result;
    }

    if (trim(line) != expected_header) {
        result.error = "unexpected schema header";
        return result;
    }

    FieldMap fields;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }

        const std::size_t split = line.find('=');
        if (split == std::string::npos || split == 0) {
            result.error = "invalid key-value line";
            return result;
        }

        const std::string key = trim(line.substr(0, split));
        const auto decoded = unescape(std::string_view(line).substr(split + 1));
        if (!decoded.ok) {
            result.error = "failed to decode value for key: " + key;
            return result;
        }

        fields[key] = decoded.value;
    }

    result.ok = true;
    result.value = std::move(fields);
    return result;
}

std::optional<std::string> required_text(const FieldMap& fields, const char* key, std::string* error) {
    const auto it = fields.find(key);
    if (it == fields.end() || it->second.empty()) {
        if (error != nullptr) {
            *error = std::string("missing required field: ") + key;
        }
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::uint64_t> required_u64(const FieldMap& fields, const char* key, std::string* error) {
    const auto it = fields.find(key);
    if (it == fields.end() || it->second.empty()) {
        if (error != nullptr) {
            *error = std::string("missing required field: ") + key;
        }
        return std::nullopt;
    }

    std::uint64_t out = 0;
    const char* begin = it->second.data();
    const char* end = begin + it->second.size();
    const auto parse = std::from_chars(begin, end, out);
    if (parse.ec != std::errc{} || parse.ptr != end) {
        if (error != nullptr) {
            *error = std::string("invalid uint64 field: ") + key;
        }
        return std::nullopt;
    }

    return out;
}

std::optional<int> required_int(const FieldMap& fields, const char* key, std::string* error) {
    const auto it = fields.find(key);
    if (it == fields.end() || it->second.empty()) {
        if (error != nullptr) {
            *error = std::string("missing required field: ") + key;
        }
        return std::nullopt;
    }

    int out = 0;
    const char* begin = it->second.data();
    const char* end = begin + it->second.size();
    const auto parse = std::from_chars(begin, end, out);
    if (parse.ec != std::errc{} || parse.ptr != end) {
        if (error != nullptr) {
            *error = std::string("invalid int field: ") + key;
        }
        return std::nullopt;
    }

    return out;
}

ParseResult<std::string> base64_encode(const std::vector<unsigned char>& data) {
    ParseResult<std::string> result;
    if (data.empty()) {
        result.ok = true;
        return result;
    }

    std::string encoded;
    encoded.resize(4 * ((data.size() + 2) / 3));

    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        data.data(),
        static_cast<int>(data.size()));

    if (written < 0) {
        result.error = "base64 encode failed";
        return result;
    }

    encoded.resize(static_cast<std::size_t>(written));
    result.ok = true;
    result.value = std::move(encoded);
    return result;
}

ParseResult<std::vector<unsigned char>> base64_decode(std::string_view input) {
    ParseResult<std::vector<unsigned char>> result;
    if (input.empty()) {
        result.ok = true;
        return result;
    }

    std::vector<unsigned char> decoded((input.size() * 3) / 4 + 3);
    const int raw_len = EVP_DecodeBlock(
        decoded.data(),
        reinterpret_cast<const unsigned char*>(input.data()),
        static_cast<int>(input.size()));

    if (raw_len < 0) {
        result.error = "base64 decode failed";
        return result;
    }

    int padding = 0;
    if (!input.empty() && input.back() == '=') {
        ++padding;
    }
    if (input.size() >= 2 && input[input.size() - 2] == '=') {
        ++padding;
    }

    const int final_len = raw_len - padding;
    if (final_len < 0) {
        result.error = "invalid base64 payload";
        return result;
    }

    decoded.resize(static_cast<std::size_t>(final_len));
    result.ok = true;
    result.value = std::move(decoded);
    return result;
}

bool fill_random(std::vector<unsigned char>* buffer, std::string* error) {
    if (buffer == nullptr || buffer->empty()) {
        if (error != nullptr) {
            *error = "random buffer is empty";
        }
        return false;
    }

    if (RAND_bytes(buffer->data(), static_cast<int>(buffer->size())) != 1) {
        if (error != nullptr) {
            *error = "RAND_bytes failed";
        }
        return false;
    }
    return true;
}

bool derive_key_pbkdf2(
    std::string_view passphrase,
    const std::vector<unsigned char>& salt,
    int iterations,
    std::vector<unsigned char>* key,
    std::string* error) {
    if (passphrase.empty()) {
        if (error != nullptr) {
            *error = "passphrase must not be empty";
        }
        return false;
    }
    if (key == nullptr || key->size() != kAes256KeySize) {
        if (error != nullptr) {
            *error = "invalid key buffer size";
        }
        return false;
    }
    if (iterations <= 0) {
        if (error != nullptr) {
            *error = "invalid pbkdf2 iteration count";
        }
        return false;
    }

    const int ok = PKCS5_PBKDF2_HMAC(
        passphrase.data(),
        static_cast<int>(passphrase.size()),
        salt.data(),
        static_cast<int>(salt.size()),
        iterations,
        EVP_sha256(),
        static_cast<int>(key->size()),
        key->data());

    if (ok != 1) {
        if (error != nullptr) {
            *error = "PBKDF2 derivation failed";
        }
        return false;
    }
    return true;
}

ParseResult<std::vector<unsigned char>> aes256_gcm_encrypt(
    std::string_view plaintext,
    const std::vector<unsigned char>& key,
    const std::vector<unsigned char>& iv) {
    ParseResult<std::vector<unsigned char>> result;
    if (key.size() != kAes256KeySize || iv.size() != kAesGcmIvSize) {
        result.error = "invalid key or iv size";
        return result;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        result.error = "EVP_CIPHER_CTX_new failed";
        return result;
    }

    std::vector<unsigned char> output(plaintext.size() + kAesGcmTagSize);
    int out_len = 0;
    int total = 0;

    const bool ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) == 1 &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) == 1 &&
        EVP_EncryptUpdate(ctx, output.data(), &out_len, reinterpret_cast<const unsigned char*>(plaintext.data()), static_cast<int>(plaintext.size())) == 1;

    if (!ok) {
        EVP_CIPHER_CTX_free(ctx);
        result.error = "AES-GCM encrypt update failed";
        return result;
    }

    total = out_len;
    if (EVP_EncryptFinal_ex(ctx, output.data() + total, &out_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        result.error = "AES-GCM encrypt final failed";
        return result;
    }
    total += out_len;

    std::vector<unsigned char> tag(kAesGcmTagSize);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        result.error = "AES-GCM get tag failed";
        return result;
    }

    EVP_CIPHER_CTX_free(ctx);

    output.resize(static_cast<std::size_t>(total));
    output.insert(output.end(), tag.begin(), tag.end());

    result.ok = true;
    result.value = std::move(output);
    return result;
}

ParseResult<std::vector<unsigned char>> aes256_gcm_decrypt(
    const std::vector<unsigned char>& ciphertext,
    const std::vector<unsigned char>& tag,
    const std::vector<unsigned char>& key,
    const std::vector<unsigned char>& iv) {
    ParseResult<std::vector<unsigned char>> result;
    if (key.size() != kAes256KeySize || iv.size() != kAesGcmIvSize || tag.size() != kAesGcmTagSize) {
        result.error = "invalid key/iv/tag size";
        return result;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        result.error = "EVP_CIPHER_CTX_new failed";
        return result;
    }

    std::vector<unsigned char> plaintext(ciphertext.size() + 1);
    int out_len = 0;
    int total = 0;

    const bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) == 1 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) == 1 &&
        EVP_DecryptUpdate(ctx, plaintext.data(), &out_len, ciphertext.data(), static_cast<int>(ciphertext.size())) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()), const_cast<unsigned char*>(tag.data())) == 1;

    if (!ok) {
        EVP_CIPHER_CTX_free(ctx);
        result.error = "AES-GCM decrypt update failed";
        return result;
    }

    total = out_len;
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + total, &out_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        result.error = "authentication check failed";
        return result;
    }
    total += out_len;

    EVP_CIPHER_CTX_free(ctx);

    plaintext.resize(static_cast<std::size_t>(total));
    result.ok = true;
    result.value = std::move(plaintext);
    return result;
}

std::uint32_t fnv1a_32(std::string_view input) {
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : input) {
        hash ^= static_cast<std::uint32_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

std::string checksum_hex(std::string_view input) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << fnv1a_32(input);
    return out.str();
}

}  // namespace

bool validate_offer_blob_v1(const OfferBlobV1& offer, std::string* error) {
    if (offer.schema_version != kSchemaVersionV1) {
        if (error != nullptr) {
            *error = "unsupported offer schema version";
        }
        return false;
    }
    if (offer.session_id.empty() || offer.host_peer_id.empty() || offer.controller_peer_id.empty()) {
        if (error != nullptr) {
            *error = "session and peer ids must be non-empty";
        }
        return false;
    }
    if (!validate_nonce_policy(offer.nonce, error)) {
        return false;
    }
    if (offer.created_at_ms == 0) {
        if (error != nullptr) {
            *error = "created_at_ms must be non-zero";
        }
        return false;
    }
    if (offer.description_sdp.empty()) {
        if (error != nullptr) {
            *error = "description_sdp is required";
        }
        return false;
    }
    if (offer.ice_ufrag.empty() || offer.ice_pwd.empty() || offer.host_fingerprint.empty()) {
        if (error != nullptr) {
            *error = "ICE credentials and fingerprint are required";
        }
        return false;
    }
    if (offer.candidates.empty() && !offer.trickle_ice) {
        if (error != nullptr) {
            *error = "at least one candidate is required unless trickle ICE is enabled";
        }
        return false;
    }
    return true;
}

bool validate_handshake_message_v1(const HandshakeMessageV1& message, std::string* error) {
    if (message.schema_version != kSchemaVersionV1) {
        if (error != nullptr) {
            *error = "unsupported handshake schema version";
        }
        return false;
    }
    if (message.session_id.empty() || message.initiator_peer_id.empty() || message.responder_peer_id.empty()) {
        if (error != nullptr) {
            *error = "session and peer ids must be non-empty";
        }
        return false;
    }
    if (!validate_nonce_policy(message.nonce, error)) {
        return false;
    }
    if (message.created_at_ms == 0 || message.responder_fingerprint.empty()) {
        if (error != nullptr) {
            *error = "timestamp and responder fingerprint are required";
        }
        return false;
    }
    return true;
}

bool validate_session_state_snapshot_v1(const SessionStateSnapshotV1& snapshot, std::string* error) {
    if (snapshot.schema_version != kSchemaVersionV1) {
        if (error != nullptr) {
            *error = "unsupported session schema version";
        }
        return false;
    }
    if (snapshot.session_id.empty()) {
        if (error != nullptr) {
            *error = "session_id is required";
        }
        return false;
    }
    if (snapshot.revision == 0) {
        if (error != nullptr) {
            *error = "revision must be non-zero";
        }
        return false;
    }
    return true;
}

bool validate_uac_consent_envelope_v1(const UacConsentEnvelopeV1& envelope, std::string* error) {
    if (envelope.schema_version != kSchemaVersionV1) {
        if (error != nullptr) {
            *error = "unsupported uac consent schema version";
        }
        return false;
    }

    if (envelope.session_id.empty() || envelope.token_id.empty() || envelope.uac_prompt_id.empty()) {
        if (error != nullptr) {
            *error = "session_id, token_id and uac_prompt_id are required";
        }
        return false;
    }

    return true;
}

std::string serialize_offer_blob_v1(const OfferBlobV1& offer) {
    std::ostringstream out;
    out << kHeaderOffer << '\n';
    append_field(out, "schema_version", offer.schema_version);
    append_field(out, "session_id", offer.session_id);
    append_field(out, "host_peer_id", offer.host_peer_id);
    append_field(out, "controller_peer_id", offer.controller_peer_id);
    append_field(out, "nonce", offer.nonce);
    append_field(out, "created_at_ms", offer.created_at_ms);
    append_field(out, "description_sdp", offer.description_sdp);
    append_field(out, "ice_ufrag", offer.ice_ufrag);
    append_field(out, "ice_pwd", offer.ice_pwd);
    append_field(out, "host_fingerprint", offer.host_fingerprint);
    append_field(out, "trickle_ice", offer.trickle_ice ? 1 : 0);
    append_field(out, "candidate_count", static_cast<int>(offer.candidates.size()));

    for (std::size_t i = 0; i < offer.candidates.size(); ++i) {
        append_field(out, "candidate_" + std::to_string(i), offer.candidates[i]);
    }
    return out.str();
}

std::string serialize_handshake_message_v1(const HandshakeMessageV1& message) {
    std::ostringstream out;
    out << kHeaderHandshake << '\n';
    append_field(out, "schema_version", message.schema_version);
    append_field(out, "session_id", message.session_id);
    append_field(out, "nonce", message.nonce);
    append_field(out, "created_at_ms", message.created_at_ms);
    append_field(out, "initiator_peer_id", message.initiator_peer_id);
    append_field(out, "responder_peer_id", message.responder_peer_id);
    append_field(out, "responder_fingerprint", message.responder_fingerprint);
    return out.str();
}

std::string serialize_session_state_snapshot_v1(const SessionStateSnapshotV1& snapshot) {
    std::ostringstream out;
    out << kHeaderSession << '\n';
    append_field(out, "schema_version", snapshot.schema_version);
    append_field(out, "session_id", snapshot.session_id);
    append_field(out, "state", to_string(snapshot.state));
    append_field(out, "revision", snapshot.revision);
    append_field(out, "controller_attached", snapshot.controller_attached ? 1 : 0);
    return out.str();
}

std::string serialize_uac_consent_envelope_v1(const UacConsentEnvelopeV1& envelope) {
    std::ostringstream out;
    out << kHeaderUacConsent << '\n';
    append_field(out, "schema_version", envelope.schema_version);
    append_field(out, "session_id", envelope.session_id);
    append_field(out, "token_id", envelope.token_id);
    append_field(out, "uac_prompt_id", envelope.uac_prompt_id);
    append_field(out, "decision", to_string(envelope.decision));
    return out.str();
}

ParseResult<OfferBlobV1> parse_offer_blob_v1(std::string_view serialized) {
    ParseResult<OfferBlobV1> result;
    const auto parsed = parse_key_values(serialized, kHeaderOffer);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }

    OfferBlobV1 offer;
    std::string error;

    const auto version = required_int(parsed.value, "schema_version", &error);
    const auto session_id = required_text(parsed.value, "session_id", &error);
    const auto host_peer_id = required_text(parsed.value, "host_peer_id", &error);
    const auto controller_peer_id = required_text(parsed.value, "controller_peer_id", &error);
    const auto nonce = required_text(parsed.value, "nonce", &error);
    const auto created_at_ms = required_u64(parsed.value, "created_at_ms", &error);
    const auto description_sdp = required_text(parsed.value, "description_sdp", &error);
    const auto ice_ufrag = required_text(parsed.value, "ice_ufrag", &error);
    const auto ice_pwd = required_text(parsed.value, "ice_pwd", &error);
    const auto host_fingerprint = required_text(parsed.value, "host_fingerprint", &error);
    const auto candidate_count = required_int(parsed.value, "candidate_count", &error);

    if (!version || !session_id || !host_peer_id || !controller_peer_id || !nonce || !created_at_ms || !description_sdp || !ice_ufrag || !ice_pwd || !host_fingerprint || !candidate_count) {
        result.error = error;
        return result;
    }

    bool trickle_ice = false;
    const auto trickle_it = parsed.value.find("trickle_ice");
    if (trickle_it != parsed.value.end()) {
        const auto parsed_trickle = required_int(parsed.value, "trickle_ice", &error);
        if (!parsed_trickle || (*parsed_trickle != 0 && *parsed_trickle != 1)) {
            result.error = parsed_trickle ? "trickle_ice must be 0 or 1" : error;
            return result;
        }
        trickle_ice = *parsed_trickle == 1;
    }

    if (*candidate_count < 0 || (*candidate_count == 0 && !trickle_ice)) {
        result.error = trickle_ice
            ? "candidate_count must not be negative"
            : "candidate_count must be positive unless trickle ICE is enabled";
        return result;
    }

    offer.schema_version = *version;
    offer.session_id = *session_id;
    offer.host_peer_id = *host_peer_id;
    offer.controller_peer_id = *controller_peer_id;
    offer.nonce = *nonce;
    offer.created_at_ms = *created_at_ms;
    offer.description_sdp = *description_sdp;
    offer.ice_ufrag = *ice_ufrag;
    offer.ice_pwd = *ice_pwd;
    offer.host_fingerprint = *host_fingerprint;
    offer.trickle_ice = trickle_ice;

    for (int i = 0; i < *candidate_count; ++i) {
        const std::string key = "candidate_" + std::to_string(i);
        const auto candidate = required_text(parsed.value, key.c_str(), &error);
        if (!candidate) {
            result.error = error;
            return result;
        }
        offer.candidates.push_back(*candidate);
    }

    if (!validate_offer_blob_v1(offer, &error)) {
        result.error = error;
        return result;
    }

    result.ok = true;
    result.value = std::move(offer);
    return result;
}

ParseResult<HandshakeMessageV1> parse_handshake_message_v1(std::string_view serialized) {
    ParseResult<HandshakeMessageV1> result;
    const auto parsed = parse_key_values(serialized, kHeaderHandshake);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }

    HandshakeMessageV1 message;
    std::string error;

    const auto version = required_int(parsed.value, "schema_version", &error);
    const auto session_id = required_text(parsed.value, "session_id", &error);
    const auto nonce = required_text(parsed.value, "nonce", &error);
    const auto created_at_ms = required_u64(parsed.value, "created_at_ms", &error);
    const auto initiator_peer_id = required_text(parsed.value, "initiator_peer_id", &error);
    const auto responder_peer_id = required_text(parsed.value, "responder_peer_id", &error);
    const auto responder_fingerprint = required_text(parsed.value, "responder_fingerprint", &error);

    if (!version || !session_id || !nonce || !created_at_ms || !initiator_peer_id || !responder_peer_id || !responder_fingerprint) {
        result.error = error;
        return result;
    }

    message.schema_version = *version;
    message.session_id = *session_id;
    message.nonce = *nonce;
    message.created_at_ms = *created_at_ms;
    message.initiator_peer_id = *initiator_peer_id;
    message.responder_peer_id = *responder_peer_id;
    message.responder_fingerprint = *responder_fingerprint;

    if (!validate_handshake_message_v1(message, &error)) {
        result.error = error;
        return result;
    }

    result.ok = true;
    result.value = std::move(message);
    return result;
}

ParseResult<SessionStateSnapshotV1> parse_session_state_snapshot_v1(std::string_view serialized) {
    ParseResult<SessionStateSnapshotV1> result;
    const auto parsed = parse_key_values(serialized, kHeaderSession);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }

    SessionStateSnapshotV1 snapshot;
    std::string error;

    const auto version = required_int(parsed.value, "schema_version", &error);
    const auto session_id = required_text(parsed.value, "session_id", &error);
    const auto state_raw = required_text(parsed.value, "state", &error);
    const auto revision = required_u64(parsed.value, "revision", &error);
    const auto controller_attached = required_int(parsed.value, "controller_attached", &error);

    if (!version || !session_id || !state_raw || !revision || !controller_attached) {
        result.error = error;
        return result;
    }

    const auto state = parse_session_state_v1(*state_raw);
    if (!state.ok) {
        result.error = state.error;
        return result;
    }

    if (*controller_attached != 0 && *controller_attached != 1) {
        result.error = "controller_attached must be 0 or 1";
        return result;
    }

    snapshot.schema_version = *version;
    snapshot.session_id = *session_id;
    snapshot.state = state.value;
    snapshot.revision = *revision;
    snapshot.controller_attached = (*controller_attached == 1);

    if (!validate_session_state_snapshot_v1(snapshot, &error)) {
        result.error = error;
        return result;
    }

    result.ok = true;
    result.value = std::move(snapshot);
    return result;
}

ParseResult<UacConsentEnvelopeV1> parse_uac_consent_envelope_v1(std::string_view serialized) {
    ParseResult<UacConsentEnvelopeV1> result;
    const auto parsed = parse_key_values(serialized, kHeaderUacConsent);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }

    UacConsentEnvelopeV1 envelope;
    std::string error;

    const auto version = required_int(parsed.value, "schema_version", &error);
    const auto session_id = required_text(parsed.value, "session_id", &error);
    const auto token_id = required_text(parsed.value, "token_id", &error);
    const auto uac_prompt_id = required_text(parsed.value, "uac_prompt_id", &error);
    const auto decision_raw = required_text(parsed.value, "decision", &error);

    if (!version || !session_id || !token_id || !uac_prompt_id || !decision_raw) {
        result.error = error;
        return result;
    }

    const auto decision = parse_uac_consent_decision_v1(*decision_raw);
    if (!decision.ok) {
        result.error = decision.error;
        return result;
    }

    envelope.schema_version = *version;
    envelope.session_id = *session_id;
    envelope.token_id = *token_id;
    envelope.uac_prompt_id = *uac_prompt_id;
    envelope.decision = decision.value;

    if (!validate_uac_consent_envelope_v1(envelope, &error)) {
        result.error = error;
        return result;
    }

    result.ok = true;
    result.value = std::move(envelope);
    return result;
}

std::string serialize_encrypted_offer_blob_v1(const EncryptedOfferBlobV1& encrypted) {
    std::ostringstream out;
    out << kHeaderEncryptedOffer << '\n';
    append_field(out, "schema_version", encrypted.schema_version);
    append_field(out, "kdf", encrypted.kdf);
    append_field(out, "pbkdf2_iterations", encrypted.pbkdf2_iterations);
    append_field(out, "salt_b64", encrypted.salt_b64);
    append_field(out, "iv_b64", encrypted.iv_b64);
    append_field(out, "tag_b64", encrypted.tag_b64);
    append_field(out, "ciphertext_b64", encrypted.ciphertext_b64);
    return out.str();
}

ParseResult<EncryptedOfferBlobV1> parse_encrypted_offer_blob_v1(std::string_view serialized) {
    ParseResult<EncryptedOfferBlobV1> result;
    const auto parsed = parse_key_values(serialized, kHeaderEncryptedOffer);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }

    EncryptedOfferBlobV1 encrypted;
    std::string error;

    const auto version = required_int(parsed.value, "schema_version", &error);
    const auto kdf = required_text(parsed.value, "kdf", &error);
    const auto iterations = required_int(parsed.value, "pbkdf2_iterations", &error);
    const auto salt_b64 = required_text(parsed.value, "salt_b64", &error);
    const auto iv_b64 = required_text(parsed.value, "iv_b64", &error);
    const auto tag_b64 = required_text(parsed.value, "tag_b64", &error);
    const auto ciphertext_b64 = required_text(parsed.value, "ciphertext_b64", &error);

    if (!version || !kdf || !iterations || !salt_b64 || !iv_b64 || !tag_b64 || !ciphertext_b64) {
        result.error = error;
        return result;
    }

    encrypted.schema_version = *version;
    encrypted.kdf = *kdf;
    encrypted.pbkdf2_iterations = *iterations;
    encrypted.salt_b64 = *salt_b64;
    encrypted.iv_b64 = *iv_b64;
    encrypted.tag_b64 = *tag_b64;
    encrypted.ciphertext_b64 = *ciphertext_b64;

    if (encrypted.schema_version != kSchemaVersionV1) {
        result.error = "unsupported encrypted offer schema version";
        return result;
    }
    if (encrypted.kdf != "pbkdf2-sha256") {
        result.error = "unsupported kdf";
        return result;
    }
    if (encrypted.pbkdf2_iterations <= 0) {
        result.error = "invalid pbkdf2 iteration count";
        return result;
    }

    result.ok = true;
    result.value = std::move(encrypted);
    return result;
}

ParseResult<std::string> encrypt_offer_blob_v1(const OfferBlobV1& offer, std::string_view passphrase) {
    ParseResult<std::string> result;
    std::string error;

    if (!validate_offer_blob_v1(offer, &error)) {
        result.error = error;
        return result;
    }

    const std::string plaintext = serialize_offer_blob_v1(offer);

    std::vector<unsigned char> salt(kSaltSize);
    std::vector<unsigned char> iv(kAesGcmIvSize);
    std::vector<unsigned char> key(kAes256KeySize);

    if (!fill_random(&salt, &error) || !fill_random(&iv, &error)) {
        result.error = error;
        return result;
    }
    if (!derive_key_pbkdf2(passphrase, salt, kPbkdf2Iterations, &key, &error)) {
        result.error = error;
        return result;
    }

    const auto ciphertext_and_tag = aes256_gcm_encrypt(plaintext, key, iv);
    if (!ciphertext_and_tag.ok) {
        result.error = ciphertext_and_tag.error;
        return result;
    }
    if (ciphertext_and_tag.value.size() < kAesGcmTagSize) {
        result.error = "ciphertext output too short";
        return result;
    }

    const auto split = static_cast<std::vector<unsigned char>::difference_type>(
        ciphertext_and_tag.value.size() - kAesGcmTagSize);
    const std::vector<unsigned char> ciphertext(ciphertext_and_tag.value.begin(), ciphertext_and_tag.value.begin() + split);
    const std::vector<unsigned char> tag(ciphertext_and_tag.value.begin() + split, ciphertext_and_tag.value.end());

    const auto salt_b64 = base64_encode(salt);
    const auto iv_b64 = base64_encode(iv);
    const auto tag_b64 = base64_encode(tag);
    const auto ciphertext_b64 = base64_encode(ciphertext);

    if (!salt_b64.ok || !iv_b64.ok || !tag_b64.ok || !ciphertext_b64.ok) {
        result.error = "base64 encoding failed";
        return result;
    }

    EncryptedOfferBlobV1 encrypted;
    encrypted.schema_version = kSchemaVersionV1;
    encrypted.kdf = "pbkdf2-sha256";
    encrypted.pbkdf2_iterations = kPbkdf2Iterations;
    encrypted.salt_b64 = salt_b64.value;
    encrypted.iv_b64 = iv_b64.value;
    encrypted.tag_b64 = tag_b64.value;
    encrypted.ciphertext_b64 = ciphertext_b64.value;

    result.ok = true;
    result.value = serialize_encrypted_offer_blob_v1(encrypted);
    return result;
}

ParseResult<OfferBlobV1> decrypt_offer_blob_v1(std::string_view encrypted_blob, std::string_view passphrase) {
    ParseResult<OfferBlobV1> result;

    if (passphrase.empty()) {
        result.error = "passphrase must not be empty";
        return result;
    }

    const auto parsed = parse_encrypted_offer_blob_v1(encrypted_blob);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }

    const auto salt = base64_decode(parsed.value.salt_b64);
    const auto iv = base64_decode(parsed.value.iv_b64);
    const auto tag = base64_decode(parsed.value.tag_b64);
    const auto ciphertext = base64_decode(parsed.value.ciphertext_b64);

    if (!salt.ok || !iv.ok || !tag.ok || !ciphertext.ok) {
        result.error = "base64 decode failed";
        return result;
    }
    if (salt.value.size() != kSaltSize || iv.value.size() != kAesGcmIvSize || tag.value.size() != kAesGcmTagSize) {
        result.error = "invalid encrypted blob field sizes";
        return result;
    }

    std::vector<unsigned char> key(kAes256KeySize);
    std::string error;
    if (!derive_key_pbkdf2(passphrase, salt.value, parsed.value.pbkdf2_iterations, &key, &error)) {
        result.error = error;
        return result;
    }

    const auto plaintext = aes256_gcm_decrypt(ciphertext.value, tag.value, key, iv.value);
    if (!plaintext.ok) {
        result.error = plaintext.error;
        return result;
    }

    const std::string payload(plaintext.value.begin(), plaintext.value.end());
    const auto offer = parse_offer_blob_v1(payload);
    if (!offer.ok) {
        result.error = offer.error;
        return result;
    }

    result.ok = true;
    result.value = offer.value;
    return result;
}

std::string serialize_qr_payload_v1(const QrPayloadV1& payload) {
    std::ostringstream out;
    out << kHeaderQrPayload << '\n';
    append_field(out, "schema_version", payload.schema_version);
    append_field(out, "purpose", payload.purpose);
    append_field(out, "frame_index", payload.frame_index);
    append_field(out, "total_frames", payload.total_frames);
    append_field(out, "payload_b64", payload.payload_b64);
    append_field(out, "checksum_hex", payload.checksum_hex);
    return out.str();
}

ParseResult<QrPayloadV1> parse_qr_payload_v1(std::string_view serialized) {
    ParseResult<QrPayloadV1> result;
    const auto parsed = parse_key_values(serialized, kHeaderQrPayload);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }

    QrPayloadV1 payload;
    std::string error;

    const auto version = required_int(parsed.value, "schema_version", &error);
    const auto purpose = required_text(parsed.value, "purpose", &error);
    const auto frame_index = required_int(parsed.value, "frame_index", &error);
    const auto total_frames = required_int(parsed.value, "total_frames", &error);
    const auto payload_b64 = required_text(parsed.value, "payload_b64", &error);
    const auto checksum = required_text(parsed.value, "checksum_hex", &error);

    if (!version || !purpose || !frame_index || !total_frames || !payload_b64 || !checksum) {
        result.error = error;
        return result;
    }

    payload.schema_version = *version;
    payload.purpose = *purpose;
    payload.frame_index = *frame_index;
    payload.total_frames = *total_frames;
    payload.payload_b64 = *payload_b64;
    payload.checksum_hex = *checksum;

    if (payload.schema_version != kSchemaVersionV1) {
        result.error = "unsupported qr schema version";
        return result;
    }
    if (payload.purpose.empty()) {
        result.error = "purpose must not be empty";
        return result;
    }
    if (payload.frame_index < 0 || payload.total_frames <= 0 || payload.frame_index >= payload.total_frames) {
        result.error = "invalid qr frame coordinates";
        return result;
    }
    if (payload.payload_b64.empty()) {
        result.error = "payload_b64 must not be empty";
        return result;
    }

    result.ok = true;
    result.value = std::move(payload);
    return result;
}

ParseResult<std::string> encode_offer_for_qr_v1(std::string_view encrypted_offer_blob) {
    ParseResult<std::string> result;
    if (encrypted_offer_blob.empty()) {
        result.error = "encrypted offer blob must not be empty";
        return result;
    }

    const std::vector<unsigned char> bytes(encrypted_offer_blob.begin(), encrypted_offer_blob.end());
    const auto payload_b64 = base64_encode(bytes);
    if (!payload_b64.ok) {
        result.error = payload_b64.error;
        return result;
    }

    QrPayloadV1 payload;
    payload.schema_version = kSchemaVersionV1;
    payload.purpose = "offer";
    payload.frame_index = 0;
    payload.total_frames = 1;
    payload.payload_b64 = payload_b64.value;
    payload.checksum_hex = checksum_hex(payload.payload_b64);

    result.ok = true;
    result.value = serialize_qr_payload_v1(payload);
    return result;
}

ParseResult<std::string> decode_offer_from_qr_v1(std::string_view qr_payload) {
    ParseResult<std::string> result;
    const auto parsed = parse_qr_payload_v1(qr_payload);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }

    if (parsed.value.purpose != "offer") {
        result.error = "unsupported qr payload purpose";
        return result;
    }
    if (parsed.value.total_frames != 1 || parsed.value.frame_index != 0) {
        result.error = "multi-frame qr is not supported yet";
        return result;
    }

    if (checksum_hex(parsed.value.payload_b64) != parsed.value.checksum_hex) {
        result.error = "qr payload checksum mismatch";
        return result;
    }

    const auto decoded = base64_decode(parsed.value.payload_b64);
    if (!decoded.ok) {
        result.error = decoded.error;
        return result;
    }

    result.ok = true;
    result.value = std::string(decoded.value.begin(), decoded.value.end());
    return result;
}

std::string to_string(SessionStateV1 state) {
    switch (state) {
    case SessionStateV1::idle:
        return "idle";
    case SessionStateV1::offering:
        return "offering";
    case SessionStateV1::connecting:
        return "connecting";
    case SessionStateV1::established:
        return "established";
    case SessionStateV1::recovering:
        return "recovering";
    case SessionStateV1::terminated:
        return "terminated";
    }
    return "idle";
}

ParseResult<SessionStateV1> parse_session_state_v1(std::string_view value) {
    ParseResult<SessionStateV1> result;
    const std::string normalized = trim(value);

    if (normalized == "idle") {
        result.ok = true;
        result.value = SessionStateV1::idle;
    } else if (normalized == "offering") {
        result.ok = true;
        result.value = SessionStateV1::offering;
    } else if (normalized == "connecting") {
        result.ok = true;
        result.value = SessionStateV1::connecting;
    } else if (normalized == "established") {
        result.ok = true;
        result.value = SessionStateV1::established;
    } else if (normalized == "recovering") {
        result.ok = true;
        result.value = SessionStateV1::recovering;
    } else if (normalized == "terminated") {
        result.ok = true;
        result.value = SessionStateV1::terminated;
    } else {
        result.error = "invalid session state";
    }

    return result;
}

std::string to_string(UacConsentDecisionV1 decision) {
    switch (decision) {
    case UacConsentDecisionV1::allow:
        return "allow";
    case UacConsentDecisionV1::deny:
        return "deny";
    case UacConsentDecisionV1::timeout:
        return "timeout";
    }

    return "deny";
}

ParseResult<UacConsentDecisionV1> parse_uac_consent_decision_v1(std::string_view value) {
    ParseResult<UacConsentDecisionV1> result;
    const std::string normalized = trim(value);

    if (normalized == "allow") {
        result.ok = true;
        result.value = UacConsentDecisionV1::allow;
    } else if (normalized == "deny") {
        result.ok = true;
        result.value = UacConsentDecisionV1::deny;
    } else if (normalized == "timeout") {
        result.ok = true;
        result.value = UacConsentDecisionV1::timeout;
    } else {
        result.error = "invalid uac consent decision";
    }

    return result;
}

std::string_view module_name() {
    return "protocol";
}

}
