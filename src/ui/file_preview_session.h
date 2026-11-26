#pragma once

#include "runtime/preview_lifecycle.h"
#include "ui/playback/playback_canvas.h"
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <memory>

namespace redclaw::ui {
class FilePreviewSession {
public:
  bool start(bool timed_smoke) {
    directory_ = std::make_unique<QTemporaryDir>();
    generation_ = 0;
    retired_generation_ = 0;
    receipt_pending_ = timed_smoke;
    return directory_->isValid();
  }
  QString directory() const { return directory_ ? directory_->path() : QString{}; }
  void stop() { directory_.reset(); generation_ = 0; }
  bool apply(runtime::PreviewLifecycle state) {
    if (!directory_ || !state.generation || state.generation <= retired_generation_ || state.generation < generation_) return false;
    if (state.active && state.generation == generation_) return false;
    if (!state.active) retired_generation_ = state.generation;
    generation_ = state.active ? state.generation : 0;
    return true;
  }
  QString path() const {
    return generation_ && directory_ ? directory_->filePath("controller-preview.ppm") : QString{};
  }
  QImage read() const {
    if (path().isEmpty()) return {};
    QFile file(path());
    if (!file.open(QIODevice::ReadOnly) || file.readLine(4) != "P6\n"
        || file.readLine(40) != "# " + QByteArray::number(generation_) + '\n' || !file.seek(0)) return {};
    QImage image;
    image.load(&file, "PPM");
    return image;
  }
  PresentOutcome present(PlaybackCanvas& canvas, const QImage& image, QString* error, std::function<void()> drawn) {
    if (path().isEmpty()) return PresentOutcome::kFailed;
    const auto result = canvas.present_image(image, error);
    canvas.after_draw(result, [this, worker = directory(), generation = generation_, drawn = std::move(drawn)] {
      if (worker != directory() || generation != generation_) return;
      if (receipt_pending_) {
        QFile receipt(directory_->filePath("presented"));
        if (receipt.open(QIODevice::WriteOnly | QIODevice::NewOnly)) receipt_pending_ = false;
      }
      drawn();
    });
    return result;
  }
private:
  std::unique_ptr<QTemporaryDir> directory_;
  quint64 generation_ = 0;
  quint64 retired_generation_ = 0;
  bool receipt_pending_ = false;
};
}
