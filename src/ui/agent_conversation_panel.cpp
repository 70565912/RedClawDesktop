#include "ui/agent_conversation_panel.h"
#include "ui/agent_conversation_viewport.h"
#include "ui/agent_message_text_view.h"
#include "ui/gui_latency_probe.h"

#include <algorithm>
#include <map>
#include <utility>

#include <QAbstractItemView>
#include <QComboBox>
#include <QEvent>
#include <QElapsedTimer>
#include <QFrame>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyle>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace redclaw::ui {

namespace {

constexpr int kMaximumConversationItems = 300;
constexpr int kMaximumEventTailLines = 1000;
constexpr int kMaximumInstructionBytes =
    static_cast<int>(redclaw::protocol::kMaxAgentInstructionBytes);

QString protocol_name(auto value) {
    const auto name = redclaw::protocol::to_string(value);
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

bool task_is_running(redclaw::protocol::AgentTaskStateV1 state) {
    return state == redclaw::protocol::AgentTaskStateV1::kQueued
        || state == redclaw::protocol::AgentTaskStateV1::kStarting
        || state == redclaw::protocol::AgentTaskStateV1::kRunning
        || state == redclaw::protocol::AgentTaskStateV1::kAwaitingApproval;
}

bool activity_is_failure(const QString& kind) {
    return kind.contains("error", Qt::CaseInsensitive)
        || kind.contains("fail", Qt::CaseInsensitive)
        || kind.contains("reject", Qt::CaseInsensitive)
        || kind.contains("timeout", Qt::CaseInsensitive)
        || kind.contains("exit", Qt::CaseInsensitive);
}

bool is_agent_text_delta(const QString& kind) {
    return kind == "text_delta"
        || kind.contains("agentMessage/delta", Qt::CaseInsensitive)
        || kind.contains("assistant", Qt::CaseInsensitive);
}

QString activity_title(const QString& kind, int count, bool failed) {
    QString normalized = kind;
    normalized.replace('/', " · ");
    normalized.replace('_', ' ');
    if (normalized.isEmpty()) {
        normalized = "Agent activity";
    }
    if (count > 1) {
        normalized += QString(" (%1)").arg(count);
    }
    return failed ? QString("Failed · %1").arg(normalized) : normalized;
}

class ComposerEdit final : public QPlainTextEdit {
public:
    using SubmitHandler = std::function<void()>;

    explicit ComposerEdit(QWidget* parent = nullptr) : QPlainTextEdit(parent) {}

    void set_submit_handler(SubmitHandler handler) {
        submit_handler_ = std::move(handler);
    }

protected:
    bool event(QEvent* event) override {
        if (event != nullptr && event->type() == QEvent::InputMethod) {
            const auto* input = static_cast<QInputMethodEvent*>(event);
            composing_ = !input->preeditString().isEmpty();
        }
        return QPlainTextEdit::event(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event != nullptr
            && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && !event->modifiers().testFlag(Qt::ShiftModifier)
            && !event->modifiers().testFlag(Qt::ControlModifier)
            && !event->modifiers().testFlag(Qt::AltModifier)
            && !composing_) {
            if (submit_handler_) {
                submit_handler_();
            }
            event->accept();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }

private:
    SubmitHandler submit_handler_;
    bool composing_ = false;
};

class ContextPopupFrame final : public QFrame {
public:
    explicit ContextPopupFrame(QWidget* parent)
        : QFrame(parent, Qt::Popup) {}

    void set_hidden_handler(std::function<void()> handler) {
        hidden_handler_ = std::move(handler);
    }

protected:
    void hideEvent(QHideEvent* event) override {
        QFrame::hideEvent(event);
        if (hidden_handler_) {
            hidden_handler_();
        }
    }

private:
    std::function<void()> hidden_handler_;
};


enum class ConversationRole {
    kUser,
    kAgent,
    kActivity,
    kSystem,
};

struct ConversationItem {
    ConversationRole role = ConversationRole::kSystem;
    QString kind;
    QString text;
    int activity_count = 1;
    bool failed = false;
    bool mergeable = false;
    std::uint64_t id = 0;
};

class MeasuredTaskStatusLabel : public QLabel {
public:
    using QLabel::QLabel;
    std::function<void()> painted;
protected:
    void paintEvent(QPaintEvent* event) override {
        QLabel::paintEvent(event);
        if (painted) painted();
    }
};

struct TaskConversation {
    redclaw::protocol::AgentTaskStateV1 state =
        redclaw::protocol::AgentTaskStateV1::kIdle;
    std::vector<ConversationItem> items;
    std::uint64_t last_event_sequence = 0;
    QString decided_approval;
    std::uint64_t first_output_us = 0, terminal_consumed_us = 0, terminal_painted_us = 0;
    bool output_gap = false;
};

}  // namespace

struct AgentConversationPanel::Impl {
    AgentConversationPanel* owner = nullptr;
    QSettings* settings = nullptr;

    QLabel* connection_status = nullptr;
    QComboBox* task_selector = nullptr;
    QPushButton* new_task = nullptr;
    QToolButton* context_toggle = nullptr;
    QToolButton* collapse = nullptr;
    QWidget* context_box = nullptr;
    QComboBox* provider = nullptr;
    QComboBox* model = nullptr;
    QComboBox* project = nullptr;
    QComboBox* work_mode = nullptr;
    QLabel* status = nullptr;
    AgentConversationViewport* scroll = nullptr;
    QFrame* approval = nullptr;
    QLabel* approval_text = nullptr;
    QPushButton* approve = nullptr;
    QPushButton* reject = nullptr;
    ComposerEdit* composer = nullptr;
    QLabel* composer_hint = nullptr;
    QPushButton* send = nullptr;
    QPushButton* interrupt = nullptr;

    SubmitCallback submit_callback;
    InterruptCallback interrupt_callback;
    ApprovalCallback approval_callback;
    TaskSelectedCallback task_selected_callback;
    CollapseCallback collapse_callback;

    std::map<int, AgentProviderView> provider_views;
    std::map<QString, AgentProjectView> project_views;
    std::map<QString, TaskConversation> tasks;
    QString current_task_id;
    QString current_approval_request_id;
    QString last_valid_instruction;
    QStringList event_tail;
    bool authorized = false;
    bool channel_open = false;
    bool sync_required = false;
    bool rebuilding = false;
    bool rebuild_queued = false;
    std::uint64_t next_item_id = 1;
    QString rendered_task;

    explicit Impl(AgentConversationPanel* panel, QSettings* value)
        : owner(panel), settings(value) {
        build_ui();
        refresh_context_summary();
        refresh_controls();
    }

    void build_ui() {
        owner->setObjectName("agentConversationPanel");
        owner->setMinimumWidth(320);
        owner->setMaximumWidth(560);
        owner->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

        auto* root = new QVBoxLayout(owner);
        root->setSizeConstraint(QLayout::SetNoConstraint);
        root->setContentsMargins(6, 6, 6, 6);
        root->setSpacing(6);

        auto* header = new QVBoxLayout();
        header->setSpacing(7);
        auto* title_row = new QHBoxLayout();
        title_row->setSpacing(7);
        auto* title = new QLabel("Agent", owner);
        title->setObjectName("agentPanelTitle");
        connection_status = new QLabel("Offline", owner);
        connection_status->setObjectName("agentConnectionState");
        task_selector = new QComboBox(owner);
        task_selector->setObjectName("agentTaskSelector");
        task_selector->setMinimumWidth(120);
        task_selector->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        task_selector->setMinimumContentsLength(14);
        new_task = new QPushButton("New task", owner);
        new_task->setObjectName("agentPanelSecondaryAction");
        context_toggle = new QToolButton(owner);
        context_toggle->setObjectName("agentContextToggle");
        context_toggle->setCheckable(true);
        context_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        context_toggle->setArrowType(Qt::RightArrow);
        context_toggle->setMinimumWidth(0);
        context_toggle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        collapse = new QToolButton(owner);
        collapse->setObjectName("agentPanelCollapse");
        collapse->setIcon(owner->style()->standardIcon(QStyle::SP_ArrowRight));
        collapse->setToolTip("Hide Agent panel");
        title_row->addWidget(title);
        title_row->addWidget(connection_status);
        title_row->addStretch(1);
        title_row->addWidget(collapse);
        auto* task_row = new QHBoxLayout();
        task_row->setSpacing(7);
        task_row->addWidget(task_selector, 1);
        task_row->addWidget(new_task);
        header->addLayout(title_row);
        header->addLayout(task_row);
        root->addLayout(header);
        root->addWidget(context_toggle);

        auto* context_popup = new ContextPopupFrame(owner);
        context_box = context_popup;
        context_box->setObjectName("agentContextBox");
        auto* context_layout = new QGridLayout(context_box);
        context_layout->setContentsMargins(8, 8, 8, 8);
        context_layout->setHorizontalSpacing(8);
        context_layout->setVerticalSpacing(6);
        provider = new QComboBox(context_box);
        model = new QComboBox(context_box);
        project = new QComboBox(context_box);
        work_mode = new QComboBox(context_box);
        work_mode->addItem("Isolated worktree", "isolated_worktree");
        work_mode->addItem("Direct workspace", "direct_workspace");
        context_layout->addWidget(new QLabel("Provider", context_box), 0, 0);
        context_layout->addWidget(provider, 0, 1);
        context_layout->addWidget(new QLabel("Model", context_box), 1, 0);
        context_layout->addWidget(model, 1, 1);
        context_layout->addWidget(new QLabel("Project", context_box), 2, 0);
        context_layout->addWidget(project, 2, 1);
        context_layout->addWidget(new QLabel("Workspace", context_box), 3, 0);
        context_layout->addWidget(work_mode, 3, 1);
        context_layout->setColumnStretch(1, 1);
        context_box->hide();
        context_popup->set_hidden_handler([this]() {
            const QSignalBlocker blocker(context_toggle);
            context_toggle->setChecked(false);
            context_toggle->setArrowType(Qt::RightArrow);
        });

        auto* measured_status = new MeasuredTaskStatusLabel("Agent channel not ready", owner);
        measured_status->painted = [this] {
            const auto found = tasks.find(current_task_id);
            if (found != tasks.end() && found->second.terminal_consumed_us && !found->second.terminal_painted_us) {
                found->second.terminal_painted_us = gui_monotonic_us();
                if (auto* probe = gui_latency_probe()) probe->finish_task(current_task_id);
            }
        };
        status = measured_status;
        status->setObjectName("agentPanelStatus");
        status->setWordWrap(true);
        root->addWidget(status);

        scroll = new AgentConversationViewport(owner);
        scroll->setObjectName("agentConversationScroll");
        root->addWidget(scroll, 1);

        approval = new QFrame(owner);
        approval->setObjectName("agentApprovalBanner");
        auto* approval_layout = new QVBoxLayout(approval);
        approval_layout->setContentsMargins(10, 9, 10, 9);
        approval_layout->setSpacing(7);
        approval_text = new QLabel(approval);
        approval_text->setWordWrap(true);
        auto* approval_actions = new QHBoxLayout();
        approve = new QPushButton("Approve", approval);
        approve->setObjectName("agentApproveAction");
        reject = new QPushButton("Reject", approval);
        reject->setObjectName("agentPanelSecondaryAction");
        approval_actions->addWidget(approve);
        approval_actions->addWidget(reject);
        approval_actions->addStretch(1);
        approval_layout->addWidget(approval_text);
        approval_layout->addLayout(approval_actions);
        approval->hide();
        root->addWidget(approval);

        composer = new ComposerEdit(owner);
        composer->setObjectName("agentComposer");
        composer->setPlaceholderText("Send instructions to the remote Agent…");
        composer->setFixedHeight(88);
        root->addWidget(composer);

        auto* composer_row = new QHBoxLayout();
        composer_hint = new QLabel(owner);
        composer_hint->setObjectName("agentComposerHint");
        composer_hint->setMinimumWidth(0);
        composer_hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        interrupt = new QPushButton("Interrupt", owner);
        interrupt->setObjectName("agentPanelSecondaryAction");
        send = new QPushButton("Send", owner);
        send->setObjectName("agentSendAction");
        composer_row->addWidget(composer_hint, 1);
        composer_row->addWidget(interrupt);
        composer_row->addWidget(send);
        root->addLayout(composer_row);

        QObject::connect(context_toggle, &QToolButton::toggled, owner, [this](bool shown) {
            context_toggle->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
            if (!shown) {
                context_box->hide();
                return;
            }
            context_box->adjustSize();
            context_box->setFixedWidth((std::max)(220, owner->width() - 12));
            context_box->move(context_toggle->mapToGlobal(
                QPoint(0, context_toggle->height() + 4)));
            context_box->show();
            context_box->raise();
        });
        QObject::connect(collapse, &QToolButton::clicked, owner, [this]() {
            if (collapse_callback) {
                collapse_callback();
            }
        });
        QObject::connect(new_task, &QPushButton::clicked, owner, [this]() {
            set_current_task({}, redclaw::protocol::AgentTaskStateV1::kIdle, true);
            set_status("Ready for a new task", false);
            composer->setFocus();
        });
        QObject::connect(task_selector, qOverload<int>(&QComboBox::currentIndexChanged),
            owner, [this](int index) {
                if (rebuilding || index < 0) {
                    return;
                }
                const QString task_id = task_selector->itemData(index).toString();
                if (!task_id.isEmpty() && task_id != current_task_id) {
                    set_current_task(task_id, tasks[task_id].state, false);
                    if (task_selected_callback) {
                        task_selected_callback(task_id);
                    }
                }
            });
        QObject::connect(provider, qOverload<int>(&QComboBox::currentIndexChanged),
            owner, [this](int) {
                populate_models();
                persist_context();
                refresh_context_summary();
                refresh_controls();
            });
        QObject::connect(model, qOverload<int>(&QComboBox::currentIndexChanged),
            owner, [this](int) {
                persist_context();
                refresh_context_summary();
            });
        QObject::connect(project, qOverload<int>(&QComboBox::currentIndexChanged),
            owner, [this](int) {
                refresh_work_mode();
                persist_context();
                refresh_context_summary();
                refresh_controls();
            });
        QObject::connect(work_mode, qOverload<int>(&QComboBox::currentIndexChanged),
            owner, [this](int) {
                persist_context();
                refresh_context_summary();
            });
        const auto close_context_after_selection = [this](int) {
            context_toggle->setChecked(false);
        };
        QObject::connect(provider, qOverload<int>(&QComboBox::activated),
            owner, close_context_after_selection);
        QObject::connect(model, qOverload<int>(&QComboBox::activated),
            owner, close_context_after_selection);
        QObject::connect(project, qOverload<int>(&QComboBox::activated),
            owner, close_context_after_selection);
        QObject::connect(work_mode, qOverload<int>(&QComboBox::activated),
            owner, close_context_after_selection);
        QObject::connect(composer, &QPlainTextEdit::textChanged, owner, [this]() {
            const QString text = composer->toPlainText();
            const int bytes = text.toUtf8().size();
            if (bytes <= kMaximumInstructionBytes) {
                last_valid_instruction = text;
            } else {
                const QSignalBlocker blocker(composer);
                composer->setPlainText(last_valid_instruction);
                composer->moveCursor(QTextCursor::End);
                set_status("Instruction limit is 16384 UTF-8 bytes.", true);
            }
            refresh_controls();
        });
        composer->set_submit_handler([this]() { (void)trigger_submit(auto_submit_mode(), nullptr); });
        QObject::connect(send, &QPushButton::clicked, owner, [this]() {
            (void)trigger_submit(auto_submit_mode(), nullptr);
        });
        QObject::connect(interrupt, &QPushButton::clicked, owner, [this]() {
            (void)trigger_interrupt(nullptr);
        });
        QObject::connect(approve, &QPushButton::clicked, owner, [this]() {
            (void)trigger_approval(redclaw::protocol::AgentApprovalDecisionV1::kAccept, nullptr);
        });
        QObject::connect(reject, &QPushButton::clicked, owner, [this]() {
            (void)trigger_approval(redclaw::protocol::AgentApprovalDecisionV1::kReject, nullptr);
        });
    }

    AgentSubmitMode auto_submit_mode() const {
        if (current_task_id.isEmpty()) {
            return AgentSubmitMode::kCreateTask;
        }
        return task_is_running(current_task_state())
            ? AgentSubmitMode::kSteerTurn
            : AgentSubmitMode::kStartTurn;
    }

    redclaw::protocol::AgentTaskStateV1 current_task_state() const {
        const auto found = tasks.find(current_task_id);
        return found == tasks.end()
            ? redclaw::protocol::AgentTaskStateV1::kIdle
            : found->second.state;
    }

    void refresh_controls() {
        const int provider_key = provider->currentData(Qt::UserRole).toInt();
        const auto provider_found = provider_views.find(provider_key);
        const bool provider_ready = provider_found != provider_views.end()
            && provider_found->second.available;
        const bool catalog_ready = project->currentIndex() >= 0;
        const bool transport_ready = authorized && channel_open && !sync_required;
        const bool text_ready = !composer->toPlainText().trimmed().isEmpty()
            && composer->toPlainText().toUtf8().size() <= kMaximumInstructionBytes;
        send->setEnabled(transport_ready && provider_ready && catalog_ready && text_ready);
        composer->setEnabled(transport_ready && provider_ready && catalog_ready);
        new_task->setEnabled(transport_ready && provider_ready && catalog_ready);
        const bool running = task_is_running(current_task_state());
        interrupt->setVisible(running);
        interrupt->setEnabled(transport_ready && running && !current_task_id.isEmpty());
        send->setText(running && !current_task_id.isEmpty() ? "Steer" : "Send");
        const int bytes = composer->toPlainText().toUtf8().size();
        composer_hint->setText(QString("Enter send · Shift+Enter newline · %1 B")
            .arg(bytes));
    }

    void refresh_context_summary() {
        QStringList parts;
        if (provider->currentIndex() >= 0) {
            parts.push_back(provider->currentText());
        }
        if (model->currentIndex() >= 0) {
            parts.push_back(model->currentText());
        }
        if (project->currentIndex() >= 0) {
            parts.push_back(project->currentText());
        }
        parts.push_back(work_mode->currentText());
        context_toggle->setText(parts.isEmpty() ? "Choose Agent context" : parts.join(" · "));
    }

    void persist_context() {
        if (settings == nullptr) {
            return;
        }
        settings->setValue("controller/agent/provider", provider->currentData(Qt::UserRole));
        settings->setValue("controller/agent/model", model->currentData(Qt::UserRole));
        settings->setValue("controller/agent/project", project->currentData(Qt::UserRole));
        settings->setValue("controller/agent/work_mode", work_mode->currentData(Qt::UserRole));
    }

    void restore_context_selection() {
        if (settings == nullptr) {
            return;
        }
        const int provider_value = settings->value("controller/agent/provider", -1).toInt();
        const int provider_index = provider->findData(provider_value, Qt::UserRole);
        if (provider_index >= 0) {
            provider->setCurrentIndex(provider_index);
        }
        const QString project_value = settings->value("controller/agent/project").toString();
        const int project_index = project->findData(project_value, Qt::UserRole);
        if (project_index >= 0) {
            project->setCurrentIndex(project_index);
        }
        const QString mode_value = settings->value(
            "controller/agent/work_mode", "isolated_worktree").toString();
        const int mode_index = work_mode->findData(mode_value, Qt::UserRole);
        if (mode_index >= 0) {
            work_mode->setCurrentIndex(mode_index);
        }
        populate_models();
        const QString model_value = settings->value("controller/agent/model").toString();
        const int model_index = model->findData(model_value, Qt::UserRole);
        if (model_index >= 0) {
            model->setCurrentIndex(model_index);
        }
    }

    void populate_models() {
        const QString preserved = model->currentData(Qt::UserRole).toString();
        const QSignalBlocker blocker(model);
        model->clear();
        const int provider_key = provider->currentData(Qt::UserRole).toInt();
        const auto found = provider_views.find(provider_key);
        if (found != provider_views.end()) {
            for (const QString& item : found->second.models) {
                model->addItem(item, item);
            }
            if (found->second.kind == redclaw::protocol::AgentProviderKindV1::kCodex) {
                model->addItem("Host default", QString());
            }
        }
        const int preserved_index = model->findData(preserved, Qt::UserRole);
        if (preserved_index >= 0) {
            model->setCurrentIndex(preserved_index);
        }
    }

    void refresh_work_mode() {
        const bool git_repository = project->currentData(Qt::UserRole + 1).toBool();
        if (auto* item_model = qobject_cast<QStandardItemModel*>(work_mode->model());
            item_model != nullptr && item_model->item(0) != nullptr) {
            item_model->item(0)->setEnabled(git_repository);
        }
        if (!git_repository && work_mode->currentData(Qt::UserRole).toString()
                == "isolated_worktree") {
            work_mode->setCurrentIndex(1);
        }
    }

    void set_status(const QString& text, bool error) {
        if (status->text() == text && status->property("error").toBool() == error) return;
        status->setText(text);
        if (!status->property("error").isValid() || status->property("error").toBool() != error) {
            status->setProperty("error", error);
            status->style()->unpolish(status);
            status->style()->polish(status);
        }
    }

    void set_current_task(
        const QString& task_id,
        redclaw::protocol::AgentTaskStateV1 state,
        bool user_initiated) {
        current_task_id = task_id;
        if (!task_id.isEmpty()) {
            auto& task = tasks[task_id];
            task.state = state;
            int index = task_selector->findData(task_id);
            if (index < 0) {
                const QString short_id = task_id.size() > 18 ? task_id.left(18) + "…" : task_id;
                task_selector->addItem(short_id, task_id);
                index = task_selector->count() - 1;
            }
            task_selector->setItemText(index, task_label(task_id, state));
            const QSignalBlocker blocker(task_selector);
            task_selector->setCurrentIndex(index);
        } else {
            const QSignalBlocker blocker(task_selector);
            task_selector->setCurrentIndex(-1);
        }
        current_approval_request_id.clear();
        approval->hide();
        rebuild_conversation();
        refresh_controls();
        if (!user_initiated && !task_id.isEmpty()) {
            set_status(QString("Task %1 · %2").arg(task_id, protocol_name(state)), false);
        }
    }

    QString task_label(
        const QString& task_id,
        redclaw::protocol::AgentTaskStateV1 state) const {
        const QString short_id = task_id.size() > 14 ? task_id.left(14) + "…" : task_id;
        return QString("%1 · %2").arg(short_id, protocol_name(state));
    }

    void update_task_selector(const QString& task_id, redclaw::protocol::AgentTaskStateV1 state) {
        int index = task_selector->findData(task_id);
        if (index < 0) {
            task_selector->addItem(task_label(task_id, state), task_id);
            index = task_selector->count() - 1;
        } else {
            const auto label = task_label(task_id, state);
            if (task_selector->itemText(index) != label) task_selector->setItemText(index, label);
        }
        if (task_id == current_task_id) {
            const QSignalBlocker blocker(task_selector);
            task_selector->setCurrentIndex(index);
        }
    }

    void append_tail(const QString& line) {
        if (line.isEmpty()) {
            return;
        }
        event_tail.push_back(line);
        while (event_tail.size() > kMaximumEventTailLines) {
            event_tail.removeFirst();
        }
    }

    void append_item(const QString& task_id, ConversationItem item) {
        if (task_id.isEmpty()) {
            return;
        }
        auto& task = tasks[task_id];
        bool merged = false;
        if (!task.items.empty() && item.mergeable && task.items.back().mergeable
            && item.role == ConversationRole::kAgent
            && task.items.back().role == ConversationRole::kAgent
            && task.items.back().kind == item.kind
            && task.items.back().text.size() + item.text.size() <= 8192) {
            task.items.back().text += item.text;
            merged = true;
        } else if (!task.items.empty() && item.mergeable && task.items.back().mergeable
            && item.role == ConversationRole::kActivity
            && task.items.back().role == ConversationRole::kActivity
            && !task.items.back().failed && !item.failed
            && task.items.back().text.size() + item.text.size() <= 8192) {
            auto& prior = task.items.back();
            ++prior.activity_count;
            if (!item.text.isEmpty()) {
                if (!prior.text.isEmpty()) {
                    prior.text += '\n';
                }
                prior.text += QString("[%1] %2").arg(item.kind, item.text);
            } else {
                if (!prior.text.isEmpty()) {
                    prior.text += '\n';
                }
                prior.text += QString("[%1]").arg(item.kind);
            }
            prior.kind = item.kind;
            merged = true;
        }
        if (!merged) {
            item.id = next_item_id++;
            task.items.push_back(std::move(item));
        }
        if (static_cast<int>(task.items.size()) > kMaximumConversationItems) {
            if (task.items.empty() || task.items.front().role != ConversationRole::kSystem
                || task.items.front().kind != "history_gap") {
                task.items.insert(task.items.begin(), ConversationItem{
                    .role = ConversationRole::kSystem,
                    .kind = "history_gap",
                    .text = "Older conversation items were removed from this in-memory view.",
                    .id = next_item_id++,
                });
            }
            while (static_cast<int>(task.items.size()) > kMaximumConversationItems) {
                task.items.erase(task.items.begin() + 1);
            }
        }
        if (task_id == current_task_id) {
            queue_conversation_rebuild();
        }
    }

    void queue_conversation_rebuild() {
        if (rebuild_queued) {
            return;
        }
        rebuild_queued = true;
        QTimer::singleShot(50, owner, [this]() {
            rebuild_queued = false;
            rebuild_conversation();
        });
    }

    QWidget* create_message_widget(const ConversationItem& item, QWidget* parent) {
        GuiLatencyScope timing(GuiStage::kAgentCreate);
        auto* row = new QWidget(parent);
        row->setProperty("messageId", QVariant::fromValue<qulonglong>(item.id));
        row->setProperty("messageRole", static_cast<int>(item.role));
        row->resize(std::max(80, parent->width() - 8), 32);
        row->setProperty("messageText", item.text);
        row->setProperty("messageStreaming", item.mergeable);
        row->setProperty("messageCount", item.activity_count);
        row->setProperty("messageKind", item.kind);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(0);

        if (item.role == ConversationRole::kUser) {
            row_layout->addStretch(1);
            auto* bubble = new QFrame(row);
            bubble->setObjectName("agentUserBubble");
            bubble->setMaximumWidth(430);
            auto* bubble_layout = new QVBoxLayout(bubble);
            bubble_layout->setContentsMargins(11, 8, 11, 8);
            auto* text = new QLabel(item.text, bubble);
            text->setWordWrap(true);
            text->setTextInteractionFlags(Qt::TextSelectableByMouse);
            bubble_layout->addWidget(text);
            row_layout->addWidget(bubble);
            return row;
        }

        if (item.role == ConversationRole::kAgent) {
            auto* browser = new AgentMessageTextView(row);
            browser->setObjectName("agentReplyText");
            row_layout->addWidget(browser, 1);
            row_layout->activate();
            browser->set_markdown(item.text, item.mergeable);
            return row;
        }

        if (item.role == ConversationRole::kActivity) {
            auto* activity = new QFrame(row);
            activity->setObjectName(item.failed ? "agentActivityFailed" : "agentActivityGroup");
            auto* activity_layout = new QVBoxLayout(activity);
            activity_layout->setContentsMargins(8, 5, 8, 5);
            activity_layout->setSpacing(4);
            auto* toggle = new QToolButton(activity);
            toggle->setCheckable(true);
            toggle->setChecked(item.failed);
            toggle->setArrowType(item.failed ? Qt::DownArrow : Qt::RightArrow);
            toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            toggle->setText(activity_title(item.kind, item.activity_count, item.failed));
            auto* details = new QLabel(item.text, activity);
            details->setObjectName("agentActivityDetails");
            details->setWordWrap(true);
            details->setTextInteractionFlags(Qt::TextSelectableByMouse);
            details->setVisible(item.failed);
            QObject::connect(toggle, &QToolButton::toggled, activity,
                [this, toggle, details](bool shown) {
                    toggle->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
                    details->setVisible(shown);
                    queue_conversation_rebuild();
                });
            activity_layout->addWidget(toggle);
            activity_layout->addWidget(details);
            row_layout->addWidget(activity, 1);
            return row;
        }

        auto* system = new QLabel(item.text, row);
        system->setObjectName(item.failed ? "agentSystemError" : "agentSystemMessage");
        system->setWordWrap(true);
        system->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row_layout->addWidget(system, 1);
        return row;
    }

    void rebuild_conversation() {
        if (rendered_task != current_task_id) {
            scroll->clear_rows();
            rendered_task = current_task_id;
        }
        const auto found = tasks.find(current_task_id);
        if (found == tasks.end() || found->second.items.empty()) {
            owner->setProperty("retained_reply_utf16_units", 0);
            scroll->set_rows({0}, [this](std::size_t, QWidget* parent) {
                return create_message_widget(ConversationItem{.text = current_task_id.isEmpty()
                    ? "Start a new task to talk to the remote Agent."
                    : "Waiting for the remote Agent…"}, parent);
            }, {});
            return;
        }
        std::vector<std::uint64_t> ids;
        qlonglong retained_reply_units = 0;
        for (const auto& item : found->second.items) {
            if (item.role == ConversationRole::kAgent) retained_reply_units += item.text.size();
        }
        owner->setProperty("retained_reply_utf16_units", retained_reply_units);
        for (const auto& item : found->second.items) ids.push_back(item.id);
        // The callback snapshot shares QString storage, not mutable vector indices
        // that history trimming can change before a queued scroll/resize event.
        auto items = std::make_shared<const std::vector<ConversationItem>>(found->second.items);
        scroll->set_rows(std::move(ids), [this, items](std::size_t index, QWidget* parent) {
            return create_message_widget(items->at(index), parent);
        }, [items](std::size_t index, QWidget* widget) {
            const auto& item = items->at(index);
            widget->setProperty("messageId", QVariant::fromValue<qulonglong>(item.id));
            if (widget->property("messageText").toString() == item.text
                && widget->property("messageStreaming").toBool() == item.mergeable
                && widget->property("messageCount").toInt() == item.activity_count
                && widget->property("messageKind").toString() == item.kind) return false;
            if (item.role == ConversationRole::kAgent) {
                if (auto* browser = widget->findChild<QWidget*>("agentReplyText"))
                    static_cast<AgentMessageTextView*>(browser)->set_markdown(item.text, item.mergeable);
            } else if (item.role == ConversationRole::kActivity) {
                if (auto* label = widget->findChild<QLabel*>("agentActivityDetails")) label->setText(item.text);
                if (auto* toggle = widget->findChild<QToolButton*>())
                    toggle->setText(activity_title(item.kind, item.activity_count, item.failed));
            }
            widget->setProperty("messageText", item.text);
            widget->setProperty("messageStreaming", item.mergeable);
            widget->setProperty("messageCount", item.activity_count);
            widget->setProperty("messageKind", item.kind);
            return true;
        }, [items](std::size_t index, QWidget* widget) {
            return items->at(index).role == ConversationRole::kAgent
                && widget->property("messageRole").toInt() == static_cast<int>(ConversationRole::kAgent);
        }, [items](std::size_t index) {
            const auto& item = items->at(index);
            // Use the known bounded body height before positioning new output,
            // not a 32 px placeholder that becomes 720 px on the next paint.
            return item.role == ConversationRole::kAgent
                && (item.text.size() > 4096 || item.text.count('\n') > 64) ? 720 : 32;
        });
    }

    AgentSubmitResult submit(AgentSubmitMode mode, const QString& text) {
        if (!submit_callback) {
            return {.ok = false, .error = "Agent submit callback is unavailable."};
        }
        return submit_callback(mode, text);
    }

    bool trigger_submit(AgentSubmitMode mode, QString* error) {
        const QString instruction = composer->toPlainText().trimmed();
        if (instruction.isEmpty()) {
            set_status("Enter an Agent instruction first.", true);
            if (error != nullptr) {
                *error = status->text();
            }
            return false;
        }
        if (!send->isEnabled() && mode == auto_submit_mode()) {
            set_status("Agent submission is unavailable until the channel and catalog are ready.", true);
            if (error != nullptr) {
                *error = status->text();
            }
            return false;
        }
        const AgentSubmitResult result = submit(mode, instruction);
        if (!result.ok) {
            set_status("Agent request failed: " + result.error, true);
            if (error != nullptr) {
                *error = result.error;
            }
            return false;
        }
        if (!result.task_id.isEmpty()) {
            set_current_task(result.task_id,
                mode == AgentSubmitMode::kSteerTurn
                    ? redclaw::protocol::AgentTaskStateV1::kRunning
                    : redclaw::protocol::AgentTaskStateV1::kQueued,
                false);
        }
        append_item(current_task_id, ConversationItem{
            .role = ConversationRole::kUser,
            .kind = mode == AgentSubmitMode::kSteerTurn ? "steer" : "instruction",
            .text = instruction,
        });
        append_tail(QString("[user] %1").arg(instruction));
        composer->clear();
        set_status(mode == AgentSubmitMode::kCreateTask
            ? "Task queued locally; awaiting Host acceptance"
            : mode == AgentSubmitMode::kSteerTurn ? "Steer queued locally" : "Turn queued locally; awaiting Host acceptance",
            false);
        return true;
    }

    bool trigger_interrupt(QString* error) {
        if (!interrupt_callback || current_task_id.isEmpty()) {
            if (error != nullptr) {
                *error = "No Agent task is selected.";
            }
            return false;
        }
        QString detail;
        const bool ok = interrupt_callback(&detail);
        set_status(ok ? "Interrupt requested" : "Interrupt failed: " + detail, !ok);
        if (!ok && error != nullptr) {
            *error = detail;
        }
        return ok;
    }

    bool trigger_approval(
        redclaw::protocol::AgentApprovalDecisionV1 decision,
        QString* error) {
        if (!approval_callback || current_approval_request_id.isEmpty()) {
            if (error != nullptr) {
                *error = "No Agent approval is pending.";
            }
            return false;
        }
        QString detail;
        const bool ok = approval_callback(decision, &detail);
        if (!ok) {
            set_status("Approval response failed: " + detail, true);
            if (error != nullptr) {
                *error = detail;
            }
            return false;
        }
        append_item(current_task_id, ConversationItem{
            .role = ConversationRole::kSystem,
            .kind = "approval_decision",
            .text = decision == redclaw::protocol::AgentApprovalDecisionV1::kAccept
                ? "Approval accepted." : "Approval rejected.",
        });
        tasks[current_task_id].decided_approval = current_approval_request_id;
        current_approval_request_id.clear();
        approval->hide();
        refresh_controls();
        return true;
    }
};

AgentConversationPanel::AgentConversationPanel(QSettings* settings, QWidget* parent)
    : QWidget(parent), impl_(std::make_unique<Impl>(this, settings)) {}

AgentConversationPanel::~AgentConversationPanel() = default;

void AgentConversationPanel::set_submit_callback(SubmitCallback callback) {
    impl_->submit_callback = std::move(callback);
}

void AgentConversationPanel::set_interrupt_callback(InterruptCallback callback) {
    impl_->interrupt_callback = std::move(callback);
}

void AgentConversationPanel::set_approval_callback(ApprovalCallback callback) {
    impl_->approval_callback = std::move(callback);
}

void AgentConversationPanel::set_task_selected_callback(TaskSelectedCallback callback) {
    impl_->task_selected_callback = std::move(callback);
}

void AgentConversationPanel::set_collapse_callback(CollapseCallback callback) {
    impl_->collapse_callback = std::move(callback);
}

void AgentConversationPanel::set_transport_state(
    bool authorized,
    bool channel_open,
    bool sync_required,
    const QString& detail) {
    if (detail.isEmpty() && impl_->authorized == authorized && impl_->channel_open == channel_open
        && impl_->sync_required == sync_required) return;
    const bool was_ready = impl_->authorized && impl_->channel_open && !impl_->sync_required;
    impl_->authorized = authorized;
    impl_->channel_open = channel_open;
    impl_->sync_required = sync_required;
    impl_->connection_status->setText(
        authorized && channel_open && !sync_required ? "Connected"
        : sync_required ? "Syncing"
        : channel_open ? "Unauthorized" : "Offline");
    impl_->connection_status->setProperty(
        "connected", authorized && channel_open && !sync_required);
    impl_->connection_status->style()->unpolish(impl_->connection_status);
    impl_->connection_status->style()->polish(impl_->connection_status);
    if (!detail.isEmpty()) {
        impl_->set_status(detail, false);
    } else if (!channel_open) {
        impl_->set_status("Agent channel disconnected. Conversation is preserved while reconnecting.", false);
    } else if (sync_required) {
        impl_->set_status("Synchronizing the active Agent task before accepting new work.", false);
    } else if (authorized && !was_ready) {
        impl_->set_status("Agent connected. Conversation restored.", false);
    }
    impl_->refresh_controls();
}

void AgentConversationPanel::apply_capability_message(
    const redclaw::protocol::AgentMessageEnvelopeV1& message) {
    if (message.provider == redclaw::protocol::AgentProviderKindV1::kNone) {
        return;
    }
    const int key = static_cast<int>(message.provider);
    auto& view = impl_->provider_views[key];
    view.kind = message.provider;
    view.available = message.available;
    view.readiness = message.provider_readiness;
    view.label = protocol_name(message.provider)
        + (message.available ? QString() : " (unavailable)");
    view.display_name = QString::fromStdString(message.display_name);
    view.reason = QString::fromStdString(message.text);
    if (message.model.empty()) {
        view.models.clear();
    } else {
        const QString model_name = QString::fromStdString(message.model);
        if (!view.models.contains(model_name)) {
            view.models.push_back(model_name);
        }
    }
    int index = impl_->provider->findData(key, Qt::UserRole);
    if (index < 0) {
        impl_->provider->addItem(view.label, key);
        index = impl_->provider->count() - 1;
    } else {
        impl_->provider->setItemText(index, view.label);
    }
    impl_->provider->setItemData(index, view.available, Qt::UserRole + 1);
    if (impl_->provider->currentIndex() < 0) {
        impl_->provider->setCurrentIndex(index);
    }
    impl_->restore_context_selection();
    impl_->populate_models();
    if (!message.available && !message.text.empty()) {
        impl_->set_status(QString("%1: %2")
            .arg(protocol_name(message.provider_readiness), view.reason), true);
    } else if (message.available) {
        impl_->set_status(message.requires_turn_approval
            ? "Provider ready; each turn requires one pre-approval."
            : "Provider ready; tool approvals are relayed individually.", false);
    }
    impl_->refresh_context_summary();
    impl_->refresh_controls();
}

void AgentConversationPanel::apply_project_catalog_message(
    const redclaw::protocol::AgentMessageEnvelopeV1& message) {
    if (message.project_id.empty()) {
        return;
    }
    AgentProjectView view{
        .id = QString::fromStdString(message.project_id),
        .name = QString::fromStdString(message.display_name),
        .git_repository = message.git_repository,
    };
    impl_->project_views[view.id] = view;
    int index = impl_->project->findData(view.id, Qt::UserRole);
    if (index < 0) {
        impl_->project->addItem(view.name, view.id);
        index = impl_->project->count() - 1;
    } else {
        impl_->project->setItemText(index, view.name);
    }
    impl_->project->setItemData(index, view.git_repository, Qt::UserRole + 1);
    if (impl_->project->currentIndex() < 0) {
        impl_->project->setCurrentIndex(index);
    }
    impl_->restore_context_selection();
    impl_->refresh_work_mode();
    impl_->refresh_context_summary();
    impl_->refresh_controls();
}

void AgentConversationPanel::apply_task_message(
    const redclaw::protocol::AgentMessageEnvelopeV1& message) {
    const QString task_id = QString::fromStdString(message.task_id);
    if (task_id.isEmpty()) {
        return;
    }
    auto& task = impl_->tasks[task_id];
    const bool approval_request = message.type == redclaw::protocol::AgentMessageTypeV1::kApprovalRequest
        || (message.type == redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot
            && message.task_state == redclaw::protocol::AgentTaskStateV1::kAwaitingApproval
            && message.event_kind == "approval_pending" && !message.request_id.empty());
    if (message.event_sequence > 0 && message.event_sequence < task.last_event_sequence) return;
    if (message.event_sequence > 0 && message.event_sequence == task.last_event_sequence
        && (message.type != redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot || message.gap)) return;
    if (approval_request
        && task.decided_approval == QString::fromStdString(message.request_id)) return;
    task.last_event_sequence = std::max(task.last_event_sequence, message.event_sequence);
    if (task_is_running(message.task_state) && !task_is_running(task.state) && task.terminal_consumed_us) {
        task.first_output_us = task.terminal_consumed_us = task.terminal_painted_us = 0;
        task.output_gap = false;
    }
    task.state = message.task_state;
    if (message.gap) task.output_gap = true;
    if (!task_is_running(message.task_state) && !task.terminal_consumed_us)
        task.terminal_consumed_us = gui_monotonic_us();
    if (!task_is_running(message.task_state)) {
        for (auto& item : task.items) item.mergeable = false;
        impl_->queue_conversation_rebuild();
    }
    if (task_id == impl_->current_task_id
        && message.task_state != redclaw::protocol::AgentTaskStateV1::kAwaitingApproval) {
        impl_->current_approval_request_id.clear();
        impl_->approval->hide();
    }
    impl_->update_task_selector(task_id, message.task_state);
    if (impl_->current_task_id.isEmpty()) {
        impl_->set_current_task(task_id, message.task_state, false);
    }

    const QString kind = QString::fromStdString(message.event_kind);
    const bool mergeable = message.type == redclaw::protocol::AgentMessageTypeV1::kEvent
        && message.task_state == redclaw::protocol::AgentTaskStateV1::kRunning;
    const QString text = QString::fromUtf8(
        message.text.data(), static_cast<qsizetype>(message.text.size()));
    if (message.gap) {
        impl_->append_item(task_id, ConversationItem{
            .role = ConversationRole::kSystem,
            .kind = "event_gap",
            .text = text.isEmpty()
                ? "Earlier Agent output is unavailable; missing text was not delivered."
                : text,
            .failed = true,
        });
        impl_->append_tail("--- Agent history gap: missing output was not delivered ---");
    }
    if (approval_request) {
        impl_->current_task_id = task_id;
        impl_->current_approval_request_id = QString::fromStdString(message.request_id);
        impl_->approval_text->setText(text.isEmpty()
            ? "The remote Agent requests approval." : text);
        impl_->approval->show();
        impl_->append_item(task_id, ConversationItem{
            .role = ConversationRole::kActivity,
            .kind = kind.isEmpty() ? "approval request" : kind,
            .text = text,
        });
    } else if (message.gap) {
        // The explicit loss notice above is not an Agent answer or tool output.
    } else if (!kind.isEmpty() && is_agent_text_delta(kind) && !text.isEmpty()) {
        if (!task.first_output_us) {
            task.first_output_us = gui_monotonic_us();
            if (task_id == impl_->current_task_id)
                if (auto* probe = gui_latency_probe()) probe->begin_task(task_id);
        }
        impl_->append_item(task_id, ConversationItem{
            .role = ConversationRole::kAgent,
            .kind = kind,
            .text = text,
            .mergeable = mergeable,
        });
    } else if (!kind.isEmpty()) {
        const bool failed = activity_is_failure(kind)
            || message.type == redclaw::protocol::AgentMessageTypeV1::kTaskError;
        impl_->append_item(task_id, ConversationItem{
            .role = ConversationRole::kActivity,
            .kind = kind,
            .text = text,
            .failed = failed,
            .mergeable = mergeable,
        });
    } else if (!text.isEmpty()) {
        impl_->append_item(task_id, ConversationItem{
            .role = message.type == redclaw::protocol::AgentMessageTypeV1::kTaskError
                ? ConversationRole::kSystem : ConversationRole::kAgent,
            .kind = protocol_name(message.type),
            .text = text,
            .failed = message.type == redclaw::protocol::AgentMessageTypeV1::kTaskError,
        });
    }
    if (!kind.isEmpty() || !text.isEmpty()) {
        impl_->append_tail(text.isEmpty()
            ? QString("[%1]").arg(kind)
            : QString("[%1] %2").arg(kind, text));
    }
    impl_->set_status(QString("Task %1 · %2")
        .arg(task_id, protocol_name(message.task_state)),
        message.type == redclaw::protocol::AgentMessageTypeV1::kTaskError);
    impl_->refresh_controls();
}

void AgentConversationPanel::set_current_task_id(
    const QString& task_id,
    redclaw::protocol::AgentTaskStateV1 state) {
    impl_->set_current_task(task_id, state, false);
}

QString AgentConversationPanel::current_task_id() const {
    return impl_->current_task_id;
}

redclaw::protocol::AgentTaskStateV1 AgentConversationPanel::current_task_state() const {
    return impl_->current_task_state();
}

QJsonObject AgentConversationPanel::diagnostic_timing() const {
    const auto found = impl_->tasks.find(impl_->current_task_id);
    if (found == impl_->tasks.end()) return {};
    const auto& task = found->second;
    qint64 reply_units = 0;
    for (const auto& item : task.items)
        if (item.role == ConversationRole::kAgent) reply_units += item.text.size();
    return {{"first_output_consumed_us", static_cast<qint64>(task.first_output_us)},
        {"terminal_consumed_us", static_cast<qint64>(task.terminal_consumed_us)},
        {"terminal_painted_us", static_cast<qint64>(task.terminal_painted_us)},
        {"retained_reply_utf16_units", reply_units}, {"output_gap", task.output_gap},
        {"panel_visible", isVisible()},
        {"gui_window", gui_latency_probe() ? gui_latency_probe()->task_snapshot(impl_->current_task_id) : QJsonObject{}}};
}

QString AgentConversationPanel::approval_request_id() const {
    return impl_->current_approval_request_id;
}

QString AgentConversationPanel::status_text() const {
    return impl_->status->text();
}

redclaw::protocol::AgentProviderKindV1 AgentConversationPanel::selected_provider() const {
    return static_cast<redclaw::protocol::AgentProviderKindV1>(
        impl_->provider->currentData(Qt::UserRole).toInt());
}

QString AgentConversationPanel::selected_model() const {
    return impl_->model->currentData(Qt::UserRole).toString();
}

QString AgentConversationPanel::selected_project_id() const {
    return impl_->project->currentData(Qt::UserRole).toString();
}

redclaw::protocol::AgentWorkDirectoryModeV1
AgentConversationPanel::selected_work_directory_mode() const {
    return impl_->work_mode->currentData(Qt::UserRole).toString() == "direct_workspace"
        ? redclaw::protocol::AgentWorkDirectoryModeV1::kDirectWorkspace
        : redclaw::protocol::AgentWorkDirectoryModeV1::kIsolatedWorktree;
}

bool AgentConversationPanel::selected_project_is_git() const {
    return impl_->project->currentData(Qt::UserRole + 1).toBool();
}

std::vector<AgentProviderView> AgentConversationPanel::providers() const {
    std::vector<AgentProviderView> result;
    result.reserve(impl_->provider_views.size());
    for (const auto& [_, view] : impl_->provider_views) {
        result.push_back(view);
    }
    return result;
}

std::vector<AgentProjectView> AgentConversationPanel::projects() const {
    std::vector<AgentProjectView> result;
    result.reserve(impl_->project_views.size());
    for (const auto& [_, view] : impl_->project_views) {
        result.push_back(view);
    }
    return result;
}

int AgentConversationPanel::provider_count() const {
    return static_cast<int>(impl_->provider_views.size());
}

int AgentConversationPanel::project_count() const {
    return static_cast<int>(impl_->project_views.size());
}

bool AgentConversationPanel::approval_pending() const {
    return !impl_->current_approval_request_id.isEmpty();
}

bool AgentConversationPanel::select_provider(
    redclaw::protocol::AgentProviderKindV1 provider) {
    const int index = impl_->provider->findData(static_cast<int>(provider), Qt::UserRole);
    if (index < 0 || !impl_->provider->itemData(index, Qt::UserRole + 1).toBool()) {
        return false;
    }
    impl_->provider->setCurrentIndex(index);
    return true;
}

void AgentConversationPanel::select_isolated_worktree() {
    const int index = impl_->work_mode->findData("isolated_worktree", Qt::UserRole);
    if (index >= 0) {
        impl_->work_mode->setCurrentIndex(index);
    }
}

void AgentConversationPanel::set_instruction_text(const QString& text) {
    impl_->composer->setPlainText(text);
}

QString AgentConversationPanel::instruction_text() const {
    return impl_->composer->toPlainText();
}

bool AgentConversationPanel::trigger_submit(AgentSubmitMode mode, QString* error) {
    return impl_->trigger_submit(mode, error);
}

bool AgentConversationPanel::trigger_interrupt(QString* error) {
    return impl_->trigger_interrupt(error);
}

bool AgentConversationPanel::trigger_approval(
    redclaw::protocol::AgentApprovalDecisionV1 decision,
    QString* error) {
    return impl_->trigger_approval(decision, error);
}

void AgentConversationPanel::set_status_text(const QString& text, bool error) {
    impl_->set_status(text, error);
}

QStringList AgentConversationPanel::bounded_event_tail(int maximum_lines) const {
    if (maximum_lines <= 0 || impl_->event_tail.isEmpty()) {
        return {};
    }
    const qsizetype start = (std::max)(
        qsizetype{0}, impl_->event_tail.size() - static_cast<qsizetype>(maximum_lines));
    return impl_->event_tail.mid(start);
}

}  // namespace redclaw::ui
