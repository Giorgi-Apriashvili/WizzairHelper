// RoundTripGrid pure parts and RecomputeTrips (01 §9, §11.8; 05 CT-RT-01,
// CT-RTR-06 texts; 03 §11 worked example). Fixture map CAT, RATES, en-US.

#include "wizz/ui/round_trip_rows.hpp"

#include "wizz/core/round_trips.hpp"

#include "ui_logic/ui_samples.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace wizz;
using namespace wizz::ui;
using test::samples::builder_a;
using test::ui::airport;
using test::ui::day;
using test::ui::dec;
using test::ui::en;

[[nodiscard]] std::shared_ptr<const Locale> en_ptr()
{
    return Locale::en_us();
}

/// CT-RT-01: outbound BUD→LTN d 20, BTS→LTN d+1 10; return LTN→BUD d+3 30, LGW→BUD d+5 15, LTN→BTS d+20 1.
[[nodiscard]] std::vector<RoundTrip> rt01_trips()
{
    const std::vector<FlightResultPtr> out{builder_a("BUD", "LTN", day(11, 10), "20"), builder_a("BTS", "LTN", day(11, 11), "10")};
    const std::vector<FlightResultPtr> ret{builder_a("LTN", "BUD", day(11, 13), "30"), builder_a("LGW", "BUD", day(11, 15), "15"),
                                           builder_a("LTN", "BTS", day(11, 30), "1")};
    return round_trips::combine(out, ret, 2, 5);
}

TEST(TripRows, Leg_text_example_of_01_9_2)
{
    const FlightResultPtr flight = test::ui::hand_fare(airport("KSC", "Kosice"), airport("LTN", "London Luton"),
                                                       Date::from_ymd(2026, 12, 5), 20, 40, "29.99", "EUR", "29.99");
    EXPECT_EQ(leg_text(*flight), "20:40  KSC \u2192 LTN  (Kosice \u2192 London Luton)");
}

TEST(TripRows, CT_RT_01_Booking_notes_and_links)
{
    const std::vector<RoundTrip> trips = rt01_trips();
    ASSERT_EQ(trips.size(), 4u);
    // 25: BTS→LTN 11-11 + LGW→BUD 11-15, neither same home nor same away
    EXPECT_EQ(booking_note(trips[0]),
              "two one-way bookings; open jaw: land LTN, fly back from LGW; back to BUD (left from BTS)");
    // 35: BUD→LTN + LGW→BUD, same home only
    EXPECT_EQ(booking_note(trips[1]), "two one-way bookings; open jaw: land LTN, fly back from LGW");
    // 40: BTS→LTN + LTN→BUD, same away only
    EXPECT_EQ(booking_note(trips[2]), "two one-way bookings; back to BUD (left from BTS)");
    // 50: BUD→LTN + LTN→BUD, one booking
    EXPECT_EQ(booking_note(trips[3]), "one booking");
    EXPECT_EQ(booking_urls(trips[3]),
              (std::vector<std::string>{"https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/2026-11-13/1/0/0/null"}));
    EXPECT_EQ(booking_urls(trips[0]),
              (std::vector<std::string>{trips[0].outbound->booking_url, trips[0].inbound->booking_url}));
}

TEST(TripRows, Columns_headers_and_cells)
{
    std::vector<std::string> names;
    for (const TripColumn column : kTripColumns) {
        names.emplace_back(trip_column_name(column));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"Idx", "Total", "Nights", "Out date", "Outbound", "Out price",
                                               "Return date", "Return", "Return price", "Booking / notes"}));
    EXPECT_EQ(trip_column_header(TripColumn::total, "EUR"), "Total (EUR)");
    EXPECT_EQ(trip_column_header(TripColumn::out_price, "EUR"), "Out price");

    const std::vector<RoundTrip> trips = rt01_trips();
    const TripRow row = make_trip_row(0, trips[0]);
    EXPECT_EQ(trip_cell_text(row, TripColumn::total, "EUR", en()), "25.00");
    EXPECT_EQ(trip_cell_text(row, TripColumn::nights, "EUR", en()), "4");
    EXPECT_EQ(trip_cell_text(row, TripColumn::out_date, "EUR", en()), "Wed 11 Nov");
    EXPECT_EQ(trip_cell_text(row, TripColumn::outbound, "EUR", en()), "06:00  BTS \u2192 LTN  (Bratislava \u2192 London Luton)");
    EXPECT_EQ(trip_cell_text(row, TripColumn::out_price, "EUR", en()), "10.00");
    EXPECT_EQ(trip_cell_text(row, TripColumn::return_date, "EUR", en()), "Sun 15 Nov");
    EXPECT_EQ(trip_cell_text(row, TripColumn::return_leg, "EUR", en()),
              "06:00  LGW \u2192 BUD  (London Gatwick \u2192 Budapest)");
    EXPECT_EQ(trip_cell_text(row, TripColumn::return_price, "EUR", en()), "15.00");
    EXPECT_EQ(trip_cell_text(row, TripColumn::booking_notes, "EUR", en()), booking_note(trips[0]));
    EXPECT_EQ(trip_cell_text(row, TripColumn::total, "HUF", en()), "25");
}

TEST(TripGrid, Set_trips_resets_the_selection_to_the_top)
{
    RoundTripGridModel grid{en_ptr()};
    EXPECT_FALSE(grid.selected());
    EXPECT_TRUE(grid.open_selected_urls().empty());
    EXPECT_FALSE(grid.outbound_url());
    grid.set_trips(rt01_trips(), "eur", "4 combinations \u00B7 \u2026");
    EXPECT_EQ(grid.count(), 4u);
    EXPECT_EQ(grid.currency(), "EUR");
    EXPECT_EQ(grid.summary_text(), "4 combinations \u00B7 \u2026");
    EXPECT_EQ(grid.header(TripColumn::total), "Total (EUR)");
    EXPECT_EQ(grid.current_row(), 0u);
    EXPECT_EQ(grid.take_scroll_request(), ScrollRequest::top);
    EXPECT_EQ(grid.selected(), &grid.trips()[0]);
    grid.mouse_down(3, MouseButton::right);
    EXPECT_EQ(grid.current_row(), 3u);
    EXPECT_EQ(grid.open_selected_urls().size(), 1u);  // the 50 trip: one combined booking
    EXPECT_EQ(grid.outbound_url(), grid.selected()->outbound->booking_url);
    EXPECT_EQ(grid.return_url(), grid.selected()->inbound->booking_url);
    grid.set_trips(rt01_trips(), "EUR", "again");  // every recompute: back to the top
    EXPECT_EQ(grid.current_row(), 0u);
    EXPECT_EQ(grid.open_selected_urls().size(), 2u);
    grid.set_trips({}, "EUR", "No combination with a stay of 2\u20135 nights. Change the nights or the dates.");
    EXPECT_FALSE(grid.current_row());
    EXPECT_EQ(grid.row_count(), 0u);
}

TEST(TripGrid, Default_sort_total_then_out_date_then_combiner_order)
{
    RoundTripGridModel grid{en_ptr()};
    std::vector<RoundTrip> trips = rt01_trips();
    std::swap(trips[0], trips[3]);  // insertion order no longer sorted
    trips[1].total = dec("50");     // ties on Total with trips[0] (50), later out date (11-10 vs 11-10)
    grid.set_trips(trips, "EUR", "");
    std::vector<std::string> totals;
    for (std::size_t row = 0; row < grid.row_count(); ++row) {
        totals.push_back(grid.cell_text(row, TripColumn::total));
    }
    EXPECT_EQ(totals, (std::vector<std::string>{"25.00", "40.00", "50.00", "50.00"}));
    EXPECT_EQ(grid.row(2).idx, 0u);  // equal total and out date: insertion order
    EXPECT_EQ(grid.row(3).idx, 1u);

    grid.sort_by_column(TripColumn::nights);
    EXPECT_EQ(grid.cell_text(0, TripColumn::nights), "2");
    grid.sort_by_column(TripColumn::nights);
    EXPECT_EQ(grid.cell_text(0, TripColumn::nights), "5");
    grid.set_trips(rt01_trips(), "EUR", "");  // the user sort persists
    EXPECT_EQ(grid.cell_text(0, TripColumn::nights), "5");
    grid.sort_by_column(TripColumn::idx);  // hidden column: ignored
    EXPECT_EQ(grid.user_sort()->column, TripColumn::nights);
}

TEST(TripGrid, Filter_checkboxes_report_changes)
{
    RoundTripGridModel grid{en_ptr()};
    EXPECT_FALSE(grid.require_same_home());
    EXPECT_TRUE(grid.set_require_same_home(true));
    EXPECT_FALSE(grid.set_require_same_home(true));
    EXPECT_TRUE(grid.set_require_same_away(true));
    EXPECT_TRUE(grid.require_same_away());
}

TEST(RecomputeTrips, Placeholder_without_both_legs)
{
    const LegSearchResult out = test::ui::section11_outbound();
    TripsView view = recompute_trips(nullptr, nullptr, 2, 10, false, false, "GBP", en());
    EXPECT_TRUE(view.trips.empty());
    EXPECT_EQ(view.currency, "GBP");  // the selected currency without an outbound result
    EXPECT_EQ(view.summary, "Search outbound and return flights to see the cheapest combinations here.");
    view = recompute_trips(&out, nullptr, 2, 10, false, false, "GBP", en());
    EXPECT_EQ(view.currency, "EUR");  // the outbound request's currency
    EXPECT_EQ(view.summary, "Search outbound and return flights to see the cheapest combinations here.");
}

TEST(RecomputeTrips, Worked_example_03_11)
{
    const LegSearchResult out = test::ui::section11_outbound();
    const LegSearchResult ret = test::ui::section11_return();
    TripsView view = recompute_trips(&out, &ret, 2, 5, false, false, "EUR", en());
    EXPECT_EQ(view.trips.size(), 19u);
    EXPECT_EQ(view.summary, "19 combinations \u00B7 cheapest 73.06 EUR: BTS\u2192LTN Thu 12 Nov, back LTN\u2192BUD Tue 17 "
                            "Nov (5 nights)");
    view = recompute_trips(&out, &ret, 2, 5, true, true, "EUR", en());
    ASSERT_FALSE(view.trips.empty());
    EXPECT_TRUE(view.trips.front().combined_booking_url.has_value());
    EXPECT_TRUE(view.summary.starts_with(std::to_string(view.trips.size()) + " combinations \u00B7 cheapest 77.84 EUR: "));
    view = recompute_trips(&out, &ret, 9, 10, false, false, "EUR", en());
    EXPECT_TRUE(view.trips.empty());
    EXPECT_EQ(view.summary, "No combination with a stay of 9\u201310 nights. Change the nights or the dates.");
}

TEST(RecomputeTrips, MatchLegs_problems_become_the_summary)
{
    const LegSearchResult out = test::ui::section11_outbound();
    LegSearchResult ret = test::ui::section11_return();
    ret.request.currency = "GBP";
    TripsView view = recompute_trips(&out, &ret, 2, 10, false, false, "EUR", en());
    EXPECT_TRUE(view.trips.empty());
    EXPECT_EQ(view.summary, "Outbound prices are in EUR but return prices in GBP. Run both with the same currency.");
    ret.request.currency = "EUR";
    ret.request.adults = 1;
    view = recompute_trips(&out, &ret, 2, 10, false, false, "EUR", en());
    EXPECT_EQ(view.summary,
              "Outbound was searched for 2 adult(s) but the return for 1. Search both legs with the same number.");
}

} // namespace
