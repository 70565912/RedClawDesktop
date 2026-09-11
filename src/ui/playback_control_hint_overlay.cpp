#include "ui/playback_control_hint_overlay.h"

#include <algorithm>

#include <QEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QTimer>

namespace redclaw::ui {

PlaybackControlHintOverlay::PlaybackControlHintOverlay(QWidget* canvas)
    : QWidget(
          canvas != nullptr ? canvas->window() : nullptr,
          Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint
              | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput),
      canvas_(canvas),
      playback_window_(canvas != nullptr ? canvas->window() : nullptr) {
    setObjectName("playbackControlHintOverlay");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    hide_timer_ = new QTimer(this);
    hide_timer_->setSingleShot(true);
    QObject::connect(hide_timer_, &QTimer::timeout, this, [this]() { hide(); });
    if (canvas_ != nullptr) {
        canvas_->installEventFilter(this);
    }
    if (playback_window_ != nullptr && playback_window_ != canvas_) {
        playback_window_->installEventFilter(this);
    }
    hide();
}

void PlaybackControlHintOverlay::show_hint(const QString& message, int duration_ms) {
    if (duration_ms > 0 && isVisible() && !hide_timer_->isActive()) {
        return; // A disconnected-frame warning owns the surface until recovery.
    }
    message_ = message;
    reposition();
    show();
    raise();
    update();
    if (duration_ms > 0) {
        hide_timer_->start(duration_ms);
    } else {
        hide_timer_->stop();
    }
}

void PlaybackControlHintOverlay::hide_hint() {
    hide_timer_->stop();
    hide();
}

void PlaybackControlHintOverlay::hide_persistent_hint() {
    if (!hide_timer_->isActive()) {
        hide_hint();
    }
}

QString PlaybackControlHintOverlay::message() const {
    return message_;
}

bool PlaybackControlHintOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (event == nullptr) {
        return QWidget::eventFilter(watched, event);
    }
    if (watched == playback_window_
        && (event->type() == QEvent::Hide || event->type() == QEvent::Close)) {
        hide_hint();
    } else if (isVisible()
               && (event->type() == QEvent::Move
                   || event->type() == QEvent::Resize
                   || event->type() == QEvent::Show
                   || event->type() == QEvent::WindowStateChange)) {
        reposition();
    }
    return QWidget::eventFilter(watched, event);
}

void PlaybackControlHintOverlay::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF card = rect().adjusted(1, 1, -1, -1);
    painter.setPen(QPen(QColor(255, 255, 255, 35), 1));
    painter.setBrush(QColor(9, 14, 28, 232));
    painter.drawRoundedRect(card, 14, 14);

    const QPointF center(48.0, height() / 2.0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(239, 68, 68), 6, Qt::SolidLine, Qt::RoundCap));
    painter.drawEllipse(center, 24, 24);
    painter.drawLine(center + QPointF(-17, 17), center + QPointF(17, -17));

    QFont text_font = font();
    text_font.setPointSizeF((std::max)(10.0, text_font.pointSizeF()));
    text_font.setWeight(QFont::DemiBold);
    painter.setFont(text_font);
    painter.setPen(QColor(248, 250, 252));
    painter.drawText(
        QRect(88, 12, width() - 104, height() - 24),
        Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap,
        message_);
}

void PlaybackControlHintOverlay::reposition() {
    if (canvas_ == nullptr || playback_window_ == nullptr || !canvas_->isVisible()) {
        return;
    }
    const int available_width = (std::max)(220, canvas_->width() - 32);
    const int overlay_width = (std::min)(520, available_width);
    const int overlay_height = 104;
    const QPoint canvas_origin = canvas_->mapToGlobal(QPoint(0, 0));
    setGeometry(
        canvas_origin.x() + (canvas_->width() - overlay_width) / 2,
        canvas_origin.y() + (canvas_->height() - overlay_height) / 2,
        overlay_width,
        overlay_height);
}

}  // namespace redclaw::ui
