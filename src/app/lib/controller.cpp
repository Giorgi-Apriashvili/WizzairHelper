#include "controller.hpp"

#include "busy_panel.hpp"
#include "flight_grid.hpp"
#include "input_panel.hpp"
#include "log_view.hpp"
#include "qt_strings.hpp"
#include "round_trip_grid.hpp"

#include "wizz/core/airbnb_links.hpp"
#include "wizz/core/cancellation.hpp"
#include "wizz/core/city_names.hpp"
#include "wizz/core/http.hpp"
#include "wizz/core/planner.hpp"
#include "wizz/core/progress.hpp"
#include "wizz/core/text.hpp"
#include "wizz/services/wizz_errors.hpp"
#include "wizz/ui/leg_stream.hpp"
#include "wizz/ui/leg_text.hpp"
#include "wizz/ui/return_request.hpp"
#include "wizz/ui/round_trip_rows.hpp"
#include "wizz/ui/strings.hpp"

#include <QTabWidget>
#include <QTimer>

#include <filesystem>
#include <ios>
#include <utility>
#include <vector>

namespace wizz::app {

namespace strings = ui::strings;

/// The `finally` of a busy phase (01 §3.2 step 3, §11.2, §11.4, §11.9): dispose the
/// stop source, SetWorking(false), End(UnexpectedErrorText, false) when still busy;
/// for a leg run also `finished = true` first. Does nothing once the controller shuts
/// down (the frame is destroyed with the window).
class Controller::PhaseGuard {
public:
    explicit PhaseGuard(Controller& controller, ui::LegStream* stream = nullptr) noexcept
        : controller_(controller), stream_(stream)
    {
    }
    PhaseGuard(const PhaseGuard&) = delete;
    PhaseGuard& operator=(const PhaseGuard&) = delete;
    ~PhaseGuard()
    {
        if (stream_ != nullptr) {
            stream_->finish();
        }
        controller_.end_phase();
    }

private:
    Controller& controller_;
    ui::LegStream* stream_;
};

Controller::Controller(std::shared_ptr<const AppServices> services, MainViews views, bool autotest, QObject* parent)
    : QObject(parent),
      services_(std::move(services)),
      views_(views),
      autotest_(autotest),
      jobs_(std::make_unique<JobRunner>()),
      flows_(std::make_unique<TaskScope>())
{
}

Controller::~Controller()
{
    shutting_down_ = true;
    if (stop_source_) {
        stop_source_->request_stop();
    }
    background_stop_.request_stop();
    flows_.reset();  // suspended flows are destroyed; their guards see shutting_down_
    jobs_.reset();   // stops and joins the workers
}

void Controller::set_dialogs(ControllerDialogs dialogs)
{
    dialogs_ = std::move(dialogs);
}

void Controller::spawn(Task<> flow)
{
    flows_->spawn(std::move(flow));
}

void Controller::request_search_outbound()
{
    spawn(search_outbound());
}

void Controller::request_search_return()
{
    spawn(search_return());
}

void Controller::request_plan_return(wizz::FlightResultPtr fare)
{
    if (fare) {
        spawn(plan_return_from(std::move(fare)));
    }
}

void Controller::cancel_work()
{
    if (stop_source_) {
        stop_source_->request_stop();
    }
}

void Controller::stop_background_work()
{
    background_stop_.request_stop();
}

// ---------------------------------------------------------------------------
// LoadCatalogAsync (01 §11.2)
// ---------------------------------------------------------------------------

Task<AirportCatalogPtr> Controller::load_catalog()
{
    if (catalog_) {
        co_return catalog_;
    }
    set_working(true);
    const std::stop_token stop = begin_phase();
    views_.busy->begin(strings::kBeginLoadingMap);
    const PhaseGuard guard{*this};
    try {
        const JobId job = jobs_->new_job_id();
        AirportCatalogPtr catalog = co_await jobs_->run(
            job, stop, [this, search = services_->search](const JobChannel& channel) {
                // Progress<SearchProgress>(_busy.Report): posted to the UI thread.
                const ProgressSink progress = [this, &channel](const SearchProgress& report) {
                    channel.post([this, report] { views_.busy->report(report); });
                };
                return search->load_airports(progress, channel.stop_token());
            });
        catalog_ = std::move(catalog);
        const std::vector<std::string> names = catalog_->suggestion_names();
        views_.input->set_suggestions(names);
        const std::size_t airports = catalog_->real_airports().size();
        views_.busy->end(strings::map_ready(airports));
        log(strings::route_map_log(airports, services_->wizz->current_api_url()));
        co_return catalog_;
    } catch (const OperationCancelled&) {
        views_.busy->end(strings::kMapLoadCancelled, false);
    } catch (const WizzApiError& error) {
        views_.busy->end(strings::map_load_failed(error.what()), false);
        log(strings::map_error_log(error.what()));
    } catch (const http::TransportError& error) {  // HttpRequestException | IOException
        views_.busy->end(strings::map_unreachable(error.what()), false);
        log(strings::map_error_log(error.what()));
    } catch (const http::TimeoutError& error) {
        views_.busy->end(strings::map_unreachable(error.what()), false);
        log(strings::map_error_log(error.what()));
    } catch (const std::ios_base::failure& error) {
        views_.busy->end(strings::map_unreachable(error.what()), false);
        log(strings::map_error_log(error.what()));
    }
    co_return nullptr;
}

Task<> Controller::prefetch_rates()
{
    // No busy phase: the user may search at once, and that search's rate stage waits for
    // this one on the service's lock, then finds the rates in memory (D-69).
    try {
        const bool loaded = co_await jobs_->run(jobs_->new_job_id(), background_stop_.get_token(),
                                                [fx = services_->fx](const JobChannel& channel) {
                                                    return fx->prefetch(channel.stop_token());
                                                });
        static_cast<void>(loaded);  // false: every source failed; a search falls back with notices
    } catch (...) {
        // Cancelled when the window closes, or anything unexpected: the prefetch is only a
        // head start, so it stays silent. No exception is bound here (D-64).
    }
}

// ---------------------------------------------------------------------------
// Homes on Airbnb (D-72)
// ---------------------------------------------------------------------------

void Controller::request_homes(const ui::HomesStay& stay, ui::HomesRadius radius, GridActions actions)
{
    if (homes_lookup_running_ || shutting_down_ || !stay.destination || background_stop_.stop_requested()) {
        return;
    }
    spawn(open_homes(stay, radius, std::move(actions)));
}

Task<> Controller::open_homes(ui::HomesStay stay, ui::HomesRadius radius, GridActions actions)
{
    const AirportPtr station = stay.destination;
    CityCentreLookup lookup;
    if (const auto known = city_centres_.find(station->iata); known != city_centres_.end()) {
        lookup = known->second;
    } else {
        // No busy phase (a search may be running): the lookup runs beside it, like the rate
        // prefetch (D-69), and only closing the window stops it.
        std::string place = city_names::clean_name(station->name);
        if (place.empty()) {
            place = std::string{text::trim(station->name)};
        }
        log(strings::homes_lookup_log(place));
        homes_lookup_running_ = true;
        bool unexpected = false;
        try {
            lookup = co_await jobs_->run(jobs_->new_job_id(), background_stop_.get_token(),
                                         [places = services_->places, station](const JobChannel& channel) {
                                             return find_city_centre(*places, *station, channel.stop_token());
                                         });
        } catch (...) {
            unexpected = true;  // no exception is bound here (D-64)
        }
        homes_lookup_running_ = false;
        if (background_stop_.stop_requested()) {
            co_return;  // the window is closing: open nothing
        }
        if (unexpected) {
            lookup = CityCentreLookup{.centre = std::nullopt, .place_name = place, .lookup_failed = true};
        }
        if (!lookup.lookup_failed) {
            city_centres_.insert_or_assign(station->iata, lookup);
        }
    }

    const std::string stay_text = strings::homes_stay_text(
        locale().format_date(stay.check_in, "dd MMM"), locale().format_date(stay.check_out, "dd MMM"), stay.adults);
    std::string url;
    if (lookup.centre) {
        url = airbnb_links::homes_in_box(airbnb_links::box_around(lookup.centre->location, ui::radius_km(radius)),
                                         stay.check_in, stay.check_out, stay.adults);
        log(strings::homes_centre_log(strings::homes_radius_label(radius), lookup.centre->name, stay_text));
    } else {
        const std::string query = airbnb_links::place_query(lookup.place_name, station->country_name);
        url = airbnb_links::homes_by_name(query, stay.check_in, stay.check_out, stay.adults);
        log(strings::homes_by_name_log(query, lookup.lookup_failed, stay_text));
    }
    (void)open_url_or_warn(actions, url);  // a Warning box names the URL on failure
}

// ---------------------------------------------------------------------------
// SearchOutboundAsync / SearchReturnAsync / PlanReturnFromAsync (01 §11.3, §11.6)
// ---------------------------------------------------------------------------

Task<> Controller::search_outbound()
{
    if (working_) {
        co_return;
    }
    std::optional<LegSearchRequest> request = co_await prepare(false, std::nullopt);
    if (!request) {
        co_return;
    }
    co_await run_leg(*std::move(request), Leg::outbound);
}

Task<> Controller::search_return(std::optional<Place> override_origin)
{
    if (working_) {
        co_return;
    }
    std::optional<LegSearchRequest> request = co_await prepare(true, std::move(override_origin));
    if (!request) {
        co_return;
    }
    co_await run_leg(*std::move(request), Leg::inbound);
}

Task<> Controller::plan_return_from(FlightResultPtr outbound)
{
    if (working_ || !catalog_ || !outbound) {
        co_return;
    }
    Place origin = ui::return_origin_for(*catalog_, *outbound);
    InputPanel& input = *views_.input;
    input.plan_return_window(outbound->date());  // _adjustingDates guard, then the unguarded return handlers
    log(ui::plan_return_log(*outbound, input.ret_from(), input.ret_to(), locale()));
    co_await search_return(std::move(origin));
}

// ---------------------------------------------------------------------------
// PrepareAsync (01 §11.4) and ResolveAsync (§11.5)
// ---------------------------------------------------------------------------

Task<std::optional<LegSearchRequest>> Controller::prepare(bool is_return, std::optional<Place> override_origin)
{
    InputPanel& input = *views_.input;
    const Date date_from = is_return ? input.ret_from() : input.out_from();
    const Date date_to = is_return ? input.ret_to() : input.out_to();
    if (const std::optional<std::string_view> error = ui::validate_search(date_from, date_to, input.from_text())) {
        show_error(*error);  // even in autotest [WART]
        if (*error == strings::kTypeStart) {
            input.focus_from();
        }
        co_return std::nullopt;
    }
    const AirportCatalogPtr catalog = co_await load_catalog();
    if (!catalog) {
        co_return std::nullopt;  // its status line explains why
    }

    set_working(true);
    const std::stop_token stop = begin_phase();
    views_.busy->begin(strings::kBeginFindingPlaces);
    const PhaseGuard guard{*this};
    try {
        const Resolved home = co_await resolve(input.from_text(), strings::kRoleStartingPoint, catalog, stop);
        if (!home.place) {
            fail(home.error);
            co_return std::nullopt;
        }
        std::optional<Place> destination;
        if (ui::should_resolve_destination(input.to_text(), is_return, override_origin.has_value())) {
            Resolved resolved = co_await resolve(input.to_text(), strings::kRoleDestination, catalog, stop);
            if (!resolved.place) {
                fail(resolved.error);
                co_return std::nullopt;
            }
            destination = std::move(resolved.place);
        }
        const ui::SearchInputs inputs = input.search_inputs();
        if (!is_return) {
            views_.busy->end(strings::kPlacesFound);
            co_return ui::outbound_request(*home.place, std::move(destination), inputs, date_from, date_to);
        }
        const std::optional<Place> picked =
            ui::explicit_outbound_destination(views_.outbound->explicit_selection(), *catalog);
        std::expected<LegSearchRequest, std::string> request =
            ui::return_request(*home.place, destination, override_origin, picked,
                               !views_.outbound->all_results().empty(), inputs, date_from, date_to);
        if (!request) {
            fail(request.error());
            co_return std::nullopt;
        }
        views_.busy->end(strings::kPlacesFound);
        co_return *std::move(request);
    } catch (const OperationCancelled& error) {
        if (stop.stop_requested()) {
            views_.busy->end(strings::kSearchNotStartedCancelled);  // success = true: green
        } else {
            fail(strings::place_lookup_failed(error.what()));  // a cancellation the user did not ask for
        }
    } catch (const http::TransportError& error) {
        fail(strings::place_lookup_failed(error.what()));
    } catch (const http::TimeoutError& error) {
        fail(strings::place_lookup_failed(error.what()));
    } catch (const std::ios_base::failure& error) {
        fail(strings::place_lookup_failed(error.what()));
    }
    co_return std::nullopt;
}

Task<Controller::Resolved> Controller::resolve(std::string text, std::string_view role, AirportCatalogPtr catalog,
                                               std::stop_token stop)
{
    const JobId job = jobs_->new_job_id();
    const PlaceLookup lookup = co_await jobs_->run(
        job, stop,
        [this, places = services_->places, clock = services_->clock, text, catalog](const JobChannel& channel) {
            const NoticeSink notices = [this, &channel, &clock](const ClientNotice& notice) {
                channel.post([this, report = ui::finding_places_progress(notice, clock->utc_now())] {
                    views_.busy->report(report);
                });
            };
            return places->resolve(text, *catalog, notices, channel.stop_token());
        });

    const ui::ResolveDecision decision = ui::decide_resolution(text, lookup, autotest_, *catalog);
    switch (decision.action) {
    case ui::ResolveAction::fail:
        co_return Resolved{.place = std::nullopt, .error = decision.error};
    case ui::ResolveAction::accept:
        if (decision.log) {
            log(*decision.log);
        }
        co_return Resolved{.place = lookup.candidates.front(), .error = {}};
    case ui::ResolveAction::ask:
        break;
    }

    // A direct report (not posted): marquee, no "waiting for server…" while the modal
    // chooser runs its own event loop and the panel keeps animating (07 §4).
    views_.busy->report(ui::waiting_for_choice_progress(decision.trimmed));
    const std::optional<std::size_t> chosen =
        dialogs_.choose_place ? dialogs_.choose_place(decision.trimmed, role, lookup.candidates, decision.hint)
                              : std::nullopt;
    if (const std::optional<std::string> line = ui::interpreted_after_choice(decision.trimmed, lookup, chosen)) {
        log(*line);
    }
    if (!chosen || *chosen >= lookup.candidates.size()) {
        co_return Resolved{.place = std::nullopt, .error = {}};  // "" = cancelled by the user
    }
    co_return Resolved{.place = lookup.candidates[*chosen], .error = {}};
}

void Controller::fail(std::string_view message)
{
    const ui::FailOutcome outcome = ui::prepare_failure(message, autotest_);
    views_.busy->end(outcome.busy_text, outcome.success);
    if (outcome.log) {
        log(message);
    }
    if (outcome.show_error_box) {
        show_error(message);
    }
}

// ---------------------------------------------------------------------------
// RunLegAsync (01 §11.9)
// ---------------------------------------------------------------------------

Task<> Controller::run_leg(LegSearchRequest request, Leg leg)
{
    const AirportCatalogPtr catalog = catalog_;
    const SearchPlan plan = planner::plan(*catalog, request, today(), locale());  // UI-side preview and guards
    views_.input->set_resolved_text(ui::show_resolved(request, plan, locale()));
    for (const std::string& line : ui::log_plan_lines(request, plan, locale())) {
        log(line);
    }

    FlightGrid& grid = grid_of(leg);
    const bool anywhere = !request.destination.has_value();
    if (plan.requests.empty()) {  // same area, no airport, no route, … (LIVE-3, UX-3)
        const std::string why = ui::nothing_to_search_text(plan);
        store(leg, nullptr);
        grid.clear(request.currency, anywhere, why);
        update_tab_titles();
        recompute_trips();
        select_tab(leg);
        views_.busy->end(why, false);
        co_return;
    }

    const std::size_t requests = plan.requests.size();
    switch (ui::request_budget(requests, autotest_)) {
    case ui::RequestBudget::refuse: {  // hard cap (UX-13); [WART] the previous results stay
        const std::string message = ui::request_cap_text(requests);
        views_.busy->end(message, false);
        log(message);
        if (!autotest_) {
            show_error(message);
        }
        co_return;
    }
    case ui::RequestBudget::confirm:
        if (!dialogs_.confirm_big_search || !dialogs_.confirm_big_search(to_qstring(ui::big_search_text(requests)))) {
            views_.busy->end(strings::kSearchNotStarted);  // green
            co_return;
        }
        break;
    case ui::RequestBudget::proceed:
        break;
    }

    set_working(true);
    const std::stop_token stop = begin_phase();
    const JobId job = jobs_->new_job_id();
    ui::LegStream stream{job};  // per-run `finished` flag and pending rows
    store(leg, nullptr);        // the old result of this leg is gone at once
    grid.clear(request.currency, anywhere, ui::searching_text(plan));
    update_tab_titles();
    recompute_trips();
    select_tab(leg);
    views_.busy->begin(ui::fetch_begin_text(request.label, requests));
    ui::NoticeLog notices{request.label};
    QTimer flush_timer;
    flush_timer.setInterval(ui::LegStream::kFlushInterval);
    connect(&flush_timer, &QTimer::timeout, this, [this, &stream, &grid] { flush(stream, grid); });
    // Progress reports and row batches in the order the worker produced them, one queued
    // delivery per burst (a cached replay produces hundreds within milliseconds).
    const JobMailbox<LegEvent> events{[this, job, &stream, &notices](std::vector<LegEvent> items) {
        deliver_leg_events(job, std::move(items), stream, notices);
    }};
    const PhaseGuard guard{*this, &stream};
    Q_EMIT leg_started(leg);
    try {
        flush_timer.start();
        LegSearchResultPtr result = co_await jobs_->run(
            job, stop, [search = services_->search, request, events](const JobChannel& channel) {
                const ProgressSink progress = [&channel, &events](const SearchProgress& report) {
                    events.push(channel, report);
                };
                const PartialResultsSink partial = [&channel, &events](std::vector<FlightResultPtr> batch) {
                    events.push(channel, std::move(batch));
                };
                return search->search(request, progress, partial, channel.stop_token());
            });
        flush_timer.stop();
        stream.finish();  // late batches are ignored from here on (UI-2)
        store(leg, result);
        grid.set_results(result->flights, request.currency, ui::summary(*result, locale()));
        update_tab_titles();
        for (const std::string& line : ui::log_result_lines(*result, locale())) {
            log(line);
        }
        recompute_trips();
        views_.busy->end(ui::status_line(*result), ui::status_success(*result));
    } catch (const OperationCancelled&) {  // cancelled before the fetch loop (map / FX stage)
        stream.finish();
        views_.busy->end(strings::kCancelled, false);
        log(strings::leg_cancelled_log(request.label));
    } catch (const WizzApiError& error) {
        leg_failed(request, stream, error.what());
    } catch (const http::TransportError& error) {
        leg_failed(request, stream, error.what());
    } catch (const http::TimeoutError& error) {
        leg_failed(request, stream, error.what());
    } catch (const std::ios_base::failure& error) {
        leg_failed(request, stream, error.what());
    }
}

void Controller::leg_failed(const LegSearchRequest& request, ui::LegStream& stream, std::string_view message)
{
    // [WART] rows already streamed stay, the summary still says "N fares so far…", and the
    // result is not stored, so round trips show the placeholder.
    stream.finish();
    views_.busy->end(message, false);
    log(strings::leg_error_log(request.label, message));
    if (!autotest_) {
        show_error(message);
    }
}

void Controller::deliver_leg_events(JobId job, std::vector<LegEvent> items, ui::LegStream& stream,
                                    ui::NoticeLog& notices)
{
    std::vector<SearchProgress> reports;
    for (LegEvent& item : items) {
        if (SearchProgress* report = std::get_if<SearchProgress>(&item); report != nullptr) {
            if (const std::optional<std::string> line = notices.line_for(*report)) {
                log(*line);
            }
            reports.push_back(std::move(*report));
        } else {
            stream.on_partial(job, std::get<std::vector<FlightResultPtr>>(item));
        }
    }
    views_.busy->report(reports);  // Report(p) for each, one refresh
}

void Controller::flush(ui::LegStream& stream, FlightGrid& grid)
{
    const std::vector<FlightResultPtr> batch = stream.take_pending();
    if (batch.empty()) {
        return;  // finished, or nothing arrived since the last tick
    }
    grid.append(batch);
    grid.set_summary(strings::fares_so_far(grid.all_results().size()));
    update_tab_titles();
}

// ---------------------------------------------------------------------------
// OnCurrencyChanged (01 §11.7), RecomputeTrips (§11.8), UpdateTabTitles (§5), Log (§12)
// ---------------------------------------------------------------------------

void Controller::on_currency_changed()
{
    if (working_) {
        return;
    }
    const std::string currency = views_.input->currency();
    reprice(outbound_result_, *views_.outbound, currency);
    reprice(return_result_, *views_.inbound, currency);
    recompute_trips();
}

void Controller::reprice(LegSearchResultPtr& result, FlightGrid& grid, const std::string& currency)
{
    if (!result || text::ordinal_ignore_case_equals(result->request.currency, currency)) {
        return;
    }
    result = FlightSearchService::reprice(*result, currency);  // pure: the leg's stored rates, no network
    grid.set_results(result->flights, currency, ui::summary(*result, locale()));
}

void Controller::recompute_trips()
{
    ui::TripsView view = ui::recompute_trips(outbound_result_.get(), return_result_.get(), views_.input->min_nights(),
                                             views_.input->max_nights(), views_.trips->require_same_home(),
                                             views_.trips->require_same_away(), views_.input->currency(), locale());
    views_.trips->set_trips(std::move(view.trips), view.currency, view.summary);
    update_tab_titles();
}

void Controller::update_tab_titles()
{
    QTabWidget& tabs = *views_.tabs;
    const auto set_title = [&tabs](QWidget* page, const std::string& title) {
        const int index = tabs.indexOf(page);
        const QString text = to_qstring(title);
        if (index >= 0 && tabs.tabText(index) != text) {
            tabs.setTabText(index, text);
        }
    };
    set_title(views_.outbound, strings::tab_title(strings::kTabOutbound, views_.outbound->all_results().size()));
    set_title(views_.inbound, strings::tab_title(strings::kTabReturn, views_.inbound->all_results().size()));
    set_title(views_.trips, strings::tab_title(strings::kTabRoundTrips, views_.trips->count()));
    set_title(views_.log, std::string{strings::kTabLog});
}

void Controller::log(std::string_view line)
{
    views_.log->append_line(line);
}

// ---------------------------------------------------------------------------
// Phases, SetWorking (01 §6.4, §7)
// ---------------------------------------------------------------------------

std::stop_token Controller::begin_phase()
{
    stop_source_.emplace();
    return stop_source_->get_token();
}

void Controller::end_phase() noexcept
{
    if (shutting_down_) {
        return;
    }
    stop_source_.reset();
    try {
        set_working(false);
        views_.busy->end_if_still_busy();
    } catch (...) {
        // A finally block must not throw; the panel keeps its last state.
    }
}

void Controller::set_working(bool working)
{
    const bool changed = working_ != working;
    working_ = working;
    views_.input->set_working(working);
    if (changed) {
        Q_EMIT working_changed(working);
    }
}

void Controller::store(Leg leg, LegSearchResultPtr result)
{
    (leg == Leg::outbound ? outbound_result_ : return_result_) = std::move(result);
}

void Controller::select_tab(Leg leg)
{
    views_.tabs->setCurrentWidget(&grid_of(leg));
}

void Controller::show_error(std::string_view text) const
{
    if (dialogs_.show_error) {
        dialogs_.show_error(to_qstring(text));
    }
}

FlightGrid& Controller::grid_of(Leg leg) const noexcept
{
    return leg == Leg::outbound ? *views_.outbound : *views_.inbound;
}

Date Controller::today() const
{
    return services_->clock->local_today();
}

} // namespace wizz::app
