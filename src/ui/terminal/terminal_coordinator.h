#pragma once
#include "redclaw/workspace/terminal_controller.h"
#include <QObject>
#include <QJsonObject>
#include <QByteArray>
#include <QMap>
#include <deque>

namespace redclaw::ui {
class WorkspacePipeServer;
class TerminalCoordinator final : public QObject {
public:
    explicit TerminalCoordinator(WorkspacePipeServer& pipe, QObject* parent = nullptr);
    workspace::TerminalController& controller() { return controller_; }
    QJsonObject invoke(const QString& method, const QJsonObject& params, const QString& id = {});
    QJsonObject read(std::uint64_t cursor, std::size_t limit = 32768) const;
    void set_blocked(bool blocked);
    void set_state_callback(std::function<void(bool, std::string_view)> callback);
    std::uint64_t first_cursor() const { return first_; }
    std::uint64_t next_cursor() const { return next_; }
    std::function<void(const QString&, const QJsonObject&)> event;
private:
    void execution(const protocol::TerminalMessageV1& message);
    void state(bool enabled, std::string_view error);
    QByteArray output_;
    QByteArray terminal_query_tail_;
    QByteArray execution_output_;
    std::uint64_t execution_first_ = 0, execution_next_ = 0;
    std::uint64_t first_ = 0, next_ = 0;
    QMap<QString, QJsonObject> operations_;
    std::deque<QString> operation_order_;
    QString opening_id_;
    std::function<void(bool, std::string_view)> state_;
    workspace::TerminalController controller_;
};
}
