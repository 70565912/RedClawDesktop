#include "ui/agent_panel_presentation.h"

#include <QDialog>
#include <QLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace redclaw::ui {

AgentPanelPresentation::AgentPanelPresentation(QWidget* panel, QWidget* owner)
    : QObject(owner), panel_(panel), sidebar_layout_(panel->parentWidget()->layout()),
      window_(new QDialog(owner)), open_(new QPushButton("Remote Agent", owner)) {
    window_->setObjectName("hostRemoteAgentWindow");
    window_->setWindowTitle("Remote Agent — call the connected device");
    window_->setModal(false);
    window_->setAttribute(Qt::WA_DeleteOnClose, false);
    window_->resize(520, 760);
    new QVBoxLayout(window_);
    open_->setObjectName("openRemoteAgentButton");
    connect(open_, &QPushButton::clicked, this, [this] {
        window_->show();
        window_->raise();
        window_->activateWindow();
    });
}

void AgentPanelPresentation::set_desktop_host(bool host) {
    window_->hide();
    open_->setVisible(host);
    if (!panel_) return;
    QLayout* target = host ? window_->layout() : sidebar_layout_.data();
    if (target == nullptr) return;
    target->addWidget(panel_);
    panel_->show();
}

void AgentPanelPresentation::hide_window() { window_->hide(); }
QPushButton* AgentPanelPresentation::open_button() const { return open_; }

}  // namespace redclaw::ui
