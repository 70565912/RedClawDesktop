#include "redclaw/controller_core/controller_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <utility>

#include "redclaw/net/net_module.h"
#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/security/connection_auth.h"
#include "redclaw/service/connection_negotiation.h"
#include "redclaw/service/dht_publication_transaction.h"
#include "redclaw/service/dht_rendezvous.h"

namespace redclaw::controller_core {
namespace {

// Same cadence constants as the Windows runtime loop (src/main.cpp).
constexpr std::uint64_t kLoopIntervalMs = 20;
constexpr std::uint64_t kDhtAlertPumpIntervalMs = 250;
constexpr std::uint64_t kDhtPublishRefreshMs = 30000;
constexpr std::uint64_t kDhtStaleRemoteFetchBackoffMs = 5000;
constexpr std::uint32_t kDhtInitialCheckWindowMs = 120000;
constexpr std::uint64_t kKeyframeRequestMinIntervalMs = 500;
constexpr std::uint32_t kMinViewportPixels = 64;
constexpr std::uint32_t kMaxViewportPixels = 16384;

// Only capabilities this core actually handles. Terminal, transfer, clipboard,
// audio and Agent stay at version zero so the Host never opens them.
constexpr std::string_view kControllerCapabilities =
    "viewport,media-transport-feedback,keyframe-recovery";

std::uint64_t now_unix_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::uint64_t now_steady_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::optional<std::pair<std::string, std::string>> split_candidate_line(std::string_view line) {
    const std::size_t split = line.find('\t');
    if (split == std::string_view::npos || split == 0 || split + 1 >= line.size()) {
        return std::nullopt;
    }
    return std::make_pair(std::string(line.substr(0, split)), std::string(line.substr(split + 1)));
}

void assign(std::string* target, std::string value) {
    if (target != nullptr) {
        *target = std::move(value);
    }
}

}  // namespace

bool validate_controller_session_config(const ControllerSessionConfig& config, std::string* error) {
    if (config.machine_code.size() != 8
        || !std::all_of(config.machine_code.begin(), config.machine_code.end(), [](char ch) {
               return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
           })) {
        assign(error, "machine code must be exactly 8 characters of A-Z and 0-9");
        return false;
    }
    if (config.password.empty() || config.password.size() > security::kMaxConnectionPasswordBytes) {
        assign(error, "connection password is empty or too long");
        return false;
    }
    if (config.ice_udp_port == 0) {
        assign(error, "ICE UDP port must be between 1 and 65535");
        return false;
    }
    if (config.mailbox_directory.empty()) {
        assign(error, "DHT mailbox directory is required");
        return false;
    }
    if (config.dht_poll_interval_ms < 250 || config.dht_poll_interval_ms > 60000
        || config.dht_publish_retry_ms < 1000 || config.dht_publish_retry_ms > 120000) {
        assign(error, "DHT poll/publish intervals are out of range");
        return false;
    }
    assign(error, {});
    return true;
}

std::string_view controller_session_phase_to_string(ControllerSessionPhase phase) noexcept {
    switch (phase) {
    case ControllerSessionPhase::kIdle: return "idle";
    case ControllerSessionPhase::kRequesting: return "requesting";
    case ControllerSessionPhase::kOfferApplied: return "offer_applied";
    case ControllerSessionPhase::kAnswerPublished: return "answer_published";
    case ControllerSessionPhase::kIceConnecting: return "ice_connecting";
    case ControllerSessionPhase::kAuthenticating: return "authenticating";
    case ControllerSessionPhase::kStreaming: return "streaming";
    case ControllerSessionPhase::kFailed: return "failed";
    case ControllerSessionPhase::kStopped: return "stopped";
    }
    return "unknown";
}

class ControllerSession::Impl {
public:
    explicit Impl(IControllerSessionObserver& observer) : observer_(observer) {}
    ~Impl() { stop(); }

    bool start(ControllerSessionConfig config, std::string* error);
    void stop();
    bool set_viewport(std::uint32_t width, std::uint32_t height);
    bool request_keyframe(std::string_view reason);
    ControllerSessionPhase phase() const {
        std::lock_guard lock(mutex_);
        return phase_;
    }
    ControllerSessionStats stats() const {
        std::lock_guard lock(mutex_);
        return stats_;
    }

private:
    void register_transport_callbacks();
    void run();
    void pump_dht(std::uint64_t now_ms);
    void publish_dht(std::uint64_t now_ms);
    void fetch_dht(std::uint64_t now_ms);
    void apply_remote_candidates(const std::vector<std::string>& candidate_lines);
    bool ensure_gathering_started(std::string* error);
    void observe_authentication();
    void on_control_message(std::span<const std::uint8_t> bytes);
    void on_media_fragment(std::span<const std::uint8_t> bytes);
    bool send_control(protocol::StreamControlMessageV1 message, std::string* error = nullptr);
    void send_viewport_if_ready();
    void flush_transport_feedback(std::uint64_t receiver_steady_us);
    void set_phase(ControllerSessionPhase phase, std::string detail = {});
    void fail(std::string reason);
    void log(std::string line) { observer_.on_log(line); }

    IControllerSessionObserver& observer_;
    ControllerSessionConfig config_;

    std::unique_ptr<net::IceConnectivityWrapper> ice_;
    std::unique_ptr<service::IDhtRendezvousStore> store_;
#if defined(REDCLAW_ENABLE_DHT_BACKENDS)
    service::LibtorrentDhtRendezvousStore* libtorrent_store_ = nullptr;
#endif
    std::unique_ptr<service::DhtRendezvousClient> dht_client_;
    service::DhtRendezvousConfig dht_config_;
    service::ConnectionNegotiationCoordinator negotiation_{service::ConnectionNegotiationRole::kController};
    std::optional<service::DhtPublicationTransaction> publication_;
    std::string publisher_instance_id_;
    std::string request_payload_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex wake_mutex_;
    std::condition_variable wake_;

    // Guards everything below; never held across IceConnectivityWrapper calls
    // or observer callbacks because the transport may call back synchronously.
    mutable std::mutex mutex_;
    ControllerSessionPhase phase_ = ControllerSessionPhase::kIdle;
    ControllerSessionStats stats_;
    bool signal_dirty_ = false;
    bool gathering_started_ = false;
    bool gathering_complete_ = false;
    bool transport_failed_ = false;
    bool connected_ = false;
    bool remote_description_applied_ = false;
    bool initial_snapshot_published_ = false;
    bool media_open_ = false;
    bool control_open_ = false;
    std::string local_description_sdp_;
    std::vector<std::string> local_candidate_lines_;
    std::vector<std::string> last_published_candidate_lines_;
    std::set<std::string> applied_remote_candidates_;
    std::set<std::string> pending_remote_candidates_;
    std::uint64_t remote_revision_ = 0;
    std::uint64_t local_revision_ = 0;
    std::uint64_t next_alert_pump_ms_ = 0;
    std::uint64_t next_publish_retry_ms_ = 0;
    std::uint64_t next_publish_refresh_ms_ = 0;
    std::uint64_t next_fetch_ms_ = 0;
    std::uint64_t last_keyframe_request_ms_ = 0;
    std::uint32_t viewport_width_ = 0;
    std::uint32_t viewport_height_ = 0;
    std::uint64_t viewport_transaction_ = 0;
    bool viewport_sent_ = false;
    std::string last_dht_error_;
    std::string last_logged_publish_error_;

    std::mutex control_send_mutex_;
    std::string control_epoch_;
    std::uint64_t control_message_id_ = 0;
    protocol::StreamControlEpochGuardV1 remote_control_guard_;

    std::mutex media_mutex_;
    net::EncodedVideoFrameReassembler reassembler_;
    net::MediaTransportFeedbackRecorder feedback_recorder_;
};

bool ControllerSession::Impl::start(ControllerSessionConfig config, std::string* error) {
    if (running_.load()) {
        assign(error, "controller session already started");
        return false;
    }
    if (!validate_controller_session_config(config, error)) {
        security::erase_secret(config.password);
        return false;
    }

    ice_ = std::make_unique<net::IceConnectivityWrapper>();
    security::ConnectionCredential credential;
    credential.host = false;
    credential.secret = std::move(config.password);
    security::erase_secret(config.password);
    if (!ice_->requireConnectionAuthentication(std::move(credential))) {
        assign(error, "connection_credentials_unavailable");
        return false;
    }
    std::string port_error;
    if (!ice_->reserveFixedUdpPort(config.ice_udp_port, config.network_bind_address, &port_error)) {
        assign(error, "ice_udp_port_unavailable: " + port_error);
        return false;
    }

    std::error_code mkdir_error;
    std::filesystem::create_directories(config.mailbox_directory, mkdir_error);
    auto secondary = std::make_unique<service::FileBackedDhtRendezvousStore>(config.mailbox_directory);
#if defined(REDCLAW_ENABLE_DHT_BACKENDS)
    service::LibtorrentDhtRendezvousStoreOptions store_options;
    store_options.bootstrap_nodes = config.dht_bootstrap_nodes;
    store_options.listen_address = config.network_bind_address;
    store_options.listen_port = config.dht_listen_port;
    store_options.enable_ipv6 = config.enable_ipv6;
    std::unique_ptr<service::LibtorrentDhtRendezvousStore> primary;
    try {
        primary = std::make_unique<service::LibtorrentDhtRendezvousStore>(store_options);
    } catch (const std::exception&) {
        // Native startup errors may embed local addresses; keep them out of logs.
        assign(error, "dht_listener_initialization_failed");
        return false;
    }
    libtorrent_store_ = primary.get();
    store_ = std::make_unique<service::IndirectDhtRendezvousStore>(std::move(primary), std::move(secondary));
    log("controller_core dht backend=libtorrent-mainline+file-mailbox");
#else
    store_ = std::move(secondary);
    log("controller_core dht backend=file-mailbox-only reason=build_without_dht_backends");
#endif
    dht_client_ = std::make_unique<service::DhtRendezvousClient>(*store_);
    dht_config_.session_code = config.machine_code;
    dht_config_.pairing_secret = config.machine_code;
    dht_config_.now_unix = now_unix_ms() / 1000ULL;
    dht_config_.ttl_seconds = service::kDhtRecordTtlSeconds;

    std::string instance_error;
    const auto instance_id = service::make_dht_publisher_instance_id(&instance_error);
    if (!instance_id.has_value()) {
        assign(error, "dht_publisher_instance_failed: " + instance_error);
        return false;
    }
    publisher_instance_id_ = *instance_id;
    publication_.emplace(service::make_dht_publish_revision_floor(now_unix_ms()));
    request_payload_ = "controller-request-" + publisher_instance_id_ + "-" + std::to_string(now_unix_ms());
    if (!negotiation_.begin_controller_request(service::derive_dht_description_tag(request_payload_)).accepted()) {
        assign(error, "controller_request_initialization_failed");
        return false;
    }

    control_epoch_ = "android-controller-" + std::to_string(now_unix_ms());
    {
        std::lock_guard lock(mutex_);
        config_ = std::move(config);
        viewport_width_ = config_.viewport_width;
        viewport_height_ = config_.viewport_height;
        signal_dirty_ = true;
        phase_ = ControllerSessionPhase::kRequesting;
    }
    register_transport_callbacks();
    log("controller_core started topic=" + service::derive_dht_rendezvous_topic_hex(
        dht_config_.session_code, dht_config_.pairing_secret));
    observer_.on_phase(ControllerSessionPhase::kRequesting, "dht_connect_request_pending");

    running_.store(true);
    worker_ = std::thread([this] { run(); });
    assign(error, {});
    return true;
}

void ControllerSession::Impl::stop() {
    const bool was_running = running_.exchange(false);
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (ice_) {
        (void)ice_->shutdown();
    }
    ice_.reset();
    dht_client_.reset();
#if defined(REDCLAW_ENABLE_DHT_BACKENDS)
    libtorrent_store_ = nullptr;
#endif
    store_.reset();
    if (was_running) {
        set_phase(ControllerSessionPhase::kStopped, "stopped_by_owner");
    }
}

void ControllerSession::Impl::register_transport_callbacks() {
    ice_->onConnectionStateChanged([this](net::IceConnectionState state) {
        std::optional<ControllerSessionPhase> next;
        {
            std::lock_guard lock(mutex_);
            if (state == net::IceConnectionState::kConnecting) {
                (void)negotiation_.mark_ice_connecting();
                next = ControllerSessionPhase::kIceConnecting;
            } else if (state == net::IceConnectionState::kConnected) {
                connected_ = true;
                (void)negotiation_.mark_connected();
                if (!(media_open_ && control_open_)) next = ControllerSessionPhase::kAuthenticating;
            } else if (state == net::IceConnectionState::kDisconnected
                       || state == net::IceConnectionState::kFailed
                       || state == net::IceConnectionState::kClosed) {
                transport_failed_ = true;
                (void)negotiation_.mark_failed();
            }
        }
        if (state == net::IceConnectionState::kFailed || state == net::IceConnectionState::kDisconnected) {
            fail("ice_transport_failed_reconnect_not_implemented");
        } else if (next.has_value()) {
            set_phase(*next);
        }
    });
    ice_->onGatheringStateChanged([this](net::IceGatheringState state) {
        std::lock_guard lock(mutex_);
        gathering_complete_ = state == net::IceGatheringState::kComplete;
        if (gathering_complete_ && !local_description_sdp_.empty()) signal_dirty_ = true;
    });
    ice_->onLocalCandidate([this](const std::string& candidate, const std::string& mid) {
        std::lock_guard lock(mutex_);
        local_candidate_lines_.push_back(mid + "\t" + candidate);
        stats_.local_candidates = local_candidate_lines_.size();
        if (!local_description_sdp_.empty()) signal_dirty_ = true;
    });
    ice_->onLocalDescription([this](const std::string& sdp, bool is_offer) {
        if (is_offer) return;
        bool bound = false;
        {
            std::lock_guard lock(mutex_);
            if (!local_description_sdp_.empty()) return;
            bound = negotiation_.mark_local_answer_ready(service::derive_dht_description_tag(sdp));
            if (bound) {
                local_description_sdp_ = sdp;
                signal_dirty_ = true;
            }
        }
        if (!bound) fail("controller_answer_not_bound_to_generation");
        wake_.notify_all();
    });
    ice_->onDataChannelOpen([this](net::DataChannelKind kind) {
        bool streaming = false;
        {
            std::lock_guard lock(mutex_);
            if (kind == net::DataChannelKind::kMedia) media_open_ = true;
            if (kind == net::DataChannelKind::kControl) control_open_ = true;
            streaming = media_open_ && control_open_;
        }
        if (kind == net::DataChannelKind::kControl) {
            auto hello = protocol::StreamControlMessageV1{};
            hello.type = protocol::StreamControlMessageTypeV1::kHello;
            hello.payload = std::string(kControllerCapabilities);
            std::string send_error;
            if (!send_control(std::move(hello), &send_error)) log("controller_core hello send failed: " + send_error);
            send_viewport_if_ready();
        }
        if (kind == net::DataChannelKind::kMedia) {
            std::lock_guard media_lock(media_mutex_);
            reassembler_.reset();
            feedback_recorder_.reset();
        }
        if (streaming) set_phase(ControllerSessionPhase::kStreaming, "media_and_control_open");
    });
    ice_->onDataChannelClosed([this](net::DataChannelKind kind) {
        bool required = kind == net::DataChannelKind::kMedia || kind == net::DataChannelKind::kControl;
        {
            std::lock_guard lock(mutex_);
            if (kind == net::DataChannelKind::kMedia) media_open_ = false;
            if (kind == net::DataChannelKind::kControl) {
                control_open_ = false;
                viewport_sent_ = false;
            }
        }
        if (required && running_.load()) fail("required_channel_closed");
    });
    ice_->onDataChannelBinaryMessage([this](net::DataChannelKind kind, std::span<const std::uint8_t> message) {
        if (kind == net::DataChannelKind::kMedia) {
            on_media_fragment(message);
        } else if (kind == net::DataChannelKind::kControl) {
            on_control_message(message);
        }
    });
}

void ControllerSession::Impl::run() {
    while (running_.load()) {
        const std::uint64_t now_ms = now_steady_ms();
        pump_dht(now_ms);
        observe_authentication();
        {
            bool expired = false;
            {
                std::lock_guard media_lock(media_mutex_);
                if (reassembler_.expired(now_ms)) {
                    const auto summary = reassembler_.require_keyframe();
                    expired = true;
                    std::lock_guard lock(mutex_);
                    stats_.frames_dropped += summary.dropped_incomplete_frames;
                }
            }
            if (expired) (void)request_keyframe("reassembly_timeout");
        }
        std::unique_lock wake_lock(wake_mutex_);
        wake_.wait_for(wake_lock, std::chrono::milliseconds(kLoopIntervalMs), [this] { return !running_.load(); });
    }
}

void ControllerSession::Impl::observe_authentication() {
    if (!ice_) return;
    {
        std::lock_guard lock(mutex_);
        if (!connected_ || phase_ == ControllerSessionPhase::kFailed) return;
    }
    std::string auth_error;
    if (ice_->authenticationState(&auth_error) == security::ConnectionAuthState::kRejected) {
        fail("connection_auth_rejected:" + auth_error);
    }
}

void ControllerSession::Impl::pump_dht(std::uint64_t now_ms) {
    if (!dht_client_) return;
    dht_config_.now_unix = now_unix_ms() / 1000ULL;
#if defined(REDCLAW_ENABLE_DHT_BACKENDS)
    if (libtorrent_store_ != nullptr && now_ms >= next_alert_pump_ms_) {
        libtorrent_store_->pump_alerts(std::chrono::milliseconds(0));
        next_alert_pump_ms_ = now_ms + kDhtAlertPumpIntervalMs;
    }
#endif
    publish_dht(now_ms);
    std::vector<std::string> pending;
    {
        std::lock_guard lock(mutex_);
        if (remote_description_applied_) {
            pending.assign(pending_remote_candidates_.begin(), pending_remote_candidates_.end());
            pending_remote_candidates_.clear();
        }
    }
    if (!pending.empty()) apply_remote_candidates(pending);
    if (now_ms >= next_fetch_ms_) fetch_dht(now_ms);
}

void ControllerSession::Impl::publish_dht(std::uint64_t now_ms) {
    if (now_ms < next_publish_retry_ms_) return;

    service::DhtSignalSnapshot snapshot;
    snapshot.role = "controller";
    snapshot.publisher_instance_id = publisher_instance_id_;
    bool connect_request = false;
    bool refresh = false;
    bool initial = false;
    std::size_t local_total = 0;
    {
        std::lock_guard lock(mutex_);
        if (transport_failed_) return;
        snapshot.generation = negotiation_.generation();
        snapshot.connection_request_tag = negotiation_.connection_request_tag();
        const bool refresh_due = local_revision_ > 0 && now_ms >= next_publish_refresh_ms_;
        if (snapshot.generation == 0 && !snapshot.connection_request_tag.empty()) {
            if (!signal_dirty_ && !refresh_due) return;
            connect_request = true;
            refresh = !signal_dirty_;
            snapshot.description_type = "request";
            snapshot.description_sdp = request_payload_;
        } else if (!local_description_sdp_.empty() && snapshot.generation > 0
                   && (signal_dirty_ || (refresh_due && !connected_))) {
            refresh = !signal_dirty_;
            snapshot.description_type = "answer";
            snapshot.description_sdp = local_description_sdp_;
            snapshot.answered_description_tag = service::select_dht_answered_description_tag(
                "answer", negotiation_.offer_description_tag());
            local_total = local_candidate_lines_.size();
            std::vector<std::string> candidates;
            if (!initial_snapshot_published_) {
                initial = true;
                std::string plan_error;
                const auto plan = service::plan_dht_initial_snapshot(snapshot, local_candidate_lines_, &plan_error);
                if (plan.has_value()) candidates = plan->candidate_lines;
            } else {
                candidates = service::prioritize_dht_candidate_lines(local_candidate_lines_);
            }
            snapshot.candidate_lines =
                service::merge_dht_candidate_snapshots(last_published_candidate_lines_, candidates);
            snapshot.candidates_complete = gathering_complete_ && snapshot.candidate_lines.size() == local_total;
        } else {
            return;
        }
        signal_dirty_ = false;
    }

    const auto revision = publication_->prepare(
        snapshot, refresh, dht_config_.now_unix, now_steady_ms(), dht_config_.ttl_seconds);
    if (!revision.has_value()) {
        fail("dht_publication_revision_exhausted");
        return;
    }
    snapshot.revision = *revision;
    std::string publish_error;
    const auto result = dht_client_->publish_signal_snapshot(dht_config_, snapshot, &publish_error);
    (void)publication_->observe(snapshot.revision, result);

    std::optional<ControllerSessionPhase> next;
    bool publish_error_changed = false;
    {
        std::lock_guard lock(mutex_);
        if (!result) {
            if (!refresh) signal_dirty_ = true;
            next_publish_retry_ms_ = now_steady_ms()
                + (result.state == service::DhtPublishState::kPending ? kLoopIntervalMs : config_.dht_publish_retry_ms);
            publish_error_changed = publish_error != last_logged_publish_error_;
            last_logged_publish_error_ = publish_error;
            last_dht_error_ = publish_error;
        }
    }
    if (!result) {
        if (publish_error_changed) {
            log("controller_core dht publish not yet confirmed type=" + snapshot.description_type
                + " error=" + (publish_error.empty() ? std::string("pending") : publish_error));
        }
        return;
    }
    {
        std::lock_guard lock(mutex_);
        ++stats_.dht_publish_success;
        last_logged_publish_error_.clear();
        local_revision_ = snapshot.revision;
        next_publish_retry_ms_ = 0;
        next_publish_refresh_ms_ = now_steady_ms() + kDhtPublishRefreshMs;
        if (connect_request) {
            if (negotiation_.phase() == service::ConnectionNegotiationPhase::kRequestReady) {
                (void)negotiation_.mark_controller_request_published();
            }
        } else {
            last_published_candidate_lines_ = snapshot.candidate_lines;
            if (initial) initial_snapshot_published_ = true;
            if (snapshot.candidate_lines.size() < local_total) signal_dirty_ = true;
            if (!negotiation_.local_answer_published() && negotiation_.mark_local_answer_published()) {
                next = ControllerSessionPhase::kAnswerPublished;
            }
        }
    }
    log("controller_core dht " + std::string(refresh ? "refreshed" : "published")
        + " type=" + snapshot.description_type
        + " revision=" + std::to_string(snapshot.revision)
        + " generation=" + std::to_string(snapshot.generation)
        + " candidates=" + std::to_string(snapshot.candidate_lines.size())
        + " complete=" + (snapshot.candidates_complete ? "true" : "false"));
    if (next.has_value()) set_phase(*next, "answer_published");
}

void ControllerSession::Impl::fetch_dht(std::uint64_t now_ms) {
    std::uint64_t after_revision = 0;
    {
        std::lock_guard lock(mutex_);
        ++stats_.dht_fetch_attempts;
        after_revision = remote_revision_;
    }
    std::string fetch_error;
    const auto remote = dht_client_->fetch_signal_snapshot(dht_config_, "host", after_revision, &fetch_error);
    std::uint64_t next_delay = config_.dht_poll_interval_ms;
    if (!remote.has_value()) {
        std::lock_guard lock(mutex_);
        if (!fetch_error.empty()) last_dht_error_ = fetch_error;
        next_fetch_ms_ = now_ms + next_delay;
        return;
    }

    const std::string offer_tag = service::derive_dht_description_tag(remote->description_sdp);
    bool apply_offer = false;
    bool newer_generation = false;
    bool rejected = false;
    service::ConnectionNegotiationResult result;
    {
        std::lock_guard lock(mutex_);
        ++stats_.dht_fetch_hits;
        remote_revision_ = std::max(remote_revision_, remote->revision);
        stats_.dht_remote_revision = remote_revision_;
        if (remote->description_type != "offer") {
            rejected = true;
        } else if (!remote_description_applied_
                   && service::should_skip_completed_dht_host_offer_on_fresh_controller(
                       negotiation_.generation(), remote->acknowledged_answer_tag)) {
            rejected = true;
        } else {
            const auto adoption = service::decide_persistent_host_offer_adoption({
                .remote_description_ever_applied = remote_description_applied_,
                .last_applied_host_instance_id = {},
                .offered_host_instance_id = remote->publisher_instance_id,
                .exact_failed_offer = false,
                .completed_offer = false,
                .authenticated_once = false,
                .current_generation = negotiation_.generation(),
                .offered_generation = remote->generation,
            });
            result = negotiation_.observe_host_offer(
                remote->generation, remote->connection_request_tag, offer_tag, adoption);
            rejected = !result.accepted();
            newer_generation = result.accepted() && result.generation_changed && remote_description_applied_;
            apply_offer = result.accepted() && !remote_description_applied_;
        }
        next_fetch_ms_ = now_ms + (rejected ? std::max<std::uint64_t>(next_delay, kDhtStaleRemoteFetchBackoffMs) : next_delay);
    }
    if (rejected) {
        log("controller_core skipped host record revision=" + std::to_string(remote->revision)
            + " type=" + remote->description_type
            + " observation=" + std::string(service::connection_negotiation_observation_to_string(result.observation)));
        return;
    }
    if (newer_generation) {
        fail("host_generation_changed_reconnect_not_implemented");
        return;
    }

    if (apply_offer) {
        std::string apply_error;
        if (!ensure_gathering_started(&apply_error)
            || !ice_->applyRemoteDescription(remote->description_sdp, true, &apply_error, false)) {
            fail("apply_remote_offer_failed: " + apply_error);
            return;
        }
        {
            std::lock_guard lock(mutex_);
            remote_description_applied_ = true;
            (void)negotiation_.mark_remote_offer_applied();
        }
        log("controller_core host offer applied generation=" + std::to_string(remote->generation)
            + " candidates=" + std::to_string(remote->candidate_lines.size()));
        set_phase(ControllerSessionPhase::kOfferApplied, "host_offer_applied");
    }

    if (!remote->acknowledged_answer_tag.empty()) {
        std::lock_guard lock(mutex_);
        (void)negotiation_.observe_answer_applied(
            remote->generation, remote->connection_request_tag, offer_tag, remote->acknowledged_answer_tag);
    }
    apply_remote_candidates(remote->candidate_lines);

    if (apply_offer) {
        std::string answer_error;
        if (!ice_->emitLocalDescription(&answer_error)) {
            fail("emit_local_answer_failed: " + answer_error);
        }
    }
}

void ControllerSession::Impl::apply_remote_candidates(const std::vector<std::string>& candidate_lines) {
    for (const auto& line : candidate_lines) {
        {
            std::lock_guard lock(mutex_);
            if (transport_failed_ || applied_remote_candidates_.contains(line)) continue;
        }
        const auto parsed = split_candidate_line(line);
        if (!parsed.has_value()) continue;
        std::string candidate_error;
        const auto outcome = ice_->tryApplyRemoteCandidate(parsed->second, parsed->first, &candidate_error);
        std::lock_guard lock(mutex_);
        if (outcome == net::RemoteCandidateApplyOutcome::kApplied) {
            applied_remote_candidates_.insert(line);
            stats_.remote_candidates_applied = applied_remote_candidates_.size();
        } else if (outcome == net::RemoteCandidateApplyOutcome::kNotReady) {
            pending_remote_candidates_.insert(line);
        } else if (outcome != net::RemoteCandidateApplyOutcome::kInvalid) {
            transport_failed_ = true;
            (void)negotiation_.mark_failed();
            break;
        }
    }
}

bool ControllerSession::Impl::ensure_gathering_started(std::string* error) {
    {
        std::lock_guard lock(mutex_);
        if (gathering_started_) return true;
        gathering_started_ = true;
    }
    net::IceGatheringConfig gathering;
    gathering.initiate_offer = false;
    gathering.initial_check_window_ms = kDhtInitialCheckWindowMs;
    gathering.ice_servers = config_.ice_servers;
    gathering.bind_address = config_.network_bind_address;
    gathering.port_range_begin = config_.ice_udp_port;
    gathering.port_range_end = config_.ice_udp_port;
    return ice_->startGathering(gathering, error);
}

bool ControllerSession::Impl::send_control(protocol::StreamControlMessageV1 message, std::string* error) {
    if (!ice_) {
        assign(error, "transport not started");
        return false;
    }
    std::lock_guard send_lock(control_send_mutex_);
    message.session_epoch = control_epoch_;
    message.message_id = ++control_message_id_;
    message.sent_at_ms = now_unix_ms();
    if (!protocol::validate_stream_control_message_v1(message, error)) return false;
    const std::string serialized = protocol::serialize_stream_control_message_v1(message);
    if (serialized.empty()) {
        assign(error, "stream control serialization failed");
        return false;
    }
    return ice_->sendDataChannelBinaryMessage(
        net::DataChannelKind::kControl,
        {reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size()},
        error);
}

void ControllerSession::Impl::on_control_message(std::span<const std::uint8_t> bytes) {
    const auto parsed = protocol::parse_stream_control_message_v1(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!parsed.ok) {
        log("controller_core invalid control message rejected: " + parsed.error);
        return;
    }
    const auto& control = parsed.value;
    if (control.type == protocol::StreamControlMessageTypeV1::kConnectionAuth || !control.auth_step.empty()) return;
    {
        std::lock_guard lock(mutex_);
        std::string guard_error;
        if (!remote_control_guard_.accept(control, &guard_error)) return;
    }
    if (control.type == protocol::StreamControlMessageTypeV1::kHello) {
        protocol::StreamControlMessageV1 capabilities;
        capabilities.type = protocol::StreamControlMessageTypeV1::kCapabilities;
        capabilities.payload = std::string(kControllerCapabilities);
        (void)send_control(std::move(capabilities));
        log("controller_core host hello capabilities=" + control.payload);
    } else if (control.type == protocol::StreamControlMessageTypeV1::kPing) {
        protocol::StreamControlMessageV1 pong;
        pong.type = protocol::StreamControlMessageTypeV1::kPong;
        pong.log_cursor = control.log_cursor;
        (void)send_control(std::move(pong));
    }
}

void ControllerSession::Impl::on_media_fragment(std::span<const std::uint8_t> bytes) {
    net::EncodedVideoFragmentView fragment;
    std::string fragment_error;
    const std::uint64_t now_ms = now_steady_ms();
    std::optional<net::ReassembledEncodedVideoFrame> complete;
    bool keyframe_required = false;
    {
        std::lock_guard media_lock(media_mutex_);
        if (!net::parse_encoded_video_fragment(bytes, &fragment, &fragment_error)) {
            const auto summary = reassembler_.require_keyframe();
            std::lock_guard lock(mutex_);
            ++stats_.media_fragment_parse_failures;
            stats_.frames_dropped += summary.dropped_incomplete_frames;
            keyframe_required = true;
        } else {
            const std::uint64_t receiver_us = now_ms * 1000ULL;
            if (!feedback_recorder_.record(fragment.transport_sequence, receiver_us, fragment.rate_revision)) {
                feedback_recorder_.reset();
            }
            auto result = reassembler_.push(fragment, now_ms);
            std::lock_guard lock(mutex_);
            ++stats_.media_fragments;
            stats_.frames_dropped += result.dropped_incomplete_frames + result.dropped_dependency_frames;
            keyframe_required = result.keyframe_required || !result.error.empty();
            if (result.status == net::EncodedVideoReassemblyStatus::kComplete) {
                ++stats_.frames_reassembled;
                if (result.frame.keyframe) ++stats_.keyframes_reassembled;
                stats_.latest_frame_id = result.frame.frame_id;
                stats_.latest_codec = result.frame.codec;
                stats_.latest_width = result.frame.width;
                stats_.latest_height = result.frame.height;
                complete = std::move(result.frame);
            }
        }
    }
    flush_transport_feedback(now_ms * 1000ULL);
    if (keyframe_required) (void)request_keyframe(fragment_error.empty() ? "reassembly_dependency_break" : "fragment_parse_failure");
    if (complete.has_value()) observer_.on_encoded_frame(std::move(*complete));
}

void ControllerSession::Impl::flush_transport_feedback(std::uint64_t receiver_steady_us) {
    std::optional<net::MediaTransportFeedbackBatch> batch;
    {
        std::lock_guard media_lock(media_mutex_);
        batch = feedback_recorder_.take_feedback(receiver_steady_us);
    }
    if (!batch.has_value()) return;
    protocol::StreamControlMessageV1 message;
    message.type = protocol::StreamControlMessageTypeV1::kMediaTransportFeedback;
    message.transport_feedback_id = batch->feedback_id;
    message.observed_rate_revision = batch->observed_rate_revision;
    message.media_transport_arrivals.reserve(batch->arrivals.size());
    for (const auto& arrival : batch->arrivals) {
        message.media_transport_arrivals.push_back({
            .transport_sequence = arrival.transport_sequence,
            .receiver_steady_us = arrival.receiver_steady_us,
        });
    }
    (void)send_control(std::move(message));
}

bool ControllerSession::Impl::request_keyframe(std::string_view reason) {
    const std::uint64_t now_ms = now_steady_ms();
    {
        std::lock_guard lock(mutex_);
        if (!control_open_ || (last_keyframe_request_ms_ != 0
                                  && now_ms - last_keyframe_request_ms_ < kKeyframeRequestMinIntervalMs)) {
            return false;
        }
        last_keyframe_request_ms_ = now_ms;
    }
    protocol::StreamControlMessageV1 request;
    request.type = protocol::StreamControlMessageTypeV1::kKeyframeRequest;
    request.payload = std::string(reason.substr(0, 128));
    if (!send_control(std::move(request))) return false;
    std::lock_guard lock(mutex_);
    ++stats_.keyframe_requests_sent;
    return true;
}

bool ControllerSession::Impl::set_viewport(std::uint32_t width, std::uint32_t height) {
    if (width < kMinViewportPixels || height < kMinViewportPixels
        || width > kMaxViewportPixels || height > kMaxViewportPixels) {
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        if (viewport_width_ == width && viewport_height_ == height && viewport_sent_) return true;
        viewport_width_ = width;
        viewport_height_ = height;
        viewport_sent_ = false;
    }
    send_viewport_if_ready();
    return true;
}

void ControllerSession::Impl::send_viewport_if_ready() {
    protocol::StreamControlMessageV1 request;
    request.type = protocol::StreamControlMessageTypeV1::kViewportRequest;
    {
        std::lock_guard lock(mutex_);
        if (!control_open_ || viewport_sent_ || viewport_width_ < kMinViewportPixels
            || viewport_height_ < kMinViewportPixels) {
            return;
        }
        request.viewport_width = viewport_width_;
        request.viewport_height = viewport_height_;
        request.log_cursor = ++viewport_transaction_;
    }
    if (send_control(std::move(request))) {
        std::lock_guard lock(mutex_);
        viewport_sent_ = true;
    }
}

void ControllerSession::Impl::set_phase(ControllerSessionPhase phase, std::string detail) {
    {
        std::lock_guard lock(mutex_);
        if (phase_ == phase || phase_ == ControllerSessionPhase::kFailed) return;
        phase_ = phase;
    }
    observer_.on_phase(phase, detail);
}

void ControllerSession::Impl::fail(std::string reason) {
    {
        std::lock_guard lock(mutex_);
        if (phase_ == ControllerSessionPhase::kFailed || phase_ == ControllerSessionPhase::kStopped) return;
        phase_ = ControllerSessionPhase::kFailed;
        transport_failed_ = true;
    }
    log("controller_core failed reason=" + reason);
    observer_.on_phase(ControllerSessionPhase::kFailed, reason);
}

ControllerSession::ControllerSession(IControllerSessionObserver& observer)
    : impl_(std::make_unique<Impl>(observer)) {}
ControllerSession::~ControllerSession() = default;
bool ControllerSession::start(ControllerSessionConfig config, std::string* error) {
    return impl_->start(std::move(config), error);
}
void ControllerSession::stop() { impl_->stop(); }
bool ControllerSession::set_viewport(std::uint32_t width, std::uint32_t height) {
    return impl_->set_viewport(width, height);
}
bool ControllerSession::request_keyframe(std::string_view reason) { return impl_->request_keyframe(reason); }
ControllerSessionPhase ControllerSession::phase() const { return impl_->phase(); }
ControllerSessionStats ControllerSession::stats() const { return impl_->stats(); }

}  // namespace redclaw::controller_core
