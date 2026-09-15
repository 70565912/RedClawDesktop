#pragma once
#include <QWidget>
#include <memory>
#include <functional>
class QSettings;
namespace redclaw::ui {
class WorkspacePipeServer;
class TerminalPanel final : public QWidget {
public:
    TerminalPanel(WorkspacePipeServer& pipe, QSettings* settings, QWidget* parent = nullptr,
        QString runtime_directory = {}, QString profile_directory = {});
    ~TerminalPanel() override;
    void set_workspace_blocked(bool blocked);
    void end_desktop(std::function<void()> finished);
protected:
    void showEvent(QShowEvent* event) override;
private:
    void start_if_expanded();
    void resize_parent_splitter();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
