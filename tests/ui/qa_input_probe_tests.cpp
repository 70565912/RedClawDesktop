#if defined(_WIN32)
#include "ui/qa_input_probe.h"
#include <gtest/gtest.h>
#include <QAbstractEventDispatcher>

TEST(QaInputProbe, NativeTargetMappingRoundTripsOffsetRegionAndAllRotations) {
    using namespace redclaw::input;
    for (const unsigned rotation : {0U, 90U, 180U, 270U}) {
        const DesktopGeometry geometry{-1920, 120, 1680, 1050, rotation, 7};
        const DesktopCaptureRegion region{120, 80, 1200, 800};
        for (const auto value : {1000, 20000, 32768, 60000}) {
            DesktopPoint point;
            ASSERT_TRUE(map_normalized_capture_region_point(value, 65535-value, geometry, region, &point));
            std::uint16_t x, y;
            ASSERT_TRUE(redclaw::ui::QaInputProbe::normalize_target(point, geometry, region, &x, &y));
            DesktopPoint roundtrip;
            ASSERT_TRUE(map_normalized_capture_region_point(x, y, geometry, region, &roundtrip));
            EXPECT_NEAR(point.x, roundtrip.x, 1); EXPECT_NEAR(point.y, roundtrip.y, 1);
        }
    }
}

TEST(QaInputProbe, RejectsTargetOutsideCapturedRegionAndMissingLease) {
    using namespace redclaw::input;
    std::uint16_t x, y;
    EXPECT_FALSE(redclaw::ui::QaInputProbe::normalize_target({1, 1}, {0, 0, 1680, 1050, 0, 1}, {100, 100, 500, 500}, &x, &y));
    QWidget canvas;
    redclaw::ui::ControllerRemoteInputCapture capture(&canvas, &canvas);
    redclaw::ui::QaInputProbe probe(canvas, capture);
    QString error;
    EXPECT_FALSE(probe.start(&error)); EXPECT_FALSE(error.isEmpty());
}

TEST(ControllerRemoteInputCapture, ShutdownDetachesNativeLifecycleBeforeWidgetDestruction) {
    auto window = std::make_unique<QWidget>();
    auto* canvas = new QWidget(window.get());
    auto* capture = new redclaw::ui::ControllerRemoteInputCapture(canvas, window.get());
    int callbacks = 0;
    capture->set_forwarding_changed_callback([&] { ++callbacks; });
    capture->shutdown();
    QEvent deactivate(QEvent::WindowDeactivate);
    QCoreApplication::sendEvent(window.get(), &deactivate);
    window.reset();
    EXPECT_EQ(callbacks, 0);
}

TEST(ControllerRemoteInputCapture, SuppressesMarkedMouseAtNativeBoundaryBeforeQtConversion) {
    QWidget canvas;
    redclaw::ui::ControllerRemoteInputCapture capture(&canvas);
    MSG message{}; message.hwnd = reinterpret_cast<HWND>(canvas.winId());
    message.message = WM_LBUTTONDOWN;
    // The actual Windows mouse message transport retained only these bits in
    // the native receipt reproduction, even between two 64-bit processes.
    const auto previous = SetMessageExtraInfo(static_cast<std::uint32_t>(redclaw::input::kRedClawInputExtraInfo));
    qintptr result = 0;
    const bool suppressed = QAbstractEventDispatcher::instance()->filterNativeEvent("windows_generic_MSG", &message, &result);
    SetMessageExtraInfo(previous);
    EXPECT_TRUE(suppressed);
    EXPECT_EQ(capture.suppressed_mouse_loopback_count(), 1);
    EXPECT_EQ(capture.queued_critical_event_count(), 0);
    SetMessageExtraInfo(0);
    EXPECT_FALSE(QAbstractEventDispatcher::instance()->filterNativeEvent("windows_generic_MSG", &message, &result));
    SetMessageExtraInfo(previous);
}
#endif
