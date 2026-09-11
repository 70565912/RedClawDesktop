#pragma once

#include <memory>

#include <QDialog>
#include <QString>
#include <QStringList>

class QSettings;
class QCloseEvent;

namespace redclaw::ui {

class AgentSettingsDialog final : public QDialog {
public:
    explicit AgentSettingsDialog(QSettings* settings, QWidget* parent = nullptr);
    ~AgentSettingsDialog() override;

    void set_runtime_active(bool active);
    void refresh_provider_status();
    [[nodiscard]] bool replace_project_roots_for_automation(
        const QStringList& roots,
        QString* error_detail = nullptr);

    [[nodiscard]] QStringList project_roots() const;
    [[nodiscard]] bool any_provider_ready() const;
    [[nodiscard]] QString readiness_summary() const;

protected:
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::ui
