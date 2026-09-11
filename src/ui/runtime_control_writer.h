#pragma once
#include "gui_latency_probe.h"
#include "redclaw/protocol/stream_control_protocol.h"
#include <QProcess>

namespace redclaw::ui {
inline bool write_runtime_control_message(QProcess& process,
    const redclaw::protocol::StreamControlMessageV1& message, QString* error = nullptr) {
    GuiLatencyScope timing(GuiStage::kControlWrite);
    const auto fail = [&](const QString& detail) { if (error) *error = detail; return false; };
    if (process.state() != QProcess::Running) return fail("Runtime process is not running.");
    const auto encoded = redclaw::protocol::serialize_local_runtime_control_frame_v2(message);
    if (encoded.empty() || encoded.size() > redclaw::protocol::kMaxLocalRuntimeControlFrameBytes)
        return fail("Local runtime control frame exceeds the size limit.");
    QByteArray wire(encoded.data(), static_cast<qsizetype>(encoded.size()));
    wire.push_back('\n');
    if (process.write(wire) != wire.size()) return fail(process.errorString());
#ifdef _WIN32
    // Qt 6.2 buffers write() until a GUI timer starts the overlapped pipe write.
    // A zero-deadline poll starts that work now, without waiting for completion
    // or a GUI event-loop turn. False can mean still pending, not write failure.
    (void)process.waitForBytesWritten(0);
    if (process.error() == QProcess::WriteError) return fail(process.errorString());
#endif
    if (error) error->clear();
    return true;
}
}
