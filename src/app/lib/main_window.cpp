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

    // Dock order (01 §4.2): the busy panel has a fixed height and is laid out first; the
    // input panel takes its height for the width; the tabs fill the rest.
    layout->addWidget(input_);
    layout->addWidget(tabs_, 1);
    layout->addWidget(busy_);
    setCentralWidget(central);

    // Tab order (01 §6.3): the input panel's own order, then the result tabs, then Cancel.
    const std::vector<QWidget*> inputs = input_->tab_order();
    if (!inputs.empty()) {
        QWidget::setTabOrder(inputs.back(), tabs_);
    }
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
    // CenterScreen: the window rectangle centred in the work area of the cursor's screen.
    QRect placed{QPoint{}, outer};
    placed.moveCenter(work.center());
    move(placed.topLeft() + QPoint{frame.left() - qt_frame.left(), frame.top() - qt_frame.top()});

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
    if (const std::optional<QSize> native = native_normal_outer_size(*this)) {
        return *native;
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
    settings.maximized = isMaximized();
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
    // OnShownAsync runs once the window is visible, so the progress UI can be seen.
    QMetaObject::invokeMethod(this, [this] { controller_->spawn(on_shown()); }, Qt::QueuedConnection);
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
        (void)current_settings().save(services_->settings_path());  // I/O errors are ignored
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
