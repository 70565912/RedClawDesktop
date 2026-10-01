#include "runtime_log_view.h"

#include <QHideEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <algorithm>
#include "gui_latency_probe.h"

namespace redclaw::ui {

void RuntimeLogBuffer::append(const QString& text) {
    qsizetype begin = 0;
    do {
        const auto newline = text.indexOf('\n', begin);
        const auto end = newline < 0 ? text.size() : newline;
        // A single oversized line retains its most recent suffix in the UI.
        // Slice before allocating, so pathological input cannot grow the cache.
        auto start = std::max(begin, end - (kMaxBytes / 2 - 1));
        if (start < end && text[start].isLowSurrogate()) ++start;
        QString line = text.mid(start, end - start);
        if (line.endsWith('\r')) line.chop(1);
        const auto size = (line.size() + 1) * 2;
        while (!lines_.empty() && (line_count() >= kMaxLines || bytes_ + size > kMaxBytes)) {
            bytes_ -= (lines_.front().size() + 1) * 2;
            lines_.pop_front();
            ++first_line_;
        }
        bytes_ += size;
        lines_.push_back(std::move(line));
        if (newline < 0) break;
        begin = newline + 1;
    } while (begin <= text.size());
    for (auto* view : views_) view->schedule_refresh();
}

void RuntimeLogBuffer::clear() {
    first_line_ += lines_.size();
    lines_.clear();
    bytes_ = 0;
    for (auto* view : views_) view->reset_display();
}

QString RuntimeLogBuffer::text() const {
    QString result;
    result.reserve(bytes_ / 2);
    bool first = true;
    for (const auto& line : lines_) {
        if (!first) result += '\n';
        result += line;
        first = false;
    }
    return result;
}

RuntimeLogView::RuntimeLogView(QWidget* parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    document()->setMaximumBlockCount(static_cast<int>(RuntimeLogBuffer::kMaxLines));
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(kRefreshMs);
    QObject::connect(&timer_, &QTimer::timeout, this, &RuntimeLogView::flush_pending);
    set_buffer(std::make_shared<RuntimeLogBuffer>());
}

RuntimeLogView::~RuntimeLogView() {
    std::erase(buffer_->views_, this);
}

void RuntimeLogView::set_buffer(std::shared_ptr<RuntimeLogBuffer> buffer) {
    if (!buffer || buffer == buffer_) return;
    if (buffer_) std::erase(buffer_->views_, this);
    buffer_ = std::move(buffer);
    buffer_->views_.push_back(this);
    reset_display();
    schedule_refresh();
}

void RuntimeLogView::reset_display() {
    timer_.stop();
    QPlainTextEdit::clear();
    next_line_ = displayed_first_ = buffer_->first_line_;
    line_offset_ = 0;
    has_text_ = false;
}

void RuntimeLogView::schedule_refresh() {
    if (isVisible() && !timer_.isActive()) timer_.start();
}

void RuntimeLogView::showEvent(QShowEvent* event) {
    QPlainTextEdit::showEvent(event);
    schedule_refresh();
}

void RuntimeLogView::hideEvent(QHideEvent* event) {
    timer_.stop();
    QPlainTextEdit::hideEvent(event);
}

void RuntimeLogView::flush_pending() {
    timer_.stop();
    if (!isVisible()) return;
    const auto end = buffer_->first_line_ + buffer_->lines_.size();
    if (next_line_ == end && displayed_first_ == buffer_->first_line_) return;
    GuiLatencyScope timing(GuiStage::kLogWidget);
    const bool follow = verticalScrollBar()->value() >= verticalScrollBar()->maximum();
    const int old_scroll = verticalScrollBar()->value();
    const auto anchor = firstVisibleBlock();
    const int anchor_line = old_scroll - anchor.firstLineNumber();
    if (next_line_ < buffer_->first_line_ ||
        (next_line_ == buffer_->first_line_ && line_offset_ == 0)) {
        reset_display();
    } else if (displayed_first_ < buffer_->first_line_ && buffer_->line_count() < RuntimeLogBuffer::kMaxLines) {
        QTextCursor trim(document());
        const auto expired = buffer_->first_line_ - displayed_first_;
        trim.movePosition(QTextCursor::Start);
        trim.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, static_cast<int>(expired));
        trim.removeSelectedText();
        displayed_first_ = buffer_->first_line_;
    }
    const bool continuing_line = line_offset_ != 0;
    const bool separator_before_batch = has_text_ && !continuing_line;
    QString batch;
    qsizetype completed = 0;
    while (next_line_ < end && completed < kBatchLines) {
        const auto& line = buffer_->lines_[static_cast<std::size_t>(next_line_ - buffer_->first_line_)];
        const bool separator = line_offset_ == 0 && has_text_;
        const auto available = kBatchBytes / 2 - batch.size() - (separator ? 1 : 0);
        if (available <= 0) break;
        auto take = std::min(available, line.size() - line_offset_);
        if (take > 0 && line_offset_ + take < line.size() && line[line_offset_ + take - 1].isHighSurrogate()) --take;
        if (take == 0 && line_offset_ < line.size()) break;
        if (separator) batch += '\n';
        batch += QStringView(line).mid(line_offset_, take);
        has_text_ = true;
        line_offset_ += take;
        if (line_offset_ == line.size()) {
            ++next_line_;
            ++completed;
            line_offset_ = 0;
        }
    }
    if (!continuing_line && !(separator_before_batch && document()->isEmpty())) {
        // appendPlainText uses QPlainTextEdit's incremental layout path. A generic
        // cursor insertion re-lays out more of the document and increases the
        // dispatch tail as the bounded history fills.
        QPlainTextEdit::appendPlainText(separator_before_batch ? batch.mid(1) : batch);
    } else {
        QTextCursor cursor(document());
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(batch);
    }
    // For line-count eviction, let appendPlainText trim in the same layout pass.
    // Byte-limit eviction above still removes expired blocks explicitly.
    displayed_first_ = next_line_ + (line_offset_ != 0 ? 1 : 0)
        - static_cast<std::uint64_t>(document()->blockCount());
    if (follow) verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    else {
        // The scrollbar uses document layout lines, whose height need not equal
        // fontMetrics().lineSpacing(). Preserve the retained block and its
        // wrapped-line offset directly instead of rounding a pixel estimate.
        verticalScrollBar()->setValue(anchor.isValid()
            ? anchor.firstLineNumber() + anchor_line : old_scroll);
    }
    ++refresh_count_;
    if (next_line_ < end) schedule_refresh();
}

}
