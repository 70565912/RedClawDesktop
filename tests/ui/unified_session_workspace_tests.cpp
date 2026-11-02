#include "ui/unified_session_workspace.h"
#include "ui/connection_overlay.h"
#include "ui/desktop_task_workspace.h"
#include "ui/playback_window_fullscreen_controller.h"
#include "ui/playback_window_geometry_controller.h"
#include "ui/runtime_log_view.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QEventLoop>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
using namespace redclaw::ui;
void settle() { QApplication::processEvents(); }
ConnectionFlowSnapshot connected() {
    ConnectionFlowSnapshot flow; flow.role = ConnectionFlowRole::kController;
    flow.view = ConnectionFlowView::kConnecting; flow.status = ConnectionFlowStatus::kComplete;
    flow.transport_ready = true; flow.attempt = 1; flow.can_exit = true; return flow;
}
class UnifiedSessionFixture : public testing::Test {
protected:
    QTemporaryDir directory;
    QSettings settings{directory.filePath("ui.ini"), QSettings::IniFormat};
    QMainWindow window;
    QWidget* home = new QWidget;
    RuntimeLogView* log = nullptr;
    std::unique_ptr<UnifiedSessionWorkspace> workspace;
    std::unique_ptr<DesktopTaskWorkspace> tools;
    QWidget* canvas = nullptr;
    void SetUp() override {
        window.setAttribute(Qt::WA_QuitOnClose, false);
        auto* layout = new QVBoxLayout(home);
        log = new RuntimeLogView(home); layout->addWidget(log);
        workspace = std::make_unique<UnifiedSessionWorkspace>(&window, home, log, &settings);
        canvas = new QWidget(workspace->desktop_page()); canvas->setFocusPolicy(Qt::StrongFocus);
        workspace->desktop_page()->layout()->addWidget(canvas);
        tools = std::make_unique<DesktopTaskWorkspace>(workspace->desktop_page(), &settings);
        workspace->connection_overlay()->bind_button(tools->connection_button());
        workspace->bind_disconnect_button(tools->disconnect_button());
        window.resize(1000, 700); window.show(); workspace->update_connection(connected()); settle();
    }
    ConnectionOverlay* overlay() const { return workspace->connection_overlay(); }
    void TearDown() override { tools.reset(); workspace.reset(); }
};

TEST(UnifiedSessionOwnership, MainWindowDestroysTheActiveWorkspaceAndOwnedToolsExactlyOnce) {
    auto* window = new QMainWindow;
    window->setAttribute(Qt::WA_QuitOnClose, false);
    auto* home = new QWidget; auto* layout = new QVBoxLayout(home);
    auto* log = new RuntimeLogView(home); layout->addWidget(log);
    QPointer<UnifiedSessionWorkspace> session = new UnifiedSessionWorkspace(window, home, log, nullptr);
    auto* canvas = new QWidget(session->desktop_page()); session->desktop_page()->layout()->addWidget(canvas);
    QPointer<DesktopTaskWorkspace> tools = new DesktopTaskWorkspace(session->desktop_page(), nullptr);
    session->connection_overlay()->bind_button(tools->connection_button());
    window->resize(1000, 700); window->show(); session->update_connection(connected()); settle();
    QPointer<QWidget> panel = session->connection_overlay()->panel();
    QPointer<ConnectionOverlay> overlay = session->connection_overlay();
    QPointer<RuntimeLogView> retained_log = log;
    delete window;
    EXPECT_TRUE(session.isNull()); EXPECT_TRUE(tools.isNull()); EXPECT_TRUE(panel.isNull());
    EXPECT_TRUE(overlay.isNull()); EXPECT_TRUE(retained_log.isNull());
}

TEST_F(UnifiedSessionFixture, OneMainWindowAndOverlayNeverResizesCanvasOrPublishesViewport) {
    PlaybackWindowGeometryController geometry(&window, canvas);
    geometry.start(); geometry.publish_initial_viewport(); settle();
    const auto before = geometry.viewport_commit_total();
    const auto size = canvas->size();
    EXPECT_TRUE(workspace->desktop_visible()); EXPECT_FALSE(workspace->desktop_page()->isWindow());
    int primary = 0;
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->isVisible() && (widget->windowFlags() & Qt::WindowType_Mask) == Qt::Window) ++primary;
    EXPECT_EQ(primary, 1);
    for (auto state : {ConnectionOverlayState::kCollapsed, ConnectionOverlayState::kHidden, ConnectionOverlayState::kExpanded}) {
        overlay()->set_state(state); settle();
        EXPECT_EQ(canvas->size(), size); EXPECT_EQ(geometry.viewport_commit_total(), before);
    }
    EXPECT_EQ(log->buffer()->line_count(), 0);
}

TEST_F(UnifiedSessionFixture, FoldHideAndBothPersistentEntriesRestoreThePanel) {
    EXPECT_EQ(overlay()->state(), ConnectionOverlayState::kExpanded);
    EXPECT_TRUE(overlay()->panel()->isVisible());
    EXPECT_GE(overlay()->panel()->width(), 400); EXPECT_GE(overlay()->panel()->height(), 300);
    overlay()->panel()->findChild<QToolButton*>("connectionOverlayFold")->click();
    EXPECT_EQ(overlay()->state(), ConnectionOverlayState::kCollapsed);
    EXPECT_LE(overlay()->panel()->height(), 42); EXPECT_FALSE(log->isVisible());
    tools->connection_button()->click(); EXPECT_EQ(overlay()->state(), ConnectionOverlayState::kExpanded);
    overlay()->panel()->findChild<QToolButton*>("connectionOverlayHide")->click();
    EXPECT_FALSE(overlay()->panel()->isVisible()); EXPECT_TRUE(overlay()->status_entry()->isVisible());
    overlay()->status_entry()->findChild<QToolButton*>("connectionStatusEntry")->click();
    EXPECT_EQ(overlay()->state(), ConnectionOverlayState::kExpanded); EXPECT_TRUE(log->isVisible());
    EXPECT_TRUE(tools->connection_button()->isChecked());
}

TEST_F(UnifiedSessionFixture, CollapseShrinksNativeWindowAndRestoresExpandedGeometryAfterLayout) {
    auto* panel = overlay()->panel();
    panel->setStyleSheet("QWidget { font: 10pt 'Segoe UI'; } QToolButton { padding: 6px 2px; }");
    settle();
    const auto expanded = panel->size();
    const auto canvas_size = canvas->size();
    auto* fold = panel->findChild<QToolButton*>("connectionOverlayFold");
    ASSERT_NE(fold, nullptr);
    const auto output = qEnvironmentVariable("REDCLAW_UI_EVIDENCE_DIR");
    QFile style(qEnvironmentVariable("REDCLAW_UI_STYLE_FILE"));
    if (style.open(QIODevice::ReadOnly)) { panel->setStyleSheet(QString::fromUtf8(style.readAll())); settle(); }
    if (!output.isEmpty()) { QDir().mkpath(output); panel->grab().save(output + "/connection-expanded.png"); }
    fold->click(); settle();
    EXPECT_LE(panel->height(), 42);
    EXPECT_LT(panel->width(), expanded.width());
    EXPECT_EQ(canvas->size(), canvas_size);
    EXPECT_FALSE(log->isVisible());
#ifdef _WIN32
    RECT client{};
    ASSERT_TRUE(GetClientRect(reinterpret_cast<HWND>(panel->winId()), &client));
    EXPECT_LE(client.bottom - client.top, qRound(42 * panel->devicePixelRatioF()) + 2);
#endif
    if (!output.isEmpty()) panel->grab().save(output + "/connection-collapsed.png");
    window.resize(1200, 760); settle();
    EXPECT_LE(panel->height(), 42);
    fold->click(); settle();
    EXPECT_EQ(panel->size(), expanded); EXPECT_TRUE(log->isVisible());
    EXPECT_NE(canvas_size, canvas->size()); // Only the owner's explicit resize changes the canvas.
}

TEST_F(UnifiedSessionFixture, IncidentsAutoExpandOnceWithoutStealingFocusAndRecoveryPreservesChoice) {
    canvas->setFocus(); settle(); const auto focus = QApplication::focusWidget();
    overlay()->set_state(ConnectionOverlayState::kHidden);
    for (int i = 0; i < 20; ++i) workspace->update_connection(connected());
    EXPECT_EQ(overlay()->state(), ConnectionOverlayState::kHidden);
    auto flow = connected(); flow.status = ConnectionFlowStatus::kDisconnected; flow.transport_ready = false;
    workspace->update_connection(flow); settle();
    EXPECT_EQ(overlay()->state(), ConnectionOverlayState::kExpanded); EXPECT_EQ(QApplication::focusWidget(), focus);
    overlay()->set_state(ConnectionOverlayState::kHidden);
    workspace->update_connection(flow); EXPECT_EQ(overlay()->state(), ConnectionOverlayState::kHidden);
    flow.status = ConnectionFlowStatus::kRetryWaiting; workspace->update_connection(flow);
    EXPECT_EQ(overlay()->state(), ConnectionOverlayState::kExpanded);
    overlay()->set_state(ConnectionOverlayState::kCollapsed); workspace->update_connection(connected());
    EXPECT_EQ(overlay()->state(), ConnectionOverlayState::kCollapsed);
    flow.status = ConnectionFlowStatus::kFailed; workspace->update_connection(flow);
    EXPECT_EQ(overlay()->state(), ConnectionOverlayState::kExpanded);
}

TEST_F(UnifiedSessionFixture, MissingTransportEvidenceRemainsUnknownRatherThanImplyingNegotiationOrDirect) {
    auto contains = [&](const QString& text) {
        const auto labels = overlay()->panel()->findChildren<QLabel*>();
        return std::any_of(labels.begin(), labels.end(), [&](auto* label) { return label->text().contains(text); });
    };
    workspace->update_runtime("direct", "frame available"); EXPECT_TRUE(contains(QString::fromUtf8("直连")));
    workspace->update_runtime("relay", "frame available"); EXPECT_TRUE(contains(QString::fromUtf8("中继")));
    workspace->update_runtime("unknown", "frame available"); EXPECT_TRUE(contains(QString::fromUtf8("路径未知")));
    EXPECT_FALSE(contains(QString::fromUtf8("协商中")));
}

TEST_F(UnifiedSessionFixture, HiddenAndCollapsedLogsDoNotRefreshAndBufferRemainsShared) {
    auto buffer = log->buffer(); log->flush_pending();
    for (auto state : {ConnectionOverlayState::kHidden, ConnectionOverlayState::kCollapsed}) {
        overlay()->set_state(state); const auto count = log->refresh_count();
        buffer->append("retained while hidden"); log->flush_pending();
        EXPECT_EQ(log->refresh_count(), count); EXPECT_EQ(log->buffer(), buffer);
    }
    overlay()->set_state(ConnectionOverlayState::kExpanded); log->flush_pending();
    EXPECT_EQ(log->toPlainText().count("retained while hidden"), 2);
    workspace->set_confirm_disconnect([] { return true; });
    workspace->set_request_stop([] {}); workspace->set_runtime_running([] { return false; });
    workspace->request_disconnect(); settle();
    EXPECT_EQ(log->parentWidget(), home); EXPECT_EQ(log->buffer(), buffer);
    workspace->update_connection(connected()); settle();
    EXPECT_TRUE(workspace->desktop_visible()); EXPECT_EQ(log->buffer(), buffer);
    EXPECT_EQ(overlay()->state(), ConnectionOverlayState::kExpanded);
}

TEST_F(UnifiedSessionFixture, CloseCancellationRetainsFullscreenAndConfirmedStopWaitsForActualCompletion) {
    PlaybackWindowFullscreenController fullscreen(&window, tools->button_bar(), workspace->desktop_page());
    workspace->set_before_home([&] { fullscreen.set_fullscreen(false); });
    fullscreen.set_fullscreen(true); settle();
    int confirmations = 0, stops = 0;
    workspace->set_confirm_disconnect([&] { ++confirmations; return false; });
    workspace->set_request_stop([&] { ++stops; }); workspace->set_runtime_running([] { return true; });
    EXPECT_FALSE(window.close()); EXPECT_TRUE(window.isFullScreen()); EXPECT_TRUE(workspace->desktop_visible());
    EXPECT_EQ(stops, 0);
    workspace->set_confirm_disconnect([&] { ++confirmations; return true; });
    EXPECT_FALSE(window.close()); EXPECT_EQ(stops, 1); EXPECT_TRUE(window.isVisible());
    EXPECT_EQ(workspace->disconnect_state(), SessionDisconnectState::kStopping);
    EXPECT_FALSE(tools->disconnect_button()->isEnabled());
    EXPECT_FALSE(window.close()); EXPECT_EQ(stops, 1); EXPECT_EQ(confirmations, 2);
    EXPECT_TRUE(workspace->desktop_visible());
    workspace->stop_failed("stop remains unconfirmed");
    EXPECT_EQ(workspace->disconnect_state(), SessionDisconnectState::kFailed);
    EXPECT_TRUE(workspace->desktop_visible()); EXPECT_TRUE(overlay()->panel()->isVisible());
    workspace->runtime_stopped(); settle();
    EXPECT_FALSE(workspace->desktop_visible()); EXPECT_FALSE(window.isFullScreen()); EXPECT_TRUE(window.isVisible());
    auto* retained = canvas; workspace->update_connection(connected()); settle();
    EXPECT_EQ(canvas, retained); EXPECT_TRUE(workspace->desktop_visible());
    workspace->runtime_stopped(); // Not a user-requested shutdown: keep failure/reconnect workspace.
    EXPECT_TRUE(workspace->desktop_visible());
    workspace->set_before_home({});
}

TEST_F(UnifiedSessionFixture, SmallWindowMoveAndReopenKeepOverlayAndToolbarInsideTheDesktop) {
    window.resize(360, 240); window.move(70, 80); settle();
    const QRect bounds(workspace->desktop_page()->mapToGlobal(QPoint()), workspace->desktop_page()->size());
    EXPECT_TRUE(bounds.contains(overlay()->panel()->geometry())) << "bounds=" << bounds.x() << "," << bounds.y() << "," << bounds.width() << "," << bounds.height() << " panel=" << overlay()->panel()->x() << "," << overlay()->panel()->y() << "," << overlay()->panel()->width() << "," << overlay()->panel()->height();
    EXPECT_TRUE(bounds.contains(overlay()->status_entry()->geometry()));
    EXPECT_TRUE(bounds.contains(tools->button_bar()->geometry()));
    overlay()->set_state(ConnectionOverlayState::kCollapsed); settle();
    window.move(100, 90); settle();
    const QRect moved(workspace->desktop_page()->mapToGlobal(QPoint()), workspace->desktop_page()->size());
    EXPECT_TRUE(moved.contains(overlay()->panel()->geometry()));
    EXPECT_LE(overlay()->panel()->height(), 42);
    window.move(70, 80); settle();
    EXPECT_TRUE(bounds.contains(overlay()->panel()->geometry())) << "bounds=" << bounds.x() << "," << bounds.y() << "," << bounds.width() << "," << bounds.height() << " panel=" << overlay()->panel()->x() << "," << overlay()->panel()->y() << "," << overlay()->panel()->width() << "," << overlay()->panel()->height();
}

TEST_F(UnifiedSessionFixture, DragPersistsPositionAndCollapseDoesNotOverwriteExpandedSize) {
    const auto expanded = overlay()->panel()->size();
    auto drag = [&](QPoint delta) {
        auto* header = overlay()->panel()->findChild<QToolButton*>("connectionOverlayFold")->parentWidget();
        const auto start = header->mapToGlobal(QPoint(8, 8));
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(8, 8), QPointF(start), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(header, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(header->mapFromGlobal(start + delta)), QPointF(start + delta), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(header, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(header->mapFromGlobal(start + delta)), QPointF(start + delta), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(header, &release); settle();
    };
    drag(QPoint(-100, -70));
    const auto first = settings.value("controller/connection_overlay/v1/geometry").toRect();
    ASSERT_TRUE(first.isValid()); EXPECT_EQ(first.size(), expanded);
    overlay()->set_state(ConnectionOverlayState::kCollapsed); drag(QPoint(-40, -20));
    const auto saved = settings.value("controller/connection_overlay/v1/geometry").toRect();
    EXPECT_EQ(saved.size(), expanded); EXPECT_NE(saved.topLeft(), first.topLeft());
    workspace->set_confirm_disconnect([] { return true; }); workspace->set_request_stop([] {});
    workspace->set_runtime_running([] { return false; }); workspace->request_disconnect();
    workspace->update_connection(connected()); settle();
    EXPECT_EQ(overlay()->state(), ConnectionOverlayState::kExpanded);
    EXPECT_EQ(overlay()->panel()->size(), expanded);
    EXPECT_EQ(overlay()->panel()->pos() - workspace->desktop_page()->mapToGlobal(QPoint()), saved.topLeft());
}

TEST_F(UnifiedSessionFixture, HostPageCloseIsNotRoutedAsControllerDisconnect) {
    ConnectionFlowSnapshot idle; workspace->update_connection(idle); settle();
    auto host = connected(); host.role = ConnectionFlowRole::kHost; host.view = ConnectionFlowView::kWaiting;
    workspace->update_connection(host);
    int confirmations = 0; workspace->set_confirm_disconnect([&] { ++confirmations; return false; });
    EXPECT_TRUE(window.close()); EXPECT_EQ(confirmations, 0); EXPECT_FALSE(workspace->desktop_visible());
}
} // namespace
