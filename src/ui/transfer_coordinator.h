#pragma once
#include "redclaw/protocol/stream_control_protocol.h"
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QStringList>
#include <functional>
#include <deque>

namespace redclaw::ui {
class TransferCoordinator final : public QObject {
public:
    using Control = protocol::StreamControlMessageV1;
    using Send = std::function<bool(const Control&, QString*)>;
    explicit TransferCoordinator(Send send, QObject* parent = nullptr);
    bool submit(const Control& message, QString* error);
    bool start_files(Control request, const QStringList& sources, QString* error);
    void receive(const Control& message);
    void disconnected();
    QJsonObject invoke(const QString& method, const QJsonObject& params, const QString& id);
    bool busy() const { return runtime_busy_ || !active_.isEmpty(); }
    std::function<void(const Control&)> started;
    std::function<void(const Control&)> local_event;
    std::function<void(const QString&, const QJsonObject&)> event;
private:
    void next_source();
    void notify(const QString& id);
    struct Operation { Control request; QJsonObject state; QJsonArray entries; std::uint64_t first = 0, next = 0; };
    Send send_;
    QMap<QString, Operation> operations_;
    std::deque<QString> order_;
    QString active_, browsing_;
    QStringList sources_;
    qsizetype source_index_ = 0;
    bool selection_sent_ = false, runtime_busy_ = false;
    std::uint32_t file_version_ = 0, clipboard_version_ = 0;
};
}
