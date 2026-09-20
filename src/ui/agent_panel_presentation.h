#pragma once

#include <QObject>
#include <QPointer>

class QDialog;
class QLayout;
class QPushButton;
class QWidget;

namespace redclaw::ui {
class AgentConversationPanel;

// One conversation model/widget, hosted either in the floating workspace or
// a modeless window. Closing the window never interrupts its task.
class AgentPanelPresentation final : public QObject {
public:
    AgentPanelPresentation(AgentConversationPanel* panel, QWidget* owner);
    void set_desktop_host(bool host);
    void hide_window();
    [[nodiscard]] QPushButton* open_button() const;
private:
    QPointer<AgentConversationPanel> panel_;
    QPointer<QLayout> controller_layout_;
    QDialog* window_;
    QPushButton* open_;
};

}  // namespace redclaw::ui
