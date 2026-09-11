#pragma once

#include <QObject>
#include <QPointer>

class QDialog;
class QLayout;
class QPushButton;
class QWidget;

namespace redclaw::ui {

// One conversation model/widget, hosted either in the playback sidebar or
// a modeless window. Closing the window never interrupts its task.
class AgentPanelPresentation final : public QObject {
public:
    AgentPanelPresentation(QWidget* panel, QWidget* owner);
    void set_desktop_host(bool host);
    void hide_window();
    [[nodiscard]] QPushButton* open_button() const;
private:
    QPointer<QWidget> panel_;
    QPointer<QLayout> sidebar_layout_;
    QDialog* window_;
    QPushButton* open_;
};

}  // namespace redclaw::ui
