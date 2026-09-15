#include "ui/terminal/workspace_pipe_server.h"
#include "redclaw/protocol/compressed_protobuf.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QUuid>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace redclaw::ui {
namespace { constexpr qsizetype kMaximumFrame = protocol::kMaxProtobufWireBytes; }
struct WorkspacePipeServer::Impl {
    WorkspacePipeServer* owner;
    QLocalServer server;
    QPointer<QLocalSocket> socket;
    QByteArray incoming;
    quint32 expected_pid = 0;
    bool verified = false;
    std::function<void(std::string_view)> receive;
    std::function<void(bool)> connection;
    std::function<void()> writable;

    void verify() {
        if (!socket || verified || !expected_pid) return;
#ifdef _WIN32
        ULONG client_pid = 0;
        if (!GetNamedPipeClientProcessId(reinterpret_cast<HANDLE>(socket->socketDescriptor()), &client_pid)
            || client_pid != expected_pid) { socket->abort(); return; }
        verified = true;
        if (connection) connection(true);
        read();
#endif
    }
    void read() {
        if (!socket || !verified) return;
        if (incoming.size() + socket->bytesAvailable() > 2 * (kMaximumFrame + 4)) { socket->abort(); return; }
        incoming += socket->readAll();
        while (socket && incoming.size() >= 4) {
            quint32 length = 0;
            for (unsigned index = 0; index < 4; ++index) length |= static_cast<quint32>(static_cast<unsigned char>(incoming[index])) << (8 * index);
            if (!length || length > kMaximumFrame) { socket->abort(); return; }
            if (incoming.size() < 4 + length) break;
            if (receive) receive(std::string_view(incoming.constData() + 4, length));
            incoming.remove(0, 4 + length);
        }
    }
    void accept() {
        while (server.hasPendingConnections()) {
            auto* candidate = server.nextPendingConnection();
            if (socket) { candidate->abort(); candidate->deleteLater(); continue; }
            socket = candidate;
            socket->setReadBufferSize(2 * (kMaximumFrame + 4));
            QObject::connect(candidate, &QLocalSocket::readyRead, owner, [this] { read(); });
            QObject::connect(candidate, &QLocalSocket::bytesWritten, owner, [this](qint64) {
                if (socket && socket->bytesToWrite() == 0 && writable) writable();
            });
            QObject::connect(candidate, &QLocalSocket::disconnected, owner, [this, candidate] {
                if (socket == candidate) {
                    socket.clear(); verified = false; incoming.clear();
                    if (connection) connection(false);
                }
                candidate->deleteLater();
            });
            verify();
        }
    }
};
WorkspacePipeServer::WorkspacePipeServer(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    impl_->server.setSocketOptions(QLocalServer::UserAccessOption);
    impl_->server.setMaxPendingConnections(1);
    connect(&impl_->server, &QLocalServer::newConnection, this, [this] { impl_->accept(); });
}
WorkspacePipeServer::~WorkspacePipeServer() {
    impl_->connection = {}; impl_->writable = {}; impl_->receive = {};
    close();
}
QString WorkspacePipeServer::listen(QString* error) {
    close();
    const auto name = QStringLiteral("RedClawDesktop.Workspace.v1.") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!impl_->server.listen(name)) { if (error) *error = "workspace_pipe_listen_failed"; return {}; }
    return name;
}
void WorkspacePipeServer::set_expected_runtime_pid(quint32 pid) {
    if (impl_->expected_pid && impl_->expected_pid != pid) { close(); return; }
    impl_->expected_pid = pid;
    impl_->verify();
}
bool WorkspacePipeServer::send(std::string_view frame) {
    if (!connected() || impl_->socket->bytesToWrite() != 0 || frame.empty() || frame.size() > static_cast<std::size_t>(kMaximumFrame)) return false;
    QByteArray bytes(4 + static_cast<qsizetype>(frame.size()), Qt::Uninitialized);
    for (unsigned index = 0; index < 4; ++index) bytes[index] = static_cast<char>((frame.size() >> (8 * index)) & 255);
    std::copy(frame.begin(), frame.end(), bytes.begin() + 4);
    return impl_->socket->write(bytes) == bytes.size();
}
bool WorkspacePipeServer::connected() const {
    return impl_->verified && impl_->socket && impl_->socket->state() == QLocalSocket::ConnectedState;
}
void WorkspacePipeServer::close() {
    if (impl_->socket) {
        auto* socket = impl_->socket.data();
        QObject::disconnect(socket, nullptr, this, nullptr);
        socket->abort(); socket->deleteLater(); impl_->socket.clear();
    }
    const bool was_connected = impl_->verified;
    impl_->server.close(); impl_->incoming.clear(); impl_->verified = false; impl_->expected_pid = 0;
    if (was_connected && impl_->connection) impl_->connection(false);
}
void WorkspacePipeServer::set_receive_callback(std::function<void(std::string_view)> callback) { impl_->receive = std::move(callback); }
void WorkspacePipeServer::set_connection_callback(std::function<void(bool)> callback) { impl_->connection = std::move(callback); }
void WorkspacePipeServer::set_writable_callback(std::function<void()> callback) { impl_->writable = std::move(callback); }
}
