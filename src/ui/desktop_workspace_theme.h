#pragma once

#include <QColor>
#include <QPainter>
#include <QPalette>
#include <QWidget>
#include <QString>
#include <array>

namespace redclaw::ui::desktop_theme {
inline constexpr QRgb kCanvas = 0xff0d131b;
inline constexpr QRgb kSurface = 0xff1b2633;
inline constexpr QRgb kBorder = 0xff303e4f;
inline constexpr auto kText = "#e4ebf3";
inline constexpr auto kMuted = "#9aaabd";
inline constexpr auto kSuccess = "#60c5a3";
inline constexpr auto kProgress = "#78aae9";
inline constexpr auto kWarning = "#d7b66c";
inline constexpr auto kDanger = "#de8d8d";
inline constexpr std::array<float, 4> kCanvasClear{13.0F / 255, 19.0F / 255, 27.0F / 255, 1.0F};

inline void apply_canvas_palette(QWidget* widget) {
    auto palette = widget->palette();
    palette.setColor(QPalette::Window, QColor(kCanvas));
    palette.setColor(QPalette::Base, QColor(kCanvas));
    palette.setColor(QPalette::WindowText, QColor(kText));
    widget->setPalette(palette);
    widget->setAutoFillBackground(true);
}
inline void paint_waiting_desktop(QPainter& painter, const QRect& bounds) {
    const int center = bounds.width() > 800 ? bounds.width() / 3 : bounds.width() / 2;
    const int y = bounds.height() / 2;
    auto font = painter.font(); font.setPointSize(14); font.setBold(true);
    painter.setFont(font); painter.setPen(QColor(kText));
    painter.drawText(QRect(center - 225, y - 14, 450, 32), Qt::AlignCenter,
        QString::fromUtf8("正在连接远程桌面"));
    font.setPointSize(10); font.setBold(false); painter.setFont(font); painter.setPen(QColor(kMuted));
    painter.drawText(QRect(center - 250, y + 24, 500, 28), Qt::AlignCenter,
        QString::fromUtf8("安全连接建立后，画面将在这里显示"));
}
[[nodiscard]] QString connection_panel_style();
[[nodiscard]] QString toolbar_style();
} // namespace redclaw::ui::desktop_theme
