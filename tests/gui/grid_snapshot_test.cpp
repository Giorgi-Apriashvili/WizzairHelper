// Renders the result grids to PNG files for a visual review (01 §8.1-§8.3,
// §9.1). Skipped unless WIZZ_GUI_SNAPSHOT_DIR names an output directory; run
// it with QT_QPA_PLATFORM=windows (the minimal platform draws no text) and
// optionally WIZZ_GUI_SNAPSHOT_STYLE=windows11 / windowsvista / fusion.

#include "flight_grid_samples.hpp"
#include "grid_test_support.hpp"
#include "round_trip_grid.hpp"

#include "wizz/ui/round_trip_rows.hpp"
#include "wizz/ui/strings.hpp"

#include <QApplication>
#include <QDir>
#include <QItemSelectionModel>
#include <QStyle>

#include <gtest/gtest.h>

#include <vector>

namespace {

using namespace wizz;
using namespace wizz::test::gui;
using ui::FlightColumn;

TEST(GridSnapshots, Render_the_grids)
{
    const QString dir = qEnvironmentVariable("WIZZ_GUI_SNAPSHOT_DIR");
    if (dir.isEmpty()) {
        GTEST_SKIP() << "set WIZZ_GUI_SNAPSHOT_DIR to render the grids";
    }
    QDir{}.mkpath(dir);
    if (const QString style = qEnvironmentVariable("WIZZ_GUI_SNAPSHOT_STYLE"); !style.isEmpty()) {
        QApplication::setStyle(style);
    }

    // Outbound: the CT-FSS-01 fares plus check-on-site rows, a selected cheapest row.
    GridFixture outbound;
    outbound.grid.resize(1320, 420);
    outbound.grid.show();
    std::vector<FlightResultPtr> fares = fss01();
    for (const FlightResultPtr& fare : checkprice_fares()) {
        fares.push_back(fare);
    }
    outbound.grid.clear("EUR", false, ui::strings::searching_routes(3, 3));
    outbound.grid.set_results(fares, "EUR", "16 fares · cheapest 29.98 EUR (BTS→LTN Thu 12 Nov)");
    settle();
    EXPECT_TRUE(outbound.grid.grab().save(dir + "/flight_grid_selected_cheapest.png"));
    click(*outbound.grid.table(), cell_center(*outbound.grid.table(), 3, col(FlightColumn::from)));
    outbound.grid.table()->setFocus();
    settle();
    EXPECT_TRUE(outbound.grid.grab().save(dir + "/flight_grid_selected_row.png"));
    // Several selected rows (D-76): rows 3, 5 and 6, the current row 6 with the bar.
    {
        QItemSelectionModel& selection = *outbound.grid.table()->selectionModel();
        const int last = outbound.grid.model()->columnCount() - 1;
        selection.setCurrentIndex(outbound.grid.model()->index(6, col(FlightColumn::from)), QItemSelectionModel::NoUpdate);
        selection.select(QItemSelection(outbound.grid.model()->index(5, 0), outbound.grid.model()->index(6, last)),
                         QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    settle();
    EXPECT_TRUE(outbound.grid.grab().save(dir + "/flight_grid_selected_rows.png"));

    // Anywhere: Cheapest per destination, no "km from dest." column.
    GridFixture anywhere;
    anywhere.grid.resize(1320, 260);
    anywhere.grid.show();
    anywhere.grid.clear("EUR", true, "");
    anywhere.grid.set_results(rt03_rows(), "EUR", "2 destinations");
    settle();
    EXPECT_TRUE(anywhere.grid.grab().save(dir + "/flight_grid_anywhere.png"));

    // Round trips: the 03 §11 worked example.
    app::RoundTripGrid trips{Locale::en_us()};
    trips.resize(1320, 420);
    trips.show();
    const LegSearchResult out = test::ui::section11_outbound();
    const LegSearchResult ret = test::ui::section11_return();
    ui::TripsView view = ui::recompute_trips(&out, &ret, 2, 5, false, false, "EUR", test::ui::en());
    trips.set_trips(std::move(view.trips), view.currency, view.summary);
    settle();
    EXPECT_TRUE(trips.grab().save(dir + "/round_trip_grid.png"));
}

} // namespace
