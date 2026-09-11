#include <cstdint>
#include <iostream>
#include <string>

#include "redclaw/render/render_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_timeline_append_and_latest() {
    redclaw::render::RuntimeStatusTimelineConfig config;
    config.max_events = 4;
    redclaw::render::RuntimeStatusTimeline timeline(config);

    for (std::uint64_t i = 1; i <= 6; ++i) {
        redclaw::render::RuntimeStatusEvent event;
        event.timestamp_ms = i;
        event.category = "runtime";
        event.message = "event-" + std::to_string(i);
        timeline.append(event);
    }

    const auto latest = timeline.latest(3);
    return expect_true(timeline.size() == 4, "timeline should keep bounded max size")
        && expect_true(latest.size() == 3, "timeline latest should return requested number of events")
        && expect_true(latest.front().timestamp_ms == 4, "latest head should be event 4")
        && expect_true(latest.back().timestamp_ms == 6, "latest tail should be event 6");
}

bool test_timeline_text_rendering_contains_severity_and_category() {
    redclaw::render::RuntimeStatusTimeline timeline;

    redclaw::render::RuntimeStatusEvent info;
    info.timestamp_ms = 100;
    info.severity = redclaw::render::RuntimeStatusSeverity::kInfo;
    info.category = "signal";
    info.message = "offer published";
    timeline.append(info);

    redclaw::render::RuntimeStatusEvent error;
    error.timestamp_ms = 200;
    error.severity = redclaw::render::RuntimeStatusSeverity::kError;
    error.category = "ice";
    error.message = "connection failed";
    timeline.append(error);

    const std::string rendered = redclaw::render::render_runtime_status_timeline_text(timeline, 8);
    return expect_true(rendered.find("[INFO][signal] offer published") != std::string::npos,
            "rendered text should include info entry")
        && expect_true(rendered.find("[ERROR][ice] connection failed") != std::string::npos,
            "rendered text should include error entry");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_timeline_append_and_latest() && ok;
    ok = test_timeline_text_rendering_contains_severity_and_category() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_render_runtime_status_timeline_tests" << '\n';
    return 0;
}
