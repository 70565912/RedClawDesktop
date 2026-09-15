#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace redclaw::net {

enum class IceConnectionState {
	kNew,
	kGathering,
	kConnecting,
	kConnected,
	kDisconnected,
	kFailed,
	kClosed,
};

enum class IceGatheringState {
    kNew,
    kInProgress,
    kComplete,
};

enum class DataChannelKind {
	kMedia,
	kControl,
	kAgent,
	kNavigation,
	kDebugBridge,
	kTerminal,
	kTransfer,
};

struct IceGatheringConfig {
	std::vector<std::string> ice_servers;
	std::string bind_address;
	bool enable_ice_tcp = false;
    // Initial connectivity only. Zero preserves native defaults; no timer renewal on trickle.
    std::uint32_t initial_check_window_ms = 0;
	std::uint16_t port_range_begin = 1024;
	std::uint16_t port_range_end = 65535;
	bool initiate_offer = true;
	std::vector<DataChannelKind> data_channels = {
		DataChannelKind::kMedia,
		DataChannelKind::kControl,
		DataChannelKind::kAgent,
		DataChannelKind::kNavigation,
	};
};

struct StunServerProbeResult {
	std::string server_uri;
	bool responded = false;
	std::uint32_t response_time_ms = 0;
};

[[nodiscard]] std::string choose_preferred_stun_server(
	const std::vector<std::string>& ordered_stun_server_uris,
	std::span<const StunServerProbeResult> probe_results);

using ConnectionStateChangedCallback = std::function<void(IceConnectionState)>;
using GatheringStateChangedCallback = std::function<void(IceGatheringState)>;
using LocalCandidateCallback = std::function<void(const std::string& candidate_sdp, const std::string& mid)>;
using LocalDescriptionCallback = std::function<void(const std::string& sdp, bool is_offer)>;

struct DataChannelDeliveryPolicy {
	bool reliable = true;
	bool ordered = true;
	std::uint32_t max_retransmits = 0;
};

inline constexpr std::string_view kMediaDataChannelLabel = "redclaw-media-v1";
inline constexpr std::string_view kControlDataChannelLabel = "redclaw-control-v1";
inline constexpr std::string_view kAgentDataChannelLabel = "redclaw-agent-v1";
inline constexpr std::string_view kNavigationDataChannelLabel = "redclaw-navigation-v1";
inline constexpr std::string_view kDebugBridgeDataChannelLabel = "redclaw-debug-bridge-v1";
inline constexpr std::string_view kTerminalDataChannelLabel = "redclaw-terminal-v1";
inline constexpr std::string_view kTransferDataChannelLabel = "redclaw-transfer-v1";

[[nodiscard]] DataChannelDeliveryPolicy data_channel_delivery_policy(DataChannelKind kind);

using DataChannelOpenCallback = std::function<void(DataChannelKind kind)>;
using DataChannelClosedCallback = std::function<void(DataChannelKind kind)>;
using DataChannelMessageCallback =
	std::function<void(DataChannelKind kind, const std::string& message)>;
using DataChannelBinaryMessageCallback =
	std::function<void(DataChannelKind kind, std::span<const std::uint8_t> message)>;
using DataChannelWritableCallback = std::function<void(DataChannelKind kind)>;

enum class DataChannelSendDisposition {
	kImmediate,
	kAcceptedQueued,
};

struct DataChannelSendOutcome {
	DataChannelSendDisposition disposition = DataChannelSendDisposition::kImmediate;
	std::size_t buffered_amount = 0;
};

struct DataChannelTransportStats {
	bool available = false;
	bool open = false;
	bool send_blocked = false;
	std::size_t buffered_amount = 0;
	std::size_t available_amount = 0;
	std::size_t max_message_size = 0;
	std::size_t buffered_amount_low_threshold = 0;
};

enum class TransportDiagnosticLayer { kPeerConnection, kIce, kDataChannel, kLocalClose, kIceCheck, kIceCandidate };
enum class IceCheckKind { kRequestTx, kResponseTx, kRequestRx, kResponseRx, kValidationFailed,
    kUnmatched, kTimeout, kNominationTx, kNominationRx, kSendFailed, kCredentialsMissing,
    kSocketRx, kSocketRxPeer, kSocketRxServer, kSocketRxUnknown, kSocketIoFailed,
    kNonStunPreselection, kStunParseFailed, kStateIgnored, kRemoteAddressUnmatched,
    kStunServerResponse, kPeerStunParsed, kLocalCandidateSocketMatch,
    kLocalCandidateSocketMismatch, kSendTargetMatch, kSendTargetMismatch, kSocketReady,
    kUdpIgnoredConnReset, kUdpIgnoredNetReset, kUdpIgnoredConnRefused, kUdpErrorObservationReady, kCount };
inline constexpr std::size_t kIceCheckKindCount = static_cast<std::size_t>(IceCheckKind::kCount);
[[nodiscard]] std::string_view ice_check_kind_name(IceCheckKind kind);
struct TransportDiagnosticEvent {
    std::uint64_t sequence = 0;
    std::uint64_t steady_ms = 0;
    std::uint64_t peer_generation = 0;
    std::uint64_t channel_generation = 0;
    TransportDiagnosticLayer layer = TransportDiagnosticLayer::kPeerConnection;
    std::optional<DataChannelKind> channel;
    int native_state = 0;
    bool failure = false;
    std::string reason;
    int pair_id = -1;
    int local_candidate_type = 0;
    int remote_candidate_type = 0;
    std::uint64_t occurrences = 0;
};
inline constexpr std::size_t kDataChannelKindCount = 7;
struct TransportDiagnostics {
    std::uint64_t event_sequence = 0;
    std::optional<TransportDiagnosticEvent> first_failure;
    std::vector<TransportDiagnosticEvent> recent_events;
    std::vector<TransportDiagnosticEvent> candidate_events; // current generation, <=64, no raw endpoints
    std::uint64_t ignored_stale_callbacks = 0;
    std::uint64_t callback_failures = 0;
    std::uint64_t ice_check_generation = 0;
    std::array<std::uint64_t, kIceCheckKindCount> ice_check_totals{};
};

enum class RemoteCandidateApplyOutcome {
    kApplied, kNotReady, kClosed, kSuperseded, kInvalid, kNativeFailure,
};

class IceConnectivityWrapper {
public:
	IceConnectivityWrapper();
	~IceConnectivityWrapper();
    IceConnectivityWrapper(const IceConnectivityWrapper&) = delete;
    IceConnectivityWrapper& operator=(const IceConnectivityWrapper&) = delete;

	bool reserveFixedUdpPort(
		std::uint16_t port,
		const std::string& bind_address = {},
		std::string* error_detail = nullptr);
	bool startGathering(const IceGatheringConfig& config, std::string* error_detail = nullptr);
	bool applyRemoteCandidate(const std::string& candidate_sdp, const std::string& mid, std::string* error_detail = nullptr);
    [[nodiscard]] RemoteCandidateApplyOutcome tryApplyRemoteCandidate(
        const std::string& candidate_sdp, const std::string& mid, std::string* error_detail = nullptr);
	bool applyRemoteDescription(
		const std::string& sdp,
		bool is_offer,
		std::string* error_detail = nullptr,
		bool auto_answer = true);
	bool emitLocalDescription(std::string* error_detail = nullptr);
	void onConnectionStateChanged(ConnectionStateChangedCallback callback);
	void onGatheringStateChanged(GatheringStateChangedCallback callback);
	void onLocalCandidate(LocalCandidateCallback callback);
	void onLocalDescription(LocalDescriptionCallback callback);
	void onDataChannelOpen(DataChannelOpenCallback callback);
	void onDataChannelClosed(DataChannelClosedCallback callback);
	void onDataChannelMessage(DataChannelMessageCallback callback);
	void onDataChannelBinaryMessage(DataChannelBinaryMessageCallback callback);
	void onDataChannelWritable(DataChannelWritableCallback callback);
	bool sendDataChannelMessage(
		DataChannelKind kind,
		const std::string& message,
		std::string* error_detail = nullptr);
	bool sendDataChannelBinaryMessage(
		DataChannelKind kind,
		std::span<const std::uint8_t> message,
		std::string* error_detail = nullptr,
		DataChannelSendOutcome* outcome = nullptr);
	bool setDataChannelBufferedAmountLowThreshold(
		DataChannelKind kind,
		std::size_t threshold_bytes,
		std::string* error_detail = nullptr);
	bool getDataChannelTransportStats(
		DataChannelKind kind,
		DataChannelTransportStats* stats,
		std::string* error_detail = nullptr) const;
	bool closeDataChannel(
		DataChannelKind kind,
		std::string* error_detail = nullptr);
	bool ensureDataChannel(
		DataChannelKind kind,
		std::string* error_detail = nullptr);
	IceConnectionState connection_state() const;
	void close();
    // Owner-only reconnect barrier: invalidate/close, then drain accepted callbacks.
    // Clear application signaling state ONLY after success, with no app locks held
    // across this call. Registrations survive; startGathering may be used again.
    [[nodiscard]] bool retireAndDrain();
    // Owner-thread teardown barrier. Never wait for a callback from itself.
    bool shutdown();
    [[nodiscard]] TransportDiagnostics diagnostics() const;

private:
	class Impl;
	std::shared_ptr<Impl> impl_;
};


std::string_view module_name();
}
