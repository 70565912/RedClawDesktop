#include "ui/file_transfer_panel.h"
#include "ui/file_transfer_results_dialog.h"
#include "ui/clipboard_copies_dialog.h"
#include <algorithm>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QProgressBar>
#include <QStackedWidget>
#include <QPushButton>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

namespace redclaw::ui {
namespace {
using Action = protocol::WorkspaceActionV1;
using Direction = protocol::TransferDirectionV1;
using Purpose = protocol::WorkspaceTransferPurposeV1;
using Control = protocol::StreamControlMessageV1;
QString qtext(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
std::string identity() { return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); }
Control control(Action action, std::string request, Direction direction = Direction::kToHost) {
    Control result; result.type = protocol::StreamControlMessageTypeV1::kWorkspace; result.request_id = std::move(request);
    // Runtime replaces this local envelope with its negotiated epoch/send ID.
    result.session_epoch = "gui-workspace"; result.message_id = 1;
    result.sent_at_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    result.workspace.emplace(); result.workspace->action = action; result.workspace->direction = direction; return result;
}
QString amount(std::uint64_t bytes) {
    return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1) + " MiB";
}
// The remote listing uses only structured, current-user browse requests. No
// local QFileDialog is pointed at a peer path and no remote shell is invoked.
class RemoteBrowser final : public QDialog {
public:
    RemoteBrowser(FileTransferPanel::Send send, bool folders_only, QWidget* parent)
        : QDialog(parent, Qt::Widget), send_(std::move(send)), folders_only_(folders_only) {
        setWindowFlags(Qt::Widget);
        setWindowTitle(QString::fromUtf8("选择对端文件或目录")); resize(720, 480);
        auto* layout = new QVBoxLayout(this); auto* row = new QHBoxLayout;
        path_ = new QLineEdit(this); path_->setPlaceholderText(QString::fromUtf8("对端目录路径"));
        auto* go = new QPushButton(QString::fromUtf8("打开"), this);
        auto* up = new QPushButton(QString::fromUtf8("上一级"), this);
        auto* drives = new QPushButton(QString::fromUtf8("磁盘"), this);
        row->addWidget(path_, 1); row->addWidget(go); row->addWidget(up); row->addWidget(drives); layout->addLayout(row);
        entries_ = new QTreeWidget(this); entries_->setColumnCount(2);
        entries_->setStyleSheet("QHeaderView::section { background: #142338; color: #dce8f6; border: 1px solid #2b4059; padding: 6px; }");
        entries_->setHeaderLabels({QString::fromUtf8("名称"), QString::fromUtf8("大小")});
        entries_->setRootIsDecorated(false); entries_->setSelectionMode(folders_only ? QAbstractItemView::SingleSelection : QAbstractItemView::ExtendedSelection);
        entries_->setColumnWidth(0, 460); layout->addWidget(entries_, 1);
        status_ = new QLabel(this); status_->setWordWrap(true); layout->addWidget(status_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(folders_only ? QString::fromUtf8("选择目录") : QString::fromUtf8("选择文件和目录"));
        layout->addWidget(buttons);
        connect(go, &QPushButton::clicked, this, [this] { browse(path_->text()); });
        connect(path_, &QLineEdit::returnPressed, this, [this] { browse(path_->text()); });
        connect(drives, &QPushButton::clicked, this, [this] { browse({}); });
        connect(up, &QPushButton::clicked, this, [this] {
            if (current_.size() <= 3) browse({});
            else {
                const auto clean = QDir::cleanPath(QDir::fromNativeSeparators(current_));
                const auto slash = clean.lastIndexOf('/');
                browse(slash <= 2 ? clean.left(3) : clean.left(slash));
            }
        });
        connect(entries_, &QTreeWidget::itemDoubleClicked, this, [this](auto* item, int) {
            if (item->data(0, Qt::UserRole + 1).toBool()) browse(item->data(0, Qt::UserRole).toString());
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            selected_.clear();
            for (auto* item : entries_->selectedItems()) {
                if (!folders_only_ || item->data(0, Qt::UserRole + 1).toBool()) selected_.push_back(item->data(0, Qt::UserRole).toString());
            }
            if (folders_only_ && selected_.isEmpty() && !current_.isEmpty() && listing_ok_) selected_.push_back(current_);
            if (selected_.isEmpty()) { status_->setText(QString::fromUtf8("请选择文件或目录。")); return; }
            accept();
        });
    }
    void start() { browse({}); }
    void receive(const Control& message) {
        if (!message.workspace || message.request_id != request_) return;
        const auto& details = *message.workspace;
        if (details.action == Action::kBrowseEntry) {
            if (folders_only_ && !details.directory) return;
            const auto path = qtext(details.path); const auto name = QFileInfo(path).fileName();
            auto* item = new QTreeWidgetItem(entries_, {name.isEmpty() ? path : name,
                details.directory ? QString::fromUtf8("目录") : amount(details.bytes)});
            item->setData(0, Qt::UserRole, path); item->setData(0, Qt::UserRole + 1, details.directory);
        } else if (details.action == Action::kBrowseEnd) {
            listing_ok_ = details.error_code.empty();
            status_->setText(listing_ok_ ? QString::fromUtf8("可选择当前目录中的文件和目录。") : qtext(details.error_code));
        }
    }
    const QStringList& selected() const { return selected_; }
private:
    void browse(QString directory) {
        request_ = identity(); current_ = directory; path_->setText(directory); entries_->clear(); listing_ok_ = false;
        status_->setText(QString::fromUtf8("正在读取对端目录…"));
        auto request = control(Action::kBrowse, request_); request.workspace->path = directory.toUtf8().toStdString();
        QString error; if (!send_(request, &error)) status_->setText(error);
    }
    FileTransferPanel::Send send_;
    bool folders_only_, listing_ok_ = false;
    std::string request_;
    QString current_;
    QStringList selected_;
    QLineEdit* path_;
    QTreeWidget* entries_;
    QLabel* status_;
};
}
struct FileTransferPanel::Impl {
    FileTransferPanel* owner;
    Send send;
    std::function<void(bool)> busy_changed;
    QPushButton* files = nullptr;
    QPushButton* copies = nullptr;
    QPushButton* cancel = nullptr;
    QPushButton* results = nullptr;
    QString results_path;
    QLabel* status = nullptr;
    QProgressBar* progress = nullptr;
    QStackedWidget* pages = nullptr;
    QWidget* selection_page = nullptr;
    QPointer<QDialog> results_page;
    QString loaded_results_path;
    QPointer<RemoteBrowser> browser;
    QPointer<ClipboardCopiesDialog> copies_dialog;
    bool available = false, runtime_busy = false, requested = false, notified_busy = false;
    bool clipboard_available = false;
    bool copies_available = false;
    bool result_visible = false;
    bool selection_complete_sent = false;
    std::uint64_t revision = 0, source_index = 0;
    QElapsedTimer rate_timer;
    std::uint64_t rate_bytes = 0;
    double bytes_per_second = 0;
    std::string operation;
    Direction direction = Direction::kToHost;
    Purpose purpose = Purpose::kFiles;
    protocol::TransferConflictV1 conflict = protocol::TransferConflictV1::kKeepBoth;
    QStringList sources;
    Impl(FileTransferPanel* widget, Send sender) : owner(widget), send(std::move(sender)) {}
    bool busy() const { return requested || runtime_busy; }
    void refresh() {
        const bool blocked = busy(); files->setEnabled(available && !blocked);
        copies->setEnabled(copies_available && !blocked);
        cancel->setVisible(blocked); progress->setVisible(blocked);
        results->setVisible(!results_path.isEmpty());
        if (selection_page) selection_page->setEnabled(available && !blocked);
        if (browser) browser->setEnabled(available && !blocked);
        if (copies_dialog) copies_dialog->setEnabled(copies_available && !blocked);
        if (blocked != notified_busy) { notified_busy = blocked; if (busy_changed) busy_changed(blocked); }
    }
    void remote_select(bool folders, std::function<void(const QStringList&)> selected) {
        if (browser || busy() || !available) return;
        auto* page = new RemoteBrowser(send, folders, pages); browser = page;
        pages->addWidget(page); pages->setCurrentWidget(page);
        QObject::connect(page, &QDialog::finished, owner, [this, page, selected = std::move(selected)](int result) {
            if (result == QDialog::Accepted && !busy() && available) selected(page->selected());
            pages->setCurrentWidget(selection_page);
            pages->removeWidget(page); browser.clear(); page->deleteLater();
        });
        page->start();
    }
    void show_selection() {
        if (browser) browser->reject();
        pages->setCurrentWidget(selection_page);
    }
    void show_results() {
        if (results_path.isEmpty()) return;
        if (!results_page || loaded_results_path != results_path) {
            if (results_page) { pages->removeWidget(results_page); results_page->deleteLater(); }
            results_page = create_file_transfer_results_page(results_path, pages);
            loaded_results_path = results_path; pages->addWidget(results_page);
            QObject::connect(results_page, &QDialog::rejected, owner, [this] { show_selection(); });
        }
        if (browser) browser->reject();
        pages->setCurrentWidget(results_page); results_page->show();
    }
    void show_copies() {
        if (busy() || !copies_available) return;
        if (browser) browser->reject();
        if (!copies_dialog) {
            copies_dialog = new ClipboardCopiesDialog(send, pages); pages->addWidget(copies_dialog);
            QObject::connect(copies_dialog, &QDialog::rejected, owner, [this] { show_selection(); });
            QObject::connect(copies_dialog, &QDialog::accepted, owner, [this] {
                show_selection();
                if (busy() || !copies_available) return;
                purpose = copies_dialog->selected_action(); direction = Direction::kToHost;
                operation = identity(); requested = true; results_path.clear(); sources.clear(); result_visible = true;
                cancel->setEnabled(true); progress->setRange(0, 0); status->setText(QString::fromUtf8("正在处理对端剪贴板副本…")); refresh();
                auto request = control(Action::kPrepare, operation); request.workspace->purpose = purpose;
                request.workspace->path = copies_dialog->selected_id();
                QString error;
                if (!send(request, &error)) { requested = false; operation.clear(); status->setText(error); refresh(); }
            });
        }
        pages->setCurrentWidget(copies_dialog); copies_dialog->show(); copies_dialog->start();
    }
    void create_selection_page() {
        auto* page = new QWidget(pages); selection_page = page;
        page->setObjectName("fileTransferSelectionPage");
        auto* layout = new QVBoxLayout(page);
        auto* mode = new QComboBox(page); mode->addItems({QString::fromUtf8("本机 → 对端"), QString::fromUtf8("对端 → 本机")});
        layout->addWidget(mode);
        auto* selected = new QListWidget(page); selected->setObjectName("fileTransferSources");
        selected->setSelectionMode(QAbstractItemView::ExtendedSelection); layout->addWidget(selected, 1);
        auto* row = new QHBoxLayout;
        auto* add_files = new QPushButton(QString::fromUtf8("添加文件"), page);
        auto* add_folder = new QPushButton(QString::fromUtf8("添加目录"), page);
        auto* remove = new QPushButton(QString::fromUtf8("移除选中"), page);
        row->addWidget(add_files); row->addWidget(add_folder); row->addWidget(remove); layout->addLayout(row);
        auto* destination = new QLineEdit(page); destination->setPlaceholderText(QString::fromUtf8("接收目录"));
        destination->setObjectName("fileTransferDestination");
        auto* choose = new QPushButton(QString::fromUtf8("选择接收目录"), page);
        auto* destination_row = new QHBoxLayout; destination_row->addWidget(destination, 1); destination_row->addWidget(choose); layout->addLayout(destination_row);
        auto* policy = new QComboBox(page); policy->addItems({QString::fromUtf8("重名时保留两个文件"), QString::fromUtf8("覆盖重名文件"), QString::fromUtf8("跳过重名文件")}); layout->addWidget(policy);
        auto* message = new QLabel(QString::fromUtf8("传送期间暂停远程键鼠、终端输入和 Agent 提交；画面与已有任务输出继续更新。"), page);
        message->setWordWrap(true); message->setTextFormat(Qt::PlainText); layout->addWidget(message);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, page);
        buttons->button(QDialogButtonBox::Ok)->setText(QString::fromUtf8("开始传送")); layout->addWidget(buttons);
        const auto append = [selected](const QStringList& paths) {
            for (const auto& path : paths) if (selected->findItems(path, Qt::MatchExactly).isEmpty()) selected->addItem(path);
        };
        QObject::connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), page, [=](int) {
            selected->clear(); destination->clear(); add_folder->setVisible(mode->currentIndex() == 0);
            add_files->setText(mode->currentIndex() == 0 ? QString::fromUtf8("添加文件") : QString::fromUtf8("选择对端文件和目录"));
        });
        QObject::connect(add_files, &QPushButton::clicked, page, [this, page, mode, append] {
            if (mode->currentIndex() == 0) append(QFileDialog::getOpenFileNames(page, QString::fromUtf8("选择本机文件")));
            else remote_select(false, append);
        });
        QObject::connect(add_folder, &QPushButton::clicked, page, [page, append] {
            const auto folder = QFileDialog::getExistingDirectory(page, QString::fromUtf8("选择本机目录")); if (!folder.isEmpty()) append({folder});
        });
        QObject::connect(remove, &QPushButton::clicked, page, [selected] { for (auto* item : selected->selectedItems()) delete item; });
        QObject::connect(choose, &QPushButton::clicked, page, [this, page, mode, destination] {
            if (mode->currentIndex() == 0) remote_select(true, [destination](const auto& paths) { if (!paths.isEmpty()) destination->setText(paths.front()); });
            else { const auto path = QFileDialog::getExistingDirectory(page, QString::fromUtf8("选择本机接收目录")); if (!path.isEmpty()) destination->setText(path); }
        });
        QObject::connect(buttons, &QDialogButtonBox::accepted, page, [this, selected, destination, mode, policy, message] {
            if (busy() || !available) return;
            if (!selected->count() || destination->text().trimmed().isEmpty()) { message->setText(QString::fromUtf8("请选择源文件和接收目录。")); return; }
            sources.clear(); for (int index = 0; index < selected->count(); ++index) sources.push_back(selected->item(index)->text());
            direction = mode->currentIndex() == 0 ? Direction::kToHost : Direction::kToController;
            purpose = Purpose::kFiles;
            conflict = static_cast<protocol::TransferConflictV1>(policy->currentIndex());
            auto request = control(Action::kPrepare, identity(), direction); request.workspace->conflict = conflict;
            request.workspace->path = destination->text().trimmed().toUtf8().toStdString();
            operation = request.request_id; requested = true; source_index = 0; selection_complete_sent = false; result_visible = false;
            rate_timer.invalidate(); rate_bytes = 0; bytes_per_second = 0;
            cancel->setEnabled(true); progress->setRange(0, 0); status->setText(QString::fromUtf8("正在准备传送…")); refresh();
            QString error;
            if (!send(request, &error)) { requested = false; status->setText(error); refresh(); message->setText(error); }
        });
        pages->addWidget(page);
    }
    void send_next_source() {
        if (!requested || selection_complete_sent || purpose != Purpose::kFiles) return;
        const bool complete = source_index >= static_cast<std::uint64_t>(sources.size());
        auto request = control(complete ? Action::kSelectionComplete : Action::kSelectSource, operation, direction);
        if (!complete) request.workspace->path = sources[static_cast<qsizetype>(source_index)].toUtf8().toStdString();
        request.workspace->conflict = conflict;
        QString error;
        if (!send(request, &error)) { status->setText(error); cancel_transfer(); return; }
        selection_complete_sent = complete;
    }
    void cancel_transfer() {
        if (!busy() || operation.empty()) return;
        QString error;
        auto request = control(Action::kCancel, operation, direction); request.workspace->purpose = purpose;
        if (send(request, &error)) {
            cancel->setEnabled(false); status->setText(QString::fromUtf8("正在中止传送并清理未完成文件…"));
        } else status->setText(error);
    }
};
FileTransferPanel::FileTransferPanel(Send send, QWidget* parent) : QWidget(parent), impl_(std::make_unique<Impl>(this, std::move(send))) {
    setObjectName("fileTransferPanel");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    auto* row = new QHBoxLayout();
    impl_->files = new QPushButton(QString::fromUtf8("文件传送"), this); impl_->files->setObjectName("fileTransferButton");
    impl_->copies = new QPushButton(QString::fromUtf8("剪贴板文件"), this); impl_->copies->setObjectName("clipboardCopiesButton");
    impl_->status = new QLabel(QString::fromUtf8("等待对端文件传送能力"), this); impl_->status->setWordWrap(true);
    impl_->status->setTextFormat(Qt::PlainText);
    impl_->status->setObjectName("fileTransferStatus");
    impl_->progress = new QProgressBar(this); impl_->progress->setObjectName("fileTransferProgress");
    impl_->progress->setStyleSheet("QProgressBar { background: #142338; border: 1px solid #2b4059; border-radius: 4px; text-align: center; color: #dce8f6; } QProgressBar::chunk { background: #0d998f; }");
    impl_->cancel = new QPushButton(QString::fromUtf8("中止传送"), this); impl_->cancel->setObjectName("fileTransferCancel");
    impl_->results = new QPushButton(QString::fromUtf8("查看结果"), this); impl_->results->setObjectName("fileTransferResultsButton");
    row->addWidget(impl_->files); row->addWidget(impl_->copies);
    row->addWidget(impl_->results);
    layout->addLayout(row);
    impl_->pages = new QStackedWidget(this); layout->addWidget(impl_->pages, 1);
    impl_->create_selection_page();
    layout->addWidget(impl_->status);
    auto* progress_row = new QHBoxLayout(); progress_row->addWidget(impl_->progress, 1); progress_row->addWidget(impl_->cancel);
    layout->addLayout(progress_row);
    connect(impl_->results, &QPushButton::clicked, this, [this] { impl_->show_results(); });
    connect(impl_->files, &QPushButton::clicked, this, [this] { impl_->show_selection(); });
    connect(impl_->copies, &QPushButton::clicked, this, [this] { impl_->show_copies(); });
    connect(impl_->cancel, &QPushButton::clicked, this, [this] { impl_->cancel_transfer(); }); impl_->refresh();
}
FileTransferPanel::~FileTransferPanel() = default;
void FileTransferPanel::set_busy_callback(std::function<void(bool)> callback) { impl_->busy_changed = std::move(callback); }
bool FileTransferPanel::busy() const { return impl_->busy(); }
void FileTransferPanel::request_clipboard_paste(std::uint32_t sequence) {
    if (impl_->busy()) return;
    impl_->result_visible = true;
    if (!impl_->clipboard_available || !sequence) {
        impl_->status->setText(QString::fromUtf8("无法传送剪贴板：对端尚不支持此能力，或本机剪贴板不可用。")); return;
    }
    impl_->purpose = Purpose::kClipboard; impl_->direction = Direction::kToHost;
    impl_->operation = identity(); impl_->requested = true; impl_->results_path.clear(); impl_->sources.clear();
    impl_->rate_timer.invalidate(); impl_->rate_bytes = 0; impl_->bytes_per_second = 0;
    impl_->cancel->setEnabled(true); impl_->progress->setRange(0, 0);
    impl_->status->setText(QString::fromUtf8("正在准备剪贴板；完成传送并复核对端焦点后粘贴…")); impl_->refresh();
    auto request = control(Action::kPrepare, impl_->operation);
    request.workspace->purpose = Purpose::kClipboard; request.workspace->clipboard_sequence = sequence;
    QString error;
    if (!impl_->send(request, &error)) { impl_->requested = false; impl_->operation.clear(); impl_->status->setText(error); impl_->refresh(); }
}
void FileTransferPanel::cancel_clipboard_paste() {
    if (impl_->purpose == Purpose::kClipboard && impl_->cancel->isEnabled()) impl_->cancel_transfer();
}
void FileTransferPanel::runtime_stopped() {
    impl_->clipboard_available = impl_->available = impl_->runtime_busy = impl_->requested = false;
    impl_->copies_available = false;
    impl_->status->setText(QString::fromUtf8("连接已结束；未完成的传送不会自动重放。")); impl_->refresh();
    if (impl_->browser) impl_->browser->reject();
    if (impl_->copies_dialog) impl_->copies_dialog->reject();
}
void FileTransferPanel::receive(const Control& message) {
    if (message.type != protocol::StreamControlMessageTypeV1::kWorkspace || !message.workspace) return;
    if (impl_->browser) impl_->browser->receive(message);
    if (impl_->copies_dialog) impl_->copies_dialog->receive(message);
    const auto& details = *message.workspace;
    if (details.action == Action::kAvailability) {
        impl_->available = message.file_transfer_version >= 1; impl_->runtime_busy = details.active;
        impl_->clipboard_available = message.clipboard_version >= 1;
        impl_->copies_available = message.clipboard_version >= 2;
        if (details.active && impl_->operation.empty()) { impl_->operation = message.request_id; impl_->direction = details.direction; }
        impl_->revision = details.operation_revision;
        if (!impl_->busy() && !impl_->result_visible) impl_->status->setText(impl_->available ? QString::fromUtf8("文件传送可用") : QString::fromUtf8("对端未连接或版本尚不支持文件传送"));
        impl_->refresh(); return;
    }
    if (message.request_id != impl_->operation || details.purpose != impl_->purpose) return;
    switch (details.action) {
    case Action::kPrepared: impl_->send_next_source(); break;
    case Action::kSourceAccepted:
        if (details.accepted_sources == impl_->source_index + 1) { ++impl_->source_index; impl_->send_next_source(); } break;
    case Action::kScanProgress:
        impl_->progress->setRange(0, 0);
        impl_->status->setText(impl_->purpose == Purpose::kClipboard
            ? QString::fromUtf8("正在准备剪贴板：%1").arg(amount(details.bytes))
            : QString::fromUtf8("正在扫描：%1 个项目，%2").arg(details.entries).arg(amount(details.bytes))); break;
    case Action::kOffer: impl_->status->setText(QString::fromUtf8("扫描完成，正在准备接收…")); break;
    case Action::kReady:
        impl_->rate_timer.start(); impl_->rate_bytes = 0;
        impl_->status->setText(QString::fromUtf8("正在接收并校验…")); break;
    case Action::kProgress:
        if (impl_->purpose == Purpose::kClipboardCleanup || impl_->purpose == Purpose::kClipboardOpenCopy) {
            impl_->progress->setRange(0, 0);
            impl_->status->setText(impl_->purpose == Purpose::kClipboardCleanup
                ? QString::fromUtf8("正在清理剪贴板副本，已移除 %1 项；可中止后续清理。").arg(details.completed_entries)
                : QString::fromUtf8("正在对端打开副本目录…")); break;
        }
        impl_->progress->setRange(0, 1000);
        impl_->progress->setValue(details.bytes ? static_cast<int>(1000.0L * details.completed_bytes / details.bytes)
            : (details.entries ? static_cast<int>(1000.0L * details.completed_entries / details.entries) : 0));
        if (impl_->rate_timer.isValid() && impl_->rate_timer.elapsed() >= 200) {
            impl_->bytes_per_second = details.completed_bytes >= impl_->rate_bytes
                ? static_cast<double>(details.completed_bytes - impl_->rate_bytes) * 1000.0 / impl_->rate_timer.elapsed() : 0.0;
            impl_->rate_bytes = details.completed_bytes; impl_->rate_timer.restart();
        } else if (!impl_->rate_timer.isValid()) { impl_->rate_timer.start(); impl_->rate_bytes = details.completed_bytes; }
        impl_->status->setText(QString::fromUtf8("%1 · %2/s\n已接收 %3 / %4；已校验 %5；文件 %6 / %7\n%8")
            .arg(impl_->direction == Direction::kToHost ? QString::fromUtf8("本机 → 对端") : QString::fromUtf8("对端 → 本机"))
            .arg(amount(static_cast<std::uint64_t>(impl_->bytes_per_second)))
            .arg(amount(details.completed_bytes)).arg(amount(details.bytes)).arg(amount(details.committed_bytes))
            .arg(details.completed_files).arg(details.files)
            .arg(impl_->status->fontMetrics().elidedText(qtext(details.path), Qt::ElideMiddle, std::max(120, impl_->status->width()))));
        impl_->status->setToolTip(qtext(details.path)); break;
    case Action::kCancel: impl_->cancel->setEnabled(false); impl_->status->setText(QString::fromUtf8("正在中止传送并等待双端清理…")); break;
    case Action::kFinished:
    case Action::kError:
        impl_->requested = false;
        impl_->result_visible = true;
        impl_->results_path = qtext(details.results_path);
        impl_->status->setToolTip({});
        impl_->status->setText((details.error_code.empty() ? QString::fromUtf8("传送完成。")
            : QString::fromUtf8("传送已结束：") + qtext(details.error_code))
            + QString::fromUtf8(" 已校验 %1；完成 %2 / %3 个文件，跳过 %4 项。")
                .arg(amount(details.committed_bytes)).arg(details.completed_files).arg(details.files).arg(details.skipped_entries));
        if (impl_->purpose == Purpose::kClipboard) impl_->status->setText(
            details.error_code.empty() && details.paste_submitted
                ? QString::fromUtf8("剪贴板已送达，已向原对端窗口提交一次粘贴。")
                : QString::fromUtf8("剪贴板传送已结束，未确认粘贴：") + qtext(details.error_code));
        if (impl_->purpose == Purpose::kClipboardCleanup || impl_->purpose == Purpose::kClipboardOpenCopy)
            impl_->status->setText(!details.error_code.empty() ? QString::fromUtf8("副本操作已结束：") + qtext(details.error_code)
                : impl_->purpose == Purpose::kClipboardCleanup ? QString::fromUtf8("剪贴板副本清理完成。") : QString::fromUtf8("已在对端打开副本目录。"));
        impl_->sources.clear(); impl_->operation.clear(); impl_->refresh(); break;
    default: break;
    }
}
}
