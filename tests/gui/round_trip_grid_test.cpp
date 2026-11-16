// RoundTripGrid widget (01 §9): top bar and filters, columns and formats, the
// 03 §11 worked-example trips (MatchLegs → Combine through ui::recompute_trips),
// SetTrips resetting the selection to the top, the default sort with combiner
// ties and persistent header sorts, the open actions (one combined booking or
// two one-way bookings, CT-RT-01) through an injected opener, and the context
// menu. en-US.

#include "round_trip_grid.hpp"
#include "round_trip_table_model.hpp"
#include "grid_colors.hpp"
#include "grid_table_view.hpp"
#include "ui_style.hpp"

#include "wizz/ui/round_trip_rows.hpp"

#include "wizz/core/round_trips.hpp"

#include "grid_test_support.hpp"
#include "ui_logic/ui_samples.hpp"

#include <QAction>
#include <QCheckBox>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QScrollBar>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace wizz;
using namespace wizz::test::gui;
using app::RoundTripGrid;
using app::RoundTripTableModel;
using test::samples::builder_a;
using test::samples::day;
using ui::TripColumn;

[[nodiscard]] int col(TripColumn column)
{
    return RoundTripTableModel::column_index(column);
}

/// The 03 §11 trips: the CT-FSS-01 outbound leg and its synthetic GBP return leg.
[[nodiscard]] ui::TripsView section11_trips(int min_nights, int max_nights, bool same_home = false,
                                            bool same_away = false)
{
    const LegSearchResult out = test::ui::section11_outbound();
    const LegSearchResult ret = test::ui::section11_return();
    return ui::recompute_trips(&out, &ret, min_nights, max_nights, same_home, same_away, "EUR", test::ui::en());
}

/// CT-RT-01: outbound BUD→LTN d 20, BTS→LTN d+1 10; return LTN→BUD d+3 30, LGW→BUD d+5 15,
/// LTN→BTS d+20 1; stays of 2–5 nights → totals 25, 35, 40, 50.
[[nodiscard]] std::vector<RoundTrip> rt01_trips()
{
    const std::vector<FlightResultPtr> out{builder_a("BUD", "LTN", day(11, 10), "20"),
                                           builder_a("BTS", "LTN", day(11, 11), "10")};
    const std::vector<FlightResultPtr> ret{builder_a("LTN", "BUD", day(11, 13), "30"),
                                           builder_a("LGW", "BUD", day(11, 15), "15"),
                                           builder_a("LTN", "BTS", day(11, 30), "1")};
    return round_trips::combine(out, ret, 2, 5);
}

[[nodiscard]] std::string text_at(const RoundTripGrid& grid, int row, TripColumn column)
{
    return grid.model()->index(row, col(column)).data().toString().toStdString();
}

[[nodiscard]] int current_view_row(const RoundTripGrid& grid)
{
    return grid.table()->currentIndex().row();
}

void click_header(RoundTripGrid& grid, TripColumn column)
{
    QHeaderView* header = grid.table()->horizontalHeader();
    const int section = col(column);
    const QPoint pos{header->sectionViewportPosition(section) + header->sectionSize(section) / 2,
                     header->height() / 2};
    send_mouse(header->viewport(), QEvent::MouseButtonPress, pos, Qt::LeftButton);
    send_mouse(header->viewport(), QEvent::MouseButtonRelease, pos, Qt::LeftButton);
}

struct TripFixture {
    TripFixture() { grid.set_actions(recorder.actions()); }
    ActionRecorder recorder;
    RoundTripGrid grid{Locale::en_us()};
};

TEST(RoundTripGridWidget, Top_bar_checkboxes_and_summary_label)
{
    TripFixture f;
    EXPECT_EQ(f.grid.same_home_box()->text(), QStringLiteral("Return to the airport I left from"));
    EXPECT_EQ(f.grid.same_away_box()->text(), QStringLiteral("Fly back from the airport I landed at"));
    EXPECT_FALSE(f.grid.same_home_box()->isChecked());
    EXPECT_FALSE(f.grid.same_away_box()->isChecked());
    EXPECT_FALSE(f.grid.require_same_home());
    EXPECT_FALSE(f.grid.require_same_away());
    EXPECT_EQ(f.grid.summary_label()->palette().color(QPalette::WindowText), app::grid_colors::gray_text());
    EXPECT_EQ(f.grid.summary_label()->textFormat(), Qt::PlainText);
}

TEST(RoundTripGridWidget, Checkboxes_raise_filters_changed)
{
    TripFixture f;
    int changes = 0;
    QObject::connect(&f.grid, &RoundTripGrid::filters_changed, [&] { ++changes; });
    f.grid.same_home_box()->click();
    EXPECT_EQ(changes, 1);
    EXPECT_TRUE(f.grid.require_same_home());
    EXPECT_TRUE(f.grid.model()->state().require_same_home());
    f.grid.same_away_box()->setChecked(true);
    EXPECT_EQ(changes, 2);
    EXPECT_TRUE(f.grid.require_same_away());
    f.grid.same_home_box()->setChecked(true);  // unchanged: no event
    EXPECT_EQ(changes, 2);
    f.grid.same_home_box()->click();
    EXPECT_EQ(changes, 3);
    EXPECT_FALSE(f.grid.require_same_home());
}

TEST(RoundTripGridWidget, Booking_notes_fill_the_rest_but_keep_120_and_the_header_stays_one_line)
{
    // D-53: at WinForms' 5 px minimum the "Booking / notes" header wrapped into three
    // lines in a narrow window, and an 85 px "Return price" wrapped into two: every line
    // of header cost a round trip in view.
    TripFixture f;
    f.grid.set_trips(rt01_trips(), "EUR", "");
    f.grid.resize(1288, 400);  // the round-trip table of the default 1320 x 820 window
    f.grid.show();
    test::gui::settle();
    app::GridTableView& table = *f.grid.table();
    QHeaderView& header = *table.horizontalHeader();
    EXPECT_EQ(header.minimumHeight(), 0) << "every header text on one line at the default width";
    const int one_line = header.height();
    EXPECT_EQ(one_line, QFontMetrics(header.font()).height() + 2 * 5) << "one line and the 5 px padding";

    f.grid.resize(700, 400);
    test::gui::settle();
    EXPECT_EQ(header.sectionSize(col(TripColumn::booking_notes)), 120) << "min L(120), then horizontal scrolling";
    EXPECT_EQ(header.height(), one_line);
}

TEST(RoundTripGridWidget, Columns_headers_widths_and_formats)
{
    TripFixture f;
    const RoundTripTableModel& model = *f.grid.model();
    ASSERT_EQ(model.columnCount(), 10);
    std::vector<std::string> headers;
    for (int section = 0; section < model.columnCount(); ++section) {
        headers.push_back(model.headerData(section, Qt::Horizontal).toString().toStdString());
    }
    EXPECT_EQ(headers, (std::vector<std::string>{"Idx", "Total (EUR)", "Nights", "Out date", "Outbound", "Out price",
                                                 "Return date", "Return", "Return price", "Booking / notes"}));
    const app::GridTableView& table = *f.grid.table();
    EXPECT_TRUE(table.isColumnHidden(col(TripColumn::idx)));
    EXPECT_EQ(table.columnWidth(col(TripColumn::total)), 95);
    EXPECT_EQ(table.columnWidth(col(TripColumn::nights)), 55);
    EXPECT_EQ(table.columnWidth(col(TripColumn::out_date)), 95);
    EXPECT_EQ(table.columnWidth(col(TripColumn::outbound)), 300);
    EXPECT_EQ(table.columnWidth(col(TripColumn::out_price)), 80);
    EXPECT_EQ(table.columnWidth(col(TripColumn::return_date)), 95);
    EXPECT_EQ(table.columnWidth(col(TripColumn::return_leg)), 300);
    // "Return price" is 100 wide (01: 85) so its semibold header stays on one line, and
    // "Booking / notes" fills the rest but keeps 120 (01: WinForms' 5 px, which wrapped its
    // header into three lines): one header line, one more round trip in view (D-53).
    EXPECT_EQ(table.columnWidth(col(TripColumn::return_price)), 100);
    EXPECT_EQ(table.horizontalHeader()->font(), wizz::app::style::bold_form_font());
    EXPECT_EQ(table.selectionBehavior(), QAbstractItemView::SelectRows);
    EXPECT_EQ(table.selectionMode(), QAbstractItemView::SingleSelection);
    EXPECT_EQ(table.editTriggers(), QAbstractItemView::NoEditTriggers);
    EXPECT_FALSE(table.verticalHeader()->isVisible());

    EXPECT_EQ(RoundTripTableModel::alignment(TripColumn::total), Qt::AlignRight | Qt::AlignVCenter);
    EXPECT_EQ(RoundTripTableModel::alignment(TripColumn::nights), Qt::AlignHCenter | Qt::AlignVCenter);
    EXPECT_EQ(RoundTripTableModel::alignment(TripColumn::out_date), Qt::AlignLeft | Qt::AlignVCenter);
    EXPECT_EQ(RoundTripTableModel::alignment(TripColumn::outbound), Qt::AlignLeft | Qt::AlignVCenter);
    EXPECT_EQ(RoundTripTableModel::alignment(TripColumn::out_price), Qt::AlignRight | Qt::AlignVCenter);
    EXPECT_EQ(RoundTripTableModel::alignment(TripColumn::return_price), Qt::AlignRight | Qt::AlignVCenter);
    EXPECT_EQ(RoundTripTableModel::alignment(TripColumn::booking_notes), Qt::AlignLeft | Qt::AlignVCenter);

    f.grid.set_trips(rt01_trips(), "huf", "");
    EXPECT_EQ(model.headerData(col(TripColumn::total), Qt::Horizontal).toString(), QStringLiteral("Total (HUF)"));
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::total), "25");  // HUF: no minor unit
    const QModelIndex total = model.index(0, col(TripColumn::total));
    EXPECT_TRUE(total.data(Qt::FontRole).value<QFont>().bold());
    EXPECT_FALSE(model.index(0, col(TripColumn::out_price)).data(Qt::FontRole).isValid());
    EXPECT_FALSE(total.data(Qt::BackgroundRole).isValid());  // no per-row colouring
    EXPECT_FALSE(total.data(Qt::ForegroundRole).isValid());
}

TEST(RoundTripGridWidget, Worked_example_03_11_rows)
{
    TripFixture f;
    const ui::TripsView view = section11_trips(2, 5);
    f.grid.set_trips(view.trips, view.currency, view.summary);
    EXPECT_EQ(f.grid.count(), 19u);
    EXPECT_EQ(f.grid.trips().size(), 19u);
    EXPECT_EQ(f.grid.model()->rowCount(), 19);
    EXPECT_EQ(f.grid.summary_text(), "19 combinations · cheapest 73.06 EUR: BTS→LTN Thu 12 Nov, back "
                                     "LTN→BUD Tue 17 Nov (5 nights)");
    EXPECT_EQ(f.grid.summary_label()->text().toStdString(), f.grid.summary_text());

    EXPECT_EQ(text_at(f.grid, 0, TripColumn::total), "73.06");
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::nights), "5");
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::out_date), "Thu 12 Nov");
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::outbound), "07:15  BTS → LTN  (Bratislava → London Luton)");
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::out_price), "29.98");
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::return_date), "Tue 17 Nov");
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::return_leg), "21:35  LTN → BUD  (London Luton → Budapest)");
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::return_price), "43.08");
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::booking_notes), "two one-way bookings; back to BUD (left from BTS)");

    // Default display order: Total, then Out date, then the combiner order.
    for (int row = 1; row < f.grid.model()->rowCount(); ++row) {
        const ui::TripRow& previous = f.grid.model()->state().row(static_cast<std::size_t>(row - 1));
        const ui::TripRow& current = f.grid.model()->state().row(static_cast<std::size_t>(row));
        ASSERT_TRUE(*previous.total < *current.total ||
                    (*previous.total == *current.total &&
                     (previous.out_date < current.out_date ||
                      (previous.out_date == current.out_date && previous.idx < current.idx))))
            << row;
    }

    // Both filters: only same-airport trips, booked as one return ticket.
    const ui::TripsView same = section11_trips(2, 5, true, true);
    f.grid.set_trips(same.trips, same.currency, same.summary);
    ASSERT_GT(f.grid.count(), 0u);
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::total), "77.84");
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::booking_notes), "one booking");
}

TEST(RoundTripGridWidget, Set_trips_resets_the_selection_to_the_top)
{
    TripFixture f;
    f.grid.resize(1600, 220);
    f.grid.show();
    const ui::TripsView view = section11_trips(2, 5);
    f.grid.set_trips(view.trips, view.currency, view.summary);
    settle();
    app::GridTableView& table = *f.grid.table();
    EXPECT_EQ(current_view_row(f.grid), 0);
    EXPECT_EQ(table.currentIndex().column(), col(TripColumn::total));
    EXPECT_EQ(f.grid.selected(), &f.grid.trips()[f.grid.model()->state().row(0).idx]);

    click(table, cell_center(table, 3, col(TripColumn::outbound)));
    EXPECT_EQ(current_view_row(f.grid), 3);
    EXPECT_EQ(f.grid.selected(), &f.grid.trips()[f.grid.model()->state().row(3).idx]);
    table.verticalScrollBar()->setValue(table.verticalScrollBar()->maximum());
    settle();
    ASSERT_GT(table.verticalScrollBar()->value(), 0);

    f.grid.set_trips(view.trips, view.currency, "again");  // every recompute: back to the top
    EXPECT_EQ(current_view_row(f.grid), 0);
    EXPECT_EQ(table.currentIndex().column(), col(TripColumn::total));
    EXPECT_TRUE(table.selectionModel()->isRowSelected(0));
    EXPECT_EQ(table.verticalScrollBar()->value(), 0);
    EXPECT_EQ(f.grid.summary_label()->text(), QStringLiteral("again"));

    f.grid.set_trips({}, "EUR", "No combination with a stay of 9–10 nights. Change the nights or the dates.");
    EXPECT_EQ(f.grid.count(), 0u);
    EXPECT_EQ(f.grid.selected(), nullptr);
    EXPECT_FALSE(table.currentIndex().isValid());
    EXPECT_EQ(f.grid.summary_text(), "No combination with a stay of 9–10 nights. Change the nights or the dates.");
}

TEST(RoundTripGridWidget, Set_trips_scrolls_the_Total_cell_of_row_0_into_view)
{
    // 01 §9.3 "make row 0 current (at the Total column)": DataGridView.CurrentCell scrolls
    // that cell into view, so a grid scrolled right to read "Booking / notes" comes back left.
    TripFixture f;
    f.grid.resize(600, 300);
    f.grid.show();
    const ui::TripsView view = section11_trips(2, 5);
    f.grid.set_trips(view.trips, view.currency, view.summary);
    app::GridTableView& table = *f.grid.table();
    // The user widened "Outbound": the columns no longer fit and the grid scrolls horizontally
    // (whatever the width the two checkboxes need on this platform).
    table.setColumnWidth(col(TripColumn::outbound), 2 * f.grid.width());
    settle();
    QScrollBar& bar = *table.horizontalScrollBar();
    ASSERT_GT(bar.maximum(), 0);
    bar.setValue(bar.maximum());
    settle();
    ASSERT_EQ(bar.value(), bar.maximum());

    f.grid.set_trips(view.trips, view.currency, view.summary);  // a recompute (nights, filters)
    settle();
    EXPECT_EQ(current_view_row(f.grid), 0);
    EXPECT_EQ(table.currentIndex().column(), col(TripColumn::total));
    EXPECT_EQ(bar.value(), 0);
    EXPECT_TRUE(table.viewport()->rect().contains(table.visualRect(table.currentIndex())));
    EXPECT_EQ(table.verticalScrollBar()->value(), 0);
}

TEST(RoundTripGridWidget, Default_sort_ties_keep_the_combiner_order_and_header_sorts_persist)
{
    TripFixture f;
    f.grid.resize(1600, 400);
    f.grid.show();
    std::vector<RoundTrip> trips = rt01_trips();
    std::swap(trips[0], trips[3]);         // insertion order no longer sorted
    trips[1].total = test::ui::dec("50");  // ties with trips[0] on Total and Out date
    f.grid.set_trips(trips, "EUR", "");
    settle();
    std::vector<std::string> totals;
    for (int row = 0; row < 4; ++row) {
        totals.push_back(text_at(f.grid, row, TripColumn::total));
    }
    EXPECT_EQ(totals, (std::vector<std::string>{"25.00", "40.00", "50.00", "50.00"}));
    EXPECT_EQ(text_at(f.grid, 2, TripColumn::idx), "0");  // equal Total and Out date: insertion order
    EXPECT_EQ(text_at(f.grid, 3, TripColumn::idx), "1");

    const QHeaderView& header = *f.grid.table()->horizontalHeader();
    EXPECT_EQ(header.sortIndicatorSection(), -1);
    click(*f.grid.table(), cell_center(*f.grid.table(), 1, col(TripColumn::outbound)));
    const RoundTrip* picked = f.grid.selected();
    int layouts = 0;
    QObject::connect(f.grid.model(), &QAbstractItemModel::layoutChanged, [&] { ++layouts; });
    click_header(f.grid, TripColumn::nights);
    EXPECT_EQ(layouts, 1);
    EXPECT_EQ(header.sortIndicatorSection(), col(TripColumn::nights));
    EXPECT_EQ(header.sortIndicatorOrder(), Qt::AscendingOrder);
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::nights), "2");
    EXPECT_EQ(f.grid.selected(), picked);  // the current trip stays current
    EXPECT_EQ(f.grid.model()->state().current_row(),
              std::optional<std::size_t>{static_cast<std::size_t>(current_view_row(f.grid))});  // view == model
    click_header(f.grid, TripColumn::nights);
    EXPECT_EQ(header.sortIndicatorOrder(), Qt::DescendingOrder);
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::nights), "5");

    f.grid.set_trips(rt01_trips(), "EUR", "");  // the user sort persists
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::nights), "5");
    EXPECT_EQ(header.sortIndicatorSection(), col(TripColumn::nights));
}

TEST(RoundTripGridWidget, A_double_click_on_a_header_sorts_once)
{
    // The second click of a double-click is no header click in WinForms (01 §8.4, §9.1).
    TripFixture f;
    f.grid.resize(1600, 400);
    f.grid.show();
    f.grid.set_trips(rt01_trips(), "EUR", "");
    settle();
    int layouts = 0;
    QObject::connect(f.grid.model(), &QAbstractItemModel::layoutChanged, [&] { ++layouts; });
    QHeaderView& header = *f.grid.table()->horizontalHeader();

    double_click_header(header, col(TripColumn::nights));
    EXPECT_EQ(layouts, 1);
    ASSERT_TRUE(f.grid.model()->state().user_sort());
    EXPECT_EQ(f.grid.model()->state().user_sort()->column, TripColumn::nights);
    EXPECT_EQ(f.grid.model()->state().user_sort()->direction, ui::SortDirection::ascending);
    EXPECT_EQ(header.sortIndicatorSection(), col(TripColumn::nights));
    EXPECT_EQ(header.sortIndicatorOrder(), Qt::AscendingOrder);
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::nights), "2");

    click_header(f.grid, TripColumn::nights);  // the next click toggles
    EXPECT_EQ(layouts, 2);
    EXPECT_EQ(header.sortIndicatorOrder(), Qt::DescendingOrder);
    EXPECT_EQ(text_at(f.grid, 0, TripColumn::nights), "5");
}

TEST(RoundTripGridWidget, Tab_visits_the_grid_before_the_filter_checkboxes)
{
    // 01 §6.3: "Within a result tab, visit the grid before its view or filter controls."
    // The grid sits on a tab page the way MainWindow builds it.
    QWidget window;
    auto* tabs = new QTabWidget(&window);
    auto* grid = new RoundTripGrid(Locale::en_us(), tabs);
    tabs->addTab(grid, QStringLiteral("Round trips"));
    auto* layout = new QVBoxLayout(&window);
    layout->addWidget(tabs);
    window.resize(1200, 500);
    window.show();
    grid->set_trips(rt01_trips(), "EUR", "");
    settle();
    EXPECT_EQ(tab_stops(window), (std::vector<QWidget*>{tabs->tabBar(), grid->table(), grid->same_home_box(),
                                                        grid->same_away_box()}));

    // Tab on the tab strip goes to the grid.
    tabs->tabBar()->setFocus(Qt::TabFocusReason);
    ASSERT_EQ(window.focusWidget(), tabs->tabBar());
    send_key(tabs->tabBar(), QEvent::KeyPress, Qt::Key_Tab);
    EXPECT_EQ(window.focusWidget(), grid->table());
}

TEST(RoundTripGridWidget, CT_RT_01_Open_one_combined_booking_or_both_one_way_bookings)
{
    TripFixture f;
    f.grid.resize(1600, 400);
    f.grid.show();
    const std::vector<RoundTrip> trips = rt01_trips();
    f.grid.set_trips(trips, "EUR", "");
    settle();
    app::GridTableView& table = *f.grid.table();

    // Row 0: the 25 trip, two one-way bookings: outbound, then return (two tabs).
    double_click(table, cell_center(table, 0, col(TripColumn::outbound)));
    ASSERT_EQ(f.recorder.opened.size(), 2u);
    EXPECT_EQ(f.recorder.opened[0].toStdString(), trips[0].outbound->booking_url);
    EXPECT_EQ(f.recorder.opened[1].toStdString(), trips[0].inbound->booking_url);

    // Row 3: the 50 trip, same airports both ways: one combined booking (Enter).
    click(table, cell_center(table, 3, col(TripColumn::outbound)));
    EXPECT_TRUE(send_key(&table, QEvent::KeyPress, Qt::Key_Return));
    ASSERT_EQ(f.recorder.opened.size(), 3u);
    EXPECT_EQ(f.recorder.opened[2].toStdString(),
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/2026-11-13/1/0/0/null");
    EXPECT_EQ(text_at(f.grid, 3, TripColumn::booking_notes), "one booking");

    // The menu: always enabled; outbound only and return only.
    const QList<QAction*> actions = f.grid.context_menu()->actions();
    ASSERT_EQ(actions.size(), 3);
    EXPECT_EQ(actions[0]->text(), QStringLiteral("Open on wizzair.com"));
    EXPECT_EQ(actions[1]->text(), QStringLiteral("Open outbound only"));
    EXPECT_EQ(actions[2]->text(), QStringLiteral("Open return only"));
    for (const QAction* action : actions) {
        EXPECT_TRUE(action->isEnabled());
    }
    actions[1]->trigger();
    actions[2]->trigger();
    actions[0]->trigger();
    ASSERT_EQ(f.recorder.opened.size(), 6u);
    EXPECT_EQ(f.recorder.opened[3].toStdString(), trips[3].outbound->booking_url);
    EXPECT_EQ(f.recorder.opened[4].toStdString(), trips[3].inbound->booking_url);
    EXPECT_EQ(f.recorder.opened[5], f.recorder.opened[2]);

    // No current trip: the items stay enabled and do nothing.
    f.grid.set_trips({}, "EUR", "");
    for (QAction* action : f.grid.context_menu()->actions()) {
        EXPECT_TRUE(action->isEnabled());
        action->trigger();
    }
    f.grid.open_selected();
    EXPECT_EQ(f.recorder.opened.size(), 6u);
}

TEST(RoundTripGridWidget, Right_click_makes_the_row_current_and_opens_the_menu)
{
    TripFixture f;
    f.grid.resize(1600, 400);
    f.grid.show();
    f.grid.set_trips(rt01_trips(), "EUR", "");
    settle();
    app::GridTableView& table = *f.grid.table();
    const QPoint row2 = cell_center(table, 2, col(TripColumn::return_leg));
    click(table, row2, Qt::RightButton);
    EXPECT_EQ(current_view_row(f.grid), 2);
    EXPECT_EQ(table.currentIndex().column(), col(TripColumn::return_leg));
    context_menu_at(table, row2);
    EXPECT_TRUE(f.grid.context_menu()->isVisible());
    f.grid.context_menu()->hide();
    f.grid.context_menu()->actions()[2]->trigger();  // "Open return only" of the clicked trip
    ASSERT_EQ(f.recorder.opened.size(), 1u);
    EXPECT_EQ(f.recorder.opened[0].toStdString(), f.grid.selected()->inbound->booking_url);
}

TEST(RoundTripGridWidget, Browser_failure_shows_one_warning_per_url)
{
    TripFixture f;
    f.recorder.open_error = QStringLiteral("boom");
    int failures = 0;
    QObject::connect(&f.grid, &RoundTripGrid::open_url_failed, [&] { ++failures; });
    const std::vector<RoundTrip> trips = rt01_trips();
    f.grid.set_trips(trips, "EUR", "");
    f.grid.open_selected();  // row 0: two one-way bookings
    ASSERT_EQ(f.recorder.messages.size(), 2u);
    EXPECT_EQ(failures, 2);
    EXPECT_EQ(f.recorder.messages[0].first, QMessageBox::Warning);
    EXPECT_EQ(f.recorder.messages[0].second.toStdString(),
              "Could not open the browser:\nboom\n\n" + trips[0].outbound->booking_url);
    EXPECT_EQ(f.recorder.messages[1].second.toStdString(),
              "Could not open the browser:\nboom\n\n" + trips[0].inbound->booking_url);
}

} // namespace
