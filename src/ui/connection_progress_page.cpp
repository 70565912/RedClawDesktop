#include "ui/connection_progress_page.h"

#include <array>
#include <utility>

#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace redclaw::ui {

namespace {

constexpr std::array<const char*, kConnectionStageCount> kControllerStages = {
    "Find waiting device",
    "Exchange secure pairing data",
    "Establish network path",
    "Start remote desktop",
};

constexpr std::array<const char*, kConnectionStageCount> kHostStages = {
    "Publish this device code",
    "Wait for peer",
    "Establish secure connection",
    "Start desktop sharing",
};

QString state_text(ConnectionStageState state) {
  switch (state) {
    case ConnectionStageState::kPending:
      return "Pending";
    case ConnectionStageState::kCurrent:
      return "Current";
    case ConnectionStageState::kComplete:
      return "Complete";
    case ConnectionStageState::kFailed:
      return "Failed";
  }
  return "Pending";
}

QString stage_style(ConnectionStageState state) {
  switch (state) {
    case ConnectionStageState::kCurrent:
      return "QFrame#connectionStage { background-color: rgba(37, 99, 235, 0.18); border: 1px solid #60a5fa; border-radius: 16px; }";
    case ConnectionStageState::kComplete:
      return "QFrame#connectionStage { background-color: rgba(6, 95, 70, 0.26); border: 1px solid #10b981; border-radius: 16px; }";
    case ConnectionStageState::kFailed:
      return "QFrame#connectionStage { background-color: rgba(127, 29, 29, 0.34); border: 1px solid #ef4444; border-radius: 16px; }";
    case ConnectionStageState::kPending:
      return "QFrame#connectionStage { background-color: #0a1424; border: 1px solid #2a405e; border-radius: 16px; }";
  }
  return {};
}

QString state_style(ConnectionStageState state) {
  switch (state) {
    case ConnectionStageState::kCurrent:
      return "QLabel#connectionStageState { background-color: rgba(37, 99, 235, 0.22); color: #dbeafe; border: 1px solid #60a5fa; border-radius: 6px; padding: 6px 8px; }";
    case ConnectionStageState::kComplete:
      return "QLabel#connectionStageState { background-color: rgba(6, 95, 70, 0.34); color: #d1fae5; border: 1px solid #10b981; border-radius: 6px; padding: 6px 8px; }";
    case ConnectionStageState::kFailed:
      return "QLabel#connectionStageState { background-color: rgba(127, 29, 29, 0.42); color: #fee2e2; border: 1px solid #ef4444; border-radius: 6px; padding: 6px 8px; }";
    case ConnectionStageState::kPending:
      return "QLabel#connectionStageState { background-color: rgba(120, 53, 15, 0.30); color: #fde68a; border: 1px solid #f59e0b; border-radius: 6px; padding: 6px 8px; }";
  }
  return {};
}

}  // namespace

ConnectionProgressPage::ConnectionProgressPage(ConnectionFlowRole role, QWidget* parent)
    : QWidget(parent), role_(role) {
  setObjectName(role == ConnectionFlowRole::kHost ? "waitingConnectionPage" : "connectingPage");

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(24, 12, 24, 10);
  root->setSpacing(7);

  title_ = new QLabel(this);
  title_->setObjectName("heroTitle");
  subtitle_ = new QLabel(this);
  subtitle_->setObjectName("heroSubtitle");
  subtitle_->setWordWrap(true);
  subtitle_->hide();
  phase_ = new QLabel(this);
  phase_->setObjectName("connectionPhase");
  attempt_ = new QLabel(this);
  attempt_->setObjectName("statusCardTitle");
  attempt_->hide();
  code_ = new QLabel(this);
  code_->setObjectName("progressCode");
  code_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  if (role_ == ConnectionFlowRole::kHost) {
    title_->setText("Waiting for a connection");
    subtitle_->setText("Keep RedClaw open while the other device connects with this code.");
  } else {
    title_->setText("Connecting to remote device");
    subtitle_->setText("RedClaw is finding the waiting device and preparing the secure desktop path.");
  }

  root->addWidget(title_);
  root->addWidget(code_);
  if (role_ == ConnectionFlowRole::kHost) {
    remote_control_ = new QLabel(this);
    remote_control_->setObjectName("statusCardTitle");
    remote_control_->setWordWrap(true);
    root->addWidget(remote_control_);
    set_remote_control_authorized(false);
    remote_agent_ = new QLabel(this);
    remote_agent_->setObjectName("statusCardTitle");
    remote_agent_->setWordWrap(true);
    root->addWidget(remote_agent_);
    set_remote_agent_authorized(false);
  }
  root->addWidget(phase_);
  root->addWidget(attempt_);

  auto* flow_box = new QGroupBox("Connection progress", this);
  flow_box->setObjectName("connectionFlowBox");
  auto* flow = new QHBoxLayout(flow_box);
  flow->setContentsMargins(14, 14, 14, 10);
  flow->setSpacing(8);

  const auto& stage_names = role_ == ConnectionFlowRole::kHost ? kHostStages : kControllerStages;
  for (int index = 0; index < kConnectionStageCount; ++index) {
    auto* frame = new QFrame(flow_box);
    frame->setObjectName("connectionStage");
    frame->setMinimumWidth(124);
    frame->setMinimumHeight(116);
    auto* card = new QVBoxLayout(frame);
    card->setContentsMargins(12, 11, 12, 11);
    card->setSpacing(7);

    auto* number = new QLabel(QString("Stage %1").arg(index + 1), frame);
    number->setObjectName("connectionStageNumber");
    number->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* name = new QLabel(QString::fromLatin1(stage_names[index]), frame);
    name->setObjectName("connectionStageTitle");
    name->setWordWrap(true);
    name->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    name->setMinimumHeight(36);
    auto* state = new QLabel("Pending", frame);
    state->setObjectName("connectionStageState");
    state->setAlignment(Qt::AlignCenter);
    state->setMinimumHeight(32);

    card->addWidget(number);
    card->addWidget(name, 1);
    card->addWidget(state);
    stages_[index] = StageWidgets{frame, number, name, state};
    flow->addWidget(frame, 1);

    if (index < kConnectionStageCount - 1) {
      auto* arrow = new QLabel(flow_box);
      arrow->setObjectName("connectionStageArrow");
      arrow->setAccessibleName(QString("Stage %1 to stage %2").arg(index + 1).arg(index + 2));
      arrow->setAlignment(Qt::AlignCenter);
      arrow->setFixedWidth(22);
      arrow->setAutoFillBackground(false);
      arrow->setPixmap(style()->standardIcon(QStyle::SP_ArrowRight).pixmap(18, 18));
      arrows_[index] = arrow;
      flow->addWidget(arrow);
    }
  }
  root->addWidget(flow_box);

  failure_ = new QLabel(this);
  failure_->setObjectName("connectionFailure");
  failure_->setWordWrap(true);
  failure_->hide();
  root->addWidget(failure_);
  root->addStretch();

  auto* actions = new QHBoxLayout();
  actions->addStretch();
  retry_ = new QPushButton("Retry", this);
  retry_->setObjectName("primaryAction");
  retry_->hide();
  exit_ = new QPushButton("Exit", this);
  exit_->setObjectName("exitAction");
  actions->addWidget(retry_);
  actions->addWidget(exit_);
  root->addLayout(actions);

  QObject::connect(retry_, &QPushButton::clicked, this, [this]() {
    if (on_retry_) {
      on_retry_();
    }
  });
  QObject::connect(exit_, &QPushButton::clicked, this, [this]() {
    if (on_exit_) {
      on_exit_();
    }
  });

  set_snapshot(ConnectionFlowSnapshot{});
}

void ConnectionProgressPage::set_connection_code(const QString& code) {
  code_->setText(role_ == ConnectionFlowRole::kHost
      ? QString("This device: %1").arg(code)
      : QString("Connecting to: %1").arg(code));
}

void ConnectionProgressPage::set_remote_control_authorized(bool authorized) {
  if (remote_control_ == nullptr) {
    return;
  }
  remote_control_->setText(authorized
      ? "Remote control allowed for this wait session (ordinary desktop only)."
      : "View-only session: remote keyboard and mouse control is disabled.");
  const QString style = authorized ? "color: #d1fae5;" : "color: #fde68a;";
  if (remote_control_->styleSheet() != style) remote_control_->setStyleSheet(style);
}

void ConnectionProgressPage::set_remote_agent_authorized(
    bool authorized,
    const QString& readiness) {
  if (remote_agent_ == nullptr) {
    return;
  }
  QString text = authorized
      ? "Development Agent access allowed for registered projects in this wait session."
      : "Development Agent access is disabled for this wait session.";
  if (authorized && !readiness.isEmpty()) {
    text += " " + readiness;
  }
  remote_agent_->setText(text);
  const QString style = authorized ? "color: #d1fae5;" : "color: #fde68a;";
  if (remote_agent_->styleSheet() != style) remote_agent_->setStyleSheet(style);
}

void ConnectionProgressPage::set_snapshot(const ConnectionFlowSnapshot& snapshot) {
  for (int index = 0; index < kConnectionStageCount; ++index) {
    apply_stage_state(index, snapshot.stages[index]);
  }

  const int displayed_stage = snapshot.status == ConnectionFlowStatus::kComplete
      ? kConnectionStageCount
      : snapshot.current_stage + 1;
  if (snapshot.status == ConnectionFlowStatus::kComplete) {
    phase_->setText(role_ == ConnectionFlowRole::kHost
        ? "Stage 4 of 4 — Connected and sharing desktop"
        : "Stage 4 of 4 — Remote desktop ready");
  } else if (snapshot.status == ConnectionFlowStatus::kFailed) {
    phase_->setText(QString("Stage %1 of 4 — Failed").arg(displayed_stage));
  } else if (snapshot.status == ConnectionFlowStatus::kStopping) {
    phase_->setText("Closing the current connection…");
  } else if (snapshot.status == ConnectionFlowStatus::kRetryWaiting) {
    phase_->setText("Connection attempt failed — retrying automatically. No action required.");
  } else if (snapshot.status == ConnectionFlowStatus::kDisconnected) {
    phase_->setText(role_ == ConnectionFlowRole::kHost
        ? "Connection ended — waiting for a Controller."
        : "Connection interrupted — reconnecting automatically.");
  } else {
    phase_->setText(QString("Stage %1 of 4 — %2")
                        .arg(displayed_stage)
                        .arg(stages_[snapshot.current_stage].title->text()));
  }

  attempt_->setVisible(snapshot.attempt > 1);
  attempt_->setText(QString("Automatic attempt %1").arg(snapshot.attempt));
  failure_->setVisible(snapshot.status == ConnectionFlowStatus::kFailed);
  failure_->setText(snapshot.failure_detail.isEmpty()
      ? "The connection could not continue. Retry with the same code or exit."
      : snapshot.failure_detail);
  retry_->setVisible(snapshot.can_retry);
  retry_->setEnabled(snapshot.can_retry);
  exit_->setEnabled(snapshot.can_exit);
  exit_->setText(snapshot.status == ConnectionFlowStatus::kComplete ? "Disconnect" : "Exit");
}

void ConnectionProgressPage::set_on_retry(std::function<void()> callback) {
  on_retry_ = std::move(callback);
}

void ConnectionProgressPage::set_on_exit(std::function<void()> callback) {
  on_exit_ = std::move(callback);
}

int ConnectionProgressPage::stage_count() const {
  return kConnectionStageCount;
}

ConnectionStageState ConnectionProgressPage::stage_state(int index) const {
  if (index < 0 || index >= kConnectionStageCount) {
    return ConnectionStageState::kPending;
  }
  return stage_states_[index];
}

QString ConnectionProgressPage::phase_text() const {
  return phase_->text();
}

QPushButton* ConnectionProgressPage::retry_button() const {
  return retry_;
}

QPushButton* ConnectionProgressPage::exit_button() const {
  return exit_;
}

void ConnectionProgressPage::apply_stage_state(int index, ConnectionStageState state) {
  stage_states_[index] = state;
  stages_[index].state->setText(state_text(state));
  // setStyleSheet repolishes the whole stage subtree even for an identical
  // string. Periodic transport snapshots must not invalidate its layout.
  const auto frame_style = stage_style(state), label_style = state_style(state);
  if (stages_[index].frame->styleSheet() != frame_style) stages_[index].frame->setStyleSheet(frame_style);
  if (stages_[index].state->styleSheet() != label_style) stages_[index].state->setStyleSheet(label_style);
}

}  // namespace redclaw::ui
