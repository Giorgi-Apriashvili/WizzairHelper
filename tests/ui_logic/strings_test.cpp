// The UI string catalogue (01 §18 incl. the §18.5 index). Expected texts are
// written with \u escapes so every typographic character is checked by code
// point: — U+2014, – U+2013, → U+2192, … U+2026, “ ” U+201C/U+201D, · U+00B7,
// ⏳ U+23F3, © U+00A9.

#include "wizz/ui/strings.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using namespace wizz::ui::strings;

TEST(Strings, Shell_and_crash_texts)
{
    EXPECT_EQ(kAppCaption, "WizzairHelper");
    EXPECT_EQ(kWindowTitle, "WizzairHelper \u2014 cheapest Wizz Air flights from nearby airports");
    EXPECT_EQ(kUnexpectedErrorText,
              "Stopped by an unexpected error \u2014 details are in %LOCALAPPDATA%\\WizzairHelper\\errors.log.");
    EXPECT_EQ(crash_message("Boom."),
              "Something went wrong:\n\nBoom.\n\nDetails were written to %LOCALAPPDATA%\\WizzairHelper\\errors.log.");
    EXPECT_EQ(errors_log_entry(wizz::make_local(wizz::Date::from_ymd(2026, 11, 2), 14, 46, 14), "System.Exception: x"),
              "[2026-11-02T14:46:14] System.Exception: x\r\n\r\n");
}

TEST(Strings, Input_panel_texts)
{
    EXPECT_EQ(kHintAnywhere, "(empty \u201CTo\u201D = anywhere)");
    EXPECT_EQ(kHintReturn, "Return flights go from the destination area back to any airport near home.");
    EXPECT_EQ(kTooltipFrom,
              "City or airport you start from (any city works \u2014 nearby Wizz Air airports are found automatically)");
    EXPECT_EQ(kTooltipTo, "Destination city or airport. Leave empty to search ANYWHERE Wizz Air flies.");
    EXPECT_EQ(kFareItems[0], "Regular fares");
    EXPECT_EQ(kFareItems[1], "Wizz Discount Club fares");
    EXPECT_EQ(kResolvedDefault, "Fares come from Wizz Air's website. Double-click a result to open it on wizzair.com.");
    EXPECT_EQ(kLabelRoundTripsStay, "Round trips: stay");
    EXPECT_EQ(kSearchOutbound, "Search outbound");
    EXPECT_EQ(kSearchReturn, "Search return");
}

TEST(Strings, Currency_list_order_and_exact_matching)
{
    const std::string joined = [] {
        std::string text;
        for (const std::string_view code : kCurrencies) {
            text += std::string{code} + ",";
        }
        return text;
    }();
    EXPECT_EQ(joined, "EUR,GBP,USD,HUF,PLN,RON,CZK,CHF,SEK,NOK,DKK,GEL,RSD,MKD,ALL,BAM,MDL,AED,UAH,TRY,ILS,");
    EXPECT_TRUE(is_listed_currency("EUR"));
    EXPECT_TRUE(is_listed_currency("ILS"));
    EXPECT_FALSE(is_listed_currency("eur"));  // case-sensitive, ordinal
    EXPECT_FALSE(is_listed_currency("JPY"));
    EXPECT_FALSE(is_listed_currency(""));
    EXPECT_FALSE(is_listed_currency("EUR "));
}

TEST(Strings, Tab_titles)
{
    EXPECT_EQ(tab_title(kTabOutbound, 56), "Outbound (56)");
    EXPECT_EQ(tab_title(kTabReturn, 0), "Return");
    EXPECT_EQ(tab_title(kTabRoundTrips, 500), "Round trips (500)");
    EXPECT_EQ(kTabLog, "Log");
}

TEST(Strings, Busy_panel_texts)
{
    EXPECT_EQ(kReady, "Ready.");
    EXPECT_EQ(kCancel, "Cancel");
    EXPECT_EQ(kCancelling, "Cancelling\u2026 (finishing the current request)");
    EXPECT_EQ(kBeginLoadingMap, "Loading Wizz Air route map\u2026");
    EXPECT_EQ(kBeginFindingPlaces, "Finding the places you typed\u2026");
    EXPECT_EQ(fetch_begin("Outbound", 7, "9 s"), "Outbound: starting (7 requests, about 9 s)\u2026");
    EXPECT_EQ(retry_countdown("4.2"), "\u23F3 retrying in 4.2 s");
    EXPECT_EQ(waiting_for_server("6"), "waiting for server\u2026 6 s");
    EXPECT_EQ(map_ready(11),
              "Ready \u2014 11 Wizz Air airports loaded. Choose your trip and press \u201CSearch outbound\u201D.");
    EXPECT_EQ(kMapLoadCancelled,
              "Cancelled. The route map is needed for searching \u2014 press a Search button to try again.");
    EXPECT_EQ(map_load_failed("x"), "Could not load the Wizz Air route map: x");
    EXPECT_EQ(map_unreachable("x"), "Could not reach wizzair.com (offline?): x");
    EXPECT_EQ(kPlacesFound, "Places found.");
    EXPECT_EQ(kSearchNotStartedCancelled, "Search not started (cancelled).");
    EXPECT_EQ(kSearchNotStarted, "Search not started.");
    EXPECT_EQ(place_lookup_failed("timeout"), "Place lookup failed: timeout");
    EXPECT_EQ(kNothingToSearch, "Nothing to search.");
    EXPECT_EQ(kCancelled, "Cancelled.");
    EXPECT_EQ(kStageFindingPlaces, "Finding places");
    EXPECT_EQ(kStageWaitingForChoice, "Waiting for your choice");
}

TEST(Strings, Place_texts_E5_to_E10)
{
    EXPECT_EQ(kLookupOffline, "Could not reach the place-lookup service (offline?). Type a Wizz Air city or airport "
                              "instead, e.g. \u201CLjubljana\u201D or \u201CLJU\u201D.");
    EXPECT_EQ(place_not_found("Grazz"),
              "Could not find \u201CGrazz\u201D. Check the spelling, or type a nearby city or airport.");
    EXPECT_EQ(not_an_airport_code("STN"),
              "\u201CSTN\u201D is not a Wizz Air airport code, so it was looked up as a place name.");
    EXPECT_EQ(which_place("Londn"), "which place is \u201CLondn\u201D?");
    EXPECT_EQ(interpreted("Londn", "London"), "Interpreted \u201CLondn\u201D as \u201CLondon\u201D.");
    EXPECT_EQ(chooser_title(kRoleStartingPoint), "Which starting point?");
    EXPECT_EQ(chooser_title(kRoleDestination), "Which destination?");
    EXPECT_EQ(chooser_intro("Londn", kRoleDestination, 1, std::nullopt),
              "Did you mean this place for \u201CLondn\u201D?");
    EXPECT_EQ(chooser_intro("Londn", kRoleDestination, 2, std::nullopt),
              "Did you mean one of these places for \u201CLondn\u201D? Pick the destination:");
    EXPECT_EQ(chooser_intro("stn", kRoleStartingPoint, 2, not_an_airport_code("STN")),
              "\u201CSTN\u201D is not a Wizz Air airport code, so it was looked up as a place name.\r\nDid you mean one "
              "of these places for \u201Cstn\u201D? Pick the starting point:");
    EXPECT_EQ(kUseThisPlace, "Use this place");
    EXPECT_EQ(kEndBeforeStart, "The end date is before the start date.");
    EXPECT_EQ(kTypeStart, "Type where you start from (a city or airport).");
}

TEST(Strings, Leg_texts)
{
    EXPECT_EQ(kBigSearchCaption, "Big search");
    EXPECT_EQ(request_cap(501, 11),
              "This search would need 501 requests to Wizz Air (about 11 min). That is more than the 500 this app "
              "allows, to stay polite to the site. Use a smaller radius, fewer dates, or a specific destination.");
    EXPECT_EQ(big_search(61, "1 min 19 s"),
              "This search needs 61 requests to Wizz Air (about 1 min 19 s, longer if Wizz Air slows us down).\n\nTip: "
              "a smaller radius or a specific destination makes it much faster.\n\nContinue?");
    EXPECT_EQ(searching_routes(3, 3), "Searching 3 route(s) from 3 airport(s)\u2026");
    EXPECT_EQ(fares_so_far(11), "11 fares so far\u2026");
    EXPECT_EQ(leg_cancelled_log("Outbound"), "Outbound: cancelled.");
    EXPECT_EQ(leg_error_log("Return", "boom"), "Return ERROR: boom");
    EXPECT_EQ(notice_log("Outbound", "Wizz Air is busy (HTTP 503) \u2014 retry 1 of 3"),
              "  Outbound: Wizz Air is busy (HTTP 503) \u2014 retry 1 of 3");
    EXPECT_EQ(kSummaryCancelledEarly, " \u00B7 cancelled early");
    EXPECT_EQ(kSummaryStoppedEarly, " \u00B7 stopped early");
}

TEST(Strings, Round_trip_texts_F6_to_F8)
{
    EXPECT_EQ(kTripsPlaceholder, "Search outbound and return flights to see the cheapest combinations here.");
    EXPECT_EQ(no_combination(2, 10), "No combination with a stay of 2\u201310 nights. Change the nights or the dates.");
    EXPECT_EQ(open_jaw("LTN", "LGW"), "open jaw: land LTN, fly back from LGW");
    EXPECT_EQ(back_to("BUD", "BTS"), "back to BUD (left from BTS)");
    EXPECT_EQ(kOneBooking, "one booking");
    EXPECT_EQ(kTwoBookings, "two one-way bookings");
    EXPECT_EQ(kSameHomeCheckbox, "Return to the airport I left from");
    EXPECT_EQ(kSameAwayCheckbox, "Fly back from the airport I landed at");
    EXPECT_EQ(kOpenOnWizzair, "Open on wizzair.com");
    EXPECT_EQ(kOpenOutboundOnly, "Open outbound only");
    EXPECT_EQ(kOpenReturnOnly, "Open return only");
}

TEST(Strings, Grid_texts)
{
    EXPECT_EQ(kShowLabel, "Show:");
    EXPECT_EQ(kViewItems[0], "All fares");
    EXPECT_EQ(kViewItems[1], "Cheapest per day");
    EXPECT_EQ(kViewItems[2], "Cheapest per destination");
    EXPECT_EQ(kNoteMergedDay, "* = this fare; other times may land at another airport of the same group");
    EXPECT_EQ(kNoteCheckOnSite, "price not shown by Wizz \u2014 check on wizzair.com");
    EXPECT_EQ(note_no_rate("HUF"), "no exchange rate for HUF");
    EXPECT_EQ(note_outside_radius("London Gatwick"),
              "lands at London Gatwick, outside your destination radius (cheapest airport of its group that day)");
    EXPECT_EQ(note_total_for(2), "total for 2 adults");
    EXPECT_EQ(kNoLocalFare, "\u2014");
    EXPECT_EQ(kCopyDetails, "Copy details");
    EXPECT_EQ(kPlanReturnMenu, "Search return flights from this destination");
    EXPECT_EQ(kPlanReturnMenuBusy,
              "Search return flights from this destination (finish or cancel the current search first)");
    EXPECT_EQ(browser_error("No application is associated.", "https://x"),
              "Could not open the browser:\nNo application is associated.\n\nhttps://x");
    EXPECT_EQ(kClipboardBusy, "The clipboard is busy (another program is using it). Try again.");
}

TEST(Strings, Log_texts)
{
    EXPECT_EQ(kStartedLog, "WizzairHelper started. Fares: wizzair.com (unofficial website API). Geocoding: Photon / "
                           "Nominatim, data \u00A9 OpenStreetMap contributors. Exchange rates: Frankfurter / "
                           "currency-api.");
    EXPECT_EQ(route_map_log(11, std::string{"https://be.wizzair.com/29.23.1/Api"}),
              "Route map: 11 airports, API https://be.wizzair.com/29.23.1/Api.");
    EXPECT_EQ(route_map_log(189, std::nullopt), "Route map: 189 airports (from the local cache).");
    EXPECT_EQ(map_error_log("x"), "ERROR loading route map: x");
    EXPECT_EQ(return_from_pick_log("London Luton (LTN)", "22 Nov", "30 Nov"),
              "Return will start from London Luton (LTN) (picked in the outbound results), 22 Nov \u2013 30 Nov.");
    EXPECT_EQ(screenshot_failed("1-outbound.png", "Access is denied."), "screenshot 1-outbound.png failed: Access is denied.");
    EXPECT_EQ(kSameAreaLogLine, "  not searched: start and destination are the same area");
    EXPECT_EQ(kNoResult, "no result");
}

} // namespace
