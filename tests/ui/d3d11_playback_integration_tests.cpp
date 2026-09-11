#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui/playback/playback_backends.h"
#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>

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

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
