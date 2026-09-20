#include "ui/desktop_task_workspace.h"

#include <QApplication>
#include <QCloseEvent>
#include <QHideEvent>
#include <QLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <vector>

namespace redclaw::ui {
namespace {
constexpr auto kSettings = "controller/floating_workspace/v1/";
QString task_key(DesktopTask id) {
    switch (id) {
    case DesktopTask::kAgent: return "agent";
    case DesktopTask::kTerminal: return "terminal";
    case DesktopTask::kNavigation: return "navigation";
    case DesktopTask::kFiles: return "files";
    }
    return {};
}
QRect bounded_rect(QRect rect, const QRect& bounds) {
    rect.setSize(rect.size().expandedTo(QSize(1, 1)).boundedTo(bounds.size().expandedTo(QSize(1, 1))));
    rect.moveLeft(std::clamp(rect.x(), bounds.left(), bounds.right() - rect.width() + 1));
    rect.moveTop(std::clamp(rect.y(), bounds.top(), bounds.bottom() - rect.height() + 1));
    return rect;
}

// Owned native tool windows avoid the D3D11/WebView native-child stacking gap.
// Geometry remains strictly inside the owner's client rectangle, not the screen.
class FloatingTaskWindow final : public QWidget {
public:
    FloatingTaskWindow(QWidget* owner, bool translucent)
        : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
          translucent_(translucent) {
        setAttribute(Qt::WA_DeleteOnClose, false);
        setAttribute(Qt::WA_ShowWithoutActivating);
        if (translucent) setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);
    }
    std::function<void()> dismissed, interacted, geometry_committed;
    void set_bounds(QRect bounds) {
        if (bounds != bounds_) stop_drag();
        bounds_ = bounds;
    }
    void set_drag_handle(QWidget* handle) { handle->installEventFilter(this); handle->setCursor(Qt::SizeAllCursor); }
    [[nodiscard]] bool dragging() const { return moving_ || edges_ != Qt::Edges{}; }
protected:
    void hideEvent(QHideEvent* event) override { stop_drag(); QWidget::hideEvent(event); }
    void closeEvent(QCloseEvent* event) override {
        event->ignore();
        if (interacted) interacted();
        if (dismissed) dismissed();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QColor(73, 104, 136));
        painter.setBrush(QColor(9, 20, 35, translucent_ ? 204 : 255));
        painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 7, 7);
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove
            || event->type() == QEvent::MouseButtonRelease) {
            return mouse(static_cast<QMouseEvent*>(event), qobject_cast<QWidget*>(watched), true);
        }
        return QWidget::eventFilter(watched, event);
    }
    void mousePressEvent(QMouseEvent* event) override { (void)mouse(event, this, false); }
    void mouseMoveEvent(QMouseEvent* event) override { (void)mouse(event, this, false); }
    void mouseReleaseEvent(QMouseEvent* event) override { (void)mouse(event, this, false); }
private:
    void stop_drag() {
        moving_ = false; edges_ = {};
        if (grabber_) grabber_->releaseMouse();
        grabber_.clear();
    }
    Qt::Edges hit_edges(QPoint point) const {
        Qt::Edges edges;
        if (translucent_) return edges;
        if (point.x() < 6) edges |= Qt::LeftEdge;
        if (point.x() >= width() - 6) edges |= Qt::RightEdge;
        if (point.y() < 6) edges |= Qt::TopEdge;
        if (point.y() >= height() - 6) edges |= Qt::BottomEdge;
        return edges;
    }
    bool mouse(QMouseEvent* event, QWidget* target, bool handle) {
        const auto point = event->globalPosition().toPoint();
        if (event->type() == QEvent::MouseButtonPress && event->button() == Qt::LeftButton) {
            if (interacted) interacted();
            edges_ = handle ? Qt::Edges{} : hit_edges(mapFromGlobal(point));
            moving_ = handle;
            if (!dragging()) return false;
            origin_ = point; original_ = geometry(); grabber_ = target;
            target->grabMouse();
            return true;
        }
        if (event->type() == QEvent::MouseMove && dragging()) {
            const auto delta = point - origin_;
            QRect next = original_;
            if (moving_) next.translate(delta);
            else {
                const QSize minimum = QSize(240, 140).boundedTo(bounds_.size());
                if (edges_.testFlag(Qt::LeftEdge)) next.setLeft(std::clamp(original_.left() + delta.x(), bounds_.left(), original_.right() - minimum.width() + 1));
                if (edges_.testFlag(Qt::RightEdge)) next.setRight(std::clamp(original_.right() + delta.x(), original_.left() + minimum.width() - 1, bounds_.right()));
                if (edges_.testFlag(Qt::TopEdge)) next.setTop(std::clamp(original_.top() + delta.y(), bounds_.top(), original_.bottom() - minimum.height() + 1));
                if (edges_.testFlag(Qt::BottomEdge)) next.setBottom(std::clamp(original_.bottom() + delta.y(), original_.top() + minimum.height() - 1, bounds_.bottom()));
            }
            setGeometry(bounded_rect(next, bounds_));
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && event->button() == Qt::LeftButton && dragging()) {
            stop_drag();
            if (geometry_committed) geometry_committed();
            return true;
        }
        if (!handle && !dragging()) {
            const auto edges = hit_edges(mapFromGlobal(point));
            const bool horizontal = edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge);
            const bool vertical = edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge);
            setCursor(horizontal && vertical
                ? ((edges.testFlag(Qt::LeftEdge) == edges.testFlag(Qt::TopEdge)) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor)
                : horizontal ? Qt::SizeHorCursor : vertical ? Qt::SizeVerCursor : Qt::ArrowCursor);
        }
        return false;
    }
    bool translucent_, moving_ = false;
    Qt::Edges edges_;
    QRect bounds_, original_;
    QPoint origin_;
    QPointer<QWidget> grabber_;
};

class TaskStatusLabel final : public QLabel {
public:
    explicit TaskStatusLabel(QWidget* parent) : QLabel(parent) {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0); setMaximumWidth(150);
    }
    QSize sizeHint() const override { return {150, 26}; }
protected:
    void paintEvent(QPaintEvent*) override {
        setToolTip(text());
        QPainter painter(this); painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(contentsRect(), Qt::AlignVCenter | Qt::AlignLeft,
            fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};

class TaskBarLayout final : public QLayout {
public:
    explicit TaskBarLayout(QWidget* parent) : QLayout(parent) { setContentsMargins(8, 6, 8, 6); setSpacing(6); }
    ~TaskBarLayout() override { while (auto* item = takeAt(0)) delete item; }
    void addItem(QLayoutItem* item) override { items_.push_back(item); invalidate(); }
    int count() const override { return static_cast<int>(items_.size()); }
    QLayoutItem* itemAt(int index) const override { return index >= 0 && index < count() ? items_[index] : nullptr; }
    QLayoutItem* takeAt(int index) override {
        auto* item = itemAt(index); if (item) items_.erase(items_.begin() + index); return item;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize minimumSize() const override { return {1, 1}; }
    QSize sizeHint() const override {
        int width = 16;
        for (auto* item : items_) if (!item->isEmpty()) width += item->sizeHint().width() + spacing();
        return {width, heightForWidth(width)};
    }
    void setGeometry(const QRect& rect) override { QLayout::setGeometry(rect); (void)arrange(rect, true); }
private:
    int arrange(const QRect& rect, bool apply) const {
        const QRect area = rect.marginsRemoved(contentsMargins());
        int x = area.left(), y = area.top(), row_height = 0;
        for (auto* item : items_) {
            if (item->isEmpty()) continue;
            const QSize size(std::min(std::max(1, area.width()), item->sizeHint().width()), std::max(28, item->sizeHint().height()));
            if (x > area.left() && x + size.width() > area.right() + 1) { x = area.left(); y += row_height + spacing(); row_height = 0; }
            if (apply) item->setGeometry(QRect(QPoint(x, y), size));
            x += size.width() + spacing(); row_height = std::max(row_height, size.height());
        }
        return y + row_height - rect.top() + contentsMargins().bottom();
    }
    std::vector<QLayoutItem*> items_;
};
} // namespace

struct DesktopTaskWorkspace::Impl {
    struct Task {
        DesktopTask id;
        std::unique_ptr<FloatingTaskWindow> window;
        QToolButton* button = nullptr;
        QRect preferred;
        QSize initial;
        bool visible = false;
    };
    QPointer<QWidget> owner;
    QPointer<QSettings> settings;
    std::unique_ptr<FloatingTaskWindow> bar;
    TaskBarLayout* bar_layout;
    std::vector<Task> tasks;
    QPushButton *control, *retry;
    QLabel *control_state, *connection_state;
    QPoint bar_position;
    bool has_bar_position = false, enabled = true, syncing = false;
    std::function<void()> local_interaction;

    Impl(QWidget* window, QSettings* store) : owner(window), settings(store), bar(std::make_unique<FloatingTaskWindow>(window, true)) {
        bar->setObjectName("desktopTaskButtonBar");
        // Override the application's opaque QWidget rule, without fading text.
        bar->setStyleSheet(
            "QWidget#desktopTaskButtonBar, QLabel { background: transparent; color: #e2e8f0; }"
            "QToolButton { padding: 5px 7px; border: 1px solid transparent; border-radius: 5px; color: #e2e8f0; background: transparent; }"
            "QToolButton:checked { background: #234766; border-color: #6198c2; }"
            "QToolButton:hover { background: #24415b; }");
        bar_layout = new TaskBarLayout(bar.get());
        bar_layout->setSizeConstraint(QLayout::SetNoConstraint);
        bar_layout->setContentsMargins(8, 6, 8, 6); bar_layout->setSpacing(6);
        auto* grip = new QLabel(QString::fromUtf8("⠿"), bar.get());
        grip->setObjectName("desktopTaskBarGrip"); grip->setToolTip(QString::fromUtf8("拖动任务按钮条"));
        grip->setFixedSize(20, 28); bar->set_drag_handle(grip); bar_layout->addWidget(grip);
        control = new QPushButton("Start Control", bar.get()); control->setObjectName("remoteControlButton"); control->setEnabled(false);
        retry = new QPushButton(QString::fromUtf8("重试画面"), bar.get()); retry->setObjectName("retryCaptureButton"); retry->hide();
        control_state = new TaskStatusLabel(bar.get()); control_state->setObjectName("remoteControlStatus");
        connection_state = new TaskStatusLabel(bar.get()); connection_state->setObjectName("playbackWindowStatus");
        for (auto* widget : std::vector<QWidget*>{control, retry, control_state, connection_state}) bar_layout->addWidget(widget);
        bar->interacted = [this] { interact(); };
        bar->geometry_committed = [this] {
            bar_position = bar->pos() - bounds().topLeft(); has_bar_position = true;
            if (settings) settings->setValue(QString(kSettings) + "bar_position", bar_position);
        };
        if (settings && settings->contains(QString(kSettings) + "bar_position")) {
            has_bar_position = true; bar_position = settings->value(QString(kSettings) + "bar_position").toPoint();
        }
    }
    QRect bounds() const { return owner ? QRect(owner->mapToGlobal(owner->contentsRect().topLeft()), owner->contentsRect().size()) : QRect{}; }
    Task* find(DesktopTask id) {
        const auto it = std::find_if(tasks.begin(), tasks.end(), [id](const auto& task) { return task.id == id; });
        return it == tasks.end() ? nullptr : &*it;
    }
    void interact() { if (local_interaction) local_interaction(); }
    bool available() const { return enabled && owner && owner->isVisible() && !owner->isMinimized(); }
    void arrange_bar(const QRect& area) {
        const int width = std::min({920, area.width(), bar_layout->sizeHint().width()});
        const QSize size(width, std::min(area.height(), bar_layout->heightForWidth(width)));
        const QPoint position = has_bar_position ? bar_position : QPoint((area.width() - size.width()) / 2, 12);
        bar->set_bounds(area);
        if (!bar->dragging()) bar->setGeometry(bounded_rect(QRect(area.topLeft() + position, size), area));
    }
    void sync() {
        if (syncing || !owner) return;
        syncing = true;
        const QRect area = bounds();
        if (area.isEmpty()) { syncing = false; return; }
        arrange_bar(area);
        for (auto& task : tasks) {
            QRect target = task.preferred;
            if (!target.isValid()) {
                const auto size = task.initial.boundedTo(area.size());
                target = QRect(QPoint((area.width() - size.width()) / 2, (area.height() - size.height()) / 2), size);
                if (available() && task.visible) {
                    task.preferred = target;
                    if (settings) settings->setValue(QString(kSettings) + task_key(task.id) + "/geometry", target);
                }
            }
            task.window->set_bounds(area);
            if (!task.window->dragging()) task.window->setGeometry(bounded_rect(target.translated(area.topLeft()), area));
            task.window->setVisible(available() && task.visible);
        }
        bar->setVisible(available());
        if (bar->isVisible()) bar->raise();
        syncing = false;
    }
};

DesktopTaskWorkspace::DesktopTaskWorkspace(QWidget* playback_window, QSettings* settings)
    : QObject(playback_window), impl_(std::make_unique<Impl>(playback_window, settings)) {
    qApp->installEventFilter(this);
}
DesktopTaskWorkspace::~DesktopTaskWorkspace() { qApp->removeEventFilter(this); }
void DesktopTaskWorkspace::add_task(DesktopTask id, const QString& title, QWidget* content, QSize initial_size) {
    Q_ASSERT(!impl_->find(id));
    if (impl_->find(id) || !content) return;
    Impl::Task task; task.id = id; task.initial = initial_size;
    task.window = std::make_unique<FloatingTaskWindow>(impl_->owner, false);
    task.window->setObjectName("desktopTaskWindow_" + task_key(id)); task.window->setWindowTitle(title);
    auto* layout = new QVBoxLayout(task.window.get()); layout->setContentsMargins(6, 6, 6, 6); layout->setSpacing(4);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    auto* header = new QWidget(task.window.get()); header->setObjectName("desktopTaskTitleBar"); header->setFixedHeight(30);
    auto* row = new QHBoxLayout(header); row->setContentsMargins(4, 0, 0, 0);
    auto* label = new QLabel(title, header); row->addWidget(label, 1);
    auto* close = new QToolButton(header); close->setObjectName("desktopTaskClose"); close->setText(QString::fromUtf8("×"));
    close->setToolTip(QString::fromUtf8("隐藏窗口（任务继续运行）")); row->addWidget(close); layout->addWidget(header);
    task.window->set_drag_handle(header); task.window->set_drag_handle(label);
    auto* scroll = new QScrollArea(task.window.get()); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true);
    scroll->setMinimumSize(0, 0);
    auto* holder = new QWidget(scroll); auto* contents = new QVBoxLayout(holder); contents->setContentsMargins(0, 0, 0, 0); contents->addWidget(content);
    scroll->setWidget(holder); layout->addWidget(scroll, 1);
    task.button = new QToolButton(impl_->bar.get()); task.button->setObjectName("desktopTaskButton_" + task_key(id));
    task.button->setText(title); task.button->setCheckable(true); task.button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    // Insert tasks before the controls/status tail, preserving registration order.
    std::vector<QLayoutItem*> tail;
    for (int i = 0; i < 4; ++i) tail.push_back(impl_->bar_layout->takeAt(impl_->bar_layout->count() - 4 + i));
    impl_->bar_layout->addWidget(task.button);
    for (auto* item : tail) impl_->bar_layout->addItem(item);
    if (impl_->settings) {
        const auto prefix = QString(kSettings) + task_key(id) + '/';
        task.visible = impl_->settings->value(prefix + "visible", false).toBool();
        task.preferred = impl_->settings->value(prefix + "geometry").toRect();
    }
    task.button->setChecked(task.visible);
    connect(task.button, &QToolButton::toggled, this, [this, id](bool visible) { set_task_visible(id, visible); });
    connect(close, &QToolButton::clicked, this, [this, id] { set_task_visible(id, false); });
    task.window->dismissed = [this, id] { set_task_visible(id, false); };
    task.window->interacted = [this] { impl_->interact(); impl_->bar->raise(); };
    task.window->geometry_committed = [this, id] {
        auto* entry = impl_->find(id); if (!entry) return;
        entry->preferred = entry->window->geometry().translated(-impl_->bounds().topLeft());
        if (impl_->settings) impl_->settings->setValue(QString(kSettings) + task_key(id) + "/geometry", entry->preferred);
    };
    impl_->tasks.push_back(std::move(task)); impl_->sync();
}
void DesktopTaskWorkspace::set_task_visible(DesktopTask id, bool visible) {
    auto* task = impl_->find(id); if (!task) return;
    impl_->interact(); task->visible = visible;
    { const QSignalBlocker blocker(task->button); task->button->setChecked(visible); }
    if (impl_->settings) impl_->settings->setValue(QString(kSettings) + task_key(id) + "/visible", visible);
    impl_->sync();
    if (visible && impl_->available()) { task->window->raise(); task->window->activateWindow(); impl_->bar->raise(); }
}
bool DesktopTaskWorkspace::task_visible(DesktopTask id) const { auto* task = impl_->find(id); return task && task->visible; }
QWidget* DesktopTaskWorkspace::task_window(DesktopTask id) const { auto* task = impl_->find(id); return task ? task->window.get() : nullptr; }
QToolButton* DesktopTaskWorkspace::task_button(DesktopTask id) const { auto* task = impl_->find(id); return task ? task->button : nullptr; }
QWidget* DesktopTaskWorkspace::button_bar() const { return impl_->bar.get(); }
QPushButton* DesktopTaskWorkspace::control_button() const { return impl_->control; }
QPushButton* DesktopTaskWorkspace::retry_button() const { return impl_->retry; }
QLabel* DesktopTaskWorkspace::control_status() const { return impl_->control_state; }
QLabel* DesktopTaskWorkspace::connection_status() const { return impl_->connection_state; }
void DesktopTaskWorkspace::set_enabled(bool enabled) { impl_->enabled = enabled; impl_->sync(); }
void DesktopTaskWorkspace::set_local_interaction_callback(std::function<void()> callback) { impl_->local_interaction = std::move(callback); }
bool DesktopTaskWorkspace::eventFilter(QObject* watched, QEvent* event) {
    if (!impl_ || impl_->syncing) return false;
    if (watched == impl_->owner) {
        switch (event->type()) {
        case QEvent::Show: case QEvent::Hide: case QEvent::Move: case QEvent::Resize:
        case QEvent::WindowStateChange: case QEvent::ScreenChangeInternal: impl_->sync(); break;
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        case QEvent::DevicePixelRatioChange: impl_->sync(); break;
#endif
        default: break;
        }
    }
    if (watched == impl_->retry && (event->type() == QEvent::Show || event->type() == QEvent::Hide)) impl_->sync();
    if (watched == impl_->bar.get() && event->type() == QEvent::LayoutRequest) impl_->sync();
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget) return false;
    QWidget* top = widget->window();
    const bool ours = top == impl_->bar.get() || std::any_of(impl_->tasks.begin(), impl_->tasks.end(), [top](const auto& task) { return task.window.get() == top; });
    if (ours && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick
        || event->type() == QEvent::Wheel || event->type() == QEvent::KeyPress
        || event->type() == QEvent::FocusIn || event->type() == QEvent::WindowActivate)) {
        impl_->interact();
        if (event->type() == QEvent::MouseButtonPress) top->raise();
        // WebView2 may activate its tool through native input, without a Qt
        // mouse press on the content. Keep the bar above that activated tool.
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::WindowActivate) impl_->bar->raise();
    }
    return false;
}
} // namespace redclaw::ui
