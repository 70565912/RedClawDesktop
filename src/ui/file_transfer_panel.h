#pragma once
#include "redclaw/protocol/stream_control_protocol.h"
#include <QWidget>
#include <functional>
#include <memory>

namespace redclaw::ui {
class TransferCoordinator;
class FileTransferPanel final : public QWidget {
public:
    using Send = std::function<bool(const protocol::StreamControlMessageV1&, QString*)>;
    struct ClipboardAvailability {
        bool remote_offer = false;
        bool files = false;
    };
    using ClipboardProbe = std::function<ClipboardAvailability()>;
    FileTransferPanel(Send send, QWidget* parent = nullptr, ClipboardProbe clipboard_probe = {});
    ~FileTransferPanel() override;
    void receive(const protocol::StreamControlMessageV1& message);
    void runtime_stopped();
    void request_clipboard_paste(std::uint32_t sequence);
    void cancel_clipboard_paste();
    void set_busy_callback(std::function<void(bool)> callback);
    void set_activity_callback(std::function<void()> callback);
    [[nodiscard]] bool busy() const;
    TransferCoordinator& coordinator();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
