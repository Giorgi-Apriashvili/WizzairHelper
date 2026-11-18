// FlightSearchService (03 §5): the worker hop, the search pipeline (map → rates →
// plan → serial fetch loop → final report) and LoadAirports (03 §5.10).

#include "wizz/services/flight_search_service.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/core/fx_rates.hpp"
#include "wizz/core/planner.hpp"
#include "wizz/core/pricing.hpp"
#include "wizz/core/result_views.hpp"
#include "wizz/services/wizz_errors.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <format>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

namespace wizz {

namespace {

// Punctuation of the user-visible texts (03 §0), spelled as escapes so that a
// look-alike character cannot slip in: → U+2192, – U+2013.
constexpr std::string_view kArrow = "→";
constexpr std::string_view kEnDash = "–";

/// `{d:d MMM}` (03 §0): day without a leading zero and the culture's abbreviated month.
constexpr std::string_view kDayMonthPattern = "d MMM";

/// The insertion-ordered `best` map of 03 §5.3, keyed by the response row's
/// (From, To, Date) with ordinal string equality. A replacement keeps the key's
/// original position, which is the final tie-break of SortByPrice (03 §5.5).
class BestFares {
public:
    /// Stores `candidate` when its key is new or it beats the stored row (is_better).
    /// Returns true when it was stored (it then belongs in the partial batch).
    bool offer(const FlightResultPtr& candidate)
    {
        Key key{candidate->fare.from, candidate->fare.to, candidate->fare.date};
        if (const auto found = index_.find(key); found != index_.end()) {
            FlightResultPtr& existing = rows_[found->second];
            if (!FlightSearchService::is_better(*candidate, *existing)) {
                return false;  // ties keep the existing row
            }
            existing = candidate;
            return true;
        }
        index_.emplace(std::move(key), rows_.size());
        rows_.push_back(candidate);
        return true;
    }

    /// The rows in first-insertion order of their keys.
    [[nodiscard]] const std::vector<FlightResultPtr>& values() const noexcept { return rows_; }

private:
    using Key = std::tuple<std::string, std::string, Date>;
    std::map<Key, std::size_t> index_;
    std::vector<FlightResultPtr> rows_;
};

/// One execution of 03 §5.3. Holds the per-search state the pseudocode keeps in
/// local variables; lives on the stack of the thread running the search.
class SearchRun {
public:
    SearchRun(const LegSearchRequest& request, const ProgressSink& progress, const PartialResultsSink& partial_results,
              std::stop_token stop, WizzApiClient& wizz, CurrencyService& fx, const Clock& clock, const Locale& locale)
        : request_(request),
          progress_(progress),
          partial_results_(partial_results),
          stop_(std::move(stop)),
          wizz_(wizz),
          fx_(fx),
          clock_(clock),
          locale_(locale),
          started_(clock.monotonic_now()),
          reuse_window_(wizz.timetable_reuse_window()),
          step_started_(clock.utc_now())
    {
    }

    [[nodiscard]] LegSearchResultPtr run()
    {
        const NoticeSink notices = [this](const ClientNotice& notice) { on_notice(notice); };

        // Report 1 is always sent (03 §5.4), then the route map. Exceptions propagate.
        report();
        throw_if_cancelled(stop_);
        catalog_ = wizz_.get_airports(notices, stop_);

        // Report 2 is always sent, then the rates. Exceptions propagate.
        begin_stage(FlightSearchService::kStageGettingRates);
        throw_if_cancelled(stop_);
        rates_ = fx_.get_rates(notices, stop_);

        plan_ = planner::plan(*catalog_, request_, clock_.local_today(), locale_);
        warnings_ = plan_.notes;
        if (rates_->is_approximate) {
            warnings_.push_back(std::format("Exchange rates are approximate ({}, {}); converted prices may be off by a "
                                            "few percent.",
                                            rates_->source, rates_->date.iso()));
        }
        if (!fx_rates::supports(*rates_, request_.currency)) {
            // The currency as typed, not upper-cased (03 §5.8 item 3).
            warnings_.push_back(
                std::format("No exchange rate for {}; converted prices are unavailable.", request_.currency));
        }

        total_ = static_cast<int>(plan_.requests.size());
        stage_ = FlightSearchService::kStageFetchingFares;  // no report of its own (03 §5.4)
        origin_iatas_ = pricing::iata_set(plan_.origin_airports);
        destination_iatas_ = pricing::iata_set(plan_.destination_airports);

        for (std::size_t i = 0; i < plan_.requests.size(); ++i) {
            completed_ = static_cast<int>(i);
            if (!fetch(plan_.requests[i], notices)) {
                break;
            }
        }
        return finish();
    }

private:
    /// `Report(waitUntil)` (03 §5.3): an immutable snapshot of the current state.
    void report(std::optional<UtcInstant> waiting_until = std::nullopt) const
    {
        if (progress_) {
            progress_(SearchProgress{.stage = std::string{stage_},
                                     .detail = detail_,
                                     .completed = completed_,
                                     .total = total_,
                                     .waiting_until_utc = waiting_until,
                                     .step_started_utc = step_started_,
                                     .waiting_for_user = false});
        }
    }

    /// A new stage with an empty detail and a new step start, reported at once.
    void begin_stage(std::string_view stage)
    {
        stage_ = stage;
        detail_.clear();
        step_started_ = clock_.utc_now();
        report();
    }

    /// Client notices replace Detail, restart the step and carry their WaitUntilUtc (03 §5.4).
    void on_notice(const ClientNotice& notice)
    {
        detail_ = notice.message;
        step_started_ = clock_.utc_now();
        report(notice.wait_until_utc);
    }

    [[nodiscard]] std::string day_month(Date date) const { return locale_.format_date(date, kDayMonthPattern); }

    /// One planned request (the loop body of 03 §5.3). Returns false to stop the loop.
    [[nodiscard]] bool fetch(const PlannedRequest& planned, const NoticeSink& notices)
    {
        const std::string& from = planned.from->iata;
        if (const auto mark = not_served_.find(std::pair{from, planned.group_key}); mark != not_served_.end()) {
            // A later chunk of an unserved route: skipped without a report (CORE-4). When a
            // reused not-served mark said so, that mark answers this chunk too (D-70).
            if (mark->second) {
                ++requests_from_cache_;
            }
            return true;
        }

        const std::string group = planner::group_label(*catalog_, planned.group_key);
        detail_ = std::format("{} {} {}   {} {} {}", from, kArrow, group, day_month(planned.start_date), kEnDash,
                              day_month(planned.end_date));
        step_started_ = clock_.utc_now();
        report();

        try {
            // A cache hit answers without any wait, so look at the token here too:
            // a cancel is then noticed even when every timetable is cached.
            throw_if_cancelled(stop_);
            const TimetableResult timetable = wizz_.get_timetable(TimetableQuery{.from = from,
                                                                                 .to = planned.query_to,
                                                                                 .start_date = planned.start_date,
                                                                                 .end_date = planned.end_date,
                                                                                 .price_type = request_.price_type,
                                                                                 .adults = request_.adults},
                                                                  notices, stop_, reuse_window_);
            count_reuse(timetable);
            consecutive_failures_ = 0;
            if (timetable.route_not_served) {
                // Success-like: no warning. Remembers whether a reused mark answered it (D-70).
                not_served_.emplace(std::pair{from, planned.group_key}, timetable.from_cache);
                return true;
            }
            process(planned, group, timetable.fares);
        } catch (const OperationCancelled&) {
            if (!stop_.stop_requested()) {
                throw;  // `catch … when ct.IsCancellationRequested` (03 §5.6)
            }
            cancelled_ = true;
            return false;
        } catch (const WizzBlockedError& error) {
            // Caught before its base class: no per-request warning (03 §5.6).
            abort_reason_ = error.what();
            return false;
        } catch (const WizzApiError& error) {
            warnings_.push_back(std::format("{} {} {} ({}{}{}): {}", from, kArrow, group, day_month(planned.start_date),
                                            kEnDash, day_month(planned.end_date), error.what()));
            if (++consecutive_failures_ >= FlightSearchService::kMaxConsecutiveFailures) {
                abort_reason_ =
                    std::format("Stopped after {} failed requests in a row. Last error: {}",
                                FlightSearchService::kMaxConsecutiveFailures, error.what());
                return false;
            }
        }
        return true;
    }

    /// Counts a planned request answered from reused fares, in full or in part (D-70).
    void count_reuse(const TimetableResult& timetable)
    {
        if (timetable.from_cache) {
            ++requests_from_cache_;
        } else if (timetable.partly_cached) {
            ++requests_partly_cached_;
        }
        if (timetable.oldest_reused && (!oldest_reused_ || *timetable.oldest_reused < *oldest_reused_)) {
            oldest_reused_ = timetable.oldest_reused;
        }
    }

    /// 03 §5.5: date filter, foreign-origin safety net, BuildResult, de-duplication, batch.
    void process(const PlannedRequest& planned, const std::string& group, const std::vector<DayFare>& fares)
    {
        const std::string& from = planned.from->iata;
        std::vector<FlightResultPtr> batch;
        for (const DayFare& fare : fares) {  // response order
            if (fare.date < plan_.effective_from || fare.date > plan_.effective_to) {
                continue;
            }
            if (!origin_iatas_.contains(fare.from)) {
                if (foreign_origins_.add(std::format("{}{}{}", from, kArrow, fare.from))) {
                    warnings_.push_back(std::format("{} {} {} was answered with flights from {}, which is outside your "
                                                    "start radius; those fares were left out.",
                                                    from, kArrow, group, planner::display(*catalog_, fare.from)));
                }
                continue;
            }
            FlightResultPtr result =
                pricing::build_result(*catalog_, request_, *rates_, fare, &missing_currencies_, &destination_iatas_);
            if (best_.offer(result)) {
                batch.push_back(std::move(result));
            }
        }
        if (!batch.empty() && partial_results_) {
            partial_results_(std::move(batch));
        }
    }

    [[nodiscard]] LegSearchResultPtr finish()
    {
        if (!cancelled_ && !abort_reason_) {
            completed_ = total_;
        }
        for (const std::string& currency : missing_currencies_) {
            warnings_.push_back(
                std::format("No exchange rate for {}; those fares are shown without a converted price.", currency));
        }
        if (abort_reason_) {
            warnings_.push_back(*abort_reason_);
        }
        begin_stage(cancelled_      ? FlightSearchService::kStageCancelled
                    : abort_reason_ ? FlightSearchService::kStageStopped
                                    : FlightSearchService::kStageDone);

        return std::make_shared<const LegSearchResult>(LegSearchResult{
            .request = request_,
            .plan = std::move(plan_),
            .flights = result_views::sort_by_price(best_.values()),
            .warnings = std::move(warnings_),
            .rates = rates_,
            .requests_completed = completed_,
            .routes_not_served = static_cast<int>(not_served_.size()),
            .cancelled = cancelled_,
            .abort_reason = std::move(abort_reason_),
            .elapsed = clock_.monotonic_now() - started_,
            .requests_from_cache = requests_from_cache_,
            .requests_partly_cached = requests_partly_cached_,
            .oldest_reused_age = oldest_reused_ ? std::optional<Duration>{std::max(Duration::zero(),
                                                                                    clock_.utc_now() - *oldest_reused_)}
                                                : std::nullopt,
        });
    }

    // Inputs
    const LegSearchRequest& request_;
    const ProgressSink& progress_;
    const PartialResultsSink& partial_results_;
    const std::stop_token stop_;
    WizzApiClient& wizz_;
    CurrencyService& fx_;
    const Clock& clock_;
    const Locale& locale_;
    const Ticks started_;  ///< monotonic, after the thread hop (03 §5.1 Elapsed, 06 §3)
    const Duration reuse_window_;  ///< the client's reuse window when the search began (D-68)

    // Progress state
    std::string_view stage_ = FlightSearchService::kStageLoadingMap;
    std::string detail_;
    int completed_ = 0;
    int total_ = 0;
    UtcInstant step_started_;

    // Search state
    AirportCatalogPtr catalog_;
    FxRatesPtr rates_;
    SearchPlan plan_;
    std::vector<std::string> warnings_;
    pricing::CodeSet origin_iatas_;
    pricing::CodeSet destination_iatas_;
    BestFares best_;
    pricing::OrderedCodeSet missing_currencies_;
    /// (from IATA, group key), ordinal → whether a reused not-served mark said so (D-70).
    std::map<std::pair<std::string, std::string>, bool> not_served_;
    pricing::OrderedCodeSet foreign_origins_;                     ///< "{queried}→{answering}", ci
    int consecutive_failures_ = 0;
    bool cancelled_ = false;
    std::optional<std::string> abort_reason_;
    int requests_from_cache_ = 0;
    int requests_partly_cached_ = 0;
    std::optional<UtcInstant> oldest_reused_;
};

/// Writes the client's reused fares to fares.json when the search ends, however it ends
/// (D-67): on the search's own worker thread, never on a UI thread.
class SaveFaresAtTheEnd {
public:
    explicit SaveFaresAtTheEnd(WizzApiClient& wizz) noexcept : wizz_(wizz) {}
    ~SaveFaresAtTheEnd() { wizz_.save_timetables(); }

    SaveFaresAtTheEnd(const SaveFaresAtTheEnd&) = delete;
    SaveFaresAtTheEnd& operator=(const SaveFaresAtTheEnd&) = delete;
    SaveFaresAtTheEnd(SaveFaresAtTheEnd&&) = delete;
    SaveFaresAtTheEnd& operator=(SaveFaresAtTheEnd&&) = delete;

private:
    WizzApiClient& wizz_;
};

} // namespace

// ---------------------------------------------------------------------------
// Worker bookkeeping
// ---------------------------------------------------------------------------

/// What a worker needs from the `search_async` call; owned by the worker thread.
struct FlightSearchService::Job {
    LegSearchRequest request;
    ProgressSink progress;
    PartialResultsSink partial_results;
    std::promise<LegSearchResultPtr> promise;
};

/// One worker thread and the stop source its search observes.
struct FlightSearchService::Worker {
    /// Forwards a stop request of the caller's token to the job's own source.
    struct Link {
        std::stop_source target;
        void operator()() noexcept { target.request_stop(); }
    };

    std::stop_source stop;                         ///< requested by the caller's token or by ~FlightSearchService
    std::optional<std::stop_callback<Link>> link;  ///< caller's token → `stop`
    std::atomic<bool> finished{false};             ///< set as the worker's last action
    std::jthread thread;
};

FlightSearchService::FlightSearchService(std::shared_ptr<WizzApiClient> wizz, std::shared_ptr<CurrencyService> fx,
                                         std::shared_ptr<const Clock> clock, std::shared_ptr<const Locale> locale)
    : wizz_(std::move(wizz)), fx_(std::move(fx)), clock_(std::move(clock)), locale_(std::move(locale))
{
    if (!wizz_) {
        throw std::invalid_argument("FlightSearchService needs a Wizz Air client.");
    }
    if (!fx_) {
        throw std::invalid_argument("FlightSearchService needs a currency service.");
    }
    if (!clock_) {
        throw std::invalid_argument("FlightSearchService needs a clock.");
    }
    if (!locale_) {
        throw std::invalid_argument("FlightSearchService needs a locale.");
    }
}

FlightSearchService::~FlightSearchService()
{
    std::vector<std::unique_ptr<Worker>> workers;
    {
        const std::scoped_lock lock{workers_mutex_};
        workers.swap(workers_);
    }
    for (const std::unique_ptr<Worker>& worker : workers) {
        worker->stop.request_stop();
    }
    for (std::unique_ptr<Worker>& worker : workers) {
        if (!worker->thread.joinable()) {
            continue;
        }
        if (worker->thread.get_id() == std::this_thread::get_id()) {
            // The last owner let go of the service from inside one of its own
            // callbacks: that worker cannot join itself. Its search keeps its own
            // references to the client, rates service, clock and locale, and after
            // the search it touches only its Worker record, which must therefore
            // outlive it (a deliberate, bounded leak in a pathological case).
            worker->thread.detach();
            static_cast<void>(worker.release());
            continue;
        }
        worker->thread.join();
    }
}

void FlightSearchService::reap_finished_workers()
{
    for (auto it = workers_.begin(); it != workers_.end();) {
        if ((*it)->finished.load(std::memory_order_acquire)) {
            (*it)->thread.join();  // past its last statement: returns at once
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
}

std::future<LegSearchResultPtr> FlightSearchService::search_async(LegSearchRequest request, ProgressSink progress,
                                                                  PartialResultsSink partial_results,
                                                                  std::stop_token stop)
{
    auto worker = std::make_unique<Worker>();
    // Linked before the thread starts, so a token that is already stopped is
    // seen by the worker from its first check on.
    worker->link.emplace(stop, Worker::Link{worker->stop});

    auto job = std::make_unique<Job>(Job{.request = std::move(request),
                                         .progress = std::move(progress),
                                         .partial_results = std::move(partial_results),
                                         .promise = {}});
    std::future<LegSearchResultPtr> future = job->promise.get_future();

    const std::scoped_lock lock{workers_mutex_};
    reap_finished_workers();
    Worker& record = *worker;
    workers_.push_back(std::move(worker));
    try {
        // The worker refers to `this` and to `record`; both outlive it because the
        // destructor (and reap_finished_workers) join it before destroying them.
        record.thread = std::jthread{
            [this, &record, owned = std::move(job)]() mutable noexcept { run_job(std::move(owned), record); }};
    } catch (...) {
        workers_.pop_back();  // the thread never started
        throw;
    }
    return future;
}

void FlightSearchService::run_job(std::unique_ptr<Job> job, Worker& worker) noexcept
{
    std::promise<LegSearchResultPtr> promise = std::move(job->promise);
    try {
        LegSearchResultPtr result = search(job->request, job->progress, job->partial_results, worker.stop.get_token());
        job.reset();  // release the caller's callbacks before the future becomes ready
        promise.set_value(std::move(result));
    } catch (...) {
        job.reset();
        promise.set_exception(std::current_exception());
    }
    worker.finished.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// 03 §5.3 SearchAsync (the body), §5.10 LoadAirportsAsync, §6.3 Reprice
// ---------------------------------------------------------------------------

LegSearchResultPtr FlightSearchService::search(const LegSearchRequest& request, const ProgressSink& progress,
                                               const PartialResultsSink& partial_results, std::stop_token stop)
{
    // Own references for the duration of the run: the search never depends on the
    // service object staying alive.
    const std::shared_ptr<WizzApiClient> wizz = wizz_;
    const std::shared_ptr<CurrencyService> fx = fx_;
    const std::shared_ptr<const Clock> clock = clock_;
    const std::shared_ptr<const Locale> locale = locale_;
    const SaveFaresAtTheEnd save{*wizz};  // after the run, also when it throws
    SearchRun run{request, progress, partial_results, std::move(stop), *wizz, *fx, *clock, *locale};
    return run.run();
}

AirportCatalogPtr FlightSearchService::load_airports(const ProgressSink& progress, std::stop_token stop,
                                                     bool force_refresh)
{
    const auto report = [&](std::string detail, std::optional<UtcInstant> waiting_until) {
        if (progress) {
            progress(SearchProgress{.stage = std::string{kStageLoadingMap},
                                    .detail = std::move(detail),
                                    .completed = 0,
                                    .total = 0,
                                    .waiting_until_utc = waiting_until,
                                    .step_started_utc = clock_->utc_now(),
                                    .waiting_for_user = false});
        }
    };
    report({}, std::nullopt);
    const NoticeSink notices = [&](const ClientNotice& notice) { report(notice.message, notice.wait_until_utc); };
    return wizz_->get_airports(notices, std::move(stop), force_refresh);
}

LegSearchResultPtr FlightSearchService::reprice(const LegSearchResult& result, std::string_view currency)
{
    return pricing::reprice(result, currency);
}

bool FlightSearchService::is_better(const FlightResult& candidate, const FlightResult& existing) noexcept
{
    if (candidate.is_available() != existing.is_available()) {
        return candidate.is_available();
    }
    const Decimal candidate_total = candidate.total_converted.value_or(Decimal::max_value());
    const Decimal existing_total = existing.total_converted.value_or(Decimal::max_value());
    return candidate_total < existing_total;  // strict: ties keep the existing row
}

} // namespace wizz
