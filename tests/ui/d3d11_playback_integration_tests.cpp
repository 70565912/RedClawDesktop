#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui/playback/playback_backends.h"
#include "ui/playback/direct_frame_receiver.h"
#include "ui/desktop_task_workspace.h"
#include "ui/playback_window_geometry_controller.h"
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
#include <QPushButton>
#include <QWindow>
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>
#include <chrono>
#include <thread>
#include <QUuid>

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
