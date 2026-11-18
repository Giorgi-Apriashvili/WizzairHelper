// WizzApiClient timetables: GetTimetable (04 §9.1) with the per-day reuse of fetched fares
// (DECISIONS D-66) and fares.json (D-67), and FetchTimetable (04 §9.2).

#include "wizz/services/wizz_client.hpp"

#include "wizz/core/date_chunker.hpp"
#include "wizz/core/errors.hpp"
#include "wizz/core/json.hpp"
#include "wizz/services/wizz_protocol.hpp"

#include <cstdint>
#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wizz {

namespace {

constexpr std::string_view kTimetablePath = "search/timetableV2";
constexpr std::string_view kUnreadableTimetable = "Wizz Air sent a timetable that could not be read: ";

// Validation codes the fetch handles, in precedence order (04 §9.2).
constexpr std::string_view kInvalidMarket = "InvalidMarket";
constexpr std::string_view kInvalidFromDate = "InvalidFromDate";
constexpr std::string_view kInvalidTimeDateRange = "InvalidTimeDateRange";

/// At most two InvalidFromDate shifts: the original send plus two (04 §9.2, D-16).
constexpr int kMaxFromDateShifts = 2;

void append(std::vector<DayFare>& target, std::vector<DayFare>&& source)
{
    target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

} // namespace

// ---------------------------------------------------------------------------
// 04 §9.1 GetTimetable with per-day reuse (D-66) and fares.json (D-67)
// ---------------------------------------------------------------------------

void WizzApiClient::set_timetable_reuse_window(Duration window) noexcept
{
    reuse_window_ticks_.store(window.count());
}

Duration WizzApiClient::timetable_reuse_window() const noexcept
{
    return Duration{reuse_window_ticks_.load()};
}

bool WizzApiClient::keeps_timetables_on_disk() const noexcept
{
    return options_.keep_timetables_on_disk && options_.cache.directory().has_value();
}

void WizzApiClient::load_timetables_once()
{
    if (timetables_loaded_) {
        return;
    }
    timetables_loaded_ = true;
    if (!keeps_timetables_on_disk()) {
        return;
    }
    const std::optional<std::string> text = options_.cache.try_read(kTimetableCacheFile, std::nullopt, *options_.clock);
    if (!text) {
        return;
    }
    try {
        fare_days::Store loaded = fare_days::parse(*text);
        loaded.prune(options_.clock->utc_now(), options_.clock->local_today());
        timetables_ = std::move(loaded);
    } catch (const std::exception&) {
        // Malformed in any way: the whole file is a miss, overwritten at the next save (D-67).
    }
}

void WizzApiClient::save_timetables() noexcept
{
    if (!keeps_timetables_on_disk()) {
        return;
    }
    try {
        const std::scoped_lock saving{save_mutex_};
        fare_days::Store snapshot;
        std::uint64_t generation = 0;
        {
            const std::scoped_lock guard{timetable_mutex_};
            if (timetables_generation_ == saved_generation_) {
                return;  // nothing changed since the last save (or since the start)
            }
            timetables_.prune(options_.clock->utc_now(), options_.clock->local_today());
            snapshot = timetables_;
            generation = timetables_generation_;
        }
        options_.cache.try_write(kTimetableCacheFile, fare_days::serialize(snapshot));  // temp file, then a move
        saved_generation_ = generation;
    } catch (...) {
        // Out of memory while copying or writing: the fares stay in memory, the file as it was.
    }
}

TimetableResult WizzApiClient::get_timetable(const TimetableQuery& query, const NoticeSink& notices,
                                             std::stop_token stop)
{
    return get_timetable(query, notices, std::move(stop), timetable_reuse_window());
}

TimetableResult WizzApiClient::get_timetable(const TimetableQuery& query, const NoticeSink& notices,
                                             std::stop_token stop, Duration reuse_window)
{
    if (query.end_date < query.start_date) {
        return {};  // no request
    }
    TimetableQuery clamped = query;
    const Date today = options_.clock->local_today();
    if (clamped.start_date < today) {
        clamped.start_date = today;
    }
    if (clamped.end_date < clamped.start_date) {
        return {};  // entirely past: no request, not even discovery (CT-API-11)
    }

    const auto chunks = date_chunker::split(clamped.start_date, clamped.end_date, kMaxDaysPerRequest);
    if (chunks.size() > 1) {
        TimetableResult all{.fares = {}, .route_not_served = false, .from_cache = true};
        for (const auto& [from, to] : chunks) {
            TimetableQuery part_query = clamped;
            part_query.start_date = from;
            part_query.end_date = to;
            TimetableResult part = get_timetable(part_query, notices, stop, reuse_window);
            if (part.route_not_served) {
                all.route_not_served = true;  // the remaining chunks are skipped; fares so far are kept
                break;
            }
            append(all.fares, std::move(part.fares));
            all.from_cache = all.from_cache && part.from_cache;
            all.partly_cached = all.partly_cached || part.partly_cached;
            if (part.oldest_reused && (!all.oldest_reused || *part.oldest_reused < *all.oldest_reused)) {
                all.oldest_reused = part.oldest_reused;
            }
        }
        return all;
    }

    // Single chunk. No cancellation check when nothing is fetched: it is at the top of SendApi.
    const fare_days::RouteKey key{
        .from = clamped.from, .to = clamped.to, .price_type = clamped.price_type, .adults = clamped.adults};
    fare_days::Lookup look;
    {
        const std::scoped_lock guard{timetable_mutex_};
        load_timetables_once();
        look = timetables_.lookup(key, clamped.start_date, clamped.end_date, options_.clock->utc_now(), reuse_window,
                                  today);
    }
    if (look.not_served) {
        return TimetableResult{.fares = {}, .route_not_served = true, .from_cache = true, .oldest_reused = look.oldest};
    }
    if (!look.fetch) {
        return TimetableResult{
            .fares = std::move(look.before), .route_not_served = false, .from_cache = true, .oldest_reused = look.oldest};
    }

    TimetableQuery part = clamped;  // the hull: the days not fetched within the window
    part.start_date = look.fetch->first;
    part.end_date = look.fetch->second;
    TimetableResult fresh = fetch_timetable(part, notices, stop);  // errors and cancellations record nothing
    {
        // Stamped after the fetch. No single-flight: two concurrent misses both fetch and
        // the later record wins per day.
        const std::scoped_lock guard{timetable_mutex_};
        const UtcInstant at = options_.clock->utc_now();
        if (fresh.route_not_served) {
            timetables_.record_not_served(key, at);
        } else {
            timetables_.record(key, part.start_date, part.end_date, fresh.fares, at);
        }
        ++timetables_generation_;
    }
    if (fresh.route_not_served) {
        return TimetableResult{.fares = {}, .route_not_served = true, .from_cache = false};
    }

    const bool reused = look.reused_days > 0;
    std::vector<DayFare> fares = std::move(look.before);
    append(fares, std::move(fresh.fares));
    append(fares, std::move(look.after));
    return TimetableResult{.fares = std::move(fares),
                           .route_not_served = false,
                           .from_cache = false,
                           .partly_cached = reused,
                           .oldest_reused = reused ? look.oldest : std::nullopt};
}

// ---------------------------------------------------------------------------
// 04 §9.2 FetchTimetable (no clamp, no reuse; the outer window is recorded)
// ---------------------------------------------------------------------------

TimetableResult WizzApiClient::fetch_timetable(const TimetableQuery& query, const NoticeSink& notices,
                                               std::stop_token stop)
{
    Date start = query.start_date;
    int shifts = 0;
    for (;;) {
        const std::string body = protocol::build_timetable_body(query.from, query.to, start, query.end_date,
                                                                query.price_type, query.adults);
        std::optional<std::string> json;
        try {
            json = send_api("POST", kTimetablePath, body, notices, stop);
        } catch (const WizzValidationError& validation) {
            if (validation.has(kInvalidMarket)) {
                return TimetableResult{.fares = {}, .route_not_served = true, .from_cache = false};
            }
            if (validation.has(kInvalidFromDate)) {
                if (shifts >= kMaxFromDateShifts || start >= query.end_date) {
                    return {};  // "no fares", not an error (CT-SRCH-05)
                }
                start = start.add_days(1);
                ++shifts;
                continue;
            }
            if (!(validation.has(kInvalidTimeDateRange) && query.end_date > start)) {
                throw;  // other codes, or InvalidTimeDateRange on a one-day window
            }
            // InvalidTimeDateRange on a longer window: halve it below.
        }

        if (!json) {
            const Date mid = start.add_days(days_between(query.end_date, start) / 2);  // floor: end > start
            TimetableQuery first = query;
            first.start_date = start;
            first.end_date = mid;
            TimetableQuery second = query;
            second.start_date = mid.add_days(1);
            TimetableResult a = fetch_timetable(first, notices, stop);  // shifts reset
            TimetableResult b = fetch_timetable(second, notices, stop);
            append(a.fares, std::move(b.fares));  // a's rows first, then b's
            return TimetableResult{.fares = std::move(a.fares),
                                   .route_not_served = a.route_not_served && b.route_not_served,
                                   .from_cache = false};
        }

        try {
            return TimetableResult{.fares = protocol::parse_timetable(*json), .route_not_served = false, .from_cache = false};
        } catch (const json::Error& error) {
            // One bad response counts as one failed request, not a crash (04 §9.2, D-27).
            throw WizzApiError(std::string{kUnreadableTimetable} + error.what());
        } catch (const FormatError& error) {
            throw WizzApiError(std::string{kUnreadableTimetable} + error.what());
        }
    }
}

} // namespace wizz
