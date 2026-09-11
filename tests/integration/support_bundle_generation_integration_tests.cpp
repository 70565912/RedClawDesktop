#include <iostream>
#include <string>

#include "redclaw/diag/diag_module.h"
#include "redclaw/service/service_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_support_bundle_includes_redacted_service_audit_line() {
    redclaw::service::PrivilegedAuditEvent audit;
    audit.session_id = "session-int";
    audit.operator_id = "operator-int";
    audit.action = "confirm_uac";
    audit.detail = "auth_token_invalid";
    audit.decision = redclaw::service::PrivilegedDecision::kBlocked;
    audit.error = redclaw::service::PrivilegedError::kReplayDetected;
    audit.timestamp_unix = 1'742'428'900ULL;

    redclaw::diag::SupportBundleRequest request;
    request.bundle_id = "bundle-int-001";
    request.module = "integration";
    request.generated_at_unix = 1'742'428'901ULL;
    request.raw_log_lines.push_back(redclaw::service::format_privileged_audit_log_line(audit));

    const auto bundle = redclaw::diag::export_support_bundle(request);

    return expect_true(bundle.find("[raw_logs]") != std::string::npos, "bundle should include raw logs section")
        && expect_true(bundle.find("component=service.privileged_broker") != std::string::npos, "bundle should include service audit log component")
        && expect_true(bundle.find("auth_detail=[REDACTED:") != std::string::npos, "service audit sensitive data should remain redacted in bundle");
}

}  // namespace

int main() {
    if (!test_support_bundle_includes_redacted_service_audit_line()) {
        return 1;
    }

    std::cout << "[PASS] redclaw_support_bundle_generation_integration_tests" << '\n';
    return 0;
}
