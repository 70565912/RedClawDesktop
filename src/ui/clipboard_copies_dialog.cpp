#include "ui/clipboard_copies_dialog.h"
#include <QDateTime>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace redclaw::ui {
using Action = protocol::WorkspaceActionV1;
using Purpose = protocol::WorkspaceTransferPurposeV1;
ClipboardCopiesDialog::ClipboardCopiesDialog(FileTransferPanel::Send send, QWidget* parent)
    : QDialog(parent, Qt::Widget), send_(std::move(send)) {
    setWindowFlags(Qt::Widget);
    setObjectName("clipboardCopiesDialog"); setWindowTitle(QString::fromUtf8("对端剪贴板文件副本")); resize(620, 400);
    auto* layout = new QVBoxLayout(this);
    auto* description = new QLabel(QString::fromUtf8("这里保留剪贴板传送已复制的文件，包括粘贴取消后留下的完整文件。清理会删除选中批次的副本；中止后尚未清理的文件继续保留。"), this);
    description->setWordWrap(true); layout->addWidget(description);
    entries_ = new QListWidget(this); entries_->setObjectName("clipboardCopiesList"); layout->addWidget(entries_, 1);
    status_ = new QLabel(this); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    open_ = buttons->addButton(QString::fromUtf8("在对端打开目录"), QDialogButtonBox::ActionRole);
    cleanup_ = buttons->addButton(QString::fromUtf8("清理选中副本"), QDialogButtonBox::ActionRole);
    open_->setObjectName("clipboardCopiesOpen"); cleanup_->setObjectName("clipboardCopiesCleanup");
    open_->setEnabled(false); cleanup_->setEnabled(false); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(entries_, &QListWidget::itemSelectionChanged, this, [this] {
        open_->setEnabled(listing_ok_ && !entries_->selectedItems().isEmpty()); cleanup_->setEnabled(open_->isEnabled());
    });
    connect(open_, &QPushButton::clicked, this, [this] { select(Purpose::kClipboardOpenCopy); });
    connect(cleanup_, &QPushButton::clicked, this, [this] { select(Purpose::kClipboardCleanup); });
}
void ClipboardCopiesDialog::start() {
    listing_ok_ = false; selected_id_.clear(); entries_->clear();
    open_->setEnabled(false); cleanup_->setEnabled(false);
    request_ = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    protocol::StreamControlMessageV1 request;
    request.type = protocol::StreamControlMessageTypeV1::kWorkspace; request.session_epoch = "gui-workspace";
    request.message_id = 1; request.sent_at_ms = QDateTime::currentMSecsSinceEpoch(); request.request_id = request_;
    request.workspace.emplace(); request.workspace->action = Action::kBrowseClipboardCopies;
    status_->setText(QString::fromUtf8("正在读取对端剪贴板副本…"));
    QString error; if (!send_(request, &error)) status_->setText(error);
}
void ClipboardCopiesDialog::receive(const protocol::StreamControlMessageV1& message) {
    if (message.request_id != request_ || !message.workspace) return;
    const auto& details = *message.workspace;
    if (details.action == Action::kBrowseEntry) {
        if (details.path.size() != 32 || !std::all_of(details.path.begin(), details.path.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) return;
        const auto id = QString::fromLatin1(details.path.data(), static_cast<qsizetype>(details.path.size()));
        const auto time = details.created_at_ms ? QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(details.created_at_ms)).toString("yyyy-MM-dd HH:mm:ss")
            : QString::fromUtf8("时间未知");
        auto* item = new QListWidgetItem(time + "  ·  " + id.left(8), entries_);
        item->setData(Qt::UserRole, id); item->setToolTip(id);
    } else if (details.action == Action::kBrowseEnd) {
        listing_ok_ = details.error_code.empty();
        status_->setText(listing_ok_ ? (entries_->count() ? QString::fromUtf8("请选择需要打开或清理的副本。")
            : QString::fromUtf8("暂无剪贴板文件副本。")) : QString::fromStdString(details.error_code));
        open_->setEnabled(listing_ok_ && !entries_->selectedItems().isEmpty()); cleanup_->setEnabled(open_->isEnabled());
    }
}
void ClipboardCopiesDialog::select(Purpose action) {
    const auto selected = entries_->selectedItems();
    if (!listing_ok_ || selected.size() != 1) return;
    selected_id_ = selected.front()->data(Qt::UserRole).toString().toStdString(); action_ = action; accept();
}
}
