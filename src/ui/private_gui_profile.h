#pragma once

#include "ui/gui_startup_options.h"
#include <QtGui/qwindowdefs.h>
#include <memory>

class QSettings;
class QObject;
class QMainWindow;
namespace redclaw::security { struct ConnectionCredential; }

namespace redclaw::ui {
// Called before process logging, QApplication, or any GUI settings consumer.
bool prepare_private_gui_profile(const runtime::RuntimeOptions& args, GuiAutoStartOptions& gui);
bool isolate_private_qt_logging();
QStringList private_gui_worker_arguments(const GuiAutoStartOptions& gui);
QSettings* open_gui_settings(const GuiAutoStartOptions& gui, const QString& log_dir, QObject* parent);
bool load_private_controller_credential(const QString& root, security::ConnectionCredential& result);
std::unique_ptr<QMainWindow> create_gui_window(bool private_presenter);
bool show_gui_window(QMainWindow& window, bool private_presenter);
#if defined(_WIN32) && defined(_DEBUG)
bool show_private_gui_window_behind(QMainWindow& window, WId foreground_id);
#endif
}
