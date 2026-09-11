#include <iostream>
#include <string>

#include "redclaw/diag/diag_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_snapshot_required_fields_and_state_projection() {
    redclaw::diag::ConnectivityDiagnosticsSnapshot snapshot;
    snapshot.session_id = "session-100";
    snapshot.peer_id = "peer-abc";
    snapshot.collected_at_unix = 1'742'600'001ULL;
    snapshot.ice_state = redclaw::net::IceConnectionState::kConnected;
    snapshot.session_state = redclaw::protocol::SessionStateV1::established;
    snapshot.local_candidate_count = 4;
    snapshot.remote_candidate_count = 3;
    snapshot.reconnect_attempt_count = 1;
    snapshot.recent_error_count = 0;

    const auto report = redclaw::diag::export_connectivity_diagnostics_snapshot(snapshot);

    return expect_true(report.find("connectivity_snapshot_version=1") != std::string::npos, "snapshot should include version")
        && expect_true(report.find("session_id=session-100") != std::string::npos, "snapshot should include session id")
        && expect_true(report.find("peer_id=peer-abc") != std::string::npos, "snapshot should include peer id")
        && expect_true(report.find("ice_state=connected") != std::string::npos, "snapshot should include ice state")
        && expect_true(report.find("session_state=established") != std::string::npos, "snapshot should include session state")
        && expect_true(report.find("local_candidate_count=4") != std::string::npos, "snapshot should include local candidate count")
        && expect_true(report.find("remote_candidate_count=3") != std::string::npos, "snapshot should include remote candidate count");
}

bool test_snapshot_redacts_sensitive_fields() {
    redclaw::diag::ConnectivityDiagnosticsSnapshot snapshot;
    snapshot.last_error = "token=secret-token-value";
    snapshot.details = {
        {"controller_token", "xyz-123"},
        {"session_id", "session-visible"},
    };

    const auto report = redclaw::diag::export_connectivity_diagnostics_snapshot(snapshot);

    return expect_true(report.find("last_error=[REDACTED:") != std::string::npos, "last_error should be redacted")
        && expect_true(report.find("controller_token=[REDACTED:") != std::string::npos, "sensitive detail should be redacted")
        && expect_true(report.find("session_id=session-visible") != std::string::npos, "non-sensitive detail should remain visible");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_snapshot_required_fields_and_state_projection() && ok;
    ok = test_snapshot_redacts_sensitive_fields() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_diag_connectivity_diagnostics_snapshot_tests" << '\n';
    return 0;
}
