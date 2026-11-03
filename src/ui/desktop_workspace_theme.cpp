#include "ui/desktop_workspace_theme.h"
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QWidget>

namespace redclaw::ui::desktop_theme {
QString connection_panel_style() {
    return QString::fromLatin1(R"CSS(
QWidget { background: transparent; color: #e4ebf3; font: 10pt "Microsoft YaHei UI"; }
QLabel { border: none; }
QLabel#connectionOverlayTitle { font-size: 14pt; font-weight: 600; }
QLabel#connectionSectionTitle { font-size: 11pt; font-weight: 600; }
QLabel#connectionRuntimeLabel, QLabel#connectionRuntimeDetails { color: #9aaabd; }
QWidget#connectionOverlayHeader { border-bottom: 1px solid #303e4f; }
QWidget#connectionOverlayActions { border-top: 1px solid #303e4f; }
QScrollArea { border: none; background: transparent; }
QGroupBox { border: none; margin-top: 18px; padding: 0; background: transparent; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 0; padding: 0; color: #e4ebf3; }
QPushButton { background: #233140; border: 1px solid #3b4b5c; border-radius: 8px;
    padding: 6px 14px; color: #e4ebf3; font-weight: 400; }
QPushButton:hover { background: #2b3b4c; }
QPushButton:pressed { background: #172330; }
QPushButton:disabled { background: #202c39; color: #718398; border-color: #303e4f; }
QPushButton#connectionOverlayDisconnect { color: #de8d8d; border-color: #91616a; }
QPushButton#connectionOverlayDisconnect:hover { background: #3b2c37; }
QToolButton { background: transparent; color: #9aaabd; border: none; border-radius: 6px; padding: 4px; }
QToolButton:hover { background: #2b3b4c; color: #e4ebf3; }
QGroupBox#primaryCard QPushButton { background: transparent; border: none; color: #9aaabd;
    padding: 3px 6px; font-size: 9pt; }
QGroupBox#primaryCard QPushButton:hover { background: #2b3b4c; color: #e4ebf3; }
QPlainTextEdit { background: #121b25; color: #c5d0dd; border: 1px solid #303e4f;
    border-radius: 8px; padding: 8px; font: 9pt "Consolas"; selection-background-color: #315573; }
QScrollBar:vertical { background: transparent; width: 7px; margin: 2px 0; border: none; }
QScrollBar::handle:vertical { background: #4b6076; min-height: 28px; border-radius: 3px; }
QScrollBar:horizontal { background: transparent; height: 7px; margin: 0 2px; border: none; }
QScrollBar::handle:horizontal { background: #4b6076; min-width: 28px; border-radius: 3px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
)CSS");
}

QString toolbar_style() {
    return QString::fromLatin1(R"CSS(
QWidget#desktopTaskButtonBar, QLabel { background: transparent; color: #9aaabd; font: 9pt "Microsoft YaHei UI"; }
QToolButton, QPushButton { background: transparent; color: #e4ebf3; padding: 5px 8px;
    border: 1px solid transparent; border-radius: 7px; font: 9pt "Microsoft YaHei UI"; }
QToolButton:checked, QPushButton:checked { background: #2b3b4c; border-color: #52677e; }
QToolButton:hover, QPushButton:hover { background: #2b3b4c; }
QToolButton:disabled, QPushButton:disabled { color: #718398; }
QPushButton#disconnectSessionButton { color: #de8d8d; }
)CSS");
}
} // namespace redclaw::ui::desktop_theme
