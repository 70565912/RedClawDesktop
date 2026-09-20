#pragma once
#include <QWidget>
#include <memory>
#include <functional>
namespace redclaw::ui {
class WorkspacePipeServer;
class TerminalPanel final : public QWidget {
public:
    TerminalPanel(WorkspacePipeServer& pipe, QWidget* parent = nullptr,
        QString runtime_directory = {}, QString profile_directory = {});
    ~TerminalPanel() override;
    void set_workspace_blocked(bool blocked);
    void end_desktop(std::function<void()> finished);
protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
private:
    void start_if_visible();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
