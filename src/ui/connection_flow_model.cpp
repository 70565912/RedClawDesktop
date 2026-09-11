#include "ui/connection_flow_model.h"

#include <algorithm>
#include <limits>

namespace redclaw::ui {

void ConnectionFlowModel::start(ConnectionFlowRole role) {
  reset();
  snapshot_.role = role;
  snapshot_.view = role == ConnectionFlowRole::kHost
      ? ConnectionFlowView::kWaiting
      : ConnectionFlowView::kConnecting;
  snapshot_.status = ConnectionFlowStatus::kRunning;
  snapshot_.attempt = 1;
  snapshot_.can_exit = true;
  snapshot_.stages[0] = ConnectionStageState::kCurrent;
}

void ConnectionFlowModel::apply(ConnectionFlowEvent event, const QString& detail) {
  if (event == ConnectionFlowEvent::kExitCompleted) {
    reset();
    return;
  }
  if (event == ConnectionFlowEvent::kStarted) {
    start(snapshot_.role);
    return;
  }
  if (event == ConnectionFlowEvent::kAutomaticRetry) {
    if (snapshot_.status == ConnectionFlowStatus::kIdle
        || snapshot_.status == ConnectionFlowStatus::kStopping) {
      return;
    }
    snapshot_.status = ConnectionFlowStatus::kRunning;
    snapshot_.transport_ready = false;
    baseline_encoded_ = last_encoded_;
    baseline_transmitted_ = last_transmitted_;
    if (snapshot_.attempt < (std::numeric_limits<int>::max)()) {
      snapshot_.attempt = (std::max)(1, snapshot_.attempt + 1);
    }
    snapshot_.failure_detail.clear();
    snapshot_.can_retry = false;
    snapshot_.can_exit = true;
    snapshot_.current_stage = 0;
    snapshot_.stages.fill(ConnectionStageState::kPending);
    snapshot_.stages[0] = ConnectionStageState::kCurrent;
    return;
  }

  if (snapshot_.status == ConnectionFlowStatus::kIdle
      || snapshot_.status == ConnectionFlowStatus::kStopping) {
    return;
  }

  if (event == ConnectionFlowEvent::kRetryWaiting) {
    snapshot_.transport_ready = false;
    baseline_encoded_ = last_encoded_;
    baseline_transmitted_ = last_transmitted_;
    snapshot_.status = ConnectionFlowStatus::kRetryWaiting;
    snapshot_.failure_detail.clear();
    snapshot_.can_retry = false;
    snapshot_.can_exit = true;
    return;
  }

  if (event == ConnectionFlowEvent::kFatalFailure) {
    snapshot_.transport_ready = false;
    snapshot_.status = ConnectionFlowStatus::kFailed;
    snapshot_.failure_detail = detail;
    snapshot_.can_retry = true;
    snapshot_.can_exit = true;
    snapshot_.stages[snapshot_.current_stage] = ConnectionStageState::kFailed;
    return;
  }

  if (snapshot_.status == ConnectionFlowStatus::kFailed
      || snapshot_.status == ConnectionFlowStatus::kComplete) {
    return;
  }

  switch (event) {
    case ConnectionFlowEvent::kLocalDescriptionPublished:
      set_current_stage(snapshot_.role == ConnectionFlowRole::kHost ? 1 : 2);
      break;
    case ConnectionFlowEvent::kRemoteDescriptionApplied:
      set_current_stage(snapshot_.role == ConnectionFlowRole::kHost ? 2 : 1);
      break;
    case ConnectionFlowEvent::kIceConnected:
    case ConnectionFlowEvent::kChannelOpened:
      set_current_stage(3);
      break;
    case ConnectionFlowEvent::kFirstFrameReady:
      if (snapshot_.transport_ready) {
        complete_flow();
      }
      break;
    case ConnectionFlowEvent::kStarted:
    case ConnectionFlowEvent::kRetryWaiting:
    case ConnectionFlowEvent::kAutomaticRetry:
    case ConnectionFlowEvent::kFatalFailure:
    case ConnectionFlowEvent::kExitCompleted:
      break;
  }
}

void ConnectionFlowModel::observe_transport(bool connected, bool channel_open) {
  if (snapshot_.status == ConnectionFlowStatus::kIdle
      || snapshot_.status == ConnectionFlowStatus::kStopping
      || snapshot_.status == ConnectionFlowStatus::kFailed) {
    return;
  }
  const bool ready = connected && channel_open;
  if (!ready && snapshot_.transport_ready) {
    // Runtime survives peer departure. Completion must not survive its transport.
    snapshot_.status = ConnectionFlowStatus::kDisconnected;
    snapshot_.current_stage = 0;
    snapshot_.stages.fill(ConnectionStageState::kPending);
    snapshot_.stages[0] = ConnectionStageState::kCurrent;
    snapshot_.can_retry = false;
    snapshot_.failure_detail.clear();
  }
  snapshot_.transport_ready = ready;
  if (!ready) {
    baseline_encoded_ = last_encoded_;
    baseline_transmitted_ = last_transmitted_;
  } else if (snapshot_.status == ConnectionFlowStatus::kDisconnected
             || snapshot_.status == ConnectionFlowStatus::kRetryWaiting) {
    snapshot_.status = ConnectionFlowStatus::kRunning;
    set_current_stage(3);
  }
}

void ConnectionFlowModel::observe_host_frames(
    std::uint64_t captured, std::uint64_t encoded, std::uint64_t transmitted) {
  // Counters may reset with a replacement runtime, or remain cumulative across
  // ICE retries. Never use the previous connection's nonzero totals as success.
  if (encoded < last_encoded_) baseline_encoded_ = 0;
  if (transmitted < last_transmitted_) baseline_transmitted_ = 0;
  last_encoded_ = encoded;
  last_transmitted_ = transmitted;
  if (!snapshot_.transport_ready) {
    baseline_encoded_ = encoded;
    baseline_transmitted_ = transmitted;
  } else if (snapshot_.role == ConnectionFlowRole::kHost && captured > 0
             && encoded > baseline_encoded_ && transmitted > baseline_transmitted_) {
    // A retained real static capture can supply the new session's first IDR;
    // require new encode/send progress, not an unnecessary fresh capture.
    apply(ConnectionFlowEvent::kFirstFrameReady);
  }
}

void ConnectionFlowModel::begin_stopping() {
  if (snapshot_.status == ConnectionFlowStatus::kIdle) {
    return;
  }
  snapshot_.status = ConnectionFlowStatus::kStopping;
  snapshot_.transport_ready = false;
  snapshot_.can_retry = false;
  snapshot_.can_exit = false;
}

void ConnectionFlowModel::reset() {
  snapshot_ = ConnectionFlowSnapshot{};
  last_encoded_ = last_transmitted_ = 0;
  baseline_encoded_ = baseline_transmitted_ = 0;
}

const ConnectionFlowSnapshot& ConnectionFlowModel::snapshot() const {
  return snapshot_;
}

void ConnectionFlowModel::set_current_stage(int stage_index) {
  stage_index = (std::clamp)(stage_index, 0, kConnectionStageCount - 1);
  if (stage_index <= snapshot_.current_stage) {
    return;
  }
  for (int index = 0; index < kConnectionStageCount; ++index) {
    snapshot_.stages[index] = index < stage_index
        ? ConnectionStageState::kComplete
        : (index == stage_index ? ConnectionStageState::kCurrent : ConnectionStageState::kPending);
  }
  snapshot_.current_stage = stage_index;
}

void ConnectionFlowModel::complete_flow() {
  snapshot_.stages.fill(ConnectionStageState::kComplete);
  snapshot_.current_stage = kConnectionStageCount - 1;
  snapshot_.status = ConnectionFlowStatus::kComplete;
  snapshot_.failure_detail.clear();
  snapshot_.can_retry = false;
  snapshot_.can_exit = true;
}

ConnectionFlowLineEvent classify_connection_flow_line(
    QStringView line,
    ConnectionFlowRole role) {
  ConnectionFlowLineEvent result;
  if (line.startsWith(u"Runtime rebuilding signaling session role=")) {
    result.matched = true;
    result.event = ConnectionFlowEvent::kAutomaticRetry;
    return result;
  }

  const bool retryable_ice_failure =
      line.contains(u"Runtime ICE state failed before DHT signaling completed; continuing")
      || line.contains(u"Runtime ICE state failed; DHT retry remains active");
  if (retryable_ice_failure) {
    result.matched = true;
    result.event = ConnectionFlowEvent::kRetryWaiting;
    return result;
  }
  if (line.contains(u"Runtime signaling timeout")
          || line.contains(u"Runtime signaling failed")
          || line.contains(u"Runtime ICE state failed")) {
    result.matched = true;
    result.event = ConnectionFlowEvent::kFatalFailure;
    result.detail = line.toString();
    return result;
  }

  if (line.startsWith(u"Runtime remote description applied role=")) {
    result.matched = true;
    result.event = ConnectionFlowEvent::kRemoteDescriptionApplied;
    return result;
  }
  if (line.startsWith(u"Runtime DHT signal published role=")) {
    const QStringView expected_role = role == ConnectionFlowRole::kHost
        ? QStringView(u"role=host")
        : QStringView(u"role=controller");
    if (line.contains(expected_role)) {
      result.matched = true;
      result.event = ConnectionFlowEvent::kLocalDescriptionPublished;
      return result;
    }
  }
  if (line.startsWith(u"Runtime state role=") && line.contains(u"connected=true")) {
    result.matched = true;
    result.event = ConnectionFlowEvent::kIceConnected;
    return result;
  }
  if (line.contains(u"desktop stream data channel opened")) {
    result.matched = true;
    result.event = ConnectionFlowEvent::kChannelOpened;
    return result;
  }
  return result;
}

}  // namespace redclaw::ui
