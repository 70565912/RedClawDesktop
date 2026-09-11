#pragma once
#include <cstdint>
#include <string>
// Test-only harnesses: do not ship diagnostic peer pairs in the runtime library.
namespace redclaw::net {
struct PeerLoopbackPocResult {
	bool ok = false;
	std::string error;
	std::string received_message;
};

struct LanPeerIntegrationHarnessConfig {
	std::uint32_t timeout_ms = 8000;
	bool block_direct_candidates = false;
	bool enable_relay_fallback = true;
};

struct LanPeerIntegrationHarnessResult {
	bool ok = false;
	bool used_relay_fallback = false;
	std::string error;
	std::string received_message;
};

PeerLoopbackPocResult run_peer_loopback_poc(std::uint32_t timeout_ms = 8000);
LanPeerIntegrationHarnessResult run_lan_peer_integration_harness(
	const LanPeerIntegrationHarnessConfig& config = {});
}  // namespace redclaw::net
