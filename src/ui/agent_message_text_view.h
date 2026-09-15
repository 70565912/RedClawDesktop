#pragma once

#include <algorithm>

#include <QAbstractTextDocumentLayout>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStackedLayout>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include "ui/gui_latency_probe.h"

namespace redclaw::ui {

// Rich tables use QTextBrowser only once they settle and remain small. Streaming
// and large output use Qt's incremental plain-text layout, not the rich layout
// engine with plain text merely inserted into it. Both preserve selectable text.
class AgentMessageTextView final : public QWidget {
public:
    explicit AgentMessageTextView(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName("agentReplyText");
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        layout_ = new QStackedLayout(this);
        layout_->setContentsMargins(0, 0, 0, 0);
        plain_ = new QPlainTextEdit(this);
        plain_->setObjectName("agentReplyPlain");
        plain_->setReadOnly(true);
        plain_->setUndoRedoEnabled(false);
        plain_->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        plain_->setFrameShape(QFrame::NoFrame);
        plain_->document()->setDocumentMargin(0);
        plain_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        layout_->addWidget(plain_);
    }

    void set_markdown(const QString& text, bool streaming, bool follow_tail = false) {
        GuiLatencyScope timing(GuiStage::kAgentText);
        const bool use_plain = streaming || text.size() > 4096 || text.count('\n') > 64;
        if (text_ == text && use_plain_ == use_plain && initialized_) return;
        if (use_plain) {
            const auto wrap = text.size() > 1024 && !text.contains(' ') && !text.contains('\n')
                ? QTextOption::WrapAnywhere : QTextOption::WrapAtWordBoundaryOrAnywhere;
            if (plain_->wordWrapMode() != wrap) plain_->setWordWrapMode(wrap);
            if (use_plain_ && initialized_ && text.startsWith(text_)) {
                QTextCursor cursor(plain_->document());
                cursor.movePosition(QTextCursor::End);
                cursor.insertText(text.mid(text_.size()));
            } else plain_->setPlainText(text);
            layout_->setCurrentWidget(plain_);
        } else {
            if (!rich_) {
                rich_ = new QTextBrowser(this);
                rich_->setObjectName("agentReplyMarkdown");
                rich_->setOpenExternalLinks(false);
                rich_->setOpenLinks(false);
                rich_->setUndoRedoEnabled(false);
                rich_->setFrameShape(QFrame::NoFrame);
                rich_->document()->setDocumentMargin(0);
                rich_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
                layout_->addWidget(rich_);
            }
            rich_->setMarkdown(text);
            layout_->setCurrentWidget(rich_);
        }
        text_ = text;
        use_plain_ = use_plain;
        initialized_ = true;
        update_height();
        // Coalesce streaming chunks and wait for the new document/viewport
        // geometry. This is requested only by changed content, so a quiet
        // conversation remains available for manual history browsing.
        if (follow_tail && !tail_queued_) {
            tail_queued_ = true;
            QTimer::singleShot(0, this, [this] {
                tail_queued_ = false;
                if (use_plain_) {
                    plain_->moveCursor(QTextCursor::End);
                    plain_->ensureCursorVisible();
                    plain_->verticalScrollBar()->setValue(plain_->verticalScrollBar()->maximum());
                } else if (rich_) {
                    rich_->moveCursor(QTextCursor::End);
                    rich_->ensureCursorVisible();
                    rich_->verticalScrollBar()->setValue(rich_->verticalScrollBar()->maximum());
                }
            });
        }
    }

    QString toPlainText() const {
        return use_plain_ || !rich_ ? plain_->toPlainText() : rich_->toPlainText();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        update_height();
    }

private:
    void update_height() {
        int target = 24;
        if (use_plain_) {
            if (text_.size() > 4096 || text_.count('\n') > 64) {
                // A newly created plain document initially reports one estimated
                // line before its viewport has been laid out. Treating that as
                // its final height creates dozens of tiny scrollable text views
                // and repaints all of them. Large bodies use the existing bounded
                // message viewport, with their full document available to scroll.
                target = 720;
            } else {
                const auto lines = plain_->document()->documentLayout()->documentSize().height();
                target = static_cast<int>(std::min<qreal>(40, lines) * plain_->fontMetrics().lineSpacing()) + 8;
            }
        } else if (rich_) {
            const int width = std::max(80, rich_->viewport()->width());
            if (rich_->document()->textWidth() != width) rich_->document()->setTextWidth(width);
            target = static_cast<int>(rich_->document()->size().height()) + 8;
        }
        const int height = std::clamp(target, 24, 720);
        if (minimumHeight() != height || maximumHeight() != height) setFixedHeight(height);
    }
    QStackedLayout* layout_ = nullptr;
    QPlainTextEdit* plain_ = nullptr;
    QTextBrowser* rich_ = nullptr;
    QString text_;
    bool initialized_ = false;
    bool use_plain_ = true;
    bool tail_queued_ = false;
};

}  // namespace redclaw::ui
