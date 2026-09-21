#include "ui/terminal/terminal_coordinator.h"
#include "ui/terminal/workspace_pipe_server.h"
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <algorithm>

namespace redclaw::ui {
namespace {
QJsonObject failure(const char* code) { return {{"ok", false}, {"error", code}}; }
constexpr qsizetype kOutputCapacity = 1024 * 1024;
}
TerminalCoordinator::TerminalCoordinator(WorkspacePipeServer& pipe, QObject* parent)
    : QObject(parent), controller_([&pipe](const auto& message) { return pipe.send(protocol::serialize_terminal_message_v1(message)); }, {
        [this](auto) { output_.clear(); first_ = next_ = 0; controller_.surface_ready(true); },
        [this](auto bytes) {
            terminal_query_tail_.append(bytes.data(), static_cast<qsizetype>(bytes.size()));
            if (terminal_query_tail_.contains("\x1b[6n")) {
                QTimer::singleShot(0, this, [this] { (void)controller_.input("\x1b[1;1R"); });
            }
            terminal_query_tail_ = terminal_query_tail_.right(3);
            output_.append(bytes.data(), static_cast<qsizetype>(bytes.size())); next_ += bytes.size();
            if (output_.size() > kOutputCapacity) {
                const auto drop = output_.size() - kOutputCapacity; output_.remove(0, drop); first_ += drop;
            }
            // ACK follows acceptance into the bounded shared journal. A slow or
            // absent WebView cannot stall script output or the remote shell.
            QTimer::singleShot(0, this, [this] { controller_.output_parsed(); });
            return true;
        },
        [this](bool enabled, auto error) { state(enabled, error); },
        [this](const auto& message) { execution(message); }
    }) {
    QPointer<TerminalCoordinator> weak(this);
    pipe.set_receive_callback([weak](auto frame) {
        if (!weak) return;
        const auto message = protocol::parse_terminal_message_v1(frame);
        if (message.ok) (void)weak->controller_.receive(message.value);
    });
    pipe.set_connection_callback([weak](bool open) {
        if (!weak) return;
        if (!open) weak->controller_.disconnected(); else weak->controller_.pump();
    });
    pipe.set_writable_callback([weak] { if (weak) weak->controller_.pump(); });
    controller_.surface_ready(true);
}
void TerminalCoordinator::set_state_callback(std::function<void(bool, std::string_view)> callback) { state_ = std::move(callback); }
void TerminalCoordinator::set_blocked(bool blocked) { controller_.set_surface_input_paused(blocked); }
void TerminalCoordinator::state(bool enabled, std::string_view error) {
    const bool opened = enabled && (controller_.capability_version() < 2 || controller_.prompt_ready());
    if (!opening_id_.isEmpty() && (opened || !error.empty())) {
        auto& result = operations_[opening_id_];
        result["state"] = opened ? "succeeded" : error == "terminal_connection_unavailable" ? "unknown" : "failed";
        result["error"] = QString::fromUtf8(error.data(), static_cast<qsizetype>(error.size()));
        result["terminal_id"] = QString::fromStdString(std::string(controller_.terminal_id()));
        if (event) event(opening_id_, result);
        opening_id_.clear();
    }
    if (state_) state_(enabled && controller_.execution_id().empty(), error);
}
QJsonObject TerminalCoordinator::read(std::uint64_t cursor, std::size_t limit) const {
    if (cursor > next_) return failure("cursor_ahead");
    const bool gap = cursor < first_; cursor = std::max(cursor, first_);
    const auto bytes = output_.mid(static_cast<qsizetype>(cursor - first_),
        static_cast<qsizetype>(std::min<std::uint64_t>(std::min<std::size_t>(limit, 32768), next_ - cursor)));
    return {{"ok", true}, {"gap", gap}, {"first_cursor", QString::number(first_)},
        {"next_cursor", QString::number(cursor + bytes.size())}, {"end_cursor", QString::number(next_)},
        {"output_base64", QString::fromLatin1(bytes.toBase64())}, {"output", QString::fromUtf8(bytes)}};
}
QJsonObject TerminalCoordinator::invoke(const QString& method, const QJsonObject& params, const QString& id) {
    if (method == "terminal.open") {
        if (controller_.input_paused()) return failure("terminal_transfer_busy");
        if (!opening_id_.isEmpty()) return operations_[opening_id_];
        if (!id.isEmpty() && !controller_.input_enabled()) {
            if (!controller_.capability_version()) return failure("terminal_peer_unavailable");
            while (operation_order_.size() >= 128) { operations_.remove(operation_order_.front()); operation_order_.pop_front(); }
            opening_id_ = id; operation_order_.push_back(id);
            operations_[id] = {{"ok", true}, {"operation_id", id}, {"kind", "terminal.open"}, {"state", "submitted"}};
            if (event) event(id, operations_[id]);
            controller_.request_open(); return operations_[id];
        }
        controller_.request_open(); return invoke("terminal.status", {});
    }
    if (method == "terminal.status") return {{"ok", true}, {"terminal_id", QString::fromStdString(std::string(controller_.terminal_id()))},
        {"input_enabled", controller_.input_enabled()}, {"prompt_ready", controller_.prompt_ready()},
        {"capability_version", static_cast<int>(controller_.capability_version())},
        {"operation_id", QString::fromStdString(std::string(controller_.execution_id()))},
        {"buffer_bytes", output_.size()}, {"first_cursor", QString::number(first_)}, {"next_cursor", QString::number(next_)}};
    if (method == "terminal.read") {
        bool valid = false; const auto cursor = params.value("cursor").toString("0").toULongLong(&valid);
        return valid ? read(cursor) : failure("invalid_cursor");
    }
    if (method == "operation.status" || method == "operation.result") {
        if (!operations_.contains(id)) return failure("operation_not_found");
        auto result = operations_[id];
        if (result.value("kind") == "terminal.open") return result;
        const auto start = result.value("start_cursor").toString().toULongLong();
        const auto finish = result.contains("end_cursor") ? result.value("end_cursor").toString().toULongLong() : execution_next_;
        if (method == "operation.result") {
            bool valid = false; const auto cursor = params.value("cursor").toString(QString::number(start)).toULongLong(&valid);
            if (!valid || cursor < start || cursor > finish) return failure("invalid_cursor");
            const auto first = std::min(finish, std::max(cursor, execution_first_));
            const auto count = first >= finish ? 0 : std::min<std::uint64_t>(32768, finish - first);
            const auto bytes = count == 0 ? QByteArray{} : execution_output_.mid(static_cast<qsizetype>(first - execution_first_), static_cast<qsizetype>(count));
            result["output_base64"] = QString::fromLatin1(bytes.toBase64()); result["output"] = QString::fromUtf8(bytes);
            result["gap"] = cursor < execution_first_; result["first_cursor"] = QString::number(execution_first_);
            result["next_cursor"] = QString::number(first + bytes.size());
            result["result_end_cursor"] = QString::number(finish);
        }
        return result;
    }
    if (method == "terminal.cancel" || method == "operation.cancel") {
        if (!id.isEmpty() && operations_.contains(id)) {
            const auto& result = operations_[id]; const auto state = result.value("state").toString();
            if (state == "cancelling" || state == "succeeded" || state == "failed" || state == "cancelled" || state == "unknown" || state == "rejected") return result;
        }
        if (!id.isEmpty() && id.toStdString() != controller_.execution_id()) return failure("operation_not_running");
        const auto current = QString::fromStdString(std::string(controller_.execution_id()));
        if (current.isEmpty()) return failure("terminal_not_running");
        if (operations_.value(current).value("state") == "cancelling") return operations_[current];
        if (!controller_.cancel_execution()) return failure("cancel_unconfirmed");
        auto& result = operations_[current]; result["state"] = "cancelling";
        if (event) event(current, result);
        return result;
    }
    controller_.request_open();
    if (!controller_.input_enabled()) return failure("terminal_not_ready");
    if (method == "terminal.input") {
        const auto bytes = params.value("text").toString().toUtf8();
        if (!params.value("text").isString() || bytes.isEmpty() || bytes.size() > 16384) return failure("invalid_input");
        return controller_.input({bytes.constData(), static_cast<std::size_t>(bytes.size())})
            ? QJsonObject{{"ok", true}} : failure("terminal_busy");
    }
    if (method == "terminal.resize") {
        if (!controller_.execution_id().empty()) return failure("terminal_busy");
        const int columns = params.value("columns").toInt(), rows = params.value("rows").toInt();
        if (columns < 2 || columns > 32767 || rows < 2 || rows > 32767) return failure("invalid_size");
        controller_.resize(columns, rows); return {{"ok", true}};
    }
    if (method != "terminal.exec") return failure("unknown_operation");
    if (controller_.capability_version() < 2) return failure("terminal_capability_insufficient");
    const auto bytes = params.value("command").toString().toUtf8();
    if (!params.value("command").isString() || bytes.isEmpty() || bytes.size() > 16384) return failure("invalid_command");
    if (!controller_.prompt_ready()) return failure("terminal_busy");
    if (!controller_.execute(id.toStdString(), {bytes.constData(), static_cast<std::size_t>(bytes.size())})) return failure("terminal_busy");
    while (operation_order_.size() >= 128) { operations_.remove(operation_order_.front()); operation_order_.pop_front(); }
    QJsonObject result{{"ok", true}, {"operation_id", id}, {"state", "submitted"}, {"start_cursor", QString::number(execution_next_)}};
    operations_[id] = result; operation_order_.push_back(id);
    if (event) event(id, result);
    return result;
}
void TerminalCoordinator::execution(const protocol::TerminalMessageV1& message) {
    const auto id = QString::fromStdString(message.operation_id);
    if (!operations_.contains(id)) return;
    if (message.execution_state == "output") {
        execution_output_.append(message.bytes.data(), static_cast<qsizetype>(message.bytes.size()));
        execution_next_ += message.bytes.size();
        if (execution_output_.size() > kOutputCapacity) {
            const auto count = execution_output_.size() - kOutputCapacity;
            execution_output_.remove(0, count); execution_first_ += count;
        }
        return;
    }
    auto& result = operations_[id]; result["state"] = QString::fromStdString(message.execution_state);
    result["error"] = QString::fromStdString(message.error_code);
    if (message.execution_state == "running") result["start_cursor"] = QString::number(execution_next_);
    else if (message.execution_state != "cancelling") {
        result["end_cursor"] = QString::number(execution_next_);
        result["powershell_success"] = message.execution_state == "succeeded" || message.execution_state == "failed"
            ? QJsonValue(message.powershell_success) : QJsonValue();
        result["last_native_exit_code"] = message.has_native_exit_code ? QJsonValue(message.last_native_exit_code) : QJsonValue();
    }
    if (event) event(id, result);
}
}
