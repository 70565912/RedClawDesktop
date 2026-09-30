#include <iostream>
#include <string>

#include "redclaw/capture/capture_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_low_latency_profile_rejects_invalid_dimensions() {
    redclaw::capture::EncoderProfileRequest request;
    request.width = 0;
    request.height = 1080;
    request.fps = 60;

    redclaw::capture::EncoderConfigProfile profile;
    std::string error;
    const bool ok = redclaw::capture::build_low_latency_encoder_profile(request, &profile, &error);

    return expect_true(!ok, "profile build should fail when width is zero")
        && expect_true(error.find("non-zero") != std::string::npos, "error should indicate invalid input");
}

bool test_low_latency_profile_interactive_defaults() {
    redclaw::capture::EncoderProfileRequest request;
    request.width = 1920;
    request.height = 1080;
    request.fps = 60;
    request.workload = redclaw::capture::EncoderWorkload::kInteractiveDesktop;

    redclaw::capture::EncoderConfigProfile profile;
    std::string error;
    const bool ok = redclaw::capture::build_low_latency_encoder_profile(request, &profile, &error);

    return expect_true(ok, std::string("profile build should succeed: ") + error)
        && expect_true(profile.rate_control == redclaw::capture::EncoderRateControl::kCbr, "rate control should be CBR")
        && expect_true(profile.gop_length_frames == 120, "interactive profile should use 2-second GOP")
        && expect_true(profile.b_frames == 0, "low-latency profile should disable B-frames")
        && expect_true(!profile.lookahead_enabled, "low-latency profile should disable lookahead")
        && expect_true(profile.zero_latency_tuning, "zero-latency tuning should be enabled");
}

bool test_low_latency_profile_scales_bitrate_with_viewport_pixels() {
    redclaw::capture::EncoderProfileRequest full;
    full.width = 1920;
    full.height = 1080;
    full.fps = 30;
    full.workload = redclaw::capture::EncoderWorkload::kInteractiveDesktop;
    redclaw::capture::EncoderProfileRequest half = full;
    half.width = 960;
    half.height = 540;

    redclaw::capture::EncoderConfigProfile full_profile;
    redclaw::capture::EncoderConfigProfile half_profile;
    std::string error;
    const bool ok = redclaw::capture::build_low_latency_encoder_profile(full, &full_profile, &error)
        && redclaw::capture::build_low_latency_encoder_profile(half, &half_profile, &error);
    return expect_true(ok, std::string("viewport bitrate profiles should build: ") + error)
        && expect_true(half_profile.target_bitrate_kbps < full_profile.target_bitrate_kbps,
            "smaller viewport should start with a lower bitrate")
        && expect_true(half_profile.target_bitrate_kbps >= 400,
            "interactive bitrate should retain the weak-network floor");
}

bool test_viewport_encode_dimensions_fit_both_axes_and_manual_cap() {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string error;
    const bool tall_viewport_ok = redclaw::capture::resolve_viewport_encode_dimensions(
        1920, 1080, 1000, 500, 0, &width, &height, &error);
    const bool tall_viewport_fits = width == 888 && height == 500;
    const bool cap_ok = redclaw::capture::resolve_viewport_encode_dimensions(
        1920, 1080, 1600, 1000, 1280, &width, &height, &error);
    return expect_true(tall_viewport_ok && tall_viewport_fits,
            "viewport height should constrain an aspect-fit even encode target")
        && expect_true(cap_ok && width == 1280 && height == 720,
            "manual width cap should remain authoritative");
}

bool test_low_latency_profile_fast_action_has_higher_bitrate_and_shorter_gop() {
    redclaw::capture::EncoderProfileRequest interactive;
    interactive.width = 1920;
    interactive.height = 1080;
    interactive.fps = 60;
    interactive.workload = redclaw::capture::EncoderWorkload::kInteractiveDesktop;

    redclaw::capture::EncoderProfileRequest fast = interactive;
    fast.workload = redclaw::capture::EncoderWorkload::kFastAction;

    redclaw::capture::EncoderConfigProfile interactive_profile;
    redclaw::capture::EncoderConfigProfile fast_profile;
    std::string error;

    const bool ok_interactive = redclaw::capture::build_low_latency_encoder_profile(interactive, &interactive_profile, &error);
    const bool ok_fast = redclaw::capture::build_low_latency_encoder_profile(fast, &fast_profile, &error);

    return expect_true(ok_interactive && ok_fast, std::string("both profile builds should succeed: ") + error)
        && expect_true(fast_profile.target_bitrate_kbps > interactive_profile.target_bitrate_kbps,
            "fast-action profile should have higher bitrate")
        && expect_true(fast_profile.gop_length_frames == 60, "fast-action profile should use 1-second GOP")
        && expect_true(fast_profile.max_bitrate_kbps > fast_profile.target_bitrate_kbps,
            "max bitrate should be above target bitrate");
}

bool test_low_latency_profile_uses_all_intra_at_one_fps() {
    redclaw::capture::EncoderProfileRequest request;
    request.width = 1334;
    request.height = 834;
    request.fps = 1;
    request.workload = redclaw::capture::EncoderWorkload::kInteractiveDesktop;

    redclaw::capture::EncoderConfigProfile profile;
    std::string error;
    const bool ok = redclaw::capture::build_low_latency_encoder_profile(request, &profile, &error);
    return expect_true(ok, std::string("one-fps low-latency profile should succeed: ") + error)
        && expect_true(profile.fps == 1, "one-fps target should remain one fps")
        && expect_true(profile.gop_length_frames == 2, "one-fps desktop should retain predictive frames")
        && expect_true(profile.target_bitrate_kbps == 1669,
            "one-fps all-intra desktop should retain a text-quality bitrate floor")
        && expect_true(profile.b_frames == 0, "one-fps mode must not introduce B-frames")
        && expect_true(!profile.lookahead_enabled, "one-fps mode must keep lookahead disabled")
        && expect_true(profile.zero_latency_tuning, "one-fps mode must keep zero-latency tuning enabled");
}

bool test_desktop_codec_budget_is_independent_of_submission_cadence() {
    redclaw::capture::EncoderProfileRequest request;
    std::string error;
    struct Step { std::uint32_t width, height, fps, kbps; };
    // Low-cadence resize, cadence recovery, resize back and same-size reconnect.
    for (const auto [width, height, fps, kbps] : {
             Step{1184, 666, 30, 1892}, Step{1778, 1000, 5, 4267},
             Step{1778, 1000, 30, 4267}, Step{1778, 1000, 1, 4267},
             Step{1184, 666, 1, 1892}, Step{1184, 666, 5, 1892}}) {
        request.width = width;
        request.height = height;
        request.fps = fps;
        redclaw::capture::EncoderConfigProfile desktop;
        if (!expect_true(redclaw::capture::build_desktop_encoder_profile(request, &desktop, &error),
                "desktop profile should build: " + error)
            || !expect_true(desktop.fps == 30 && desktop.gop_length_frames == 60,
                "1-to-30-to-1 cadence must retain the nominal clock and frame-count GOP")
            || !expect_true(desktop.target_bitrate_kbps == kbps
                && desktop.max_bitrate_kbps == kbps + kbps / 5,
                "resize/reconnect at low cadence must retain the nominal quality budget; actual="
                    + std::to_string(desktop.target_bitrate_kbps) + " expected=" + std::to_string(kbps))
            || !expect_true(desktop.width == request.width && desktop.height == request.height,
                "desktop policy must preserve pixel geometry")) {
            return false;
        }
    }
    redclaw::capture::EncoderConfigProfile desktop;
    request.fps = 0;
    return expect_true(!redclaw::capture::build_desktop_encoder_profile(request, &desktop, &error),
        "nominal clock substitution must still reject invalid cadence");
}

bool test_interactive_desktop_bitrate_floor_preserves_clarity_before_cadence() {
    return expect_true(
               redclaw::capture::resolve_interactive_desktop_bitrate_floor_kbps(580, 362, 1) == 400,
               "small one-fps viewports should keep the absolute bitrate floor")
        && expect_true(
            redclaw::capture::resolve_interactive_desktop_bitrate_floor_kbps(1680, 1050, 1) == 2646,
            "large one-fps viewports should scale the all-intra quality floor with pixels")
        && expect_true(
            redclaw::capture::resolve_interactive_desktop_bitrate_floor_kbps(1680, 1050, 30) == 4233,
            "normal cadence should retain the full per-frame interactive quality budget")
        && expect_true(
            redclaw::capture::resolve_interactive_desktop_bitrate_floor_kbps(1334, 834, 28) == 2492,
            "reducing cadence should be the only way to reduce the normal inter-frame floor")
        && expect_true(
            redclaw::capture::resolve_interactive_desktop_bitrate_floor_kbps(0, 1050, 2) == 400,
            "unknown dimensions should fail back to the absolute bitrate floor");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_low_latency_profile_rejects_invalid_dimensions() && ok;
    ok = test_low_latency_profile_interactive_defaults() && ok;
    ok = test_low_latency_profile_scales_bitrate_with_viewport_pixels() && ok;
    ok = test_viewport_encode_dimensions_fit_both_axes_and_manual_cap() && ok;
    ok = test_low_latency_profile_fast_action_has_higher_bitrate_and_shorter_gop() && ok;
    ok = test_low_latency_profile_uses_all_intra_at_one_fps() && ok;
    ok = test_interactive_desktop_bitrate_floor_preserves_clarity_before_cadence() && ok;
    ok = test_desktop_codec_budget_is_independent_of_submission_cadence() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_capture_encoder_low_latency_profile_tests" << '\n';
    return 0;
}
