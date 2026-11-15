// WizzApiClient timetables: GetTimetable (04 §9.1) and FetchTimetable (04 §9.2).

#include "wizz/services/wizz_client.hpp"

#include "wizz/core/date_chunker.hpp"
#include "wizz/core/errors.hpp"
#include "wizz/core/json.hpp"
#include "wizz/services/wizz_protocol.hpp"

#include <iterator>
#include <utility>

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
// 04 §9.1 GetTimetable
// ---------------------------------------------------------------------------

TimetableResult WizzApiClient::get_timetable(const TimetableQuery& query, const NoticeSink& notices,
                                             std::stop_token stop)
{
    if (query.end_date < query.start_date) {
        return {};  // no request
    }
    TimetableQuery clamped = query;
    if (const Date today = options_.clock->local_today(); clamped.start_date < today) {
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
            TimetableResult part = get_timetable(part_query, notices, stop);  // own cache key
            if (part.route_not_served) {
                all.route_not_served = true;  // the remaining chunks are skipped; fares so far are kept
                break;
            }
            append(all.fares, std::move(part.fares));
            all.from_cache = all.from_cache && part.from_cache;
        }
        return all;  // the aggregate itself is not cached
    }

    // Single chunk. No cancellation check on a hit: it is at the top of SendApi.
    {
        const std::scoped_lock guard{timetable_mutex_};
        if (const auto hit = timetable_cache_.find(clamped);
            hit != timetable_cache_.end() && options_.clock->utc_now() - hit->second.at < options_.timetable_cache_age) {
            TimetableResult cached = hit->second.result;
            cached.from_cache = true;
            return cached;
        }
    }

    TimetableResult result = fetch_timetable(clamped, notices, stop);  // errors are not cached
    {
        const std::scoped_lock guard{timetable_mutex_};
        // Stamped after the fetch; stored with from_cache = false. No single-flight:
        // two concurrent identical misses both fetch and the later one wins.
        timetable_cache_.insert_or_assign(clamped, CachedTimetable{.at = options_.clock->utc_now(), .result = result});
    }
    return result;
}

// ---------------------------------------------------------------------------
// 04 §9.2 FetchTimetable (no clamp, no cache; the memory cache keeps the outer key)
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
