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

bool test_fit_mode_letterboxes_with_centered_destination() {
    redclaw::render::RenderViewportConfig config;
    config.viewport_width = 1280;
    config.viewport_height = 1024;
    config.scale_mode = redclaw::render::ViewportScaleMode::kFit;

    redclaw::render::RenderViewportLayout layout;
    std::string error;
    const bool ok = redclaw::render::compute_render_viewport_layout(1920, 1080, config, &layout, &error);

    return expect_true(ok, "fit layout should succeed")
        && expect_true(error.empty(), "fit layout should not return error")
        && expect_true(layout.destination.width == 1280, "fit destination width mismatch")
        && expect_true(layout.destination.height == 720, "fit destination height mismatch")
        && expect_true(layout.destination.x == 0, "fit destination x should be centered")
        && expect_true(layout.destination.y == 152, "fit destination y should be centered")
        && expect_true(layout.source.width == 1920, "fit source width should remain full frame")
        && expect_true(layout.source.height == 1080, "fit source height should remain full frame");
}

bool test_fill_mode_crops_source() {
    redclaw::render::RenderViewportConfig config;
    config.viewport_width = 1024;
    config.viewport_height = 1024;
    config.scale_mode = redclaw::render::ViewportScaleMode::kFill;

    redclaw::render::RenderViewportLayout layout;
    std::string error;
    const bool ok = redclaw::render::compute_render_viewport_layout(1920, 1080, config, &layout, &error);

    return expect_true(ok, "fill layout should succeed")
        && expect_true(layout.destination.width == 1024, "fill destination should cover viewport width")
        && expect_true(layout.destination.height == 1024, "fill destination should cover viewport height")
        && expect_true(layout.source.width < 1920, "fill mode should crop source width")
        && expect_true(layout.source.height == 1080, "fill mode should keep full source height for this aspect");
}

bool test_pan_changes_cropped_source_region() {
    redclaw::render::RenderViewportConfig centered;
    centered.viewport_width = 1024;
    centered.viewport_height = 1024;
    centered.scale_mode = redclaw::render::ViewportScaleMode::kFill;

    redclaw::render::RenderViewportConfig panned = centered;
    panned.pan_x = 120;

    redclaw::render::RenderViewportLayout layout_centered;
    redclaw::render::RenderViewportLayout layout_panned;

    std::string error;
    const bool ok_centered = redclaw::render::compute_render_viewport_layout(1920, 1080, centered, &layout_centered, &error);
    const bool ok_panned = redclaw::render::compute_render_viewport_layout(1920, 1080, panned, &layout_panned, &error);

    return expect_true(ok_centered && ok_panned, "panned/centered fill layouts should succeed")
        && expect_true(layout_panned.source.x < layout_centered.source.x, "positive pan_x should shift source crop window")
        && expect_true(layout_panned.source.width > 0, "panned source crop width should remain valid")
        && expect_true(layout_panned.destination.width == 1024, "panned fill should still cover viewport width");
}

bool test_one_to_one_mode_clips_when_viewport_smaller_than_frame() {
    redclaw::render::RenderViewportConfig config;
    config.viewport_width = 640;
    config.viewport_height = 360;
    config.scale_mode = redclaw::render::ViewportScaleMode::kOneToOne;

    redclaw::render::RenderViewportLayout layout;
    std::string error;
    const bool ok = redclaw::render::compute_render_viewport_layout(1280, 720, config, &layout, &error);

    return expect_true(ok, "one-to-one layout should succeed")
        && expect_true(layout.scale == 1.0, "one-to-one scale should be 1")
        && expect_true(layout.destination.width == 640, "one-to-one destination width should match viewport")
        && expect_true(layout.destination.height == 360, "one-to-one destination height should match viewport")
        && expect_true(layout.source.width == 640, "one-to-one source should be center-cropped to viewport width")
        && expect_true(layout.source.height == 360, "one-to-one source should be center-cropped to viewport height")
        && expect_true(layout.source.x == 320, "one-to-one source x should be centered")
        && expect_true(layout.source.y == 180, "one-to-one source y should be centered");
}

bool test_invalid_dimensions_fail_fast() {
    redclaw::render::RenderViewportConfig config;
    config.viewport_width = 0;
    config.viewport_height = 360;

    redclaw::render::RenderViewportLayout layout;
    std::string error;
    const bool ok = redclaw::render::compute_render_viewport_layout(1280, 720, config, &layout, &error);

    return expect_true(!ok, "invalid viewport dimensions should fail")
        && expect_true(!error.empty(), "invalid viewport dimensions should return error");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_fit_mode_letterboxes_with_centered_destination() && ok;
    ok = test_fill_mode_crops_source() && ok;
    ok = test_pan_changes_cropped_source_region() && ok;
    ok = test_one_to_one_mode_clips_when_viewport_smaller_than_frame() && ok;
    ok = test_invalid_dimensions_fail_fast() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_render_viewport_controls_tests" << '\n';
    return 0;
}
