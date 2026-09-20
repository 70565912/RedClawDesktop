#include <iostream>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <cctype>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <Windows.h>
#endif

#include "redclaw/core/core_module.h"
#include "redclaw/protocol/protocol_module.h"
#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/protocol/agent_protocol.h"
#include "redclaw/agent/agent_providers.h"
#include "redclaw/agent/remote_agent_broker.h"
#include "redclaw/agent/agent_peer_session.h"
#include "redclaw/agent/agent_executor.h"
#include "redclaw/agent/local_agent_pipe.h"
#include "redclaw/workspace/terminal_runtime_bridge.h"
#include "redclaw/workspace/transfer_operation_gate.h"
#include "redclaw/workspace/transfer_runtime_bridge.h"
#include "redclaw/net/net_module.h"
#include "redclaw/net/video_frame_transport.h"
#include "redclaw/security/security_module.h"
#include "redclaw/session/session_module.h"
#include "redclaw/capture/capture_module.h"
#include "redclaw/capture/capture_stream_gate.h"
#include "redclaw/render/render_module.h"
#include "redclaw/input/input_module.h"
#include "redclaw/service/service_module.h"
#include "redclaw/service/connection_negotiation.h"
#include "redclaw/service/dht_rendezvous.h"
#include "redclaw/service/dht_publication_transaction.h"
#include "redclaw/service/upnp_port_mapping.h"
#include "redclaw/diag/diag_module.h"
#include "redclaw/helper/direct_frame_shared_memory.h"
#include "redclaw/helper/runtime_profile.h"
#include "ui/gui_shell.h"
#include "runtime/runtime_options.h"
#include "runtime/local_control_output.h"
#include "runtime/input_qa_receipts.h"
#include "runtime/runtime_loop_wake.h"

namespace {
constexpr std::uint32_t kDesktopStreamTargetFps = 30;
constexpr std::uint64_t kDesktopStreamFrameIntervalMs = 1000ULL / kDesktopStreamTargetFps;
constexpr std::uint32_t kDesktopStreamCaptureAcquireTimeoutMs = 50;
constexpr std::uint32_t kDesktopStreamCaptureIdleWaitMs = 50;
constexpr std::uint32_t kRuntimeLoopIntervalMs = 20;
constexpr std::uint32_t kDhtAlertPumpIntervalMs = 250;
constexpr std::uint32_t kDhtPublishRefreshMs = 30000;
// Bounded Host-only repair of abandoned records. Controller network failures
// keep retrying with capped backoff until the runtime is explicitly stopped.
// A record whose seal is already old points at a peer that is very likely gone,
// so that case gives up quickly instead of waiting for late candidates.
constexpr std::uint32_t kDhtSignalingRepairDelayMs = 15000;
constexpr std::uint32_t kDhtSignalingRepairPatientDelayMs = 60000;
constexpr std::int64_t kDhtAbandonedRecordSealAgeSeconds = 90;
constexpr std::uint32_t kDhtAbandonedHostRepairAttemptLimit = 20;
constexpr std::uint32_t kDhtStaleRemoteFetchBackoffMs = 5000;
// After a size-gated description-only publication, send one direct
// relay/srflx/host candidate before attempting the complete snapshot.
constexpr std::size_t kDhtDirectCandidatePublishLimit = 1;
constexpr std::size_t kDesktopStreamVideoFragmentPacketBytes = 16 * 1024;
constexpr std::size_t kDesktopStreamDataChannelBufferedAmountResumeBytes = 512 * 1024;
constexpr std::size_t kDesktopStreamDataChannelBufferedAmountHighWatermarkBytes =
    kDesktopStreamDataChannelBufferedAmountResumeBytes + (32 * kDesktopStreamVideoFragmentPacketBytes);
constexpr std::size_t kRemoteInputControlBufferedAmountHighWatermarkBytes = 64 * 1024;
constexpr std::size_t kAgentDataChannelBufferedAmountHighWatermarkBytes = 256 * 1024;
constexpr std::uint64_t kAgentDataChannelRebuildOpenTimeoutMs = 3000;
constexpr std::uint64_t kAgentDataChannelRebuildStableMs = 3000;
constexpr std::uint32_t kAgentDataChannelRebuildMaxAttempts = 3;
constexpr std::uint32_t kDesktopStreamAdaptiveBitrateFloorKbps = 400;
constexpr std::uint32_t kDesktopStreamTransportBitrateCeilingKbps = 20000;
constexpr std::uint64_t kDesktopStreamPlaybackStarvationHoldMs = 5000;
constexpr std::uint64_t kDesktopStreamRttPingIntervalMs = 1000;
constexpr std::uint64_t kDesktopStreamRttPingTimeoutMs = 3000;
// FFmpeg requires a non-zero time base. One FPS is the only lower bound; weak
// links are allowed to sacrifice cadence completely to preserve decodable frames.
constexpr std::uint32_t kDesktopStreamAdaptiveMinFps = 1;
constexpr std::uint64_t kDesktopStreamAdaptiveReconfigureCooldownMs = 2000;
constexpr std::uint64_t kDesktopStreamEncoderStartRetryCooldownMs = 5000;
constexpr std::uint32_t kDesktopStreamAdaptiveReliefQueueDelayMs = 40;
constexpr std::uint32_t kDesktopStreamAdaptiveHighQueueDelayMs = 120;
constexpr std::uint32_t kDesktopStreamAdaptiveSevereQueueDelayMs = 200;
constexpr std::uint32_t kDesktopStreamAdaptivePressureWindowThreshold = 3;
constexpr std::uint32_t kDesktopStreamAdaptiveReliefWindowThreshold = 2;
constexpr std::uint32_t kDesktopStreamAdaptiveBackpressureReconfigureThreshold = 6;
constexpr std::uint64_t kRequiredChannelLossGraceMs = 3000;
constexpr std::uint64_t kAutomaticReconnectAttemptWindowMs = 90'000;
constexpr std::uint64_t kStreamQaRequiredChannelStableMs = 5000;
constexpr std::uint64_t kDesktopStreamFirstMediaDeadlineMs = 20000;
constexpr std::uint64_t kDesktopStreamInitialViewportGraceMs = 3000;
constexpr std::uint32_t kDesktopStreamViewportReconnectExtraSends = 2;
constexpr std::size_t kDesktopStreamSentMetadataCapacity = 256;

using redclaw::runtime::RuntimeOptions;
using redclaw::runtime::RuntimeRole;
using redclaw::runtime::parse_runtime_options;
using redclaw::runtime::print_usage;

bool parse_uint64(std::string_view value, std::uint64_t* out) {
    if (value.empty()) {
        return false;
    }

    std::uint64_t parsed = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }

        const std::uint64_t next = (parsed * 10ULL) + static_cast<std::uint64_t>(ch - '0');
        if (next < parsed) {
            return false;
        }
        parsed = next;
    }

    *out = parsed;
    return true;
}

redclaw::session::FingerprintTrustPolicyRuntime::LoadFallbackPolicy parse_fallback_policy(std::string_view value) {
    if (value == "retain") {
        return redclaw::session::FingerprintTrustPolicyRuntime::LoadFallbackPolicy::kRetainExistingOnError;
    }
    return redclaw::session::FingerprintTrustPolicyRuntime::LoadFallbackPolicy::kClearOnError;
}

void bootstrap_session_security_policy_wiring() {
    redclaw::security::InMemoryPeerFingerprintVerifier fingerprint_verifier;
    const auto bootstrap_config = redclaw::service::resolve_session_security_bootstrap_config();
    const auto fallback_policy = parse_fallback_policy(bootstrap_config.trust_store_fallback);

    redclaw::security::FileBackedPeerFingerprintStore trust_store(bootstrap_config.trust_store_path);
    redclaw::session::FingerprintTrustPolicyRuntime trust_policy_runtime(fingerprint_verifier, trust_store);

    std::string policy_error;
    if (trust_policy_runtime.load_on_startup(fallback_policy, &policy_error)) {
        std::cout << "Session trust policy loaded" << '\n';
    } else {
        std::cout << "Session trust policy load skipped: " << policy_error << '\n';
    }

    // Keep a minimal startup-path handshake ingress wiring to validate module linkage.
    std::uint64_t now_ms = 0;
    redclaw::security::InMemoryHandshakeReplayGuard replay_guard(
        {},
        [&now_ms]() { return now_ms; });
    redclaw::session::IncomingHandshakeProcessor handshake_processor(replay_guard, fingerprint_verifier);

    (void)trust_policy_runtime;
    (void)handshake_processor;
}

struct FileSignalingPaths {
    std::filesystem::path directory;
    std::filesystem::path host_offer;
    std::filesystem::path controller_answer;
    std::filesystem::path host_candidates;
    std::filesystem::path controller_candidates;
    std::filesystem::path host_offer_sealed;
    std::filesystem::path controller_answer_sealed;
};

FileSignalingPaths make_file_signaling_paths(const RuntimeOptions& options) {
    const std::filesystem::path directory = std::filesystem::path(options.signal_dir);
    return FileSignalingPaths{
        directory,
        directory / "host-offer.sdp",
        directory / "controller-answer.sdp",
        directory / "host-candidates.txt",
        directory / "controller-candidates.txt",
        directory / "host-offer.sealed.txt",
        directory / "controller-answer.sealed.txt",
    };
}

bool ensure_signaling_directory(const std::filesystem::path& directory, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        *error = "failed to create signaling directory: " + ec.message();
        return false;
    }
    return true;
}

bool write_text_file(const std::filesystem::path& file_path, std::string_view contents, std::string* error) {
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        *error = "failed to open signaling file for write: " + file_path.string();
        return false;
    }

    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.flush();
    if (!out.good()) {
        *error = "failed to write signaling file: " + file_path.string();
        return false;
    }

    return true;
}

bool read_text_file(const std::filesystem::path& file_path, std::string* contents, std::string* error) {
    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) {
        *error = "failed to open signaling file for read: " + file_path.string();
        return false;
    }

    contents->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        *error = "failed to read signaling file: " + file_path.string();
        return false;
    }

    return true;
}

bool append_candidate_line(
    const std::filesystem::path& file_path,
    const std::string& candidate,
    const std::string& mid,
    std::string* error) {
    std::ofstream out(file_path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        *error = "failed to open candidate signaling file for append: " + file_path.string();
        return false;
    }

    out << mid << '\t' << candidate << '\n';
    out.flush();
    if (!out.good()) {
        *error = "failed to append candidate signaling line: " + file_path.string();
        return false;
    }

    return true;
}

bool remove_file_if_exists(const std::filesystem::path& file_path, std::string* error) {
    std::error_code ec;
    const bool removed = std::filesystem::remove(file_path, ec);
    if (ec) {
        *error = "failed to clean signaling file: " + file_path.string() + " (" + ec.message() + ")";
        return false;
    }

    (void)removed;
    return true;
}

std::optional<std::pair<std::string, std::string>> parse_candidate_line(std::string_view line) {
    const std::size_t split = line.find('\t');
    if (split == std::string::npos || split == 0 || split + 1 >= line.size()) {
        return std::nullopt;
    }

    return std::make_pair(
        std::string(line.substr(0, split)),
        std::string(line.substr(split + 1)));
}

std::uint64_t now_unix_ms();

std::string to_lower_copy(std::string_view value) {
    std::string lowered(value);
    for (char& ch : lowered) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered;
}

std::string classify_sealed_blob_error(std::string_view error_text) {
    const std::string lowered = to_lower_copy(error_text);
    if (lowered.find("hmac") != std::string::npos
        || lowered.find("auth") != std::string::npos
        || lowered.find("integrity") != std::string::npos
        || lowered.find("passphrase") != std::string::npos) {
        return "wrong-passphrase-or-tampered-blob";
    }

    if (lowered.find("base64") != std::string::npos
        || lowered.find("format") != std::string::npos
        || lowered.find("parse") != std::string::npos) {
        return "malformed-sealed-blob";
    }

    return "sealed-blob-parse-failure";
}

std::optional<std::string> extract_sdp_attribute(std::string_view sdp, std::string_view prefix) {
    std::size_t line_begin = 0;
    while (line_begin < sdp.size()) {
        std::size_t line_end = sdp.find('\n', line_begin);
        if (line_end == std::string::npos) {
            line_end = sdp.size();
        }

        std::string_view line = sdp.substr(line_begin, line_end - line_begin);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (line.rfind(prefix, 0) == 0 && line.size() > prefix.size()) {
            return std::string(line.substr(prefix.size()));
        }

        if (line_end == sdp.size()) {
            break;
        }
        line_begin = line_end + 1;
    }

    return std::nullopt;
}

std::string make_runtime_nonce(std::string_view role_name) {
    return "runtime-" + std::string(role_name) + "-nonce-" + std::to_string(now_unix_ms()) + "-a1";
}

std::string make_runtime_session_id(std::string_view role_name) {
    return "runtime-" + std::string(role_name) + "-session-" + std::to_string(now_unix_ms()) + "-a1";
}

bool build_sealed_signal_blob(
    std::string_view role_name,
    const std::string& description_sdp,
    const std::vector<std::string>& candidate_lines,
    const RuntimeOptions& options,
    std::string* sealed_blob,
    std::string* error) {
    if (options.signal_passphrase.empty()) {
        *error = "sealed signaling passphrase is empty";
        return false;
    }

    const auto ice_ufrag = extract_sdp_attribute(description_sdp, "a=ice-ufrag:");
    const auto ice_pwd = extract_sdp_attribute(description_sdp, "a=ice-pwd:");
    const auto fingerprint = extract_sdp_attribute(description_sdp, "a=fingerprint:");

    if (!ice_ufrag.has_value() || !ice_pwd.has_value() || !fingerprint.has_value()) {
        *error = "failed to extract ICE credentials from local description for sealed signaling";
        return false;
    }

    redclaw::protocol::OfferBlobV1 offer_blob;
    offer_blob.session_id = "runtime-sealed-session";
    offer_blob.host_peer_id = "runtime-host";
    offer_blob.controller_peer_id = "runtime-controller";
    offer_blob.nonce = make_runtime_nonce(role_name);
    offer_blob.created_at_ms = now_unix_ms();
    offer_blob.description_sdp = description_sdp;
    offer_blob.ice_ufrag = ice_ufrag.value();
    offer_blob.ice_pwd = ice_pwd.value();
    offer_blob.host_fingerprint = fingerprint.value();
    offer_blob.candidates = candidate_lines;

    const auto encrypted = redclaw::protocol::encrypt_offer_blob_v1(offer_blob, options.signal_passphrase);
    if (!encrypted.ok) {
        *error = "failed to encrypt sealed signaling blob: " + encrypted.error;
        return false;
    }

    *sealed_blob = encrypted.value;
    return true;
}

bool parse_sealed_signal_blob(
    const std::string& sealed_blob,
    const RuntimeOptions& options,
    std::string* description_sdp,
    std::vector<std::pair<std::string, std::string>>* candidate_pairs,
    std::string* error) {
    const auto decrypted = redclaw::protocol::decrypt_offer_blob_v1(sealed_blob, options.signal_passphrase);
    if (!decrypted.ok) {
        *error = "failed to decrypt sealed signaling blob: " + decrypted.error;
        return false;
    }

    description_sdp->assign(decrypted.value.description_sdp);
    candidate_pairs->clear();

    for (const std::string& candidate_line : decrypted.value.candidates) {
        const auto parsed = parse_candidate_line(candidate_line);
        if (!parsed.has_value()) {
            *error = "invalid candidate entry in sealed signaling blob";
            return false;
        }
        candidate_pairs->push_back(parsed.value());
    }

    return true;
}

std::uint64_t now_unix_ms() {
    const auto now = std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

std::uint64_t now_steady_ms() {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

std::string escape_transport_field(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\\') {
            escaped += "\\\\";
        } else if (ch == '\t') {
            escaped += "\\t";
        } else if (ch == '\n') {
            escaped += "\\n";
        } else if (ch == '\r') {
            escaped += "\\r";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

bool unescape_transport_field(std::string_view value, std::string* output) {
    output->clear();
    output->reserve(value.size());

    bool escape_pending = false;
    for (const char ch : value) {
        if (!escape_pending) {
            if (ch == '\\') {
                escape_pending = true;
            } else {
                output->push_back(ch);
            }
            continue;
        }

        if (ch == 'n') {
            output->push_back('\n');
        } else if (ch == 'r') {
            output->push_back('\r');
        } else if (ch == 't') {
            output->push_back('\t');
        } else if (ch == '\\') {
            output->push_back('\\');
        } else {
            return false;
        }
        escape_pending = false;
    }

    return !escape_pending;
}

bool parse_event_log_line(std::string_view line, std::vector<std::string>* fields) {
    fields->clear();

    std::string current;
    bool escape_pending = false;
    for (const char ch : line) {
        if (!escape_pending) {
            if (ch == '\\') {
                escape_pending = true;
                continue;
            }
            if (ch == '\t') {
                fields->push_back(current);
                current.clear();
                continue;
            }
            current.push_back(ch);
            continue;
        }

        current.push_back('\\');
        current.push_back(ch);
        escape_pending = false;
    }

    if (escape_pending) {
        return false;
    }

    fields->push_back(current);

    for (std::string& field : *fields) {
        std::string unescaped;
        if (!unescape_transport_field(field, &unescaped)) {
            return false;
        }
        field = std::move(unescaped);
    }

    return true;
}

bool append_event_log_line(
    const std::filesystem::path& file_path,
    const std::vector<std::string>& fields,
    std::string* error) {
    std::ofstream out(file_path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        *error = "failed to open signaling event log for append: " + file_path.string();
        return false;
    }

    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            out << '\t';
        }
        out << escape_transport_field(fields[i]);
    }
    out << '\n';
    out.flush();
    if (!out.good()) {
        *error = "failed to append signaling event log: " + file_path.string();
        return false;
    }

    return true;
}

std::string format_signal_event_line(const std::vector<std::string>& fields) {
    std::ostringstream out;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            out << '\t';
        }
        out << escape_transport_field(fields[i]);
    }
    out << '\n';
    return out.str();
}

#ifdef _WIN32

constexpr std::uint16_t kDefaultTcpSignalingPort = 45909;

struct ParsedWinHttpBaseUrl {
    bool secure = false;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
    std::wstring host;
    std::wstring base_path;
};

struct WinHttpResponse {
    std::uint32_t status_code = 0;
    std::string body;
};

struct RendezvousRegisterHttpResult {
    bool accepted = false;
    std::string error = "invalid_request";
    std::uint64_t expires_at_unix = 0;
};

struct RendezvousLookupHttpResult {
    bool found = false;
    bool claimed = false;
    std::string error = "not_found";
    std::string session_id;
    std::string host_display_name;
    std::string host_fingerprint_summary;
    std::uint64_t expires_at_unix = 0;
};

struct RendezvousClaimHttpResult {
    bool claimed = false;
    std::string error = "not_found";
    std::string session_id;
};

struct RendezvousPublishHttpResult {
    bool accepted = false;
    std::string error = "invalid_request";
    std::uint64_t revision = 0;
    std::uint64_t updated_at_unix = 0;
};

struct RendezvousSignalSnapshot {
    bool found = false;
    std::string error = "not_found";
    std::string description_type;
    std::string description_sdp;
    std::vector<std::string> candidate_lines;
    std::uint64_t revision = 0;
    std::uint64_t updated_at_unix = 0;
};

struct RendezvousTransportDiagnostics {
    std::uint32_t register_attempts = 0;
    std::uint32_t register_success = 0;
    std::uint32_t lookup_attempts = 0;
    std::uint32_t lookup_hits = 0;
    std::uint32_t claim_attempts = 0;
    std::uint32_t claim_success = 0;
    std::uint32_t publish_attempts = 0;
    std::uint32_t publish_success = 0;
    std::uint32_t fetch_attempts = 0;
    std::uint32_t fetch_hits = 0;
    std::uint64_t local_revision = 0;
    std::uint64_t remote_revision = 0;
    std::string last_error;
};

class ScopedWinHttpHandle {
public:
    ScopedWinHttpHandle() = default;

    explicit ScopedWinHttpHandle(HINTERNET handle)
        : handle_(handle) {}

    ~ScopedWinHttpHandle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    ScopedWinHttpHandle(const ScopedWinHttpHandle&) = delete;
    ScopedWinHttpHandle& operator=(const ScopedWinHttpHandle&) = delete;

    ScopedWinHttpHandle(ScopedWinHttpHandle&& other) noexcept
        : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    ScopedWinHttpHandle& operator=(ScopedWinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) {
                WinHttpCloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const {
        return handle_ != nullptr;
    }

    [[nodiscard]] HINTERNET get() const {
        return handle_;
    }

private:
    HINTERNET handle_ = nullptr;
};

bool utf8_to_wstring(std::string_view value, std::wstring* output, std::string* error) {
    output->clear();
    if (value.empty()) {
        return true;
    }

    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) {
        *error = "failed to convert UTF-8 string to wide-char, Win32 error=" + std::to_string(GetLastError());
        return false;
    }

    output->assign(static_cast<std::size_t>(required), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        output->data(),
        required);
    if (converted != required) {
        *error = "failed to complete UTF-8 string conversion, Win32 error=" + std::to_string(GetLastError());
        output->clear();
        return false;
    }

    return true;
}

std::wstring join_url_path(const std::wstring& base_path, std::wstring_view endpoint_path) {
    if (base_path.empty() || base_path == L"/") {
        if (!endpoint_path.empty() && endpoint_path.front() == L'/') {
            return std::wstring(endpoint_path);
        }
        return L"/" + std::wstring(endpoint_path);
    }

    std::wstring combined = base_path;
    if (!combined.empty() && combined.back() == L'/') {
        combined.pop_back();
    }
    if (!endpoint_path.empty() && endpoint_path.front() != L'/') {
        combined.push_back(L'/');
    }
    combined.append(endpoint_path);
    return combined;
}

bool parse_winhttp_base_url(std::string_view url, ParsedWinHttpBaseUrl* parsed, std::string* error) {
    std::wstring wide_url;
    if (!utf8_to_wstring(url, &wide_url, error)) {
        return false;
    }

    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
        *error = "failed to parse rendezvous URL, Win32 error=" + std::to_string(GetLastError());
        return false;
    }

    if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) {
        *error = "rendezvous URL must use http or https";
        return false;
    }
    if (components.dwHostNameLength == 0 || components.lpszHostName == nullptr) {
        *error = "rendezvous URL must include a host name";
        return false;
    }
    if (components.dwExtraInfoLength > 0) {
        *error = "rendezvous URL must not include query or fragment";
        return false;
    }

    parsed->secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    parsed->port = components.nPort;
    parsed->host.assign(components.lpszHostName, components.dwHostNameLength);
    parsed->base_path.clear();
    if (components.dwUrlPathLength > 0 && components.lpszUrlPath != nullptr) {
        parsed->base_path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }

    return true;
}

bool send_winhttp_request(
    const ParsedWinHttpBaseUrl& base_url,
    const wchar_t* method,
    std::string_view endpoint_path,
    std::string_view request_body,
    WinHttpResponse* response,
    std::string* error) {
    std::wstring endpoint_path_w;
    if (!utf8_to_wstring(endpoint_path, &endpoint_path_w, error)) {
        return false;
    }

    const std::wstring request_path = join_url_path(base_url.base_path, endpoint_path_w);
    ScopedWinHttpHandle session(WinHttpOpen(
        L"RedClawDesktop/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session.valid()) {
        *error = "failed to open WinHTTP session, Win32 error=" + std::to_string(GetLastError());
        return false;
    }

    constexpr int kWinHttpTimeoutMs = 5000;
    if (!WinHttpSetTimeouts(session.get(), kWinHttpTimeoutMs, kWinHttpTimeoutMs, kWinHttpTimeoutMs, kWinHttpTimeoutMs)) {
        *error = "failed to configure WinHTTP timeouts, Win32 error=" + std::to_string(GetLastError());
        return false;
    }

    ScopedWinHttpHandle connect(WinHttpConnect(session.get(), base_url.host.c_str(), base_url.port, 0));
    if (!connect.valid()) {
        *error = "failed to connect WinHTTP session, Win32 error=" + std::to_string(GetLastError());
        return false;
    }

    const DWORD request_flags = base_url.secure ? WINHTTP_FLAG_SECURE : 0;
    ScopedWinHttpHandle request(WinHttpOpenRequest(
        connect.get(),
        method,
        request_path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        request_flags));
    if (!request.valid()) {
        *error = "failed to open WinHTTP request, Win32 error=" + std::to_string(GetLastError());
        return false;
    }

    const wchar_t* headers = L"Content-Type: text/plain; charset=utf-8\r\n";
    if (!WinHttpSendRequest(
            request.get(),
            headers,
            static_cast<DWORD>(-1),
            request_body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request_body.data()),
            static_cast<DWORD>(request_body.size()),
            static_cast<DWORD>(request_body.size()),
            0)) {
        *error = "failed to send WinHTTP request, Win32 error=" + std::to_string(GetLastError());
        return false;
    }

    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        *error = "failed to receive WinHTTP response, Win32 error=" + std::to_string(GetLastError());
        return false;
    }

    DWORD status_code = 0;
    DWORD status_code_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_code_size,
            WINHTTP_NO_HEADER_INDEX)) {
        *error = "failed to query WinHTTP status code, Win32 error=" + std::to_string(GetLastError());
        return false;
    }

    response->status_code = status_code;
    response->body.clear();

    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            *error = "failed to query WinHTTP response body size, Win32 error=" + std::to_string(GetLastError());
            return false;
        }
        if (available == 0) {
            break;
        }

        std::string chunk(static_cast<std::size_t>(available), '\0');
        DWORD bytes_read = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), available, &bytes_read)) {
            *error = "failed to read WinHTTP response body, Win32 error=" + std::to_string(GetLastError());
            return false;
        }

        response->body.append(chunk.data(), chunk.data() + bytes_read);
    }

    return true;
}

bool parse_transport_document(
    std::string_view document,
    std::vector<std::vector<std::string>>* rows,
    std::string* error) {
    rows->clear();

    std::istringstream stream{std::string(document)};
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        std::vector<std::string> fields;
        if (!parse_event_log_line(line, &fields)) {
            *error = "failed to parse rendezvous transport document line";
            rows->clear();
            return false;
        }

        rows->push_back(std::move(fields));
    }

    return true;
}

std::string build_rendezvous_register_body(
    std::string_view session_id,
    std::string_view host_display_name,
    std::string_view host_fingerprint_summary,
    std::uint32_t ttl_seconds) {
    return format_signal_event_line({
        "register",
        std::string(session_id),
        std::string(host_display_name),
        std::string(host_fingerprint_summary),
        std::to_string(ttl_seconds),
    });
}

bool parse_rendezvous_register_response(
    std::string_view body,
    RendezvousRegisterHttpResult* result,
    std::string* error) {
    std::vector<std::vector<std::string>> rows;
    if (!parse_transport_document(body, &rows, error)) {
        return false;
    }
    if (rows.empty() || rows[0].size() < 4 || rows[0][0] != "result") {
        *error = "unexpected rendezvous register response body";
        return false;
    }

    result->accepted = rows[0][1] == "1";
    result->error = rows[0][2];
    if (!parse_uint64(rows[0][3], &result->expires_at_unix)) {
        *error = "invalid rendezvous register expiry";
        return false;
    }

    return true;
}

bool rendezvous_register_session_http(
    const ParsedWinHttpBaseUrl& base_url,
    std::string_view session_code,
    std::string_view session_id,
    std::string_view host_display_name,
    std::string_view host_fingerprint_summary,
    std::uint32_t ttl_seconds,
    RendezvousRegisterHttpResult* result,
    std::string* error) {
    WinHttpResponse response;
    const std::string endpoint = "/v1/sessions/" + std::string(session_code);
    const std::string body = build_rendezvous_register_body(
        session_id,
        host_display_name,
        host_fingerprint_summary,
        ttl_seconds);
    if (!send_winhttp_request(base_url, L"PUT", endpoint, body, &response, error)) {
        return false;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        *error = "rendezvous register failed with HTTP status=" + std::to_string(response.status_code);
        return false;
    }
    return parse_rendezvous_register_response(response.body, result, error);
}

bool parse_rendezvous_lookup_response(
    std::string_view body,
    RendezvousLookupHttpResult* result,
    std::string* error) {
    std::vector<std::vector<std::string>> rows;
    if (!parse_transport_document(body, &rows, error)) {
        return false;
    }
    if (rows.empty() || rows[0].size() < 8 || rows[0][0] != "lookup") {
        *error = "unexpected rendezvous lookup response body";
        return false;
    }

    result->found = rows[0][1] == "1";
    result->claimed = rows[0][2] == "1";
    result->error = rows[0][3];
    result->session_id = rows[0][4];
    result->host_display_name = rows[0][5];
    result->host_fingerprint_summary = rows[0][6];
    if (!parse_uint64(rows[0][7], &result->expires_at_unix)) {
        *error = "invalid rendezvous lookup expiry";
        return false;
    }

    return true;
}

bool rendezvous_lookup_session_http(
    const ParsedWinHttpBaseUrl& base_url,
    std::string_view session_code,
    RendezvousLookupHttpResult* result,
    std::string* error) {
    WinHttpResponse response;
    const std::string endpoint = "/v1/sessions/" + std::string(session_code);
    if (!send_winhttp_request(base_url, L"GET", endpoint, "", &response, error)) {
        return false;
    }
    if (response.status_code == 404) {
        *result = RendezvousLookupHttpResult{};
        return true;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        *error = "rendezvous lookup failed with HTTP status=" + std::to_string(response.status_code);
        return false;
    }
    return parse_rendezvous_lookup_response(response.body, result, error);
}

bool parse_rendezvous_claim_response(
    std::string_view body,
    RendezvousClaimHttpResult* result,
    std::string* error) {
    std::vector<std::vector<std::string>> rows;
    if (!parse_transport_document(body, &rows, error)) {
        return false;
    }
    if (rows.empty() || rows[0].size() < 4 || rows[0][0] != "claim") {
        *error = "unexpected rendezvous claim response body";
        return false;
    }

    result->claimed = rows[0][1] == "1";
    result->error = rows[0][2];
    result->session_id = rows[0][3];
    return true;
}

bool rendezvous_claim_session_http(
    const ParsedWinHttpBaseUrl& base_url,
    std::string_view session_code,
    RendezvousClaimHttpResult* result,
    std::string* error) {
    WinHttpResponse response;
    const std::string endpoint = "/v1/sessions/" + std::string(session_code) + "/claim";
    if (!send_winhttp_request(base_url, L"POST", endpoint, "", &response, error)) {
        return false;
    }
    if (response.status_code == 404) {
        *result = RendezvousClaimHttpResult{};
        return true;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        *error = "rendezvous claim failed with HTTP status=" + std::to_string(response.status_code);
        return false;
    }
    return parse_rendezvous_claim_response(response.body, result, error);
}

bool build_rendezvous_signal_body(
    std::string_view description_type,
    std::string_view description_sdp,
    const std::vector<std::string>& candidate_lines,
    std::string* body,
    std::string* error) {
    *body = format_signal_event_line({
        "signal",
        std::string(description_type),
        std::string(description_sdp),
    });

    for (const std::string& candidate_line : candidate_lines) {
        const auto parsed = parse_candidate_line(candidate_line);
        if (!parsed.has_value()) {
            *error = "invalid local candidate line while building rendezvous signal body";
            return false;
        }
        body->append(format_signal_event_line({"candidate", parsed->first, parsed->second}));
    }

    return true;
}

bool parse_rendezvous_publish_response(
    std::string_view body,
    RendezvousPublishHttpResult* result,
    std::string* error) {
    std::vector<std::vector<std::string>> rows;
    if (!parse_transport_document(body, &rows, error)) {
        return false;
    }
    if (rows.empty() || rows[0].size() < 5 || rows[0][0] != "publish") {
        *error = "unexpected rendezvous publish response body";
        return false;
    }

    result->accepted = rows[0][1] == "1";
    result->error = rows[0][2];
    if (!parse_uint64(rows[0][3], &result->revision)) {
        *error = "invalid rendezvous publish revision";
        return false;
    }
    if (!parse_uint64(rows[0][4], &result->updated_at_unix)) {
        *error = "invalid rendezvous publish timestamp";
        return false;
    }

    return true;
}

bool rendezvous_publish_signal_http(
    const ParsedWinHttpBaseUrl& base_url,
    std::string_view session_id,
    std::string_view lane,
    std::string_view description_type,
    std::string_view description_sdp,
    const std::vector<std::string>& candidate_lines,
    RendezvousPublishHttpResult* result,
    std::string* error) {
    std::string body;
    if (!build_rendezvous_signal_body(description_type, description_sdp, candidate_lines, &body, error)) {
        return false;
    }

    WinHttpResponse response;
    const std::string endpoint = "/v1/runtime-sessions/" + std::string(session_id) + "/" + std::string(lane) + "-signal";
    if (!send_winhttp_request(base_url, L"PUT", endpoint, body, &response, error)) {
        return false;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        *error = "rendezvous signal publish failed with HTTP status=" + std::to_string(response.status_code);
        return false;
    }
    return parse_rendezvous_publish_response(response.body, result, error);
}

bool parse_rendezvous_signal_snapshot(
    std::string_view body,
    RendezvousSignalSnapshot* snapshot,
    std::string* error) {
    std::vector<std::vector<std::string>> rows;
    if (!parse_transport_document(body, &rows, error)) {
        return false;
    }
    if (rows.empty() || rows[0].size() < 7 || rows[0][0] != "fetch") {
        *error = "unexpected rendezvous signal fetch response body";
        return false;
    }

    snapshot->found = rows[0][1] == "1";
    snapshot->error = rows[0][2];
    snapshot->description_type = rows[0][3];
    snapshot->description_sdp = rows[0][4];
    if (!parse_uint64(rows[0][5], &snapshot->revision)) {
        *error = "invalid rendezvous fetch revision";
        return false;
    }
    if (!parse_uint64(rows[0][6], &snapshot->updated_at_unix)) {
        *error = "invalid rendezvous fetch timestamp";
        return false;
    }

    snapshot->candidate_lines.clear();
    for (std::size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].size() < 3 || rows[i][0] != "candidate") {
            continue;
        }
        snapshot->candidate_lines.push_back(rows[i][1] + "\t" + rows[i][2]);
    }

    return true;
}

bool rendezvous_fetch_signal_http(
    const ParsedWinHttpBaseUrl& base_url,
    std::string_view session_id,
    std::string_view lane,
    RendezvousSignalSnapshot* snapshot,
    std::string* error) {
    WinHttpResponse response;
    const std::string endpoint = "/v1/runtime-sessions/" + std::string(session_id) + "/" + std::string(lane) + "-signal";
    if (!send_winhttp_request(base_url, L"GET", endpoint, "", &response, error)) {
        return false;
    }
    if (response.status_code == 404) {
        *snapshot = RendezvousSignalSnapshot{};
        return true;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        *error = "rendezvous signal fetch failed with HTTP status=" + std::to_string(response.status_code);
        return false;
    }
    return parse_rendezvous_signal_snapshot(response.body, snapshot, error);
}

enum class TcpSignalFailureCategory {
    kNone,
    kResolveFailed,
    kConnectFailed,
    kAcceptFailed,
    kSendFailed,
    kReceiveFailed,
    kPeerDisconnected,
    kMalformedMessage,
};

struct TcpSignalDiagnostics {
    std::uint32_t connect_attempt_count = 0;
    std::uint32_t connect_success_count = 0;
    std::uint32_t reconnect_count = 0;
    std::uint32_t disconnect_count = 0;
    std::uint32_t send_failure_count = 0;
    std::uint32_t receive_failure_count = 0;
    std::uint32_t malformed_message_count = 0;
    std::uint32_t current_backoff_ms = 0;
    TcpSignalFailureCategory last_failure = TcpSignalFailureCategory::kNone;
    int last_wsa_error = 0;
    std::string last_failure_detail;
};

std::string_view to_string(TcpSignalFailureCategory category) {
    switch (category) {
    case TcpSignalFailureCategory::kNone:
        return "none";
    case TcpSignalFailureCategory::kResolveFailed:
        return "resolve_failed";
    case TcpSignalFailureCategory::kConnectFailed:
        return "connect_failed";
    case TcpSignalFailureCategory::kAcceptFailed:
        return "accept_failed";
    case TcpSignalFailureCategory::kSendFailed:
        return "send_failed";
    case TcpSignalFailureCategory::kReceiveFailed:
        return "receive_failed";
    case TcpSignalFailureCategory::kPeerDisconnected:
        return "peer_disconnected";
    case TcpSignalFailureCategory::kMalformedMessage:
        return "malformed_message";
    }

    return "unknown";
}

struct ScopedWinsock {
    bool initialized = false;

    ~ScopedWinsock() {
        if (initialized) {
            WSACleanup();
        }
    }
};

struct ScopedSocket {
    SOCKET value = INVALID_SOCKET;

    ~ScopedSocket() {
        reset();
    }

    void reset(SOCKET next = INVALID_SOCKET) {
        if (value != INVALID_SOCKET) {
            closesocket(value);
        }
        value = next;
    }

    [[nodiscard]] bool valid() const {
        return value != INVALID_SOCKET;
    }
};

bool initialize_winsock(ScopedWinsock* winsock, std::string* error) {
    if (winsock->initialized) {
        return true;
    }

    WSADATA wsadata{};
    const int startup_result = WSAStartup(MAKEWORD(2, 2), &wsadata);
    if (startup_result != 0) {
        *error = "WSAStartup failed: " + std::to_string(startup_result);
        return false;
    }

    winsock->initialized = true;
    return true;
}

bool set_socket_non_blocking(SOCKET socket, std::string* error) {
    u_long non_blocking = 1;
    if (ioctlsocket(socket, FIONBIO, &non_blocking) != 0) {
        *error = "failed to set non-blocking mode, WSA error=" + std::to_string(WSAGetLastError());
        return false;
    }
    return true;
}

bool create_tcp_listener(std::uint16_t port, ScopedSocket* listener, std::string* error) {
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        *error = "failed to create TCP signaling listener socket, WSA error=" + std::to_string(WSAGetLastError());
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        *error = "failed to bind TCP signaling listener, WSA error=" + std::to_string(WSAGetLastError());
        closesocket(socket);
        return false;
    }

    if (::listen(socket, 1) == SOCKET_ERROR) {
        *error = "failed to listen on TCP signaling listener, WSA error=" + std::to_string(WSAGetLastError());
        closesocket(socket);
        return false;
    }

    if (!set_socket_non_blocking(socket, error)) {
        closesocket(socket);
        return false;
    }

    listener->reset(socket);
    return true;
}

bool try_accept_tcp_peer(SOCKET listener, ScopedSocket* peer, bool* accepted, std::string* error) {
    *accepted = false;

    sockaddr_in remote_address{};
    int remote_length = sizeof(remote_address);
    SOCKET accepted_socket = ::accept(
        listener,
        reinterpret_cast<sockaddr*>(&remote_address),
        &remote_length);

    if (accepted_socket == INVALID_SOCKET) {
        const int wsa_error = WSAGetLastError();
        if (wsa_error == WSAEWOULDBLOCK) {
            return true;
        }
        *error = "failed to accept TCP signaling peer, WSA error=" + std::to_string(wsa_error);
        return false;
    }

    peer->reset(accepted_socket);
    *accepted = true;
    return true;
}

bool connect_tcp_peer(
    const std::string& host,
    std::uint16_t port,
    ScopedSocket* peer,
    bool* resolve_failed,
    int* last_wsa_error,
    std::string* error) {
    *resolve_failed = false;
    *last_wsa_error = 0;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* resolved = nullptr;
    const std::string service = std::to_string(port);
    const int gai_result = getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved);
    if (gai_result != 0) {
        *resolve_failed = true;
        *error = "failed to resolve TCP signaling host: " + host + " error=" + std::to_string(gai_result);
        return false;
    }

    bool connected = false;
    for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
        SOCKET socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (socket == INVALID_SOCKET) {
            continue;
        }

        if (::connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
            peer->reset(socket);
            connected = true;
            break;
        }

        *last_wsa_error = WSAGetLastError();

        closesocket(socket);
    }

    freeaddrinfo(resolved);

    if (!connected) {
        *error = "failed to connect TCP signaling peer host=" + host + " port=" + std::to_string(port);
        return false;
    }

    return true;
}

bool send_socket_all(SOCKET socket, const std::string& payload, std::string* error) {
    int wsa_error = 0;
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const int chunk = ::send(
            socket,
            payload.data() + sent,
            static_cast<int>(payload.size() - sent),
            0);
        if (chunk == SOCKET_ERROR) {
            wsa_error = WSAGetLastError();
            *error = "failed to send TCP signaling payload, WSA error=" + std::to_string(wsa_error);
            return false;
        }

        if (chunk == 0) {
            *error = "TCP signaling socket closed while sending payload";
            return false;
        }

        sent += static_cast<std::size_t>(chunk);
    }

    return true;
}

bool read_socket_lines(
    SOCKET socket,
    std::string* receive_buffer,
    std::vector<std::string>* lines,
    bool* peer_closed,
    int* last_wsa_error,
    std::string* error) {
    *peer_closed = false;
    *last_wsa_error = 0;
    lines->clear();

    while (true) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket, &read_set);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        const int select_result = ::select(0, &read_set, nullptr, nullptr, &timeout);
        if (select_result == SOCKET_ERROR) {
            *last_wsa_error = WSAGetLastError();
            *error = "failed to poll TCP signaling socket, WSA error=" + std::to_string(*last_wsa_error);
            return false;
        }

        if (select_result == 0) {
            break;
        }

        char buffer[4096];
        const int received = ::recv(socket, buffer, sizeof(buffer), 0);
        if (received == SOCKET_ERROR) {
            *last_wsa_error = WSAGetLastError();
            if (*last_wsa_error == WSAECONNRESET
                || *last_wsa_error == WSAECONNABORTED
                || *last_wsa_error == WSAENETRESET
                || *last_wsa_error == WSAETIMEDOUT) {
                *peer_closed = true;
                return true;
            }
            *error = "failed to receive TCP signaling payload, WSA error=" + std::to_string(*last_wsa_error);
            return false;
        }

        if (received == 0) {
            *peer_closed = true;
            break;
        }

        receive_buffer->append(buffer, buffer + received);
    }

    std::size_t cursor = 0;
    while (true) {
        const std::size_t newline = receive_buffer->find('\n', cursor);
        if (newline == std::string::npos) {
            break;
        }

        std::string line = receive_buffer->substr(cursor, newline - cursor);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines->push_back(std::move(line));
        cursor = newline + 1;
    }

    receive_buffer->erase(0, cursor);
    return true;
}

#endif

enum class CandidateType {
    kHost,
    kSrflx,
    kRelay,
    kPrflx,
    kUnknown,
};

struct CandidateDiagnostics {
    std::uint32_t local_total = 0;
    std::uint32_t local_host = 0;
    std::uint32_t local_srflx = 0;
    std::uint32_t local_relay = 0;
    std::uint32_t local_prflx = 0;
    std::uint32_t local_unknown = 0;
    std::uint32_t local_ipv6 = 0;
    std::uint32_t remote_total = 0;
    std::uint32_t remote_host = 0;
    std::uint32_t remote_srflx = 0;
    std::uint32_t remote_relay = 0;
    std::uint32_t remote_prflx = 0;
    std::uint32_t remote_unknown = 0;
    std::uint32_t remote_ipv6 = 0;
    std::uint32_t remote_dht_exchanged = 0;
};

CandidateType classify_candidate_type(const std::string& candidate_sdp) {
    if (candidate_sdp.find(" typ host") != std::string::npos) {
        return CandidateType::kHost;
    }
    if (candidate_sdp.find(" typ srflx") != std::string::npos) {
        return CandidateType::kSrflx;
    }
    if (candidate_sdp.find(" typ relay") != std::string::npos) {
        return CandidateType::kRelay;
    }
    if (candidate_sdp.find(" typ prflx") != std::string::npos) {
        return CandidateType::kPrflx;
    }
    return CandidateType::kUnknown;
}

void record_candidate(CandidateDiagnostics* diagnostics, bool is_local, CandidateType type, const std::string& candidate_sdp = {}) {
    if (is_local) {
        ++diagnostics->local_total;
        if (!candidate_sdp.empty() && redclaw::service::candidate_looks_ipv6(candidate_sdp)) {
            ++diagnostics->local_ipv6;
        }
        if (type == CandidateType::kHost) {
            ++diagnostics->local_host;
        } else if (type == CandidateType::kSrflx) {
            ++diagnostics->local_srflx;
        } else if (type == CandidateType::kRelay) {
            ++diagnostics->local_relay;
        } else if (type == CandidateType::kPrflx) {
            ++diagnostics->local_prflx;
        } else {
            ++diagnostics->local_unknown;
        }
        return;
    }

    ++diagnostics->remote_total;
    if (!candidate_sdp.empty() && redclaw::service::candidate_looks_ipv6(candidate_sdp)) {
        ++diagnostics->remote_ipv6;
    }
    if (type == CandidateType::kHost) {
        ++diagnostics->remote_host;
    } else if (type == CandidateType::kSrflx) {
        ++diagnostics->remote_srflx;
    } else if (type == CandidateType::kRelay) {
        ++diagnostics->remote_relay;
    } else if (type == CandidateType::kPrflx) {
        ++diagnostics->remote_prflx;
    } else {
        ++diagnostics->remote_unknown;
    }
}

enum class RemoteDhtCandidateApplyOutcome {
    kApplied,
    kDeferred,
    kInvalid,
    kFailed,
};

RemoteDhtCandidateApplyOutcome apply_remote_dht_candidate_line(
    redclaw::net::IceConnectivityWrapper& ice_wrapper,
    std::string_view candidate_line,
    CandidateDiagnostics* diagnostics,
    std::mutex* diagnostics_mutex,
    std::string* error_detail) {
    const auto parsed = parse_candidate_line(std::string(candidate_line));
    if (!parsed.has_value()) {
        return RemoteDhtCandidateApplyOutcome::kInvalid;
    }

    using Outcome = redclaw::net::RemoteCandidateApplyOutcome;
    switch (ice_wrapper.tryApplyRemoteCandidate(parsed->second, parsed->first, error_detail)) {
    case Outcome::kApplied: break;
    case Outcome::kNotReady: return RemoteDhtCandidateApplyOutcome::kDeferred;
    case Outcome::kInvalid: return RemoteDhtCandidateApplyOutcome::kInvalid;
    case Outcome::kClosed:
    case Outcome::kSuperseded:
    case Outcome::kNativeFailure: return RemoteDhtCandidateApplyOutcome::kFailed;
    }

    if (diagnostics != nullptr && diagnostics_mutex != nullptr) {
        std::lock_guard<std::mutex> lock(*diagnostics_mutex);
        record_candidate(
            diagnostics,
            false,
            classify_candidate_type(parsed->second),
            parsed->second);
        ++diagnostics->remote_dht_exchanged;
    }
    return RemoteDhtCandidateApplyOutcome::kApplied;
}

std::string classify_nat_failure(
    const CandidateDiagnostics& diagnostics,
    bool dht_enabled,
    bool dht_reachable,
    bool port_mapping_enabled,
    const std::string& port_mapping_status) {
    if (dht_enabled && !dht_reachable) {
        return "dht_unreachable";
    }
    if (port_mapping_enabled && port_mapping_status != "mapped" && port_mapping_status != "disabled") {
        return "port_mapping_failed";
    }
    if (diagnostics.local_total == 0 || diagnostics.remote_total == 0) {
        return "candidate_exchange_incomplete";
    }
    return "ice_failure_unknown";
}

std::string format_ice_server_list(const std::vector<std::string>& ice_servers) {
    if (ice_servers.empty()) {
        return "[]";
    }

    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < ice_servers.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << redclaw::diag::redact_log_text(ice_servers[i]);
    }
    out << "]";
    return out.str();
}

std::string format_ice_server_type_counts(const std::vector<std::string>& ice_servers) {
    std::array<std::size_t, 4> counts{};
    for (const std::string& server : ice_servers) {
        std::string scheme;
        const auto separator = server.find(':');
        if (separator != std::string::npos) {
            scheme = server.substr(0, separator);
            std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
        }
        if (scheme == "stun" || scheme == "stuns") {
            ++counts[0];
        } else if (scheme == "turn") {
            ++counts[1];
        } else if (scheme == "turns") {
            ++counts[2];
        } else {
            ++counts[3];
        }
    }

    const std::array<std::string_view, 4> names = {"stun", "turn", "turns", "unknown"};
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (std::size_t index = 0; index < counts.size(); ++index) {
        if (counts[index] == 0) {
            continue;
        }
        if (!first) {
            out << ',';
        }
        first = false;
        out << names[index] << '=' << counts[index];
    }
    out << ']';
    return out.str();
}

struct DesktopStreamSmokeCounters {
    std::uint32_t captured_frames = 0;
    std::uint32_t synthetic_frames = 0;
    std::uint32_t capture_failures = 0;
    std::uint32_t encoded_frames = 0;
    std::uint32_t encode_failures = 0;
    std::uint32_t transmitted_frames = 0;
    std::uint32_t transmit_failures = 0;
    std::uint32_t transmit_backpressure_drops = 0;
    std::uint32_t transmit_local_pacing_drops = 0;
    std::uint32_t received_frames = 0;
    std::uint32_t decode_failures = 0;
    std::uint32_t decoded_frames = 0;
    std::uint32_t render_failures = 0;
    std::uint32_t rendered_frames = 0;
    std::string last_error;
    std::string last_preview_path;
};

struct HostStreamStageTelemetry {
    std::uint64_t total_capture_us = 0;
    std::uint64_t total_packetize_us = 0;
    std::uint64_t total_send_us = 0;
    std::uint32_t encode_submit_attempt_count = 0;
    std::uint32_t encode_output_not_ready_count = 0;
    std::uint32_t send_blocked_event_count = 0;
    std::uint32_t missed_deadline_count = 0;
    std::uint32_t skipped_interval_count = 0;
};

struct DirectFramePipeTransportStats {
    bool connected = false;
    std::uint64_t write_sequence = 0;
    std::uint64_t latest_sequence = 0;
    std::uint64_t reader_sequence = 0;
    std::uint64_t reader_active_sequence = 0;
    std::uint64_t writer_frame_count = 0;
    std::uint64_t writer_oversize_drop_count = 0;
    std::uint64_t writer_busy_drop_count = 0;
};

struct ControllerStreamStageTelemetry {
    std::uint64_t total_decode_us = 0;
    std::uint64_t total_direct_pipe_write_us = 0;
    std::uint32_t decode_attempt_count = 0;
    std::uint32_t direct_pipe_write_attempt_count = 0;
    std::uint32_t direct_pipe_write_success_count = 0;
    std::uint32_t direct_pipe_write_failure_count = 0;
    std::uint64_t current_direct_pipe_backlog = 0;
    std::uint64_t max_direct_pipe_backlog = 0;
    DirectFramePipeTransportStats direct_pipe_transport_stats;
};

struct StreamRttTelemetry {
    std::uint64_t last_sent_ping_sequence = 0;
    std::uint64_t in_flight_ping_sequence = 0;
    std::uint64_t in_flight_ping_sent_steady_ms = 0;
    std::uint32_t ping_sent_count = 0;
    std::uint32_t ping_ack_count = 0;
    std::uint64_t last_ack_steady_ms = 0;
    std::uint32_t ping_timeout_count = 0;
    std::uint64_t total_rtt_ms = 0;
    std::uint32_t last_rtt_ms = 0;
    std::uint32_t min_rtt_ms = 0;
    std::uint32_t max_rtt_ms = 0;
    std::uint32_t smoothed_rtt_ms = 0;
};

struct StreamAdaptiveControlState {
    std::uint64_t rate_revision = 1;
    std::uint32_t target_fps = kDesktopStreamTargetFps;
    std::uint32_t target_bitrate_kbps = 0;
    std::uint32_t target_max_bitrate_kbps = 0;
    std::uint32_t applied_fps = 0;
    std::uint32_t applied_bitrate_kbps = 0;
    std::uint32_t applied_max_bitrate_kbps = 0;
    std::uint32_t last_capture_fps_hint = 0;
    std::uint32_t last_frame_present_fps_hint = 0;
    std::uint32_t last_source_gap = 0;
    std::uint32_t last_encode_budget_gap = 0;
    std::uint32_t last_encoder_backpressure_delta = 0;
    std::uint32_t last_transmit_failures_delta = 0;
    std::uint32_t last_transmit_backpressure_delta = 0;
    std::uint32_t last_receiver_assembly_loss_per_mille = 0;
    redclaw::net::ReceiverAssemblyPressure last_receiver_assembly_pressure =
        redclaw::net::ReceiverAssemblyPressure::kStable;
    std::uint32_t last_receiver_decode_fps = 0;
    std::uint32_t receiver_decode_pressure_windows = 0;
    std::uint32_t receiver_decode_stable_windows = 0;
    std::uint64_t receiver_decode_fps_decrease_total = 0;
    std::uint64_t receiver_decode_fps_increase_total = 0;
    std::uint32_t last_rtt_ms = 0;
    std::uint32_t last_rtt_baseline_ms = 0;
    std::uint32_t last_rtt_queue_delay_ms = 0;
    std::uint32_t pressure_window_count = 0;
    std::uint32_t relief_window_count = 0;
    std::uint32_t encoder_restart_count = 0;
    std::uint32_t fresh_feedback_count = 0;
    std::uint32_t delayed_feedback_count = 0;
    std::uint32_t expired_feedback_count = 0;
    std::uint32_t stale_revision_feedback_count = 0;
    std::uint32_t unknown_frame_feedback_count = 0;
    std::uint32_t immediate_feedback_rate_change_count = 0;
    std::uint32_t delayed_feedback_queued_count = 0;
    std::uint32_t ignored_feedback_count = 0;
    std::uint64_t last_feedback_age_ms = 0;
    std::uint64_t last_rate_control_attempt_revision = 0;
    std::uint64_t rate_control_restart_fallback_revision = 0;
    std::uint64_t last_playback_starvation_ms = 0;
    std::uint64_t last_reconfigure_ms = 0;
    bool encoder_reconfigure_pending = false;
    bool playback_starvation_pacing_active = false;
};

struct PendingIncompleteMediaFeedback {
    std::uint64_t due_steady_ms = 0;
    std::uint64_t report_count = 0;
    std::uint64_t incomplete_frame_id = 0;
    std::uint64_t latest_received_frame_id = 0;
    std::uint64_t latest_complete_frame_id = 0;
    std::uint64_t latest_complete_keyframe_id = 0;
    std::uint64_t observed_rate_revision = 0;
    std::string reason;
};

std::uint32_t stream_counter_delta(std::uint32_t current, std::uint32_t previous) {
    return current >= previous ? (current - previous) : current;
}

std::uint64_t stream_total_delta(std::uint64_t current, std::uint64_t previous) {
    return current >= previous ? (current - previous) : current;
}

double stream_rate_per_second(std::uint32_t delta_count, std::uint64_t window_ms) {
    if (window_ms == 0) {
        return 0.0;
    }

    return static_cast<double>(delta_count) * 1000.0 / static_cast<double>(window_ms);
}

double stream_rate_per_second(std::uint64_t delta_count, std::uint64_t window_ms) {
    if (window_ms == 0) {
        return 0.0;
    }

    return static_cast<double>(delta_count) * 1000.0 / static_cast<double>(window_ms);
}

std::string format_stream_rate(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}

std::string format_stream_stage_ms(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

double average_stage_ms(std::uint64_t total_us, std::uint32_t count) {
    if (count == 0) {
        return 0.0;
    }

    return static_cast<double>(total_us) / 1000.0 / static_cast<double>(count);
}

double average_rtt_ms(const StreamRttTelemetry& telemetry) {
    if (telemetry.ping_ack_count == 0) {
        return 0.0;
    }

    return static_cast<double>(telemetry.total_rtt_ms) / static_cast<double>(telemetry.ping_ack_count);
}

std::uint32_t clamp_stream_target_fps(std::uint32_t fps) {
    return std::max(kDesktopStreamAdaptiveMinFps, std::min(kDesktopStreamTargetFps, fps));
}

std::uint64_t stream_frame_interval_ms(std::uint32_t fps) {
    const std::uint32_t clamped_fps = clamp_stream_target_fps(fps);
    return std::max<std::uint64_t>(1, 1000ULL / clamped_fps);
}

std::uint32_t stream_fps_hint_from_rate(double fps) {
    if (fps <= 0.0) {
        return 0;
    }

    return clamp_stream_target_fps(static_cast<std::uint32_t>(fps + 0.5));
}

std::uint32_t stream_fps_hint_from_frame_present_delta(double frame_present_delta_ms) {
    if (frame_present_delta_ms <= 0.0) {
        return 0;
    }

    return clamp_stream_target_fps(static_cast<std::uint32_t>((1000.0 / frame_present_delta_ms) + 0.5));
}

std::uint32_t clamp_stream_bitrate_kbps(
    std::uint32_t bitrate_kbps,
    std::uint32_t min_bitrate_kbps,
    std::uint32_t max_bitrate_kbps) {
    return std::max(min_bitrate_kbps, std::min(max_bitrate_kbps, bitrate_kbps));
}

std::uint32_t resolve_stream_target_fps(const StreamAdaptiveControlState& adaptive_control) {
    if (adaptive_control.target_fps != 0) {
        return clamp_stream_target_fps(adaptive_control.target_fps);
    }
    if (adaptive_control.applied_fps != 0) {
        return clamp_stream_target_fps(adaptive_control.applied_fps);
    }
    return kDesktopStreamTargetFps;
}

std::uint32_t resolve_stream_target_bitrate_kbps(const StreamAdaptiveControlState& adaptive_control) {
    if (adaptive_control.target_bitrate_kbps != 0) {
        return adaptive_control.target_bitrate_kbps;
    }
    return adaptive_control.applied_bitrate_kbps;
}

std::uint32_t resolve_stream_target_max_bitrate_kbps(const StreamAdaptiveControlState& adaptive_control) {
    if (adaptive_control.target_max_bitrate_kbps != 0) {
        return adaptive_control.target_max_bitrate_kbps;
    }

    const std::uint32_t bitrate_kbps = resolve_stream_target_bitrate_kbps(adaptive_control);
    return bitrate_kbps == 0 ? 0 : (bitrate_kbps + (bitrate_kbps / 5));
}

std::uint32_t resolve_stream_effective_video_max_width(
    std::uint32_t configured_max_width,
    const StreamAdaptiveControlState& adaptive_control) {
    (void)adaptive_control;
    return configured_max_width;
}

std::string capture_backend_to_string(redclaw::capture::CaptureBackendType backend) {
    switch (backend) {
    case redclaw::capture::CaptureBackendType::kDesktopDuplication:
        return "DesktopDuplication";
    case redclaw::capture::CaptureBackendType::kWindowsGraphicsCapture:
        return "WindowsGraphicsCapture";
    case redclaw::capture::CaptureBackendType::kGdiBitBlt:
        return "GdiBitBlt";
    default:
        return "Unknown";
    }
}

std::string encoder_backend_to_string(redclaw::capture::EncoderBackendType backend) {
    switch (backend) {
    case redclaw::capture::EncoderBackendType::kAuto:
        return "Auto";
    case redclaw::capture::EncoderBackendType::kNvenc:
        return "NVENC";
    case redclaw::capture::EncoderBackendType::kQuickSync:
        return "QuickSync";
    case redclaw::capture::EncoderBackendType::kAmf:
        return "AMF";
    case redclaw::capture::EncoderBackendType::kSoftware:
        return "Software";
    default:
        return "Unknown";
    }
}

struct LocalRuntimeControlFrames {
    std::vector<redclaw::protocol::StreamControlMessageV1> stream_controls;
};

LocalRuntimeControlFrames poll_local_runtime_control_messages(
    std::string* pending_input, redclaw::runtime::InputQaReceipts& input_qa_receipts) {
    LocalRuntimeControlFrames messages;
    if (pending_input == nullptr) {
        return messages;
    }

    std::array<char, 64U * 1024U> input{};
    std::size_t bytes_read = 0;
#if defined(_WIN32)
    const HANDLE input_handle = GetStdHandle(STD_INPUT_HANDLE);
    if (input_handle != nullptr && input_handle != INVALID_HANDLE_VALUE
        && GetFileType(input_handle) == FILE_TYPE_PIPE) {
        DWORD available = 0;
        if (PeekNamedPipe(input_handle, nullptr, 0, nullptr, &available, nullptr) != FALSE
            && available > 0) {
            DWORD read = 0;
            const DWORD requested = (std::min)(available, static_cast<DWORD>(input.size()));
            if (ReadFile(input_handle, input.data(), requested, &read, nullptr) != FALSE) {
                bytes_read = read;
            }
        }
    }
#else
    const std::streamsize available = std::cin.rdbuf()->in_avail();
    if (available > 0) {
        const std::streamsize requested = (std::min)(
            available,
            static_cast<std::streamsize>(input.size()));
        std::cin.read(input.data(), requested);
        bytes_read = static_cast<std::size_t>(std::cin.gcount());
    }
#endif
    const auto read_us = redclaw::diag::monotonic_time_us();
    if (bytes_read > 0) {
        pending_input->append(input.data(), bytes_read);
    }
    if (pending_input->size() > redclaw::protocol::kMaxStreamControlMessageBytes * 2U) {
        pending_input->clear();
        return messages;
    }

    std::size_t line_end = 0;
    while ((line_end = pending_input->find('\n')) != std::string::npos) {
        std::string line = pending_input->substr(0, line_end);
        pending_input->erase(0, line_end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto parsed = redclaw::protocol::parse_local_runtime_control_frame_v2(line);
        if (parsed.ok) {
            input_qa_receipts.command(redclaw::runtime::InputQaStage::kLocalRead, parsed.value, read_us);
            messages.stream_controls.push_back(parsed.value);
            continue;
        }

    }
    return messages;
}

redclaw::render::EncodedVideoCodec to_render_codec(redclaw::capture::EncoderCodec codec) {
    if (codec == redclaw::capture::EncoderCodec::kHevc) {
        return redclaw::render::EncodedVideoCodec::kHevc;
    }
    return redclaw::render::EncodedVideoCodec::kH264;
}

std::uint8_t encoded_video_codec_to_binary(redclaw::render::EncodedVideoCodec codec) {
    if (codec == redclaw::render::EncodedVideoCodec::kH264) {
        return 1;
    }
    if (codec == redclaw::render::EncodedVideoCodec::kHevc) {
        return 2;
    }
    return 0;
}

redclaw::render::EncodedVideoCodec encoded_video_codec_from_binary(std::uint8_t value) {
    if (value == 1) {
        return redclaw::render::EncodedVideoCodec::kH264;
    }
    if (value == 2) {
        return redclaw::render::EncodedVideoCodec::kHevc;
    }
    return redclaw::render::EncodedVideoCodec::kUnknown;
}

bool write_decoded_frame_ppm(
    const redclaw::render::DecodedVideoFrame& frame,
    const std::filesystem::path& output_path,
    std::string* error) {
    if (!frame.bgra || frame.width == 0 || frame.height == 0 || frame.row_pitch < frame.width * 4) {
        *error = "decoded frame must be non-empty BGRA";
        return false;
    }
    if (frame.pixels.size() < static_cast<std::size_t>(frame.row_pitch) * frame.height) {
        *error = "decoded frame buffer is smaller than row_pitch * height";
        return false;
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        *error = "failed to open decoded frame output: " + output_path.string();
        return false;
    }

    out << "P6\n" << frame.width << " " << frame.height << "\n255\n";
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        const auto* row = frame.pixels.data() + static_cast<std::size_t>(y) * frame.row_pitch;
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::size_t src = static_cast<std::size_t>(x) * 4;
            const char rgb[3] = {
                static_cast<char>(row[src + 2]),
                static_cast<char>(row[src + 1]),
                static_cast<char>(row[src + 0]),
            };
            out.write(rgb, sizeof(rgb));
        }
    }

    if (!out.good()) {
        *error = "failed while writing decoded frame output: " + output_path.string();
        return false;
    }
    return true;
}

std::uint32_t floor_even_dimension(std::uint32_t value) {
    if (value <= 2U) {
        return 2U;
    }
    return (value % 2U) == 0U ? value : value - 1U;
}

bool resolve_video_encode_dimensions(
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint32_t max_width,
    std::uint32_t* target_width,
    std::uint32_t* target_height,
    std::string* error) {
    if (target_width == nullptr || target_height == nullptr) {
        if (error != nullptr) {
            *error = "target dimension outputs must be non-null";
        }
        return false;
    }
    if (source_width == 0 || source_height == 0) {
        if (error != nullptr) {
            *error = "source frame dimensions must be non-zero";
        }
        return false;
    }

    if (max_width == 0 || source_width <= max_width) {
        *target_width = source_width;
        *target_height = source_height;
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    *target_width = floor_even_dimension(std::min(max_width, source_width));
    *target_height = floor_even_dimension(static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(source_height) * *target_width + (source_width / 2U)) / source_width));
    if (*target_width == 0 || *target_height == 0) {
        if (error != nullptr) {
            *error = "encoded frame dimensions resolved to zero";
        }
        return false;
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

#ifdef _WIN32
constexpr std::uint64_t kDiagnosticPreviewWriteIntervalMs = 1000;

enum class DirectFramePipeWriteStatus {
    kWritten,
    kBusyDrop,
    kOversizeDrop,
    kInvalidFrame,
    kDisconnected,
    kFailed,
};

class DirectFramePipeClient {
public:
    explicit DirectFramePipeClient(std::string channel_name)
        : channel_name_(std::move(channel_name)) {}

    ~DirectFramePipeClient() {
        close();
    }

    bool enabled() const {
        return !channel_name_.empty();
    }

    DirectFramePipeWriteStatus write_bgra_frame(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t row_pitch,
        std::uint64_t timestamp_ms,
        const std::uint8_t* pixels,
        std::size_t pixel_bytes,
        std::string* error) {
        if (error != nullptr) {
            error->clear();
        }
        if (channel_name_.empty()) {
            return DirectFramePipeWriteStatus::kDisconnected;
        }
        if (pixels == nullptr || width == 0 || height == 0 || row_pitch < width * 4U) {
            if (error != nullptr) {
                *error = "invalid BGRA frame for direct pipe";
            }
            return DirectFramePipeWriteStatus::kInvalidFrame;
        }
        if (pixel_bytes < static_cast<std::size_t>(row_pitch) * static_cast<std::size_t>(height)) {
            if (error != nullptr) {
                *error = "BGRA frame buffer is smaller than row_pitch * height";
            }
            return DirectFramePipeWriteStatus::kInvalidFrame;
        }
        if (pixel_bytes > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
            if (error != nullptr) {
                *error = "BGRA frame is too large for direct pipe";
            }
            return DirectFramePipeWriteStatus::kInvalidFrame;
        }
        if (pixel_bytes > redclaw::helper::kDirectFrameMaxPayloadBytes) {
            if (error != nullptr) {
                *error = "BGRA frame is too large for shared frame channel";
            }
            return DirectFramePipeWriteStatus::kOversizeDrop;
        }
        if (!ensure_connected(error)) {
            return DirectFramePipeWriteStatus::kDisconnected;
        }

        redclaw::helper::DirectFrameChannelFrameHeader header = {};
        std::memcpy(header.magic,
                    redclaw::helper::kDirectFrameChannelMagic,
                    sizeof(redclaw::helper::kDirectFrameChannelMagic));
        header.width = width;
        header.height = height;
        header.row_pitch = row_pitch;
        header.format = redclaw::helper::kDirectFrameFormatBgra;
        header.timestamp_ms = timestamp_ms;
        header.payload_size = static_cast<std::uint32_t>(pixel_bytes);
        header.capture_region_revision = 1;
        header.content_rect_width = width;
        header.content_rect_height = height;

        return write_frame(header, pixels, pixel_bytes, error);
    }

    // Write a raw encoded frame (H264/HEVC NAL units) into the shared-memory
    // ring.  The GUI process decodes it in-process, avoiding named-pipe kernel
    // copies and keeping the receive loop non-blocking.
    DirectFramePipeWriteStatus write_encoded_frame(
        const redclaw::render::EncodedVideoFrame& frame,
        std::string* error) {
        if (error != nullptr) {
            error->clear();
        }
        if (channel_name_.empty()) {
            return DirectFramePipeWriteStatus::kDisconnected;
        }
        if (frame.payload.empty() || frame.width == 0 || frame.height == 0) {
            if (error != nullptr) {
                *error = "invalid encoded frame for direct pipe";
            }
            return DirectFramePipeWriteStatus::kInvalidFrame;
        }

        std::uint32_t format = 0;
        switch (frame.codec) {
            case redclaw::render::EncodedVideoCodec::kH264:
                format = redclaw::helper::kDirectFrameFormatH264;
                break;
            case redclaw::render::EncodedVideoCodec::kHevc:
                format = redclaw::helper::kDirectFrameFormatHevc;
                break;
            default:
                if (error != nullptr) {
                    *error = "unsupported codec for encoded direct pipe";
                }
                return DirectFramePipeWriteStatus::kInvalidFrame;
        }
        if (frame.payload.size() > redclaw::helper::kDirectFrameMaxPayloadBytes) {
            if (error != nullptr) {
                *error = "encoded frame is too large for shared frame channel";
            }
            return DirectFramePipeWriteStatus::kOversizeDrop;
        }

        if (!ensure_connected(error)) {
            return DirectFramePipeWriteStatus::kDisconnected;
        }

        redclaw::helper::DirectFrameChannelFrameHeader header = {};
        std::memcpy(header.magic,
                    redclaw::helper::kDirectFrameChannelMagic,
                    sizeof(redclaw::helper::kDirectFrameChannelMagic));
        header.format = format;
        header.width = frame.width;
        header.height = frame.height;
        header.row_pitch = 0;  // not applicable for encoded frames
        header.timestamp_ms = frame.timestamp_ms;
        header.frame_id = frame.frame_id;
        header.payload_size = static_cast<std::uint32_t>(frame.payload.size());
        header.flags = frame.keyframe ? 1u : 0u;
        header.capture_region_revision = frame.capture_region_revision;
        header.content_rect_x = frame.content_rect_x;
        header.content_rect_y = frame.content_rect_y;
        header.content_rect_width = frame.content_rect_width;
        header.content_rect_height = frame.content_rect_height;

        return write_frame(header, frame.payload.data(), frame.payload.size(), error);
    }

    DirectFramePipeWriteStatus write_navigation_thumbnail(
        const redclaw::net::NavigationThumbnail& thumbnail,
        std::string* error) {
        if (thumbnail.display_id.empty()
            || thumbnail.display_id.size() >= 128
            || thumbnail.jpeg.empty()
            || thumbnail.jpeg.size() > redclaw::helper::kDirectFrameMaxPayloadBytes
            || thumbnail.width == 0 || thumbnail.height == 0) {
            if (error != nullptr) {
                *error = "invalid navigation thumbnail for direct pipe";
            }
            return DirectFramePipeWriteStatus::kInvalidFrame;
        }
        if (!ensure_connected(error)) {
            return DirectFramePipeWriteStatus::kDisconnected;
        }
        redclaw::helper::DirectFrameChannelFrameHeader header = {};
        std::memcpy(header.magic,
                    redclaw::helper::kDirectFrameChannelMagic,
                    sizeof(redclaw::helper::kDirectFrameChannelMagic));
        header.width = thumbnail.width;
        header.height = thumbnail.height;
        header.format = redclaw::helper::kDirectFrameFormatJpeg;
        header.payload_size = static_cast<std::uint32_t>(thumbnail.jpeg.size());
        header.capture_region_revision = thumbnail.thumbnail_revision;
        header.content_rect_width = thumbnail.width;
        header.content_rect_height = thumbnail.height;
        std::memcpy(header.display_id,
                    thumbnail.display_id.data(), thumbnail.display_id.size());
        return write_frame(
            header, thumbnail.jpeg.data(), thumbnail.jpeg.size(), error);
    }

    void close() {
        shared_header_ = nullptr;
        if (mapping_view_ != nullptr) {
            UnmapViewOfFile(mapping_view_);
            mapping_view_ = nullptr;
        }
        if (data_event_ != nullptr) {
            CloseHandle(data_event_);
            data_event_ = nullptr;
        }
        if (mapping_handle_ != nullptr) {
            CloseHandle(mapping_handle_);
            mapping_handle_ = nullptr;
        }
    }

    DirectFramePipeTransportStats stats_snapshot() const {
        DirectFramePipeTransportStats snapshot;
        const auto* shared_header = shared_header_;
        if (shared_header == nullptr || mapping_view_ == nullptr) {
            return snapshot;
        }

        snapshot.connected = true;
        snapshot.write_sequence = static_cast<std::uint64_t>(
            redclaw::helper::direct_frame_atomic_load_i64(&shared_header->write_sequence));
        snapshot.latest_sequence = static_cast<std::uint64_t>(
            redclaw::helper::direct_frame_atomic_load_i64(&shared_header->latest_sequence));
        snapshot.reader_sequence = static_cast<std::uint64_t>(
            redclaw::helper::direct_frame_atomic_load_i64(&shared_header->reader_sequence));
        snapshot.reader_active_sequence = static_cast<std::uint64_t>(
            redclaw::helper::direct_frame_atomic_load_i64(&shared_header->reader_active_sequence));
        snapshot.writer_frame_count = static_cast<std::uint64_t>(
            redclaw::helper::direct_frame_atomic_load_i64(&shared_header->writer_frame_count));
        snapshot.writer_oversize_drop_count = static_cast<std::uint64_t>(
            redclaw::helper::direct_frame_atomic_load_i64(&shared_header->writer_oversize_drop_count));
        snapshot.writer_busy_drop_count = static_cast<std::uint64_t>(
            redclaw::helper::direct_frame_atomic_load_i64(&shared_header->writer_busy_drop_count));
        return snapshot;
    }

private:
    bool ensure_connected(std::string* error) {
        if (shared_header_ != nullptr && mapping_view_ != nullptr && data_event_ != nullptr) {
            return true;
        }

        const std::wstring mapping_name = redclaw::helper::direct_frame_shared_mapping_name(channel_name_);
        mapping_handle_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mapping_name.c_str());
        if (mapping_handle_ == nullptr) {
            if (error != nullptr) {
                *error = "OpenFileMapping shared frame channel failed: " + std::to_string(GetLastError());
            }
            close();
            return false;
        }

        mapping_view_ = MapViewOfFile(mapping_handle_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
        if (mapping_view_ == nullptr) {
            if (error != nullptr) {
                *error = "MapViewOfFile shared frame channel failed: " + std::to_string(GetLastError());
            }
            close();
            return false;
        }

        shared_header_ = static_cast<redclaw::helper::DirectFrameSharedMemoryHeader*>(mapping_view_);
        if (std::memcmp(shared_header_->magic,
                        redclaw::helper::kDirectFrameSharedMemoryMagic,
                        sizeof(redclaw::helper::kDirectFrameSharedMemoryMagic)) != 0
            || shared_header_->version != redclaw::helper::kDirectFrameSharedMemoryVersion
            || shared_header_->slot_count != redclaw::helper::kDirectFrameSharedMemorySlotCount
            || shared_header_->slot_payload_bytes != redclaw::helper::kDirectFrameSharedMemorySlotPayloadBytes) {
            if (error != nullptr) {
                *error = "shared frame channel header validation failed";
            }
            close();
            return false;
        }

        const std::wstring event_name = redclaw::helper::direct_frame_shared_event_name(channel_name_);
        data_event_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, event_name.c_str());
        if (data_event_ == nullptr) {
            if (error != nullptr) {
                *error = "OpenEvent shared frame channel failed: " + std::to_string(GetLastError());
            }
            close();
            return false;
        }

        return true;
    }

    DirectFramePipeWriteStatus write_frame(
        const redclaw::helper::DirectFrameChannelFrameHeader& header,
        const std::uint8_t* payload,
        std::size_t payload_bytes,
        std::string* error) {
        if (shared_header_ == nullptr || mapping_view_ == nullptr || data_event_ == nullptr) {
            if (error != nullptr) {
                *error = "shared frame channel is not connected";
            }
            return DirectFramePipeWriteStatus::kDisconnected;
        }
        if (payload == nullptr || payload_bytes == 0 || payload_bytes > shared_header_->slot_payload_bytes) {
            redclaw::helper::direct_frame_atomic_increment_i64(&shared_header_->writer_oversize_drop_count);
            if (error != nullptr) {
                *error = "payload does not fit shared frame channel slot";
            }
            return DirectFramePipeWriteStatus::kOversizeDrop;
        }

        const LONG64 sequence =
            redclaw::helper::direct_frame_atomic_load_i64(&shared_header_->write_sequence) + 1;
        const std::uint32_t slot_index = static_cast<std::uint32_t>((sequence - 1) % shared_header_->slot_count);
        auto* slot_header = redclaw::helper::direct_frame_shared_slot_header(mapping_view_, slot_index);
        const LONG64 committed_sequence =
            redclaw::helper::direct_frame_atomic_load_i64(&slot_header->committed_sequence);
        const LONG64 reader_active_sequence =
            redclaw::helper::direct_frame_atomic_load_i64(&shared_header_->reader_active_sequence);
        if (committed_sequence != 0 && committed_sequence == reader_active_sequence) {
            redclaw::helper::direct_frame_atomic_increment_i64(
                &shared_header_->writer_busy_drop_count);
            return DirectFramePipeWriteStatus::kBusyDrop;
        }
        auto* slot_payload = redclaw::helper::direct_frame_shared_slot_payload(slot_header);

        redclaw::helper::direct_frame_atomic_store_i64(&slot_header->committed_sequence, 0);
        slot_header->frame = header;
        std::memcpy(slot_payload, payload, payload_bytes);
        MemoryBarrier();
        redclaw::helper::direct_frame_atomic_store_i64(&slot_header->committed_sequence, sequence);
        redclaw::helper::direct_frame_atomic_store_i64(&shared_header_->write_sequence, sequence);
        redclaw::helper::direct_frame_atomic_store_i64(&shared_header_->latest_sequence, sequence);
        redclaw::helper::direct_frame_atomic_increment_i64(&shared_header_->writer_frame_count);

        if (SetEvent(data_event_) == 0) {
            if (error != nullptr) {
                *error = "SetEvent shared frame channel failed: " + std::to_string(GetLastError());
            }
            close();
            return DirectFramePipeWriteStatus::kFailed;
        }
        return DirectFramePipeWriteStatus::kWritten;
    }

    std::string channel_name_;
    HANDLE mapping_handle_ = nullptr;
    HANDLE data_event_ = nullptr;
    void* mapping_view_ = nullptr;
    redclaw::helper::DirectFrameSharedMemoryHeader* shared_header_ = nullptr;
};

#endif

int run_runtime_mode(
    const RuntimeOptions& options,
    redclaw::diag::ProcessFileLogger* process_logger,
    redclaw::runtime::LocalControlOutput& local_control_output) {
    const std::string role_name = options.role == RuntimeRole::kHost ? "host" : "controller";
    const bool use_event_log_transport = options.signal_transport == "event-log";
    const bool use_tcp_transport = options.signal_transport == "tcp";
    const bool use_sealed_file_transport = options.signal_transport == "sealed-file";
    const bool use_rendezvous_transport = options.signal_transport == "rendezvous";
    const bool use_dht_transport = options.signal_transport == "dht";
    const FileSignalingPaths signaling = make_file_signaling_paths(options);
    const std::filesystem::path stream_preview_output = signaling.directory / "controller-preview.ppm";
    const std::filesystem::path dht_mailbox_directory = signaling.directory / "dht-mailbox";
    const std::uint16_t tcp_signaling_port = options.target_port > 0
        ? options.target_port
        : static_cast<std::uint16_t>(
#ifdef _WIN32
            kDefaultTcpSignalingPort
#else
            45909
#endif
        );
    redclaw::net::IceConnectivityWrapper ice_wrapper;
    std::string ice_port_reservation_error;
    if (!ice_wrapper.reserveFixedUdpPort(
            options.ice_udp_port,
            options.network_bind_address,
            &ice_port_reservation_error)) {
        std::cerr << "Runtime ICE UDP port reservation failed role=" << role_name
                  << " ice_udp_port=" << options.ice_udp_port
                  << " error=" << ice_port_reservation_error << '\n';
        return 1;
    }
    std::cout << "RedClawDesktop runtime mode started role=" << role_name
              << " signal_dir=" << signaling.directory.string() << '\n';
    std::cout << "Runtime signaling transport=" << options.signal_transport << '\n';
    std::cout << "Runtime ICE config ice_server_count=" << options.ice_servers.size()
              << " ice_server_types=" << format_ice_server_type_counts(options.ice_servers)
              << " enable_ice_tcp=" << (options.enable_ice_tcp ? "true" : "false")
              << " ice_udp_port=" << options.ice_udp_port
              << " port_mapping=" << (options.enable_port_mapping ? "requested" : "disabled")
              << " network_bind_address="
              << (options.network_bind_address.empty() ? "auto" : options.network_bind_address)
              << " network_bind_ipv6="
              << (options.network_bind_ipv6_address.empty() ? "none" : "selected-interface")
              << " signal_timeout_seconds=" << options.signal_timeout_seconds << '\n';
    if (options.stream_smoke) {
        std::cout << "Runtime desktop stream smoke enabled preview_width=" << options.stream_preview_width
                  << " require_capture=" << (options.stream_require_capture ? "true" : "false")
                  << " controller_preview_output=" << stream_preview_output.string() << '\n';
    }
    if (use_sealed_file_transport) {
        std::cout << "Runtime sealed signaling enabled role=" << role_name
                  << " passphrase_length=" << options.signal_passphrase.size() << '\n';
    }
    if (use_rendezvous_transport) {
        std::cout << "Runtime rendezvous signaling enabled role=" << role_name
                  << " url=" << options.rendezvous_url << '\n';
        if (!options.session_code.empty()) {
            std::cout << "Runtime rendezvous session-code=" << options.session_code << '\n';
        }
    }
    if (use_dht_transport) {
#if defined(REDCLAW_ENABLE_DHT_BACKENDS)
        const char* dht_primary_backend = "libtorrent-mainline";
#else
        const char* dht_primary_backend = "file-mailbox-only";
#endif
        const auto helper_probe = redclaw::service::probe_local_helper_path(dht_mailbox_directory);
        std::cout << "Runtime DHT config role=" << role_name
                  << " backend=" << dht_primary_backend
                  << " secondary_store=file-mailbox"
                  << " bootstrap_nodes=" << format_ice_server_list(options.dht_bootstrap_nodes)
                  << " mailbox_path=" << dht_mailbox_directory.string()
                  << " listen_address=" << (options.network_bind_address.empty() ? "auto" : options.network_bind_address)
                  << " listen_ipv6="
                  << (options.network_bind_ipv6_address.empty() ? "none" : "selected-interface")
                  << " listen_port=" << options.dht_listen_port
                  << " poll_interval_ms=" << options.dht_poll_interval_ms
                  << " publish_retry_ms=" << options.dht_publish_retry_ms
                  << " helper_status=" << redclaw::service::local_helper_path_status_to_string(helper_probe.status)
                  << " helper_role=" << options.helper_role
                  << " ipv6_candidates=" << (options.enable_ipv6_candidates ? "enabled" : "disabled")
                  << '\n';
        if (!helper_probe.detail.empty()) {
            std::cout << "Runtime DHT helper detail role=" << role_name
                      << " detail=" << helper_probe.detail << '\n';
        }
    }

    if (options.role == RuntimeRole::kController && !options.target_host.empty()) {
        std::cout << "Controller target configured host=" << options.target_host;
        if (options.target_port > 0) {
            std::cout << " port=" << options.target_port;
        }
        std::cout << '\n';
    }

    if (use_tcp_transport) {
        if (options.role == RuntimeRole::kHost) {
            std::cout << "Runtime TCP signaling listen port=" << tcp_signaling_port << '\n';
        } else {
            std::cout << "Runtime TCP signaling remote=" << options.target_host
                      << ":" << tcp_signaling_port << '\n';
        }
    }

    redclaw::render::RuntimeStatusTimeline runtime_timeline;
    redclaw::render::RuntimeStatusTimelineUiModel runtime_timeline_ui_model;
    runtime_timeline_ui_model.bind(&runtime_timeline);
    redclaw::render::RuntimeStatusTimelineWidgetComponent runtime_timeline_widget_component;
    runtime_timeline_widget_component.bind(&runtime_timeline_ui_model);
    const std::filesystem::path host_event_log = signaling.directory / "host-events.log";
    const std::filesystem::path controller_event_log = signaling.directory / "controller-events.log";
    const std::filesystem::path local_event_log = options.role == RuntimeRole::kHost
        ? host_event_log
        : controller_event_log;
    const std::filesystem::path remote_event_log = options.role == RuntimeRole::kHost
        ? controller_event_log
        : host_event_log;

    std::string runtime_error;
    if (!ensure_signaling_directory(signaling.directory, &runtime_error)) {
        std::cerr << "Runtime signaling init failed: " << runtime_error << '\n';
        return 1;
    }

    bool signal_snapshot_dirty = false;

#ifdef _WIN32
    ParsedWinHttpBaseUrl rendezvous_base_url;
    std::string rendezvous_session_id;
    std::uint64_t rendezvous_expires_at_unix = 0;
    std::size_t applied_remote_rendezvous_candidates = 0;
    RendezvousTransportDiagnostics rendezvous_diagnostics;
    bool rendezvous_host_claim_observed = false;
    std::uint64_t rendezvous_next_poll_ms = 0;

    if (use_rendezvous_transport
        && !parse_winhttp_base_url(options.rendezvous_url, &rendezvous_base_url, &runtime_error)) {
        std::cerr << "Runtime rendezvous URL validation failed: " << runtime_error << '\n';
        return 1;
    }
#else
    if (use_rendezvous_transport) {
        std::cerr << "Runtime rendezvous signaling transport is unsupported on this platform" << '\n';
        return 1;
    }
#endif

    std::unique_ptr<redclaw::service::IDhtRendezvousStore> dht_store;
#if defined(REDCLAW_ENABLE_DHT_BACKENDS)
    redclaw::service::LibtorrentDhtRendezvousStore* libtorrent_dht_store = nullptr;
#endif
    std::unique_ptr<redclaw::service::DhtRendezvousClient> dht_client;
    redclaw::service::DhtRendezvousConfig dht_config;
    bool dht_reachable = false;
    std::uint32_t dht_publish_attempts = 0;
    std::uint32_t dht_publish_success = 0;
    std::uint32_t dht_generation_publish_success = 0;
    std::uint32_t dht_fetch_attempts = 0;
    std::uint32_t dht_fetch_hits = 0;
    std::uint64_t dht_local_revision = 0;
    std::uint64_t dht_remote_revision = 0;
    redclaw::service::DhtPublicationTransaction dht_publication(use_dht_transport
        ? redclaw::service::make_dht_publish_revision_floor(now_unix_ms())
        : 0);
    std::uint64_t dht_remote_record_expiry = 0;
    std::uint64_t dht_failed_remote_expiry = 0;
    std::uint64_t dht_failed_remote_revision = 0;
    std::uint64_t dht_failed_remote_generation = 0;
    std::string dht_failed_remote_offer_tag;
    std::string dht_publisher_instance_id;
    std::string dht_last_applied_host_instance_id;
    std::uint64_t dht_persistent_offer_adopted_initial_total = 0;
    std::uint64_t dht_persistent_offer_adopted_host_restart_total = 0;
    std::uint64_t dht_persistent_offer_same_instance_rejected_total = 0;
    std::uint64_t dht_peer_instance_missing_total = 0;
    std::uint64_t dht_duplicate_offer_application_suppressed_total = 0;
    std::uint32_t dht_repair_attempts = 0;
    std::uint64_t dht_repair_after_ms = 0;
    std::optional<std::uint64_t> dht_initial_failure_started_steady_ms;
    std::uint32_t dht_stale_remote_skips = 0;
    bool dht_stale_remote_reported = false;
    bool dht_peer_republished_description = false;
    bool dht_controller_request_superseded = false;
    std::string applied_remote_description_sdp;
    bool dht_initial_snapshot_publish_done = false;
    bool dht_full_candidate_publish_done = false;
    bool dht_answer_ack_publish_done = false;
    std::vector<std::string> dht_last_published_candidate_lines;
    std::size_t dht_direct_candidate_publish_count = 0;
    std::size_t dht_last_remote_candidate_count = 0;
    bool dht_remote_candidates_complete = false;
    std::set<std::string> applied_remote_dht_candidate_lines;
    std::set<std::string> pending_remote_dht_candidate_lines;
    std::string dht_last_error;
    std::string dht_backend_name = "none";
    redclaw::service::UpnpPortMappingResult ice_port_mapping_result;
    bool ice_port_mapping_attempted = false;
    std::uint64_t dht_next_alert_pump_ms = 0;
    std::uint64_t dht_next_publish_retry_ms = 0;
    std::uint64_t dht_next_publish_refresh_ms = 0;
    std::uint64_t dht_next_fetch_ms = 0;
    std::uint64_t dht_host_generation_sequence = 0;
    bool dht_host_persistent_offer_active = false;
    bool local_candidate_gathering_complete = false;
    redclaw::service::ConnectionNegotiationCoordinator dht_negotiation(
        options.role == RuntimeRole::kHost
            ? redclaw::service::ConnectionNegotiationRole::kHost
            : redclaw::service::ConnectionNegotiationRole::kController);
    std::string dht_connection_request_payload;

    // Records seal their publish time into the expiry, so the age of that seal
    // tells how long ago the peer last published anything.
    const auto dht_sealed_age_seconds = [](std::uint64_t expires_at_unix) -> std::int64_t {
        const std::int64_t sealed_at = static_cast<std::int64_t>(expires_at_unix)
            - static_cast<std::int64_t>(redclaw::service::kDhtRecordTtlSeconds);
        return static_cast<std::int64_t>(now_unix_ms() / 1000ULL) - sealed_at;
    };
    const auto dht_instance_summary = [](std::string_view instance_id) {
        constexpr std::size_t kVisibleCharacters = 8;
        return instance_id.empty()
            ? std::string("legacy")
            : std::string(instance_id.substr(0, kVisibleCharacters));
    };

    if (use_dht_transport) {
#if defined(REDCLAW_ENABLE_DHT_BACKENDS)
        redclaw::service::LibtorrentDhtRendezvousStoreOptions dht_store_options;
        dht_store_options.bootstrap_nodes = options.dht_bootstrap_nodes;
        dht_store_options.listen_address = options.network_bind_address;
        dht_store_options.listen_ipv6_address = options.network_bind_ipv6_address;
        dht_store_options.listen_port = options.dht_listen_port;
        dht_store_options.enable_ipv6 = options.enable_ipv6_candidates;
        std::unique_ptr<redclaw::service::LibtorrentDhtRendezvousStore> primary_store;
        try {
            primary_store = std::make_unique<redclaw::service::LibtorrentDhtRendezvousStore>(dht_store_options);
        } catch (const std::exception&) {
            // Native startup errors may contain local addresses or configuration.
            // Fail before starting session workers without exposing that output.
            std::cerr << "Runtime DHT listener startup failed role=" << role_name
                      << " reason=listener_initialization_failed" << '\n';
            std::cout << "Runtime DHT backend diagnostics role=" << role_name
                      << " listen_ready=0 listen_startup_failed=1"
                      << " listen_port_probe_attempts=0" << '\n';
            return 1;
        }
        libtorrent_dht_store = primary_store.get();
        auto secondary_store = std::make_unique<redclaw::service::FileBackedDhtRendezvousStore>(dht_mailbox_directory);
        dht_store = std::make_unique<redclaw::service::IndirectDhtRendezvousStore>(
            std::move(primary_store),
            std::move(secondary_store));
        dht_backend_name = "libtorrent-mainline+file-mailbox";
#else
        dht_store = std::make_unique<redclaw::service::FileBackedDhtRendezvousStore>(dht_mailbox_directory);
        dht_backend_name = "file-mailbox-only";
        std::cout << "Runtime DHT network backend disabled role=" << role_name
                  << " backend=" << dht_backend_name
                  << " reason=build_without_dht_backends" << '\n';
#endif
        dht_client = std::make_unique<redclaw::service::DhtRendezvousClient>(*dht_store);
        dht_config.session_code = options.session_code;
        dht_config.pairing_secret = options.signal_passphrase.empty() ? options.session_code : options.signal_passphrase;
        dht_config.now_unix = now_unix_ms() / 1000ULL;
        dht_config.ttl_seconds = redclaw::service::kDhtRecordTtlSeconds;
        std::string instance_error;
        const auto publisher_instance_id =
            redclaw::service::make_dht_publisher_instance_id(&instance_error);
        if (!publisher_instance_id.has_value()) {
            std::cerr << "Runtime DHT publisher instance initialization failed role="
                      << role_name << " error=" << instance_error << '\n';
            return 1;
        }
        dht_publisher_instance_id = *publisher_instance_id;
        std::cout << "Runtime DHT topic role=" << role_name
                  << " topic=" << redclaw::service::derive_dht_rendezvous_topic_hex(
                         dht_config.session_code,
                         dht_config.pairing_secret)
                  << " publisher_instance="
                  << dht_instance_summary(dht_publisher_instance_id)
                  << '\n';
    }

    if (use_dht_transport) {
        std::cout << "Runtime DHT setup complete role=" << role_name
                  << " backend=" << dht_backend_name
                  << '\n';
        if (options.role == RuntimeRole::kController) {
            dht_connection_request_payload = make_runtime_session_id("controller-request");
            const std::string request_tag = redclaw::service::derive_dht_description_tag(
                dht_connection_request_payload);
            const auto request_result = dht_negotiation.begin_controller_request(request_tag);
            if (!request_result.accepted()) {
                std::cerr << "Runtime failed to initialize Controller DHT connect request" << '\n';
                return 1;
            }
            signal_snapshot_dirty = true;
        }
    }

    auto append_timeline = [&](redclaw::render::RuntimeStatusSeverity severity,
                               std::string category,
                               std::string message) {
        redclaw::render::RuntimeStatusEvent event;
        event.timestamp_ms = now_unix_ms();
        event.severity = severity;
        event.category = std::move(category);
        event.message = std::move(message);
        runtime_timeline.append(std::move(event));
    };

    append_timeline(redclaw::render::RuntimeStatusSeverity::kInfo, "runtime", "runtime mode initialized");

#ifdef _WIN32
    if (use_rendezvous_transport && options.role == RuntimeRole::kHost) {
        rendezvous_session_id = make_runtime_session_id(role_name);
        const std::uint32_t rendezvous_ttl_seconds = std::max<std::uint32_t>(
            300U,
            options.signal_timeout_seconds > 0 ? options.signal_timeout_seconds + 60U : 300U);

        RendezvousRegisterHttpResult register_result;
        ++rendezvous_diagnostics.register_attempts;
        if (!rendezvous_register_session_http(
                rendezvous_base_url,
                options.session_code,
                rendezvous_session_id,
                "Runtime Host",
                "fingerprint-pending",
                rendezvous_ttl_seconds,
                &register_result,
                &runtime_error)) {
            std::cerr << "Runtime rendezvous register failed: " << runtime_error << '\n';
            return 1;
        }
        if (!register_result.accepted) {
            std::cerr << "Runtime rendezvous register rejected error=" << register_result.error << '\n';
            return 1;
        }

        ++rendezvous_diagnostics.register_success;
        rendezvous_expires_at_unix = register_result.expires_at_unix;
        rendezvous_diagnostics.last_error.clear();
        std::cout << "Runtime rendezvous registered role=" << role_name
                  << " session_code=" << options.session_code
                  << " session_id=" << rendezvous_session_id
                  << " expires_at_unix=" << register_result.expires_at_unix
                  << '\n';
        append_timeline(
            redclaw::render::RuntimeStatusSeverity::kInfo,
            "signal",
            "rendezvous session registered for machine-code signaling");
    }
#endif

    if (options.role == RuntimeRole::kHost) {
        if (!remove_file_if_exists(signaling.host_offer, &runtime_error)
            || !remove_file_if_exists(signaling.controller_answer, &runtime_error)
            || !remove_file_if_exists(signaling.host_candidates, &runtime_error)
            || !remove_file_if_exists(signaling.controller_candidates, &runtime_error)
            || !remove_file_if_exists(signaling.host_offer_sealed, &runtime_error)
            || !remove_file_if_exists(signaling.controller_answer_sealed, &runtime_error)
            || !remove_file_if_exists(host_event_log, &runtime_error)
            || !remove_file_if_exists(controller_event_log, &runtime_error)) {
            std::cerr << "Runtime signaling cleanup failed: " << runtime_error << '\n';
            return 1;
        }
    }

    std::cout << "Runtime ICE UDP port reserved role=" << role_name
              << " ice_udp_port=" << options.ice_udp_port << '\n';
    std::mutex callback_mutex;
    redclaw::net::IceConnectionState connection_state = redclaw::net::IceConnectionState::kNew;
    bool saw_connected_state = false;
    bool saw_failed_state = false;
    bool local_description_written = false;
    CandidateDiagnostics candidate_diagnostics;
    std::vector<std::vector<std::string>> pending_outbound_signal_events;
    std::vector<std::vector<std::string>> outbound_signal_history;
    std::string local_description_sdp;
    std::vector<std::string> local_candidate_lines;
    std::string last_remote_sealed_blob;
    std::size_t applied_remote_sealed_candidates = 0;
    std::uint64_t last_remote_sealed_blob_update_ms = 0;
    bool remote_sealed_blob_seen = false;
    DesktopStreamSmokeCounters stream_counters;
    bool stream_media_channel_open = false;
    bool stream_control_channel_open = false;
    bool stream_agent_channel_open = false;
    bool stream_navigation_channel_open = false;
    redclaw::net::DataChannelTransportStats stream_data_channel_transport_stats_snapshot;
    std::string stream_control_epoch = "runtime-" + role_name + "-" + std::to_string(now_unix_ms());
    std::atomic<std::uint64_t> stream_control_message_id{0};
    std::uint64_t stream_control_epoch_generation = 0;
    redclaw::protocol::StreamControlEpochGuardV1 stream_remote_control_guard;
    std::uint32_t stream_requested_viewport_width = 0;
    std::uint32_t stream_requested_viewport_height = 0;
    std::vector<redclaw::capture::CaptureDisplayDescriptor> stream_capture_displays;
    std::optional<redclaw::capture::CaptureDisplayDescriptor> stream_selected_display;
    redclaw::capture::CaptureRegion stream_active_capture_region;
    std::optional<redclaw::protocol::StreamControlMessageV1>
        stream_pending_capture_region_request;
    std::optional<redclaw::protocol::StreamControlMessageV1>
        stream_capture_region_apply_pending;
    std::optional<redclaw::capture::CaptureDisplayDescriptor>
        stream_capture_region_rollback_display;
    redclaw::capture::CaptureRegion stream_capture_region_rollback_region;
    std::uint64_t stream_capture_region_revision = 1;
    std::uint64_t stream_navigation_catalog_revision = 1;
    std::uint64_t stream_navigation_thumbnail_revision = 0;
    std::uint64_t stream_last_navigation_thumbnail_ms = 0;
    std::uint64_t stream_last_display_catalog_check_ms = 0;
    redclaw::protocol::StreamControlMessageV1 stream_receiver_stats_snapshot;
    std::uint64_t stream_received_wire_bytes = 0;
    std::uint64_t stream_media_fragments_sent = 0;
    std::uint64_t stream_media_fragments_received = 0;
    std::uint64_t stream_fragment_parse_failures = 0;
    std::uint64_t stream_reassembly_failures = 0;
    std::string stream_last_media_assembly_error;
    std::uint64_t stream_qa_media_fragment_drop_total = 0;
    std::uint64_t stream_qa_media_loss_frame_id = 0;
    std::uint64_t stream_qa_media_recovery_keyframe_id = 0;
    std::uint64_t stream_qa_forced_channel_close_total = 0;
    std::uint64_t stream_qa_forced_channel_close_frame_id = 0;
    std::uint64_t stream_qa_recovery_success_total_at_forced_close = 0;
    std::uint64_t stream_qa_post_reconnect_frame_id = 0;
    std::uint64_t stream_qa_post_reconnect_direct_pipe_written_total = 0;
    bool stream_qa_forced_channel_close_attempted = false;
    std::optional<PendingIncompleteMediaFeedback> stream_qa_pending_incomplete_feedback;
    std::uint64_t stream_encoded_frames_reassembled = 0;
    std::uint64_t stream_incomplete_frames_dropped = 0;
    std::uint64_t stream_dependency_frames_dropped = 0;
    std::uint64_t stream_completed_keyframes = 0;
    std::uint64_t stream_dropped_keyframes = 0;
    std::uint64_t stream_reassembly_timeout_total = 0;
    std::uint64_t stream_keyframe_request_sent_total = 0;
    std::uint64_t stream_keyframe_request_received_total = 0;
    std::uint64_t stream_playback_starvation_sent_total = 0;
    std::uint64_t stream_playback_starvation_received_total = 0;
    std::uint64_t stream_static_keyframe_refresh_attempt_total = 0;
    std::uint64_t stream_static_keyframe_refresh_success_total = 0;
    std::atomic<std::uint64_t> stream_keyframe_refresh_request_generation{0};
    std::atomic<std::uint64_t> stream_keyframe_refresh_completed_generation{0};
    std::uint64_t last_stream_playback_starvation_frame_id = 0;
    std::uint64_t stream_last_playback_starvation_frame_id_received = 0;
    std::uint64_t stream_latest_received_frame_id = 0;
    std::uint64_t stream_latest_complete_frame_id = 0;
    std::uint64_t stream_latest_complete_keyframe_id = 0;
    std::uint64_t stream_observed_rate_revision = 0;
    std::mutex stream_transport_feedback_mutex;
    redclaw::net::MediaTransportFeedbackRecorder stream_transport_feedback_recorder;
    redclaw::net::MediaTransportEstimator stream_transport_estimator;
    redclaw::net::MediaCongestionController stream_congestion_controller;
    redclaw::net::DesktopMediaSendPacer stream_media_pacer;
    redclaw::net::MediaTransportEstimate stream_transport_estimate_snapshot;
    redclaw::net::MediaCongestionDecision stream_congestion_decision_snapshot;
    std::uint64_t stream_transport_feedback_sent_total = 0;
    std::uint64_t stream_transport_feedback_received_total = 0;
    std::uint64_t stream_transport_feedback_ignored_total = 0;
    std::uint64_t stream_transport_feedback_invalid_total = 0;
    redclaw::net::SentVideoFrameMetadataRing stream_sent_frame_metadata(
        kDesktopStreamSentMetadataCapacity);
    std::uint64_t stream_geometry_transaction_id = 0;
    std::uint64_t stream_geometry_revision = 1;
    std::uint64_t stream_viewport_request_total = 0;
    std::uint64_t stream_encoder_resolution_reconfigure_total = 0;
    std::string remote_log_follow_request_id;
    std::uint64_t remote_log_follow_cursor = 0;
    std::uint32_t remote_log_follow_chunk_index = 0;
    std::deque<redclaw::protocol::StreamControlMessageV1> pending_remote_log_messages;
    std::deque<redclaw::protocol::StreamControlMessageV1> pending_local_control_requests;
    std::optional<redclaw::protocol::StreamControlMessageV1> last_local_viewport_request;
    std::optional<redclaw::protocol::StreamControlMessageV1> last_local_capture_region_request;
    std::optional<redclaw::protocol::StreamControlMessageV1> last_local_log_follow_request;
    std::uint32_t local_viewport_reconnect_extra_sends = 0;
    std::string local_control_input_buffer;
    redclaw::agent::LocalAgentPipe local_agent_pipe;
    {
        const char* name = std::getenv("REDCLAW_AGENT_PIPE_NAME");
        const char* owner = std::getenv("REDCLAW_AGENT_PIPE_OWNER_PID");
        if (name && owner) {
            char* end = nullptr;
            const auto owner_pid = std::strtoul(owner, &end, 10);
            std::string pipe_error;
            if (end != owner && *end == '\0' && owner_pid != 0) {
                (void)local_agent_pipe.start(name, false, static_cast<std::uint32_t>(owner_pid), &pipe_error);
            }
        }
    }
    bool agent_rebuild_pending = false;
    bool agent_rebuild_awaiting_open = false;
    bool agent_unavailable_until_reconnect = false;
    std::uint32_t agent_rebuild_attempts = 0;
    std::uint64_t next_agent_rebuild_ms = 0;
    std::uint64_t agent_rebuild_open_deadline_ms = 0;
    std::uint64_t agent_channel_opened_at_ms = 0;
    std::uint64_t agent_channel_open_total = 0;
    std::uint64_t agent_channel_close_total = 0;
    std::uint64_t agent_channel_rebuild_attempt_total = 0;
    std::uint64_t agent_channel_rebuild_success_total = 0;
    std::uint64_t agent_qa_forced_channel_close_total = 0;
    bool agent_qa_forced_channel_close_attempted = false;
    redclaw::workspace::TransferOperationGate transfer_operation_gate;
    const auto workspace_mutations_allowed = [&transfer_operation_gate] { return !transfer_operation_gate.blocks_mutation(); };
    std::unique_ptr<redclaw::agent::AgentExecutor> remote_agent_broker;
    {
        std::filesystem::path agent_metadata_path;
        if (!options.agent_project_manifest.empty()) {
            agent_metadata_path = std::filesystem::path(options.agent_project_manifest)
                .parent_path() / "agent-task-metadata-v1.frames";
        }
        auto broker = std::make_unique<redclaw::agent::RemoteAgentBroker>(
            redclaw::agent::RemoteAgentBrokerConfig{
                .authorized = options.allow_remote_agent,
                .metadata_path = std::move(agent_metadata_path),
                .mutations_allowed = workspace_mutations_allowed,
            });
        if (!options.agent_project_manifest.empty()) {
            std::vector<redclaw::agent::AgentProjectRegistration> projects;
            std::string manifest_error;
            if (redclaw::agent::load_agent_project_manifest(
                    options.agent_project_manifest, &projects, &manifest_error)) {
                for (auto& project : projects) {
                    broker->add_project(std::move(project));
                }
            } else {
                broker->set_authorized(false);
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "agent",
                    "remote Agent authorization failed closed: " + manifest_error);
            }
        }
        if (options.agent_qa_fixture_provider) {
            broker->add_provider(redclaw::agent::make_debug_fixture_agent_provider());
        } else {
            broker->add_provider(redclaw::agent::make_codex_app_server_provider());
            broker->add_provider(redclaw::agent::make_cursor_agent_provider());
        }
        remote_agent_broker = std::make_unique<redclaw::agent::AgentExecutor>(std::move(broker));
    }
    redclaw::agent::AgentPeerSession agent_peer_session(std::move(remote_agent_broker),
        [&local_agent_pipe](const redclaw::protocol::AgentMessageEnvelopeV1& message, std::string* error) {
            return local_agent_pipe.send(message, error);
        }, workspace_mutations_allowed);
    redclaw::input::InputPolicyGate remote_input_policy_gate;
    redclaw::input::WindowsSendInputInjectorBackend remote_input_backend;
    redclaw::runtime::InputQaReceipts input_qa_receipts(
        options.agent_qa_fixture_provider || options.input_diagnostics, options.log_dir);
    redclaw::input::RemoteInputSession remote_input_session(
        remote_input_policy_gate, remote_input_backend);
    std::uint64_t applied_transfer_input_revision = 0;
    if ((options.agent_qa_fixture_provider || options.input_diagnostics) && options.role == RuntimeRole::kHost)
        remote_input_session.set_injection_observer([&input_qa_receipts](const auto& receipt) { input_qa_receipts.record(receipt); });
    if (options.input_diagnostics && options.role == RuntimeRole::kHost)
        remote_input_backend.set_diagnostic_observer(
            [&input_qa_receipts](const auto& receipt) { input_qa_receipts.record_native(receipt); });
    remote_input_session.set_authorized(
        options.role == RuntimeRole::kHost && options.allow_remote_input);
    redclaw::input::DesktopGeometry remote_input_geometry;
    redclaw::input::DesktopGeometry last_advertised_input_geometry;
    std::deque<redclaw::protocol::StreamControlMessageV1> pending_remote_input_messages;
    redclaw::runtime::RuntimeLoopWake runtime_loop_wake;
    redclaw::workspace::TerminalRuntimeBridge terminal_bridge(
        options.role == RuntimeRole::kHost, stream_control_epoch, std::filesystem::current_path(),
        [&ice_wrapper](std::string_view bytes) {
            redclaw::net::DataChannelTransportStats stats;
            if (!ice_wrapper.getDataChannelTransportStats(redclaw::net::DataChannelKind::kTerminal, &stats)
                || stats.buffered_amount > 64U * 1024U) return false;
            return ice_wrapper.sendDataChannelBinaryMessage(redclaw::net::DataChannelKind::kTerminal,
                {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
        },
        [&ice_wrapper] { return ice_wrapper.ensureDataChannel(redclaw::net::DataChannelKind::kTerminal); },
        [] { return redclaw::capture::probe_capture_desktop(redclaw::capture::CaptureProbeDetail::kAccessOnly).access
            == redclaw::capture::CaptureDesktopAccess::kOrdinary; });
    terminal_bridge.connect_gui_from_environment();
    bool remote_input_queue_overflow = false;
    bool remote_input_disconnect_pending = false;
    bool controller_remote_input_supported = false;
    bool controller_remote_input_authorized = false;
    bool controller_input_congestion_notified = false;
    std::uint64_t remote_input_capabilities_total = 0;
    std::uint64_t remote_input_status_total = 0;
    const bool remote_diagnostics_allowed =
        redclaw::diag::remote_diagnostics_allowed_by_policy(
#if defined(NDEBUG)
            true,
#else
            false,
#endif
            options.allow_remote_diagnostics);
    std::uint64_t required_channel_closed_at_ms = 0;
    bool stream_required_channels_ready = false;
    std::atomic<bool> stream_media_submission_paused = true;
    enum class HostStreamWorkerStage : std::uint8_t {
        kInactive,
        kWaitingForChannel,
        kCaptureFrame,
        kPublishCapture,
        kWaitingForFrame,
        kWaitingForViewport,
        kWaitingForEncoder,
        kEncodeFrame,
        kSubmitFrame,
        kStopped,
    };
    std::atomic<HostStreamWorkerStage> stream_capture_worker_stage =
        HostStreamWorkerStage::kInactive;
    std::atomic<HostStreamWorkerStage> stream_encoder_worker_stage =
        HostStreamWorkerStage::kInactive;
    std::atomic<std::uint64_t> stream_capture_worker_stage_since_ms = now_steady_ms();
    std::atomic<std::uint64_t> stream_encoder_worker_stage_since_ms = now_steady_ms();
    auto set_stream_worker_stage = [](
                                       std::atomic<HostStreamWorkerStage>* target,
                                       std::atomic<std::uint64_t>* since_ms,
                                       HostStreamWorkerStage stage) {
        if (target->exchange(stage) != stage) {
            since_ms->store(now_steady_ms());
        }
    };
    auto stream_worker_stage_name = [](HostStreamWorkerStage stage) {
        switch (stage) {
        case HostStreamWorkerStage::kInactive: return "inactive";
        case HostStreamWorkerStage::kWaitingForChannel: return "waiting_channel";
        case HostStreamWorkerStage::kCaptureFrame: return "capture_frame";
        case HostStreamWorkerStage::kPublishCapture: return "publish_capture";
        case HostStreamWorkerStage::kWaitingForFrame: return "waiting_frame";
        case HostStreamWorkerStage::kWaitingForViewport: return "waiting_viewport";
        case HostStreamWorkerStage::kWaitingForEncoder: return "waiting_encoder";
        case HostStreamWorkerStage::kEncodeFrame: return "encode_frame";
        case HostStreamWorkerStage::kSubmitFrame: return "submit_frame";
        case HostStreamWorkerStage::kStopped: return "stopped";
        }
        return "unknown";
    };
    std::uint64_t stream_required_channels_ready_at_ms = 0;
    std::uint64_t stream_required_channels_open_total = 0;
    std::uint64_t stream_recovery_reset_total = 0;
    std::uint64_t stream_recovery_success_total = 0;
    std::uint64_t connected_failure_observed_at_ms = 0;
    redclaw::service::AutomaticReconnectSchedule reconnect_schedule;
    auto& reconnect_retry_after_ms = reconnect_schedule.retry_after_ms;
    auto& reconnect_attempt_deadline_ms = reconnect_schedule.deadline_ms;
    auto& reconnect_attempt_count = reconnect_schedule.attempt_count;
    auto& automatic_recovery_active = reconnect_schedule.active;
    auto& reconnect_attempt_in_flight = reconnect_schedule.in_flight;
    std::uint64_t next_stream_encode_ms = 0;
    std::uint64_t stream_pacer_geometry_revision = 0; // Encoder worker owned.
    std::optional<bool> stream_last_published_budget_wait; // Runtime loop owned.
    bool stream_capture_started = false;
    redclaw::capture::CaptureStreamGate stream_capture_gate;
    std::atomic<std::uint32_t> peer_capture_status_version{0};
    std::uint64_t applied_capture_input_pause_revision = 0;
    redclaw::capture::CaptureBackendTelemetry stream_capture_telemetry_snapshot;
    redclaw::capture::EncoderExecutionDiagnostics stream_encoder_diagnostics_snapshot;
    HostStreamStageTelemetry stream_host_stage_telemetry_snapshot;
    ControllerStreamStageTelemetry stream_controller_stage_telemetry_snapshot;
    StreamRttTelemetry stream_rtt_telemetry_snapshot;
    StreamAdaptiveControlState stream_adaptive_control_snapshot;
    std::mutex stream_source_activity_mutex;
    redclaw::session::DesktopSourceActivityTracker stream_source_activity_tracker;
    redclaw::session::HostStreamWorkCoordinator stream_work_coordinator;
    redclaw::session::ControllerDecoderRecoveryCoordinator
        stream_decoder_recovery_coordinator;
    redclaw::net::ReceiverDecodeCapacityController
        stream_receiver_decode_capacity_controller;
    std::uint64_t stream_latest_displayable_frame_id = 0;
    std::uint64_t stream_latest_displayable_keyframe_id = 0;
    std::uint64_t stream_latest_presented_frame_id = 0;
    std::uint64_t stream_gui_decoded_frames = 0;
    std::uint64_t stream_gui_rendered_frames = 0;
    std::uint64_t stream_gui_decoded_baseline = 0;
    std::uint64_t stream_gui_rendered_baseline = 0;
    std::uint64_t stream_gui_last_raw_decoded_frames = 0;
    std::uint64_t stream_gui_last_raw_rendered_frames = 0;
    bool stream_gui_stats_baseline_pending = false;
    std::uint64_t stream_observed_source_activity_revision = 0;
    stream_source_activity_tracker.reset(
        stream_geometry_revision,
        stream_adaptive_control_snapshot.rate_revision);
    std::uint64_t next_stream_rtt_ping_steady_ms = 0;
    redclaw::capture::WindowsCaptureSession stream_capture_session;
    std::mutex stream_capture_frame_mutex;
    std::shared_ptr<redclaw::capture::CapturedFrame> stream_latest_captured_frame;
    std::uint64_t stream_latest_captured_frame_sequence = 0;
    std::uint64_t stream_latest_captured_frame_ready_ms = 0;
    std::uint32_t stream_source_width_snapshot = 0;
    std::uint32_t stream_source_height_snapshot = 0;
    std::uint32_t stream_encoded_width_snapshot = 0;
    std::uint32_t stream_encoded_height_snapshot = 0;
    redclaw::capture::EncoderExecutionSession stream_encoder_session;
    std::mutex stream_encoder_mutex;
    bool stream_encoder_started = false;
    std::uint64_t stream_last_preserved_hardware_rate_revision = 0;
    std::uint64_t stream_last_preserved_hardware_geometry_transaction_id = 0;
    bool stream_capture_skip_cpu_readback = false;
    std::uint64_t stream_encoder_start_retry_after_ms = 0;
    std::uint32_t stream_encoder_width = 0;
    std::uint32_t stream_encoder_height = 0;
    std::uint64_t stream_last_encoded_capture_sequence = 0;
    std::uint64_t stream_video_frame_id = 0;
    std::uint64_t stream_last_applied_geometry_transaction_id = 0;
    redclaw::render::FfmpegVideoFrameDecoder stream_video_decoder;
    std::mutex stream_receive_mutex;
    std::uint32_t stream_decoder_width = 0;
    std::uint32_t stream_decoder_height = 0;
    redclaw::net::EncodedVideoFrameReassembler stream_video_reassembly;
#ifdef _WIN32
    DirectFramePipeClient direct_frame_pipe(options.stream_frame_pipe);
    DirectFramePipeClient navigation_frame_pipe(options.stream_navigation_pipe);
    std::uint64_t next_diagnostic_video_write_ms = 0;
    if (direct_frame_pipe.enabled()) {
        std::cout << "Runtime direct frame channel enabled role=" << role_name
                  << " channel=" << options.stream_frame_pipe << '\n';
    }
    if (navigation_frame_pipe.enabled()) {
        std::cout << "Runtime navigation frame channel enabled role=" << role_name
                  << " channel=" << options.stream_navigation_pipe << '\n';
    }
#endif

#ifdef _WIN32
    ScopedWinsock winsock;
    ScopedSocket tcp_listener;
    ScopedSocket tcp_peer;
    std::string tcp_receive_buffer;
    TcpSignalDiagnostics tcp_diagnostics;
    std::uint64_t tcp_next_connect_attempt_ms = 0;
    std::uint32_t tcp_connect_backoff_ms = 250;
    constexpr std::uint32_t kTcpConnectBackoffMaxMs = 4000;

    auto record_tcp_failure = [&](TcpSignalFailureCategory category, int wsa_error, std::string detail) {
        tcp_diagnostics.last_failure = category;
        tcp_diagnostics.last_wsa_error = wsa_error;
        tcp_diagnostics.last_failure_detail = std::move(detail);
    };
#endif

    if (use_tcp_transport) {
#ifdef _WIN32
        if (!initialize_winsock(&winsock, &runtime_error)) {
            std::cerr << "Runtime TCP signaling init failed: " << runtime_error << '\n';
            return 1;
        }

        if (options.role == RuntimeRole::kHost) {
            if (!create_tcp_listener(tcp_signaling_port, &tcp_listener, &runtime_error)) {
                std::cerr << "Runtime TCP listener setup failed: " << runtime_error << '\n';
                return 1;
            }
        }
#else
        std::cerr << "Runtime TCP signaling transport is unsupported on this platform" << '\n';
        return 1;
#endif
    }

    ice_wrapper.onConnectionStateChanged([&](redclaw::net::IceConnectionState state) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        connection_state = state;
        if (use_dht_transport && state == redclaw::net::IceConnectionState::kConnecting) {
            (void)dht_negotiation.mark_ice_connecting();
        }
        if (state == redclaw::net::IceConnectionState::kConnected) {
            saw_connected_state = true;
            saw_failed_state = false;
            automatic_recovery_active = false;
            reconnect_attempt_in_flight = false;
            reconnect_attempt_count = 0;
            reconnect_retry_after_ms = 0;
            reconnect_attempt_deadline_ms = 0;
            connected_failure_observed_at_ms = 0;
            if (stream_media_channel_open && stream_control_channel_open) {
                required_channel_closed_at_ms = 0;
                stream_media_submission_paused.store(false);
                stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
                if (!stream_required_channels_ready) {
                    stream_required_channels_ready = true;
                    stream_required_channels_ready_at_ms = now_steady_ms();
                    ++stream_required_channels_open_total;
                    if (stream_recovery_reset_total > 0) {
                        ++stream_recovery_success_total;
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kInfo,
                            "recovery",
                            "required media/control channels reopened after session reset");
                    }
                }
            }
            if (use_dht_transport) {
                (void)dht_negotiation.mark_connected();
            }
        }
        if ((state == redclaw::net::IceConnectionState::kDisconnected
                || state == redclaw::net::IceConnectionState::kClosed)
            && (saw_connected_state || use_dht_transport)) {
            saw_failed_state = true;
            remote_input_disconnect_pending = true;
            runtime_loop_wake.notify();
            if (use_dht_transport) {
                (void)dht_negotiation.mark_failed();
            }
            if (saw_connected_state && connected_failure_observed_at_ms == 0) {
                connected_failure_observed_at_ms = now_steady_ms();
            }
        }
        if (state == redclaw::net::IceConnectionState::kFailed) {
            saw_failed_state = true;
            remote_input_disconnect_pending = true;
            runtime_loop_wake.notify();
            if (use_dht_transport) {
                (void)dht_negotiation.mark_failed();
            }
        }
    });

    ice_wrapper.onGatheringStateChanged([&](redclaw::net::IceGatheringState state) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        local_candidate_gathering_complete = state == redclaw::net::IceGatheringState::kComplete;
        if (local_candidate_gathering_complete
            && (use_dht_transport || use_rendezvous_transport)
            && !local_description_sdp.empty()) {
            signal_snapshot_dirty = true;
        }
    });

    std::mutex stream_control_envelope_mutex;
    auto stamp_control_message = [&](redclaw::protocol::StreamControlMessageV1* message) {
        std::lock_guard<std::mutex> lock(stream_control_envelope_mutex);
        message->session_epoch = stream_control_epoch;
        message->message_id = ++stream_control_message_id;
        message->sent_at_ms = now_unix_ms();
    };
    auto make_control_message = [&](redclaw::protocol::StreamControlMessageTypeV1 type) {
        redclaw::protocol::StreamControlMessageV1 message;
        message.type = type;
        stamp_control_message(&message);
        return message;
    };

    auto send_control_message = [&](redclaw::protocol::StreamControlMessageV1 message,
                                    std::string* error_detail) {
        input_qa_receipts.command(redclaw::runtime::InputQaStage::kSendBegin, message);
        std::lock_guard<std::mutex> envelope_lock(stream_control_envelope_mutex);
        input_qa_receipts.command(redclaw::runtime::InputQaStage::kSendLocked, message);
        if (message.session_epoch != stream_control_epoch) {
            if (error_detail != nullptr) {
                *error_detail = "stream control message belongs to a stale local epoch";
            }
            return false;
        }
        // Message ids describe actual reliable-channel send order, not creation
        // order. Control messages are created and queued from several threads;
        // assigning an id before taking the send lock allowed N+1 to reach the
        // peer before N and made the replay guard reject valid recovery/input.
        message.message_id = ++stream_control_message_id;
        message.sent_at_ms = now_unix_ms();
        std::string validation_error;
        if (!redclaw::protocol::validate_stream_control_message_v1(message, &validation_error)) {
            if (error_detail != nullptr) {
                *error_detail = validation_error;
            }
            return false;
        }
        const std::string serialized = redclaw::protocol::serialize_stream_control_message_v1(message);
        if (serialized.empty()) {
            if (error_detail != nullptr) {
                *error_detail = "stream control serialization failed";
            }
            return false;
        }
        input_qa_receipts.command(redclaw::runtime::InputQaStage::kSendEncoded, message);
        const bool sent = ice_wrapper.sendDataChannelBinaryMessage(
            redclaw::net::DataChannelKind::kControl,
            {reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size()},
            error_detail);
        input_qa_receipts.command(redclaw::runtime::InputQaStage::kSendEnd, message);
        return sent;
    };

    redclaw::workspace::TransferRuntimeBridge transfer_bridge(
        options.role == RuntimeRole::kHost, stream_control_epoch, std::filesystem::temp_directory_path(),
        transfer_operation_gate,
        [&](const auto& message) { std::string error; return send_control_message(message, &error); },
        [&](auto message) { stamp_control_message(&message); return local_control_output.send(message); },
        [&ice_wrapper](std::string_view bytes) {
            redclaw::net::DataChannelTransportStats stats;
            if (!ice_wrapper.getDataChannelTransportStats(redclaw::net::DataChannelKind::kTransfer, &stats)
                || stats.buffered_amount > 1024U * 1024U) return false;
            return ice_wrapper.sendDataChannelBinaryMessage(redclaw::net::DataChannelKind::kTransfer,
                {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
        },
        [&ice_wrapper] { return ice_wrapper.ensureDataChannel(redclaw::net::DataChannelKind::kTransfer); },
        {
            [&] {
                remote_input_session.set_transfer_blocked(transfer_operation_gate.blocks_mutation(), now_unix_ms());
                const bool ordinary = redclaw::capture::probe_capture_desktop(redclaw::capture::CaptureProbeDetail::kAccessOnly).access
                    == redclaw::capture::CaptureDesktopAccess::kOrdinary;
                const auto capture = stream_capture_gate.snapshot();
                const bool visible = capture.availability == redclaw::capture::CaptureAvailability::kRunning
                    && (peer_capture_status_version.load() == 0 || capture.presented)
                    && capture.input_pause_revision == applied_capture_input_pause_revision;
                bool channel_valid = false;
                {
                    std::lock_guard lock(callback_mutex);
                    channel_valid = stream_required_channels_ready && stream_control_channel_open
                        && !remote_input_disconnect_pending && !remote_input_queue_overflow
                        && std::none_of(pending_remote_input_messages.begin(), pending_remote_input_messages.end(), [](const auto& message) {
                            return message.type == redclaw::protocol::StreamControlMessageTypeV1::kInputReleaseAll
                                || (message.type == redclaw::protocol::StreamControlMessageTypeV1::kInputControlRequest && !message.input_requested_active);
                        });
                }
                return redclaw::workspace::ClipboardInputEligibility{
                    ordinary && visible && channel_valid && remote_input_session.clipboard_paste_eligible(), remote_input_session.eligibility_revision()};
            },
            [&](std::uint64_t revision, std::string* error) { return remote_input_session.paste_verified_clipboard(revision, error); }
        });

    auto make_source_activity_message = [&](
                                            const redclaw::session::DesktopSourceActivitySnapshot& snapshot) {
        auto message = make_control_message(
            redclaw::protocol::StreamControlMessageTypeV1::kSourceActivityState);
        message.source_activity_revision = snapshot.revision;
        switch (snapshot.state) {
        case redclaw::session::DesktopSourceActivityState::kUnknown:
            message.source_activity_state =
                redclaw::protocol::DesktopSourceActivityStateV1::kUnknown;
            break;
        case redclaw::session::DesktopSourceActivityState::kActive:
            message.source_activity_state =
                redclaw::protocol::DesktopSourceActivityStateV1::kActive;
            break;
        case redclaw::session::DesktopSourceActivityState::kStaticPending:
            message.source_activity_state =
                redclaw::protocol::DesktopSourceActivityStateV1::kStaticPending;
            break;
        case redclaw::session::DesktopSourceActivityState::kStatic:
            message.source_activity_state =
                redclaw::protocol::DesktopSourceActivityStateV1::kStatic;
            break;
        }
        message.reference_frame_id = snapshot.reference_frame_id;
        message.reference_keyframe_id = snapshot.reference_keyframe_id;
        message.stream_geometry_revision = snapshot.stream_geometry_revision;
        message.rate_revision = snapshot.rate_revision;
        message.media_budget_waiting = stream_media_pacer.telemetry().rejected_wire_bytes != 0;
        if (peer_capture_status_version.load() >= 1) {
            const auto capture = stream_capture_gate.snapshot();
            message.capture_status_version = 1;
            message.source_activity_revision = std::max<std::uint64_t>(1, message.source_activity_revision);
            message.stream_geometry_revision = std::max<std::uint64_t>(1, message.stream_geometry_revision);
            message.rate_revision = std::max<std::uint64_t>(1, message.rate_revision);
            message.capture_generation = capture.generation;
            message.capture_first_frame_id = capture.first_frame_id;
            message.capture_status = capture.availability == redclaw::capture::CaptureAvailability::kRunning ? 1U
                : capture.availability == redclaw::capture::CaptureAvailability::kPaused ? 3U : 2U;
        }
        return message;
    };

    auto publish_source_activity = [&](
                                      const redclaw::session::DesktopSourceActivitySnapshot& snapshot) {
        bool control_open = false;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            control_open = stream_control_channel_open;
        }
        if (!control_open || (snapshot.revision == 0 && peer_capture_status_version.load() == 0)
            || (snapshot.state == redclaw::session::DesktopSourceActivityState::kStaticPending
                && (snapshot.reference_frame_id == 0
                    || snapshot.reference_keyframe_id == 0))) {
            return false;
        }
        std::string send_error;
        return send_control_message(make_source_activity_message(snapshot), &send_error);
    };

    auto apply_source_activity_update = [&](
                                         const redclaw::session::DesktopSourceActivityUpdate& update) {
        if (update.request_keyframe) {
            stream_encoder_session.request_keyframe();
            stream_keyframe_refresh_request_generation.fetch_add(1);
            stream_work_coordinator.post(
                update.request_reference_frame
                    ? redclaw::session::HostStreamWorkReason::kSourceReference
                    : redclaw::session::HostStreamWorkReason::kKeyframe);
            stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
        }
        if (update.state_changed) {
            stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
            (void)publish_source_activity(update.snapshot);
        }
    };

    if (options.role == RuntimeRole::kHost) {
        std::string display_error;
        stream_capture_displays = redclaw::capture::enumerate_capture_displays(&display_error);
        if (!stream_capture_displays.empty()) {
            const auto primary = std::find_if(
                stream_capture_displays.begin(),
                stream_capture_displays.end(),
                [](const redclaw::capture::CaptureDisplayDescriptor& display) {
                    return display.primary;
                });
            stream_selected_display = primary != stream_capture_displays.end()
                ? *primary
                : stream_capture_displays.front();
            stream_active_capture_region = redclaw::capture::normalize_capture_region(
                {},
                stream_selected_display->pixel_width,
                stream_selected_display->pixel_height);
        } else {
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kWarning,
                "capture",
                "desktop display catalog unavailable: " + display_error);
        }
    }

    auto send_desktop_display_catalog = [&]() {
        if (options.role != RuntimeRole::kHost || stream_capture_displays.empty()) {
            return;
        }
        auto catalog = make_control_message(
            redclaw::protocol::StreamControlMessageTypeV1::kDesktopDisplayCatalog);
        catalog.display_catalog_revision = stream_navigation_catalog_revision;
        catalog.desktop_displays.reserve(stream_capture_displays.size());
        for (const auto& display : stream_capture_displays) {
            catalog.desktop_displays.push_back({
                .display_id = display.id,
                .display_name = display.name,
                .desktop_origin_x = display.desktop_origin_x,
                .desktop_origin_y = display.desktop_origin_y,
                .pixel_width = display.pixel_width,
                .pixel_height = display.pixel_height,
                .rotation = display.rotation,
                .primary = display.primary,
            });
        }
        std::string catalog_error;
        if (!send_control_message(catalog, &catalog_error)) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kWarning,
                "capture",
                "desktop display catalog send failed: " + catalog_error);
        }
    };

    const bool media_pacer_started = stream_media_pacer.start(
        [&](std::span<const std::uint8_t> packet) {
            redclaw::net::DataChannelSendOutcome outcome;
            std::string send_error;
            const bool accepted = ice_wrapper.sendDataChannelBinaryMessage(
                redclaw::net::DataChannelKind::kMedia,
                packet,
                &send_error,
                &outcome);
            return redclaw::net::MediaPacerSendResult{
                .accepted = accepted,
                .queued = accepted
                    && outcome.disposition
                        == redclaw::net::DataChannelSendDisposition::kAcceptedQueued,
                .buffered_amount = outcome.buffered_amount,
            };
        },
        [&]() {
            redclaw::net::DataChannelTransportStats transport_stats;
            std::string transport_error;
            const bool available = ice_wrapper.getDataChannelTransportStats(
                redclaw::net::DataChannelKind::kMedia,
                &transport_stats,
                &transport_error);
            if (available) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                stream_data_channel_transport_stats_snapshot = transport_stats;
            }
            return redclaw::net::MediaPacerTransportState{
                .open = available && transport_stats.open,
                .buffered_amount = transport_stats.buffered_amount,
            };
        },
        [&](const redclaw::net::SentMediaTransportPacket& packet) {
            (void)stream_transport_estimator.record_sent(packet);
            std::lock_guard lock(callback_mutex);
            ++stream_media_fragments_sent;
            if (packet.first_packet) {
                (void)stream_sent_frame_metadata.record({
                    .frame_id = packet.frame_id,
                    .steady_send_time_ms = packet.steady_send_us / 1000,
                    .rate_revision = packet.rate_revision,
                    .fps = packet.target_fps,
                    .bitrate_kbps = packet.target_bitrate_kbps,
                    .keyframe = packet.keyframe,
                });
            }
        },
        [&](const redclaw::net::MediaPacerFrameEvent& event) {
            if (event.type == redclaw::net::MediaPacerFrameEventType::kSent) {
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    stream_host_stage_telemetry_snapshot.total_send_us += event.elapsed_us;
                    ++stream_counters.transmitted_frames;
                    stream_counters.last_error.clear();
                    (void)stream_sent_frame_metadata.finish(event.frame_id,
                        redclaw::net::SentVideoFrameState::kCompleted, event.sent_fragments);
                    if (event.keyframe && event.keyframe_refresh_generation != 0) {
                        ++stream_static_keyframe_refresh_success_total;
                    }
                }
                if (event.keyframe && event.keyframe_refresh_generation != 0) {
                    std::uint64_t completed =
                        stream_keyframe_refresh_completed_generation.load();
                    while (completed < event.keyframe_refresh_generation
                           && !stream_keyframe_refresh_completed_generation
                                   .compare_exchange_weak(
                                       completed,
                                       event.keyframe_refresh_generation)) {
                    }
                }
                stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
                return;
            }

            if (event.type == redclaw::net::MediaPacerFrameEventType::kDropped) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                (void)stream_sent_frame_metadata.finish(event.frame_id,
                    redclaw::net::SentVideoFrameState::kDropped, event.sent_fragments);
                if (event.reason == "pacer_frame_budget_infeasible"
                    || event.reason == "pacer_token_deadline"
                    || event.reason == "recovery_probe_unconfirmed") {
                    ++stream_counters.transmit_local_pacing_drops;
                } else if (event.reason == "pacer_in_flight_deadline"
                           || event.reason == "data_channel_buffer_deadline"
                           || event.reason == "media_channel_unavailable_deadline") {
                    ++stream_counters.transmit_backpressure_drops;
                } else {
                    ++stream_counters.transmit_failures;
                }
                stream_counters.last_error = "media pacer dropped frame_id="
                    + std::to_string(event.frame_id)
                    + " reason=" + event.reason
                    + " wire_bytes=" + std::to_string(event.wire_bytes)
                    + " pacing_kbps=" + std::to_string(event.pacing_bitrate_kbps)
                    + " pacing_ms=" + std::to_string(event.pacing_duration_ms)
                    + " deadline_ms=" + std::to_string(event.deadline_ms)
                    + " elapsed_ms=" + std::to_string(event.elapsed_us / 1000ULL)
                    + " fragments=" + std::to_string(event.sent_fragments)
                    + "/" + std::to_string(event.fragment_count);
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream",
                    stream_counters.last_error);
                return;
            }

            stream_encoder_session.request_keyframe();
            stream_keyframe_refresh_request_generation.fetch_add(1);
            stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
        },
        [&]() {
            stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kTransportWritable);
        });
    if (!media_pacer_started) {
        std::cerr << "Runtime desktop media pacer failed to start" << '\n';
        return 1;
    }
    struct MediaPacerScope final {
        redclaw::net::DesktopMediaSendPacer* pacer = nullptr;
        ~MediaPacerScope() {
            if (pacer != nullptr) {
                pacer->stop();
            }
        }
    } media_pacer_scope{&stream_media_pacer};
    stream_media_pacer.update_budget(
        kDesktopStreamTransportBitrateCeilingKbps,
        0,
        0);
    ice_wrapper.onDataChannelWritable([&](redclaw::net::DataChannelKind kind) {
        if (kind == redclaw::net::DataChannelKind::kMedia) {
            stream_media_pacer.notify_writable();
            stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
        }
    });

    auto send_remote_keyframe_request = [&](std::string_view reason) {
        if (options.role != RuntimeRole::kController) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (!stream_control_channel_open) {
                return false;
            }
        }

        auto request = make_control_message(
            redclaw::protocol::StreamControlMessageTypeV1::kKeyframeRequest);
        request.payload = std::string(reason.substr(0, 128));
        std::string send_error;
        if (!send_control_message(request, &send_error)) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kWarning,
                "stream-control",
                "keyframe request send failed: " + send_error);
            return false;
        }

        std::lock_guard<std::mutex> lock(callback_mutex);
        ++stream_keyframe_request_sent_total;
        append_timeline(
            redclaw::render::RuntimeStatusSeverity::kInfo,
            "stream-control",
            "keyframe requested reason=" + request.payload);
        return true;
    };

    auto apply_decoder_recovery_update = [&]
        (const redclaw::session::ControllerDecoderRecoveryUpdate& update,
         std::string_view reason) {
        if (!update.request_keyframe) {
            return false;
        }
        return send_remote_keyframe_request(reason);
    };

    auto report_decoder_dependency_break = [&]
        (redclaw::session::ControllerDecoderBreakReason reason,
         std::uint64_t frame_id,
         std::string_view reason_name) {
        std::uint32_t srtt_ms = 0;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            srtt_ms = stream_rtt_telemetry_snapshot.smoothed_rtt_ms;
        }
        const auto update = stream_decoder_recovery_coordinator.on_dependency_break(
            reason,
            frame_id,
            now_steady_ms(),
            srtt_ms);
        return apply_decoder_recovery_update(update, reason_name);
    };

    auto send_incomplete_media_feedback = [&](const PendingIncompleteMediaFeedback& feedback) {
        auto pressure = make_control_message(
            redclaw::protocol::StreamControlMessageTypeV1::kPlaybackStarvation);
        pressure.playback_starvation_count = feedback.report_count;
        pressure.incomplete_frame_id = feedback.incomplete_frame_id;
        pressure.latest_received_frame_id = feedback.latest_received_frame_id;
        pressure.latest_complete_frame_id = feedback.latest_complete_frame_id;
        pressure.latest_complete_keyframe_id = feedback.latest_complete_keyframe_id;
        pressure.observed_rate_revision = feedback.observed_rate_revision;
        pressure.payload = feedback.reason;
        std::string send_error;
        if (!send_control_message(pressure, &send_error)) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kWarning,
                "stream-control",
                "incomplete media frame feedback failed frame_id="
                    + std::to_string(feedback.incomplete_frame_id)
                    + " error=" + send_error);
            return false;
        }

        std::lock_guard<std::mutex> lock(callback_mutex);
        append_timeline(
            redclaw::render::RuntimeStatusSeverity::kWarning,
            "stream-control",
            "incomplete media frame reported frame_id="
                + std::to_string(feedback.incomplete_frame_id)
                + " reason=" + feedback.reason
                + " qa_feedback_class=" + options.stream_qa_incomplete_feedback_class);
        return true;
    };

    auto report_incomplete_media_frame = [&](std::uint64_t frame_id, std::string_view reason) {
        if (options.role != RuntimeRole::kController || frame_id == 0) {
            return false;
        }

        PendingIncompleteMediaFeedback feedback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (!stream_control_channel_open
                || frame_id <= last_stream_playback_starvation_frame_id) {
                return false;
            }
            last_stream_playback_starvation_frame_id = frame_id;
            feedback.report_count = ++stream_playback_starvation_sent_total;
            feedback.incomplete_frame_id = frame_id;
            feedback.latest_received_frame_id = stream_latest_received_frame_id;
            feedback.latest_complete_frame_id = stream_latest_complete_frame_id;
            feedback.latest_complete_keyframe_id = stream_latest_complete_keyframe_id;
            feedback.observed_rate_revision = stream_observed_rate_revision;
            feedback.reason = std::string(reason.substr(0, 128));
        }

        if (options.stream_qa_drop_one_media_fragment
            && options.stream_qa_incomplete_feedback_class == "stale-revision") {
            feedback.observed_rate_revision = feedback.observed_rate_revision
                    == (std::numeric_limits<std::uint64_t>::max)()
                ? feedback.observed_rate_revision - 1
                : feedback.observed_rate_revision + 1;
        }

        std::uint64_t qa_delay_ms = 0;
        if (options.stream_qa_drop_one_media_fragment
            && options.stream_qa_incomplete_feedback_class == "delayed") {
            qa_delay_ms = 400;
        } else if (options.stream_qa_drop_one_media_fragment
                   && options.stream_qa_incomplete_feedback_class == "expired") {
            qa_delay_ms = 1500;
        }
        if (qa_delay_ms == 0) {
            return send_incomplete_media_feedback(feedback);
        }

        feedback.due_steady_ms = now_steady_ms() + qa_delay_ms;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (stream_qa_pending_incomplete_feedback.has_value()) {
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream-qa",
                    "additional delayed incomplete-frame feedback rejected frame_id="
                        + std::to_string(frame_id));
                return false;
            }
            stream_qa_pending_incomplete_feedback = feedback;
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kInfo,
                "stream-qa",
                "incomplete media frame feedback scheduled frame_id="
                    + std::to_string(frame_id)
                    + " delay_ms=" + std::to_string(qa_delay_ms)
                    + " expected_class=" + options.stream_qa_incomplete_feedback_class);
        }
        return true;
    };

    auto input_control_state = [&]() {
        using State = redclaw::input::RemoteInputSessionState;
        switch (remote_input_session.state()) {
        case State::kAvailable: return redclaw::protocol::RemoteInputControlStateV1::kAvailable;
        case State::kActive: return redclaw::protocol::RemoteInputControlStateV1::kActive;
        case State::kPaused: return redclaw::protocol::RemoteInputControlStateV1::kPaused;
        case State::kDenied: return redclaw::protocol::RemoteInputControlStateV1::kDenied;
        case State::kUnavailable: return redclaw::protocol::RemoteInputControlStateV1::kUnavailable;
        }
        return redclaw::protocol::RemoteInputControlStateV1::kUnavailable;
    };

    auto input_status_reason = [&]() {
        using Reason = redclaw::input::RemoteInputPauseReason;
        switch (remote_input_session.pause_reason()) {
        case Reason::kNone: return redclaw::protocol::RemoteInputStatusReasonV1::kNone;
        case Reason::kNotAuthorized: return redclaw::protocol::RemoteInputStatusReasonV1::kNotAuthorized;
        case Reason::kNoVideo: return redclaw::protocol::RemoteInputStatusReasonV1::kNoVideo;
        case Reason::kLocalPause: return redclaw::protocol::RemoteInputStatusReasonV1::kLocalPause;
        case Reason::kLeaseExpired: return redclaw::protocol::RemoteInputStatusReasonV1::kLeaseExpired;
        case Reason::kQueueOverflow: return redclaw::protocol::RemoteInputStatusReasonV1::kQueueOverflow;
        case Reason::kInjectionFailed: return redclaw::protocol::RemoteInputStatusReasonV1::kInjectionFailed;
        case Reason::kDisconnected: return redclaw::protocol::RemoteInputStatusReasonV1::kDisconnected;
        case Reason::kGeometryChanged: return redclaw::protocol::RemoteInputStatusReasonV1::kGeometryChanged;
        case Reason::kStaleSequence: return redclaw::protocol::RemoteInputStatusReasonV1::kStaleSequence;
        }
        return redclaw::protocol::RemoteInputStatusReasonV1::kNone;
    };

    auto send_input_status = [&]() {
        if (options.role != RuntimeRole::kHost) {
            return;
        }
        auto status = make_control_message(
            redclaw::protocol::StreamControlMessageTypeV1::kInputControlStatus);
        status.input_supported = true;
        status.input_authorized = remote_input_session.authorized();
        status.input_state = input_control_state();
        status.input_reason = input_status_reason();
        status.input_sequence = remote_input_session.stats().last_applied_sequence;
        std::string send_error;
        if (send_control_message(status, &send_error)) {
            ++remote_input_status_total;
        }
    };

    auto send_input_capabilities = [&]() {
        redclaw::input::DesktopGeometry geometry;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            geometry = remote_input_geometry;
        }
        if (options.role != RuntimeRole::kHost
            || !redclaw::input::is_valid_desktop_geometry(geometry)) {
            return false;
        }
        auto capabilities = make_control_message(
            redclaw::protocol::StreamControlMessageTypeV1::kInputCapabilities);
        capabilities.input_supported = true;
        capabilities.input_authorized = remote_input_session.authorized();
        capabilities.input_state = input_control_state();
        capabilities.input_reason = input_status_reason();
        capabilities.desktop_origin_x = geometry.origin_x;
        capabilities.desktop_origin_y = geometry.origin_y;
        capabilities.desktop_width = geometry.width;
        capabilities.desktop_height = geometry.height;
        capabilities.desktop_rotation = geometry.rotation;
        capabilities.desktop_geometry_revision = geometry.revision;
        std::string send_error;
        if (!send_control_message(capabilities, &send_error)) {
            return false;
        }
        last_advertised_input_geometry = geometry;
        ++remote_input_capabilities_total;
        return true;
    };

    auto map_remote_input_event = [&](const redclaw::protocol::RemoteInputEventV1& remote,
                                      const redclaw::input::DesktopGeometry& geometry,
                                      std::vector<redclaw::input::InputEvent>* mapped) {
        redclaw::input::DesktopPoint point;
        const bool needs_position = remote.type != redclaw::protocol::RemoteInputEventTypeV1::kKeyDown
            && remote.type != redclaw::protocol::RemoteInputEventTypeV1::kKeyUp;
        redclaw::capture::CaptureRegion active_region;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            active_region = stream_active_capture_region;
        }
        if (needs_position
            && !redclaw::input::map_normalized_capture_region_point(
                remote.normalized_x,
                remote.normalized_y,
                geometry,
                redclaw::input::DesktopCaptureRegion{
                    .x = active_region.x,
                    .y = active_region.y,
                    .width = active_region.width,
                    .height = active_region.height,
                },
                &point)) {
            return false;
        }
        auto append_position = [&]() {
            redclaw::input::InputEvent move;
            move.type = redclaw::input::InputEventType::kMouseMove;
            move.x = point.x;
            move.y = point.y;
            move.absolute_coordinates = true;
            mapped->push_back(move);
        };
        redclaw::input::InputEvent event;
        event.scan_code = remote.scan_code;
        event.virtual_key = remote.virtual_key;
        event.extended = remote.extended;
        event.repeat = remote.repeat;
        event.x = point.x;
        event.y = point.y;
        event.absolute_coordinates = needs_position;
        switch (remote.mouse_button) {
        case redclaw::protocol::RemoteInputMouseButtonV1::kLeft:
            event.mouse_button = redclaw::input::MouseButton::kLeft; break;
        case redclaw::protocol::RemoteInputMouseButtonV1::kRight:
            event.mouse_button = redclaw::input::MouseButton::kRight; break;
        case redclaw::protocol::RemoteInputMouseButtonV1::kMiddle:
            event.mouse_button = redclaw::input::MouseButton::kMiddle; break;
        case redclaw::protocol::RemoteInputMouseButtonV1::kX1:
            event.mouse_button = redclaw::input::MouseButton::kX1; break;
        case redclaw::protocol::RemoteInputMouseButtonV1::kX2:
            event.mouse_button = redclaw::input::MouseButton::kX2; break;
        case redclaw::protocol::RemoteInputMouseButtonV1::kNone:
            break;
        }
        switch (remote.type) {
        case redclaw::protocol::RemoteInputEventTypeV1::kKeyDown:
            event.type = redclaw::input::InputEventType::kKeyDown;
            break;
        case redclaw::protocol::RemoteInputEventTypeV1::kKeyUp:
            event.type = redclaw::input::InputEventType::kKeyUp;
            break;
        case redclaw::protocol::RemoteInputEventTypeV1::kMouseMove:
            append_position();
            return true;
        case redclaw::protocol::RemoteInputEventTypeV1::kMouseButtonDown:
            append_position();
            event.type = redclaw::input::InputEventType::kMouseButtonDown;
            break;
        case redclaw::protocol::RemoteInputEventTypeV1::kMouseButtonUp:
            append_position();
            event.type = redclaw::input::InputEventType::kMouseButtonUp;
            break;
        case redclaw::protocol::RemoteInputEventTypeV1::kMouseWheel:
            append_position();
            event.type = redclaw::input::InputEventType::kMouseWheel;
            event.wheel_delta = remote.wheel_delta;
            break;
        case redclaw::protocol::RemoteInputEventTypeV1::kMouseHorizontalWheel:
            append_position();
            event.type = redclaw::input::InputEventType::kMouseHorizontalWheel;
            event.wheel_delta = remote.wheel_delta;
            break;
        }
        mapped->push_back(event);
        return true;
    };

    auto queue_remote_log_response = [&](const redclaw::protocol::StreamControlMessageV1& request) {
        constexpr std::size_t kSnapshotMaxLines = 50;
        constexpr std::size_t kSnapshotMaxBytes = 64U * 1024U;
        constexpr std::size_t kFollowMaxBytes = 64U * 1024U;
        constexpr std::size_t kMaxQueuedLogMessages = (kSnapshotMaxBytes / redclaw::protocol::kMaxRemoteLogChunkBytes) + 2U;

        if (!remote_diagnostics_allowed || process_logger == nullptr) {
            auto response = make_control_message(redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogError);
            response.request_id = request.request_id;
            response.payload = "remote diagnostics are disabled on this endpoint";
            std::lock_guard<std::mutex> lock(callback_mutex);
            pending_remote_log_messages.push_back(std::move(response));
            return;
        }
        if (request.log_mode == redclaw::protocol::RemoteLogModeV1::kStop) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (remote_log_follow_request_id == request.request_id) {
                remote_log_follow_request_id.clear();
                remote_log_follow_cursor = 0;
                remote_log_follow_chunk_index = 0;
            }
            return;
        }

        const bool follow = request.log_mode == redclaw::protocol::RemoteLogModeV1::kFollow;
        const auto snapshot = process_logger->snapshot(
            follow ? request.log_cursor : 0,
            kSnapshotMaxLines,
            follow ? kFollowMaxBytes : kSnapshotMaxBytes);
        std::vector<std::string> chunks;
        std::string chunk;
        chunk.reserve(redclaw::protocol::kMaxRemoteLogChunkBytes);
        for (const auto& line : snapshot.lines) {
            const std::string safe_line = redclaw::diag::redact_log_text(line);
            std::size_t offset = 0;
            while (offset < safe_line.size()) {
                const std::size_t available = redclaw::protocol::kMaxRemoteLogChunkBytes - chunk.size();
                const std::size_t copied = (std::min)(available, safe_line.size() - offset);
                chunk.append(safe_line, offset, copied);
                offset += copied;
                if (chunk.size() == redclaw::protocol::kMaxRemoteLogChunkBytes) {
                    chunks.push_back(std::move(chunk));
                    chunk.clear();
                    chunk.reserve(redclaw::protocol::kMaxRemoteLogChunkBytes);
                }
            }
        }
        if (!chunk.empty()) {
            chunks.push_back(std::move(chunk));
        }

        std::lock_guard<std::mutex> lock(callback_mutex);
        if (follow) {
            remote_log_follow_request_id = request.request_id;
            remote_log_follow_cursor = snapshot.next_cursor;
        }
        for (std::size_t index = 0;
             index < chunks.size() && pending_remote_log_messages.size() < kMaxQueuedLogMessages;
             ++index) {
            auto response = make_control_message(redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogChunk);
            response.request_id = request.request_id;
            response.chunk_index = follow ? remote_log_follow_chunk_index++ : static_cast<std::uint32_t>(index);
            response.log_cursor = snapshot.next_cursor;
            response.gap = snapshot.gap && index == 0;
            response.payload = std::move(chunks[index]);
            pending_remote_log_messages.push_back(std::move(response));
        }
        if (!follow && pending_remote_log_messages.size() < kMaxQueuedLogMessages) {
            auto complete = make_control_message(redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogComplete);
            complete.request_id = request.request_id;
            complete.log_cursor = snapshot.next_cursor;
            complete.complete = true;
            complete.gap = snapshot.gap;
            pending_remote_log_messages.push_back(std::move(complete));
        }
    };

    ice_wrapper.onDataChannelOpen([&](redclaw::net::DataChannelKind kind) {
        if (kind == redclaw::net::DataChannelKind::kTransfer) {
            transfer_bridge.channel_open(true); runtime_loop_wake.notify(); return;
        }
        if (kind == redclaw::net::DataChannelKind::kTerminal) {
            terminal_bridge.channel_open(true);
            runtime_loop_wake.notify();
            return;
        }
        redclaw::net::DataChannelTransportStats transport_stats;
        std::string transport_error;
        std::uint32_t open_pacing_kbps = kDesktopStreamTransportBitrateCeilingKbps;
        std::uint32_t open_rtt_ms = 0;
        ice_wrapper.getDataChannelTransportStats(kind, &transport_stats, &transport_error);
        if (kind == redclaw::net::DataChannelKind::kAgent) {
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                stream_agent_channel_open = true;
                ++agent_channel_open_total;
                agent_rebuild_pending = false;
                agent_rebuild_awaiting_open = false;
                agent_unavailable_until_reconnect = false;
                next_agent_rebuild_ms = 0;
                agent_rebuild_open_deadline_ms = 0;
                agent_channel_opened_at_ms = now_steady_ms();
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kInfo,
                    "agent",
                    "development Agent data channel opened");
            }
            agent_peer_session.open(stream_control_epoch);
            return;
        }
        if (kind == redclaw::net::DataChannelKind::kNavigation) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            stream_navigation_channel_open = true;
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kInfo,
                "navigation",
                "optional desktop navigation data channel opened");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (kind == redclaw::net::DataChannelKind::kMedia) {
                stream_media_channel_open = true;
                stream_data_channel_transport_stats_snapshot = transport_stats;
            } else if (kind == redclaw::net::DataChannelKind::kControl) {
                stream_control_channel_open = true;
            }
            open_pacing_kbps =
                stream_congestion_decision_snapshot.pacing_bitrate_kbps != 0
                    ? stream_congestion_decision_snapshot.pacing_bitrate_kbps
                    : kDesktopStreamTransportBitrateCeilingKbps;
            open_rtt_ms = stream_rtt_telemetry_snapshot.smoothed_rtt_ms;
            if (stream_media_channel_open && stream_control_channel_open) {
                required_channel_closed_at_ms = 0;
                stream_media_submission_paused.store(false);
                stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
                if (!stream_required_channels_ready) {
                    stream_required_channels_ready = true;
                    stream_required_channels_ready_at_ms = now_steady_ms();
                    ++stream_required_channels_open_total;
                    if (stream_recovery_reset_total > 0) {
                        ++stream_recovery_success_total;
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kInfo,
                            "recovery",
                            "required media/control channels reopened after session reset");
                    }
                }
            }
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kInfo,
                "stream",
                kind == redclaw::net::DataChannelKind::kMedia
                    ? "desktop media data channel opened"
                    : "desktop control data channel opened");
        }
        if (kind == redclaw::net::DataChannelKind::kMedia) {
            const auto estimate = stream_transport_estimator.snapshot(
                now_steady_ms() * 1000ULL,
                open_rtt_ms);
            stream_media_pacer.update_budget(
                open_pacing_kbps,
                open_rtt_ms,
                estimate.in_flight_bytes);
            const auto pacer_telemetry = stream_media_pacer.telemetry();
            std::string threshold_error;
            (void)ice_wrapper.setDataChannelBufferedAmountLowThreshold(
                redclaw::net::DataChannelKind::kMedia,
                std::max<std::size_t>(
                    kDesktopStreamVideoFragmentPacketBytes,
                    pacer_telemetry.buffered_limit_bytes / 2),
                &threshold_error);
            stream_media_pacer.notify_writable();
        }
        if (kind == redclaw::net::DataChannelKind::kControl) {
            auto hello = make_control_message(redclaw::protocol::StreamControlMessageTypeV1::kHello);
            hello.capture_status_version = 1;
            hello.terminal_version = redclaw::workspace::kTerminalCapabilityVersion;
            hello.file_transfer_version = redclaw::workspace::kFileTransferCapabilityVersion;
            hello.clipboard_version = redclaw::workspace::kClipboardCapabilityVersion;
            hello.payload = "viewport,source-activity,displayable-ack,network-stats,media-transport-feedback,playback-starvation,keyframe-recovery,remote-logs,reconnect,remote-input,capture-region,navigation";
            std::string send_error;
            if (!send_control_message(hello, &send_error)) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream",
                    "stream control hello send failed: " + send_error);
            }
            std::deque<redclaw::protocol::StreamControlMessageV1> pending;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                pending.swap(pending_local_control_requests);
            }
            while (!pending.empty()) {
                auto request = std::move(pending.front());
                pending.pop_front();
                stamp_control_message(&request);
                if (!send_control_message(request, &send_error)) {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    pending_local_control_requests.push_front(std::move(request));
                    while (!pending.empty()) {
                        pending_local_control_requests.push_back(std::move(pending.front()));
                        pending.pop_front();
                    }
                    break;
                }
            }
            if (options.role == RuntimeRole::kController) {
                stream_decoder_recovery_coordinator.reset();
                (void)report_decoder_dependency_break(
                    redclaw::session::ControllerDecoderBreakReason::kSessionStart,
                    0,
                    "control_channel_open");
            }
        }
    });

    ice_wrapper.onDataChannelClosed([&](redclaw::net::DataChannelKind kind) {
        if (kind == redclaw::net::DataChannelKind::kTransfer) {
            transfer_bridge.channel_open(false); runtime_loop_wake.notify(); return;
        }
        if (kind == redclaw::net::DataChannelKind::kTerminal) {
            terminal_bridge.channel_open(false);
            runtime_loop_wake.notify();
            return;
        }
        if (kind == redclaw::net::DataChannelKind::kAgent) {
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                stream_agent_channel_open = false;
                ++agent_channel_close_total;
                agent_rebuild_awaiting_open = false;
                agent_rebuild_open_deadline_ms = 0;
                agent_channel_opened_at_ms = 0;
                if (options.role == RuntimeRole::kHost
                    && agent_rebuild_attempts >= kAgentDataChannelRebuildMaxAttempts) {
                    agent_rebuild_pending = false;
                    agent_unavailable_until_reconnect = true;
                    next_agent_rebuild_ms = 0;
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "agent",
                        "Agent channel rebuild exhausted; desktop connection remains active");
                } else {
                    agent_rebuild_pending = options.role == RuntimeRole::kHost;
                    next_agent_rebuild_ms = now_steady_ms() + 1000;
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "agent",
                        "development Agent data channel closed; media_open="
                            + std::to_string(stream_media_channel_open)
                            + " control_open=" + std::to_string(stream_control_channel_open));
                }
            }
            agent_peer_session.close();
            return;
        }
        if (kind == redclaw::net::DataChannelKind::kNavigation) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            stream_navigation_channel_open = false;
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kWarning,
                "navigation",
                "optional desktop navigation data channel closed; desktop channels remain active");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (kind == redclaw::net::DataChannelKind::kMedia) {
                stream_media_channel_open = false;
            } else if (kind == redclaw::net::DataChannelKind::kControl) {
                stream_control_channel_open = false;
                stream_qa_pending_incomplete_feedback.reset();
            }
            stream_required_channels_ready = false;
            stream_media_submission_paused.store(true);
            stream_required_channels_ready_at_ms = 0;
            stream_rtt_telemetry_snapshot.in_flight_ping_sequence = 0;
            stream_rtt_telemetry_snapshot.in_flight_ping_sent_steady_ms = 0;
            next_stream_rtt_ping_steady_ms = 0;
            stream_adaptive_control_snapshot.encoder_reconfigure_pending = false;
            stream_adaptive_control_snapshot.rate_control_restart_fallback_revision = 0;
            remote_input_disconnect_pending = true;
            runtime_loop_wake.notify();
            if (saw_connected_state && required_channel_closed_at_ms == 0) {
                required_channel_closed_at_ms = now_steady_ms();
            }
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kWarning,
                "stream",
                kind == redclaw::net::DataChannelKind::kMedia
                    ? "desktop media data channel closed"
                    : "desktop control data channel closed");
        }
        stream_media_pacer.reset(true);
        stream_transport_estimator.reset();
        stream_congestion_controller.reset();
        stream_decoder_recovery_coordinator.reset();
        stream_receiver_decode_capacity_controller.reset();
        {
            std::lock_guard<std::mutex> feedback_lock(stream_transport_feedback_mutex);
            stream_transport_feedback_recorder.reset();
        }
        stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
    });

    auto handle_received_encoded_video = [&](const redclaw::render::EncodedVideoFrame& encoded_frame) {
        std::lock_guard<std::mutex> receive_lock(stream_receive_mutex);
        if (stream_decoder_width != 0
            && (stream_decoder_width != encoded_frame.width
                || stream_decoder_height != encoded_frame.height)) {
            stream_video_decoder.reset();
        }
        stream_decoder_width = encoded_frame.width;
        stream_decoder_height = encoded_frame.height;
        bool direct_pipe_written = false;
        bool direct_pipe_busy_drop = false;
        bool direct_pipe_write_failed = false;
        bool direct_pipe_enabled = false;
        std::string direct_pipe_error;
        bool should_write_diagnostic_ppm = true;
        bool direct_pipe_write_attempted = false;
        std::uint64_t direct_pipe_write_us = 0;
        bool decode_attempted = false;
        std::uint64_t decode_us = 0;
        DirectFramePipeTransportStats direct_pipe_transport_stats;

#ifdef _WIN32
        direct_pipe_enabled = direct_frame_pipe.enabled();
        if (direct_pipe_enabled) {
            direct_pipe_write_attempted = true;
            const auto direct_pipe_write_start = std::chrono::steady_clock::now();
            const DirectFramePipeWriteStatus direct_pipe_write_status =
                direct_frame_pipe.write_encoded_frame(encoded_frame, &direct_pipe_error);
            direct_pipe_written = direct_pipe_write_status == DirectFramePipeWriteStatus::kWritten;
            direct_pipe_busy_drop = direct_pipe_write_status == DirectFramePipeWriteStatus::kBusyDrop;
            direct_pipe_write_failed = !direct_pipe_written && !direct_pipe_busy_drop;
            direct_pipe_write_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - direct_pipe_write_start)
                    .count());
            direct_pipe_transport_stats = direct_frame_pipe.stats_snapshot();
            // When the GUI direct-frame path is active, avoid redundant decode
            // and diagnostic PPM writes on the runtime thread.
            should_write_diagnostic_ppm = false;
        } else {
            const std::uint64_t now_ms = now_steady_ms();
            should_write_diagnostic_ppm = now_ms >= next_diagnostic_video_write_ms;
            if (should_write_diagnostic_ppm) {
                next_diagnostic_video_write_ms = now_ms + kDiagnosticPreviewWriteIntervalMs;
            }
        }
#endif

        const auto update_controller_stage_telemetry = [&]() {
            if (options.role != RuntimeRole::kController) {
                return;
            }

            if (decode_us > 0 || decode_attempted) {
                stream_controller_stage_telemetry_snapshot.total_decode_us += decode_us;
                if (decode_attempted) {
                    ++stream_controller_stage_telemetry_snapshot.decode_attempt_count;
                }
            }

            if (!direct_pipe_write_attempted) {
                return;
            }

            stream_controller_stage_telemetry_snapshot.total_direct_pipe_write_us += direct_pipe_write_us;
            ++stream_controller_stage_telemetry_snapshot.direct_pipe_write_attempt_count;
            if (direct_pipe_written) {
                ++stream_controller_stage_telemetry_snapshot.direct_pipe_write_success_count;
                if (stream_qa_post_reconnect_frame_id != 0) {
                    ++stream_qa_post_reconnect_direct_pipe_written_total;
                }
            } else if (direct_pipe_write_failed) {
                ++stream_controller_stage_telemetry_snapshot.direct_pipe_write_failure_count;
            }

            stream_controller_stage_telemetry_snapshot.direct_pipe_transport_stats = direct_pipe_transport_stats;
            const std::uint64_t direct_pipe_backlog =
                direct_pipe_transport_stats.latest_sequence > direct_pipe_transport_stats.reader_sequence
                ? (direct_pipe_transport_stats.latest_sequence - direct_pipe_transport_stats.reader_sequence)
                : 0;
            stream_controller_stage_telemetry_snapshot.current_direct_pipe_backlog = direct_pipe_backlog;
            if (direct_pipe_backlog > stream_controller_stage_telemetry_snapshot.max_direct_pipe_backlog) {
                stream_controller_stage_telemetry_snapshot.max_direct_pipe_backlog = direct_pipe_backlog;
            }
        };

        if (direct_pipe_enabled) {
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                update_controller_stage_telemetry();
                ++stream_counters.received_frames;
                if (direct_pipe_write_failed) {
                    stream_counters.last_error = direct_pipe_error;
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "stream",
                        "encoded desktop video shared-memory delivery failed: " + direct_pipe_error);
                }
            }
            if (direct_pipe_written && encoded_frame.keyframe) {
                std::uint32_t srtt_ms = 0;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    srtt_ms = stream_rtt_telemetry_snapshot.smoothed_rtt_ms;
                }
                (void)stream_decoder_recovery_coordinator.on_keyframe_submitted(
                    encoded_frame.frame_id,
                    now_steady_ms(),
                    srtt_ms);
            } else if (direct_pipe_busy_drop) {
                (void)report_decoder_dependency_break(
                    redclaw::session::ControllerDecoderBreakReason::kDirectPipeBusy,
                    encoded_frame.frame_id,
                    "direct_frame_two_slot_busy");
            } else if (direct_pipe_write_failed) {
                (void)report_decoder_dependency_break(
                    redclaw::session::ControllerDecoderBreakReason::kDirectPipeFailure,
                    encoded_frame.frame_id,
                    "direct_frame_delivery_failure");
            }
            return;
        }

        redclaw::render::DecodedVideoFrame decoded_frame;
        bool decoded_ok = false;
        bool decoded_frame_ready = false;
        if (!direct_pipe_enabled) {
            std::string decode_error;
            decode_attempted = true;
            const auto decode_start = std::chrono::steady_clock::now();
            decoded_ok = stream_video_decoder.decode_frame_view(
                encoded_frame.codec,
                encoded_frame.width,
                encoded_frame.height,
                encoded_frame.timestamp_ms,
                encoded_frame.keyframe,
                encoded_frame.payload.data(),
                encoded_frame.payload.size(),
                &decoded_frame_ready,
                should_write_diagnostic_ppm ? &decoded_frame : nullptr,
                &decode_error);
            decode_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - decode_start)
                    .count());
            if (!decoded_ok) {
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    update_controller_stage_telemetry();
                    ++stream_counters.received_frames;
                    ++stream_counters.decode_failures;
                    stream_counters.last_error = decode_error;
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "stream",
                        "encoded desktop video decode failed: " + decode_error);
                }
                (void)report_decoder_dependency_break(
                    redclaw::session::ControllerDecoderBreakReason::kDecodeFailure,
                    encoded_frame.frame_id,
                    "runtime_decode_failure");
                return;
            }
        }

        std::string write_error;
        const bool diagnostic_written = (should_write_diagnostic_ppm && decoded_ok && decoded_frame_ready)
            ? write_decoded_frame_ppm(decoded_frame, stream_preview_output, &write_error)
            : false;
        const bool diagnostic_deferred = !direct_pipe_enabled && decoded_ok && !diagnostic_written;
        const bool rendered = diagnostic_written || diagnostic_deferred;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            update_controller_stage_telemetry();
            ++stream_counters.received_frames;
            ++stream_counters.decoded_frames;
            if (rendered) {
                ++stream_counters.rendered_frames;
                if (diagnostic_written) {
                    stream_counters.last_preview_path = stream_preview_output.string();
                }
            } else {
                ++stream_counters.render_failures;
                stream_counters.last_error = direct_pipe_enabled ? direct_pipe_error : write_error;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream",
                    "decoded desktop video render failed: " + stream_counters.last_error);
            }
        }

        if (!rendered) {
            std::cerr << "Runtime desktop stream video render failed: "
                      << (direct_pipe_enabled ? direct_pipe_error : write_error) << '\n';
        } else if (encoded_frame.keyframe) {
            std::uint32_t srtt_ms = 0;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                srtt_ms = stream_rtt_telemetry_snapshot.smoothed_rtt_ms;
            }
            (void)stream_decoder_recovery_coordinator.on_keyframe_submitted(
                encoded_frame.frame_id,
                now_steady_ms(),
                srtt_ms);
            (void)stream_decoder_recovery_coordinator.on_displayable_ack(
                encoded_frame.frame_id);
        }
    };

    auto flush_media_transport_feedback = [&](std::uint64_t receiver_steady_us) {
        std::optional<redclaw::net::MediaTransportFeedbackBatch> feedback;
        {
            std::lock_guard<std::mutex> feedback_lock(stream_transport_feedback_mutex);
            feedback = stream_transport_feedback_recorder.take_feedback(
                receiver_steady_us);
        }
        if (!feedback.has_value()) {
            return false;
        }

        auto message = make_control_message(
            redclaw::protocol::StreamControlMessageTypeV1::kMediaTransportFeedback);
        message.transport_feedback_id = feedback->feedback_id;
        message.observed_rate_revision = feedback->observed_rate_revision;
        message.media_transport_arrivals.reserve(feedback->arrivals.size());
        for (const auto& arrival : feedback->arrivals) {
            message.media_transport_arrivals.push_back({
                .transport_sequence = arrival.transport_sequence,
                .receiver_steady_us = arrival.receiver_steady_us,
            });
        }
        std::string send_error;
        if (!send_control_message(message, &send_error)) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            ++stream_transport_feedback_invalid_total;
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kWarning,
                "stream-control",
                "media transport feedback send failed: " + send_error);
            return false;
        }
        std::lock_guard<std::mutex> lock(callback_mutex);
        ++stream_transport_feedback_sent_total;
        return true;
    };

    const auto handle_compressed_message =
        [&](redclaw::net::DataChannelKind kind, std::string_view message) {
        if (kind == redclaw::net::DataChannelKind::kAgent) {
            const auto generation = agent_peer_session.snapshot().generation;
            const auto parsed = redclaw::protocol::parse_agent_message_v1(message);
            if (!parsed.ok) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "agent",
                    "invalid Agent message rejected: " + parsed.error);
                return;
            }
            std::string agent_error;
            if (!agent_peer_session.receive(parsed.value, generation, &agent_error)) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                append_timeline(redclaw::render::RuntimeStatusSeverity::kWarning,
                    "agent", "Agent message rejected: " + agent_error);
            }
            return;
        }
        if (kind == redclaw::net::DataChannelKind::kControl) {
            const auto received_monotonic_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            const auto parsed = redclaw::protocol::parse_stream_control_message_v1(message);
            if (!parsed.ok) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                if (parsed.value.type == redclaw::protocol::StreamControlMessageTypeV1::kMediaTransportFeedback) {
                    ++stream_transport_feedback_invalid_total;
                }
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream-control",
                    "invalid stream control message rejected: " + parsed.error);
                return;
            }
            const auto& control = parsed.value;
            input_qa_receipts.command(redclaw::runtime::InputQaStage::kPeerReceived, control, received_monotonic_us);
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                std::string guard_error;
                if (!stream_remote_control_guard.accept(control, &guard_error)) {
                    const auto type_name = redclaw::protocol::to_string(control.type);
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "stream-control",
                        "stream control message rejected type="
                            + std::string(type_name.data(), type_name.size())
                            + " message_id=" + std::to_string(control.message_id)
                            + ": " + guard_error);
                    return;
                }
            }

            if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kHello
                || control.type == redclaw::protocol::StreamControlMessageTypeV1::kCapabilities) {
                peer_capture_status_version.store(control.capture_status_version >= 1 ? 1U : 0U);
                terminal_bridge.peer_capability(control.terminal_version, control.session_epoch);
                transfer_bridge.peer_capability(control.file_transfer_version, control.session_epoch, control.clipboard_version);
            }
            if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kHello) {
                auto capabilities = make_control_message(
                    redclaw::protocol::StreamControlMessageTypeV1::kCapabilities);
                capabilities.capture_status_version = 1;
                capabilities.terminal_version = redclaw::workspace::kTerminalCapabilityVersion;
                capabilities.file_transfer_version = redclaw::workspace::kFileTransferCapabilityVersion;
                capabilities.clipboard_version = redclaw::workspace::kClipboardCapabilityVersion;
                capabilities.payload = remote_diagnostics_allowed
                    ? "viewport,capture-region,navigation,network-stats,media-transport-feedback,playback-starvation,keyframe-recovery,remote-logs,reconnect,remote-input"
                    : "viewport,capture-region,navigation,network-stats,media-transport-feedback,playback-starvation,keyframe-recovery,reconnect,remote-input";
                std::string send_error;
                (void)send_control_message(capabilities, &send_error);
                send_desktop_display_catalog();
                return;
            }
            if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kWorkspace) {
                (void)transfer_bridge.receive_control(control); runtime_loop_wake.notify(); return;
            }
            if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kPing) {
                auto pong = make_control_message(redclaw::protocol::StreamControlMessageTypeV1::kPong);
                pong.log_cursor = control.log_cursor;
                std::string send_error;
                if (!send_control_message(pong, &send_error)) {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "stream",
                        "stream RTT pong send failed: " + send_error);
                }
                return;
            }
            if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kPong
                && options.stream_smoke && options.role == RuntimeRole::kHost) {
                const std::uint64_t now_ms = now_steady_ms();
                std::lock_guard<std::mutex> lock(callback_mutex);
                if (stream_required_channels_ready
                    && stream_media_channel_open
                    && stream_control_channel_open
                    && stream_rtt_telemetry_snapshot.in_flight_ping_sequence == control.log_cursor
                    && stream_rtt_telemetry_snapshot.in_flight_ping_sent_steady_ms > 0
                    && now_ms >= stream_rtt_telemetry_snapshot.in_flight_ping_sent_steady_ms) {
                    const std::uint32_t rtt_ms = static_cast<std::uint32_t>(
                        now_ms - stream_rtt_telemetry_snapshot.in_flight_ping_sent_steady_ms);
                    stream_rtt_telemetry_snapshot.in_flight_ping_sequence = 0;
                    stream_rtt_telemetry_snapshot.in_flight_ping_sent_steady_ms = 0;
                    ++stream_rtt_telemetry_snapshot.ping_ack_count;
                    stream_rtt_telemetry_snapshot.last_ack_steady_ms = now_ms;
                    stream_rtt_telemetry_snapshot.total_rtt_ms += rtt_ms;
                    stream_rtt_telemetry_snapshot.last_rtt_ms = rtt_ms;
                    stream_rtt_telemetry_snapshot.smoothed_rtt_ms =
                        stream_rtt_telemetry_snapshot.smoothed_rtt_ms == 0
                            ? rtt_ms
                            : static_cast<std::uint32_t>(
                                  (static_cast<std::uint64_t>(
                                       stream_rtt_telemetry_snapshot.smoothed_rtt_ms)
                                       * 7ULL
                                   + rtt_ms)
                                  / 8ULL);
                    if (stream_rtt_telemetry_snapshot.min_rtt_ms == 0
                        || rtt_ms < stream_rtt_telemetry_snapshot.min_rtt_ms) {
                        stream_rtt_telemetry_snapshot.min_rtt_ms = rtt_ms;
                    }
                    stream_rtt_telemetry_snapshot.max_rtt_ms = (std::max)(
                        stream_rtt_telemetry_snapshot.max_rtt_ms, rtt_ms);
                }
                return;
            }
            if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kViewportRequest
                && options.role == RuntimeRole::kHost) {
                bool changed = false;
                std::uint64_t geometry_revision = 0;
                std::uint64_t rate_revision = 0;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    ++stream_viewport_request_total;
                    if (stream_requested_viewport_width != control.viewport_width
                        || stream_requested_viewport_height != control.viewport_height) {
                        stream_requested_viewport_width = control.viewport_width;
                        stream_requested_viewport_height = control.viewport_height;
                        stream_geometry_transaction_id = control.log_cursor;
                        geometry_revision = ++stream_geometry_revision;
                        rate_revision = stream_adaptive_control_snapshot.rate_revision;
                        changed = true;
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kInfo,
                            "stream-control",
                            "viewport target accepted width=" + std::to_string(control.viewport_width)
                                + " height=" + std::to_string(control.viewport_height));
                    }
                }
                if (changed) {
                    redclaw::session::DesktopSourceActivityUpdate activity_update;
                    {
                        std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
                        activity_update = stream_source_activity_tracker.on_viewport_change(
                            now_steady_ms(), geometry_revision, rate_revision);
                    }
                    stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kViewport);
                    if (activity_update.request_keyframe) {
                        stream_encoder_session.request_keyframe();
                        stream_keyframe_refresh_request_generation.fetch_add(1);
                    }
                    stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
                    if (activity_update.state_changed) {
                        (void)publish_source_activity(activity_update.snapshot);
                    }
                }
                return;
            }
            if (control.type
                    == redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRequest
                && options.role == RuntimeRole::kHost) {
                bool stale = false;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    stale = control.capture_region_revision <= stream_capture_region_revision;
                    if (!stale) {
                        stream_pending_capture_region_request = control;
                    }
                }
                if (stale) {
                    auto rejected = make_control_message(
                        redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRejected);
                    rejected.display_id = control.display_id;
                    rejected.region_left = control.region_left;
                    rejected.region_top = control.region_top;
                    rejected.region_right = control.region_right;
                    rejected.region_bottom = control.region_bottom;
                    rejected.capture_region_revision = control.capture_region_revision;
                    rejected.payload = "stale_region_revision";
                    std::string rejected_error;
                    (void)send_control_message(rejected, &rejected_error);
                }
                return;
            }
            if (control.type
                    == redclaw::protocol::StreamControlMessageTypeV1::kMediaTransportFeedback
                && options.role == RuntimeRole::kHost) {
                redclaw::net::MediaTransportFeedbackBatch feedback;
                feedback.feedback_id = control.transport_feedback_id;
                feedback.observed_rate_revision = control.observed_rate_revision;
                feedback.arrivals.reserve(control.media_transport_arrivals.size());
                for (const auto& arrival : control.media_transport_arrivals) {
                    feedback.arrivals.push_back({
                        .transport_sequence = arrival.transport_sequence,
                        .receiver_steady_us = arrival.receiver_steady_us,
                    });
                }
                std::uint64_t current_rate_revision = 0;
                std::uint32_t smoothed_rtt_ms = 0;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    current_rate_revision = stream_adaptive_control_snapshot.rate_revision;
                    smoothed_rtt_ms = stream_rtt_telemetry_snapshot.smoothed_rtt_ms;
                }
                const std::uint64_t host_steady_us = now_steady_ms() * 1000ULL;
                const bool applied = stream_transport_estimator.apply_feedback(
                    feedback,
                    host_steady_us,
                    current_rate_revision);
                const auto estimate = stream_transport_estimator.snapshot(
                    host_steady_us,
                    smoothed_rtt_ms);
                std::uint32_t pacing_bitrate_kbps =
                    kDesktopStreamTransportBitrateCeilingKbps;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    stream_transport_estimate_snapshot = estimate;
                    pacing_bitrate_kbps =
                        stream_congestion_decision_snapshot.pacing_bitrate_kbps != 0
                            ? stream_congestion_decision_snapshot.pacing_bitrate_kbps
                            : kDesktopStreamTransportBitrateCeilingKbps;
                    if (applied) {
                        ++stream_transport_feedback_received_total;
                    } else {
                        ++stream_transport_feedback_ignored_total;
                    }
                }
                stream_media_pacer.update_budget(
                    pacing_bitrate_kbps,
                    smoothed_rtt_ms,
                    estimate.in_flight_bytes);
                if (applied && estimate.feedback_fresh
                    && estimate.rate_revision == current_rate_revision) {
                    stream_media_pacer.observe_ack_progress(estimate.acknowledged_packets);
                }
                stream_media_pacer.notify_writable();
                return;
            }
            if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kReceiverNetworkStats
                && options.role == RuntimeRole::kHost) {
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    stream_receiver_stats_snapshot = control;
                }
                if (stream_capture_gate.presented(control.latest_presented_frame_id)) {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    append_timeline(redclaw::render::RuntimeStatusSeverity::kInfo, "capture",
                        "capture recovery confirmed by new frame presentation");
                }
                redclaw::session::DesktopSourceActivityUpdate activity_update;
                {
                    std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
                    activity_update = stream_source_activity_tracker.on_displayable_ack(
                        control.observed_source_activity_revision,
                        control.latest_displayable_keyframe_id);
                }
                if (activity_update.state_changed) {
                    (void)publish_source_activity(activity_update.snapshot);
                }
                return;
            }
            if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kSourceActivityState
                && options.role == RuntimeRole::kController) {
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    stream_observed_source_activity_revision = control.source_activity_revision;
                }
                (void)local_control_output.send(control, received_monotonic_us);
                return;
            }
            if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kPlaybackStarvation
                && options.role == RuntimeRole::kHost) {
                const std::uint64_t steady_now_ms = now_steady_ms();
                std::lock_guard<std::mutex> lock(callback_mutex);
                if (control.incomplete_frame_id
                    <= stream_last_playback_starvation_frame_id_received) {
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "stream-control",
                        "duplicate incomplete media frame feedback ignored frame_id="
                            + std::to_string(control.incomplete_frame_id));
                    return;
                }
                stream_last_playback_starvation_frame_id_received = control.incomplete_frame_id;
                ++stream_playback_starvation_received_total;

                const auto sent_frame = stream_sent_frame_metadata.find(
                    control.incomplete_frame_id);
                const auto assessment = redclaw::net::assess_receiver_feedback(
                    sent_frame,
                    steady_now_ms,
                    stream_rtt_telemetry_snapshot.smoothed_rtt_ms,
                    control.observed_rate_revision,
                    stream_adaptive_control_snapshot.rate_revision);
                stream_adaptive_control_snapshot.last_feedback_age_ms =
                    assessment.feedback_age_ms;

                std::string freshness_name;
                switch (assessment.freshness) {
                case redclaw::net::ReceiverFeedbackFreshness::kFresh: {
                    freshness_name = "fresh";
                    ++stream_adaptive_control_snapshot.fresh_feedback_count;
                    stream_adaptive_control_snapshot.last_playback_starvation_ms = steady_now_ms;
                    stream_adaptive_control_snapshot.playback_starvation_pacing_active = true;
                    stream_adaptive_control_snapshot.relief_window_count = 0;
                    break;
                }
                case redclaw::net::ReceiverFeedbackFreshness::kDelayed:
                    freshness_name = "delayed";
                    ++stream_adaptive_control_snapshot.delayed_feedback_count;
                    break;
                case redclaw::net::ReceiverFeedbackFreshness::kExpired:
                    freshness_name = "expired";
                    ++stream_adaptive_control_snapshot.expired_feedback_count;
                    ++stream_adaptive_control_snapshot.ignored_feedback_count;
                    break;
                case redclaw::net::ReceiverFeedbackFreshness::kStaleRevision:
                    freshness_name = "stale_revision";
                    ++stream_adaptive_control_snapshot.stale_revision_feedback_count;
                    ++stream_adaptive_control_snapshot.ignored_feedback_count;
                    break;
                case redclaw::net::ReceiverFeedbackFreshness::kUnknownFrame:
                    freshness_name = "unknown_frame";
                    ++stream_adaptive_control_snapshot.unknown_frame_feedback_count;
                    ++stream_adaptive_control_snapshot.ignored_feedback_count;
                    break;
                }
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream-control",
                    "incomplete media frame evidence observed count="
                        + std::to_string(control.playback_starvation_count)
                        + " frame_id=" + std::to_string(control.incomplete_frame_id)
                        + " latest_received_frame_id="
                        + std::to_string(control.latest_received_frame_id)
                        + " latest_complete_frame_id="
                        + std::to_string(control.latest_complete_frame_id)
                        + " observed_revision="
                        + std::to_string(control.observed_rate_revision)
                        + " current_revision="
                        + std::to_string(stream_adaptive_control_snapshot.rate_revision)
                        + " feedback_age_ms="
                        + std::to_string(assessment.feedback_age_ms)
                        + " freshness=" + freshness_name
                        + " target_fps="
                        + std::to_string(resolve_stream_target_fps(
                            stream_adaptive_control_snapshot)));
                return;
            }
            if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kKeyframeRequest
                && options.role == RuntimeRole::kHost) {
                if (peer_capture_status_version.load() >= 1 && control.capture_status_version == 1
                    && control.capture_retry_requested) {
                    stream_capture_session.retryCapture();
                    stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
                }
                const auto pacer = stream_media_pacer.telemetry();
                // Receiver retries must not replace a normally progressing IDR
                // or reset the admission cooldown for the same recovery.
                if (!pacer.keyframe_required
                    && stream_keyframe_refresh_request_generation.load()
                        <= stream_keyframe_refresh_completed_generation.load()) {
                    stream_encoder_session.request_keyframe();
                    stream_keyframe_refresh_request_generation.fetch_add(1);
                    stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kKeyframe);
                }
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    ++stream_keyframe_request_received_total;
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kInfo,
                        "stream-control",
                        "keyframe request accepted reason="
                            + (control.payload.empty() ? std::string("unspecified") : control.payload));
                }
                return;
            }
            if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogRequest) {
                queue_remote_log_response(control);
                return;
            }
            if ((control.type == redclaw::protocol::StreamControlMessageTypeV1::kInputControlRequest
                 || control.type == redclaw::protocol::StreamControlMessageTypeV1::kInputBatch
                 || control.type == redclaw::protocol::StreamControlMessageTypeV1::kInputStateSync
                 || control.type == redclaw::protocol::StreamControlMessageTypeV1::kInputReleaseAll)
                && options.role == RuntimeRole::kHost) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                if (pending_remote_input_messages.size() >= 64) {
                    remote_input_queue_overflow = true;
                } else {
                    pending_remote_input_messages.push_back(control);
                    input_qa_receipts.command(redclaw::runtime::InputQaStage::kPeerEnqueued, control);
                }
                runtime_loop_wake.notify();
                return;
            }
            if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogChunk
                || control.type == redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogComplete
                || control.type == redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogError
                || control.type == redclaw::protocol::StreamControlMessageTypeV1::kStreamTargetApplied
                || control.type == redclaw::protocol::StreamControlMessageTypeV1::kSourceActivityState
                || control.type == redclaw::protocol::StreamControlMessageTypeV1::kDesktopDisplayCatalog
                || control.type == redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionApplied
                || control.type == redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRejected
                || control.type == redclaw::protocol::StreamControlMessageTypeV1::kInputCapabilities
                || control.type == redclaw::protocol::StreamControlMessageTypeV1::kInputControlStatus) {
                if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kInputCapabilities
                    || control.type == redclaw::protocol::StreamControlMessageTypeV1::kInputControlStatus) {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    controller_remote_input_supported = control.input_supported;
                    controller_remote_input_authorized = control.input_authorized;
                }
                (void)local_control_output.send(control, received_monotonic_us);
                return;
            }
            return;
        }

    };

    ice_wrapper.onDataChannelBinaryMessage(
        [&](redclaw::net::DataChannelKind kind, std::span<const std::uint8_t> message) {
        if (kind == redclaw::net::DataChannelKind::kTransfer) {
            (void)transfer_bridge.receive_bulk({reinterpret_cast<const char*>(message.data()), message.size()});
            runtime_loop_wake.notify(); return;
        }
        if (kind == redclaw::net::DataChannelKind::kTerminal) {
            (void)terminal_bridge.receive({reinterpret_cast<const char*>(message.data()), message.size()});
            runtime_loop_wake.notify();
            return;
        }
        if (kind == redclaw::net::DataChannelKind::kControl || kind == redclaw::net::DataChannelKind::kAgent) {
            handle_compressed_message(kind, {reinterpret_cast<const char*>(message.data()), message.size()});
            return;
        }
        if (kind == redclaw::net::DataChannelKind::kNavigation
            && options.role == RuntimeRole::kController) {
            redclaw::net::NavigationThumbnail thumbnail;
            std::string parse_error;
            if (!redclaw::net::parse_navigation_thumbnail(
                    message, &thumbnail, &parse_error)) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "navigation",
                    "invalid navigation thumbnail rejected: " + parse_error);
                return;
            }
#ifdef _WIN32
            std::string pipe_error;
            const auto status = navigation_frame_pipe.write_navigation_thumbnail(
                thumbnail, &pipe_error);
            if (status != DirectFramePipeWriteStatus::kWritten
                && status != DirectFramePipeWriteStatus::kBusyDrop) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "navigation",
                    "navigation shared-memory delivery failed: " + pipe_error);
            }
#endif
            return;
        }
        if (kind != redclaw::net::DataChannelKind::kMedia
            || !options.stream_smoke || options.role != RuntimeRole::kController) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            stream_received_wire_bytes += message.size();
            ++stream_media_fragments_received;
        }

        redclaw::net::EncodedVideoFragmentView fragment;
        std::string fragment_error;
        if (!redclaw::net::parse_encoded_video_fragment(message, &fragment, &fragment_error)) {
            redclaw::net::EncodedVideoDropSummary drop_summary;
            {
                std::lock_guard<std::mutex> receive_lock(stream_receive_mutex);
                drop_summary = stream_video_reassembly.require_keyframe();
            }
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                ++stream_fragment_parse_failures;
                stream_incomplete_frames_dropped += drop_summary.dropped_incomplete_frames;
                stream_dropped_keyframes += drop_summary.dropped_keyframes;
                stream_last_media_assembly_error = fragment_error;
                stream_counters.last_error = fragment_error;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream",
                    "encoded desktop video fragment decode failed: " + fragment_error);
            }
            if (drop_summary.dropped_incomplete_frames > 0) {
                (void)report_incomplete_media_frame(
                    drop_summary.dropped_frame_id,
                    "fragment_parse_abandoned_incomplete_frame");
            }
            (void)report_decoder_dependency_break(
                redclaw::session::ControllerDecoderBreakReason::kFragmentParse,
                drop_summary.dropped_frame_id,
                "fragment_parse_failure");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (fragment.frame_id >= stream_latest_received_frame_id) {
                stream_latest_received_frame_id = fragment.frame_id;
                stream_observed_rate_revision = fragment.rate_revision;
            }
        }

        bool qa_drop_fragment = false;
        if (options.stream_qa_drop_one_media_fragment
            && fragment.fragment_count > 1
            && fragment.fragment_index == 1) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (stream_qa_media_fragment_drop_total == 0
                && stream_encoded_frames_reassembled >= 30) {
                ++stream_qa_media_fragment_drop_total;
                stream_qa_media_loss_frame_id = fragment.frame_id;
                qa_drop_fragment = true;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream-qa",
                    "one multipart media fragment deliberately dropped frame_id="
                        + std::to_string(fragment.frame_id)
                        + " fragment_index=" + std::to_string(fragment.fragment_index));
            }
        }
        if (qa_drop_fragment) {
            return;
        }

        const std::uint64_t receiver_steady_us = now_steady_ms() * 1000ULL;
        bool arrival_recorded = false;
        {
            std::lock_guard<std::mutex> feedback_lock(stream_transport_feedback_mutex);
            arrival_recorded = stream_transport_feedback_recorder.record(
                fragment.transport_sequence,
                receiver_steady_us,
                fragment.rate_revision);
        }
        if (!arrival_recorded) {
            (void)flush_media_transport_feedback(receiver_steady_us);
            std::lock_guard<std::mutex> feedback_lock(stream_transport_feedback_mutex);
            arrival_recorded = stream_transport_feedback_recorder.record(
                fragment.transport_sequence,
                receiver_steady_us,
                fragment.rate_revision);
        }
        if (!arrival_recorded) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            ++stream_transport_feedback_invalid_total;
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kWarning,
                "stream-control",
                "media transport arrival rejected sequence="
                    + std::to_string(fragment.transport_sequence));
        } else {
            (void)flush_media_transport_feedback(receiver_steady_us);
        }

        redclaw::net::EncodedVideoReassemblyResult reassembly_result;
        {
            std::lock_guard<std::mutex> receive_lock(stream_receive_mutex);
            reassembly_result = stream_video_reassembly.push(fragment, now_steady_ms());
        }
        if (reassembly_result.dropped_incomplete_frames > 0
            || reassembly_result.dropped_dependency_frames > 0
            || reassembly_result.dropped_keyframes > 0) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            stream_incomplete_frames_dropped += reassembly_result.dropped_incomplete_frames;
            stream_dependency_frames_dropped += reassembly_result.dropped_dependency_frames;
            stream_dropped_keyframes += reassembly_result.dropped_keyframes;
        }
        if (reassembly_result.dropped_incomplete_frames > 0) {
            (void)report_incomplete_media_frame(
                reassembly_result.dropped_frame_id,
                reassembly_result.error.empty()
                    ? "incomplete_frame_superseded"
                    : "fragment_reassembly_failure");
        }
        if (!reassembly_result.error.empty()) {
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                ++stream_reassembly_failures;
                stream_last_media_assembly_error = reassembly_result.error;
                stream_counters.last_error = reassembly_result.error;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream",
                    "encoded desktop video fragment reassembly failed frame_id="
                        + std::to_string(fragment.frame_id)
                        + " fragment_index=" + std::to_string(fragment.fragment_index)
                        + " fragment_count=" + std::to_string(fragment.fragment_count)
                        + ": " + reassembly_result.error);
            }
            (void)report_decoder_dependency_break(
                redclaw::session::ControllerDecoderBreakReason::kFragmentReassembly,
                reassembly_result.dropped_frame_id,
                "fragment_reassembly_failure");
            return;
        }
        if (reassembly_result.keyframe_required) {
            const bool dependency_frame = reassembly_result.dropped_dependency_frames > 0;
            (void)report_decoder_dependency_break(
                dependency_frame
                    ? redclaw::session::ControllerDecoderBreakReason::kDependencyFrame
                    : redclaw::session::ControllerDecoderBreakReason::kFragmentReassembly,
                reassembly_result.dropped_frame_id,
                dependency_frame
                    ? "dependency_frame_discarded"
                    : "incomplete_frame_abandoned");
        }

        if (reassembly_result.status != redclaw::net::EncodedVideoReassemblyStatus::kComplete) {
            return;
        }

        if (!stream_decoder_recovery_coordinator.should_accept_frame(
                reassembly_result.frame.keyframe)) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            ++stream_dependency_frames_dropped;
            return;
        }

        redclaw::render::EncodedVideoFrame encoded_frame;
        encoded_frame.frame_id = reassembly_result.frame.frame_id;
        encoded_frame.codec = encoded_video_codec_from_binary(reassembly_result.frame.codec);
        if (encoded_frame.codec == redclaw::render::EncodedVideoCodec::kUnknown) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            ++stream_counters.decode_failures;
            stream_counters.last_error = "encoded video fragment codec is unsupported";
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kWarning,
                "stream",
                "encoded desktop video fragment codec is unsupported");
            return;
        }
        encoded_frame.width = reassembly_result.frame.width;
        encoded_frame.height = reassembly_result.frame.height;
        encoded_frame.timestamp_ms = reassembly_result.frame.timestamp_ms;
        encoded_frame.keyframe = reassembly_result.frame.keyframe;
        encoded_frame.capture_region_revision =
            reassembly_result.frame.capture_region_revision;
        encoded_frame.content_rect_x = reassembly_result.frame.content_rect_x;
        encoded_frame.content_rect_y = reassembly_result.frame.content_rect_y;
        encoded_frame.content_rect_width = reassembly_result.frame.content_rect_width;
        encoded_frame.content_rect_height = reassembly_result.frame.content_rect_height;
        encoded_frame.payload = std::move(reassembly_result.frame.payload);
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            ++stream_encoded_frames_reassembled;
            stream_latest_complete_frame_id = reassembly_result.frame.frame_id;
            stream_observed_rate_revision = reassembly_result.frame.rate_revision;
            if (stream_qa_forced_channel_close_total > 0
                && stream_recovery_success_total
                    > stream_qa_recovery_success_total_at_forced_close
                && stream_qa_post_reconnect_frame_id == 0) {
                stream_qa_post_reconnect_frame_id = reassembly_result.frame.frame_id;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kInfo,
                    "stream-qa",
                    "first complete post-reconnect frame received frame_id="
                        + std::to_string(reassembly_result.frame.frame_id));
            }
            if (encoded_frame.keyframe) {
                ++stream_completed_keyframes;
                stream_latest_complete_keyframe_id = reassembly_result.frame.frame_id;
                if (!stream_last_media_assembly_error.empty()
                    && stream_counters.last_error == stream_last_media_assembly_error) {
                    stream_counters.last_error.clear();
                }
                stream_last_media_assembly_error.clear();
                if (stream_qa_media_loss_frame_id != 0
                    && stream_qa_media_recovery_keyframe_id == 0
                    && reassembly_result.frame.frame_id > stream_qa_media_loss_frame_id) {
                    stream_qa_media_recovery_keyframe_id = reassembly_result.frame.frame_id;
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kInfo,
                        "stream-qa",
                        "media recovery keyframe completed frame_id="
                            + std::to_string(reassembly_result.frame.frame_id));
                }
            }
        }
        handle_received_encoded_video(encoded_frame);
    });

    const std::filesystem::path local_candidates_file =
        options.role == RuntimeRole::kHost ? signaling.host_candidates : signaling.controller_candidates;
    const std::filesystem::path remote_candidates_file =
        options.role == RuntimeRole::kHost ? signaling.controller_candidates : signaling.host_candidates;
    const std::filesystem::path inbound_description_file = use_sealed_file_transport
        ? (options.role == RuntimeRole::kHost ? signaling.controller_answer_sealed : signaling.host_offer_sealed)
        : (options.role == RuntimeRole::kHost ? signaling.controller_answer : signaling.host_offer);
    const std::filesystem::path outbound_description_file = use_sealed_file_transport
        ? (options.role == RuntimeRole::kHost ? signaling.host_offer_sealed : signaling.controller_answer_sealed)
        : (options.role == RuntimeRole::kHost ? signaling.host_offer : signaling.controller_answer);

    ice_wrapper.onLocalCandidate([&](const std::string& candidate, const std::string& mid) {
        bool should_publish_sealed = false;
        std::string local_description_snapshot;
        std::vector<std::string> local_candidates_snapshot;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            record_candidate(&candidate_diagnostics, true, classify_candidate_type(candidate), candidate);
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kInfo,
                "signal",
                "local candidate emitted type=" + std::to_string(static_cast<int>(classify_candidate_type(candidate))));

            if (use_tcp_transport) {
                std::vector<std::string> event_fields = {"candidate", mid, candidate};
                pending_outbound_signal_events.push_back(event_fields);
                outbound_signal_history.push_back(std::move(event_fields));
            } else if (use_sealed_file_transport) {
                local_candidate_lines.push_back(mid + "\t" + candidate);
                if (local_description_written && !local_description_sdp.empty()) {
                    should_publish_sealed = true;
                    local_description_snapshot = local_description_sdp;
                    local_candidates_snapshot = local_candidate_lines;
                }
            } else if (use_rendezvous_transport || use_dht_transport) {
                local_candidate_lines.push_back(mid + "\t" + candidate);
                if (!local_description_sdp.empty()) {
                    signal_snapshot_dirty = true;
                }
            }
        }

        if (use_tcp_transport) {
            return;
        }

        if (use_sealed_file_transport) {
            if (!should_publish_sealed) {
                return;
            }

            std::string local_error;
            std::string sealed_blob;
            if (!build_sealed_signal_blob(
                    role_name,
                    local_description_snapshot,
                    local_candidates_snapshot,
                    options,
                    &sealed_blob,
                    &local_error)
                || !write_text_file(outbound_description_file, sealed_blob, &local_error)) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                if (runtime_error.empty()) {
                    runtime_error = local_error;
                }
                saw_failed_state = true;
                return;
            }

            std::lock_guard<std::mutex> lock(callback_mutex);
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kInfo,
                "signal",
                "sealed signaling blob refreshed with latest local candidates");
            return;
        }

        if (use_rendezvous_transport || use_dht_transport) {
            return;
        }

        std::string local_error;
        bool published = false;
        if (use_event_log_transport) {
            published = append_event_log_line(local_event_log, {"candidate", mid, candidate}, &local_error);
        } else {
            published = append_candidate_line(local_candidates_file, candidate, mid, &local_error);
        }

        if (!published) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (runtime_error.empty()) {
                runtime_error = local_error;
            }
            saw_failed_state = true;
        }
    });

    ice_wrapper.onLocalDescription([&](const std::string& sdp, bool is_offer) {
        const bool is_expected_description = (options.role == RuntimeRole::kHost && is_offer)
            || (options.role == RuntimeRole::kController && !is_offer);
        if (!is_expected_description) {
            return;
        }

        std::vector<std::string> local_candidates_snapshot;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (local_description_written) {
                return;
            }

            local_description_sdp = sdp;

            if (use_dht_transport) {
                const std::string description_tag =
                    redclaw::service::derive_dht_description_tag(local_description_sdp);
                if (options.role == RuntimeRole::kHost) {
                    const auto generation_result = dht_negotiation.begin_host_generation(
                        dht_host_generation_sequence,
                        description_tag);
                    if (!generation_result.accepted()) {
                        if (runtime_error.empty()) {
                            runtime_error = "failed to begin Host DHT negotiation generation";
                        }
                        saw_failed_state = true;
                        return;
                    }
                } else if (!dht_negotiation.mark_local_answer_ready(description_tag)) {
                    if (runtime_error.empty()) {
                        runtime_error = "failed to bind Controller answer to active DHT generation";
                    }
                    saw_failed_state = true;
                    return;
                }
            }

            if (use_tcp_transport) {
                std::vector<std::string> event_fields = {"description", is_offer ? "offer" : "answer", sdp};
                pending_outbound_signal_events.push_back(event_fields);
                outbound_signal_history.push_back(std::move(event_fields));
                local_description_written = true;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kInfo,
                    "signal",
                    std::string("local ") + (is_offer ? "offer" : "answer") + " queued for tcp signaling");
                return;
            }

            if (use_sealed_file_transport) {
                local_candidates_snapshot = local_candidate_lines;
            }

            if (use_rendezvous_transport || use_dht_transport) {
                local_description_written = true;
                signal_snapshot_dirty = true;
                std::cout << "Runtime local description captured role=" << role_name
                          << " type=" << (is_offer ? "offer" : "answer")
                          << " sdp_bytes=" << sdp.size()
                          << '\n';
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kInfo,
                    "signal",
                    std::string("local ") + (is_offer ? "offer" : "answer")
                        + (use_dht_transport ? " captured for DHT publication" : " captured for rendezvous publication"));
                return;
            }
        }

        std::string local_error;
        bool published = false;
        if (use_event_log_transport) {
            published = append_event_log_line(
                local_event_log,
                {"description", is_offer ? "offer" : "answer", sdp},
                &local_error);
        } else if (use_sealed_file_transport) {
            if (local_candidates_snapshot.empty()) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                local_description_written = true;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kInfo,
                    "signal",
                    std::string("local ") + (is_offer ? "offer" : "answer")
                        + " captured; waiting for local candidate before sealed publication");
                return;
            }

            std::string sealed_blob;
            if (build_sealed_signal_blob(
                    role_name,
                    sdp,
                    local_candidates_snapshot,
                    options,
                    &sealed_blob,
                    &local_error)) {
                published = write_text_file(outbound_description_file, sealed_blob, &local_error);
            }
        } else {
            published = write_text_file(outbound_description_file, sdp, &local_error);
        }

        std::lock_guard<std::mutex> lock(callback_mutex);
        if (!published) {
            if (runtime_error.empty()) {
                runtime_error = local_error;
            }
            saw_failed_state = true;
            return;
        }

        local_description_written = true;
        append_timeline(
            redclaw::render::RuntimeStatusSeverity::kInfo,
            "signal",
            use_sealed_file_transport
                ? (std::string("local ") + (is_offer ? "offer" : "answer") + " published as sealed blob")
                : (std::string("local ") + (is_offer ? "offer" : "answer") + " published"));
    });

    // Declared after every callback-owned dependency and before gathering can
    // start. The later worker scope drains sends before this callback barrier.
    struct TransportShutdownScope final {
        redclaw::net::IceConnectivityWrapper& wrapper;
        ~TransportShutdownScope() { (void)wrapper.shutdown(); }
    } transport_shutdown_scope{ice_wrapper};

    redclaw::net::IceGatheringConfig gathering_config;
    gathering_config.initiate_offer = options.role == RuntimeRole::kHost;
    gathering_config.enable_ice_tcp = options.enable_ice_tcp;
    // DHT delivery may outlast the native initial ICE burst. Keep candidates
    // immediate, with bounded initial probes; established consent remains native.
    gathering_config.initial_check_window_ms = use_dht_transport ? 120000 : 0;
    gathering_config.ice_servers = options.ice_servers;
    gathering_config.bind_address = options.network_bind_address;
    gathering_config.port_range_begin = options.ice_udp_port;
    gathering_config.port_range_end = options.ice_udp_port;

    const bool defer_ice_gathering_until_signaling_peer =
        use_dht_transport || use_rendezvous_transport;
    bool ice_gathering_started = false;
    auto ensure_ice_gathering_started = [&](std::string_view reason, std::string* error_detail = nullptr) {
        if (ice_gathering_started) {
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

        if (options.enable_port_mapping && !ice_port_mapping_attempted) {
            ice_port_mapping_attempted = true;
            ice_port_mapping_result = redclaw::service::attempt_upnp_udp_port_mapping(
                options.ice_udp_port,
                "RedClawDesktop ICE",
                options.network_bind_address);
            std::cout << "Runtime ICE port mapping role=" << role_name
                      << " status=" << ice_port_mapping_result.status
                      << " internal_port=" << ice_port_mapping_result.internal_port
                      << " external_port=" << ice_port_mapping_result.external_port;
            if (!ice_port_mapping_result.external_ip.empty()) {
                std::cout << " external_ip=" << ice_port_mapping_result.external_ip;
            }
            if (!ice_port_mapping_result.detail.empty()) {
                std::cout << " detail=" << ice_port_mapping_result.detail;
            }
            std::cout << '\n';
        }

        std::string start_error;
        if (ice_port_mapping_result.mapped) {
            gathering_config.udp_port_mapping = redclaw::net::IceUdpPortMapping{
                ice_port_mapping_result.internal_ip,
                static_cast<std::uint16_t>(ice_port_mapping_result.internal_port),
                ice_port_mapping_result.external_ip,
                static_cast<std::uint16_t>(ice_port_mapping_result.external_port)};
        }
        std::cout << "Runtime ICE gathering start requested role=" << role_name
                  << " reason=" << reason
                  << " ice_udp_port=" << options.ice_udp_port
                  << " initial_check_window_ms=" << gathering_config.initial_check_window_ms
                  << '\n';
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            local_candidate_gathering_complete = false;
            if (use_dht_transport && options.role == RuntimeRole::kHost) {
                ++dht_host_generation_sequence;
            }
        }
        if (!ice_wrapper.startGathering(gathering_config, &start_error)) {
            if (error_detail != nullptr) {
                *error_detail = "ICE UDP port " + std::to_string(options.ice_udp_port)
                    + " could not be opened: " + start_error;
            }
            return false;
        }

        ice_gathering_started = true;
        std::cout << "Runtime ICE gathering started role=" << role_name
                  << " reason=" << reason
                  << '\n';
        if (defer_ice_gathering_until_signaling_peer) {
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kInfo,
                "ice",
                std::string(reason));
        }
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    };

    auto prepare_host_persistent_offer = [&](std::string* error_detail = nullptr) {
        if (!use_dht_transport || options.role != RuntimeRole::kHost) {
            return true;
        }
        dht_connection_request_payload = make_runtime_session_id("host-standby-offer");
        const std::string standby_tag = redclaw::service::derive_dht_description_tag(
            dht_connection_request_payload);
        const auto standby = dht_negotiation.observe_controller_request(standby_tag);
        if (!standby.accepted()) {
            if (error_detail != nullptr) {
                *error_detail = "failed to initialize persistent Host offer lease";
            }
            return false;
        }
        dht_host_persistent_offer_active = true;
        signal_snapshot_dirty = true;
        return ensure_ice_gathering_started(
            "persistent Host DHT offer gathering started", error_detail);
    };

    if (use_dht_transport && options.role == RuntimeRole::kHost) {
        std::string ice_error;
        if (!prepare_host_persistent_offer(&ice_error)) {
            std::cerr << "Runtime persistent Host DHT offer failed: " << ice_error << '\n';
            return 1;
        }
    } else if (!defer_ice_gathering_until_signaling_peer) {
        std::string ice_error;
        if (!ensure_ice_gathering_started("runtime ICE gathering started", &ice_error)) {
            std::cerr << "Runtime ICE gathering failed: " << ice_error << '\n';
            return 1;
        }
    }

    bool remote_description_applied = false;
    bool remote_description_ever_applied = false;
    std::size_t applied_remote_candidate_lines = 0;
    std::size_t applied_remote_event_lines = 0;
    std::vector<std::pair<std::string, std::string>> pending_remote_candidates;
    const bool inbound_is_offer = options.role == RuntimeRole::kController;

    auto dht_remote_candidate_application_allowed = [&]() {
        std::lock_guard<std::mutex> lock(callback_mutex);
        redclaw::service::DhtIceFailureRecoveryInput input;
        input.role = options.role == RuntimeRole::kHost
            ? redclaw::service::ConnectionNegotiationRole::kHost
            : redclaw::service::ConnectionNegotiationRole::kController;
        input.transport_failed = saw_failed_state;
        input.connected_once = saw_connected_state;
        input.remote_description_applied = remote_description_applied;
        input.repair_attempts = dht_repair_attempts;
        return redclaw::service::decide_dht_ice_failure_recovery(input)
            .may_apply_remote_candidates;
    };

    auto restart_tcp_signaling_session = [&](const std::string& reason) {
        if (!ice_wrapper.retireAndDrain()) {
            runtime_error = "tcp signaling retirement requires owner thread";
            return false;
        }
        remote_description_applied = false;
        pending_remote_candidates.clear();
#ifdef _WIN32
        tcp_receive_buffer.clear();
#endif

        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            local_description_written = false;
            pending_outbound_signal_events.clear();
        }
        outbound_signal_history.clear();

        ice_gathering_started = false;
        std::string restart_error;
        if (!ensure_ice_gathering_started("tcp signaling session reset after " + reason, &restart_error)) {
            runtime_error = "failed to restart tcp signaling session after " + reason + ": " + restart_error;
            return false;
        }

        std::lock_guard<std::mutex> lock(callback_mutex);
        append_timeline(
            redclaw::render::RuntimeStatusSeverity::kInfo,
            "signal",
            "tcp signaling session reset after " + reason);
        return true;
    };

    enum class HostStreamCaptureTickResult {
        kNoWork,
        kCaptureAttempted,
        kFailure,
    };

    auto apply_capture_availability = [&](const redclaw::capture::CaptureBackendTelemetry& telemetry) {
        const auto previous = stream_capture_gate.snapshot();
        if (stream_capture_gate.update(telemetry.availability, telemetry.generation)) {
            {
                std::lock_guard<std::mutex> lock(stream_capture_frame_mutex);
                stream_latest_captured_frame.reset();
                stream_latest_captured_frame_ready_ms = 0;
            }
            stream_media_pacer.reset();
            stream_encoder_session.request_keyframe();
            stream_keyframe_refresh_request_generation.fetch_add(1);
            runtime_loop_wake.notify();
        }
        return previous.availability != telemetry.availability || previous.generation != telemetry.generation;
    };

    auto run_host_stream_capture_tick = [&]() -> HostStreamCaptureTickResult {
        if (!options.stream_smoke || options.role != RuntimeRole::kHost) {
            return HostStreamCaptureTickResult::kNoWork;
        }

        bool channel_open = false;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            channel_open = stream_media_channel_open;
        }
        if (!channel_open) {
            set_stream_worker_stage(
                &stream_capture_worker_stage,
                &stream_capture_worker_stage_since_ms,
                HostStreamWorkerStage::kWaitingForChannel);
            return HostStreamCaptureTickResult::kNoWork;
        }

        const std::uint64_t catalog_now_ms = now_steady_ms();
        if (stream_last_display_catalog_check_ms == 0
            || catalog_now_ms - stream_last_display_catalog_check_ms >= 2000) {
            stream_last_display_catalog_check_ms = catalog_now_ms;
            std::string catalog_error;
            auto refreshed_displays =
                redclaw::capture::enumerate_capture_displays(&catalog_error);
            const auto same_catalog = [](const auto& left, const auto& right) {
                if (left.size() != right.size()) {
                    return false;
                }
                for (std::size_t index = 0; index < left.size(); ++index) {
                    const auto& a = left[index];
                    const auto& b = right[index];
                    if (a.id != b.id
                        || a.desktop_origin_x != b.desktop_origin_x
                        || a.desktop_origin_y != b.desktop_origin_y
                        || a.pixel_width != b.pixel_width
                        || a.pixel_height != b.pixel_height
                        || a.rotation != b.rotation
                        || a.primary != b.primary) {
                        return false;
                    }
                }
                return true;
            };
            if (!refreshed_displays.empty()
                && !same_catalog(refreshed_displays, stream_capture_displays)) {
                const std::string current_id = stream_selected_display.has_value()
                    ? stream_selected_display->id : std::string{};
                stream_capture_displays = std::move(refreshed_displays);
                ++stream_navigation_catalog_revision;
                const auto current = std::find_if(
                    stream_capture_displays.begin(),
                    stream_capture_displays.end(),
                    [&](const auto& display) { return display.id == current_id; });
                if (current != stream_capture_displays.end()) {
                    stream_selected_display = *current;
                } else {
                    const auto primary = std::find_if(
                        stream_capture_displays.begin(),
                        stream_capture_displays.end(),
                        [](const auto& display) { return display.primary; });
                    stream_selected_display = primary != stream_capture_displays.end()
                        ? *primary : stream_capture_displays.front();
                    stream_active_capture_region = redclaw::capture::normalize_capture_region(
                        redclaw::capture::CaptureRegion{
                            .revision = stream_capture_region_revision + 1},
                        stream_selected_display->pixel_width,
                        stream_selected_display->pixel_height);
                    stream_capture_region_revision = stream_active_capture_region.revision;
                    if (stream_capture_started) {
                        stream_capture_session.stop();
                        stream_capture_started = false;
                    }
                    remote_input_session.pause(
                        redclaw::input::RemoteInputPauseReason::kGeometryChanged);
                    stream_keyframe_refresh_request_generation.fetch_add(1);
                }
                stream_last_navigation_thumbnail_ms = 0;
                send_desktop_display_catalog();
            }
        }

        std::optional<redclaw::protocol::StreamControlMessageV1> region_request;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (stream_pending_capture_region_request.has_value()) {
                region_request = std::move(stream_pending_capture_region_request);
                stream_pending_capture_region_request.reset();
            }
        }
        if (region_request.has_value()) {
            const auto requested_display = std::find_if(
                stream_capture_displays.begin(),
                stream_capture_displays.end(),
                [&](const redclaw::capture::CaptureDisplayDescriptor& display) {
                    return display.id == region_request->display_id;
                });
            const bool transfer_busy = transfer_operation_gate.blocks_mutation();
            if (transfer_busy || requested_display == stream_capture_displays.end()) {
                auto rejected = make_control_message(
                    redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRejected);
                rejected.display_id = region_request->display_id;
                rejected.region_left = region_request->region_left;
                rejected.region_top = region_request->region_top;
                rejected.region_right = region_request->region_right;
                rejected.region_bottom = region_request->region_bottom;
                rejected.capture_region_revision = region_request->capture_region_revision;
                rejected.payload = transfer_busy ? "workspace_transfer_busy" : "display_unavailable";
                std::string rejected_error;
                (void)send_control_message(rejected, &rejected_error);
            } else {
                const auto region = redclaw::capture::capture_region_from_normalized_bounds(
                    region_request->region_left,
                    region_request->region_top,
                    region_request->region_right,
                    region_request->region_bottom,
                    requested_display->pixel_width,
                    requested_display->pixel_height,
                    region_request->capture_region_revision);
                const bool display_changed = !stream_selected_display.has_value()
                    || stream_selected_display->id != requested_display->id;
                if (display_changed) {
                    stream_capture_region_rollback_display = stream_selected_display;
                    stream_capture_region_rollback_region = stream_active_capture_region;
                    if (stream_capture_started) {
                        stream_capture_session.stop();
                        stream_capture_started = false;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    stream_selected_display = *requested_display;
                    stream_active_capture_region = region;
                    stream_capture_region_apply_pending = *region_request;
                    pending_remote_input_messages.clear();
                    if (display_changed) {
                        remote_input_session.pause(
                            redclaw::input::RemoteInputPauseReason::kGeometryChanged);
                    } else {
                        remote_input_session.release_all();
                    }
                }
                {
                    std::lock_guard<std::mutex> frame_lock(stream_capture_frame_mutex);
                    stream_latest_captured_frame.reset();
                    stream_last_encoded_capture_sequence = stream_latest_captured_frame_sequence;
                }
                stream_media_pacer.reset();
                stream_encoder_session.request_keyframe();
                stream_keyframe_refresh_request_generation.fetch_add(1);
            }
        }

        if (!stream_capture_started) {
            redclaw::capture::CaptureSessionConfig capture_config;
            if (process_logger) {
                capture_config.local_evidence_directory = (process_logger->log_path().parent_path() / "capture-evidence").string();
            }
            if (stream_selected_display.has_value()) {
                capture_config.adapter_index = stream_selected_display->adapter_index;
                capture_config.output_index = stream_selected_display->output_index;
                capture_config.display_id = stream_selected_display->id;
            }
            capture_config.frame_acquire_timeout_ms = kDesktopStreamCaptureAcquireTimeoutMs;
            capture_config.capture_native_d3d11_textures = true;
            capture_config.skip_cpu_readback_when_native_texture_available =
                stream_capture_skip_cpu_readback;
            std::string start_error;
            if (!stream_capture_session.start(capture_config, &start_error)) {
                const auto unavailable = stream_capture_session.telemetry();
                stream_capture_started = unavailable.availability == redclaw::capture::CaptureAvailability::kPaused;
                runtime_loop_wake.notify();
                std::optional<redclaw::protocol::StreamControlMessageV1> failed_region;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    failed_region = std::move(stream_capture_region_apply_pending);
                    stream_capture_region_apply_pending.reset();
                    if (stream_capture_region_rollback_display.has_value()) {
                        stream_selected_display = stream_capture_region_rollback_display;
                        stream_active_capture_region = stream_capture_region_rollback_region;
                        stream_capture_region_rollback_display.reset();
                    }
                }
                if (failed_region.has_value()) {
                    auto rejected = make_control_message(
                        redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRejected);
                    rejected.display_id = failed_region->display_id;
                    rejected.region_left = failed_region->region_left;
                    rejected.region_top = failed_region->region_top;
                    rejected.region_right = failed_region->region_right;
                    rejected.region_bottom = failed_region->region_bottom;
                    rejected.capture_region_revision = failed_region->capture_region_revision;
                    rejected.payload = "capture_switch_failed";
                    std::string rejected_error;
                    (void)send_control_message(rejected, &rejected_error);
                }
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    ++stream_counters.capture_failures;
                    stream_counters.last_error = start_error;
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "stream",
                        "desktop capture start failed; encoded media waits for real capture: " + start_error);
                }
                stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
                return HostStreamCaptureTickResult::kFailure;
            }

            stream_capture_started = true;
            const auto capture_telemetry = stream_capture_session.telemetry();
            const std::string capture_backend = capture_backend_to_string(capture_telemetry.active_backend);
            std::cout << "Runtime desktop capture session started role=" << role_name
                      << " backend=" << capture_backend
                      << " fallback_attempts=" << capture_telemetry.fallback_attempt_count
                      << " fallback_reason=" << std::quoted(capture_telemetry.last_fallback_reason)
                      << '\n';
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                stream_capture_telemetry_snapshot = capture_telemetry;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kInfo,
                    "stream",
                    "desktop capture session started for stream smoke backend=" + capture_backend);
            }
        }

        redclaw::capture::CapturedFrame captured_frame;
        std::string capture_error;
        const std::uint64_t navigation_now_ms = now_steady_ms();
        bool navigation_thumbnail_due = false;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            navigation_thumbnail_due = stream_navigation_channel_open
                && (stream_last_navigation_thumbnail_ms == 0
                    || navigation_now_ms - stream_last_navigation_thumbnail_ms >= 1000
                    || stream_capture_region_apply_pending.has_value());
        }
        stream_capture_session.configureNativeFrameDelivery(
            true,
            stream_capture_skip_cpu_readback && !navigation_thumbnail_due);
        set_stream_worker_stage(
            &stream_capture_worker_stage,
            &stream_capture_worker_stage_since_ms,
            HostStreamWorkerStage::kCaptureFrame);
        {
            std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
            stream_source_activity_tracker.on_capture_poll_started(now_steady_ms());
        }
        const auto capture_start = std::chrono::steady_clock::now();
        if (!stream_capture_session.captureFrame(&captured_frame, &capture_error)) {
            stream_capture_session.configureNativeFrameDelivery(
                true, stream_capture_skip_cpu_readback);
            set_stream_worker_stage(
                &stream_capture_worker_stage,
                &stream_capture_worker_stage_since_ms,
                HostStreamWorkerStage::kPublishCapture);
            const auto capture_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - capture_start)
                    .count());
            const auto capture_telemetry = stream_capture_session.telemetry();
            const bool capture_timeout = capture_error == "timeout waiting for desktop frame";
            const bool availability_changed = apply_capture_availability(capture_telemetry);
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                stream_host_stage_telemetry_snapshot.total_capture_us += capture_us;
                stream_capture_telemetry_snapshot = capture_telemetry;
            }
            if (capture_telemetry.availability == redclaw::capture::CaptureAvailability::kPaused
                && !availability_changed) {
                return HostStreamCaptureTickResult::kNoWork;
            }
            if (capture_timeout) {
                redclaw::session::DesktopSourceActivityUpdate activity_update;
                {
                    std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
                    activity_update = stream_source_activity_tracker.on_capture_timeout(
                        now_steady_ms());
                }
                apply_source_activity_update(activity_update);
                return HostStreamCaptureTickResult::kCaptureAttempted;
            }
            redclaw::session::DesktopSourceActivityUpdate activity_update;
            {
                std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
                activity_update = stream_source_activity_tracker.on_capture_failure(
                    now_steady_ms());
            }
            apply_source_activity_update(activity_update);
            (void)publish_source_activity(activity_update.snapshot);
            std::optional<redclaw::protocol::StreamControlMessageV1> failed_region;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                if (stream_capture_region_apply_pending.has_value()
                    && stream_capture_region_rollback_display.has_value()) {
                    failed_region = std::move(stream_capture_region_apply_pending);
                    stream_capture_region_apply_pending.reset();
                    stream_selected_display = stream_capture_region_rollback_display;
                    stream_active_capture_region = stream_capture_region_rollback_region;
                    stream_capture_region_rollback_display.reset();
                }
            }
            if (failed_region.has_value()) {
                stream_capture_session.stop();
                stream_capture_started = false;
                auto rejected = make_control_message(
                    redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRejected);
                rejected.display_id = failed_region->display_id;
                rejected.region_left = failed_region->region_left;
                rejected.region_top = failed_region->region_top;
                rejected.region_right = failed_region->region_right;
                rejected.region_bottom = failed_region->region_bottom;
                rejected.capture_region_revision = failed_region->capture_region_revision;
                rejected.payload = "capture_switch_failed";
                std::string rejected_error;
                (void)send_control_message(rejected, &rejected_error);
            }
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                ++stream_counters.capture_failures;
                stream_counters.last_error = capture_error;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream",
                    "desktop frame capture failed; encoded media waits for a real frame: " + capture_error);
            }
            stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
            return HostStreamCaptureTickResult::kFailure;
        }
        stream_capture_session.configureNativeFrameDelivery(
            true, stream_capture_skip_cpu_readback);

        (void)apply_capture_availability(stream_capture_session.telemetry());

        const auto capture_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - capture_start)
                .count());
        set_stream_worker_stage(
            &stream_capture_worker_stage,
            &stream_capture_worker_stage_since_ms,
            HostStreamWorkerStage::kPublishCapture);
        const std::uint32_t captured_width = captured_frame.width;
        const std::uint32_t captured_height = captured_frame.height;
        std::optional<redclaw::protocol::StreamControlMessageV1> applied_region;
        redclaw::capture::CaptureRegion active_region;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            active_region = redclaw::capture::normalize_capture_region(
                stream_active_capture_region,
                captured_width,
                captured_height);
            stream_active_capture_region = active_region;
            stream_capture_region_revision = active_region.revision;
            if (stream_capture_region_apply_pending.has_value()) {
                applied_region = std::move(stream_capture_region_apply_pending);
                stream_capture_region_apply_pending.reset();
                stream_capture_region_rollback_display.reset();
            }
        }
        captured_frame.source_region = active_region;
        if (navigation_thumbnail_due && stream_selected_display.has_value()) {
            std::vector<std::uint8_t> jpeg;
            std::uint32_t thumbnail_width = 0;
            std::uint32_t thumbnail_height = 0;
            std::string thumbnail_error;
            if (redclaw::capture::encode_navigation_thumbnail_jpeg(
                    captured_frame,
                    320,
                    &jpeg,
                    &thumbnail_width,
                    &thumbnail_height,
                    &thumbnail_error)) {
                redclaw::net::NavigationThumbnailView thumbnail;
                thumbnail.catalog_revision = stream_navigation_catalog_revision;
                thumbnail.thumbnail_revision = ++stream_navigation_thumbnail_revision;
                thumbnail.width = thumbnail_width;
                thumbnail.height = thumbnail_height;
                thumbnail.display_id = stream_selected_display->id;
                thumbnail.jpeg = jpeg;
                std::vector<std::uint8_t> packet;
                if (redclaw::net::serialize_navigation_thumbnail(
                        thumbnail, &packet, &thumbnail_error)) {
                    redclaw::net::DataChannelSendOutcome outcome;
                    if (ice_wrapper.sendDataChannelBinaryMessage(
                            redclaw::net::DataChannelKind::kNavigation,
                            packet,
                            &thumbnail_error,
                            &outcome)) {
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        stream_last_navigation_thumbnail_ms = navigation_now_ms;
                    }
                }
            }
            if (!thumbnail_error.empty()) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "navigation",
                    "navigation thumbnail update failed: " + thumbnail_error);
            }
        }
        redclaw::input::DesktopGeometry captured_geometry;
        captured_geometry.origin_x = captured_frame.desktop_origin_x;
        captured_geometry.origin_y = captured_frame.desktop_origin_y;
        captured_geometry.width = captured_frame.desktop_width;
        captured_geometry.height = captured_frame.desktop_height;
        captured_geometry.rotation = captured_frame.desktop_rotation;
        captured_geometry.revision = active_region.revision;
        auto latest_frame = std::make_shared<redclaw::capture::CapturedFrame>(std::move(captured_frame));
        {
            std::lock_guard<std::mutex> frame_lock(stream_capture_frame_mutex);
            stream_latest_captured_frame = std::move(latest_frame);
            ++stream_latest_captured_frame_sequence;
            stream_latest_captured_frame_ready_ms = now_steady_ms();
        }
        stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
        stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kNewCapture);

        redclaw::session::DesktopSourceActivityUpdate activity_update;
        std::uint64_t activity_geometry_revision = 0;
        std::uint64_t activity_rate_revision = 0;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            activity_geometry_revision = stream_geometry_revision;
            activity_rate_revision = stream_adaptive_control_snapshot.rate_revision;
        }
        {
            std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
            activity_update = stream_source_activity_tracker.on_capture_frame(
                now_steady_ms(),
                activity_geometry_revision,
                activity_rate_revision);
        }
        apply_source_activity_update(activity_update);

        if (applied_region.has_value()) {
            const std::uint32_t canvas_width = stream_encoder_width >= 2
                ? stream_encoder_width
                : (captured_width & ~1U);
            const std::uint32_t canvas_height = stream_encoder_height >= 2
                ? stream_encoder_height
                : (captured_height & ~1U);
            const auto content = redclaw::capture::resolve_capture_region_content_rect(
                active_region,
                canvas_width,
                canvas_height);
            auto applied = make_control_message(
                redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionApplied);
            applied.display_id = applied_region->display_id;
            applied.region_left = applied_region->region_left;
            applied.region_top = applied_region->region_top;
            applied.region_right = applied_region->region_right;
            applied.region_bottom = applied_region->region_bottom;
            applied.capture_region_revision = active_region.revision;
            applied.encoded_width = canvas_width;
            applied.encoded_height = canvas_height;
            applied.content_rect_x = content.x;
            applied.content_rect_y = content.y;
            applied.content_rect_width = content.width;
            applied.content_rect_height = content.height;
            std::string applied_error;
            (void)send_control_message(applied, &applied_error);
        }

        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            stream_host_stage_telemetry_snapshot.total_capture_us += capture_us;
            stream_capture_telemetry_snapshot = stream_capture_session.telemetry();
            stream_source_width_snapshot = captured_width;
            stream_source_height_snapshot = captured_height;
            remote_input_geometry = captured_geometry;
            ++stream_counters.captured_frames;
        }
        return HostStreamCaptureTickResult::kCaptureAttempted;
    };

    auto run_host_stream_smoke_tick = [&](std::uint64_t now_ms) {
        if (!options.stream_smoke || options.role != RuntimeRole::kHost) {
            return;
        }
        if (stream_media_submission_paused.load()) {
            set_stream_worker_stage(
                &stream_encoder_worker_stage,
                &stream_encoder_worker_stage_since_ms,
                HostStreamWorkerStage::kWaitingForChannel);
            return;
        }
        set_stream_worker_stage(
            &stream_encoder_worker_stage,
            &stream_encoder_worker_stage_since_ms,
            HostStreamWorkerStage::kWaitingForEncoder);
        std::lock_guard<std::mutex> encoder_lock(stream_encoder_mutex);

        bool required_channels_open = false;
        bool session_recovering = false;
        std::uint64_t required_channels_ready_at_ms = 0;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            required_channels_open = stream_media_channel_open && stream_control_channel_open;
            session_recovering = saw_failed_state || required_channel_closed_at_ms != 0;
            required_channels_ready_at_ms = stream_required_channels_ready_at_ms;
        }
        if (!required_channels_open || session_recovering) {
            return;
        }

        {
            std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
            const auto state = stream_source_activity_tracker.snapshot().state;
            if (state != redclaw::session::DesktopSourceActivityState::kActive
                && state != redclaw::session::DesktopSourceActivityState::kStaticPending) {
                return;
            }
        }

        // Encoding is admitted only when the pacer has room for one latest
        // pending frame. The capture side already owns the two-slot
        // latest-frame policy, so network pressure skips old capture
        // opportunities without damaging the encoder prediction chain.
        if (!stream_media_pacer.can_accept_frame()) {
            return;
        }
        const bool pacer_keyframe_required =
            stream_media_pacer.telemetry().keyframe_required;

        std::shared_ptr<redclaw::capture::CapturedFrame> captured_frame;
        std::uint64_t capture_sequence = 0;
        std::uint64_t capture_ready_ms = 0;
        std::uint64_t keyframe_refresh_generation = 0;
        bool static_keyframe_refresh = false;
        {
            std::lock_guard<std::mutex> frame_lock(stream_capture_frame_mutex);
            keyframe_refresh_generation = stream_keyframe_refresh_request_generation.load();
            static_keyframe_refresh = pacer_keyframe_required
                || keyframe_refresh_generation
                    > stream_keyframe_refresh_completed_generation.load();
            if (stream_latest_captured_frame
                && (stream_latest_captured_frame_sequence != stream_last_encoded_capture_sequence
                    || static_keyframe_refresh)) {
                captured_frame = stream_latest_captured_frame;
                capture_sequence = stream_latest_captured_frame_sequence;
                capture_ready_ms = static_keyframe_refresh
                    ? now_ms
                    : stream_latest_captured_frame_ready_ms;
            }
        }
        if (!captured_frame || !stream_capture_gate.accepts(captured_frame->capture_generation)) {
            return;
        }

        StreamAdaptiveControlState adaptive_control;
        const auto encoder_backend_before_reconfigure = stream_encoder_started
            ? stream_encoder_session.diagnostics().backend
            : redclaw::capture::EncoderBackendType::kSoftware;
        std::uint32_t requested_viewport_width = 0;
        std::uint32_t requested_viewport_height = 0;
        std::uint64_t geometry_transaction_id = 0;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            adaptive_control = stream_adaptive_control_snapshot;
            requested_viewport_width = stream_requested_viewport_width;
            requested_viewport_height = stream_requested_viewport_height;
            geometry_transaction_id = stream_geometry_transaction_id;
        }
        const bool viewport_ready = requested_viewport_width >= 64
            && requested_viewport_height >= 64;
        if (redclaw::capture::should_defer_initial_encoder_start_for_viewport(
                stream_encoder_started,
                viewport_ready,
                required_channels_ready_at_ms,
                now_ms,
                kDesktopStreamInitialViewportGraceMs)) {
            set_stream_worker_stage(
                &stream_encoder_worker_stage,
                &stream_encoder_worker_stage_since_ms,
                HostStreamWorkerStage::kWaitingForViewport);
            return;
        }
        const std::uint32_t adaptive_target_fps = clamp_stream_target_fps(
            adaptive_control.target_fps == 0 ? kDesktopStreamTargetFps : adaptive_control.target_fps);
        const std::uint64_t encode_interval_ms = stream_frame_interval_ms(adaptive_target_fps);

        if (next_stream_encode_ms == 0) {
            next_stream_encode_ms = now_ms;
        }
        if (now_ms < next_stream_encode_ms) {
            return;
        }
        if (capture_ready_ms != 0 && capture_ready_ms <= next_stream_encode_ms && now_ms > next_stream_encode_ms) {
            const std::uint32_t skipped_intervals = static_cast<std::uint32_t>(
                (now_ms - next_stream_encode_ms) / encode_interval_ms);
            std::lock_guard<std::mutex> lock(callback_mutex);
            ++stream_host_stage_telemetry_snapshot.missed_deadline_count;
            stream_host_stage_telemetry_snapshot.skipped_interval_count += skipped_intervals;
        }
        // Every encode attempt, including setup failures, consumes one pacing
        // interval. Otherwise an event-driven worker can retry the same frame
        // in a tight loop when a hardware backend is temporarily unavailable.
        next_stream_encode_ms = now_ms + encode_interval_ms;

        bool should_attempt_rate_control_update = false;
        std::uint64_t rate_control_revision = 0;
        std::uint32_t rate_control_target_bitrate_kbps = 0;
        std::uint32_t rate_control_max_bitrate_kbps = 0;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            const bool required_session_active = stream_media_channel_open
                && stream_control_channel_open
                && !saw_failed_state
                && required_channel_closed_at_ms == 0;
            stream_adaptive_control_snapshot.applied_fps =
                resolve_stream_target_fps(stream_adaptive_control_snapshot);
            rate_control_revision = stream_adaptive_control_snapshot.rate_revision;
            rate_control_target_bitrate_kbps =
                resolve_stream_target_bitrate_kbps(stream_adaptive_control_snapshot);
            rate_control_max_bitrate_kbps =
                resolve_stream_target_max_bitrate_kbps(stream_adaptive_control_snapshot);
            should_attempt_rate_control_update = required_session_active
                && stream_encoder_started
                && stream_adaptive_control_snapshot.encoder_reconfigure_pending
                && rate_control_target_bitrate_kbps != 0
                && rate_control_max_bitrate_kbps >= rate_control_target_bitrate_kbps
                && (stream_adaptive_control_snapshot.applied_bitrate_kbps
                        != rate_control_target_bitrate_kbps
                    || stream_adaptive_control_snapshot.applied_max_bitrate_kbps
                        != rate_control_max_bitrate_kbps)
                && stream_adaptive_control_snapshot.last_rate_control_attempt_revision
                    != rate_control_revision;
            if (should_attempt_rate_control_update) {
                stream_adaptive_control_snapshot.last_rate_control_attempt_revision =
                    rate_control_revision;
            }
        }

        if (should_attempt_rate_control_update) {
            std::string rate_control_error;
            const auto update_status = stream_encoder_session.update_rate_control(
                rate_control_target_bitrate_kbps,
                rate_control_max_bitrate_kbps,
                &rate_control_error);
            std::lock_guard<std::mutex> lock(callback_mutex);
            stream_encoder_diagnostics_snapshot = stream_encoder_session.diagnostics();
            const bool required_session_active = stream_media_channel_open
                && stream_control_channel_open
                && !saw_failed_state
                && required_channel_closed_at_ms == 0;
            if (!required_session_active) {
                stream_adaptive_control_snapshot.encoder_reconfigure_pending = false;
                stream_adaptive_control_snapshot.rate_control_restart_fallback_revision = 0;
                return;
            }
            if (update_status
                == redclaw::capture::EncoderRateControlUpdateStatus::kApplied) {
                stream_adaptive_control_snapshot.applied_bitrate_kbps =
                    rate_control_target_bitrate_kbps;
                stream_adaptive_control_snapshot.applied_max_bitrate_kbps =
                    rate_control_max_bitrate_kbps;
                if (stream_adaptive_control_snapshot.rate_revision == rate_control_revision) {
                    stream_adaptive_control_snapshot.encoder_reconfigure_pending = false;
                    stream_adaptive_control_snapshot.rate_control_restart_fallback_revision = 0;
                }
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kInfo,
                    "stream",
                    "encoder rate control updated in place revision="
                        + std::to_string(rate_control_revision)
                        + " bitrate_kbps="
                        + std::to_string(rate_control_target_bitrate_kbps));
            } else {
                if (stream_adaptive_control_snapshot.rate_revision == rate_control_revision) {
                    const bool restart_cooldown_elapsed =
                        stream_adaptive_control_snapshot.last_reconfigure_ms == 0
                        || (now_ms >= stream_adaptive_control_snapshot.last_reconfigure_ms
                            && now_ms - stream_adaptive_control_snapshot.last_reconfigure_ms
                                   >= kDesktopStreamAdaptiveReconfigureCooldownMs);
                    if (restart_cooldown_elapsed) {
                        stream_adaptive_control_snapshot.encoder_reconfigure_pending = true;
                        stream_adaptive_control_snapshot.rate_control_restart_fallback_revision = 0;
                    } else {
                        stream_adaptive_control_snapshot.rate_control_restart_fallback_revision =
                            rate_control_revision;
                    }
                }
                append_timeline(
                    update_status
                            == redclaw::capture::EncoderRateControlUpdateStatus::kUnsupported
                        ? redclaw::render::RuntimeStatusSeverity::kInfo
                        : redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream",
                    "encoder rate control hot update not applied revision="
                        + std::to_string(rate_control_revision)
                        + " detail=" + rate_control_error);
            }
        }

        bool restart_due_to_adaptive_control = false;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            const bool required_session_active = stream_media_channel_open
                && stream_control_channel_open
                && !saw_failed_state
                && required_channel_closed_at_ms == 0;
            if (!required_session_active) {
                stream_adaptive_control_snapshot.encoder_reconfigure_pending = false;
                stream_adaptive_control_snapshot.rate_control_restart_fallback_revision = 0;
            } else if (stream_adaptive_control_snapshot.rate_control_restart_fallback_revision != 0) {
                if (stream_adaptive_control_snapshot.rate_control_restart_fallback_revision
                    != stream_adaptive_control_snapshot.rate_revision) {
                    stream_adaptive_control_snapshot.rate_control_restart_fallback_revision = 0;
                } else if (stream_adaptive_control_snapshot.last_reconfigure_ms == 0
                           || (now_ms >= stream_adaptive_control_snapshot.last_reconfigure_ms
                               && now_ms - stream_adaptive_control_snapshot.last_reconfigure_ms
                                      >= kDesktopStreamAdaptiveReconfigureCooldownMs)) {
                    stream_adaptive_control_snapshot.encoder_reconfigure_pending = true;
                    stream_adaptive_control_snapshot.rate_control_restart_fallback_revision = 0;
                }
            }
            restart_due_to_adaptive_control =
                required_session_active
                && stream_adaptive_control_snapshot.encoder_reconfigure_pending
                && stream_encoder_started;
        }
        if (redclaw::capture::should_preserve_encoder_during_live_reconfiguration(
                encoder_backend_before_reconfigure,
                stream_encoder_started,
                false,
                restart_due_to_adaptive_control)) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (stream_last_preserved_hardware_rate_revision
                != stream_adaptive_control_snapshot.rate_revision) {
                stream_last_preserved_hardware_rate_revision =
                    stream_adaptive_control_snapshot.rate_revision;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kInfo,
                    "stream",
                    "hardware encoder live restart suppressed; pacing keeps the active driver session revision="
                        + std::to_string(stream_adaptive_control_snapshot.rate_revision));
            }
            stream_adaptive_control_snapshot.encoder_reconfigure_pending = false;
            stream_adaptive_control_snapshot.rate_control_restart_fallback_revision = 0;
            restart_due_to_adaptive_control = false;
        }
        if (restart_due_to_adaptive_control) {
            stream_encoder_session.stop();
            stream_encoder_started = false;
        }

        const std::uint32_t effective_video_max_width =
            options.stream_qa_native_size ? 0U
                : resolve_stream_effective_video_max_width(options.stream_video_max_width, adaptive_control);
        std::uint32_t encode_width = 0;
        std::uint32_t encode_height = 0;
        std::string encode_dimension_error;
        const bool encode_dimensions_ready =
            !options.stream_qa_native_size && requested_viewport_width >= 64 && requested_viewport_height >= 64
            ? redclaw::capture::resolve_viewport_encode_dimensions(
                  captured_frame->width,
                  captured_frame->height,
                  requested_viewport_width,
                  requested_viewport_height,
                  effective_video_max_width,
                  &encode_width,
                  &encode_height,
                  &encode_dimension_error)
            : resolve_video_encode_dimensions(
                  captured_frame->width,
                  captured_frame->height,
                  effective_video_max_width,
                  &encode_width,
                  &encode_height,
                  &encode_dimension_error);
        if (!encode_dimensions_ready) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            ++stream_counters.encode_failures;
            stream_counters.last_error = encode_dimension_error;
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kWarning,
                "stream",
                "desktop video encode dimensions failed: " + encode_dimension_error);
            return;
        }

        bool encoder_resolution_change_requested = stream_encoder_started
            && (stream_encoder_width != encode_width || stream_encoder_height != encode_height);
        if (redclaw::capture::should_preserve_encoder_during_live_reconfiguration(
                encoder_backend_before_reconfigure,
                stream_encoder_started,
                encoder_resolution_change_requested,
                false)) {
            if (stream_last_preserved_hardware_geometry_transaction_id != geometry_transaction_id) {
                stream_last_preserved_hardware_geometry_transaction_id = geometry_transaction_id;
                std::lock_guard<std::mutex> lock(callback_mutex);
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kInfo,
                    "stream-control",
                    "hardware encoder geometry preserved width="
                        + std::to_string(stream_encoder_width)
                        + " height=" + std::to_string(stream_encoder_height)
                        + "; Controller scales the active stream");
            }
            encode_width = stream_encoder_width;
            encode_height = stream_encoder_height;
            encoder_resolution_change_requested = false;
        }
        if (encoder_resolution_change_requested
            && adaptive_control.last_reconfigure_ms != 0
            && now_ms >= adaptive_control.last_reconfigure_ms
            && now_ms - adaptive_control.last_reconfigure_ms
                   < kDesktopStreamAdaptiveReconfigureCooldownMs) {
            // Software encoders may still rebuild for a changed viewport, but
            // coalesce rapid GUI geometry changes behind the common cooldown.
            return;
        }

        if (!stream_encoder_started
            || stream_encoder_width != encode_width
            || stream_encoder_height != encode_height
            || restart_due_to_adaptive_control) {
            if (!stream_encoder_started && now_ms < stream_encoder_start_retry_after_ms) {
                stream_last_encoded_capture_sequence = capture_sequence;
                return;
            }

            const bool encoder_resolution_changed = stream_encoder_width != 0
                && (stream_encoder_width != encode_width || stream_encoder_height != encode_height);
            stream_encoder_session.stop();
            stream_encoder_started = false;
            stream_encoder_width = encode_width;
            stream_encoder_height = encode_height;

            redclaw::capture::EncoderProfileRequest profile_request;
            profile_request.width = encode_width;
            profile_request.height = encode_height;
            profile_request.fps = encoder_resolution_changed
                && adaptive_control.applied_fps != 0
                ? adaptive_control.applied_fps
                : adaptive_target_fps;
            profile_request.workload = redclaw::capture::EncoderWorkload::kInteractiveDesktop;
            profile_request.preferred_codec = redclaw::capture::EncoderCodec::kH264;

            redclaw::capture::EncoderConfigProfile encoder_profile;
            std::string encoder_error;
            if (!redclaw::capture::build_low_latency_encoder_profile(
                    profile_request,
                    &encoder_profile,
                    &encoder_error)) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                ++stream_counters.encode_failures;
                stream_counters.last_error = encoder_error;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream",
                    "desktop video encoder profile build failed: " + encoder_error);
                return;
            }

            if (encoder_resolution_changed) {
                // Bitrate belongs to the committed pixel geometry. Carrying a
                // larger viewport's target into this rebuild wastes bandwidth
                // until a later adaptation window. Rebase immediately while
                // preserving the independently tracked pacing target.
                encoder_profile.target_bitrate_kbps =
                    redclaw::capture::resolve_interactive_desktop_bitrate_floor_kbps(
                        encode_width,
                        encode_height,
                        adaptive_target_fps);
                encoder_profile.max_bitrate_kbps = encoder_profile.target_bitrate_kbps
                    + (encoder_profile.target_bitrate_kbps / 5);
            } else if (adaptive_control.target_bitrate_kbps != 0) {
                const std::uint32_t quality_floor_kbps =
                    redclaw::capture::resolve_interactive_desktop_bitrate_floor_kbps(
                        encode_width,
                        encode_height,
                        adaptive_target_fps);
                encoder_profile.target_bitrate_kbps = std::max(
                    adaptive_control.target_bitrate_kbps,
                    quality_floor_kbps);
                encoder_profile.max_bitrate_kbps =
                    adaptive_control.target_max_bitrate_kbps != 0
                        ? std::max(
                              adaptive_control.target_max_bitrate_kbps,
                              encoder_profile.target_bitrate_kbps
                                  + (encoder_profile.target_bitrate_kbps / 5))
                        : (encoder_profile.target_bitrate_kbps
                            + (encoder_profile.target_bitrate_kbps / 5));
            }

            redclaw::capture::EncoderBackendBridgeRequest bridge_request;
            bridge_request.codec = redclaw::capture::EncoderCodec::kH264;
            bridge_request.preferred_backend = redclaw::capture::EncoderBackendType::kAuto;
            bridge_request.allow_hardware_fallback = true;
            // Desktop Duplication capture and D3D11 hardware-frame encoding otherwise share
            // an immediate context across worker threads. Keep the hardware codec backend,
            // but use its CPU BGRA upload path so reconnect cannot strand both workers in the
            // display driver.
            bridge_request.allow_hardware_frame_input = false;
            bridge_request.capture_adapter_vendor =
                redclaw::capture::detect_captured_frame_adapter_vendor(*captured_frame);

            redclaw::capture::EncoderBackendBridgePlan bridge_plan;
            if (!redclaw::capture::start_encoder_execution_from_bridge(
                    encoder_profile,
                    bridge_request,
                    &stream_encoder_session,
                    &bridge_plan,
                    &encoder_error)) {
                stream_encoder_start_retry_after_ms = now_ms + kDesktopStreamEncoderStartRetryCooldownMs;
                stream_last_encoded_capture_sequence = capture_sequence;
                std::lock_guard<std::mutex> lock(callback_mutex);
                ++stream_counters.encode_failures;
                stream_counters.last_error = encoder_error;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "stream",
                    "desktop video encoder start exhausted all backends; retrying after "
                        + std::to_string(kDesktopStreamEncoderStartRetryCooldownMs)
                        + " ms: "
                        + encoder_error);
                return;
            }

            stream_encoder_started = true;
            stream_encoder_start_retry_after_ms = 0;
            const auto encoder_diagnostics = stream_encoder_session.diagnostics();
            const bool prefer_native_capture_only = encoder_diagnostics.hardware_frame_input_active
                && ((encode_width == captured_frame->width
                     && encode_height == captured_frame->height)
                    || encoder_diagnostics.d3d11_video_processor_scaling_active);
            if (stream_capture_skip_cpu_readback != prefer_native_capture_only) {
                stream_capture_session.configureNativeFrameDelivery(true, prefer_native_capture_only);
                stream_capture_skip_cpu_readback = prefer_native_capture_only;
            }
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                const bool encoder_target_changed = encoder_resolution_changed
                    || stream_adaptive_control_snapshot.target_bitrate_kbps
                        != encoder_profile.target_bitrate_kbps
                    || stream_adaptive_control_snapshot.target_max_bitrate_kbps
                        != encoder_profile.max_bitrate_kbps;
                stream_encoder_diagnostics_snapshot = encoder_diagnostics;
                // Publish the dimensions actually owned by the running encoder.
                // A committed viewport may spend one cooldown interval pending;
                // reporting its target dimensions before this start succeeds
                // makes adaptation compute a bitrate for pixels that are not yet
                // being encoded and can trigger a redundant restart fallback.
                stream_encoded_width_snapshot = stream_encoder_width;
                stream_encoded_height_snapshot = stream_encoder_height;
                stream_adaptive_control_snapshot.applied_fps = encoder_profile.fps;
                stream_adaptive_control_snapshot.applied_bitrate_kbps = encoder_profile.target_bitrate_kbps;
                stream_adaptive_control_snapshot.applied_max_bitrate_kbps = encoder_profile.max_bitrate_kbps;
                stream_adaptive_control_snapshot.last_reconfigure_ms = now_ms;
                if (encoder_resolution_changed
                    || stream_adaptive_control_snapshot.target_bitrate_kbps == 0) {
                    stream_adaptive_control_snapshot.target_bitrate_kbps = encoder_profile.target_bitrate_kbps;
                    stream_adaptive_control_snapshot.target_max_bitrate_kbps = encoder_profile.max_bitrate_kbps;
                }
                if (encoder_target_changed) {
                    ++stream_adaptive_control_snapshot.rate_revision;
                }
                if (restart_due_to_adaptive_control) {
                    ++stream_adaptive_control_snapshot.encoder_restart_count;
                }
                if (encoder_resolution_changed) {
                    ++stream_encoder_resolution_reconfigure_total;
                }
                stream_adaptive_control_snapshot.encoder_reconfigure_pending = false;
                stream_adaptive_control_snapshot.rate_control_restart_fallback_revision = 0;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kInfo,
                    "stream",
                    "desktop video encoder started backend="
                        + encoder_backend_to_string(encoder_diagnostics.backend)
                        + " encoder="
                        + (encoder_diagnostics.encoder_name.empty() ? std::string("n/a") : encoder_diagnostics.encoder_name)
                        + " input_mode="
                        + (encoder_diagnostics.input_mode.empty() ? std::string("n/a") : encoder_diagnostics.input_mode)
                        + " gpu_scale="
                        + (encoder_diagnostics.d3d11_video_processor_scaling_active ? "true" : "false")
                        + " gpu_to_cpu_readback="
                        + (encoder_diagnostics.gpu_to_cpu_readback_active ? "true" : "false")
                        + " fps="
                        + std::to_string(encoder_profile.fps)
                        + " bitrate_kbps="
                        + std::to_string(encoder_profile.target_bitrate_kbps));
            }

            std::cout << "Runtime desktop video encoder started role=" << role_name
                      << " backend=" << encoder_backend_to_string(encoder_diagnostics.backend)
                      << " encoder="
                      << (encoder_diagnostics.encoder_name.empty() ? "n/a" : encoder_diagnostics.encoder_name)
                      << " input_mode="
                      << (encoder_diagnostics.input_mode.empty() ? "n/a" : encoder_diagnostics.input_mode)
                      << " gpu_scale="
                      << (encoder_diagnostics.d3d11_video_processor_scaling_active ? "true" : "false")
                      << " gpu_to_cpu_readback="
                      << (encoder_diagnostics.gpu_to_cpu_readback_active ? "true" : "false")
                      << " fps=" << encoder_profile.fps
                      << " gop_frames=" << encoder_profile.gop_length_frames
                      << " b_frames=" << encoder_profile.b_frames
                      << " lookahead=" << (encoder_profile.lookahead_enabled ? "true" : "false")
                      << " zero_latency=" << (encoder_profile.zero_latency_tuning ? "true" : "false")
                      << " target_bitrate_kbps=" << encoder_profile.target_bitrate_kbps
                      << " max_bitrate_kbps=" << encoder_profile.max_bitrate_kbps
                      << " bridge_reason=" << bridge_plan.reason
                      << '\n';

        }

        std::uint64_t encoded_rate_revision = 0;
        std::uint32_t encoded_target_fps = 0;
        std::uint32_t encoded_target_bitrate_kbps = 0;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            encoded_rate_revision = stream_adaptive_control_snapshot.rate_revision;
            encoded_target_fps = resolve_stream_target_fps(stream_adaptive_control_snapshot);
            encoded_target_bitrate_kbps =
                resolve_stream_target_bitrate_kbps(stream_adaptive_control_snapshot);
        }

        redclaw::capture::EncodedFramePacket encoded_packet;
        std::string encode_error;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            ++stream_host_stage_telemetry_snapshot.encode_submit_attempt_count;
        }
        if (static_keyframe_refresh) {
            // Desktop Duplication is event driven and does not produce a new
            // capture sequence for an unchanged desktop. A lost startup/IDR
            // frame must still be recoverable, so re-encode the retained latest
            // capture as an IDR without creating another captured-frame slot.
            stream_encoder_session.request_keyframe();
            std::lock_guard<std::mutex> lock(callback_mutex);
            ++stream_static_keyframe_refresh_attempt_total;
        }
        set_stream_worker_stage(
            &stream_encoder_worker_stage,
            &stream_encoder_worker_stage_since_ms,
            HostStreamWorkerStage::kEncodeFrame);
        if (!stream_encoder_session.encode_bgra_frame(
                *captured_frame,
                now_unix_ms(),
                &encoded_packet,
                &encode_error)) {
            const auto encoder_diagnostics = stream_encoder_session.diagnostics();
            const bool output_not_ready =
                encoder_diagnostics.last_failure
                == redclaw::capture::EncoderExecutionFailureCategory::kOutputNotReady;
            const bool prefer_native_capture_only = encoder_diagnostics.hardware_frame_input_active
                && ((encode_width == captured_frame->width
                     && encode_height == captured_frame->height)
                    || encoder_diagnostics.d3d11_video_processor_scaling_active);
            if (stream_capture_skip_cpu_readback != prefer_native_capture_only) {
                stream_capture_session.configureNativeFrameDelivery(true, prefer_native_capture_only);
                stream_capture_skip_cpu_readback = prefer_native_capture_only;
            }
            std::lock_guard<std::mutex> lock(callback_mutex);
            stream_encoder_diagnostics_snapshot = encoder_diagnostics;
            if (output_not_ready) {
                ++stream_host_stage_telemetry_snapshot.encode_output_not_ready_count;
                return;
            }
            stream_last_encoded_capture_sequence = capture_sequence;
            ++stream_counters.encode_failures;
            stream_counters.last_error = encode_error;
            append_timeline(
                redclaw::render::RuntimeStatusSeverity::kWarning,
                "stream",
                "desktop video encode failed: " + encode_error);
            return;
        }

        redclaw::render::EncodedVideoFrame encoded_frame;
        encoded_frame.codec = to_render_codec(encoded_packet.codec);
        encoded_frame.width = encode_width;
        encoded_frame.height = encode_height;
        encoded_frame.timestamp_ms = encoded_packet.timestamp_ms;
        encoded_frame.keyframe = encoded_packet.keyframe;
        encoded_frame.payload = std::move(encoded_packet.payload);
        const auto encoder_diagnostics = stream_encoder_session.diagnostics();
        const bool prefer_native_capture_only = encoder_diagnostics.hardware_frame_input_active
            && ((encode_width == captured_frame->width
                 && encode_height == captured_frame->height)
                || encoder_diagnostics.d3d11_video_processor_scaling_active);
        if (stream_capture_skip_cpu_readback != prefer_native_capture_only) {
            stream_capture_session.configureNativeFrameDelivery(true, prefer_native_capture_only);
            stream_capture_skip_cpu_readback = prefer_native_capture_only;
        }
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            stream_encoder_diagnostics_snapshot = encoder_diagnostics;
            ++stream_counters.encoded_frames;
        }
        stream_last_encoded_capture_sequence = capture_sequence;
        const std::uint64_t frame_id = ++stream_video_frame_id;
        encoded_frame.frame_id = frame_id;
        redclaw::net::PacedEncodedVideoFrame paced_frame;
        paced_frame.frame_id = frame_id;
        paced_frame.rate_revision = encoded_rate_revision;
        paced_frame.capture_region_revision =
            captured_frame->source_region.revision;
        paced_frame.codec = encoded_video_codec_to_binary(encoded_frame.codec);
        paced_frame.width = encoded_frame.width;
        paced_frame.height = encoded_frame.height;
        paced_frame.timestamp_ms = encoded_frame.timestamp_ms;
        paced_frame.target_fps = encoded_target_fps;
        paced_frame.target_bitrate_kbps = encoded_target_bitrate_kbps;
        paced_frame.keyframe_refresh_generation = static_keyframe_refresh
            ? keyframe_refresh_generation
            : 0;
        paced_frame.keyframe = encoded_frame.keyframe;
        const auto encoded_content = redclaw::capture::resolve_capture_region_content_rect(
            captured_frame->source_region,
            encoded_frame.width,
            encoded_frame.height);
        paced_frame.content_rect_x = encoded_content.x;
        paced_frame.content_rect_y = encoded_content.y;
        paced_frame.content_rect_width = encoded_content.width;
        paced_frame.content_rect_height = encoded_content.height;
        paced_frame.payload = std::move(encoded_frame.payload);
        set_stream_worker_stage(
            &stream_encoder_worker_stage,
            &stream_encoder_worker_stage_since_ms,
            HostStreamWorkerStage::kSubmitFrame);
        if (stream_media_submission_paused.load()
            || !stream_capture_gate.accepts(captured_frame->capture_generation)) {
            return;
        }
        redclaw::net::MediaAdmissionResult admission;
        if (!stream_capture_gate.submit(captured_frame->capture_generation, [&]() {
                admission = stream_media_pacer.submit_frame(std::move(paced_frame));
            })) { return; }
        if (!admission.ready()) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            ++stream_counters.transmit_backpressure_drops;
            stream_counters.last_error = "encoded frame rejected by media pacer frame_id="
                + std::to_string(frame_id) + " admission="
                + std::to_string(static_cast<unsigned>(admission.state));
            return;
        }

        if (encoded_frame.keyframe) {
            const bool first_capture_keyframe = stream_capture_gate.submitted_keyframe(captured_frame->capture_generation, frame_id);
            redclaw::session::DesktopSourceActivityUpdate activity_update;
            {
                std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
                activity_update = stream_source_activity_tracker.on_reference_submitted(
                    frame_id, frame_id, now_steady_ms(), encoded_rate_revision);
            }
            apply_source_activity_update(activity_update);
            if (first_capture_keyframe) { (void)publish_source_activity(activity_update.snapshot); }
        }

        if (encoded_frame.keyframe
            && requested_viewport_width >= 64
            && requested_viewport_height >= 64
            && geometry_transaction_id != 0
            && geometry_transaction_id != stream_last_applied_geometry_transaction_id) {
            auto applied = make_control_message(
                redclaw::protocol::StreamControlMessageTypeV1::kStreamTargetApplied);
            applied.encoded_width = stream_encoder_width;
            applied.encoded_height = stream_encoder_height;
            applied.target_fps = adaptive_target_fps;
            applied.target_bitrate_kbps = encoded_target_bitrate_kbps;
            applied.log_cursor = geometry_transaction_id;
            std::string applied_error;
            if (send_control_message(applied, &applied_error)) {
                stream_last_applied_geometry_transaction_id = geometry_transaction_id;
            }
        }
    };

    std::atomic<bool> stream_capture_worker_running = false;
    std::thread stream_capture_worker;
    std::atomic<bool> stream_worker_running = false;
    std::thread stream_worker;
    auto stop_stream_workers = [&]() {
        stream_worker_running.store(false);
        stream_capture_worker_running.store(false);
        stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kStateChanged);
        if (stream_worker.joinable()) {
            stream_worker.join();
        }
        if (stream_capture_worker.joinable()) {
            stream_capture_worker.join();
        }
        stream_media_pacer.stop();
    };
    struct StreamWorkerScope final {
        std::function<void()> stop;
        ~StreamWorkerScope() {
            if (stop) {
                stop();
            }
        }
    } stream_worker_scope{stop_stream_workers};

    if (options.stream_smoke && options.role == RuntimeRole::kHost) {
        stream_capture_worker_running.store(true);
        stream_capture_worker = std::thread([&]() {
            while (stream_capture_worker_running.load()) {
                const auto observed = stream_work_coordinator.snapshot().generation;
                const auto tick_result = run_host_stream_capture_tick();
                if (stream_capture_gate.snapshot().availability == redclaw::capture::CaptureAvailability::kPaused) {
                    // Reuse the coordinator for explicit retry/stop. The bounded
                    // deadline only probes desktop/display state, never a backend.
                    (void)stream_work_coordinator.wait_for_change(observed, std::chrono::milliseconds(500));
                } else if (tick_result != HostStreamCaptureTickResult::kCaptureAttempted) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kDesktopStreamCaptureIdleWaitMs));
                }
            }
            set_stream_worker_stage(
                &stream_capture_worker_stage,
                &stream_capture_worker_stage_since_ms,
                HostStreamWorkerStage::kStopped);

            redclaw::capture::CaptureBackendTelemetry capture_telemetry;
            if (stream_capture_started) {
                capture_telemetry = stream_capture_session.telemetry();
                stream_capture_session.stop();
                stream_capture_skip_cpu_readback = false;
            }
            {
                std::lock_guard<std::mutex> frame_lock(stream_capture_frame_mutex);
                stream_latest_captured_frame.reset();
                stream_latest_captured_frame_sequence = 0;
                stream_latest_captured_frame_ready_ms = 0;
            }
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                stream_capture_telemetry_snapshot = capture_telemetry;
                stream_capture_started = false;
                stream_source_width_snapshot = 0;
                stream_source_height_snapshot = 0;
                stream_encoded_width_snapshot = 0;
                stream_encoded_height_snapshot = 0;
            }
        });

        stream_worker_running.store(true);
        stream_worker = std::thread([&]() {
            while (stream_worker_running.load()) {
                const auto observed = stream_work_coordinator.snapshot().generation;
                redclaw::session::HostStreamEncodeReadiness readiness;
                readiness.due_ms = next_stream_encode_ms;
                std::uint64_t geometry_revision = 0;
                {
                    std::lock_guard lock(callback_mutex);
                    readiness.channels_ready = stream_media_channel_open
                        && stream_control_channel_open && !saw_failed_state
                        && required_channel_closed_at_ms == 0
                        && !stream_media_submission_paused.load();
                    geometry_revision = stream_geometry_revision;
                    if (!stream_encoder_started && (stream_requested_viewport_width < 64
                            || stream_requested_viewport_height < 64)) {
                        readiness.due_ms = std::max(readiness.due_ms,
                            stream_required_channels_ready_at_ms + kDesktopStreamInitialViewportGraceMs);
                    }
                }
                if (readiness.channels_ready && stream_pacer_geometry_revision != geometry_revision) {
                    // Cancel old geometry callbacks without resetting the v3
                    // transport sequence within the same connection epoch.
                    stream_media_pacer.reset(false);
                    stream_pacer_geometry_revision = geometry_revision;
                }
                {
                    std::lock_guard lock(stream_source_activity_mutex);
                    const auto state = stream_source_activity_tracker.snapshot().state;
                    readiness.source_active = state == redclaw::session::DesktopSourceActivityState::kActive
                        || state == redclaw::session::DesktopSourceActivityState::kStaticPending;
                }
                const auto admission = stream_media_pacer.admission();
                readiness.capacity_available = admission.ready()
                    || admission.state == redclaw::net::MediaAdmissionState::kBudgetInfeasible;
                if (admission.state == redclaw::net::MediaAdmissionState::kBudgetInfeasible) {
                    readiness.due_ms = std::max(readiness.due_ms, admission.next_check_ms);
                }
                const bool recovery_pending = stream_media_pacer.telemetry().keyframe_required;
                {
                    std::lock_guard lock(stream_capture_frame_mutex);
                    readiness.frame_pending = stream_latest_captured_frame
                        && (recovery_pending
                            || stream_latest_captured_frame_sequence != stream_last_encoded_capture_sequence
                            || stream_keyframe_refresh_request_generation.load()
                                > stream_keyframe_refresh_completed_generation.load());
                }
                const auto wait = redclaw::session::host_stream_encode_wait(readiness, now_steady_ms());
                if (wait != std::chrono::milliseconds::zero()) {
                    (void)stream_work_coordinator.wait_for_change(observed, wait);
                    continue;
                }
                run_host_stream_smoke_tick(now_steady_ms());
                set_stream_worker_stage(
                    &stream_encoder_worker_stage,
                    &stream_encoder_worker_stage_since_ms,
                    HostStreamWorkerStage::kWaitingForFrame);

            }
            set_stream_worker_stage(
                &stream_encoder_worker_stage,
                &stream_encoder_worker_stage_since_ms,
                HostStreamWorkerStage::kStopped);

            const auto encoder_diagnostics = stream_encoder_session.diagnostics();
            if (!encoder_diagnostics.hardware_frame_input_active) {
                stream_encoder_session.stop();
            }
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                stream_encoder_started = false;
            }
        });
    }

    const std::uint64_t runtime_loop_started_ms = now_steady_ms();
    std::uint32_t elapsed_seconds = 0;
    DesktopStreamSmokeCounters last_stream_rate_snapshot;
    DesktopStreamSmokeCounters last_stream_adaptation_snapshot;
    redclaw::capture::CaptureBackendTelemetry last_stream_capture_telemetry_snapshot;
    redclaw::capture::EncoderExecutionDiagnostics last_stream_encoder_diagnostics_snapshot;
    redclaw::capture::EncoderExecutionDiagnostics last_stream_adaptation_encoder_diagnostics_snapshot;
    HostStreamStageTelemetry last_stream_host_stage_telemetry_snapshot;
    ControllerStreamStageTelemetry last_stream_controller_stage_telemetry_snapshot;
    StreamRttTelemetry last_stream_adaptation_rtt_snapshot;
    redclaw::protocol::StreamControlMessageV1 last_stream_receiver_stats_snapshot;
    std::uint64_t last_stream_adaptation_sample_ms = now_steady_ms();
    std::uint64_t last_stream_rate_sample_ms = last_stream_adaptation_sample_ms;
    bool ice_failure_reported = false;
    std::uint64_t last_transport_diagnostic_sequence = 0;
    while (true) {
        if (local_control_output.failed()) {
            remote_input_session.pause(redclaw::input::RemoteInputPauseReason::kDisconnected);
            remote_input_session.release_all();
            ice_wrapper.close();
            stop_stream_workers();
            return 1;
        }
        redclaw::runtime::InputQaReceipts::LoopTiming input_loop_timing(input_qa_receipts);
        runtime_loop_wake.wait_for(std::chrono::milliseconds(kRuntimeLoopIntervalMs));
        input_loop_timing.next(redclaw::runtime::InputQaStage::kBeforeLocalInput);

        const std::uint64_t runtime_loop_now_ms = now_steady_ms();
        {
            bool desktop_connected = false;
            { std::lock_guard lock(callback_mutex); desktop_connected = stream_required_channels_ready; }
            terminal_bridge.pump(runtime_loop_now_ms, desktop_connected,
                workspace_mutations_allowed(), transfer_operation_gate.revision());
        }
        const std::uint64_t runtime_elapsed_seconds =
            runtime_loop_now_ms >= runtime_loop_started_ms
                ? (runtime_loop_now_ms - runtime_loop_started_ms) / 1000ULL
                : 0;
        const bool heartbeat_due = runtime_elapsed_seconds > elapsed_seconds;
        elapsed_seconds = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
            runtime_elapsed_seconds,
            std::numeric_limits<std::uint32_t>::max()));

        if (heartbeat_due) {
            const auto checks = ice_wrapper.diagnostics();
            auto diagnostic_events = checks.recent_events;
            for (const auto& candidate : checks.candidate_events) {
                if (candidate.sequence > last_transport_diagnostic_sequence
                    && std::none_of(diagnostic_events.begin(), diagnostic_events.end(), [&](const auto& event) {
                        return event.sequence == candidate.sequence;
                    })) diagnostic_events.push_back(candidate);
            }
            std::sort(diagnostic_events.begin(), diagnostic_events.end(), [](const auto& a, const auto& b) {
                return a.sequence < b.sequence;
            });
            for (const auto& event : diagnostic_events) {
                if (event.sequence <= last_transport_diagnostic_sequence) continue;
                std::cout << "Runtime transport diagnostic sequence=" << event.sequence
                          << " steady_ms=" << event.steady_ms
                          << " peer_generation=" << event.peer_generation
                          << " channel_generation=" << event.channel_generation
                          << " layer=" << static_cast<int>(event.layer)
                          << " channel=" << (event.channel ? static_cast<int>(*event.channel) : 99)
                          << " native_state=" << event.native_state
                          << " failure=" << (event.failure ? "true" : "false")
                          << " reason=" << event.reason
                          << " pair_id=" << event.pair_id
                          << " local_type=" << event.local_candidate_type
                          << " remote_type=" << event.remote_candidate_type
                          << " occurrences=" << event.occurrences << '\n';
                last_transport_diagnostic_sequence = event.sequence;
            }
            std::cout << "Runtime ICE check stats peer_generation=" << checks.ice_check_generation
                      << " backend=libjuice ice_tcp_supported=false";
            for (std::size_t index = 0; index < redclaw::net::kIceCheckKindCount; ++index) {
                std::cout << ' ' << redclaw::net::ice_check_kind_name(static_cast<redclaw::net::IceCheckKind>(index))
                          << '=' << checks.ice_check_totals[index];
            }
            std::cout << '\n';
        }

        std::optional<PendingIncompleteMediaFeedback> due_incomplete_feedback;
        if (options.role == RuntimeRole::kController) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (stream_qa_pending_incomplete_feedback.has_value()
                && runtime_loop_now_ms >= stream_qa_pending_incomplete_feedback->due_steady_ms) {
                due_incomplete_feedback = std::move(stream_qa_pending_incomplete_feedback);
                stream_qa_pending_incomplete_feedback.reset();
            }
        }
        if (due_incomplete_feedback.has_value()) {
            (void)send_incomplete_media_feedback(*due_incomplete_feedback);
        }
        if (options.role == RuntimeRole::kController) {
            std::uint32_t recovery_srtt_ms = 0;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                recovery_srtt_ms = stream_rtt_telemetry_snapshot.smoothed_rtt_ms;
            }
            bool progressing_idr = false;
            {
                std::lock_guard receive_lock(stream_receive_mutex);
                progressing_idr = stream_video_reassembly.keyframe_progressing(
                    runtime_loop_now_ms, recovery_srtt_ms);
            }
            if (!progressing_idr) {
                apply_decoder_recovery_update(
                    stream_decoder_recovery_coordinator.tick(runtime_loop_now_ms, recovery_srtt_ms),
                    "decoder_recovery_retry");
            }
        }
        if (options.stream_smoke && options.role == RuntimeRole::kController) {
            (void)flush_media_transport_feedback(runtime_loop_now_ms * 1000ULL);
        }

        input_loop_timing.next(redclaw::runtime::InputQaStage::kLocalInput);
        auto local_runtime_frames =
            poll_local_runtime_control_messages(&local_control_input_buffer, input_qa_receipts);
        for (auto command : std::move(local_runtime_frames.stream_controls)) {
            if (command.type == redclaw::protocol::StreamControlMessageTypeV1::kWorkspace) {
                transfer_bridge.from_gui(command); continue;
            }
            const bool input_command =
                command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputControlRequest
                || command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputBatch
                || command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputStateSync
                || command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputReleaseAll;
            const bool gui_receiver_stats = command.type
                == redclaw::protocol::StreamControlMessageTypeV1::kReceiverNetworkStats;
            if ((options.agent_qa_fixture_provider || options.input_diagnostics) && command.input_sequence != 0
                && command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputReleaseAll) {
                (void)input_qa_receipts.request_export(command.input_sequence);
                // A fixture export generation is local metadata. Controller
                // export must not send a remote pause or change the active lease.
                if (options.role == RuntimeRole::kController) continue;
            }
            if (options.role == RuntimeRole::kHost
                && command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputReleaseAll) {
                remote_input_session.pause(redclaw::input::RemoteInputPauseReason::kLocalPause);
                bool local_control_open = false;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    local_control_open = stream_control_channel_open;
                }
                if (local_control_open) {
                    send_input_status();
                }
                continue;
            }
            if (options.role != RuntimeRole::kController
                || (command.type != redclaw::protocol::StreamControlMessageTypeV1::kViewportRequest
                    && command.type != redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRequest
                    && command.type != redclaw::protocol::StreamControlMessageTypeV1::kKeyframeRequest
                    && command.type != redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogRequest
                    && !gui_receiver_stats
                    && !input_command)) {
                continue;
            }
            if (gui_receiver_stats) {
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    stream_observed_source_activity_revision =
                        command.observed_source_activity_revision;
                    stream_latest_displayable_frame_id = command.latest_displayable_frame_id;
                    stream_latest_displayable_keyframe_id =
                        command.latest_displayable_keyframe_id;
                    stream_latest_presented_frame_id = command.latest_presented_frame_id;
                    if (stream_gui_stats_baseline_pending
                        || command.decoded_frames < stream_gui_last_raw_decoded_frames
                        || command.rendered_frames < stream_gui_last_raw_rendered_frames) {
                        stream_gui_decoded_baseline = command.decoded_frames;
                        stream_gui_rendered_baseline = command.rendered_frames;
                        stream_gui_stats_baseline_pending = false;
                    }
                    stream_gui_last_raw_decoded_frames = command.decoded_frames;
                    stream_gui_last_raw_rendered_frames = command.rendered_frames;
                    stream_gui_decoded_frames = command.decoded_frames
                        >= stream_gui_decoded_baseline
                            ? command.decoded_frames - stream_gui_decoded_baseline
                            : 0;
                    stream_gui_rendered_frames = command.rendered_frames
                        >= stream_gui_rendered_baseline
                            ? command.rendered_frames - stream_gui_rendered_baseline
                            : 0;
                    // The GUI owns displayability/presentation, but the runtime owns
                    // reassembly truth. Merge the local acknowledgement into one
                    // complete receiver snapshot before putting it on the wire so a
                    // fast displayable ACK cannot temporarily replace the Host's
                    // network evidence with zero-valued partial statistics.
                    command.received_bytes = stream_received_wire_bytes;
                    command.received_fragments = stream_media_fragments_received;
                    command.reassembled_frames = stream_encoded_frames_reassembled;
                    command.completed_keyframes = stream_completed_keyframes;
                    command.dropped_keyframes = stream_dropped_keyframes;
                    command.dropped_frames =
                        stream_incomplete_frames_dropped + stream_dependency_frames_dropped;
                    command.received_frames = command.reassembled_frames + command.dropped_frames;
                    command.decoded_frames = stream_gui_decoded_frames;
                    command.rendered_frames = stream_gui_rendered_frames;
                    command.reassembly_timeouts = stream_reassembly_timeout_total;
                    command.latest_received_frame_id = stream_latest_received_frame_id;
                    command.latest_complete_frame_id = stream_latest_complete_frame_id;
                    command.latest_complete_keyframe_id = stream_latest_complete_keyframe_id;
                    command.observed_rate_revision = stream_observed_rate_revision;
                }
                (void)stream_decoder_recovery_coordinator.on_displayable_ack(
                    command.latest_displayable_keyframe_id);
            }
            if (command.type
                == redclaw::protocol::StreamControlMessageTypeV1::kKeyframeRequest) {
                if (command.capture_retry_requested && command.capture_status_version == 1
                    && peer_capture_status_version.load() >= 1) {
                    std::string retry_error;
                    (void)send_control_message(command, &retry_error);
                    continue;
                }
                auto reason = redclaw::session::ControllerDecoderBreakReason::kDecodeFailure;
                if (command.payload == "shared_memory_sequence_gap") {
                    reason = redclaw::session::ControllerDecoderBreakReason::kSharedSequenceGap;
                } else if (command.payload == "shared_memory_sequence_changed_during_snapshot") {
                    reason = redclaw::session::ControllerDecoderBreakReason::kSharedSequenceChanged;
                } else if (command.payload == "latest_frame_missing_keyframe_dependency") {
                    reason = redclaw::session::ControllerDecoderBreakReason::kDependencyFrame;
                } else if (command.payload == "no_first_presented_frame") {
                    reason = redclaw::session::ControllerDecoderBreakReason::kSessionStart;
                }
                (void)report_decoder_dependency_break(
                    reason,
                    command.incomplete_frame_id,
                    command.payload.empty() ? "gui_decoder_dependency_break" : command.payload);
                continue;
            }
            stamp_control_message(&command);
            if (transfer_operation_gate.blocks_mutation()
                && command.type == redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRequest) {
                command.type = redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRejected;
                command.payload = "workspace_transfer_busy";
                (void)local_control_output.send(command);
                continue;
            }
            if (command.type == redclaw::protocol::StreamControlMessageTypeV1::kViewportRequest) {
                stream_decoder_recovery_coordinator.reset();
                last_local_viewport_request = command;
            } else if (command.type
                       == redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRequest) {
                stream_decoder_recovery_coordinator.reset();
                last_local_capture_region_request = command;
            } else if (command.log_mode == redclaw::protocol::RemoteLogModeV1::kFollow) {
                last_local_log_follow_request = command;
            } else if (command.log_mode == redclaw::protocol::RemoteLogModeV1::kStop) {
                last_local_log_follow_request.reset();
            }
            bool control_open = false;
            bool input_supported = false;
            bool input_authorized = false;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                control_open = stream_control_channel_open;
                input_supported = controller_remote_input_supported;
                input_authorized = controller_remote_input_authorized;
            }
            const bool activates_input =
                command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputBatch
                || command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputStateSync
                || (command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputControlRequest
                    && command.input_requested_active);
            if (activates_input && transfer_operation_gate.blocks_mutation()) continue;
            redclaw::net::DataChannelTransportStats control_stats;
            std::string stats_error;
            const bool control_congested = activates_input
                && ice_wrapper.getDataChannelTransportStats(
                    redclaw::net::DataChannelKind::kControl, &control_stats, &stats_error)
                && control_stats.buffered_amount
                    >= kRemoteInputControlBufferedAmountHighWatermarkBytes;
            if (control_congested) {
                if (!controller_input_congestion_notified) {
                    auto local_status = make_control_message(
                        redclaw::protocol::StreamControlMessageTypeV1::kInputControlStatus);
                    local_status.input_supported = input_supported;
                    local_status.input_authorized = input_authorized;
                    local_status.input_state = redclaw::protocol::RemoteInputControlStateV1::kPaused;
                    local_status.input_reason = redclaw::protocol::RemoteInputStatusReasonV1::kQueueOverflow;
                    (void)local_control_output.send(local_status);
                    auto release = make_control_message(
                        redclaw::protocol::StreamControlMessageTypeV1::kInputReleaseAll);
                    std::string release_error;
                    if (control_open) {
                        (void)send_control_message(release, &release_error);
                    }
                    controller_input_congestion_notified = true;
                }
                continue;
            }
            controller_input_congestion_notified = false;
            std::string control_error;
            if (control_open && send_control_message(command, &control_error)) {
                continue;
            }
            if (input_command) {
                continue;
            }
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (command.type == redclaw::protocol::StreamControlMessageTypeV1::kViewportRequest) {
                pending_local_control_requests.erase(
                    std::remove_if(
                        pending_local_control_requests.begin(),
                        pending_local_control_requests.end(),
                        [](const auto& queued) {
                            return queued.type
                                == redclaw::protocol::StreamControlMessageTypeV1::kViewportRequest;
                        }),
                    pending_local_control_requests.end());
            }
            if (command.type
                == redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRequest) {
                pending_local_control_requests.erase(
                    std::remove_if(
                        pending_local_control_requests.begin(),
                        pending_local_control_requests.end(),
                        [](const auto& queued) {
                            return queued.type
                                == redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRequest;
                        }),
                    pending_local_control_requests.end());
            }
            if (pending_local_control_requests.size() < 64) {
                pending_local_control_requests.push_back(std::move(command));
            }
        }

        // IPC outlives an Agent channel. Drain/reject messages while disconnected
        // so old GUI requests cannot become new work after channel recreation.
        const auto local_agent_capacity = agent_peer_session.snapshot().channel_open
            ? std::min<std::size_t>(8, agent_peer_session.request_capacity()) : 8;
        input_loop_timing.next(redclaw::runtime::InputQaStage::kBeforeHostInput);
        for (auto message : local_agent_pipe.take_received(local_agent_capacity)) {
            std::string request_error;
            if (!agent_peer_session.enqueue_request(std::move(message), &request_error)) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                append_timeline(redclaw::render::RuntimeStatusSeverity::kWarning,
                    "agent", "Local Agent request rejected: " + request_error);
            }
        }

        bool should_rebuild_agent_channel = false;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (options.role == RuntimeRole::kHost
                && stream_agent_channel_open
                && agent_rebuild_attempts > 0
                && agent_channel_opened_at_ms != 0
                && runtime_loop_now_ms >= agent_channel_opened_at_ms
                && runtime_loop_now_ms - agent_channel_opened_at_ms
                    >= kAgentDataChannelRebuildStableMs) {
                ++agent_channel_rebuild_success_total;
                agent_rebuild_attempts = 0;
                agent_channel_opened_at_ms = 0;
            }
            if (options.role == RuntimeRole::kHost
                && agent_rebuild_awaiting_open
                && !stream_agent_channel_open
                && runtime_loop_now_ms >= agent_rebuild_open_deadline_ms) {
                agent_rebuild_awaiting_open = false;
                agent_rebuild_open_deadline_ms = 0;
                if (agent_rebuild_attempts >= kAgentDataChannelRebuildMaxAttempts) {
                    agent_rebuild_pending = false;
                    agent_unavailable_until_reconnect = true;
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "agent",
                        "Agent channel rebuild exhausted; desktop connection remains active");
                } else {
                    agent_rebuild_pending = true;
                    next_agent_rebuild_ms = runtime_loop_now_ms;
                }
            }
            should_rebuild_agent_channel = options.role == RuntimeRole::kHost
                && agent_rebuild_pending
                && !agent_rebuild_awaiting_open
                && !agent_unavailable_until_reconnect
                && stream_required_channels_ready
                && runtime_loop_now_ms >= next_agent_rebuild_ms;
        }
        if (should_rebuild_agent_channel) {
            std::string rebuild_error;
            const bool rebuilt = ice_wrapper.ensureDataChannel(
                redclaw::net::DataChannelKind::kAgent, &rebuild_error);
            std::lock_guard<std::mutex> lock(callback_mutex);
            ++agent_rebuild_attempts;
            ++agent_channel_rebuild_attempt_total;
            if (rebuilt) {
                agent_rebuild_pending = false;
                agent_rebuild_awaiting_open = true;
                agent_rebuild_open_deadline_ms = runtime_loop_now_ms
                    + kAgentDataChannelRebuildOpenTimeoutMs;
                next_agent_rebuild_ms = 0;
            } else if (agent_rebuild_attempts >= kAgentDataChannelRebuildMaxAttempts) {
                agent_rebuild_pending = false;
                agent_rebuild_awaiting_open = false;
                agent_unavailable_until_reconnect = true;
                agent_rebuild_open_deadline_ms = 0;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "agent",
                    "Agent channel rebuild exhausted; desktop connection remains active");
            } else {
                next_agent_rebuild_ms = runtime_loop_now_ms
                    + (1000ULL << std::min<std::uint32_t>(agent_rebuild_attempts, 2U));
            }
        }

        // A data channel may report open before its first application send is
        // writable. Keep bounded replayable requests queued and retry them
        // while the replacement control channel remains open; otherwise a
        // viewport or remote-log follow request can be lost for the entire
        // reconnected session after one transient send failure.
        if (heartbeat_due && options.role == RuntimeRole::kController) {
            std::deque<redclaw::protocol::StreamControlMessageV1> pending;
            bool control_open = false;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                control_open = stream_control_channel_open;
                if (control_open) {
                    const bool viewport_already_pending = std::any_of(
                        pending_local_control_requests.begin(),
                        pending_local_control_requests.end(),
                        [](const auto& queued) {
                            return queued.type
                                == redclaw::protocol::StreamControlMessageTypeV1::kViewportRequest;
                        });
                    if (!viewport_already_pending
                        && local_viewport_reconnect_extra_sends > 0
                        && last_local_viewport_request.has_value()) {
                        pending_local_control_requests.push_back(*last_local_viewport_request);
                        --local_viewport_reconnect_extra_sends;
                    }
                    pending.swap(pending_local_control_requests);
                }
            }
            std::string control_error;
            while (!pending.empty()) {
                auto request = std::move(pending.front());
                pending.pop_front();
                stamp_control_message(&request);
                if (!send_control_message(request, &control_error)) {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    pending_local_control_requests.push_front(std::move(request));
                    while (!pending.empty()) {
                        pending_local_control_requests.push_back(std::move(pending.front()));
                        pending.pop_front();
                    }
                    break;
                }
            }
        }

        input_loop_timing.next(redclaw::runtime::InputQaStage::kHostInput);
        if (options.role == RuntimeRole::kHost) {
            std::deque<redclaw::protocol::StreamControlMessageV1> remote_input_messages;
            bool queue_overflow = false;
            bool disconnected = false;
            bool control_open = false;
            redclaw::input::DesktopGeometry current_geometry;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                remote_input_messages.swap(pending_remote_input_messages);
                queue_overflow = std::exchange(remote_input_queue_overflow, false);
                disconnected = std::exchange(remote_input_disconnect_pending, false);
                control_open = stream_control_channel_open;
                current_geometry = remote_input_geometry;
            }
            bool status_changed = false;
            const std::uint64_t applied_sequence_before =
                remote_input_session.stats().last_applied_sequence;
            if (applied_transfer_input_revision != transfer_operation_gate.revision()) {
                applied_transfer_input_revision = transfer_operation_gate.revision();
                remote_input_session.set_transfer_blocked(true, now_unix_ms());
                // Drop old ordinary input at the transfer boundary, but never
                // discard an independently requested stop/revocation.
                std::erase_if(remote_input_messages, [](const auto& message) {
                    return message.type != redclaw::protocol::StreamControlMessageTypeV1::kInputReleaseAll
                        && !(message.type == redclaw::protocol::StreamControlMessageTypeV1::kInputControlRequest && !message.input_requested_active);
                });
            }
            remote_input_session.set_transfer_blocked(transfer_operation_gate.blocks_mutation(), now_unix_ms());
            const auto capture_state = stream_capture_gate.snapshot();
            const bool capture_available = capture_state.availability == redclaw::capture::CaptureAvailability::kRunning
                && (peer_capture_status_version.load() == 0 || capture_state.presented);
            const bool capture_input_pause_pending = capture_state.input_pause_revision != applied_capture_input_pause_revision;
            if (!capture_available || capture_input_pause_pending) {
                applied_capture_input_pause_revision = capture_state.input_pause_revision;
                if (!remote_input_messages.empty() || capture_input_pause_pending
                    || remote_input_session.state() == redclaw::input::RemoteInputSessionState::kActive) {
                    remote_input_session.pause(redclaw::input::RemoteInputPauseReason::kNoVideo);
                    status_changed = true;
                }
                remote_input_messages.clear();
            }
            if (disconnected) {
                remote_input_session.pause(redclaw::input::RemoteInputPauseReason::kDisconnected);
                remote_input_messages.clear();
                status_changed = true;
            }
            if (queue_overflow) {
                remote_input_session.pause(redclaw::input::RemoteInputPauseReason::kQueueOverflow);
                remote_input_messages.clear();
                status_changed = true;
            }
            if (control_open
                && remote_input_session.state()
                    == redclaw::input::RemoteInputSessionState::kPaused
                && remote_input_session.pause_reason()
                    == redclaw::input::RemoteInputPauseReason::kDisconnected) {
                remote_input_session.set_authorized(options.allow_remote_input);
                status_changed = true;
            }
            const bool geometry_shape_changed =
                current_geometry.origin_x != last_advertised_input_geometry.origin_x
                || current_geometry.origin_y != last_advertised_input_geometry.origin_y
                || current_geometry.width != last_advertised_input_geometry.width
                || current_geometry.height != last_advertised_input_geometry.height
                || current_geometry.rotation != last_advertised_input_geometry.rotation;
            const bool geometry_revision_changed =
                current_geometry.revision != last_advertised_input_geometry.revision;
            const bool geometry_changed = geometry_shape_changed || geometry_revision_changed;
            if (geometry_changed
                && remote_input_session.state() == redclaw::input::RemoteInputSessionState::kActive) {
                if (geometry_shape_changed) {
                    remote_input_session.pause(
                        redclaw::input::RemoteInputPauseReason::kGeometryChanged);
                } else {
                    remote_input_session.release_all();
                }
                remote_input_messages.clear();
                status_changed = true;
            }

            for (const auto& command : remote_input_messages) {
                input_qa_receipts.command(redclaw::runtime::InputQaStage::kHostConsume, command);
                std::string input_error;
                if (command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputControlRequest) {
                    if (!command.input_requested_active) {
                        remote_input_session.pause(redclaw::input::RemoteInputPauseReason::kLocalPause);
                    } else if (redclaw::input::is_valid_desktop_geometry(current_geometry)) {
                        (void)remote_input_session.request_active(now_unix_ms(), &input_error);
                    } else {
                        remote_input_session.pause(redclaw::input::RemoteInputPauseReason::kNoVideo);
                    }
                    status_changed = true;
                    continue;
                }
                if (command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputReleaseAll) {
                    remote_input_session.pause(redclaw::input::RemoteInputPauseReason::kLocalPause);
                    status_changed = true;
                    continue;
                }
                if (command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputBatch) {
                    if (command.desktop_geometry_revision != current_geometry.revision) {
                        input_qa_receipts.command(redclaw::runtime::InputQaStage::kGeometryRejected, command);
                        remote_input_session.release_all();
                        status_changed = true;
                        continue;
                    }
                    std::vector<redclaw::input::InputEvent> events;
                    events.reserve(command.input_events.size() * 2U);
                    bool mapped = true;
                    for (const auto& event : command.input_events) {
                        if (!map_remote_input_event(event, current_geometry, &events)) {
                            mapped = false;
                            break;
                        }
                    }
                    const bool enqueued = mapped && remote_input_session.enqueue_batch(
                        command.input_sequence, std::move(events), now_unix_ms(), &input_error);
                    input_qa_receipts.command(!mapped ? redclaw::runtime::InputQaStage::kMappingRejected
                        : enqueued ? redclaw::runtime::InputQaStage::kSessionEnqueued
                        : redclaw::runtime::InputQaStage::kSessionRejected, command);
                    if (!enqueued) {
                        status_changed = true;
                    }
                    continue;
                }
                if (command.type == redclaw::protocol::StreamControlMessageTypeV1::kInputStateSync) {
                    if (command.desktop_geometry_revision != current_geometry.revision) {
                        remote_input_session.release_all();
                        status_changed = true;
                        continue;
                    }
                    if (!remote_input_session.synchronize_state(
                            command.input_sequence,
                            command.pressed_scan_codes,
                            command.pressed_mouse_buttons,
                            now_unix_ms(),
                            &input_error)) {
                        status_changed = true;
                    }
                }
            }

            std::string drain_error;
            if (!remote_input_session.drain(now_unix_ms(), &drain_error)) {
                status_changed = true;
            }
            if (remote_input_session.expire_lease(now_unix_ms())) {
                status_changed = true;
            }
            if (remote_input_session.stats().last_applied_sequence != applied_sequence_before) {
                status_changed = true;
            }
            if (control_open && geometry_changed
                && redclaw::input::is_valid_desktop_geometry(current_geometry)) {
                (void)send_input_capabilities();
            }
            if (control_open && status_changed) {
                send_input_status();
            }
        }

        {
            bool desktop_connected = false;
            { std::lock_guard lock(callback_mutex); desktop_connected = stream_required_channels_ready; }
            // Clipboard publication/input must observe this tick's capture,
            // geometry, lease, disconnect and explicit input-stop decisions.
            transfer_bridge.pump(runtime_loop_now_ms, desktop_connected);
        }
        input_loop_timing.next(redclaw::runtime::InputQaStage::kAgentPump);
        // Desktop input/control gets its send opportunity before Agent traffic.
        agent_peer_session.pump([&ice_wrapper](
            const redclaw::protocol::AgentMessageEnvelopeV1& message, std::string* error) {
            redclaw::net::DataChannelTransportStats stats;
            if (!ice_wrapper.getDataChannelTransportStats(
                    redclaw::net::DataChannelKind::kAgent, &stats, error)
                || stats.buffered_amount >= kAgentDataChannelBufferedAmountHighWatermarkBytes)
                return false;
            const auto serialized = redclaw::protocol::serialize_agent_message_v1(message);
            if (serialized.empty()) return false;
            return ice_wrapper.sendDataChannelBinaryMessage(redclaw::net::DataChannelKind::kAgent,
                {reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size()}, error);
        });

        input_loop_timing.next(redclaw::runtime::InputQaStage::kAfterAgent);
        if (options.agent_qa_force_channel_close_after_event
            && !agent_qa_forced_channel_close_attempted) {
            bool should_force_close = false;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                should_force_close = stream_agent_channel_open
                    && stream_required_channels_ready
                    && agent_peer_session.snapshot().received_task_events > 0;
            }
            if (should_force_close) {
                agent_qa_forced_channel_close_attempted = true;
                std::string close_error;
                if (ice_wrapper.closeDataChannel(
                        redclaw::net::DataChannelKind::kAgent, &close_error)) {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    ++agent_qa_forced_channel_close_total;
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "agent-qa",
                        "forced Agent-only channel close after first task event; media/control remain active");
                } else {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kError,
                        "agent-qa",
                        "forced Agent-only channel close failed: " + close_error);
                }
            }
        }

        if (!options.stream_qa_force_required_channel_close.empty()
            && options.role == RuntimeRole::kController
            && !stream_qa_forced_channel_close_attempted) {
            bool should_force_close = false;
            std::uint64_t complete_frame_id = 0;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                should_force_close = stream_required_channels_ready
                    && saw_connected_state
                    && !saw_failed_state
                    && stream_required_channels_ready_at_ms != 0
                    && now_steady_ms() - stream_required_channels_ready_at_ms
                        >= kStreamQaRequiredChannelStableMs
                    && stream_completed_keyframes > 0
                    && stream_controller_stage_telemetry_snapshot
                           .direct_pipe_write_success_count > 0;
                complete_frame_id = stream_latest_complete_frame_id;
            }
            if (should_force_close) {
                stream_qa_forced_channel_close_attempted = true;
                const auto channel_kind = options.stream_qa_force_required_channel_close == "media"
                    ? redclaw::net::DataChannelKind::kMedia
                    : redclaw::net::DataChannelKind::kControl;
                std::string close_error;
                if (ice_wrapper.closeDataChannel(channel_kind, &close_error)) {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    ++stream_qa_forced_channel_close_total;
                    stream_qa_forced_channel_close_frame_id = complete_frame_id;
                    stream_qa_recovery_success_total_at_forced_close =
                        stream_recovery_success_total;
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "stream-qa",
                        "forced required channel close kind="
                            + options.stream_qa_force_required_channel_close
                            + " after_complete_frame_id="
                            + std::to_string(complete_frame_id));
                } else {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    stream_counters.last_error = "forced required channel close failed: "
                        + close_error;
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kError,
                        "stream-qa",
                        stream_counters.last_error);
                }
            }
        }

        bool should_repair_signaling = false;
        bool reset_for_new_controller_request = false;
        bool post_connected_recovery = false;
        bool reset_runtime_session = false;
        bool connected_recovery_waiting = false;
        std::string accepted_controller_request_tag;
        std::string repair_reason;
        {
            std::unique_lock<std::mutex> lock(callback_mutex);
            if (!runtime_error.empty()) {
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kError,
                    "runtime",
                    "runtime signaling failed: " + runtime_error);
                std::cerr << "Runtime signaling failed: " << runtime_error << '\n';
                lock.unlock();
                ice_wrapper.close();
                return 1;
            }
            if (dht_controller_request_superseded
                && use_dht_transport
                && options.role == RuntimeRole::kHost
                && !saw_connected_state) {
                should_repair_signaling = true;
                reset_for_new_controller_request = true;
                accepted_controller_request_tag = dht_negotiation.connection_request_tag();
                repair_reason = "Controller published a newer connect request";
            }
            const std::uint64_t recovery_now_ms = now_steady_ms();
            if (saw_connected_state) {
                if (saw_failed_state && connected_failure_observed_at_ms == 0) {
                    connected_failure_observed_at_ms = recovery_now_ms;
                }
                const auto established_recovery =
                    redclaw::service::decide_established_session_recovery({
                        .connected_once = true,
                        .transport_failed = saw_failed_state,
                        .transport_failure_observed_at_ms = connected_failure_observed_at_ms,
                        .required_channel_closed_at_ms = required_channel_closed_at_ms,
                        .now_ms = recovery_now_ms,
                        .grace_ms = kRequiredChannelLossGraceMs,
                    });
                connected_recovery_waiting = established_recovery
                    != redclaw::service::EstablishedSessionRecoveryAction::kNone;
                if (established_recovery
                    == redclaw::service::EstablishedSessionRecoveryAction::kRecover) {
                    connected_recovery_waiting = true;
                    if (!use_dht_transport) {
                        connected_recovery_waiting = false;
                        saw_failed_state = true;
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kError,
                            "recovery",
                            "automatic reconnect requires the online DHT signaling path");
                    } else if (options.role == RuntimeRole::kHost) {
                        should_repair_signaling = true;
                        post_connected_recovery = true;
                        reset_runtime_session = true;
                        repair_reason = "connected session lost; Host returning to wait";
                    } else if (!automatic_recovery_active) {
                        // Do not let the first reconnect attempt immediately
                        // adopt the durable offer whose established transport
                        // just died. The Controller must wait for a strictly
                        // newer Host-owned generation (or a fresh standby
                        // generation published during the same recovery).
                        dht_failed_remote_generation = std::max(
                            dht_failed_remote_generation,
                            dht_negotiation.generation());
                        dht_failed_remote_offer_tag = dht_negotiation.offer_description_tag();
                        automatic_recovery_active = true;
                        reconnect_attempt_in_flight = false;
                        reconnect_attempt_count = 0;
                        reconnect_retry_after_ms = 0;
                        reconnect_attempt_deadline_ms = 0;
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kWarning,
                            "recovery",
                            "required channel/ICE loss detected; Controller reconnect sequence started");
                    }
                }
            }
            if (automatic_recovery_active && !should_repair_signaling) {
                connected_recovery_waiting = true;
                if (reconnect_schedule.observe_progress(dht_negotiation.phase(),
                        dht_negotiation.generation(), recovery_now_ms, kAutomaticReconnectAttemptWindowMs)) {
                    std::cout << "Runtime recovery progress owner=automatic attempt=" << reconnect_attempt_count
                              << " phase=" << redclaw::service::connection_negotiation_phase_to_string(reconnect_schedule.phase)
                              << " generation=" << reconnect_schedule.generation
                              << " deadline_ms=" << reconnect_attempt_deadline_ms << '\n';
                }
                const auto recovery_action = reconnect_schedule.tick(
                    recovery_now_ms, kAutomaticReconnectAttemptWindowMs, saw_failed_state);
                if (recovery_action == redclaw::service::AutomaticReconnectAction::kAttemptFailed) {
                    dht_failed_remote_generation = std::max(dht_failed_remote_generation, dht_negotiation.generation());
                    if (!dht_negotiation.offer_description_tag().empty()) {
                        dht_failed_remote_offer_tag = dht_negotiation.offer_description_tag();
                    }
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "recovery",
                        "Controller reconnect attempt "
                            + std::to_string(reconnect_attempt_count)
                            + (saw_failed_state ? " ICE terminated; next_retry_ms=" : " phase deadline expired; next_retry_ms=")
                            + std::to_string(reconnect_retry_after_ms));
                }
                if (recovery_action == redclaw::service::AutomaticReconnectAction::kRebuild) {
                        should_repair_signaling = true;
                        reset_for_new_controller_request = true;
                        post_connected_recovery = true;
                        reset_runtime_session = true;
                        repair_reason = "connected session lost; Controller automatic reconnect attempt "
                            + std::to_string(reconnect_attempt_count);
                }
            }
            // A newly accepted replacement already owns rebuilding. Do not
            // stamp its generation/Offer as the old transport's failure.
            if (saw_failed_state
                && !automatic_recovery_active
                && !should_repair_signaling
                && !dht_peer_republished_description) {
                const std::uint64_t failure_now_ms = now_steady_ms();
                redclaw::service::DhtIceFailureRecoveryInput recovery_input;
                recovery_input.role = options.role == RuntimeRole::kHost
                    ? redclaw::service::ConnectionNegotiationRole::kHost
                    : redclaw::service::ConnectionNegotiationRole::kController;
                recovery_input.transport_failed = use_dht_transport;
                recovery_input.connected_once = saw_connected_state;
                recovery_input.automatic_recovery_owned = automatic_recovery_active;
                recovery_input.remote_description_applied = remote_description_applied;
                const auto failure_steady_ms = now_steady_ms();
                if (!dht_initial_failure_started_steady_ms.has_value()) {
                    dht_initial_failure_started_steady_ms = failure_steady_ms;
                }
                recovery_input.failure_elapsed_ms =
                    failure_steady_ms - *dht_initial_failure_started_steady_ms;
                recovery_input.repair_attempts = dht_repair_attempts;
                const auto recovery_decision =
                    redclaw::service::decide_dht_ice_failure_recovery(recovery_input);

                if (use_dht_transport
                    && options.role == RuntimeRole::kHost
                    && remote_description_applied
                    && dht_negotiation.answer_acknowledged()
                    && local_candidate_gathering_complete
                    && dht_remote_candidates_complete
                    && !saw_connected_state) {
                    if (dht_repair_after_ms == 0) {
                        const bool paired_record_looks_abandoned = dht_remote_record_expiry > 0
                            && dht_sealed_age_seconds(dht_remote_record_expiry)
                                > kDhtAbandonedRecordSealAgeSeconds;
                        dht_repair_after_ms = failure_now_ms
                            + (paired_record_looks_abandoned
                                ? kDhtSignalingRepairDelayMs
                                : kDhtSignalingRepairPatientDelayMs);
                    } else if (failure_now_ms >= dht_repair_after_ms
                        && dht_repair_attempts < kDhtAbandonedHostRepairAttemptLimit) {
                        // The peer this attempt paired with never completed ICE.
                        // With a session code that stays valid across sessions
                        // that is usually a record left behind by a peer which
                        // already exited, so give up on this attempt, remember
                        // the record that failed, and rebuild the session for
                        // whichever peer publishes something newer.
                        ++dht_repair_attempts;
                        should_repair_signaling = true;
                        repair_reason = "peer did not connect";
                        dht_failed_remote_expiry = std::max(dht_failed_remote_expiry, dht_remote_record_expiry);
                        dht_failed_remote_revision = std::max(dht_failed_remote_revision, dht_remote_revision);
                    }
                }

                if (!should_repair_signaling
                    && recovery_decision.action
                        == redclaw::service::DhtIceFailureRecoveryAction::kRebuildControllerRequest) {
                    dht_failed_remote_generation = std::max(
                        dht_failed_remote_generation,
                        dht_negotiation.generation());
                    dht_failed_remote_offer_tag = dht_negotiation.offer_description_tag();
                    if (dht_repair_attempts < std::numeric_limits<std::uint32_t>::max()) {
                        ++dht_repair_attempts;
                    }
                    should_repair_signaling = true;
                    reset_runtime_session = true;
                    repair_reason = "transport terminated; Controller publishing a fresh request after bounded backoff";
                }

                const bool dht_repair_pending = use_dht_transport
                    && options.role == RuntimeRole::kHost
                    && remote_description_applied
                    && !saw_connected_state
                    && dht_repair_attempts < kDhtAbandonedHostRepairAttemptLimit;
                const bool dht_controller_waiting_for_backoff = use_dht_transport
                    && options.role == RuntimeRole::kController
                    && !saw_connected_state
                    && recovery_decision.action
                        == redclaw::service::DhtIceFailureRecoveryAction::kWaitBackoff;

                const std::size_t dht_local_candidate_count = use_dht_transport
                    ? local_candidate_lines.size()
                    : 0;
                const std::size_t dht_direct_candidate_target = std::min(
                    dht_local_candidate_count,
                    kDhtDirectCandidatePublishLimit);
                const bool dht_local_candidates_pending = use_dht_transport
                    && !local_candidate_lines.empty()
                    && !dht_full_candidate_publish_done
                    && (dht_local_revision <= 1
                        || dht_direct_candidate_publish_count < dht_direct_candidate_target);
                const bool dht_full_candidates_pending = use_dht_transport
                    && dht_local_candidate_count > 1
                    && !dht_full_candidate_publish_done;
                const bool dht_remote_candidates_pending = use_dht_transport
                    && !dht_remote_candidates_complete;
                const bool dht_signal_exchange_incomplete = use_dht_transport
                    && (!remote_description_applied
                        || dht_local_revision == 0
                        || dht_generation_publish_success == 0
                        || !local_candidate_gathering_complete
                        || !dht_remote_candidates_complete
                        || (options.role == RuntimeRole::kController
                            && !dht_negotiation.answer_acknowledged())
                        || dht_local_candidates_pending
                        || dht_full_candidates_pending
                        || dht_remote_candidates_pending);
                if (should_repair_signaling) {
                    // The rebuild path below reports and retries this failure.
                } else if (dht_signal_exchange_incomplete
                    || dht_repair_pending
                    || dht_controller_waiting_for_backoff) {
                    if (!ice_failure_reported) {
                        ice_failure_reported = true;
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kWarning,
                            "ice",
                            dht_signal_exchange_incomplete
                                ? "ice state failed before DHT signaling exchange completed"
                                : "ice state failed; DHT signaling repair remains active");
                        std::cout << (dht_signal_exchange_incomplete
                                ? "Runtime ICE state failed before DHT signaling completed; continuing role="
                                : "Runtime ICE state failed; DHT retry remains active role=")
                                  << role_name
                                  << " local_revision=" << dht_local_revision
                                  << " generation_publish_success=" << dht_generation_publish_success
                                  << " generation=" << dht_negotiation.generation()
                                  << " negotiation_phase="
                                  << redclaw::service::connection_negotiation_phase_to_string(
                                         dht_negotiation.phase())
                                  << " remote_description_applied="
                                  << (remote_description_applied ? "true" : "false")
                                  << " local_candidates_pending="
                                  << (dht_local_candidates_pending ? "true" : "false")
                                  << " full_candidates_pending="
                                  << (dht_full_candidates_pending ? "true" : "false")
                                  << " remote_candidates_pending="
                                  << (dht_remote_candidates_pending ? "true" : "false")
                                  << " recovery_action="
                                  << redclaw::service::dht_ice_failure_recovery_action_to_string(
                                         recovery_decision.action)
                                  << " ice_udp_port=" << options.ice_udp_port
                                  << " local_candidate_count=" << dht_local_candidate_count
                                  << " direct_candidate_published=" << dht_direct_candidate_publish_count
                                  << " remote_dht_exchanged=" << candidate_diagnostics.remote_dht_exchanged
                                  << '\n';
                    }
                } else if (!connected_recovery_waiting && !should_repair_signaling) {
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kError,
                        "ice",
                        "ice state transitioned to failed");
                    std::cerr << "Runtime ICE state failed ice_udp_port="
                              << options.ice_udp_port << '\n';
                    lock.unlock();
                    ice_wrapper.close();
                    return 1;
                }
            }

            if (dht_peer_republished_description
                && options.role == RuntimeRole::kController
                && !should_repair_signaling) {
                if (dht_repair_attempts < std::numeric_limits<std::uint32_t>::max()) {
                    ++dht_repair_attempts;
                }
                should_repair_signaling = true;
                repair_reason = "Host published a newer negotiation generation";
            }

            if (should_repair_signaling) {
                // Accepted old callbacks may still be waiting on callback_mutex.
                // Retire/drain without this lock BEFORE clearing any shared state.
                lock.unlock();
                if (!ice_wrapper.retireAndDrain()) {
                    std::cerr << "Runtime ICE retirement requires owner thread" << '\n';
                    return 1;
                }
                lock.lock();
                saw_failed_state = false;
                saw_connected_state = false;
                local_description_sdp.clear();
                local_candidate_lines.clear();
                local_description_written = false;
                signal_snapshot_dirty = false;
                stream_media_channel_open = false;
                stream_control_channel_open = false;
                stream_agent_channel_open = false;
                stream_navigation_channel_open = false;
                agent_rebuild_pending = false;
                agent_rebuild_awaiting_open = false;
                agent_unavailable_until_reconnect = false;
                agent_rebuild_attempts = 0;
                next_agent_rebuild_ms = 0;
                agent_rebuild_open_deadline_ms = 0;
                agent_channel_opened_at_ms = 0;
                stream_required_channels_ready = false;
                stream_media_submission_paused.store(true);
                stream_required_channels_ready_at_ms = 0;
                pending_remote_input_messages.clear();
                remote_input_disconnect_pending = true;
                last_advertised_input_geometry = {};
                stream_remote_control_guard.reset();
                peer_capture_status_version.store(0);
                {
                    std::lock_guard<std::mutex> envelope_lock(stream_control_envelope_mutex);
                    stream_control_epoch = "runtime-" + role_name + "-"
                        + std::to_string(now_unix_ms()) + "-generation-"
                        + std::to_string(++stream_control_epoch_generation);
                    stream_control_message_id.store(0);
                    terminal_bridge.reset_transport(stream_control_epoch);
                }
                transfer_bridge.reset_transport(stream_control_epoch);
                required_channel_closed_at_ms = 0;
                connected_failure_observed_at_ms = 0;
                candidate_diagnostics = CandidateDiagnostics {};
                local_candidate_gathering_complete = false;
                append_timeline(
                    redclaw::render::RuntimeStatusSeverity::kWarning,
                    "signal",
                    repair_reason + "; rebuilding the session for a new pairing attempt");
            }
        }

        const bool defer_host_rebuild_until_controller_request =
            options.role == RuntimeRole::kHost
            && redclaw::service::should_defer_dht_host_rebuild_until_controller_request(
                post_connected_recovery,
                accepted_controller_request_tag);
        if (should_repair_signaling) {
            agent_peer_session.reset();
            if (options.role == RuntimeRole::kHost) {
                remote_input_session.pause(redclaw::input::RemoteInputPauseReason::kDisconnected);
            }
            std::cout << "Runtime rebuilding signaling session role=" << role_name
                      << " attempt=" << (post_connected_recovery
                              ? reconnect_attempt_count
                              : dht_repair_attempts)
                      << " reason=" << repair_reason
                      << " epoch_generation=" << stream_control_epoch_generation
                      << " failed_remote_revision=" << dht_failed_remote_revision
                      << " failed_remote_expiry=" << dht_failed_remote_expiry
                      << " failed_remote_generation=" << dht_failed_remote_generation << '\n';

            remote_description_applied = false;
            applied_remote_description_sdp.clear();
            dht_peer_republished_description = false;
            dht_controller_request_superseded = false;
            applied_remote_dht_candidate_lines.clear();
            pending_remote_dht_candidate_lines.clear();
            dht_remote_revision = 0;
            dht_remote_record_expiry = 0;
            dht_last_remote_candidate_count = 0;
            dht_remote_candidates_complete = false;
            dht_initial_snapshot_publish_done = false;
            dht_full_candidate_publish_done = false;
            dht_answer_ack_publish_done = false;
            dht_last_published_candidate_lines.clear();
            dht_direct_candidate_publish_count = 0;
            dht_generation_publish_success = 0;
            dht_publication.reset();
            dht_next_publish_retry_ms = 0;
            dht_next_publish_refresh_ms = 0;
            dht_next_fetch_ms = 0;
            dht_repair_after_ms = 0;
            dht_initial_failure_started_steady_ms.reset();
            dht_stale_remote_reported = false;
            ice_failure_reported = false;
            if (reset_runtime_session) {
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    ++stream_recovery_reset_total;
                    stream_required_channels_ready = false;
                    stream_required_channels_ready_at_ms = 0;
                }
                if (options.role == RuntimeRole::kHost) {
                    // A signaling reconnect does not change the desktop source
                    // or codec contract. Keep the proven capture/encoder
                    // pipeline alive and gate its output until the replacement
                    // media/control epoch is ready. Stopping a hardware encoder
                    // concurrently with driver-owned frames can block the Host
                    // before it publishes the next DHT generation.
                    stream_keyframe_refresh_request_generation.fetch_add(1);
                } else {
                    std::lock_guard<std::mutex> encoder_lock(stream_encoder_mutex);
                    stream_encoder_session.stop();
                    stream_encoder_started = false;
                    stream_encoder_width = 0;
                    stream_encoder_height = 0;
                    stream_last_encoded_capture_sequence = 0;
                    stream_last_applied_geometry_transaction_id = 0;
                    stream_keyframe_refresh_request_generation.store(0);
                    stream_keyframe_refresh_completed_generation.store(0);
                }
                {
                    std::lock_guard<std::mutex> receive_lock(stream_receive_mutex);
                    stream_video_decoder.reset();
                    stream_decoder_width = 0;
                    stream_decoder_height = 0;
                    stream_video_reassembly.reset();
                }
                stream_media_pacer.reset(true);
                stream_transport_estimator.reset();
                stream_congestion_controller.reset();
                stream_decoder_recovery_coordinator.reset();
                stream_receiver_decode_capacity_controller.reset();
                {
                    std::lock_guard<std::mutex> feedback_lock(
                        stream_transport_feedback_mutex);
                    stream_transport_feedback_recorder.reset();
                }
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    pending_remote_log_messages.clear();
                    remote_log_follow_request_id.clear();
                    remote_log_follow_cursor = 0;
                    stream_sent_frame_metadata.clear();
                    stream_last_playback_starvation_frame_id_received = 0;
                    last_stream_playback_starvation_frame_id = 0;
                    stream_latest_received_frame_id = 0;
                    stream_latest_complete_frame_id = 0;
                    stream_latest_complete_keyframe_id = 0;
                    stream_observed_rate_revision = 0;
                    stream_latest_displayable_frame_id = 0;
                    stream_latest_displayable_keyframe_id = 0;
                    stream_latest_presented_frame_id = 0;
                    stream_gui_decoded_frames = 0;
                    stream_gui_rendered_frames = 0;
                    stream_gui_decoded_baseline = 0;
                    stream_gui_rendered_baseline = 0;
                    stream_gui_last_raw_decoded_frames = 0;
                    stream_gui_last_raw_rendered_frames = 0;
                    stream_gui_stats_baseline_pending = true;
                    stream_observed_source_activity_revision = 0;
                    stream_last_media_assembly_error.clear();
                    stream_qa_pending_incomplete_feedback.reset();
                    stream_rtt_telemetry_snapshot = {};
                    next_stream_rtt_ping_steady_ms = 0;
                    stream_adaptive_control_snapshot.last_rtt_ms = 0;
                    stream_adaptive_control_snapshot.last_rtt_baseline_ms = 0;
                    stream_adaptive_control_snapshot.last_rtt_queue_delay_ms = 0;
                    stream_adaptive_control_snapshot.pressure_window_count = 0;
                    stream_adaptive_control_snapshot.relief_window_count = 0;
                    stream_transport_estimate_snapshot = {};
                    stream_congestion_decision_snapshot = {};
                    stream_counters.last_error.clear();
                }
                stream_work_coordinator.reset();
                if (options.role == RuntimeRole::kHost) {
                    std::uint64_t current_geometry_revision = 0;
                    std::uint64_t current_rate_revision = 0;
                    {
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        current_geometry_revision = stream_geometry_revision;
                        current_rate_revision = stream_adaptive_control_snapshot.rate_revision;
                    }
                    bool retained_capture = false;
                    {
                        std::lock_guard<std::mutex> frame_lock(stream_capture_frame_mutex);
                        retained_capture = stream_latest_captured_frame != nullptr;
                    }
                    redclaw::session::DesktopSourceActivityUpdate source_update;
                    {
                        std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
                        stream_source_activity_tracker.reset(
                            current_geometry_revision,
                            current_rate_revision);
                        if (retained_capture) {
                            stream_source_activity_tracker.on_capture_poll_started(
                                now_steady_ms());
                            source_update = stream_source_activity_tracker.on_capture_frame(
                                now_steady_ms(),
                                current_geometry_revision,
                                current_rate_revision);
                        }
                    }
                    apply_source_activity_update(source_update);
                }
                last_stream_adaptation_rtt_snapshot = {};
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    if (options.role == RuntimeRole::kController) {
                        pending_local_control_requests.clear();
                        local_viewport_reconnect_extra_sends = 0;
                        if (last_local_viewport_request.has_value()) {
                            pending_local_control_requests.push_back(*last_local_viewport_request);
                            local_viewport_reconnect_extra_sends =
                                kDesktopStreamViewportReconnectExtraSends;
                        }
                        if (last_local_capture_region_request.has_value()) {
                            pending_local_control_requests.push_back(
                                *last_local_capture_region_request);
                        }
                        if (last_local_log_follow_request.has_value()) {
                            pending_local_control_requests.push_back(*last_local_log_follow_request);
                        }
                    }
                }
                if (options.role == RuntimeRole::kController) {
                    dht_connection_request_payload = make_runtime_session_id("controller-reconnect");
                    const std::string request_tag = redclaw::service::derive_dht_description_tag(
                        dht_connection_request_payload);
                    if (!dht_negotiation.reset_for_reconnect(request_tag)) {
                        std::cerr << "Runtime failed to reset Controller negotiation for reconnect" << '\n';
                        ice_wrapper.close();
                        return 1;
                    }
                    signal_snapshot_dirty = true;
                } else if (!dht_negotiation.reset_for_reconnect()) {
                    std::cerr << "Runtime failed to reset Host negotiation for reconnect" << '\n';
                    ice_wrapper.close();
                    return 1;
                } else if (defer_host_rebuild_until_controller_request) {
                    dht_host_persistent_offer_active = false;
                    signal_snapshot_dirty = false;
                    std::cout << "Runtime Host recovery waiting for a fresh Controller request"
                              << '\n';
                } else {
                    dht_connection_request_payload = make_runtime_session_id("host-standby-offer");
                    const std::string standby_tag = redclaw::service::derive_dht_description_tag(
                        dht_connection_request_payload);
                    const std::string rebuild_request_tag =
                        redclaw::service::select_dht_host_rebuild_request_tag(
                            reset_for_new_controller_request,
                            accepted_controller_request_tag,
                            standby_tag);
                    if (!dht_negotiation.observe_controller_request(rebuild_request_tag).accepted()) {
                        std::cerr << "Runtime failed to reset persistent Host offer lease" << '\n';
                        ice_wrapper.close();
                        return 1;
                    }
                    if (!accepted_controller_request_tag.empty()) {
                        std::cout << "Runtime preserved accepted Controller request across Host ICE rebuild"
                                  << " connection_request_tag=" << rebuild_request_tag << '\n';
                    }
                    dht_host_persistent_offer_active = true;
                    signal_snapshot_dirty = true;
                }
            }
            if (reset_for_new_controller_request) {
                dht_repair_attempts = 0;
                dht_failed_remote_expiry = 0;
                dht_failed_remote_revision = 0;
            }

            // startGathering tears down the peer connection and builds a new one
            // with fresh ICE credentials, so the next publish carries a
            // description the peer can pair with from scratch.
            ice_gathering_started = false;
            if ((defer_ice_gathering_until_signaling_peer
                    && options.role == RuntimeRole::kController)
                || defer_host_rebuild_until_controller_request) {
                ice_wrapper.close();
            } else {
                std::string restart_error;
                if (!ensure_ice_gathering_started("ICE session rebuilt after a failed pairing", &restart_error)) {
                    std::cerr << "Runtime failed to rebuild ICE session: " << restart_error << '\n';
                    ice_wrapper.close();
                    return 1;
                }
            }
            continue;
        }

        input_loop_timing.next(redclaw::runtime::InputQaStage::kMaintenance);

        if (heartbeat_due) {
            std::string follow_request_id;
            std::uint64_t follow_cursor = 0;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                follow_request_id = remote_log_follow_request_id;
                follow_cursor = remote_log_follow_cursor;
            }
            if (!follow_request_id.empty()) {
                auto follow_request = make_control_message(
                    redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogRequest);
                follow_request.request_id = follow_request_id;
                follow_request.log_mode = redclaw::protocol::RemoteLogModeV1::kFollow;
                follow_request.log_cursor = follow_cursor;
                queue_remote_log_response(follow_request);
            }

            if (options.role == RuntimeRole::kController) {
                bool reassembly_expired = false;
                redclaw::net::EncodedVideoDropSummary expired_drop;
                {
                    std::lock_guard<std::mutex> receive_lock(stream_receive_mutex);
                    reassembly_expired = stream_video_reassembly.expired(now_steady_ms());
                    if (reassembly_expired) {
                        expired_drop = stream_video_reassembly.require_keyframe();
                    }
                }
                if (reassembly_expired) {
                    (void)report_incomplete_media_frame(
                        expired_drop.dropped_frame_id,
                        "reassembly_timeout");
                    (void)report_decoder_dependency_break(
                        redclaw::session::ControllerDecoderBreakReason::kReassemblyTimeout,
                        expired_drop.dropped_frame_id,
                        "reassembly_timeout");
                }
                auto stats = make_control_message(
                    redclaw::protocol::StreamControlMessageTypeV1::kReceiverNetworkStats);
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    const bool qa_hold_loss_stats_for_expired_feedback =
                        options.stream_qa_drop_one_media_fragment
                        && options.stream_qa_incomplete_feedback_class == "expired"
                        && stream_qa_pending_incomplete_feedback.has_value();
                    if (reassembly_expired) {
                        ++stream_reassembly_timeout_total;
                        stream_incomplete_frames_dropped += expired_drop.dropped_incomplete_frames;
                        stream_dropped_keyframes += expired_drop.dropped_keyframes;
                    }
                    stats.received_bytes = stream_received_wire_bytes;
                    stats.received_fragments = stream_media_fragments_received;
                    stats.reassembled_frames = stream_encoded_frames_reassembled;
                    stats.completed_keyframes = stream_completed_keyframes;
                    stats.dropped_keyframes = qa_hold_loss_stats_for_expired_feedback
                        ? 0
                        : stream_dropped_keyframes;
                    stats.dropped_frames = qa_hold_loss_stats_for_expired_feedback
                        ? 0
                        : stream_incomplete_frames_dropped + stream_dependency_frames_dropped;
                    stats.received_frames = stats.reassembled_frames + stats.dropped_frames;
                    stats.decoded_frames = stream_gui_decoded_frames;
                    stats.rendered_frames = stream_gui_rendered_frames;
                    stats.reassembly_timeouts = qa_hold_loss_stats_for_expired_feedback
                        ? 0
                        : stream_reassembly_timeout_total;
                    stats.latest_received_frame_id = stream_latest_received_frame_id;
                    stats.latest_complete_frame_id = stream_latest_complete_frame_id;
                    stats.latest_complete_keyframe_id = stream_latest_complete_keyframe_id;
                    stats.observed_rate_revision = stream_observed_rate_revision;
                    stats.observed_source_activity_revision =
                        stream_observed_source_activity_revision;
                    stats.latest_displayable_frame_id = stream_latest_displayable_frame_id;
                    stats.latest_displayable_keyframe_id =
                        stream_latest_displayable_keyframe_id;
                    stats.latest_presented_frame_id = stream_latest_presented_frame_id;
                }
                std::string stats_error;
                (void)send_control_message(stats, &stats_error);
            }
        }

        redclaw::protocol::StreamControlMessageV1 queued_log_message;
        bool has_queued_log_message = false;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            if (stream_control_channel_open
                && pending_remote_input_messages.empty()
                && remote_input_session.queued_event_count() == 0
                && !pending_remote_log_messages.empty()) {
                queued_log_message = std::move(pending_remote_log_messages.front());
                pending_remote_log_messages.pop_front();
                has_queued_log_message = true;
            }
        }
        if (has_queued_log_message) {
            std::string log_send_error;
            if (!send_control_message(queued_log_message, &log_send_error)) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                if (pending_remote_log_messages.size() < 66) {
                    pending_remote_log_messages.push_front(std::move(queued_log_message));
                }
            }
        }

        input_loop_timing.next(redclaw::runtime::InputQaStage::kAdaptation);

        if (options.stream_smoke && options.role == RuntimeRole::kHost && heartbeat_due) {
            const std::uint64_t now_ms = now_steady_ms();
            redclaw::session::DesktopSourceActivityUpdate activity_update;
            {
                std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
                activity_update = stream_source_activity_tracker.tick(now_ms);
            }
            apply_source_activity_update(activity_update);

            std::uint32_t source_srtt_ms = 0;
            std::uint32_t source_pacing_kbps = kDesktopStreamTransportBitrateCeilingKbps;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                source_srtt_ms = stream_rtt_telemetry_snapshot.smoothed_rtt_ms;
                if (stream_congestion_decision_snapshot.pacing_bitrate_kbps != 0) {
                    source_pacing_kbps =
                        stream_congestion_decision_snapshot.pacing_bitrate_kbps;
                }
            }
            const auto transport_estimate = stream_transport_estimator.snapshot(
                now_ms * 1000ULL, source_srtt_ms);
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                stream_transport_estimate_snapshot = transport_estimate;
            }
            stream_media_pacer.update_budget(
                source_pacing_kbps, source_srtt_ms, transport_estimate.in_flight_bytes);
            const auto pacer_snapshot = stream_media_pacer.telemetry();
            const bool budget_waiting = pacer_snapshot.rejected_wire_bytes != 0;
            if (!stream_last_published_budget_wait.has_value()
                || *stream_last_published_budget_wait != budget_waiting) {
                redclaw::session::DesktopSourceActivitySnapshot source;
                {
                    std::lock_guard source_lock(stream_source_activity_mutex);
                    source = stream_source_activity_tracker.snapshot();
                }
                if (publish_source_activity(source)) stream_last_published_budget_wait = budget_waiting;
            }
            bool retry_reference = false;
            {
                std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
                retry_reference = transport_estimate.in_flight_bytes == 0
                    && pacer_snapshot.active_depth + pacer_snapshot.pending_depth == 0
                    && stream_media_pacer.admission().ready()
                    && stream_source_activity_tracker.reference_retry_due(now_ms, source_srtt_ms);
                if (retry_reference) {
                    activity_update = stream_source_activity_tracker.mark_reference_retry(now_ms);
                }
            }
            if (retry_reference) {
                apply_source_activity_update(activity_update);
            }
            bool channel_open = false;
            bool should_send_ping = false;
            std::uint64_t ping_sequence = 0;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                channel_open = stream_required_channels_ready
                    && stream_media_channel_open
                    && stream_control_channel_open;
                if (channel_open) {
                    if (stream_rtt_telemetry_snapshot.in_flight_ping_sequence != 0
                        && now_ms >= stream_rtt_telemetry_snapshot.in_flight_ping_sent_steady_ms
                        && now_ms - stream_rtt_telemetry_snapshot.in_flight_ping_sent_steady_ms >= kDesktopStreamRttPingTimeoutMs) {
                        stream_rtt_telemetry_snapshot.in_flight_ping_sequence = 0;
                        stream_rtt_telemetry_snapshot.in_flight_ping_sent_steady_ms = 0;
                        ++stream_rtt_telemetry_snapshot.ping_timeout_count;
                    }

                    if (next_stream_rtt_ping_steady_ms == 0) {
                        next_stream_rtt_ping_steady_ms = now_ms;
                    }

                    if (stream_rtt_telemetry_snapshot.in_flight_ping_sequence == 0
                        && now_ms >= next_stream_rtt_ping_steady_ms) {
                        ping_sequence = stream_rtt_telemetry_snapshot.last_sent_ping_sequence + 1;
                        should_send_ping = true;
                    }
                }
            }

            if (channel_open && should_send_ping) {
                auto ping_message = make_control_message(
                    redclaw::protocol::StreamControlMessageTypeV1::kPing);
                ping_message.log_cursor = ping_sequence;

                std::string ping_error;
                if (send_control_message(ping_message, &ping_error)) {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    stream_rtt_telemetry_snapshot.last_sent_ping_sequence = ping_sequence;
                    stream_rtt_telemetry_snapshot.in_flight_ping_sequence = ping_sequence;
                    stream_rtt_telemetry_snapshot.in_flight_ping_sent_steady_ms = now_ms;
                    ++stream_rtt_telemetry_snapshot.ping_sent_count;
                    next_stream_rtt_ping_steady_ms = now_ms + kDesktopStreamRttPingIntervalMs;
                } else {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    next_stream_rtt_ping_steady_ms = now_ms + kDesktopStreamRttPingIntervalMs;
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kWarning,
                        "stream",
                        "stream RTT ping send failed: " + ping_error);
                }
            }
        }

        bool stream_adaptation_session_active = false;
        if (options.stream_smoke && options.role == RuntimeRole::kHost) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            stream_adaptation_session_active = stream_media_channel_open
                && stream_control_channel_open
                && !saw_failed_state
                && required_channel_closed_at_ms == 0;
        }
        redclaw::session::DesktopSourceActivitySnapshot adaptation_source_snapshot;
        if (stream_adaptation_session_active) {
            std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
            adaptation_source_snapshot = stream_source_activity_tracker.snapshot();
            const auto source_state = adaptation_source_snapshot.state;
            if (source_state == redclaw::session::DesktopSourceActivityState::kStatic
                || source_state == redclaw::session::DesktopSourceActivityState::kStaticPending) {
                stream_adaptation_session_active = false;
            }
        }
        if (options.stream_smoke && options.role == RuntimeRole::kHost
            && elapsed_seconds > 0 && heartbeat_due
            && stream_adaptation_session_active) {
            DesktopStreamSmokeCounters stream_snapshot;
            redclaw::capture::CaptureBackendTelemetry capture_telemetry;
            redclaw::capture::EncoderExecutionDiagnostics encoder_diagnostics;
            StreamRttTelemetry rtt_telemetry;
            StreamAdaptiveControlState adaptive_control;
            redclaw::protocol::StreamControlMessageV1 receiver_stats;
            redclaw::net::DataChannelTransportStats media_transport_stats;
            std::uint32_t source_width = 0;
            std::uint32_t source_height = 0;
            std::uint32_t encoded_width = 0;
            std::uint32_t encoded_height = 0;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                stream_snapshot = stream_counters;
                capture_telemetry = stream_capture_telemetry_snapshot;
                encoder_diagnostics = stream_encoder_diagnostics_snapshot;
                rtt_telemetry = stream_rtt_telemetry_snapshot;
                adaptive_control = stream_adaptive_control_snapshot;
                receiver_stats = stream_receiver_stats_snapshot;
                media_transport_stats = stream_data_channel_transport_stats_snapshot;
                source_width = stream_source_width_snapshot;
                source_height = stream_source_height_snapshot;
                encoded_width = stream_encoded_width_snapshot;
                encoded_height = stream_encoded_height_snapshot;
            }

            const std::uint64_t adaptation_sample_ms = now_steady_ms();
            const std::uint64_t adaptation_window_ms =
                adaptation_sample_ms >= last_stream_adaptation_sample_ms
                    ? (adaptation_sample_ms - last_stream_adaptation_sample_ms)
                    : 0;
            if (adaptation_window_ms > 0) {
                const std::uint64_t now_ms = adaptation_sample_ms;
                const bool recent_playback_starvation =
                    adaptive_control.last_playback_starvation_ms != 0
                    && now_ms >= adaptive_control.last_playback_starvation_ms
                    && now_ms - adaptive_control.last_playback_starvation_ms
                           < kDesktopStreamPlaybackStarvationHoldMs;
                const std::uint32_t captured_delta = stream_counter_delta(
                    stream_snapshot.captured_frames,
                    last_stream_adaptation_snapshot.captured_frames);
                const std::uint32_t encoded_delta = stream_counter_delta(
                    stream_snapshot.encoded_frames,
                    last_stream_adaptation_snapshot.encoded_frames);
                const std::uint32_t source_gap = captured_delta > encoded_delta ? (captured_delta - encoded_delta) : 0;
                const std::uint32_t target_encode_budget =
                    static_cast<std::uint32_t>((std::min<std::uint64_t>)(
                        (static_cast<std::uint64_t>(resolve_stream_target_fps(adaptive_control))
                             * adaptation_window_ms
                         + 999ULL)
                            / 1000ULL,
                        std::numeric_limits<std::uint32_t>::max()));
                const std::uint32_t encode_budget_gap =
                    redclaw::net::resolve_available_encode_budget_gap(
                        captured_delta,
                        encoded_delta,
                        target_encode_budget);
                const auto encode_budget_pressure =
                    redclaw::net::evaluate_available_encode_budget_pressure(
                        captured_delta,
                        encoded_delta,
                        target_encode_budget);
                const std::uint32_t encoder_backpressure_delta = stream_counter_delta(
                    encoder_diagnostics.backpressure_event_count,
                    last_stream_adaptation_encoder_diagnostics_snapshot.backpressure_event_count);
                const std::uint32_t transmit_failures_delta = stream_counter_delta(
                    stream_snapshot.transmit_failures,
                    last_stream_adaptation_snapshot.transmit_failures);
                const std::uint32_t transmit_backpressure_delta = stream_counter_delta(
                    stream_snapshot.transmit_backpressure_drops,
                    last_stream_adaptation_snapshot.transmit_backpressure_drops);
                const std::uint32_t ping_timeout_delta = stream_counter_delta(
                    rtt_telemetry.ping_timeout_count,
                    last_stream_adaptation_rtt_snapshot.ping_timeout_count);
                const bool fresh_rtt_sample = stream_counter_delta(
                    rtt_telemetry.ping_ack_count,
                    last_stream_adaptation_rtt_snapshot.ping_ack_count) > 0;
                const auto rtt_signal = redclaw::net::evaluate_stream_rtt_signal(
                    rtt_telemetry.last_rtt_ms,
                    rtt_telemetry.min_rtt_ms,
                    kDesktopStreamAdaptiveReliefQueueDelayMs,
                    kDesktopStreamAdaptiveHighQueueDelayMs,
                    kDesktopStreamAdaptiveSevereQueueDelayMs);
                const bool rtt_sample_fresh = rtt_telemetry.last_ack_steady_ms != 0
                    && adaptation_sample_ms >= rtt_telemetry.last_ack_steady_ms
                    && adaptation_sample_ms - rtt_telemetry.last_ack_steady_ms <= 3000
                    && ping_timeout_delta == 0;
                const bool low_rtt_sample = rtt_sample_fresh
                    && rtt_signal.relief;
                const bool receiver_stats_match_current_revision =
                    receiver_stats.observed_rate_revision != 0
                    && receiver_stats.observed_rate_revision == adaptive_control.rate_revision
                    && last_stream_receiver_stats_snapshot.observed_rate_revision
                        == adaptive_control.rate_revision
                    && receiver_stats.observed_source_activity_revision != 0
                    && receiver_stats.observed_source_activity_revision
                        == adaptation_source_snapshot.revision
                    && last_stream_receiver_stats_snapshot.observed_source_activity_revision
                        == adaptation_source_snapshot.revision;
                const std::uint64_t receiver_dropped_delta =
                    receiver_stats_match_current_revision
                        ? stream_total_delta(
                              receiver_stats.dropped_frames,
                              last_stream_receiver_stats_snapshot.dropped_frames)
                        : 0;
                const std::uint64_t receiver_reassembled_delta =
                    receiver_stats_match_current_revision
                        ? stream_total_delta(
                              receiver_stats.reassembled_frames,
                              last_stream_receiver_stats_snapshot.reassembled_frames)
                        : 0;
                const std::uint64_t receiver_decoded_delta =
                    receiver_stats_match_current_revision
                        ? stream_total_delta(
                              receiver_stats.decoded_frames,
                              last_stream_receiver_stats_snapshot.decoded_frames)
                        : 0;
                const std::uint64_t receiver_dropped_keyframe_delta =
                    receiver_stats_match_current_revision
                        ? stream_total_delta(
                              receiver_stats.dropped_keyframes,
                              last_stream_receiver_stats_snapshot.dropped_keyframes)
                        : 0;
                const std::uint64_t receiver_reassembly_timeout_delta =
                    receiver_stats_match_current_revision
                        ? stream_total_delta(
                              receiver_stats.reassembly_timeouts,
                              last_stream_receiver_stats_snapshot.reassembly_timeouts)
                        : 0;
                const redclaw::net::ReceiverAssemblyQuality receiver_assembly_quality =
                    redclaw::net::evaluate_receiver_assembly_quality({
                        .completed_frames = receiver_reassembled_delta,
                        .dropped_frames = receiver_dropped_delta,
                        .dropped_keyframes = receiver_dropped_keyframe_delta,
                        .reassembly_timeouts = receiver_reassembly_timeout_delta,
                    });
                auto transport_estimate = stream_transport_estimator.snapshot(
                    adaptation_sample_ms * 1000ULL,
                    rtt_telemetry.smoothed_rtt_ms);
                if (transport_estimate.rate_revision != adaptive_control.rate_revision) {
                    transport_estimate.feedback_fresh = false;
                }
                // Fresh transport feedback is the single congestion authority.
                // Reassembly damage still requests an IDR, but must not multiply
                // the same loss into another rate backoff.
                const bool severe_receiver_damage = !transport_estimate.feedback_fresh
                    && (receiver_dropped_keyframe_delta > 0
                        || receiver_reassembly_timeout_delta > 0
                        || receiver_assembly_quality.pressure
                            >= redclaw::net::ReceiverAssemblyPressure::kDegraded);
                redclaw::net::MediaCongestionSample congestion_sample;
                congestion_sample.now_steady_ms = adaptation_sample_ms;
                const std::uint32_t encoder_transport_budget_kbps =
                    resolve_stream_target_bitrate_kbps(adaptive_control);
                congestion_sample.encoder_target_bitrate_kbps =
                    encoder_transport_budget_kbps == 0
                        ? kDesktopStreamTransportBitrateCeilingKbps
                        : std::min<std::uint32_t>(
                              encoder_transport_budget_kbps,
                              kDesktopStreamTransportBitrateCeilingKbps);
                congestion_sample.smoothed_rtt_ms = rtt_telemetry.smoothed_rtt_ms;
                congestion_sample.rtt_queue_delay_ms = rtt_signal.queue_delay_ms;
                congestion_sample.rtt_sample_id = rtt_telemetry.ping_ack_count;
                congestion_sample.rtt_fresh = rtt_sample_fresh;
                congestion_sample.local_backpressure = severe_receiver_damage
                    || transmit_failures_delta > 0
                    || media_transport_stats.send_blocked
                    || media_transport_stats.buffered_amount
                        >= kDesktopStreamDataChannelBufferedAmountHighWatermarkBytes;
                congestion_sample.transport = transport_estimate;
                const auto admission_telemetry = stream_media_pacer.telemetry();
                congestion_sample.recovery_budget_blocked = admission_telemetry.keyframe_required
                    && admission_telemetry.rejected_wire_bytes != 0;
                congestion_sample.media_channel_open = media_transport_stats.open;
                congestion_sample.buffered_amount = media_transport_stats.buffered_amount;
                const auto congestion_decision = stream_congestion_controller.update(
                    congestion_sample);
                const bool receiver_decode_network_stable =
                    congestion_decision.pressure == redclaw::net::MediaNetworkPressure::kStable
                    && transport_estimate.feedback_fresh
                    && !recent_playback_starvation
                    && transmit_failures_delta == 0
                    && transmit_backpressure_delta == 0
                    && encoder_backpressure_delta == 0
                    && encode_budget_pressure
                        == redclaw::net::AvailableEncodeBudgetPressure::kStable
                    && low_rtt_sample;
                const auto receiver_decode_capacity =
                    stream_receiver_decode_capacity_controller.update({
                        .reassembled_frames = receiver_reassembled_delta,
                        .decoded_frames = receiver_decoded_delta,
                        .window_ms = adaptation_window_ms,
                        .current_target_fps = clamp_stream_target_fps(adaptive_control.target_fps),
                        .maximum_target_fps = kDesktopStreamTargetFps,
                        .revision_consistent = receiver_stats_match_current_revision,
                        .source_active = adaptation_source_snapshot.state
                            == redclaw::session::DesktopSourceActivityState::kActive,
                        .network_stable = receiver_decode_network_stable,
                        .hard_pressure = congestion_decision.backoff || ping_timeout_delta > 0
                            || encoder_backpressure_delta > 0,
                        .rate_revision = adaptive_control.rate_revision,
                        .source_revision = adaptation_source_snapshot.revision,
                        .pending_frames = receiver_stats.latest_complete_frame_id
                            > receiver_stats.latest_displayable_frame_id
                            ? receiver_stats.latest_complete_frame_id - receiver_stats.latest_displayable_frame_id : 0,
                    });
                stream_media_pacer.update_budget(
                    congestion_decision.pacing_bitrate_kbps,
                    rtt_telemetry.smoothed_rtt_ms,
                    transport_estimate.in_flight_bytes, &congestion_decision.recovery_probe);
                const auto pacer_telemetry = stream_media_pacer.telemetry();
                std::string low_threshold_error;
                (void)ice_wrapper.setDataChannelBufferedAmountLowThreshold(
                    redclaw::net::DataChannelKind::kMedia,
                    std::max<std::size_t>(
                        kDesktopStreamVideoFragmentPacketBytes,
                        pacer_telemetry.buffered_limit_bytes / 2),
                    &low_threshold_error);
                const std::uint32_t capture_fps_hint = stream_fps_hint_from_rate(
                    stream_rate_per_second(captured_delta, adaptation_window_ms));
                const std::uint32_t frame_present_fps_hint = stream_fps_hint_from_frame_present_delta(
                    capture_telemetry.frame_present_delta_ms);

                std::uint32_t source_fps_hint = 0;
                if (capture_fps_hint > 0) {
                    source_fps_hint = capture_fps_hint;
                } else if (frame_present_fps_hint > 0) {
                    source_fps_hint = frame_present_fps_hint;
                }
                if (source_fps_hint == 0) {
                    source_fps_hint = clamp_stream_target_fps(adaptive_control.target_fps);
                }

                std::uint32_t desired_fps = redclaw::net::select_stream_target_fps(
                    clamp_stream_target_fps(adaptive_control.target_fps),
                    encoder_backpressure_delta > 0, congestion_decision, receiver_decode_capacity);
                desired_fps = clamp_stream_target_fps(desired_fps);
                if (recent_playback_starvation
                    && adaptive_control.applied_fps != 0) {
                    desired_fps = std::min(desired_fps, clamp_stream_target_fps(adaptive_control.applied_fps));
                }

                std::uint32_t desired_bitrate_kbps = adaptive_control.target_bitrate_kbps;
                std::uint32_t desired_max_bitrate_kbps = adaptive_control.target_max_bitrate_kbps;
                const std::uint32_t bitrate_width = encoded_width > 0 ? encoded_width : source_width;
                const std::uint32_t bitrate_height = encoded_height > 0 ? encoded_height : source_height;
                if (bitrate_width > 0 && bitrate_height > 0) {
                    redclaw::capture::EncoderProfileRequest adaptive_profile_request;
                    adaptive_profile_request.width = bitrate_width;
                    adaptive_profile_request.height = bitrate_height;
                    adaptive_profile_request.fps = desired_fps;
                    adaptive_profile_request.workload = redclaw::capture::EncoderWorkload::kInteractiveDesktop;
                    adaptive_profile_request.preferred_codec = redclaw::capture::EncoderCodec::kH264;

                    redclaw::capture::EncoderConfigProfile adaptive_profile;
                    std::string adaptive_profile_error;
                    if (redclaw::capture::build_low_latency_encoder_profile(
                            adaptive_profile_request,
                            &adaptive_profile,
                            &adaptive_profile_error)) {
                        // build_low_latency_encoder_profile already includes
                        // the target FPS in its pixels-per-second bitrate.
                        // Scaling it by FPS a second time made low cadence
                        // unnecessarily blurry.
                        const std::uint32_t scaled_base_bitrate_kbps =
                            adaptive_profile.target_bitrate_kbps;
                        const std::uint32_t quality_floor_kbps =
                            redclaw::capture::resolve_interactive_desktop_bitrate_floor_kbps(
                                bitrate_width,
                                bitrate_height,
                                desired_fps);

                        // Local encode/decode capacity pressure reduces cadence,
                        // never the per-frame clarity budget. Only transport
                        // congestion may govern the independent media pacer.
                        desired_bitrate_kbps = clamp_stream_bitrate_kbps(
                            scaled_base_bitrate_kbps,
                            quality_floor_kbps,
                            adaptive_profile.target_bitrate_kbps);
                        desired_max_bitrate_kbps = desired_bitrate_kbps + (desired_bitrate_kbps / 5);

                    }
                }

                const bool target_changed = desired_fps != adaptive_control.target_fps
                    || (desired_bitrate_kbps != 0 && desired_bitrate_kbps != adaptive_control.target_bitrate_kbps)
                    || (desired_max_bitrate_kbps != 0 && desired_max_bitrate_kbps != adaptive_control.target_max_bitrate_kbps);
                if (target_changed || congestion_decision.backoff) {
                    std::cout << "Runtime stream adaptation decision"
                              << " rate_revision=" << adaptive_control.rate_revision
                              << " source_revision=" << adaptation_source_snapshot.revision
                              << " fps_before=" << adaptive_control.target_fps << " fps_after=" << desired_fps
                              << " rtt_sample=" << rtt_telemetry.ping_ack_count
                              << " rtt_fresh=" << rtt_sample_fresh << " rtt_new=" << fresh_rtt_sample
                              << " rtt_queue_ms=" << rtt_signal.queue_delay_ms
                              << " media_sample=" << transport_estimate.feedback_sample_id
                              << " media_age_ms=" << transport_estimate.feedback_age_ms
                              << " media_queue_ms=" << transport_estimate.queue_delay_ms
                              << " loss_per_mille=" << transport_estimate.loss_per_mille
                              << " in_flight=" << transport_estimate.in_flight_bytes
                              << " buffered=" << media_transport_stats.buffered_amount
                              << " blocked=" << congestion_sample.local_backpressure
                              << " encoder_pressure=" << encoder_backpressure_delta
                              << " network_pressure=" << static_cast<int>(congestion_decision.pressure)
                              << " capacity_mature=" << receiver_decode_capacity.valid
                              << " capacity_pressure=" << receiver_decode_capacity.pressure
                              << " recovery_healthy=" << receiver_decode_network_stable
                              << " pacing_kbps=" << congestion_decision.pacing_bitrate_kbps << '\n';
                }
                const std::uint32_t applied_bitrate_kbps =
                    adaptive_control.applied_bitrate_kbps != 0
                        ? adaptive_control.applied_bitrate_kbps
                        : adaptive_control.target_bitrate_kbps;
                const std::uint32_t bitrate_difference_kbps =
                    desired_bitrate_kbps > applied_bitrate_kbps
                        ? (desired_bitrate_kbps - applied_bitrate_kbps)
                        : (applied_bitrate_kbps - desired_bitrate_kbps);
                std::uint32_t bitrate_reconfigure_threshold_kbps =
                    std::max<std::uint32_t>(1000, applied_bitrate_kbps / 6);
                if (receiver_assembly_quality.pressure
                    == redclaw::net::ReceiverAssemblyPressure::kSevere) {
                    bitrate_reconfigure_threshold_kbps =
                        std::max<std::uint32_t>(100, applied_bitrate_kbps / 20);
                } else if (receiver_assembly_quality.pressure
                           == redclaw::net::ReceiverAssemblyPressure::kDegraded) {
                    bitrate_reconfigure_threshold_kbps =
                        std::max<std::uint32_t>(200, applied_bitrate_kbps / 10);
                }
                const bool bitrate_reconfigure_needed = desired_bitrate_kbps != 0
                    && applied_bitrate_kbps != 0
                    && bitrate_difference_kbps >= bitrate_reconfigure_threshold_kbps;
                const bool urgent_network_or_encoder_pressure = transmit_failures_delta > 0
                    || congestion_decision.pressure
                        == redclaw::net::MediaNetworkPressure::kSevere
                    || recent_playback_starvation
                    || encode_budget_pressure
                        == redclaw::net::AvailableEncodeBudgetPressure::kSevere
                    || encoder_backpressure_delta >= kDesktopStreamAdaptiveBackpressureReconfigureThreshold;
                const bool pressure_window = urgent_network_or_encoder_pressure
                    || congestion_decision.pressure
                        != redclaw::net::MediaNetworkPressure::kStable
                    || encode_budget_pressure
                        != redclaw::net::AvailableEncodeBudgetPressure::kStable
                    || encoder_backpressure_delta >= 3
                    || transmit_backpressure_delta >= 3;
                const bool relief_window = ping_timeout_delta == 0
                    && transmit_failures_delta == 0
                    && transmit_backpressure_delta == 0
                    && receiver_dropped_delta == 0
                    && receiver_reassembly_timeout_delta == 0
                    && !recent_playback_starvation
                    && encode_budget_pressure
                        == redclaw::net::AvailableEncodeBudgetPressure::kStable
                    && encoder_backpressure_delta == 0
                    && low_rtt_sample
                    && transport_estimate.feedback_fresh
                    && congestion_decision.pressure
                        == redclaw::net::MediaNetworkPressure::kStable;

                std::uint32_t pressure_window_count = adaptive_control.pressure_window_count;
                std::uint32_t relief_window_count = adaptive_control.relief_window_count;
                if (pressure_window) {
                    pressure_window_count = std::min(
                        pressure_window_count + 1,
                        kDesktopStreamAdaptivePressureWindowThreshold);
                    relief_window_count = 0;
                } else if (relief_window) {
                    relief_window_count = std::min(
                        relief_window_count + 1,
                        kDesktopStreamAdaptiveReliefWindowThreshold);
                    if (relief_window_count >= kDesktopStreamAdaptiveReliefWindowThreshold) {
                        pressure_window_count = 0;
                    }
                } else {
                    relief_window_count = 0;
                    if (pressure_window_count > 0) {
                        --pressure_window_count;
                    }
                }

                const bool sustained_network_or_encoder_pressure =
                    pressure_window_count >= kDesktopStreamAdaptivePressureWindowThreshold;
                const bool direct_encoder_reconfigure_pressure = transmit_failures_delta > 0
                    || congestion_decision.pressure
                        == redclaw::net::MediaNetworkPressure::kSevere
                    || encoder_backpressure_delta >= kDesktopStreamAdaptiveBackpressureReconfigureThreshold;
                const bool encoder_reconfigure_needed = bitrate_reconfigure_needed
                    && (direct_encoder_reconfigure_pressure
                        || (!adaptive_control.playback_starvation_pacing_active
                            && sustained_network_or_encoder_pressure));

                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    stream_adaptive_control_snapshot.last_capture_fps_hint = capture_fps_hint;
                    stream_adaptive_control_snapshot.last_frame_present_fps_hint = frame_present_fps_hint;
                    stream_adaptive_control_snapshot.last_source_gap = source_gap;
                    stream_adaptive_control_snapshot.last_encode_budget_gap = encode_budget_gap;
                    stream_adaptive_control_snapshot.last_encoder_backpressure_delta = encoder_backpressure_delta;
                    stream_adaptive_control_snapshot.last_transmit_failures_delta = transmit_failures_delta;
                    stream_adaptive_control_snapshot.last_transmit_backpressure_delta = transmit_backpressure_delta;
                    stream_adaptive_control_snapshot.last_receiver_assembly_loss_per_mille =
                        receiver_assembly_quality.loss_per_mille;
                    stream_adaptive_control_snapshot.last_receiver_assembly_pressure =
                        receiver_assembly_quality.pressure;
                    stream_adaptive_control_snapshot.last_receiver_decode_fps =
                        receiver_decode_capacity.observed_decode_fps;
                    stream_adaptive_control_snapshot.receiver_decode_pressure_windows =
                        receiver_decode_capacity.pressure_windows;
                    stream_adaptive_control_snapshot.receiver_decode_stable_windows =
                        receiver_decode_capacity.stable_windows;
                    stream_adaptive_control_snapshot.receiver_decode_fps_decrease_total =
                        receiver_decode_capacity.decrease_total;
                    stream_adaptive_control_snapshot.receiver_decode_fps_increase_total =
                        receiver_decode_capacity.increase_total;
                    stream_adaptive_control_snapshot.last_rtt_ms = rtt_telemetry.last_rtt_ms;
                    stream_adaptive_control_snapshot.last_rtt_baseline_ms =
                        rtt_signal.baseline_rtt_ms;
                    stream_adaptive_control_snapshot.last_rtt_queue_delay_ms =
                        rtt_signal.queue_delay_ms;
                    stream_transport_estimate_snapshot = transport_estimate;
                    stream_congestion_decision_snapshot = congestion_decision;
                    stream_adaptive_control_snapshot.pressure_window_count = pressure_window_count;
                    stream_adaptive_control_snapshot.relief_window_count = relief_window_count;
                    if (relief_window_count >= kDesktopStreamAdaptiveReliefWindowThreshold) {
                        stream_adaptive_control_snapshot.playback_starvation_pacing_active = false;
                    }
                    stream_adaptive_control_snapshot.target_fps = desired_fps;
                    if (desired_bitrate_kbps != 0) {
                        stream_adaptive_control_snapshot.target_bitrate_kbps = desired_bitrate_kbps;
                    }
                    if (desired_max_bitrate_kbps != 0) {
                        stream_adaptive_control_snapshot.target_max_bitrate_kbps = desired_max_bitrate_kbps;
                    }
                    if (target_changed) {
                        ++stream_adaptive_control_snapshot.rate_revision;
                        stream_work_coordinator.post(redclaw::session::HostStreamWorkReason::kRateChange);
                    }

                    if (target_changed
                        && encoder_reconfigure_needed
                        && (stream_adaptive_control_snapshot.last_reconfigure_ms == 0
                            || adaptation_sample_ms - stream_adaptive_control_snapshot.last_reconfigure_ms
                                   >= kDesktopStreamAdaptiveReconfigureCooldownMs)) {
                        stream_adaptive_control_snapshot.last_reconfigure_ms = adaptation_sample_ms;
                        stream_adaptive_control_snapshot.encoder_reconfigure_pending = true;
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kInfo,
                            "stream",
                            "adaptive stream retarget scheduled fps="
                                + std::to_string(desired_fps)
                                + " bitrate_kbps="
                                + std::to_string(desired_bitrate_kbps)
                                + " pressure_windows="
                                + std::to_string(pressure_window_count)
                                + " rtt_ms="
                                + std::to_string(rtt_telemetry.last_rtt_ms)
                                + " rtt_baseline_ms="
                                + std::to_string(rtt_signal.baseline_rtt_ms)
                                + " rtt_queue_delay_ms="
                                + std::to_string(rtt_signal.queue_delay_ms)
                                + " backpressure_delta="
                                + std::to_string(encoder_backpressure_delta)
                                + " source_gap="
                                + std::to_string(source_gap)
                                + " encode_budget_gap="
                                + std::to_string(encode_budget_gap)
                                + " transmit_failures_delta="
                                + std::to_string(transmit_failures_delta)
                                + " receiver_assembly_loss_per_mille="
                                + std::to_string(receiver_assembly_quality.loss_per_mille));
                    }
                }

                last_stream_adaptation_snapshot = stream_snapshot;
                last_stream_adaptation_encoder_diagnostics_snapshot = encoder_diagnostics;
                last_stream_adaptation_rtt_snapshot = rtt_telemetry;
                last_stream_receiver_stats_snapshot = receiver_stats;
                last_stream_adaptation_sample_ms = adaptation_sample_ms;
            }
        } else if (options.stream_smoke && options.role == RuntimeRole::kHost
                   && elapsed_seconds > 0 && heartbeat_due) {
            std::lock_guard<std::mutex> lock(callback_mutex);
            stream_adaptive_control_snapshot.encoder_reconfigure_pending = false;
            stream_adaptive_control_snapshot.rate_control_restart_fallback_revision = 0;
            last_stream_adaptation_snapshot = stream_counters;
            last_stream_adaptation_encoder_diagnostics_snapshot =
                stream_encoder_diagnostics_snapshot;
            last_stream_adaptation_rtt_snapshot = stream_rtt_telemetry_snapshot;
            last_stream_receiver_stats_snapshot = stream_receiver_stats_snapshot;
            last_stream_adaptation_sample_ms = now_steady_ms();
        }

        input_loop_timing.next(redclaw::runtime::InputQaStage::kOtherSignaling);

        if (use_tcp_transport) {
#ifdef _WIN32
            if (!tcp_peer.valid()) {
                if (options.role == RuntimeRole::kHost) {
                    bool accepted = false;
                    if (!try_accept_tcp_peer(tcp_listener.value, &tcp_peer, &accepted, &runtime_error)) {
                        record_tcp_failure(TcpSignalFailureCategory::kAcceptFailed, WSAGetLastError(), runtime_error);
                        std::cerr << "Runtime TCP accept failed (non-fatal): " << runtime_error << '\n';
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kWarning,
                            "signal",
                            "tcp accept failed, runtime keeps listening");
                        runtime_error.clear();
                    }

                    if (accepted) {
                        const bool is_reconnect = tcp_diagnostics.connect_success_count > 0;
                        if (is_reconnect) {
                            ++tcp_diagnostics.reconnect_count;
                        }
                        ++tcp_diagnostics.connect_success_count;
                        record_tcp_failure(TcpSignalFailureCategory::kNone, 0, "");
                        if (is_reconnect) {
                            std::lock_guard<std::mutex> lock(callback_mutex);
                            if (pending_outbound_signal_events.empty() && !outbound_signal_history.empty()) {
                                pending_outbound_signal_events.insert(
                                    pending_outbound_signal_events.end(),
                                    outbound_signal_history.begin(),
                                    outbound_signal_history.end());
                                append_timeline(
                                    redclaw::render::RuntimeStatusSeverity::kInfo,
                                    "signal",
                                    "tcp signaling reconnected, replayed local signaling snapshot");
                            }
                        }
                        std::cout << "Runtime TCP signaling peer connected role=" << role_name << '\n';
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kInfo,
                            "signal",
                            "tcp signaling peer connected");
                    }
                } else {
                    const std::uint64_t now_ms = now_steady_ms();
                    if (now_ms >= tcp_next_connect_attempt_ms) {
                        ++tcp_diagnostics.connect_attempt_count;
                        std::string connect_error;
                        bool resolve_failed = false;
                        int connect_wsa_error = 0;
                        if (connect_tcp_peer(
                                options.target_host,
                                tcp_signaling_port,
                                &tcp_peer,
                                &resolve_failed,
                                &connect_wsa_error,
                                &connect_error)) {
                            const bool is_reconnect = tcp_diagnostics.connect_success_count > 0;
                            if (is_reconnect) {
                                ++tcp_diagnostics.reconnect_count;
                            }
                            ++tcp_diagnostics.connect_success_count;
                            tcp_connect_backoff_ms = 250;
                            tcp_diagnostics.current_backoff_ms = tcp_connect_backoff_ms;
                            tcp_next_connect_attempt_ms = now_ms;
                            record_tcp_failure(TcpSignalFailureCategory::kNone, 0, "");
                            if (is_reconnect) {
                                std::lock_guard<std::mutex> lock(callback_mutex);
                                if (pending_outbound_signal_events.empty() && !outbound_signal_history.empty()) {
                                    pending_outbound_signal_events.insert(
                                        pending_outbound_signal_events.end(),
                                        outbound_signal_history.begin(),
                                        outbound_signal_history.end());
                                    append_timeline(
                                        redclaw::render::RuntimeStatusSeverity::kInfo,
                                        "signal",
                                        "tcp signaling reconnected, replayed local signaling snapshot");
                                }
                            }
                            std::cout << "Runtime TCP signaling connected to host=" << options.target_host
                                      << " port=" << tcp_signaling_port << '\n';
                            std::lock_guard<std::mutex> lock(callback_mutex);
                            append_timeline(
                                redclaw::render::RuntimeStatusSeverity::kInfo,
                                "signal",
                                "tcp signaling connected to remote peer");
                        } else {
                            const TcpSignalFailureCategory category = resolve_failed
                                ? TcpSignalFailureCategory::kResolveFailed
                                : TcpSignalFailureCategory::kConnectFailed;
                            record_tcp_failure(category, connect_wsa_error, connect_error);

                            const std::uint32_t jitter_ms = (tcp_diagnostics.connect_attempt_count % 5) * 50;
                            tcp_next_connect_attempt_ms = now_ms + tcp_connect_backoff_ms + jitter_ms;
                            tcp_connect_backoff_ms = std::min(tcp_connect_backoff_ms * 2, kTcpConnectBackoffMaxMs);
                            tcp_diagnostics.current_backoff_ms = tcp_connect_backoff_ms;
                        }
                    }
                }
            }

            if (tcp_peer.valid()) {
                std::vector<std::vector<std::string>> outbound_events;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    outbound_events.swap(pending_outbound_signal_events);
                }

                for (std::size_t event_index = 0; event_index < outbound_events.size(); ++event_index) {
                    const auto& event_fields = outbound_events[event_index];
                    std::string send_error;
                    if (!send_socket_all(tcp_peer.value, format_signal_event_line(event_fields), &send_error)) {
                        std::cerr << "Runtime TCP signaling send failed: " << send_error << '\n';
                        ++tcp_diagnostics.send_failure_count;
                        ++tcp_diagnostics.disconnect_count;
                        record_tcp_failure(TcpSignalFailureCategory::kSendFailed, WSAGetLastError(), send_error);
                        tcp_peer.reset();
                        {
                            std::lock_guard<std::mutex> lock(callback_mutex);
                            append_timeline(
                                redclaw::render::RuntimeStatusSeverity::kWarning,
                                "signal",
                                "tcp signaling send failed, resetting session");
                        }
                        if (options.role == RuntimeRole::kController) {
                            tcp_next_connect_attempt_ms = now_steady_ms();
                        }
                        if (!restart_tcp_signaling_session("tcp send failure")) {
                            std::cerr << "Runtime failed to restart tcp signaling session after send failure" << '\n';
                            ice_wrapper.close();
                            return 1;
                        }
                        break;
                    }
                }

                if (tcp_peer.valid()) {
                    std::vector<std::string> inbound_lines;
                    bool peer_closed = false;
                    int read_wsa_error = 0;
                    std::string read_error;
                    if (!read_socket_lines(
                            tcp_peer.value,
                            &tcp_receive_buffer,
                            &inbound_lines,
                            &peer_closed,
                            &read_wsa_error,
                            &read_error)) {
                        ++tcp_diagnostics.receive_failure_count;
                        ++tcp_diagnostics.disconnect_count;
                        record_tcp_failure(TcpSignalFailureCategory::kReceiveFailed, read_wsa_error, read_error);
                        std::cerr << "Runtime TCP signaling receive failed (recoverable): " << read_error << '\n';
                        tcp_peer.reset();
                        if (options.role == RuntimeRole::kController) {
                            tcp_next_connect_attempt_ms = now_steady_ms();
                        }
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kWarning,
                            "signal",
                            "tcp signaling receive failed, resetting session");
                        if (!restart_tcp_signaling_session("tcp receive failure")) {
                            std::cerr << "Runtime failed to restart tcp signaling session after receive failure" << '\n';
                            ice_wrapper.close();
                            return 1;
                        }
                        continue;
                    }

                    if (peer_closed) {
                        tcp_peer.reset();
                        ++tcp_diagnostics.disconnect_count;
                        record_tcp_failure(
                            TcpSignalFailureCategory::kPeerDisconnected,
                            read_wsa_error,
                            "peer closed tcp signaling connection");
                        if (options.role == RuntimeRole::kController) {
                            tcp_next_connect_attempt_ms = now_steady_ms();
                        }
                        {
                            std::lock_guard<std::mutex> lock(callback_mutex);
                            append_timeline(
                                redclaw::render::RuntimeStatusSeverity::kWarning,
                                "signal",
                                "tcp signaling peer disconnected, resetting session");
                        }
                        if (!restart_tcp_signaling_session("peer disconnected")) {
                            std::cerr << "Runtime failed to restart tcp signaling session after peer disconnect" << '\n';
                            ice_wrapper.close();
                            return 1;
                        }
                        continue;
                    }

                    for (const std::string& line : inbound_lines) {
                        if (line.empty()) {
                            continue;
                        }

                        std::vector<std::string> fields;
                        if (!parse_event_log_line(line, &fields) || fields.empty()) {
                            std::cerr << "Runtime skipped malformed tcp signaling line" << '\n';
                            ++tcp_diagnostics.malformed_message_count;
                            record_tcp_failure(
                                TcpSignalFailureCategory::kMalformedMessage,
                                0,
                                "malformed tcp signaling line");
                            continue;
                        }

                        if (fields[0] == "description") {
                            if (fields.size() < 3 || remote_description_applied) {
                                continue;
                            }

                            const bool event_is_offer = fields[1] == "offer";
                            std::string apply_error;
                            if (!ensure_ice_gathering_started("controller ICE gathering started after remote tcp description", &apply_error)
                                || !ice_wrapper.applyRemoteDescription(fields[2], event_is_offer, &apply_error)) {
                                std::cerr << "Runtime failed to apply remote tcp description: " << apply_error << '\n';
                                ice_wrapper.close();
                                return 1;
                            }

                            remote_description_applied = true;
                            remote_description_ever_applied = true;
                            std::cout << "Runtime remote description applied role=" << role_name << '\n';
                            {
                                std::lock_guard<std::mutex> lock(callback_mutex);
                                append_timeline(
                                    redclaw::render::RuntimeStatusSeverity::kInfo,
                                    "signal",
                                    std::string("remote ") + (event_is_offer ? "offer" : "answer") + " applied");
                            }
                            continue;
                        }

                        if (fields[0] == "candidate") {
                            if (fields.size() < 3) {
                                continue;
                            }

                            const std::string& remote_mid = fields[1];
                            const std::string& remote_candidate = fields[2];
                            if (!remote_description_applied) {
                                pending_remote_candidates.emplace_back(remote_mid, remote_candidate);
                                continue;
                            }

                            std::string apply_error;
                            if (!ice_wrapper.applyRemoteCandidate(remote_candidate, remote_mid, &apply_error)) {
                                std::cerr << "Runtime failed to apply remote tcp candidate: " << apply_error << '\n';
                                ice_wrapper.close();
                                return 1;
                            }

                            std::lock_guard<std::mutex> lock(callback_mutex);
                            record_candidate(&candidate_diagnostics, false, classify_candidate_type(remote_candidate), remote_candidate);
                        }
                    }
                }
            }
#endif
        }

#ifdef _WIN32
        if (use_rendezvous_transport) {
            constexpr std::uint64_t kRendezvousPollIntervalMs = 250;
            const std::uint64_t rendezvous_now_ms = now_steady_ms();
            const bool rendezvous_poll_due = rendezvous_next_poll_ms == 0
                || rendezvous_now_ms >= rendezvous_next_poll_ms;
            if (rendezvous_poll_due) {
                rendezvous_next_poll_ms = rendezvous_now_ms + kRendezvousPollIntervalMs;
            }

            if (options.role == RuntimeRole::kHost
                && !rendezvous_host_claim_observed
                && !rendezvous_session_id.empty()
                && rendezvous_poll_due) {
                RendezvousLookupHttpResult lookup_result;
                std::string lookup_error;
                ++rendezvous_diagnostics.lookup_attempts;
                if (rendezvous_lookup_session_http(
                        rendezvous_base_url,
                        options.session_code,
                        &lookup_result,
                        &lookup_error)) {
                    if (lookup_result.found) {
                        ++rendezvous_diagnostics.lookup_hits;
                    }
                    if (lookup_result.found && lookup_result.claimed) {
                        std::string start_error;
                        if (!ensure_ice_gathering_started(
                                "Host ICE gathering started after rendezvous claim",
                                &start_error)) {
                            std::cerr << "Runtime failed to start Host ICE after rendezvous claim: "
                                      << start_error << '\n';
                            ice_wrapper.close();
                            return 1;
                        }
                        rendezvous_host_claim_observed = true;
                        rendezvous_diagnostics.last_error.clear();
                        std::cout << "Runtime rendezvous claim observed role=" << role_name
                                  << " session_id=" << rendezvous_session_id
                                  << " action=start_fresh_host_ice" << '\n';
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kInfo,
                            "signal",
                            "Controller rendezvous claim observed; starting fresh Host ICE session");
                    }
                } else {
                    rendezvous_diagnostics.last_error = lookup_error;
                }
            }

            if (options.role == RuntimeRole::kController
                && rendezvous_session_id.empty()
                && rendezvous_poll_due) {
                RendezvousLookupHttpResult lookup_result;
                std::string lookup_error;
                ++rendezvous_diagnostics.lookup_attempts;
                if (rendezvous_lookup_session_http(
                        rendezvous_base_url,
                        options.session_code,
                        &lookup_result,
                        &lookup_error)) {
                    if (lookup_result.found) {
                        ++rendezvous_diagnostics.lookup_hits;
                        std::cout << "Runtime rendezvous lookup hit role=" << role_name
                                  << " session_code=" << options.session_code
                                  << " claimed=" << (lookup_result.claimed ? "true" : "false")
                                  << " expires_at_unix=" << lookup_result.expires_at_unix
                                  << '\n';
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kInfo,
                            "signal",
                            "rendezvous lookup found peer session metadata");

                        if (lookup_result.claimed) {
                            rendezvous_diagnostics.last_error = "rendezvous session code already claimed";
                        } else {
                            RendezvousClaimHttpResult claim_result;
                            std::string claim_error;
                            ++rendezvous_diagnostics.claim_attempts;
                            if (rendezvous_claim_session_http(
                                    rendezvous_base_url,
                                    options.session_code,
                                    &claim_result,
                                    &claim_error)) {
                                if (claim_result.claimed) {
                                    ++rendezvous_diagnostics.claim_success;
                                    rendezvous_session_id = claim_result.session_id;
                                    rendezvous_diagnostics.last_error.clear();
                                    std::cout << "Runtime rendezvous claim succeeded role=" << role_name
                                              << " session_code=" << options.session_code
                                              << " session_id=" << rendezvous_session_id
                                              << '\n';
                                    append_timeline(
                                        redclaw::render::RuntimeStatusSeverity::kInfo,
                                        "signal",
                                        "rendezvous session claim succeeded");
                                } else {
                                    rendezvous_diagnostics.last_error = "rendezvous claim rejected error=" + claim_result.error;
                                }
                            } else {
                                rendezvous_diagnostics.last_error = claim_error;
                            }
                        }
                    }
                } else {
                    rendezvous_diagnostics.last_error = lookup_error;
                }
            }

            bool should_publish_rendezvous_signal = false;
            std::string local_description_snapshot;
            std::vector<std::string> local_candidate_snapshot;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                if (!rendezvous_session_id.empty()
                    && signal_snapshot_dirty
                    && !local_description_sdp.empty()) {
                    should_publish_rendezvous_signal = true;
                    local_description_snapshot = local_description_sdp;
                    local_candidate_snapshot = local_candidate_lines;
                    signal_snapshot_dirty = false;
                }
            }

            if (should_publish_rendezvous_signal) {
                RendezvousPublishHttpResult publish_result;
                std::string publish_error;
                ++rendezvous_diagnostics.publish_attempts;
                if (rendezvous_publish_signal_http(
                        rendezvous_base_url,
                        rendezvous_session_id,
                        options.role == RuntimeRole::kHost ? "host" : "controller",
                        options.role == RuntimeRole::kHost ? "offer" : "answer",
                        local_description_snapshot,
                        local_candidate_snapshot,
                        &publish_result,
                        &publish_error)) {
                    if (publish_result.accepted) {
                        ++rendezvous_diagnostics.publish_success;
                        rendezvous_diagnostics.local_revision = publish_result.revision;
                        rendezvous_diagnostics.last_error.clear();
                        std::cout << "Runtime rendezvous signal published role=" << role_name
                                  << " session_id=" << rendezvous_session_id
                                  << " revision=" << publish_result.revision
                                  << " candidates=" << local_candidate_snapshot.size()
                                  << '\n';
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kInfo,
                            "signal",
                            "local rendezvous signal snapshot published");
                    } else {
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        signal_snapshot_dirty = true;
                        rendezvous_diagnostics.last_error = "rendezvous publish rejected error=" + publish_result.error;
                    }
                } else {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    signal_snapshot_dirty = true;
                    rendezvous_diagnostics.last_error = publish_error;
                }
            }

            if (!rendezvous_session_id.empty() && rendezvous_poll_due) {
                RendezvousSignalSnapshot remote_signal;
                std::string fetch_error;
                ++rendezvous_diagnostics.fetch_attempts;
                if (rendezvous_fetch_signal_http(
                        rendezvous_base_url,
                        rendezvous_session_id,
                        options.role == RuntimeRole::kHost ? "controller" : "host",
                        &remote_signal,
                        &fetch_error)) {
                    if (remote_signal.found && remote_signal.revision > rendezvous_diagnostics.remote_revision) {
                        ++rendezvous_diagnostics.fetch_hits;
                        if (!remote_description_applied && !remote_signal.description_sdp.empty()) {
                            const bool remote_is_offer = remote_signal.description_type == "offer";
                            std::string apply_error;
                            if (!ensure_ice_gathering_started("controller ICE gathering started after remote rendezvous description", &apply_error)
                                || !ice_wrapper.applyRemoteDescription(remote_signal.description_sdp, remote_is_offer, &apply_error)) {
                                std::cerr << "Runtime failed to apply remote rendezvous description: " << apply_error << '\n';
                                ice_wrapper.close();
                                return 1;
                            }

                            remote_description_applied = true;
                            remote_description_ever_applied = true;
                            std::cout << "Runtime remote description applied role=" << role_name << " (rendezvous)\n";
                            append_timeline(
                                redclaw::render::RuntimeStatusSeverity::kInfo,
                                "signal",
                                std::string("remote ") + remote_signal.description_type + " applied from rendezvous");
                        }

                        for (std::size_t i = applied_remote_rendezvous_candidates;
                             i < remote_signal.candidate_lines.size();
                             ++i) {
                            const auto parsed = parse_candidate_line(remote_signal.candidate_lines[i]);
                            if (!parsed.has_value()) {
                                std::cerr << "Runtime skipped invalid rendezvous candidate snapshot entry" << '\n';
                                continue;
                            }

                            std::string apply_error;
                            if (!ice_wrapper.applyRemoteCandidate(parsed->second, parsed->first, &apply_error)) {
                                std::cerr << "Runtime failed to apply remote rendezvous candidate: " << apply_error << '\n';
                                ice_wrapper.close();
                                return 1;
                            }

                            std::lock_guard<std::mutex> lock(callback_mutex);
                            record_candidate(&candidate_diagnostics, false, classify_candidate_type(parsed->second), parsed->second);
                        }

                        applied_remote_rendezvous_candidates = remote_signal.candidate_lines.size();
                        rendezvous_diagnostics.remote_revision = remote_signal.revision;
                        rendezvous_diagnostics.last_error.clear();
                    }
                } else {
                    rendezvous_diagnostics.last_error = fetch_error;
                }
            }
        }
#endif

        input_loop_timing.next(redclaw::runtime::InputQaStage::kDht);

        if (use_dht_transport && dht_client) {
            const std::uint64_t dht_loop_now_ms = now_steady_ms();
            dht_config.now_unix = now_unix_ms() / 1000ULL;
#if defined(REDCLAW_ENABLE_DHT_BACKENDS)
            if (libtorrent_dht_store != nullptr
                && (dht_next_alert_pump_ms == 0 || dht_loop_now_ms >= dht_next_alert_pump_ms)) {
                libtorrent_dht_store->pump_alerts(std::chrono::milliseconds(0));
                dht_next_alert_pump_ms = dht_loop_now_ms + kDhtAlertPumpIntervalMs;
            }
#endif

            bool should_publish_dht_signal = false;
            bool dht_publish_refresh = false;
            bool dht_publish_connect_request = false;
            bool dht_publish_initial_snapshot = false;
            bool dht_publish_description_only = false;
            bool dht_publish_priority_candidates = false;
            bool dht_publish_direct_candidate = false;
            bool dht_publish_full_candidates = false;
            bool dht_publish_answer_ack = false;
            std::size_t dht_direct_candidate_index = 0;
            std::size_t local_candidate_total_snapshot = 0;
            std::string local_description_snapshot;
            std::vector<std::string> local_candidate_snapshot;
            redclaw::service::DhtSignalSnapshot snapshot_shape;
            snapshot_shape.role = options.role == RuntimeRole::kHost ? "host" : "controller";
            snapshot_shape.description_type = options.role == RuntimeRole::kHost ? "offer" : "answer";
            snapshot_shape.publisher_instance_id = dht_publisher_instance_id;
            if (dht_next_publish_retry_ms == 0 || dht_loop_now_ms >= dht_next_publish_retry_ms) {
                std::lock_guard<std::mutex> lock(callback_mutex);
                snapshot_shape.generation = dht_negotiation.generation();
                snapshot_shape.connection_request_tag =
                    dht_negotiation.connection_request_tag();
                snapshot_shape.acknowledged_answer_tag =
                    dht_negotiation.outbound_answer_acknowledgement();
                snapshot_shape.answered_description_tag =
                    redclaw::service::select_dht_answered_description_tag(
                        snapshot_shape.description_type,
                        dht_negotiation.offer_description_tag());
                snapshot_shape.candidates_complete = local_candidate_gathering_complete;
                local_candidate_total_snapshot = local_candidate_lines.size();
                const bool controller_request_active = options.role == RuntimeRole::kController
                    && snapshot_shape.generation == 0
                    && !dht_connection_request_payload.empty()
                    && !snapshot_shape.connection_request_tag.empty();
                const bool request_refresh_due = dht_local_revision > 0
                    && (dht_next_publish_refresh_ms == 0
                        || dht_loop_now_ms >= dht_next_publish_refresh_ms);
                if (saw_failed_state) {
                    // A dead transport cannot consume its old Answer/Offer.
                    // Continue fetching repair requests, not publishing stale SDP.
                    should_publish_dht_signal = false;
                } else if (controller_request_active
                    && (signal_snapshot_dirty || request_refresh_due)) {
                    should_publish_dht_signal = true;
                    dht_publish_connect_request = true;
                    dht_publish_refresh = !signal_snapshot_dirty;
                    snapshot_shape.description_type = "request";
                    snapshot_shape.description_sdp = dht_connection_request_payload;
                    snapshot_shape.candidates_complete = false;
                    local_description_snapshot = dht_connection_request_payload;
                    local_candidate_total_snapshot = 0;
                    signal_snapshot_dirty = false;
                } else if (signal_snapshot_dirty
                    && !local_description_sdp.empty()
                    && snapshot_shape.generation > 0) {
                    should_publish_dht_signal = true;
                    local_description_snapshot = local_description_sdp;
                    snapshot_shape.description_sdp = local_description_snapshot;
                    const bool host_answer_ack_pending = options.role == RuntimeRole::kHost
                        && !snapshot_shape.acknowledged_answer_tag.empty()
                        && !dht_answer_ack_publish_done;
                    if (host_answer_ack_pending) {
                        // DHT lanes are latest-state registers. Preserve every
                        // candidate already visible to the peer when adding the
                        // answer acknowledgement, even if that makes this
                        // revision use the indirect/chunked record path.
                        dht_publish_answer_ack = true;
                        local_candidate_snapshot =
                            redclaw::service::merge_dht_candidate_snapshots(
                                dht_last_published_candidate_lines,
                                local_candidate_lines);
                        dht_publish_description_only = local_candidate_snapshot.empty();
                    } else if (!dht_initial_snapshot_publish_done) {
                        dht_publish_initial_snapshot = true;
                        std::string plan_error;
                        const auto initial_plan = redclaw::service::plan_dht_initial_snapshot(
                            snapshot_shape,
                            local_candidate_lines,
                            &plan_error);
                        if (initial_plan.has_value()) {
                            local_candidate_snapshot = initial_plan->candidate_lines;
                        } else {
                            dht_last_error = "failed to plan initial DHT snapshot: " + plan_error;
                        }
                        if (local_candidate_snapshot.empty()) {
                            dht_publish_description_only = true;
                        } else if (local_candidate_snapshot.size() == local_candidate_lines.size()) {
                            dht_publish_full_candidates = true;
                        } else {
                            dht_publish_direct_candidate = true;
                            dht_publish_priority_candidates = true;
                            dht_direct_candidate_index = local_candidate_snapshot.size() - 1;
                        }
                    } else if (!local_candidate_lines.empty()
                        && dht_direct_candidate_publish_count < local_candidate_lines.size()
                        && dht_direct_candidate_publish_count < kDhtDirectCandidatePublishLimit
                        && !dht_full_candidate_publish_done) {
                        auto prioritized_candidates =
                            redclaw::service::prioritize_dht_candidate_lines(local_candidate_lines);
                        if (dht_direct_candidate_publish_count < prioritized_candidates.size()) {
                            dht_direct_candidate_index = dht_direct_candidate_publish_count;
                            local_candidate_snapshot.push_back(prioritized_candidates[dht_direct_candidate_index]);
                            snapshot_shape.candidate_lines = local_candidate_snapshot;
                            if (redclaw::service::dht_signal_snapshot_fits_direct_record(snapshot_shape)) {
                                dht_publish_direct_candidate = true;
                                dht_publish_priority_candidates = dht_direct_candidate_publish_count == 0;
                            } else {
                                dht_publish_full_candidates = true;
                                local_candidate_snapshot = std::move(prioritized_candidates);
                            }
                        } else {
                            dht_publish_full_candidates = true;
                            local_candidate_snapshot = std::move(prioritized_candidates);
                        }
                    } else {
                        dht_publish_full_candidates = true;
                        local_candidate_snapshot =
                            redclaw::service::prioritize_dht_candidate_lines(local_candidate_lines);
                    }
                    signal_snapshot_dirty = false;
                } else if (dht_local_revision > 0
                    && !saw_connected_state
                    && !local_description_sdp.empty()
                    && snapshot_shape.generation > 0
                    && (dht_next_publish_refresh_ms == 0 || dht_loop_now_ms >= dht_next_publish_refresh_ms)) {
                    should_publish_dht_signal = true;
                    dht_publish_refresh = true;
                    local_description_snapshot = local_description_sdp;
                    local_candidate_snapshot =
                        redclaw::service::prioritize_dht_candidate_lines(local_candidate_lines);
                }
            }

            if (should_publish_dht_signal) {
                redclaw::service::DhtSignalSnapshot snapshot;
                snapshot.role = snapshot_shape.role;
                snapshot.description_type = snapshot_shape.description_type;
                snapshot.description_sdp = local_description_snapshot;
                snapshot.publisher_instance_id = snapshot_shape.publisher_instance_id;
                snapshot.generation = snapshot_shape.generation;
                snapshot.connection_request_tag = snapshot_shape.connection_request_tag;
                snapshot.acknowledged_answer_tag = snapshot_shape.acknowledged_answer_tag;
                if (snapshot.description_type == "answer") {
                    snapshot.answered_description_tag = snapshot_shape.answered_description_tag;
                }
                snapshot.candidate_lines = dht_publish_connect_request
                    ? local_candidate_snapshot
                    : redclaw::service::merge_dht_candidate_snapshots(
                        dht_last_published_candidate_lines,
                        local_candidate_snapshot);
                snapshot.candidates_complete = snapshot_shape.candidates_complete
                    && snapshot.candidate_lines.size() == local_candidate_total_snapshot;
                const auto publish_revision = dht_publication.prepare(snapshot, dht_publish_refresh,
                    dht_config.now_unix, now_steady_ms(), dht_config.ttl_seconds);
                if (!publish_revision) {
                    std::cerr << "Runtime DHT publication revision/expiry exhausted" << '\n';
                    ice_wrapper.close();
                    return 1;
                }
                snapshot.revision = *publish_revision;

                const auto encrypted_blob_bytes =
                    redclaw::service::estimate_dht_encrypted_blob_bytes(snapshot);

                ++dht_publish_attempts;
                std::string publish_error;
                const auto publication = dht_client->publish_signal_snapshot(dht_config, snapshot, &publish_error);
                (void)dht_publication.observe(snapshot.revision, publication);
                if (publication) {
                    ++dht_publish_success;
                    if (!dht_publish_connect_request) {
                        ++dht_generation_publish_success;
                    }
                    dht_local_revision = snapshot.revision;
                    if (!dht_publish_connect_request) {
                        dht_last_published_candidate_lines = snapshot.candidate_lines;
                    }
                    dht_reachable = true;
                    dht_next_publish_retry_ms = 0;
                    dht_next_publish_refresh_ms = now_steady_ms() + kDhtPublishRefreshMs;
                    dht_last_error.clear();
                    if (dht_publish_connect_request) {
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        if (dht_negotiation.phase()
                            == redclaw::service::ConnectionNegotiationPhase::kRequestReady) {
                            if (!dht_negotiation.mark_controller_request_published()) {
                                if (runtime_error.empty()) {
                                    runtime_error = "failed to mark Controller connect request published";
                                }
                                saw_failed_state = true;
                            }
                        }
                    }
                    if (options.role == RuntimeRole::kController
                        && snapshot.description_type == "answer") {
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        if (!saw_failed_state) {
                            if (!dht_negotiation.mark_local_answer_published()) {
                                if (runtime_error.empty()) {
                                    runtime_error = "failed to mark Controller answer published";
                                }
                                saw_failed_state = true;
                            }
                        }
                    }
                    if (dht_publish_initial_snapshot) {
                        dht_initial_snapshot_publish_done = true;
                    }
                    if (dht_publish_direct_candidate) {
                        dht_direct_candidate_publish_count = std::max(
                            dht_direct_candidate_publish_count,
                            dht_direct_candidate_index + 1);
                    }
                    if (dht_publish_full_candidates) {
                        dht_full_candidate_publish_done = true;
                    }
                    if (dht_publish_answer_ack) {
                        dht_answer_ack_publish_done = true;
                        signal_snapshot_dirty = true;
                    }
                    if ((dht_publish_initial_snapshot || dht_publish_direct_candidate)
                        && !dht_full_candidate_publish_done) {
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        if (!local_candidate_lines.empty()) {
                            signal_snapshot_dirty = true;
                        }
                    }
                    std::cout << "Runtime DHT signal "
                              << (dht_publish_refresh ? "refreshed" : "published")
                              << " role=" << role_name
                              << " revision=" << snapshot.revision
                              << " description_tag="
                              << redclaw::service::derive_dht_description_tag(snapshot.description_sdp)
                              << " answered_description_tag="
                              << (snapshot.answered_description_tag.empty()
                                      ? "none"
                                      : snapshot.answered_description_tag)
                              << " generation=" << snapshot.generation
                              << " connection_request_tag=" << snapshot.connection_request_tag
                              << " publisher_instance="
                              << dht_instance_summary(snapshot.publisher_instance_id)
                              << " acknowledged_answer_tag="
                              << (snapshot.acknowledged_answer_tag.empty()
                                      ? "none"
                                      : snapshot.acknowledged_answer_tag)
                              << " candidates_complete="
                              << (snapshot.candidates_complete ? "true" : "false")
                              << " candidates=" << snapshot.candidate_lines.size()
                              << " encrypted_blob_bytes="
                              << encrypted_blob_bytes.value_or(0)
                              << " direct_record="
                              << (encrypted_blob_bytes.has_value()
                                      && *encrypted_blob_bytes <= redclaw::service::kDirectDhtEncryptedBlobBudgetBytes
                                  ? "true"
                                  : "false")
                              << " phase="
                              << (dht_publish_connect_request
                                      ? "connect-request"
                                      : (dht_publish_answer_ack
                                      ? "answer-ack"
                                      : (dht_publish_description_only
                                      ? "description-only"
                                      : (dht_publish_direct_candidate
                                          ? (dht_publish_priority_candidates ? "priority-candidates" : "direct-candidate")
                                          : "full"))));
                    if (dht_publish_direct_candidate) {
                        std::cout << " direct_candidate_index=" << dht_direct_candidate_index
                                  << " direct_candidate_published=" << dht_direct_candidate_publish_count;
                    }
                    if (dht_publish_full_candidates) {
                        std::cout << " full_candidate_published=true";
                    }
                    if (dht_publish_initial_snapshot) {
                        std::cout << " initial_snapshot=true";
                    }
                    std::cout << '\n';
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kInfo,
                        "signal",
                        dht_publish_refresh
                            ? "local DHT signal snapshot refreshed"
                            : "local DHT signal snapshot published");
                } else {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    if (!dht_publish_refresh) {
                        signal_snapshot_dirty = true;
                    }
                    dht_next_publish_retry_ms = now_steady_ms()
                        + (publication.state == redclaw::service::DhtPublishState::kPending
                            ? kRuntimeLoopIntervalMs : options.dht_publish_retry_ms);
                    dht_last_error = publish_error;
                }
            }

            if (remote_description_applied
                && !pending_remote_dht_candidate_lines.empty()
                && dht_remote_candidate_application_allowed()) {
                std::size_t released_count = 0;
                for (auto it = pending_remote_dht_candidate_lines.begin();
                     it != pending_remote_dht_candidate_lines.end();) {
                    std::string candidate_error;
                    const auto outcome = apply_remote_dht_candidate_line(
                        ice_wrapper,
                        *it,
                        &candidate_diagnostics,
                        &callback_mutex,
                        &candidate_error);
                    if (outcome == RemoteDhtCandidateApplyOutcome::kFailed) {
                        {
                            std::lock_guard<std::mutex> lock(callback_mutex);
                            saw_failed_state = true;
                            (void)dht_negotiation.mark_failed();
                        }
                        std::cout << "Runtime deferred remote DHT candidate after ICE transport failure role="
                                  << role_name
                                  << " stage=pending-candidate-release"
                                  << " action=recover_terminated_transport"
                                  << '\n';
                        break;
                    }
                    if (outcome == RemoteDhtCandidateApplyOutcome::kDeferred) {
                        ++it;
                        continue;
                    }
                    if (outcome == RemoteDhtCandidateApplyOutcome::kInvalid) {
                        std::cerr << "Runtime skipped invalid pending DHT candidate snapshot entry" << '\n';
                    } else {
                        applied_remote_dht_candidate_lines.insert(*it);
                        ++released_count;
                    }
                    it = pending_remote_dht_candidate_lines.erase(it);
                }
                if (released_count > 0) {
                    std::cout << "Runtime released pending remote DHT candidates role="
                              << role_name
                              << " released=" << released_count
                              << " pending=" << pending_remote_dht_candidate_lines.size()
                              << " generation=" << dht_negotiation.generation()
                              << '\n';
                }
            }

            if (dht_next_fetch_ms == 0 || dht_loop_now_ms >= dht_next_fetch_ms) {
                ++dht_fetch_attempts;
                const std::string remote_lane = options.role == RuntimeRole::kHost ? "controller" : "host";
                std::string fetch_error;
                const auto remote_signal = dht_client->fetch_signal_snapshot(
                    dht_config,
                    remote_lane,
                    dht_remote_revision,
                    &fetch_error);
                const std::string remote_description_tag = remote_signal.has_value()
                    ? redclaw::service::derive_dht_description_tag(remote_signal->description_sdp)
                    : std::string{};
                // Generation counters restart with the Host process. Reject the
                // exact failed SDP/ICE offer, but allow a restarted Host's fresh
                // offer even when its generation number starts over at one.
                const bool failed_host_generation = remote_signal.has_value()
                    && options.role == RuntimeRole::kController
                    && remote_signal->description_type == "offer"
                    && redclaw::service::should_skip_failed_dht_host_offer(
                        remote_signal->generation,
                        dht_failed_remote_generation,
                        remote_description_tag,
                        dht_failed_remote_offer_tag);
                const bool completed_host_offer = remote_signal.has_value()
                    && options.role == RuntimeRole::kController
                    && remote_signal->description_type == "offer"
                    && redclaw::service::should_skip_completed_dht_host_offer_on_fresh_controller(
                        dht_negotiation.generation(),
                        remote_signal->acknowledged_answer_tag);
                const bool expired_failed_record = remote_signal.has_value()
                    && dht_failed_remote_expiry > 0
                    && remote_signal->expires_at_unix <= dht_failed_remote_expiry
                    && remote_signal->revision <= dht_failed_remote_revision;
                const bool remote_record_already_failed = remote_signal.has_value()
                    && !remote_description_applied
                    && (failed_host_generation || completed_host_offer || expired_failed_record);
                bool remote_record_rejected = false;
                bool remote_generation_requires_rebuild = false;
                bool remote_connect_request_accepted = false;
                bool remote_connect_request_new = false;
                bool remote_connect_request_superseded = false;
                bool controller_adopted_offer = false;
                redclaw::service::PersistentHostOfferAdoption persistent_offer_adoption =
                    redclaw::service::PersistentHostOfferAdoption::kRejected;
                redclaw::service::ConnectionNegotiationResult negotiation_result;
                if (remote_signal.has_value() && !remote_record_already_failed) {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    if (options.role == RuntimeRole::kHost
                        && remote_signal->description_type == "request") {
                        if (remote_signal->connection_request_tag != remote_description_tag) {
                            negotiation_result.observation =
                                redclaw::service::ConnectionNegotiationObservation::kRejectedInvalid;
                        } else if (
                            redclaw::service::should_defer_dht_controller_request_for_persistent_offer(
                                dht_host_persistent_offer_active,
                                saw_failed_state,
                                remote_description_applied,
                                remote_description_ever_applied,
                                dht_publication.standby_viable(dht_config.now_unix, now_steady_ms()))) {
                            negotiation_result.observation =
                                redclaw::service::ConnectionNegotiationObservation::kDuplicate;
                        } else {
                            negotiation_result = dht_negotiation.observe_controller_request(
                                remote_signal->connection_request_tag);
                        }
                        remote_connect_request_accepted = negotiation_result.accepted();
                        remote_connect_request_new = negotiation_result.state_changed;
                        remote_connect_request_superseded = negotiation_result.generation_changed;
                    } else if (options.role == RuntimeRole::kController
                        && remote_signal->description_type == "offer") {
                        persistent_offer_adoption =
                            redclaw::service::decide_persistent_host_offer_adoption({
                                .remote_description_ever_applied =
                                    remote_description_ever_applied,
                                .last_applied_host_instance_id =
                                    dht_last_applied_host_instance_id,
                                .offered_host_instance_id =
                                    remote_signal->publisher_instance_id,
                                .exact_failed_offer = failed_host_generation,
                                .completed_offer = completed_host_offer,
                            });
                        const bool request_tag_mismatch =
                            remote_signal->connection_request_tag
                            != dht_negotiation.connection_request_tag();
                        negotiation_result = dht_negotiation.observe_host_offer(
                            remote_signal->generation,
                            remote_signal->connection_request_tag,
                            remote_description_tag,
                            persistent_offer_adoption);
                        controller_adopted_offer = negotiation_result.accepted()
                            && negotiation_result.generation_changed;
                        if (controller_adopted_offer && request_tag_mismatch) {
                            if (persistent_offer_adoption
                                == redclaw::service::PersistentHostOfferAdoption::kFreshController) {
                                ++dht_persistent_offer_adopted_initial_total;
                            } else if (persistent_offer_adoption
                                       == redclaw::service::PersistentHostOfferAdoption::kRestartedHost) {
                                ++dht_persistent_offer_adopted_host_restart_total;
                            }
                        } else if (request_tag_mismatch
                                   && negotiation_result.observation
                                       == redclaw::service::ConnectionNegotiationObservation::kRejectedOfferMismatch
                                   && remote_description_ever_applied) {
                            if (!redclaw::service::is_valid_dht_publisher_instance_id(
                                    dht_last_applied_host_instance_id)
                                || !redclaw::service::is_valid_dht_publisher_instance_id(
                                    remote_signal->publisher_instance_id)) {
                                ++dht_peer_instance_missing_total;
                            } else if (dht_last_applied_host_instance_id
                                       == remote_signal->publisher_instance_id) {
                                ++dht_persistent_offer_same_instance_rejected_total;
                            }
                        } else if (negotiation_result.accepted()
                                   && !negotiation_result.generation_changed
                                   && remote_description_applied) {
                            ++dht_duplicate_offer_application_suppressed_total;
                        }
                        remote_generation_requires_rebuild = negotiation_result.accepted()
                            && negotiation_result.generation_changed
                            && remote_description_applied;
                    } else if (options.role == RuntimeRole::kHost
                        && remote_signal->description_type == "answer") {
                        negotiation_result = dht_negotiation.observe_controller_answer(
                            remote_signal->generation,
                            remote_signal->connection_request_tag,
                            remote_signal->answered_description_tag,
                            remote_description_tag);
                    } else {
                        negotiation_result.observation =
                            redclaw::service::ConnectionNegotiationObservation::kRejectedInvalid;
                    }
                    remote_record_rejected = !negotiation_result.accepted();
                    if (remote_generation_requires_rebuild) {
                        dht_peer_republished_description = true;
                    }
                }

                if (remote_connect_request_accepted) {
                    ++dht_fetch_hits;
                    dht_reachable = true;
                    dht_remote_revision = remote_signal->revision;
                    dht_remote_record_expiry = std::max(
                        dht_remote_record_expiry,
                        remote_signal->expires_at_unix);
                    dht_stale_remote_reported = false;
                    if (remote_connect_request_new) {
                        if (remote_connect_request_superseded) {
                            dht_controller_request_superseded = true;
                            std::cout << "Runtime newer Controller DHT connect request accepted role="
                                      << role_name
                                      << " connection_request_tag="
                                      << remote_signal->connection_request_tag
                                      << " remote_revision=" << remote_signal->revision
                                      << " action=rebuild_host_generation"
                                      << '\n';
                            append_timeline(
                                redclaw::render::RuntimeStatusSeverity::kInfo,
                                "signal",
                                "newer Controller request superseded the unconnected ICE generation");
                        } else {
                            std::string start_error;
                            if (!ensure_ice_gathering_started(
                                    "Host ICE gathering started after Controller connect request",
                                    &start_error)) {
                                std::cerr << "Runtime failed to start Host ICE after Controller request: "
                                          << start_error << '\n';
                                ice_wrapper.close();
                                return 1;
                            }
                            std::cout << "Runtime Controller DHT connect request accepted role="
                                      << role_name
                                      << " connection_request_tag="
                                      << remote_signal->connection_request_tag
                                      << " remote_revision=" << remote_signal->revision
                                      << '\n';
                            append_timeline(
                                redclaw::render::RuntimeStatusSeverity::kInfo,
                                "signal",
                                "Controller connect request accepted; starting fresh Host ICE generation");
                        }
                    }
                } else if (remote_record_rejected) {
                    ++dht_stale_remote_skips;
                    dht_reachable = true;
                    dht_remote_revision = std::max(dht_remote_revision, remote_signal->revision);
                    if (!dht_stale_remote_reported) {
                        dht_stale_remote_reported = true;
                        std::cout << "Runtime rejected remote DHT negotiation record role=" << role_name
                                  << " remote_lane=" << remote_lane
                                  << " remote_revision=" << remote_signal->revision
                                  << " generation=" << remote_signal->generation
                                  << " observation="
                                  << redclaw::service::connection_negotiation_observation_to_string(
                                         negotiation_result.observation)
                                  << " publisher_instance="
                                  << dht_instance_summary(remote_signal->publisher_instance_id)
                                  << " adoption_reason="
                                  << redclaw::service::persistent_host_offer_adoption_to_string(
                                         persistent_offer_adoption)
                                  << " sealed_age_seconds="
                                  << dht_sealed_age_seconds(remote_signal->expires_at_unix) << '\n';
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kWarning,
                            "signal",
                            "rejected stale or mismatched DHT negotiation record");
                    }
                } else if (remote_record_already_failed) {
                    ++dht_stale_remote_skips;
                    dht_reachable = true;
                    dht_remote_revision = std::max(dht_remote_revision, remote_signal->revision);
                    if (!dht_stale_remote_reported) {
                        dht_stale_remote_reported = true;
                        std::cout << "Runtime skipped remote DHT record that already failed role=" << role_name
                                  << " remote_lane=" << remote_lane
                                  << " remote_revision=" << remote_signal->revision
                                  << " generation=" << remote_signal->generation
                                  << " failed_generation=" << dht_failed_remote_generation
                                  << " completed_offer="
                                  << (completed_host_offer ? "true" : "false")
                                  << " sealed_age_seconds="
                                  << dht_sealed_age_seconds(remote_signal->expires_at_unix) << '\n';
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kWarning,
                            "signal",
                            "waiting for the peer to publish a new rendezvous record");
                    }
                } else if (remote_generation_requires_rebuild) {
                    ++dht_fetch_hits;
                    dht_reachable = true;
                    dht_remote_revision = remote_signal->revision;
                    dht_remote_record_expiry = std::max(
                        dht_remote_record_expiry,
                        remote_signal->expires_at_unix);
                    dht_stale_remote_reported = false;
                    std::cout << "Runtime observed newer Host DHT generation role=" << role_name
                              << " generation=" << remote_signal->generation
                              << " remote_revision=" << remote_signal->revision
                              << " action=rebuild_controller_session" << '\n';
                } else if (remote_signal.has_value()) {
                    ++dht_fetch_hits;
                    dht_reachable = true;
                    dht_remote_candidates_complete = remote_signal->candidates_complete;
                    if (controller_adopted_offer) {
                        if (persistent_offer_adoption
                            == redclaw::service::PersistentHostOfferAdoption::kRestartedHost) {
                            applied_remote_dht_candidate_lines.clear();
                            pending_remote_dht_candidate_lines.clear();
                            dht_last_remote_candidate_count = 0;
                            dht_remote_candidates_complete = remote_signal->candidates_complete;
                        }
                        if (remote_signal->generation > dht_failed_remote_generation) {
                            dht_failed_remote_generation = 0;
                        }
                        dht_failed_remote_offer_tag.clear();
                        dht_initial_snapshot_publish_done = false;
                        dht_full_candidate_publish_done = false;
                        dht_last_published_candidate_lines.clear();
                        dht_direct_candidate_publish_count = 0;
                        dht_generation_publish_success = 0;
                        dht_publication.reset();
                        dht_next_publish_retry_ms = 0;
                        std::cout << "Runtime Controller adopted persistent Host offer role="
                                  << role_name
                                  << " adoption_reason="
                                  << redclaw::service::persistent_host_offer_adoption_to_string(
                                         persistent_offer_adoption)
                                  << " publisher_instance="
                                  << dht_instance_summary(remote_signal->publisher_instance_id)
                                  << " generation=" << remote_signal->generation
                                  << " remote_revision=" << remote_signal->revision
                                  << '\n';
                    }
                    if (!remote_description_applied) {
                        std::cout << "Runtime applying remote DHT record role=" << role_name
                                  << " remote_lane=" << remote_lane
                                  << " remote_revision=" << remote_signal->revision
                                  << " generation=" << remote_signal->generation
                                  << " sealed_age_seconds="
                                  << dht_sealed_age_seconds(remote_signal->expires_at_unix) << '\n';
                    }
                    dht_remote_record_expiry = std::max(dht_remote_record_expiry, remote_signal->expires_at_unix);
                    bool remote_description_applied_now = false;
                    bool defer_local_answer_until_candidates = false;
                    if (!remote_description_applied && !remote_signal->description_sdp.empty()) {
                        const bool remote_is_offer = remote_signal->description_type == "offer";
                        defer_local_answer_until_candidates =
                            options.role == RuntimeRole::kController && remote_is_offer;
                        std::string apply_error;
                        if (!ensure_ice_gathering_started("controller ICE gathering started after remote DHT description", &apply_error)
                            || !ice_wrapper.applyRemoteDescription(
                                remote_signal->description_sdp,
                                remote_is_offer,
                                &apply_error,
                                !defer_local_answer_until_candidates)) {
                            std::cerr << "Runtime failed to apply remote DHT description: " << apply_error << '\n';
                            ice_wrapper.close();
                            return 1;
                        }

                        remote_description_applied = true;
                        remote_description_ever_applied = true;
                        remote_description_applied_now = true;
                        applied_remote_description_sdp = remote_signal->description_sdp;
                        if (options.role == RuntimeRole::kController && remote_is_offer) {
                            dht_last_applied_host_instance_id =
                                remote_signal->publisher_instance_id;
                        }
                        bool negotiation_state_updated = false;
                        {
                            std::lock_guard<std::mutex> lock(callback_mutex);
                            negotiation_state_updated = options.role == RuntimeRole::kController
                                ? dht_negotiation.mark_remote_offer_applied()
                                : true;
                        }
                        if (!negotiation_state_updated) {
                            std::cerr << "Runtime failed to advance DHT negotiation after applying remote description role="
                                      << role_name << '\n';
                            ice_wrapper.close();
                            return 1;
                        }
                        std::cout << "Runtime remote description applied role=" << role_name
                                  << " (dht)"
                                  << " remote_revision=" << remote_signal->revision
                                  << " generation=" << remote_signal->generation
                                  << " remote_description_tag="
                                  << redclaw::service::derive_dht_description_tag(remote_signal->description_sdp)
                                  << " answered_description_tag="
                                  << (remote_signal->answered_description_tag.empty()
                                          ? "none"
                                          : remote_signal->answered_description_tag)
                                  << '\n';
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kInfo,
                            "signal",
                            std::string("remote ") + remote_signal->description_type + " applied from DHT");
                    }

                    if (options.role == RuntimeRole::kController
                        && !remote_signal->acknowledged_answer_tag.empty()) {
                        redclaw::service::ConnectionNegotiationResult ack_result;
                        {
                            std::lock_guard<std::mutex> lock(callback_mutex);
                            ack_result = dht_negotiation.observe_answer_applied(
                                remote_signal->generation,
                                remote_signal->connection_request_tag,
                                redclaw::service::derive_dht_description_tag(
                                    remote_signal->description_sdp),
                                remote_signal->acknowledged_answer_tag);
                            if (ack_result.accepted() && saw_connected_state) {
                                (void)dht_negotiation.mark_connected();
                            }
                        }
                        if (!ack_result.accepted()) {
                            std::cout << "Runtime rejected Host DHT answer acknowledgement role="
                                      << role_name
                                      << " generation=" << remote_signal->generation
                                      << " observation="
                                      << redclaw::service::connection_negotiation_observation_to_string(
                                             ack_result.observation)
                                      << '\n';
                        } else {
                            if (ack_result.state_changed) {
                                std::cout << "Runtime Host DHT answer acknowledgement accepted role="
                                          << role_name
                                          << " generation=" << remote_signal->generation
                                          << " answer_tag=" << remote_signal->acknowledged_answer_tag
                                          << '\n';
                                append_timeline(
                                    redclaw::render::RuntimeStatusSeverity::kInfo,
                                    "signal",
                                    "Host acknowledged the Controller answer");
                            }
                        }
                    }

                    if (remote_description_applied) {
                        bool may_apply_remote_candidates =
                            dht_remote_candidate_application_allowed();
                        for (const auto& candidate_line : remote_signal->candidate_lines) {
                            if (applied_remote_dht_candidate_lines.find(candidate_line)
                                != applied_remote_dht_candidate_lines.end()) {
                                continue;
                            }

                            if (!may_apply_remote_candidates
                                || !dht_remote_candidate_application_allowed()) {
                                break;
                            }

                            std::string candidate_error;
                            const auto outcome = apply_remote_dht_candidate_line(
                                    ice_wrapper,
                                    candidate_line,
                                    &candidate_diagnostics,
                                    &callback_mutex,
                                    &candidate_error);
                            if (outcome == RemoteDhtCandidateApplyOutcome::kApplied) {
                                applied_remote_dht_candidate_lines.insert(candidate_line);
                            } else if (outcome == RemoteDhtCandidateApplyOutcome::kDeferred) {
                                pending_remote_dht_candidate_lines.insert(candidate_line);
                            } else if (outcome == RemoteDhtCandidateApplyOutcome::kInvalid) {
                                std::cerr << "Runtime skipped invalid DHT candidate snapshot entry" << '\n';
                            } else if (outcome == RemoteDhtCandidateApplyOutcome::kFailed) {
                                {
                                    std::lock_guard<std::mutex> lock(callback_mutex);
                                    saw_failed_state = true;
                                    (void)dht_negotiation.mark_failed();
                                }
                                may_apply_remote_candidates = false;
                                std::cout << "Runtime deferred remote DHT candidate after ICE transport failure role="
                                          << role_name
                                          << " stage=cumulative-snapshot"
                                          << " action=recover_terminated_transport"
                                          << '\n';
                            }
                        }
                    }

                    bool host_acknowledged_answer_now = false;
                    if (options.role == RuntimeRole::kHost
                        && remote_description_applied
                        && (!applied_remote_dht_candidate_lines.empty()
                            || dht_remote_candidates_complete)) {
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        if (!saw_failed_state && !dht_negotiation.answer_acknowledged()) {
                            host_acknowledged_answer_now = dht_negotiation.mark_remote_answer_applied(
                                dht_negotiation.answer_description_tag());
                            if (host_acknowledged_answer_now) {
                                signal_snapshot_dirty = true;
                                dht_publication.reset();
                                dht_next_publish_retry_ms = 0;
                            }
                        }
                    }
                    if (host_acknowledged_answer_now) {
                        std::cout << "Runtime Host applied Controller DHT answer and candidates role="
                                  << role_name
                                  << " generation=" << remote_signal->generation
                                  << " answer_tag="
                                  << redclaw::service::derive_dht_description_tag(
                                         remote_signal->description_sdp)
                                  << " controller_candidates_complete="
                                  << (dht_remote_candidates_complete ? "true" : "false")
                                  << '\n';
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kInfo,
                            "signal",
                            "Controller answer and candidates applied; publishing acknowledgement");
                    }

                    if (remote_description_applied_now && defer_local_answer_until_candidates) {
                        std::string apply_error;
                        if (!ice_wrapper.emitLocalDescription(&apply_error)) {
                            std::cerr << "Runtime failed to emit local DHT answer: " << apply_error << '\n';
                            ice_wrapper.close();
                            return 1;
                        }
                    }

                    dht_last_remote_candidate_count = remote_signal->candidate_lines.size();
                    dht_remote_revision = remote_signal->revision;
                    dht_last_error.clear();
                } else if (!fetch_error.empty()) {
                    dht_last_error = fetch_error;
                }
                dht_next_fetch_ms = now_steady_ms()
                    + (remote_record_already_failed || remote_record_rejected
                        ? std::max<std::uint32_t>(options.dht_poll_interval_ms, kDhtStaleRemoteFetchBackoffMs)
                        : options.dht_poll_interval_ms);
            }
        }

        input_loop_timing.next(redclaw::runtime::InputQaStage::kNegotiation);

        if (!remote_description_applied) {
            if (!remote_description_ever_applied
                && options.signal_timeout_seconds > 0
                && elapsed_seconds >= options.signal_timeout_seconds) {
                std::string timeout_detail;
                if (use_sealed_file_transport) {
                    if (!remote_sealed_blob_seen) {
                        timeout_detail = " no_remote_sealed_blob_seen=true path=" + inbound_description_file.string();
                    } else if (last_remote_sealed_blob_update_ms > 0) {
                        const std::uint64_t age_ms = now_steady_ms() - last_remote_sealed_blob_update_ms;
                        timeout_detail = " remote_sealed_blob_stale_seconds=" + std::to_string(age_ms / 1000ULL);
                    } else {
                        timeout_detail = " remote_sealed_blob_seen_but_not_updated=true";
                    }
                } else if (use_rendezvous_transport) {
                    timeout_detail = " session_code=" + options.session_code
                        + " session_id_known=" + (rendezvous_session_id.empty() ? "false" : "true")
                        + " publish_success=" + std::to_string(rendezvous_diagnostics.publish_success)
                        + " fetch_hits=" + std::to_string(rendezvous_diagnostics.fetch_hits);
                } else if (use_dht_transport) {
                    timeout_detail = " session_code=" + options.session_code
                        + " dht_reachable=" + (dht_reachable ? "true" : "false")
                        + " publish_success=" + std::to_string(dht_publish_success)
                        + " fetch_hits=" + std::to_string(dht_fetch_hits)
                        + " repair_attempts=" + std::to_string(dht_repair_attempts)
                        + " stale_remote_skips=" + std::to_string(dht_stale_remote_skips)
                        + " last_error=" + dht_last_error;
                }

                std::cerr << "Runtime signaling timeout after seconds=" << options.signal_timeout_seconds
                          << " role=" << role_name << timeout_detail << '\n';
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    append_timeline(
                        redclaw::render::RuntimeStatusSeverity::kError,
                        "signal",
                        use_sealed_file_transport
                            ? "signaling timeout reached before remote description (sealed-file path)"
                            : "signaling timeout reached before remote description");
                }
                ice_wrapper.close();
                return 1;
            }

            if (use_event_log_transport) {
                std::ifstream in(remote_event_log, std::ios::binary);
                if (in.is_open()) {
                    std::string line;
                    std::size_t line_index = 0;
                    while (std::getline(in, line)) {
                        ++line_index;
                        if (line_index <= applied_remote_event_lines || line.empty()) {
                            continue;
                        }

                        std::vector<std::string> fields;
                        if (!parse_event_log_line(line, &fields) || fields.empty()) {
                            std::cerr << "Runtime skipped malformed event-log line index=" << line_index << '\n';
                            continue;
                        }

                        if (fields[0] == "description") {
                            if (fields.size() < 3 || remote_description_applied) {
                                continue;
                            }

                            const bool event_is_offer = fields[1] == "offer";
                            std::string apply_error;
                            if (!ensure_ice_gathering_started("controller ICE gathering started after remote event-log description", &apply_error)
                                || !ice_wrapper.applyRemoteDescription(fields[2], event_is_offer, &apply_error)) {
                                std::cerr << "Runtime failed to apply remote event-log description: " << apply_error << '\n';
                                ice_wrapper.close();
                                return 1;
                            }

                            remote_description_applied = true;
                            remote_description_ever_applied = true;
                            std::cout << "Runtime remote description applied role=" << role_name << '\n';
                            {
                                std::lock_guard<std::mutex> lock(callback_mutex);
                                append_timeline(
                                    redclaw::render::RuntimeStatusSeverity::kInfo,
                                    "signal",
                                    std::string("remote ") + (event_is_offer ? "offer" : "answer") + " applied");
                            }
                            continue;
                        }

                        if (fields[0] == "candidate") {
                            if (fields.size() < 3) {
                                continue;
                            }

                            const std::string& remote_mid = fields[1];
                            const std::string& remote_candidate = fields[2];

                            if (!remote_description_applied) {
                                pending_remote_candidates.emplace_back(remote_mid, remote_candidate);
                                continue;
                            }

                            std::string apply_error;
                            if (!ice_wrapper.applyRemoteCandidate(remote_candidate, remote_mid, &apply_error)) {
                                std::cerr << "Runtime failed to apply remote event-log candidate: " << apply_error << '\n';
                                ice_wrapper.close();
                                return 1;
                            }

                            std::lock_guard<std::mutex> lock(callback_mutex);
                            record_candidate(&candidate_diagnostics, false, classify_candidate_type(remote_candidate), remote_candidate);
                        }
                    }

                    applied_remote_event_lines = line_index;
                }
            } else if (use_sealed_file_transport) {
                std::error_code exists_ec;
                if (std::filesystem::exists(inbound_description_file, exists_ec) && !exists_ec) {
                    remote_sealed_blob_seen = true;
                    std::string inbound_blob;
                    if (!read_text_file(inbound_description_file, &inbound_blob, &runtime_error)) {
                        std::cerr << "Runtime signaling read failed: " << runtime_error << '\n';
                        ice_wrapper.close();
                        return 1;
                    }

                    if (inbound_blob != last_remote_sealed_blob) {
                        std::string inbound_sdp;
                        std::vector<std::pair<std::string, std::string>> remote_candidates_from_blob;
                        std::string parse_error;
                        if (!parse_sealed_signal_blob(
                                inbound_blob,
                                options,
                                &inbound_sdp,
                                &remote_candidates_from_blob,
                                &parse_error)) {
                            const std::string error_class = classify_sealed_blob_error(parse_error);
                            std::cerr << "Runtime failed to parse remote sealed signaling blob class="
                                      << error_class << " detail=" << parse_error << '\n';
                            {
                                std::lock_guard<std::mutex> lock(callback_mutex);
                                append_timeline(
                                    redclaw::render::RuntimeStatusSeverity::kError,
                                    "signal",
                                    "remote sealed blob rejected class=" + error_class);
                            }
                            ice_wrapper.close();
                            return 1;
                        }

                        if (!remote_description_applied) {
                            std::string apply_error;
                            if (!ensure_ice_gathering_started("controller ICE gathering started after remote sealed description", &apply_error)
                                || !ice_wrapper.applyRemoteDescription(inbound_sdp, inbound_is_offer, &apply_error)) {
                                std::cerr << "Runtime failed to apply remote sealed description: " << apply_error << '\n';
                                ice_wrapper.close();
                                return 1;
                            }

                            remote_description_applied = true;
                            remote_description_ever_applied = true;
                            std::cout << "Runtime remote description applied role=" << role_name << " (sealed-file)\n";
                            {
                                std::lock_guard<std::mutex> lock(callback_mutex);
                                append_timeline(
                                    redclaw::render::RuntimeStatusSeverity::kInfo,
                                    "signal",
                                    std::string("remote ") + (inbound_is_offer ? "offer" : "answer") + " applied from sealed blob");
                            }
                        }

                        for (std::size_t i = applied_remote_sealed_candidates;
                             i < remote_candidates_from_blob.size();
                             ++i) {
                            const auto& candidate_entry = remote_candidates_from_blob[i];
                            std::string apply_error;
                            if (!ice_wrapper.applyRemoteCandidate(candidate_entry.second, candidate_entry.first, &apply_error)) {
                                std::cerr << "Runtime failed to apply remote sealed candidate: " << apply_error << '\n';
                                ice_wrapper.close();
                                return 1;
                            }

                            std::lock_guard<std::mutex> lock(callback_mutex);
                            record_candidate(
                                &candidate_diagnostics,
                                false,
                                classify_candidate_type(candidate_entry.second),
                                candidate_entry.second);
                        }

                        applied_remote_sealed_candidates = remote_candidates_from_blob.size();
                        last_remote_sealed_blob_update_ms = now_steady_ms();
                        last_remote_sealed_blob = std::move(inbound_blob);
                    }
                }
            } else if (!use_tcp_transport) {
                std::error_code exists_ec;
                if (std::filesystem::exists(inbound_description_file, exists_ec) && !exists_ec) {
                    std::string inbound_sdp;
                    if (!read_text_file(inbound_description_file, &inbound_sdp, &runtime_error)) {
                        std::cerr << "Runtime signaling read failed: " << runtime_error << '\n';
                        ice_wrapper.close();
                        return 1;
                    }

                    std::string apply_error;
                    if (!ensure_ice_gathering_started("controller ICE gathering started after remote file description", &apply_error)
                        || !ice_wrapper.applyRemoteDescription(inbound_sdp, inbound_is_offer, &apply_error)) {
                        std::cerr << "Runtime failed to apply remote description: " << apply_error << '\n';
                        ice_wrapper.close();
                        return 1;
                    }

                    remote_description_applied = true;
                    remote_description_ever_applied = true;
                    std::cout << "Runtime remote description applied role=" << role_name << '\n';
                    {
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        append_timeline(
                            redclaw::render::RuntimeStatusSeverity::kInfo,
                            "signal",
                            std::string("remote ") + (inbound_is_offer ? "offer" : "answer") + " applied");
                    }
                }
            }
        }

        if (remote_description_applied) {
            if (!pending_remote_candidates.empty()) {
                for (const auto& candidate : pending_remote_candidates) {
                    std::string apply_error;
                    if (!ice_wrapper.applyRemoteCandidate(candidate.second, candidate.first, &apply_error)) {
                        std::cerr << "Runtime failed to apply pending candidate: " << apply_error << '\n';
                        ice_wrapper.close();
                        return 1;
                    }

                    std::lock_guard<std::mutex> lock(callback_mutex);
                    record_candidate(&candidate_diagnostics, false, classify_candidate_type(candidate.second), candidate.second);
                }
                pending_remote_candidates.clear();
            }

            if (!use_event_log_transport
                && !use_tcp_transport
                && !use_sealed_file_transport
                && !use_rendezvous_transport
                && !use_dht_transport) {
                std::ifstream in(remote_candidates_file, std::ios::binary);
                if (in.is_open()) {
                    std::string line;
                    std::size_t line_index = 0;
                    while (std::getline(in, line)) {
                        ++line_index;
                        if (line_index <= applied_remote_candidate_lines || line.empty()) {
                            continue;
                        }

                        const auto parsed = parse_candidate_line(line);
                        if (!parsed.has_value()) {
                            std::cerr << "Runtime skipped malformed candidate line index=" << line_index << '\n';
                            continue;
                        }

                        const std::string& remote_mid = parsed->first;
                        const std::string& remote_candidate = parsed->second;
                        std::string apply_error;
                        if (!ice_wrapper.applyRemoteCandidate(remote_candidate, remote_mid, &apply_error)) {
                            std::cerr << "Runtime failed to apply remote candidate line=" << line_index
                                      << " error=" << apply_error << '\n';
                            ice_wrapper.close();
                            return 1;
                        }

                        {
                            std::lock_guard<std::mutex> lock(callback_mutex);
                            record_candidate(&candidate_diagnostics, false, classify_candidate_type(remote_candidate), remote_candidate);
                        }
                    }

                    applied_remote_candidate_lines = line_index;
                }
            }
        }

        input_loop_timing.next(redclaw::runtime::InputQaStage::kPeriodicStats);

        if (elapsed_seconds > 0 && elapsed_seconds % 10 == 0 && heartbeat_due) {
            redclaw::net::IceConnectionState state_snapshot;
            std::uint64_t negotiation_generation_snapshot = 0;
            redclaw::service::ConnectionNegotiationPhase negotiation_phase_snapshot =
                redclaw::service::ConnectionNegotiationPhase::kIdle;
            bool answer_acknowledged_snapshot = false;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                state_snapshot = connection_state;
                negotiation_generation_snapshot = dht_negotiation.generation();
                negotiation_phase_snapshot = dht_negotiation.phase();
                answer_acknowledged_snapshot = dht_negotiation.answer_acknowledged();
            }
            std::cout << "Runtime heartbeat role=" << role_name << " elapsed_seconds=" << elapsed_seconds << '\n';
            std::cout << "Runtime state role=" << role_name
                      << " state=" << static_cast<int>(state_snapshot)
                      << " remote_description_applied=" << (remote_description_applied ? "true" : "false")
                      << " connected=" << (saw_connected_state ? "true" : "false")
                      << '\n';
            CandidateDiagnostics diagnostics_snapshot;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                diagnostics_snapshot = candidate_diagnostics;
            }
            std::cout << "Runtime candidate stats role=" << role_name
                      << " local_total=" << diagnostics_snapshot.local_total
                      << " local_host=" << diagnostics_snapshot.local_host
                      << " local_srflx=" << diagnostics_snapshot.local_srflx
                      << " local_relay=" << diagnostics_snapshot.local_relay
                      << " local_prflx=" << diagnostics_snapshot.local_prflx
                      << " local_unknown=" << diagnostics_snapshot.local_unknown
                      << " local_ipv6=" << diagnostics_snapshot.local_ipv6
                      << " remote_total=" << diagnostics_snapshot.remote_total
                      << " remote_host=" << diagnostics_snapshot.remote_host
                      << " remote_srflx=" << diagnostics_snapshot.remote_srflx
                      << " remote_relay=" << diagnostics_snapshot.remote_relay
                      << " remote_prflx=" << diagnostics_snapshot.remote_prflx
                      << " remote_unknown=" << diagnostics_snapshot.remote_unknown
                      << " remote_ipv6=" << diagnostics_snapshot.remote_ipv6
                      << " remote_dht_exchanged=" << diagnostics_snapshot.remote_dht_exchanged;
            if (use_dht_transport) {
                std::cout << " local_dht_direct_published=" << dht_direct_candidate_publish_count
                          << " local_dht_full_published=" << (dht_full_candidate_publish_done ? "true" : "false")
                          << " remote_dht_latest_candidates=" << dht_last_remote_candidate_count;
            }
            std::cout << '\n';

#ifdef _WIN32
            if (use_rendezvous_transport) {
                std::cout << "Runtime rendezvous stats role=" << role_name
                          << " register_attempts=" << rendezvous_diagnostics.register_attempts
                          << " register_success=" << rendezvous_diagnostics.register_success
                          << " lookup_attempts=" << rendezvous_diagnostics.lookup_attempts
                          << " lookup_hits=" << rendezvous_diagnostics.lookup_hits
                          << " claim_attempts=" << rendezvous_diagnostics.claim_attempts
                          << " claim_success=" << rendezvous_diagnostics.claim_success
                          << " publish_attempts=" << rendezvous_diagnostics.publish_attempts
                          << " publish_success=" << rendezvous_diagnostics.publish_success
                          << " fetch_attempts=" << rendezvous_diagnostics.fetch_attempts
                          << " fetch_hits=" << rendezvous_diagnostics.fetch_hits
                          << " local_revision=" << rendezvous_diagnostics.local_revision
                          << " remote_revision=" << rendezvous_diagnostics.remote_revision;
                if (!rendezvous_session_id.empty()) {
                    std::cout << " session_id=" << rendezvous_session_id;
                }
                if (rendezvous_expires_at_unix > 0) {
                    std::cout << " expires_at_unix=" << rendezvous_expires_at_unix;
                }
                if (!rendezvous_diagnostics.last_error.empty()) {
                    std::cout << " last_error=" << rendezvous_diagnostics.last_error;
                }
                std::cout << '\n';
            }
#endif

            if (use_dht_transport) {
                const std::string port_mapping_status = ice_port_mapping_result.status;
                std::cout << "Runtime DHT stats role=" << role_name
                          << " backend=" << dht_backend_name
                          << " reachable=" << (dht_reachable ? "true" : "false")
                          << " publish_attempts=" << dht_publish_attempts
                          << " publish_success=" << dht_publish_success
                          << " generation_publish_success=" << dht_generation_publish_success
                          << " fetch_attempts=" << dht_fetch_attempts
                          << " fetch_hits=" << dht_fetch_hits
                          << " generation=" << negotiation_generation_snapshot
                          << " negotiation_phase="
                          << redclaw::service::connection_negotiation_phase_to_string(
                                 negotiation_phase_snapshot)
                          << " answer_acknowledged="
                          << (answer_acknowledged_snapshot ? "true" : "false")
                          << " local_revision=" << dht_local_revision
                          << " remote_revision=" << dht_remote_revision
                          << " publish_revision=" << dht_publication.revision()
                          << " publish_expiry=" << dht_publication.expires_at_unix()
                          << " publish_expired_total=" << dht_publication.expired_total()
                          << " publish_late_results=" << dht_publication.late_result_total()
                          << " repair_attempts=" << dht_repair_attempts
                          << " stale_remote_skips=" << dht_stale_remote_skips
                          << " publisher_instance="
                          << dht_instance_summary(dht_publisher_instance_id)
                          << " last_host_instance="
                          << dht_instance_summary(dht_last_applied_host_instance_id)
                          << " persistent_offer_adopted_initial_total="
                          << dht_persistent_offer_adopted_initial_total
                          << " persistent_offer_adopted_host_restart_total="
                          << dht_persistent_offer_adopted_host_restart_total
                          << " persistent_offer_same_instance_rejected_total="
                          << dht_persistent_offer_same_instance_rejected_total
                          << " peer_instance_missing_total="
                          << dht_peer_instance_missing_total
                          << " duplicate_offer_application_suppressed_total="
                          << dht_duplicate_offer_application_suppressed_total;
                if (!dht_last_error.empty()) {
                    std::cout << " last_error=" << dht_last_error;
                }
                std::cout << '\n';
#if defined(REDCLAW_ENABLE_DHT_BACKENDS)
                if (libtorrent_dht_store != nullptr) {
                    const auto dht_backend_diagnostics = libtorrent_dht_store->diagnostics_snapshot();
                    std::cout << "Runtime DHT backend diagnostics role=" << role_name
                              << " listen_port=" << dht_backend_diagnostics.listen_port
                              << " listen_port_auto_selected="
                              << (dht_backend_diagnostics.listen_port_auto_selected ? "true" : "false")
                              << " listen_ready=" << (dht_backend_diagnostics.listen_ready ? "true" : "false")
                              << " listen_startup_failed=" << (dht_backend_diagnostics.listen_startup_failed ? "true" : "false")
                              << " listen_port_probe_attempts=" << dht_backend_diagnostics.listen_port_probe_attempts
                              << " bootstrap_seen=" << (dht_backend_diagnostics.bootstrap_seen ? "true" : "false")
                              << " bootstrap_count=" << dht_backend_diagnostics.bootstrap_count
                              << " listen_ok=" << dht_backend_diagnostics.listen_succeeded_count
                              << " listen_failed=" << dht_backend_diagnostics.listen_failed_count
                              << " dht_errors=" << dht_backend_diagnostics.dht_error_count
                              << " external_ip_count=" << dht_backend_diagnostics.external_ip_count
                              << " put_alerts=" << dht_backend_diagnostics.put_alert_count
                              << " publish_records=" << dht_backend_diagnostics.publish_records
                              << " publish_in_flight=" << dht_backend_diagnostics.publish_in_flight
                              << " publish_records_peak=" << dht_backend_diagnostics.publish_records_peak
                              << " publish_late_events=" << dht_backend_diagnostics.publish_late_events
                              << " publish_backpressure=" << dht_backend_diagnostics.publish_backpressure
                              << " last_put_num_success=" << dht_backend_diagnostics.last_put_num_success
                              << " mutable_get_started=" << dht_backend_diagnostics.mutable_get_started_count
                              << " mutable_get_responses=" << dht_backend_diagnostics.mutable_get_response_count
                              << " mutable_get_authoritative=" << dht_backend_diagnostics.mutable_get_authoritative_count
                              << " mutable_get_non_authoritative=" << dht_backend_diagnostics.mutable_get_non_authoritative_count
                              << " mutable_get_pending_polls=" << dht_backend_diagnostics.mutable_get_pending_poll_count
                              << " mutable_get_cache_hits=" << dht_backend_diagnostics.mutable_get_cache_hit_count
                              << " mutable_get_pending=" << dht_backend_diagnostics.mutable_get_pending_count
                              << " last_mutable_get_elapsed_ms=" << dht_backend_diagnostics.last_mutable_get_elapsed_ms
                              << " max_mutable_get_elapsed_ms=" << dht_backend_diagnostics.max_mutable_get_elapsed_ms
                              << " last_mutable_get_sequence=" << dht_backend_diagnostics.last_mutable_get_sequence
                              << " dht_nodes=" << dht_backend_diagnostics.dht_nodes
                              << " dht_node_cache=" << dht_backend_diagnostics.dht_node_cache
                              << " listen_address=" << (dht_backend_diagnostics.listen_address.empty() ? "auto" : dht_backend_diagnostics.listen_address)
                              << " listen_interfaces=" << dht_backend_diagnostics.listen_interfaces
                              << " bootstrap_nodes=" << format_ice_server_list(dht_backend_diagnostics.bootstrap_nodes);
                    if (!dht_backend_diagnostics.listen_port_selection_error.empty()) {
                        std::cout << " listen_port_selection_error="
                                  << dht_backend_diagnostics.listen_port_selection_error;
                    }
                    if (!dht_backend_diagnostics.last_external_ip.empty()) {
                        std::cout << " external_ip=" << dht_backend_diagnostics.last_external_ip;
                    }
                    if (!dht_backend_diagnostics.last_listen_success.empty()) {
                        std::cout << " last_listen_success=" << dht_backend_diagnostics.last_listen_success;
                    }
                    if (!dht_backend_diagnostics.last_listen_failure.empty()) {
                        std::cout << " last_listen_failure=" << dht_backend_diagnostics.last_listen_failure;
                    }
                    if (!dht_backend_diagnostics.last_dht_error.empty()) {
                        std::cout << " last_dht_error=" << dht_backend_diagnostics.last_dht_error;
                    }
                    if (!dht_backend_diagnostics.recent_dht_logs.empty()) {
                        std::cout << " recent_dht_log=" << dht_backend_diagnostics.recent_dht_logs.back();
                    }
                    std::cout << '\n';
                }
#endif
                std::cout << "Runtime NAT diagnostics role=" << role_name
                          << " dht_backend=" << dht_backend_name
                          << " dht_reachable=" << (dht_reachable ? "true" : "false")
                          << " ipv6_candidates=" << (options.enable_ipv6_candidates ? "enabled" : "disabled")
                          << " ice_udp_port=" << options.ice_udp_port
                          << " port_mapping=" << port_mapping_status
                          << " mapped_internal_port=" << ice_port_mapping_result.internal_port
                          << " mapped_external_port=" << ice_port_mapping_result.external_port
                          << " failure_class=" << classify_nat_failure(
                                 diagnostics_snapshot,
                                 true,
                                 dht_reachable,
                                 options.enable_port_mapping,
                                 port_mapping_status)
                          << '\n';
            }

#ifdef _WIN32
            if (use_tcp_transport) {
                std::cout << "Runtime tcp stats role=" << role_name
                          << " connect_attempts=" << tcp_diagnostics.connect_attempt_count
                          << " connect_success=" << tcp_diagnostics.connect_success_count
                          << " reconnects=" << tcp_diagnostics.reconnect_count
                          << " disconnects=" << tcp_diagnostics.disconnect_count
                          << " send_failures=" << tcp_diagnostics.send_failure_count
                          << " receive_failures=" << tcp_diagnostics.receive_failure_count
                          << " malformed_messages=" << tcp_diagnostics.malformed_message_count
                          << " backoff_ms=" << tcp_diagnostics.current_backoff_ms
                          << " last_failure=" << to_string(tcp_diagnostics.last_failure)
                          << " last_wsa_error=" << tcp_diagnostics.last_wsa_error
                          << '\n';

                if (!tcp_diagnostics.last_failure_detail.empty()) {
                    std::cout << "Runtime tcp failure detail role=" << role_name
                              << " detail=" << tcp_diagnostics.last_failure_detail << '\n';
                }
            }
#endif

            if (options.stream_smoke) {
                DesktopStreamSmokeCounters stream_snapshot;
                bool channel_open = false;
                bool media_channel_open = false;
                bool control_channel_open = false;
                redclaw::capture::CaptureBackendTelemetry capture_telemetry;
                redclaw::capture::EncoderExecutionDiagnostics encoder_diagnostics;
                HostStreamStageTelemetry host_stage_telemetry;
                ControllerStreamStageTelemetry controller_stage_telemetry;
                StreamRttTelemetry rtt_telemetry;
                StreamAdaptiveControlState adaptive_control;
                redclaw::net::DataChannelTransportStats transport_stats;
                redclaw::net::MediaTransportEstimate transport_estimate;
                redclaw::net::MediaCongestionDecision congestion_decision;
                redclaw::net::MediaPacerTelemetry pacer_telemetry;
                std::uint32_t source_width = 0;
                std::uint32_t source_height = 0;
                std::uint32_t encoded_width = 0;
                std::uint32_t encoded_height = 0;
                std::uint64_t viewport_request_total = 0;
                std::uint64_t encoder_resolution_reconfigure_total = 0;
                std::uint64_t media_fragments_sent = 0;
                std::uint64_t media_fragments_received = 0;
                std::uint64_t fragment_parse_failures = 0;
                std::uint64_t reassembly_failures = 0;
                std::uint64_t qa_media_fragment_drops = 0;
                std::uint64_t qa_media_loss_frame_id = 0;
                std::uint64_t qa_media_recovery_keyframe_id = 0;
                std::uint64_t qa_forced_channel_close_total = 0;
                std::uint64_t qa_forced_channel_close_frame_id = 0;
                std::uint64_t qa_post_reconnect_frame_id = 0;
                std::uint64_t qa_post_reconnect_direct_pipe_written_total = 0;
                std::uint64_t required_channels_open_total = 0;
                std::uint64_t recovery_reset_total = 0;
                std::uint64_t recovery_success_total = 0;
                std::uint64_t encoded_frames_reassembled = 0;
                std::uint64_t incomplete_frames_dropped = 0;
                std::uint64_t dependency_frames_dropped = 0;
                std::uint64_t completed_keyframes = 0;
                std::uint64_t dropped_keyframes = 0;
                std::uint64_t keyframe_requests_sent = 0;
                std::uint64_t keyframe_requests_received = 0;
                std::uint64_t playback_starvation_sent = 0;
                std::uint64_t playback_starvation_received = 0;
                std::uint64_t static_keyframe_refresh_attempts = 0;
                std::uint64_t static_keyframe_refresh_successes = 0;
                std::uint64_t transport_feedback_sent = 0;
                std::uint64_t transport_feedback_received = 0;
                std::uint64_t transport_feedback_ignored = 0;
                std::uint64_t transport_feedback_invalid = 0;
                redclaw::session::DesktopSourceActivitySnapshot source_activity;
                redclaw::session::ControllerDecoderRecoverySnapshot decoder_recovery;
                bool required_channels_ready = false;
                std::uint64_t required_channels_ready_at_ms = 0;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    stream_snapshot = stream_counters;
                    media_channel_open = stream_media_channel_open;
                    control_channel_open = stream_control_channel_open;
                    channel_open = media_channel_open && control_channel_open;
                    capture_telemetry = stream_capture_telemetry_snapshot;
                    encoder_diagnostics = stream_encoder_diagnostics_snapshot;
                    host_stage_telemetry = stream_host_stage_telemetry_snapshot;
                    controller_stage_telemetry = stream_controller_stage_telemetry_snapshot;
                    rtt_telemetry = stream_rtt_telemetry_snapshot;
                    adaptive_control = stream_adaptive_control_snapshot;
                    transport_stats = stream_data_channel_transport_stats_snapshot;
                    transport_estimate = stream_transport_estimate_snapshot;
                    congestion_decision = stream_congestion_decision_snapshot;
                    source_width = stream_source_width_snapshot;
                    source_height = stream_source_height_snapshot;
                    encoded_width = stream_encoded_width_snapshot;
                    encoded_height = stream_encoded_height_snapshot;
                    viewport_request_total = stream_viewport_request_total;
                    encoder_resolution_reconfigure_total =
                        stream_encoder_resolution_reconfigure_total;
                    media_fragments_sent = stream_media_fragments_sent;
                    media_fragments_received = stream_media_fragments_received;
                    fragment_parse_failures = stream_fragment_parse_failures;
                    reassembly_failures = stream_reassembly_failures;
                    qa_media_fragment_drops = stream_qa_media_fragment_drop_total;
                    qa_media_loss_frame_id = stream_qa_media_loss_frame_id;
                    qa_media_recovery_keyframe_id = stream_qa_media_recovery_keyframe_id;
                    qa_forced_channel_close_total = stream_qa_forced_channel_close_total;
                    qa_forced_channel_close_frame_id =
                        stream_qa_forced_channel_close_frame_id;
                    qa_post_reconnect_frame_id = stream_qa_post_reconnect_frame_id;
                    qa_post_reconnect_direct_pipe_written_total =
                        stream_qa_post_reconnect_direct_pipe_written_total;
                    required_channels_open_total = stream_required_channels_open_total;
                    recovery_reset_total = stream_recovery_reset_total;
                    recovery_success_total = stream_recovery_success_total;
                    encoded_frames_reassembled = stream_encoded_frames_reassembled;
                    incomplete_frames_dropped = stream_incomplete_frames_dropped;
                    dependency_frames_dropped = stream_dependency_frames_dropped;
                    completed_keyframes = stream_completed_keyframes;
                    dropped_keyframes = stream_dropped_keyframes;
                    keyframe_requests_sent = stream_keyframe_request_sent_total;
                    keyframe_requests_received = stream_keyframe_request_received_total;
                    playback_starvation_sent =
                        stream_playback_starvation_sent_total;
                    playback_starvation_received =
                        stream_playback_starvation_received_total;
                    static_keyframe_refresh_attempts =
                        stream_static_keyframe_refresh_attempt_total;
                    static_keyframe_refresh_successes =
                        stream_static_keyframe_refresh_success_total;
                    transport_feedback_sent = stream_transport_feedback_sent_total;
                    transport_feedback_received = stream_transport_feedback_received_total;
                    transport_feedback_ignored = stream_transport_feedback_ignored_total;
                    transport_feedback_invalid = stream_transport_feedback_invalid_total;
                    required_channels_ready = stream_required_channels_ready;
                    required_channels_ready_at_ms = stream_required_channels_ready_at_ms;
                }
                pacer_telemetry = stream_media_pacer.telemetry();
                {
                    std::lock_guard<std::mutex> source_lock(stream_source_activity_mutex);
                    source_activity = stream_source_activity_tracker.snapshot();
                }
                decoder_recovery = stream_decoder_recovery_coordinator.snapshot();

                const std::uint64_t rate_sample_ms = now_steady_ms();
                const std::uint64_t rate_window_ms =
                    rate_sample_ms >= last_stream_rate_sample_ms
                        ? (rate_sample_ms - last_stream_rate_sample_ms)
                        : 0;
                const std::uint32_t captured_delta = stream_counter_delta(
                    stream_snapshot.captured_frames,
                    last_stream_rate_snapshot.captured_frames);
                const std::uint32_t synthetic_delta = stream_counter_delta(
                    stream_snapshot.synthetic_frames,
                    last_stream_rate_snapshot.synthetic_frames);
                const std::uint32_t encoded_delta = stream_counter_delta(
                    stream_snapshot.encoded_frames,
                    last_stream_rate_snapshot.encoded_frames);
                const std::uint32_t transmitted_delta = stream_counter_delta(
                    stream_snapshot.transmitted_frames,
                    last_stream_rate_snapshot.transmitted_frames);
                const std::uint32_t received_delta = stream_counter_delta(
                    stream_snapshot.received_frames,
                    last_stream_rate_snapshot.received_frames);
                const std::uint32_t decoded_delta = stream_counter_delta(
                    stream_snapshot.decoded_frames,
                    last_stream_rate_snapshot.decoded_frames);
                const std::uint32_t rendered_delta = stream_counter_delta(
                    stream_snapshot.rendered_frames,
                    last_stream_rate_snapshot.rendered_frames);
                const std::uint32_t encode_failures_delta = stream_counter_delta(
                    stream_snapshot.encode_failures,
                    last_stream_rate_snapshot.encode_failures);
                const std::uint32_t capture_failures_delta = stream_counter_delta(
                    stream_snapshot.capture_failures,
                    last_stream_rate_snapshot.capture_failures);
                const std::uint32_t transmit_failures_delta = stream_counter_delta(
                    stream_snapshot.transmit_failures,
                    last_stream_rate_snapshot.transmit_failures);
                const std::uint32_t transmit_backpressure_delta = stream_counter_delta(
                    stream_snapshot.transmit_backpressure_drops,
                    last_stream_rate_snapshot.transmit_backpressure_drops);
                const std::uint32_t transmit_local_pacing_delta = stream_counter_delta(
                    stream_snapshot.transmit_local_pacing_drops,
                    last_stream_rate_snapshot.transmit_local_pacing_drops);
                const std::uint32_t decode_failures_delta = stream_counter_delta(
                    stream_snapshot.decode_failures,
                    last_stream_rate_snapshot.decode_failures);
                const std::uint32_t render_failures_delta = stream_counter_delta(
                    stream_snapshot.render_failures,
                    last_stream_rate_snapshot.render_failures);
                const std::uint32_t source_delta = captured_delta + synthetic_delta;
                const std::uint32_t source_gap = source_delta > encoded_delta ? (source_delta - encoded_delta) : 0;
                const std::uint32_t target_encode_budget =
                    static_cast<std::uint32_t>((std::min<std::uint64_t>)(
                        (static_cast<std::uint64_t>(resolve_stream_target_fps(adaptive_control))
                             * rate_window_ms
                         + 999ULL)
                            / 1000ULL,
                        std::numeric_limits<std::uint32_t>::max()));
                const std::uint32_t encode_budget_gap =
                    redclaw::net::resolve_available_encode_budget_gap(
                        source_delta,
                        encoded_delta,
                        target_encode_budget);
                const auto encode_budget_pressure =
                    redclaw::net::evaluate_available_encode_budget_pressure(
                        source_delta,
                        encoded_delta,
                        target_encode_budget);
                std::uint32_t network_gap = 0;
                std::uint32_t playback_gap = 0;
                const std::uint32_t encoder_output_delta = stream_counter_delta(
                    encoder_diagnostics.encoded_frame_count,
                    last_stream_encoder_diagnostics_snapshot.encoded_frame_count);
                const std::uint32_t encoder_dropped_delta = stream_counter_delta(
                    encoder_diagnostics.dropped_frame_count,
                    last_stream_encoder_diagnostics_snapshot.dropped_frame_count);
                const std::uint32_t encoder_backpressure_delta = stream_counter_delta(
                    encoder_diagnostics.backpressure_event_count,
                    last_stream_encoder_diagnostics_snapshot.backpressure_event_count);
                const std::uint32_t encode_attempts_delta = encoder_output_delta + encoder_dropped_delta;
                const std::uint32_t capture_attempts_delta = stream_counter_delta(
                    capture_telemetry.total_capture_attempt_count,
                    last_stream_capture_telemetry_snapshot.total_capture_attempt_count);
                const std::uint32_t capture_timeouts_delta = stream_counter_delta(
                    capture_telemetry.total_timeout_count,
                    last_stream_capture_telemetry_snapshot.total_timeout_count);
                const std::uint64_t capture_us_delta = stream_total_delta(
                    host_stage_telemetry.total_capture_us,
                    last_stream_host_stage_telemetry_snapshot.total_capture_us);
                const std::uint64_t capture_wait_us_delta = stream_total_delta(
                    capture_telemetry.total_capture_wait_us,
                    last_stream_capture_telemetry_snapshot.total_capture_wait_us);
                const std::uint64_t capture_copy_us_delta = stream_total_delta(
                    capture_telemetry.total_capture_copy_us,
                    last_stream_capture_telemetry_snapshot.total_capture_copy_us);
                const std::uint64_t accumulated_frames_delta = stream_total_delta(
                    capture_telemetry.total_accumulated_frames,
                    last_stream_capture_telemetry_snapshot.total_accumulated_frames);
                const std::uint64_t packetize_us_delta = stream_total_delta(
                    host_stage_telemetry.total_packetize_us,
                    last_stream_host_stage_telemetry_snapshot.total_packetize_us);
                const std::uint64_t send_us_delta = stream_total_delta(
                    host_stage_telemetry.total_send_us,
                    last_stream_host_stage_telemetry_snapshot.total_send_us);
                const std::uint64_t input_prepare_us_delta = stream_total_delta(
                    encoder_diagnostics.total_input_prepare_us,
                    last_stream_encoder_diagnostics_snapshot.total_input_prepare_us);
                const std::uint64_t send_frame_us_delta = stream_total_delta(
                    encoder_diagnostics.total_send_frame_us,
                    last_stream_encoder_diagnostics_snapshot.total_send_frame_us);
                const std::uint64_t receive_packet_us_delta = stream_total_delta(
                    encoder_diagnostics.total_receive_packet_us,
                    last_stream_encoder_diagnostics_snapshot.total_receive_packet_us);
                const std::uint64_t payload_copy_us_delta = stream_total_delta(
                    encoder_diagnostics.total_payload_copy_us,
                    last_stream_encoder_diagnostics_snapshot.total_payload_copy_us);
                const std::uint64_t encode_total_us_delta = stream_total_delta(
                    encoder_diagnostics.total_encode_us,
                    last_stream_encoder_diagnostics_snapshot.total_encode_us);
                const std::uint32_t missed_deadlines_delta = stream_counter_delta(
                    host_stage_telemetry.missed_deadline_count,
                    last_stream_host_stage_telemetry_snapshot.missed_deadline_count);
                const std::uint32_t send_blocked_events_delta = stream_counter_delta(
                    host_stage_telemetry.send_blocked_event_count,
                    last_stream_host_stage_telemetry_snapshot.send_blocked_event_count);
                const std::uint32_t skipped_intervals_delta = stream_counter_delta(
                    host_stage_telemetry.skipped_interval_count,
                    last_stream_host_stage_telemetry_snapshot.skipped_interval_count);
                const std::uint32_t encode_submit_attempts_delta = stream_counter_delta(
                    host_stage_telemetry.encode_submit_attempt_count,
                    last_stream_host_stage_telemetry_snapshot.encode_submit_attempt_count);
                const std::uint32_t encode_output_not_ready_delta = stream_counter_delta(
                    host_stage_telemetry.encode_output_not_ready_count,
                    last_stream_host_stage_telemetry_snapshot.encode_output_not_ready_count);
                const std::uint64_t controller_decode_us_delta = stream_total_delta(
                    controller_stage_telemetry.total_decode_us,
                    last_stream_controller_stage_telemetry_snapshot.total_decode_us);
                const std::uint64_t controller_direct_pipe_write_us_delta = stream_total_delta(
                    controller_stage_telemetry.total_direct_pipe_write_us,
                    last_stream_controller_stage_telemetry_snapshot.total_direct_pipe_write_us);
                const std::uint32_t controller_decode_attempts_delta = stream_counter_delta(
                    controller_stage_telemetry.decode_attempt_count,
                    last_stream_controller_stage_telemetry_snapshot.decode_attempt_count);
                const std::uint32_t controller_direct_pipe_attempts_delta = stream_counter_delta(
                    controller_stage_telemetry.direct_pipe_write_attempt_count,
                    last_stream_controller_stage_telemetry_snapshot.direct_pipe_write_attempt_count);
                const std::uint32_t controller_direct_pipe_success_delta = stream_counter_delta(
                    controller_stage_telemetry.direct_pipe_write_success_count,
                    last_stream_controller_stage_telemetry_snapshot.direct_pipe_write_success_count);
                const std::uint32_t controller_direct_pipe_failures_delta = stream_counter_delta(
                    controller_stage_telemetry.direct_pipe_write_failure_count,
                    last_stream_controller_stage_telemetry_snapshot.direct_pipe_write_failure_count);
                const std::uint64_t controller_direct_pipe_writer_delta = stream_total_delta(
                    controller_stage_telemetry.direct_pipe_transport_stats.writer_frame_count,
                    last_stream_controller_stage_telemetry_snapshot.direct_pipe_transport_stats.writer_frame_count);
                const std::uint64_t controller_direct_pipe_reader_delta = stream_total_delta(
                    controller_stage_telemetry.direct_pipe_transport_stats.reader_sequence,
                    last_stream_controller_stage_telemetry_snapshot.direct_pipe_transport_stats.reader_sequence);
                const std::uint64_t controller_direct_pipe_oversize_drops_delta = stream_total_delta(
                    controller_stage_telemetry.direct_pipe_transport_stats.writer_oversize_drop_count,
                    last_stream_controller_stage_telemetry_snapshot.direct_pipe_transport_stats.writer_oversize_drop_count);
                const std::uint64_t controller_direct_pipe_busy_drops_delta = stream_total_delta(
                    controller_stage_telemetry.direct_pipe_transport_stats.writer_busy_drop_count,
                    last_stream_controller_stage_telemetry_snapshot.direct_pipe_transport_stats.writer_busy_drop_count);
                if (options.role == RuntimeRole::kController) {
                    if (controller_stage_telemetry.direct_pipe_transport_stats.connected) {
                        playback_gap = controller_direct_pipe_writer_delta
                                > controller_direct_pipe_reader_delta
                            ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                  controller_direct_pipe_writer_delta
                                      - controller_direct_pipe_reader_delta,
                                  (std::numeric_limits<std::uint32_t>::max)()))
                            : 0;
                    } else {
                        playback_gap = received_delta > rendered_delta
                            ? (received_delta - rendered_delta)
                            : 0;
                    }
                }
                const auto stream_health_value = redclaw::net::classify_desktop_stream_health({
                    .role = options.role == RuntimeRole::kHost
                        ? redclaw::net::DesktopStreamEndpointRole::kHost
                        : redclaw::net::DesktopStreamEndpointRole::kController,
                    .startup_tracking = saw_connected_state
                        && required_channels_ready
                        && required_channels_ready_at_ms != 0,
                    .startup_elapsed_ms = required_channels_ready_at_ms != 0
                        && now_steady_ms() >= required_channels_ready_at_ms
                        ? now_steady_ms() - required_channels_ready_at_ms
                        : 0,
                    .startup_timeout_ms = kDesktopStreamFirstMediaDeadlineMs,
                    .captured_frames_total = stream_snapshot.captured_frames,
                    .encode_submit_attempts_total =
                        host_stage_telemetry.encode_submit_attempt_count,
                    .encoded_frames_total = stream_snapshot.encoded_frames,
                    .transmitted_frames_total = stream_snapshot.transmitted_frames,
                    .received_frames_total = stream_snapshot.received_frames,
                    .reassembled_frames_total = static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(
                            encoded_frames_reassembled,
                            (std::numeric_limits<std::uint32_t>::max)())),
                    .delivered_frames_total =
                        controller_stage_telemetry.direct_pipe_write_success_count,
                    .relay_path = diagnostics_snapshot.local_relay > 0
                        || diagnostics_snapshot.remote_relay > 0,
                    .direct_nat_path = diagnostics_snapshot.local_srflx > 0
                        || diagnostics_snapshot.remote_srflx > 0
                        || diagnostics_snapshot.local_ipv6 > 0
                        || diagnostics_snapshot.remote_ipv6 > 0,
                    .encode_backpressure = encode_budget_pressure
                        != redclaw::net::AvailableEncodeBudgetPressure::kStable,
                    .encode_failures = encode_failures_delta,
                    .transmit_failures = transmit_failures_delta,
                    .transmit_backpressure = transmit_backpressure_delta,
                    .decode_failures = decode_failures_delta,
                    .render_failures = render_failures_delta,
                    .receiver_loss_per_mille = options.role == RuntimeRole::kHost
                        ? adaptive_control.last_receiver_assembly_loss_per_mille
                        : 0,
                    .transmitted_frames = transmitted_delta,
                    .received_frames = received_delta,
                    .rendered_frames = rendered_delta,
                    .direct_pipe_connected =
                        controller_stage_telemetry.direct_pipe_transport_stats.connected,
                    .direct_pipe_write_failures = controller_direct_pipe_failures_delta,
                    .direct_pipe_writer_frames = controller_direct_pipe_writer_delta,
                    .direct_pipe_reader_frames = controller_direct_pipe_reader_delta,
                    .direct_pipe_backlog =
                        controller_stage_telemetry.current_direct_pipe_backlog,
                });
                const std::string stream_health(
                    redclaw::net::desktop_stream_health_name(stream_health_value));

                std::cout << "Runtime desktop stream stats role=" << role_name
                          << " channel_open=" << (channel_open ? "true" : "false")
                          << " media_channel_open=" << (media_channel_open ? "true" : "false")
                          << " control_channel_open=" << (control_channel_open ? "true" : "false")
                          << " capture_backend=" << capture_backend_to_string(capture_telemetry.active_backend)
                          << " capture_availability=" << static_cast<int>(capture_telemetry.availability)
                          << " capture_generation=" << capture_telemetry.generation
                          << " capture_failure_stage=" << static_cast<int>(capture_telemetry.last_failure.stage)
                          << " capture_failure_kind=" << static_cast<int>(capture_telemetry.last_failure.kind)
                          << " capture_failure_hresult=" << capture_telemetry.last_failure.hresult
                          << " capture_dda_rebuilds=" << capture_telemetry.dda_rebuild_count
                          << " capture_backend_switches=" << capture_telemetry.backend_switch_count
                          << " capture_fallback_attempts=" << capture_telemetry.fallback_attempt_count
                          << " capture_fallback_reason=" << std::quoted(capture_telemetry.last_fallback_reason)
                          << " capture_timeouts=" << capture_telemetry.consecutive_timeouts
                          << " capture_consecutive_failures=" << capture_telemetry.consecutive_failures
                          << " capture_stale_frames=" << capture_telemetry.stale_frame_count
                          << " capture_black_frame_ratio=" << format_stream_stage_ms(capture_telemetry.black_frame_ratio * 100.0)
                          << " capture_attempt_total=" << capture_telemetry.total_capture_attempt_count
                          << " capture_timeout_total=" << capture_telemetry.total_timeout_count
                          << " native_texture_pool_creates=" << capture_telemetry.native_texture_pool_create_count
                          << " native_texture_pool_reuses=" << capture_telemetry.native_texture_pool_reuse_count
                          << " native_texture_pool_exhaustions=" << capture_telemetry.native_texture_pool_exhaustion_count
                          << " capture_frame_pool_recreates=" << capture_telemetry.frame_pool_recreate_count
                          << " last_capture_frame_pool_recreate_ms="
                          << format_stream_stage_ms(capture_telemetry.last_frame_pool_recreate_ms)
                          << " frame_present_delta_ms=" << format_stream_stage_ms(capture_telemetry.frame_present_delta_ms)
                          << " accumulated_frames_total=" << capture_telemetry.total_accumulated_frames
                          << " last_accumulated_frames=" << capture_telemetry.last_accumulated_frames
                          << " source_width=" << source_width
                          << " source_height=" << source_height
                          << " encoded_width=" << encoded_width
                          << " encoded_height=" << encoded_height
                          << " viewport_request_total=" << viewport_request_total
                          << " encoder_resolution_reconfigure_total="
                          << encoder_resolution_reconfigure_total
                          << " source_activity_state="
                          << static_cast<int>(source_activity.state)
                          << " source_activity_revision=" << source_activity.revision
                          << " source_reference_frame_id="
                          << source_activity.reference_frame_id
                          << " source_reference_keyframe_id="
                          << source_activity.reference_keyframe_id
                          << " source_static_entry_total="
                          << source_activity.static_entry_total
                          << " source_static_exit_total="
                          << source_activity.static_exit_total
                          << " source_capture_stall_total="
                          << source_activity.capture_stall_total
                          << " source_reference_retry_total="
                          << source_activity.reference_retry_total
                          << " captured=" << stream_snapshot.captured_frames
                          << " synthetic=" << stream_snapshot.synthetic_frames
                          << " capture_failures=" << stream_snapshot.capture_failures
                          << " encoded=" << stream_snapshot.encoded_frames
                          << " encode_failures=" << stream_snapshot.encode_failures
                           << " transmitted=" << stream_snapshot.transmitted_frames
                          << " transmit_failures=" << stream_snapshot.transmit_failures
                          << " transmit_backpressure_drops=" << stream_snapshot.transmit_backpressure_drops
                          << " transmit_local_pacing_drops=" << stream_snapshot.transmit_local_pacing_drops
                          << " media_fragments_sent=" << media_fragments_sent
                           << " received=" << stream_snapshot.received_frames
                           << " media_fragments_received=" << media_fragments_received
                           << " fragment_parse_failures=" << fragment_parse_failures
                           << " reassembly_failures=" << reassembly_failures
                           << " qa_media_fragment_drops=" << qa_media_fragment_drops
                           << " qa_media_loss_frame_id=" << qa_media_loss_frame_id
                           << " qa_media_recovery_keyframe_id=" << qa_media_recovery_keyframe_id
                           << " qa_forced_channel_close_total=" << qa_forced_channel_close_total
                           << " qa_forced_channel_close_frame_id="
                           << qa_forced_channel_close_frame_id
                           << " qa_post_reconnect_frame_id=" << qa_post_reconnect_frame_id
                           << " qa_post_reconnect_direct_pipe_written_total="
                           << qa_post_reconnect_direct_pipe_written_total
                           << " required_channels_open_total=" << required_channels_open_total
                           << " recovery_reset_total=" << recovery_reset_total
                           << " recovery_success_total=" << recovery_success_total
                           << " capture_worker_stage="
                           << stream_worker_stage_name(stream_capture_worker_stage.load())
                           << " capture_worker_stage_age_ms="
                           << (now_steady_ms() >= stream_capture_worker_stage_since_ms.load()
                                   ? now_steady_ms() - stream_capture_worker_stage_since_ms.load()
                                   : 0)
                           << " encoder_worker_stage="
                           << stream_worker_stage_name(stream_encoder_worker_stage.load())
                           << " encoder_worker_stage_age_ms="
                           << (now_steady_ms() >= stream_encoder_worker_stage_since_ms.load()
                                   ? now_steady_ms() - stream_encoder_worker_stage_since_ms.load()
                                   : 0)
                           << " encoded_frames_reassembled=" << encoded_frames_reassembled
                           << " incomplete_frames_dropped=" << incomplete_frames_dropped
                           << " dependency_frames_dropped=" << dependency_frames_dropped
                           << " completed_keyframes=" << completed_keyframes
                           << " dropped_keyframes=" << dropped_keyframes
                           << " incomplete_frame_feedback_sent_total="
                           << playback_starvation_sent
                           << " incomplete_frame_feedback_received_total="
                           << playback_starvation_received
                           << " direct_pipe_written="
                           << controller_stage_telemetry.direct_pipe_write_success_count
                           << " decoded=" << stream_snapshot.decoded_frames
                          << " decode_failures=" << stream_snapshot.decode_failures
                          << " rendered=" << stream_snapshot.rendered_frames
                          << " render_failures=" << stream_snapshot.render_failures;
                if (!stream_snapshot.last_preview_path.empty()) {
                    std::cout << " preview_path=" << stream_snapshot.last_preview_path;
                }
                if (!stream_snapshot.last_error.empty()) {
                    std::cout << " last_error=" << stream_snapshot.last_error;
                }
                std::cout << '\n';

                if (options.role == RuntimeRole::kHost) {
                    if (const auto timing = stream_transport_estimator.timing_snapshot()) {
                        std::cout << redclaw::net::format_media_transport_timing_sample(
                            *timing, now_steady_ms() * 1000ULL,
                            adaptive_control.last_rtt_queue_delay_ms) << '\n';
                    }
                }
                std::cout << "Runtime media transport feedback stats role=" << role_name
                          << " feedback_sent_total=" << transport_feedback_sent
                          << " feedback_received_total=" << transport_feedback_received
                          << " feedback_ignored_total=" << transport_feedback_ignored
                          << " feedback_invalid_total=" << transport_feedback_invalid
                          << " feedback_fresh="
                          << (transport_estimate.feedback_fresh ? "true" : "false")
                          << " feedback_age_ms=" << transport_estimate.feedback_age_ms
                          << " acked_bitrate_kbps="
                          << transport_estimate.acknowledged_bitrate_kbps
                          << " pacing_bitrate_kbps="
                          << pacer_telemetry.pacing_bitrate_kbps
                          << " transport_loss_per_mille="
                          << transport_estimate.loss_per_mille
                          << " transport_queue_delay_ms="
                          << transport_estimate.queue_delay_ms
                          << " transport_acked_packets_total="
                          << transport_estimate.acknowledged_packets
                          << " transport_lost_packets_total="
                          << transport_estimate.lost_packets
                          << " transport_feedback_sample_id="
                          << transport_estimate.feedback_sample_id
                          << " expired_in_flight_packets="
                          << transport_estimate.expired_in_flight_packets
                          << " expired_in_flight_bytes="
                          << transport_estimate.expired_in_flight_bytes
                          << " in_flight_bytes=" << transport_estimate.in_flight_bytes
                          << " in_flight_limit_bytes="
                          << pacer_telemetry.in_flight_limit_bytes
                          << " pacer_active_depth=" << pacer_telemetry.active_depth
                          << " pacer_pending_depth=" << pacer_telemetry.pending_depth
                          << " pacer_deadline_drops=" << pacer_telemetry.deadline_drops
                          << " pacer_frame_budget_rejections="
                          << pacer_telemetry.frame_budget_rejections
                          << " pacer_recovery_generation=" << pacer_telemetry.recovery_generation
                          << " pacer_dependency_pending_drops=" << pacer_telemetry.dependency_pending_drops
                          << " pacer_suppressed_keyframe_requests=" << pacer_telemetry.suppressed_keyframe_requests
                          << " pacer_budget_revision=" << pacer_telemetry.budget_revision
                          << " pacer_budget_retry_attempt=" << pacer_telemetry.budget_retry_attempt
                          << " pacer_next_admission_ms=" << pacer_telemetry.next_admission_ms
                          << " pacer_probe_wire_bytes=" << pacer_telemetry.probe_wire_bytes
                          << " pacer_token_deadline_drops="
                          << pacer_telemetry.pacing_token_deadline_drops
                          << " pacer_in_flight_deadline_drops="
                          << pacer_telemetry.in_flight_deadline_drops
                          << " pacer_buffered_deadline_drops="
                          << pacer_telemetry.buffered_deadline_drops
                          << " pacer_channel_deadline_drops="
                          << pacer_telemetry.channel_deadline_drops
                          << " pacer_send_failures=" << pacer_telemetry.send_failures
                          << " pacer_in_flight_timeouts="
                          << pacer_telemetry.in_flight_timeouts
                          << " pacer_keyframe_required="
                          << (pacer_telemetry.keyframe_required ? "true" : "false")
                          << " transport_probe_total=" << congestion_decision.probe_count
                          << " transport_backoff_total=" << congestion_decision.backoff_count
                          << '\n';

                if (options.role == RuntimeRole::kHost) {
                    const auto& input_stats = remote_input_session.stats();
                    std::cout << "Runtime remote input stats role=" << role_name
                              << " authorized=" << (remote_input_session.authorized() ? "true" : "false")
                              << " state=" << static_cast<int>(remote_input_session.state())
                              << " received_batches=" << input_stats.received_batches
                              << " injected_events=" << input_stats.injected_events
                              << " rejected_batches=" << input_stats.rejected_batches
                              << " release_all_total=" << input_stats.release_all_count
                              << " queue_peak=" << input_stats.queue_peak
                              << " queue_current=" << remote_input_session.queued_event_count()
                              << " last_applied_sequence=" << input_stats.last_applied_sequence
                              << " capabilities_total=" << remote_input_capabilities_total
                              << " status_total=" << remote_input_status_total
                              << '\n';
                }

                {
                    const auto agent_peer = agent_peer_session.snapshot();
                    const auto agent_executor_snapshot = agent_peer_session.executor_snapshot();
                    const auto agent_metrics = agent_executor_snapshot.broker;
                    const bool agent_authorized = agent_peer.remote_authorized;
                    const auto agent_cached_event_bytes = agent_executor_snapshot.cached_event_bytes;
                    const auto agent_outbound_queue_current = agent_peer.request_depth + agent_peer.result_depth;
                    bool agent_channel_open = false;
                    bool agent_channel_unavailable = false;
                    std::uint64_t agent_open_total = 0;
                    std::uint64_t agent_close_total = 0;
                    std::uint64_t agent_rebuild_total = 0;
                    std::uint64_t agent_rebuild_success_total = 0;
                    const auto agent_qa_event_total = agent_peer.received_task_events;
                    std::uint64_t agent_qa_close_total = 0;
                    {
                        std::lock_guard<std::mutex> lock(callback_mutex);
                        agent_channel_open = stream_agent_channel_open;
                        agent_channel_unavailable = agent_unavailable_until_reconnect
                            || agent_peer.client_sync_pending != 0;
                        agent_open_total = agent_channel_open_total;
                        agent_close_total = agent_channel_close_total;
                        agent_rebuild_total = agent_channel_rebuild_attempt_total;
                        agent_rebuild_success_total = agent_channel_rebuild_success_total;
                        agent_qa_close_total = agent_qa_forced_channel_close_total;
                    }
                    std::cout << "Runtime remote agent stats role=" << role_name
                              << " authorized=" << (agent_authorized ? "true" : "false")
                              << " local_authorized=" << (agent_executor_snapshot.execution.authorized ? "true" : "false")
                              << " local_execution_state=" << static_cast<int>(agent_executor_snapshot.execution.state)
                              << " local_execution_task=" << (agent_executor_snapshot.execution.task_id.empty() ? "none" : agent_executor_snapshot.execution.task_id)
                              << " local_execution_queued=" << agent_executor_snapshot.execution.queued_turns
                              << " remote_authorized=" << (agent_peer.remote_authorized ? "true" : "false")
                              << " request_queue_depth=" << agent_peer.request_depth
                              << " result_queue_depth=" << agent_peer.result_depth
                              << " request_queue_peak=" << agent_peer.request_peak
                              << " result_queue_peak=" << agent_peer.result_peak
                              << " sent_requests=" << agent_peer.sent_requests
                              << " sent_results=" << agent_peer.sent_results
                              << " received_commands=" << agent_peer.received_commands
                              << " received_results=" << agent_peer.received_results
                              << " peer_rejected_total=" << agent_peer.rejected_total
                              << " peer_stale_total=" << agent_peer.stale_total
                              << " send_pump_max_us=" << agent_peer.max_pump_us
                              << " channel_open=" << (agent_channel_open ? "true" : "false")
                              << " channel_unavailable="
                              << (agent_channel_unavailable ? "true" : "false")
                              << " channel_open_total=" << agent_open_total
                              << " channel_close_total=" << agent_close_total
                              << " channel_rebuild_attempt_total=" << agent_rebuild_total
                              << " channel_rebuild_success_total="
                              << agent_rebuild_success_total
                              << " qa_task_event_received_total=" << agent_qa_event_total
                              << " qa_forced_channel_close_total=" << agent_qa_close_total
                              << " capability_refresh_total="
                              << agent_metrics.capability_refresh_total
                              << " task_create_total=" << agent_metrics.task_create_total
                              << " task_complete_total=" << agent_metrics.task_complete_total
                              << " task_failed_total=" << agent_metrics.task_failed_total
                              << " duplicate_task_rejected_total="
                              << agent_metrics.duplicate_task_rejected_total
                              << " event_total=" << agent_metrics.event_total
                              << " event_ack_total=" << agent_metrics.event_ack_total
                              << " replayed_event_total=" << agent_metrics.replayed_event_total
                              << " gap_total=" << agent_metrics.gap_total
                              << " approval_request_total="
                              << agent_metrics.approval_request_total
                              << " approval_accept_total=" << agent_metrics.approval_accept_total
                              << " approval_reject_total=" << agent_metrics.approval_reject_total
                              << " approval_timeout_total=" << agent_metrics.approval_timeout_total
                              << " queue_peak=" << agent_metrics.queue_peak
                              << " cached_event_bytes="
                              << agent_cached_event_bytes
                              << " cached_event_bytes_peak="
                              << agent_metrics.cached_event_bytes_peak
                              << " outbound_queue_current="
                              << agent_outbound_queue_current
                              << " outbound_queue_peak="
                              << agent_metrics.outbound_queue_peak
                              << " executor_queue_depth=" << agent_executor_snapshot.queued_commands
                              << " executor_queue_bytes=" << agent_executor_snapshot.queued_bytes
                              << " executor_wait_max_ms=" << agent_executor_snapshot.max_queue_wait_ms
                              << " executor_processing_max_ms=" << agent_executor_snapshot.max_processing_ms
                              << " executor_rejected_total=" << agent_executor_snapshot.rejected_total
                              << " local_ipc_received=" << local_agent_pipe.stats().received_total
                              << " local_ipc_sent=" << local_agent_pipe.stats().sent_total
                              << " local_ipc_rejected=" << local_agent_pipe.stats().rejected_total
                              << '\n';
                }

                std::cout << "Runtime desktop stream rates role=" << role_name
                          << " window_seconds="
                          << format_stream_stage_ms(static_cast<double>(rate_window_ms) / 1000.0)
                          << " capture_fps=" << format_stream_rate(stream_rate_per_second(captured_delta, rate_window_ms))
                          << " synthetic_fps=" << format_stream_rate(stream_rate_per_second(synthetic_delta, rate_window_ms))
                          << " source_fps=" << format_stream_rate(stream_rate_per_second(source_delta, rate_window_ms))
                          << " encode_fps=" << format_stream_rate(stream_rate_per_second(encoded_delta, rate_window_ms))
                          << " transmit_fps=" << format_stream_rate(stream_rate_per_second(transmitted_delta, rate_window_ms))
                          << " receive_fps=" << format_stream_rate(stream_rate_per_second(received_delta, rate_window_ms))
                          << " decode_fps=" << format_stream_rate(stream_rate_per_second(decoded_delta, rate_window_ms))
                          << " render_fps=" << format_stream_rate(stream_rate_per_second(rendered_delta, rate_window_ms))
                          << " source_drop_est=" << source_gap
                          << " encode_budget_gap_est=" << encode_budget_gap
                          << " network_drop_est=" << network_gap
                          << " playback_drop_est=" << playback_gap
                          << " capture_failures_delta=" << capture_failures_delta
                          << " encode_failures_delta=" << encode_failures_delta
                          << " transmit_failures_delta=" << transmit_failures_delta
                          << " transmit_backpressure_delta=" << transmit_backpressure_delta
                          << " transmit_local_pacing_delta=" << transmit_local_pacing_delta
                          << " decode_failures_delta=" << decode_failures_delta
                          << " render_failures_delta=" << render_failures_delta
                          << " first_media_deadline_ms=" << kDesktopStreamFirstMediaDeadlineMs
                          << " health=" << stream_health
                          << '\n';

                if (options.role == RuntimeRole::kHost) {
                    std::cout << "Runtime host stream diagnostics role=" << role_name
                              << " encoder_backend=" << encoder_backend_to_string(encoder_diagnostics.backend)
                              << " encoder_name="
                              << (encoder_diagnostics.encoder_name.empty() ? "n/a" : encoder_diagnostics.encoder_name)
                              << " encoder_configured_fps=" << encoder_diagnostics.configured_fps
                              << " encoder_configured_kbps=" << encoder_diagnostics.configured_bitrate_kbps
                              << " qsv_low_delay_brc=" << encoder_diagnostics.qsv_low_delay_brc
                              << " qsv_brc_verified=" << encoder_diagnostics.qsv_low_delay_brc_verified
                              << " encoder_input_mode="
                              << (encoder_diagnostics.input_mode.empty() ? "n/a" : encoder_diagnostics.input_mode)
                              << " encoder_gpu_scale="
                              << (encoder_diagnostics.d3d11_video_processor_scaling_active ? "true" : "false")
                              << " encoder_gpu_to_cpu_readback="
                              << (encoder_diagnostics.gpu_to_cpu_readback_active ? "true" : "false")
                               << " encoder_outputs_total=" << encoder_diagnostics.encoded_frame_count
                               << " encoder_keyframes_total=" << encoder_diagnostics.encoded_keyframe_count
                               << " keyframe_requests_received_total=" << keyframe_requests_received
                               << " keyframe_requests_sent_total=" << keyframe_requests_sent
                               << " incomplete_frame_feedback_sent_total="
                               << playback_starvation_sent
                               << " playback_starvation_received_total="
                               << playback_starvation_received
                              << " static_keyframe_refresh_attempts_total="
                              << static_keyframe_refresh_attempts
                              << " static_keyframe_refresh_success_total="
                              << static_keyframe_refresh_successes
                              << " encoder_drops_total=" << encoder_diagnostics.dropped_frame_count
                              << " encoder_backpressure_total=" << encoder_diagnostics.backpressure_event_count
                              << " rate_control_hot_supported="
                              << (encoder_diagnostics.rate_control_hot_update_supported ? "true" : "false")
                              << " rate_control_update_attempts="
                              << encoder_diagnostics.rate_control_update_attempt_count
                              << " rate_control_update_success="
                              << encoder_diagnostics.rate_control_update_success_count
                              << " rate_control_update_unsupported="
                              << encoder_diagnostics.rate_control_update_unsupported_count
                              << " rate_control_update_failures="
                              << encoder_diagnostics.rate_control_update_failure_count;
                    if (!encoder_diagnostics.capture_adapter_summary.empty()) {
                        std::cout << " encoder_capture_adapter=\""
                                  << encoder_diagnostics.capture_adapter_summary
                                  << "\"";
                    }
                    if (!encoder_diagnostics.hardware_input_block_reason.empty()) {
                        std::cout << " encoder_hwframe_block_reason=\""
                                  << encoder_diagnostics.hardware_input_block_reason
                                  << "\"";
                    }
                    if (!encoder_diagnostics.last_error_detail.empty()) {
                        std::cout << " encoder_last_error=" << encoder_diagnostics.last_error_detail;
                    }
                    std::cout << '\n';

                    std::cout << "Runtime stream latency stats role=" << role_name
                              << " ping_sent_total=" << rtt_telemetry.ping_sent_count
                              << " ping_ack_total=" << rtt_telemetry.ping_ack_count
                              << " ping_timeout_total=" << rtt_telemetry.ping_timeout_count
                              << " rtt_last_ms=" << rtt_telemetry.last_rtt_ms
                              << " rtt_min_ms=" << rtt_telemetry.min_rtt_ms
                              << " rtt_max_ms=" << rtt_telemetry.max_rtt_ms
                              << " srtt_ms=" << rtt_telemetry.smoothed_rtt_ms
                              << " rtt_avg_ms=" << format_stream_stage_ms(average_rtt_ms(rtt_telemetry))
                              << " ping_inflight=" << (rtt_telemetry.in_flight_ping_sequence != 0 ? "true" : "false")
                              << '\n';

                    std::cout << "Runtime stream transport stats role=" << role_name
                              << " channel_available=" << (transport_stats.available ? "true" : "false")
                              << " channel_open=" << (transport_stats.open ? "true" : "false")
                              << " send_blocked=" << (transport_stats.send_blocked ? "true" : "false")
                              << " buffered_amount=" << transport_stats.buffered_amount
                              << " available_amount=" << transport_stats.available_amount
                              << " max_message_size=" << transport_stats.max_message_size
                              << " buffered_low_threshold=" << transport_stats.buffered_amount_low_threshold
                              << '\n';

                    std::cout << "Runtime stream adaptation stats role=" << role_name
                              << " rate_revision=" << adaptive_control.rate_revision
                              << " target_fps=" << adaptive_control.target_fps
                              << " applied_fps=" << adaptive_control.applied_fps
                              << " target_bitrate_kbps=" << adaptive_control.target_bitrate_kbps
                              << " applied_bitrate_kbps=" << adaptive_control.applied_bitrate_kbps
                              << " target_max_bitrate_kbps=" << adaptive_control.target_max_bitrate_kbps
                              << " applied_max_bitrate_kbps=" << adaptive_control.applied_max_bitrate_kbps
                              << " configured_video_max_width=" << options.stream_video_max_width
                              << " qa_native_size=" << (options.stream_qa_native_size ? "true" : "false")
                              << " effective_video_max_width="
                              << resolve_stream_effective_video_max_width(options.stream_video_max_width, adaptive_control)
                              << " capture_fps_hint=" << adaptive_control.last_capture_fps_hint
                              << " frame_present_fps_hint=" << adaptive_control.last_frame_present_fps_hint
                              << " source_gap_hint=" << adaptive_control.last_source_gap
                              << " encode_budget_gap_hint=" << adaptive_control.last_encode_budget_gap
                              << " backpressure_delta_hint=" << adaptive_control.last_encoder_backpressure_delta
                              << " transmit_failures_delta_hint=" << adaptive_control.last_transmit_failures_delta
                              << " transmit_backpressure_delta_hint=" << adaptive_control.last_transmit_backpressure_delta
                              << " receiver_assembly_loss_per_mille="
                              << adaptive_control.last_receiver_assembly_loss_per_mille
                              << " receiver_assembly_pressure="
                              << static_cast<int>(adaptive_control.last_receiver_assembly_pressure)
                              << " receiver_decode_fps="
                              << adaptive_control.last_receiver_decode_fps
                              << " receiver_decode_pressure_windows="
                              << adaptive_control.receiver_decode_pressure_windows
                              << " receiver_decode_stable_windows="
                              << adaptive_control.receiver_decode_stable_windows
                              << " receiver_decode_fps_decrease_total="
                              << adaptive_control.receiver_decode_fps_decrease_total
                              << " receiver_decode_fps_increase_total="
                              << adaptive_control.receiver_decode_fps_increase_total
                              << " rtt_hint_ms=" << adaptive_control.last_rtt_ms
                              << " rtt_baseline_ms=" << adaptive_control.last_rtt_baseline_ms
                              << " rtt_queue_delay_ms=" << adaptive_control.last_rtt_queue_delay_ms
                              << " pressure_windows=" << adaptive_control.pressure_window_count
                              << " relief_windows=" << adaptive_control.relief_window_count
                              << " encoder_restarts_total=" << adaptive_control.encoder_restart_count
                              << " last_playback_starvation_ms="
                              << adaptive_control.last_playback_starvation_ms
                              << " feedback_age_ms=" << adaptive_control.last_feedback_age_ms
                              << " feedback_fresh_total=" << adaptive_control.fresh_feedback_count
                              << " feedback_delayed_total=" << adaptive_control.delayed_feedback_count
                              << " feedback_expired_total=" << adaptive_control.expired_feedback_count
                              << " feedback_stale_revision_total="
                              << adaptive_control.stale_revision_feedback_count
                              << " feedback_unknown_frame_total="
                              << adaptive_control.unknown_frame_feedback_count
                              << " feedback_immediate_rate_change_total="
                              << adaptive_control.immediate_feedback_rate_change_count
                              << " feedback_delayed_queued_total="
                              << adaptive_control.delayed_feedback_queued_count
                              << " feedback_ignored_total="
                              << adaptive_control.ignored_feedback_count
                              << " starvation_pacing_only="
                              << (adaptive_control.playback_starvation_pacing_active ? "true" : "false")
                              << " reconfigure_pending=" << (adaptive_control.encoder_reconfigure_pending ? "true" : "false")
                              << '\n';

                    std::cout << "Runtime host stream stage timings role=" << role_name
                              << " window_seconds="
                              << format_stream_stage_ms(static_cast<double>(rate_window_ms) / 1000.0)
                              << " capture_attempts=" << capture_attempts_delta
                              << " capture_timeouts_delta=" << capture_timeouts_delta
                              << " encode_submit_attempts=" << encode_submit_attempts_delta
                              << " encode_output_not_ready_delta=" << encode_output_not_ready_delta
                              << " encode_attempts=" << encode_attempts_delta
                              << " avg_capture_ms=" << format_stream_stage_ms(average_stage_ms(capture_us_delta, capture_attempts_delta))
                              << " avg_capture_wait_ms=" << format_stream_stage_ms(average_stage_ms(capture_wait_us_delta, capture_attempts_delta))
                              << " avg_capture_copy_ms=" << format_stream_stage_ms(average_stage_ms(capture_copy_us_delta, captured_delta))
                              << " avg_accumulated_frames=" << format_stream_stage_ms(average_stage_ms(accumulated_frames_delta * 1000ULL, captured_delta))
                              << " avg_input_prep_ms=" << format_stream_stage_ms(average_stage_ms(input_prepare_us_delta, encode_attempts_delta))
                              << " avg_encode_send_ms=" << format_stream_stage_ms(average_stage_ms(send_frame_us_delta, encode_attempts_delta))
                              << " avg_encode_receive_ms=" << format_stream_stage_ms(average_stage_ms(receive_packet_us_delta, encode_attempts_delta))
                              << " avg_payload_copy_ms=" << format_stream_stage_ms(average_stage_ms(payload_copy_us_delta, encoder_output_delta))
                              << " avg_encode_total_ms=" << format_stream_stage_ms(average_stage_ms(encode_total_us_delta, encode_attempts_delta))
                              << " avg_packetize_ms=" << format_stream_stage_ms(average_stage_ms(packetize_us_delta, encoded_delta))
                              << " avg_send_ms=" << format_stream_stage_ms(average_stage_ms(send_us_delta, encoded_delta))
                              << " send_blocked_events_delta=" << send_blocked_events_delta
                              << " missed_deadlines_delta=" << missed_deadlines_delta
                              << " skipped_intervals_delta=" << skipped_intervals_delta
                              << " encoder_backpressure_delta=" << encoder_backpressure_delta
                              << '\n';
                } else if (options.role == RuntimeRole::kController) {
                    std::string controller_consumption_health = "idle";
                    if (controller_direct_pipe_failures_delta > 0) {
                        controller_consumption_health = "direct_pipe_write_failures";
                    } else if (decode_failures_delta > 0) {
                        controller_consumption_health = "decode_failures";
                    } else if (controller_stage_telemetry.direct_pipe_transport_stats.connected) {
                        const std::uint64_t direct_pipe_progress_gap =
                            controller_direct_pipe_writer_delta > controller_direct_pipe_reader_delta
                            ? controller_direct_pipe_writer_delta - controller_direct_pipe_reader_delta
                            : 0;
                        if (controller_stage_telemetry.current_direct_pipe_backlog >= 2
                            || direct_pipe_progress_gap >= 3) {
                            controller_consumption_health = "gui_decode_backlog";
                        } else if (controller_stage_telemetry.current_direct_pipe_backlog > 0) {
                            controller_consumption_health = "gui_decode_draining";
                        } else if (controller_direct_pipe_writer_delta > 0 || controller_direct_pipe_reader_delta > 0) {
                            controller_consumption_health = "healthy";
                        }
                    } else if (controller_decode_attempts_delta > 0 || rendered_delta > 0) {
                        controller_consumption_health = "runtime_decode_path";
                    }

                    std::cout << "Runtime controller stream consumption stats role=" << role_name
                              << " window_seconds="
                              << format_stream_stage_ms(static_cast<double>(rate_window_ms) / 1000.0)
                              << " direct_pipe_connected="
                              << (controller_stage_telemetry.direct_pipe_transport_stats.connected ? "true" : "false")
                              << " decode_attempts=" << controller_decode_attempts_delta
                              << " avg_decode_ms="
                              << format_stream_stage_ms(average_stage_ms(controller_decode_us_delta, controller_decode_attempts_delta))
                              << " direct_pipe_attempts=" << controller_direct_pipe_attempts_delta
                              << " direct_pipe_success=" << controller_direct_pipe_success_delta
                              << " direct_pipe_failures=" << controller_direct_pipe_failures_delta
                              << " avg_direct_pipe_write_ms="
                              << format_stream_stage_ms(average_stage_ms(
                                     controller_direct_pipe_write_us_delta,
                                     controller_direct_pipe_attempts_delta))
                              << " direct_pipe_write_fps="
                              << format_stream_rate(stream_rate_per_second(
                                     controller_direct_pipe_writer_delta,
                                     rate_window_ms))
                              << " gui_decode_fps="
                              << format_stream_rate(stream_rate_per_second(
                                     controller_direct_pipe_reader_delta,
                                     rate_window_ms))
                              << " direct_pipe_backlog=" << controller_stage_telemetry.current_direct_pipe_backlog
                              << " direct_pipe_backlog_peak=" << controller_stage_telemetry.max_direct_pipe_backlog
                              << " direct_pipe_latest_sequence=" << controller_stage_telemetry.direct_pipe_transport_stats.latest_sequence
                              << " direct_pipe_reader_sequence=" << controller_stage_telemetry.direct_pipe_transport_stats.reader_sequence
                              << " direct_pipe_oversize_drops_delta=" << controller_direct_pipe_oversize_drops_delta
                              << " direct_pipe_busy_drops_delta=" << controller_direct_pipe_busy_drops_delta
                              << " direct_pipe_reader_active_sequence="
                              << controller_stage_telemetry.direct_pipe_transport_stats.reader_active_sequence
                              << " decoder_recovery_state="
                              << static_cast<int>(decoder_recovery.state)
                              << " decoder_recovery_generation="
                              << decoder_recovery.generation
                              << " decoder_recovery_pending_keyframe_id="
                              << decoder_recovery.pending_keyframe_id
                              << " decoder_recovery_request_attempt="
                              << decoder_recovery.request_attempt
                              << " decoder_recovery_breaks_total="
                              << decoder_recovery.dependency_break_total
                              << " decoder_recovery_requests_total="
                              << decoder_recovery.request_total
                              << " decoder_recovery_retries_total="
                              << decoder_recovery.retry_total
                              << " decoder_recovery_suppressed_total="
                              << decoder_recovery.suppressed_request_total
                              << " decoder_recovery_displayable_acks_total="
                              << decoder_recovery.displayable_ack_total
                              << " decoder_recovery_invalidated_total="
                              << decoder_recovery.invalidated_keyframe_total
                              << " health=" << controller_consumption_health
                              << '\n';
                }

                last_stream_rate_snapshot = stream_snapshot;
                last_stream_capture_telemetry_snapshot = capture_telemetry;
                last_stream_encoder_diagnostics_snapshot = encoder_diagnostics;
                last_stream_host_stage_telemetry_snapshot = host_stage_telemetry;
                last_stream_controller_stage_telemetry_snapshot = controller_stage_telemetry;
                last_stream_rate_sample_ms = rate_sample_ms;
            }

            const std::string timeline_text = redclaw::render::render_runtime_status_timeline_text(runtime_timeline, 6);
            if (!timeline_text.empty()) {
                std::cout << "Runtime timeline role=" << role_name << '\n'
                          << timeline_text;
            }

            runtime_timeline_ui_model.refresh();
            runtime_timeline_widget_component.refresh();
            const std::string widget_text = redclaw::render::render_runtime_status_timeline_widget_text(runtime_timeline_ui_model);
            if (!widget_text.empty()) {
                std::cout << "Runtime ui timeline widget role=" << role_name << '\n'
                          << widget_text;
            }

            const auto& widget_state = runtime_timeline_widget_component.state();
            std::cout << "Runtime ui timeline widget state role=" << role_name
                      << " items=" << widget_state.items.size()
                      << " has_warning=" << (widget_state.has_warning ? "true" : "false")
                      << " has_error=" << (widget_state.has_error ? "true" : "false")
                      << '\n';
        }

        input_loop_timing.next(redclaw::runtime::InputQaStage::kLoopTail);

        if (options.run_seconds > 0 && elapsed_seconds >= options.run_seconds) {
            std::cout << "Runtime mode exiting after run-seconds=" << options.run_seconds << '\n';
            if (options.stream_smoke) {
                DesktopStreamSmokeCounters stream_snapshot;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    stream_snapshot = stream_counters;
                }

                if (options.role == RuntimeRole::kHost && stream_snapshot.transmitted_frames == 0) {
                    std::cerr << "Runtime desktop stream smoke failed: host transmitted zero video frames";
                    if (!stream_snapshot.last_error.empty()) {
                        std::cerr << " last_error=" << stream_snapshot.last_error;
                    }
                    std::cerr << '\n';
                    ice_wrapper.close();
                    stop_stream_workers();
                    return 1;
                }

                if (options.role == RuntimeRole::kHost
                    && options.stream_require_capture
                    && stream_snapshot.captured_frames == 0) {
                    std::cerr << "Runtime desktop stream smoke failed: host captured zero real desktop frames";
                    if (!stream_snapshot.last_error.empty()) {
                        std::cerr << " last_error=" << stream_snapshot.last_error;
                    }
                    std::cerr << '\n';
                    ice_wrapper.close();
                    stop_stream_workers();
                    return 1;
                }

                if (options.role == RuntimeRole::kController && stream_snapshot.rendered_frames == 0) {
                    std::cerr << "Runtime desktop stream smoke failed: controller rendered zero preview frames";
                    if (!stream_snapshot.last_error.empty()) {
                        std::cerr << " last_error=" << stream_snapshot.last_error;
                    }
                    std::cerr << '\n';
                    ice_wrapper.close();
                    return 1;
                }
            }
            ice_wrapper.close();
            stop_stream_workers();
            return 0;
        }
    }
}

std::filesystem::path default_process_log_directory() {
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data != nullptr && *local_app_data != '\0') {
        return std::filesystem::path(local_app_data) / "RedClawDesktop" / "logs";
    }
    if (const char* temp = std::getenv("TEMP"); temp != nullptr && *temp != '\0') {
        return std::filesystem::path(temp) / "RedClawDesktop" / "logs";
    }
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    return (ec ? std::filesystem::current_path() : temp) / "RedClawDesktop" / "logs";
}

}  // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    RuntimeOptions runtime_options;
    std::string parse_error;
    if (!parse_runtime_options(argc, argv, &runtime_options, &parse_error)) {
        std::cerr << "Error: " << parse_error << '\n';
        print_usage();
        return 2;
    }

    if (runtime_options.help_requested) {
        print_usage();
        return 0;
    }

    redclaw::diag::ProcessFileLogger process_logger;
    redclaw::diag::ProcessFileLoggerConfig log_config;
    log_config.directory = runtime_options.log_dir.empty()
        ? default_process_log_directory()
        : std::filesystem::path(runtime_options.log_dir);
    log_config.file_stem = "redclaw-desktop";
    log_config.role = runtime_options.role_name.empty() ? "gui" : runtime_options.role_name;

    std::string log_error;
    if (!process_logger.start(log_config, &log_error)) {
        if (!runtime_options.log_dir.empty()) {
            std::cerr << "Error: explicit process log setup failed: " << log_error << '\n';
            return 2;
        }
        std::error_code ec;
        const auto fallback_root = std::filesystem::temp_directory_path(ec);
        if (!ec) {
            log_config.directory = fallback_root / "RedClawDesktop" / "logs";
        }
        if (ec || !process_logger.start(log_config, &log_error)) {
            std::cerr << "Error: process log setup failed: " << log_error << '\n';
            return 2;
        }
        std::cerr << "Process log default directory unavailable; using temporary fallback="
                  << log_config.directory.string() << '\n';
    }

    redclaw::runtime::LocalControlOutput local_control_output(runtime_options.gui_runtime_stdio);
    redclaw::diag::ScopedProcessStreamCapture stream_capture;
    if (!stream_capture.start(&process_logger, &log_error, runtime_options.gui_runtime_stdio)) {
        std::cerr << "Error: process stream capture setup failed: " << log_error << '\n';
        return 2;
    }
    std::cout << "Process file log path=" << process_logger.log_path().string()
              << " run_id=" << (runtime_options.run_id.empty() ? "none" : runtime_options.run_id) << '\n';

    if (runtime_options.role == RuntimeRole::kNone && !runtime_options.force_cli) {
        std::string gui_error;
        if (redclaw::ui::launch_gui_shell(argc, argv, &process_logger, &gui_error)) {
            return 0;
        }

        if (!gui_error.empty()) {
            std::cerr << "GUI shell launch skipped: " << gui_error << '\n';
        }
        if (runtime_options.enable_debug_control) {
            return 2;
        }
    }

    bootstrap_session_security_policy_wiring();

    const std::array<std::string_view, 10> modules = {
        redclaw::core::module_name(),
        redclaw::protocol::module_name(),
        redclaw::net::module_name(),
        redclaw::security::module_name(),
        redclaw::session::module_name(),
        redclaw::capture::module_name(),
        redclaw::render::module_name(),
        redclaw::input::module_name(),
        redclaw::service::module_name(),
        redclaw::diag::module_name(),
    };

    std::cout << "RedClawDesktop module skeleton loaded:" << '\n';
    for (const auto module : modules) {
        std::cout << "- " << module << '\n';
    }

    if (runtime_options.role != RuntimeRole::kNone) {
        const int result = run_runtime_mode(runtime_options, &process_logger, local_control_output);
        return local_control_output.failed() ? 1 : result;
    }

    return 0;
}
