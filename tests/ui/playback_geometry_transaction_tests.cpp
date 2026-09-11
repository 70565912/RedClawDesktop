#include <gtest/gtest.h>

#include "ui/playback_geometry_transaction.h"
#include "ui/playback_window_geometry_controller.h"
#include "ui/remote_input_capture.h"
#include <QCoreApplication>
#include <QWidget>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {

#ifdef _WIN32
TEST(PlaybackGeometryLifecycleTests, UnstartedFilterMustNotCreateNativeWindow) {
    QWidget window;
    QWidget canvas(&window);
    redclaw::ui::PlaybackWindowGeometryController controller(&window, &canvas);
    redclaw::ui::ControllerRemoteInputCapture input(&canvas);
    ASSERT_EQ(window.internalWinId(), 0U);
    MSG event{};
    event.hwnd = reinterpret_cast<HWND>(1);
    event.message = WM_NULL;
    qintptr result = 0;
    EXPECT_FALSE(controller.nativeEventFilter("windows_generic_MSG", &event, &result));
    EXPECT_EQ(window.internalWinId(), 0U);
    EXPECT_FALSE(input.keyboard_target_is_active());
    EXPECT_EQ(window.internalWinId(), 0U);
}

class NativeLifecycleWindow final : public QWidget {
public:
    void destroy_native() { destroy(true, true); }
};

TEST(PlaybackGeometryLifecycleTests, NativeDestroyAndRecreateDoesNotReenterWindowCreation) {
    NativeLifecycleWindow window;
    QWidget canvas(&window);
    redclaw::ui::PlaybackWindowGeometryController controller(&window, &canvas);
    controller.start();
    for (int i = 0; i < 20; ++i) {
        ASSERT_NE(window.winId(), 0U); // Explicit creation outside a native callback.
        window.destroy_native();
        EXPECT_EQ(window.internalWinId(), 0U);
        MSG event{};
        event.hwnd = reinterpret_cast<HWND>(1);
        event.message = WM_NULL;
        qintptr result = 0;
        EXPECT_FALSE(controller.nativeEventFilter("windows_generic_MSG", &event, &result));
        EXPECT_EQ(window.internalWinId(), 0U);
    }
    EXPECT_EQ(controller.geometry_commit_total(), 0U);
}
#endif

TEST(PlaybackGeometryLifecycleTests, QueuedCommitIsCancelledByStopOrControllerDestruction) {
    QWidget window;
    QWidget canvas(&window);
    canvas.resize(300, 200);
    canvas.show();
    int commits = 0;
    auto* controller = new redclaw::ui::PlaybackWindowGeometryController(&window, &canvas);
    controller->set_viewport_committed_callback([&](QSize, std::uint64_t) { ++commits; });
    controller->start();
    controller->publish_initial_viewport();
    controller->stop();
    QCoreApplication::processEvents();
    EXPECT_EQ(commits, 0);
    controller->start();
    controller->publish_initial_viewport();
    delete controller;
    QCoreApplication::processEvents();
    EXPECT_EQ(commits, 0);
}

TEST(PlaybackGeometryTransactionTests, ManyPreviewsProduceOneCommit) {
    redclaw::ui::PlaybackGeometryTransaction transaction;
    transaction.begin({10, 20, 800, 600});
    EXPECT_TRUE(transaction.preview({10, 20, 900, 650}));
    EXPECT_TRUE(transaction.preview({10, 20, 1000, 700}));
    EXPECT_TRUE(transaction.preview({10, 20, 1200, 800}));

    const auto committed = transaction.complete();

    ASSERT_TRUE(committed.has_value());
    EXPECT_EQ(committed->width, 1200);
    EXPECT_EQ(committed->height, 800);
    EXPECT_EQ(transaction.counters().preview_total, 3U);
    EXPECT_EQ(transaction.counters().commit_total, 1U);
}

TEST(PlaybackGeometryTransactionTests, CancelProducesNoCommit) {
    redclaw::ui::PlaybackGeometryTransaction transaction;
    transaction.begin({0, 0, 800, 600});
    ASSERT_TRUE(transaction.preview({0, 0, 1280, 720}));
    transaction.cancel();

    EXPECT_FALSE(transaction.complete().has_value());
    EXPECT_EQ(transaction.counters().commit_total, 0U);
    EXPECT_EQ(transaction.counters().cancel_total, 1U);
}

TEST(PlaybackGeometryTransactionTests, RepeatedFinalRectIsNoOp) {
    redclaw::ui::PlaybackGeometryTransaction transaction;
    transaction.begin({0, 0, 800, 600});
    ASSERT_TRUE(transaction.preview({0, 0, 800, 600}));
    EXPECT_FALSE(transaction.complete().has_value());
    EXPECT_EQ(transaction.counters().commit_total, 0U);
}

}  // namespace
