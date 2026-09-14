#include "runtime_options_detail.h"
#include "redclaw/service/dht_rendezvous.h"
#include <algorithm>
#include <cctype>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace redclaw::runtime {
namespace {
constexpr std::size_t kMinSealedPassphraseLength = 16;
constexpr std::uint32_t kDesktopStreamMinVideoMaxWidth = 320;
constexpr std::uint32_t kDesktopStreamMaxVideoMaxWidth = 7680;
constexpr std::uint32_t kDhtMinPollIntervalMs = 250;
constexpr std::uint32_t kDhtMaxPollIntervalMs = 60000;
constexpr std::uint32_t kDhtMinPublishRetryMs = 1000;
constexpr std::uint32_t kDhtMaxPublishRetryMs = 120000;

bool validate_session_code(std::string_view code, std::string* error) {
    if (code.empty()) {
        if (error != nullptr) {
            *error = "session code is empty";
        }
        return false;
    }

    if (code.size() != 8) {
        if (error != nullptr) {
            *error = "session code must be exactly 8 characters";
        }
        return false;
    }

    for (const char ch : code) {
        const bool is_upper = ch >= 'A' && ch <= 'Z';
        const bool is_digit = ch >= '0' && ch <= '9';
        if (!is_upper && !is_digit) {
            if (error != nullptr) {
                *error = "session code must use uppercase letters and digits only";
            }
            return false;
        }
    }

    return true;
}

bool validate_run_id(std::string_view value, std::string* error) {
    if (value.empty() || value.size() > 64) {
        *error = "--run-id must contain 1..64 characters";
        return false;
    }
    for (const char ch : value) {
        const bool allowed = std::isalnum(static_cast<unsigned char>(ch)) != 0
            || ch == '-' || ch == '_' || ch == '.';
        if (!allowed) {
            *error = "--run-id may contain only letters, digits, dot, dash, and underscore";
            return false;
        }
    }
    return true;
}

bool is_valid_ipv4_address(std::string_view value) {
    if (value.empty()) {
        return false;
    }

    std::size_t octet_count = 0;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('.', begin);
        const std::string_view octet = value.substr(
            begin,
            end == std::string_view::npos ? value.size() - begin : end - begin);
        if (octet.empty() || octet.size() > 3) {
            return false;
        }
        std::uint32_t parsed = 0;
        if (!parse_uint32(octet, &parsed) || parsed > 255) {
            return false;
        }
        ++octet_count;
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return octet_count == 4;
}

#ifdef _WIN32
bool is_ipv4_address_assigned_locally(std::string_view value, std::string* error) {
    WSADATA wsadata{};
    const int startup_result = WSAStartup(MAKEWORD(2, 2), &wsadata);
    if (startup_result != 0) {
        *error = "failed to validate --network-bind-address, WSAStartup error="
            + std::to_string(startup_result);
        return false;
    }

    const SOCKET socket_handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        const int socket_error = WSAGetLastError();
        WSACleanup();
        *error = "failed to validate --network-bind-address, socket error="
            + std::to_string(socket_error);
        return false;
    }

    const std::string address_text(value);
    sockaddr_in local_address{};
    local_address.sin_family = AF_INET;
    local_address.sin_port = 0;
    if (InetPtonA(AF_INET, address_text.c_str(), &local_address.sin_addr) != 1
        || local_address.sin_addr.s_addr == htonl(INADDR_ANY)) {
        closesocket(socket_handle);
        WSACleanup();
        *error = "--network-bind-address must identify one assigned local IPv4 address";
        return false;
    }

    const int bind_result = ::bind(
        socket_handle,
        reinterpret_cast<const sockaddr*>(&local_address),
        sizeof(local_address));
    const int bind_error = bind_result == SOCKET_ERROR ? WSAGetLastError() : 0;
    closesocket(socket_handle);
    WSACleanup();
    if (bind_result == SOCKET_ERROR) {
        *error = "--network-bind-address is not assigned to this machine, WSA error="
            + std::to_string(bind_error);
        return false;
    }
    return true;
}

bool is_ipv6_address_assigned_locally(std::string_view value, std::string* error) {
    WSADATA wsadata{};
    const int startup_result = WSAStartup(MAKEWORD(2, 2), &wsadata);
    if (startup_result != 0) {
        *error = "failed to validate --network-bind-ipv6-address, WSAStartup error="
            + std::to_string(startup_result);
        return false;
    }
    const SOCKET socket_handle = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        const int socket_error = WSAGetLastError();
        WSACleanup();
        *error = "failed to validate --network-bind-ipv6-address, socket error="
            + std::to_string(socket_error);
        return false;
    }
    const std::string address_text(value);
    sockaddr_in6 local_address{};
    local_address.sin6_family = AF_INET6;
    if (InetPtonA(AF_INET6, address_text.c_str(), &local_address.sin6_addr) != 1
        || IN6_IS_ADDR_UNSPECIFIED(&local_address.sin6_addr)) {
        closesocket(socket_handle);
        WSACleanup();
        *error = "--network-bind-ipv6-address must identify one assigned local IPv6 address";
        return false;
    }
    const int bind_result = ::bind(
        socket_handle,
        reinterpret_cast<const sockaddr*>(&local_address),
        sizeof(local_address));
    const int bind_error = bind_result == SOCKET_ERROR ? WSAGetLastError() : 0;
    closesocket(socket_handle);
    WSACleanup();
    if (bind_result == SOCKET_ERROR) {
        *error = "--network-bind-ipv6-address is not assigned to this machine, WSA error="
            + std::to_string(bind_error);
        return false;
    }
    return true;
}
#endif
}  // namespace

bool validate_runtime_options(const RuntimeOptions& options, std::string* error) {
    if (options.gui_runtime_stdio && (options.role == RuntimeRole::kNone || !options.force_cli)) {
        *error = "--gui-runtime-stdio requires a CLI runtime role";
        return false;
    }
    if (!options.run_id.empty() && !validate_run_id(options.run_id, error)) {
        return false;
    }
    if (options.ice_udp_port == 0) {
        *error = "--ice-udp-port must be within [1, 65535]";
        return false;
    }

    if (options.debug_control_name.empty() || options.debug_control_name.size() > 128) {
        *error = "--debug-control-name must contain 1..128 characters";
        return false;
    }
    if (options.agent_control_name.empty() || options.agent_control_name.size() > 128) {
        *error = "--agent-control-name must contain 1..128 characters";
        return false;
    }
    if (options.coordination_journal_path.size() > 1024) {
        *error = "--coordination-journal-path must contain at most 1024 characters";
        return false;
    }
    if (!options.coordination_git_sha.empty()
        && (options.coordination_git_sha.size() < 7
            || options.coordination_git_sha.size() > 64
            || !std::all_of(
                options.coordination_git_sha.begin(), options.coordination_git_sha.end(),
                [](char ch) {
                    return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
                }))) {
        *error = "--coordination-git-sha must contain 7..64 hexadecimal characters";
        return false;
    }
    if (!options.network_bind_address.empty()
        && !is_valid_ipv4_address(options.network_bind_address)) {
        *error = "--network-bind-address must be an IPv4 address";
        return false;
    }
#ifdef _WIN32
    if (!options.network_bind_address.empty()
        && !is_ipv4_address_assigned_locally(options.network_bind_address, error)) {
        return false;
    }
    if (!options.network_bind_ipv6_address.empty()
        && !is_ipv6_address_assigned_locally(options.network_bind_ipv6_address, error)) {
        return false;
    }
#endif
    if (!options.network_bind_ipv6_address.empty()
        && options.network_bind_address.empty()) {
        *error = "--network-bind-ipv6-address requires --network-bind-address from the same adapter";
        return false;
    }
    if (options.signal_transport != "rendezvous"
        && options.signal_transport != "dht"
        && options.role == RuntimeRole::kController
        && options.target_port > 0
        && options.target_host.empty()) {
        *error = "--target-port requires --target-host in controller mode";
        return false;
    }

    if (options.signal_transport != "file"
        && options.signal_transport != "event-log"
        && options.signal_transport != "tcp"
        && options.signal_transport != "sealed-file"
        && options.signal_transport != "rendezvous"
        && options.signal_transport != "dht") {
        *error = "invalid --signal-transport value: " + options.signal_transport;
        return false;
    }

    if (options.signal_transport == "tcp"
        && options.role == RuntimeRole::kController
        && options.target_host.empty()) {
        *error = "--signal-transport tcp in controller mode requires --target-host";
        return false;
    }

    if (options.signal_transport == "sealed-file" && options.signal_passphrase.empty()) {
        *error = "--signal-transport sealed-file requires --signal-passphrase";
        return false;
    }

    if (options.signal_transport == "sealed-file"
        && options.signal_passphrase.size() < kMinSealedPassphraseLength) {
        *error = "--signal-passphrase must be at least 16 characters for sealed-file";
        return false;
    }

    if (options.signal_transport == "rendezvous") {
        if (options.rendezvous_url.empty()) {
            *error = "--signal-transport rendezvous requires --rendezvous-url";
            return false;
        }

        if (options.session_code.empty()) {
            *error = "--signal-transport rendezvous requires --session-code";
            return false;
        }

        if (!validate_session_code(options.session_code, error)) {
            return false;
        }
    }

    if (options.signal_transport == "dht") {
        if (options.session_code.empty()) {
            *error = "--signal-transport dht requires --session-code";
            return false;
        }

        if (!validate_session_code(options.session_code, error)) {
            return false;
        }

        if (options.dht_poll_interval_ms < kDhtMinPollIntervalMs
            || options.dht_poll_interval_ms > kDhtMaxPollIntervalMs) {
            *error = "--dht-poll-interval-ms must be within [250, 60000]";
            return false;
        }

        if (options.dht_publish_retry_ms < kDhtMinPublishRetryMs
            || options.dht_publish_retry_ms > kDhtMaxPublishRetryMs) {
            *error = "--dht-publish-retry-ms must be within [1000, 120000]";
            return false;
        }

        redclaw::service::LocalHelperRole helper_role = redclaw::service::LocalHelperRole::kOff;
        if (!redclaw::service::parse_local_helper_role(options.helper_role, &helper_role, error)) {
            return false;
        }
    }

    if (options.stream_preview_width < 16 || options.stream_preview_width > 640) {
        *error = "--stream-preview-width must be within [16, 640]";
        return false;
    }

    if (options.stream_qa_native_size && (!options.stream_require_capture || options.stream_video_max_width != 0)) {
        *error = "--stream-qa-native-size requires real capture and no explicit width cap";
        return false;
    }
    if (options.stream_video_max_width != 0
        && (options.stream_video_max_width < kDesktopStreamMinVideoMaxWidth
            || options.stream_video_max_width > kDesktopStreamMaxVideoMaxWidth)) {
        *error = "--stream-video-max-width must be 0 or within [320, 7680]";
        return false;
    }

    if (!options.stream_frame_pipe.empty()) {
        if (options.role != RuntimeRole::kController) {
            *error = "--stream-frame-pipe is only valid in controller role";
            return false;
        }
        if (!options.stream_smoke) {
            *error = "--stream-frame-pipe requires --stream-smoke";
            return false;
        }
    }

    if (!options.stream_navigation_pipe.empty()) {
        if (options.role != RuntimeRole::kController) {
            *error = "--stream-navigation-pipe is only valid in controller role";
            return false;
        }
        if (!options.stream_smoke) {
            *error = "--stream-navigation-pipe requires --stream-smoke";
            return false;
        }
    }

    if (options.stream_qa_drop_one_media_fragment) {
        // The GUI parent is parsed before --gui-role is applied, so kNone is
        // permitted here and is validated by the GUI parser. The spawned
        // runtime child receives the concrete --role controller contract.
        if (options.role != RuntimeRole::kNone
            && options.role != RuntimeRole::kController) {
            *error = "--stream-qa-drop-one-media-fragment is only valid in controller role";
            return false;
        }
        if (!options.stream_smoke) {
            *error = "--stream-qa-drop-one-media-fragment requires --stream-smoke";
            return false;
        }
    }
    const bool valid_qa_feedback_class =
        options.stream_qa_incomplete_feedback_class == "fresh"
        || options.stream_qa_incomplete_feedback_class == "delayed"
        || options.stream_qa_incomplete_feedback_class == "expired"
        || options.stream_qa_incomplete_feedback_class == "stale-revision";
    if (!valid_qa_feedback_class) {
        *error = "--stream-qa-incomplete-feedback-class must be fresh, delayed, expired, or stale-revision";
        return false;
    }
    if (options.stream_qa_incomplete_feedback_class != "fresh"
        && !options.stream_qa_drop_one_media_fragment) {
        *error = "non-fresh --stream-qa-incomplete-feedback-class requires --stream-qa-drop-one-media-fragment";
        return false;
    }
    if (!options.stream_qa_force_required_channel_close.empty()) {
        if (options.stream_qa_force_required_channel_close != "media"
            && options.stream_qa_force_required_channel_close != "control") {
            *error = "--stream-qa-force-required-channel-close must be media or control";
            return false;
        }
        if (options.role != RuntimeRole::kNone
            && options.role != RuntimeRole::kController) {
            *error = "--stream-qa-force-required-channel-close is only valid in controller role";
            return false;
        }
        if (!options.stream_smoke) {
            *error = "--stream-qa-force-required-channel-close requires --stream-smoke";
            return false;
        }
        if (options.signal_transport != "dht") {
            *error = "--stream-qa-force-required-channel-close requires --signal-transport dht";
            return false;
        }
    }

    if (options.input_diagnostics) {
#ifdef NDEBUG
        *error = "--input-diagnostics is Debug-only";
        return false;
#else
        if (!options.stream_smoke || options.log_dir.empty()) {
            *error = "--input-diagnostics requires stream smoke and an explicit log directory";
            return false;
        }
#endif
    }
    if (options.agent_qa_fixture_provider) {
#ifdef NDEBUG
        *error = "--agent-qa-fixture-provider is Debug-only";
        return false;
#else
        if (!options.stream_smoke) {
            *error = "--agent-qa-fixture-provider requires explicit stream smoke";
            return false;
        }
#endif
    }
    if (options.agent_qa_force_channel_close_after_event) {
#ifdef NDEBUG
        *error = "--agent-qa-force-channel-close-after-event is Debug-only";
        return false;
#else
        if (!options.stream_smoke || options.signal_transport != "dht") {
            *error = "--agent-qa-force-channel-close-after-event requires DHT stream smoke";
            return false;
        }
#endif
    }

    return true;
}

}  // namespace redclaw::runtime
