#include "ui/agent_settings_dialog.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <utility>
#include <vector>

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include "redclaw/agent/agent_providers.h"

namespace redclaw::ui {
namespace {

using redclaw::protocol::AgentProviderKindV1;
using redclaw::protocol::AgentProviderReadinessV1;

QString provider_name(AgentProviderKindV1 provider) {
    return provider == AgentProviderKindV1::kCodex ? "Codex" : "Cursor / Grok";
}

std::string provider_executable(AgentProviderKindV1 provider) {
    return provider == AgentProviderKindV1::kCodex ? "codex" : "cursor-agent";
}

QString clean_first_line(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()))
        .section('\n', 0, 0).trimmed().left(160);
}

redclaw::agent::AgentProviderProbe probe_provider(AgentProviderKindV1 provider) {
    auto instance = provider == AgentProviderKindV1::kCodex
        ? redclaw::agent::make_codex_app_server_provider()
        : redclaw::agent::make_cursor_agent_provider();
    auto result = instance->probe(true);
    instance->shutdown();
    return result;
}

struct ProjectInspection {
    QString root;
    QString display;
    bool git_repository = false;
};

ProjectInspection inspect_project(const QString& selected) {
    QFileInfo info(selected);
    ProjectInspection inspection;
    inspection.root = info.canonicalFilePath();
    if (inspection.root.isEmpty()) {
        inspection.root = info.absoluteFilePath();
    }
    inspection.display = QFileInfo(inspection.root).fileName() + " — directory";

    QProcess git;
    git.start("git", {"-C", inspection.root, "rev-parse", "--show-toplevel"});
    if (!git.waitForFinished(3000) || git.exitStatus() != QProcess::NormalExit
        || git.exitCode() != 0) {
        return inspection;
    }
    const QString git_root = QString::fromUtf8(git.readAllStandardOutput()).trimmed();
    const QFileInfo git_info(git_root);
    const QString canonical_git_root = git_info.canonicalFilePath();
    if (!canonical_git_root.isEmpty()) {
        inspection.root = canonical_git_root;
    }
    inspection.git_repository = true;

    QProcess status;
    status.start("git", {"-C", inspection.root, "status", "--porcelain"});
    const bool status_ready = status.waitForFinished(3000)
        && status.exitStatus() == QProcess::NormalExit && status.exitCode() == 0;
    const bool dirty = status_ready && !status.readAllStandardOutput().trimmed().isEmpty();
    inspection.display = QFileInfo(inspection.root).fileName()
        + (dirty ? " — Git dirty" : " — Git clean");
    return inspection;
}

}  // namespace

struct AgentSettingsDialog::Impl {
    struct ProviderRow {
        AgentProviderKindV1 provider = AgentProviderKindV1::kNone;
        QLabel* status = nullptr;
        QLabel* version = nullptr;
        QLabel* models = nullptr;
        QPushButton* login = nullptr;
        QPushButton* logout = nullptr;
        redclaw::agent::AgentProviderProbe probe;
    };

    AgentSettingsDialog* owner = nullptr;
    QSettings* settings = nullptr;
    std::array<ProviderRow, 2> providers;
    QListWidget* projects = nullptr;
    QPushButton* add_project = nullptr;
    QPushButton* remove_project = nullptr;
    QPushButton* refresh = nullptr;
    QPushButton* cancel_operation = nullptr;
    QDialogButtonBox* close_buttons = nullptr;
    QLabel* operation_status = nullptr;
    QPlainTextEdit* account_output = nullptr;
    QProcess* account_process = nullptr;
    QTimer* account_timeout = nullptr;
    AgentProviderKindV1 active_provider = AgentProviderKindV1::kNone;
    std::string active_action;
    bool account_cancelled = false;
    bool account_timed_out = false;
    bool runtime_active = false;
    int pending_probes = 0;

    static constexpr qsizetype kMaxAccountOutputCharacters = 32 * 1024;

    ProviderRow* row(AgentProviderKindV1 provider) {
        for (auto& candidate : providers) {
            if (candidate.provider == provider) {
                return &candidate;
            }
        }
        return nullptr;
    }

    void update_controls() {
        const bool account_running = account_process->state() != QProcess::NotRunning;
        refresh->setEnabled(!account_running && pending_probes == 0);
        add_project->setEnabled(!runtime_active && !account_running);
        remove_project->setEnabled(
            !runtime_active && !account_running && projects->currentItem() != nullptr);
        cancel_operation->setEnabled(account_running);
        close_buttons->setEnabled(!account_running);
        for (auto& provider : providers) {
            provider.login->setEnabled(!runtime_active && !account_running
                && provider.probe.readiness != AgentProviderReadinessV1::kReady);
            provider.logout->setEnabled(!runtime_active && !account_running
                && provider.probe.readiness == AgentProviderReadinessV1::kReady);
        }
        if (runtime_active && !account_running) {
            operation_status->setText(
                "Stop the active Runtime before changing Agent accounts or projects.");
        }
    }

    void apply_probe(const redclaw::agent::AgentProviderProbe& probe) {
        auto* provider = row(probe.provider);
        if (provider == nullptr) {
            return;
        }
        provider->probe = probe;
        const auto readiness = redclaw::protocol::to_string(probe.readiness);
        provider->status->setText(QString::fromLatin1(
            readiness.data(), static_cast<qsizetype>(readiness.size())));
        provider->status->setToolTip(QString::fromStdString(probe.unavailable_reason));
        provider->version->setText(probe.version.empty() ? "—" : clean_first_line(probe.version));
        QStringList models;
        for (const auto& model : probe.models) {
            models.push_back(QString::fromStdString(model));
        }
        provider->models->setText(models.isEmpty() ? "Host default / unavailable"
                                                   : models.join(", "));
        update_controls();
    }

    void refresh_all() {
        if (pending_probes != 0 || account_process->state() != QProcess::NotRunning) {
            return;
        }
        pending_probes = static_cast<int>(providers.size());
        operation_status->setText("Checking local Agent readiness…");
        update_controls();
        for (const auto& provider : providers) {
            auto* watcher = new QFutureWatcher<redclaw::agent::AgentProviderProbe>(owner);
            QObject::connect(watcher, &QFutureWatcherBase::finished, owner,
                [this, watcher]() {
                    apply_probe(watcher->result());
                    watcher->deleteLater();
                    --pending_probes;
                    if (pending_probes == 0) {
                        operation_status->setText(any_ready()
                            ? "At least one local Agent provider is ready."
                            : "No local Agent provider is ready. Login or inspect the status tooltip.");
                    }
                    update_controls();
                });
            watcher->setFuture(QtConcurrent::run([kind = provider.provider]() {
                return probe_provider(kind);
            }));
        }
    }

    bool any_ready() const {
        return std::any_of(providers.begin(), providers.end(), [](const auto& provider) {
            return provider.probe.readiness == AgentProviderReadinessV1::kReady;
        });
    }

    [[nodiscard]] bool account_running() const {
        return account_process->state() != QProcess::NotRunning;
    }

    void append_account_output(const QByteArray& bytes) {
        QString cleaned;
        const QString decoded = QString::fromUtf8(bytes);
        cleaned.reserve(decoded.size());
        for (const QChar character : decoded) {
            if (character == '\n' || character == '\t' || character.unicode() >= 0x20U) {
                cleaned.push_back(character);
            }
        }
        if (cleaned.isEmpty()) {
            return;
        }

        QString combined = account_output->toPlainText() + cleaned;
        if (combined.size() > kMaxAccountOutputCharacters) {
            static const QString omitted = "[Earlier account output omitted]\n";
            combined = omitted + combined.right(kMaxAccountOutputCharacters - omitted.size());
        }
        account_output->setPlainText(combined);
        account_output->moveCursor(QTextCursor::End);
    }

    [[nodiscard]] QString account_operation_name() const {
        const QString action = active_action == "logout" ? "logout" : "login";
        return provider_name(active_provider) + " " + action;
    }

    [[nodiscard]] QString account_recovery_hint() const {
        if (active_action != "login") {
            return "Retry from Development Agent Settings.";
        }
        if (active_provider == AgentProviderKindV1::kCodex) {
            return "Run 'codex login' (or 'codex login --device-auth') in a visible PowerShell window, then click Refresh.";
        }
        return "Run 'cursor-agent login' in a visible PowerShell window, then click Refresh.";
    }

    void prevent_account_operation_hide() {
        operation_status->setText(
            "An Agent account operation is still running. Use Cancel account operation before closing.");
    }

    void persist_projects() {
        QStringList roots;
        for (int index = 0; index < projects->count(); ++index) {
            roots.push_back(projects->item(index)->data(Qt::UserRole).toString());
        }
        settings->setValue("host/agent_project_roots", roots);
    }

    void add_project_root(const QString& root) {
        const ProjectInspection inspection = inspect_project(root);
        if (inspection.root.isEmpty()) {
            return;
        }
        for (int index = 0; index < projects->count(); ++index) {
            if (projects->item(index)->data(Qt::UserRole).toString().compare(
                    inspection.root, Qt::CaseInsensitive) == 0) {
                return;
            }
        }
        auto* item = new QListWidgetItem(inspection.display, projects);
        item->setData(Qt::UserRole, inspection.root);
        item->setData(Qt::UserRole + 1, inspection.git_repository);
        item->setToolTip(inspection.root);
    }

    void start_account_operation(
        AgentProviderKindV1 provider,
        const std::string& action) {
        if (runtime_active || account_process->state() != QProcess::NotRunning) {
            return;
        }
        redclaw::agent::AgentResolvedCommand command;
        std::string resolve_error;
        std::vector<std::string> requested_arguments;
        if (provider == AgentProviderKindV1::kCodex) {
            requested_arguments = {"-c", "service_tier=\"fast\"", action};
        } else {
            requested_arguments = {action};
        }
        if (!redclaw::agent::resolve_agent_command(
                provider_executable(provider), requested_arguments, &command, &resolve_error)) {
            operation_status->setText(QString::fromStdString(resolve_error));
            return;
        }
        active_provider = provider;
        active_action = action;
        account_cancelled = false;
        account_timed_out = false;
        account_output->clear();
        account_process->setProgram(QString::fromStdWString(command.application.wstring()));
        QStringList process_arguments;
        for (const auto& argument : command.arguments) {
            process_arguments.push_back(QString::fromStdString(argument));
        }
        account_process->setArguments(process_arguments);
        account_process->setProcessChannelMode(QProcess::MergedChannels);
        operation_status->setText("Starting " + account_operation_name() + "…");
        account_process->start();
        update_controls();
    }
};

AgentSettingsDialog::AgentSettingsDialog(QSettings* settings, QWidget* parent)
    : QDialog(parent), impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    impl_->settings = settings;
    setWindowTitle("Development Agent Settings");
    resize(760, 620);

    auto* layout = new QVBoxLayout(this);
    auto* provider_group = new QGroupBox("Local provider accounts", this);
    auto* provider_layout = new QFormLayout(provider_group);
    impl_->providers[0].provider = AgentProviderKindV1::kCodex;
    impl_->providers[1].provider = AgentProviderKindV1::kCursor;
    for (auto& provider : impl_->providers) {
        auto* row = new QWidget(provider_group);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        provider.status = new QLabel("probe_failed", row);
        provider.version = new QLabel("—", row);
        provider.models = new QLabel("—", row);
        provider.models->setWordWrap(true);
        provider.login = new QPushButton("Login", row);
        provider.logout = new QPushButton("Logout", row);
        const bool is_codex = provider.provider == AgentProviderKindV1::kCodex;
        provider.status->setObjectName(is_codex ? "codexProviderStatus" : "cursorProviderStatus");
        provider.login->setObjectName(is_codex ? "codexLoginButton" : "cursorLoginButton");
        provider.logout->setObjectName(is_codex ? "codexLogoutButton" : "cursorLogoutButton");
        row_layout->addWidget(provider.status);
        row_layout->addWidget(provider.version);
        row_layout->addWidget(provider.models, 1);
        row_layout->addWidget(provider.login);
        row_layout->addWidget(provider.logout);
        provider_layout->addRow(provider_name(provider.provider), row);
        QObject::connect(provider.login, &QPushButton::clicked, this,
            [this, kind = provider.provider]() {
                impl_->start_account_operation(kind, "login");
            });
        QObject::connect(provider.logout, &QPushButton::clicked, this,
            [this, kind = provider.provider]() {
                if (QMessageBox::question(this, "Logout Agent account",
                        "Remove the locally stored " + provider_name(kind)
                            + " authentication credentials?") == QMessageBox::Yes) {
                    impl_->start_account_operation(kind, "logout");
                }
            });
    }
    layout->addWidget(provider_group);

    auto* project_group = new QGroupBox("Registered Host projects", this);
    auto* project_layout = new QVBoxLayout(project_group);
    impl_->projects = new QListWidget(project_group);
    project_layout->addWidget(impl_->projects);
    auto* project_actions = new QHBoxLayout();
    impl_->add_project = new QPushButton("Add project", project_group);
    impl_->remove_project = new QPushButton("Remove selected", project_group);
    project_actions->addWidget(impl_->add_project);
    project_actions->addWidget(impl_->remove_project);
    project_actions->addStretch(1);
    project_layout->addLayout(project_actions);
    layout->addWidget(project_group, 1);
    for (const auto& root : settings->value("host/agent_project_roots").toStringList()) {
        impl_->add_project_root(root);
    }
    QObject::connect(impl_->add_project, &QPushButton::clicked, this, [this]() {
        const QString selected = QFileDialog::getExistingDirectory(
            this, "Register development Agent project");
        if (!selected.isEmpty()) {
            impl_->add_project_root(selected);
            impl_->persist_projects();
            impl_->update_controls();
        }
    });
    QObject::connect(impl_->remove_project, &QPushButton::clicked, this, [this]() {
        delete impl_->projects->takeItem(impl_->projects->currentRow());
        impl_->persist_projects();
        impl_->update_controls();
    });
    QObject::connect(impl_->projects, &QListWidget::currentItemChanged, this,
        [this]() { impl_->update_controls(); });

    auto* bottom = new QHBoxLayout();
    impl_->refresh = new QPushButton("Refresh", this);
    impl_->cancel_operation = new QPushButton("Cancel account operation", this);
    impl_->cancel_operation->setObjectName("agentAccountCancelButton");
    impl_->operation_status = new QLabel(this);
    impl_->operation_status->setObjectName("agentAccountOperationStatus");
    impl_->operation_status->setWordWrap(true);
    impl_->close_buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    bottom->addWidget(impl_->refresh);
    bottom->addWidget(impl_->cancel_operation);
    bottom->addWidget(impl_->operation_status, 1);
    bottom->addWidget(impl_->close_buttons);
    layout->addLayout(bottom);

    auto* output_label = new QLabel("Local account command output", this);
    impl_->account_output = new QPlainTextEdit(this);
    impl_->account_output->setObjectName("agentAccountOperationOutput");
    impl_->account_output->setReadOnly(true);
    impl_->account_output->setMaximumHeight(130);
    impl_->account_output->setPlaceholderText(
        "Login and logout output appears here. It is kept local and is not written to the Runtime log.");
    layout->addWidget(output_label);
    layout->addWidget(impl_->account_output);

    impl_->account_process = new QProcess(this);
    impl_->account_timeout = new QTimer(this);
    impl_->account_timeout->setSingleShot(true);
    QObject::connect(impl_->account_process, &QProcess::started, this, [this]() {
        const bool login = impl_->active_action == "login";
        impl_->account_timeout->start(login ? 10 * 60 * 1000 : 10 * 1000);
        impl_->operation_status->setText(login
            ? impl_->account_operation_name()
                + " started. Complete the browser sign-in. If no browser opens, use the output below "
                  "or cancel it. " + impl_->account_recovery_hint()
            : impl_->account_operation_name() + " is running.");
        impl_->update_controls();
    });
    QObject::connect(impl_->account_process, &QProcess::readyRead, this, [this]() {
        impl_->append_account_output(impl_->account_process->readAll());
    });
    QObject::connect(impl_->account_process, &QProcess::errorOccurred, this,
        [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) {
                return;
            }
            impl_->account_timeout->stop();
            const QString operation = impl_->account_operation_name();
            const QString recovery = impl_->account_recovery_hint();
            impl_->append_account_output(impl_->account_process->errorString().toUtf8() + '\n');
            impl_->active_provider = AgentProviderKindV1::kNone;
            impl_->active_action.clear();
            impl_->operation_status->setText(
                "Failed to start " + operation + ": " + impl_->account_process->errorString()
                + ". " + recovery);
            impl_->update_controls();
        });
    QObject::connect(impl_->account_process,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int exit_code, QProcess::ExitStatus exit_status) {
            impl_->append_account_output(impl_->account_process->readAll());
            impl_->account_timeout->stop();
            const QString operation = impl_->account_operation_name();
            const QString recovery = impl_->account_recovery_hint();
            const bool cancelled = impl_->account_cancelled;
            const bool timed_out = impl_->account_timed_out;
            impl_->active_provider = AgentProviderKindV1::kNone;
            impl_->active_action.clear();
            impl_->account_cancelled = false;
            impl_->account_timed_out = false;
            impl_->update_controls();

            if (timed_out) {
                impl_->operation_status->setText(
                    operation + " timed out and was stopped. " + recovery);
                return;
            }
            if (cancelled) {
                impl_->operation_status->setText(operation + " was cancelled.");
                return;
            }
            if (exit_status != QProcess::NormalExit) {
                impl_->operation_status->setText(
                    operation + " stopped unexpectedly. Review the output below. " + recovery);
                return;
            }
            if (exit_code != 0) {
                impl_->operation_status->setText(
                    operation + " exited with code " + QString::number(exit_code)
                    + ". Review the output below. " + recovery);
                return;
            }

            impl_->operation_status->setText(operation + " finished; checking readiness…");
            impl_->refresh_all();
        });
    QObject::connect(impl_->account_timeout, &QTimer::timeout, this, [this]() {
        impl_->account_timed_out = true;
        impl_->operation_status->setText(
            impl_->account_operation_name() + " timed out; stopping the process…");
        impl_->account_process->kill();
    });
    QObject::connect(impl_->refresh, &QPushButton::clicked, this,
        [this]() { impl_->refresh_all(); });
    QObject::connect(impl_->cancel_operation, &QPushButton::clicked, this, [this]() {
        impl_->account_cancelled = true;
        impl_->operation_status->setText(
            "Cancelling " + impl_->account_operation_name() + "…");
        impl_->account_process->kill();
    });
    QObject::connect(impl_->close_buttons, &QDialogButtonBox::rejected,
        this, [this]() { reject(); });
    impl_->refresh_all();
}

AgentSettingsDialog::~AgentSettingsDialog() = default;

void AgentSettingsDialog::closeEvent(QCloseEvent* event) {
    if (impl_->account_running()) {
        impl_->prevent_account_operation_hide();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void AgentSettingsDialog::reject() {
    if (impl_->account_running()) {
        impl_->prevent_account_operation_hide();
        return;
    }
    QDialog::reject();
}

void AgentSettingsDialog::set_runtime_active(bool active) {
    impl_->runtime_active = active;
    impl_->update_controls();
}

void AgentSettingsDialog::refresh_provider_status() {
    impl_->refresh_all();
}

bool AgentSettingsDialog::replace_project_roots_for_automation(
    const QStringList& roots,
    QString* error_detail) {
    if (impl_->runtime_active || impl_->account_process->state() != QProcess::NotRunning) {
        if (error_detail != nullptr) {
            *error_detail = "Agent project registration is locked while Runtime or login is active.";
        }
        return false;
    }
    for (const auto& root : roots) {
        const QFileInfo info(root);
        if (!info.exists() || !info.isDir()) {
            if (error_detail != nullptr) {
                *error_detail = "Every automated Agent project root must be an existing directory.";
            }
            return false;
        }
    }
    impl_->projects->clear();
    for (const auto& root : roots) {
        impl_->add_project_root(root);
    }
    if (impl_->projects->count() != roots.size()) {
        if (error_detail != nullptr) {
            *error_detail = "Automated Agent project roots contain duplicates or invalid entries.";
        }
        return false;
    }
    impl_->persist_projects();
    impl_->update_controls();
    if (error_detail != nullptr) {
        error_detail->clear();
    }
    return true;
}

QStringList AgentSettingsDialog::project_roots() const {
    QStringList roots;
    for (int index = 0; index < impl_->projects->count(); ++index) {
        roots.push_back(impl_->projects->item(index)->data(Qt::UserRole).toString());
    }
    return roots;
}

bool AgentSettingsDialog::any_provider_ready() const {
    return impl_->any_ready();
}

QString AgentSettingsDialog::readiness_summary() const {
    QStringList states;
    for (const auto& provider : impl_->providers) {
        states.push_back(provider_name(provider.provider) + ": " + provider.status->text());
    }
    return states.join("; ");
}

}  // namespace redclaw::ui
