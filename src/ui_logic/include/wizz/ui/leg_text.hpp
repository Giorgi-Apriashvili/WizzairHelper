#pragma once
// Texts and decisions of one leg search (01 §11.8-§11.10, §12.2): the request
// budget gate (U-01), the fetch Begin text, the >500 cap and >60 "Big search"
// texts, the grid summary `Summary`, the busy line `StatusLine`, the `_resolved`
// line `ShowResolved` (U-11), the Log lines of `LogPlan` / `LogResult`, the
// notice-logging filter and the Round trips summary.
//
// Formatting goes through the locale service (07 §1): dates "ddd dd MMM",
// money via CurrencyFormat (03 §6.4), `{km:0}` / `{radius:0}` with the 07 §2
// label algorithm, FormatDuration with D-17's ties-to-even. Plain functions,
// no state; safe to call from any thread.

#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/progress.hpp"
#include "wizz/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wizz::ui {

// ---------------------------------------------------------------------------
// Request budget (01 §11.9, 05 U-01)
// ---------------------------------------------------------------------------

/// Above this many requests the user must confirm ("Big search"), unless autotest.
inline constexpr std::size_t kConfirmAboveRequests = 60;
/// Above this many requests the search is refused (UX-13). Exactly 500 is allowed.
inline constexpr std::size_t kMaxRequests = 500;

/// What RunLegAsync does with a plan of `requests` requests (> 0; a plan with no
/// requests takes the "nothing to search" path before this gate).
enum class RequestBudget {
    proceed, ///< 1..60, or 61..500 in autotest
    confirm, ///< 61..500 interactively: ask the "Big search" question
    refuse,  ///< > 500: end red with the cap message (no question, also in autotest)
};
[[nodiscard]] RequestBudget request_budget(std::size_t requests, bool autotest) noexcept;

/// The estimate `N × 1.3 s` as .NET `TimeSpan.FromSeconds(N * 1.3)` builds it
/// (double seconds, truncated to 100 ns ticks).
[[nodiscard]] Duration request_estimate(std::size_t requests) noexcept;
/// `ceil(estimate.TotalMinutes)` of the cap message (ticks / ticks-per-minute as double).
[[nodiscard]] std::int64_t estimate_minutes_ceiling(Duration estimate) noexcept;

/// Fetch phase Begin text: "{label}: starting ({N} requests, about {FormatDuration(N×1.3 s)})…".
[[nodiscard]] std::string fetch_begin_text(std::string_view label, std::size_t requests);
/// The cap message with `ceil(N·1.3/60)` minutes (red busy line, Log, MessageBox).
[[nodiscard]] std::string request_cap_text(std::size_t requests);
/// The "Big search" question with FormatDuration(N×1.3 s).
[[nodiscard]] std::string big_search_text(std::size_t requests);
/// 0-request plan: the first plan note, else "Nothing to search." (red line and grid text, 01 §11.9).
[[nodiscard]] std::string nothing_to_search_text(const SearchPlan& plan);
/// Grid summary at leg start: "Searching {RoutesConsidered} route(s) from {origins} airport(s)…".
[[nodiscard]] std::string searching_text(const SearchPlan& plan);

// ---------------------------------------------------------------------------
// Summary, StatusLine and ShowResolved (01 §11.10)
// ---------------------------------------------------------------------------

/// `Money.ToString()` = CurrencyFormat.Format(amount, currency) ("29.99 EUR", "8,790 HUF").
[[nodiscard]] std::string money_text(const Money& money, const Locale& locale);
/// `Price(f)`: CurrencyFormat.Format(TotalConverted, TargetCurrency) when converted,
/// else the fare's own per-person price (`Money.ToString()`).
[[nodiscard]] std::string price_text(const FlightResult& flight, const Locale& locale);

/// `Summary(r)`, the grid summary label (UX-11), e.g.
/// "12 fares · cheapest 29.98 EUR: BTS → LTN on Thu 12 Nov".
[[nodiscard]] std::string summary(const LegSearchResult& result, const Locale& locale);

/// `ExtraWarnings(r)`: the warnings that are not plan notes (exact string match), in order.
[[nodiscard]] std::vector<std::string> extra_warnings(const LegSearchResult& result);

/// `StatusLine(r)`, the final busy line, e.g. "Outbound Done: 12 fares from 3 of 3 requests in 2 s."
/// plus " {AbortReason}" or " {n} note(s) in the Log tab." (n = extra warnings + plan notes).
[[nodiscard]] std::string status_line(const LegSearchResult& result);
/// Colour of the StatusLine: green when (no abort and not cancelled) or any fares.
[[nodiscard]] bool status_success(const LegSearchResult& result) noexcept;

/// `ShowResolved(request, plan)`, the `_resolved` line (LIVE2-6, U-11). The two
/// halves are separated by three spaces; "not searched (same area)" replaces both
/// airport lists for a same-area plan, "none in range" an empty one.
[[nodiscard]] std::string show_resolved(const LegSearchRequest& request, const SearchPlan& plan, const Locale& locale);

// ---------------------------------------------------------------------------
// Log tab (01 §12)
// ---------------------------------------------------------------------------

/// One Log text-box record: "[" + now "HH:mm:ss" (culture time separator) + "] " + line + CRLF.
[[nodiscard]] std::string log_entry(LocalDateTime now, std::string_view line, const Locale& locale);

/// `LogPlan(request, plan)` lines, leading spaces included:
/// "{Label}: {Origin} (+{r:0} km) → {Destination or anywhere}[ (+{r:0} km)], {from} … {to},
/// {Adults} adult(s), {Regular|WizzDiscountClub}, {Currency}", then for a same-area plan
/// "  not searched: start and destination are the same area" and "  note: {n}" per note;
/// otherwise "  origin airports: {IATA} ({km:0} km), …", "  destination airports: …" (when a
/// destination is set), "  {routes} route(s), {requests} request(s)" and "  note: {n}" per note.
[[nodiscard]] std::vector<std::string> log_plan_lines(const LegSearchRequest& request, const SearchPlan& plan,
                                                      const Locale& locale);

/// Maximum fare lines `LogResult` writes.
inline constexpr std::size_t kLoggedFares = 5;
/// `LogResult(r)` lines: "{Label}: {N} fares, {completed}/{planned} requests, {n} route(s) not served,
/// {FormatDuration}, FX {Source} {yyyy-MM-dd}[ (approximate)]", then "  ! {w}" per extra
/// warning, then up to 5 fare lines "    {ddd dd MMM} {HH:mm}  {FROM}→{TO}  {PricePerPerson}  = {Price(f)}".
/// Choice (01 open question 11): `{From}→{To}` are the IATA codes; `HH:mm` uses the
/// culture time separator like the C# interpolation ("current culture", C-17).
[[nodiscard]] std::vector<std::string> log_result_lines(const LegSearchResult& result, const Locale& locale);

/// Notice logging during a leg (01 §11.9, UX-18): a progress report is logged as
/// "  {Label}: {Detail}" when it carries a countdown (`waiting_until_utc`) or its
/// detail contains "API version" or "cached" (ordinal, case-sensitive), once per
/// distinct detail text per run. One instance per leg run; UI thread only.
class NoticeLog {
public:
    explicit NoticeLog(std::string label) : label_(std::move(label)) {}

    /// The Log line for `progress`, or nullopt when it is not logged (filtered or repeated).
    [[nodiscard]] std::optional<std::string> line_for(const SearchProgress& progress);

private:
    std::string label_;
    std::set<std::string> logged_;
};

// ---------------------------------------------------------------------------
// Round trips summary (01 §11.8)
// ---------------------------------------------------------------------------

/// "No combination with a stay of {min}–{max} nights. Change the nights or the dates." for no
/// trips; otherwise "{count} combinations · cheapest {Format(b.Total, currency)}:
/// {OUT_FROM}→{OUT_TO} {ddd dd MMM}, back {RET_FROM}→{RET_TO} {ddd dd MMM} ({nights} nights)"
/// for the first trip b (arrows without spaces, always "nights").
[[nodiscard]] std::string round_trip_summary(std::span<const RoundTrip> trips, int min_nights, int max_nights,
                                             std::string_view currency, const Locale& locale);

} // namespace wizz::ui
