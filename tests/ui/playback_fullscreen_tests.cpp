#include "ui/playback_window_fullscreen_controller.h"
#include "ui/playback_window_geometry_controller.h"
#include "ui/playback_window_lifecycle.h"
#include "ui/desktop_task_workspace.h"
#include <gtest/gtest.h>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QScreen>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
void settle() { for (int i = 0; i < 8; ++i) QApplication::processEvents(); }
struct FullscreenFixture : testing::Test {
    QWidget window{nullptr, Qt::Window};
    redclaw::ui::DesktopTaskWorkspace workspace{&window, nullptr};
    redclaw::ui::PlaybackWindowFullscreenController mode{&window, workspace.button_bar()};
    QWidget* canvas = new QWidget(&window);
    void SetUp() override {
        auto* layout = new QVBoxLayout(&window); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(canvas);
        window.setGeometry(QRect(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(50, 50), QSize(520, 340)));
        QObject::connect(workspace.fullscreen_button(), &QToolButton::clicked, &mode, [this] { mode.toggle_fullscreen(); });
        mode.set_changed_callback([this](bool enabled) {
            const QSignalBlocker blocker(workspace.fullscreen_button());
            workspace.fullscreen_button()->setChecked(enabled);
        });
        window.show(); settle();
    }
    void key(QWidget* target, QEvent::Type type, int code, Qt::KeyboardModifiers modifiers, bool repeat = false) {
        QKeyEvent event(type, code, modifiers, {}, repeat); QApplication::sendEvent(target, &event);
    }
};

TEST_F(FullscreenFixture, ToolbarRestoresNormalGeometryAndKeepsToolsInsideCanvas) {
    const QRect original = window.geometry();
    workspace.add_task(redclaw::ui::DesktopTask::kFiles, "Files", new QLineEdit, {360, 240});
    workspace.set_task_visible(redclaw::ui::DesktopTask::kFiles, true);
    for (int i = 0; i < 3; ++i) {
        workspace.fullscreen_button()->click(); settle();
        ASSERT_TRUE(mode.fullscreen()); EXPECT_TRUE(workspace.fullscreen_button()->isChecked());
        EXPECT_EQ(canvas->geometry(), window.contentsRect());
        const QRect bounds(window.mapToGlobal(window.contentsRect().topLeft()), window.contentsRect().size());
        EXPECT_TRUE(bounds.contains(workspace.button_bar()->geometry()));
        EXPECT_TRUE(bounds.contains(workspace.task_window(redclaw::ui::DesktopTask::kFiles)->geometry()));
        workspace.fullscreen_button()->click(); settle();
        EXPECT_FALSE(mode.fullscreen()); EXPECT_FALSE(workspace.fullscreen_button()->isChecked());
        EXPECT_EQ(window.geometry(), original); EXPECT_EQ(canvas->geometry(), window.contentsRect());
        EXPECT_TRUE(workspace.task_visible(redclaw::ui::DesktopTask::kFiles));
    }
}
TEST_F(FullscreenFixture, MaximizedModeAndItsNormalGeometrySurviveFullscreen) {
    const QRect normal = window.geometry();
    window.showMaximized(); settle();
    mode.set_fullscreen(true); settle(); ASSERT_TRUE(mode.fullscreen());
    mode.set_fullscreen(false); settle(); EXPECT_TRUE(window.isMaximized());
    window.showNormal(); settle(); EXPECT_EQ(window.geometry(), normal);
}
TEST_F(FullscreenFixture, LocalChordDoesNotRepeatOrStealEditorEscapeAndF11) {
    constexpr auto chord = Qt::ControlModifier | Qt::AltModifier;
    key(canvas, QEvent::KeyPress, Qt::Key_Return, chord); settle(); ASSERT_TRUE(mode.fullscreen());
    key(canvas, QEvent::KeyPress, Qt::Key_Return, chord, true); settle(); EXPECT_TRUE(mode.fullscreen());
    key(canvas, QEvent::KeyRelease, Qt::Key_Return, chord);
    auto* editor = new QLineEdit;
    workspace.add_task(redclaw::ui::DesktopTask::kTerminal, "Terminal", editor, {360, 240});
    workspace.set_task_visible(redclaw::ui::DesktopTask::kTerminal, true);
    key(editor, QEvent::KeyPress, Qt::Key_Enter, chord); settle(); EXPECT_TRUE(mode.fullscreen());
    key(canvas, QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    key(canvas, QEvent::KeyPress, Qt::Key_F11, Qt::NoModifier); settle(); EXPECT_TRUE(mode.fullscreen());
    key(workspace.button_bar(), QEvent::KeyPress, Qt::Key_Enter, chord); settle(); EXPECT_FALSE(mode.fullscreen());
    key(workspace.button_bar(), QEvent::KeyRelease, Qt::Key_Enter, chord);
}
TEST_F(FullscreenFixture, ReconnectRetainsFullscreenButCloseRestoresMainAndResetsMode) {
    QWidget main;
    redclaw::ui::PlaybackWindowLifecycle lifecycle(&window, &main);
    redclaw::ui::PlaybackWindowGeometryController geometry(&window, canvas);
    QSize viewport; geometry.set_viewport_committed_callback([&](QSize size, std::uint64_t) { viewport = size; });
    geometry.start(); mode.set_fullscreen(true); settle(); geometry.publish_initial_viewport(); settle();
    EXPECT_EQ(viewport, canvas->size() * canvas->devicePixelRatioF());
    geometry.stop(); geometry.start(); geometry.publish_initial_viewport(); settle();
    EXPECT_TRUE(mode.fullscreen());
    window.close(); settle(); EXPECT_FALSE(mode.fullscreen()); EXPECT_TRUE(main.isVisible());
    window.show(); settle(); EXPECT_FALSE(mode.fullscreen());
    geometry.stop();
}
} // namespace
