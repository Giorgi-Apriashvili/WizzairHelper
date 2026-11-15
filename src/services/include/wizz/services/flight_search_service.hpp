#pragma once
// FlightSearchService (03 §5, §5.10; 01 §11.2, §11.9; 06 §2): executes one leg
// search. It loads the route map, gets the exchange rates, plans the leg again
// with its own clock (03 §1 step 4), then fetches every planned timetable window
// serially, filters, prices and de-duplicates the fares, streams the new and
// improved rows as partial batches, and returns the ranked LegSearchResult with
// its warnings and counters.
//
// Threading (03 §5.2, 06 §2, ARCHITECTURE "Threading model"):
// - `search_async` ALWAYS runs the search on a new worker thread, even when the
//   map, the rates and every timetable come from memory (CT-SRCH-06, review
//   UI-2). It never runs any part of the search on the calling thread and never
//   waits for a callback, so it returns while the search is still running.
// - Progress and partial-result callbacks run synchronously on the worker thread
//   (03 §5.2); the UI marshals them into its own ordered channel. Client notices
//   are forwarded synchronously as progress reports.
// - One std::stop_token flows through every wait (locks, the request gate,
//   back-off delays, HTTP transfers). A user cancel during the fetch loop gives a
//   result with `cancelled == true`; one observed before the loop propagates as
//   wizz::OperationCancelled (03 §5.6). Per-attempt timeouts are transient
//   failures inside the client, never cancellations.
// - The service holds no per-search state, so searches are self-contained and
//   may run concurrently. It owns its worker threads: destroying the service
//   requests stop on every search it started and joins their workers (RAII).

#include "wizz/core/catalog.hpp"
#include "wizz/core/clocks.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/progress.hpp"
#include "wizz/services/currency_service.hpp"
#include "wizz/services/wizz_client.hpp"

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <vector>

namespace wizz {

/// Receives one partial batch (03 §5.5 step 5): the new and improving rows of one
/// timetable response, in response order, never empty. Batches are append-only;
/// a later batch may hold a row that supersedes an earlier one with the same
/// (From, To, Date) key, and the final `LegSearchResult::flights` is authoritative.
/// Runs synchronously on the worker thread; the batch is handed over by value so
/// the receiver can move it into its own queue. Empty function = no partial results.
using PartialResultsSink = std::function<void(std::vector<FlightResultPtr> batch)>;

/// `FlightSearchService(WizzApiClient, CurrencyService, TimeProvider)` (03 §5).
/// Thread-safe; non-copyable and non-movable (it owns its worker threads).
class FlightSearchService {
public:
    /// The stages of SearchProgress (03 §5.1), exactly.
    static constexpr std::string_view kStageLoadingMap = "Loading Wizz Air route map";
    static constexpr std::string_view kStageGettingRates = "Getting exchange rates";
    static constexpr std::string_view kStageFetchingFares = "Fetching fares";
    static constexpr std::string_view kStageDone = "Done";
    static constexpr std::string_view kStageCancelled = "Cancelled";
    static constexpr std::string_view kStageStopped = "Stopped";

    /// `MaxConsecutiveFailures` (03 §5.3): this many WizzApiErrors in a row abort the loop.
    static constexpr int kMaxConsecutiveFailures = 4;

    /// `wizz` and `fx` are shared with the rest of the app (one client and one rate
    /// service per process, shared by both legs). `clock` gives "today" for the plan
    /// (its local date), every StepStartedUtc and the elapsed time (its monotonic
    /// counter, 06 §3). `locale` formats the plan notes and the "d MMM" dates of
    /// progress details and warnings (07 §1); pass the locale of
    /// WizzClientOptions::locale. Throws std::invalid_argument when any is null.
    FlightSearchService(std::shared_ptr<WizzApiClient> wizz, std::shared_ptr<CurrencyService> fx,
                        std::shared_ptr<const Clock> clock, std::shared_ptr<const Locale> locale);

    FlightSearchService(const FlightSearchService&) = delete;
    FlightSearchService& operator=(const FlightSearchService&) = delete;
    FlightSearchService(FlightSearchService&&) = delete;
    FlightSearchService& operator=(FlightSearchService&&) = delete;

    /// Requests stop on every search started with `search_async` that is still
    /// running and joins its worker (each wait honours the stop, so this takes
    /// milliseconds). Their futures then hold a cancelled result or OperationCancelled.
    ~FlightSearchService();

    /// `SearchAsync(request, progress, partialResults, ct)` (03 §5.3): starts the
    /// search on a new worker thread and returns its future at once. The worker
    /// observes `stop` through a stop source of its own that `stop` is linked to.
    ///
    /// The future yields the LegSearchResult, or rethrows what the search threw:
    /// a WizzApiError from the route map, OperationCancelled when the user
    /// cancelled before the fetch loop, or any unexpected error (also one thrown
    /// by a callback). Dropping the future does not block.
    [[nodiscard]] std::future<LegSearchResultPtr> search_async(LegSearchRequest request, ProgressSink progress,
                                                               PartialResultsSink partial_results,
                                                               std::stop_token stop);

    /// The search itself, run on the CALLING thread (blocking). `search_async` is
    /// the entry point for a UI thread; call this one only from a worker thread
    /// that the caller owns (e.g. a UI job runner). 03 §5.3 exactly:
    /// 1. report "Loading Wizz Air route map" 0/0, then the route map (exceptions,
    ///    including cancellation, propagate);
    /// 2. report "Getting exchange rates" 0/0, then the rates (exceptions propagate);
    /// 3. plan with the clock's local today; Warnings = plan notes, then
    ///    "Exchange rates are approximate (…)" and "No exchange rate for {currency
    ///    as typed}; converted prices are unavailable." when they apply (03 §5.8);
    /// 4. stage "Fetching fares" (no report of its own); for each planned request i:
    ///    Completed = i; a later chunk of an (origin, group) pair answered
    ///    InvalidMarket is skipped silently; otherwise report "{From} → {GroupLabel}
    ///    {d MMM} – {d MMM}" (three spaces, en dash) and fetch it. Fares outside
    ///    [EffectiveFrom, EffectiveTo] are dropped; fares from an airport that is
    ///    not a plan origin are dropped with one warning per (queried → answering)
    ///    pair; the rest are priced (pricing::build_result) and de-duplicated by
    ///    (From, To, Date) with `is_better`, a replacement keeping its position;
    ///    a non-empty batch of new and replacing rows goes to `partial_results`;
    /// 5. errors per request, in this catch order: OperationCancelled while `stop`
    ///    is requested → Cancelled; WizzBlockedError → abort with its message;
    ///    other WizzApiError → "{From} → {GroupLabel} ({d MMM}–{d MMM}): {message}"
    ///    and, at the 4th in a row, abort with "Stopped after 4 failed requests in
    ///    a row. Last error: {message}". Success or InvalidMarket resets the count;
    /// 6. Completed = Total unless cancelled or aborted; append one "No exchange
    ///    rate for {c}; those fares are shown without a converted price." per
    ///    missing fare currency (first-seen order) and the abort reason; report
    ///    "Done" / "Cancelled" / "Stopped". Flights = result_views::sort_by_price.
    /// Every report carries StepStartedUtc (reset on each stage, request and client
    /// notice); a client notice replaces Detail and carries its WaitUntilUtc.
    /// A stop requested before the route-map or rate stage starts throws
    /// OperationCancelled after that stage's report; inside the loop the stop is
    /// also checked after each request's report, so a fully cached run notices it.
    [[nodiscard]] LegSearchResultPtr search(const LegSearchRequest& request, const ProgressSink& progress,
                                            const PartialResultsSink& partial_results, std::stop_token stop);

    /// `LoadAirportsAsync(progress, ct, forceRefresh)` (03 §5.10, 01 §11.2), on the
    /// calling thread (no thread hop is forced): reports "Loading Wizz Air route
    /// map" with Detail "", 0/0 and StepStartedUtc = now; each client notice is
    /// forwarded with the same stage, Detail = the message, its WaitUntilUtc and a
    /// new StepStartedUtc. Returns WizzApiClient::get_airports(…, force_refresh);
    /// its exceptions propagate. The app never passes force_refresh = true.
    [[nodiscard]] AirportCatalogPtr load_airports(const ProgressSink& progress, std::stop_token stop,
                                                  bool force_refresh = false);

    /// `Reprice(LegSearchResult, currency)` (03 §6.3): pricing::reprice; no network.
    [[nodiscard]] static LegSearchResultPtr reprice(const LegSearchResult& result, std::string_view currency);

    /// `IsBetter(candidate, existing)` (03 §5.3): an available fare beats a
    /// CheckOnSite one; otherwise the strictly lower TotalConverted wins, a missing
    /// total counting as DECIMAL_MAX — so ties keep the existing row.
    [[nodiscard]] static bool is_better(const FlightResult& candidate, const FlightResult& existing) noexcept;

private:
    struct Job;
    struct Worker;

    void run_job(std::unique_ptr<Job> job, Worker& worker) noexcept;
    /// Joins and forgets the workers whose search has finished. Requires workers_mutex_.
    void reap_finished_workers();

    const std::shared_ptr<WizzApiClient> wizz_;
    const std::shared_ptr<CurrencyService> fx_;
    const std::shared_ptr<const Clock> clock_;
    const std::shared_ptr<const Locale> locale_;

    std::mutex workers_mutex_;
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace wizz
