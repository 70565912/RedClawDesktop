#pragma once
#include <QStringList>
#include <optional>
#include <utility>
namespace redclaw::ui {
struct RuntimeMaintenanceSnapshot {
    QStringList gui_arguments, runtime_arguments;
};
// One immutable, private context for each GUI-owned Host runtime. Receipts and
// the independent worker live beside it, outside the replaceable runtime bundle.
class RuntimeMaintenanceContext final {
public:
    explicit RuntimeMaintenanceContext(QString previous_context = {}) : previous_context_(std::move(previous_context)) {}
    bool prepare(QString* error);
    bool save(const QStringList& gui_arguments, const QStringList& runtime_arguments, quint64 runtime_pid, QString* error);
    [[nodiscard]] QString path() const { return path_; }
    static std::optional<RuntimeMaintenanceSnapshot> read(const QString& path, QString* error);
private:
    QString path_;
    QString previous_context_;
};
}
