#include "ui/file_transfer_results_dialog.h"
#include "redclaw/workspace/transfer_result_journal.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace redclaw::ui {
namespace {
class ResultsDialog final : public QDialog {
public:
    ResultsDialog(const QString& journal, QWidget* parent) : QDialog(parent), watcher_(this) {
#ifdef _WIN32
        journal_ = journal.toStdWString();
#else
        journal_ = journal.toStdString();
#endif
        setObjectName("fileTransferResultsDialog"); setWindowTitle(QString::fromUtf8("传送结果")); resize(760, 480);
        auto* layout = new QVBoxLayout(this);
        auto* explanation = new QLabel(QString::fromUtf8("已完成的文件和目录会保留；中止传送不会删除这些结果。"), this);
        explanation->setWordWrap(true); layout->addWidget(explanation);
        entries_ = new QTreeWidget(this); entries_->setObjectName("fileTransferResultsEntries");
        entries_->setRootIsDecorated(false); entries_->setColumnCount(3);
        entries_->setHeaderLabels({QString::fromUtf8("接收路径"), QString::fromUtf8("大小"), QString::fromUtf8("结果")});
        entries_->setStyleSheet("QHeaderView::section { background: #142338; color: #dce8f6; border: 1px solid #2b4059; padding: 6px; }");
        entries_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        entries_->setColumnWidth(1, 110); entries_->setColumnWidth(2, 130); layout->addWidget(entries_, 1);
        auto* row = new QHBoxLayout;
        previous_ = new QPushButton(QString::fromUtf8("上一页"), this); previous_->setObjectName("fileTransferResultsPrevious");
        next_ = new QPushButton(QString::fromUtf8("下一页"), this); next_->setObjectName("fileTransferResultsNext");
        status_ = new QLabel(this); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true);
        status_->setObjectName("fileTransferResultsStatus");
        row->addWidget(previous_); row->addWidget(status_, 1); row->addWidget(next_); layout->addLayout(row);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this); layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(previous_, &QPushButton::clicked, this, [this] { load(page_.begin, true); });
        connect(next_, &QPushButton::clicked, this, [this] { load(page_.end, false); });
        connect(&watcher_, &QFutureWatcher<workspace::TransferResultPage>::finished, this, [this] {
            page_ = watcher_.result(); entries_->clear();
            for (const auto& entry : page_.entries) {
                const auto path = QString::fromUtf8(entry.relative_path.data(), static_cast<qsizetype>(entry.relative_path.size()));
                auto* item = new QTreeWidgetItem(entries_, {path,
                    entry.directory ? QString::fromUtf8("目录") : QString::number(entry.size) + " B",
                    entry.skipped ? QString::fromUtf8("已跳过") : entry.directory ? QString::fromUtf8("已创建 / 保留") : QString::fromUtf8("已校验并保存")});
                item->setToolTip(0, path);
            }
            status_->setText(page_.error.empty()
                ? (page_.entries.empty() ? QString::fromUtf8("没有已完成的文件或目录。") : QString::fromUtf8("本页 %1 项").arg(page_.entries.size()))
                : QString::fromUtf8("部分结果无法读取：") + QString::fromStdString(page_.error));
            previous_->setEnabled(page_.error.empty() && page_.has_previous);
            next_->setEnabled(page_.error.empty() && page_.has_next);
        });
        load(0, false);
    }
private:
    void load(std::uint64_t cursor, bool previous) {
        previous_->setEnabled(false); next_->setEnabled(false); status_->setText(QString::fromUtf8("正在读取结果…"));
        // The bounded disk job owns copies only. Closing the dialog neither
        // waits for disk I/O nor leaves a callback referencing destroyed UI.
        watcher_.setFuture(QtConcurrent::run([path = journal_, cursor, previous] {
            return workspace::read_transfer_result_page(path, cursor, previous);
        }));
    }
    std::filesystem::path journal_;
    QFutureWatcher<workspace::TransferResultPage> watcher_;
    workspace::TransferResultPage page_;
    QTreeWidget* entries_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton *previous_ = nullptr, *next_ = nullptr;
};
}
void show_file_transfer_results(const QString& journal, QWidget* parent) {
    ResultsDialog dialog(journal, parent); dialog.exec();
}
}
