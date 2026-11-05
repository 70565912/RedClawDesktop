#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui/playback/playback_backends.h"
#include "ui/playback/direct_frame_receiver.h"
#include "ui/desktop_task_workspace.h"
#include "ui/unified_session_workspace.h"
#include "ui/connection_overlay.h"
#include "ui/desktop_workspace_theme.h"
#include <QMainWindow>
#include "ui/playback_window_geometry_controller.h"
#include "ui/playback_window_fullscreen_controller.h"
#include "ui/playback_control_hint_overlay.h"
#include "ui/remote_input_capture.h"
#include "redclaw/helper/direct_frame_shared_memory.h"
#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QVBoxLayout>
#include <QLabel>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QToolButton>
#include "ui/runtime_log_view.h"
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QWindow>
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>
#include <chrono>
#include <thread>
#include <QUuid>

TEST(D3D11Playback, NativeEmptyBackgroundIsDarkBeforeFirstFrameAndAfterClear) {
    QWidget owner; owner.resize(640, 400);
    const auto renderer = redclaw::ui::try_create_d3d11_playback_renderer(&owner);
    ASSERT_NE(renderer.widget, nullptr);
    renderer.widget->resize(owner.size()); owner.show(); renderer.widget->show(); QCoreApplication::processEvents();
    auto* dc = CreateCompatibleDC(nullptr); ASSERT_NE(dc, nullptr);
    auto* bitmap = CreateBitmap(4, 4, 1, 32, nullptr); ASSERT_NE(bitmap, nullptr);
    auto* previous = SelectObject(dc, bitmap);
    auto check_dark = [&] {
        RECT sample{0, 0, 4, 4}; FillRect(dc, &sample, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        EXPECT_EQ(SendMessageW(reinterpret_cast<HWND>(renderer.widget->winId()), WM_ERASEBKGND, reinterpret_cast<WPARAM>(dc), 0), 1);
        const auto pixel = GetPixel(dc, 1, 1);
        EXPECT_LT(GetRValue(pixel), 32); EXPECT_LT(GetGValue(pixel), 32); EXPECT_LT(GetBValue(pixel), 40);
    };
    check_dark();
    QImage image(64, 64, QImage::Format_RGB32); image.fill(Qt::white); QString error;
    EXPECT_NE(renderer.canvas->present_image(image, &error), redclaw::ui::PresentOutcome::kFailed);
    renderer.canvas->clear_frame(); QCoreApplication::processEvents(); check_dark();
    SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc);
}

TEST(PlaybackTheme, EmptyAndLetterboxedQtCanvasStayDarkWithoutRecoloringRemotePixels) {
    QWidget owner; owner.resize(640, 400);
    const auto renderer = redclaw::ui::create_qt_playback_renderer(&owner);
    renderer.widget->resize(owner.size()); owner.show(); renderer.widget->show(); QCoreApplication::processEvents();
    auto dark_corner = [&] {
        const auto image = renderer.widget->grab().toImage(); const auto color = image.pixelColor(4, 4);
        EXPECT_LT(color.red(), 32); EXPECT_LT(color.green(), 32); EXPECT_LT(color.blue(), 40);
    };
    dark_corner();
    QImage frame(64, 64, QImage::Format_RGB32); frame.fill(Qt::white); QString error;
    ASSERT_NE(renderer.canvas->present_image(frame, &error), redclaw::ui::PresentOutcome::kFailed);
    QCoreApplication::processEvents(); dark_corner();
    auto image = renderer.widget->grab().toImage();
    EXPECT_EQ(image.pixelColor(image.width() / 2, image.height() / 2), QColor(Qt::white));
    renderer.canvas->clear_frame(); QCoreApplication::processEvents(); dark_corner();
}

TEST(PlaybackTheme, ApprovedWorkspaceLayoutCanBeReviewedWithRealIsolatedComponents) {
    using namespace redclaw::ui;
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    QSettings settings(directory.filePath("ui.ini"), QSettings::IniFormat);
    // Existing installations may remember the former fixed default size.
    settings.setValue("controller/connection_overlay/v1/geometry", QRect(24, 32, 560, 560));
    QMainWindow window; auto* home = new QWidget; auto* layout = new QVBoxLayout(home);
    auto* log_box = new QGroupBox(home); log_box->setObjectName("primaryCard");
    log_box->setMinimumHeight(160);
    const auto home_log_policy = log_box->sizePolicy();
    auto* log_layout = new QVBoxLayout(log_box); auto* log_header = new QHBoxLayout;
    const QMargins home_log_margins(12, 6, 12, 10);
    log_layout->setContentsMargins(home_log_margins); log_layout->setSpacing(6);
    auto* label = new QLabel(QString::fromUtf8("运行日志")); label->setObjectName("connectionSectionTitle");
    log_header->addWidget(label); log_header->addStretch();
    log_header->addWidget(new QPushButton(QString::fromUtf8("复制日志")));
    log_header->addWidget(new QPushButton(QString::fromUtf8("清空显示")));
    log_layout->addLayout(log_header); auto* log = new RuntimeLogView(log_box); log->setMinimumHeight(84);
    const auto home_editor_policy = log->sizePolicy();
    log_layout->addWidget(log, 1); layout->addWidget(log_box);
    UnifiedSessionWorkspace session(&window, home, &settings);
    const auto renderer = create_qt_playback_renderer(session.desktop_page());
    session.desktop_page()->layout()->addWidget(renderer.widget);
    DesktopTaskWorkspace tools(session.desktop_page(), nullptr); session.connection_overlay()->bind_button(tools.connection_button());
    tools.add_task(DesktopTask::kNavigation, QString::fromUtf8("桌面导航"), new QWidget, {360, 320});
    ConnectionFlowModel flow; flow.start(ConnectionFlowRole::kController);
    flow.apply(ConnectionFlowEvent::kLocalDescriptionPublished);
    window.resize(1440, 900); window.show(); session.update_connection(flow.snapshot());
    session.connection_overlay()->set_pairing_state(ConnectionPairingState::kConnected);
    session.connection_overlay()->set_handshake_ready(true);
    session.connection_overlay()->set_process_state(ConnectionProcessState::kActive);
    session.update_runtime("unknown", QString::fromUtf8("等待首帧"));
    for (const auto* text : {"开始连接远程设备", "已找到等待设备", "安全配对已完成", "正在建立网络路径"})
        log->appendPlainText(QString::fromUtf8("[10:24:01] ") + QString::fromUtf8(text));
    QCoreApplication::processEvents(); log->flush_pending(); QCoreApplication::processEvents();
    auto* panel = session.connection_overlay()->panel();
    auto* scroll = panel->findChild<QScrollArea*>("connectionOverlayScroll"); ASSERT_NE(scroll, nullptr);
    EXPECT_EQ(panel->findChild<QPlainTextEdit*>(), nullptr);
    EXPECT_EQ(log_box->parentWidget(), home);
    EXPECT_FALSE(panel->isAncestorOf(log_box));
    const auto expect_full_information = [&](const char* phase) {
        SCOPED_TRACE(phase);
        // Native tool windows cannot exceed the physical display's tracking
        // limit, even if an oversized test owner extends beyond the display.
        const int available = QApplication::platformName() == "offscreen" ? session.desktop_page()->height()
            : std::min(session.desktop_page()->height(), panel->screen()->availableGeometry().height());
        const bool room = panel->height() < available;
        EXPECT_EQ(panel->findChild<QPlainTextEdit*>(), nullptr);
        EXPECT_EQ(log_box->parentWidget(), home);
        if (room) {
            EXPECT_EQ(scroll->verticalScrollBar()->maximum(), 0);
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            auto* details = panel->findChild<QLabel*>("connectionRuntimeDetails");
            ASSERT_NE(details, nullptr);
            const QRect detail_bounds(details->mapTo(scroll->viewport(), QPoint()), details->size());
            EXPECT_TRUE(scroll->viewport()->rect().contains(detail_bounds))
                << "viewport=" << scroll->viewport()->width() << 'x' << scroll->viewport()->height()
                << " details=" << detail_bounds.x() << ',' << detail_bounds.y() << ',' << detail_bounds.width() << ',' << detail_bounds.height();
        }
        auto* disconnect = panel->findChild<QPushButton*>("connectionOverlayDisconnect");
        EXPECT_NE(disconnect, nullptr);
        if (!disconnect) return false;
        EXPECT_TRUE(panel->rect().contains(QRect(disconnect->mapTo(panel, QPoint()), disconnect->size())));
        return room;
    };
    expect_full_information("initial layout");
    auto* progress = panel->findChild<QWidget*>("connectingPage"); ASSERT_NE(progress, nullptr);
    EXPECT_LE(panel->height(), 500);
    const QSize expanded = panel->size();
    const QSize canvas = renderer.widget->size();
    window.resize(360, 240); QCoreApplication::processEvents();
    const QRect small_bounds(session.desktop_page()->mapToGlobal(QPoint()), session.desktop_page()->size());
    EXPECT_TRUE(small_bounds.contains(panel->geometry()));
    EXPECT_GT(scroll->verticalScrollBar()->maximum(), 0);
    window.resize(1440, 900); QCoreApplication::processEvents(); QCoreApplication::processEvents();
    expect_full_information("owner restored"); EXPECT_EQ(panel->size(), expanded); EXPECT_EQ(renderer.widget->size(), canvas);
    // Home-page log history must not change the floating panel, which has no log view.
    for (int line = 0; line < 80; ++line) log->appendPlainText(QString("log entry %1").arg(line));
    log->flush_pending(); QCoreApplication::processEvents();
    EXPECT_EQ(panel->size(), expanded); expect_full_information("long log history");
    EXPECT_EQ(panel->findChild<QPlainTextEdit*>(), nullptr);
    const auto output = qEnvironmentVariable("REDCLAW_UI_EVIDENCE_DIR");
    if (!output.isEmpty()) {
        QDir().mkpath(output);
        // Composite only this fixture's real widgets; no live desktop capture.
        auto preview = window.grab(); QPainter painter(&preview);
        for (auto* widget : {session.connection_overlay()->panel(), tools.button_bar()})
            painter.drawPixmap(widget->mapToGlobal(QPoint()) - window.mapToGlobal(QPoint()), widget->grab());
        painter.end(); preview.save(output + "/workspace-connecting.png");
        session.connection_overlay()->panel()->grab().save(output + "/connection-panel.png");
    }
    session.update_runtime("direct", QString("Frame diagnostic detail ").repeated(12));
    QCoreApplication::processEvents(); QCoreApplication::processEvents();
    if (expect_full_information("wrapped runtime detail")) EXPECT_GE(panel->height(), expanded.height());
    EXPECT_EQ(renderer.widget->size(), canvas);
    EXPECT_FALSE(tools.task_visible(DesktopTask::kNavigation));
    flow.observe_transport(true, true); flow.apply(ConnectionFlowEvent::kFirstFrameReady);
    session.update_connection(flow.snapshot());
    EXPECT_EQ(session.connection_overlay()->state(), ConnectionOverlayState::kHidden);
    EXPECT_FALSE(session.connection_overlay()->panel()->isVisible());
    tools.connection_button()->click();
    session.update_runtime("unknown", "Remote desktop live (1760x990, backend: d3d11).");
    QCoreApplication::processEvents(); QCoreApplication::processEvents();
    EXPECT_LE(panel->height(), 500);
    EXPECT_EQ(log_box->parentWidget(), home);
    if (!output.isEmpty()) {
        panel->grab().save(output + "/connection-connected.png");
        QFile metrics(output + "/connection-metrics.json");
        ASSERT_TRUE(metrics.open(QIODevice::WriteOnly));
        metrics.write(QJsonDocument(QJsonObject{
            {"width", panel->width()}, {"height", panel->height()},
            {"section_left", progress->mapTo(panel, QPoint()).x()},
            {"section_right", progress->mapTo(panel, QPoint()).x() + progress->width()},
            {"log_in_panel", false}}).toJson());
    }
    session.set_confirm_disconnect([] { return true; });
    session.set_request_stop([] {}); session.set_runtime_running([] { return false; });
    session.request_disconnect(); QCoreApplication::processEvents();
    EXPECT_EQ(log_box->parentWidget(), home);
    EXPECT_EQ(log_layout->contentsMargins(), home_log_margins);
    EXPECT_EQ(log_box->sizePolicy(), home_log_policy);
    EXPECT_EQ(log->sizePolicy(), home_editor_policy);
}

TEST(D3D11Playback, UnifiedWindowOverlayDoesNotResizeRendererAndReconnectRetainsCanvas) {
    using namespace redclaw::ui;
    QMainWindow window;
    auto* home = new QWidget;
    UnifiedSessionWorkspace session(&window, home, nullptr);
    const auto renderer = try_create_d3d11_playback_renderer(session.desktop_page());
    ASSERT_NE(renderer.widget, nullptr);
    session.desktop_page()->layout()->addWidget(renderer.widget);
    DesktopTaskWorkspace tools(session.desktop_page(), nullptr);
    session.connection_overlay()->bind_button(tools.connection_button());
    PlaybackWindowGeometryController geometry(&window, renderer.widget);
    geometry.set_active_changed_callback([&](bool active) { renderer.canvas->set_geometry_transaction_active(active); });
    geometry.set_viewport_committed_callback([&](QSize, std::uint64_t id) { renderer.canvas->commit_geometry(id); });
    geometry.start(); window.resize(1000, 700); window.show();
    ConnectionFlowSnapshot flow; flow.role = ConnectionFlowRole::kController;
    flow.view = ConnectionFlowView::kConnecting; flow.status = ConnectionFlowStatus::kComplete;
    session.update_connection(flow); QCoreApplication::processEvents();
    geometry.publish_initial_viewport(); QCoreApplication::processEvents();
    const auto size = renderer.widget->size();
    QImage image(1280, 720, QImage::Format_RGB32); image.fill(QColor(40, 80, 120)); QString error;
    ASSERT_NE(renderer.canvas->present_image(image, &error), PresentOutcome::kFailed) << error.toStdString();
    const auto resizes = renderer.canvas->swap_chain_resize_count();
    const auto viewports = geometry.viewport_commit_total();
    for (auto state : {ConnectionOverlayState::kCollapsed, ConnectionOverlayState::kHidden, ConnectionOverlayState::kExpanded}) {
        session.connection_overlay()->set_state(state); QCoreApplication::processEvents();
        EXPECT_EQ(renderer.widget->size(), size);
        EXPECT_NE(renderer.canvas->present_image(image, &error), PresentOutcome::kFailed) << error.toStdString();
        EXPECT_EQ(renderer.canvas->swap_chain_resize_count(), resizes);
        EXPECT_EQ(geometry.viewport_commit_total(), viewports);
    }
    PlaybackWindowFullscreenController fullscreen(&window, tools.button_bar(), session.desktop_page());
    session.set_before_home([&] { fullscreen.set_fullscreen(false); });
    fullscreen.set_fullscreen(true); QCoreApplication::processEvents();
    EXPECT_NE(renderer.canvas->present_image(image, &error), PresentOutcome::kFailed);
    session.set_confirm_disconnect([] { return false; }); EXPECT_FALSE(window.close()); EXPECT_TRUE(window.isFullScreen());
    session.set_confirm_disconnect([] { return true; }); session.set_request_stop([] {});
    session.request_disconnect(); session.runtime_stopped(); QCoreApplication::processEvents();
    EXPECT_FALSE(session.desktop_visible()); EXPECT_FALSE(window.isFullScreen());
    session.update_connection(flow); QCoreApplication::processEvents();
    EXPECT_EQ(renderer.widget->parentWidget(), session.desktop_page());
    EXPECT_NE(renderer.canvas->present_image(image, &error), PresentOutcome::kFailed);
    session.set_before_home({});
}

TEST(D3D11Playback, OnlySuccessfulNewSubmissionCountsAsPresented) {
    using namespace redclaw::ui;
    EXPECT_EQ(classify_dxgi_present_result(S_OK), PresentOutcome::kPresented);
    EXPECT_EQ(classify_dxgi_present_result(DXGI_ERROR_WAS_STILL_DRAWING), PresentOutcome::kBusyDrop);
    EXPECT_EQ(classify_dxgi_present_result(DXGI_STATUS_OCCLUDED), PresentOutcome::kBusyDrop);
    EXPECT_EQ(classify_dxgi_present_result(DXGI_ERROR_DEVICE_REMOVED), PresentOutcome::kFailed);
}

TEST(D3D11Playback, NativeCanvasFillsItsPlaybackWindowLayout) {
    QWidget window(nullptr, Qt::Window);
    window.setMinimumSize(360, 240);
    auto* layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    const auto renderer = redclaw::ui::try_create_d3d11_playback_renderer(&window);
    ASSERT_NE(renderer.widget, nullptr);
    renderer.widget->setObjectName("playbackWindowCanvas");
    layout->addWidget(renderer.widget, 1);
    redclaw::ui::DesktopTaskWorkspace workspace(&window, nullptr);
    for (const auto task : {redclaw::ui::DesktopTask::kAgent, redclaw::ui::DesktopTask::kTerminal,
                           redclaw::ui::DesktopTask::kNavigation, redclaw::ui::DesktopTask::kFiles}) {
        workspace.add_task(task, "Task", new QLabel("content"), {360, 320});
    }
    workspace.audio_button()->show();
    redclaw::ui::PlaybackControlHintOverlay overlay(renderer.widget);
    redclaw::ui::PlaybackWindowGeometryController geometry(&window, renderer.widget);
    geometry.set_active_changed_callback([&](bool active) {
        renderer.canvas->set_geometry_transaction_active(active);
    });
    geometry.set_viewport_committed_callback([&](QSize size, std::uint64_t id) {
        renderer.canvas->commit_geometry(id);
        EXPECT_EQ(size, QSize(qRound(window.width() * window.devicePixelRatioF()),
                              qRound(window.height() * window.devicePixelRatioF())));
    });
    geometry.start();
    workspace.set_task_visible(redclaw::ui::DesktopTask::kNavigation, true);
    window.resize(1000, 700);
    window.show();
    QCoreApplication::processEvents();
    EXPECT_EQ(renderer.widget->geometry(), window.rect());
    QImage image(1680, 1050, QImage::Format_RGB32);
    image.fill(QColor(40, 80, 120));
    QString error;
    EXPECT_NE(renderer.canvas->present_image(image, &error), redclaw::ui::PresentOutcome::kFailed)
        << error.toStdString();
    workspace.set_task_visible(redclaw::ui::DesktopTask::kNavigation, true);
    QCoreApplication::processEvents();
    EXPECT_EQ(renderer.widget->geometry(), window.rect());
    redclaw::ui::PlaybackWindowFullscreenController fullscreen(&window, workspace.button_bar());
    fullscreen.set_fullscreen(true);
    QCoreApplication::processEvents();
    EXPECT_TRUE(fullscreen.fullscreen());
    EXPECT_EQ(renderer.widget->geometry(), window.rect());
    EXPECT_NE(renderer.canvas->present_image(image, &error), redclaw::ui::PresentOutcome::kFailed);
    fullscreen.set_fullscreen(false);
    QCoreApplication::processEvents();
    EXPECT_EQ(renderer.widget->geometry(), window.rect());
    window.showMaximized();
    QCoreApplication::processEvents();
    EXPECT_EQ(renderer.widget->geometry(), window.rect());
    window.showNormal();
    window.resize(800, 600);
    QCoreApplication::processEvents();
    EXPECT_EQ(renderer.widget->geometry(), window.rect());
    for (int i = 0; i < 4; ++i) {
        window.showMinimized();
        QCoreApplication::processEvents();
        window.resize(900, 650);
        QCoreApplication::processEvents();
        window.showNormal();
        QCoreApplication::processEvents();
        EXPECT_EQ(renderer.widget->geometry(), window.rect()) << "minimize/restore " << i;
        window.hide();
        QCoreApplication::processEvents();
        window.resize(800, 600);
        layout->activate();
        window.show();
        QCoreApplication::processEvents();
        EXPECT_EQ(renderer.widget->geometry(), window.rect()) << "hide/show " << i;
    }
}

TEST(D3D11Playback, NativeVisibilityBeforeFirstShowDoesNotLeaveAnEmptyLayoutCache) {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    const auto renderer = redclaw::ui::try_create_d3d11_playback_renderer(&window);
    ASSERT_NE(renderer.widget, nullptr);
    layout->addWidget(renderer.widget, 1);
    redclaw::ui::PlaybackWindowGeometryController geometry(&window, renderer.widget);
    QSize committed_viewport;
    geometry.set_viewport_committed_callback([&](QSize size, std::uint64_t id) {
        committed_viewport = size;
        renderer.canvas->commit_geometry(id);
    });
    geometry.start();
    renderer.widget->winId();
    ASSERT_NE(renderer.widget->windowHandle(), nullptr);
    renderer.widget->windowHandle()->hide();
    const auto cached_hidden_size = layout->maximumSize();
    RecordProperty("hidden_max_height", cached_hidden_size.height());
    window.resize(1000, 700);
    window.show();
    QCoreApplication::processEvents();
    EXPECT_FALSE(renderer.widget->isHidden());
    EXPECT_EQ(renderer.widget->geometry(), window.rect());
    EXPECT_EQ(committed_viewport, QSize(qRound(window.width() * window.devicePixelRatioF()),
                                       qRound(window.height() * window.devicePixelRatioF())));
    // Exercise the window-manager command used by the title-bar button, which
    // the geometry controller observes through its native event filter.
    ASSERT_TRUE(PostMessageW(reinterpret_cast<HWND>(window.winId()), WM_SYSCOMMAND, SC_MAXIMIZE, 0));
    QEventLoop loop;
    QTimer::singleShot(200, &loop, &QEventLoop::quit);
    loop.exec();
    ASSERT_TRUE(window.isMaximized());
    EXPECT_EQ(renderer.widget->geometry(), window.rect());
    EXPECT_EQ(committed_viewport, QSize(qRound(window.width() * window.devicePixelRatioF()),
                                       qRound(window.height() * window.devicePixelRatioF())));
}

TEST(D3D11Playback, NewFramesDuringDeferredGeometryUseTheActualOutputResource) {
    const auto renderer = redclaw::ui::try_create_d3d11_playback_renderer(nullptr);
    ASSERT_NE(renderer.widget, nullptr);
    std::unique_ptr<QWidget> owner(renderer.widget);
    owner->resize(640, 360); owner->show(); QCoreApplication::processEvents();
    QString error;
    auto* device = renderer.canvas->d3d11_decode_device(&error);
    ASSERT_NE(device, nullptr) << error.toStdString();
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 640; description.Height = 368; description.ArraySize = 2;
    description.MipLevels = 1; description.Format = DXGI_FORMAT_NV12;
    description.SampleDesc.Count = 1; description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_DECODER;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &texture)));
    redclaw::ui::DirectFrameData frame;
    frame.width = 640; frame.height = 360; frame.bgra = false;
    frame.d3d11_surface = std::make_shared<redclaw::render::D3D11DecodedSurface>();
    frame.d3d11_surface->device = device; frame.d3d11_surface->texture = texture.Get();
    frame.d3d11_surface->array_slice = 1; frame.d3d11_surface->dxgi_format = DXGI_FORMAT_NV12;
    ASSERT_NE(renderer.canvas->present_frame(frame, &error), redclaw::ui::PresentOutcome::kFailed) << error.toStdString();
    renderer.canvas->set_geometry_transaction_active(true);
    owner->resize(1000, 700); QCoreApplication::processEvents();
    error.clear();
    EXPECT_NE(renderer.canvas->present_frame(frame, &error), redclaw::ui::PresentOutcome::kFailed) << error.toStdString();
    renderer.canvas->commit_geometry(1);
    error.clear();
    EXPECT_NE(renderer.canvas->present_frame(frame, &error), redclaw::ui::PresentOutcome::kFailed) << error.toStdString();
    renderer.canvas->set_geometry_transaction_active(true);
    owner->resize(420, 240); QCoreApplication::processEvents();
    error.clear();
    EXPECT_NE(renderer.canvas->present_frame(frame, &error), redclaw::ui::PresentOutcome::kFailed) << error.toStdString();
    renderer.canvas->commit_geometry(2);
    EXPECT_EQ(frame.pixels.size(), 0);

    // Exercise the same canvas after surface/CPU fallback and session clearing.
    // A retained swap chain must remain usable across every geometry commit.
    QImage cpu_frame(640, 360, QImage::Format_RGB32);
    cpu_frame.fill(QColor(40, 80, 120));
    for (std::uint64_t revision = 3; revision < 35; ++revision) {
        SCOPED_TRACE(revision);
        renderer.canvas->set_geometry_transaction_active(true);
        owner->resize(revision % 2 == 0 ? 900 : 420, revision % 2 == 0 ? 580 : 240);
        QCoreApplication::processEvents();
        error.clear();
        ASSERT_NE(renderer.canvas->present_frame(frame, &error), redclaw::ui::PresentOutcome::kFailed)
            << error.toStdString();
        renderer.canvas->commit_geometry(revision);
        error.clear();
        ASSERT_NE(renderer.canvas->present_image(cpu_frame, &error), redclaw::ui::PresentOutcome::kFailed)
            << error.toStdString();
        renderer.canvas->clear_frame();
        error.clear();
        ASSERT_NE(renderer.canvas->present_frame(frame, &error), redclaw::ui::PresentOutcome::kFailed)
            << error.toStdString();
        error.clear();
        ASSERT_NE(renderer.canvas->present_image(cpu_frame, &error), redclaw::ui::PresentOutcome::kFailed)
            << error.toStdString();
        QThread::msleep(10);
    }
}

TEST(D3D11Playback, NativeWindowRecreationRebindsPresentationWithoutReplacingDecoder) {
    class RecreatingWindow : public QWidget {
    public:
        void recreate_native_window() {
            destroy();
            create();
            show();
        }
    } window;
    auto* layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    const auto renderer = redclaw::ui::try_create_d3d11_playback_renderer(&window);
    ASSERT_NE(renderer.widget, nullptr);
    layout->addWidget(renderer.widget);
    window.resize(640, 360);
    window.show();
    QCoreApplication::processEvents();
    QString error;
    auto* decoder_device = renderer.canvas->d3d11_decode_device(&error);
    ASSERT_NE(decoder_device, nullptr) << error.toStdString();
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 640;
    description.Height = 368;
    description.ArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_NV12;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_DECODER;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    ASSERT_TRUE(SUCCEEDED(decoder_device->CreateTexture2D(&description, nullptr, &texture)));
    redclaw::ui::DirectFrameData frame;
    frame.width = 640;
    frame.height = 360;
    frame.bgra = false;
    frame.d3d11_surface = std::make_shared<redclaw::render::D3D11DecodedSurface>();
    frame.d3d11_surface->device = decoder_device;
    frame.d3d11_surface->texture = texture.Get();
    frame.d3d11_surface->dxgi_format = DXGI_FORMAT_NV12;
    QImage image(640, 360, QImage::Format_RGB32);
    image.fill(QColor(40, 80, 120));
    ASSERT_EQ(renderer.canvas->present_image(image, &error), redclaw::ui::PresentOutcome::kPresented)
        << error.toStdString();
    ASSERT_TRUE(renderer.canvas->diagnostic_snapshot()["output_window_matches_canvas"].toBool());

    for (int i = 0; i < 6; ++i) {
        SCOPED_TRACE(i);
        const auto old_window = renderer.widget->winId();
        window.recreate_native_window();
        QCoreApplication::processEvents();
        ASSERT_NE(renderer.widget->winId(), old_window);
        error.clear();
        const auto outcome = i % 2 == 0 ? renderer.canvas->present_frame(frame, &error)
                                       : renderer.canvas->present_image(image, &error);
        ASSERT_NE(outcome, redclaw::ui::PresentOutcome::kFailed) << error.toStdString();
        EXPECT_TRUE(renderer.canvas->diagnostic_snapshot()["output_window_matches_canvas"].toBool());
        EXPECT_EQ(renderer.canvas->d3d11_decode_device(&error), decoder_device);
    }
}

TEST(DirectFrameReceiver, SessionResetDiscardsRetainedFrameAndRestartsFrameWatermarks) {
    using namespace redclaw::helper;
    redclaw::ui::DirectFramePipeServer receiver;
    const auto channel = QString("reset-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QString error;
    ASSERT_TRUE(receiver.start(channel, &error)) << error.toStdString();
    struct Writer {
        HANDLE mapping = nullptr, event = nullptr;
        void* view = nullptr;
        ~Writer() { if (view) UnmapViewOfFile(view); if (mapping) CloseHandle(mapping); if (event) CloseHandle(event); }
    } writer;
    writer.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, direct_frame_shared_mapping_name(channel.toStdString()).c_str());
    ASSERT_NE(writer.mapping, nullptr);
    writer.view = MapViewOfFile(writer.mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    ASSERT_NE(writer.view, nullptr);
    writer.event = OpenEventW(EVENT_MODIFY_STATE, FALSE, direct_frame_shared_event_name(channel.toStdString()).c_str());
    ASSERT_NE(writer.event, nullptr);
    auto* shared = static_cast<DirectFrameSharedMemoryHeader*>(writer.view);
    std::uint64_t sequence = 0;
    auto publish = [&](std::uint64_t frame_id) {
        const auto next = sequence + 1;
        const auto slot_index = static_cast<std::uint32_t>((next - 1) % kDirectFrameSharedMemorySlotCount);
        auto* slot = direct_frame_shared_slot_header(writer.view, slot_index);
        const auto active = direct_frame_atomic_load_i64(&shared->reader_active_sequence);
        if (active > 0 && static_cast<std::uint32_t>((active - 1) % kDirectFrameSharedMemorySlotCount) == slot_index) return;
        direct_frame_atomic_store_i64(&slot->committed_sequence, 0);
        slot->frame = {};
        std::memcpy(slot->frame.magic, kDirectFrameChannelMagic, sizeof(kDirectFrameChannelMagic));
        slot->frame.width = slot->frame.height = 2; slot->frame.row_pitch = 8;
        slot->frame.format = kDirectFrameFormatBgra; slot->frame.frame_id = frame_id;
        slot->frame.capture_region_revision = 1;
        slot->frame.content_rect_width = slot->frame.content_rect_height = 2;
        slot->frame.payload_size = 16;
        std::memset(direct_frame_shared_slot_payload(slot), 128, 16);
        direct_frame_atomic_store_i64(&slot->committed_sequence, static_cast<LONG64>(next));
        direct_frame_atomic_store_i64(&shared->latest_sequence, static_cast<LONG64>(next));
        sequence = next; SetEvent(writer.event);
    };
    auto await_frame = [&](std::uint64_t id, std::uint64_t generation) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            publish(id);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            redclaw::ui::DirectFrameData frame; quint64 count = 0;
            if (receiver.take_latest_frame(&frame, &count) && frame.frame_id == id && frame.playback_generation == generation) return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    ASSERT_TRUE(await_frame(31000, 1));
    ASSERT_EQ(receiver.stats_snapshot().latest_displayable_frame_id, 31000U);
    const auto generation = receiver.request_session_reset();
    EXPECT_EQ(generation, 2U);
    EXPECT_EQ(receiver.stats_snapshot().latest_displayable_frame_id, 0U);
    redclaw::ui::DirectFrameData stale; quint64 count = 0;
    EXPECT_FALSE(receiver.take_latest_frame(&stale, &count));
    ASSERT_TRUE(await_frame(1, generation));
    EXPECT_EQ(receiver.stats_snapshot().playback_generation, generation);
    EXPECT_EQ(receiver.stats_snapshot().latest_displayable_frame_id, 1U);
    EXPECT_EQ(receiver.stats_snapshot().decode_failures, 0U);
    receiver.stop();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
