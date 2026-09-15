#include "ui/file_transfer_panel.h"
#include "ui/file_transfer_results_dialog.h"
#include "redclaw/workspace/transfer_result_journal.h"
#include <gtest/gtest.h>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTreeWidget>

namespace {
using namespace redclaw::protocol;
TEST(FileTransferPanel, RealDialogSendsValidFramesAndWaitsForRuntimeCleanupBeforeUnblocking) {
    std::vector<StreamControlMessageV1> sent;
    std::vector<bool> gate;
    redclaw::ui::FileTransferPanel panel([&](const auto& message, QString*) {
        const auto frame = serialize_local_runtime_control_frame_v2(message);
        EXPECT_FALSE(frame.empty());
        if (frame.empty()) return false;
        const auto parsed = parse_local_runtime_control_frame_v2(frame);
        EXPECT_TRUE(parsed.ok) << parsed.error;
        sent.push_back(message); return parsed.ok;
    });
    panel.set_busy_callback([&](bool busy) { gate.push_back(busy); });
    StreamControlMessageV1 state;
    state.type = StreamControlMessageTypeV1::kWorkspace; state.request_id = "workspace-status";
    state.file_transfer_version = 1; state.workspace.emplace(); state.workspace->action = WorkspaceActionV1::kAvailability;
    panel.receive(state);
    auto* button = panel.findChild<QPushButton*>("fileTransferButton");
    ASSERT_TRUE(button && button->isEnabled());
    QTimer::singleShot(0, &panel, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { ADD_FAILURE() << "transfer dialog absent"; return; }
        auto* sources = dialog->findChild<QListWidget*>("fileTransferSources");
        auto* destination = dialog->findChild<QLineEdit*>("fileTransferDestination");
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (!sources || !destination || !buttons) { ADD_FAILURE() << "transfer choices absent"; dialog->reject(); return; }
        sources->addItem("C:/selected/file.txt"); destination->setText("D:/received");
        buttons->button(QDialogButtonBox::Ok)->click();
        if (sent.empty()) dialog->reject();
    });
    button->click();
    ASSERT_EQ(sent.size(), 1U); EXPECT_EQ(sent.back().workspace->action, WorkspaceActionV1::kPrepare);
    EXPECT_TRUE(panel.busy()); EXPECT_EQ(gate, (std::vector<bool>{true}));
    state.request_id = sent.front().request_id; state.workspace->active = true; state.workspace->operation_revision = 1;
    panel.receive(state);
    auto response = state; response.workspace->active = false; response.workspace->operation_revision = 0;
    response.workspace->action = WorkspaceActionV1::kPrepared; panel.receive(response);
    ASSERT_EQ(sent.size(), 2U); EXPECT_EQ(sent.back().workspace->action, WorkspaceActionV1::kSelectSource);
    response.workspace->action = WorkspaceActionV1::kSourceAccepted; response.workspace->accepted_sources = 1; panel.receive(response);
    ASSERT_EQ(sent.size(), 3U); EXPECT_EQ(sent.back().workspace->action, WorkspaceActionV1::kSelectionComplete);
    response.workspace->action = WorkspaceActionV1::kProgress;
    response.workspace->entries = 2; response.workspace->files = 1; response.workspace->bytes = 1048576;
    response.workspace->completed_bytes = 524288; response.workspace->path = "folder/<b>file.bin</b>";
    panel.receive(response);
    const auto* status = panel.findChild<QLabel*>("fileTransferStatus");
    ASSERT_TRUE(status); EXPECT_EQ(status->textFormat(), Qt::PlainText);
    EXPECT_TRUE(status->text().contains(QString::fromUtf8("已接收 0.5 MiB / 1.0 MiB")));
    EXPECT_TRUE(status->text().contains(QString::fromUtf8("已校验 0.0 MiB")));
    EXPECT_EQ(status->toolTip(), QString::fromUtf8("folder/<b>file.bin</b>"));
    response.workspace->path.clear();
    panel.findChild<QPushButton*>("fileTransferCancel")->click();
    EXPECT_EQ(sent.back().workspace->action, WorkspaceActionV1::kCancel); EXPECT_TRUE(panel.busy());
    response.workspace->action = WorkspaceActionV1::kFinished; response.workspace->error_code = "transfer_cancelled"; panel.receive(response);
    EXPECT_TRUE(panel.busy()); // runtime's independent transfer reason still set
    state.workspace->active = false; panel.receive(state);
    EXPECT_FALSE(panel.busy()); EXPECT_EQ(gate, (std::vector<bool>{true, false}));
    EXPECT_TRUE(panel.findChild<QLabel*>("fileTransferStatus")->text().contains("transfer_cancelled"));
}
TEST(FileTransferPanel, ResultDialogReadsBoundedPagesWithoutLosingSkipOrUnicodeResults) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows result journal.";
#else
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    redclaw::workspace::TransferResultJournal journal;
    std::string error;
    ASSERT_TRUE(journal.begin(std::filesystem::path(directory.path().toStdWString()), &error)) << error;
    const auto unicode = QString::fromUtf8("目录/已完成-").toUtf8().toStdString();
    for (unsigned i = 0; i < 129; ++i)
        ASSERT_TRUE(journal.append({unicode + std::to_string(i), i, false, i == 128}, &error)) << error;
    journal.finish();
    QTimer timer; QElapsedTimer elapsed; elapsed.start(); int phase = 0;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        if (elapsed.elapsed() > 5000) { ADD_FAILURE() << "result page timeout"; dialog->reject(); return; }
        auto* rows = dialog->findChild<QTreeWidget*>("fileTransferResultsEntries");
        auto* next = dialog->findChild<QPushButton*>("fileTransferResultsNext");
        auto* previous = dialog->findChild<QPushButton*>("fileTransferResultsPrevious");
        if (!rows || !next || !previous) { ADD_FAILURE() << "result controls absent"; dialog->reject(); return; }
        if (phase == 0 && next->isEnabled()) {
            EXPECT_EQ(rows->topLevelItemCount(), 128); EXPECT_FALSE(previous->isEnabled());
            EXPECT_EQ(rows->topLevelItem(0)->text(0), QString::fromUtf8("目录/已完成-0"));
            ++phase; next->click();
        } else if (phase == 1 && previous->isEnabled()) {
            EXPECT_EQ(rows->topLevelItemCount(), 1); EXPECT_FALSE(next->isEnabled());
            EXPECT_EQ(rows->topLevelItem(0)->text(2), QString::fromUtf8("已跳过"));
            ++phase; previous->click();
        } else if (phase == 2 && next->isEnabled()) {
            EXPECT_EQ(rows->topLevelItemCount(), 128); ++phase; dialog->reject();
        }
    });
    timer.start(10);
    redclaw::ui::show_file_transfer_results(QString::fromStdWString(journal.path().wstring()), nullptr);
    EXPECT_EQ(phase, 3);
#endif
}
TEST(FileTransferPanel, ClipboardUsesOneDeferredPrepareAndCancelsWithoutSelectingOrReplaying) {
    std::vector<StreamControlMessageV1> sent;
    redclaw::ui::FileTransferPanel panel([&](const auto& message, QString*) {
        EXPECT_FALSE(serialize_local_runtime_control_frame_v2(message).empty()); sent.push_back(message); return true;
    });
    panel.request_clipboard_paste(19); EXPECT_TRUE(sent.empty()); EXPECT_FALSE(panel.busy());
    StreamControlMessageV1 status; status.type = StreamControlMessageTypeV1::kWorkspace;
    status.file_transfer_version = status.clipboard_version = 1;
    status.workspace.emplace(); status.workspace->action = WorkspaceActionV1::kAvailability;
    panel.receive(status); panel.request_clipboard_paste(19);
    ASSERT_EQ(sent.size(), 1U); EXPECT_TRUE(panel.busy());
    EXPECT_EQ(sent.front().workspace->purpose, WorkspaceTransferPurposeV1::kClipboard);
    EXPECT_EQ(sent.front().workspace->clipboard_sequence, 19U); EXPECT_TRUE(sent.front().workspace->path.empty());
    panel.request_clipboard_paste(20); EXPECT_EQ(sent.size(), 1U);
    auto reply = sent.front(); reply.workspace->clipboard_sequence = 0; reply.workspace->action = WorkspaceActionV1::kPrepared;
    panel.receive(reply); EXPECT_EQ(sent.size(), 1U);
    panel.cancel_clipboard_paste(); ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent.back().workspace->action, WorkspaceActionV1::kCancel);
    EXPECT_EQ(sent.back().workspace->purpose, WorkspaceTransferPurposeV1::kClipboard);
    panel.cancel_clipboard_paste(); EXPECT_EQ(sent.size(), 2U); EXPECT_TRUE(panel.busy());
    reply.workspace->action = WorkspaceActionV1::kFinished; reply.workspace->error_code = "clipboard_focus_changed";
    panel.receive(reply); EXPECT_FALSE(panel.busy());
    EXPECT_TRUE(panel.findChild<QLabel*>("fileTransferStatus")->text().contains("clipboard_focus_changed"));
    panel.receive(status); EXPECT_EQ(sent.size(), 2U);
}
TEST(FileTransferPanel, ClipboardCopiesRequireVersionTwoAndSendOnlySelectedOpaqueId) {
    std::vector<StreamControlMessageV1> sent;
    redclaw::ui::FileTransferPanel panel([&](const auto& request, QString*) {
        EXPECT_FALSE(serialize_local_runtime_control_frame_v2(request).empty()); sent.push_back(request); return true;
    });
    StreamControlMessageV1 state; state.type = StreamControlMessageTypeV1::kWorkspace;
    state.file_transfer_version = state.clipboard_version = 1; state.workspace.emplace();
    state.workspace->action = WorkspaceActionV1::kAvailability; panel.receive(state);
    auto* copies = panel.findChild<QPushButton*>("clipboardCopiesButton"); ASSERT_TRUE(copies); EXPECT_FALSE(copies->isEnabled());
    state.clipboard_version = 2; panel.receive(state); ASSERT_TRUE(copies->isEnabled());
    const std::string id(32, 'a');
    QTimer::singleShot(0, &panel, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || sent.empty()) { ADD_FAILURE() << "copies dialog missing"; if (dialog) dialog->reject(); return; }
        auto reply = sent.back(); EXPECT_EQ(reply.workspace->action, WorkspaceActionV1::kBrowseClipboardCopies);
        reply.workspace->action = WorkspaceActionV1::kBrowseEntry; reply.workspace->path = id;
        reply.workspace->directory = true; reply.workspace->entries = 1; reply.workspace->created_at_ms = 1700000000000;
        panel.receive(reply); reply.workspace->action = WorkspaceActionV1::kBrowseEnd; reply.workspace->path.clear(); reply.workspace->created_at_ms = 0;
        panel.receive(reply);
        auto* list = dialog->findChild<QListWidget*>("clipboardCopiesList");
        auto* cleanup = dialog->findChild<QPushButton*>("clipboardCopiesCleanup");
        if (!list || !cleanup || list->count() != 1) { ADD_FAILURE() << "copy listing absent"; dialog->reject(); return; }
        // Native UI Automation selects an item without changing currentItem.
        list->item(0)->setSelected(true); EXPECT_EQ(list->currentItem(), nullptr);
        EXPECT_TRUE(cleanup->isEnabled()); cleanup->click();
    });
    copies->click(); ASSERT_EQ(sent.size(), 2U); EXPECT_TRUE(panel.busy()); EXPECT_FALSE(copies->isEnabled());
    EXPECT_EQ(sent.back().workspace->purpose, WorkspaceTransferPurposeV1::kClipboardCleanup);
    EXPECT_EQ(sent.back().workspace->path, id);
    auto finished = sent.back(); finished.workspace->path.clear(); finished.workspace->action = WorkspaceActionV1::kFinished;
    panel.receive(finished); EXPECT_FALSE(panel.busy()); EXPECT_TRUE(copies->isEnabled());
    EXPECT_EQ(panel.findChild<QLabel*>("fileTransferStatus")->text(), QString::fromUtf8("剪贴板副本清理完成。"));
}
}
