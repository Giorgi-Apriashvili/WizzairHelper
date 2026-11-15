#include "wizz/ui/flight_rows.hpp"

#include "wizz/ui/leg_text.hpp"
#include "wizz/ui/strings.hpp"

#include "wizz/core/currency_format.hpp"
#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/format.hpp"
#include "wizz/core/result_views.hpp"
#include "wizz/core/text.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace wizz::ui {

namespace {

[[nodiscard]] std::string join(const std::vector<std::string>& parts, std::string_view separator)
{
    std::string text;
    for (const std::string& part : parts) {
        if (!text.empty()) {
            text += separator;
        }
        text += part;
    }
    return text;
}

[[nodiscard]] std::string money_cell(const std::optional<Decimal>& amount, std::string_view currency,
                                     const Locale& locale)
{
    return amount ? amount->format(currency_format::number_format(currency), locale.number_symbols()) : std::string{};
}

[[nodiscard]] std::string km_cell(const std::optional<double>& km, const Locale& locale)
{
    return km ? format_label(*km, 0, locale.number_symbols()) : std::string{};
}

[[nodiscard]] std::string_view iata_of(const AirportPtr& airport) noexcept
{
    return airport ? std::string_view{airport->iata} : std::string_view{};
}

} // namespace

std::string_view view_mode_name(ViewMode mode) noexcept
{
    switch (mode) {
    case ViewMode::cheapest_per_day:
        return "CheapestPerDay";
    case ViewMode::cheapest_per_destination:
        return "CheapestPerDestination";
    case ViewMode::all_fares:
        break;
    }
    return "AllFares";
}

std::vector<FlightResultPtr> view_list(std::span<const FlightResultPtr> all, ViewMode mode)
{
    switch (mode) {
    case ViewMode::cheapest_per_day:
        return result_views::cheapest_per_day(all);
    case ViewMode::cheapest_per_destination:
        return result_views::cheapest_per_destination(all);
    case ViewMode::all_fares:
        break;
    }
    return {all.begin(), all.end()};
}

std::string_view flight_column_name(FlightColumn column) noexcept
{
    switch (column) {
    case FlightColumn::idx:
        return "Idx";
    case FlightColumn::price_sort:
        return "PriceSort";
    case FlightColumn::date:
        return "Date";
    case FlightColumn::departs:
        return "Departs";
    case FlightColumn::from:
        return "From";
    case FlightColumn::km_from_start:
        return "km from start";
    case FlightColumn::to:
        return "To";
    case FlightColumn::km_from_dest:
        return "km from dest.";
    case FlightColumn::total:
        return "Total";
    case FlightColumn::per_person:
        return "Per person";
    case FlightColumn::fare_local:
        return "Fare (local)";
    case FlightColumn::all_departures:
        return "All departures";
    case FlightColumn::notes:
        break;
    }
    return "Notes";
}

std::string flight_column_header(FlightColumn column, std::string_view currency)
{
    if (column == FlightColumn::total || column == FlightColumn::per_person) {
        return std::format("{} ({})", flight_column_name(column), currency);
    }
    return std::string{flight_column_name(column)};
}

bool flight_column_visible(FlightColumn column, bool anywhere) noexcept
{
    switch (column) {
    case FlightColumn::idx:
    case FlightColumn::price_sort:
        return false;
    case FlightColumn::km_from_dest:
        return !anywhere;
    default:
        return true;
    }
}

std::string departures_text(const FlightResult& flight)
{
    const DayFare& fare = flight.fare;
    const bool mark = fare.has_mac_flight && fare.departures.size() > 1;
    std::string text;
    for (const LocalDateTime departure : fare.departures) {
        if (!text.empty()) {
            text += ", ";
        }
        text += format_hhmm(departure);
        if (mark && departure == fare.cheapest_departure) {
            text += '*';
        }
    }
    return text;
}

std::string notes_text(const FlightResult& flight)
{
    std::vector<std::string> parts;
    if (flight.fare.has_mac_flight && flight.fare.departures.size() > 1) {
        parts.emplace_back(strings::kNoteMergedDay);
    }
    if (!flight.is_available()) {
        parts.emplace_back(strings::kNoteCheckOnSite);
    } else if (!flight.total_converted) {
        parts.push_back(strings::note_no_rate(flight.fare.price_per_person.currency));
    }
    if (flight.outside_destination_radius) {
        parts.push_back(strings::note_outside_radius(flight.to_airport ? flight.to_airport->name : flight.fare.to));
    }
    if (flight.adults > 1 && flight.is_available()) {
        parts.push_back(strings::note_total_for(flight.adults));
    }
    return join(parts, "; ");
}

std::string airport_cell_text(const Airport& airport)
{
    return airport.iata + "  " + airport.name;
}

std::string departs_text(const FlightResult& flight)
{
    return format_hhmm(flight.fare.cheapest_departure);
}

std::string fare_local_text(const FlightResult& flight, const Locale& locale)
{
    return flight.is_available() ? money_text(flight.fare.price_per_person, locale) : std::string{strings::kNoLocalFare};
}

double grid_km(double km) noexcept
{
    return round_half_even(km);
}

Decimal price_sort_value(const FlightResult& flight)
{
    return flight.total_converted ? *flight.total_converted : kNoPrice;
}

std::string copy_details_text(const FlightResult& flight, const Locale& locale)
{
    const std::string from = flight.from_airport ? flight.from_airport->display() : flight.fare.from;
    const std::string to = flight.to_airport ? flight.to_airport->display() : flight.fare.to;
    std::string text = std::format("{} {}  {} → {}  ", locale.format_date(flight.date(), "ddd dd MMM yyyy"),
                                   locale.format_time(flight.fare.cheapest_departure, "HH:mm"), from, to);
    if (flight.is_available()) {
        text += money_text(flight.fare.price_per_person, locale);
        text += strings::kCopyPerPersonSuffix;
    } else {
        text += strings::kCopyCheckOnSite;
    }
    if (flight.total_converted) {
        text += std::format(" (total {})", currency_format::format(*flight.total_converted, flight.target_currency,
                                                                    locale.number_symbols()));
    }
    text += "\r\n";
    text += flight.booking_url;
    return text;
}

std::string_view plan_return_menu_text(bool busy) noexcept
{
    return busy ? strings::kPlanReturnMenuBusy : strings::kPlanReturnMenu;
}

FlightRow make_flight_row(std::size_t idx, FlightResultPtr fare, const Locale& locale)
{
    if (!fare) {
        throw std::invalid_argument("A grid row needs a fare.");
    }
    FlightRow row;
    row.idx = idx;
    row.price_sort = price_sort_value(*fare);
    row.date = fare->date();
    row.date_text = locale.format_date(row.date, "ddd dd MMM yyyy");
    row.departs = departs_text(*fare);
    row.from = fare->from_airport ? airport_cell_text(*fare->from_airport) : fare->fare.from;
    row.km_from_start = grid_km(fare->from_distance_km);
    row.to = fare->to_airport ? airport_cell_text(*fare->to_airport) : fare->fare.to;
    if (fare->to_distance_km) {
        row.km_from_dest = grid_km(*fare->to_distance_km);
    }
    row.total = fare->total_converted;
    row.per_person = fare->price_per_person_converted;
    row.fare_local = fare_local_text(*fare, locale);
    row.all_departures = departures_text(*fare);
    row.notes = notes_text(*fare);
    row.fare = std::move(fare);
    return row;
}

std::string flight_cell_text(const FlightRow& row, FlightColumn column, std::string_view currency, const Locale& locale)
{
    switch (column) {
    case FlightColumn::idx:
        return std::to_string(row.idx);
    case FlightColumn::price_sort:
        return row.price_sort.to_string(locale.number_symbols());
    case FlightColumn::date:
        return row.date_text;
    case FlightColumn::departs:
        return row.departs;
    case FlightColumn::from:
        return row.from;
    case FlightColumn::km_from_start:
        return km_cell(row.km_from_start, locale);
    case FlightColumn::to:
        return row.to;
    case FlightColumn::km_from_dest:
        return km_cell(row.km_from_dest, locale);
    case FlightColumn::total:
        return money_cell(row.total, currency, locale);
    case FlightColumn::per_person:
        return money_cell(row.per_person, currency, locale);
    case FlightColumn::fare_local:
        return row.fare_local;
    case FlightColumn::all_departures:
        return row.all_departures;
    case FlightColumn::notes:
        break;
    }
    return row.notes;
}

int compare_flight_rows(const FlightRow& a, const FlightRow& b, FlightColumn column, const Locale& locale)
{
    switch (column) {
    case FlightColumn::idx:
        return compare_values(a.idx, b.idx);
    case FlightColumn::price_sort:
        return compare_values(a.price_sort, b.price_sort);
    case FlightColumn::date:
        return compare_values(a.date, b.date);
    case FlightColumn::departs:
        return compare_text(a.departs, b.departs, locale);
    case FlightColumn::from:
        return compare_text(a.from, b.from, locale);
    case FlightColumn::km_from_start:
        return compare_values(a.km_from_start, b.km_from_start);
    case FlightColumn::to:
        return compare_text(a.to, b.to, locale);
    case FlightColumn::km_from_dest:
        return compare_nullable(a.km_from_dest, b.km_from_dest);
    case FlightColumn::total:
        return compare_nullable(a.total, b.total);
    case FlightColumn::per_person:
        return compare_nullable(a.per_person, b.per_person);
    case FlightColumn::fare_local:
        return compare_text(a.fare_local, b.fare_local, locale);
    case FlightColumn::all_departures:
        return compare_text(a.all_departures, b.all_departures, locale);
    case FlightColumn::notes:
        break;
    }
    return compare_text(a.notes, b.notes, locale);
}

RowTone flight_row_tone(const FlightRow& row, const Decimal& cheapest)
{
    if (row.price_sort >= kNoPrice) {
        return RowTone::unavailable;
    }
    return row.price_sort == cheapest ? RowTone::cheapest : RowTone::normal;
}

std::string visible_row_text(const FlightRow& row, const Locale& locale)
{
    const std::string total = row.total ? row.total->to_string(locale.number_symbols()) : std::string{};
    return std::format("{} {} {} -> {} total={} local={}", row.date.iso(), row.departs, row.from, row.to, total,
                       row.fare_local);
}

std::string current_row_text(const FlightResult* flight, const Locale& locale)
{
    if (!flight) {
        return std::string{strings::kNoCurrentRow};
    }
    const std::string total =
        flight->total_converted ? flight->total_converted->to_string(locale.number_symbols()) : std::string{};
    return std::format("{} {}->{} {}", flight->date().iso(), iata_of(flight->from_airport), iata_of(flight->to_airport),
                       total);
}

// ---------------------------------------------------------------------------
// FlightGridModel
// ---------------------------------------------------------------------------

FlightGridModel::FlightGridModel(std::shared_ptr<const Locale> locale, bool offer_plan_return)
    : locale_(std::move(locale)), offer_plan_return_(offer_plan_return)
{
    if (!locale_) {
        throw std::invalid_argument("FlightGridModel needs a locale.");
    }
}

void FlightGridModel::clear(std::string_view currency, bool anywhere, std::string_view summary)
{
    all_.clear();
    currency_ = text::to_upper_invariant(currency);
    anywhere_ = anywhere;
    picked_.reset();
    if (!user_chose_view_) {
        mode_ = anywhere ? ViewMode::cheapest_per_destination : ViewMode::all_fares;  // not a user choice
    }
    rebuild(true);
    summary_ = std::string{summary};
}

void FlightGridModel::append(std::span<const FlightResultPtr> batch)
{
    std::vector<FlightResultPtr> items;
    for (const FlightResultPtr& flight : batch) {
        if (flight) {
            items.push_back(flight);
        }
    }
    if (items.empty()) {
        return;
    }
    all_.insert(all_.end(), items.begin(), items.end());
    if (mode_ != ViewMode::all_fares) {
        rebuild(false);
        return;
    }
    const FlightResultPtr picked = picked_;
    for (const FlightResultPtr& flight : items) {
        const std::size_t idx = shown_.size();
        shown_.push_back(flight);
        rows_.push_back(make_flight_row(idx, flight, *locale_));
        const auto position = std::ranges::upper_bound(order_, idx, [this](std::size_t a, std::size_t b) {
            return row_less(a, b);
        });
        order_.insert(position, idx);
        if (flight->total_converted && *flight->total_converted < cheapest_) {
            cheapest_ = *flight->total_converted;
        }
        if (!current_) {
            current_ = idx;  // the first row added to an empty grid becomes current
        }
    }
    if (picked && selected() != picked) {
        restore_selection(picked, false);
    }
}

void FlightGridModel::set_results(std::span<const FlightResultPtr> results, std::string_view currency,
                                  std::string_view summary)
{
    all_.clear();
    for (const FlightResultPtr& flight : results) {
        if (flight) {
            all_.push_back(flight);
        }
    }
    currency_ = text::to_upper_invariant(currency);
    rebuild(true);
    summary_ = std::string{summary};
}

void FlightGridModel::set_summary(std::string_view summary)
{
    summary_ = std::string{summary};
}

void FlightGridModel::choose_view(ViewMode mode)
{
    if (mode == mode_) {
        return;  // no SelectedIndexChanged
    }
    user_chose_view_ = true;
    mode_ = mode;
    rebuild(true);
}

void FlightGridModel::sort_by_column(FlightColumn column)
{
    if (!flight_column_visible(column, anywhere_)) {
        return;  // hidden columns have no header to click
    }
    user_sort_ = next_sort(user_sort_, column);
    sort_order();
}

void FlightGridModel::mouse_down(std::size_t display_row, MouseButton button)
{
    if (display_row >= order_.size()) {
        return;
    }
    const std::size_t idx = order_[display_row];
    if (rows_[idx].fare) {
        picked_ = rows_[idx].fare;  // RowFare(row) ?? _pickedFare
    }
    if (button != MouseButton::middle) {
        current_ = idx;
    }
}

void FlightGridModel::set_current_row(std::size_t display_row)
{
    if (display_row < order_.size()) {
        current_ = order_[display_row];
    }
}

void FlightGridModel::navigation_key_up()
{
    if (FlightResultPtr current = selected()) {
        picked_ = std::move(current);
    }
}

void FlightGridModel::restore_selection(const FlightResultPtr& target, bool scroll_top_if_missing)
{
    if (order_.empty()) {
        return;
    }
    if (target) {
        for (std::size_t position = 0; position < order_.size(); ++position) {
            const FlightResult& candidate = *rows_[order_[position]].fare;
            if (rows_[order_[position]].fare == target ||
                (iata_of(candidate.from_airport) == iata_of(target->from_airport) &&
                 iata_of(candidate.to_airport) == iata_of(target->to_airport) && candidate.date() == target->date())) {
                make_current(position, ScrollRequest::current);
                return;
            }
        }
        if (mode_ == ViewMode::cheapest_per_destination && target->to_airport) {
            for (std::size_t position = 0; position < order_.size(); ++position) {
                const FlightResult& candidate = *rows_[order_[position]].fare;
                if (candidate.to_airport && candidate.to_airport->group_key() == target->to_airport->group_key()) {
                    make_current(position, ScrollRequest::current);
                    return;
                }
            }
        }
    }
    if (!scroll_top_if_missing) {
        return;
    }
    make_current(0, ScrollRequest::top);
}

const FlightRow& FlightGridModel::row(std::size_t display_row) const
{
    return rows_.at(order_.at(display_row));
}

std::optional<std::size_t> FlightGridModel::current_row() const
{
    return current_ ? position_of(*current_) : std::nullopt;
}

FlightResultPtr FlightGridModel::selected() const
{
    return current_ && *current_ < rows_.size() ? rows_[*current_].fare : FlightResultPtr{};
}

RowTone FlightGridModel::tone(std::size_t display_row) const
{
    return flight_row_tone(row(display_row), cheapest_);
}

std::string FlightGridModel::cell_text(std::size_t display_row, FlightColumn column) const
{
    return flight_cell_text(row(display_row), column, currency_, *locale_);
}

ScrollRequest FlightGridModel::take_scroll_request() noexcept
{
    return std::exchange(scroll_, ScrollRequest::none);
}

FlightMenuState FlightGridModel::context_menu(bool busy) const
{
    const bool has = selected() != nullptr;
    FlightMenuState state;
    state.open_enabled = has;
    state.copy_enabled = has;
    state.plan_return_visible = offer_plan_return_;
    state.plan_return_enabled = offer_plan_return_ && has && !busy;
    state.plan_return_text = std::string{plan_return_menu_text(busy)};
    return state;
}

std::vector<std::string> FlightGridModel::visible_rows_text(std::size_t max) const
{
    std::vector<std::string> lines;
    const std::size_t count = std::min(max, order_.size());
    lines.reserve(count);
    for (std::size_t position = 0; position < count; ++position) {
        lines.push_back(visible_row_text(rows_[order_[position]], *locale_));
    }
    return lines;
}

std::string FlightGridModel::current_row_text() const
{
    const FlightResultPtr current = selected();
    return ui::current_row_text(current.get(), *locale_);
}

std::optional<std::string> FlightGridModel::copy_selected_text() const
{
    const FlightResultPtr current = selected();
    if (!current) {
        return std::nullopt;
    }
    return copy_details_text(*current, *locale_);
}

void FlightGridModel::rebuild(bool keep_top_if_not_picked)
{
    const FlightResultPtr picked = picked_;  // captured before _shown changes (UI-4, LIVE-1)
    shown_ = view_list(all_, mode_);
    cheapest_ = kNoPrice;
    rows_.clear();
    rows_.reserve(shown_.size());
    for (std::size_t idx = 0; idx < shown_.size(); ++idx) {
        rows_.push_back(make_flight_row(idx, shown_[idx], *locale_));
        if (shown_[idx]->total_converted && *shown_[idx]->total_converted < cheapest_) {
            cheapest_ = *shown_[idx]->total_converted;
        }
    }
    order_.resize(rows_.size());
    for (std::size_t idx = 0; idx < order_.size(); ++idx) {
        order_[idx] = idx;
    }
    sort_order();
    // A full reload makes row 0 current and scrolls to the top (01 §8.6).
    current_.reset();
    if (!order_.empty()) {
        current_ = order_.front();
    }
    scroll_ = ScrollRequest::top;
    if (picked || keep_top_if_not_picked) {
        restore_selection(picked, true);
    }
}

void FlightGridModel::sort_order()
{
    std::ranges::sort(order_, [this](std::size_t a, std::size_t b) { return row_less(a, b); });
}

bool FlightGridModel::row_less(std::size_t a, std::size_t b) const
{
    const FlightRow& left = rows_[a];
    const FlightRow& right = rows_[b];
    if (user_sort_) {
        const int result =
            directed(compare_flight_rows(left, right, user_sort_->column, *locale_), user_sort_->direction);
        if (result != 0) {
            return result < 0;
        }
    } else {
        for (const FlightColumn column : kDefaultSort) {
            const int result = compare_flight_rows(left, right, column, *locale_);
            if (result != 0) {
                return result < 0;
            }
        }
    }
    return left.idx < right.idx;  // equal values keep the insertion order
}

std::optional<std::size_t> FlightGridModel::position_of(std::size_t idx) const noexcept
{
    const auto found = std::ranges::find(order_, idx);
    if (found == order_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - order_.begin());
}

void FlightGridModel::make_current(std::size_t display_row, ScrollRequest scroll)
{
    current_ = order_[display_row];
    scroll_ = scroll;
}

} // namespace wizz::ui
