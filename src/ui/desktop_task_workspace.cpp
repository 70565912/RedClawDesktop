#include "ui/desktop_task_workspace.h"
#include "ui/bounded_floating_window.h"

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
        std::unique_ptr<BoundedFloatingWindow> window;
        QToolButton* button = nullptr;
        QRect preferred;
        QSize initial;
        bool visible = false;
    };
    QPointer<QWidget> owner, root;
    QPointer<QSettings> settings;
    std::unique_ptr<BoundedFloatingWindow> bar;
    TaskBarLayout* bar_layout;
    std::vector<Task> tasks;
    QPushButton *control, *audio, *retry;
    QToolButton *fullscreen, *connection;
    QPushButton* disconnect;
    QLabel *control_state, *connection_state;
    QPoint bar_position;
    bool has_bar_position = false, enabled = true, syncing = false;
    std::function<void()> local_interaction;

    Impl(QWidget* window, QSettings* store) : owner(window), root(window->window()), settings(store), bar(std::make_unique<BoundedFloatingWindow>(window, true)) {
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
        audio = new QPushButton(QString::fromUtf8("播放声音"), bar.get());
        audio->setObjectName("remoteAudioButton");
        audio->setCheckable(true);
        audio->setEnabled(false);
        audio->hide();
        audio->setToolTip(QString::fromUtf8("等待对端音频能力"));
        retry = new QPushButton(QString::fromUtf8("重试画面"), bar.get()); retry->setObjectName("retryCaptureButton"); retry->hide();
        fullscreen = new QToolButton(bar.get()); fullscreen->setObjectName("playbackFullscreenButton");
        fullscreen->setCheckable(true); fullscreen->setText(QString::fromUtf8("全屏"));
        fullscreen->setToolTip(QString::fromUtf8("全屏（Ctrl+Alt+Enter）"));
        connection = new QToolButton(bar.get()); connection->setObjectName("connectionOverlayButton");
        connection->setText(QString::fromUtf8("连接")); connection->setCheckable(true);
        disconnect = new QPushButton(QString::fromUtf8("断开"), bar.get()); disconnect->setObjectName("disconnectSessionButton");
        control_state = new TaskStatusLabel(bar.get()); control_state->setObjectName("remoteControlStatus");
        connection_state = new TaskStatusLabel(bar.get()); connection_state->setObjectName("playbackWindowStatus");
        for (auto* widget : std::vector<QWidget*>{control, audio, retry, fullscreen, connection, disconnect, control_state, connection_state}) bar_layout->addWidget(widget);
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
    bool available() const { return enabled && owner && owner->isVisible() && !owner->window()->isMinimized(); }
    void arrange_bar(const QRect& area) {
        const int width = std::min({920, area.width(), bar_layout->sizeHint().width()});
        const QSize size(width, std::min(area.height(), bar_layout->heightForWidth(width)));
        const QPoint position = has_bar_position ? bar_position : QPoint((area.width() - size.width()) / 2, 12);
        bar->set_bounds(area);
        if (!bar->dragging()) bar->setGeometry(bounded_floating_rect(QRect(area.topLeft() + position, size), area));
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
            if (!task.window->dragging()) task.window->setGeometry(bounded_floating_rect(target.translated(area.topLeft()), area));
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
    task.window = std::make_unique<BoundedFloatingWindow>(impl_->owner, false);
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
QPushButton* DesktopTaskWorkspace::audio_button() const { return impl_->audio; }
QPushButton* DesktopTaskWorkspace::retry_button() const { return impl_->retry; }
QToolButton* DesktopTaskWorkspace::fullscreen_button() const { return impl_->fullscreen; }
QToolButton* DesktopTaskWorkspace::connection_button() const { return impl_->connection; }
QPushButton* DesktopTaskWorkspace::disconnect_button() const { return impl_->disconnect; }
QLabel* DesktopTaskWorkspace::control_status() const { return impl_->control_state; }
QLabel* DesktopTaskWorkspace::connection_status() const { return impl_->connection_state; }
void DesktopTaskWorkspace::set_enabled(bool enabled) { impl_->enabled = enabled; impl_->sync(); }
void DesktopTaskWorkspace::set_local_interaction_callback(std::function<void()> callback) { impl_->local_interaction = std::move(callback); }
bool DesktopTaskWorkspace::eventFilter(QObject* watched, QEvent* event) {
    if (!impl_ || impl_->syncing) return false;
    if (watched == impl_->owner || watched == impl_->root) {
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
