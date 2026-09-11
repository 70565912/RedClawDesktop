#pragma once
#include <QPlainTextEdit>
#include <QTextDocument>

namespace redclaw::ui {
class RuntimeLogView : public QPlainTextEdit {
public:
    explicit RuntimeLogView(QWidget* parent = nullptr) : QPlainTextEdit(parent) {
        setReadOnly(true);
        // Keep the full line in the document. Wrapping limits paint work to
        // visible visual lines instead of drawing long off-screen glyph runs.
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        document()->setMaximumBlockCount(4096);
    }
};
}
