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
#include "settings_button.hpp"
#include "settings_dialog.hpp"
#include "update_bar.hpp"
#include "ui_style.hpp"
#include "window_animation.hpp"
#include "window_placement.hpp"

#include "wizz/services/updater.hpp"
#include "wizz/ui/strings.hpp"
#include "wizz/ui/update_model.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
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

/// A QElapsedTimer started now, in nanoseconds.
[[nodiscard]] MainWindow::MonotonicClock elapsed_timer_clock()
{
    QElapsedTimer timer;
    timer.start();
    return [timer] { return timer.nsecsElapsed(); };
}

} // namespace

MainWindow::MainWindow(std::shared_ptr<const AppServices> services, ui::AppSettings settings,
                       std::optional<ui::AutoTestOptions> autotest, QWidget* parent)
    : QMainWindow(parent),
      services_(std::move(services)),
      settings_(std::move(settings)),
      autotest_(std::move(autotest)),
      open_animation_probe_(&opens_with_animation),
      clock_(elapsed_timer_clock())
{
    setWindowTitle(main_window_title());
    build();
    start_timer_ = new QTimer(this);
    start_timer_->setObjectName(QStringLiteral("startTimer"));
    start_timer_->setSingleShot(true);
    start_timer_->setTimerType(Qt::PreciseTimer);
    connect(start_timer_, &QTimer::timeout, this, &MainWindow::start_when_due);

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
    // The Settings window goes first (it follows the updater), then the flows (they
    // reference the widgets), then the widgets, while the services they use (clock,
    // locale) are still alive.
    delete settings_dialog_.data();
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
    // The gear at the right end of the tab row (D-63): a corner widget is never taller
    // than the tab bar, so it costs the results no height.
    settings_button_ = new SettingsButton(tabs_);
    tabs_->setCornerWidget(settings_button_, Qt::TopRightCorner);

    // Dock order (01 §4.2): the busy panel has a fixed height and is laid out first; the
    // input panel takes its height for the width; the tabs fill the rest. The update bar
    // (D-46) takes its height for the width between the input panel and the tabs.
    layout->addWidget(input_);
    layout->addWidget(update_bar_);
    // The results sit in a rounded card (the tab pane) with the same side margins as the
    // input cards above them.
    auto* results = new QWidget(central);
    results->setObjectName(QStringLiteral("resultsArea"));
    auto* results_layout = new QVBoxLayout(results);
    results_layout->setContentsMargins(kResultsMargins);
    results_layout->setSpacing(0);
    tabs_->setParent(results);
    results_layout->addWidget(tabs_);
    layout->addWidget(results, 1);
    layout->addWidget(busy_);
    setCentralWidget(central);

    // Tab order (01 §6.3): the input panel's own order, then the result tabs, then Cancel;
    // the update bar's buttons come between the input panel and the tabs while it shows,
    // and Settings right after the tabs, at the end of their row (D-63), before the page.
    const std::vector<QWidget*> inputs = input_->tab_order();
    if (!inputs.empty()) {
        QWidget::setTabOrder(inputs.back(), update_bar_->action_button());
    }
    QWidget::setTabOrder(update_bar_->action_button(), update_bar_->later_button());
    QWidget::setTabOrder(update_bar_->later_button(), tabs_);
    QWidget::setTabOrder(tabs_->tabBar(), settings_button_);
}

bool MainWindow::enable_updates(UpdateHooks hooks)
{
    if (is_autotest() || updates_ != nullptr) {
        return false;  // a self-test run never contacts GitHub (D-45)
    }
    Controller* const controller = controller_;
    updates_ = new UpdateController(std::move(hooks), *update_bar_,
                                    [controller](std::string_view line) { controller->log(line); }, this);
    updates_->set_options(settings_.updates);  // settings.json's update options (D-60)
    updates_->set_working(controller_->is_working());
    connect(controller_, &Controller::working_changed, updates_, &UpdateController::set_working);
    connect(update_bar_, &UpdateBar::action_clicked, updates_, &UpdateController::trigger_action);
    connect(update_bar_, &UpdateBar::later_clicked, updates_, &UpdateController::dismiss);
    connect(updates_, &UpdateController::restart_requested, this, &QWidget::close);
    if (startup_begun()) {
        updates_->start();  // enabled after the start-up work began: start at once
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
    // The start of the first busy period, for diagnostics.json (D-65).
    connect(busy_, &BusyPanel::animation_started, this, [this] {
        if (shown_once_ && !startup_.first_busy_ms) {
            startup_.first_busy_ms = since_shown_ms();
        }
    });
    connect(settings_button_, &QPushButton::clicked, this, [this] { (void)open_settings(); });
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

void MainWindow::set_start_timing(OpenAnimationProbe probe, MonotonicClock clock)
{
    if (probe) {
        open_animation_probe_ = std::move(probe);
    }
    if (clock) {
        clock_ = std::move(clock);
    }
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
// The Settings window (D-62, D-63)
// ---------------------------------------------------------------------------

SettingsDialog* MainWindow::settings_dialog() const noexcept
{
    return settings_dialog_.data();
}

SettingsDialog* MainWindow::open_settings()
{
    if (SettingsDialog* open = settings_dialog_.data(); open != nullptr) {
        open->raise();
        open->activateWindow();
        return open;
    }
    const ui::UpdateStatusView status =
        updates_ != nullptr ? updates_->status_view()
                            : ui::updates_off_view(update::parse_version(WIZZ_VERSION_STRING).value_or(update::Version{}));
    auto* dialog = new SettingsDialog(settings_.updates, status, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    settings_dialog_ = dialog;
    connect(dialog, &SettingsDialog::options_changed, this, &MainWindow::apply_update_options);
    if (updates_ != nullptr) {
        UpdateController* const updates = updates_;
        connect(dialog, &SettingsDialog::check_now_requested, updates, &UpdateController::check_now);
        // A check or a download shows live, and so does what the bar's buttons did (D-62).
        connect(updates, &UpdateController::status_changed, dialog,
                [dialog, updates] { dialog->show_status(updates->status_view()); });
    }
    dialog->open();  // window-modal: no nested event loop, the updater keeps running
    return dialog;
}

void MainWindow::apply_update_options(const update::UpdateOptions& options)
{
    settings_.updates = options;
    if (updates_ != nullptr) {
        updates_->set_options(options);  // applies at once (D-61)
    }
    if (!is_autotest()) {
        (void)current_settings().save(services_->settings_path());  // I/O errors are ignored (01 §13)
    }
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

bool MainWindow::event(QEvent* event)
{
    if (event->type() == QEvent::ThemeChange) {
        style::schedule_appearance_refresh();  // e.g. High Contrast switched on or off
    }
    return QMainWindow::event(event);
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (shown_once_ || event->spontaneous()) {
        return;
    }
    shown_once_ = true;
    shown_ns_ = clock_();  // show() sends Shown just before the native window appears
    input_->focus_from();
    // OnShownAsync runs once the window is visible, so the progress UI can be seen; the
    // updater's first evaluation runs on its own worker next to it (D-45). While Windows
    // animates the window in, both wait until the animation is over (D-65).
    startup_.window_animates = open_animation_probe_(*this);
    startup_.start_delay = startup_.window_animates ? kStartDelay : std::chrono::milliseconds{0};
    start_timer_->start(startup_.start_delay);  // 0: the next event-loop pass
}

void MainWindow::start_when_due()
{
    const std::int64_t due = std::chrono::nanoseconds{startup_.start_delay}.count();
    if (const std::int64_t waited = clock_() - shown_ns_; waited < due) {
        // Timers may fire a little early: the first work never starts before the delay.
        const std::int64_t rest_ms = (due - waited + 999'999) / 1'000'000;
        start_timer_->start(std::chrono::milliseconds{rest_ms});
        return;
    }
    begin_startup();
}

void MainWindow::begin_startup()
{
    if (closed_ || startup_begun()) {
        return;  // closed during the delay: nothing starts
    }
    startup_.work_started_ms = since_shown_ms();
    controller_->spawn(on_shown());
    if (updates_ != nullptr) {
        updates_->start();
    }
}

double MainWindow::since_shown_ms() const
{
    return static_cast<double>(clock_() - shown_ns_) / 1'000'000.0;
}

Task<> MainWindow::on_shown()
{
    // A search started during the start delay loads the map itself (PrepareAsync, 01 §11.4);
    // a second load beside it would take over its phase (stop source, busy panel).
    if (!controller_->is_working()) {
        co_await controller_->load_catalog();
    }
    if (autotest_runner_ != nullptr) {
        co_await autotest_runner_->run();
    }
    Q_EMIT startup_finished();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (SettingsDialog* settings = settings_dialog_.data(); settings != nullptr) {
        settings->close();  // e.g. "Update now" finished while it was open
    }
    closed_ = true;
    start_timer_->stop();        // closed during the start delay: no work starts (D-65)
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
