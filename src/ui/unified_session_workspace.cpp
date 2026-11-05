#include "ui/unified_session_workspace.h"
#include "ui/connection_overlay.h"
#include "ui/desktop_workspace_theme.h"
#include <QCloseEvent>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace redclaw::ui {
struct UnifiedSessionWorkspace::Impl {
    QMainWindow* window;
    QStackedWidget* pages;
    QWidget *home, *desktop;
    QPointer<ConnectionOverlay> overlay;
    ConnectionFlowSnapshot flow;
    SessionDisconnectState disconnect = SessionDisconnectState::kIdle;
    bool active = false, confirming = false;
    QTimer* stop_deadline;
    QPointer<QPushButton> disconnect_button;
    std::function<bool()> confirm, runtime_running;
    std::function<void()> request_stop, before_home;

    Impl(QMainWindow* owner, QWidget* entry, QSettings* settings)
        : window(owner), pages(new QStackedWidget(owner)), home(entry), desktop(new QWidget(pages)) {
        pages->setObjectName("sessionWorkspacePages"); desktop->setObjectName("playbackWindow");
        desktop_theme::apply_canvas_palette(desktop);
        desktop->setStyleSheet("QWidget#playbackWindow { background: #0d131b; }");
        auto* layout = new QVBoxLayout(desktop); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
        pages->addWidget(home); pages->addWidget(desktop); pages->setCurrentWidget(home);
        owner->setCentralWidget(pages);
        overlay = new ConnectionOverlay(desktop, settings);
        stop_deadline = new QTimer(pages); stop_deadline->setSingleShot(true); stop_deadline->setInterval(15000);
        confirm = [owner] {
            QMessageBox prompt(QMessageBox::Question, QString::fromUtf8("断开连接？"),
                QString::fromUtf8("断开当前连接并返回连接首页？"), QMessageBox::NoButton, owner);
            auto* accept = prompt.addButton(QString::fromUtf8("断开并返回首页"), QMessageBox::AcceptRole);
            auto* cancel = prompt.addButton(QMessageBox::Cancel);
            prompt.setDefaultButton(cancel); prompt.setEscapeButton(cancel);
            prompt.exec(); return prompt.clickedButton() == accept;
        };
    }
    ~Impl() { delete overlay.data(); }
    void show_home() {
        stop_deadline->stop(); disconnect = SessionDisconnectState::kIdle; active = false;
        overlay->set_active(false);
        if (disconnect_button) disconnect_button->setEnabled(false);
        if (before_home) before_home();
        pages->setCurrentWidget(home);
    }
};
UnifiedSessionWorkspace::UnifiedSessionWorkspace(QMainWindow* window, QWidget* home, QSettings* settings)
    : QObject(window), impl_(std::make_unique<Impl>(window, home, settings)) {
    window->installEventFilter(this);
    impl_->overlay->set_disconnect_callback([this] { request_disconnect(); });
    connect(impl_->stop_deadline, &QTimer::timeout, this, [this] {
        stop_failed(QString::fromUtf8("未确认连接进程已经停止。请查看日志后重试，当前窗口保留。"));
    });
}
UnifiedSessionWorkspace::~UnifiedSessionWorkspace() { impl_->window->removeEventFilter(this); }
QWidget* UnifiedSessionWorkspace::desktop_page() const { return impl_->desktop; }
ConnectionOverlay* UnifiedSessionWorkspace::connection_overlay() const { return impl_->overlay.data(); }
bool UnifiedSessionWorkspace::desktop_visible() const { return impl_->pages->currentWidget() == impl_->desktop; }
SessionDisconnectState UnifiedSessionWorkspace::disconnect_state() const { return impl_->disconnect; }
void UnifiedSessionWorkspace::present_desktop() {
    if (!impl_->active) return;
    impl_->pages->setCurrentWidget(impl_->desktop);
    impl_->overlay->set_active(true);
}
void UnifiedSessionWorkspace::update_connection(const ConnectionFlowSnapshot& snapshot) {
    impl_->flow = snapshot;
    const bool controller_session = snapshot.role == ConnectionFlowRole::kController
        && snapshot.view != ConnectionFlowView::kPreConnection && snapshot.status != ConnectionFlowStatus::kIdle;
    if (controller_session) {
        impl_->active = true; present_desktop();
        if (impl_->disconnect_button) impl_->disconnect_button->setEnabled(impl_->disconnect != SessionDisconnectState::kStopping);
        if (impl_->disconnect == SessionDisconnectState::kIdle) impl_->overlay->update_connection(snapshot);
    } else if (impl_->disconnect == SessionDisconnectState::kIdle && impl_->active) {
        impl_->show_home();
    }
}
void UnifiedSessionWorkspace::update_runtime(const QString& transport, const QString& frame) {
    impl_->overlay->update_runtime(transport, frame);
}
void UnifiedSessionWorkspace::request_disconnect() {
    if (!impl_->active || impl_->confirming || impl_->disconnect == SessionDisconnectState::kStopping) return;
    impl_->confirming = true;
    const bool confirmed = impl_->confirm && impl_->confirm();
    impl_->confirming = false;
    if (!confirmed) return;
    impl_->disconnect = SessionDisconnectState::kStopping;
    if (impl_->disconnect_button) impl_->disconnect_button->setEnabled(false);
    auto stopping = impl_->flow; stopping.status = ConnectionFlowStatus::kStopping;
    impl_->overlay->update_connection(stopping);
    impl_->overlay->set_state(ConnectionOverlayState::kExpanded);
    impl_->stop_deadline->start();
    if (impl_->request_stop) impl_->request_stop();
    if (impl_->runtime_running && !impl_->runtime_running()) runtime_stopped();
}
void UnifiedSessionWorkspace::runtime_stopped() {
    if (impl_->disconnect != SessionDisconnectState::kIdle) impl_->show_home();
}
void UnifiedSessionWorkspace::stop_failed(const QString& reason) {
    if (impl_->disconnect != SessionDisconnectState::kStopping) return;
    impl_->stop_deadline->stop(); impl_->disconnect = SessionDisconnectState::kFailed;
    impl_->overlay->show_stop_failure(reason);
    if (impl_->disconnect_button) impl_->disconnect_button->setEnabled(true);
}
void UnifiedSessionWorkspace::bind_disconnect_button(QPushButton* button) {
    impl_->disconnect_button = button;
    button->setEnabled(impl_->active && impl_->disconnect != SessionDisconnectState::kStopping);
    connect(button, &QPushButton::clicked, this, [this] { request_disconnect(); });
}
void UnifiedSessionWorkspace::set_confirm_disconnect(std::function<bool()> callback) { impl_->confirm = std::move(callback); }
void UnifiedSessionWorkspace::set_request_stop(std::function<void()> callback) { impl_->request_stop = std::move(callback); }
void UnifiedSessionWorkspace::set_runtime_running(std::function<bool()> callback) { impl_->runtime_running = std::move(callback); }
void UnifiedSessionWorkspace::set_before_home(std::function<void()> callback) { impl_->before_home = std::move(callback); }
bool UnifiedSessionWorkspace::eventFilter(QObject* watched, QEvent* event) {
    if (watched == impl_->window && event->type() == QEvent::Close && impl_->active) {
        static_cast<QCloseEvent*>(event)->ignore(); request_disconnect(); return true;
    }
    return false;
}
} // namespace redclaw::ui
