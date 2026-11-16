#pragma once
// The desktop app's own user-visible texts (01 §18, incl. the §18.5 index):
// every caption, label, tooltip, busy-panel line, MessageBox, grid and log text
// the MainForm / BusyPanel / grids / chooser produce. Texts that core or
// services produce (plan notes A, search warnings and stages B, client notices
// C, client errors D, place descriptions E1–E4, MatchLegs problems F1–F5) are
// NOT repeated here; the UI shows those verbatim.
//
// All texts are UTF-8 and copied from 01 with their typographic characters:
// — U+2014, – U+2013, → U+2192, … U+2026, “ ” U+201C/U+201D, · U+00B7,
// ⏳ U+23F3, © U+00A9. Interpolated values are passed already formatted
// (dates, money, labels go through the locale service, 07 §1-§2); counts are
// plain integers.

#include "wizz/core/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wizz::ui::strings {

// ---------------------------------------------------------------------------
// Application shell (01 §3.2, §4.1, §18.1)
// ---------------------------------------------------------------------------

/// MessageBox caption of every box except "Big search" (01 §18.1).
inline constexpr std::string_view kAppCaption = "WizzairHelper";
/// Main window title (01 §4.1).
inline constexpr std::string_view kWindowTitle = "WizzairHelper — cheapest Wizz Air flights from nearby airports";
/// Busy line of every phase's `finally` safety net (01 §11.9, §18.2).
inline constexpr std::string_view kUnexpectedErrorText =
    "Stopped by an unexpected error — details are in %LOCALAPPDATA%\\WizzairHelper\\errors.log.";

/// Crash box text (01 §3.2): "Something went wrong:\n\n{message}\n\nDetails were
/// written to %LOCALAPPDATA%\WizzairHelper\errors.log." (the literal %LOCALAPPDATA%).
[[nodiscard]] std::string crash_message(std::string_view exception_message);
/// One errors.log record (01 §3.2): "[" + now "s" (local yyyy-MM-ddTHH:mm:ss) + "] " + text + CRLF CRLF.
[[nodiscard]] std::string errors_log_entry(LocalDateTime now, std::string_view exception_text);

// ---------------------------------------------------------------------------
// Input panel (01 §4.4)
// ---------------------------------------------------------------------------

inline constexpr std::string_view kLabelFrom = "From";
inline constexpr std::string_view kLabelWithin = "within";
inline constexpr std::string_view kLabelKm = "km";
inline constexpr std::string_view kLabelTo = "To";
inline constexpr std::string_view kHintAnywhere = "(empty “To” = anywhere)";
inline constexpr std::string_view kLabelAnd = "and";
inline constexpr std::string_view kLabelAdults = "Adults";
inline constexpr std::string_view kLabelPricesIn = "Prices in";
inline constexpr std::string_view kLabelRoundTripsStay = "Round trips: stay";
inline constexpr std::string_view kLabelNightsTo = "to";
inline constexpr std::string_view kLabelNights = "nights";
inline constexpr std::string_view kHintReturn = "Return flights go from the destination area back to any airport near home.";
inline constexpr std::string_view kSearchOutbound = "Search outbound";
inline constexpr std::string_view kSearchReturn = "Search return";
// The grouped input area (DECISIONS D-53, D-54) names its sections, and 01's
// "Outbound between" / "Return between" became the section name + "between".
inline constexpr std::string_view kSectionWhere = "Where";
inline constexpr std::string_view kSectionTravellers = "Travellers & prices";
inline constexpr std::string_view kSectionOutbound = "Outbound";
inline constexpr std::string_view kSectionReturn = "Return";
inline constexpr std::string_view kLabelBetween = "between";
inline constexpr std::string_view kTooltipFrom =
    "City or airport you start from (any city works — nearby Wizz Air airports are found automatically)";
inline constexpr std::string_view kTooltipTo = "Destination city or airport. Leave empty to search ANYWHERE Wizz Air flies.";
/// `_fare` items; index 1 = Wizz Discount Club (01 §4.4).
inline constexpr std::array<std::string_view, 2> kFareItems{"Regular fares", "Wizz Discount Club fares"};
/// Date-picker custom format, e.g. "Fri 20 Nov 2026" (01 §4.4).
inline constexpr std::string_view kDatePickerFormat = "ddd dd MMM yyyy";
/// `_resolved` text before the first search (ASCII apostrophe, 01 §4.4).
inline constexpr std::string_view kResolvedDefault =
    "Fares come from Wizz Air's website. Double-click a result to open it on wizzair.com.";

/// The "Prices in" list, in display order (01 §4.4).
inline constexpr std::array<std::string_view, 21> kCurrencies{
    "EUR", "GBP", "USD", "HUF", "PLN", "RON", "CZK", "CHF", "SEK", "NOK", "DKK",
    "GEL", "RSD", "MKD", "ALL", "BAM", "MDL", "AED", "UAH", "TRY", "ILS"};
/// True when `code` is in kCurrencies, compared exactly (case-sensitive, ordinal:
/// "eur" is not listed; 01 §4.4, §13, §17.1).
[[nodiscard]] bool is_listed_currency(std::string_view code) noexcept;

// ---------------------------------------------------------------------------
// Result tabs (01 §5)
// ---------------------------------------------------------------------------

inline constexpr std::string_view kTabOutbound = "Outbound";
inline constexpr std::string_view kTabReturn = "Return";
inline constexpr std::string_view kTabRoundTrips = "Round trips";
inline constexpr std::string_view kTabLog = "Log";
/// `UpdateTabTitles`: "{base} ({count})" when count > 0, else "{base}" ("Outbound (12)", "Return").
[[nodiscard]] std::string tab_title(std::string_view base, std::size_t count);

// ---------------------------------------------------------------------------
// Busy panel (01 §7, §10, §18.2, §18.3)
// ---------------------------------------------------------------------------

inline constexpr std::string_view kReady = "Ready.";
inline constexpr std::string_view kCancel = "Cancel";
inline constexpr std::string_view kCancelling = "Cancelling… (finishing the current request)";
inline constexpr std::string_view kBeginLoadingMap = "Loading Wizz Air route map…";
inline constexpr std::string_view kBeginFindingPlaces = "Finding the places you typed…";
/// MainForm progress stages (01 §11.5, §18.3 E8).
inline constexpr std::string_view kStageFindingPlaces = "Finding places";
inline constexpr std::string_view kStageWaitingForChoice = "Waiting for your choice";
/// "{label}: starting ({requests} requests, about {duration})…" (01 §7, §11.9).
[[nodiscard]] std::string fetch_begin(std::string_view label, std::size_t requests, std::string_view duration_text);
/// "⏳ retrying in {seconds} s" (01 §10.5; `seconds` is the "0.0" label).
[[nodiscard]] std::string retry_countdown(std::string_view seconds_text);
/// "waiting for server… {seconds} s" (01 §10.5; `seconds` is the "0" label).
[[nodiscard]] std::string waiting_for_server(std::string_view seconds_text);

/// "Ready — {N} Wizz Air airports loaded. Choose your trip and press “Search outbound”." (green, 01 §11.2).
[[nodiscard]] std::string map_ready(std::size_t real_airports);
inline constexpr std::string_view kMapLoadCancelled =
    "Cancelled. The route map is needed for searching — press a Search button to try again.";
/// "Could not load the Wizz Air route map: {message}" (WizzApiException, red).
[[nodiscard]] std::string map_load_failed(std::string_view message);
/// "Could not reach wizzair.com (offline?): {message}" (HTTP / IO errors, red).
[[nodiscard]] std::string map_unreachable(std::string_view message);
inline constexpr std::string_view kPlacesFound = "Places found.";
/// Green although nothing ran (01 §11.4, §18.2).
inline constexpr std::string_view kSearchNotStartedCancelled = "Search not started (cancelled).";
/// Green: chooser cancelled or "Big search" answered No (01 §18.2).
inline constexpr std::string_view kSearchNotStarted = "Search not started.";
/// "Place lookup failed: {message}" (red, 01 §11.4).
[[nodiscard]] std::string place_lookup_failed(std::string_view message);
/// 0-request plan without notes (red, 01 §11.9).
inline constexpr std::string_view kNothingToSearch = "Nothing to search.";
/// Cancel before the fetch loop (red, 01 §11.9).
inline constexpr std::string_view kCancelled = "Cancelled.";

// ---------------------------------------------------------------------------
// PrepareAsync, ResolveAsync and the place chooser (01 §11.4, §11.5, §18.5 E5–E10)
// ---------------------------------------------------------------------------

inline constexpr std::string_view kEndBeforeStart = "The end date is before the start date.";
inline constexpr std::string_view kTypeStart = "Type where you start from (a city or airport).";
/// Return search, "To" empty and the outbound grid has fares (01 §11.4).
inline constexpr std::string_view kReturnNeedsPick =
    "“To” is empty (anywhere), so tell me where the return starts: click a flight in the Outbound tab (or "
    "right-click it → “Search return flights from this destination”), or type the destination in “To”.";
/// Return search, "To" empty and no outbound fares (01 §11.4).
inline constexpr std::string_view kReturnNeedsDestination =
    "For the return search, type your destination in “To” (or run an outbound search first and pick a flight).";
inline constexpr std::string_view kRoleStartingPoint = "starting point";
inline constexpr std::string_view kRoleDestination = "destination";
inline constexpr std::string_view kLabelOutbound = "Outbound";
inline constexpr std::string_view kLabelReturn = "Return";
/// E5: the geocoder could not be reached.
inline constexpr std::string_view kLookupOffline =
    "Could not reach the place-lookup service (offline?). Type a Wizz Air city or airport instead, e.g. “Ljubljana” or “LJU”.";
/// E6: "Could not find “{trimmed}”. Check the spelling, or type a nearby city or airport."
[[nodiscard]] std::string place_not_found(std::string_view trimmed_text);
/// E7: "“{UPPER}” is not a Wizz Air airport code, so it was looked up as a place name."
[[nodiscard]] std::string not_an_airport_code(std::string_view upper_text);
/// E8: busy detail "which place is “{t}”?" (stage "Waiting for your choice").
[[nodiscard]] std::string which_place(std::string_view trimmed_text);
/// E9: log line "Interpreted “{typed}” as “{corrected}”."
[[nodiscard]] std::string interpreted(std::string_view typed, std::string_view corrected);
/// E10: chooser title "Which {role}?".
[[nodiscard]] std::string chooser_title(std::string_view role);
/// E10: chooser intro, "Did you mean this place for “{typed}”?" (one candidate) or
/// "Did you mean one of these places for “{typed}”? Pick the {role}:"; with a hint
/// the hint, a line break ("\r\n", Environment.NewLine) and the intro.
[[nodiscard]] std::string chooser_intro(std::string_view typed, std::string_view role, std::size_t candidates,
                                        const std::optional<std::string>& hint);
inline constexpr std::string_view kUseThisPlace = "Use this place";

// ---------------------------------------------------------------------------
// RunLegAsync (01 §11.9)
// ---------------------------------------------------------------------------

inline constexpr std::string_view kBigSearchCaption = "Big search";
/// The >500 cap message (01 §11.9; 05 U-01's "L-07 text").
[[nodiscard]] std::string request_cap(std::size_t requests, std::int64_t minutes);
/// The >60 "Big search" question (01 §11.9), "\n" line breaks as in the C# literal.
[[nodiscard]] std::string big_search(std::size_t requests, std::string_view duration_text);
/// Grid summary at leg start: "Searching {routes} route(s) from {airports} airport(s)…".
[[nodiscard]] std::string searching_routes(std::int64_t routes, std::size_t airports);
/// Grid summary while streaming: "{fares} fares so far…".
[[nodiscard]] std::string fares_so_far(std::size_t fares);
/// Log line "{label}: cancelled." (cancel before the fetch loop).
[[nodiscard]] std::string leg_cancelled_log(std::string_view label);
/// Log line "{label} ERROR: {message}" (leg exception).
[[nodiscard]] std::string leg_error_log(std::string_view label, std::string_view message);
/// Log line "  {label}: {detail}" for a logged notice (01 §11.9, §12.2 UX-18).
[[nodiscard]] std::string notice_log(std::string_view label, std::string_view detail);

// ---------------------------------------------------------------------------
// Summary / StatusLine / ShowResolved pieces (01 §11.10)
// ---------------------------------------------------------------------------

inline constexpr std::string_view kSummaryNoFaresPrefix = "No fares: ";
inline constexpr std::string_view kSummaryCancelledBeforeFares = "Cancelled before any fares arrived.";
inline constexpr std::string_view kSummaryRoutesNotServed =
    "Wizz Air says it does not sell these routes right now (its route map may be out of date).";
inline constexpr std::string_view kSummaryNoFlights = "No flights on sale for these dates.";
inline constexpr std::string_view kSummaryCancelledEarly = " · cancelled early";
inline constexpr std::string_view kSummaryStoppedEarly = " · stopped early";
/// Prefix of the plan note `Summary` skips for a 0-fare result (ordinal, A1).
inline constexpr std::string_view kDatesBeforePrefix = "Dates before";
inline constexpr std::string_view kVerbDone = "Done";
inline constexpr std::string_view kVerbCancelled = "Cancelled";
inline constexpr std::string_view kVerbStoppedEarly = "Stopped early";
inline constexpr std::string_view kResolvedSameArea = "not searched (same area)";
inline constexpr std::string_view kResolvedNoneInRange = "none in range";
inline constexpr std::string_view kResolvedAnywhere = "To: anywhere.";

// ---------------------------------------------------------------------------
// Round trips (01 §9, §11.8, §18.5 F6–F8)
// ---------------------------------------------------------------------------

inline constexpr std::string_view kTripsPlaceholder =
    "Search outbound and return flights to see the cheapest combinations here.";
/// F6: "No combination with a stay of {min}–{max} nights. Change the nights or the dates."
[[nodiscard]] std::string no_combination(std::int64_t min_nights, std::int64_t max_nights);
inline constexpr std::string_view kSameHomeCheckbox = "Return to the airport I left from";
inline constexpr std::string_view kSameAwayCheckbox = "Fly back from the airport I landed at";
inline constexpr std::string_view kOpenOnWizzair = "Open on wizzair.com";
inline constexpr std::string_view kOpenOutboundOnly = "Open outbound only";
inline constexpr std::string_view kOpenReturnOnly = "Open return only";
inline constexpr std::string_view kOneBooking = "one booking";
inline constexpr std::string_view kTwoBookings = "two one-way bookings";
/// F7: "open jaw: land {out_to}, fly back from {ret_from}".
[[nodiscard]] std::string open_jaw(std::string_view out_to, std::string_view ret_from);
/// F7: "back to {ret_to} (left from {out_from})".
[[nodiscard]] std::string back_to(std::string_view ret_to, std::string_view out_from);

// ---------------------------------------------------------------------------
// FlightGrid (01 §8)
// ---------------------------------------------------------------------------

inline constexpr std::string_view kShowLabel = "Show:";
/// `_view` items in combo order (ViewMode index).
inline constexpr std::array<std::string_view, 3> kViewItems{"All fares", "Cheapest per day", "Cheapest per destination"};
inline constexpr std::string_view kNoteMergedDay = "* = this fare; other times may land at another airport of the same group";
inline constexpr std::string_view kNoteCheckOnSite = "price not shown by Wizz — check on wizzair.com";
/// "no exchange rate for {fare currency}" (01 §8.5 [WART]).
[[nodiscard]] std::string note_no_rate(std::string_view fare_currency);
/// "lands at {name}, outside your destination radius (cheapest airport of its group that day)".
[[nodiscard]] std::string note_outside_radius(std::string_view airport_name);
/// "total for {adults} adults".
[[nodiscard]] std::string note_total_for(int adults);
/// "Fare (local)" of a fare without a price (U+2014).
inline constexpr std::string_view kNoLocalFare = "—";
inline constexpr std::string_view kCopyDetails = "Copy details";
inline constexpr std::string_view kPlanReturnMenu = "Search return flights from this destination";
inline constexpr std::string_view kPlanReturnMenuBusy =
    "Search return flights from this destination (finish or cancel the current search first)";
inline constexpr std::string_view kCopyPerPersonSuffix = " per person";
inline constexpr std::string_view kCopyCheckOnSite = "price: check on site";
/// `CurrentRowText` without rows (01 §8.10).
inline constexpr std::string_view kNoCurrentRow = "(none)";
/// "Could not open the browser:\n{message}\n\n{url}" (Warning box, no owner, 01 §8.8).
[[nodiscard]] std::string browser_error(std::string_view message, std::string_view url);
inline constexpr std::string_view kClipboardBusy = "The clipboard is busy (another program is using it). Try again.";

// ---------------------------------------------------------------------------
// Log tab (01 §12.2)
// ---------------------------------------------------------------------------

inline constexpr std::string_view kStartedLog =
    "WizzairHelper started. Fares: wizzair.com (unofficial website API). Geocoding: Photon / Nominatim, data © "
    "OpenStreetMap contributors. Exchange rates: Frankfurter / currency-api.";
/// "Route map: {N} airports, API {url}." or "Route map: {N} airports (from the local cache)." (LIVE-9).
[[nodiscard]] std::string route_map_log(std::size_t real_airports, const std::optional<std::string>& api_url);
/// "ERROR loading route map: {message}".
[[nodiscard]] std::string map_error_log(std::string_view message);
/// "Return will start from {display} (picked in the outbound results), {from} – {to}." (dates "dd MMM").
[[nodiscard]] std::string return_from_pick_log(std::string_view airport_display, std::string_view from_text,
                                               std::string_view to_text);
/// "screenshot {name} failed: {message}" (autotest).
[[nodiscard]] std::string screenshot_failed(std::string_view name, std::string_view message);
inline constexpr std::string_view kSameAreaLogLine = "  not searched: start and destination are the same area";

// ---------------------------------------------------------------------------
// --autotest summary.json (01 §17.3)
// ---------------------------------------------------------------------------

inline constexpr std::string_view kNoResult = "no result";

} // namespace wizz::ui::strings
