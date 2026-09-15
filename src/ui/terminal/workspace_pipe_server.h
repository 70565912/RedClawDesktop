#pragma once
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <string_view>

namespace redclaw::ui {
class WorkspacePipeServer final : public QObject {
public:
    explicit WorkspacePipeServer(QObject* parent = nullptr);
    ~WorkspacePipeServer() override;
    QString listen(QString* error = nullptr);
    void set_expected_runtime_pid(quint32 pid);
    bool send(std::string_view frame);
    bool connected() const;
    void close();
    void set_receive_callback(std::function<void(std::string_view)> callback);
    void set_connection_callback(std::function<void(bool)> callback);
    void set_writable_callback(std::function<void()> callback);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
