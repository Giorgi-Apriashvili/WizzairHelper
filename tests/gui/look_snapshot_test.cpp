// Renders the main window and every popup, dialog and state of the look (DECISIONS
// D-53..D-56) in light and dark to PNG files for a visual review, and checks the
// layout of each window it renders. Skipped unless WIZZ_GUI_SNAPSHOT_DIR names an
// output directory and the real "windows" platform runs (the minimal platform draws
// no text): run the test binary directly, optionally with QT_SCALE_FACTOR.
//
// Files (<scheme> = light, dark): <scheme>-window.png and -window-980x600.png (the
// client area), -combo-list, -place-list (the From list around "Stockholm"), -completer, -calendar, -context-menu, -tooltip,
// -messagebox-question, -messagebox-error, -chooser (with the title bar), -update-bar,
// -disabled, -hover, -focus-*; win10-light-window*.png (WIZZ_FONTS=windows10) and
// contrast-window*.png (the High Contrast fallback, probe forced on). Everything is drawn
// with the run's WIZZ_TEXT_RENDERING (D-58: the default, or hinted / natural to compare).

#include "app_test_support.hpp"
#include "busy_panel.hpp"
#include "controller_dialogs.hpp"
#include "date_picker.hpp"
#include "fractional_spin_box.hpp"
#include "place_combo.hpp"
#include "flight_grid.hpp"
#include "grid_row_delegate.hpp"
#include "heavy_replay.hpp"
#include "grid_table_view.hpp"
#include "layout_check.hpp"
#include "log_view.hpp"
#include "look_test_support.hpp"
#include "modern_style.hpp"
#include "place_chooser_dialog.hpp"
#include "round_trip_grid.hpp"
#include "search_button.hpp"
#include "ui_style.hpp"
#include "update_bar.hpp"
#include "window_capture.hpp"

#include "wizz/core/catalog.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/ui/strings.hpp"
#include "wizz/ui/update_model.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCalendarWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QHeaderView>
#include <QHoverEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QStyleOption>
#include <QTabBar>
#include <QTabWidget>
#include <QToolTip>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

namespace style = wizz::app::style;
namespace gui = wizz::test::gui;
using wizz::test::app::WindowRig;
using wizz::test::app::WindowRigOptions;

constexpr wizz::Date d(int year, unsigned month, unsigned day)
{
    return wizz::Date::from_ymd(year, month, day);
}

[[nodiscard]] QString snapshot_dir()
{
    return qEnvironmentVariable("WIZZ_GUI_SNAPSHOT_DIR");
}

void save(const QImage& image, const QString& name)
{
    const QString path = snapshot_dir() + QStringLiteral("/") + name + QStringLiteral(".png");
    EXPECT_TRUE(image.save(path)) << path.toStdString();
}

void pump()
{
    for (int i = 0; i < 4; ++i) {
        QApplication::processEvents();
    }
}

/// `window`'s client area with `popups` drawn where they are on screen, cropped to `focus`
/// (window coordinates, plus a margin) when that is valid.
[[nodiscard]] QImage compose(QWidget& window, const std::vector<QWidget*>& popups, QRect focus = {})
{
    pump();
    const qreal ratio = window.devicePixelRatioF();
    const QPoint origin = window.mapToGlobal(QPoint(0, 0));
    QRect area = window.rect();
    for (const QWidget* popup : popups) {
        area |= QRect(popup->mapToGlobal(QPoint(0, 0)) - origin, popup->size());
    }
    QImage image((QSizeF(area.size()) * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(QColor(128, 128, 128));
    {
        QPainter painter(&image);
        painter.translate(-area.topLeft());
        painter.drawPixmap(QPoint(0, 0), window.grab());
        for (QWidget* popup : popups) {
            painter.drawPixmap(popup->mapToGlobal(QPoint(0, 0)) - origin, popup->grab());
        }
    }
    if (!focus.isValid()) {
        return image;
    }
    for (const QWidget* popup : popups) {
        focus |= QRect(popup->mapToGlobal(QPoint(0, 0)) - origin, popup->size());
    }
    const QRect crop = (focus.adjusted(-20, -20, 20, 20) & area).translated(-area.topLeft());
    return image.copy(QRectF(QPointF(crop.topLeft()) * ratio, QSizeF(crop.size()) * ratio).toRect());
}

/// `widget`'s rectangle in `window` coordinates.
[[nodiscard]] QRect in_window(const QWidget& widget, const QWidget& window)
{
    return QRect(widget.mapTo(&window, QPoint(0, 0)), widget.size());
}

[[nodiscard]] QWidget* visible_tooltip()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("QTipLabel") && widget->isVisible()) {
            return widget;
        }
    }
    return nullptr;
}

/// A top-level window with its title bar (PrintWindow), or its client area.
[[nodiscard]] QImage capture_top_level(QWidget& widget)
{
    pump();
    if (const auto captured = wizz::app::capture_window(widget)) {
        return *captured;
    }
    return widget.grab().toImage();
}

[[nodiscard]] WindowRigOptions options_for(QSize outer)
{
    WindowRigOptions options;
    const qreal ratio = qApp->devicePixelRatio();
    options.settings.window_width = qRound(outer.width() * ratio);  // settings.json holds device pixels
    options.settings.window_height = qRound(outer.height() * ratio);
    return options;
}

/// The 03 §11 searches: outbound, return, round trips.
void run_searches(WindowRig& rig)
{
    rig.input().set_from_radius_km(200);
    rig.input().set_to_radius_km(30);
    rig.input().apply_autotest_dates(wizz::ui::DateWindow{.start = d(2026, 11, 10), .end = d(2026, 11, 20)},
                                     wizz::ui::DateWindow{.start = d(2026, 11, 12), .end = d(2026, 11, 20)});
    rig.run(rig.controller().search_outbound());
    rig.run(rig.controller().search_return());
}

[[nodiscard]] int visible_rows(const wizz::app::GridTableView& table)
{
    return table.viewport()->height() / table.row_height();
}

/// "Header: n, ..." for the columns with cells whose text is cut (elided) among the rows in view.
[[nodiscard]] std::string cut_cells(const wizz::app::GridTableView& table)
{
    const QAbstractItemModel& model = *table.model();
    std::string text;
    for (int column = 0; column < model.columnCount(); ++column) {
        if (table.isColumnHidden(column)) {
            continue;
        }
        int cut = 0;
        for (int row = 0; row < model.rowCount(); ++row) {
            const QModelIndex index = model.index(row, column);
            const QRect rect = table.visualRect(index);
            if (!rect.isValid() || rect.top() >= table.viewport()->height()) {
                continue;
            }
            QStyleOptionViewItem option;
            option.rect = rect;
            option.text = index.data(Qt::DisplayRole).toString();
            option.font = table.font();
            if (const QVariant font = index.data(Qt::FontRole); font.isValid()) {
                option.font = qvariant_cast<QFont>(font).resolve(table.font());
            }
            option.fontMetrics = QFontMetrics(option.font);
            if (!option.text.isEmpty() && wizz::app::GridRowDelegate::text_truncated(option, &table)) {
                ++cut;
            }
        }
        if (cut > 0) {
            text += (text.empty() ? "" : ", ") + model.headerData(column, Qt::Horizontal).toString().toStdString() + ": " +
                    std::to_string(cut);
        }
    }
    return text.empty() ? std::string{"none"} : text;
}

void expect_clean_layout(WindowRig& rig, const std::string& what)
{
    for (const wizz::app::MainWindow::Page page :
         {wizz::app::MainWindow::Page::outbound, wizz::app::MainWindow::Page::round_trips,
          wizz::app::MainWindow::Page::log}) {
        rig.window().select_page(page);
        pump();
        EXPECT_EQ(wizz::app::layout_problems(rig.window()), std::vector<std::string>{})
            << what << ", page " << static_cast<int>(page);
    }
}

/// The main window at the default and the minimum size: layout checked, client area saved.
void render_windows(const QString& prefix, bool count_rows)
{
    {
        WindowRig rig;
        rig.start();
        run_searches(rig);
        rig.window().select_page(wizz::app::MainWindow::Page::round_trips);
        pump();
        const int rows = visible_rows(*rig.trips().table());
        const wizz::app::GridTableView& trips = *rig.trips().table();
        std::printf("%s: %dx%d client, %d round-trip rows in view (viewport %d, row %d, header %d, font %s)\n",
                    qPrintable(prefix), rig.window().width(), rig.window().height(), rows, trips.viewport()->height(),
                    trips.row_height(), trips.horizontalHeader()->height(), qPrintable(trips.font().toString()));
        std::printf("%s: cut cells in view: round trips %s; outbound %s\n", qPrintable(prefix), cut_cells(trips).c_str(),
                    cut_cells(*rig.outbound().table()).c_str());
        // The default 1320 x 820 window, unless a small screen (or a large scale) clamped it.
        const QSize outer = rig.window().normal_outer_device_size();
        const QSize wanted = wizz::app::MainWindow::kDefaultSize * rig.window().devicePixelRatioF();
        const bool full_size = std::abs(outer.width() - wanted.width()) <= 3 && std::abs(outer.height() - wanted.height()) <= 3;
        if (count_rows && full_size) {
            EXPECT_GE(rows, 14) << "round trips in view at the default size";
        }
        save(compose(rig.window(), {}), prefix + QStringLiteral("-window"));
        expect_clean_layout(rig, prefix.toStdString() + " default size");
    }
    {
        WindowRig rig{options_for(wizz::app::MainWindow::kMinimumSize)};
        rig.start();
        run_searches(rig);
        rig.window().select_page(wizz::app::MainWindow::Page::round_trips);
        save(compose(rig.window(), {}), prefix + QStringLiteral("-window-980x600"));
        expect_clean_layout(rig, prefix.toStdString() + " 980x600");
    }
}

void render_surfaces(const QString& scheme)
{
    WindowRig rig;
    rig.start();
    run_searches(rig);
    QWidget& window = rig.window();
    wizz::app::InputPanel& input = rig.input();
    const auto name = [&scheme](const char* surface) { return scheme + QStringLiteral("-") + QLatin1String(surface); };

    // Combo drop-down list.
    QComboBox* fare = input.fare_box();
    fare->showPopup();
    pump();
    if (QWidget* popup = fare->view()->window(); popup->isVisible()) {
        save(compose(window, {popup}, in_window(*fare, window)), name("combo-list"));
    }
    fare->hidePopup();

    // The From place list around "Stockholm" (the names of the 2.0.0 font report), with the
    // suggestions of the frozen live map, as the app shows them.
    wizz::app::PlaceCombo* from_list = input.from_box();
    const QStringList rig_names = from_list->suggestions();
    const std::vector<std::string> live_names =
        wizz::AirportCatalog::from_map_json(wizz::test::app::read_file(wizz::test::app::live_map_path()),
                                            wizz::Locale::en_us())
            ->suggestion_names();
    input.set_suggestions(live_names);
    from_list->setCurrentIndex(std::max(0, from_list->findText(QStringLiteral("Stockholm"))));
    from_list->showPopup();
    pump();
    from_list->view()->scrollTo(from_list->model()->index(std::max(0, from_list->findText(QStringLiteral("Split"))), 0),
                                QAbstractItemView::PositionAtTop);  // Split, Stavanger, Stockholm, ... as reported
    pump();
    if (QWidget* popup = from_list->view()->window(); popup->isVisible()) {
        save(compose(window, {popup}, in_window(*from_list, window)), name("place-list"));
    }
    from_list->hidePopup();
    std::vector<std::string> names;
    for (const QString& rig_name : rig_names) {
        names.push_back(rig_name.toStdString());
    }
    input.set_suggestions(names);
    input.set_from_text("Budapest");

    // Place completer.
    QLineEdit* from = input.from_box()->lineEdit();
    from->setFocus(Qt::OtherFocusReason);
    from->clear();
    gui::type_text(from, QStringLiteral("Bu"));
    pump();
    if (QWidget* popup = input.from_box()->completer()->popup(); popup->isVisible()) {
        save(compose(window, {popup}, in_window(*input.from_box(), window)), name("completer"));
        popup->hide();
    }
    input.set_from_text("Budapest");

    // Calendar drop-down: a click on the picker's arrow.
    wizz::app::DatePicker* picker = input.out_from_picker();
    picker->setFocus(Qt::TabFocusReason);
    const QPoint arrow(picker->width() - 12, picker->height() / 2);
    gui::send_mouse(picker, QEvent::MouseButtonPress, arrow, Qt::LeftButton);
    gui::send_mouse(picker, QEvent::MouseButtonRelease, arrow, Qt::LeftButton);
    pump();
    if (QWidget* popup = picker->calendarWidget()->window(); popup != &window && popup->isVisible()) {
        save(compose(window, {popup}, in_window(*picker, window)), name("calendar"));
        popup->hide();
    }

    // Context menu of the outbound grid with "Copy details" under the mouse.
    rig.window().select_page(wizz::app::MainWindow::Page::outbound);
    pump();
    wizz::app::GridTableView* table = rig.outbound().table();
    const QPoint cell = gui::cell_center(*table, 1, 2);
    gui::context_menu_at(*table, cell);
    pump();
    if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
        menu->setActiveAction(menu->actions().value(1));
        save(compose(window, {menu}, QRect(table->viewport()->mapTo(&window, cell), QSize(1, 1))), name("context-menu"));
        menu->hide();
    }

    // Tooltip of the From box.
    QToolTip::showText(input.from_box()->mapToGlobal(QPoint(40, input.from_box()->height())),
                       input.from_box()->toolTip(), input.from_box());
    pump();
    if (QWidget* tip = visible_tooltip()) {
        save(compose(window, {tip}, in_window(*input.from_box(), window)), name("tooltip"));
    }
    QToolTip::hideText();

    // Message boxes (the controller's own builders) and the place chooser, title bars included.
    {
        const auto box = wizz::app::make_big_search_box(
            QString::fromStdString(wizz::ui::strings::big_search(612, "11 minutes")), &window);
        box->open();
        save(capture_top_level(*box), name("messagebox-question"));
        box->done(QMessageBox::No);
    }
    {
        const auto box = wizz::app::make_error_box(
            QStringLiteral("Could not find “Atlantis”. Check the spelling, or try a nearby city."), &window);
        box->open();
        save(capture_top_level(*box), name("messagebox-error"));
        box->done(QMessageBox::Ok);
    }
    {
        std::vector<wizz::Place> candidates;
        for (const char* place : {"London, United Kingdom", "London, Ontario, Canada", "London, Kentucky, United States"}) {
            candidates.push_back(wizz::Place{.name = "London", .description = place});
        }
        wizz::app::PlaceChooserDialog chooser("London", "destination", candidates, std::nullopt, &window);
        chooser.show();
        save(capture_top_level(chooser), name("chooser"));
        chooser.reject();
    }

    // Update bar (an installed copy with a verified installer).
    wizz::app::UpdateBar* bar = rig.window().update_bar();
    bar->show_view(wizz::ui::UpdateBarView{
        .visible = true,
        .text = "WizzairHelper 1.1.0 is ready to install. It will be installed when you close WizzairHelper.",
        .action = std::string{wizz::ui::update_strings::kRestartAndUpdate},
        .action_enabled = true});
    pump();
    save(compose(window, {}, QRect(0, 0, window.width(), in_window(*bar, window).bottom() + 60)), name("update-bar"));

    // Keyboard focus, after a Tab press (State_KeyboardFocusChange).
    window.setAttribute(Qt::WA_KeyboardFocusChange);
    const auto focus_shot = [&](QWidget* target, const char* surface, QRect area = {}) {
        target->setFocus(Qt::TabFocusReason);
        pump();
        window.setAttribute(Qt::WA_KeyboardFocusChange);  // as after a Tab press
        target->update();
        save(compose(window, {}, area.isValid() ? area : in_window(*target, window)), name(surface));
    };
    focus_shot(bar->later_button(), "focus-later");
    bar->hide();
    focus_shot(input.search_outbound_button(), "focus-search");
    focus_shot(fare, "focus-combo");
    focus_shot(input.min_nights_box(), "focus-spinbox");
    focus_shot(rig.window().tabs()->tabBar(), "focus-tabs");
    focus_shot(table, "focus-grid", QRect(0, in_window(*table, window).top(), window.width() / 2, 120));
    rig.window().select_page(wizz::app::MainWindow::Page::round_trips);
    focus_shot(rig.trips().same_home_box(), "focus-checkbox");
    rig.window().select_page(wizz::app::MainWindow::Page::log);
    focus_shot(rig.window().log_view(), "focus-log", QRect(0, in_window(*rig.window().log_view(), window).top(),
                                                          window.width() / 2, 160));

    // Hover: the primary button, a combo, a check box and a header section.
    rig.window().select_page(wizz::app::MainWindow::Page::round_trips);
    window.setFocus(Qt::OtherFocusReason);
    pump();
    for (QWidget* hovered : std::vector<QWidget*>{input.search_return_button(), input.currency_box(),
                                                  rig.trips().same_away_box()}) {
        hovered->setAttribute(Qt::WA_UnderMouse);
        hovered->update();
    }
    QHeaderView* header = rig.trips().table()->horizontalHeader();
    const QPoint section = gui::header_center(*header, 1);
    QHoverEvent hover{QEvent::HoverMove, QPointF(section), QPointF(header->mapToGlobal(section)), QPointF(-1, -1)};
    QApplication::sendEvent(header->viewport(), &hover);
    header->viewport()->update();
    save(compose(window, {}), name("hover"));
    for (QWidget* hovered : std::vector<QWidget*>{input.search_return_button(), input.currency_box(),
                                                  rig.trips().same_away_box()}) {
        hovered->setAttribute(Qt::WA_UnderMouse, false);
    }

}

/// Working: the inputs and Search buttons disabled, the busy panel running (a server that
/// takes 30 s), Cancel focused from the keyboard.
void render_working(const QString& scheme)
{
    WindowRigOptions options;
    options.scenario = "cancel_waiting";
    options.keep_delays = true;
    WindowRig rig{std::move(options)};
    rig.start();
    rig.input().set_from_radius_km(200);
    rig.input().set_to_radius_km(30);
    rig.input().apply_autotest_dates(wizz::ui::DateWindow{.start = d(2026, 11, 10), .end = d(2026, 11, 20)},
                                     std::nullopt);
    rig.controller().spawn(rig.controller().search_outbound());
    ASSERT_TRUE(gui::wait_until([&rig] { return rig.busy().is_busy() && rig.busy().cancel_button()->isEnabled(); }));
    ASSERT_TRUE(gui::wait_until([&rig] { return rig.busy().status_text().starts_with("Fetching"); }));
    rig.busy().cancel_button()->setFocus(Qt::TabFocusReason);
    pump();
    rig.window().setAttribute(Qt::WA_KeyboardFocusChange);
    save(compose(rig.window(), {}), scheme + QStringLiteral("-disabled"));
    rig.controller().cancel_work();
    ASSERT_TRUE(gui::wait_until([&rig] { return !rig.controller().is_working(); }));
}

TEST(LookSnapshots, Render_every_surface_in_both_schemes)
{
    if (snapshot_dir().isEmpty() || QGuiApplication::platformName() != QStringLiteral("windows")) {
        GTEST_SKIP() << "set WIZZ_GUI_SNAPSHOT_DIR and run on the windows platform to render the look";
    }
    QDir{}.mkpath(snapshot_dir());
    // Popups appear at once (no roll-out or fade), so they can be captured.
    for (const Qt::UIEffect effect : {Qt::UI_AnimateCombo, Qt::UI_AnimateMenu, Qt::UI_FadeMenu, Qt::UI_AnimateTooltip,
                                      Qt::UI_FadeTooltip}) {
        QApplication::setEffectEnabled(effect, false);
    }
    const QByteArray rendering = qgetenv("WIZZ_TEXT_RENDERING");
    const wizz::test::look::LookGuard guard;  // puts the suite's look back afterwards
    if (!rendering.isEmpty()) {
        qputenv("WIZZ_TEXT_RENDERING", rendering);  // the run's rendering, to compare renderings
    }

    for (const char* scheme : {"light", "dark"}) {
        qputenv("WIZZ_COLOR_SCHEME", scheme);
        ASSERT_TRUE(style::apply_application_look());
        render_windows(QString::fromLatin1(scheme), true);
        render_surfaces(QString::fromLatin1(scheme));
        render_working(QString::fromLatin1(scheme));
    }

    // Windows 10: Segoe UI, Consolas and Segoe MDL2 Assets.
    qputenv("WIZZ_COLOR_SCHEME", "light");
    qputenv("WIZZ_FONTS", "windows10");
    ASSERT_TRUE(style::apply_application_look());
    render_windows(QStringLiteral("win10-light"), true);
    qunsetenv("WIZZ_FONTS");

    // High Contrast: the system palette and the plain native style.
    style::set_high_contrast_probe([] { return true; });
    ASSERT_TRUE(style::apply_application_look());
    ASSERT_EQ(style::appearance(), style::Appearance::high_contrast);
    render_windows(QStringLiteral("contrast"), false);
}

} // namespace
