// FlightGrid pure parts (01 §8): cell texts, notes, styling, sorting, view
// modes, streaming, selection semantics (05 §7.2 U-09), context menu,
// CopySelected and the self-test accessors; R-03 cost of 6000 streamed fares.
// Fixture map CAT, RATES, en-US.

#include "wizz/ui/flight_rows.hpp"

#include "wizz/core/pricing.hpp"
#include "wizz/core/result_views.hpp"

#include "ui_logic/ui_samples.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace wizz;
using namespace wizz::ui;
using test::ui::airport;
using test::ui::cat;
using test::ui::day;
using test::ui::dec;
using test::ui::en;
using test::ui::hand_fare;
using test::ui::rates;
using test::ui::station;

[[nodiscard]] std::shared_ptr<const Locale> en_ptr()
{
    return Locale::en_us();
}

/// The 12 CT-FSS-01 fares in insertion order (BUD response, then BTS).
[[nodiscard]] std::vector<FlightResultPtr> fss01()
{
    return test::samples::section11_flights();
}

[[nodiscard]] FlightResultPtr find_fare(const std::vector<FlightResultPtr>& fares, std::string_view to, Date date)
{
    const auto found = std::ranges::find_if(fares, [&](const FlightResultPtr& f) {
        return f->to_airport->iata == to && f->date() == date;
    });
    return found == fares.end() ? nullptr : *found;
}

[[nodiscard]] std::vector<std::string> display_routes(const FlightGridModel& grid)
{
    std::vector<std::string> routes;
    for (std::size_t row = 0; row < grid.row_count(); ++row) {
        const FlightResult& f = *grid.row(row).fare;
        routes.push_back(f.from_airport->iata + ">" + f.to_airport->iata + " " + f.date().iso());
    }
    return routes;
}

/// CT-RT-03 rows: BUD→LTN d 30, BTS→LTN d 20, BUD→BCN d+1 40, BUD→LGW d+1 35 (fare builder A).
[[nodiscard]] std::vector<FlightResultPtr> rt03_rows()
{
    return {test::samples::builder_a("BUD", "LTN", day(11, 10), "30"), test::samples::builder_a("BTS", "LTN", day(11, 10), "20"),
            test::samples::builder_a("BUD", "BCN", day(11, 11), "40"), test::samples::builder_a("BUD", "LGW", day(11, 11), "35")};
}

// ---------------------------------------------------------------------------
// Cell texts (01 §8.2, §8.5)
// ---------------------------------------------------------------------------

TEST(FlightRows, Fixture_row_2_departures_and_notes)
{
    const FlightResultPtr row2 = find_fare(fss01(), "LTN", day(11, 11));  // hasMacFlight, 06:00*, 08:30, 17:30
    ASSERT_TRUE(row2);
    EXPECT_EQ(departures_text(*row2), "06:00*, 08:30, 17:30");
    EXPECT_EQ(notes_text(*row2),
              "* = this fare; other times may land at another airport of the same group; total for 2 adults");

    const FlightResultPtr row1 = find_fare(fss01(), "LTN", day(11, 10));  // not merged
    EXPECT_EQ(departures_text(*row1), "06:00, 17:30");
    EXPECT_EQ(notes_text(*row1), "total for 2 adults");

    const FlightResultPtr row4 = find_fare(fss01(), "LTN", day(11, 13));  // priced at 12:50
    EXPECT_EQ(departures_text(*row4), "06:00, 08:30, 12:50*, 17:30");

    const FlightResultPtr bts = find_fare(fss01(), "LTN", day(11, 12));
    ASSERT_TRUE(bts);
}

TEST(FlightRows, Single_departure_is_never_starred)
{
    LegSearchRequest request = test::samples::budapest_to_london();
    request.adults = 1;
    DayFare fare = test::samples::day_fare("BUD", "LTN", day(11, 10), {6, 0}, {{6, 0}}, "30790.0", "HUF",
                                           FareStatus::available, true);
    const FlightResultPtr flight = pricing::build_result(*cat(), request, *rates(), fare);
    EXPECT_EQ(departures_text(*flight), "06:00");
    EXPECT_EQ(notes_text(*flight), "");
}

TEST(FlightRows, Notes_in_order_with_their_texts)
{
    // Check on site: no exchange-rate note and no "total for" note.
    LegSearchRequest request;
    request.origin = test::ui::place("Budapest");
    request.from = day(11, 2);
    request.to = day(11, 5);
    request.adults = 3;
    const std::vector<DayFare> rows = test::samples::checkprice_rows();
    const FlightResultPtr check = pricing::build_result(*cat(), request, *rates(), rows[0]);
    EXPECT_EQ(notes_text(*check), "price not shown by Wizz \u2014 check on wizzair.com");
    EXPECT_EQ(fare_local_text(*check, en()), "\u2014");

    // No rate for the target currency: the note names the fare's currency [WART].
    request.currency = "XYZ";
    const FlightResultPtr no_rate = pricing::build_result(*cat(), request, *rates(), rows[1]);
    EXPECT_FALSE(no_rate->total_converted);
    EXPECT_EQ(notes_text(*no_rate),
              "* = this fare; other times may land at another airport of the same group; no exchange rate for HUF; "
              "total for 3 adults");

    // P-18: LGW lies outside the destination radius.
    LegSearchRequest luton = test::samples::budapest_to_london();
    luton.destination = test::ui::place("London Luton");
    const pricing::CodeSet destinations{"LTN"};
    const FlightResultPtr lgw = pricing::build_result(*cat(), luton, *rates(), test::samples::bud_ltn_rows()[5],
                                                      nullptr, &destinations);
    EXPECT_TRUE(lgw->outside_destination_radius);
    EXPECT_EQ(notes_text(*lgw), "* = this fare; other times may land at another airport of the same group; lands at "
                                "London Gatwick, outside your destination radius (cheapest airport of its group that "
                                "day); total for 2 adults");
}

TEST(FlightRows, Cell_texts_and_values)
{
    const FlightResultPtr bts = find_fare(fss01(), "LTN", day(11, 12));
    const std::vector<FlightResultPtr> all = fss01();
    const FlightResultPtr bts_fare = *std::ranges::find_if(all, [](const FlightResultPtr& f) {
        return f->from_airport->iata == "BTS";
    });
    const FlightRow row = make_flight_row(7, bts_fare, en());
    EXPECT_EQ(row.idx, 7u);
    EXPECT_EQ(row.price_sort, dec("29.98"));
    EXPECT_EQ(row.date_text, "Thu 12 Nov 2026");
    EXPECT_EQ(row.departs, "07:15");
    EXPECT_EQ(row.from, "BTS  Bratislava");
    EXPECT_EQ(row.to, "LTN  London Luton");
    EXPECT_EQ(row.km_from_start, 173.0);  // 172.544
    EXPECT_EQ(row.km_from_dest, 44.0);    // 44.072
    EXPECT_EQ(row.fare_local, "14.99 EUR");
    EXPECT_EQ(flight_cell_text(row, FlightColumn::total, "EUR", en()), "29.98");
    EXPECT_EQ(flight_cell_text(row, FlightColumn::per_person, "EUR", en()), "14.99");
    EXPECT_EQ(flight_cell_text(row, FlightColumn::km_from_start, "EUR", en()), "173");
    EXPECT_EQ(flight_cell_text(row, FlightColumn::km_from_dest, "EUR", en()), "44");
    EXPECT_EQ(flight_cell_text(row, FlightColumn::date, "EUR", en()), "Thu 12 Nov 2026");

    const FlightResultPtr bud = find_fare(all, "LGW", day(11, 15));
    const FlightRow lgw_row = make_flight_row(0, bud, en());
    EXPECT_EQ(lgw_row.fare_local, "34,690 HUF");
    EXPECT_EQ(lgw_row.km_from_start, 0.0);
    EXPECT_EQ(lgw_row.km_from_dest, 41.0);  // 40.558

    // HUF grid: no minor unit.
    const FlightResultPtr huf = pricing::reprice(*bud, *rates(), "HUF");
    const FlightRow huf_row = make_flight_row(0, huf, en());
    EXPECT_EQ(flight_cell_text(huf_row, FlightColumn::total, "HUF", en()), "69,380");
    EXPECT_EQ(flight_cell_text(huf_row, FlightColumn::per_person, "HUF", en()), "34,690");
    (void)bts;
}

TEST(FlightRows, Grid_km_rounds_half_to_even)
{
    EXPECT_EQ(grid_km(0.5), 0.0);
    EXPECT_EQ(grid_km(1.5), 2.0);
    EXPECT_EQ(grid_km(2.5), 2.0);
    EXPECT_EQ(grid_km(172.5), 172.0);
    EXPECT_EQ(grid_km(173.5), 174.0);
    EXPECT_EQ(grid_km(40.558), 41.0);
}

TEST(FlightRows, Headers_and_visibility)
{
    EXPECT_EQ(flight_column_header(FlightColumn::total, "EUR"), "Total (EUR)");
    EXPECT_EQ(flight_column_header(FlightColumn::per_person, "HUF"), "Per person (HUF)");
    EXPECT_EQ(flight_column_header(FlightColumn::km_from_dest, "EUR"), "km from dest.");
    EXPECT_EQ(flight_column_header(FlightColumn::fare_local, "EUR"), "Fare (local)");
    std::vector<std::string> names;
    for (const FlightColumn column : kFlightColumns) {
        names.emplace_back(flight_column_name(column));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"Idx", "PriceSort", "Date", "Departs", "From", "km from start", "To",
                                               "km from dest.", "Total", "Per person", "Fare (local)", "All departures",
                                               "Notes"}));
    EXPECT_FALSE(flight_column_visible(FlightColumn::idx, false));
    EXPECT_FALSE(flight_column_visible(FlightColumn::price_sort, false));
    EXPECT_TRUE(flight_column_visible(FlightColumn::km_from_dest, false));
    EXPECT_FALSE(flight_column_visible(FlightColumn::km_from_dest, true));
    EXPECT_TRUE(flight_column_visible(FlightColumn::notes, true));
}

TEST(FlightRows, Copy_details_example_of_01_8_9)
{
    const Date sat = Date::from_ymd(2026, 12, 5);
    const FlightResultPtr flight =
        hand_fare(airport("KSC", "Kosice"), airport("LTN", "London Luton"), sat, 20, 40, "29.99", "EUR", "29.99");
    EXPECT_EQ(copy_details_text(*flight, en()),
              "Sat 05 Dec 2026 20:40  Kosice (KSC) \u2192 London Luton (LTN)  29.99 EUR per person (total 29.99 EUR)"
              "\r\nhttps://www.wizzair.com/en-gb/booking/select-flight/KSC/LTN/2026-12-05/null/1/0/0/null");

    FlightResult check = *flight;
    check.fare.status = FareStatus::check_on_site;
    check.total_converted.reset();
    check.price_per_person_converted.reset();
    EXPECT_EQ(copy_details_text(check, en()),
              "Sat 05 Dec 2026 20:40  Kosice (KSC) \u2192 London Luton (LTN)  price: check on site\r\n" +
                  check.booking_url);
}

TEST(FlightRows, Self_test_line_formats)
{
    const std::vector<FlightResultPtr> all = fss01();
    const FlightResultPtr bts_fare = *std::ranges::find_if(all, [](const FlightResultPtr& f) {
        return f->from_airport->iata == "BTS";
    });
    EXPECT_EQ(visible_row_text(make_flight_row(0, bts_fare, en()), en()),
              "2026-11-12 07:15 BTS  Bratislava -> LTN  London Luton total=29.98 local=14.99 EUR");
    EXPECT_EQ(current_row_text(bts_fare.get(), en()), "2026-11-12 BTS->LTN 29.98");
    EXPECT_EQ(current_row_text(nullptr, en()), "(none)");

    // The raw decimal keeps its own scale: 19290 HUF re-priced to HUF totals 38580 (T-08).
    const FlightResultPtr huf = pricing::reprice(*find_fare(all, "LTN", day(11, 14)), *rates(), "HUF");
    EXPECT_EQ(current_row_text(huf.get(), en()), "2026-11-14 BUD->LTN 38580");
    EXPECT_EQ(visible_row_text(make_flight_row(0, huf, en()), en()),
              "2026-11-14 06:00 BUD  Budapest -> LTN  London Luton total=38580 local=19,290 HUF");

    FlightResult no_total = *bts_fare;
    no_total.total_converted.reset();
    EXPECT_EQ(current_row_text(&no_total, en()), "2026-11-12 BTS->LTN ");  // trailing space
    EXPECT_EQ(visible_row_text(make_flight_row(0, std::make_shared<const FlightResult>(no_total), en()), en()),
              "2026-11-12 07:15 BTS  Bratislava -> LTN  London Luton total= local=14.99 EUR");
}

TEST(FlightRows, Row_tones)
{
    const std::vector<FlightResultPtr> all = fss01();
    const FlightRow bud_12 = make_flight_row(0, find_fare(all, "LTN", day(11, 12)), en());  // BUD→LTN 117.88
    EXPECT_EQ(flight_row_tone(bud_12, dec("117.880")), RowTone::cheapest);  // exact decimal equality, any scale
    EXPECT_EQ(flight_row_tone(bud_12, dec("29.98")), RowTone::normal);
    LegSearchRequest request;
    request.origin = test::ui::place("Budapest");
    const FlightResultPtr check = pricing::build_result(*cat(), request, *rates(), test::samples::checkprice_rows()[0]);
    EXPECT_EQ(make_flight_row(0, check, en()).price_sort, kNoPrice);
    EXPECT_EQ(flight_row_tone(make_flight_row(0, check, en()), kNoPrice), RowTone::unavailable);
}

// ---------------------------------------------------------------------------
// Sorting (01 §8.4)
// ---------------------------------------------------------------------------

TEST(FlightGrid, Default_sort_is_price_date_departs_then_insertion_order)
{
    FlightGridModel grid{en_ptr()};
    grid.clear("EUR", false, "Searching 3 route(s) from 3 airport(s)\u2026");
    grid.set_results(fss01(), "eur", "12 fares");
    EXPECT_EQ(grid.currency(), "EUR");
    EXPECT_EQ(display_routes(grid),
              (std::vector<std::string>{"BTS>LTN 2026-11-12", "BUD>LTN 2026-11-14", "BUD>LTN 2026-11-18",
                                        "BUD>LTN 2026-11-19", "BUD>LTN 2026-11-20", "BUD>LTN 2026-11-11",
                                        "BUD>LTN 2026-11-12", "BUD>LTN 2026-11-13", "BUD>LTN 2026-11-17",
                                        "BUD>LTN 2026-11-16", "BUD>LTN 2026-11-10", "BUD>LGW 2026-11-15"}));
    EXPECT_EQ(grid.cheapest(), dec("29.98"));
    EXPECT_EQ(grid.tone(0), RowTone::cheapest);
    EXPECT_EQ(grid.tone(1), RowTone::normal);
    EXPECT_EQ(grid.header(FlightColumn::total), "Total (EUR)");
}

TEST(FlightGrid, Equal_rows_keep_insertion_order_and_ties_are_all_highlighted)
{
    const FlightResultPtr a = test::samples::builder_a("BUD", "LTN", day(11, 10), "20");
    const FlightResultPtr b = test::samples::builder_a("BUD", "LTN", day(11, 10), "20");
    const FlightResultPtr c = test::samples::builder_a("BTS", "LTN", day(11, 10), "20");
    FlightGridModel grid{en_ptr()};
    grid.set_results(std::vector<FlightResultPtr>{b, c, a}, "EUR", "");
    EXPECT_EQ(grid.row(0).fare, b);
    EXPECT_EQ(grid.row(1).fare, c);
    EXPECT_EQ(grid.row(2).fare, a);
    for (std::size_t row = 0; row < 3; ++row) {
        EXPECT_EQ(grid.tone(row), RowTone::cheapest);
    }
}

TEST(FlightGrid, Header_click_sorts_one_column_and_toggles)
{
    FlightGridModel grid{en_ptr()};
    grid.set_results(fss01(), "EUR", "");
    grid.sort_by_column(FlightColumn::date);
    ASSERT_TRUE(grid.user_sort());
    EXPECT_EQ(grid.user_sort()->direction, SortDirection::ascending);
    EXPECT_EQ(display_routes(grid).front(), "BUD>LTN 2026-11-10");
    EXPECT_EQ(display_routes(grid)[2], "BUD>LTN 2026-11-12");  // equal dates: insertion order (BUD before BTS)
    EXPECT_EQ(display_routes(grid)[3], "BTS>LTN 2026-11-12");
    grid.sort_by_column(FlightColumn::date);
    EXPECT_EQ(grid.user_sort()->direction, SortDirection::descending);
    EXPECT_EQ(display_routes(grid).front(), "BUD>LTN 2026-11-20");
    EXPECT_EQ(display_routes(grid)[8], "BUD>LTN 2026-11-12");  // ties stay in insertion order when descending
    EXPECT_EQ(display_routes(grid)[9], "BTS>LTN 2026-11-12");
    grid.sort_by_column(FlightColumn::from);  // another column: ascending again
    EXPECT_EQ(grid.user_sort()->direction, SortDirection::ascending);
    EXPECT_EQ(display_routes(grid).front(), "BTS>LTN 2026-11-12");
    grid.sort_by_column(FlightColumn::price_sort);  // hidden: no header to click
    EXPECT_EQ(grid.user_sort()->column, FlightColumn::from);

    // The user's sort persists across rebuilds and later searches.
    grid.clear("EUR", false);
    grid.set_results(fss01(), "EUR", "");
    EXPECT_EQ(grid.user_sort()->column, FlightColumn::from);
    EXPECT_EQ(display_routes(grid).front(), "BTS>LTN 2026-11-12");
}

TEST(FlightGrid, Null_cells_sort_first_ascending)
{
    LegSearchRequest request;
    request.origin = test::ui::place("Budapest");
    request.from = day(11, 2);
    request.to = day(11, 5);
    std::vector<FlightResultPtr> fares;
    for (const DayFare& fare : test::samples::checkprice_rows()) {
        fares.push_back(pricing::build_result(*cat(), request, *rates(), fare));
    }
    FlightGridModel grid{en_ptr()};
    grid.set_results(fares, "EUR", "");
    EXPECT_EQ(grid.row(3).fare->fare.status, FareStatus::check_on_site);  // default sort: NoPrice last
    EXPECT_EQ(grid.tone(3), RowTone::unavailable);
    grid.sort_by_column(FlightColumn::total);
    EXPECT_EQ(grid.row(0).fare->fare.status, FareStatus::check_on_site);  // DBNull first
    EXPECT_EQ(grid.cell_text(0, FlightColumn::total), "");
    grid.sort_by_column(FlightColumn::total);
    EXPECT_EQ(grid.row(3).fare->fare.status, FareStatus::check_on_site);
}

TEST(FlightGrid, Strings_compare_case_insensitively_but_accent_sensitively)
{
    const Locale& locale = en();
    FlightRow a;
    FlightRow b;
    a.from = "BUD  budapest";
    b.from = "BUD  Budapest";
    EXPECT_EQ(compare_flight_rows(a, b, FlightColumn::from, locale), 0);
    a.from = "BUD  Budap\u00E9st";
    EXPECT_NE(compare_flight_rows(a, b, FlightColumn::from, locale), 0);
    a.km_from_dest.reset();
    b.km_from_dest = 0.0;
    EXPECT_LT(compare_flight_rows(a, b, FlightColumn::km_from_dest, locale), 0);
}

TEST(FlightGrid, CT_RT_03_Cheapest_per_day_is_still_ordered_by_price)
{
    FlightGridModel grid{en_ptr()};
    grid.clear("EUR", false);
    grid.set_results(rt03_rows(), "EUR", "");
    grid.choose_view(ViewMode::cheapest_per_day);
    EXPECT_EQ(display_routes(grid), (std::vector<std::string>{"BTS>LTN 2026-11-10", "BUD>LGW 2026-11-11"}));
    grid.choose_view(ViewMode::cheapest_per_destination);
    EXPECT_EQ(display_routes(grid), (std::vector<std::string>{"BTS>LTN 2026-11-10", "BUD>BCN 2026-11-11"}));
    EXPECT_EQ(grid.shown_results().size(), 2u);
    EXPECT_EQ(grid.all_results().size(), 4u);
    EXPECT_EQ(view_mode_name(grid.mode()), "CheapestPerDestination");
    EXPECT_EQ(view_mode_name(ViewMode::all_fares), "AllFares");
    EXPECT_EQ(view_mode_name(ViewMode::cheapest_per_day), "CheapestPerDay");
}

// ---------------------------------------------------------------------------
// View switching and selection (01 §8.6, §8.7; U-09)
// ---------------------------------------------------------------------------

TEST(FlightGrid, LIVE_5_Clear_switches_the_view_unless_the_user_chose_one)
{
    FlightGridModel grid{en_ptr()};
    grid.clear("EUR", true, "x");
    EXPECT_EQ(grid.mode(), ViewMode::cheapest_per_destination);
    EXPECT_TRUE(grid.anywhere());
    EXPECT_FALSE(grid.column_visible(FlightColumn::km_from_dest));
    EXPECT_EQ(grid.summary(), "x");
    grid.clear("EUR", false);
    EXPECT_EQ(grid.mode(), ViewMode::all_fares);
    EXPECT_FALSE(grid.user_chose_view());

    grid.choose_view(ViewMode::all_fares);  // the same item: no change event
    EXPECT_FALSE(grid.user_chose_view());
    grid.choose_view(ViewMode::cheapest_per_day);
    grid.choose_view(ViewMode::all_fares);  // back to All fares still counts as a choice
    EXPECT_TRUE(grid.user_chose_view());
    grid.clear("EUR", true);
    EXPECT_EQ(grid.mode(), ViewMode::all_fares);
}

TEST(FlightGrid, U_09_Pick_survives_rebuilds_by_identity_and_key)
{
    FlightGridModel grid{en_ptr()};
    const std::vector<FlightResultPtr> fares = fss01();
    grid.clear("EUR", false);
    EXPECT_FALSE(grid.selected());
    EXPECT_EQ(grid.current_row_text(), "(none)");
    grid.set_results(fares, "EUR", "");
    EXPECT_EQ(grid.current_row(), 0u);  // row 0 current, scrolled to the top
    EXPECT_EQ(grid.take_scroll_request(), ScrollRequest::top);
    EXPECT_FALSE(grid.explicit_selection());  // row 0 is not an explicit pick
    EXPECT_EQ(grid.selected(), grid.row(0).fare);

    grid.mouse_down(5, MouseButton::left);
    const FlightResultPtr pick = grid.row(5).fare;
    EXPECT_EQ(grid.explicit_selection(), pick);
    EXPECT_EQ(grid.current_row(), 5u);

    // Same objects, rebuilt: the pick is current again (identity).
    grid.choose_view(ViewMode::cheapest_per_day);
    grid.choose_view(ViewMode::all_fares);
    EXPECT_EQ(grid.selected(), pick);
    // Full reload: back to the top first, then the pick into view (01 §8.6-§8.7).
    EXPECT_EQ(grid.take_scroll_request(), ScrollRequest::top_then_current);

    // Re-priced objects: restored by (From, To, Date); the stored pick stays the old object.
    std::vector<FlightResultPtr> repriced;
    for (const FlightResultPtr& f : fares) {
        repriced.push_back(pricing::reprice(*f, *rates(), "HUF"));
    }
    grid.set_results(result_views::sort_by_price(repriced), "HUF", "");
    ASSERT_TRUE(grid.selected());
    EXPECT_NE(grid.selected(), pick);
    EXPECT_EQ(grid.selected()->date(), pick->date());
    EXPECT_EQ(grid.selected()->to_airport->iata, pick->to_airport->iata);
    EXPECT_EQ(grid.explicit_selection(), pick);

    // Gone from the results: row 0 + top, and row 0 does NOT become the pick.
    std::vector<FlightResultPtr> without;
    for (const FlightResultPtr& f : fares) {
        if (!(f->date() == pick->date() && f->from_airport->iata == pick->from_airport->iata)) {
            without.push_back(f);
        }
    }
    grid.set_results(without, "EUR", "");
    EXPECT_EQ(grid.current_row(), 0u);
    EXPECT_EQ(grid.take_scroll_request(), ScrollRequest::top);
    EXPECT_EQ(grid.explicit_selection(), pick);
    EXPECT_NE(grid.selected(), pick);

    // Only Clear resets the pick.
    grid.clear("EUR", false);
    EXPECT_FALSE(grid.explicit_selection());
    grid.set_results(fares, "EUR", "");
    EXPECT_FALSE(grid.explicit_selection());
}

TEST(FlightGrid, Keyboard_navigation_sets_the_pick_on_key_up)
{
    FlightGridModel grid{en_ptr()};
    grid.set_results(fss01(), "EUR", "");
    grid.set_current_row(4);  // arrow keys move the current row …
    EXPECT_FALSE(grid.explicit_selection());
    grid.navigation_key_up();  // … and the key-up makes it the pick
    EXPECT_EQ(grid.explicit_selection(), grid.row(4).fare);
    grid.mouse_down(99, MouseButton::left);  // not a data cell (header / empty space)
    EXPECT_EQ(grid.explicit_selection(), grid.row(4).fare);
    grid.mouse_down(2, MouseButton::right);  // right button: pick and current
    EXPECT_EQ(grid.explicit_selection(), grid.row(2).fare);
    EXPECT_EQ(grid.current_row(), 2u);
    grid.mouse_down(6, MouseButton::middle);  // any button picks
    EXPECT_EQ(grid.explicit_selection(), grid.row(6).fare);
    EXPECT_EQ(grid.current_row(), 2u);
}

TEST(FlightGrid, Cheapest_per_destination_falls_back_to_the_picks_group)
{
    FlightGridModel grid{en_ptr()};
    grid.clear("EUR", true);  // anywhere → CheapestPerDestination
    const std::vector<FlightResultPtr> rows = rt03_rows();
    grid.set_results(rows, "EUR", "");
    ASSERT_EQ(grid.row_count(), 2u);
    // Pick BUD→BCN, then a result without it but with another BCN fare.
    grid.mouse_down(1, MouseButton::left);
    const FlightResultPtr bcn = test::samples::builder_a("BTS", "BCN", day(11, 12), "50");
    grid.set_results(std::vector<FlightResultPtr>{rows[0], rows[1], rows[3], bcn}, "EUR", "");
    EXPECT_EQ(grid.selected(), bcn);  // same ToAirport.GroupKey
    EXPECT_EQ(grid.take_scroll_request(), ScrollRequest::top_then_current);
}

TEST(FlightGrid, Restore_selection_takes_the_first_display_match)
{
    FlightGridModel grid{en_ptr()};
    const FlightResultPtr older = test::samples::builder_a("BUD", "LTN", day(11, 10), "30");
    const FlightResultPtr better = test::samples::builder_a("BUD", "LTN", day(11, 10), "25");  // same key, cheaper
    grid.set_results(std::vector<FlightResultPtr>{older, better}, "EUR", "");
    grid.restore_selection(older, true);
    EXPECT_EQ(grid.selected(), better);  // display order first, identity or key alike
    grid.restore_selection(test::samples::builder_a("BTS", "BCN", day(11, 1), "1"), false);
    EXPECT_EQ(grid.selected(), better);  // missing and no scroll-to-top: unchanged
    grid.set_current_row(1);
    grid.restore_selection(nullptr, true);
    EXPECT_EQ(grid.current_row(), 0u);
}

// ---------------------------------------------------------------------------
// Streaming (01 §8.6)
// ---------------------------------------------------------------------------

TEST(FlightGrid, All_fares_streaming_keeps_the_current_fare_and_scroll)
{
    FlightGridModel grid{en_ptr()};
    grid.clear("EUR", false, "Searching 3 route(s) from 3 airport(s)\u2026");
    (void)grid.take_scroll_request();
    const std::vector<FlightResultPtr> fares = fss01();
    const std::vector<FlightResultPtr> first(fares.begin(), fares.begin() + 11);  // BUD batch
    grid.append(first);
    EXPECT_EQ(grid.row_count(), 11u);
    EXPECT_EQ(grid.selected(), fares[0]);  // the first row added becomes current
    EXPECT_EQ(grid.take_scroll_request(), ScrollRequest::none);
    const std::size_t before = *grid.current_row();
    grid.append(std::vector<FlightResultPtr>{fares[11]});  // the cheaper BTS fare goes above
    EXPECT_EQ(grid.row(0).fare, fares[11]);
    EXPECT_EQ(grid.selected(), fares[0]);  // same fare, index shifted
    EXPECT_EQ(*grid.current_row(), before + 1);
    EXPECT_EQ(grid.take_scroll_request(), ScrollRequest::none);
    EXPECT_EQ(grid.cheapest(), dec("29.98"));  // lowered incrementally
    EXPECT_EQ(grid.all_results().size(), 12u);
}

TEST(FlightGrid, All_fares_streaming_returns_to_an_explicit_pick)
{
    FlightGridModel grid{en_ptr()};
    grid.clear("EUR", false);
    const std::vector<FlightResultPtr> fares = fss01();
    grid.append(std::vector<FlightResultPtr>(fares.begin(), fares.begin() + 6));
    grid.mouse_down(3, MouseButton::left);
    const FlightResultPtr pick = grid.explicit_selection();
    grid.set_current_row(0);  // the user moved away without a key-up (e.g. a click elsewhere)
    (void)grid.take_scroll_request();
    grid.append(std::vector<FlightResultPtr>(fares.begin() + 6, fares.end()));
    EXPECT_EQ(grid.selected(), pick);
    EXPECT_EQ(grid.take_scroll_request(), ScrollRequest::current);  // into view, not to the top
}

TEST(FlightGrid, Grouped_views_rebuild_on_every_flush)
{
    FlightGridModel grid{en_ptr()};
    grid.clear("EUR", true);
    const std::vector<FlightResultPtr> rows = rt03_rows();
    grid.append(std::vector<FlightResultPtr>{rows[0], rows[2]});
    grid.set_current_row(1);
    (void)grid.take_scroll_request();
    grid.append(std::vector<FlightResultPtr>{rows[1], rows[3]});
    EXPECT_EQ(grid.current_row(), 0u);  // full reload: row 0 + top
    EXPECT_EQ(grid.take_scroll_request(), ScrollRequest::top);
    EXPECT_EQ(display_routes(grid), (std::vector<std::string>{"BTS>LTN 2026-11-10", "BUD>BCN 2026-11-11"}));

    grid.mouse_down(1, MouseButton::left);  // pick BUD→BCN
    grid.append(std::vector<FlightResultPtr>{test::samples::builder_a("BTS", "LGW", day(11, 12), "5")});
    EXPECT_EQ(grid.selected()->to_airport->iata, "BCN");  // pick restored after the rebuild
    grid.append({});
    EXPECT_EQ(grid.all_results().size(), 5u);
}

// ---------------------------------------------------------------------------
// Context menu, CopySelected and accessors (01 §8.9, §8.10)
// ---------------------------------------------------------------------------

TEST(FlightGrid, UI_9_Context_menu_state)
{
    FlightGridModel outbound{en_ptr(), true};
    EXPECT_EQ(outbound.context_menu(false),
              (FlightMenuState{.open_enabled = false,
                               .copy_enabled = false,
                               .plan_return_visible = true,
                               .plan_return_enabled = false,
                               .plan_return_text = "Search return flights from this destination"}));
    outbound.set_results(fss01(), "EUR", "");
    EXPECT_EQ(outbound.context_menu(false),
              (FlightMenuState{.open_enabled = true,
                               .copy_enabled = true,
                               .plan_return_visible = true,
                               .plan_return_enabled = true,
                               .plan_return_text = "Search return flights from this destination"}));
    EXPECT_EQ(outbound.context_menu(true),
              (FlightMenuState{.open_enabled = true,
                               .copy_enabled = true,
                               .plan_return_visible = true,
                               .plan_return_enabled = false,
                               .plan_return_text = "Search return flights from this destination (finish or cancel "
                                                   "the current search first)"}));
    FlightGridModel inbound{en_ptr(), false};
    inbound.set_results(fss01(), "EUR", "");
    EXPECT_FALSE(inbound.context_menu(false).plan_return_visible);
    EXPECT_FALSE(inbound.context_menu(false).plan_return_enabled);
}

TEST(FlightGrid, Self_test_accessors)
{
    FlightGridModel grid{en_ptr()};
    EXPECT_FALSE(grid.copy_selected_text());
    grid.set_results(fss01(), "EUR", "12 fares");
    const std::vector<std::string> top = grid.visible_rows_text(8);
    ASSERT_EQ(top.size(), 8u);
    EXPECT_EQ(top[0], "2026-11-12 07:15 BTS  Bratislava -> LTN  London Luton total=29.98 local=14.99 EUR");
    EXPECT_EQ(top[1], "2026-11-14 06:00 BUD  Budapest -> LTN  London Luton total=105.32 local=19,290 HUF");
    EXPECT_EQ(grid.visible_rows_text(100).size(), 12u);
    EXPECT_EQ(grid.current_row_text(), "2026-11-12 BTS->LTN 29.98");
    ASSERT_TRUE(grid.copy_selected_text());
    EXPECT_TRUE(grid.copy_selected_text()->starts_with(
        "Thu 12 Nov 2026 07:15  Bratislava (BTS) \u2192 London Luton (LTN)  14.99 EUR per person (total 29.98 EUR)\r\n"));
    EXPECT_EQ(grid.cell_text(0, FlightColumn::notes), "total for 2 adults");
    EXPECT_EQ(grid.summary(), "12 fares");
    grid.set_summary("x");
    EXPECT_EQ(grid.summary(), "x");
}

// ---------------------------------------------------------------------------
// R-03: no grid operation may take 100 ms (6000 fares in batches of 40)
// ---------------------------------------------------------------------------

TEST(FlightGrid, R_03_Streaming_6000_fares_stays_cheap)
{
    const std::vector<FlightResultPtr> base = fss01();
    std::vector<FlightResultPtr> fares;
    fares.reserve(6000);
    for (int i = 0; i < 6000; ++i) {
        FlightResult copy = *base[static_cast<std::size_t>(i) % base.size()];
        copy.fare.date = copy.fare.date.add_days(i / 12);
        copy.total_converted = *copy.total_converted + Decimal{i % 37};
        fares.push_back(std::make_shared<const FlightResult>(std::move(copy)));
    }
    FlightGridModel grid{en_ptr()};
    grid.clear("EUR", false);
    auto slowest = std::chrono::steady_clock::duration::zero();
    for (std::size_t start = 0; start < fares.size(); start += 40) {
        const auto begin = std::chrono::steady_clock::now();
        grid.append(std::span<const FlightResultPtr>{fares}.subspan(start, 40));
        slowest = std::max(slowest, std::chrono::steady_clock::now() - begin);
    }
    EXPECT_EQ(grid.row_count(), 6000u);
    const auto begin = std::chrono::steady_clock::now();
    grid.set_results(result_views::sort_by_price(fares), "EUR", "6000 fares");
    const auto rebuild = std::chrono::steady_clock::now() - begin;
#ifdef NDEBUG
    constexpr auto kBudget = std::chrono::milliseconds{100};
#else
    constexpr auto kBudget = std::chrono::milliseconds{1000};  // Debug iterator checks; the release gate is 100 ms
#endif
    EXPECT_LT(slowest, kBudget);
    EXPECT_LT(rebuild, kBudget);
    for (std::size_t row = 1; row < grid.row_count(); ++row) {
        ASSERT_LE(grid.row(row - 1).price_sort, grid.row(row).price_sort);
    }
}

} // namespace
