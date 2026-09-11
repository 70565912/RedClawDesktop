#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QWidget>

#include "redclaw/protocol/agent_protocol.h"

class QSettings;

namespace redclaw::ui {

enum class AgentSubmitMode {
    kCreateTask,
    kStartTurn,
    kSteerTurn,
};

struct AgentSubmitResult {
    bool ok = false;
    QString task_id;
    QString error;
};

struct AgentProviderView {
    redclaw::protocol::AgentProviderKindV1 kind =
        redclaw::protocol::AgentProviderKindV1::kNone;
    redclaw::protocol::AgentProviderReadinessV1 readiness =
        redclaw::protocol::AgentProviderReadinessV1::kProbeFailed;
    QString label;
    QString display_name;
    QString reason;
    QStringList models;
    bool available = false;
};

struct AgentProjectView {
    QString id;
    QString name;
    bool git_repository = false;
};

class AgentConversationPanel final : public QWidget {
public:
    using SubmitCallback = std::function<AgentSubmitResult(AgentSubmitMode, const QString&)>;
    using InterruptCallback = std::function<bool(QString*)>;
    using ApprovalCallback = std::function<bool(
        redclaw::protocol::AgentApprovalDecisionV1,
        QString*)>;
    using TaskSelectedCallback = std::function<void(const QString&)>;
    using CollapseCallback = std::function<void()>;

    explicit AgentConversationPanel(QSettings* settings, QWidget* parent = nullptr);
    ~AgentConversationPanel() override;

    void set_submit_callback(SubmitCallback callback);
    void set_interrupt_callback(InterruptCallback callback);
    void set_approval_callback(ApprovalCallback callback);
    void set_task_selected_callback(TaskSelectedCallback callback);
    void set_collapse_callback(CollapseCallback callback);

    void set_transport_state(
        bool authorized,
        bool channel_open,
        bool sync_required,
        const QString& detail = {});

    void apply_capability_message(
        const redclaw::protocol::AgentMessageEnvelopeV1& message);
    void apply_project_catalog_message(
        const redclaw::protocol::AgentMessageEnvelopeV1& message);
    void apply_task_message(
        const redclaw::protocol::AgentMessageEnvelopeV1& message);

    void set_current_task_id(
        const QString& task_id,
        redclaw::protocol::AgentTaskStateV1 state =
            redclaw::protocol::AgentTaskStateV1::kQueued);
    [[nodiscard]] QString current_task_id() const;
    [[nodiscard]] redclaw::protocol::AgentTaskStateV1 current_task_state() const;
    [[nodiscard]] QString approval_request_id() const;
    [[nodiscard]] QString status_text() const;
    [[nodiscard]] QJsonObject diagnostic_timing() const;

    [[nodiscard]] redclaw::protocol::AgentProviderKindV1 selected_provider() const;
    [[nodiscard]] QString selected_model() const;
    [[nodiscard]] QString selected_project_id() const;
    [[nodiscard]] redclaw::protocol::AgentWorkDirectoryModeV1 selected_work_directory_mode() const;
    [[nodiscard]] bool selected_project_is_git() const;

    [[nodiscard]] std::vector<AgentProviderView> providers() const;
    [[nodiscard]] std::vector<AgentProjectView> projects() const;
    [[nodiscard]] int provider_count() const;
    [[nodiscard]] int project_count() const;
    [[nodiscard]] bool approval_pending() const;

    [[nodiscard]] bool select_provider(redclaw::protocol::AgentProviderKindV1 provider);
    void select_isolated_worktree();
    void set_instruction_text(const QString& text);
    [[nodiscard]] QString instruction_text() const;
    [[nodiscard]] bool trigger_submit(AgentSubmitMode mode, QString* error = nullptr);
    [[nodiscard]] bool trigger_interrupt(QString* error = nullptr);
    [[nodiscard]] bool trigger_approval(
        redclaw::protocol::AgentApprovalDecisionV1 decision,
        QString* error = nullptr);

    void set_status_text(const QString& text, bool error = false);
    [[nodiscard]] QStringList bounded_event_tail(int maximum_lines) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::ui
