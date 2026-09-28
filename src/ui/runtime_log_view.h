#pragma once
#include <QPlainTextEdit>
#include <QTimer>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace redclaw::ui {
class RuntimeLogView;

// GUI-thread owned text. The file sink remains independent and unabridged.
class RuntimeLogBuffer {
public:
    static constexpr qsizetype kMaxLines = 4096;
    // UTF-16 storage, including a separator per line; excludes container overhead.
    static constexpr qsizetype kMaxBytes = 4 * 1024 * 1024;
    void append(const QString& text);
    void clear();
    QString text() const;
    qsizetype bytes() const { return bytes_; }
    qsizetype line_count() const { return static_cast<qsizetype>(lines_.size()); }

private:
    friend class RuntimeLogView;
    std::deque<QString> lines_;
    std::vector<RuntimeLogView*> views_;
    qsizetype bytes_ = 0;
    std::uint64_t first_line_ = 0;
};

class RuntimeLogView : public QPlainTextEdit {
public:
    static constexpr int kRefreshMs = 100;
    static constexpr qsizetype kBatchLines = 64;
    static constexpr qsizetype kBatchBytes = 32 * 1024;
    explicit RuntimeLogView(QWidget* parent = nullptr);
    ~RuntimeLogView() override;
    void set_buffer(std::shared_ptr<RuntimeLogBuffer> buffer);
    const std::shared_ptr<RuntimeLogBuffer>& buffer() const { return buffer_; }
    void appendPlainText(const QString& text) { buffer_->append(text); }
    void clear_cached() { buffer_->clear(); }
    QString cached_text() const { return buffer_->text(); }
    // Also used by deterministic local validation; hidden views do no work.
    void flush_pending();
    std::uint64_t refresh_count() const { return refresh_count_; }

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    friend class RuntimeLogBuffer;
    void schedule_refresh();
    void reset_display();
    std::shared_ptr<RuntimeLogBuffer> buffer_;
    QTimer timer_;
    std::uint64_t next_line_ = 0;
    std::uint64_t displayed_first_ = 0;
    qsizetype line_offset_ = 0;
    bool has_text_ = false;
    std::uint64_t refresh_count_ = 0;
};
}
