#pragma once
#include "redclaw/protocol/stream_control_protocol.h"
#include <QWidget>
#include <functional>
#include <memory>

namespace redclaw::ui {
class FileTransferPanel final : public QWidget {
public:
    using Send = std::function<bool(const protocol::StreamControlMessageV1&, QString*)>;
    FileTransferPanel(Send send, QWidget* parent = nullptr);
    ~FileTransferPanel() override;
    void receive(const protocol::StreamControlMessageV1& message);
    void runtime_stopped();
    void request_clipboard_paste(std::uint32_t sequence);
    void cancel_clipboard_paste();
    void set_busy_callback(std::function<void(bool)> callback);
    [[nodiscard]] bool busy() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
