#include "playback_backends.h"
namespace redclaw::ui {
PlaybackWidgetResult create_best_playback_renderer(QWidget* parent) {
#if defined(_WIN32)
    if (auto result = try_create_d3d11_playback_renderer(parent); result.widget != nullptr) {
        return result;
    }
#endif
#if defined(REDCLAW_ENABLE_QT_OPENGL)
    return create_opengl_playback_renderer(parent);
#else
    return create_qt_playback_renderer(parent);
#endif
}
}  // namespace redclaw::ui
