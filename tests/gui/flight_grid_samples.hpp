#pragma once
// Inputs and probes of the FlightGrid widget tests: the CT-FSS-01 fares and the
// CT-RT-03 rows built with core pricing over the fixture map CAT and RATES
// (05 §0.3), the check-on-site timetable, and a grid wired to an
// ActionRecorder. Locale en-US.

#include "flight_grid.hpp"
#include "flight_table_model.hpp"
#include "grid_table_view.hpp"

#include "wizz/ui/flight_rows.hpp"

#include "wizz/core/domain.hpp"
#include "wizz/core/pricing.hpp"

#include "grid_test_support.hpp"
#include "ui_logic/ui_samples.hpp"

#include <QHeaderView>

#include <string>
#include <vector>

namespace wizz::test::gui {

[[nodiscard]] inline int col(wizz::ui::FlightColumn column)
{
    return app::FlightTableModel::column_index(column);
}

/// The 12 CT-FSS-01 fares in insertion order (the BUD response, then BTS).
[[nodiscard]] inline std::vector<FlightResultPtr> fss01()
{
    return samples::section11_flights();
}

/// CT-RT-03 rows: BUD→LTN d 30, BTS→LTN d 20, BUD→BCN d+1 40, BUD→LGW d+1 35.
[[nodiscard]] inline std::vector<FlightResultPtr> rt03_rows()
{
    using samples::builder_a;
    using samples::day;
    return {builder_a("BUD", "LTN", day(11, 10), "30"), builder_a("BTS", "LTN", day(11, 10), "20"),
            builder_a("BUD", "BCN", day(11, 11), "40"), builder_a("BUD", "LGW", day(11, 11), "35")};
}

/// The timetable_checkprice.json rows priced for Budapest → anywhere (11-02 is check-on-site).
[[nodiscard]] inline std::vector<FlightResultPtr> checkprice_fares()
{
    LegSearchRequest request;
    request.origin = samples::place("Budapest");
    request.from = samples::day(11, 2);
    request.to = samples::day(11, 5);
    std::vector<FlightResultPtr> fares;
    for (const DayFare& fare : samples::checkprice_rows()) {
        fares.push_back(pricing::build_result(*samples::cat(), request, *samples::rates(), fare));
    }
    return fares;
}

/// Display text of a cell.
[[nodiscard]] inline std::string text_at(const app::FlightGrid& grid, int row, wizz::ui::FlightColumn column)
{
    return grid.model()->index(row, col(column)).data().toString().toStdString();
}

/// "FROM>TO yyyy-MM-dd" of a display row.
[[nodiscard]] inline std::string route_at(const app::FlightGrid& grid, int row)
{
    const FlightResultPtr fare = grid.model()->fare_at(row);
    return fare->from_airport->iata + ">" + fare->to_airport->iata + " " + fare->date().iso();
}

/// The view's current row.
[[nodiscard]] inline int current_view_row(const app::FlightGrid& grid)
{
    return grid.table()->currentIndex().row();
}

/// A user click on a column header.
inline void click_header(app::FlightGrid& grid, wizz::ui::FlightColumn column)
{
    QHeaderView* header = grid.table()->horizontalHeader();
    const int section = col(column);
    const QPoint pos{header->sectionViewportPosition(section) + header->sectionSize(section) / 2,
                     header->height() / 2};
    send_mouse(header->viewport(), QEvent::MouseButtonPress, pos, Qt::LeftButton);
    send_mouse(header->viewport(), QEvent::MouseButtonRelease, pos, Qt::LeftButton);
}

/// A grid whose side effects are recorded.
struct GridFixture {
    explicit GridFixture(bool offer_plan_return = true) : grid(Locale::en_us(), offer_plan_return)
    {
        grid.set_actions(recorder.actions());
    }
    ActionRecorder recorder;
    app::FlightGrid grid;
};

} // namespace wizz::test::gui
