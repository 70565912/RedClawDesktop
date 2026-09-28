#pragma once

#include <functional>
#include <utility>

#include <QCloseEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QWidget>

namespace redclaw::ui {

// Confirm user exits before the window or runtime starts tearing down a live Host.
class HostExitConfirmation final : public QObject {
public:
  HostExitConfirmation(QWidget& window, std::function<bool()> has_connected_host)
      : QObject(&window), window_(window), has_connected_host_(std::move(has_connected_host)) {
    window_.installEventFilter(this);
  }

  bool confirm_exit() {
    if (prompt_active_) {
      return false;
    }
    if (!has_connected_host_()) {
      return true;
    }

    QScopedValueRollback<bool> prompt_active(prompt_active_, true);
    QMessageBox prompt(QMessageBox::Warning,
                       tr("Exit Host session?"),
                       tr("A Controller is connected. Exiting will disconnect the remote desktop, "
                          "terminal, and Agent connection."),
                       QMessageBox::NoButton,
                       &window_);
    auto* exit_button = prompt.addButton(tr("Disconnect and exit"), QMessageBox::AcceptRole);
    auto* cancel_button = prompt.addButton(QMessageBox::Cancel);
    prompt.setDefaultButton(cancel_button);
    prompt.setEscapeButton(cancel_button);
    prompt.exec();
    return prompt.clickedButton() == exit_button;
  }

protected:
  bool eventFilter(QObject* target, QEvent* event) override {
    if (target == &window_ && event->type() == QEvent::Close && !confirm_exit()) {
      static_cast<QCloseEvent*>(event)->ignore();
      return true;
    }
    return false;
  }

private:
  QWidget& window_;
  std::function<bool()> has_connected_host_;
  bool prompt_active_ = false;
};

}  // namespace redclaw::ui
