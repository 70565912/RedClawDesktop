#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui/playback/playback_backends.h"
#include "ui/playback/direct_frame_receiver.h"
#include "redclaw/helper/direct_frame_shared_memory.h"
#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
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
