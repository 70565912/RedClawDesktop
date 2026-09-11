#include "ui/playback_geometry_transaction.h"

namespace redclaw::ui {

void PlaybackGeometryTransaction::begin(PlaybackWindowRect committed_rect) {
    committed_rect_ = committed_rect;
    proposed_rect_ = committed_rect;
    active_ = true;
    cancelled_ = false;
    has_preview_ = false;
}

bool PlaybackGeometryTransaction::preview(PlaybackWindowRect proposed_rect) {
    if (!active_ || proposed_rect.width <= 0 || proposed_rect.height <= 0) {
        return false;
    }
    proposed_rect_ = proposed_rect;
    has_preview_ = true;
    ++counters_.preview_total;
    return true;
}

void PlaybackGeometryTransaction::cancel() {
    if (!active_) {
        return;
    }
    cancelled_ = true;
    ++counters_.cancel_total;
}

std::optional<PlaybackWindowRect> PlaybackGeometryTransaction::complete() {
    if (!active_) {
        return std::nullopt;
    }
    active_ = false;
    if (cancelled_ || !has_preview_ || proposed_rect_ == committed_rect_) {
        return std::nullopt;
    }
    committed_rect_ = proposed_rect_;
    ++counters_.commit_total;
    return committed_rect_;
}

bool PlaybackGeometryTransaction::active() const noexcept {
    return active_;
}

PlaybackWindowRect PlaybackGeometryTransaction::committed_rect() const noexcept {
    return committed_rect_;
}

PlaybackWindowRect PlaybackGeometryTransaction::proposed_rect() const noexcept {
    return proposed_rect_;
}

PlaybackGeometryTransactionCounters PlaybackGeometryTransaction::counters() const noexcept {
    return counters_;
}

}  // namespace redclaw::ui
