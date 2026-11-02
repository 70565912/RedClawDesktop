#include "ui/connection_overlay.h"
#include "ui/bounded_floating_window.h"
#include "ui/connection_progress_page.h"
#include <QApplication>
#include <QGridLayout>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace redclaw::ui {
namespace {
constexpr auto kGeometry = "controller/connection_overlay/v1/geometry";
void indicator(QLabel* label, const QString& text, const char* color) {
    if (label->text() != text) label->setText(text);
    const auto style = QString("color: %1; background: transparent;").arg(color);
    if (label->styleSheet() != style) label->setStyleSheet(style);
}
QString phase_text(const ConnectionFlowSnapshot& flow) {
    switch (flow.status) {
    case ConnectionFlowStatus::kComplete: return QString::fromUtf8("已连接");
    case ConnectionFlowStatus::kStopping: return QString::fromUtf8("正在断开");
    case ConnectionFlowStatus::kFailed: return QString::fromUtf8("连接失败");
    case ConnectionFlowStatus::kDisconnected:
    case ConnectionFlowStatus::kRetryWaiting: return QString::fromUtf8("正在重连");
    case ConnectionFlowStatus::kRunning: return QString::fromUtf8("连接中 · 第 %1/4 阶段").arg(flow.current_stage + 1);
    default: return QString::fromUtf8("尚未连接");
    }
}
}

struct ConnectionOverlay::Impl {
    QPointer<QWidget> surface, root, log, home;
    QLayout* home_layout;
    QPointer<QSettings> settings;
    std::unique_ptr<BoundedFloatingWindow> panel;
    QWidget *header, *body, *actions;
    QVBoxLayout* body_layout;
    QScrollArea* scroll;
    ConnectionProgressPage* progress;
    QLabel *summary, *details;
    QLabel *pairing, *handshake, *link, *process;
    QToolButton* fold;
    QPushButton *disconnect, *reconnect;
    QPointer<QToolButton> button;
    ConnectionOverlayState state = ConnectionOverlayState::kExpanded;
    ConnectionFlowSnapshot flow;
    QRect preferred;
    QString transport, frame, stop_error, log_style;
    bool active = false, syncing = false;
    std::function<void()> interacted, disconnect_requested, reconnect_requested;

    Impl(QWidget* owner, QWidget* view, QSettings* store)
        : surface(owner), root(owner->window()), log(view), home(view->parentWidget()),
          home_layout(home->layout()), settings(store),
          panel(std::make_unique<BoundedFloatingWindow>(owner, false)) {
        log_style = view->styleSheet();
        panel->setObjectName("connectionOverlay");
        panel->setWindowTitle(QString::fromUtf8("连接"));
        auto* layout = new QVBoxLayout(panel.get()); layout->setContentsMargins(6, 6, 6, 6);
        layout->setSizeConstraint(QLayout::SetNoConstraint);
        header = new QWidget(panel.get()); header->setFixedHeight(30);
        auto* row = new QHBoxLayout(header); row->setContentsMargins(4, 0, 0, 0);
        auto* title = new QLabel(QString::fromUtf8("连接"), header);
        summary = new QLabel(header); summary->setTextFormat(Qt::PlainText);
        row->addWidget(title); row->addWidget(summary, 1);
        fold = new QToolButton(header); fold->setObjectName("connectionOverlayFold");
        auto* hide = new QToolButton(header); hide->setText(QString::fromUtf8("×"));
        hide->setObjectName("connectionOverlayHide"); hide->setToolTip(QString::fromUtf8("隐藏连接浮层"));
        row->addWidget(fold); row->addWidget(hide); layout->addWidget(header);
        panel->set_drag_handle(header); panel->set_drag_handle(title); panel->set_drag_handle(summary);
        scroll = new QScrollArea(panel.get()); scroll->setObjectName("connectionOverlayScroll");
        scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true); scroll->setMinimumSize(0, 0);
        body = new QWidget(scroll); body->setMinimumWidth(448); body_layout = new QVBoxLayout(body);
        body_layout->setContentsMargins(6, 0, 6, 6); body_layout->setSpacing(8);
        progress = new ConnectionProgressPage(ConnectionFlowRole::kController, body, ConnectionProgressPresentation::kOverlay);
        body_layout->addWidget(progress);
        auto* status_title = new QLabel(QString::fromUtf8("运行状态"), body); body_layout->addWidget(status_title);
        auto* states = new QGridLayout; states->setHorizontalSpacing(12); states->setVerticalSpacing(4);
        auto add_indicator = [&](int row, int column, const QString& title, const char* name) {
            auto* label = new QLabel(title, body);
            auto* value = new QLabel(body); value->setObjectName(name); value->setTextFormat(Qt::PlainText);
            states->addWidget(label, row, column); states->addWidget(value, row, column + 1);
            return value;
        };
        pairing = add_indicator(0, 0, QString::fromUtf8("配对"), "connectionPairingState");
        handshake = add_indicator(0, 2, QString::fromUtf8("握手"), "connectionHandshakeState");
        link = add_indicator(1, 0, QString::fromUtf8("实时链路"), "connectionLiveLinkState");
        process = add_indicator(1, 2, QString::fromUtf8("程序"), "connectionProcessState");
        indicator(pairing, QString::fromUtf8("就绪"), "#cbd5e1");
        indicator(handshake, QString::fromUtf8("等待中"), "#fde68a");
        indicator(link, QString::fromUtf8("等待中"), "#fde68a");
        indicator(process, QString::fromUtf8("空闲"), "#cbd5e1");
        body_layout->addLayout(states);
        details = new QLabel(body); details->setTextFormat(Qt::PlainText); details->setWordWrap(true);
        details->setObjectName("connectionRuntimeDetails");
        body_layout->addWidget(details);
        actions = new QWidget(panel.get());
        auto* buttons = new QHBoxLayout(actions); buttons->setContentsMargins(6, 0, 6, 0);
        reconnect = new QPushButton(QString::fromUtf8("重连"), body); reconnect->setObjectName("connectionOverlayReconnect");
        disconnect = new QPushButton(QString::fromUtf8("断开"), body); disconnect->setObjectName("connectionOverlayDisconnect");
        disconnect->setStyleSheet("QPushButton#connectionOverlayDisconnect { border-color: #ef4444; color: #fca5a5; }");
        buttons->addWidget(reconnect, 1); buttons->addWidget(disconnect, 1);
        scroll->setWidget(body); layout->addWidget(scroll, 1); layout->addWidget(actions);
        if (settings) preferred = settings->value(kGeometry).toRect();
        panel->interacted = [this] { interact(); };
        panel->geometry_committed = [this] {
            QRect rect = panel->geometry().translated(-bounds().topLeft());
            if (state == ConnectionOverlayState::kCollapsed) preferred.moveTopLeft(rect.topLeft());
            else preferred = rect;
            if (settings) settings->setValue(kGeometry, preferred);
        };
        QObject::connect(fold, &QToolButton::clicked, owner, [this] {
            interact(); change(state == ConnectionOverlayState::kCollapsed ? ConnectionOverlayState::kExpanded : ConnectionOverlayState::kCollapsed);
        });
        QObject::connect(hide, &QToolButton::clicked, owner, [this] { interact(); change(ConnectionOverlayState::kHidden); });
        panel->dismissed = [this] { change(ConnectionOverlayState::kHidden); };
        QObject::connect(disconnect, &QPushButton::clicked, owner, [this] { if (disconnect_requested) disconnect_requested(); });
        QObject::connect(reconnect, &QPushButton::clicked, owner, [this] { if (reconnect_requested) reconnect_requested(); });
    }
    // Do not reparent/show log widgets during owner destruction: GUI callbacks
    // may already have gone out of scope. Normal return-home restores the log.
    ~Impl() = default;
    QRect bounds() const { return surface ? QRect(surface->mapToGlobal(QPoint()), surface->size()) : QRect{}; }
    void raise_toolbar() { if (button && button->window()->isVisible()) button->window()->raise(); }
    void interact() { raise_toolbar(); if (interacted) interacted(); }
    void move_log_home() {
        if (!log || !home || log->parentWidget() == home) return;
        body_layout->removeWidget(log); log->setParent(home); log->setStyleSheet(log_style);
        home_layout->addWidget(log); log->show();
    }
    void change(ConnectionOverlayState next) {
        state = next; sync();
        if (button) { const QSignalBlocker blocker(button); button->setChecked(state == ConnectionOverlayState::kExpanded); }
    }
    void text() {
        const QString phase = phase_text(flow);
        const auto color = flow.status == ConnectionFlowStatus::kComplete ? "#86efac"
            : flow.status == ConnectionFlowStatus::kFailed ? "#fca5a5" : "#fde68a";
        indicator(summary, phase, color);
        progress->set_snapshot(flow);
        if (button) {
            button->setText(QString::fromUtf8("● ") + phase);
            button->setAccessibleName(QString::fromUtf8("连接：%1").arg(phase));
            button->setToolTip(QString::fromUtf8("%1；点击显示或隐藏连接状态和日志").arg(phase));
            const auto style = QString("QToolButton { color: %1; }").arg(color);
            if (button->styleSheet() != style) button->setStyleSheet(style);
        }
        const QString path = transport.contains("relay", Qt::CaseInsensitive) ? QString::fromUtf8("中继")
            : transport.contains("direct", Qt::CaseInsensitive) ? QString::fromUtf8("直连") : QString::fromUtf8("路径未知");
        QString text = QString::fromUtf8("连接路径：%1\n画面：%2").arg(path, frame.isEmpty() ? QString::fromUtf8("等待画面") : frame);
        if (!flow.failure_detail.isEmpty()) text += '\n' + flow.failure_detail;
        if (!stop_error.isEmpty()) text += '\n' + stop_error;
        if (details->text() != text) details->setText(text);
        disconnect->setEnabled(flow.status != ConnectionFlowStatus::kStopping);
        reconnect->setEnabled(flow.status != ConnectionFlowStatus::kStopping
            && (flow.status == ConnectionFlowStatus::kComplete || flow.status == ConnectionFlowStatus::kFailed
                || flow.status == ConnectionFlowStatus::kDisconnected || flow.status == ConnectionFlowStatus::kRetryWaiting));
    }
    void sync() {
        if (syncing || !surface) return;
        syncing = true;
        const QRect area = bounds();
        const bool available = active && surface->isVisible() && !surface->window()->isMinimized() && !area.isEmpty();
        if (!available) { panel->hide(); syncing = false; return; }
        if (!preferred.isValid()) {
            const QSize size = QSize(560, 560).boundedTo(area.size());
            preferred = QRect(QPoint(std::max(0, area.width() - size.width() - 12), std::max(0, area.height() - size.height() - 12)), size);
        }
        QRect target = preferred.translated(area.topLeft());
        const bool collapsed = state == ConnectionOverlayState::kCollapsed;
        scroll->setVisible(!collapsed);
        actions->setVisible(!collapsed);
        fold->setText(collapsed ? QString::fromUtf8("展开") : QString::fromUtf8("收起"));
        // Constrain the native tool window as well as Qt's requested geometry.
        // Hide the expanded contents before negotiating its compact size.
        if (collapsed) {
            const int height = std::min(42, area.height());
            panel->setFixedHeight(height);
            target.setSize(QSize(std::min(target.width(), header->sizeHint().width() + 12), height));
        } else {
            panel->setMinimumHeight(0);
            panel->setMaximumHeight(QWIDGETSIZE_MAX);
        }
        panel->set_resizable(!collapsed); panel->set_bounds(area);
        if (!panel->dragging()) panel->setGeometry(bounded_floating_rect(target, area));
        panel->setVisible(available && state != ConnectionOverlayState::kHidden);
        if (panel->isVisible()) panel->raise();
        raise_toolbar();
        syncing = false;
    }
};

ConnectionOverlay::ConnectionOverlay(QWidget* surface, QWidget* log, QSettings* settings)
    : QObject(surface), impl_(std::make_unique<Impl>(surface, log, settings)) {
    qApp->installEventFilter(this);
}
ConnectionOverlay::~ConnectionOverlay() { qApp->removeEventFilter(this); }
void ConnectionOverlay::set_active(bool active) {
    if (impl_->active == active) return;
    impl_->active = active;
    if (active) {
        impl_->home_layout->removeWidget(impl_->log); impl_->log->setParent(impl_->body);
        impl_->log->setStyleSheet(impl_->log_style +
            " QGroupBox#primaryCard { border: none; margin-top: 18px; padding: 0; background: transparent; }"
            " QGroupBox#primaryCard::title { left: 0; padding: 0; }");
        impl_->body_layout->addWidget(impl_->log, 1); impl_->log->show();
        impl_->flow = ConnectionFlowSnapshot{};
        impl_->stop_error.clear(); impl_->change(ConnectionOverlayState::kExpanded);
    } else { impl_->move_log_home(); impl_->sync(); }
}
void ConnectionOverlay::set_state(ConnectionOverlayState state) { impl_->change(state); }
ConnectionOverlayState ConnectionOverlay::state() const { return impl_->state; }
void ConnectionOverlay::update_connection(const ConnectionFlowSnapshot& snapshot) {
    const bool completed = snapshot.status == ConnectionFlowStatus::kComplete
        && impl_->flow.status != ConnectionFlowStatus::kComplete;
    const bool incident = (snapshot.status != impl_->flow.status
        && (snapshot.status == ConnectionFlowStatus::kDisconnected || snapshot.status == ConnectionFlowStatus::kRetryWaiting
            || snapshot.status == ConnectionFlowStatus::kFailed)) || snapshot.attempt > impl_->flow.attempt;
    impl_->flow = snapshot; impl_->text();
    if (impl_->active) {
        if (completed) impl_->change(ConnectionOverlayState::kHidden);
        else if (incident) impl_->change(ConnectionOverlayState::kExpanded);
    }
}
void ConnectionOverlay::set_pairing_state(ConnectionPairingState state) {
    switch (state) {
    case ConnectionPairingState::kReady: indicator(impl_->pairing, QString::fromUtf8("就绪"), "#cbd5e1"); break;
    case ConnectionPairingState::kConnecting: indicator(impl_->pairing, QString::fromUtf8("连接中"), "#fde68a"); break;
    case ConnectionPairingState::kConnected: indicator(impl_->pairing, QString::fromUtf8("已连接"), "#86efac"); break;
    case ConnectionPairingState::kAttention: indicator(impl_->pairing, QString::fromUtf8("连接异常"), "#fca5a5"); break;
    case ConnectionPairingState::kClosed: indicator(impl_->pairing, QString::fromUtf8("已关闭"), "#cbd5e1"); break;
    }
}
void ConnectionOverlay::set_handshake_ready(bool ready) {
    indicator(impl_->handshake, QString::fromUtf8(ready ? "已就绪" : "等待中"), ready ? "#86efac" : "#fde68a");
}
void ConnectionOverlay::set_link_live(bool live) {
    indicator(impl_->link, QString::fromUtf8(live ? "在线" : "等待中"), live ? "#86efac" : "#fde68a");
}
void ConnectionOverlay::set_process_state(ConnectionProcessState state) {
    switch (state) {
    case ConnectionProcessState::kIdle: indicator(impl_->process, QString::fromUtf8("空闲"), "#cbd5e1"); break;
    case ConnectionProcessState::kActive: indicator(impl_->process, QString::fromUtf8("运行中"), "#86efac"); break;
    case ConnectionProcessState::kStopping: indicator(impl_->process, QString::fromUtf8("正在停止"), "#fde68a"); break;
    case ConnectionProcessState::kFailed: indicator(impl_->process, QString::fromUtf8("运行异常"), "#fca5a5"); break;
    }
}
void ConnectionOverlay::update_runtime(const QString& transport, const QString& frame) {
    // Frame presentation repeats the same caption: do not restyle/re-layout UI
    // for every received frame or for an unchanged diagnostic heartbeat.
    if (transport == impl_->transport && (frame.isEmpty() || frame == impl_->frame)) return;
    impl_->transport = transport; if (!frame.isEmpty()) impl_->frame = frame; impl_->text();
}
void ConnectionOverlay::show_stop_failure(const QString& message) {
    impl_->stop_error = message; impl_->flow.status = ConnectionFlowStatus::kFailed; impl_->text();
    impl_->change(ConnectionOverlayState::kExpanded);
}
void ConnectionOverlay::bind_button(QToolButton* button) {
    impl_->button = button;
    connect(button, &QToolButton::clicked, this, [this] { impl_->interact(); impl_->change(impl_->state == ConnectionOverlayState::kExpanded
        ? ConnectionOverlayState::kHidden : ConnectionOverlayState::kExpanded); });
    impl_->text();
    impl_->change(impl_->state);
}
void ConnectionOverlay::set_local_interaction_callback(std::function<void()> callback) { impl_->interacted = std::move(callback); }
void ConnectionOverlay::set_disconnect_callback(std::function<void()> callback) { impl_->disconnect_requested = std::move(callback); }
void ConnectionOverlay::set_reconnect_callback(std::function<void()> callback) { impl_->reconnect_requested = std::move(callback); }
QWidget* ConnectionOverlay::panel() const { return impl_->panel.get(); }
bool ConnectionOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (impl_->syncing) return false;
    if (watched == impl_->surface || watched == impl_->root) {
        switch (event->type()) {
        case QEvent::Show: case QEvent::Hide: case QEvent::Move: case QEvent::Resize:
        case QEvent::WindowStateChange: case QEvent::ScreenChangeInternal: impl_->sync(); break;
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        case QEvent::DevicePixelRatioChange: impl_->sync(); break;
#endif
        default: break;
        }
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget && widget->window() == impl_->panel.get()) {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel
            || event->type() == QEvent::KeyPress || event->type() == QEvent::FocusIn) impl_->interact();
        if (event->type() == QEvent::WindowActivate) impl_->raise_toolbar();
    }
    return false;
}
} // namespace redclaw::ui
