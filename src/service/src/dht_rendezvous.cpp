#include "redclaw/service/dht_rendezvous.h"
#include "redclaw/protocol/compressed_protobuf.h"
#include "redclaw_wire.pb.h"

#include "dht_signal_codec_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace redclaw::service {

namespace {

constexpr std::string_view kEncryptedHeader = "RCD-DHT-SIGNAL-ENC-V1";
constexpr std::string_view kEncryptedCompactHeader = "RCE1";
constexpr std::string_view kPlainHeaderV3 = "RCD-DHT-SIGNAL-V3";
constexpr std::string_view kPlainHeaderV4 = "RCD-DHT-SIGNAL-V4";
constexpr std::string_view kPlainCompactHeaderV3 = "RCS3";
constexpr std::string_view kPlainCompactHeaderV4 = "RCS4";
constexpr std::string_view kIndirectHeader = "RCD-DHT-INDIRECT-V1";
constexpr std::size_t kAes256KeySize = 32;
constexpr std::size_t kAesGcmIvSize = 12;
constexpr std::size_t kAesGcmTagSize = 16;
constexpr std::size_t kIndirectDhtChunkPayloadBytes = 640;
constexpr std::size_t kDhtPublisherInstanceBytes = 16;
constexpr std::size_t kDhtPublisherInstanceHexCharacters = kDhtPublisherInstanceBytes * 2;

struct IndirectRecordSummary {
    std::string blob_lane;
    std::string blob_sha256;
    std::uint64_t blob_bytes = 0;
    std::uint64_t chunk_count = 0;
    std::uint64_t chunk_bytes = 0;
};

std::string trim_copy(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string to_lower_ascii(std::string_view value) {
    std::string lower;
    lower.reserve(value.size());
    for (char ch : value) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lower;
}

std::string hex_encode(const unsigned char* data, std::size_t size) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        out << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return out.str();
}

std::string hex_encode(std::string_view value) {
    return hex_encode(
        reinterpret_cast<const unsigned char*>(value.data()),
        value.size());
}

std::optional<std::vector<unsigned char>> hex_decode(std::string_view value) {
    if ((value.size() % 2) != 0) {
        return std::nullopt;
    }
    std::vector<unsigned char> bytes;
    bytes.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        unsigned int parsed = 0;
        std::istringstream in{std::string(value.substr(i, 2))};
        in >> std::hex >> parsed;
        if (!in || parsed > 0xFFU) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<unsigned char>(parsed));
    }
    return bytes;
}

std::optional<std::string> hex_decode_string(std::string_view value) {
    const auto bytes = hex_decode(value);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    return std::string(bytes->begin(), bytes->end());
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool read_u32_be(std::string_view in, std::size_t* cursor, std::uint32_t* value) {
    if (cursor == nullptr || value == nullptr || *cursor > in.size() || in.size() - *cursor < 4) {
        return false;
    }
    const auto byte_at = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(in[*cursor + offset]));
    };
    *value = (byte_at(0) << 24U) | (byte_at(1) << 16U) | (byte_at(2) << 8U) | byte_at(3);
    *cursor += 4;
    return true;
}

bool read_u8(std::string_view in, std::size_t* cursor, std::uint32_t* value) {
    if (cursor == nullptr || value == nullptr || *cursor >= in.size()) {
        return false;
    }
    *value = static_cast<std::uint32_t>(static_cast<unsigned char>(in[*cursor]));
    ++(*cursor);
    return true;
}

bool read_u64_be(std::string_view in, std::size_t* cursor, std::uint64_t* value) {
    if (cursor == nullptr || value == nullptr || *cursor > in.size() || in.size() - *cursor < 8) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (std::size_t offset = 0; offset < 8; ++offset) {
        parsed = (parsed << 8U)
            | static_cast<std::uint64_t>(static_cast<unsigned char>(in[*cursor + offset]));
    }
    *cursor += 8;
    *value = parsed;
    return true;
}

std::array<unsigned char, SHA256_DIGEST_LENGTH> sha256_bytes(std::string_view value) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest {};
    SHA256(
        reinterpret_cast<const unsigned char*>(value.data()),
        value.size(),
        digest.data());
    return digest;
}

std::string sha256_hex(std::string_view value) {
    const auto digest = sha256_bytes(value);
    return hex_encode(digest.data(), digest.size());
}

bool fill_random(std::vector<unsigned char>* bytes, std::string* error_detail) {
    if (bytes == nullptr || bytes->empty()) {
        if (error_detail != nullptr) {
            *error_detail = "random output buffer is empty";
        }
        return false;
    }
    if (RAND_bytes(bytes->data(), static_cast<int>(bytes->size())) != 1) {
        if (error_detail != nullptr) {
            *error_detail = "RAND_bytes failed";
        }
        return false;
    }
    return true;
}

bool parse_u64(std::string_view value, std::uint64_t* out) {
    if (value.empty()) {
        return false;
    }
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, *out);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool aes_gcm_encrypt(
    std::string_view plaintext,
    const std::array<unsigned char, SHA256_DIGEST_LENGTH>& key,
    const std::vector<unsigned char>& iv,
    std::vector<unsigned char>* ciphertext,
    std::vector<unsigned char>* tag,
    std::string* error_detail) {
    if (iv.size() != kAesGcmIvSize || ciphertext == nullptr || tag == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "invalid AES-GCM encrypt buffers";
        }
        return false;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "EVP_CIPHER_CTX_new failed";
        }
        return false;
    }

    ciphertext->assign(plaintext.size() + kAesGcmTagSize, 0);
    int out_len = 0;
    int total = 0;
    const bool ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) == 1
        && EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) == 1
        && EVP_EncryptUpdate(
               ctx,
               ciphertext->data(),
               &out_len,
               reinterpret_cast<const unsigned char*>(plaintext.data()),
               static_cast<int>(plaintext.size())) == 1;

    if (!ok) {
        EVP_CIPHER_CTX_free(ctx);
        if (error_detail != nullptr) {
            *error_detail = "AES-GCM encrypt update failed";
        }
        return false;
    }
    total = out_len;
    if (EVP_EncryptFinal_ex(ctx, ciphertext->data() + total, &out_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        if (error_detail != nullptr) {
            *error_detail = "AES-GCM encrypt final failed";
        }
        return false;
    }
    total += out_len;
    ciphertext->resize(static_cast<std::size_t>(total));

    tag->assign(kAesGcmTagSize, 0);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag->size()), tag->data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        if (error_detail != nullptr) {
            *error_detail = "AES-GCM get tag failed";
        }
        return false;
    }

    EVP_CIPHER_CTX_free(ctx);
    return true;
}

bool aes_gcm_decrypt(
    const std::vector<unsigned char>& ciphertext,
    const std::vector<unsigned char>& tag,
    const std::array<unsigned char, SHA256_DIGEST_LENGTH>& key,
    const std::vector<unsigned char>& iv,
    std::string* plaintext,
    std::string* error_detail) {
    if (iv.size() != kAesGcmIvSize || tag.size() != kAesGcmTagSize || plaintext == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "invalid AES-GCM decrypt buffers";
        }
        return false;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "EVP_CIPHER_CTX_new failed";
        }
        return false;
    }

    std::vector<unsigned char> out(ciphertext.size() + 1);
    int out_len = 0;
    int total = 0;
    const bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) == 1
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) == 1
        && EVP_DecryptUpdate(
               ctx,
               out.data(),
               &out_len,
               ciphertext.data(),
               static_cast<int>(ciphertext.size())) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()), const_cast<unsigned char*>(tag.data())) == 1;

    if (!ok) {
        EVP_CIPHER_CTX_free(ctx);
        if (error_detail != nullptr) {
            *error_detail = "AES-GCM decrypt update failed";
        }
        return false;
    }
    total = out_len;
    if (EVP_DecryptFinal_ex(ctx, out.data() + total, &out_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        if (error_detail != nullptr) {
            *error_detail = "authentication check failed";
        }
        return false;
    }
    total += out_len;
    EVP_CIPHER_CTX_free(ctx);

    plaintext->assign(out.begin(), out.begin() + total);
    return true;
}

std::string serialize_plain_snapshot(const DhtSignalSnapshot& snapshot) {
    std::ostringstream out;
    out << kPlainHeaderV4 << '\n'
        << "role=" << snapshot.role << '\n'
        << "description_type=" << snapshot.description_type << '\n'
        << "description_sdp_hex=" << hex_encode(snapshot.description_sdp) << '\n'
        << "publisher_instance_id=" << snapshot.publisher_instance_id << '\n'
        << "generation=" << snapshot.generation << '\n'
        << "connection_request_tag=" << snapshot.connection_request_tag << '\n'
        << "answered_description_tag=" << snapshot.answered_description_tag << '\n'
        << "acknowledged_answer_tag=" << snapshot.acknowledged_answer_tag << '\n'
        << "candidates_complete=" << (snapshot.candidates_complete ? 1 : 0) << '\n'
        << "candidate_count=" << snapshot.candidate_lines.size() << '\n';
    for (std::size_t i = 0; i < snapshot.candidate_lines.size(); ++i) {
        out << "candidate_" << i << "_hex=" << hex_encode(snapshot.candidate_lines[i]) << '\n';
    }
    return out.str();
}

std::optional<std::string> serialize_compressed_protobuf_snapshot(
    const DhtSignalSnapshot& snapshot,
    std::string* error_detail) {
    const auto instance = hex_decode(snapshot.publisher_instance_id);
    if ((snapshot.role != "host" && snapshot.role != "controller")
        || (snapshot.description_type != "request" && snapshot.description_type != "offer"
            && snapshot.description_type != "answer")
        || (snapshot.description_type == "request" ? snapshot.generation != 0 : snapshot.generation == 0)
        || !is_valid_dht_publisher_instance_id(snapshot.publisher_instance_id)
        || !instance || instance->size() != kDhtPublisherInstanceBytes
        || snapshot.connection_request_tag.size() > 255
        || snapshot.answered_description_tag.size() > 255
        || snapshot.acknowledged_answer_tag.size() > 255 || snapshot.candidate_lines.size() > 255) {
        if (error_detail) *error_detail = "invalid Protobuf DHT snapshot identity";
        return std::nullopt;
    }
    redclaw::protocol::wire::DhtSignalSnapshot encoded;
    encoded.set_schema_version(5);
    encoded.set_role(snapshot.role);
    encoded.set_description_type(snapshot.description_type);
    encoded.set_description_sdp(snapshot.description_sdp);
    encoded.set_publisher_instance_id(instance->data(), instance->size());
    encoded.set_generation(snapshot.generation);
    encoded.set_connection_request_tag(snapshot.connection_request_tag);
    encoded.set_answered_description_tag(snapshot.answered_description_tag);
    encoded.set_acknowledged_answer_tag(snapshot.acknowledged_answer_tag);
    encoded.set_candidates_complete(snapshot.candidates_complete);
    for (const auto& candidate : snapshot.candidate_lines) encoded.add_candidate_lines(candidate);
    auto compressed = redclaw::protocol::compress_protobuf(
        encoded, redclaw::protocol::ProtobufWireKind::kDht);
    if (compressed.empty()) {
        if (error_detail) *error_detail = "Protobuf DHT snapshot exceeds bounded message size";
        return std::nullopt;
    }
    return compressed;
}

std::optional<DhtSignalSnapshot> parse_compressed_protobuf_snapshot(
    std::string_view text, std::uint64_t revision, std::uint64_t expires_at_unix,
    std::string* error_detail) {
    redclaw::protocol::wire::DhtSignalSnapshot encoded;
    if (!redclaw::protocol::decompress_protobuf(text,
            redclaw::protocol::ProtobufWireKind::kDht, encoded, error_detail)) return std::nullopt;
    if (encoded.schema_version() != 5
        || (encoded.role() != "host" && encoded.role() != "controller")
        || (encoded.description_type() != "request" && encoded.description_type() != "offer"
            && encoded.description_type() != "answer")
        || (encoded.description_type() == "request" ? encoded.generation() != 0 : encoded.generation() == 0)
        || encoded.publisher_instance_id().size() != kDhtPublisherInstanceBytes
        || encoded.connection_request_tag().size() > 255
        || encoded.answered_description_tag().size() > 255
        || encoded.acknowledged_answer_tag().size() > 255
        || encoded.candidate_lines_size() > 255) {
        if (error_detail) *error_detail = "invalid Protobuf DHT snapshot identity";
        return std::nullopt;
    }
    DhtSignalSnapshot snapshot;
    snapshot.role = encoded.role();
    snapshot.description_type = encoded.description_type();
    snapshot.description_sdp = encoded.description_sdp();
    snapshot.publisher_instance_id = hex_encode(
        reinterpret_cast<const unsigned char*>(encoded.publisher_instance_id().data()),
        encoded.publisher_instance_id().size());
    snapshot.generation = encoded.generation();
    snapshot.connection_request_tag = encoded.connection_request_tag();
    snapshot.answered_description_tag = encoded.answered_description_tag();
    snapshot.acknowledged_answer_tag = encoded.acknowledged_answer_tag();
    snapshot.candidates_complete = encoded.candidates_complete();
    snapshot.candidate_lines.assign(encoded.candidate_lines().begin(), encoded.candidate_lines().end());
    snapshot.revision = revision;
    snapshot.expires_at_unix = expires_at_unix;
    return snapshot;
}

std::optional<DhtSignalSnapshot> parse_compact_plain_snapshot(
    std::string_view text,
    std::uint64_t revision,
    std::uint64_t expires_at_unix,
    std::string* error_detail) {
    if (text.starts_with("RCP1")) {
        return parse_compressed_protobuf_snapshot(text, revision, expires_at_unix, error_detail);
    }
    const bool is_v4 = starts_with(text, kPlainCompactHeaderV4);
    const bool is_v3 = starts_with(text, kPlainCompactHeaderV3);
    if (!is_v4 && !is_v3) {
        if (error_detail != nullptr) {
            *error_detail = "unexpected compact DHT signal plaintext header";
        }
        return std::nullopt;
    }

    std::size_t cursor = is_v4
        ? kPlainCompactHeaderV4.size()
        : kPlainCompactHeaderV3.size();
    if (text.size() - cursor < 11) {
        if (error_detail != nullptr) {
            *error_detail = "compact DHT signal plaintext is truncated";
        }
        return std::nullopt;
    }

    DhtSignalSnapshot snapshot;
    const char role_code = text[cursor++];
    const char description_code = text[cursor++];
    if (role_code == 'h') {
        snapshot.role = "host";
    } else if (role_code == 'c') {
        snapshot.role = "controller";
    } else {
        if (error_detail != nullptr) {
            *error_detail = "compact DHT signal role is invalid";
        }
        return std::nullopt;
    }
    if (description_code == 'o') {
        snapshot.description_type = "offer";
    } else if (description_code == 'a') {
        snapshot.description_type = "answer";
    } else if (description_code == 'r') {
        snapshot.description_type = "request";
    } else {
        if (error_detail != nullptr) {
            *error_detail = "compact DHT signal description_type is invalid";
        }
        return std::nullopt;
    }

    if (!read_u64_be(text, &cursor, &snapshot.generation)
        || (snapshot.description_type == "request" && snapshot.generation != 0)
        || (snapshot.description_type != "request" && snapshot.generation == 0)) {
        if (error_detail != nullptr) {
            *error_detail = "compact DHT signal generation is invalid";
        }
        return std::nullopt;
    }
    snapshot.candidates_complete = text[cursor++] != '\0';

    if (is_v4) {
        if (text.size() - cursor < kDhtPublisherInstanceBytes) {
            if (error_detail != nullptr) {
                *error_detail = "compact DHT signal publisher instance ID is truncated";
            }
            return std::nullopt;
        }
        snapshot.publisher_instance_id = hex_encode(
            reinterpret_cast<const unsigned char*>(text.data() + cursor),
            kDhtPublisherInstanceBytes);
        cursor += kDhtPublisherInstanceBytes;
        if (!is_valid_dht_publisher_instance_id(snapshot.publisher_instance_id)) {
            if (error_detail != nullptr) {
                *error_detail = "compact DHT signal publisher instance ID is invalid";
            }
            return std::nullopt;
        }
    }

    std::uint32_t request_tag_size = 0;
    if (!(is_v4
            ? read_u8(text, &cursor, &request_tag_size)
            : read_u32_be(text, &cursor, &request_tag_size))
        || text.size() - cursor < request_tag_size) {
        if (error_detail != nullptr) {
            *error_detail = "compact DHT signal connection request tag is truncated";
        }
        return std::nullopt;
    }
    snapshot.connection_request_tag.assign(text.substr(cursor, request_tag_size));
    cursor += request_tag_size;

    std::uint32_t sdp_size = 0;
    if (!read_u32_be(text, &cursor, &sdp_size) || text.size() - cursor < sdp_size) {
        if (error_detail != nullptr) {
            *error_detail = "compact DHT signal SDP is truncated";
        }
        return std::nullopt;
    }
    snapshot.description_sdp.assign(text.substr(cursor, sdp_size));
    cursor += sdp_size;

    std::uint32_t answered_tag_size = 0;
    if (!(is_v4
            ? read_u8(text, &cursor, &answered_tag_size)
            : read_u32_be(text, &cursor, &answered_tag_size))
        || text.size() - cursor < answered_tag_size) {
        if (error_detail != nullptr) {
            *error_detail = "compact DHT signal answered description tag is truncated";
        }
        return std::nullopt;
    }
    snapshot.answered_description_tag.assign(text.substr(cursor, answered_tag_size));
    cursor += answered_tag_size;

    std::uint32_t acknowledged_tag_size = 0;
    if (!(is_v4
            ? read_u8(text, &cursor, &acknowledged_tag_size)
            : read_u32_be(text, &cursor, &acknowledged_tag_size))
        || text.size() - cursor < acknowledged_tag_size) {
        if (error_detail != nullptr) {
            *error_detail = "compact DHT signal acknowledged answer tag is truncated";
        }
        return std::nullopt;
    }
    snapshot.acknowledged_answer_tag.assign(text.substr(cursor, acknowledged_tag_size));
    cursor += acknowledged_tag_size;

    std::uint32_t candidate_count = 0;
    if (!(is_v4
            ? read_u8(text, &cursor, &candidate_count)
            : read_u32_be(text, &cursor, &candidate_count))) {
        if (error_detail != nullptr) {
            *error_detail = "compact DHT signal candidate count is missing";
        }
        return std::nullopt;
    }
    snapshot.candidate_lines.reserve(candidate_count);
    for (std::uint32_t i = 0; i < candidate_count; ++i) {
        std::uint32_t candidate_size = 0;
        if (!read_u32_be(text, &cursor, &candidate_size) || text.size() - cursor < candidate_size) {
            if (error_detail != nullptr) {
                *error_detail = "compact DHT signal candidate is truncated";
            }
            return std::nullopt;
        }
        snapshot.candidate_lines.emplace_back(text.substr(cursor, candidate_size));
        cursor += candidate_size;
    }
    if (cursor != text.size()) {
        if (error_detail != nullptr) {
            *error_detail = "compact DHT signal has trailing bytes";
        }
        return std::nullopt;
    }

    snapshot.revision = revision;
    snapshot.expires_at_unix = expires_at_unix;
    return snapshot;
}

std::unordered_map<std::string, std::string> parse_key_values(std::string_view text, std::string* header) {
    std::unordered_map<std::string, std::string> fields;
    std::istringstream in{std::string(text)};
    std::string line;
    if (std::getline(in, line)) {
        *header = trim_copy(line);
    }
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const std::size_t split = line.find('=');
        if (split == std::string::npos || split == 0) {
            continue;
        }
        fields[trim_copy(std::string_view(line).substr(0, split))] =
            trim_copy(std::string_view(line).substr(split + 1));
    }
    return fields;
}

std::optional<DhtSignalSnapshot> parse_plain_snapshot(
    std::string_view text,
    std::uint64_t revision,
    std::uint64_t expires_at_unix,
    std::string* error_detail) {
    std::string header;
    const auto fields = parse_key_values(text, &header);
    const bool is_v4 = header == kPlainHeaderV4;
    const bool is_v3 = header == kPlainHeaderV3;
    if (!is_v4 && !is_v3) {
        if (error_detail != nullptr) {
            *error_detail = "unexpected DHT signal plaintext header";
        }
        return std::nullopt;
    }

    const auto role = fields.find("role");
    const auto description_type = fields.find("description_type");
    const auto description_sdp = fields.find("description_sdp_hex");
    const auto publisher_instance_id = fields.find("publisher_instance_id");
    const auto generation = fields.find("generation");
    const auto connection_request_tag = fields.find("connection_request_tag");
    const auto candidates_complete = fields.find("candidates_complete");
    const auto candidate_count = fields.find("candidate_count");
    if (role == fields.end() || description_type == fields.end() || description_sdp == fields.end()
        || generation == fields.end() || connection_request_tag == fields.end()
        || (is_v4 && publisher_instance_id == fields.end())
        || candidates_complete == fields.end()
        || candidate_count == fields.end()) {
        if (error_detail != nullptr) {
            *error_detail = "missing DHT signal plaintext fields";
        }
        return std::nullopt;
    }

    std::uint64_t count = 0;
    std::uint64_t parsed_generation = 0;
    std::uint64_t parsed_candidates_complete = 0;
    if (!parse_u64(candidate_count->second, &count)
        || !parse_u64(generation->second, &parsed_generation)
        || !parse_u64(candidates_complete->second, &parsed_candidates_complete)
        || parsed_candidates_complete > 1) {
        if (error_detail != nullptr) {
            *error_detail = "invalid DHT signal candidate count";
        }
        return std::nullopt;
    }

    DhtSignalSnapshot snapshot;
    snapshot.role = role->second;
    snapshot.description_type = description_type->second;
    const auto sdp = hex_decode_string(description_sdp->second);
    if (!sdp.has_value()) {
        if (error_detail != nullptr) {
            *error_detail = "invalid DHT signal SDP payload";
        }
        return std::nullopt;
    }
    snapshot.description_sdp = *sdp;
    if (is_v4) {
        snapshot.publisher_instance_id = publisher_instance_id->second;
        if (!is_valid_dht_publisher_instance_id(snapshot.publisher_instance_id)) {
            if (error_detail != nullptr) {
                *error_detail = "DHT signal publisher instance ID is invalid";
            }
            return std::nullopt;
        }
    }
    snapshot.generation = parsed_generation;
    snapshot.connection_request_tag = connection_request_tag->second;
    if ((snapshot.description_type == "request" && snapshot.generation != 0)
        || (snapshot.description_type != "request" && snapshot.generation == 0)) {
        if (error_detail != nullptr) {
            *error_detail = "invalid DHT signal generation";
        }
        return std::nullopt;
    }
    snapshot.candidates_complete = parsed_candidates_complete == 1;
    if (const auto answered_tag = fields.find("answered_description_tag"); answered_tag != fields.end()) {
        snapshot.answered_description_tag = answered_tag->second;
    }
    if (const auto acknowledged_tag = fields.find("acknowledged_answer_tag"); acknowledged_tag != fields.end()) {
        snapshot.acknowledged_answer_tag = acknowledged_tag->second;
    }
    snapshot.revision = revision;
    snapshot.expires_at_unix = expires_at_unix;

    for (std::uint64_t i = 0; i < count; ++i) {
        const std::string key = "candidate_" + std::to_string(i) + "_hex";
        const auto candidate = fields.find(key);
        if (candidate == fields.end()) {
            if (error_detail != nullptr) {
                *error_detail = "missing DHT signal candidate entry";
            }
            return std::nullopt;
        }
        const auto line = hex_decode_string(candidate->second);
        if (!line.has_value()) {
            if (error_detail != nullptr) {
                *error_detail = "invalid DHT signal candidate payload";
            }
            return std::nullopt;
        }
        snapshot.candidate_lines.push_back(*line);
    }
    return snapshot;
}

std::array<unsigned char, SHA256_DIGEST_LENGTH> derive_dht_key(
    std::string_view session_code,
    std::string_view pairing_secret) {
    return sha256_bytes(
        std::string("redclaw-dht-key-v1\n")
        + std::string(session_code)
        + "\n"
        + std::string(pairing_secret));
}

std::array<unsigned char, SHA256_DIGEST_LENGTH> derive_dht_routing_key(
    std::string_view session_code,
    std::string_view pairing_secret,
    std::string_view lane) {
    return sha256_bytes(
        std::string("redclaw-dht-routing-v1\n")
        + std::string(session_code)
        + "\n"
        + std::string(pairing_secret)
        + "\n"
        + std::string(lane));
}

bool encrypt_snapshot(
    const DhtRendezvousConfig& config,
    const DhtSignalSnapshot& snapshot,
    DhtEncryptedRecord* record,
    std::string* error_detail) {
    if (config.session_code.empty()) {
        if (error_detail != nullptr) {
            *error_detail = "DHT session_code must not be empty";
        }
        return false;
    }
    if (snapshot.role.empty() || snapshot.description_type.empty() || snapshot.description_sdp.empty()
        || !is_valid_dht_publisher_instance_id(snapshot.publisher_instance_id)
        || snapshot.connection_request_tag.empty()
        || (snapshot.description_type == "request" && snapshot.generation != 0)
        || (snapshot.description_type != "request" && snapshot.generation == 0)
        || snapshot.revision == 0) {
        if (error_detail != nullptr) {
            *error_detail = "DHT signal snapshot is incomplete";
        }
        return false;
    }

    std::vector<unsigned char> iv(kAesGcmIvSize);
    if (!fill_random(&iv, error_detail)) {
        return false;
    }

    std::vector<unsigned char> ciphertext;
    std::vector<unsigned char> tag;
    const auto plaintext = serialize_compressed_protobuf_snapshot(snapshot, error_detail);
    if (!plaintext.has_value()) {
        return false;
    }
    const auto key = derive_dht_key(config.session_code, config.pairing_secret);
    if (!aes_gcm_encrypt(*plaintext, key, iv, &ciphertext, &tag, error_detail)) {
        return false;
    }

    const std::uint64_t expires_at_unix = config.now_unix + config.ttl_seconds;
    std::string out;
    out.reserve(kEncryptedCompactHeader.size() + iv.size() + tag.size() + ciphertext.size());
    out.append(kEncryptedCompactHeader);
    out.append(reinterpret_cast<const char*>(iv.data()), iv.size());
    out.append(reinterpret_cast<const char*>(tag.data()), tag.size());
    out.append(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());

    record->topic_hex = derive_dht_rendezvous_topic_hex(config.session_code, config.pairing_secret);
    record->lane = snapshot.role;
    record->encrypted_blob = std::move(out);
    record->revision = snapshot.revision;
    record->expires_at_unix = expires_at_unix;
    return true;
}

std::optional<DhtSignalSnapshot> decrypt_snapshot(
    const DhtRendezvousConfig& config,
    const DhtEncryptedRecord& record,
    std::string* error_detail) {
    if (starts_with(record.encrypted_blob, kEncryptedCompactHeader)) {
        if (record.revision == 0 || record.expires_at_unix == 0) {
            if (error_detail != nullptr) {
                *error_detail = "compact DHT encrypted record has invalid metadata";
            }
            return std::nullopt;
        }
        if (config.now_unix > 0 && record.expires_at_unix <= config.now_unix) {
            if (error_detail != nullptr) {
                *error_detail = "DHT encrypted record expired";
            }
            return std::nullopt;
        }

        const std::size_t header_size = kEncryptedCompactHeader.size();
        const std::size_t fixed_payload_size = header_size + kAesGcmIvSize + kAesGcmTagSize;
        if (record.encrypted_blob.size() <= fixed_payload_size) {
            if (error_detail != nullptr) {
                *error_detail = "compact DHT encrypted record is truncated";
            }
            return std::nullopt;
        }

        const std::vector<unsigned char> iv(
            record.encrypted_blob.begin() + static_cast<std::ptrdiff_t>(header_size),
            record.encrypted_blob.begin() + static_cast<std::ptrdiff_t>(header_size + kAesGcmIvSize));
        const std::vector<unsigned char> tag(
            record.encrypted_blob.begin() + static_cast<std::ptrdiff_t>(header_size + kAesGcmIvSize),
            record.encrypted_blob.begin() + static_cast<std::ptrdiff_t>(fixed_payload_size));
        const std::vector<unsigned char> ciphertext(
            record.encrypted_blob.begin() + static_cast<std::ptrdiff_t>(fixed_payload_size),
            record.encrypted_blob.end());

        std::string plaintext;
        const auto key = derive_dht_key(config.session_code, config.pairing_secret);
        if (!aes_gcm_decrypt(ciphertext, tag, key, iv, &plaintext, error_detail)) {
            return std::nullopt;
        }
        return parse_compact_plain_snapshot(plaintext, record.revision, record.expires_at_unix, error_detail);
    }

    std::string header;
    const auto fields = parse_key_values(record.encrypted_blob, &header);
    if (header != kEncryptedHeader) {
        if (error_detail != nullptr) {
            *error_detail = "unexpected DHT encrypted record header";
        }
        return std::nullopt;
    }

    const auto revision_it = fields.find("revision");
    const auto expires_it = fields.find("expires_at_unix");
    const auto iv_it = fields.find("iv_hex");
    const auto tag_it = fields.find("tag_hex");
    const auto ciphertext_it = fields.find("ciphertext_hex");
    if (revision_it == fields.end() || expires_it == fields.end() || iv_it == fields.end()
        || tag_it == fields.end() || ciphertext_it == fields.end()) {
        if (error_detail != nullptr) {
            *error_detail = "DHT encrypted record missing fields";
        }
        return std::nullopt;
    }

    std::uint64_t revision = 0;
    std::uint64_t expires_at_unix = 0;
    if (!parse_u64(revision_it->second, &revision) || !parse_u64(expires_it->second, &expires_at_unix)) {
        if (error_detail != nullptr) {
            *error_detail = "DHT encrypted record has invalid revision or expiry";
        }
        return std::nullopt;
    }
    if (config.now_unix > 0 && expires_at_unix <= config.now_unix) {
        if (error_detail != nullptr) {
            *error_detail = "DHT encrypted record expired";
        }
        return std::nullopt;
    }

    const auto iv = hex_decode(iv_it->second);
    const auto tag = hex_decode(tag_it->second);
    const auto ciphertext = hex_decode(ciphertext_it->second);
    if (!iv.has_value() || !tag.has_value() || !ciphertext.has_value()) {
        if (error_detail != nullptr) {
            *error_detail = "DHT encrypted record contains invalid hex payload";
        }
        return std::nullopt;
    }

    std::string plaintext;
    const auto key = derive_dht_key(config.session_code, config.pairing_secret);
    if (!aes_gcm_decrypt(*ciphertext, *tag, key, *iv, &plaintext, error_detail)) {
        return std::nullopt;
    }
    return parse_plain_snapshot(plaintext, revision, expires_at_unix, error_detail);
}

std::string record_key(std::string_view topic_hex, std::string_view lane) {
    return std::string(topic_hex) + ":" + std::string(lane);
}

std::string make_indirect_blob_lane(
    std::string_view lane,
    std::uint64_t revision,
    std::string_view blob_sha256) {
    constexpr std::size_t kLaneDigestHexCharacters = 24;
    return std::string(lane)
        + "-blob-r" + std::to_string(revision)
        + "-d" + std::string(blob_sha256.substr(0, kLaneDigestHexCharacters));
}

std::string make_indirect_chunk_lane(std::string_view blob_lane, std::uint64_t chunk_index) {
    return std::string(blob_lane) + "-c" + std::to_string(chunk_index);
}

std::string make_indirect_chunk_topic_hex(
    std::string_view topic_hex,
    std::string_view chunk_lane) {
    const std::string material =
        std::string("redclaw-dht-indirect-chunk-topic-v1\n")
        + std::string(topic_hex)
        + "\n"
        + std::string(chunk_lane);
    return sha256_hex(material);
}

std::string derive_indirect_chunk_routing_token(
    std::string_view routing_token,
    std::string_view chunk_lane) {
    const std::string material =
        std::string("redclaw-dht-indirect-chunk-routing-v1\n")
        + std::string(routing_token)
        + "\n"
        + std::string(chunk_lane);
    const auto digest = sha256_bytes(material);
    return hex_encode(digest.data(), digest.size());
}

std::string serialize_indirect_summary(const IndirectRecordSummary& summary) {
    std::ostringstream out;
    out << kIndirectHeader << '\n'
        << "blob_lane=" << summary.blob_lane << '\n'
        << "blob_bytes=" << summary.blob_bytes << '\n'
        << "blob_sha256=" << summary.blob_sha256 << '\n'
        << "chunk_count=" << summary.chunk_count << '\n'
        << "chunk_bytes=" << summary.chunk_bytes << '\n';
    return out.str();
}

bool is_indirect_summary(std::string_view text) {
    return text.substr(0, kIndirectHeader.size()) == kIndirectHeader;
}

std::optional<IndirectRecordSummary> parse_indirect_summary(
    std::string_view text,
    std::string* error_detail) {
    std::string header;
    const auto fields = parse_key_values(text, &header);
    if (header != kIndirectHeader) {
        if (error_detail != nullptr) {
            *error_detail = "unexpected DHT indirect summary header";
        }
        return std::nullopt;
    }

    const auto blob_lane_it = fields.find("blob_lane");
    const auto blob_bytes_it = fields.find("blob_bytes");
    const auto blob_sha_it = fields.find("blob_sha256");
    if (blob_lane_it == fields.end() || blob_bytes_it == fields.end() || blob_sha_it == fields.end()) {
        if (error_detail != nullptr) {
            *error_detail = "DHT indirect summary missing fields";
        }
        return std::nullopt;
    }

    IndirectRecordSummary summary;
    summary.blob_lane = blob_lane_it->second;
    if (summary.blob_lane.empty()) {
        if (error_detail != nullptr) {
            *error_detail = "DHT indirect summary blob lane is empty";
        }
        return std::nullopt;
    }
    if (!parse_u64(blob_bytes_it->second, &summary.blob_bytes)) {
        if (error_detail != nullptr) {
            *error_detail = "DHT indirect summary blob size is invalid";
        }
        return std::nullopt;
    }
    summary.blob_sha256 = blob_sha_it->second;
    if (summary.blob_sha256.size() != SHA256_DIGEST_LENGTH * 2) {
        if (error_detail != nullptr) {
            *error_detail = "DHT indirect summary blob digest is invalid";
        }
        return std::nullopt;
    }
    const auto chunk_count_it = fields.find("chunk_count");
    if (chunk_count_it != fields.end()) {
        if (!parse_u64(chunk_count_it->second, &summary.chunk_count)) {
            if (error_detail != nullptr) {
                *error_detail = "DHT indirect summary chunk count is invalid";
            }
            return std::nullopt;
        }
    }
    const auto chunk_bytes_it = fields.find("chunk_bytes");
    if (chunk_bytes_it != fields.end()) {
        if (!parse_u64(chunk_bytes_it->second, &summary.chunk_bytes)) {
            if (error_detail != nullptr) {
                *error_detail = "DHT indirect summary chunk size is invalid";
            }
            return std::nullopt;
        }
    }
    if (summary.chunk_count > 0 && summary.chunk_bytes == 0) {
        if (error_detail != nullptr) {
            *error_detail = "DHT indirect summary chunk size must be present for chunked blobs";
        }
        return std::nullopt;
    }
    return summary;
}

std::optional<DhtEncryptedRecord> parse_record_file(
    const std::string& contents,
    std::string_view topic_hex,
    std::string_view lane,
    std::string* error_detail) {
    std::string header;
    const auto fields = parse_key_values(contents, &header);
    if (header != "RCD-DHT-STORE-V1") {
        if (error_detail != nullptr) {
            *error_detail = "unexpected DHT store header";
        }
        return std::nullopt;
    }

    const auto revision_it = fields.find("revision");
    const auto expires_it = fields.find("expires_at_unix");
    const auto blob_it = fields.find("encrypted_blob_hex");
    if (revision_it == fields.end() || expires_it == fields.end() || blob_it == fields.end()) {
        if (error_detail != nullptr) {
            *error_detail = "DHT store file missing fields";
        }
        return std::nullopt;
    }

    std::uint64_t revision = 0;
    std::uint64_t expires_at_unix = 0;
    if (!parse_u64(revision_it->second, &revision) || !parse_u64(expires_it->second, &expires_at_unix)) {
        if (error_detail != nullptr) {
            *error_detail = "DHT store file has invalid revision or expiry";
        }
        return std::nullopt;
    }

    const auto blob = hex_decode_string(blob_it->second);
    if (!blob.has_value()) {
        if (error_detail != nullptr) {
            *error_detail = "DHT store file contains invalid encrypted blob";
        }
        return std::nullopt;
    }

    DhtEncryptedRecord record;
    record.topic_hex = std::string(topic_hex);
    record.lane = std::string(lane);
    record.encrypted_blob = *blob;
    record.revision = revision;
    record.expires_at_unix = expires_at_unix;
    return record;
}

std::string serialize_record_file(const DhtEncryptedRecord& record) {
    std::ostringstream out;
    out << "RCD-DHT-STORE-V1\n"
        << "revision=" << record.revision << '\n'
        << "expires_at_unix=" << record.expires_at_unix << '\n'
        << "encrypted_blob_hex=" << hex_encode(record.encrypted_blob) << '\n';
    return out.str();
}

}  // namespace

bool is_valid_dht_publisher_instance_id(std::string_view instance_id) noexcept {
    if (instance_id.size() != kDhtPublisherInstanceHexCharacters) {
        return false;
    }
    return std::all_of(
        instance_id.begin(),
        instance_id.end(),
        [](char value) {
            return (value >= '0' && value <= '9')
                || (value >= 'a' && value <= 'f');
        });
}

std::optional<std::string> make_dht_publisher_instance_id(
    std::string* error_detail) {
    std::vector<unsigned char> random_bytes(kDhtPublisherInstanceBytes);
    if (!fill_random(&random_bytes, error_detail)) {
        return std::nullopt;
    }
    return hex_encode(random_bytes.data(), random_bytes.size());
}

std::optional<DhtSignalSnapshot> detail::parse_dht_signal_plaintext(
    std::string_view plaintext,
    std::uint64_t revision,
    std::uint64_t expires_at_unix,
    std::string* error_detail) {
    if (starts_with(plaintext, "RCP1") || starts_with(plaintext, kPlainCompactHeaderV4)
        || starts_with(plaintext, kPlainCompactHeaderV3)) {
        return parse_compact_plain_snapshot(
            plaintext,
            revision,
            expires_at_unix,
            error_detail);
    }
    return parse_plain_snapshot(
        plaintext,
        revision,
        expires_at_unix,
        error_detail);
}

bool parse_local_helper_role(
    std::string_view value,
    LocalHelperRole* role,
    std::string* error_detail) {
    if (role == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "helper role output must be non-null";
        }
        return false;
    }

    const std::string normalized = to_lower_ascii(trim_copy(value));
    if (normalized == "off") {
        *role = LocalHelperRole::kOff;
        return true;
    }
    if (normalized == "client") {
        *role = LocalHelperRole::kClient;
        return true;
    }
    if (normalized == "lan-helper") {
        *role = LocalHelperRole::kLanHelper;
        return true;
    }
    if (normalized == "relay") {
        *role = LocalHelperRole::kRelay;
        return true;
    }

    if (error_detail != nullptr) {
        *error_detail = "helper_role must be one of off, client, lan-helper, relay";
    }
    return false;
}

std::string local_helper_role_to_string(LocalHelperRole role) {
    switch (role) {
    case LocalHelperRole::kOff:
        return "off";
    case LocalHelperRole::kClient:
        return "client";
    case LocalHelperRole::kLanHelper:
        return "lan-helper";
    case LocalHelperRole::kRelay:
        return "relay";
    }
    return "off";
}

std::string local_helper_path_status_to_string(LocalHelperPathStatus status) {
    switch (status) {
    case LocalHelperPathStatus::kUnavailable:
        return "unavailable";
    case LocalHelperPathStatus::kMissing:
        return "missing";
    case LocalHelperPathStatus::kNotDirectory:
        return "not_directory";
    case LocalHelperPathStatus::kReadOnly:
        return "read_only";
    case LocalHelperPathStatus::kWritable:
        return "writable";
    }
    return "unavailable";
}

std::string derive_dht_rendezvous_topic_hex(
    std::string_view session_code,
    std::string_view pairing_secret) {
    const std::string material =
        std::string("redclaw-dht-topic-v1\n") + std::string(session_code) + "\n" + std::string(pairing_secret);
    const auto digest = sha256_bytes(material);
    return hex_encode(digest.data(), digest.size());
}

std::string derive_dht_rendezvous_routing_token_hex(
    std::string_view session_code,
    std::string_view pairing_secret,
    std::string_view lane) {
    const auto digest = derive_dht_routing_key(session_code, pairing_secret, lane);
    return hex_encode(digest.data(), digest.size());
}

std::string derive_dht_description_tag(std::string_view description_sdp) {
    if (description_sdp.empty()) {
        return {};
    }
    // Only a short prefix is kept: the tag rides in every record, and it just
    // has to distinguish the descriptions of one exchange from each other.
    return sha256_hex(std::string("redclaw-dht-description-tag-v1\n") + std::string(description_sdp))
        .substr(0, 16);
}

std::uint64_t make_dht_publish_revision_floor(std::uint64_t unix_time_ms) {
    constexpr std::uint64_t kRevisionHeadroom = 1'000'000'000ULL;
    constexpr std::uint64_t kMaximumSignedRevision =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return std::clamp(
        unix_time_ms,
        std::uint64_t {1},
        kMaximumSignedRevision - kRevisionHeadroom);
}

std::string make_dht_signal_publication_key(const DhtSignalSnapshot& snapshot) {
    std::string key = snapshot.description_type + "\n" + snapshot.description_sdp
        + "\n" + snapshot.publisher_instance_id + "\n" + std::to_string(snapshot.generation)
        + "\n" + snapshot.connection_request_tag + "\n" + snapshot.answered_description_tag
        + "\n" + snapshot.acknowledged_answer_tag
        + "\n" + (snapshot.candidates_complete ? "complete" : "gathering");
    for (const auto& candidate : snapshot.candidate_lines) {
        key += "\n";
        key += candidate;
    }
    return key;
}

std::string select_dht_answered_description_tag(
    std::string_view description_type,
    std::string_view offer_description_tag) {
    if (description_type != "answer") {
        return {};
    }
    return std::string(offer_description_tag);
}

LocalHelperPathProbe probe_local_helper_path(const std::filesystem::path& path) {
    LocalHelperPathProbe probe;
    probe.path = path;
    if (path.empty()) {
        probe.status = LocalHelperPathStatus::kUnavailable;
        probe.detail = "local helper path is empty";
        return probe;
    }

    std::error_code ec;
    probe.exists = std::filesystem::exists(path, ec);
    if (ec) {
        probe.status = LocalHelperPathStatus::kUnavailable;
        probe.detail = ec.message();
        return probe;
    }
    if (!probe.exists) {
        probe.status = LocalHelperPathStatus::kMissing;
        probe.detail = "path does not exist";
        return probe;
    }
    if (!std::filesystem::is_directory(path, ec) || ec) {
        probe.status = LocalHelperPathStatus::kNotDirectory;
        probe.detail = ec ? ec.message() : "path is not a directory";
        return probe;
    }

    const auto probe_file = path / ".redclaw-helper-write-probe.tmp";
    {
        std::ofstream out(probe_file, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            probe.status = LocalHelperPathStatus::kReadOnly;
            probe.detail = "probe file could not be opened for write";
            return probe;
        }
        out << "redclaw-helper-probe\n";
        out.flush();
        if (!out.good()) {
            probe.status = LocalHelperPathStatus::kReadOnly;
            probe.detail = "probe file write failed";
            return probe;
        }
    }

    std::filesystem::remove(probe_file, ec);
    probe.status = LocalHelperPathStatus::kWritable;
    probe.writable = true;
    probe.detail = "path is writable";
    return probe;
}

bool candidate_looks_ipv6(std::string_view candidate_sdp) {
    std::istringstream in{std::string(candidate_sdp)};
    std::string token;
    for (int index = 0; std::getline(in, token, ' ');) {
        if (token.empty()) {
            continue;
        }
        if (index == 4) {
            return token.find(':') != std::string::npos;
        }
        ++index;
    }
    return false;
}

bool candidate_looks_server_reflexive(std::string_view candidate_sdp) {
    return candidate_sdp.find(" typ srflx") != std::string_view::npos;
}

bool candidate_looks_relay(std::string_view candidate_sdp) {
    return candidate_sdp.find(" typ relay") != std::string_view::npos;
}

std::vector<std::string> prioritize_dht_candidate_lines(
    const std::vector<std::string>& candidate_lines) {
    auto candidate_priority = [](std::string_view candidate_line) {
        if (candidate_looks_relay(candidate_line)) {
            return 0;
        }
        if (candidate_looks_server_reflexive(candidate_line)) {
            return 1;
        }
        if (candidate_line.find(" typ prflx") != std::string_view::npos) {
            return 2;
        }
        if (candidate_line.find(" typ host") != std::string_view::npos) {
            return 3;
        }
        return 4;
    };

    std::vector<std::string> prioritized = candidate_lines;
    std::stable_sort(
        prioritized.begin(),
        prioritized.end(),
        [&candidate_priority](const std::string& left, const std::string& right) {
            return candidate_priority(left) < candidate_priority(right);
        });
    return prioritized;
}

std::vector<std::string> merge_dht_candidate_snapshots(
    const std::vector<std::string>& published_candidate_lines,
    const std::vector<std::string>& gathered_candidate_lines) {
    std::vector<std::string> cumulative = published_candidate_lines;
    cumulative.reserve(published_candidate_lines.size() + gathered_candidate_lines.size());
    for (const auto& candidate_line : gathered_candidate_lines) {
        if (std::find(cumulative.begin(), cumulative.end(), candidate_line) == cumulative.end()) {
            cumulative.push_back(candidate_line);
        }
    }
    return prioritize_dht_candidate_lines(cumulative);
}

std::optional<std::size_t> estimate_dht_encrypted_blob_bytes(
    const DhtSignalSnapshot& snapshot,
    std::string* error_detail) {
    const auto plaintext = serialize_compressed_protobuf_snapshot(snapshot, error_detail);
    if (!plaintext.has_value()) {
        return std::nullopt;
    }
    return kEncryptedCompactHeader.size()
        + kAesGcmIvSize
        + kAesGcmTagSize
        + plaintext->size();
}

bool dht_signal_snapshot_fits_direct_record(
    const DhtSignalSnapshot& snapshot,
    std::size_t* encrypted_blob_bytes,
    std::string* error_detail) {
    const auto estimated_bytes = estimate_dht_encrypted_blob_bytes(snapshot, error_detail);
    if (!estimated_bytes.has_value()) {
        return false;
    }
    if (encrypted_blob_bytes != nullptr) {
        *encrypted_blob_bytes = *estimated_bytes;
    }
    return *estimated_bytes <= kDirectDhtEncryptedBlobBudgetBytes;
}

std::optional<DhtInitialSnapshotPlan> plan_dht_initial_snapshot(
    const DhtSignalSnapshot& description_snapshot,
    const std::vector<std::string>& candidate_lines,
    std::string* error_detail) {
    DhtSignalSnapshot full_snapshot = description_snapshot;
    full_snapshot.candidate_lines = prioritize_dht_candidate_lines(candidate_lines);

    DhtInitialSnapshotPlan plan;
    const auto estimated_bytes = estimate_dht_encrypted_blob_bytes(full_snapshot, error_detail);
    if (!estimated_bytes.has_value()) {
        return std::nullopt;
    }
    plan.full_encrypted_blob_bytes = *estimated_bytes;
    plan.full_snapshot_fits_direct =
        *estimated_bytes <= kDirectDhtEncryptedBlobBudgetBytes;
    if (plan.full_snapshot_fits_direct) {
        plan.candidate_lines = std::move(full_snapshot.candidate_lines);
        return plan;
    }

    DhtSignalSnapshot direct_snapshot = description_snapshot;
    for (const auto& candidate_line : full_snapshot.candidate_lines) {
        direct_snapshot.candidate_lines.push_back(candidate_line);
        const auto direct_bytes = estimate_dht_encrypted_blob_bytes(direct_snapshot, error_detail);
        if (!direct_bytes.has_value()) {
            return std::nullopt;
        }
        if (*direct_bytes > kDirectDhtEncryptedBlobBudgetBytes) {
            break;
        }
        plan.candidate_lines = direct_snapshot.candidate_lines;
    }
    return plan;
}

DhtPublishResult InMemoryDhtRendezvousStore::publish(
    const DhtEncryptedRecord& record,
    std::string_view routing_token,
    std::string* error_detail) {
    (void)routing_token;
    if (record.topic_hex.empty() || record.lane.empty() || record.encrypted_blob.empty() || record.revision == 0) {
        if (error_detail != nullptr) {
            *error_detail = "DHT record is incomplete";
        }
        return false;
    }
    records_[record_key(record.topic_hex, record.lane)] = record;
    return true;
}

std::optional<DhtEncryptedRecord> InMemoryDhtRendezvousStore::fetch(
    std::string_view topic_hex,
    std::string_view lane,
    std::uint64_t after_revision,
    std::string_view routing_token,
    std::string* error_detail) {
    (void)routing_token;
    (void)error_detail;
    const auto it = records_.find(record_key(topic_hex, lane));
    if (it == records_.end() || it->second.revision <= after_revision) {
        return std::nullopt;
    }
    return it->second;
}

FileBackedDhtRendezvousStore::FileBackedDhtRendezvousStore(std::filesystem::path root_directory)
    : root_directory_(std::move(root_directory)) {}

std::filesystem::path FileBackedDhtRendezvousStore::record_path(std::string_view topic_hex, std::string_view lane) const {
    return root_directory_ / (std::string(topic_hex) + "-" + std::string(lane) + ".rcd-dht");
}

DhtPublishResult FileBackedDhtRendezvousStore::publish(
    const DhtEncryptedRecord& record,
    std::string_view routing_token,
    std::string* error_detail) {
    (void)routing_token;
    if (record.topic_hex.empty() || record.lane.empty() || record.encrypted_blob.empty() || record.revision == 0) {
        if (error_detail != nullptr) {
            *error_detail = "DHT record is incomplete";
        }
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(root_directory_, ec);
    if (ec) {
        if (error_detail != nullptr) {
            *error_detail = "failed to create DHT mailbox directory: " + ec.message();
        }
        return false;
    }

    const auto path = record_path(record.topic_hex, record.lane);
    auto temp_path = path;
    temp_path += ".tmp";

    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_detail != nullptr) {
            *error_detail = "failed to open DHT mailbox temp record for write: " + temp_path.string();
        }
        return false;
    }
    const std::string serialized = serialize_record_file(record);
    out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    out.flush();
    if (!out.good()) {
        if (error_detail != nullptr) {
            *error_detail = "failed to write DHT mailbox temp record: " + temp_path.string();
        }
        out.close();
        std::filesystem::remove(temp_path, ec);
        return false;
    }

    out.close();

    std::filesystem::rename(temp_path, path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(path, remove_ec);
        ec.clear();
        std::filesystem::rename(temp_path, path, ec);
    }
    if (ec) {
        const std::string rename_error = ec.message();
        ec.clear();
        std::filesystem::copy_file(
            temp_path,
            path,
            std::filesystem::copy_options::overwrite_existing,
            ec);
        if (!ec) {
            std::filesystem::remove(temp_path, ec);
            return true;
        }
        if (error_detail != nullptr) {
            *error_detail = "failed to publish DHT mailbox record atomically: "
                + rename_error
                + "; copy fallback failed: "
                + ec.message();
        }
        std::filesystem::remove(temp_path, ec);
        return false;
    }
    return true;
}

std::optional<DhtEncryptedRecord> FileBackedDhtRendezvousStore::fetch(
    std::string_view topic_hex,
    std::string_view lane,
    std::uint64_t after_revision,
    std::string_view routing_token,
    std::string* error_detail) {
    (void)routing_token;
    const auto path = record_path(topic_hex, lane);
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        if (error_detail != nullptr) {
            *error_detail = "failed to read DHT mailbox record: " + path.string();
        }
        return std::nullopt;
    }

    const auto record = parse_record_file(contents, topic_hex, lane, error_detail);
    if (!record.has_value() || record->revision <= after_revision) {
        return std::nullopt;
    }
    return record;
}

std::optional<DhtEncryptedRecord> FileBackedDhtRendezvousStore::fetch_latest_indirect_blob(
    std::string_view topic_hex,
    std::string_view lane,
    std::uint64_t after_revision,
    std::string* error_detail) {
    const std::string prefix = std::string(topic_hex) + "-" + std::string(lane) + "-blob-r";
    constexpr std::string_view kSuffix = ".rcd-dht";

    std::error_code ec;
    if (!std::filesystem::exists(root_directory_, ec) || ec) {
        return std::nullopt;
    }

    std::uint64_t latest_revision = 0;
    std::string latest_lane;
    for (const auto& entry : std::filesystem::directory_iterator(root_directory_, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }

        const std::string filename = entry.path().filename().string();
        if (filename.rfind(prefix, 0) != 0 || filename.size() <= prefix.size() + kSuffix.size()) {
            continue;
        }
        if (filename.compare(filename.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
            continue;
        }

        const std::size_t digest_separator = filename.find("-d", prefix.size());
        if (digest_separator == std::string::npos
            || digest_separator + 2 >= filename.size() - kSuffix.size()) {
            continue;
        }
        const std::string_view revision_text(
            filename.data() + prefix.size(),
            digest_separator - prefix.size());
        std::uint64_t revision = 0;
        const auto parse_result = std::from_chars(
            revision_text.data(),
            revision_text.data() + revision_text.size(),
            revision);
        if (parse_result.ec != std::errc{} || parse_result.ptr != revision_text.data() + revision_text.size()) {
            continue;
        }
        if (revision <= after_revision || revision <= latest_revision) {
            continue;
        }

        latest_revision = revision;
        latest_lane = filename.substr(
            std::string(topic_hex).size() + 1,
            filename.size() - std::string(topic_hex).size() - 1 - kSuffix.size());
    }

    if (latest_revision == 0) {
        return std::nullopt;
    }

    return fetch(
        topic_hex,
        latest_lane,
        0,
        "",
        error_detail);
}

IndirectDhtRendezvousStore::IndirectDhtRendezvousStore(
    std::unique_ptr<IDhtRendezvousStore> primary_store,
    std::unique_ptr<IDhtRendezvousStore> secondary_store)
    : primary_store_(std::move(primary_store))
    , secondary_store_(std::move(secondary_store)) {}

void IndirectDhtRendezvousStore::advance() {
    if (primary_store_) primary_store_->advance();
}

DhtPublishResult IndirectDhtRendezvousStore::publish(
    const DhtEncryptedRecord& record,
    std::string_view routing_token,
    std::string* error_detail) {
    if (primary_store_ == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "indirect DHT primary store is not configured";
        }
        return false;
    }
    if (record.topic_hex.empty() || record.lane.empty() || record.encrypted_blob.empty() || record.revision == 0) {
        if (error_detail != nullptr) {
            *error_detail = "DHT record is incomplete";
        }
        return false;
    }

    if (record.encrypted_blob.size() <= kDirectDhtEncryptedBlobBudgetBytes) {
        return primary_store_->publish(record, routing_token, error_detail);
    }

    const std::string blob_sha256 = sha256_hex(record.encrypted_blob);
    const std::string blob_lane = make_indirect_blob_lane(
        record.lane, record.revision, blob_sha256);
    const std::uint64_t chunk_count = static_cast<std::uint64_t>(
        (record.encrypted_blob.size() + kIndirectDhtChunkPayloadBytes - 1) / kIndirectDhtChunkPayloadBytes);
    const std::string chunk_cache_key = record.topic_hex
        + "\n"
        + record.lane
        + "\n"
        + std::to_string(record.revision)
        + "\n"
        + blob_sha256;
    if (cached_chunk_publish_key_ != chunk_cache_key
        || cached_chunk_publish_success_.size() != static_cast<std::size_t>(chunk_count)) {
        cached_chunk_publish_key_ = chunk_cache_key;
        cached_chunk_publish_success_.assign(static_cast<std::size_t>(chunk_count), false);
        cached_chunk_cursor_ = 0;
    }

    std::uint64_t last_touched_chunk = 0;
    bool any_chunk_touched = false;
    bool any_chunk_pending = false;
    std::string first_pending_error;
    const auto turn_started = std::chrono::steady_clock::now();
    unsigned touched = 0;
    for (std::uint64_t scanned = 0; scanned < chunk_count; ++scanned) {
        if (touched >= 8 || std::chrono::steady_clock::now() - turn_started >= std::chrono::milliseconds(5)) break;
        const auto index = cached_chunk_cursor_++ % chunk_count;
        if (cached_chunk_publish_success_[static_cast<std::size_t>(index)]) {
            continue;
        }
        any_chunk_touched = true;
        ++touched;
        last_touched_chunk = index;
        const std::size_t offset = static_cast<std::size_t>(index) * kIndirectDhtChunkPayloadBytes;
        const std::size_t remaining = record.encrypted_blob.size() - offset;
        const std::size_t count = std::min(remaining, kIndirectDhtChunkPayloadBytes);
        const std::string chunk_lane = make_indirect_chunk_lane(blob_lane, index);
        const std::string chunk_topic = make_indirect_chunk_topic_hex(record.topic_hex, chunk_lane);

        DhtEncryptedRecord chunk_record = record;
        chunk_record.topic_hex = chunk_topic;
        chunk_record.lane = chunk_lane;
        chunk_record.encrypted_blob = record.encrypted_blob.substr(offset, count);
        const std::string chunk_routing_token = derive_indirect_chunk_routing_token(routing_token, chunk_lane);
        std::string chunk_error;
        const auto result = primary_store_->publish(chunk_record, chunk_routing_token, &chunk_error);
        if (!result) {
            if (result.state == DhtPublishState::kPending) {
                any_chunk_pending = true;
                if (first_pending_error.empty()) {
                    first_pending_error = std::move(chunk_error);
                }
                continue;
            }
            if (error_detail != nullptr) {
                const auto published_count = std::count(
                    cached_chunk_publish_success_.begin(),
                    cached_chunk_publish_success_.end(),
                    true);
                *error_detail = "failed to publish DHT indirect chunk "
                    + std::to_string(index)
                    + "/"
                    + std::to_string(chunk_count);
                if (!chunk_error.empty()) {
                    *error_detail += ": " + chunk_error;
                }
                *error_detail += " published_chunks="
                    + std::to_string(published_count)
                    + "/"
                    + std::to_string(chunk_count);
            }
            return result;
        }

        cached_chunk_publish_success_[static_cast<std::size_t>(index)] = true;
    }

    const auto published_count = std::count(
        cached_chunk_publish_success_.begin(),
        cached_chunk_publish_success_.end(),
        true);
    if (static_cast<std::uint64_t>(published_count) < chunk_count) {
        if (error_detail != nullptr) {
            *error_detail = "DHT indirect chunk publish in progress";
            if (any_chunk_touched) {
                *error_detail += "; last_chunk=" + std::to_string(last_touched_chunk);
            }
            *error_detail += " published_chunks="
                + std::to_string(published_count)
                + "/"
                + std::to_string(chunk_count);
            if (any_chunk_pending && !first_pending_error.empty()) {
                *error_detail += "; first_pending=" + first_pending_error;
            }
        }
        return DhtPublishState::kPending;
    }

    if (touched >= 8 || std::chrono::steady_clock::now() - turn_started >= std::chrono::milliseconds(5)) {
        return DhtPublishState::kPending;
    }
    if (secondary_store_ != nullptr) {
        DhtEncryptedRecord secondary_record = record;
        secondary_record.lane = blob_lane;
        std::string secondary_error;
        (void)secondary_store_->publish(secondary_record, "", &secondary_error);
    }

    IndirectRecordSummary summary;
    summary.blob_lane = blob_lane;
    summary.blob_bytes = static_cast<std::uint64_t>(record.encrypted_blob.size());
    summary.blob_sha256 = blob_sha256;
    summary.chunk_count = chunk_count;
    summary.chunk_bytes = static_cast<std::uint64_t>(kIndirectDhtChunkPayloadBytes);

    DhtEncryptedRecord primary_record = record;
    primary_record.encrypted_blob = serialize_indirect_summary(summary);
    const auto summary_result = primary_store_->publish(primary_record, routing_token, error_detail);
    if (!summary_result) {
        if (error_detail != nullptr && !error_detail->empty()) {
            *error_detail = "failed to publish compact DHT summary: " + *error_detail;
        }
        return summary_result;
    }
    std::fill(cached_chunk_publish_success_.begin(), cached_chunk_publish_success_.end(), false);
    return true;
}

std::optional<DhtEncryptedRecord> IndirectDhtRendezvousStore::fetch(
    std::string_view topic_hex,
    std::string_view lane,
    std::uint64_t after_revision,
    std::string_view routing_token,
    std::string* error_detail) {
    if (primary_store_ == nullptr) {
        if (error_detail != nullptr) {
            *error_detail = "indirect DHT primary store is not configured";
        }
        return std::nullopt;
    }

    const auto primary_record = primary_store_->fetch(
        topic_hex,
        lane,
        after_revision,
        routing_token,
        error_detail);
    if (!primary_record.has_value()) {
        if (secondary_store_ != nullptr) {
            auto* secondary_file_store = dynamic_cast<FileBackedDhtRendezvousStore*>(secondary_store_.get());
            if (secondary_file_store == nullptr) {
                return std::nullopt;
            }
            std::string secondary_error;
            auto secondary_record = secondary_file_store->fetch_latest_indirect_blob(
                topic_hex,
                lane,
                after_revision,
                &secondary_error);
            if (secondary_record.has_value()) {
                secondary_record->topic_hex = std::string(topic_hex);
                secondary_record->lane = std::string(lane);
                if (error_detail != nullptr) {
                    error_detail->clear();
                }
                return secondary_record;
            }
            if (error_detail != nullptr && !secondary_error.empty()) {
                *error_detail = secondary_error;
            }
        }
        return std::nullopt;
    }
    if (!is_indirect_summary(primary_record->encrypted_blob)) {
        return primary_record;
    }

    const auto summary = parse_indirect_summary(primary_record->encrypted_blob, error_detail);
    if (!summary.has_value()) {
        return std::nullopt;
    }

    std::optional<DhtEncryptedRecord> indirect_record;
    std::string indirect_error;
    if (summary->chunk_count > 0) {
        std::string encrypted_blob;
        encrypted_blob.reserve(static_cast<std::size_t>(summary->blob_bytes));
        bool chunks_complete = true;
        for (std::uint64_t index = 0; index < summary->chunk_count; ++index) {
            const std::string chunk_lane = make_indirect_chunk_lane(summary->blob_lane, index);
            const std::string chunk_topic = make_indirect_chunk_topic_hex(topic_hex, chunk_lane);
            const std::string chunk_routing_token = derive_indirect_chunk_routing_token(routing_token, chunk_lane);
            std::string chunk_error;
            auto chunk_record = primary_store_->fetch(
                chunk_topic,
                chunk_lane,
                0,
                chunk_routing_token,
                &chunk_error);
            if (!chunk_record.has_value()) {
                chunks_complete = false;
                indirect_error = chunk_error.empty()
                    ? "DHT indirect chunk is missing: " + chunk_lane
                    : chunk_error;
                break;
            }
            if (chunk_record->revision != primary_record->revision) {
                chunks_complete = false;
                indirect_error = "DHT indirect chunk metadata mismatch"
                    + std::string(" summary_revision=") + std::to_string(primary_record->revision)
                    + " chunk_revision=" + std::to_string(chunk_record->revision)
                    + " summary_expires=" + std::to_string(primary_record->expires_at_unix)
                    + " chunk_expires=" + std::to_string(chunk_record->expires_at_unix);
                break;
            }
            encrypted_blob += chunk_record->encrypted_blob;
        }

        if (chunks_complete) {
            DhtEncryptedRecord chunked_record = *primary_record;
            chunked_record.lane = std::string(lane);
            chunked_record.encrypted_blob = std::move(encrypted_blob);
            indirect_record = std::move(chunked_record);
        }
    }

    if (!indirect_record.has_value() && secondary_store_ != nullptr) {
        std::string secondary_error;
        auto secondary_record = secondary_store_->fetch(
            topic_hex,
            summary->blob_lane,
            0,
            "",
            &secondary_error);
        if (secondary_record.has_value()) {
            indirect_record = std::move(secondary_record);
        } else if (indirect_error.empty()) {
            indirect_error = secondary_error.empty()
                ? "DHT indirect blob is missing from DHT chunks and secondary store"
                : secondary_error;
        }
    }
    if (!indirect_record.has_value()) {
        if (error_detail != nullptr) {
            *error_detail = indirect_error.empty()
                ? "DHT indirect blob is missing from DHT chunks"
                : indirect_error;
        }
        return std::nullopt;
    }
    if (indirect_record->revision != primary_record->revision
        || indirect_record->expires_at_unix != primary_record->expires_at_unix) {
        if (error_detail != nullptr) {
            *error_detail = "DHT indirect blob metadata mismatch"
                + std::string(" summary_revision=") + std::to_string(primary_record->revision)
                + " blob_revision=" + std::to_string(indirect_record->revision)
                + " summary_expires=" + std::to_string(primary_record->expires_at_unix)
                + " blob_expires=" + std::to_string(indirect_record->expires_at_unix);
        }
        return std::nullopt;
    }
    if (static_cast<std::uint64_t>(indirect_record->encrypted_blob.size()) != summary->blob_bytes) {
        if (error_detail != nullptr) {
            *error_detail = "DHT indirect blob size mismatch"
                + std::string(" summary_blob_bytes=") + std::to_string(summary->blob_bytes)
                + " actual_blob_bytes=" + std::to_string(indirect_record->encrypted_blob.size());
        }
        return std::nullopt;
    }
    const std::string actual_blob_sha256 = sha256_hex(indirect_record->encrypted_blob);
    if (actual_blob_sha256 != summary->blob_sha256) {
        if (error_detail != nullptr) {
            *error_detail = "DHT indirect blob digest mismatch"
                + std::string(" summary_blob_sha256=") + summary->blob_sha256
                + " actual_blob_sha256=" + actual_blob_sha256;
        }
        return std::nullopt;
    }

    indirect_record->topic_hex = std::string(topic_hex);
    indirect_record->lane = std::string(lane);
    return indirect_record;
}

DhtRendezvousClient::DhtRendezvousClient(IDhtRendezvousStore& store)
    : store_(store) {}

DhtPublishResult DhtRendezvousClient::publish_signal_snapshot(
    const DhtRendezvousConfig& config,
    const DhtSignalSnapshot& snapshot,
    std::string* error_detail) {
    store_.advance();
    const std::string publish_cache_key = std::string(config.session_code)
        + "\n"
        + std::string(config.pairing_secret)
        + "\n"
        + std::to_string(snapshot.revision)
        + "\n"
        + serialize_plain_snapshot(snapshot);

    // Retries reuse the cached ciphertext so the transport can deduplicate an
    // in-flight publish and so a chunked publish is never restarted midway.
    // A record is only re-sealed under a new revision, which keeps the
    // previously published chunk set readable until the new one lands.
    if (!cached_publish_record_.has_value() || cached_publish_key_ != publish_cache_key) {
        DhtEncryptedRecord record;
        if (!encrypt_snapshot(config, snapshot, &record, error_detail)) {
            return false;
        }
        cached_publish_key_ = publish_cache_key;
        cached_publish_record_ = std::move(record);
    }

    const DhtEncryptedRecord& record = *cached_publish_record_;
    if (record.expires_at_unix <= config.now_unix) {
        if (error_detail) *error_detail = "DHT publication record expired";
        DhtPublishResult expired{DhtPublishState::kPermanentFailure, DhtPublishFailure::kExpired};
        expired.expires_at_unix = record.expires_at_unix;
        return expired;
    }
    auto result = store_.publish(
        record,
        derive_dht_rendezvous_routing_token_hex(config.session_code, config.pairing_secret, snapshot.role),
        error_detail);
    result.expires_at_unix = record.expires_at_unix;
    return result;
}

std::optional<DhtSignalSnapshot> DhtRendezvousClient::fetch_signal_snapshot(
    const DhtRendezvousConfig& config,
    std::string_view lane,
    std::uint64_t after_revision,
    std::string* error_detail) {
    const std::string topic = derive_dht_rendezvous_topic_hex(config.session_code, config.pairing_secret);
    const auto record = store_.fetch(
        topic,
        lane,
        after_revision,
        derive_dht_rendezvous_routing_token_hex(config.session_code, config.pairing_secret, lane),
        error_detail);
    if (!record.has_value()) {
        return std::nullopt;
    }
    const auto snapshot = decrypt_snapshot(config, *record, error_detail);
    if (!snapshot.has_value()) {
        return std::nullopt;
    }
    if (snapshot->role != lane) {
        if (error_detail != nullptr) {
            *error_detail = "DHT signal snapshot role mismatch";
        }
        return std::nullopt;
    }
    return snapshot;
}

std::vector<std::string> default_dht_bootstrap_nodes() {
    return {
        "router.bittorrent.com:6881",
        "dht.transmissionbt.com:6881",
        "dht.libtorrent.org:25401",
        "router.utorrent.com:6881",
    };
}

std::vector<std::string> cached_dht_bootstrap_fallback_nodes(std::string_view node) {
    const std::string lowered = to_lower_ascii(trim_copy(node));
    if (lowered == "router.bittorrent.com:6881") {
        return {"67.215.246.10:6881"};
    }
    if (lowered == "dht.transmissionbt.com:6881") {
        return {
            "87.98.162.88:6881",
            "212.129.33.59:6881",
        };
    }
    if (lowered == "dht.libtorrent.org:25401") {
        return {"185.157.221.247:25401"};
    }
    if (lowered == "router.utorrent.com:6881") {
        return {"82.221.103.244:6881"};
    }
    return {};
}

std::vector<std::string> normalize_dht_bootstrap_nodes(const std::vector<std::string>& requested) {
    std::vector<std::string> nodes;
    std::unordered_set<std::string> seen;
    const auto& source = requested.empty() ? default_dht_bootstrap_nodes() : requested;
    for (const std::string& node : source) {
        const std::string trimmed = trim_copy(node);
        if (trimmed.empty()) {
            continue;
        }

        const std::string lowered = to_lower_ascii(trimmed);
        const std::vector<std::string> fallbacks = cached_dht_bootstrap_fallback_nodes(trimmed);
        for (const std::string& fallback : fallbacks) {
            const std::string fallback_key = to_lower_ascii(fallback);
            if (seen.insert(fallback_key).second) {
                nodes.push_back(fallback);
            }
        }

        // Known default hostnames are replaced by cached numeric fallbacks so
        // local fake-DNS pools (for example FlClash 28.0.0.x) cannot block bootstrap.
        if (fallbacks.empty() && seen.insert(lowered).second) {
            nodes.push_back(trimmed);
        }
    }
    return nodes;
}

}  // namespace redclaw::service
