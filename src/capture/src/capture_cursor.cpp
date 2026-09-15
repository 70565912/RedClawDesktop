#include "redclaw/capture/capture_cursor.h"
#include <algorithm>
#include <limits>

namespace redclaw::capture {
bool CaptureCursorShape::update(CaptureCursorKind type, std::uint32_t w, std::uint32_t h,
    std::uint32_t pitch, std::span<const std::uint8_t> bytes) {
    const bool mono = type == CaptureCursorKind::kMonochrome;
    if ((!mono && type != CaptureCursorKind::kColor && type != CaptureCursorKind::kMaskedColor)
        || !w || !h || (mono && h % 2) || w > 16384 || h > 16384
        || pitch < (mono ? (w + 7) / 8 : w * 4)
        || static_cast<std::uint64_t>(pitch) * h > bytes.size()) { return false; }
    const auto visible_height = mono ? h / 2 : h;
    std::vector<std::uint8_t> next(static_cast<std::size_t>(w) * visible_height * 4);
    for (std::uint32_t y = 0; y < visible_height; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            auto* out = next.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            if (mono) {
                const auto mask = 0x80U >> (x % 8);
                out[0] = (bytes[static_cast<std::size_t>(y) * pitch + x / 8] & mask) ? 255 : 0;
                out[1] = (bytes[static_cast<std::size_t>(y + visible_height) * pitch + x / 8] & mask) ? 255 : 0;
            } else {
                std::copy_n(bytes.data() + static_cast<std::size_t>(y) * pitch + x * 4, 4, out);
            }
        }
    }
    kind = type; width = w; height = visible_height; pixels = std::move(next); ++revision;
    return true;
}

CaptureCursorRect capture_cursor_rect(const CaptureCursorShape& shape,
    CaptureCursorPlacement p, std::uint32_t w, std::uint32_t h) {
    // Map the upright output coordinates to the unrotated duplication surface.
    switch (p.rotation) {
    case 90: return {p.y, static_cast<std::int32_t>(h) - p.x - static_cast<std::int32_t>(shape.width), shape.height, shape.width};
    case 180: return {static_cast<std::int32_t>(w) - p.x - static_cast<std::int32_t>(shape.width),
        static_cast<std::int32_t>(h) - p.y - static_cast<std::int32_t>(shape.height), shape.width, shape.height};
    case 270: return {static_cast<std::int32_t>(w) - p.y - static_cast<std::int32_t>(shape.height), p.x, shape.height, shape.width};
    default: return {p.x, p.y, shape.width, shape.height};
    }
}

void composite_capture_cursor(const CaptureCursorShape& shape, CaptureCursorPlacement p,
    std::span<std::uint8_t> pixels, std::uint32_t w, std::uint32_t h, std::uint32_t pitch) {
    if (!p.visible || !shape.width || !shape.height || pitch < static_cast<std::uint64_t>(w) * 4
        || static_cast<std::uint64_t>(pitch) * h > pixels.size()) { return; }
    const auto rect = capture_cursor_rect(shape, p, w, h);
    for (std::uint32_t y = 0; y < rect.height; ++y) {
        const auto dy = static_cast<std::int64_t>(rect.y) + y;
        if (dy < 0 || dy >= h) { continue; }
        for (std::uint32_t x = 0; x < rect.width; ++x) {
            const auto dx = static_cast<std::int64_t>(rect.x) + x;
            if (dx < 0 || dx >= w) { continue; }
            std::uint32_t sx = x, sy = y;
            switch (p.rotation) {
            case 90: sx = shape.width - 1 - y; sy = x; break;
            case 180: sx = shape.width - 1 - x; sy = shape.height - 1 - y; break;
            case 270: sx = y; sy = shape.height - 1 - x; break;
            default: break;
            }
            const auto* cursor = shape.pixels.data() + (static_cast<std::size_t>(sy) * shape.width + sx) * 4;
            auto* dst = pixels.data() + static_cast<std::size_t>(dy) * pitch + static_cast<std::size_t>(dx) * 4;
            for (int c = 0; c < 3; ++c) {
                if (shape.kind == CaptureCursorKind::kMonochrome) { dst[c] = (dst[c] & cursor[0]) ^ cursor[1]; }
                else if (shape.kind == CaptureCursorKind::kMaskedColor) { dst[c] = cursor[3] ? dst[c] ^ cursor[c] : cursor[c]; }
                else { dst[c] = static_cast<std::uint8_t>((cursor[c] * cursor[3] + dst[c] * (255 - cursor[3]) + 127) / 255); }
            }
        }
    }
}
} // namespace redclaw::capture
