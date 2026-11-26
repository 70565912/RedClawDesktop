#pragma once
#include "redclaw/render/render_module.h"
#include <QImage>
#include <QString>
#include <QJsonObject>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <utility>

#if defined(_WIN32)
struct ID3D11Device;
#endif
namespace redclaw::ui {
using DirectFrameData = redclaw::render::DecodedVideoFrame;
enum class PresentOutcome { kPresented, kDeferred, kBusyDrop, kFailed };

// Widgets are owned by their Qt parent; the canvas pointer aliases the widget.
class PlaybackCanvas {
 public:
  virtual ~PlaybackCanvas() = default;
  virtual PresentOutcome present_frame(const DirectFrameData& frame, QString* error_detail) = 0;
  virtual PresentOutcome present_image(const QImage& image, QString* error_detail) = 0;
  virtual void clear_frame() {}
  template<class Callback>
  bool after_draw(PresentOutcome outcome, Callback&& callback) {
    if (outcome == PresentOutcome::kPresented) callback();
    else if (outcome == PresentOutcome::kDeferred) pending_draw_ = std::forward<Callback>(callback);
    else return false;
    return true;
  }
  void cancel_pending_draw() { pending_draw_ = {}; }
  [[nodiscard]] virtual QString backend_name() const = 0;
  [[nodiscard]] virtual QJsonObject diagnostic_snapshot() const { return {{"backend", backend_name()}}; }
  virtual void set_geometry_transaction_active(bool) {}
  virtual void commit_geometry(std::uint64_t) {}
  [[nodiscard]] virtual std::uint64_t swap_chain_resize_count() const { return 0; }
  // Warnings the backend wants in the session log (adapter choice, slow
  // D3D calls). Invoked on the GUI thread.
  virtual void set_diagnostic_log(std::function<void(const QString&)>) {}
  // Immediately publishes the current state, then changes on the GUI thread.
  // True pauses decoding too; the short reason/trigger scopes recovery.
  // D3D11 includes power/occlusion, other canvases include hide/minimize.
  virtual void set_playback_state_callback(std::function<void(bool, const QString&)>) {}
#if defined(_WIN32)
  // The presenter's own device. Decode never shares it; tests use it to
  // build same-device surfaces.
  virtual ID3D11Device* d3d11_presentation_device(QString*) { return nullptr; }
  // LUID of the presenter's verified monitor owner, for pinned hardware
  // decode. A device chosen only as a presentation fallback is insufficient.
  virtual bool d3d11_presentation_adapter(std::uint64_t*, QString* detail) {
    if (detail != nullptr) *detail = "non-D3D11 playback backend";
    return false;
  }
#endif
 protected:
  void finish_draw() {
    if (auto callback = std::exchange(pending_draw_, {})) callback();
  }
 private:
  std::function<void()> pending_draw_;
};

struct PlaybackWidgetResult {
    QWidget* widget = nullptr;
    PlaybackCanvas* canvas = nullptr;
};
enum class PlaybackPresenter { kOrdinary, kPrivateTest };
PlaybackWidgetResult create_best_playback_renderer(QWidget* parent, PlaybackPresenter presenter = PlaybackPresenter::kOrdinary);
}  // namespace redclaw::ui
