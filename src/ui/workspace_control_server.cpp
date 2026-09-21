#include "ui/workspace_control_server.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace redclaw::ui {
namespace {
QJsonObject failure(const char* error) { return {{"ok", false}, {"error", error}}; }
bool mutation(const QString& method) {
    return method != "capabilities" && method != "status" && method != "events" && method != "operations"
        && method != "terminal.read" && method != "terminal.status" && method != "operation.status" && method != "operation.result";
}
}
WorkspaceControlServer::WorkspaceControlServer(Invoke invoke, QObject* parent)
    : QObject(parent), invoke_(std::move(invoke)), server_(this), instance_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}
bool WorkspaceControlServer::start(const QString& name, QString* error) {
    static const QRegularExpression valid("^[A-Za-z0-9_.-]{1,128}$");
    if (!valid.match(name).hasMatch()) { if (error) *error = "workspace_control_invalid_name"; return false; }
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    server_.setMaxPendingConnections(16);
    connect(&server_, &QLocalServer::newConnection, this, [this] { accept(); });
    if (!server_.listen(name)) { if (error) *error = server_.errorString(); return false; }
    return true;
}
bool WorkspaceControlServer::current_user_client(qintptr descriptor) {
#ifdef _WIN32
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(reinterpret_cast<HANDLE>(descriptor), &pid) || !pid) return false;
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    HANDLE client = nullptr, owner = nullptr;
    const bool opened = OpenProcessToken(process, TOKEN_QUERY, &client)
        && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &owner);
    bool equal = false;
    if (opened) {
        DWORD client_size = 0, owner_size = 0;
        GetTokenInformation(client, TokenUser, nullptr, 0, &client_size);
        GetTokenInformation(owner, TokenUser, nullptr, 0, &owner_size);
        if (client_size && owner_size && client_size <= 4096 && owner_size <= 4096) {
            QByteArray client_user(client_size, 0), owner_user(owner_size, 0);
            if (GetTokenInformation(client, TokenUser, client_user.data(), client_size, &client_size)
                && GetTokenInformation(owner, TokenUser, owner_user.data(), owner_size, &owner_size))
                equal = EqualSid(reinterpret_cast<TOKEN_USER*>(client_user.data())->User.Sid,
                    reinterpret_cast<TOKEN_USER*>(owner_user.data())->User.Sid) != FALSE;
        }
    }
    if (client) CloseHandle(client); if (owner) CloseHandle(owner); CloseHandle(process);
    return equal;
#else
    (void)descriptor; return true; // owner-only Unix socket permissions
#endif
}
void WorkspaceControlServer::accept() {
    while (auto* socket = server_.nextPendingConnection()) {
        socket->setParent(&server_);
        if (server_.findChildren<QLocalSocket*>().size() > 16 || !current_user_client(socket->socketDescriptor())) {
            socket->abort(); socket->deleteLater(); continue;
        }
        socket->setReadBufferSize(65537);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(5000, socket, [socket] { socket->abort(); });
        auto read = [this, socket] {
            if (socket->property("complete").toBool()) return;
            auto bytes = socket->property("request").toByteArray(); bytes += socket->readAll();
            if (bytes.size() <= 65536 && !bytes.contains('\n')) { socket->setProperty("request", bytes); return; }
            socket->setProperty("complete", true);
            QJsonObject reply;
            if (bytes.size() > 65536) reply = failure("request_too_large");
            else {
                QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes.trimmed(), &error);
                reply = error.error == QJsonParseError::NoError && document.isObject()
                    ? dispatch(document.object()) : failure("invalid_json");
            }
            socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
            socket->disconnectFromServer();
        };
        connect(socket, &QLocalSocket::readyRead, socket, read); read();
    }
}
QJsonObject WorkspaceControlServer::dispatch(const QJsonObject& request) {
    if (request.value("version").toInt() != 1) return failure("protocol_version_incompatible");
    const auto method = request.value("operation").toString(), id = request.value("request_id").toString();
    static const QRegularExpression token("^[A-Za-z0-9_.:-]{1,128}$");
    if (!token.match(id).hasMatch() || !request.value("parameters").isObject() || !token.match(method).hasMatch())
        return failure("invalid_request");
    const auto params = request.value("parameters").toObject();
    if (method == "capabilities") {
        auto result = invoke_(method, {}, {});
        result["ok"] = true; result["version"] = 1; result["instance_id"] = instance_;
        result["request_capacity"] = 4096; result["event_capacity"] = 2048;
        result["operations"] = QJsonArray{"capabilities", "status", "operations", "events",
            "operation.status", "operation.result", "operation.cancel", "terminal.open", "terminal.input",
            "terminal.read", "terminal.status", "terminal.resize", "terminal.exec", "terminal.cancel",
            "files.browse", "files.upload", "files.download", "clipboard.read", "clipboard.write", "clipboard.paste",
            "clipboard.copies.list", "clipboard.copies.open", "clipboard.copies.cleanup"};
        result["request_id"] = id; return result;
    }
    if (request.value("instance_id").toString() != instance_) return failure("instance_changed");
    if (method == "events") {
        bool valid = false; const auto cursor = params.value("cursor").toString("0").toULongLong(&valid);
        if (!valid || cursor > event_cursor_) return failure("invalid_cursor");
        QJsonArray page; const auto first = event_cursor_ - events_.size(); auto next = std::max(cursor, first);
        for (const auto& event : events_) if (event.value("cursor").toString().toULongLong() >= next && page.size() < 128) {
            page.append(event); next = event.value("cursor").toString().toULongLong() + 1;
        }
        return {{"ok", true}, {"events", page}, {"gap", cursor < first}, {"first_cursor", QString::number(first)}, {"next_cursor", QString::number(next)}};
    }
    if (method == "operations") { QJsonArray list; for (const auto& item : operations_) list.append(item); return {{"ok", true}, {"operations", list}}; }
    const bool writes = mutation(method);
    const auto digest = QCryptographicHash::hash(QJsonDocument(QJsonObject{{"operation", method}, {"parameters", params}}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    if (writes && requests_.contains(id)) {
        const auto& seen = requests_[id];
        return seen.digest == digest ? seen.reply : failure("request_id_conflict");
    }
    if (writes && requests_.size() >= 4096) return failure("request_history_full");
    const auto operation = method.startsWith("operation.") || method == "terminal.cancel"
        ? params.value("operation_id").toString() : QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto reply = invoke_(method, params, operation);
    reply["version"] = 1; reply["request_id"] = id; reply["instance_id"] = instance_;
    if (writes) requests_[id] = {digest, reply}; // retain tombstones for this instance; never evict and rerun
    return reply;
}
void WorkspaceControlServer::observe(const QString& id, const QJsonObject& state) {
    if (!operations_.contains(id)) {
        while (operation_order_.size() >= 128) { operations_.remove(operation_order_.front()); operation_order_.pop_front(); }
        operation_order_.push_back(id);
    }
    operations_[id] = state;
    auto event = state; event["operation_id"] = id; event["cursor"] = QString::number(event_cursor_++);
    events_.push_back(event); if (events_.size() > 2048) events_.pop_front();
}
}
