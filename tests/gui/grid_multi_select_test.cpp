// Several selected rows in the Outbound / Return grids (DECISIONS D-76): Windows' gestures
// (Ctrl+click, Shift+click, Ctrl+A) select several rows; the grid keeps the user's selection
// across sorting, streaming and the final result; Copy details (also Ctrl+C) and Copy as table
// copy the selected rows in display order through the grid's clipboard (never Qt's copy of one
// cell); Open and the other one-row actions act on the current row, the one with the bar.

#include "flight_grid.hpp"
#include "grid_table_view.hpp"
#include "ui_style.hpp"

#include "wizz/ui/flight_rows.hpp"

#include "flight_grid_samples.hpp"
#include "grid_test_support.hpp"

#include <QApplication>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMouseEvent>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace wizz;
using namespace wizz::test::gui;
using ui::FlightColumn;
using Fixture = GridFixture;

/// A left click on `row` with `modifiers` held, as the user clicks (press and release).
void click_row(QTableView& view, int row, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPoint pos = cell_center(view, row, col(FlightColumn::to));
    const QPointF global = view.viewport()->mapToGlobal(QPointF(pos));
    QMouseEvent press{QEvent::MouseButtonPress, QPointF(pos), global, Qt::LeftButton, Qt::LeftButton, modifiers};
    QApplication::sendEvent(view.viewport(), &press);
    QMouseEvent release{QEvent::MouseButtonRelease, QPointF(pos), global, Qt::LeftButton, Qt::NoButton, modifiers};
    QApplication::sendEvent(view.viewport(), &release);
}

/// A shown Outbound grid with the FSS-01 fares.
struct Shown : Fixture {
    Shown()
    {
        grid.resize(1400, 520);
        grid.show();
        grid.set_results(fss01(), "EUR", "");
        settle();
    }
    [[nodiscard]] app::GridTableView& table() const { return *grid.table(); }
    [[nodiscard]] std::vector<std::size_t> view_rows() const { return grid.table()->selected_rows(); }
    [[nodiscard]] std::vector<std::size_t> model_rows() const { return grid.model()->state().selected_rows(); }
};

TEST(GridMultiSelect, D_76_Ctrl_click_Shift_click_and_Ctrl_A_select_rows_and_the_model_follows)
{
    Shown f;
    EXPECT_EQ(f.view_rows(), std::vector<std::size_t>{0}) << "row 0, as before";
    click_row(f.table(), 1);
    EXPECT_EQ(f.view_rows(), std::vector<std::size_t>{1});
    click_row(f.table(), 3, Qt::ControlModifier);
    EXPECT_EQ(f.view_rows(), (std::vector<std::size_t>{1, 3}));
    EXPECT_EQ(f.model_rows(), f.view_rows());
    EXPECT_EQ(f.grid.model()->state().current_row(), 3u) << "the last click is current";
    // Shift+click: Qt selects the range from the last clicked row itself, reading the keyboard's
    // global state (QGuiApplication::keyboardModifiers()) when the current row moves, which an
    // event sent to a widget does not set; so the range is made as Qt's Shift+click makes it.
    {
        QItemSelectionModel& selection = *f.table().selectionModel();
        const int last = f.grid.model()->columnCount() - 1;
        selection.setCurrentIndex(f.grid.model()->index(5, col(FlightColumn::to)), QItemSelectionModel::NoUpdate);
        selection.select(QItemSelection(f.grid.model()->index(3, 0), f.grid.model()->index(5, last)),
                         QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    EXPECT_EQ(f.view_rows(), (std::vector<std::size_t>{3, 4, 5}));
    EXPECT_EQ(f.model_rows(), f.view_rows());
    click_row(f.table(), 4, Qt::ControlModifier);  // Ctrl+click on a selected row: off
    EXPECT_EQ(f.view_rows(), (std::vector<std::size_t>{3, 5}));
    EXPECT_EQ(f.model_rows(), f.view_rows());
    f.table().setFocus(Qt::OtherFocusReason);
    send_key(&f.table(), QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    EXPECT_EQ(f.view_rows().size(), f.grid.model()->rowCount());
    EXPECT_EQ(f.model_rows(), f.view_rows());
    // A plain click selects one row again.
    click_row(f.table(), 2);
    EXPECT_EQ(f.view_rows(), std::vector<std::size_t>{2});
    EXPECT_EQ(f.model_rows(), std::vector<std::size_t>{2});
}

TEST(GridMultiSelect, D_76_The_selection_stays_on_its_fares_through_a_sort_streaming_and_the_final_result)
{
    Fixture f;
    f.grid.resize(1400, 520);
    f.grid.show();
    const std::vector<FlightResultPtr> fares = fss01();
    f.grid.clear("EUR", false, "");
    f.grid.append(std::vector<FlightResultPtr>{fares.begin(), fares.begin() + 6});
    settle();
    app::GridTableView& table = *f.grid.table();
    click_row(table, 1);
    click_row(table, 4, Qt::ControlModifier);
    const std::string first = route_at(f.grid, 1);
    const std::string second = route_at(f.grid, 4);
    const auto selected_routes = [&f] {
        std::vector<std::string> routes;
        for (const std::size_t row : f.grid.table()->selected_rows()) {
            routes.push_back(route_at(f.grid, static_cast<int>(row)));
        }
        std::ranges::sort(routes);
        return routes;
    };
    std::vector<std::string> expected{first, second};
    std::ranges::sort(expected);
    f.grid.append(std::vector<FlightResultPtr>{fares.begin() + 6, fares.end()});  // a streamed batch
    settle();
    EXPECT_EQ(selected_routes(), expected) << "streaming";
    click_header(f.grid, FlightColumn::date);
    EXPECT_EQ(selected_routes(), expected) << "a header sort";
    f.grid.set_results(fss01(), "EUR", "");  // the final result: a full reload
    settle();
    EXPECT_EQ(selected_routes(), expected) << "the final result";
    f.grid.clear("EUR", false, "");  // a new search
    f.grid.set_results(fss01(), "EUR", "");
    EXPECT_EQ(f.grid.table()->selected_rows().size(), 1u) << "back to one row";
}

TEST(GridMultiSelect, D_76_Copy_details_Ctrl_C_and_Copy_as_table_copy_the_selected_rows_in_display_order)
{
    Shown f;
    click_row(f.table(), 3);
    click_row(f.table(), 0, Qt::ControlModifier);  // selected out of order
    const std::string expected_details =
        ui::copy_details_text(*f.grid.model()->fare_at(0), *Locale::en_us()) + "\r\n\r\n" +
        ui::copy_details_text(*f.grid.model()->fare_at(3), *Locale::en_us());

    f.grid.prepare_context_menu()->actions()[1]->trigger();  // "Copy details"
    ASSERT_EQ(f.recorder.clipboard.size(), 1u);
    EXPECT_EQ(f.recorder.clipboard[0].toStdString(), expected_details);

    // Ctrl+C in the table: the same text through the grid's clipboard, not Qt's copy of one cell.
    f.table().setFocus(Qt::OtherFocusReason);
    QKeyEvent override{QEvent::ShortcutOverride, Qt::Key_C, Qt::ControlModifier};
    QApplication::sendEvent(&f.table(), &override);
    EXPECT_TRUE(override.isAccepted()) << "claimed before any window shortcut";
    send_key(&f.table(), QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    ASSERT_EQ(f.recorder.clipboard.size(), 2u);
    EXPECT_EQ(f.recorder.clipboard[1].toStdString(), expected_details);

    f.grid.prepare_context_menu()->actions()[2]->trigger();  // "Copy as table"
    ASSERT_EQ(f.recorder.clipboard.size(), 3u);
    const std::string table = f.recorder.clipboard[2].toStdString();
    EXPECT_TRUE(table.starts_with("Date\tDeparts\tFrom\t")) << table;
    EXPECT_EQ(std::ranges::count(table, '\n'), 2) << "a header and two rows";
    const std::size_t second_line = table.find("\r\n") + 2;
    EXPECT_TRUE(table.substr(second_line).starts_with(text_at(f.grid, 0, FlightColumn::date) + "\t")) << table;
}

TEST(GridMultiSelect, D_76_Open_acts_on_the_current_row_and_only_it_has_the_bar)
{
    QWidget window;
    auto* grid = new app::FlightGrid(Locale::en_us(), true, &window);
    ActionRecorder recorder;
    grid->set_actions(recorder.actions());
    grid->setGeometry(0, 0, 1200, 460);
    window.resize(1200, 480);
    window.show();
    grid->set_results(fss01(), "EUR", "");
    settle();
    app::GridTableView& table = *grid->table();
    click_row(table, 1);
    click_row(table, 2, Qt::ControlModifier);
    click_row(table, 3, Qt::ControlModifier);
    ASSERT_EQ(table.selected_rows(), (std::vector<std::size_t>{1, 2, 3}));
    grid->open_selected();
    ASSERT_EQ(recorder.opened.size(), 1u) << "one booking page, not three";
    EXPECT_EQ(recorder.opened[0].toStdString(), grid->model()->fare_at(3)->booking_url);

    table.setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    if (!table.hasFocus()) {
        GTEST_SKIP() << "this platform gives no widget the focus";
    }
    const app::style::Theme& theme = app::style::theme();
    const QImage image = table.viewport()->grab().toImage();
    const auto middle = [&table](int row) { return table.rowViewportPosition(row) + table.rowHeight(row) / 2; };
    EXPECT_EQ(image.pixelColor(1, middle(3)), theme.accent_line) << "the current row's bar";
    for (const int row : {1, 2}) {
        EXPECT_NE(image.pixelColor(1, middle(row)), theme.accent_line) << "row " << row << ": the tint only";
        EXPECT_NE(image.pixelColor(1, middle(row)), theme.text_muted) << "row " << row;
    }
}

TEST(GridMultiSelect, D_76_A_press_on_a_selected_row_selects_it_alone_at_once)
{
    // Qt would wait for the release, and a streamed flush between press and release would keep the
    // old selection; these tables start no drag, so the press decides.
    Shown f;
    click_row(f.table(), 2);
    click_row(f.table(), 5, Qt::ControlModifier);
    click_row(f.table(), 7, Qt::ControlModifier);
    ASSERT_EQ(f.view_rows(), (std::vector<std::size_t>{2, 5, 7}));
    const QPoint pos = cell_center(f.table(), 5, col(FlightColumn::to));
    QMouseEvent press{QEvent::MouseButtonPress, QPointF(pos), f.table().viewport()->mapToGlobal(QPointF(pos)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier};
    QApplication::sendEvent(f.table().viewport(), &press);
    EXPECT_EQ(f.view_rows(), std::vector<std::size_t>{5}) << "at the press";
    QMouseEvent release{QEvent::MouseButtonRelease, QPointF(pos), f.table().viewport()->mapToGlobal(QPointF(pos)),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier};
    QApplication::sendEvent(f.table().viewport(), &release);
    EXPECT_EQ(f.view_rows(), std::vector<std::size_t>{5});
    EXPECT_FALSE(f.grid.model()->state().selection_chosen()) << "one row, the current one";
    // A right-click on a selected row keeps the selection (the menu acts on it).
    click_row(f.table(), 3, Qt::ControlModifier);
    ASSERT_EQ(f.view_rows(), (std::vector<std::size_t>{3, 5}));
    click(f.table(), cell_center(f.table(), 5, col(FlightColumn::to)), Qt::RightButton);
    EXPECT_EQ(f.view_rows(), (std::vector<std::size_t>{3, 5}));
}

TEST(GridMultiSelect, D_76_A_selection_restored_after_a_reload_grows_with_the_next_Shift_range)
{
    // A reload resets the model, Qt drops its selection, and the grid selects the kept rows again.
    // Qt holds a selection made that way as its uncommitted "current selection", which a following
    // Shift+arrow or Shift+click (SelectCurrent) would replace: the grid commits it.
    Shown f;
    click_row(f.table(), 2);
    click_row(f.table(), 3, Qt::ControlModifier);
    click_row(f.table(), 4, Qt::ControlModifier);
    ASSERT_EQ(f.view_rows(), (std::vector<std::size_t>{2, 3, 4}));
    f.grid.set_results(fss01(), "EUR", "");  // a full reload (model reset)
    settle();
    ASSERT_EQ(f.view_rows(), (std::vector<std::size_t>{2, 3, 4})) << "kept by the model";
    QItemSelectionModel& selection = *f.table().selectionModel();
    const int last = f.grid.model()->columnCount() - 1;
    selection.select(QItemSelection(f.grid.model()->index(4, 0), f.grid.model()->index(5, last)),
                     QItemSelectionModel::SelectCurrent | QItemSelectionModel::Rows);
    EXPECT_EQ(f.view_rows(), (std::vector<std::size_t>{2, 3, 4, 5}));
}

} // namespace
