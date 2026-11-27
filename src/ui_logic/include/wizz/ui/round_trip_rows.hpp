#pragma once
// The pure part of the RoundTripGrid (Round trips tab, 01 §9) and of
// MainForm.RecomputeTrips (01 §11.8): columns and cell texts (`Leg()`,
// `BookingNote`), the default sort Total ASC, Out date ASC (ties keep the
// combiner order), header sorting with the §8.4 compare rules, SetTrips
// (selection back to the top on every recompute), the open actions (one
// combined booking or two one-way bookings) and the recompute itself
// (MatchLegs → Combine(limit 500) → summary). UI thread only.

#include "wizz/ui/flight_rows.hpp"
#include "wizz/ui/grid_sort.hpp"

#include "wizz/core/decimal.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wizz::ui {

/// Data columns in order (01 §9.2); `idx` is hidden.
enum class TripColumn {
    idx,
    total,
    nights,
    out_date,
    outbound,
    out_price,
    return_date,
    return_leg,
    return_price,
    booking_notes,
};
inline constexpr std::size_t kTripColumnCount = 10;
inline constexpr std::array<TripColumn, kTripColumnCount> kTripColumns{
    TripColumn::idx,       TripColumn::total,       TripColumn::nights,     TripColumn::out_date,
    TripColumn::outbound,  TripColumn::out_price,   TripColumn::return_date, TripColumn::return_leg,
    TripColumn::return_price, TripColumn::booking_notes};

/// "Idx", "Total", "Nights", "Out date", "Outbound", "Out price", "Return date", "Return",
/// "Return price", "Booking / notes".
[[nodiscard]] std::string_view trip_column_name(TripColumn column) noexcept;
/// The name, except "Total ({CUR})".
[[nodiscard]] std::string trip_column_header(TripColumn column, std::string_view currency);

/// `Leg(f)`: "{HH:mm invariant}  {FROM} → {TO}  ({From.Name} → {To.Name})",
/// e.g. "20:40  KSC → LTN  (Kosice → London Luton)".
[[nodiscard]] std::string leg_text(const FlightResult& flight);
/// `BookingNote(t)`, parts joined "; ": "one booking" / "two one-way bookings", then
/// "open jaw: land {out.To}, fly back from {ret.From}" unless same away airport, then
/// "back to {ret.To} (left from {out.From})" unless same home airport.
[[nodiscard]] std::string booking_note(const RoundTrip& trip);
/// `OpenSelected` (01 §9.4): the combined booking URL when set, else the outbound URL
/// then the return URL (two browser tabs).
[[nodiscard]] std::vector<std::string> booking_urls(const RoundTrip& trip);

/// One trip row.
struct TripRow {
    std::size_t idx = 0;  ///< index into the trip list
    std::optional<Decimal> total;
    int nights = 0;
    Date out_date;
    std::string outbound;
    std::optional<Decimal> out_price;
    Date return_date;
    std::string return_leg;
    std::optional<Decimal> return_price;
    std::string booking_notes;
};
[[nodiscard]] TripRow make_trip_row(std::size_t idx, const RoundTrip& trip);
/// Displayed cell text: dates "ddd dd MMM", nights "0", money in the currency's number format.
[[nodiscard]] std::string trip_cell_text(const TripRow& row, TripColumn column, std::string_view currency,
                                         const Locale& locale);
/// Ascending comparison of one column (01 §8.4 rules, null first).
[[nodiscard]] int compare_trip_rows(const TripRow& a, const TripRow& b, TripColumn column, const Locale& locale);

/// "Copy details" of a round trip (DECISIONS D-76): the outbound's and the return's
/// copy_details_text (01 §8.9: each with its booking URL), then round_trip_copy_line with the
/// trip's total in `currency` (none: "price: check on site"), then the combined booking URL
/// when the trip has one (what "Open on wizzair.com" opens); CRLF between lines.
[[nodiscard]] std::string round_trip_copy_text(const RoundTrip& trip, std::string_view currency, const Locale& locale);

class RoundTripGridModel {
public:
    /// Default display sort (01 §9.2): Total, Out date ascending, then Idx (combiner order).
    static constexpr std::array<TripColumn, 2> kDefaultSort{TripColumn::total, TripColumn::out_date};

    explicit RoundTripGridModel(std::shared_ptr<const Locale> locale);

    /// `SetTrips` (01 §9.3): replace the rows, currency upper-cased, row 0 current (at the
    /// Total column) and scrolled to the top when there are rows; the summary label.
    void set_trips(std::vector<RoundTrip> trips, std::string_view currency, std::string_view summary);

    /// The two filter checkboxes (not persisted). Return true when the value changed
    /// (FiltersChanged → MainForm.RecomputeTrips).
    bool set_require_same_home(bool value) noexcept;
    bool set_require_same_away(bool value) noexcept;
    [[nodiscard]] bool require_same_home() const noexcept { return require_same_home_; }
    [[nodiscard]] bool require_same_away() const noexcept { return require_same_away_; }

    /// Header click: that column ascending first, then toggled; persists across SetTrips.
    void sort_by_column(TripColumn column);
    /// Mouse down on a data cell: left and right buttons make the row current.
    void mouse_down(std::size_t display_row, MouseButton button);
    /// Keyboard navigation moved the current row.
    void set_current_row(std::size_t display_row);
    /// The user changed the grid's selection (D-76): `display_rows` are now the selected rows and
    /// `current_display_row` the view's current row. Exactly the current row keeps following the
    /// current row; anything else is kept as trips across sorting until the next SetTrips (which
    /// resets to row 0, 01 §9.3).
    void select_rows(std::span<const std::size_t> display_rows, std::optional<std::size_t> current_display_row);

    [[nodiscard]] std::size_t count() const noexcept { return trips_.size(); }
    [[nodiscard]] std::span<const RoundTrip> trips() const noexcept { return trips_; }
    [[nodiscard]] const std::string& summary_text() const noexcept { return summary_; }
    [[nodiscard]] const std::string& currency() const noexcept { return currency_; }
    [[nodiscard]] std::size_t row_count() const noexcept { return order_.size(); }
    [[nodiscard]] const TripRow& row(std::size_t display_row) const;
    [[nodiscard]] std::optional<std::size_t> current_row() const;
    /// The current trip, or nullptr.
    [[nodiscard]] const RoundTrip* selected() const noexcept;
    /// The selected rows in display order (D-76): the user's selected trips; before the user
    /// selected (and after SetTrips), or when they selected none, the current row. Empty without rows.
    [[nodiscard]] std::vector<std::size_t> selected_rows() const;
    /// "Copy details" (D-76): round_trip_copy_text of every selected row in display order, a blank
    /// line between two; nullopt without rows.
    [[nodiscard]] std::optional<std::string> copy_selected_text() const;
    /// "Copy as table" (D-76): the visible columns' headers, then one line per selected row with its
    /// cells as shown, tab-separated, CRLF between lines; nullopt without rows.
    [[nodiscard]] std::optional<std::string> copy_table_text() const;
    [[nodiscard]] std::string cell_text(std::size_t display_row, TripColumn column) const;
    [[nodiscard]] std::string header(TripColumn column) const { return trip_column_header(column, currency_); }
    [[nodiscard]] const std::optional<ColumnSort<TripColumn>>& user_sort() const noexcept { return user_sort_; }
    [[nodiscard]] ScrollRequest take_scroll_request() noexcept;

    /// URLs to open for double-click / Enter / "Open on wizzair.com"; empty without a current trip.
    [[nodiscard]] std::vector<std::string> open_selected_urls() const;
    /// "Open outbound only"; nullopt without a current trip.
    [[nodiscard]] std::optional<std::string> outbound_url() const;
    /// "Open return only"; nullopt without a current trip.
    [[nodiscard]] std::optional<std::string> return_url() const;

private:
    void sort_order();
    [[nodiscard]] bool row_less(std::size_t a, std::size_t b) const;

    std::shared_ptr<const Locale> locale_;
    std::vector<RoundTrip> trips_;
    std::vector<TripRow> rows_;
    std::vector<std::size_t> order_;
    std::string currency_{"EUR"};
    std::string summary_;
    bool require_same_home_ = false;
    bool require_same_away_ = false;
    std::optional<ColumnSort<TripColumn>> user_sort_;
    std::optional<std::size_t> current_;
    ScrollRequest scroll_ = ScrollRequest::none;
    bool selection_chosen_ = false;      ///< the user selected rows since the last SetTrips (D-76)
    std::set<std::size_t> selected_;     ///< their trips' indexes (trips_ changes only in SetTrips)
};

/// What RecomputeTrips hands to SetTrips.
struct TripsView {
    std::vector<RoundTrip> trips;
    std::string currency;
    std::string summary;
};

/// `RecomputeTrips` (01 §11.8): currency = outbound's Request.Currency ?? the selected one;
/// without both legs the placeholder; a MatchLegs problem as the summary; otherwise
/// Combine(match, min, max, limit 500, same home, same away) and `round_trip_summary`.
[[nodiscard]] TripsView recompute_trips(const LegSearchResult* outbound, const LegSearchResult* inbound, int min_nights,
                                        int max_nights, bool require_same_home, bool require_same_away,
                                        std::string_view selected_currency, const Locale& locale);

} // namespace wizz::ui
