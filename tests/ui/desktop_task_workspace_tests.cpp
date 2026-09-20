#include "ui/desktop_task_workspace.h"
#include "ui/agent_conversation_panel.h"
#include "ui/remote_input_capture.h"
#include "ui/playback_window_geometry_controller.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QEventLoop>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <array>

namespace {
using namespace redclaw::ui;
constexpr std::array tasks{DesktopTask::kAgent, DesktopTask::kTerminal,
    DesktopTask::kNavigation, DesktopTask::kFiles};
QRect bounds(QWidget& owner) { return {owner.mapToGlobal(QPoint()), owner.size()}; }
void mouse(QWidget* target, QEvent::Type type, QPoint global, Qt::MouseButton button,
    Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(target->mapFromGlobal(global)), QPointF(global),
        button, buttons, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}
void drag(QWidget* handle, QPoint local, QPoint delta) {
    const auto start = handle->mapToGlobal(local);
    mouse(handle, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(handle, QEvent::MouseMove, start + delta, Qt::NoButton, Qt::LeftButton);
    mouse(handle, QEvent::MouseButtonRelease, start + delta, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
}
void settle() {
    QEventLoop loop; QTimer::singleShot(150, &loop, &QEventLoop::quit); loop.exec();
}

TEST(DesktopTaskWorkspace, FirstUseHiddenAndClosePreservesAgentTaskAndDraft) {
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath("workspace.ini"), QSettings::IniFormat);
    settings.setValue("controller/agent_panel_expanded", true);
    QWidget owner; owner.resize(1100, 800);
    DesktopTaskWorkspace workspace(&owner, &settings);
    auto* agent = new AgentConversationPanel(&settings);
    agent->set_window_auto_resize(false);
    workspace.add_task(DesktopTask::kAgent, "Agent", agent, {420, 600});
    for (const auto task : {DesktopTask::kTerminal, DesktopTask::kNavigation, DesktopTask::kFiles})
        workspace.add_task(task, "Task", new QLineEdit("draft"), {360, 320});
    // The initial size is chosen on first opening, not during construction.
    owner.resize(1200, 900); owner.show(); QApplication::processEvents();
    for (const auto task : tasks) {
        EXPECT_FALSE(workspace.task_visible(task));
        EXPECT_FALSE(workspace.task_window(task)->isVisible());
        EXPECT_FALSE(workspace.task_button(task)->isChecked());
    }
    agent->set_current_task_id("running", redclaw::protocol::AgentTaskStateV1::kRunning);
    agent->set_instruction_text("unsent draft");
    workspace.task_button(DesktopTask::kAgent)->click();
    auto* floating = workspace.task_window(DesktopTask::kAgent);
    const auto geometry = floating->geometry();
    EXPECT_EQ(geometry.size(), QSize(420, 600));
    EXPECT_LE((geometry.center() - bounds(owner).center()).manhattanLength(), 2);
    redclaw::protocol::AgentMessageEnvelopeV1 message;
    message.type = redclaw::protocol::AgentMessageTypeV1::kEvent;
    message.task_id = "running"; message.task_state = redclaw::protocol::AgentTaskStateV1::kRunning;
    message.event_kind = "agentMessage/delta"; message.text = QString("message\n").repeated(100).toStdString();
    agent->apply_task_message(message); settle();
    EXPECT_EQ(floating->geometry(), geometry);
    floating->findChild<QToolButton*>("desktopTaskClose")->click();
    EXPECT_FALSE(workspace.task_visible(DesktopTask::kAgent));
    EXPECT_FALSE(workspace.task_button(DesktopTask::kAgent)->isChecked());
    message.text = "background output"; agent->apply_task_message(message);
    workspace.task_button(DesktopTask::kAgent)->click();
    EXPECT_EQ(agent->current_task_id(), "running");
    EXPECT_EQ(agent->current_task_state(), redclaw::protocol::AgentTaskStateV1::kRunning);
    EXPECT_EQ(agent->instruction_text(), "unsent draft");
    EXPECT_EQ(floating->geometry(), geometry);
    floating->close();
    EXPECT_FALSE(workspace.task_visible(DesktopTask::kAgent));
    EXPECT_FALSE(floating->isVisible());
}

TEST(DesktopTaskWorkspace, DragResizeAndPersistenceUseOwnerRelativeBounds) {
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath("workspace.ini"), QSettings::IniFormat);
    QRect relative; QPoint bar_position;
    {
        QWidget owner; owner.setGeometry(100, 100, 1000, 720);
        DesktopTaskWorkspace workspace(&owner, &settings);
        workspace.add_task(DesktopTask::kFiles, "Files", new QWidget, {640, 420});
        owner.show(); QApplication::processEvents();
        workspace.set_task_visible(DesktopTask::kFiles, true);
        auto* floating = workspace.task_window(DesktopTask::kFiles);
        drag(floating->findChild<QWidget*>("desktopTaskTitleBar"), {30, 12}, {-2000, -2000});
        EXPECT_EQ(floating->pos(), bounds(owner).topLeft());
        drag(floating, {floating->width() - 2, floating->height() - 2}, {2000, 2000});
        EXPECT_EQ(floating->geometry(), bounds(owner));
        drag(floating, {floating->width() - 2, floating->height() - 2}, {-400, -300});
        EXPECT_TRUE(bounds(owner).contains(floating->geometry()));
        relative = floating->geometry().translated(-bounds(owner).topLeft());
        drag(workspace.button_bar()->findChild<QLabel*>("desktopTaskBarGrip"), {10, 10}, {0, 300});
        EXPECT_TRUE(bounds(owner).contains(workspace.button_bar()->geometry()));
        bar_position = workspace.button_bar()->pos() - bounds(owner).topLeft();
        const auto before = floating->pos();
        owner.move(owner.pos() + QPoint(40, 30)); QApplication::processEvents();
        EXPECT_EQ(floating->pos(), before + QPoint(40, 30));
        owner.showMinimized(); QApplication::processEvents();
        EXPECT_FALSE(floating->isVisible()); EXPECT_FALSE(workspace.button_bar()->isVisible());
        EXPECT_TRUE(workspace.task_visible(DesktopTask::kFiles));
        EXPECT_TRUE(settings.value("controller/floating_workspace/v1/files/visible").toBool());
        owner.showNormal(); QApplication::processEvents();
        EXPECT_TRUE(floating->isVisible());
        owner.hide(); QApplication::processEvents();
        EXPECT_TRUE(workspace.task_visible(DesktopTask::kFiles));
        EXPECT_FALSE(floating->isVisible());
    }
    {
        QWidget owner; owner.setGeometry(200, 160, 1000, 720);
        DesktopTaskWorkspace workspace(&owner, &settings);
        workspace.add_task(DesktopTask::kFiles, "Files", new QWidget, {640, 420});
        owner.show(); QApplication::processEvents();
        auto* floating = workspace.task_window(DesktopTask::kFiles);
        EXPECT_TRUE(floating->isVisible());
        EXPECT_TRUE(workspace.task_button(DesktopTask::kFiles)->isChecked());
        EXPECT_EQ(floating->geometry().translated(-bounds(owner).topLeft()), relative);
        EXPECT_EQ(workspace.button_bar()->pos() - bounds(owner).topLeft(), bar_position);
        owner.resize(360, 240); QApplication::processEvents();
        EXPECT_TRUE(bounds(owner).contains(floating->geometry()));
        EXPECT_TRUE(bounds(owner).contains(workspace.button_bar()->geometry()));
        owner.resize(1000, 720); QApplication::processEvents();
        EXPECT_EQ(floating->geometry().translated(-bounds(owner).topLeft()), relative);
    }
}

TEST(DesktopTaskWorkspace, ToolsStayLocalAndFocusRestoreDoesNotResumeRemoteInput) {
    QWidget owner; owner.resize(900, 700);
    auto* layout = new QVBoxLayout(&owner); layout->setContentsMargins(0, 0, 0, 0);
    auto* canvas = new QWidget(&owner); layout->addWidget(canvas);
    DesktopTaskWorkspace workspace(&owner, nullptr);
    auto* editor = new QLineEdit;
    workspace.add_task(DesktopTask::kFiles, "Files", editor, {640, 420});
    ControllerRemoteInputCapture capture(canvas);
    workspace.set_local_interaction_callback([&] {
        capture.set_local_suspension(LocalInputSuspensionReason::kLocalUiFocus, true);
    });
    int sent = 0;
    capture.set_send_message_callback([&](const auto&, QString*) { ++sent; return true; });
    owner.show(); QApplication::processEvents();
    workspace.task_button(DesktopTask::kFiles)->click();
    mouse(editor, QEvent::MouseButtonPress, editor->mapToGlobal(QPoint(10, 10)), Qt::LeftButton, Qt::LeftButton);
    QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a"); QApplication::sendEvent(editor, &key);
    EXPECT_TRUE(capture.local_suspension_reason().contains("local_ui_focus"));
    EXPECT_EQ(editor->text(), "a"); EXPECT_EQ(sent, 0);
    workspace.task_window(DesktopTask::kFiles)->close();
    QFocusEvent focus(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
    QApplication::sendEvent(canvas, &focus);
    mouse(canvas, QEvent::MouseButtonRelease, canvas->mapToGlobal(QPoint(10, 10)), Qt::LeftButton, Qt::NoButton);
    EXPECT_TRUE(capture.local_suspension_reason().contains("local_ui_focus"));
    EXPECT_EQ(sent, 0);
    capture.set_local_suspension(LocalInputSuspensionReason::kWorkspaceTransfer, true);
    mouse(canvas, QEvent::MouseButtonPress, canvas->mapToGlobal(QPoint(10, 10)), Qt::LeftButton, Qt::LeftButton);
    EXPECT_FALSE(capture.local_suspension_reason().contains("local_ui_focus"));
    EXPECT_TRUE(capture.local_suspension_reason().contains("workspace_transfer_busy"));
    EXPECT_FALSE(capture.input_forwarding()); // A click cannot grant authorization.
    EXPECT_EQ(sent, 0);
}

TEST(DesktopTaskWorkspace, ToolbarWrapsAndTaskOperationsLeaveCanvasUnchanged) {
    QWidget owner; owner.resize(1000, 720);
    auto* layout = new QVBoxLayout(&owner); layout->setContentsMargins(0, 0, 0, 0);
    auto* canvas = new QWidget(&owner); layout->addWidget(canvas);
    PlaybackWindowGeometryController geometry(&owner, canvas);
    int viewports = 0;
    geometry.set_viewport_committed_callback([&](QSize, std::uint64_t) { ++viewports; });
    DesktopTaskWorkspace workspace(&owner, nullptr);
    for (const auto task : tasks) workspace.add_task(task, "Task window", new QWidget, {360, 320});
    owner.show(); QApplication::processEvents();
    geometry.start(); geometry.publish_initial_viewport(); settle();
    ASSERT_GT(viewports, 0);
    const auto initial_viewports = viewports;
    const auto original = canvas->geometry();
    for (const auto task : tasks) {
        workspace.task_button(task)->click();
        auto* window = workspace.task_window(task);
        drag(window->findChild<QWidget*>("desktopTaskTitleBar"), {30, 12}, {20, 35});
        drag(window, {window->width() - 2, window->height() - 2}, {30, 20});
        EXPECT_EQ(canvas->geometry(), original);
    }
    for (const auto task : tasks) EXPECT_TRUE(workspace.task_window(task)->isVisible());
    for (const auto task : tasks) workspace.task_window(task)->close();
    settle();
    EXPECT_EQ(viewports, initial_viewports);
    EXPECT_EQ(canvas->geometry(), original);
    owner.resize(360, 400); QApplication::processEvents();
    workspace.connection_status()->setText(QString("long status ").repeated(50));
    workspace.retry_button()->show(); settle();
    auto* bar = workspace.button_bar();
    EXPECT_TRUE(bar->testAttribute(Qt::WA_TranslucentBackground));
    EXPECT_TRUE(bar->windowFlags().testFlag(Qt::FramelessWindowHint));
    EXPECT_FALSE(bar->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    EXPECT_TRUE(bounds(owner).contains(bar->geometry()));
    for (auto* child : bar->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
        if (child->isVisible()) EXPECT_TRUE(bar->rect().contains(child->geometry())) << child->objectName().toStdString();
    EXPECT_GT(bar->height(), 40);
    EXPECT_EQ(canvas->size(), owner.size());
}
} // namespace
