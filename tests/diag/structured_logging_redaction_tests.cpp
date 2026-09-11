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

bool test_sensitive_field_redacted() {
    const auto redacted = redclaw::diag::redact_field_value("access_token", "abc123");
    return expect_true(redacted == "[REDACTED:6]", "token fields should be redacted with length");
}

bool test_non_sensitive_field_kept() {
    const auto value = redclaw::diag::redact_field_value("session_id", "session-01");
    return expect_true(value == "session-01", "session_id should remain visible");
}

bool test_formatted_line_contains_redacted_fields() {
    redclaw::diag::StructuredLogEvent event;
    event.component = "net";
    event.event = "peer_loopback_probe";
    event.level = redclaw::diag::LogLevel::kInfo;
    event.timestamp_unix = 1742428800;
    event.fields = {
        {"candidate_count", "2"},
        {"passphrase", "p@ss"},
    };

    const auto line = redclaw::diag::format_structured_log_line(event);
    return expect_true(line.find("component=net") != std::string::npos, "log line should contain component")
        && expect_true(line.find("event=peer_loopback_probe") != std::string::npos, "log line should contain event")
        && expect_true(line.find("candidate_count=2") != std::string::npos, "non-sensitive field should be preserved")
        && expect_true(line.find("passphrase=[REDACTED:4]") != std::string::npos, "sensitive field should be redacted");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_sensitive_field_redacted() && ok;
    ok = test_non_sensitive_field_kept() && ok;
    ok = test_formatted_line_contains_redacted_fields() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_diag_structured_logging_redaction_tests" << '\n';
    return 0;
}
