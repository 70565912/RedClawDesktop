#include "ui/terminal/terminal_panel.h"
#include "ui/terminal/terminal_view.h"
#include "ui/terminal/workspace_pipe_server.h"
#include "ui/terminal/terminal_coordinator.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QDebug>
#include <QHideEvent>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
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
    QTimer* repaint_timer = nullptr;
    QElapsedTimer end_clock;
    std::function<void()> after_end;
    TerminalCoordinator coordinator;
    std::uint64_t view_cursor = 0;
    explicit Impl(WorkspacePipeServer& ipc) : pipe(ipc), coordinator(ipc) {}

};
TerminalPanel::TerminalPanel(WorkspacePipeServer& pipe, QWidget* parent,
    QString runtime, QString profile) : QWidget(parent), impl_(std::make_unique<Impl>(pipe)) {
    setObjectName("remoteTerminalPanel");
    impl_->end_timer = new QTimer(this); impl_->end_timer->setInterval(20);
    connect(impl_->end_timer, &QTimer::timeout, this, [this] {
        impl_->coordinator.controller().pump();
        if (!impl_->coordinator.controller().end_complete() && impl_->end_clock.elapsed() < 2000) return;
        impl_->end_timer->stop();
        if (!impl_->coordinator.controller().end_acknowledged()) {
            impl_->coordinator.controller().expire_end_wait();
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
        // The coordinator answers device queries even without a view. Replaying
        // a retained query into xterm must not send its response a second time.
        static const QRegularExpression cursor_reply("^\\x1b\\[[0-9]+;[0-9]+R$");
        if (cursor_reply.match(QString::fromLatin1(bytes)).hasMatch()) return;
        (void)impl_->coordinator.controller().input({bytes.constData(), static_cast<std::size_t>(bytes.size())});
    });
    impl_->view->set_resize_callback([this](int cols, int rows) { impl_->coordinator.controller().resize(cols, rows); });
    impl_->coordinator.set_state_callback([this](bool enabled, auto error) {
        impl_->view->set_input_enabled(enabled && isVisible());
        impl_->status->setText(!impl_->coordinator.controller().execution_id().empty()
            ? QString::fromUtf8("脚本正在使用终端；可中断")
            : !error.empty() ? QString::fromUtf8(error.data(), static_cast<qsizetype>(error.size()))
            : enabled ? QString::fromUtf8("对端 PowerShell") : QString::fromUtf8("终端输入已暂停"));
    });
    auto* interrupt = new QPushButton(QString::fromUtf8("中断执行"), this);
    layout->insertWidget(1, interrupt);
    connect(interrupt, &QPushButton::clicked, this, [this] { (void)impl_->coordinator.invoke("terminal.cancel", {}); });
    auto* repaint = impl_->repaint_timer = new QTimer(this); repaint->setInterval(20);
    connect(repaint, &QTimer::timeout, this, [this] {
        if (!impl_->initialized || !impl_->view->ready() || impl_->view->output_pending()) return;
        if (impl_->view_cursor > impl_->coordinator.next_cursor()) {
            impl_->view_cursor = impl_->coordinator.first_cursor();
            impl_->view->reset_session(QString::fromStdString(std::string(impl_->coordinator.controller().terminal_id())));
            return;
        }
        const auto page = impl_->coordinator.read(impl_->view_cursor, 16384);
        if (page.value("gap").toBool()) {
            impl_->status->setText(QString::fromUtf8("部分历史输出已过期；以下为保留的输出。"));
            impl_->view_cursor = page.value("first_cursor").toString().toULongLong();
            impl_->view->reset_session(QString::fromStdString(std::string(impl_->coordinator.controller().terminal_id())));
            return;
        }
        const auto bytes = QByteArray::fromBase64(page.value("output_base64").toString().toLatin1());
        if (bytes.isEmpty() || impl_->view->append_output({bytes.constData(), static_cast<std::size_t>(bytes.size())}))
            impl_->view_cursor = page.value("next_cursor").toString().toULongLong();
    });

}
TerminalPanel::~TerminalPanel() = default;
TerminalCoordinator& TerminalPanel::coordinator() { return impl_->coordinator; }
void TerminalPanel::end_desktop(std::function<void()> finished) {
    impl_->after_end = std::move(finished); impl_->end_clock.start();
    impl_->coordinator.controller().request_end(); impl_->end_timer->start();
}
void TerminalPanel::set_workspace_blocked(bool blocked) {
    impl_->workspace_blocked = blocked;
    impl_->coordinator.set_blocked(blocked);
}
void TerminalPanel::start_if_visible() {
    if (!isVisible() && !impl_->initialized) return;
    if (!impl_->initialized) {
        impl_->initialized = true;
        impl_->view->reset_session("terminal-pending");
        impl_->view->initialize(impl_->runtime, impl_->profile);
    }
    impl_->coordinator.controller().request_open();
}
void TerminalPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    impl_->view->set_input_enabled(impl_->coordinator.controller().input_enabled()
        && impl_->coordinator.controller().execution_id().empty());
    impl_->repaint_timer->start();
    start_if_visible();
}
void TerminalPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    impl_->view->set_input_enabled(false);
    impl_->repaint_timer->stop();
}
}
