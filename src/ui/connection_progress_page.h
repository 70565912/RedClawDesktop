#pragma once

#include <array>
#include <functional>

#include <QWidget>

#include "ui/connection_flow_model.h"

class QLabel;
class QPushButton;
class QFrame;

namespace redclaw::ui {

class ConnectionProgressPage final : public QWidget {
 public:
  explicit ConnectionProgressPage(ConnectionFlowRole role, QWidget* parent = nullptr);

  void set_connection_code(const QString& code);
  void set_remote_control_authorized(bool authorized);
  void set_remote_agent_authorized(bool authorized, const QString& readiness = {});
  void set_snapshot(const ConnectionFlowSnapshot& snapshot);
  void set_on_retry(std::function<void()> callback);
  void set_on_exit(std::function<void()> callback);

  [[nodiscard]] int stage_count() const;
  [[nodiscard]] ConnectionStageState stage_state(int index) const;
  [[nodiscard]] QString phase_text() const;
  [[nodiscard]] QPushButton* retry_button() const;
  [[nodiscard]] QPushButton* exit_button() const;

 private:
  struct StageWidgets {
    QFrame* frame = nullptr;
    QLabel* number = nullptr;
    QLabel* title = nullptr;
    QLabel* state = nullptr;
  };

  void apply_stage_state(int index, ConnectionStageState state);

  ConnectionFlowRole role_;
  QLabel* title_ = nullptr;
  QLabel* subtitle_ = nullptr;
  QLabel* phase_ = nullptr;
  QLabel* attempt_ = nullptr;
  QLabel* code_ = nullptr;
  QLabel* remote_control_ = nullptr;
  QLabel* remote_agent_ = nullptr;
  QLabel* failure_ = nullptr;
  QPushButton* retry_ = nullptr;
  QPushButton* exit_ = nullptr;
  std::array<StageWidgets, kConnectionStageCount> stages_;
  std::array<QLabel*, kConnectionStageCount - 1> arrows_{};
  std::array<ConnectionStageState, kConnectionStageCount> stage_states_{};
  std::function<void()> on_retry_;
  std::function<void()> on_exit_;
};

}  // namespace redclaw::ui
