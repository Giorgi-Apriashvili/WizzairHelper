// MainWindow (01 §3.3, §4.1, §4.2, §5, §6.3, §6.4, §12.2, §13): title, docked
// layout, tabs, geometry and its persistence, start-up, settings on close, and
// the window-level keyboard (AcceptButton that follows focus, Esc = Cancel).

#include "app_test_support.hpp"

#include "app_info.hpp"
#include "date_picker.hpp"
#include "fractional_spin_box.hpp"
#include "grid_table_view.hpp"
#include "place_combo.hpp"
#include "qt_strings.hpp"
#include "search_button.hpp"
#include "ui_style.hpp"
#include "window_placement.hpp"

#include "wizz/services/app_paths.hpp"
#include "wizz/ui/strings.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QTabBar>
#include <QTabWidget>
#include <QWindow>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

using namespace std::chrono_literals;
using wizz::app::MainWindow;
using wizz::test::app::WindowRig;
using wizz::test::app::WindowRigOptions;
namespace strings = wizz::ui::strings;
namespace gui = wizz::test::gui;

[[nodiscard]] QString q(std::string_view text)
{
    return wizz::app::to_qstring(text);
}

/// 01 §4.1 / UX-15 for the screen the window opens on (the "minimal" test screen may be
/// smaller than the minimum size, which then wins).
[[nodiscard]] QSize expected_outer(QSize wanted, const QWidget& window)
{
    const QRect work = window.screen()->availableGeometry();
    return QSize{std::clamp(wanted.width(), MainWindow::kMinimumSize.width(),
                            std::max(MainWindow::kMinimumSize.width(), work.width())),
                 std::clamp(wanted.height(), MainWindow::kMinimumSize.height(),
                            std::max(MainWindow::kMinimumSize.height(), work.height()))};
}

/// The whole window rectangle in logical pixels (WinForms Bounds).
[[nodiscard]] QSize outer_size(const MainWindow& window)
{
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        RECT rect{};
        GetWindowRect(reinterpret_cast<HWND>(window.winId()), &rect);
        const qreal ratio = window.devicePixelRatioF();
        return QSize{qRound((rect.right - rect.left) / ratio), qRound((rect.bottom - rect.top) / ratio)};
    }
    return window.frameGeometry().size();
}

/// The whole window rectangle in logical pixels (WinForms Bounds, position included).
[[nodiscard]] QRect outer_rect(const MainWindow& window)
{
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        RECT rect{};
        GetWindowRect(reinterpret_cast<HWND>(window.winId()), &rect);
        const qreal ratio = window.devicePixelRatioF();
        return QRect{QPoint{qRound(rect.left / ratio), qRound(rect.top / ratio)}, outer_size(window)};
    }
    return window.frameGeometry();
}

/// Types `text` key by key into `target` — no Enter, no focus change, so nothing is
/// committed yet (NumericUpDown.UserEdit).
void type_uncommitted(QWidget* target, const QString& text)
{
    for (const QChar c : text) {
        QKeyEvent press{QEvent::KeyPress, static_cast<int>(c.unicode()), Qt::NoModifier, QString{c}};
        QApplication::sendEvent(target, &press);
        QKeyEvent release{QEvent::KeyRelease, static_cast<int>(c.unicode()), Qt::NoModifier, QString{c}};
        QApplication::sendEvent(target, &release);
    }
}

[[nodiscard]] wizz::ui::AppSettings saved_settings(const WindowRig& rig)
{
    const std::filesystem::path path = wizz::app_paths::settings_file(rig.data_root());
    EXPECT_TRUE(std::filesystem::exists(path));
    return wizz::ui::AppSettings::load(path);
}

TEST(MainWindow, Title_tabs_and_docked_layout)
{
    WindowRig rig;
    rig.start();
    MainWindow& window = rig.window();
    EXPECT_EQ(window.windowTitle(), q(strings::kWindowTitle));
    EXPECT_EQ(window.windowTitle(), wizz::app::main_window_title());

    QTabWidget& tabs = *window.tabs();
    ASSERT_EQ(tabs.count(), 4);
    EXPECT_EQ(tabs.widget(0), window.outbound_grid());
    EXPECT_EQ(tabs.widget(1), window.return_grid());
    EXPECT_EQ(tabs.widget(2), window.round_trip_grid());
    EXPECT_EQ(tabs.widget(3), window.log_view());
    EXPECT_EQ(tabs.tabText(0), "Outbound");
    EXPECT_EQ(tabs.tabText(1), "Return");
    EXPECT_EQ(tabs.tabText(2), "Round trips");
    EXPECT_EQ(tabs.tabText(3), "Log");
    EXPECT_TRUE(window.outbound_grid()->offers_plan_return());
    EXPECT_FALSE(window.return_grid()->offers_plan_return());

    // Input panel on top, the results card filling the middle (the tabs inside it, inset by
    // kResultsMargins like the input cards, D-53), the busy panel at the bottom with its
    // fixed height, full width; nothing overlaps.
    gui::settle();
    const QWidget* central = window.centralWidget();
    const auto in_central = [central](const QWidget* widget) {
        return QRect(widget->mapTo(central, QPoint(0, 0)), widget->size());
    };
    const QWidget* results = central->findChild<QWidget*>(QStringLiteral("resultsArea"));
    ASSERT_NE(results, nullptr);
    ASSERT_TRUE(results->isAncestorOf(&tabs));
    const QRect input = in_central(window.input_panel());
    const QRect middle = in_central(results);
    const QRect busy = in_central(window.busy_panel());
    EXPECT_EQ(input.top(), 0);
    EXPECT_LE(input.bottom(), middle.top());
    EXPECT_LE(middle.bottom(), busy.top());
    EXPECT_EQ(in_central(&tabs), middle.marginsRemoved(MainWindow::kResultsMargins));
    EXPECT_EQ(MainWindow::kResultsMargins, QMargins(16, 4, 16, 8));
    EXPECT_EQ(busy.bottom(), central->height() - 1);
    EXPECT_EQ(busy.height(), window.busy_panel()->metrics().height);
    EXPECT_EQ(busy.width(), central->width());
    EXPECT_EQ(window.menuWidget(), nullptr) << "no menus or toolbar (01 \u00A73.3)";
}

TEST(MainWindow, Tab_headers_have_the_12_4_padding)
{
    WindowRig rig;
    rig.start();
    const QTabBar& bar = *rig.window().tabs()->tabBar();
    const QFontMetrics metrics{bar.font()};
    for (int i = 0; i < bar.count(); ++i) {
        const QRect tab = bar.tabRect(i);
        EXPECT_GE(tab.width(), metrics.horizontalAdvance(bar.tabText(i)) + 2 * MainWindow::kTabPadding.width())
            << bar.tabText(i).toStdString();
        EXPECT_GE(tab.height(), metrics.height() + 2 * MainWindow::kTabPadding.height());
    }
}

TEST(MainWindow, Start_up_logs_its_line_loads_the_map_and_focuses_From)
{
    WindowRig rig;
    rig.window().show_main_window();
    // OnShownAsync runs after the first Shown, so the focus is set before the map loads.
    EXPECT_EQ(rig.window().focusWidget(), rig.input().from_box()) << "the editable combo takes the focus of its line edit";
    ASSERT_TRUE(gui::wait_until([&] { return rig.controller().catalog() != nullptr; }));
    gui::settle();

    const std::vector<std::string> log = rig.log_lines();
    ASSERT_GE(log.size(), 2u);
    EXPECT_EQ(log[0], strings::kStartedLog);
    EXPECT_EQ(log[1], "Route map: 11 airports, API https://be.wizzair.com/29.23.1/Api.");
    EXPECT_EQ(rig.busy().status_text(),
              "Ready \u2014 11 Wizz Air airports loaded. Choose your trip and press \u201CSearch outbound\u201D.");
    EXPECT_EQ(rig.busy().status_colour(), wizz::app::style::theme().success);
    EXPECT_FALSE(rig.controller().is_working());
    EXPECT_EQ(rig.wizz_requests(), 2) << "homepage + route map";
    EXPECT_GT(rig.input().from_box()->count(), 0) << "suggestions after the map loaded";
    EXPECT_EQ(rig.input().from_box()->text(), "Budapest") << "the typed text is kept";
    EXPECT_EQ(rig.trips().summary_text(), strings::kTripsPlaceholder);
}

TEST(MainWindow, Default_size_minimum_size_and_centring)
{
    WindowRig rig;
    rig.window().show_main_window();
    gui::settle();
    MainWindow& window = rig.window();
    EXPECT_EQ(outer_size(window), expected_outer(MainWindow::kDefaultSize, window));
    EXPECT_FALSE(window.isMaximized());

    const QRect work = window.screen()->availableGeometry();
    if (work.width() >= MainWindow::kDefaultSize.width() && work.height() >= MainWindow::kDefaultSize.height()) {
        const QPoint centre = window.frameGeometry().center();
        EXPECT_NEAR(centre.x(), work.center().x(), 10);
        EXPECT_NEAR(centre.y(), work.center().y(), 20);
    }
    // The minimum is the whole window: L(980) × L(600).
    const QSize frame_extra = outer_size(window) - window.size();
    EXPECT_EQ(window.minimumSize() + frame_extra, MainWindow::kMinimumSize);
}

TEST(MainWindow, A_saved_size_is_restored_and_clamped_to_the_work_area)
{
    WindowRigOptions options;
    options.settings.window_width = 1100;
    options.settings.window_height = 700;
    WindowRig rig{std::move(options)};
    rig.window().show_main_window();
    gui::settle();
    const qreal ratio = rig.window().devicePixelRatioF();
    EXPECT_EQ(outer_size(rig.window()),
              expected_outer(QSize{qRound(1100 / ratio), qRound(700 / ratio)}, rig.window()));

    WindowRigOptions huge;
    huge.settings.window_width = 20000;
    huge.settings.window_height = 9000;
    WindowRig big{std::move(huge)};
    big.window().show_main_window();
    gui::settle();
    EXPECT_EQ(outer_size(big.window()), expected_outer(QSize{20000, 9000}, big.window()));

    WindowRigOptions tiny;
    tiny.settings.window_width = 10;
    tiny.settings.window_height = 10;
    WindowRig small_window{std::move(tiny)};
    small_window.window().show_main_window();
    gui::settle();
    EXPECT_EQ(outer_size(small_window.window()), MainWindow::kMinimumSize);
}

TEST(MainWindow, A_window_larger_than_the_work_area_starts_at_its_top_left_corner)
{
    // CenterScreen: Location = (max(work.X, work.X + (work.W − W) / 2), max(work.Y, work.Y + (work.H − H) / 2)).
    // The minimum size wins over a smaller work area (UX-15), so the caption must not
    // start above or left of it (the "minimal" test screen is smaller than L(980) × L(600)).
    WindowRigOptions options;
    options.settings.window_width = 20000;
    options.settings.window_height = 9000;
    WindowRig rig{std::move(options)};
    rig.window().show_main_window();
    gui::settle();
    const QRect work = rig.window().screen()->availableGeometry();
    const QRect outer = outer_rect(rig.window());
    EXPECT_GE(outer.left(), work.left());
    EXPECT_GE(outer.top(), work.top());
    EXPECT_NEAR(outer.left(), std::max(work.left(), work.left() + (work.width() - outer.width()) / 2), 1);
    EXPECT_NEAR(outer.top(), std::max(work.top(), work.top() + (work.height() - outer.height()) / 2), 1);
}

TEST(MainWindow, Maximized_from_settings_except_in_autotest)
{
    WindowRigOptions options;
    options.settings.maximized = true;
    WindowRig rig{std::move(options)};
    rig.window().show_main_window();
    EXPECT_TRUE(rig.window().windowState().testFlag(Qt::WindowMaximized));

    WindowRigOptions autotest;
    autotest.settings.maximized = true;
    autotest.autotest = wizz::ui::AutoTestOptions{};
    autotest.autotest->keep_open = true;
    WindowRig test_rig{std::move(autotest)};
    test_rig.window().apply_start_geometry();
    EXPECT_FALSE(test_rig.window().windowState().testFlag(Qt::WindowMaximized));
}

TEST(MainWindow, Closing_saves_the_settings_with_the_window_size)
{
    WindowRig rig;
    rig.start();
    wizz::app::InputPanel& input = rig.input();
    input.set_from_text("  Wien ");
    input.set_to_text("");
    input.set_from_radius_km(175.6);
    input.set_adults(3);
    input.set_fare_index(1);
    ASSERT_TRUE(input.set_currency("GBP"));
    input.set_max_nights(12);
    input.set_min_nights(3);
    const QSize bounds = rig.window().normal_outer_device_size();
    rig.window().close();

    const std::filesystem::path path = wizz::app_paths::settings_file(rig.data_root());
    ASSERT_TRUE(std::filesystem::exists(path));
    const wizz::ui::AppSettings saved = wizz::ui::AppSettings::load(path);
    EXPECT_EQ(saved.from, "Wien");
    EXPECT_EQ(saved.to, "");
    EXPECT_EQ(saved.from_radius_km, 175);
    EXPECT_EQ(saved.to_radius_km, 100);
    EXPECT_EQ(saved.adults, 3);
    EXPECT_TRUE(saved.discount_club);
    EXPECT_EQ(saved.currency, "GBP");
    EXPECT_EQ(saved.min_nights, 3);
    EXPECT_EQ(saved.max_nights, 12);
    EXPECT_FALSE(saved.maximized);
    EXPECT_EQ(saved.window_width, bounds.width());
    EXPECT_EQ(saved.window_height, bounds.height());
    const qreal ratio = rig.window().devicePixelRatioF();
    EXPECT_EQ(QSize(qRound(saved.window_width / ratio), qRound(saved.window_height / ratio)),
              outer_size(rig.window()))
        << "WindowWidth/Height are the whole window in device pixels";
}

TEST(MainWindow, Closing_saves_a_number_typed_but_not_yet_committed)
{
    // 01 §13 saves the current values; NumericUpDown.Value commits pending user text,
    // and the title-bar X / Alt+F4 close without moving focus first.
    WindowRig rig;
    rig.start();
    wizz::app::FractionalSpinBox* radius = rig.input().from_radius_box();
    radius->findChild<QLineEdit*>()->selectAll();
    type_uncommitted(radius, QStringLiteral("300"));
    ASSERT_TRUE(radius->user_edit());
    ASSERT_EQ(radius->int_value(), 250) << "typing alone commits nothing";
    rig.window().close();
    EXPECT_EQ(saved_settings(rig).from_radius_km, 300);

    // The commit runs the nights coupling like ValueChanged does (01 §6.2).
    WindowRig nights;
    nights.start();
    wizz::app::FractionalSpinBox* min_nights = nights.input().min_nights_box();
    min_nights->findChild<QLineEdit*>()->selectAll();
    type_uncommitted(min_nights, QStringLiteral("15"));
    nights.window().close();
    const wizz::ui::AppSettings saved = saved_settings(nights);
    EXPECT_EQ(saved.min_nights, 15);
    EXPECT_EQ(saved.max_nights, 15);
}

TEST(MainWindow, A_window_minimised_from_maximised_is_saved_as_not_maximised)
{
    // Maximized = (WindowState == Maximized) (01 §13); a minimised window is Minimized,
    // although Qt remembers "maximised" for the restore.
    WindowRigOptions options;
    options.settings.maximized = true;
    WindowRig rig{std::move(options)};
    rig.start();
    ASSERT_TRUE(rig.window().isMaximized());
    EXPECT_TRUE(rig.window().current_settings().maximized);
    rig.window().showMinimized();
    gui::settle();
    ASSERT_TRUE(rig.window().isMinimized());
    EXPECT_FALSE(rig.window().current_settings().maximized);
    rig.window().close();
    EXPECT_FALSE(saved_settings(rig).maximized);
}

TEST(WindowPlacement, The_window_state_follows_the_show_command_like_Form_UpdateWindowState)
{
    using wizz::app::WindowShowState;
    using wizz::app::window_show_state;
    EXPECT_EQ(window_show_state(SW_SHOWNORMAL), WindowShowState::normal);
    EXPECT_EQ(window_show_state(SW_RESTORE), WindowShowState::normal);
    EXPECT_EQ(window_show_state(SW_SHOW), WindowShowState::normal);
    EXPECT_EQ(window_show_state(SW_SHOWNA), WindowShowState::normal);
    EXPECT_EQ(window_show_state(SW_SHOWNOACTIVATE), WindowShowState::normal);
    EXPECT_EQ(window_show_state(SW_SHOWMAXIMIZED), WindowShowState::maximized);
    EXPECT_EQ(window_show_state(SW_SHOWMINIMIZED), WindowShowState::minimized);
    EXPECT_EQ(window_show_state(SW_MINIMIZE), WindowShowState::minimized);
    EXPECT_EQ(window_show_state(SW_SHOWMINNOACTIVE), WindowShowState::minimized);
}

TEST(WindowPlacement, Closing_saves_Bounds_when_normal_and_RestoreBounds_otherwise)
{
    // bounds = (WindowState == Normal) ? Bounds : RestoreBounds (01 §13). An Aero-snapped
    // window is Normal: GetWindowRect is the snapped size, while rcNormalPosition still
    // holds the size from before the snap. (Snapping itself needs real Win+Left input,
    // so the decision is tested here and the native call only feeds it the two rects.)
    using wizz::app::bounds_to_save;
    using wizz::app::WindowShowState;
    const QSize restore{1320, 820};
    const QSize snapped{960, 1040};
    EXPECT_EQ(bounds_to_save(WindowShowState::normal, snapped, restore), snapped);
    EXPECT_EQ(bounds_to_save(WindowShowState::normal, restore, restore), restore);
    EXPECT_EQ(bounds_to_save(WindowShowState::maximized, QSize{1936, 1056}, restore), restore);
    EXPECT_EQ(bounds_to_save(WindowShowState::minimized, QSize{160, 28}, restore), restore);
}

TEST(MainWindow, Closing_during_a_search_stops_the_work_first_and_shuts_down_cleanly)
{
    // The cancel_waiting server answers a timetable request only after a long delay. The
    // window closes while the search waits for it: closing must cancel the wait, not sit
    // it out. The bound is half the server's own delay — far more than any scheduling
    // delay of a loaded machine, far less than waiting the request out would take.
    std::chrono::milliseconds server_delay{0};
    for (const wizz::ui::ReplayTransport::Rule& rule : wizz::test::app::scenario_rules("cancel_waiting", true)) {
        server_delay = std::max(server_delay, rule.delay);
    }
    ASSERT_GE(server_delay, 10s);
    QElapsedTimer elapsed;
    {
        WindowRigOptions options;
        options.scenario = "cancel_waiting";
        options.keep_delays = true;
        WindowRig rig{std::move(options)};
        rig.start();
        rig.controller().spawn(rig.controller().search_outbound());
        ASSERT_TRUE(gui::wait_until([&] { return rig.busy().status_text().starts_with("Fetching fares"); }));
        ASSERT_EQ(rig.controller().jobs().live_count(), 1u);
        elapsed.start();
        rig.window().close();  // FormClosing: _cts?.Cancel(), then the settings
        EXPECT_TRUE(std::filesystem::exists(wizz::app_paths::settings_file(rig.data_root())));
    }  // the window, its flows and the worker go away without waiting for the search
    EXPECT_LT(std::chrono::milliseconds{elapsed.elapsed()}, server_delay / 2) << "the cancelled worker is joined promptly";
}

TEST(MainWindow, Autotest_never_saves_the_settings)
{
    WindowRigOptions options;
    options.autotest = wizz::ui::AutoTestOptions{};
    options.autotest->keep_open = true;
    WindowRig rig{std::move(options)};
    rig.window().show_main_window();
    gui::settle();
    rig.window().close();
    EXPECT_FALSE(std::filesystem::exists(wizz::app_paths::settings_file(rig.data_root())));
}

// ---------------------------------------------------------------------------
// Keyboard (01 §6.3)
// ---------------------------------------------------------------------------

/// Counts the panel's search requests without starting searches.
struct AcceptCounter {
    int outbound = 0;
    int inbound = 0;

    explicit AcceptCounter(WindowRig& rig)
    {
        QObject::disconnect(&rig.input(), &wizz::app::InputPanel::search_outbound_requested, &rig.controller(),
                            nullptr);
        QObject::disconnect(&rig.input(), &wizz::app::InputPanel::search_return_requested, &rig.controller(),
                            nullptr);
        QObject::connect(&rig.input(), &wizz::app::InputPanel::search_outbound_requested, [this] { ++outbound; });
        QObject::connect(&rig.input(), &wizz::app::InputPanel::search_return_requested, [this] { ++inbound; });
    }
};

/// Focus enters `widget` (the panel tracks FocusIn), then Enter is pressed in it.
void enter_in(QWidget* widget)
{
    QFocusEvent focus{QEvent::FocusIn, Qt::TabFocusReason};
    QApplication::sendEvent(widget, &focus);
    gui::send_key(widget, QEvent::KeyPress, Qt::Key_Return);
    gui::send_key(widget, QEvent::KeyRelease, Qt::Key_Return);
}

TEST(MainWindow, Enter_presses_the_search_button_that_follows_focus)
{
    WindowRig rig;
    rig.start();
    AcceptCounter counter{rig};
    wizz::app::InputPanel& input = rig.input();

    enter_in(input.from_box()->lineEdit());
    EXPECT_EQ(counter.outbound, 1) << "From maps to Search outbound";
    enter_in(input.ret_from_picker());
    EXPECT_EQ(counter.inbound, 1) << "the return dates map to Search return";
    enter_in(input.min_nights_box());
    EXPECT_EQ(counter.outbound + counter.inbound, 2) << "the night boxes press nothing (WF-4)";

    // Controls outside the panel keep the last mapping (none, after the nights).
    enter_in(rig.trips().same_home_box());
    EXPECT_EQ(counter.outbound + counter.inbound, 2);
    enter_in(input.out_to_picker());
    EXPECT_EQ(counter.outbound, 2);
    enter_in(rig.trips().same_home_box());
    EXPECT_EQ(counter.outbound, 3) << "anything else keeps the last mapping";
}

TEST(MainWindow, Enter_in_a_result_grid_opens_the_row_and_never_searches)
{
    WindowRig rig;
    rig.start();
    rig.run(rig.controller().search_outbound());
    ASSERT_GT(rig.outbound().row_count(), 0u);
    AcceptCounter counter{rig};
    enter_in(rig.outbound().table());
    EXPECT_EQ(counter.outbound + counter.inbound, 0);
    ASSERT_EQ(rig.actions.opened.size(), 1u);
    EXPECT_TRUE(rig.actions.opened[0].startsWith("https://www.wizzair.com/en-gb/booking/select-flight/"));
}

TEST(MainWindow, Enter_while_working_starts_nothing)
{
    WindowRigOptions options;
    options.keep_delays = true;
    WindowRig rig{std::move(options)};
    rig.start();
    rig.controller().spawn(rig.controller().search_outbound());
    ASSERT_TRUE(rig.controller().is_working());
    AcceptCounter counter{rig};
    enter_in(rig.input().min_nights_box());
    enter_in(rig.trips().same_home_box());
    EXPECT_EQ(counter.outbound + counter.inbound, 0);
    rig.controller().cancel_work();
    ASSERT_TRUE(gui::wait_until([&] { return !rig.controller().is_working(); }));
}

TEST(MainWindow, Esc_while_working_cancels_once_like_the_Cancel_button)
{
    WindowRigOptions options;
    options.keep_delays = true;
    WindowRig rig{std::move(options)};
    rig.start();
    int cancels = 0;
    QObject::connect(rig.window().busy_panel(), &wizz::app::BusyPanel::cancel_requested, [&] { ++cancels; });

    rig.controller().spawn(rig.controller().search_outbound());
    ASSERT_TRUE(gui::wait_until(
        [&] { return rig.busy().status_text().starts_with("Fetching fares"); }));

    // SetWorking (01 §6.4): exactly these controls are disabled; the rest stays usable.
    for (QWidget* control : rig.input().working_controls()) {
        EXPECT_FALSE(control->isEnabled()) << control->metaObject()->className();
    }
    EXPECT_TRUE(rig.input().min_nights_box()->isEnabled());
    EXPECT_TRUE(rig.input().max_nights_box()->isEnabled());
    EXPECT_TRUE(rig.window().tabs()->isEnabled());
    EXPECT_TRUE(rig.outbound().table()->isEnabled());
    EXPECT_TRUE(rig.outbound().view_combo()->isEnabled());
    EXPECT_TRUE(rig.trips().same_home_box()->isEnabled());
    EXPECT_TRUE(rig.window().log_view()->isEnabled());

    QWidget* target = rig.outbound().table();
    gui::send_key(target, QEvent::KeyPress, Qt::Key_Escape);
    EXPECT_EQ(rig.busy().status_text(), strings::kCancelling);
    EXPECT_FALSE(rig.busy().cancel_button()->isEnabled());
    gui::send_key(rig.input().min_nights_box(), QEvent::KeyPress, Qt::Key_Escape);
    rig.busy().cancel_button()->click();  // disabled: no effect
    rig.busy().request_cancel();
    EXPECT_EQ(cancels, 1) << "Esc and Cancel share one idempotent handler (D-11)";

    ASSERT_TRUE(gui::wait_until([&] { return !rig.controller().is_working(); }));
    EXPECT_TRUE(rig.busy().status_text().starts_with("Outbound Cancelled: 0 fares from 0 of 3 requests in "))
        << rig.busy().status_text();
    EXPECT_EQ(rig.busy().status_colour(), wizz::app::style::theme().failure);
    EXPECT_EQ(rig.outbound().summary_text(), "Cancelled before any fares arrived.");
    for (QWidget* control : rig.input().working_controls()) {
        EXPECT_TRUE(control->isEnabled());
    }
}

TEST(MainWindow, Esc_that_closes_a_drop_down_or_comes_while_idle_does_not_cancel)
{
    WindowRigOptions options;
    options.keep_delays = true;
    WindowRig rig{std::move(options)};
    rig.start();
    int cancels = 0;
    QObject::connect(rig.window().busy_panel(), &wizz::app::BusyPanel::cancel_requested, [&] { ++cancels; });

    gui::send_key(rig.outbound().table(), QEvent::KeyPress, Qt::Key_Escape);
    EXPECT_EQ(cancels, 0) << "not working: Esc does nothing";

    rig.controller().spawn(rig.controller().search_outbound());
    ASSERT_TRUE(rig.controller().is_working());
    QComboBox* view = rig.outbound().view_combo();
    view->showPopup();
    if (QApplication::activePopupWidget() == nullptr) {
        rig.controller().cancel_work();
        ASSERT_TRUE(gui::wait_until([&] { return !rig.controller().is_working(); }));
        GTEST_SKIP() << "this platform plugin opens no popup windows";
    }
    gui::send_key(view, QEvent::KeyPress, Qt::Key_Escape);  // WF-3: the window's Esc does not cancel
    EXPECT_EQ(cancels, 0);
    view->hidePopup();
    gui::settle();
    gui::send_key(view, QEvent::KeyPress, Qt::Key_Escape);
    EXPECT_EQ(cancels, 1);
    ASSERT_TRUE(gui::wait_until([&] { return !rig.controller().is_working(); }));
}

} // namespace
