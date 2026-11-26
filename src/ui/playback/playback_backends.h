#pragma once
#include "playback_canvas.h"
namespace redclaw::ui {
#if defined(_WIN32)
PresentOutcome classify_dxgi_present_result(std::int32_t result);
PlaybackWidgetResult try_create_d3d11_playback_renderer(QWidget* parent, PlaybackPresenter presenter = PlaybackPresenter::kOrdinary);

// The canvas bounds have already been clipped to its native ancestors.
template <class ZOrderCoverage>
bool playback_bounds_occluded(const QRect& bounds, [[maybe_unused]] PlaybackPresenter presenter, ZOrderCoverage covered) {
    if (bounds.isEmpty()) return true;
#if defined(_DEBUG)
    if (presenter == PlaybackPresenter::kPrivateTest) return false;
#endif
    return covered(bounds);
}
#endif
#if defined(REDCLAW_ENABLE_QT_OPENGL)
PlaybackWidgetResult create_opengl_playback_renderer(QWidget* parent);
#endif
PlaybackWidgetResult create_qt_playback_renderer(QWidget* parent);
}  // namespace redclaw::ui
