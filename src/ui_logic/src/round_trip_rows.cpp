#include "wizz/ui/round_trip_rows.hpp"

#include "wizz/ui/leg_text.hpp"
#include "wizz/ui/strings.hpp"

#include "wizz/core/currency_format.hpp"
#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/round_trips.hpp"
#include "wizz/core/text.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace wizz::ui {

namespace {

[[nodiscard]] std::string_view iata_of(const AirportPtr& airport, std::string_view fallback) noexcept
{
    return airport ? std::string_view{airport->iata} : fallback;
}

[[nodiscard]] std::string_view name_of(const AirportPtr& airport, std::string_view fallback) noexcept
{
    return airport ? std::string_view{airport->name} : fallback;
}

[[nodiscard]] std::string money_cell(const std::optional<Decimal>& amount, std::string_view currency,
                                     const Locale& locale)
{
    return amount ? amount->format(currency_format::number_format(currency), locale.number_symbols()) : std::string{};
}

} // namespace

std::string_view trip_column_name(TripColumn column) noexcept
{
    switch (column) {
    case TripColumn::idx:
        return "Idx";
    case TripColumn::total:
        return "Total";
    case TripColumn::nights:
        return "Nights";
    case TripColumn::out_date:
        return "Out date";
    case TripColumn::outbound:
        return "Outbound";
    case TripColumn::out_price:
        return "Out price";
    case TripColumn::return_date:
        return "Return date";
    case TripColumn::return_leg:
        return "Return";
    case TripColumn::return_price:
        return "Return price";
    case TripColumn::booking_notes:
        break;
    }
    return "Booking / notes";
}

std::string trip_column_header(TripColumn column, std::string_view currency)
{
    if (column == TripColumn::total) {
        return std::format("Total ({})", currency);
    }
    return std::string{trip_column_name(column)};
}

std::string leg_text(const FlightResult& flight)
{
    return std::format("{}  {} → {}  ({} → {})", format_hhmm(flight.fare.cheapest_departure),
                       iata_of(flight.from_airport, flight.fare.from), iata_of(flight.to_airport, flight.fare.to),
                       name_of(flight.from_airport, flight.fare.from), name_of(flight.to_airport, flight.fare.to));
}

std::string booking_note(const RoundTrip& trip)
{
    std::string note{trip.combined_booking_url ? strings::kOneBooking : strings::kTwoBookings};
    if (!trip.same_away_airport) {
        note += "; ";
        note += strings::open_jaw(iata_of(trip.outbound->to_airport, trip.outbound->fare.to),
                                  iata_of(trip.inbound->from_airport, trip.inbound->fare.from));
    }
    if (!trip.same_home_airport) {
        note += "; ";
        note += strings::back_to(iata_of(trip.inbound->to_airport, trip.inbound->fare.to),
                                 iata_of(trip.outbound->from_airport, trip.outbound->fare.from));
    }
    return note;
}

std::vector<std::string> booking_urls(const RoundTrip& trip)
{
    if (trip.combined_booking_url) {
        return {*trip.combined_booking_url};
    }
    return {trip.outbound->booking_url, trip.inbound->booking_url};
}

TripRow make_trip_row(std::size_t idx, const RoundTrip& trip)
{
    if (!trip.outbound || !trip.inbound) {
        throw std::invalid_argument("A round trip needs both flights.");
    }
    TripRow row;
    row.idx = idx;
    row.total = trip.total;
    row.nights = trip.nights;
    row.out_date = trip.outbound->date();
    row.outbound = leg_text(*trip.outbound);
    row.out_price = trip.outbound->total_converted;
    row.return_date = trip.inbound->date();
    row.return_leg = leg_text(*trip.inbound);
    row.return_price = trip.inbound->total_converted;
    row.booking_notes = booking_note(trip);
    return row;
}

std::string trip_cell_text(const TripRow& row, TripColumn column, std::string_view currency, const Locale& locale)
{
    switch (column) {
    case TripColumn::idx:
        return std::to_string(row.idx);
    case TripColumn::total:
        return money_cell(row.total, currency, locale);
    case TripColumn::nights:
        return std::to_string(row.nights);
    case TripColumn::out_date:
        return locale.format_date(row.out_date, "ddd dd MMM");
    case TripColumn::outbound:
        return row.outbound;
    case TripColumn::out_price:
        return money_cell(row.out_price, currency, locale);
    case TripColumn::return_date:
        return locale.format_date(row.return_date, "ddd dd MMM");
    case TripColumn::return_leg:
        return row.return_leg;
    case TripColumn::return_price:
        return money_cell(row.return_price, currency, locale);
    case TripColumn::booking_notes:
        break;
    }
    return row.booking_notes;
}

int compare_trip_rows(const TripRow& a, const TripRow& b, TripColumn column, const Locale& locale)
{
    switch (column) {
    case TripColumn::idx:
        return compare_values(a.idx, b.idx);
    case TripColumn::total:
        return compare_nullable(a.total, b.total);
    case TripColumn::nights:
        return compare_values(a.nights, b.nights);
    case TripColumn::out_date:
        return compare_values(a.out_date, b.out_date);
    case TripColumn::outbound:
        return compare_text(a.outbound, b.outbound, locale);
    case TripColumn::out_price:
        return compare_nullable(a.out_price, b.out_price);
    case TripColumn::return_date:
        return compare_values(a.return_date, b.return_date);
    case TripColumn::return_leg:
        return compare_text(a.return_leg, b.return_leg, locale);
    case TripColumn::return_price:
        return compare_nullable(a.return_price, b.return_price);
    case TripColumn::booking_notes:
        break;
    }
    return compare_text(a.booking_notes, b.booking_notes, locale);
}

// ---------------------------------------------------------------------------
// RoundTripGridModel
// ---------------------------------------------------------------------------

std::string round_trip_copy_text(const RoundTrip& trip, std::string_view currency, const Locale& locale)
{
    std::string text = copy_details_text(*trip.outbound, locale);
    text += "\r\n";
    text += copy_details_text(*trip.inbound, locale);
    text += "\r\n";
    text += strings::round_trip_copy_line(
        trip.nights, trip.total ? currency_format::format(*trip.total, currency, locale.number_symbols()) : std::string{});
    if (trip.combined_booking_url) {
        text += "\r\n";
        text += *trip.combined_booking_url;
    }
    return text;
}

RoundTripGridModel::RoundTripGridModel(std::shared_ptr<const Locale> locale) : locale_(std::move(locale))
{
    if (!locale_) {
        throw std::invalid_argument("RoundTripGridModel needs a locale.");
    }
}

void RoundTripGridModel::set_trips(std::vector<RoundTrip> trips, std::string_view currency, std::string_view summary)
{
    trips_ = std::move(trips);
    currency_ = text::to_upper_invariant(currency);
    rows_.clear();
    rows_.reserve(trips_.size());
    for (std::size_t idx = 0; idx < trips_.size(); ++idx) {
        rows_.push_back(make_trip_row(idx, trips_[idx]));
    }
    order_.resize(rows_.size());
    for (std::size_t idx = 0; idx < order_.size(); ++idx) {
        order_[idx] = idx;
    }
    sort_order();
    current_.reset();
    if (!order_.empty()) {
        current_ = order_.front();  // row 0 current, at the Total column
        scroll_ = ScrollRequest::top;
    }
    selection_chosen_ = false;  // the selection resets to row 0 with every recompute (01 §9.3, D-76)
    selected_.clear();
    summary_ = std::string{summary};
}

void RoundTripGridModel::select_rows(std::span<const std::size_t> display_rows,
                                     std::optional<std::size_t> current_display_row)
{
    selected_.clear();
    selection_chosen_ =
        !(display_rows.size() == 1 && current_display_row && display_rows.front() == *current_display_row);
    if (!selection_chosen_) {
        return;  // exactly the current row: it follows the current row
    }
    for (const std::size_t row : display_rows) {
        if (row < order_.size()) {
            selected_.insert(order_[row]);
        }
    }
}

std::vector<std::size_t> RoundTripGridModel::selected_rows() const
{
    std::vector<std::size_t> rows;
    if (selection_chosen_ && !selected_.empty()) {
        for (std::size_t position = 0; position < order_.size(); ++position) {
            if (selected_.contains(order_[position])) {
                rows.push_back(position);
            }
        }
    }
    if (rows.empty()) {
        if (const std::optional<std::size_t> current = current_row()) {
            rows.push_back(*current);
        }
    }
    return rows;
}

std::optional<std::string> RoundTripGridModel::copy_selected_text() const
{
    const std::vector<std::size_t> rows = selected_rows();
    if (rows.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> blocks;
    blocks.reserve(rows.size());
    for (const std::size_t row : rows) {
        blocks.push_back(round_trip_copy_text(trips_[order_[row]], currency_, *locale_));
    }
    return join_copied_rows(blocks);
}

std::optional<std::string> RoundTripGridModel::copy_table_text() const
{
    const std::vector<std::size_t> rows = selected_rows();
    if (rows.empty()) {
        return std::nullopt;
    }
    const auto line = [](const auto& cell) {
        std::string text;
        for (const TripColumn column : kTripColumns) {
            if (column == TripColumn::idx) {
                continue;  // hidden
            }
            if (!text.empty() || column != TripColumn::total) {
                text += '\t';
            }
            text += table_cell_text(cell(column));
        }
        return text;
    };
    std::string table = line([this](TripColumn column) { return header(column); });
    for (const std::size_t row : rows) {
        table += "\r\n";
        table += line([this, row](TripColumn column) { return cell_text(row, column); });
    }
    return table;
}

bool RoundTripGridModel::set_require_same_home(bool value) noexcept
{
    return std::exchange(require_same_home_, value) != value;
}

bool RoundTripGridModel::set_require_same_away(bool value) noexcept
{
    return std::exchange(require_same_away_, value) != value;
}

void RoundTripGridModel::sort_by_column(TripColumn column)
{
    if (column == TripColumn::idx) {
        return;  // hidden
    }
    user_sort_ = next_sort(user_sort_, column);
    sort_order();
}

void RoundTripGridModel::mouse_down(std::size_t display_row, MouseButton button)
{
    if (display_row < order_.size() && button != MouseButton::middle) {
        current_ = order_[display_row];
    }
}

void RoundTripGridModel::set_current_row(std::size_t display_row)
{
    if (display_row < order_.size()) {
        current_ = order_[display_row];
    }
}

const TripRow& RoundTripGridModel::row(std::size_t display_row) const
{
    return rows_.at(order_.at(display_row));
}

std::optional<std::size_t> RoundTripGridModel::current_row() const
{
    if (!current_) {
        return std::nullopt;
    }
    const auto found = std::ranges::find(order_, *current_);
    if (found == order_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - order_.begin());
}

const RoundTrip* RoundTripGridModel::selected() const noexcept
{
    return current_ && *current_ < trips_.size() ? &trips_[*current_] : nullptr;
}

std::string RoundTripGridModel::cell_text(std::size_t display_row, TripColumn column) const
{
    return trip_cell_text(row(display_row), column, currency_, *locale_);
}

ScrollRequest RoundTripGridModel::take_scroll_request() noexcept
{
    return std::exchange(scroll_, ScrollRequest::none);
}

std::vector<std::string> RoundTripGridModel::open_selected_urls() const
{
    const RoundTrip* trip = selected();
    return trip ? booking_urls(*trip) : std::vector<std::string>{};
}

std::optional<std::string> RoundTripGridModel::outbound_url() const
{
    const RoundTrip* trip = selected();
    return trip ? std::optional<std::string>{trip->outbound->booking_url} : std::nullopt;
}

std::optional<std::string> RoundTripGridModel::return_url() const
{
    const RoundTrip* trip = selected();
    return trip ? std::optional<std::string>{trip->inbound->booking_url} : std::nullopt;
}

void RoundTripGridModel::sort_order()
{
    std::ranges::sort(order_, [this](std::size_t a, std::size_t b) { return row_less(a, b); });
}

bool RoundTripGridModel::row_less(std::size_t a, std::size_t b) const
{
    const TripRow& left = rows_[a];
    const TripRow& right = rows_[b];
    if (user_sort_) {
        const int result = directed(compare_trip_rows(left, right, user_sort_->column, *locale_), user_sort_->direction);
        if (result != 0) {
            return result < 0;
        }
    } else {
        for (const TripColumn column : kDefaultSort) {
            const int result = compare_trip_rows(left, right, column, *locale_);
            if (result != 0) {
                return result < 0;
            }
        }
    }
    return left.idx < right.idx;
}

TripsView recompute_trips(const LegSearchResult* outbound, const LegSearchResult* inbound, int min_nights,
                          int max_nights, bool require_same_home, bool require_same_away,
                          std::string_view selected_currency, const Locale& locale)
{
    TripsView view;
    view.currency = outbound ? outbound->request.currency : std::string{selected_currency};
    if (!outbound || !inbound) {
        view.summary = std::string{strings::kTripsPlaceholder};
        return view;
    }
    const LegMatch match = round_trips::match_legs(*outbound, *inbound);
    if (match.problem) {
        view.summary = *match.problem;
        return view;
    }
    view.trips = round_trips::combine(match.outbound, match.inbound, min_nights, max_nights, round_trips::kDefaultLimit,
                                      require_same_home, require_same_away);
    view.summary = round_trip_summary(view.trips, min_nights, max_nights, view.currency, locale);
    return view;
}

} // namespace wizz::ui
