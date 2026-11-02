#include "ui/connection_overlay.h"
#include "ui/bounded_floating_window.h"
#include <QApplication>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace redclaw::ui {
namespace {
constexpr auto kGeometry = "controller/connection_overlay/v1/geometry";
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
    std::unique_ptr<BoundedFloatingWindow> panel, badge;
    QWidget* body;
    QVBoxLayout* body_layout;
    QLabel *summary, *details;
    QToolButton *fold, *entry;
    QPushButton *disconnect, *reconnect;
    QPointer<QToolButton> button;
    ConnectionOverlayState state = ConnectionOverlayState::kExpanded;
    ConnectionFlowSnapshot flow;
    QRect preferred;
    QString transport, frame, stop_error;
    bool active = false, syncing = false;
    std::function<void()> interacted, disconnect_requested, reconnect_requested;

    Impl(QWidget* owner, QWidget* view, QSettings* store)
        : surface(owner), root(owner->window()), log(view), home(view->parentWidget()),
          home_layout(home->layout()), settings(store),
          panel(std::make_unique<BoundedFloatingWindow>(owner, false)),
          badge(std::make_unique<BoundedFloatingWindow>(owner, true)) {
        panel->setObjectName("connectionOverlay");
        panel->setWindowTitle(QString::fromUtf8("连接"));
        auto* layout = new QVBoxLayout(panel.get()); layout->setContentsMargins(6, 6, 6, 6);
        layout->setSizeConstraint(QLayout::SetNoConstraint);
        auto* header = new QWidget(panel.get()); header->setFixedHeight(30);
        auto* row = new QHBoxLayout(header); row->setContentsMargins(4, 0, 0, 0);
        auto* title = new QLabel(QString::fromUtf8("连接"), header);
        summary = new QLabel(header); summary->setTextFormat(Qt::PlainText);
        row->addWidget(title); row->addWidget(summary, 1);
        fold = new QToolButton(header); fold->setObjectName("connectionOverlayFold");
        auto* hide = new QToolButton(header); hide->setText(QString::fromUtf8("×"));
        hide->setObjectName("connectionOverlayHide"); hide->setToolTip(QString::fromUtf8("隐藏连接浮层"));
        row->addWidget(fold); row->addWidget(hide); layout->addWidget(header);
        panel->set_drag_handle(header); panel->set_drag_handle(title); panel->set_drag_handle(summary);
        body = new QWidget(panel.get()); body_layout = new QVBoxLayout(body);
        body_layout->setContentsMargins(0, 0, 0, 0);
        details = new QLabel(body); details->setTextFormat(Qt::PlainText); details->setWordWrap(true);
        body_layout->addWidget(details);
        auto* actions = new QHBoxLayout;
        reconnect = new QPushButton(QString::fromUtf8("重连"), body); reconnect->setObjectName("connectionOverlayReconnect");
        disconnect = new QPushButton(QString::fromUtf8("断开"), body); disconnect->setObjectName("connectionOverlayDisconnect");
        actions->addWidget(reconnect); actions->addWidget(disconnect); actions->addStretch(); body_layout->addLayout(actions);
        layout->addWidget(body, 1);
        auto* badge_layout = new QVBoxLayout(badge.get()); badge_layout->setContentsMargins(4, 2, 4, 2);
        entry = new QToolButton(badge.get()); entry->setObjectName("connectionStatusEntry"); badge_layout->addWidget(entry);
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
        QObject::connect(entry, &QToolButton::clicked, owner, [this] { interact(); change(ConnectionOverlayState::kExpanded); });
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
        body_layout->removeWidget(log); log->setParent(home); home_layout->addWidget(log); log->show();
    }
    void change(ConnectionOverlayState next) {
        state = next; sync();
        if (button) { const QSignalBlocker blocker(button); button->setChecked(state == ConnectionOverlayState::kExpanded); }
    }
    void text() {
        const QString phase = phase_text(flow);
        summary->setText(phase); entry->setText(QString::fromUtf8("● ") + phase + QString::fromUtf8(" · 连接"));
        const auto color = flow.status == ConnectionFlowStatus::kComplete ? "#86efac"
            : flow.status == ConnectionFlowStatus::kFailed ? "#fca5a5" : "#fde68a";
        entry->setStyleSheet(QString("QToolButton { color: %1; padding: 4px 8px; }").arg(color));
        const QString path = transport.contains("relay", Qt::CaseInsensitive) ? QString::fromUtf8("中继")
            : transport.contains("direct", Qt::CaseInsensitive) ? QString::fromUtf8("直连") : QString::fromUtf8("路径未知");
        QString text = QString::fromUtf8("%1 · %2\n画面：%3").arg(phase, path, frame.isEmpty() ? QString::fromUtf8("等待画面") : frame);
        if (!flow.failure_detail.isEmpty()) text += '\n' + flow.failure_detail;
        if (!stop_error.isEmpty()) text += '\n' + stop_error;
        details->setText(text);
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
        if (!available) { panel->hide(); badge->hide(); syncing = false; return; }
        if (!preferred.isValid()) {
            const QSize size = QSize(480, 340).boundedTo(area.size());
            preferred = QRect(QPoint(std::max(0, area.width() - size.width() - 12), std::max(0, area.height() - size.height() - 12)), size);
        }
        QRect target = preferred.translated(area.topLeft());
        const bool collapsed = state == ConnectionOverlayState::kCollapsed;
        if (collapsed) target.setHeight(42);
        panel->set_resizable(!collapsed); panel->set_bounds(area);
        if (!panel->dragging()) panel->setGeometry(bounded_floating_rect(target, area));
        body->setVisible(!collapsed); fold->setText(collapsed ? QString::fromUtf8("展开") : QString::fromUtf8("收起"));
        panel->setVisible(available && state != ConnectionOverlayState::kHidden);
        badge->set_bounds(area);
        const QSize badge_size = badge->sizeHint().boundedTo(area.size());
        badge->setGeometry(bounded_floating_rect(QRect(QPoint(area.left() + 12, area.bottom() - badge_size.height() - 12), badge_size), area));
        badge->setVisible(available);
        if (panel->isVisible()) panel->raise();
        if (badge->isVisible()) badge->raise();
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
        impl_->body_layout->addWidget(impl_->log, 1); impl_->log->show();
        impl_->stop_error.clear(); impl_->change(ConnectionOverlayState::kExpanded);
    } else { impl_->move_log_home(); impl_->sync(); }
}
void ConnectionOverlay::set_state(ConnectionOverlayState state) { impl_->change(state); }
ConnectionOverlayState ConnectionOverlay::state() const { return impl_->state; }
void ConnectionOverlay::update_connection(const ConnectionFlowSnapshot& snapshot) {
    const bool incident = (snapshot.status != impl_->flow.status
        && (snapshot.status == ConnectionFlowStatus::kDisconnected || snapshot.status == ConnectionFlowStatus::kRetryWaiting
            || snapshot.status == ConnectionFlowStatus::kFailed)) || snapshot.attempt > impl_->flow.attempt;
    impl_->flow = snapshot; impl_->text();
    if (incident && impl_->active) impl_->change(ConnectionOverlayState::kExpanded);
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
    impl_->change(impl_->state);
}
void ConnectionOverlay::set_local_interaction_callback(std::function<void()> callback) { impl_->interacted = std::move(callback); }
void ConnectionOverlay::set_disconnect_callback(std::function<void()> callback) { impl_->disconnect_requested = std::move(callback); }
void ConnectionOverlay::set_reconnect_callback(std::function<void()> callback) { impl_->reconnect_requested = std::move(callback); }
QWidget* ConnectionOverlay::panel() const { return impl_->panel.get(); }
QWidget* ConnectionOverlay::status_entry() const { return impl_->badge.get(); }
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
    if (widget && (widget->window() == impl_->panel.get() || widget->window() == impl_->badge.get())) {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel
            || event->type() == QEvent::KeyPress || event->type() == QEvent::FocusIn) impl_->interact();
        if (event->type() == QEvent::WindowActivate) impl_->raise_toolbar();
    }
    return false;
}
} // namespace redclaw::ui
