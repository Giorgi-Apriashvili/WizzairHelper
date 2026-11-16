// FlightGrid widget, static part (01 §8.1-§8.3, §8.5): top bar, columns,
// headers and widths, the fill column, model contents and formats for the
// CT-FSS-01 fares (core pricing over the fixture map CAT and RATES), notes,
// null cells and row colours. en-US.

#include "elided_label.hpp"
#include "flight_grid_samples.hpp"
#include "grid_colors.hpp"
#include "grid_row_delegate.hpp"
#include "grid_test_support.hpp"
#include "modern_style.hpp"
#include "ui_style.hpp"

#include "wizz/core/pricing.hpp"
#include "wizz/core/result_views.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QHeaderView>
#include <QImage>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QStyleOptionViewItem>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace wizz;
using namespace wizz::test::gui;
using app::FlightTableModel;
using test::samples::builder_a;
using test::samples::day;
using ui::FlightColumn;
using Fixture = GridFixture;

// ---------------------------------------------------------------------------
// Layout, columns and headers (01 §8.1, §8.2)
// ---------------------------------------------------------------------------

TEST(FlightGridWidget, Top_bar_texts_and_metrics)
{
    Fixture f;
    const QComboBox* view = f.grid.view_combo();
    ASSERT_EQ(view->count(), 3);
    EXPECT_EQ(view->itemText(0), QStringLiteral("All fares"));
    EXPECT_EQ(view->itemText(1), QStringLiteral("Cheapest per day"));
    EXPECT_EQ(view->itemText(2), QStringLiteral("Cheapest per destination"));
    EXPECT_EQ(view->currentIndex(), 0);
    EXPECT_EQ(view->minimumWidth(), 190);
    EXPECT_EQ(view->maximumWidth(), 190);
    EXPECT_FALSE(view->isEditable());
    const QList<QLabel*> labels = f.grid.findChildren<QLabel*>();
    EXPECT_TRUE(std::ranges::any_of(labels, [](const QLabel* l) { return l->text() == QStringLiteral("Show:"); }));
    EXPECT_EQ(f.grid.summary_label()->palette().color(QPalette::WindowText), app::grid_colors::gray_text());
    // One line that never widens the window: a summary longer than the bar ends with "…" and
    // shows in full as the tooltip.
    f.grid.set_summary("12 fares · cheapest 14.99 EUR: BTS → LTN on Thu 12 Nov, then BUD → LTN on Sat 14 Nov");
    app::ElidedLabel* summary = f.grid.summary_label();
    summary->resize(120, summary->height());
    EXPECT_TRUE(summary->is_elided());
    EXPECT_EQ(summary->tooltip_text(), summary->text());
    EXPECT_EQ(summary->sizePolicy().horizontalPolicy(), QSizePolicy::Ignored);
}

TEST(FlightGridWidget, Columns_headers_widths_and_flags)
{
    Fixture f;
    const FlightTableModel& model = *f.grid.model();
    ASSERT_EQ(model.columnCount(), 13);
    std::vector<std::string> headers;
    for (int section = 0; section < model.columnCount(); ++section) {
        headers.push_back(model.headerData(section, Qt::Horizontal).toString().toStdString());
    }
    EXPECT_EQ(headers, (std::vector<std::string>{"Idx", "PriceSort", "Date", "Departs", "From", "km from start", "To",
                                                 "km from dest.", "Total (EUR)", "Per person (EUR)", "Fare (local)",
                                                 "All departures", "Notes"}));
    const app::GridTableView& table = *f.grid.table();
    EXPECT_TRUE(table.isColumnHidden(col(FlightColumn::idx)));
    EXPECT_TRUE(table.isColumnHidden(col(FlightColumn::price_sort)));
    EXPECT_FALSE(table.isColumnHidden(col(FlightColumn::km_from_dest)));
    // The widths of 01 §8.2, adjusted for the look's fonts and cell padding (D-53): Date,
    // Departs and All departures wider, From and To narrower.
    EXPECT_EQ(table.columnWidth(col(FlightColumn::date)), 124);
    EXPECT_EQ(table.columnWidth(col(FlightColumn::departs)), 64);
    EXPECT_EQ(table.columnWidth(col(FlightColumn::from)), 160);
    EXPECT_EQ(table.columnWidth(col(FlightColumn::km_from_start)), 70);
    EXPECT_EQ(table.columnWidth(col(FlightColumn::to)), 180);
    EXPECT_EQ(table.columnWidth(col(FlightColumn::km_from_dest)), 70);
    EXPECT_EQ(table.columnWidth(col(FlightColumn::total)), 92);
    EXPECT_EQ(table.columnWidth(col(FlightColumn::per_person)), 84);
    EXPECT_EQ(table.columnWidth(col(FlightColumn::fare_local)), 110);
    EXPECT_EQ(table.columnWidth(col(FlightColumn::all_departures)), 170);
    // The cells use the 9 pt data font, the headers the semibold UI font (D-53).
    EXPECT_EQ(table.font(), app::style::data_font());
    EXPECT_EQ(table.horizontalHeader()->font(), app::style::bold_form_font());
    EXPECT_FALSE(table.showGrid()) << "rows are separated by the delegate's line";

    // Read-only, full-row single selection, no row headers, no border, fixed columns order.
    EXPECT_EQ(table.editTriggers(), QAbstractItemView::NoEditTriggers);
    EXPECT_EQ(table.selectionBehavior(), QAbstractItemView::SelectRows);
    EXPECT_EQ(table.selectionMode(), QAbstractItemView::SingleSelection);
    EXPECT_FALSE(table.verticalHeader()->isVisible());
    EXPECT_EQ(table.frameShape(), QFrame::NoFrame);
    EXPECT_FALSE(table.horizontalHeader()->sectionsMovable());
    EXPECT_TRUE(table.horizontalHeader()->sectionsClickable());
    EXPECT_EQ(table.horizontalHeader()->sectionResizeMode(col(FlightColumn::from)), QHeaderView::Interactive);
    EXPECT_EQ(table.verticalHeader()->sectionResizeMode(0), QHeaderView::Fixed);
    EXPECT_FALSE(table.isSortingEnabled());
    EXPECT_EQ(table.horizontalHeader()->defaultAlignment() & (Qt::AlignHorizontal_Mask | Qt::AlignVertical_Mask),
              Qt::AlignLeft | Qt::AlignVCenter);  // WinForms MiddleLeft header cells

    // Cell alignment and the bold Total column.
    EXPECT_EQ(FlightTableModel::alignment(FlightColumn::date), Qt::AlignLeft | Qt::AlignVCenter);
    EXPECT_EQ(FlightTableModel::alignment(FlightColumn::departs), Qt::AlignHCenter | Qt::AlignVCenter);
    EXPECT_EQ(FlightTableModel::alignment(FlightColumn::km_from_start), Qt::AlignRight | Qt::AlignVCenter);
    EXPECT_EQ(FlightTableModel::alignment(FlightColumn::km_from_dest), Qt::AlignRight | Qt::AlignVCenter);
    EXPECT_EQ(FlightTableModel::alignment(FlightColumn::total), Qt::AlignRight | Qt::AlignVCenter);
    EXPECT_EQ(FlightTableModel::alignment(FlightColumn::per_person), Qt::AlignRight | Qt::AlignVCenter);
    EXPECT_EQ(FlightTableModel::alignment(FlightColumn::fare_local), Qt::AlignRight | Qt::AlignVCenter);
    EXPECT_EQ(FlightTableModel::alignment(FlightColumn::all_departures), Qt::AlignLeft | Qt::AlignVCenter);
    EXPECT_EQ(FlightTableModel::alignment(FlightColumn::notes), Qt::AlignLeft | Qt::AlignVCenter);
    f.grid.set_results(fss01(), "EUR", "");
    const QModelIndex total = f.grid.model()->index(0, col(FlightColumn::total));
    // Semibold: the semibold family (a weight of 600 on the Text family gives its Bold), at the
    // view's size.
    EXPECT_EQ(total.data(Qt::FontRole).value<QFont>().families(), app::style::semibold_face().families());
    EXPECT_EQ(total.data(Qt::FontRole).value<QFont>().resolveMask(), QFont::FamiliesResolved);
    EXPECT_FALSE(f.grid.model()->index(0, col(FlightColumn::per_person)).data(Qt::FontRole).isValid());
    EXPECT_EQ(total.data(Qt::TextAlignmentRole).value<Qt::Alignment>(), Qt::AlignRight | Qt::AlignVCenter);
    EXPECT_EQ(f.grid.model()->flags(total), Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren);
    EXPECT_EQ(table.rowHeight(0), table.fontMetrics().height() + 12) << "D-53: font height + 12 (WinForms + 9)";
    EXPECT_EQ(table.row_height(), table.rowHeight(0));
}

TEST(FlightGridWidget, Headers_follow_the_currency_and_km_from_dest_hides_for_anywhere)
{
    Fixture f;
    int header_changes = 0;
    QObject::connect(f.grid.model(), &QAbstractItemModel::headerDataChanged, [&] { ++header_changes; });
    f.grid.clear("huf", false, "");
    EXPECT_EQ(f.grid.model()->headerData(col(FlightColumn::total), Qt::Horizontal).toString(),
              QStringLiteral("Total (HUF)"));
    EXPECT_EQ(f.grid.model()->headerData(col(FlightColumn::per_person), Qt::Horizontal).toString(),
              QStringLiteral("Per person (HUF)"));
    EXPECT_EQ(header_changes, 1);
    f.grid.set_results(fss01(), "HUF", "");  // same currency: no header change
    EXPECT_EQ(header_changes, 1);

    f.grid.clear("EUR", true, "");
    EXPECT_TRUE(f.grid.table()->isColumnHidden(col(FlightColumn::km_from_dest)));
    f.grid.set_results(fss01(), "EUR", "");  // anywhere is kept by SetResults
    EXPECT_TRUE(f.grid.table()->isColumnHidden(col(FlightColumn::km_from_dest)));
    f.grid.clear("EUR", false, "");
    EXPECT_FALSE(f.grid.table()->isColumnHidden(col(FlightColumn::km_from_dest)));
}

TEST(FlightGridWidget, Notes_column_fills_the_remaining_width_with_a_minimum)
{
    Fixture f;
    f.grid.set_results(fss01(), "EUR", "");
    f.grid.resize(1600, 400);
    f.grid.show();
    test::gui::settle();
    const app::GridTableView& table = *f.grid.table();
    const QHeaderView& header = *table.horizontalHeader();
    int others = 0;
    for (int section = 0; section < header.count(); ++section) {
        if (section != col(FlightColumn::notes) && !header.isSectionHidden(section)) {
            others += header.sectionSize(section);
        }
    }
    EXPECT_EQ(header.sectionSize(col(FlightColumn::notes)), table.viewport()->width() - others);
    EXPECT_GT(header.sectionSize(col(FlightColumn::notes)), 120);

    f.grid.resize(700, 400);
    test::gui::settle();
    EXPECT_EQ(header.sectionSize(col(FlightColumn::notes)), 120);  // min L(120), then horizontal scrolling
}

TEST(FlightGridWidget, Header_texts_wrap_and_the_header_grows_to_fit)
{
    Fixture f;
    f.grid.resize(1600, 400);
    f.grid.show();
    f.grid.set_results(fss01(), "EUR", "");
    app::GridTableView& table = *f.grid.table();
    QHeaderView& header = *table.horizontalHeader();
    EXPECT_NE(header.defaultAlignment().toInt() & Qt::TextWordWrap, 0);
    for (int section = 0; section < header.count(); ++section) {
        table.setColumnWidth(section, 400);  // every text fits on one line
    }
    test::gui::settle();
    EXPECT_EQ(header.minimumHeight(), 0);
    const int one_line = header.height();

    table.setColumnWidth(col(FlightColumn::per_person), 30);  // "Per person (EUR)" needs several lines
    test::gui::settle();
    EXPECT_GT(header.minimumHeight(), one_line);
    EXPECT_EQ(header.height(), header.minimumHeight());
    table.setColumnWidth(col(FlightColumn::per_person), 400);
    test::gui::settle();
    EXPECT_EQ(header.minimumHeight(), 0);
    EXPECT_EQ(header.height(), one_line);
}

TEST(FlightGridWidget, UI_5_User_resized_widths_survive_rebuilds)
{
    Fixture f;
    f.grid.table()->setColumnWidth(col(FlightColumn::from), 250);
    f.grid.clear("EUR", true, "");
    f.grid.set_results(rt03_rows(), "EUR", "");
    f.grid.clear("EUR", false, "");
    f.grid.set_results(fss01(), "GBP", "");
    EXPECT_EQ(f.grid.table()->columnWidth(col(FlightColumn::from)), 250);
    EXPECT_EQ(f.grid.table()->columnWidth(col(FlightColumn::date)), 124);
}

// ---------------------------------------------------------------------------
// Cell texts, notes and row colours (01 §8.2, §8.3, §8.5)
// ---------------------------------------------------------------------------

TEST(FlightGridWidget, CT_FSS_01_Model_contents)
{
    Fixture f;
    f.grid.clear("EUR", false, "Searching 3 route(s) from 3 airport(s)…");
    f.grid.set_results(fss01(), "EUR", "12 fares");
    ASSERT_EQ(f.grid.model()->rowCount(), 12);
    EXPECT_EQ(f.grid.row_count(), 12u);
    EXPECT_EQ(f.grid.all_results().size(), 12u);

    // Row 0: the BTS fare (the 12th inserted, Idx 11), the cheapest.
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::idx), "11");
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::price_sort), "29.98");
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::date), "Thu 12 Nov 2026");
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::departs), "07:15");
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::from), "BTS  Bratislava");
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::km_from_start), "173");  // 172.544
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::to), "LTN  London Luton");
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::km_from_dest), "44");  // 44.072
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::total), "29.98");
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::per_person), "14.99");
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::fare_local), "14.99 EUR");
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::all_departures), "07:15");
    EXPECT_EQ(text_at(f.grid, 0, FlightColumn::notes), "total for 2 adults");

    // Default order: price, date, departure (CT-FSS-01 [D]).
    std::vector<std::string> routes;
    for (int row = 0; row < 12; ++row) {
        routes.push_back(route_at(f.grid, row));
    }
    EXPECT_EQ(routes, (std::vector<std::string>{"BTS>LTN 2026-11-12", "BUD>LTN 2026-11-14", "BUD>LTN 2026-11-18",
                                                "BUD>LTN 2026-11-19", "BUD>LTN 2026-11-20", "BUD>LTN 2026-11-11",
                                                "BUD>LTN 2026-11-12", "BUD>LTN 2026-11-13", "BUD>LTN 2026-11-17",
                                                "BUD>LTN 2026-11-16", "BUD>LTN 2026-11-10", "BUD>LGW 2026-11-15"}));

    // Row 5: BUD→LTN 11-11, a merged day priced at 06:00.
    EXPECT_EQ(text_at(f.grid, 5, FlightColumn::from), "BUD  Budapest");
    EXPECT_EQ(text_at(f.grid, 5, FlightColumn::km_from_start), "0");
    EXPECT_EQ(text_at(f.grid, 5, FlightColumn::total), "117.88");
    EXPECT_EQ(text_at(f.grid, 5, FlightColumn::per_person), "58.94");
    EXPECT_EQ(text_at(f.grid, 5, FlightColumn::fare_local), "21,590 HUF");
    EXPECT_EQ(text_at(f.grid, 5, FlightColumn::all_departures), "06:00*, 08:30, 17:30");
    EXPECT_EQ(text_at(f.grid, 5, FlightColumn::notes),
              "* = this fare; other times may land at another airport of the same group; total for 2 adults");
    // Row 7: BUD→LTN 11-13, priced at 12:50.
    EXPECT_EQ(text_at(f.grid, 7, FlightColumn::departs), "12:50");
    EXPECT_EQ(text_at(f.grid, 7, FlightColumn::all_departures), "06:00, 08:30, 12:50*, 17:30");
    // Row 10: BUD→LTN 11-10, not merged.
    EXPECT_EQ(text_at(f.grid, 10, FlightColumn::all_departures), "06:00, 17:30");
    EXPECT_EQ(text_at(f.grid, 10, FlightColumn::notes), "total for 2 adults");
    // Last row: BUD→LGW 11-15.
    EXPECT_EQ(text_at(f.grid, 11, FlightColumn::to), "LGW  London Gatwick");
    EXPECT_EQ(text_at(f.grid, 11, FlightColumn::km_from_dest), "41");  // 40.558
    EXPECT_EQ(text_at(f.grid, 11, FlightColumn::total), "189.40");
    EXPECT_EQ(text_at(f.grid, 11, FlightColumn::fare_local), "34,690 HUF");

    // Re-priced to HUF: no minor unit, culture group separator.
    std::vector<FlightResultPtr> huf;
    for (const FlightResultPtr& fare : fss01()) {
        huf.push_back(pricing::reprice(*fare, *test::ui::rates(), "HUF"));
    }
    f.grid.set_results(result_views::sort_by_price(huf), "HUF", "");
    EXPECT_EQ(text_at(f.grid, 11, FlightColumn::total), "69,380");
    EXPECT_EQ(text_at(f.grid, 11, FlightColumn::per_person), "34,690");
}

TEST(FlightGridWidget, Unavailable_fares_have_null_cells_grey_text_and_their_notes)
{
    Fixture f;
    f.grid.clear("EUR", false, "");
    f.grid.set_results(checkprice_fares(), "EUR", "");
    ASSERT_EQ(f.grid.model()->rowCount(), 4);
    const int last = 3;  // NoPrice sorts last by default
    EXPECT_EQ(f.grid.model()->fare_at(last)->fare.status, FareStatus::check_on_site);
    EXPECT_EQ(text_at(f.grid, last, FlightColumn::price_sort), "99999999");
    EXPECT_EQ(text_at(f.grid, last, FlightColumn::total), "");
    EXPECT_EQ(text_at(f.grid, last, FlightColumn::per_person), "");
    EXPECT_EQ(text_at(f.grid, last, FlightColumn::km_from_dest), "");  // no destination: null
    EXPECT_EQ(text_at(f.grid, last, FlightColumn::fare_local), "—");
    EXPECT_EQ(text_at(f.grid, last, FlightColumn::notes), "price not shown by Wizz — check on wizzair.com");
    for (const FlightColumn column : {FlightColumn::date, FlightColumn::notes, FlightColumn::total}) {
        const QModelIndex cell = f.grid.model()->index(last, col(column));
        EXPECT_EQ(cell.data(Qt::ForegroundRole).value<QBrush>().color(), app::grid_colors::gray_text());  // whole row
        EXPECT_FALSE(cell.data(Qt::BackgroundRole).isValid());
        EXPECT_EQ(cell.data(FlightTableModel::RowToneRole).toInt(), static_cast<int>(ui::RowTone::unavailable));
    }
    EXPECT_FALSE(f.grid.model()->index(0, col(FlightColumn::date)).data(Qt::ForegroundRole).isValid());
}

TEST(FlightGridWidget, Cheapest_rows_are_green_including_ties)
{
    Fixture f;
    f.grid.set_results(fss01(), "EUR", "");
    const auto background = [&](int row) {
        return f.grid.model()->index(row, col(FlightColumn::from)).data(Qt::BackgroundRole);
    };
    ASSERT_TRUE(background(0).isValid());
    // 01 §8.3's light green became the theme's cheapest tint (D-53); text stays readable on it
    // (Look.Token_pairs_meet_WCAG_AA).
    EXPECT_EQ(background(0).value<QBrush>().color(), app::style::theme().cheapest);
    EXPECT_EQ(app::grid_colors::cheapest_back(), app::style::theme().cheapest);
    for (int row = 1; row < 12; ++row) {
        EXPECT_FALSE(background(row).isValid()) << row;
    }
    // Every row tied at the cheapest price is highlighted (exact decimal equality, any scale).
    const FlightResultPtr a = builder_a("BUD", "LTN", day(11, 10), "20");
    const FlightResultPtr b = builder_a("BTS", "LTN", day(11, 10), "20.00");
    const FlightResultPtr c = builder_a("BUD", "LGW", day(11, 11), "21");
    f.grid.set_results(std::vector<FlightResultPtr>{c, a, b}, "EUR", "");
    EXPECT_TRUE(background(0).isValid());
    EXPECT_TRUE(background(1).isValid());
    EXPECT_FALSE(background(2).isValid());
    EXPECT_EQ(f.grid.model()->fare_at(0), a);  // equal price, date and time: insertion order
    EXPECT_EQ(f.grid.model()->fare_at(1), b);
}

TEST(FlightGridWidget, Selected_rows_use_the_selection_tint_and_the_cheapest_a_deeper_green)
{
    Fixture f;
    f.grid.set_results(fss01(), "EUR", "");
    ASSERT_NE(dynamic_cast<app::GridRowDelegate*>(f.grid.table()->itemDelegate()), nullptr);
    const app::GridRowDelegate delegate;
    const auto option_for = [&](int row, bool selected) {
        QStyleOptionViewItem option;
        option.initFrom(f.grid.table()->viewport());
        option.state.setFlag(QStyle::State_Selected, selected);
        delegate.initStyleOption(&option, f.grid.model()->index(row, col(FlightColumn::date)));
        return option;
    };
    // Every selection is the theme's soft tint with the normal text colour; a selected
    // cheapest row a deeper green (01 §8.3's dark green / SystemColors.Highlight, D-53).
    const app::style::Theme& theme = app::style::theme();
    EXPECT_EQ(app::grid_colors::highlight(), theme.selection);
    EXPECT_EQ(app::grid_colors::highlight_text(), theme.selection_text);
    EXPECT_EQ(app::grid_colors::cheapest_selection_back(), theme.cheapest_selection);
    const QStyleOptionViewItem cheapest = option_for(0, true);
    EXPECT_FALSE(cheapest.state.testFlag(QStyle::State_Selected));
    EXPECT_EQ(cheapest.backgroundBrush.color(), theme.cheapest_selection);
    EXPECT_EQ(cheapest.palette.color(QPalette::Text), theme.selection_text);
    EXPECT_EQ(option_for(0, false).backgroundBrush.color(), theme.cheapest);
    EXPECT_EQ(f.grid.model()->index(0, 2).data(app::GridRowDelegate::SelectionBackgroundRole).value<QBrush>().color(),
              theme.cheapest_selection);
    const QStyleOptionViewItem other = option_for(1, true);  // the selection tint applies
    EXPECT_FALSE(other.state.testFlag(QStyle::State_Selected));
    EXPECT_EQ(other.backgroundBrush.color(), app::grid_colors::highlight());
    EXPECT_EQ(other.palette.color(QPalette::Text), app::grid_colors::highlight_text());
    EXPECT_EQ(option_for(1, false).backgroundBrush.style(), Qt::NoBrush);

    // Unavailable fares: grey text, but the highlight colours when selected.
    f.grid.set_results(checkprice_fares(), "EUR", "");
    EXPECT_EQ(option_for(3, false).palette.color(QPalette::Text), app::grid_colors::gray_text());
    EXPECT_EQ(option_for(3, true).palette.color(QPalette::Text), app::grid_colors::highlight_text());

    // No hover effect.
    QStyleOptionViewItem hovered;
    hovered.initFrom(f.grid.table()->viewport());
    hovered.state.setFlag(QStyle::State_MouseOver, true);
    delegate.initStyleOption(&hovered, f.grid.model()->index(1, col(FlightColumn::date)));
    EXPECT_FALSE(hovered.state.testFlag(QStyle::State_MouseOver));
}

TEST(FlightGridWidget, Cheapest_rows_differ_from_the_others_and_from_the_selection)
{
    // What the tint means (01 §8.3), whatever the tokens are: a cheapest row's background is
    // not a normal row's, and selecting it shows a third colour, not the normal selection.
    Fixture f;
    f.grid.set_results(fss01(), "EUR", "");
    const app::GridRowDelegate delegate;
    const auto background = [&](int row, bool selected) {
        QStyleOptionViewItem option;
        option.initFrom(f.grid.table()->viewport());
        option.state.setFlag(QStyle::State_Selected, selected);
        delegate.initStyleOption(&option, f.grid.model()->index(row, col(FlightColumn::from)));
        return option.backgroundBrush.style() == Qt::NoBrush ? f.grid.table()->palette().color(QPalette::Base)
                                                             : option.backgroundBrush.color();
    };
    ASSERT_EQ(f.grid.model()->index(0, 0).data(FlightTableModel::RowToneRole).toInt(),
              static_cast<int>(ui::RowTone::cheapest));
    ASSERT_NE(f.grid.model()->index(1, 0).data(FlightTableModel::RowToneRole).toInt(),
              static_cast<int>(ui::RowTone::cheapest));
    EXPECT_NE(background(0, false), background(1, false)) << "cheapest vs a normal row";
    EXPECT_NE(background(0, true), background(1, true)) << "selected cheapest vs the normal selection";
    EXPECT_NE(background(0, true), background(0, false)) << "a selected cheapest row vs an unselected one";
    EXPECT_NE(background(1, true), background(1, false)) << "the selection shows";
}

TEST(FlightGridWidget, Row_separators_and_the_row_bar_show_selection_and_keyboard_focus)
{
    // D-53: no cell grid and no per-cell focus frame. A 1 px separator under every row; a 3 px
    // bar at the left of the selected row: the accent while the grid has the keyboard focus,
    // the muted text colour without it; the accent bar also on the current row of a focused
    // grid that is not selected (Ctrl+Space, Ctrl+click); a ring round a focused grid without
    // a current row.
    QWidget window;
    auto* grid = new app::FlightGrid(Locale::en_us(), true, &window);
    auto* other = new QLineEdit(&window);  // takes the focus away
    grid->setGeometry(0, 0, 900, 420);
    other->setGeometry(0, 430, 100, 26);
    window.resize(900, 460);
    window.show();
    app::GridTableView* table = grid->table();
    const app::style::Theme& theme = app::style::theme();
    const auto grab = [table] {
        QCoreApplication::processEvents();
        return table->viewport()->grab().toImage();
    };

    table->setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    if (!table->hasFocus()) {
        GTEST_SKIP() << "this platform gives no widget the focus";
    }
    QImage image = grab();
    EXPECT_EQ(image.pixelColor(1, image.height() / 2), theme.accent_line) << "the ring round the empty grid";

    grid->set_results(fss01(), "EUR", "");
    table->selectRow(1);  // row 0 is the cheapest
    ASSERT_GT(table->rowViewportPosition(3), 0);
    const auto middle = [table](int row) { return table->rowViewportPosition(row) + table->rowHeight(row) / 2; };
    image = grab();
    for (int x = 0; x < app::ModernStyle::kSelectionBarWidth; ++x) {
        EXPECT_EQ(image.pixelColor(x, middle(1)), theme.accent_line) << "focused and selected, x " << x;
    }
    EXPECT_NE(image.pixelColor(3, middle(1)), theme.accent_line) << "3 px wide";
    EXPECT_NE(image.pixelColor(1, middle(2)), theme.accent_line) << "only on that row";
    EXPECT_NE(image.pixelColor(1, image.height() - 3), theme.accent_line) << "no ring with a current row";
    for (int row = 0; row < 3; ++row) {
        const int bottom = table->rowViewportPosition(row) + table->rowHeight(row) - 1;
        EXPECT_EQ(image.pixelColor(image.width() / 2, bottom), theme.row_separator) << "under row " << row;
    }
    const int above_separator = table->rowViewportPosition(2) + table->rowHeight(2) - 2;
    EXPECT_EQ(image.pixelColor(4, above_separator), theme.surface) << "1 px";

    other->setFocus(Qt::OtherFocusReason);
    ASSERT_FALSE(table->hasFocus());
    image = grab();
    for (int x = 0; x < app::ModernStyle::kSelectionBarWidth; ++x) {
        EXPECT_EQ(image.pixelColor(x, middle(1)), theme.text_muted) << "the selected row without focus, x " << x;
    }

    // The current row loses its selection while the grid keeps the focus (Ctrl+Space in
    // single selection does that): the bar still shows where the keyboard is.
    table->setFocus(Qt::TabFocusReason);
    table->selectionModel()->clearSelection();
    ASSERT_EQ(table->currentIndex().row(), 1);
    ASSERT_FALSE(table->selectionModel()->isRowSelected(1));
    image = grab();
    for (int x = 0; x < app::ModernStyle::kSelectionBarWidth; ++x) {
        EXPECT_EQ(image.pixelColor(x, middle(1)), theme.accent_line) << "the focused current row, x " << x;
    }
    // Moving the current row moves the bar, also when the current cell is not in the first column.
    table->setCurrentIndex(table->model()->index(2, 5));
    table->selectionModel()->clearSelection();
    image = grab();
    EXPECT_EQ(image.pixelColor(1, middle(2)), theme.accent_line);
    EXPECT_NE(image.pixelColor(1, middle(1)), theme.accent_line) << "gone from the previous row";

    other->setFocus(Qt::OtherFocusReason);
    image = grab();
    EXPECT_EQ(image.pixelColor(1, middle(2)), theme.surface) << "no focus, no selection: no bar";
}

TEST(FlightGridWidget, Truncated_cells_qualify_for_a_tooltip)
{
    Fixture f;
    f.grid.set_results(fss01(), "EUR", "");
    const app::GridRowDelegate delegate;
    QStyleOptionViewItem option;
    option.initFrom(f.grid.table()->viewport());
    delegate.initStyleOption(&option, f.grid.model()->index(5, col(FlightColumn::notes)));
    ASSERT_FALSE(option.text.isEmpty());
    const int text_width = option.fontMetrics.horizontalAdvance(option.text);
    option.rect = QRect{0, 0, text_width + 40, 20};
    EXPECT_FALSE(app::GridRowDelegate::text_truncated(option, f.grid.table()));
    option.rect = QRect{0, 0, text_width / 2, 20};
    EXPECT_TRUE(app::GridRowDelegate::text_truncated(option, f.grid.table()));
}

} // namespace
