#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <QEvent>
#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>

#include "redclaw/protocol/stream_control_protocol.h"
#include "input_ack_timing.h"

class QTimer;
class QWidget;

namespace redclaw::ui {

enum class LocalInputSuspensionReason : std::uint32_t {
    kWindowInactive = 1U << 0U,
    kHiddenOrMinimized = 1U << 1U,
    kGeometryTransaction = 1U << 2U,
    kLocalUiFocus = 1U << 3U,
};

class ControllerRemoteInputCapture final : public QObject, public QAbstractNativeEventFilter {
public:
    using ControlSender = std::function<bool(
        const redclaw::protocol::StreamControlMessageV1&,
        QString*)>;
    using PausedCallback = std::function<void(const QString&)>;
    using ForwardingChangedCallback = std::function<void()>;
    using BlockedClickCallback = std::function<void()>;
    using QaSendObserver = std::function<void(const redclaw::protocol::StreamControlMessageV1&, std::uint64_t)>;
    using QaAckObserver = std::function<void(const InputAckTiming&)>;

    explicit ControllerRemoteInputCapture(QWidget* canvas, QObject* parent = nullptr);
    ~ControllerRemoteInputCapture() override;

    void set_send_message_callback(ControlSender callback);
    void set_paused_callback(PausedCallback callback);
    void set_forwarding_changed_callback(ForwardingChangedCallback callback);
    void set_blocked_click_callback(BlockedClickCallback callback);
    void set_qa_observers(QaSendObserver send, QaAckObserver ack);
    void set_remote_frame_size(QSize size);
    void set_desktop_geometry_revision(std::uint64_t revision);
    bool acknowledge_input_sequence(std::uint64_t sequence, std::uint64_t consumed_us, std::uint64_t runtime_received_us);
    [[nodiscard]] bool activate(QString* error = nullptr);
    void pause(bool notify_peer, const QString& reason);
    void shutdown();
    bool nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result) override;
    void set_local_suspension(LocalInputSuspensionReason reason, bool suspended);
    [[nodiscard]] bool submit_qa_mouse_click(
        std::uint16_t normalized_x,
        std::uint16_t normalized_y,
        redclaw::protocol::RemoteInputMouseButtonV1 button,
        QString* error = nullptr);
    [[nodiscard]] bool submit_qa_key_press(
        std::uint16_t scan_code,
        std::uint16_t virtual_key,
        bool extended,
        QString* error = nullptr);

    [[nodiscard]] bool active() const;
    [[nodiscard]] bool control_enabled() const;
    [[nodiscard]] bool input_forwarding() const;
    [[nodiscard]] QString local_suspension_reason() const;
    [[nodiscard]] QSize remote_frame_size() const;
    [[nodiscard]] std::uint64_t desktop_geometry_revision() const;
    [[nodiscard]] QRect content_rect() const;
    [[nodiscard]] bool map_content_position(
        const QPointF& position,
        std::uint16_t* x,
        std::uint16_t* y) const;
    [[nodiscard]] bool keyboard_target_is_active() const;
    [[nodiscard]] static bool should_suppress_mouse_loopback(
        QEvent::Type event_type,
        std::uintptr_t message_extra_info);
    [[nodiscard]] std::size_t queued_critical_event_count() const;
    [[nodiscard]] std::uint64_t sent_batch_count() const;
    [[nodiscard]] std::uint64_t merged_mouse_move_count() const;
    [[nodiscard]] std::uint64_t suppressed_mouse_loopback_count() const;
    [[nodiscard]] std::uint64_t blocked_control_click_hint_count() const;
    [[nodiscard]] std::uint64_t last_application_ack_rtt_ms() const;
    [[nodiscard]] std::uint64_t application_ack_p95_ms() const;

#if defined(_WIN32)
    [[nodiscard]] std::intptr_t handle_low_level_keyboard(
        std::uint32_t message,
        std::uintptr_t event_data);
#endif

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static constexpr std::size_t kCriticalQueueCapacity = 256;

    [[nodiscard]] bool install_keyboard_capture(QString* error);
    void uninstall_keyboard_capture();
    void enqueue_critical(redclaw::protocol::RemoteInputEventV1 event);
    void enqueue_mouse_move(std::uint16_t normalized_x, std::uint16_t normalized_y);
    void flush_batch();
    void send_state_sync();
    void send_release_all();
    void clear_local_input_state();
    [[nodiscard]] bool send_message(redclaw::protocol::StreamControlMessageV1 message);
    [[nodiscard]] bool normalized_position(
        const QPointF& position,
        bool clamp_to_content,
        std::uint16_t* x,
        std::uint16_t* y) const;
    [[nodiscard]] bool is_canvas_widget(const QWidget* widget) const;
    [[nodiscard]] bool is_playback_ui_widget(const QWidget* widget) const;
    void update_keyboard_target(bool canvas_target);
    [[nodiscard]] static std::uint32_t button_mask(
        redclaw::protocol::RemoteInputMouseButtonV1 button);

    QPointer<QWidget> canvas_;
    QPointer<QWidget> window_;
    QTimer* flush_timer_ = nullptr;
    QTimer* state_sync_timer_ = nullptr;
    ControlSender send_message_callback_;
    PausedCallback paused_callback_;
    ForwardingChangedCallback forwarding_changed_callback_;
    BlockedClickCallback blocked_click_callback_;
    QaSendObserver qa_send_observer_;
    QaAckObserver qa_ack_observer_;
    QSize remote_frame_size_;
    std::uint64_t desktop_geometry_revision_ = 0;
    bool active_ = false;
    bool pausing_ = false;
    bool canvas_keyboard_target_ = false;
    bool local_suspension_release_sent_ = false;
    std::uint32_t local_suspension_flags_ = 0;
    std::deque<redclaw::protocol::RemoteInputEventV1> critical_events_;
    std::optional<redclaw::protocol::RemoteInputEventV1> latest_mouse_move_;
    std::set<std::uint32_t> pressed_keys_;
    std::set<std::uint16_t> pressed_virtual_keys_;
    std::uint32_t pressed_mouse_buttons_ = 0;
    std::optional<std::pair<std::uint16_t, std::uint16_t>> last_remote_pointer_position_;
    std::uint64_t local_message_id_ = 0;
    std::uint64_t input_sequence_ = 0;
    std::uint64_t sent_batch_count_ = 0;
    std::uint64_t merged_mouse_move_count_ = 0;
    std::uint64_t suppressed_mouse_loopback_count_ = 0;
    std::uint64_t blocked_control_click_hint_count_ = 0;
    InputAckTracker pending_input_acks_;
    std::deque<std::uint64_t> application_ack_samples_ms_;
    std::uint64_t last_application_ack_rtt_ms_ = 0;
#if defined(_WIN32)
    void* keyboard_hook_ = nullptr;
#endif
};

}  // namespace redclaw::ui
