#include "redclaw/security/connection_auth.h"

#include <array>
#include <charconv>
#include <map>
#include <fstream>
#include <chrono>
#include <thread>
#ifndef _WIN32
#include <poll.h>
#include <unistd.h>
#endif
#include <boost/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#endif

namespace redclaw::security {
namespace {
using Message = protocol::StreamControlMessageV1;
std::string encode(std::string_view bytes) {
    std::string result(4 * ((bytes.size() + 2) / 3), '\0');
    if (!bytes.empty()) EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
        reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    return result;
}
std::string decode(std::string_view text) {
    if (text.empty() || text.size() % 4 || text.size() > 16384) return {};
    std::string result(text.size() / 4 * 3, '\0');
    int length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(result.data()),
        reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()));
    if (length < 0) return {};
    if (text.back() == '=') --length;
    if (text.size() > 1 && text[text.size() - 2] == '=') --length;
    if (length < 0) return {};
    result.resize(static_cast<std::size_t>(length));
    return encode(result) == text ? result : std::string{};
}
std::string random_bytes(std::size_t count) {
    std::string result(count, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(result.data()), static_cast<int>(count)) != 1) return {};
    return result;
}
std::string hash(std::string_view value) {
    std::string result(32, '\0'); unsigned length = 0;
    if (EVP_Digest(value.data(), value.size(), reinterpret_cast<unsigned char*>(result.data()),
        &length, EVP_sha256(), nullptr) != 1) return {};
    return result;
}
std::string mac(std::string_view key, std::string_view value) {
    std::string result(32, '\0'); unsigned length = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
        reinterpret_cast<const unsigned char*>(value.data()), value.size(),
        reinterpret_cast<unsigned char*>(result.data()), &length)) return {};
    return result;
}
std::string derive(std::string_view password, std::string_view salt, unsigned iterations) {
    std::string result(32, '\0');
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
        reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()),
        static_cast<int>(iterations), EVP_sha256(), 32,
        reinterpret_cast<unsigned char*>(result.data())) != 1) return {};
    return result;
}
bool equal(std::string_view left, std::string_view right) {
    return !left.empty() && left.size() == right.size() && CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}
std::string xor_bytes(std::string a, std::string_view b) {
    if (a.size() != 32 || b.size() != 32) return {};
    for (std::size_t i = 0; i < a.size(); ++i) a[i] ^= b[i];
    return a;
}
boost::json::object object(std::string_view text) {
    boost::system::error_code error;
    auto parsed = boost::json::parse(text, error);
    return !error && parsed.is_object() ? parsed.as_object() : boost::json::object{};
}
std::string field(const boost::json::object& value, std::string_view name) {
    const auto* found = value.if_contains(name);
    return found && found->is_string() ? std::string(found->as_string()) : std::string{};
}
std::map<char, std::string> attributes(std::string_view text) {
    std::map<char, std::string> result;
    while (!text.empty()) {
        const auto end = text.find(','); auto part = text.substr(0, end);
        if (part.size() < 3 || part[1] != '=' || result.contains(part[0])) return {};
        result.emplace(part[0], part.substr(2));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return result;
}
bool password_valid(std::string_view password) {
    return !password.empty() && password.size() <= kMaxConnectionPasswordBytes
        && password.find('\0') == std::string_view::npos;
}
}

void erase_secret(std::string& value) { if (!value.empty()) OPENSSL_cleanse(value.data(), value.size()); value.clear(); }

bool make_connection_verifier(std::string_view password, std::string* verifier) {
    if (!verifier || !password_valid(password)) return false;
    const auto salt = random_bytes(16); if (salt.empty()) return false;
    auto salted = derive(password, salt, kConnectionPasswordIterations);
    auto client = mac(salted, "Client Key");
    if (salted.empty() || client.empty()) return false;
    boost::json::object value{{"version", 1}, {"salt", encode(salt)},
        {"iterations", kConnectionPasswordIterations}, {"stored_key", encode(hash(client))},
        {"server_key", encode(mac(salted, "Server Key"))}};
    *verifier = boost::json::serialize(value);
    erase_secret(salted); erase_secret(client);
    return true;
}
bool valid_connection_verifier(std::string_view verifier) {
    if (verifier.size() > 4096) return false;
    const auto value = object(verifier);
    const auto* version = value.if_contains("version"); const auto* iterations = value.if_contains("iterations");
    return version && *version == 1 && iterations && *iterations == kConnectionPasswordIterations
        && decode(field(value, "salt")).size() == 16
        && decode(field(value, "stored_key")).size() == 32
        && decode(field(value, "server_key")).size() == 32;
}
bool protect_connection_credential(std::string_view plaintext, std::string* protected_text) {
#ifdef _WIN32
    if (!protected_text || plaintext.empty() || plaintext.size() > 8192) return false;
    DATA_BLOB input{static_cast<DWORD>(plaintext.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"RedClaw connection credential v1", nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    *protected_text = encode({reinterpret_cast<const char*>(output.pbData), output.cbData});
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); return true;
#else
    (void)plaintext; (void)protected_text; return false;
#endif
}
bool unprotect_connection_credential(std::string_view protected_text, std::string* plaintext) {
#ifdef _WIN32
    if (!plaintext) return false;
    auto bytes = decode(protected_text); if (bytes.empty()) return false;
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    const bool valid = output.cbData > 0 && output.cbData <= 8192;
    if (valid) plaintext->assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); return valid;
#else
    (void)protected_text; (void)plaintext; return false;
#endif
}
std::string serialize_local_connection_credential(const ConnectionCredential& credential) {
    return std::string(kLocalConnectionCredentialPrefix) + boost::json::serialize(boost::json::object{
        {"version", 1}, {"role", credential.host ? "host" : "controller"}, {"secret", credential.secret}}) + "\n";
}
bool parse_local_connection_credential(std::string_view line, ConnectionCredential* credential) {
    if (!credential || !line.starts_with(kLocalConnectionCredentialPrefix) || line.size() > 16384) return false;
    auto value = object(line.substr(kLocalConnectionCredentialPrefix.size()));
    const auto* version = value.if_contains("version"); const auto role = field(value, "role");
    if (!version || *version != 1 || (role != "host" && role != "controller")) return false;
    ConnectionCredential parsed{role == "host", field(value, "secret")};
    if (parsed.host ? !valid_connection_verifier(parsed.secret) : !password_valid(parsed.secret)) return false;
    *credential = std::move(parsed); return true;
}

ConnectionAuthenticator::ConnectionAuthenticator(ConnectionCredential credential) : credential_(std::move(credential)) {}
bool load_connection_credential_file(const std::filesystem::path& path, ConnectionCredential* credential) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > 16384) return false;
    std::ifstream input(path, std::ios::binary);
    std::string protected_text(static_cast<std::size_t>(size), '\0');
    if (!input.read(protected_text.data(), static_cast<std::streamsize>(size))) return false;
    std::string plaintext;
    if (!unprotect_connection_credential(protected_text, &plaintext)) return false;
    const bool valid = parse_local_connection_credential(plaintext, credential);
    erase_secret(plaintext); return valid;
}
bool read_connection_credential_pipe(ConnectionCredential* credential) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string line;
    while (line.size() < 16384 && std::chrono::steady_clock::now() < deadline) {
        char ch = 0;
#ifdef _WIN32
        const auto handle = GetStdHandle(STD_INPUT_HANDLE);
        if (!handle || handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_PIPE) break;
        DWORD available = 0, read = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) break;
        if (!available) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
        if (!ReadFile(handle, &ch, 1, &read, nullptr) || read != 1) break;
#else
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        if (poll(&descriptor, 1, 5) <= 0) continue;
        if (read(STDIN_FILENO, &ch, 1) != 1) break;
#endif
        if (ch == '\n') { const bool ok = parse_local_connection_credential(line, credential); erase_secret(line); return ok; }
        line += ch;
    }
    erase_secret(line); return false;
}
ConnectionAuthenticator::~ConnectionAuthenticator() { erase_secret(credential_.secret); reset(); }
bool ConnectionAuthenticator::ready() const {
    return credential_.host ? valid_connection_verifier(credential_.secret) : password_valid(credential_.secret);
}
void ConnectionAuthenticator::reset() {
    state_ = ConnectionAuthState::kPending; error_.clear(); phase_.clear();
    local_fingerprint_.clear(); remote_fingerprint_.clear(); epoch_.clear(); peer_epoch_.clear();
    nonce_.clear(); client_first_.clear(); server_first_.clear(); auth_message_.clear();
    erase_secret(server_signature_); deadline_ms_ = 0; sequence_ = 0;
}
Message ConnectionAuthenticator::frame(std::string step, std::string data) {
    Message message; message.type = protocol::StreamControlMessageTypeV1::kConnectionAuth;
    message.connection_auth_version = 1; message.session_epoch = epoch_;
    message.message_id = ++sequence_; message.sent_at_ms = 1;
    message.auth_step = std::move(step); message.auth_data = std::move(data); return message;
}
std::vector<Message> ConnectionAuthenticator::reject(std::string error, std::uint64_t now_ms) {
    if (state_ == ConnectionAuthState::kRejected) return {};
    state_ = ConnectionAuthState::kRejected; error_ = std::move(error);
    if (credential_.host && error_ == "connection_password_rejected" && ++failures_ >= kConnectionAuthFailureLimit) {
        cooldown_until_ms_ = now_ms + kConnectionAuthCooldownMs; failures_ = 0;
    }
    return {frame("rejected", error_)};
}
std::vector<Message> ConnectionAuthenticator::open(std::string local, std::string remote, std::uint64_t now_ms) {
    reset(); epoch_ = encode(random_bytes(24));
    // Epochs are protocol tokens, so use a hex-independent base64url spelling.
    for (auto& c : epoch_) { if (c == '+') c = '-'; if (c == '/') c = '_'; }
    if (!ready() || epoch_.empty() || local.empty() || remote.empty()) return reject("connection_credentials_unavailable", now_ms);
    if (credential_.host && now_ms < cooldown_until_ms_) return reject("connection_auth_cooldown", now_ms);
    local_fingerprint_ = std::move(local); remote_fingerprint_ = std::move(remote);
    state_ = ConnectionAuthState::kVerifying; phase_ = "hello"; deadline_ms_ = now_ms + kConnectionAuthTimeoutMs;
    auto hello = frame("hello", credential_.host ? "host" : "controller");
    hello.type = protocol::StreamControlMessageTypeV1::kHello;
    return {std::move(hello)};
}
std::string ConnectionAuthenticator::binding() const {
    return "redclaw-dtls-endpoints-v1\nhost\n" + (credential_.host ? local_fingerprint_ : remote_fingerprint_)
        + "\ncontroller\n" + (credential_.host ? remote_fingerprint_ : local_fingerprint_)
        + "\nhost-epoch\n" + (credential_.host ? epoch_ : peer_epoch_)
        + "\ncontroller-epoch\n" + (credential_.host ? peer_epoch_ : epoch_);
}
void ConnectionAuthenticator::tick(std::uint64_t now_ms) {
    if (state_ == ConnectionAuthState::kVerifying && now_ms >= deadline_ms_) {
        (void)reject("connection_auth_timeout", now_ms);
    }
}
std::vector<Message> ConnectionAuthenticator::receive(const Message& message, std::uint64_t now_ms) {
    tick(now_ms);
    if (state_ != ConnectionAuthState::kVerifying) return {};
    if (message.connection_auth_version != 1) return reject("protocol_version_incompatible", now_ms);
    if (message.auth_step == "rejected") {
        const bool peer_diagnostic = message.auth_data == "protocol_version_incompatible"
            || (!credential_.host && (message.auth_data == "connection_auth_cooldown"
                || message.auth_data == "connection_auth_timeout"
                || message.auth_data == "connection_credentials_unavailable"));
        return reject(peer_diagnostic ? message.auth_data : "connection_password_rejected", now_ms);
    }
    if (phase_ == "hello") {
        if (message.type != protocol::StreamControlMessageTypeV1::kHello || message.auth_step != "hello"
            || message.auth_data != (credential_.host ? "controller" : "host")
            || message.session_epoch.empty() || message.session_epoch == epoch_) return reject("connection_password_rejected", now_ms);
        peer_epoch_ = message.session_epoch;
        phase_ = credential_.host ? "client_first" : "server_first";
        if (credential_.host) return {};
        nonce_ = encode(random_bytes(24)); if (nonce_.empty()) return reject("connection_credentials_unavailable", now_ms);
        client_first_ = "n=redclaw,r=" + nonce_;
        return {frame("client_first", client_first_)};
    }
    if (message.session_epoch != peer_epoch_ || message.auth_step != phase_)
        return reject("connection_password_rejected", now_ms);
    auto attr = attributes(message.auth_data);
    if (phase_ == "client_first") {
        if (attr.size() != 2 || attr['n'] != "redclaw" || decode(attr['r']).size() != 24)
            return reject("connection_password_rejected", now_ms);
        const auto suffix = encode(random_bytes(24)); if (suffix.empty()) return reject("connection_credentials_unavailable", now_ms);
        nonce_ = attr['r'] + suffix; client_first_ = message.auth_data;
        const auto verifier = object(credential_.secret);
        server_first_ = "r=" + nonce_ + ",s=" + field(verifier, "salt") + ",i=" + std::to_string(kConnectionPasswordIterations);
        phase_ = "client_final"; return {frame("server_first", server_first_)};
    }
    if (phase_ == "server_first") {
        if (attr.size() != 3 || !attr['r'].starts_with(nonce_) || attr['r'].size() != nonce_.size() * 2
            || decode(attr['s']).size() != 16 || attr['i'] != std::to_string(kConnectionPasswordIterations))
            return reject("connection_password_rejected", now_ms);
        nonce_ = attr['r']; server_first_ = message.auth_data;
        const auto final = "c=" + encode(binding()) + ",r=" + nonce_;
        auth_message_ = client_first_ + "," + server_first_ + "," + final;
        auto salted = derive(credential_.secret, decode(attr['s']), kConnectionPasswordIterations);
        auto key = mac(salted, "Client Key");
        const auto proof = encode(xor_bytes(key, mac(hash(key), auth_message_)));
        server_signature_ = mac(mac(salted, "Server Key"), auth_message_);
        erase_secret(salted); erase_secret(key);
        if (proof.empty() || server_signature_.empty()) return reject("connection_credentials_unavailable", now_ms);
        phase_ = "server_final"; return {frame("client_final", final + ",p=" + proof)};
    }
    if (phase_ == "client_final") {
        if (attr.size() != 3 || attr['r'] != nonce_ || decode(attr['c']) != binding() || decode(attr['p']).size() != 32)
            return reject("connection_password_rejected", now_ms);
        const auto final = "c=" + attr['c'] + ",r=" + attr['r'];
        auth_message_ = client_first_ + "," + server_first_ + "," + final;
        auto verifier = object(credential_.secret); const auto stored = decode(field(verifier, "stored_key"));
        auto recovered = xor_bytes(decode(attr['p']), mac(stored, auth_message_));
        const bool valid = equal(hash(recovered), stored); erase_secret(recovered);
        if (!valid) return reject("connection_password_rejected", now_ms);
        server_signature_ = mac(decode(field(verifier, "server_key")), auth_message_);
        phase_ = "finished"; return {frame("server_final", "v=" + encode(server_signature_))};
    }
    if (phase_ == "server_final") {
        if (attr.size() != 1 || !equal(decode(attr['v']), server_signature_)) return reject("connection_password_rejected", now_ms);
        phase_ = "accepted"; return {frame("finished", encode(mac(server_signature_, "finished," + auth_message_)))};
    }
    if (phase_ == "finished") {
        if (!equal(decode(message.auth_data), mac(server_signature_, "finished," + auth_message_)))
            return reject("connection_password_rejected", now_ms);
        state_ = ConnectionAuthState::kAccepted; failures_ = 0;
        return {frame("accepted", encode(mac(server_signature_, "accepted," + auth_message_)))};
    }
    if (phase_ == "accepted") {
        if (!equal(decode(message.auth_data), mac(server_signature_, "accepted," + auth_message_)))
            return reject("connection_password_rejected", now_ms);
        state_ = ConnectionAuthState::kAccepted; return {};
    }
    return reject("connection_password_rejected", now_ms);
}
} // namespace redclaw::security
