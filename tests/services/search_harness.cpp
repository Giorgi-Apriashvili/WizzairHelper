// Flight-search test harness (see search_harness.hpp).

#include "search_harness.hpp"

#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/json.hpp"
#include "wizz/services/file_cache.hpp"

#include "support/fixtures.hpp"

#include <format>
#include <stdexcept>
#include <utility>

namespace wizz::test::search {

namespace {

/// `flightList[0].<member>` of a timetable request body, or "" when it is not there.
[[nodiscard]] std::string flight_list_member(const RecordedRequest& request, std::string_view member)
{
    if (!request.body) {
        return {};
    }
    const json::Value root = json::parse(*request.body);
    const json::Value* list = root.find("flightList");
    if (list == nullptr || !list->is_array() || list->as_array().empty()) {
        return {};
    }
    return std::string{list->at(0).get_string(member).value_or(std::string_view{})};
}

} // namespace

bool is_timetable_request(const RecordedRequest& request)
{
    return request.url.contains("/search/timetableV2");
}

std::string departure_station(const RecordedRequest& request)
{
    return flight_list_member(request, "departureStation");
}

std::string arrival_station(const RecordedRequest& request)
{
    return flight_list_member(request, "arrivalStation");
}

Responder route_responder()
{
    Responder fallback = default_responder();
    std::string bud = read_fixture("timetable_v2_bud_ltn.json");
    return [fallback = std::move(fallback), bud = std::move(bud)](const RecordedRequest& request) {
        if (!is_timetable_request(request)) {
            return fallback(request);
        }
        const std::string from = departure_station(request);
        if (from == "BUD") {
            return json_response(bud);
        }
        if (from == "BTS") {
            return json_response(std::string{kTimetableB1});
        }
        return json_response(std::string{kInvalidMarketBody}, 400);
    };
}

Responder frankfurter_responder()
{
    return [body = read_fixture("fx_frankfurter_v2.json")](const RecordedRequest&) { return json_response(body); };
}

std::string timetable_json(std::span<const FareRow> rows)
{
    std::string body = R"({"outboundFlights":[)";
    bool first = true;
    for (const FareRow& row : rows) {
        if (!first) {
            body += ',';
        }
        first = false;
        const std::string currency =
            row.currency.empty() ? std::string{} : std::format(R"(,"currencyCode":"{}")", row.currency);
        body += std::format(R"({{"departureStation":"{}","arrivalStation":"{}","departureDate":"{}",)"
                            R"("price":{{"amount":{}{}}},"priceType":"{}","departureDates":[{{"date":"{}",)"
                            R"("isCheapestOfTheDay":true}}],"hasMacFlight":false}})",
                            row.from, row.to, row.departure, row.amount, currency,
                            row.check_on_site ? "checkPrice" : "price", row.departure);
    }
    body += R"(],"returnFlights":null})";
    return body;
}

// ---------------------------------------------------------------------------
// SearchHarness
// ---------------------------------------------------------------------------

SearchHarness::SearchHarness(Responder wizz_responder, SearchHarnessOptions options)
    : clock(options.clock ? std::move(options.clock) : std::make_shared<FixedClock>()),
      delay(std::move(options.on_delay)),
      wizz_transport(std::make_shared<FakeTransport>(std::move(wizz_responder))),
      fx_transport(std::make_shared<FakeTransport>(options.fx_responder ? std::move(options.fx_responder)
                                                                        : frankfurter_responder()))
{
    WizzClientOptions client_options;  // 05 §1.4 TestClients.Wizz
    client_options.request_spacing = Duration::zero();
    client_options.request_jitter = Duration::zero();
    client_options.delay = delay.delay_func();
    client_options.clock = clock;
    client_options.cache = FileCache::disabled();
    client_options.locale = Locale::en_us();
    if (options.customize) {
        options.customize(client_options);
    }
    wizz = std::make_shared<WizzApiClient>(wizz_transport, std::move(client_options));

    std::shared_ptr<const Clock> fx_clock = options.fx_clock ? std::move(options.fx_clock) : clock;
    fx = std::make_shared<CurrencyService>(fx_transport, FileCache::disabled(), std::move(fx_clock));

    service = std::make_unique<FlightSearchService>(wizz, fx, clock, Locale::en_us());
}

AirportCatalogPtr SearchHarness::warm_map()
{
    catalog_ = wizz->get_airports({}, {});
    return catalog_;
}

Place SearchHarness::place(std::string_view text)
{
    if (!catalog_) {
        // Places are resolved offline against CAT; build it without touching the transport.
        catalog_ = AirportCatalog::from_map_json(read_fixture("map_subset.json"), Locale::en_us());
    }
    std::optional<Place> place = catalog_->match_place(text);
    if (!place) {
        throw std::logic_error(std::format("CAT.MatchPlace(\"{}\") found nothing", text));
    }
    return *std::move(place);
}

std::vector<std::string> SearchHarness::timetable_bodies() const
{
    std::vector<std::string> bodies;
    for (const RecordedRequest& request : wizz_transport->requests()) {
        if (is_timetable_request(request)) {
            bodies.push_back(request.body.value_or(std::string{}));
        }
    }
    return bodies;
}

std::size_t SearchHarness::timetable_count() const
{
    return wizz_transport->count_urls_containing("/search/timetableV2");
}

// ---------------------------------------------------------------------------
// Collectors
// ---------------------------------------------------------------------------

ProgressSink ProgressLog::sink()
{
    return [this](const SearchProgress& progress) {
        const std::scoped_lock lock{mutex_};
        entries_.push_back(progress);
        threads_.push_back(std::this_thread::get_id());
    };
}

std::vector<SearchProgress> ProgressLog::entries() const
{
    const std::scoped_lock lock{mutex_};
    return entries_;
}

std::vector<std::thread::id> ProgressLog::threads() const
{
    const std::scoped_lock lock{mutex_};
    return threads_;
}

PartialResultsSink BatchLog::sink()
{
    return [this](std::vector<FlightResultPtr> batch) {
        const std::scoped_lock lock{mutex_};
        batches_.push_back(std::move(batch));
    };
}

std::vector<std::vector<FlightResultPtr>> BatchLog::batches() const
{
    const std::scoped_lock lock{mutex_};
    return batches_;
}

std::vector<std::size_t> BatchLog::sizes() const
{
    std::vector<std::size_t> sizes;
    for (const auto& batch : batches()) {
        sizes.push_back(batch.size());
    }
    return sizes;
}

std::size_t BatchLog::total_rows() const
{
    std::size_t rows = 0;
    for (const std::size_t size : sizes()) {
        rows += size;
    }
    return rows;
}

// ---------------------------------------------------------------------------
// Descriptions and builders
// ---------------------------------------------------------------------------

std::string describe(const SearchProgress& progress)
{
    std::string text = std::format("{} / {} / {}/{}", progress.stage, progress.detail, progress.completed, progress.total);
    if (progress.waiting_until_utc) {
        text += " wait=" + format_iso_utc(*progress.waiting_until_utc);
    }
    text += progress.step_started_utc ? " step=" + format_iso_utc(*progress.step_started_utc) : " step=unset";
    if (progress.waiting_for_user) {
        text += " user";
    }
    return text;
}

std::vector<std::string> describe(std::span<const SearchProgress> progress)
{
    std::vector<std::string> lines;
    for (const SearchProgress& entry : progress) {
        lines.push_back(describe(entry));
    }
    return lines;
}

std::string describe(const FlightResult& flight)
{
    return std::format("{}→{} {} {}", flight.fare.from, flight.fare.to, flight.fare.date.iso(),
                       format_hhmm(flight.fare.cheapest_departure));
}

std::vector<std::string> describe(std::span<const FlightResultPtr> flights)
{
    std::vector<std::string> lines;
    for (const FlightResultPtr& flight : flights) {
        lines.push_back(describe(*flight));
    }
    return lines;
}

LegSearchRequest make_request(Place origin, double origin_radius_km, std::optional<Place> destination,
                              double destination_radius_km, Date from, Date to, int adults, std::string currency)
{
    LegSearchRequest request;
    request.origin = std::move(origin);
    request.origin_radius_km = origin_radius_km;
    request.destination = std::move(destination);
    request.destination_radius_km = destination_radius_km;
    request.from = from;
    request.to = to;
    request.adults = adults;
    request.currency = std::move(currency);
    return request;
}

LegSearchResultPtr run_search(FlightSearchService& service, const LegSearchRequest& request, ProgressLog* progress,
                              BatchLog* batches, std::stop_token stop)
{
    return service
        .search_async(request, progress != nullptr ? progress->sink() : ProgressSink{},
                      batches != nullptr ? batches->sink() : PartialResultsSink{}, std::move(stop))
        .get();
}

Decimal dec(std::string_view lexeme)
{
    const auto parsed = Decimal::from_json_number(lexeme);
    if (!parsed) {
        throw std::logic_error(std::format("bad decimal literal {}", lexeme));
    }
    return *parsed;
}

} // namespace wizz::test::search
