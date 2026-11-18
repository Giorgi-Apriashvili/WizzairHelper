#pragma once
// The Wizz Air client (04 §1–§13, protocol-reference §2–§7): API base URL
// discovery and invalidation, the route map with its disk cache and stale
// fallback, timetable windows with the per-day fare reuse (DECISIONS D-66) kept
// in `cache\fares.json` (D-67) and the validation-code handling, and the SendApi
// state machine that classifies
// every response, retries transient failures with countdown notices and paces
// all traffic through one RequestGate.
//
// Threading (04 §12, ARCHITECTURE "Threading model"): every public member is
// thread-safe and blocking; call it on a worker thread, never on the UI thread
// (even when everything is cached). Each wait — the discovery and map locks,
// the gate, back-off delays and HTTP transfers — honours the caller's
// std::stop_token and throws wizz::OperationCancelled. A per-attempt timeout is
// not a cancellation: it is a transient failure that is retried. Notices are
// delivered synchronously on the calling thread. Lock order: map → discovery.

#include "wizz/core/cancellation.hpp"
#include "wizz/core/catalog.hpp"
#include "wizz/core/clocks.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/fare_days.hpp"
#include "wizz/core/http.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/progress.hpp"
#include "wizz/core/time.hpp"
#include "wizz/services/app_paths.hpp"
#include "wizz/services/file_cache.hpp"
#include "wizz/services/request_gate.hpp"
#include "wizz/services/wizz_errors.hpp"

#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wizz {

/// `HttpDefaults.BrowserUserAgent` (04 §2.1): sent with every Wizz request.
inline constexpr std::string_view kBrowserUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";

/// `WizzClientOptions` (04 §4): every tunable with its production default. The
/// client copies the options once; the gate is built from them. Tests (05 §1.4)
/// use spacing 0, jitter 0, a recording delay, a FixedClock and FileCache::disabled().
struct WizzClientOptions {
    /// The pages scanned for the API URL, in this order (04 §7.2).
    std::vector<std::string> discovery_pages{
        "https://www.wizzair.com/en-gb",
        "https://www.wizzair.com/",
        "https://www.wizzair.com/en-gb/flights/timetable",
    };
    /// Minimum gap between request starts; also added to a cool-down (04 §5, §11.1).
    Duration request_spacing = std::chrono::seconds{1};
    /// Pseudo-jitter scale; the effective jitter is 0..360 ms (04 §5).
    Duration request_jitter = std::chrono::milliseconds{400};
    /// Per-attempt timeout for discovery pages and API calls, starting after the
    /// gate; also printed in the timeout text "no answer within 30 s" (04 §11.2).
    Duration request_timeout = std::chrono::seconds{30};
    /// Back-off schedule; its size is the number of retries, so at most size + 1
    /// attempts per call (04 §11.4).
    std::vector<Duration> retry_delays{std::chrono::seconds{5}, std::chrono::seconds{15}, std::chrono::seconds{30}};
    /// `map.json` freshness (fresh while age ≤ max age; 04 §8).
    Duration map_max_age = std::chrono::hours{24};
    /// The initial reuse window of fetched fares (a day is reused while `now − fetched <
    /// window`; 04 §9.1, D-66); `set_timetable_reuse_window` changes it later.
    Duration timetable_cache_age = std::chrono::minutes{30};
    /// Keep the reused fares in `cache` as fares.json across restarts (D-67). A disabled
    /// `cache` keeps nothing either way.
    bool keep_timetables_on_disk = true;
    /// Gate waits and back-off delays; must throw OperationCancelled when the token fires.
    DelayFunc delay = &cancellable_sleep;
    /// All clocks: gate, cache ages, WaitUntilUtc, file ages, and "today" (local).
    std::shared_ptr<const Clock> clock = std::make_shared<SystemClock>();
    /// `api-url.txt`, `map.json` and `fares.json`; default `%LOCALAPPDATA%\WizzairHelper\cache`.
    FileCache cache{app_paths::default_cache_dir()};
    /// Collation and title case of the AirportCatalog built from the route map
    /// (02 §5). The app passes Locale::user_default(); tests keep en-US.
    std::shared_ptr<const Locale> locale = Locale::en_us();
};

/// `TimetableQuery` record (04 §1, §9.1): one route and inclusive date window.
/// Without its dates it is the key of the reused fares (fare_days::RouteKey, D-66),
/// compared field by field: codes ordinally and case-sensitively ("bud" ≠ "BUD"),
/// `adults` raw (0 ≠ 1) although both send `adultCount` 1. The display currency is
/// deliberately not part of it.
struct TimetableQuery {
    std::string from;
    std::string to;
    Date start_date;
    Date end_date;
    PriceType price_type = PriceType::regular;
    int adults = 1;

    friend bool operator==(const TimetableQuery&, const TimetableQuery&) = default;
    friend std::strong_ordering operator<=>(const TimetableQuery&, const TimetableQuery&) = default;
};

/// `TimetableResult(Fares, RouteNotServed, FromCache)` (04 §1, §9.1) and what was reused (D-66).
struct TimetableResult {
    /// Reused days before the fetched ones, the response, then reused days after it
    /// (chunks concatenated, never de-duplicated). Rows of a response dated outside the
    /// window asked for are passed on; reused rows are only those inside it.
    std::vector<DayFare> fares;
    bool route_not_served = false;  ///< Wizz answered InvalidMarket (now or within the reuse window)
    bool from_cache = false;        ///< no request was sent (for every chunk of a multi-chunk query)
    bool partly_cached = false;     ///< a request asked only for the days not fetched yet (any chunk)
    std::optional<UtcInstant> oldest_reused;  ///< the oldest fetch time among the reused days or mark
};

/// `WizzApiClient` (04 §1). One instance per process in the app; its memory
/// caches (API URL, catalog, timetables) live as long as it does.
class WizzApiClient {
public:
    /// Longest timetableV2 window in days, inclusive (04 §1, protocol-reference §5).
    static constexpr int kMaxDaysPerRequest = 42;
    /// Cache file names (04 §1, §6).
    static constexpr std::string_view kApiUrlCacheFile = "api-url.txt";
    static constexpr std::string_view kMapCacheFile = "map.json";
    /// The reused fares on disk (D-67).
    static constexpr std::string_view kTimetableCacheFile = "fares.json";

    /// The client shares `transport` (05 §1.4: tests give one fake to two clients)
    /// and builds its RequestGate from `options`. Throws std::invalid_argument for
    /// a null transport, clock or locale, or an empty delay function.
    WizzApiClient(std::shared_ptr<http::Transport> transport, WizzClientOptions options);

    WizzApiClient(const WizzApiClient&) = delete;
    WizzApiClient& operator=(const WizzApiClient&) = delete;

    /// `GetApiUrlAsync` (04 §7.1): the in-memory URL; else, under the cancellable
    /// discovery lock (double-checked), the trimmed `api-url.txt` if it matches the
    /// strict pattern (D-15; never re-written); else discovery (04 §7.2), whose
    /// result is written to `api-url.txt` verbatim, without a newline. Discovery
    /// sends at most one request per page, each through the gate, announcing
    /// "Looking up the current Wizz Air API version" before every page, and throws
    /// WizzApiError (status nullopt) listing every page's problem when all fail.
    [[nodiscard]] std::string get_api_url(const NoticeSink& notices, std::stop_token stop);

    /// `GetAirportsAsync` (04 §8): memory, then a fresh `map.json` (≤ map_max_age;
    /// no request and no API-URL lookup), then GET `asset/map?languageCode=en-gb`
    /// (written to `map.json` only after it parsed). When that fails with a
    /// WizzApiError of any kind or an unreadable map, a parsable `map.json` of any
    /// age is used instead, with the notice "Wizz Air route map unavailable — using
    /// the cached copy". Otherwise the WizzApiError is rethrown unchanged and a
    /// parse failure becomes WizzApiError("The Wizz Air route map could not be
    /// read: …"). Cancellation propagates without any fallback.
    [[nodiscard]] AirportCatalogPtr get_airports(const NoticeSink& notices, std::stop_token stop,
                                                 bool force_refresh = false);

    /// `GetTimetableAsync` (04 §9.1) with per-day reuse (D-66): clamps the start to the
    /// clock's local today (an empty or entirely past window sends nothing) and splits
    /// it into ≤ 42-day chunks (a not-served chunk stops the loop). For each chunk, the
    /// days fetched less than `reuse_window` ago (days dated today: at most 30 minutes)
    /// are reused and at most ONE request asks for the rest — the hull from the first
    /// to the last day not fetched yet — (04 §9.2: InvalidMarket → not served;
    /// InvalidFromDate → start + 1 day, at most twice; InvalidTimeDateRange → halves).
    /// A successful answer is remembered per day after it parsed; errors and
    /// cancellations remember nothing. The first call reads fares.json (D-67) on the
    /// calling thread. Throws WizzApiError (and subclasses) or OperationCancelled.
    [[nodiscard]] TimetableResult get_timetable(const TimetableQuery& query, const NoticeSink& notices,
                                                std::stop_token stop, Duration reuse_window);
    /// The same with the current `timetable_reuse_window()`.
    [[nodiscard]] TimetableResult get_timetable(const TimetableQuery& query, const NoticeSink& notices,
                                                std::stop_token stop);

    /// The reuse window of later `get_timetable` calls without one (the Settings window's
    /// "Reuse fares fetched within", D-68). Thread-safe.
    void set_timetable_reuse_window(Duration window) noexcept;
    [[nodiscard]] Duration timetable_reuse_window() const noexcept;

    /// Writes the reused fares to fares.json when they changed since the last save
    /// (pruned first: D-67), on the calling thread; a worker calls it at the end of each
    /// search. Does nothing without `keep_timetables_on_disk` or a cache directory.
    /// Never throws: a failed write keeps the old file.
    void save_timetables() noexcept;

    /// `RequestCount` (04 §13): sends to wizzair.com so far (discovery pages and API
    /// attempts, counted just before each send, failed ones included).
    [[nodiscard]] std::int64_t request_count() const noexcept { return request_count_.load(); }

    /// Callers queued on the discovery lock right now (tests wait for a caller to queue
    /// instead of sleeping).
    [[nodiscard]] std::size_t discovery_waiters() const { return discovery_lock_.waiter_count(); }

    /// `CurrentApiUrl` (04 §13): the in-memory API base, or nullopt (warm start
    /// before the URL was needed, or just after an invalidation).
    [[nodiscard]] std::optional<std::string> current_api_url() const;

    /// The options the client was built with.
    [[nodiscard]] const WizzClientOptions& options() const noexcept { return options_; }

private:
    enum class Outcome : std::uint8_t { body, rediscover, transient };
    struct AttemptResult {
        Outcome outcome = Outcome::transient;
        std::string text;        ///< the body (Outcome::body) or the transient reason
        bool slow_down = false;  ///< transient 503 / plain 429: cool the gate down
    };

    /// Reports a notice synchronously; an empty sink means "no notices" (04 §3.1).
    static void notify(const NoticeSink& notices, std::string message,
                       std::optional<UtcInstant> wait_until_utc = std::nullopt);
    /// Request::timeout for one attempt (options_.request_timeout, rounded up to ms).
    [[nodiscard]] std::chrono::milliseconds attempt_timeout() const;

    // 04 §7
    [[nodiscard]] std::string discover(const NoticeSink& notices, std::stop_token stop);
    void invalidate_api_url(const std::string& stale);

    // 04 §8
    [[nodiscard]] AirportCatalogPtr try_parse_map(const std::optional<std::string>& json) const;
    [[nodiscard]] AirportCatalogPtr stale_map(const NoticeSink& notices);

    // 04 §9
    /// fares.json is used: kept on disk and a cache directory exists.
    [[nodiscard]] bool keeps_timetables_on_disk() const noexcept;
    /// Reads fares.json once, at the first lookup, unless it was last written more than
    /// fare_days::kLongestReuse ago; any failure leaves the store empty. Requires timetable_mutex_.
    void load_timetables_once();
    [[nodiscard]] TimetableResult fetch_timetable(const TimetableQuery& query, const NoticeSink& notices,
                                                  std::stop_token stop);

    // 04 §11
    [[nodiscard]] std::string send_api(std::string_view method, std::string_view relative_path,
                                       const std::optional<std::string>& body, const NoticeSink& notices,
                                       std::stop_token stop);
    [[nodiscard]] AttemptResult attempt(const std::string& api_url, std::string_view method,
                                        std::string_view relative_path, const std::optional<std::string>& body,
                                        bool rediscovered, std::stop_token stop);
    [[nodiscard]] std::string timeout_reason() const;

    const std::shared_ptr<http::Transport> transport_;
    const WizzClientOptions options_;
    RequestGate gate_;

    std::atomic<std::int64_t> request_count_{0};

    CancellableMutex discovery_lock_;                      ///< 04 §7.1 discoveryLock
    std::atomic<std::shared_ptr<const std::string>> api_url_;  ///< 04 §7.1 memUrl

    CancellableMutex map_lock_;                            ///< 04 §8 mapLock (taken before discovery_lock_)
    std::atomic<AirportCatalogPtr> catalog_;               ///< 04 §8 memCatalog

    std::atomic<Ticks::rep> reuse_window_ticks_;           ///< timetable_reuse_window()

    std::mutex timetable_mutex_;                           ///< guards the three below (no single-flight, 04 §9.1)
    fare_days::Store timetables_;                          ///< the reused fares (D-66)
    bool timetables_loaded_ = false;                       ///< fares.json was read (or is not used)
    std::uint64_t timetables_generation_ = 0;              ///< counts the changes of timetables_

    std::mutex save_mutex_;                                ///< one save_timetables at a time
    std::uint64_t saved_generation_ = 0;                   ///< timetables_generation_ last written; save_mutex_
};

} // namespace wizz
