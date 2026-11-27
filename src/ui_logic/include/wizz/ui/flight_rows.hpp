#pragma once
// The pure part of the FlightGrid (Outbound and Return tabs, 01 §8): column
// set, cell texts (§8.2, §8.5), row styling (§8.3), the display sort (§8.4),
// view modes and the streaming data flow (§8.6), the current row versus the
// explicit pick and RestoreSelection (§8.7, U-09), the context menu state
// (§8.9), CopySelected and the self-test accessors (§8.10).
//
// FlightGridModel holds an index array over immutable rows: a rebuild is one
// sort of that array, a streamed batch is a binary insertion per row, so a Qt
// QAbstractTableModel can expose it with one reset / one insertion
// notification per batch (01 §8.6 performance rules, R-03). A rebuild formats
// only the fares it did not show before (a row depends on nothing but its fare
// and the locale). Rows are identified by `Idx` (their position in the current
// view list `_shown`); the pick is a FlightResultPtr (object identity, WF-1).
//
// Times in cells are invariant "HH:mm" (07 §1); CopySelected uses the culture
// formats as 01 §8.9 says. UI thread only.

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

/// The "Show:" combo (index = value).
enum class ViewMode { all_fares = 0, cheapest_per_day = 1, cheapest_per_destination = 2 };
/// The enum name used by summary.json: "AllFares", "CheapestPerDay", "CheapestPerDestination".
[[nodiscard]] std::string_view view_mode_name(ViewMode mode) noexcept;
/// `ResultViews` for a mode (03 §8): a copy, CheapestPerDay or CheapestPerDestination.
[[nodiscard]] std::vector<FlightResultPtr> view_list(std::span<const FlightResultPtr> all, ViewMode mode);

/// `NoPrice`: the PriceSort of a fare without a converted total.
inline constexpr Decimal kNoPrice{99'999'999};

/// Data columns in order (01 §8.2). `idx` and `price_sort` are hidden.
enum class FlightColumn {
    idx,
    price_sort,
    date,
    departs,
    from,
    km_from_start,
    to,
    km_from_dest,
    total,
    per_person,
    fare_local,
    all_departures,
    notes,
};
inline constexpr std::size_t kFlightColumnCount = 13;
inline constexpr std::array<FlightColumn, kFlightColumnCount> kFlightColumns{
    FlightColumn::idx,           FlightColumn::price_sort, FlightColumn::date,       FlightColumn::departs,
    FlightColumn::from,          FlightColumn::km_from_start, FlightColumn::to,     FlightColumn::km_from_dest,
    FlightColumn::total,         FlightColumn::per_person, FlightColumn::fare_local, FlightColumn::all_departures,
    FlightColumn::notes};

/// Data column name ("Idx", "PriceSort", "Date", "Departs", "From", "km from start", "To",
/// "km from dest.", "Total", "Per person", "Fare (local)", "All departures", "Notes").
[[nodiscard]] std::string_view flight_column_name(FlightColumn column) noexcept;
/// Header text: the name, except "Total ({CUR})" and "Per person ({CUR})" (UpdateHeaders).
[[nodiscard]] std::string flight_column_header(FlightColumn column, std::string_view currency);
/// Visibility: Idx and PriceSort never; "km from dest." only when not anywhere.
[[nodiscard]] bool flight_column_visible(FlightColumn column, bool anywhere) noexcept;

// ---------------------------------------------------------------------------
// Cell texts and values (01 §8.2, §8.5)
// ---------------------------------------------------------------------------

/// `DeparturesText`: every departure as invariant "HH:mm", joined ", "; on a merged day
/// with several departures the one equal to CheapestDeparture (full date-time) gets "*".
[[nodiscard]] std::string departures_text(const FlightResult& flight);
/// `Notes`, joined "; ": merged-day star note; check-on-site note or "no exchange rate
/// for {fare currency}"; "lands at {name}, outside your destination radius (…)";
/// "total for {n} adults" (available fares with more than one adult).
[[nodiscard]] std::string notes_text(const FlightResult& flight);
/// "{IATA}  {Name}" (two spaces).
[[nodiscard]] std::string airport_cell_text(const Airport& airport);
/// CheapestDeparture as invariant "HH:mm".
[[nodiscard]] std::string departs_text(const FlightResult& flight);
/// "Fare (local)": `Money.ToString()` for an available fare ("29.99 EUR", "8,790 HUF"), else "—".
[[nodiscard]] std::string fare_local_text(const FlightResult& flight, const Locale& locale);
/// The km columns' value: Math.Round, ties to even (07 §2 grid distances).
[[nodiscard]] double grid_km(double km) noexcept;
/// `PriceSort` = TotalConverted ?? NoPrice.
[[nodiscard]] Decimal price_sort_value(const FlightResult& flight);
/// "Copy details" text (01 §8.9), culture formats, CRLF before the booking URL:
/// "{ddd dd MMM yyyy} {HH:mm}  {From.Display} → {To.Display}  {PricePerPerson} per person
/// (total {Format(total)})" or "… price: check on site", then the URL.
[[nodiscard]] std::string copy_details_text(const FlightResult& flight, const Locale& locale);
/// The third context-menu item's text (UI-9).
[[nodiscard]] std::string_view plan_return_menu_text(bool busy) noexcept;
/// One cell of "Copy as table" (DECISIONS D-76): the text with tabs and line breaks turned into spaces,
/// so a spreadsheet keeps it in one cell.
[[nodiscard]] std::string table_cell_text(std::string_view text);
/// The "Copy details" blocks of several rows (D-76), a blank line between two: "{a}\r\n\r\n{b}".
[[nodiscard]] std::string join_copied_rows(std::span<const std::string> blocks);

/// One grid row, computed once when it is added (`AddRow`).
struct FlightRow {
    std::size_t idx = 0;              ///< index into the view list `_shown`
    FlightResultPtr fare;
    Decimal price_sort;
    Date date;
    std::string date_text;            ///< "ddd dd MMM yyyy"
    std::string departs;              ///< "HH:mm"
    std::string from;                 ///< "{IATA}  {Name}"
    double km_from_start = 0;         ///< rounded half to even
    std::string to;
    std::optional<double> km_from_dest;
    std::optional<Decimal> total;
    std::optional<Decimal> per_person;
    std::string fare_local;
    std::string all_departures;
    std::string notes;
};
[[nodiscard]] FlightRow make_flight_row(std::size_t idx, FlightResultPtr fare, const Locale& locale);

/// Displayed cell text: dates "ddd dd MMM yyyy", km "0", money in the currency's number
/// format ("#,##0.00" / "#,##0", culture separators), null cells "".
[[nodiscard]] std::string flight_cell_text(const FlightRow& row, FlightColumn column, std::string_view currency,
                                           const Locale& locale);
/// Ascending comparison of one column (01 §8.4): values, nulls first, strings at secondary strength.
[[nodiscard]] int compare_flight_rows(const FlightRow& a, const FlightRow& b, FlightColumn column, const Locale& locale);

/// Row colouring (01 §8.3).
enum class RowTone {
    normal,
    unavailable, ///< PriceSort >= NoPrice: whole row GrayText
    cheapest,    ///< PriceSort == the view's cheapest: RGB(220,245,220), selection RGB(40,140,70)
};
[[nodiscard]] RowTone flight_row_tone(const FlightRow& row, const Decimal& cheapest);

/// `VisibleRowsText` line: "{yyyy-MM-dd} {Departs} {From} -> {To} total={Total} local={Fare (local)}",
/// Total = the raw decimal with its own scale and the culture separator ("" when null).
[[nodiscard]] std::string visible_row_text(const FlightRow& row, const Locale& locale);
/// `CurrentRowText`: "{yyyy-MM-dd} {FROM}->{TO} {TotalConverted}" or "(none)" (a null total
/// leaves a trailing space).
[[nodiscard]] std::string current_row_text(const FlightResult* flight, const Locale& locale);

/// What the view must do with its scroll position after a model change.
enum class ScrollRequest {
    none,    ///< keep the first displayed row index (streaming in All fares)
    top,     ///< full reload: row 0 current and first displayed
    current, ///< scroll the current row into view (pick restored)
    /// Full reload that restored a pick: first scroll to the top (row 0 first
    /// displayed, 01 §8.6), then scroll the current row into view (§8.7).
    top_then_current,
};

enum class MouseButton { left, right, middle };

/// The context menu when it opens (01 §8.9).
struct FlightMenuState {
    bool open_enabled = false;        ///< "Open on wizzair.com"
    bool copy_enabled = false;        ///< "Copy details" and "Copy as table" (D-76)
    bool plan_return_visible = false; ///< outbound grid only
    bool plan_return_enabled = false;
    std::string plan_return_text;

    friend bool operator==(const FlightMenuState&, const FlightMenuState&) = default;
};

class FlightGridModel {
public:
    /// Default display sort keys (01 §8.4): PriceSort, Date, Departs ascending, then Idx.
    static constexpr std::array<FlightColumn, 3> kDefaultSort{FlightColumn::price_sort, FlightColumn::date,
                                                              FlightColumn::departs};

    /// `offer_plan_return`: the Outbound grid offers "Search return flights from this destination".
    explicit FlightGridModel(std::shared_ptr<const Locale> locale, bool offer_plan_return = true);

    // --- Data flow (01 §8.6) ---------------------------------------------------------

    /// Start of a leg: no fares, currency upper-cased, the pick reset; the view switches to
    /// CheapestPerDestination for anywhere / AllFares otherwise unless the user ever chose a
    /// view here (LIVE-5); then a rebuild and the summary.
    void clear(std::string_view currency, bool anywhere, std::string_view summary = {});
    /// A streamed batch (the 150 ms flush). All fares: rows go to their sorted positions, the
    /// current row stays on its fare, the scroll position is kept, and the current row returns
    /// to an explicit pick it left. Grouped views: a full rebuild (row 0 + top, or the pick).
    void append(std::span<const FlightResultPtr> batch);
    /// The final or re-priced result: replaces all fares; keeps anywhere and the pick.
    void set_results(std::span<const FlightResultPtr> results, std::string_view currency, std::string_view summary);
    void set_summary(std::string_view summary);

    // --- User actions -----------------------------------------------------------------

    /// A user change of the "Show:" combo: marks the view as chosen for the session
    /// (even back to All fares) and rebuilds. Choosing the current mode changes nothing.
    void choose_view(ViewMode mode);
    /// A header click (visible columns): sort by that column, ascending first, then toggled.
    /// The sort persists across rebuilds and searches. The current row stays on its fare.
    void sort_by_column(FlightColumn column);
    /// Mouse down on a data cell (`display_row` < row count; header clicks do not count):
    /// the pick becomes that row's fare; left and right buttons also make it current.
    void mouse_down(std::size_t display_row, MouseButton button);
    /// The grid's current row moved (keyboard navigation); the pick is not changed.
    void set_current_row(std::size_t display_row);
    /// Key-up of Up, Down, PageUp, PageDown, Home or End: pick = Selected ?? pick.
    void navigation_key_up();
    /// The user changed the grid's selection (DECISIONS D-76: Ctrl+click, Shift+click, Shift+arrows,
    /// Ctrl+A, a plain click or arrow key): `display_rows` are now the selected rows and
    /// `current_display_row` the view's current row. Exactly the current row (a plain click or arrow
    /// key) keeps following the current row, as the single selection of 01 §8.1 did. Anything else
    /// is the user's own selection: the grid keeps it as fares — by (From IATA, To IATA, Date), as
    /// RestoreSelection matches (01 §8.7) — across sorting, streaming, view switches and SetResults,
    /// until Clear or the next change.
    void select_rows(std::span<const std::size_t> display_rows, std::optional<std::size_t> current_display_row);

    /// `RestoreSelection(target, scrollTopIfMissing)` (01 §8.7): the first row in display
    /// order that is `target` itself or has its (From IATA, To IATA, Date) — ordinal —
    /// becomes current; else in CheapestPerDestination the first row of the same
    /// ToAirport.GroupKey; else, when `scroll_top_if_missing`, row 0 + scroll to top.
    void restore_selection(const FlightResultPtr& target, bool scroll_top_if_missing);

    // --- State ------------------------------------------------------------------------

    [[nodiscard]] ViewMode mode() const noexcept { return mode_; }
    [[nodiscard]] bool user_chose_view() const noexcept { return user_chose_view_; }
    [[nodiscard]] const std::string& currency() const noexcept { return currency_; }
    [[nodiscard]] bool anywhere() const noexcept { return anywhere_; }
    [[nodiscard]] const std::string& summary() const noexcept { return summary_; }
    /// Every fare the grid holds (`AllResults`; the tab title counts these).
    [[nodiscard]] std::span<const FlightResultPtr> all_results() const noexcept { return all_; }
    /// The current view list `_shown` (Idx order, not display order).
    [[nodiscard]] std::span<const FlightResultPtr> shown_results() const noexcept { return shown_; }
    [[nodiscard]] std::size_t row_count() const noexcept { return order_.size(); }
    /// Row at display position `display_row`.
    [[nodiscard]] const FlightRow& row(std::size_t display_row) const;
    /// Display position of the current row; nullopt without rows.
    [[nodiscard]] std::optional<std::size_t> current_row() const;
    /// `Selected`: the current row's fare (non-null whenever rows exist).
    [[nodiscard]] FlightResultPtr selected() const;
    /// `ExplicitSelection`: the fare the user picked since the last Clear, or null.
    [[nodiscard]] const FlightResultPtr& explicit_selection() const noexcept { return picked_; }
    /// The selected rows in display order (D-76): the rows of the user's selected fares that the view
    /// shows — in Cheapest per destination also the row of a selected fare's destination when that
    /// fare is not shown, as RestoreSelection falls back — while the fares themselves stay selected
    /// for a view that shows them again; without a selection of the user's (and after Clear), or when
    /// none of it is shown, the current row, as the single selection of 01 §8.1 did. Empty without rows.
    [[nodiscard]] std::vector<std::size_t> selected_rows() const;
    /// Whether the user's own selection is in force (D-76).
    [[nodiscard]] bool selection_chosen() const noexcept { return selection_chosen_; }
    /// `_cheapest`: the lowest TotalConverted of the current view (NoPrice if none).
    [[nodiscard]] const Decimal& cheapest() const noexcept { return cheapest_; }
    [[nodiscard]] RowTone tone(std::size_t display_row) const;
    [[nodiscard]] std::string cell_text(std::size_t display_row, FlightColumn column) const;
    [[nodiscard]] std::string header(FlightColumn column) const { return flight_column_header(column, currency_); }
    [[nodiscard]] bool column_visible(FlightColumn column) const noexcept
    {
        return flight_column_visible(column, anywhere_);
    }
    /// The user's header sort; nullopt = the default sort.
    [[nodiscard]] const std::optional<ColumnSort<FlightColumn>>& user_sort() const noexcept { return user_sort_; }
    /// The latest scroll request since the previous take.
    [[nodiscard]] ScrollRequest take_scroll_request() noexcept;
    [[nodiscard]] FlightMenuState context_menu(bool busy) const;

    /// The work the grid has done since it was made, counted so the R-03 tests can bound
    /// it independently of how fast or busy the machine is.
    struct Work {
        std::size_t rows_built = 0;   ///< rows formatted (`AddRow`)
        std::size_t comparisons = 0;  ///< row comparisons of the display sort, insertions included
        std::size_t full_sorts = 0;   ///< sorts of the whole display order
    };
    [[nodiscard]] const Work& work() const noexcept { return work_; }

    // --- Self-test accessors (01 §8.10) -----------------------------------------------

    /// `VisibleRowsText(max)`: the first `max` rows in display order.
    [[nodiscard]] std::vector<std::string> visible_rows_text(std::size_t max) const;
    /// `CurrentRowText`.
    [[nodiscard]] std::string current_row_text() const;
    /// "Copy details" (01 §8.9, D-76): the text of every selected row in display order, a blank line
    /// between two; nullopt without rows. One selected row gives 01 §8.9's text unchanged.
    [[nodiscard]] std::optional<std::string> copy_selected_text() const;
    /// "Copy as table" (D-76): a header line of the visible columns' headers, then one line per
    /// selected row with its cells as the grid shows them, tab-separated, CRLF between lines;
    /// nullopt without rows.
    [[nodiscard]] std::optional<std::string> copy_table_text() const;

private:
    void rebuild(bool keep_top_if_not_picked);
    void sort_order();
    [[nodiscard]] bool row_less(std::size_t a, std::size_t b) const;
    [[nodiscard]] std::optional<std::size_t> position_of(std::size_t idx) const noexcept;
    void make_current(std::size_t display_row, ScrollRequest scroll);
    void set_selection(std::vector<FlightResultPtr> fares);

    /// A fare as a selection matches it: (From IATA, To IATA, Date), ordinal (01 §8.7).
    struct FareKey {
        std::string from;
        std::string to;
        Date date;
        friend auto operator<=>(const FareKey&, const FareKey&) = default;
    };
    [[nodiscard]] static FareKey key_of(const FlightResult& flight);

    std::shared_ptr<const Locale> locale_;
    bool offer_plan_return_ = true;
    std::vector<FlightResultPtr> all_;
    std::vector<FlightResultPtr> shown_;
    std::vector<FlightRow> rows_;        ///< by Idx
    std::vector<std::size_t> order_;     ///< display order → Idx
    ViewMode mode_ = ViewMode::all_fares;
    bool user_chose_view_ = false;
    std::string currency_{"EUR"};
    bool anywhere_ = false;
    std::string summary_;
    FlightResultPtr picked_;
    Decimal cheapest_ = kNoPrice;
    std::optional<ColumnSort<FlightColumn>> user_sort_;
    std::optional<std::size_t> current_;  ///< Idx of the current row
    ScrollRequest scroll_ = ScrollRequest::none;
    bool selection_chosen_ = false;                ///< the user selected rows since the last Clear (D-76)
    std::vector<FlightResultPtr> selected_fares_;  ///< their fares (also ones the view does not show)
    std::set<FareKey> selected_keys_;              ///< the same fares' keys
    mutable Work work_;  ///< counted in const comparisons too
};

} // namespace wizz::ui
