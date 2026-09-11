#include "ui/connection_entry_page.h"

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace redclaw::ui {

namespace {

QString normalize_code(QString code) {
  code.remove(QRegularExpression("\\s"));
  return code.toUpper().left(8);
}

bool is_complete_code(const QString& code) {
  static const QRegularExpression expression("^[A-Z0-9]{8}$");
  return expression.match(code).hasMatch();
}

}  // namespace

void PageScrollComboBox::wheelEvent(QWheelEvent* event) {
  event->ignore();
}

ConnectionEntryPage::ConnectionEntryPage(QWidget* parent) : QWidget(parent) {
  setObjectName("preConnectionPage");
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(6, 6, 6, 6);
  root->setSpacing(10);

  hero_ = new QWidget(this);
  hero_->setObjectName("heroPanel");
  auto* hero_layout = new QVBoxLayout(hero_);
  hero_layout->setContentsMargins(20, 10, 20, 10);
  hero_layout->setSpacing(4);
  auto* accent = new QFrame(hero_);
  accent->setObjectName("accentStrip");
  auto* eyebrow = new QLabel("REDCLAW DESKTOP", hero_);
  eyebrow->setObjectName("heroEyebrow");
  hero_layout->addWidget(accent);
  hero_layout->addWidget(eyebrow);
  root->addWidget(hero_);

  auto* primary_group = new QGroupBox("Start a connection", this);
  primary_group->setObjectName("primaryCard");
  primary_card_ = primary_group;
  auto* card_layout = new QVBoxLayout(primary_group);
  card_layout->setContentsMargins(16, 16, 16, 14);
  card_layout->setSpacing(10);

  auto* code_grid = new QGridLayout();
  code_grid->setHorizontalSpacing(12);
  code_grid->setVerticalSpacing(6);

  auto* local_title_row = new QHBoxLayout();
  auto* local_title = new QLabel("Your connection code", primary_group);
  local_title->setObjectName("statusCardTitle");
  local_title_row->addWidget(local_title);
  local_title_row->addStretch();

  auto* local_card = new QFrame(primary_group);
  local_card->setObjectName("localCodeCard");
  local_card->setFixedHeight(52);
  auto* local_layout = new QHBoxLayout(local_card);
  local_layout->setContentsMargins(14, 8, 10, 8);
  local_code_ = new QLabel("Preparing…", local_card);
  local_code_->setObjectName("localCodeValue");
  local_code_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  local_code_->setFocusPolicy(Qt::NoFocus);
  auto* copy = new QPushButton("Copy", local_card);
  copy->setObjectName("secondaryAction");
  copy->setToolTip("Copy this device's connection code");
  local_layout->addWidget(local_code_, 1);
  local_layout->addWidget(copy);

  auto* peer_title = new QLabel("Peer connection code", primary_group);
  peer_title->setObjectName("statusCardTitle");
  peer_code_ = new QLineEdit(primary_group);
  peer_code_->setObjectName("peerCodeInput");
  peer_code_->setFixedHeight(52);
  peer_code_->setPlaceholderText("Enter the 8-character code");
  peer_code_->setClearButtonEnabled(true);
  peer_code_->setMaxLength(16);
  peer_helper_ = new QLabel(primary_group);
  peer_helper_->setObjectName("fieldHelper");
  peer_helper_->hide();

  auto* local_column = new QWidget(primary_group);
  auto* local_column_layout = new QVBoxLayout(local_column);
  local_column_layout->setContentsMargins(0, 0, 0, 0);
  local_column_layout->setSpacing(5);
  local_column_layout->addLayout(local_title_row);
  local_column_layout->addWidget(local_card);

  auto* peer_column = new QWidget(primary_group);
  auto* peer_column_layout = new QVBoxLayout(peer_column);
  peer_column_layout->setContentsMargins(0, 0, 0, 0);
  peer_column_layout->setSpacing(5);
  peer_column_layout->addWidget(peer_title);
  peer_column_layout->addWidget(peer_code_);
  peer_column_layout->addWidget(peer_helper_);

  code_grid->addWidget(local_column, 0, 0);
  code_grid->addWidget(peer_column, 0, 1);
  code_grid->setColumnStretch(0, 1);
  code_grid->setColumnStretch(1, 1);
  card_layout->addLayout(code_grid);

  allow_remote_control_ = new QCheckBox(
      "Allow the connected device to control this computer's keyboard and mouse",
      primary_group);
  allow_remote_control_->setObjectName("allowRemoteControlCheckbox");
  allow_remote_control_->setChecked(false);
  allow_remote_control_->setToolTip(
      "Applies only when this device waits as Host. Administrator windows, UAC, and Ctrl+Alt+Del are not included.");
  card_layout->addWidget(allow_remote_control_);
  allow_remote_agent_ = new QCheckBox(
      "When waiting as Host: allow the peer to use local development Agents",
      primary_group);
  allow_remote_agent_->setObjectName("allowRemoteAgentCheckbox");
  allow_remote_agent_->setChecked(false);
  allow_remote_agent_->setToolTip(
      "Only locally registered project IDs are exposed; remote paths are never accepted.");
  card_layout->addWidget(allow_remote_agent_);
  allow_controller_agent_ = new QCheckBox(
      "When connecting as Controller: allow the peer to use local development Agents",
      primary_group);
  allow_controller_agent_->setObjectName("allowControllerAgentCheckbox");
  allow_controller_agent_->setChecked(false);
  card_layout->addWidget(allow_controller_agent_);
  agent_settings_ = new QPushButton("Development Agent settings", primary_group);
  agent_settings_->setObjectName("agentSettingsButton");
  agent_settings_->setToolTip(
      "Manage local Agent login readiness and registered projects before starting either role.");
  card_layout->addWidget(agent_settings_, 0, Qt::AlignLeft);

  auto* action_row = new QHBoxLayout();
  action_row->setSpacing(10);
  wait_ = new QPushButton("Wait for connection", primary_group);
  wait_->setObjectName("primaryAction");
  wait_->setMinimumHeight(42);
  wait_->setEnabled(false);
  connect_ = new QPushButton("Connect", primary_group);
  connect_->setObjectName("connectAction");
  connect_->setMinimumHeight(42);
  connect_->setEnabled(false);
  action_row->addWidget(wait_, 1);
  action_row->addWidget(connect_, 1);
  card_layout->addLayout(action_row);

  status_ = new QLabel("Preparing your local code…", primary_group);
  status_->setObjectName("statusBanner");
  status_->setWordWrap(true);
  card_layout->addWidget(status_);
  root->addWidget(primary_group);

  network_toggle_ = new QToolButton(this);
  network_toggle_->setObjectName("networkSettingsToggle");
  network_toggle_->setCheckable(true);
  network_toggle_->setChecked(false);
  network_toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  network_toggle_->setArrowType(Qt::RightArrow);
  network_toggle_->setText("Connection settings");
  network_toggle_->setMaximumHeight(26);
  network_box_ = new QGroupBox(this);
  network_box_->setObjectName("secondaryCard");
  network_settings_layout_ = new QFormLayout(network_box_);
  network_settings_layout_->setContentsMargins(10, 8, 10, 18);
  network_settings_layout_->setHorizontalSpacing(10);
  network_settings_layout_->setVerticalSpacing(4);
  network_box_->hide();
  QObject::connect(network_toggle_, &QToolButton::toggled, this, [this](bool expanded) {
    network_box_->setVisible(expanded);
    network_toggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    network_toggle_->setText(expanded ? "Hide connection settings" : "Connection settings");
  });
  root->addWidget(network_toggle_);
  root->addWidget(network_box_);

  QObject::connect(copy, &QPushButton::clicked, this, [this]() {
    if (QClipboard* clipboard = QApplication::clipboard()) {
      clipboard->setText(local_code());
      set_status("Connection code copied.", "good");
    }
  });
  QObject::connect(peer_code_, &QLineEdit::textChanged, this, [this]() {
    normalize_peer_code();
    refresh_peer_validation();
  });
}

void ConnectionEntryPage::set_local_code(const QString& code) {
  local_code_->setText(normalize_code(code));
  wait_->setEnabled(is_complete_code(local_code_->text()));
}

void ConnectionEntryPage::set_peer_code(const QString& code) {
  peer_code_->setText(normalize_code(code));
}

void ConnectionEntryPage::set_status(const QString& text, const QString& tone) {
  QString style = "padding: 8px 12px; border-radius: 14px; border: 1px solid #31598a; background-color: rgba(37, 99, 235, 0.14); color: #dbeafe;";
  if (tone == "good") {
    style = "padding: 8px 12px; border-radius: 14px; border: 1px solid #10b981; background-color: rgba(6, 95, 70, 0.34); color: #d1fae5;";
  } else if (tone == "warn") {
    style = "padding: 8px 12px; border-radius: 14px; border: 1px solid #f59e0b; background-color: rgba(120, 53, 15, 0.34); color: #fde68a;";
  } else if (tone == "bad") {
    style = "padding: 8px 12px; border-radius: 14px; border: 1px solid #ef4444; background-color: rgba(127, 29, 29, 0.42); color: #fee2e2;";
  }
  status_->setText(text);
  if (status_->styleSheet() != style) status_->setStyleSheet(style);
}

void ConnectionEntryPage::set_actions_enabled(bool enabled) {
  peer_code_->setEnabled(enabled);
  wait_->setEnabled(enabled && is_complete_code(local_code()));
  connect_->setEnabled(enabled && is_complete_code(peer_code()));
  allow_remote_control_->setEnabled(enabled);
  allow_remote_agent_->setEnabled(enabled);
  allow_controller_agent_->setEnabled(enabled);
  agent_settings_->setEnabled(enabled);
}

QString ConnectionEntryPage::local_code() const {
  return local_code_->text().trimmed();
}

QString ConnectionEntryPage::peer_code() const {
  return peer_code_->text().trimmed();
}

QLabel* ConnectionEntryPage::local_code_display() const {
  return local_code_;
}

QLineEdit* ConnectionEntryPage::peer_code_input() const {
  return peer_code_;
}

QPushButton* ConnectionEntryPage::wait_button() const {
  return wait_;
}

QPushButton* ConnectionEntryPage::connect_button() const {
  return connect_;
}

QCheckBox* ConnectionEntryPage::allow_remote_control_checkbox() const {
  return allow_remote_control_;
}

QCheckBox* ConnectionEntryPage::allow_remote_agent_checkbox() const {
  return allow_remote_agent_;
}

QCheckBox* ConnectionEntryPage::allow_controller_agent_checkbox() const {
  return allow_controller_agent_;
}

QPushButton* ConnectionEntryPage::agent_settings_button() const {
  return agent_settings_;
}

QFormLayout* ConnectionEntryPage::network_settings_layout() const {
  return network_settings_layout_;
}

QToolButton* ConnectionEntryPage::network_settings_toggle() const {
  return network_toggle_;
}

QWidget* ConnectionEntryPage::network_settings_widget() const {
  return network_box_;
}

QWidget* ConnectionEntryPage::hero_panel() const {
  return hero_;
}

QWidget* ConnectionEntryPage::primary_card() const {
  return primary_card_;
}

void ConnectionEntryPage::normalize_peer_code() {
  const QString normalized = normalize_code(peer_code_->text());
  if (normalized == peer_code_->text()) {
    return;
  }
  const QSignalBlocker blocker(peer_code_);
  peer_code_->setText(normalized);
}

void ConnectionEntryPage::refresh_peer_validation() {
  const QString code = peer_code();
  const bool complete = is_complete_code(code);
  static const QRegularExpression invalid_character("[^A-Z0-9]");
  connect_->setEnabled(peer_code_->isEnabled() && complete);
  bool show_helper = false;
  if (code.isEmpty()) {
    peer_helper_->clear();
    peer_helper_->setProperty("invalid", false);
  } else if (code.contains(invalid_character)) {
    peer_helper_->setText("Use only letters A-Z and numbers 0-9.");
    peer_helper_->setProperty("invalid", true);
    show_helper = true;
  } else if (!complete) {
    peer_helper_->setText(QString("%1 of 8 characters entered.").arg(code.size()));
    peer_helper_->setProperty("invalid", true);
    show_helper = true;
  } else {
    peer_helper_->clear();
    peer_helper_->setProperty("invalid", false);
  }
  peer_helper_->setVisible(show_helper);
  peer_helper_->style()->unpolish(peer_helper_);
  peer_helper_->style()->polish(peer_helper_);
}

}  // namespace redclaw::ui
