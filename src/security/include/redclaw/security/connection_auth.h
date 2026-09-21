#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "redclaw/protocol/stream_control_protocol.h"

namespace redclaw::security {

inline constexpr std::uint32_t kConnectionAuthVersion = 1;
inline constexpr std::uint32_t kConnectionPasswordIterations = 120000;
inline constexpr std::uint64_t kConnectionAuthTimeoutMs = 30000;
inline constexpr std::uint64_t kConnectionAuthCooldownMs = 30000;
inline constexpr unsigned kConnectionAuthFailureLimit = 5;
inline constexpr std::size_t kMaxConnectionPasswordBytes = 1024;
inline constexpr std::string_view kLocalConnectionCredentialPrefix = "RCD-LOCAL-AUTH-V1 ";

enum class ConnectionAuthState { kPending, kVerifying, kAccepted, kRejected };

// The verifier contains salt, StoredKey and ServerKey, never the password.
bool make_connection_verifier(std::string_view password, std::string* verifier);
bool valid_connection_verifier(std::string_view verifier);
bool protect_connection_credential(std::string_view plaintext, std::string* protected_text);
bool unprotect_connection_credential(std::string_view protected_text, std::string* plaintext);
void erase_secret(std::string& value);

struct ConnectionCredential {
    bool host = false;
    std::string secret; // Host: serialized verifier. Client: exact UTF-8 password.
};
std::string serialize_local_connection_credential(const ConnectionCredential& credential);
bool parse_local_connection_credential(std::string_view line, ConnectionCredential* credential);
bool load_connection_credential_file(const std::filesystem::path& path, ConnectionCredential* credential);
bool read_connection_credential_pipe(ConnectionCredential* credential);

// Owned and serialized by the transport. reset() preserves the Host failure budget.
class ConnectionAuthenticator final {
public:
    explicit ConnectionAuthenticator(ConnectionCredential credential);
    ~ConnectionAuthenticator();
    ConnectionAuthenticator(const ConnectionAuthenticator&) = delete;
    ConnectionAuthenticator& operator=(const ConnectionAuthenticator&) = delete;
    void reset();
    std::vector<protocol::StreamControlMessageV1> open(
        std::string local_fingerprint, std::string remote_fingerprint, std::uint64_t now_ms);
    std::vector<protocol::StreamControlMessageV1> receive(
        const protocol::StreamControlMessageV1& message, std::uint64_t now_ms);
    void tick(std::uint64_t now_ms);
    [[nodiscard]] ConnectionAuthState state() const { return state_; }
    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] bool host() const { return credential_.host; }
    [[nodiscard]] bool ready() const;
private:
    protocol::StreamControlMessageV1 frame(std::string step, std::string data);
    std::vector<protocol::StreamControlMessageV1> reject(std::string error, std::uint64_t now_ms);
    std::string binding() const;
    ConnectionCredential credential_;
    ConnectionAuthState state_ = ConnectionAuthState::kPending;
    std::string error_, local_fingerprint_, remote_fingerprint_, epoch_, peer_epoch_;
    std::string nonce_, client_first_, server_first_, auth_message_, server_signature_;
    std::string phase_;
    std::uint64_t deadline_ms_ = 0, cooldown_until_ms_ = 0, sequence_ = 0;
    unsigned failures_ = 0;
};
} // namespace redclaw::security
