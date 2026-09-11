#include "playback_backends.h"
#include <QPainter>
namespace redclaw::ui {
namespace {
class QtPlaybackWidget final : public QWidget, public PlaybackCanvas {
 public:
  explicit QtPlaybackWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
  }

  [[nodiscard]] QString backend_name() const override { return "qt"; }

  void clear_frame() override {
    current_image_ = QImage();
    update();
  }

  PresentOutcome present_frame(const DirectFrameData& frame, QString* error_detail) override {
    if (frame.width == 0 || frame.height == 0 || frame.pixels.empty()) {
      if (error_detail != nullptr) {
        *error_detail = "Qt renderer: empty frame data";
      }
      return PresentOutcome::kFailed;
    }
    const QImage full_image(
        frame.pixels.data(),
        static_cast<int>(frame.width),
        static_cast<int>(frame.height),
        static_cast<int>(frame.row_pitch),
        QImage::Format_RGB32);
    const int content_width = static_cast<int>(frame.content_rect_width == 0
        ? frame.width : frame.content_rect_width);
    const int content_height = static_cast<int>(frame.content_rect_height == 0
        ? frame.height : frame.content_rect_height);
    current_image_ = full_image.copy(
        static_cast<int>(frame.content_rect_x),
        static_cast<int>(frame.content_rect_y),
        content_width,
        content_height);
    update();
    return PresentOutcome::kPresented;
  }

  PresentOutcome present_image(const QImage& image, QString* error_detail) override {
    if (image.isNull()) {
      if (error_detail != nullptr) {
        *error_detail = "Qt renderer: null image";
      }
      return PresentOutcome::kFailed;
    }
    current_image_ = image.copy();
    update();
    return PresentOutcome::kPresented;
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.fillRect(rect(), QColor(2, 6, 23));
    if (!current_image_.isNull()) {
      const QSize img_size = current_image_.size().scaled(
          size(), Qt::KeepAspectRatio);
      const QRect dst(
          (width()  - img_size.width())  / 2,
          (height() - img_size.height()) / 2,
          img_size.width(), img_size.height());
      p.drawImage(dst, current_image_);
    }
  }

 private:
  QImage current_image_;
};
}  // namespace
PlaybackWidgetResult create_qt_playback_renderer(QWidget* parent) {
    auto* widget = new QtPlaybackWidget(parent);
    return {widget, widget};
}
}  // namespace redclaw::ui
