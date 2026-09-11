#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include <QAbstractScrollArea>

namespace redclaw::ui {

// Only visible message widgets exist. The owner retains bounded conversation
// data; this viewport retains heights/IDs, not another copy of output text.
class AgentConversationViewport final : public QAbstractScrollArea {
public:
    using CreateRow = std::function<QWidget*(std::size_t, QWidget*)>;
    // Returns whether content changed and therefore needs remeasurement.
    using UpdateRow = std::function<bool(std::size_t, QWidget*)>;
    using CanReuseRow = std::function<bool(std::size_t, QWidget*)>;
    using InitialHeight = std::function<int(std::size_t)>;
    explicit AgentConversationViewport(QWidget* parent = nullptr);
    void set_rows(std::vector<std::uint64_t> ids, CreateRow create, UpdateRow update,
                  CanReuseRow reuse = {}, InitialHeight initial_height = {});
    void clear_rows();
protected:
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
private:
    struct Row { int height = 32; int width = -1; QWidget* widget = nullptr; };
    void schedule_refresh();
    void refresh();
    std::vector<std::uint64_t> ids_;
    std::map<std::uint64_t, Row> rows_;
    CreateRow create_;
    UpdateRow update_;
    CanReuseRow reuse_;
    QWidget* spare_ = nullptr;
    bool queued_ = false;
    bool refreshing_ = false;
    bool follow_tail_ = true;
    std::size_t scan_offset_ = 0;
    bool scan_changed_ = false;
};

}  // namespace redclaw::ui
