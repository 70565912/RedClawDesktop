#include "ui/terminal/terminal_panel.h"
#include "ui/terminal/terminal_view.h"
#include "ui/terminal/workspace_pipe_server.h"
#include "redclaw/workspace/terminal_controller.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QDebug>
#include <QHideEvent>
#include <QLabel>
#include <QPointer>
#include <QShowEvent>
#include <QTimer>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace redclaw::ui {
struct TerminalPanel::Impl {
    WorkspacePipeServer& pipe;
    TerminalView* view = nullptr;
    QLabel* status = nullptr;
    QString runtime, profile;
    bool initialized = false, workspace_blocked = false;
    QTimer* end_timer = nullptr;
    QElapsedTimer end_clock;
    std::function<void()> after_end;
    workspace::TerminalController controller;
    explicit Impl(WorkspacePipeServer& ipc) : pipe(ipc), controller(
        [this](const auto& message) { return pipe.send(protocol::serialize_terminal_message_v1(message)); },
        {
            [this](auto id) { view->reset_session(QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()))); },
            [this](auto bytes) { return view->append_output(bytes); },
            [this](bool enabled, auto error) {
                view->set_input_enabled(enabled);
                if (error == "terminal_peer_unsupported") status->setText(QString::fromUtf8("对端版本不支持终端"));
                else if (error == "terminal_process_exited") status->setText(QString::fromUtf8("PowerShell 已退出"));
                else if (!error.empty() && error != "terminal_connection_unavailable")
                    status->setText(QString::fromUtf8("终端不可用：") + QString::fromUtf8(error.data(), static_cast<qsizetype>(error.size())));
                else status->setText(enabled ? QString::fromUtf8("对端 PowerShell") : QString::fromUtf8("终端输入已暂停"));
            }
        }) {}
};
TerminalPanel::TerminalPanel(WorkspacePipeServer& pipe, QWidget* parent,
    QString runtime, QString profile) : QWidget(parent), impl_(std::make_unique<Impl>(pipe)) {
    setObjectName("remoteTerminalPanel");
    impl_->end_timer = new QTimer(this); impl_->end_timer->setInterval(20);
    connect(impl_->end_timer, &QTimer::timeout, this, [this] {
        impl_->controller.pump();
        if (!impl_->controller.end_complete() && impl_->end_clock.elapsed() < 2000) return;
        impl_->end_timer->stop();
        if (!impl_->controller.end_acknowledged()) {
            impl_->controller.expire_end_wait();
            qWarning("terminal_end_unacknowledged: desktop closed before a peer cleanup receipt");
        }
        auto finished = std::move(impl_->after_end);
        if (finished) finished();
    });
    impl_->runtime = runtime.isEmpty() ? QDir(QCoreApplication::applicationDirPath()).filePath("terminal-runtime") : runtime;
    impl_->profile = profile.isEmpty() ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath("terminal-webview") : profile;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    impl_->status = new QLabel(QString::fromUtf8("终端输入已暂停"), this);
    impl_->status->setTextFormat(Qt::PlainText);
    impl_->status->setWordWrap(true);
    impl_->view = new TerminalView(this);
    layout->addWidget(impl_->status); layout->addWidget(impl_->view, 1);
    impl_->view->set_input_callback([this](const auto& bytes) {
        (void)impl_->controller.input({bytes.constData(), static_cast<std::size_t>(bytes.size())});
    });
    impl_->view->set_resize_callback([this](int cols, int rows) { impl_->controller.resize(cols, rows); });
    impl_->view->set_writable_callback([this] {
        impl_->controller.surface_ready(impl_->view->ready());
        if (!impl_->view->output_pending()) impl_->controller.output_parsed();
    });
    impl_->view->set_error_callback([this](const QString& error) {
        impl_->controller.surface_ready(false);
        impl_->status->setText(QString::fromUtf8("终端无法启动：") + error);
    });
    QPointer<TerminalPanel> weak(this);
    pipe.set_receive_callback([weak](auto frame) {
        if (!weak) return;
        const auto message = protocol::parse_terminal_message_v1(frame);
        if (message.ok) (void)weak->impl_->controller.receive(message.value);
    });
    pipe.set_connection_callback([weak](bool open) {
        if (!weak) return;
        if (open) weak->start_if_visible();
        else weak->impl_->controller.disconnected();
    });
    pipe.set_writable_callback([weak] { if (weak) weak->impl_->controller.pump(); });
    impl_->controller.set_surface_input_paused(true);
}
TerminalPanel::~TerminalPanel() = default;
void TerminalPanel::end_desktop(std::function<void()> finished) {
    impl_->after_end = std::move(finished); impl_->end_clock.start();
    impl_->controller.request_end(); impl_->end_timer->start();
}
void TerminalPanel::set_workspace_blocked(bool blocked) {
    impl_->workspace_blocked = blocked;
    impl_->controller.set_surface_input_paused(blocked || !isVisible());
}
void TerminalPanel::start_if_visible() {
    if (!isVisible() && !impl_->initialized) return;
    if (!impl_->initialized) {
        impl_->initialized = true;
        impl_->view->reset_session("terminal-pending");
        impl_->view->initialize(impl_->runtime, impl_->profile);
    }
    impl_->controller.request_open();
}
void TerminalPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    impl_->controller.set_surface_input_paused(impl_->workspace_blocked);
    start_if_visible();
}
void TerminalPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    impl_->controller.set_surface_input_paused(true);
}
}
