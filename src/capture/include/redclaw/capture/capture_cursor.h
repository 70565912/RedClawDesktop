#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace redclaw::capture {
enum class CaptureCursorKind : std::uint32_t { kMonochrome = 1, kColor = 2, kMaskedColor = 4 };
struct CaptureCursorShape {
    CaptureCursorKind kind = CaptureCursorKind::kColor;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t revision = 0;
    // Color/masked-color: BGRA. Monochrome: AND, XOR, 0, 0 per pixel.
    std::vector<std::uint8_t> pixels;
    bool update(CaptureCursorKind type, std::uint32_t w, std::uint32_t h,
        std::uint32_t pitch, std::span<const std::uint8_t> bytes);
};
struct CaptureCursorPlacement {
    // DDA reports the top-left, not the hotspot, relative to this output.
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool visible = false;
    std::uint32_t rotation = 0;
};
struct CaptureCursorRect { std::int32_t x = 0, y = 0; std::uint32_t width = 0, height = 0; };
CaptureCursorRect capture_cursor_rect(const CaptureCursorShape& shape,
    CaptureCursorPlacement placement, std::uint32_t frame_width, std::uint32_t frame_height);
void composite_capture_cursor(const CaptureCursorShape& shape, CaptureCursorPlacement placement,
    std::span<std::uint8_t> bgra, std::uint32_t width, std::uint32_t height, std::uint32_t pitch);
} // namespace redclaw::capture
