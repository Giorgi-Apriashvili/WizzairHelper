#include "main_window.hpp"

#include "app_info.hpp"
#include "autotest_runner.hpp"
#include "busy_panel.hpp"
#include "controller.hpp"
#include "controller_dialogs.hpp"
#include "flight_grid.hpp"
#include "grid_table_view.hpp"
#include "input_panel.hpp"
#include "log_view.hpp"
#include "qt_strings.hpp"
#include "round_trip_grid.hpp"
#include "search_button.hpp"
#include "update_bar.hpp"
#include "window_placement.hpp"

#include "wizz/ui/strings.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMetaObject>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace wizz::app {

namespace strings = ui::strings;

namespace {

/// TabControl header padding (12, 4) (01 §5): each tab is its text plus 12 px on either
/// side and 4 px above and below, never smaller than the style's own tab.
class PaddedTabBar final : public QTabBar {
public:
    explicit PaddedTabBar(QWidget* parent = nullptr) : QTabBar(parent) {}

protected:
    [[nodiscard]] QSize tabSizeHint(int index) const override
    {
        const QSize base = QTabBar::tabSizeHint(index);
        const QFontMetrics metrics{font()};
        const QSize padded{metrics.horizontalAdvance(tabText(index)) + 2 * MainWindow::kTabPadding.width(),
                           metrics.height() + 2 * MainWindow::kTabPadding.height()};
        return base.expandedTo(padded);
    }
};

class ResultTabs final : public QTabWidget {
public:
    explicit ResultTabs(QWidget* parent = nullptr) : QTabWidget(parent) { setTabBar(new PaddedTabBar(this)); }
};

template <class T>
[[nodiscard]] bool is_inside(const QWidget* widget)
{
    for (const QWidget* w = widget; w != nullptr; w = w->parentWidget()) {
        if (qobject_cast<const T*>(w) != nullptr) {
            return true;
        }
        if (w->isWindow()) {
            break;
        }
    }
    return false;
}

/// Form.WindowState: the native show command when there is a Win32 window, else Qt's
/// state, where minimised wins (Qt keeps WindowMaximized while minimised).
[[nodiscard]] WindowShowState window_state_of(const QWidget& window)
{
    if (const std::optional<NativeBounds> native = native_bounds(window)) {
        return native->state;
    }
    if (window.isMinimized()) {
        return WindowShowState::minimized;
    }
    return window.isMaximized() ? WindowShowState::maximized : WindowShowState::normal;
}

} // namespace

MainWindow::MainWindow(std::shared_ptr<const AppServices> services, ui::AppSettings settings,
                       std::optional<ui::AutoTestOptions> autotest, QWidget* parent)
    : QMainWindow(parent), services_(std::move(services)), settings_(std::move(settings)), autotest_(std::move(autotest))
{
    setWindowTitle(main_window_title());
    build();

    controller_ = new Controller(services_,
                                 MainViews{.input = input_,
                                           .tabs = tabs_,
                                           .outbound = outbound_,
                                           .inbound = inbound_,
                                           .trips = trips_,
                                           .log = log_,
                                           .busy = busy_},
                                 is_autotest(), this);
    controller_->set_dialogs(ControllerDialogs::system(this));
    if (autotest_) {
        autotest_runner_ = new AutoTestRunner(*this, *autotest_, this);
    }
    wire();

    controller_->log(strings::kStartedLog);  // the constructor's line (01 §12.2)
    controller_->recompute_trips();           // Round trips placeholder and the tab titles
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
    // The flows go first (they reference the widgets), then the widgets, while the
    // services they use (clock, locale) are still alive.
    delete updates_;
    updates_ = nullptr;
    delete autotest_runner_;
    autotest_runner_ = nullptr;
    delete controller_;
    controller_ = nullptr;
    delete takeCentralWidget();
}

void MainWindow::build()
{
    const std::shared_ptr<const Locale>& locale = services_->locale;
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    input_ = new InputPanel(locale, services_->clock->local_today(), settings_, central);
    tabs_ = new ResultTabs(central);
    outbound_ = new FlightGrid(locale, true, tabs_);
    inbound_ = new FlightGrid(locale, false, tabs_);
    trips_ = new RoundTripGrid(locale, tabs_);
    log_ = new LogView(*services_->clock, locale, tabs_);
    tabs_->addTab(outbound_, to_qstring(strings::kTabOutbound));
    tabs_->addTab(inbound_, to_qstring(strings::kTabReturn));
    tabs_->addTab(trips_, to_qstring(strings::kTabRoundTrips));
    tabs_->addTab(log_, to_qstring(strings::kTabLog));
    busy_ = new BusyPanel(*services_->clock, locale, central);
    update_bar_ = new UpdateBar(central);  // hidden until an update is offered (D-46)

    // Dock order (01 §4.2): the busy panel has a fixed height and is laid out first; the
    // input panel takes its height for the width; the tabs fill the rest. The update bar
    // (D-46) takes its height for the width between the input panel and the tabs.
    layout->addWidget(input_);
    layout->addWidget(update_bar_);
    layout->addWidget(tabs_, 1);
    layout->addWidget(busy_);
    setCentralWidget(central);

    // Tab order (01 §6.3): the input panel's own order, then the result tabs, then Cancel;
    // the update bar's buttons come between the input panel and the tabs while it shows.
    const std::vector<QWidget*> inputs = input_->tab_order();
    if (!inputs.empty()) {
        QWidget::setTabOrder(inputs.back(), update_bar_->action_button());
    }
    QWidget::setTabOrder(update_bar_->action_button(), update_bar_->later_button());
    QWidget::setTabOrder(update_bar_->later_button(), tabs_);
}

bool MainWindow::enable_updates(UpdateHooks hooks)
{
    if (is_autotest() || updates_ != nullptr) {
        return false;  // a self-test run never contacts GitHub (D-45)
    }
    Controller* const controller = controller_;
    updates_ = new UpdateController(std::move(hooks), *update_bar_,
                                    [controller](std::string_view line) { controller->log(line); }, this);
    updates_->set_working(controller_->is_working());
    connect(controller_, &Controller::working_changed, updates_, &UpdateController::set_working);
    connect(update_bar_, &UpdateBar::action_clicked, updates_, &UpdateController::trigger_action);
    connect(update_bar_, &UpdateBar::later_clicked, updates_, &UpdateController::dismiss);
    connect(updates_, &UpdateController::restart_requested, this, &QWidget::close);
    if (shown_once_) {
        updates_->start();  // enabled after the first Shown: start at once
    }
    return true;
}

void MainWindow::wire()
{
    Controller* const controller = controller_;
    connect(input_, &InputPanel::search_outbound_requested, controller, &Controller::request_search_outbound);
    connect(input_, &InputPanel::search_return_requested, controller, &Controller::request_search_return);
    connect(input_, &InputPanel::currency_changed, controller, [controller] { controller->on_currency_changed(); });
    connect(input_, &InputPanel::nights_changed, controller, [controller] { controller->recompute_trips(); });
    connect(outbound_, &FlightGrid::plan_return_requested, controller, &Controller::request_plan_return);
    connect(outbound_, &FlightGrid::contents_changed, controller, [controller] { controller->update_tab_titles(); });
    connect(inbound_, &FlightGrid::contents_changed, controller, [controller] { controller->update_tab_titles(); });
    connect(trips_, &RoundTripGrid::filters_changed, controller, [controller] { controller->recompute_trips(); });
    connect(busy_, &BusyPanel::cancel_requested, controller, &Controller::cancel_work);
    outbound_->set_busy_query([controller] { return controller->is_working(); });
    inbound_->set_busy_query([controller] { return controller->is_working(); });
}

// ---------------------------------------------------------------------------
// Geometry (01 §4.1)
// ---------------------------------------------------------------------------

void MainWindow::apply_start_geometry()
{
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }
    if (windowHandle() == nullptr) {
        create();  // the native frame margins are known once the window exists
    }
    windowHandle()->setScreen(screen);

    const qreal ratio = screen->devicePixelRatio();
    const QRect work = screen->availableGeometry();
    const auto device = [ratio](QSize logical) {
        return ui::WindowSize{.width = qRound(logical.width() * ratio), .height = qRound(logical.height() * ratio)};
    };
    const auto logical = [ratio](int device_pixels) { return qRound(device_pixels / ratio); };

    // WinForms sizes are the whole window rectangle (title bar, borders and the invisible
    // resize borders); Qt sizes the client area and its frame is only the visible part.
    const QMargins qt_frame = windowHandle()->frameMargins();
    QMargins frame = qt_frame;
    if (const std::optional<QMargins> native = native_frame_margins(*this, ratio)) {
        frame = QMargins{logical(native->left()), logical(native->top()), logical(native->right()),
                         logical(native->bottom())};
    }
    const QSize frame_extra{frame.left() + frame.right(), frame.top() + frame.bottom()};

    const ui::WindowSize outer_device =
        ui::start_window_size(settings_, device(kDefaultSize), device(kMinimumSize), device(work.size()));
    const QSize outer{logical(outer_device.width), logical(outer_device.height)};
    setMinimumSize(kMinimumSize - frame_extra);
    resize(outer - frame_extra);
    // CenterScreen (Form.CenterToScreen): the window rectangle centred in the work area of
    // the cursor's screen, but never above or left of it when it is the larger one.
    const QPoint placed{std::max(work.x(), work.x() + (work.width() - outer.width()) / 2),
                        std::max(work.y(), work.y() + (work.height() - outer.height()) / 2)};
    move(placed + QPoint{frame.left() - qt_frame.left(), frame.top() - qt_frame.top()});

    if (ui::start_maximized(settings_, is_autotest())) {
        setWindowState(windowState() | Qt::WindowMaximized);
    }
}

void MainWindow::show_main_window()
{
    apply_start_geometry();
    show();
}

QSize MainWindow::normal_outer_device_size() const
{
    if (const std::optional<NativeBounds> native = native_bounds(*this)) {
        return bounds_to_save(native->state, native->bounds, native->restore_bounds);
    }
    const QRect client = (isMaximized() || isMinimized() || isFullScreen()) ? normalGeometry() : geometry();
    const QMargins frame = windowHandle() != nullptr ? windowHandle()->frameMargins() : QMargins{};
    const qreal ratio = devicePixelRatioF();
    return QSize{qRound((client.width() + frame.left() + frame.right()) * ratio),
                 qRound((client.height() + frame.top() + frame.bottom()) * ratio)};
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

QWidget* MainWindow::page(Page which) const
{
    switch (which) {
    case Page::outbound:
        return outbound_;
    case Page::inbound:
        return inbound_;
    case Page::round_trips:
        return trips_;
    case Page::log:
        return log_;
    }
    return nullptr;
}

void MainWindow::select_page(Page which)
{
    tabs_->setCurrentWidget(page(which));
}

// ---------------------------------------------------------------------------
// Settings (01 §13)
// ---------------------------------------------------------------------------

ui::AppSettings MainWindow::current_settings() const
{
    ui::AppSettings settings = settings_;
    input_->store_to(settings);
    settings.maximized = window_state_of(*this) == WindowShowState::maximized;
    const QSize bounds = normal_outer_device_size();
    settings.window_width = bounds.width();
    settings.window_height = bounds.height();
    return settings;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (shown_once_ || event->spontaneous()) {
        return;
    }
    shown_once_ = true;
    input_->focus_from();
    // OnShownAsync runs once the window is visible, so the progress UI can be seen; the
    // updater's first evaluation runs on its own worker next to it (D-45).
    QMetaObject::invokeMethod(
        this,
        [this] {
            controller_->spawn(on_shown());
            if (updates_ != nullptr) {
                updates_->start();
            }
        },
        Qt::QueuedConnection);
}

Task<> MainWindow::on_shown()
{
    co_await controller_->load_catalog();
    if (autotest_runner_ != nullptr) {
        co_await autotest_runner_->run();
    }
    Q_EMIT startup_finished();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    controller_->cancel_work();  // stop any running work first
    if (!is_autotest()) {
        input_->commit_numeric_edits();  // the X / Alt+F4 move no focus: read the typed number too
        (void)current_settings().save(services_->settings_path());  // I/O errors are ignored
    }
    if (updates_ != nullptr) {
        updates_->on_window_closing();  // a verified installer starts now (D-47)
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    if (enter && (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
        // Only Enter that no control used reaches the window. A result grid opens its row
        // and consumes Enter, so a booking never also submits a search (01 §6.3).
        QWidget* focus = focusWidget();
        if (is_inside<GridTableView>(focus)) {
            event->accept();
            return;
        }
        if (auto* button = qobject_cast<QPushButton*>(focus); button != nullptr && button->isEnabled()) {
            button->click();  // a focused button is clicked by Enter
        } else {
            input_->trigger_accept();  // the AcceptButton that follows focus
        }
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape &&
        handle_escape(watched)) {
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::handle_escape(QObject* watched)
{
    const auto* widget = qobject_cast<const QWidget*>(watched);
    if (widget == nullptr || widget->window() != this || controller_ == nullptr || !controller_->is_working()) {
        return false;
    }
    // DropDownOpen(): an open combo list (or completer, calendar, menu) is a popup window;
    // its Esc only closes it (WF-3).
    if (QApplication::activePopupWidget() != nullptr) {
        return false;
    }
    busy_->request_cancel();
    return true;
}

} // namespace wizz::app
