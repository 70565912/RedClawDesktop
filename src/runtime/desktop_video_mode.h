#pragma once

#include <optional>
#include <string_view>

namespace redclaw::runtime {

enum class DesktopVideoMode { kDisabled, kDesktop, kSmoke, kRequireCapture };

constexpr bool checks_video_frames(DesktopVideoMode mode) {
    return mode == DesktopVideoMode::kSmoke || mode == DesktopVideoMode::kRequireCapture;
}

constexpr bool controller_smoke_output(bool supervised, bool preview_written, bool presented) {
    return supervised ? presented : preview_written;
}

constexpr std::string_view timed_video_failure(DesktopVideoMode mode, bool host, bool transmitted, bool captured, bool rendered) {
    if (!checks_video_frames(mode)) return {};
    if (host && !transmitted) return "host transmitted zero video frames";
    if (host && mode == DesktopVideoMode::kRequireCapture && !captured) return "host captured zero real desktop frames";
    if (!host && !rendered) return "controller rendered zero preview frames";
    return {};
}

constexpr std::optional<DesktopVideoMode> profile_video_mode(std::optional<bool> desktop_video,
    std::optional<bool> legacy_smoke, std::optional<bool> require_capture) {
    if (desktop_video == false && (legacy_smoke == true || require_capture == true)) return std::nullopt;
    if (require_capture == true) return DesktopVideoMode::kRequireCapture;
    if (legacy_smoke == true) return DesktopVideoMode::kSmoke;
    return desktop_video.value_or(legacy_smoke.value_or(true)) ? DesktopVideoMode::kDesktop : DesktopVideoMode::kDisabled;
}

constexpr std::optional<DesktopVideoMode> desktop_video_flag(std::string_view arg) {
    if (arg == "--no-desktop-video") return DesktopVideoMode::kDisabled;
    if (arg == "--desktop-video") return DesktopVideoMode::kDesktop;
    if (arg == "--stream-smoke") return DesktopVideoMode::kSmoke;
    if (arg == "--stream-require-capture") return DesktopVideoMode::kRequireCapture;
    return std::nullopt;
}

constexpr bool add_video_flag(std::optional<DesktopVideoMode>& selected, DesktopVideoMode flag) {
    if (selected && ((*selected == DesktopVideoMode::kDisabled) != (flag == DesktopVideoMode::kDisabled))) {
        return false;
    }
    if (!selected || flag > *selected) selected = flag;
    return true;
}

constexpr bool apply_video_override(DesktopVideoMode& mode, std::optional<DesktopVideoMode> selected) {
    if (!selected) return true;
    if (*selected == DesktopVideoMode::kDisabled && mode == DesktopVideoMode::kRequireCapture) return false;
    if (*selected == DesktopVideoMode::kDisabled || *selected > mode) mode = *selected;
    return true;
}

inline constexpr auto kVideoConflict = "--no-desktop-video conflicts with an explicit video or capture request";

}  // namespace redclaw::runtime
