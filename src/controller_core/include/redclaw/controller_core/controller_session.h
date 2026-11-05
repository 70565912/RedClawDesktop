#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "redclaw/net/video_frame_transport.h"

namespace redclaw::controller_core {

// Bumped when the JNI/embedding contract of this header changes.
inline constexpr std::uint32_t kControllerCoreApiVersion = 1;

// Wire codec ids carried by EncodedVideoFragmentView::codec.
inline constexpr std::uint8_t kEncodedVideoCodecH264 = 1;
inline constexpr std::uint8_t kEncodedVideoCodecHevc = 2;

struct ControllerSessionConfig {
    // Host machine code (DHT session code): 8 characters, A-Z and 0-9.
    std::string machine_code;
    // Exact UTF-8 connection password. Consumed by start(); never logged.
    std::string password;
    std::vector<std::string> ice_servers;
    // Empty selects the shared default bootstrap list.
    std::vector<std::string> dht_bootstrap_nodes;
    // Writable app-private directory for the local DHT file mailbox.
    std::filesystem::path mailbox_directory;
    std::string network_bind_address;
    std::uint16_t ice_udp_port = 55000;
    std::uint16_t dht_listen_port = 0;
    std::uint32_t dht_poll_interval_ms = 1000;
    std::uint32_t dht_publish_retry_ms = 1000;
    bool enable_ipv6 = true;
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
};

enum class ControllerSessionPhase {
    kIdle,
    kRequesting,
    kOfferApplied,
    kAnswerPublished,
    kIceConnecting,
    kAuthenticating,
    kStreaming,
    kFailed,
    kStopped,
};

struct ControllerSessionStats {
    std::uint64_t dht_publish_success = 0;
    std::uint64_t dht_fetch_attempts = 0;
    std::uint64_t dht_fetch_hits = 0;
    std::uint64_t dht_remote_revision = 0;
    std::uint64_t remote_candidates_applied = 0;
    std::uint64_t local_candidates = 0;
    std::uint64_t media_fragments = 0;
    std::uint64_t media_fragment_parse_failures = 0;
    std::uint64_t frames_reassembled = 0;
    std::uint64_t keyframes_reassembled = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t keyframe_requests_sent = 0;
    std::uint64_t latest_frame_id = 0;
    std::uint8_t latest_codec = 0;
    std::uint32_t latest_width = 0;
    std::uint32_t latest_height = 0;
};

// Callbacks run on transport or session worker threads. Implementations must
// not call back into ControllerSession::stop() from a callback.
class IControllerSessionObserver {
public:
    virtual ~IControllerSessionObserver() = default;
    virtual void on_phase(ControllerSessionPhase phase, std::string_view detail) = 0;
    // One complete access unit. The frame is moved in; keep or drop it, but do
    // not block: the media DataChannel thread delivers it.
    virtual void on_encoded_frame(net::ReassembledEncodedVideoFrame&& frame) = 0;
    // Redacted diagnostic line (never contains the password).
    virtual void on_log(std::string_view line) { (void)line; }
};

[[nodiscard]] bool validate_controller_session_config(
    const ControllerSessionConfig& config,
    std::string* error = nullptr);

[[nodiscard]] std::string_view controller_session_phase_to_string(
    ControllerSessionPhase phase) noexcept;

// Portable Controller session: DHT rendezvous request/offer/answer, ICE and
// DataChannels, connection authentication and encoded-video reassembly.
// One instance owns one connection attempt; recovery after ICE failure is not
// implemented yet (the Windows runtime keeps that logic in run_runtime_mode).
class ControllerSession final {
public:
    explicit ControllerSession(IControllerSessionObserver& observer);
    ~ControllerSession();
    ControllerSession(const ControllerSession&) = delete;
    ControllerSession& operator=(const ControllerSession&) = delete;

    [[nodiscard]] bool start(ControllerSessionConfig config, std::string* error = nullptr);
    void stop();

    // Viewport hint in physical pixels; sent once the control channel is open.
    bool set_viewport(std::uint32_t width, std::uint32_t height);
    // Asks the Host for a fresh IDR, e.g. after a decoder reset.
    bool request_keyframe(std::string_view reason);

    [[nodiscard]] ControllerSessionPhase phase() const;
    [[nodiscard]] ControllerSessionStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::controller_core
