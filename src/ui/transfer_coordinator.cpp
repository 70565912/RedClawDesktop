#include "ui/transfer_coordinator.h"
#include "redclaw/workspace/transfer_result_journal.h"
#include "redclaw_wire.pb.h"
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace redclaw::ui {
namespace {
using Action = protocol::WorkspaceActionV1;
using Direction = protocol::TransferDirectionV1;
using Purpose = protocol::WorkspaceTransferPurposeV1;
QJsonObject failure(const char* error) { return {{"ok", false}, {"error", error}}; }
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
TransferCoordinator::Control request(Action action, const QString& id) {
    TransferCoordinator::Control result; result.type = protocol::StreamControlMessageTypeV1::kWorkspace;
    result.session_epoch = "gui-workspace"; result.message_id = 1; result.sent_at_ms = QDateTime::currentMSecsSinceEpoch();
    result.request_id = utf8(id); result.workspace.emplace(); result.workspace->action = action; return result;
}
}
TransferCoordinator::TransferCoordinator(Send send, QObject* parent) : QObject(parent), send_(std::move(send)) {}
void TransferCoordinator::notify(const QString& id) { if (event && operations_.contains(id)) event(id, operations_[id].state); }
bool TransferCoordinator::submit(const Control& message, QString* error) {
    if (!message.workspace || !protocol::validate_workspace_control_v1(*message.workspace)) {
        if (error) *error = "workspace_invalid_request"; return false;
    }
    const auto action = message.workspace->action;
    const auto id = QString::fromStdString(message.request_id);
    const bool create = action == Action::kPrepare || action == Action::kBrowse || action == Action::kBrowseClipboardCopies;
    if (create) {
        if (action != Action::kPrepare && !browsing_.isEmpty()) { if (error) *error = "workspace_browse_busy"; return false; }
        if ((action == Action::kPrepare && busy()) || operations_.contains(id)) { if (error) *error = "workspace_busy"; return false; }
        while (order_.size() >= 128) {
            if (order_.front() == active_) { if (error) *error = "operation_history_full"; return false; }
            operations_.remove(order_.front()); order_.pop_front();
        }
        operations_[id] = {message, {{"ok", true}, {"operation_id", id}, {"state", "submitted"}}};
        order_.push_back(id);
        if (action == Action::kPrepare) { active_ = id; if (started) started(message); }
        else browsing_ = id;
    }
    if (!send_ || !send_(message, error)) {
        if (create) { operations_[id].state["state"] = "unknown"; operations_[id].state["error"] = "workspace_send_unconfirmed"; notify(id); }
        if (create && local_event) {
            auto failed = message; failed.workspace->action = Action::kError;
            failed.workspace->path.clear(); failed.workspace->clipboard_source.clear(); failed.workspace->clipboard_sequence = 0;
            failed.workspace->error_code = "workspace_send_unconfirmed"; local_event(failed);
        }
        if (active_ == id) active_.clear();
        if (browsing_ == id) browsing_.clear();
        return false;
    }
    if (create) notify(id);
    return true;
}
bool TransferCoordinator::start_files(Control message, const QStringList& sources, QString* error) {
    if (busy() || sources.isEmpty()) { if (error) *error = "workspace_busy_or_empty"; return false; }
    sources_ = sources; source_index_ = 0; selection_sent_ = false;
    if (!submit(message, error)) { sources_.clear(); return false; }
    return true;
}
void TransferCoordinator::next_source() {
    if (active_.isEmpty() || selection_sent_ || !operations_.contains(active_)) return;
    auto message = operations_[active_].request;
    if (message.workspace->purpose != Purpose::kFiles) return;
    const bool done = source_index_ >= sources_.size();
    message.workspace->action = done ? Action::kSelectionComplete : Action::kSelectSource;
    message.workspace->path = done ? std::string{} : utf8(sources_[source_index_]);
    QString error;
    if (!send_(message, &error)) { (void)invoke("operation.cancel", {}, active_); return; }
    selection_sent_ = done;
}
void TransferCoordinator::receive(const Control& message) {
    if (!message.workspace) return;
    const auto& value = *message.workspace;
    if (value.action == Action::kAvailability) {
        file_version_ = message.file_transfer_version; clipboard_version_ = message.clipboard_version; runtime_busy_ = value.active; return;
    }
    const auto id = QString::fromStdString(message.request_id);
    if (!operations_.contains(id)) return;
    auto& operation = operations_[id]; auto& state = operation.state;
    if (value.action == Action::kPrepared && id == active_) next_source();
    if (value.action == Action::kSourceAccepted && id == active_ && value.accepted_sources == static_cast<std::uint64_t>(source_index_ + 1)) {
        ++source_index_; next_source();
    }
    if (value.action == Action::kBrowseEntry) {
        operation.entries.append(QJsonObject{{"path", QString::fromStdString(value.path)}, {"directory", value.directory},
            {"bytes", QString::number(value.bytes)}, {"created_at_ms", QString::number(value.created_at_ms)}});
        ++operation.next;
        if (operation.entries.size() > 2048) { operation.entries.removeFirst(); ++operation.first; }
        return;
    }
    if (value.action == Action::kBrowseEnd) {
        state["state"] = value.error_code.empty() ? "succeeded" : "failed";
        if (browsing_ == id) browsing_.clear();
    }
    else if (value.action == Action::kFinished || value.action == Action::kError) {
        state["state"] = value.error_code.empty() ? "succeeded"
            : value.error_code == "transfer_cancelled" ? "cancelled"
            : value.error_code == "workspace_connection_lost" || value.error_code == "workspace_send_unconfirmed" ? "unknown" : "failed";
        state["results_path"] = QString::fromStdString(value.results_path);
        if (!value.snapshot_path.empty()) {
            const auto snapshot = QString::fromStdString(value.snapshot_path);
            state["snapshot_path"] = snapshot; state["snapshot_state"] = "loading";
            auto* watcher = new QFutureWatcher<QJsonObject>(this);
            connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, watcher, id] {
                const auto result = watcher->result(); watcher->deleteLater();
                if (!operations_.contains(id)) return;
                for (auto it = result.begin(); it != result.end(); ++it) operations_[id].state.insert(it.key(), it.value());
                notify(id);
            });
            watcher->setFuture(QtConcurrent::run([snapshot] {
                QFile file(snapshot + "/metadata/manifest.pb");
                if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return QJsonObject{{"snapshot_state", "failed"}, {"snapshot_error", "clipboard_manifest_unavailable"}};
                protocol::wire::ClipboardPayloadManifestV1 manifest; const auto bytes = file.readAll();
                if (!manifest.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size())) || manifest.schema_version() != 1)
                    return QJsonObject{{"snapshot_state", "failed"}, {"snapshot_error", "clipboard_manifest_invalid"}};
                QJsonArray formats;
                for (const auto& format : manifest.formats()) formats.append(QJsonObject{{"kind", static_cast<int>(format.kind())},
                    {"bytes", QString::number(format.size())}, {"file", snapshot + "/metadata/format-" + QString::number(format.kind()) + ".bin"}});
                if (manifest.file_roots()) formats.append(QJsonObject{{"kind", "files"}, {"roots", static_cast<int>(manifest.file_roots())}, {"directory", snapshot + "/files"}});
                return QJsonObject{{"snapshot_state", "ready"}, {"formats", formats}};
            }));
        }
        state["paste_submitted"] = value.paste_submitted;
        if (active_ == id) { active_.clear(); sources_.clear(); }
        if (browsing_ == id) browsing_.clear();
    } else if (value.action == Action::kCancel) state["state"] = "cancelling";
    else state["state"] = "transferring";
    state["error"] = QString::fromStdString(value.error_code);
    state["received_bytes"] = QString::number(value.completed_bytes); state["verified_bytes"] = QString::number(value.committed_bytes);
    state["total_bytes"] = QString::number(value.bytes); state["completed_files"] = QString::number(value.completed_files);
    state["skipped_entries"] = QString::number(value.skipped_entries); state["total_files"] = QString::number(value.files);
    notify(id);
}
void TransferCoordinator::disconnected() {
    file_version_ = clipboard_version_ = 0; runtime_busy_ = false;
    for (auto it = operations_.begin(); it != operations_.end(); ++it) {
        const auto state = it->state.value("state").toString();
        if (state == "submitted" || state == "transferring" || state == "cancelling") {
            it->state["state"] = "unknown"; it->state["error"] = "workspace_connection_lost"; notify(it.key());
        }
    }
    active_.clear(); browsing_.clear(); sources_.clear();
}
QJsonObject TransferCoordinator::invoke(const QString& method, const QJsonObject& params, const QString& id) {
    if (method == "capabilities" || method == "status") return {{"ok", true}, {"file_transfer_version", static_cast<int>(file_version_)},
        {"clipboard_version", static_cast<int>(clipboard_version_)}, {"transfer_busy", busy()}, {"active_operation_id", active_}};
    if (method == "operation.status" || method == "operation.result") {
        if (!operations_.contains(id)) return failure("operation_not_found");
        auto& operation = operations_[id]; auto result = operation.state;
        if (method == "operation.result") {
            bool valid = false; const auto cursor = params.value("cursor").toString("0").toULongLong(&valid);
            if (!valid) return failure("invalid_cursor");
            if (operation.request.workspace->action == Action::kBrowse || operation.request.workspace->action == Action::kBrowseClipboardCopies) {
                if (cursor > operation.next) return failure("cursor_ahead");
                QJsonArray page; auto next = std::max(cursor, operation.first);
                for (; next < operation.next && page.size() < 128; ++next) page.append(operation.entries[static_cast<qsizetype>(next - operation.first)]);
                result["items"] = page; result["gap"] = cursor < operation.first;
                result["next_cursor"] = QString::number(next); result["end_cursor"] = QString::number(operation.next);
            } else if (!result.value("results_path").toString().isEmpty()) {
                const auto key = QString::number(cursor);
                if (result.value("page_cursor").toString() != key) {
                    if (result.value("items_state") == "loading") return result;
                    operation.state["items_state"] = "loading"; operation.state["page_cursor"] = key;
                    auto* watcher = new QFutureWatcher<workspace::TransferResultPage>(this);
                    connect(watcher, &QFutureWatcher<workspace::TransferResultPage>::finished, this, [this, watcher, id, key] {
                        const auto page = watcher->result(); watcher->deleteLater();
                        if (!operations_.contains(id) || operations_[id].state.value("page_cursor").toString() != key) return;
                        auto& output = operations_[id].state; QJsonArray items;
                        for (const auto& item : page.entries) items.append(QJsonObject{{"path", QString::fromStdString(item.relative_path)},
                            {"bytes", QString::number(item.size)}, {"directory", item.directory}, {"state", item.skipped ? "skipped" : "verified_committed"}});
                        output["items"] = items; output["items_state"] = page.error.empty() ? "ready" : "failed";
                        output["items_error"] = QString::fromStdString(page.error); output["next_cursor"] = QString::number(page.end);
                        output["has_next"] = page.has_next;
                    });
                    const auto path = std::filesystem::path(result.value("results_path").toString().toStdWString());
                    watcher->setFuture(QtConcurrent::run([path, cursor] { return workspace::read_transfer_result_page(path, cursor); }));
                    return operation.state;
                }
            }
        }
        return result;
    }
    if (method == "operation.cancel") {
        if (!operations_.contains(id)) return failure("operation_not_found");
        const auto state = operations_[id].state.value("state").toString();
        if (state == "cancelling" || state == "succeeded" || state == "failed" || state == "cancelled" || state == "unknown")
            return operations_[id].state;
        if (active_ != id) return failure("operation_not_running");
        auto message = operations_[id].request; message.workspace->action = Action::kCancel;
        message.workspace->path.clear(); message.workspace->clipboard_source.clear(); message.workspace->clipboard_sequence = 0;
        QString error;
        if (!submit(message, &error)) return failure("cancel_unconfirmed");
        operations_[id].state["state"] = "cancelling"; notify(id);
        return operations_[id].state;
    }
    if (!file_version_) return failure("workspace_peer_unavailable");
    const bool clipboard = method.startsWith("clipboard.");
    if (clipboard && clipboard_version_ < (method.startsWith("clipboard.copies.") ? 2U : 3U)) return failure("clipboard_capability_insufficient");
    if (busy()) return failure("workspace_transfer_busy");
    auto message = request(Action::kPrepare, id);
    auto& details = *message.workspace;
    if (method.startsWith("clipboard.copies.")) {
        const auto target = params.value("target").toString("remote");
        if (target != "local" && target != "remote") return failure("invalid_target");
        if (target == "local") {
            if (clipboard_version_ < 3) return failure("clipboard_capability_insufficient");
            details.direction = Direction::kToController;
        }
    }
    if (method == "files.browse" || method == "clipboard.copies.list") {
        details.action = method == "files.browse" ? Action::kBrowse : Action::kBrowseClipboardCopies;
        details.path = utf8(params.value("path").toString());
    } else if (method == "files.upload" || method == "files.download") {
        if (!params.value("sources").isArray() || params.value("sources").toArray().isEmpty()
            || params.value("sources").toArray().size() > 1024 || !params.value("destination").isString()) return failure("invalid_sources");
        QStringList sources;
        for (const auto& path : params.value("sources").toArray()) {
            if (!path.isString() || path.toString().isEmpty()) return failure("invalid_source");
            sources.push_back(path.toString());
        }
        details.direction = method == "files.upload" ? Direction::kToHost : Direction::kToController;
        details.path = utf8(params.value("destination").toString());
        const auto policy = params.value("conflict").toString("keepBoth");
        if (policy != "keepBoth" && policy != "overwrite" && policy != "skip") return failure("invalid_conflict");
        details.conflict = policy == "overwrite" ? protocol::TransferConflictV1::kOverwrite
            : policy == "skip" ? protocol::TransferConflictV1::kSkip : protocol::TransferConflictV1::kKeepBoth;
        QString error;
        if (!start_files(message, sources, &error)) return {{"ok", false}, {"error", error}};
        return operations_[id].state;
    } else if (method == "clipboard.copies.open" || method == "clipboard.copies.cleanup") {
        details.purpose = method == "clipboard.copies.open" ? Purpose::kClipboardOpenCopy : Purpose::kClipboardCleanup;
        details.path = utf8(params.value("copy_id").toString());
    } else if (method == "clipboard.read" || method == "clipboard.write" || method == "clipboard.paste") {
        const auto target = params.value("target").toString("remote");
        if (target != "remote" && target != "local") return failure("invalid_target");
        if (method == "clipboard.paste" && target == "local") return failure("clipboard_paste_requires_remote_target");
        details.purpose = Purpose::kClipboard;
        details.clipboard_mode = method == "clipboard.read" ? 1 : method == "clipboard.write" ? 2 : 3;
        details.direction = method == "clipboard.read" ? Direction::kToController : Direction::kToHost;
        if (target == "local") {
            details.clipboard_mode += 3; details.direction = Direction::kToController;
        }
        if (method == "clipboard.write") {
            protocol::wire::ClipboardSourceV1 source; source.set_schema_version(1);
            if (params.contains("text")) {
                if (!params.value("text").isString()) return failure("invalid_text");
                const auto text = params.value("text").toString(); auto* format = source.add_formats(); format->set_kind(1);
                format->set_data(reinterpret_cast<const char*>(text.utf16()), static_cast<std::size_t>(text.size()) * 2);
                format->mutable_data()->append(2, '\0');
            }
            if (params.contains("formats")) {
                if (!params.value("formats").isArray()) return failure("invalid_formats");
                for (const auto& entry : params.value("formats").toArray()) {
                    const auto value = entry.toObject(); const int kind = value.value("kind").toInt();
                    if (kind < 1 || kind > 6) return failure("invalid_format");
                    auto* format = source.add_formats(); format->set_kind(kind);
                    if (value.contains("file")) format->set_file(utf8(value.value("file").toString()));
                    else {
                        const auto bytes = QByteArray::fromBase64(value.value("base64").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
                        if (bytes.isEmpty()) return failure("invalid_format_data");
                        format->set_data(bytes.constData(), static_cast<std::size_t>(bytes.size()));
                    }
                }
            }
            if (params.contains("image")) { auto* format = source.add_formats(); format->set_kind(4); format->set_file(utf8(params.value("image").toString())); }
            if (params.contains("files")) {
                if (!params.value("files").isArray()) return failure("invalid_files");
                for (const auto& file : params.value("files").toArray()) {
                    if (!file.isString() || file.toString().isEmpty()) return failure("invalid_file");
                    source.add_files(utf8(file.toString()));
                }
            }
            if (params.contains("snapshot")) source.set_snapshot_directory(utf8(params.value("snapshot").toString()));
            if (!source.formats_size() && !source.files_size() && source.snapshot_directory().empty()) return failure("clipboard_source_required");
            details.clipboard_source = source.SerializeAsString();
        }
    } else return failure("unknown_operation");
    QString error;
    if (!submit(message, &error)) return {{"ok", false}, {"error", error}};
    return operations_[id].state;
}
}
