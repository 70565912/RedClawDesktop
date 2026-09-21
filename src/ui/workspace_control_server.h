#pragma once
#include <QObject>
#include <QJsonObject>
#include <QMap>
#include <QLocalServer>
#include <functional>
#include <deque>

namespace redclaw::ui {
class WorkspaceControlServer final : public QObject {
public:
    using Invoke = std::function<QJsonObject(const QString&, const QJsonObject&, const QString&)>;
    WorkspaceControlServer(Invoke invoke, QObject* parent = nullptr);
    bool start(const QString& name, QString* error);
    QJsonObject dispatch(const QJsonObject& request);
    void observe(const QString& id, const QJsonObject& state);
    QString instance_id() const { return instance_; }
    static bool current_user_client(qintptr descriptor);
private:
    void accept();
    Invoke invoke_;
    QLocalServer server_;
    QString instance_;
    struct Request { QByteArray digest; QJsonObject reply; };
    QMap<QString, Request> requests_;
    QMap<QString, QJsonObject> operations_;
    std::deque<QString> operation_order_;
    std::deque<QJsonObject> events_;
    std::uint64_t event_cursor_ = 0;
};
}
