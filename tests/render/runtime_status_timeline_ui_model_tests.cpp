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

bool test_ui_model_refresh_tracks_latest_timeline_events() {
    redclaw::render::RuntimeStatusTimeline timeline;

    for (std::uint64_t i = 1; i <= 5; ++i) {
        redclaw::render::RuntimeStatusEvent event;
        event.timestamp_ms = i;
        event.severity = (i % 2 == 0)
            ? redclaw::render::RuntimeStatusSeverity::kWarning
            : redclaw::render::RuntimeStatusSeverity::kInfo;
        event.category = "signal";
        event.message = "event-" + std::to_string(i);
        timeline.append(event);
    }

    redclaw::render::RuntimeStatusTimelineUiModelConfig config;
    config.max_items = 3;
    config.widget_title = "TimelineWidget";

    redclaw::render::RuntimeStatusTimelineUiModel model(config);
    model.bind(&timeline);
    model.refresh();

    const auto& items = model.items();
    bool ok = true;
    ok = expect_true(items.size() == 3, "ui model should keep latest max_items") && ok;
    ok = expect_true(items.front().timestamp_ms == 3, "ui model first item should be timestamp 3") && ok;
    ok = expect_true(items.back().timestamp_ms == 5, "ui model last item should be timestamp 5") && ok;
    ok = expect_true(model.widget_title() == "TimelineWidget", "ui model should expose widget title") && ok;

    const std::string text = redclaw::render::render_runtime_status_timeline_widget_text(model);
    ok = expect_true(text.find("TimelineWidget") != std::string::npos, "widget text should include title") && ok;
    ok = expect_true(text.find("event-5") != std::string::npos, "widget text should include latest event") && ok;

    return ok;
}

bool test_widget_component_binds_ui_model_state() {
    redclaw::render::RuntimeStatusTimeline timeline;

    redclaw::render::RuntimeStatusEvent info;
    info.timestamp_ms = 1;
    info.severity = redclaw::render::RuntimeStatusSeverity::kInfo;
    info.category = "runtime";
    info.message = "ready";
    timeline.append(info);

    redclaw::render::RuntimeStatusEvent warning;
    warning.timestamp_ms = 2;
    warning.severity = redclaw::render::RuntimeStatusSeverity::kWarning;
    warning.category = "signal";
    warning.message = "reconnect";
    timeline.append(warning);

    redclaw::render::RuntimeStatusEvent error;
    error.timestamp_ms = 3;
    error.severity = redclaw::render::RuntimeStatusSeverity::kError;
    error.category = "ice";
    error.message = "failed";
    timeline.append(error);

    redclaw::render::RuntimeStatusTimelineUiModel model;
    model.bind(&timeline);
    model.refresh();

    redclaw::render::RuntimeStatusTimelineWidgetComponent widget;
    widget.bind(&model);
    widget.refresh();

    const auto& state = widget.state();
    bool ok = true;
    ok = expect_true(state.title == model.widget_title(), "widget title should mirror ui model title") && ok;
    ok = expect_true(state.items.size() == model.items().size(), "widget items should mirror ui model items") && ok;
    ok = expect_true(state.has_warning, "widget should mark warning presence") && ok;
    ok = expect_true(state.has_error, "widget should mark error presence") && ok;

    widget.clear();
    ok = expect_true(widget.state().items.empty(), "widget clear should drop items") && ok;
    ok = expect_true(widget.state().title.empty(), "widget clear should reset title") && ok;
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_ui_model_refresh_tracks_latest_timeline_events() && ok;
    ok = test_widget_component_binds_ui_model_state() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_render_runtime_status_timeline_ui_model_tests" << '\n';
    return 0;
}
