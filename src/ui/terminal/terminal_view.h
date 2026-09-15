#pragma once

#include <QWidget>
#include <QString>
#include <functional>
#include <memory>
#include <string_view>

namespace redclaw::ui {

// GUI-thread owner. A single outstanding output chunk couples xterm parsing to
// transport backpressure; collapsing the widget does not reset its terminal.
class TerminalView final : public QWidget {
public:
    explicit TerminalView(QWidget* parent = nullptr);
    ~TerminalView() override;
    void initialize(const QString& fixed_runtime_directory, const QString& user_data_directory);
    void reset_session(const QString& epoch);
    void set_input_enabled(bool enabled);
    bool append_output(std::string_view bytes);
    bool ready() const;
    bool output_pending() const;
    void set_input_callback(std::function<void(const QByteArray&)> callback);
    void set_resize_callback(std::function<void(int, int)> callback);
    void set_writable_callback(std::function<void()> callback);
    void set_error_callback(std::function<void(const QString&)> callback);
protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
