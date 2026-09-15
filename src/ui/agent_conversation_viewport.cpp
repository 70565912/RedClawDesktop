#include "ui/agent_conversation_viewport.h"
#include "ui/gui_latency_probe.h"

#include <algorithm>
#include <set>
#include <utility>

#include <QEvent>
#include <QElapsedTimer>
#include <QLayout>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>

namespace redclaw::ui {

AgentConversationViewport::AgentConversationViewport(QWidget* parent)
    : QAbstractScrollArea(parent) {
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Stable width prevents scrollbar appearance from relaying out all text.
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    verticalScrollBar()->setSingleStep(32);
}

void AgentConversationViewport::clear_rows() {
    delete spare_;
    spare_ = nullptr;
    for (auto& [_, row] : rows_) delete row.widget;
    rows_.clear();
    ids_.clear();
    follow_tail_ = true;
    scan_offset_ = 0;
    scan_changed_ = false;
    verticalScrollBar()->setRange(0, 0);
    updateGeometry();
}

void AgentConversationViewport::set_rows(
    std::vector<std::uint64_t> ids, CreateRow create, UpdateRow update,
    CanReuseRow reuse, InitialHeight initial_height) {
    create_ = std::move(create);
    update_ = std::move(update);
    reuse_ = std::move(reuse);
    std::set<std::uint64_t> retained(ids.begin(), ids.end());
    for (auto it = rows_.begin(); it != rows_.end();) {
        if (!retained.contains(it->first)) {
            delete it->second.widget;
            it = rows_.erase(it);
        } else ++it;
    }
    ids_ = std::move(ids);
    for (std::size_t i = 0; i < ids_.size(); ++i) {
        auto [row, inserted] = rows_.try_emplace(ids_[i]);
        if (inserted && initial_height) row->second.height = std::max(24, initial_height(i));
    }
    scan_offset_ = 0;
    scan_changed_ = false;
    updateGeometry();
    schedule_refresh();
}

void AgentConversationViewport::schedule_refresh() {
    if (queued_ || refreshing_) return;
    queued_ = true;
    QTimer::singleShot(16, this, [this] { queued_ = false; refresh(); });
}

void AgentConversationViewport::invalidate_row(std::uint64_t id) {
    const auto found = rows_.find(id);
    if (found == rows_.end()) return;
    // Expansion changes geometry without changing the message text or width.
    found->second.width = -1;
    scan_offset_ = 0;
    schedule_refresh();
}

void AgentConversationViewport::follow_new_content() {
    follow_tail_ = true;
    scan_offset_ = 0;
    schedule_refresh();
}

void AgentConversationViewport::set_content_size_changed_callback(
    ContentSizeChanged callback) {
    content_size_changed_ = std::move(callback);
}

int AgentConversationViewport::content_height_hint() const noexcept {
    int total = 8;
    for (const auto id : ids_) {
        const auto found = rows_.find(id);
        if (found == rows_.end()) continue;
        total += std::max(24, found->second.height) + 8;
    }
    return total;
}

QSize AgentConversationViewport::sizeHint() const {
    QSize hint = QAbstractScrollArea::sizeHint();
    // Keep the parent panel's preferred height tied to the retained rows. The
    // outer window still clamps this value to the available screen height, so
    // long histories remain scrollable instead of forcing an off-screen window.
    hint.setHeight(std::clamp(content_height_hint(), 48, 720));
    return hint;
}

void AgentConversationViewport::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    scan_offset_ = 0;
    schedule_refresh();
}

void AgentConversationViewport::scrollContentsBy(int, int) {
    if (refreshing_) return;
    // A short scroll range can be less than one text row. Scrolling to its top
    // is still an explicit history request, not permission to snap to the tail.
    follow_tail_ = verticalScrollBar()->value() == verticalScrollBar()->maximum();
    scan_offset_ = 0;
    schedule_refresh();
}

void AgentConversationViewport::refresh() {
    GuiLatencyScope timing(GuiStage::kAgentLayout);
    if (refreshing_ || !create_) return;
    QElapsedTimer timer;
    timer.start();
    refreshing_ = true;
    const int width = std::max(80, viewport()->width() - 8);
    const int page = std::max(1, viewport()->height());
    auto* bar = verticalScrollBar();
    const QSignalBlocker blocker(bar);
    // Continue the same scan after a yield: repeatedly starting at the same
    // expensive visible row can starve the rest of the view indefinitely.
    {
        int total = 8;
        for (auto id : ids_) total += rows_.at(id).height + 8;
        bar->setPageStep(page);
        bar->setRange(0, std::max(0, total - page));
        if (follow_tail_) bar->setValue(bar->maximum());
        std::vector<int> positions;
        positions.reserve(ids_.size());
        int next_y = 4 - bar->value();
        for (auto id : ids_) {
            positions.push_back(next_y);
            next_y += rows_.at(id).height + 8;
        }
        // Retire off-screen widgets before creating another. Retain only one
        // reusable widget (not a message/frame queue), avoiding native text-view
        // allocation/destruction on every streaming chunk.
        for (std::size_t i = 0; i < ids_.size(); ++i) {
            auto& row = rows_.at(ids_[i]);
            if (row.widget && (positions[i] + row.height < 0 || positions[i] > page)) {
                delete spare_;
                spare_ = row.widget;
                row.widget = nullptr;
            }
        }
        for (; scan_offset_ < ids_.size(); ++scan_offset_) {
            if (timer.elapsed() >= 8) {
                break;
            }
            // While following output, create the newest visible row first. A
            // soft budget must not repeatedly start with old rows and starve
            // the tail (or shift later rows using half-updated height estimates).
            const auto i = follow_tail_ ? ids_.size() - 1 - scan_offset_ : scan_offset_;
            const int y = positions[i];
            auto& row = rows_.at(ids_[i]);
            if (y + row.height >= 0 && y <= page) {
                bool needs_layout = !row.widget || row.width != width;
                if (!row.widget) {
                    if (spare_ && reuse_ && reuse_(i, spare_)) {
                        row.widget = std::exchange(spare_, nullptr);
                        if (update_) update_(i, row.widget);
                    } else row.widget = create_(i, viewport());
                } else if (update_) needs_layout |= update_(i, row.widget);
                if (needs_layout) {
                    GuiLatencyScope row_timing(GuiStage::kAgentRowLayout);
                    row.widget->resize(width, row.height);
                    if (auto* layout = row.widget->layout()) layout->activate();
                    int height = row.widget->sizeHint().height();
                    if (row.widget->hasHeightForWidth()) height = row.widget->heightForWidth(width);
                    height = std::max(24, height);
                    scan_changed_ |= height != row.height;
                    row.height = height;
                    row.width = width;
                }
                const QRect geometry(4, y, width, row.height);
                if (row.widget->geometry() != geometry) row.widget->setGeometry(geometry);
                if (row.widget->isHidden()) row.widget->show();
            }
        }
    }
    refreshing_ = false;
    if (spare_) spare_->hide();
    const bool content_size_changed = scan_changed_;
    const bool more = scan_offset_ < ids_.size() || scan_changed_;
    if (scan_offset_ == ids_.size()) {
        scan_offset_ = 0;
        scan_changed_ = false;
    }
    if (content_size_changed) {
        updateGeometry();
        if (content_size_changed_) content_size_changed_();
    }
    if (more) schedule_refresh();
    setProperty("refresh_count", property("refresh_count").toULongLong() + 1);
    setProperty("max_refresh_ms", std::max(property("max_refresh_ms").toLongLong(), timer.elapsed()));
}

}  // namespace redclaw::ui
