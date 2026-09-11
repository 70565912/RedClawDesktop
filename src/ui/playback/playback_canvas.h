#pragma once
#include "redclaw/render/render_module.h"
#include <QImage>
#include <QString>
#include <QJsonObject>
#include <QWidget>
#include <cstdint>

#if defined(_WIN32)
struct ID3D11Device;
#endif
namespace redclaw::ui {
using DirectFrameData = redclaw::render::DecodedVideoFrame;
enum class PresentOutcome { kPresented, kBusyDrop, kFailed };

// Widgets are owned by their Qt parent; the canvas pointer aliases the widget.
// This interface never queues frames or owns a decoder.
class PlaybackCanvas {
 public:
  virtual ~PlaybackCanvas() = default;
  virtual PresentOutcome present_frame(const DirectFrameData& frame, QString* error_detail) = 0;
  virtual PresentOutcome present_image(const QImage& image, QString* error_detail) = 0;
  virtual void clear_frame() {}
  [[nodiscard]] virtual QString backend_name() const = 0;
  [[nodiscard]] virtual QJsonObject diagnostic_snapshot() const { return {{"backend", backend_name()}}; }
  virtual void set_geometry_transaction_active(bool) {}
  virtual void commit_geometry(std::uint64_t) {}
  [[nodiscard]] virtual std::uint64_t swap_chain_resize_count() const { return 0; }
#if defined(_WIN32)
  virtual ID3D11Device* d3d11_decode_device(QString*) { return nullptr; }
#endif
};

struct PlaybackWidgetResult {
    QWidget* widget = nullptr;
    PlaybackCanvas* canvas = nullptr;
};
PlaybackWidgetResult create_best_playback_renderer(QWidget* parent);
}  // namespace redclaw::ui
