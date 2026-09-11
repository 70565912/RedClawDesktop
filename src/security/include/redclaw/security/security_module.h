#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace redclaw::security {

enum class HandshakeReplayDecision {
	accepted,
	rejected_invalid_input,
	rejected_future_timestamp,
	rejected_expired_timestamp,
	rejected_duplicate_nonce,
};

struct HandshakeReplayGuardConfig {
	std::uint64_t max_age_ms = 120000;
	std::uint64_t max_future_skew_ms = 3000;
	std::size_t max_nonces_per_session = 256;
};

struct HandshakeReplayCheckRequest {
	std::string session_id;
	std::string nonce;
	std::uint64_t created_at_ms = 0;
};

struct HandshakeReplayCheckResult {
	bool accepted = false;
	HandshakeReplayDecision decision = HandshakeReplayDecision::rejected_invalid_input;
	std::string error;
};

class InMemoryHandshakeReplayGuard {
public:
	using NowProvider = std::function<std::uint64_t()>;

	explicit InMemoryHandshakeReplayGuard(
		HandshakeReplayGuardConfig config = {},
		NowProvider now_provider = {});

	HandshakeReplayCheckResult check(const HandshakeReplayCheckRequest& request);
	void reset_session(std::string_view session_id);

private:
	struct SessionCache {
		std::deque<std::pair<std::string, std::uint64_t>> arrival_order;
		std::unordered_set<std::string> seen_nonces;
	};

	void prune_expired_locked(SessionCache& cache, std::uint64_t now_ms);

	HandshakeReplayGuardConfig config_;
	NowProvider now_provider_;
	std::unordered_map<std::string, SessionCache> caches_;
};

enum class PeerFingerprintDecision {
	accepted,
	rejected_invalid_input,
	rejected_untrusted,
};

struct PeerFingerprintCheckResult {
	bool accepted = false;
	PeerFingerprintDecision decision = PeerFingerprintDecision::rejected_invalid_input;
	std::string error;
};

class InMemoryPeerFingerprintVerifier {
public:
	InMemoryPeerFingerprintVerifier() = default;
	explicit InMemoryPeerFingerprintVerifier(const std::vector<std::string>& trusted_fingerprints);

	PeerFingerprintCheckResult verify(std::string_view fingerprint) const;
	void trust_fingerprint(std::string_view fingerprint);
	void revoke_fingerprint(std::string_view fingerprint);
	void set_trusted_fingerprints(const std::vector<std::string>& fingerprints);
	[[nodiscard]] std::vector<std::string> trusted_fingerprints() const;

	static std::string normalize_fingerprint_for_policy(std::string_view fingerprint);
	static bool is_well_formed_fingerprint(std::string_view fingerprint);

private:
	std::unordered_set<std::string> trusted_fingerprints_;
};

class FileBackedPeerFingerprintStore {
public:
	explicit FileBackedPeerFingerprintStore(std::string file_path);

	bool load(std::vector<std::string>* fingerprints, std::string* error = nullptr) const;
	bool save(const std::vector<std::string>& fingerprints, std::string* error = nullptr) const;

private:
	std::string file_path_;
};

std::string_view module_name();
}
