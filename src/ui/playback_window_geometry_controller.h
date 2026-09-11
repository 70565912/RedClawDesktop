#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QSize>

class QWidget;

namespace redclaw::ui {

class PlaybackWindowGeometryController final : public QObject, public QAbstractNativeEventFilter {
public:
    using ActiveChangedCallback = std::function<void(bool active)>;
    using ViewportCommittedCallback =
        std::function<void(QSize physical_canvas_size, std::uint64_t transaction_id)>;

    PlaybackWindowGeometryController(QWidget* playback_window, QWidget* playback_canvas);
    ~PlaybackWindowGeometryController() override;

    PlaybackWindowGeometryController(const PlaybackWindowGeometryController&) = delete;
    PlaybackWindowGeometryController& operator=(const PlaybackWindowGeometryController&) = delete;

    void set_active_changed_callback(ActiveChangedCallback callback);
    void set_viewport_committed_callback(ViewportCommittedCallback callback);
    void start();
    void stop();
    void publish_initial_viewport();

    [[nodiscard]] std::uint64_t geometry_preview_total() const noexcept;
    [[nodiscard]] std::uint64_t geometry_commit_total() const noexcept;
    [[nodiscard]] std::uint64_t viewport_commit_total() const noexcept;
    [[nodiscard]] bool transitions_disabled() const noexcept;

    bool nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::ui
