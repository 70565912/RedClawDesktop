#pragma once

#include <QComboBox>
#include <QWidget>

class QFormLayout;
class QFrame;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QWheelEvent;

namespace redclaw::ui {

class PageScrollComboBox final : public QComboBox {
 public:
  using QComboBox::QComboBox;

 protected:
  void wheelEvent(QWheelEvent* event) override;
};

class ConnectionEntryPage final : public QWidget {
 public:
  explicit ConnectionEntryPage(QWidget* parent = nullptr);

  void set_local_code(const QString& code);
  void set_peer_code(const QString& code);
  void set_status(const QString& text, const QString& tone);
  void set_actions_enabled(bool enabled);

  [[nodiscard]] QString local_code() const;
  [[nodiscard]] QString peer_code() const;
  [[nodiscard]] QLabel* local_code_display() const;
  [[nodiscard]] QLineEdit* peer_code_input() const;
  [[nodiscard]] QPushButton* wait_button() const;
  [[nodiscard]] QPushButton* connect_button() const;
  [[nodiscard]] QCheckBox* allow_remote_control_checkbox() const;
  [[nodiscard]] QCheckBox* allow_remote_agent_checkbox() const;
  [[nodiscard]] QCheckBox* allow_controller_agent_checkbox() const;
  [[nodiscard]] QPushButton* agent_settings_button() const;
  [[nodiscard]] QFormLayout* network_settings_layout() const;
  [[nodiscard]] QToolButton* network_settings_toggle() const;
  [[nodiscard]] QWidget* network_settings_widget() const;
  [[nodiscard]] QWidget* hero_panel() const;
  [[nodiscard]] QWidget* primary_card() const;

 private:
  void normalize_peer_code();
  void refresh_peer_validation();

  QWidget* hero_ = nullptr;
  QWidget* primary_card_ = nullptr;
  QLabel* local_code_ = nullptr;
  QLabel* peer_helper_ = nullptr;
  QLabel* status_ = nullptr;
  QLineEdit* peer_code_ = nullptr;
  QPushButton* wait_ = nullptr;
  QPushButton* connect_ = nullptr;
  QCheckBox* allow_remote_control_ = nullptr;
  QCheckBox* allow_remote_agent_ = nullptr;
  QCheckBox* allow_controller_agent_ = nullptr;
  QPushButton* agent_settings_ = nullptr;
  QToolButton* network_toggle_ = nullptr;
  QWidget* network_box_ = nullptr;
  QFormLayout* network_settings_layout_ = nullptr;
};

}  // namespace redclaw::ui
