// FlightGrid widget, behaviour (01 §8.4, §8.6-§8.10, §18.4): header sorts
// with nulls and ties, view switching (LIVE-5), streaming notifications with
// the current row and the explicit pick (U-09), the context menu (UI-9),
// right-click / double-click / Enter, Copy details with the busy clipboard
// (UI-12), the browser failure box, summary texts and the self-test
// accessors. Side effects go to an ActionRecorder. en-US.

#include "elided_label.hpp"
#include "grid_bar_button.hpp"
#include "flight_grid_samples.hpp"
#include "grid_test_support.hpp"

#include "wizz/ui/strings.hpp"

#include "wizz/core/pricing.hpp"
#include "wizz/core/result_views.hpp"

#include <QAction>
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QScrollBar>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace wizz;
using namespace wizz::test::gui;
using app::FlightGrid;
using app::FlightTableModel;
using test::samples::builder_a;
using test::samples::day;
using ui::FlightColumn;
using ui::ViewMode;
using Fixture = GridFixture;

// ---------------------------------------------------------------------------
// Sorting (01 §8.4)
// ---------------------------------------------------------------------------

TEST(FlightGridBehaviour, Header_clicks_sort_one_column_with_nulls_first_and_stable_ties)
{
    Fixture f;
    f.grid.resize(1600, 500);
    f.grid.show();
    f.grid.set_results(checkprice_fares(), "EUR", "");
    settle();
    const QHeaderView& header = *f.grid.table()->horizontalHeader();
    EXPECT_EQ(header.sortIndicatorSection(), -1);  // the default sort shows no glyph
    EXPECT_EQ(f.grid.model()->fare_at(3)->fare.status, FareStatus::check_on_site);

    click_header(f.grid, FlightColumn::total);  // ascending first: the null total first
    EXPECT_EQ(header.sortIndicatorSection(), col(FlightColumn::total));
    EXPECT_EQ(header.sortIndicatorOrder(), Qt::AscendingOrder);
    EXPECT_EQ(f.grid.model()->fare_at(0)->fare.status, FareStatus::check_on_site);
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::total), "");
    click_header(f.grid, FlightColumn::total);  // then toggled
    EXPECT_EQ(header.sortIndicatorOrder(), Qt::DescendingOrder);
    EXPECT_EQ(f.grid.model()->fare_at(3)->fare.status, FareStatus::check_on_site);

    // Ties keep the insertion order in both directions.
    f.grid.set_results(fss01(), "EUR", "");
    click_header(f.grid, FlightColumn::date);
    EXPECT_EQ(route_at(f.grid, 0), "BUD>LTN 2026-11-10");
    EXPECT_EQ(route_at(f.grid, 2), "BUD>LTN 2026-11-12");
    EXPECT_EQ(route_at(f.grid, 3), "BTS>LTN 2026-11-12");
    click_header(f.grid, FlightColumn::date);
    EXPECT_EQ(route_at(f.grid, 0), "BUD>LTN 2026-11-20");
    EXPECT_EQ(route_at(f.grid, 8), "BUD>LTN 2026-11-12");
    EXPECT_EQ(route_at(f.grid, 9), "BTS>LTN 2026-11-12");
    click_header(f.grid, FlightColumn::from);  // another column starts ascending
    EXPECT_EQ(header.sortIndicatorSection(), col(FlightColumn::from));
    EXPECT_EQ(header.sortIndicatorOrder(), Qt::AscendingOrder);
    EXPECT_EQ(route_at(f.grid, 0), "BTS>LTN 2026-11-12");

    // The user's sort stays across rebuilds and later searches.
    f.grid.clear("EUR", false, "");
    f.grid.set_results(fss01(), "EUR", "");
    EXPECT_EQ(header.sortIndicatorSection(), col(FlightColumn::from));
    EXPECT_EQ(route_at(f.grid, 0), "BTS>LTN 2026-11-12");
    EXPECT_EQ(route_at(f.grid, 1), "BUD>LTN 2026-11-10");  // equal From: insertion order
}

TEST(FlightGridBehaviour, Header_sort_is_one_layout_change_and_keeps_the_current_fare)
{
    Fixture f;
    f.grid.resize(1600, 500);
    f.grid.show();
    f.grid.set_results(fss01(), "EUR", "");
    settle();
    app::GridTableView& table = *f.grid.table();
    click(table, cell_center(table, 5, col(FlightColumn::to)));
    const FlightResultPtr picked = f.grid.selected();
    int layouts = 0;
    int resets = 0;
    QObject::connect(f.grid.model(), &QAbstractItemModel::layoutChanged, [&] { ++layouts; });
    QObject::connect(f.grid.model(), &QAbstractItemModel::modelReset, [&] { ++resets; });
    click_header(f.grid, FlightColumn::date);
    EXPECT_EQ(layouts, 1);
    EXPECT_EQ(resets, 0);
    EXPECT_EQ(f.grid.selected(), picked);
    EXPECT_EQ(f.grid.model()->fare_at(current_view_row(f.grid)), picked);
    EXPECT_TRUE(table.selectionModel()->isRowSelected(current_view_row(f.grid)));
    EXPECT_EQ(table.currentIndex().column(), col(FlightColumn::to));
}

TEST(FlightGridBehaviour, A_double_click_on_a_header_sorts_once)
{
    // WinForms raises MouseDoubleClick, not MouseClick, for the second click of a
    // double-click, so the DataGridView sorts once: ascending first (01 §8.4).
    Fixture f;
    f.grid.resize(1600, 500);
    f.grid.show();
    f.grid.set_results(fss01(), "EUR", "");
    settle();
    int layouts = 0;
    QObject::connect(f.grid.model(), &QAbstractItemModel::layoutChanged, [&] { ++layouts; });
    QHeaderView& header = *f.grid.table()->horizontalHeader();

    double_click_header(header, col(FlightColumn::date));
    EXPECT_EQ(layouts, 1);
    ASSERT_TRUE(f.grid.model()->state().user_sort());
    EXPECT_EQ(f.grid.model()->state().user_sort()->column, FlightColumn::date);
    EXPECT_EQ(f.grid.model()->state().user_sort()->direction, ui::SortDirection::ascending);
    EXPECT_EQ(header.sortIndicatorSection(), col(FlightColumn::date));
    EXPECT_EQ(header.sortIndicatorOrder(), Qt::AscendingOrder);
    EXPECT_EQ(route_at(f.grid, 0), "BUD>LTN 2026-11-10");

    // The next click is a click again: it toggles.
    click_header(f.grid, FlightColumn::date);
    EXPECT_EQ(layouts, 2);
    EXPECT_EQ(f.grid.model()->state().user_sort()->direction, ui::SortDirection::descending);
    EXPECT_EQ(header.sortIndicatorOrder(), Qt::DescendingOrder);
}

// ---------------------------------------------------------------------------
// View modes (01 §8.6, LIVE-5, CT-RT-03)
// ---------------------------------------------------------------------------

TEST(FlightGridBehaviour, LIVE_5_View_switches_automatically_until_the_user_chooses_one)
{
    Fixture f;
    QComboBox* view = f.grid.view_combo();
    f.grid.clear("EUR", true, "");
    EXPECT_EQ(f.grid.mode(), ViewMode::cheapest_per_destination);
    EXPECT_EQ(view->currentIndex(), 2);
    f.grid.set_results(rt03_rows(), "EUR", "");
    EXPECT_EQ(f.grid.row_count(), 2u);  // one row per arrival group: LON and BCN
    EXPECT_EQ(f.grid.shown_results().size(), 2u);
    EXPECT_EQ(f.grid.all_results().size(), 4u);
    EXPECT_EQ(route_at(f.grid, 0), "BTS>LTN 2026-11-10");
    EXPECT_EQ(route_at(f.grid, 1), "BUD>BCN 2026-11-11");
    f.grid.clear("EUR", false, "");  // a return search is never anywhere
    EXPECT_EQ(f.grid.mode(), ViewMode::all_fares);
    EXPECT_EQ(view->currentIndex(), 0);

    // A user change of the combo is a choice for the rest of the session.
    f.grid.set_results(rt03_rows(), "EUR", "");
    send_key(view, QEvent::KeyPress, Qt::Key_Down);
    EXPECT_EQ(view->currentIndex(), 1);
    EXPECT_EQ(f.grid.mode(), ViewMode::cheapest_per_day);
    EXPECT_EQ(f.grid.row_count(), 2u);  // ordered by price, not by date (CT-RT-03)
    EXPECT_EQ(route_at(f.grid, 0), "BTS>LTN 2026-11-10");
    EXPECT_EQ(route_at(f.grid, 1), "BUD>LGW 2026-11-11");
    f.grid.clear("EUR", true, "");
    EXPECT_EQ(f.grid.mode(), ViewMode::cheapest_per_day);
    EXPECT_EQ(view->currentIndex(), 1);
    send_key(view, QEvent::KeyPress, Qt::Key_Up);  // back to All fares also counts
    EXPECT_EQ(f.grid.mode(), ViewMode::all_fares);
    f.grid.clear("EUR", true, "");
    EXPECT_EQ(f.grid.mode(), ViewMode::all_fares);
    EXPECT_EQ(view->currentIndex(), 0);
}

// ---------------------------------------------------------------------------
// Streaming and selection (01 §8.6, §8.7; U-09)
// ---------------------------------------------------------------------------

TEST(FlightGridBehaviour, All_fares_streaming_is_one_notification_per_batch_and_keeps_the_current_fare)
{
    Fixture f;
    f.grid.resize(1200, 190);
    f.grid.show();
    f.grid.clear("EUR", false, "Searching 3 route(s) from 3 airport(s)…");
    settle();
    int resets = 0;
    int inserts = 0;
    int layouts = 0;
    int repaints = 0;
    const FlightTableModel* model = f.grid.model();
    QObject::connect(model, &QAbstractItemModel::modelReset, [&] { ++resets; });
    QObject::connect(model, &QAbstractItemModel::rowsInserted, [&] { ++inserts; });
    QObject::connect(model, &QAbstractItemModel::layoutChanged, [&] { ++layouts; });
    QObject::connect(model, &QAbstractItemModel::dataChanged, [&] { ++repaints; });
    const std::vector<FlightResultPtr> fares = fss01();

    f.grid.append(std::span{fares}.first(11));  // the BUD response into the empty grid
    EXPECT_EQ(inserts, 1);
    EXPECT_EQ(layouts + resets, 0);
    EXPECT_EQ(model->rowCount(), 11);
    EXPECT_EQ(f.grid.selected(), fares[0]);  // the first row added becomes current
    const int before = current_view_row(f.grid);
    EXPECT_EQ(model->fare_at(before), fares[0]);
    EXPECT_TRUE(f.grid.table()->selectionModel()->isRowSelected(before));
    for (int row = 0; row < 4; ++row) {  // 105.32 × 4 are the cheapest so far
        EXPECT_EQ(model->index(row, 2).data(FlightTableModel::RowToneRole).toInt(),
                  static_cast<int>(ui::RowTone::cheapest));
    }
    settle();
    QScrollBar* scroll = f.grid.table()->verticalScrollBar();
    ASSERT_GE(scroll->maximum(), 3);
    scroll->setValue(3);
    settle();
    ASSERT_EQ(f.grid.table()->rowAt(0), 3);

    f.grid.append(std::span{fares}.subspan(11));  // BTS: cheaper, sorts above everything
    EXPECT_EQ(layouts, 1);
    EXPECT_EQ(inserts, 1);
    EXPECT_EQ(resets, 0);
    EXPECT_EQ(repaints, 1);  // the green moves to the new cheapest row
    EXPECT_EQ(route_at(f.grid, 0), "BTS>LTN 2026-11-12");
    EXPECT_EQ(f.grid.selected(), fares[0]);  // same fare, its index shifted
    EXPECT_EQ(current_view_row(f.grid), before + 1);
    EXPECT_TRUE(f.grid.table()->selectionModel()->isRowSelected(before + 1));
    EXPECT_EQ(scroll->value(), 3);  // the first displayed row index does not change
    EXPECT_EQ(f.grid.table()->rowAt(0), 3);
    EXPECT_EQ(f.grid.table()->verticalHeader()->count(), 12);
    EXPECT_EQ(model->index(0, 2).data(FlightTableModel::RowToneRole).toInt(), static_cast<int>(ui::RowTone::cheapest));
    EXPECT_EQ(model->index(1, 2).data(FlightTableModel::RowToneRole).toInt(), static_cast<int>(ui::RowTone::normal));
    EXPECT_EQ(f.grid.explicit_selection(), nullptr);  // the current row is not a pick

    f.grid.append({});  // nothing to add: no notification
    EXPECT_EQ(layouts + inserts + resets, 2);
}

TEST(FlightGridBehaviour, U_09_Streaming_returns_the_current_row_to_the_explicit_pick)
{
    Fixture f;
    f.grid.resize(1200, 600);
    f.grid.show();
    f.grid.clear("EUR", false, "");
    const std::vector<FlightResultPtr> fares = fss01();
    f.grid.append(std::span{fares}.first(6));
    settle();
    app::GridTableView& table = *f.grid.table();
    click(table, cell_center(table, 3, col(FlightColumn::from)));
    const FlightResultPtr pick = f.grid.model()->fare_at(3);
    EXPECT_EQ(f.grid.explicit_selection(), pick);
    EXPECT_EQ(f.grid.selected(), pick);

    // Arrow presses move the current row; without their key-up the pick stays.
    send_key(&table, QEvent::KeyPress, Qt::Key_Up);
    send_key(&table, QEvent::KeyPress, Qt::Key_Up);
    EXPECT_EQ(current_view_row(f.grid), 1);
    EXPECT_EQ(f.grid.selected(), f.grid.model()->fare_at(1));
    EXPECT_EQ(f.grid.explicit_selection(), pick);

    f.grid.append(std::span{fares}.subspan(6));
    EXPECT_EQ(f.grid.selected(), pick);  // back to the pick, wherever it is now
    EXPECT_EQ(f.grid.model()->fare_at(current_view_row(f.grid)), pick);

    // The key-up of a navigation key makes the current fare the pick.
    tap_key(&table, Qt::Key_Down);
    EXPECT_NE(f.grid.explicit_selection(), pick);
    EXPECT_EQ(f.grid.explicit_selection(), f.grid.selected());
}

TEST(FlightGridBehaviour, Clear_resets_the_pick_and_SetResults_keeps_view_sort_and_pick)
{
    Fixture f;
    f.grid.resize(1600, 300);
    f.grid.show();
    f.grid.clear("EUR", false, "");
    f.grid.set_results(fss01(), "EUR", "");
    settle();
    app::GridTableView& table = *f.grid.table();
    EXPECT_EQ(current_view_row(f.grid), 0);  // row 0 current, not a pick
    EXPECT_EQ(table.currentIndex().column(), col(FlightColumn::date));
    EXPECT_EQ(f.grid.explicit_selection(), nullptr);

    click(table, cell_center(table, 6, col(FlightColumn::from)));
    const FlightResultPtr pick = f.grid.selected();
    click_header(f.grid, FlightColumn::date);

    // Re-priced (new objects): view, sort and pick are kept; the pick's row is current again.
    std::vector<FlightResultPtr> repriced;
    for (const FlightResultPtr& fare : fss01()) {
        repriced.push_back(pricing::reprice(*fare, *test::ui::rates(), "HUF"));
    }
    f.grid.set_results(result_views::sort_by_price(repriced), "HUF", "re-priced");
    EXPECT_EQ(f.grid.mode(), ViewMode::all_fares);
    ASSERT_TRUE(f.grid.model()->state().user_sort());
    EXPECT_EQ(f.grid.model()->state().user_sort()->column, FlightColumn::date);
    EXPECT_EQ(f.grid.explicit_selection(), pick);
    ASSERT_TRUE(f.grid.selected());
    EXPECT_NE(f.grid.selected(), pick);
    EXPECT_EQ(f.grid.selected()->date(), pick->date());
    EXPECT_EQ(f.grid.selected()->from_airport->iata, pick->from_airport->iata);
    EXPECT_EQ(f.grid.model()->fare_at(current_view_row(f.grid)), f.grid.selected());
    EXPECT_EQ(f.grid.summary_text(), "re-priced");

    f.grid.clear("EUR", false, "");
    EXPECT_EQ(f.grid.explicit_selection(), nullptr);
    EXPECT_EQ(f.grid.row_count(), 0u);
    EXPECT_FALSE(table.currentIndex().isValid());
    EXPECT_EQ(f.grid.current_row_text(), "(none)");
    f.grid.set_results(fss01(), "EUR", "");
    EXPECT_EQ(f.grid.explicit_selection(), nullptr);
    EXPECT_EQ(current_view_row(f.grid), 0);
    EXPECT_EQ(table.verticalScrollBar()->value(), 0);  // no pick: row 0 and the top
}

TEST(FlightGridBehaviour, Rebuild_with_a_pick_returns_to_the_top_before_showing_the_pick)
{
    // 01 §8.6-§8.7: a full reload makes row 0 current and scrolls to the top;
    // RestoreSelection then only scrolls the pick into view from there. A pick
    // near the top must therefore end with the view at the top, even when the
    // user had scrolled down before the rebuild (review finding UI-SCROLL-01).
    Fixture f;
    f.grid.resize(1600, 220);  // a few rows visible, the 12 fares need scrolling
    f.grid.show();
    f.grid.set_results(fss01(), "EUR", "");
    settle();
    app::GridTableView& table = *f.grid.table();
    QScrollBar& bar = *table.verticalScrollBar();
    ASSERT_GT(bar.maximum(), 1);

    click(table, cell_center(table, 1, col(FlightColumn::from)));
    const FlightResultPtr pick = f.grid.selected();
    ASSERT_TRUE(pick);
    bar.setValue(bar.maximum());
    settle();
    ASSERT_EQ(bar.value(), bar.maximum());

    f.grid.set_results(fss01(), "EUR", "");  // full reload; the pick is restored by key
    settle();
    ASSERT_TRUE(f.grid.selected());
    EXPECT_EQ(f.grid.selected()->date(), pick->date());
    EXPECT_EQ(current_view_row(f.grid), 1);
    EXPECT_EQ(bar.value(), 0);  // top first, and row 1 is already in view
}

TEST(FlightGridBehaviour, Rebuild_without_a_pick_scrolls_the_Date_cell_of_row_0_into_view)
{
    // 01 §8.7 RestoreSelection: "make row 0 current [at the Date column]" scrolls that cell
    // into view (DataGridView.CurrentCell), so a grid scrolled to the right comes back left.
    Fixture f;
    f.grid.resize(600, 300);  // narrower than the columns: the grid scrolls horizontally
    f.grid.show();
    f.grid.set_results(fss01(), "EUR", "");
    settle();
    app::GridTableView& table = *f.grid.table();
    QScrollBar& bar = *table.horizontalScrollBar();
    ASSERT_GT(bar.maximum(), 0);
    bar.setValue(bar.maximum());
    settle();
    ASSERT_EQ(bar.value(), bar.maximum());

    f.grid.set_results(fss01(), "EUR", "");  // the end of a search with no pick
    settle();
    EXPECT_EQ(f.grid.explicit_selection(), nullptr);
    EXPECT_EQ(current_view_row(f.grid), 0);
    EXPECT_EQ(table.currentIndex().column(), col(FlightColumn::date));
    EXPECT_EQ(bar.value(), 0);
    EXPECT_TRUE(table.viewport()->rect().contains(table.visualRect(table.currentIndex())));
    EXPECT_EQ(table.verticalScrollBar()->value(), 0);
}

TEST(FlightGridBehaviour, A_grouped_flush_without_a_pick_keeps_the_horizontal_position)
{
    // 01 §8.6: a grouped-view flush is Rebuild(keepTopIfNotPicked=false). Without a pick it runs
    // no RestoreSelection: the reload makes row 0 current and returns to the top, but no cell is
    // made current through CurrentCell, so the grid stays scrolled to the right.
    Fixture f;
    f.grid.resize(600, 300);
    f.grid.show();
    f.grid.clear("EUR", true, "");  // anywhere: Cheapest per destination
    ASSERT_EQ(f.grid.mode(), ViewMode::cheapest_per_destination);
    const std::vector<FlightResultPtr> fares = fss01();
    f.grid.append(std::span{fares}.first(11));
    settle();
    app::GridTableView& table = *f.grid.table();
    QScrollBar& bar = *table.horizontalScrollBar();
    ASSERT_GT(bar.maximum(), 0);
    bar.setValue(bar.maximum());
    settle();

    f.grid.append(std::span{fares}.subspan(11));
    settle();
    EXPECT_EQ(f.grid.explicit_selection(), nullptr);
    EXPECT_EQ(current_view_row(f.grid), 0);
    EXPECT_EQ(table.verticalScrollBar()->value(), 0);
    EXPECT_EQ(bar.value(), bar.maximum());
}

TEST(FlightGridBehaviour, Cheapest_per_destination_restores_the_picks_group)
{
    Fixture f;
    f.grid.resize(1200, 400);
    f.grid.show();
    f.grid.clear("EUR", true, "");
    const std::vector<FlightResultPtr> rows = rt03_rows();
    f.grid.set_results(rows, "EUR", "");
    settle();
    ASSERT_EQ(f.grid.row_count(), 2u);
    app::GridTableView& table = *f.grid.table();
    click(table, cell_center(table, 1, col(FlightColumn::to)));  // BUD→BCN
    EXPECT_EQ(f.grid.explicit_selection(), rows[2]);

    const FlightResultPtr bcn = builder_a("BTS", "BCN", day(11, 12), "50");
    f.grid.set_results(std::vector<FlightResultPtr>{rows[0], rows[1], rows[3], bcn}, "EUR", "");
    EXPECT_EQ(f.grid.selected(), bcn);  // same ToAirport.GroupKey
    EXPECT_EQ(f.grid.model()->fare_at(current_view_row(f.grid)), bcn);

    // A flush in a grouped view is one full rebuild; the pick's group is restored again.
    int resets = 0;
    QObject::connect(f.grid.model(), &QAbstractItemModel::modelReset, [&] { ++resets; });
    f.grid.append(std::vector<FlightResultPtr>{builder_a("BTS", "LGW", day(11, 12), "5")});
    EXPECT_EQ(resets, 1);
    EXPECT_EQ(f.grid.selected()->to_airport->iata, "BCN");
    EXPECT_EQ(f.grid.all_results().size(), 5u);
}

// ---------------------------------------------------------------------------
// Mouse, keyboard and context menu (01 §8.8, §8.9)
// ---------------------------------------------------------------------------

TEST(FlightGridBehaviour, UI_9_Context_menu_texts_and_enabled_states)
{
    Fixture f;
    bool busy = false;
    f.grid.set_busy_query([&] { return busy; });
    QList<QAction*> actions = f.grid.prepare_context_menu()->actions();
    ASSERT_EQ(actions.size(), 5);
    EXPECT_EQ(actions[0]->text(), QStringLiteral("Open on wizzair.com"));
    EXPECT_EQ(actions[1]->text(), QStringLiteral("Copy details"));
    EXPECT_EQ(actions[2]->text(), QStringLiteral("Homes on Airbnb"));  // D-72
    EXPECT_TRUE(actions[3]->isSeparator());
    EXPECT_EQ(actions[4]->text(), QStringLiteral("Search return flights from this destination"));
    EXPECT_FALSE(actions[0]->isEnabled());  // no rows: Selected is null
    EXPECT_FALSE(actions[1]->isEnabled());
    EXPECT_FALSE(actions[2]->isEnabled());
    EXPECT_FALSE(actions[4]->isEnabled());

    f.grid.set_results(fss01(), "EUR", "");
    actions = f.grid.prepare_context_menu()->actions();
    EXPECT_TRUE(actions[0]->isEnabled());
    EXPECT_TRUE(actions[1]->isEnabled());
    EXPECT_FALSE(actions[2]->isEnabled()) << "no stay query: Homes on Airbnb stays disabled";
    EXPECT_TRUE(actions[4]->isEnabled());

    busy = true;
    actions = f.grid.prepare_context_menu()->actions();
    EXPECT_TRUE(actions[0]->isEnabled());
    EXPECT_TRUE(actions[1]->isEnabled());
    EXPECT_FALSE(actions[4]->isEnabled());
    EXPECT_EQ(actions[4]->text().toStdString(),
              "Search return flights from this destination (finish or cancel the current search first)");

    busy = false;
    actions = f.grid.prepare_context_menu()->actions();
    EXPECT_TRUE(actions[4]->isEnabled());
    EXPECT_EQ(actions[4]->text(), QStringLiteral("Search return flights from this destination"));
}

TEST(FlightGridBehaviour, Return_grid_menu_has_no_plan_return_item)
{
    Fixture f{false};
    f.grid.set_results(fss01(), "EUR", "");
    const QList<QAction*> actions = f.grid.prepare_context_menu()->actions();
    ASSERT_EQ(actions.size(), 3);
    EXPECT_EQ(actions[0]->text(), QStringLiteral("Open on wizzair.com"));
    EXPECT_EQ(actions[1]->text(), QStringLiteral("Copy details"));
    EXPECT_EQ(actions[2]->text(), QStringLiteral("Homes on Airbnb"));
    EXPECT_FALSE(f.grid.offers_plan_return());
}

TEST(FlightGridBehaviour, D_72_Homes_on_Airbnb_follows_the_current_rows_stay)
{
    Fixture f;
    f.grid.resize(1200, 500);
    f.grid.show();
    wizz::app::GridBarButton* button = f.grid.homes_button();
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(button->text(), QStringLiteral("Homes on Airbnb"));
    EXPECT_EQ(button->accessibleName(), QStringLiteral("Homes on Airbnb"));
    EXPECT_FALSE(button->isEnabled()) << "no query, no rows";

    std::vector<std::string> asked;  // the station codes the query was asked for
    bool usable = true;
    f.grid.set_homes_query([&](const wizz::FlightResult& fare) -> std::optional<wizz::ui::HomesStay> {
        asked.push_back(fare.to_airport->iata);
        if (!usable) {
            return std::nullopt;
        }
        return wizz::ui::HomesStay{.destination = fare.to_airport, .check_in = fare.date(),
                                   .check_out = fare.date().add_days(2), .adults = fare.adults};
    });
    EXPECT_FALSE(button->isEnabled()) << "still no rows";

    std::vector<wizz::ui::HomesStay> requested;
    QObject::connect(&f.grid, &wizz::app::FlightGrid::homes_requested,
                     [&](const wizz::ui::HomesStay& stay) { requested.push_back(stay); });
    f.grid.set_results(fss01(), "EUR", "");
    settle();
    ASSERT_TRUE(button->isEnabled());
    const wizz::FlightResultPtr first = f.grid.selected();
    ASSERT_NE(first, nullptr);

    button->click();
    ASSERT_EQ(requested.size(), 1u);
    EXPECT_EQ(requested[0].destination, first->to_airport);
    EXPECT_EQ(requested[0].check_in, first->date());
    EXPECT_EQ(requested[0].check_out, first->date().add_days(2));

    // The menu item raises the same request; a row without a stay disables both.
    f.grid.prepare_context_menu()->actions()[2]->trigger();
    EXPECT_EQ(requested.size(), 2u);
    usable = false;
    EXPECT_FALSE(f.grid.prepare_context_menu()->actions()[2]->isEnabled());
    EXPECT_FALSE(button->isEnabled());
    f.grid.request_homes();
    EXPECT_EQ(requested.size(), 2u) << "nothing to request";
    usable = true;

    // Moving to another row asks again for that row.
    asked.clear();
    tap_key(f.grid.table(), Qt::Key_Down);
    settle();
    ASSERT_FALSE(asked.empty());
    EXPECT_EQ(asked.back(), f.grid.selected()->to_airport->iata);
    EXPECT_TRUE(button->isEnabled());

    // Clearing the grid disables it.
    f.grid.clear("EUR", false);
    EXPECT_FALSE(button->isEnabled());
}

TEST(FlightGridBehaviour, D_72_The_homes_button_ends_the_top_bar_and_the_tab_order)
{
    Fixture f;
    f.grid.resize(1200, 500);
    f.grid.show();
    f.grid.set_homes_query([](const wizz::FlightResult& fare) {
        return std::optional{wizz::ui::HomesStay{
            .destination = fare.to_airport, .check_in = fare.date(), .check_out = fare.date().add_days(1), .adults = 1}};
    });
    f.grid.set_results(fss01(), "EUR", "");
    settle();
    const wizz::app::GridBarButton* button = f.grid.homes_button();
    EXPECT_GT(button->geometry().left(), f.grid.summary_label()->geometry().left());
    EXPECT_LE(button->geometry().right(), f.grid.width());
    EXPECT_LE(button->height(), f.grid.view_combo()->height()) << "it never makes the bar taller";
    EXPECT_EQ(button->focusPolicy(), Qt::StrongFocus);
    const std::vector<QWidget*> stops = tab_stops(f.grid);
    const auto combo = std::ranges::find(stops, f.grid.view_combo());
    ASSERT_NE(combo, stops.end());
    ASSERT_NE(std::next(combo), stops.end());
    EXPECT_EQ(*std::next(combo), button) << "table, view combo, then the button (01 §6.3)";
}

TEST(FlightGridBehaviour, Plan_return_raises_the_signal_with_the_selected_fare)
{
    Fixture f;
    f.grid.resize(1200, 500);
    f.grid.show();
    bool busy = false;
    f.grid.set_busy_query([&] { return busy; });
    std::vector<FlightResultPtr> requested;
    QObject::connect(&f.grid, &FlightGrid::plan_return_requested,
                     [&](const FlightResultPtr& fare) { requested.push_back(fare); });
    f.grid.set_results(fss01(), "EUR", "");
    settle();
    app::GridTableView& table = *f.grid.table();
    click(table, cell_center(table, 2, col(FlightColumn::from)), Qt::RightButton);
    QAction* plan = f.grid.prepare_context_menu()->actions().back();
    plan->trigger();
    ASSERT_EQ(requested.size(), 1u);
    EXPECT_EQ(requested[0], f.grid.model()->fare_at(2));

    busy = true;
    plan = f.grid.prepare_context_menu()->actions().back();
    plan->trigger();  // disabled while busy
    EXPECT_EQ(requested.size(), 1u);
}

TEST(FlightGridBehaviour, Right_click_makes_the_row_current_and_opens_the_menu)
{
    Fixture f;
    f.grid.resize(1600, 600);
    f.grid.show();
    f.grid.set_results(fss01(), "EUR", "");
    settle();
    app::GridTableView& table = *f.grid.table();
    QMenu* menu = f.grid.context_menu();

    const QPoint row4 = cell_center(table, 4, col(FlightColumn::to));
    send_mouse(table.viewport(), QEvent::MouseButtonPress, row4, Qt::RightButton);
    EXPECT_EQ(current_view_row(f.grid), 4);
    EXPECT_EQ(table.currentIndex().column(), col(FlightColumn::to));  // max(Date column, clicked column)
    EXPECT_EQ(f.grid.selected(), f.grid.model()->fare_at(4));
    EXPECT_EQ(f.grid.explicit_selection(), f.grid.model()->fare_at(4));
    send_mouse(table.viewport(), QEvent::MouseButtonRelease, row4, Qt::RightButton);
    context_menu_at(table, row4);
    EXPECT_TRUE(menu->isVisible());
    menu->hide();

    // Empty space below the rows: nothing changes and the menu acts on the current row.
    const QPoint empty{20, table.viewport()->height() - 3};
    ASSERT_FALSE(table.indexAt(empty).isValid());
    click(table, empty, Qt::RightButton);
    context_menu_at(table, empty);
    EXPECT_TRUE(menu->isVisible());
    EXPECT_TRUE(menu->actions()[0]->isEnabled());
    menu->hide();
    EXPECT_EQ(current_view_row(f.grid), 4);

    // The Menu key (and Shift+F10) open it too.
    QContextMenuEvent key{QContextMenuEvent::Keyboard, QPoint{}, table.mapToGlobal(QPoint{})};
    QApplication::sendEvent(&table, &key);
    EXPECT_TRUE(menu->isVisible());
    menu->hide();

    // Any other button picks without moving the current row.
    click(table, cell_center(table, 6, col(FlightColumn::from)), Qt::MiddleButton);
    EXPECT_EQ(f.grid.explicit_selection(), f.grid.model()->fare_at(6));
    EXPECT_EQ(current_view_row(f.grid), 4);
}

TEST(FlightGridBehaviour, Double_click_and_Enter_open_the_booking_url_and_Enter_stays_in_the_grid)
{
    struct KeySink final : QWidget {
        int presses = 0;

    protected:
        void keyPressEvent(QKeyEvent* event) override
        {
            ++presses;
            event->accept();
        }
    };
    KeySink window;
    ActionRecorder recorder;
    auto* grid = new FlightGrid(Locale::en_us(), true, &window);
    grid->set_actions(recorder.actions());
    grid->resize(1600, 600);
    window.resize(1600, 600);
    window.show();
    grid->set_results(fss01(), "EUR", "");
    settle();
    app::GridTableView& table = *grid->table();

    double_click(table, cell_center(table, 2, col(FlightColumn::from)));
    ASSERT_EQ(recorder.opened.size(), 1u);
    EXPECT_EQ(recorder.opened[0], QString::fromStdString(grid->model()->fare_at(2)->booking_url));
    EXPECT_TRUE(recorder.opened[0].endsWith(QStringLiteral("/BUD/LTN/2026-11-18/null/2/0/0/null")));

    // Enter claims the shortcut, opens the current fare and stays in the grid.
    QKeyEvent shortcut{QEvent::ShortcutOverride, Qt::Key_Return, Qt::NoModifier};
    shortcut.ignore();
    QApplication::sendEvent(&table, &shortcut);
    EXPECT_TRUE(shortcut.isAccepted());
    EXPECT_TRUE(send_key(&table, QEvent::KeyPress, Qt::Key_Return));
    EXPECT_TRUE(send_key(&table, QEvent::KeyPress, Qt::Key_Enter, Qt::KeypadModifier));
    EXPECT_EQ(recorder.opened.size(), 3u);
    EXPECT_EQ(recorder.opened[2], recorder.opened[0]);
    EXPECT_EQ(grid->table()->currentIndex().row(), 2);  // no move to the next row
    EXPECT_EQ(window.presses, 0);

    // Esc is not the grid's: it reaches the window (Esc == Cancel, D-11).
    send_key(&table, QEvent::KeyPress, Qt::Key_Escape);
    EXPECT_EQ(window.presses, 1);
}

TEST(FlightGridBehaviour, Typing_text_does_not_move_the_current_row)
{
    // A read-only DataGridView has no type-ahead search: only the keys of 01 §8.8 move
    // the current row (Selected, 01 §8.7).
    Fixture f;
    f.grid.resize(1600, 500);
    f.grid.show();
    f.grid.set_results(fss01(), "EUR", "");
    settle();
    app::GridTableView& table = *f.grid.table();
    ASSERT_EQ(current_view_row(f.grid), 0);
    ASSERT_EQ(table.currentIndex().column(), col(FlightColumn::date));
    const FlightResultPtr current = f.grid.selected();

    // The first letter of a later row's Date text ("Fri 13 Nov 2026" below "Thu 12 Nov 2026").
    const QString top = QString::fromStdString(text_at(f.grid, 0, FlightColumn::date));
    QString letter;
    for (int row = 1; row < f.grid.model()->rowCount() && letter.isEmpty(); ++row) {
        const QString text = QString::fromStdString(text_at(f.grid, row, FlightColumn::date));
        if (!text.isEmpty() && text.front() != top.front()) {
            letter = text.left(1);
        }
    }
    ASSERT_FALSE(letter.isEmpty());
    type_text(&table, letter);
    type_text(&table, letter.toLower());
    EXPECT_EQ(current_view_row(f.grid), 0);
    EXPECT_EQ(f.grid.selected(), current);
    EXPECT_EQ(f.grid.explicit_selection(), nullptr);
}

TEST(FlightGridBehaviour, Tab_visits_the_grid_before_the_view_combo)
{
    // 01 §6.3: "Within a result tab, visit the grid before its view or filter controls."
    // The grid sits on a tab page the way MainWindow builds it.
    QWidget window;
    auto* tabs = new QTabWidget(&window);
    auto* grid = new FlightGrid(Locale::en_us(), true, tabs);
    tabs->addTab(grid, QStringLiteral("Outbound"));
    auto* layout = new QVBoxLayout(&window);
    layout->addWidget(tabs);
    window.resize(1200, 500);
    window.show();
    grid->set_results(fss01(), "EUR", "");
    settle();
    EXPECT_EQ(tab_stops(window), (std::vector<QWidget*>{tabs->tabBar(), grid->table(), grid->view_combo()}));

    // Tab on the tab strip goes to the grid.
    tabs->tabBar()->setFocus(Qt::TabFocusReason);
    ASSERT_EQ(window.focusWidget(), tabs->tabBar());
    send_key(tabs->tabBar(), QEvent::KeyPress, Qt::Key_Tab);
    EXPECT_EQ(window.focusWidget(), grid->table());
}

TEST(FlightGridBehaviour, Browser_failure_shows_the_warning_box)
{
    Fixture f;
    f.grid.open_selected();  // no rows: nothing to open
    EXPECT_TRUE(f.recorder.messages.empty());

    f.recorder.open_error = QStringLiteral("Application not found");
    std::vector<QString> failed;
    QObject::connect(&f.grid, &FlightGrid::open_url_failed,
                     [&](const QString& url, const QString& message) { failed.push_back(url + '|' + message); });
    f.grid.set_results(fss01(), "EUR", "");
    f.grid.prepare_context_menu()->actions()[0]->trigger();  // "Open on wizzair.com"
    const std::string url = f.grid.selected()->booking_url;
    ASSERT_EQ(f.recorder.messages.size(), 1u);
    EXPECT_EQ(f.recorder.messages[0].first, QMessageBox::Warning);
    EXPECT_EQ(f.recorder.messages[0].second.toStdString(), "Could not open the browser:\nApplication not found\n\n" + url);
    ASSERT_EQ(failed.size(), 1u);
    EXPECT_EQ(failed[0].toStdString(), url + "|Application not found");
}

TEST(FlightGridBehaviour, Copy_details_puts_the_01_8_9_example_on_the_clipboard)
{
    Fixture f;
    const FlightResultPtr ksc =
        test::ui::hand_fare(test::ui::airport("KSC", "Kosice"), test::ui::airport("LTN", "London Luton"),
                            Date::from_ymd(2026, 12, 5), 20, 40, "29.99", "EUR", "29.99");
    f.grid.copy_selected();  // no rows: nothing to copy
    EXPECT_EQ(f.recorder.clipboard_attempts, 0);
    f.grid.set_results(std::vector<FlightResultPtr>{ksc}, "EUR", "");
    f.grid.prepare_context_menu()->actions()[1]->trigger();  // "Copy details"
    ASSERT_EQ(f.recorder.clipboard.size(), 1u);
    EXPECT_EQ(f.recorder.clipboard[0].toStdString(),
              "Sat 05 Dec 2026 20:40  Kosice (KSC) → London Luton (LTN)  29.99 EUR per person (total 29.99 EUR)"
              "\r\nhttps://www.wizzair.com/en-gb/booking/select-flight/KSC/LTN/2026-12-05/null/1/0/0/null");
    EXPECT_TRUE(f.recorder.messages.empty());
}

TEST(FlightGridBehaviour, UI_12_Busy_clipboard_is_retried_without_blocking_then_reported)
{
    using namespace std::chrono_literals;
    Fixture f;
    f.recorder.clipboard_busy = true;
    f.grid.set_results(fss01(), "EUR", "");
    f.grid.copy_selected();
    EXPECT_EQ(f.recorder.clipboard_attempts, 1);  // returns at once; the retries run on a timer
    EXPECT_TRUE(f.recorder.messages.empty());
    ASSERT_TRUE(wait_until([&] { return !f.recorder.messages.empty(); }));
    EXPECT_EQ(f.recorder.clipboard_attempts, app::kClipboardAttempts);
    EXPECT_EQ(f.recorder.messages[0].first, QMessageBox::Information);
    EXPECT_EQ(f.recorder.messages[0].second,
              QStringLiteral("The clipboard is busy (another program is using it). Try again."));

    // Freed while retrying: the next attempt succeeds and no box appears.
    f.recorder.messages.clear();
    f.recorder.clipboard_attempts = 0;
    f.grid.copy_selected();
    f.recorder.clipboard_busy = false;
    ASSERT_TRUE(wait_until([&] { return !f.recorder.clipboard.empty(); }));
    EXPECT_EQ(f.recorder.clipboard_attempts, 2);
    EXPECT_TRUE(f.recorder.messages.empty());
}

// ---------------------------------------------------------------------------
// Summary texts and self-test accessors (01 §8.10, §18.4)
// ---------------------------------------------------------------------------

TEST(FlightGridBehaviour, Summary_label_shows_the_18_4_texts)
{
    Fixture f;
    int changes = 0;
    QObject::connect(&f.grid, &FlightGrid::contents_changed, [&] { ++changes; });
    f.grid.clear("EUR", false, ui::strings::searching_routes(3, 3));
    EXPECT_EQ(f.grid.summary_label()->text().toStdString(), "Searching 3 route(s) from 3 airport(s)…");
    const std::vector<FlightResultPtr> fares = fss01();
    f.grid.append(std::span{fares}.first(11));
    f.grid.set_summary(ui::strings::fares_so_far(11));
    EXPECT_EQ(f.grid.summary_label()->text().toStdString(), "11 fares so far…");
    EXPECT_EQ(f.grid.summary_text(), "11 fares so far…");
    f.grid.set_results(fares, "EUR", "12 fares");
    EXPECT_EQ(f.grid.summary_label()->text(), QStringLiteral("12 fares"));
    EXPECT_EQ(changes, 3);
}

TEST(FlightGridBehaviour, Tab_titles_count_every_fare_not_the_rows_of_the_view)
{
    Fixture f;
    int changes = 0;
    QObject::connect(&f.grid, &FlightGrid::contents_changed, [&] { ++changes; });  // UpdateTabTitles
    const auto title = [&] { return ui::strings::tab_title("Outbound", f.grid.all_results().size()); };
    f.grid.clear("EUR", true, "");  // anywhere: Cheapest per destination
    EXPECT_EQ(title(), "Outbound");
    const std::vector<FlightResultPtr> rows = rt03_rows();
    f.grid.append(std::span{rows}.first(2));
    EXPECT_EQ(title(), "Outbound (2)");  // grows with each flush
    f.grid.append(std::span{rows}.subspan(2));
    EXPECT_EQ(f.grid.row_count(), 2u);
    EXPECT_EQ(title(), "Outbound (4)");  // all fares, not the 2 rows shown
    f.grid.set_results(std::span{rows}.first(3), "EUR", "");
    EXPECT_EQ(title(), "Outbound (3)");
    EXPECT_EQ(changes, 4);
}

TEST(FlightGridBehaviour, Self_test_accessors_of_01_8_10)
{
    Fixture f;
    EXPECT_EQ(f.grid.current_row_text(), "(none)");
    EXPECT_TRUE(f.grid.visible_rows_text(5).empty());
    f.grid.set_results(fss01(), "EUR", "12 fares");
    const std::vector<std::string> top = f.grid.visible_rows_text(2);
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0], "2026-11-12 07:15 BTS  Bratislava -> LTN  London Luton total=29.98 local=14.99 EUR");
    EXPECT_EQ(top[1], "2026-11-14 06:00 BUD  Budapest -> LTN  London Luton total=105.32 local=19,290 HUF");
    EXPECT_EQ(f.grid.visible_rows_text(100).size(), 12u);
    EXPECT_EQ(f.grid.current_row_text(), "2026-11-12 BTS->LTN 29.98");
    EXPECT_EQ(f.grid.row_count(), 12u);
    EXPECT_EQ(f.grid.mode(), ViewMode::all_fares);
}

} // namespace
