#pragma once

#include <array>
#include <cstdint>

#include <QString>
#include <QStringView>

namespace redclaw::ui {

constexpr int kConnectionStageCount = 4;

enum class ConnectionFlowView {
  kPreConnection,
  kConnecting,
  kWaiting,
};

enum class ConnectionFlowRole {
  kController,
  kHost,
};

enum class ConnectionFlowEvent {
  kStarted,
  kLocalDescriptionPublished,
  kRemoteDescriptionApplied,
  kIceConnected,
  kChannelOpened,
  kFirstFrameReady,
  kRetryWaiting,
  kAutomaticRetry,
  kFatalFailure,
  kExitCompleted,
};

enum class ConnectionStageState {
  kPending,
  kCurrent,
  kComplete,
  kFailed,
};

enum class ConnectionFlowStatus {
  kIdle,
  kRunning,
  kRetryWaiting,
  kFailed,
  kDisconnected,
  kComplete,
  kStopping,
};

struct ConnectionFlowSnapshot {
  ConnectionFlowView view = ConnectionFlowView::kPreConnection;
  ConnectionFlowRole role = ConnectionFlowRole::kController;
  ConnectionFlowStatus status = ConnectionFlowStatus::kIdle;
  std::array<ConnectionStageState, kConnectionStageCount> stages{
      ConnectionStageState::kPending,
      ConnectionStageState::kPending,
      ConnectionStageState::kPending,
      ConnectionStageState::kPending,
  };
  int current_stage = 0;
  int stage_count = kConnectionStageCount;
  int attempt = 0;
  QString failure_detail;
  bool can_retry = false;
  bool can_exit = false;
  bool transport_ready = false;
};

struct ConnectionFlowLineEvent {
  bool matched = false;
  ConnectionFlowEvent event = ConnectionFlowEvent::kStarted;
  QString detail;
};

class ConnectionFlowModel {
 public:
  void start(ConnectionFlowRole role);
  void apply(ConnectionFlowEvent event, const QString& detail = {});
  void observe_transport(bool connected, bool channel_open);
  void observe_host_frames(std::uint64_t captured, std::uint64_t encoded,
                           std::uint64_t transmitted);
  void begin_stopping();
  void reset();

  [[nodiscard]] const ConnectionFlowSnapshot& snapshot() const;

 private:
  void set_current_stage(int stage_index);
  void complete_flow();

  ConnectionFlowSnapshot snapshot_;
  std::uint64_t last_encoded_ = 0;
  std::uint64_t last_transmitted_ = 0;
  std::uint64_t baseline_encoded_ = 0;
  std::uint64_t baseline_transmitted_ = 0;
};

[[nodiscard]] ConnectionFlowLineEvent classify_connection_flow_line(
    QStringView line,
    ConnectionFlowRole role);

}  // namespace redclaw::ui
