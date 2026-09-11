#pragma once

#include <cstdint>
#include <optional>

namespace redclaw::ui {

struct PlaybackWindowRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const PlaybackWindowRect&) const = default;
};

struct PlaybackGeometryTransactionCounters {
    std::uint64_t preview_total = 0;
    std::uint64_t commit_total = 0;
    std::uint64_t cancel_total = 0;
};

class PlaybackGeometryTransaction final {
public:
    void begin(PlaybackWindowRect committed_rect);
    [[nodiscard]] bool preview(PlaybackWindowRect proposed_rect);
    void cancel();
    [[nodiscard]] std::optional<PlaybackWindowRect> complete();

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] PlaybackWindowRect committed_rect() const noexcept;
    [[nodiscard]] PlaybackWindowRect proposed_rect() const noexcept;
    [[nodiscard]] PlaybackGeometryTransactionCounters counters() const noexcept;

private:
    PlaybackWindowRect committed_rect_;
    PlaybackWindowRect proposed_rect_;
    PlaybackGeometryTransactionCounters counters_;
    bool active_ = false;
    bool cancelled_ = false;
    bool has_preview_ = false;
};

}  // namespace redclaw::ui
