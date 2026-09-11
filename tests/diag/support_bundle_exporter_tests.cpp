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

bool test_support_bundle_required_fields_present() {
    redclaw::diag::SupportBundleRequest request;
    request.bundle_id = "bundle-001";
    request.module = "diag";
    request.generated_at_unix = 1'742'428'800ULL;

    const auto bundle = redclaw::diag::export_support_bundle(request);

    return expect_true(bundle.find("support_bundle_version=1") != std::string::npos, "bundle should include version")
        && expect_true(bundle.find("bundle_id=bundle-001") != std::string::npos, "bundle should include bundle id")
        && expect_true(bundle.find("module=diag") != std::string::npos, "bundle should include module")
        && expect_true(bundle.find("generated_at_unix=1742428800") != std::string::npos, "bundle should include generation timestamp")
        && expect_true(bundle.find("structured_log_count=0") != std::string::npos, "bundle should include structured log count")
        && expect_true(bundle.find("raw_log_count=0") != std::string::npos, "bundle should include raw log count");
}

bool test_support_bundle_redacts_sensitive_metadata() {
    redclaw::diag::SupportBundleRequest request;
    request.bundle_id = "bundle-002";
    request.generated_at_unix = 1'742'428'801ULL;
    request.metadata = {
        {"operator_id", "operator-1"},
        {"api_token", "top-secret-token"},
    };

    const auto bundle = redclaw::diag::export_support_bundle(request);

    return expect_true(bundle.find("operator_id=operator-1") != std::string::npos, "operator id should remain visible")
        && expect_true(bundle.find("api_token=[REDACTED:") != std::string::npos, "sensitive metadata should be redacted");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_support_bundle_required_fields_present() && ok;
    ok = test_support_bundle_redacts_sensitive_metadata() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_diag_support_bundle_exporter_tests" << '\n';
    return 0;
}
