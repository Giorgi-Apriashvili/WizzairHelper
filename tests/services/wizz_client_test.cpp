// WizzApiClient (04 §2–§13) against the fake transport: 05 §3.4 CT-API-01..14,
// 05 §3.11 CT-SRCH-03..05, 05 §7.1 P-01..P-08, P-10..P-12, P-33, P-34, P-32 at the
// client level, 05 §7.3 R-02, the 04 §11.3 classification table row by row, the
// 04 §8 route-map fallback rules and the 04 §10 error types. Unless a test says
// otherwise: the 05 §1.4 harness (spacing 0, jitter 0, recording delay, FIXED
// clock 2026-11-02T10:00:00Z with local zone UTC, cache disabled).
//
// Exception types are asserted exactly (05 §1.9): `catch_exactly<WizzApiError>`
// rejects WizzBlockedError and WizzValidationError.

#include "wizz/services/wizz_client.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/core/clocks.hpp"
#include "wizz/core/http.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/time.hpp"
#include "wizz/services/file_cache.hpp"
#include "wizz/services/wizz_errors.hpp"

#include "support/fake_transport.hpp"
#include "support/fixtures.hpp"
#include "support/recording_delay.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using wizz::AirportCatalogPtr;
using wizz::ClientNotice;
using wizz::Date;
using wizz::Duration;
using wizz::FileCache;
using wizz::FixedClock;
using wizz::NoticeSink;
using wizz::OperationCancelled;
using wizz::PriceType;
using wizz::TimetableQuery;
using wizz::TimetableResult;
using wizz::UtcInstant;
using wizz::WizzApiClient;
using wizz::WizzApiError;
using wizz::WizzBlockedError;
using wizz::WizzClientOptions;
using wizz::WizzValidationError;
using wizz::test::bare_response;
using wizz::test::broken_stream_response;
using wizz::test::default_responder;
using wizz::test::FakeTransport;
using wizz::test::home_html_for;
using wizz::test::html_response;
using wizz::test::json_response;
using wizz::test::kHomeHtml;
using wizz::test::read_fixture;
using wizz::test::RecordedRequest;
using wizz::test::RecordingDelay;
using wizz::test::Responder;
using wizz::test::ScriptedResponse;
using wizz::test::string_content_response;

using namespace std::chrono_literals;
using namespace std::string_literals;

using Delays = std::vector<Duration>;
using Strings = std::vector<std::string>;

// ---------------------------------------------------------------------------
// Constants (04 §2.3, §3.1; 05 §1.6)
// ---------------------------------------------------------------------------

constexpr std::string_view kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
constexpr std::string_view kHomepage = "https://www.wizzair.com/en-gb";
constexpr std::string_view kHomepage2 = "https://www.wizzair.com/";
constexpr std::string_view kHomepage3 = "https://www.wizzair.com/en-gb/flights/timetable";
constexpr std::string_view kApiUrl = "https://be.wizzair.com/29.23.1/Api";
constexpr std::string_view kStaleApiUrl = "https://be.wizzair.com/29.22.0/Api";
constexpr std::string_view kMapUrl = "https://be.wizzair.com/29.23.1/Api/asset/map?languageCode=en-gb";
constexpr std::string_view kTimetableUrl = "https://be.wizzair.com/29.23.1/Api/search/timetableV2";
constexpr std::string_view kStaleTimetableUrl = "https://be.wizzair.com/29.22.0/Api/search/timetableV2";

const std::string kLookingUp = "Looking up the current Wizz Air API version";
const std::string kUpdatedWebsite = "Wizz Air updated its website — finding the new API version";
const std::string kCachedMap = "Wizz Air route map unavailable — using the cached copy";
const std::string kEmptyTimetable = R"({"outboundFlights":[],"returnFlights":null})";

const UtcInstant kNow = FixedClock::kDefaultUtcNow;  // 2026-11-02T10:00:00Z

// ---------------------------------------------------------------------------
// Harness (05 §1.4)
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr Date day(int year, unsigned month, unsigned d) noexcept
{
    return Date::from_ymd(year, month, d);
}

/// `TQ(from, to, start, end)`: Regular fares, 1 adult (05 §3.4).
[[nodiscard]] TimetableQuery tq(std::string from, std::string to, Date start, Date end)
{
    return TimetableQuery{.from = std::move(from),
                          .to = std::move(to),
                          .start_date = start,
                          .end_date = end,
                          .price_type = PriceType::regular,
                          .adults = 1};
}

[[nodiscard]] bool is_homepage(const RecordedRequest& request)
{
    return request.url.starts_with("https://www.wizzair.com");
}
[[nodiscard]] bool is_timetable(const RecordedRequest& request)
{
    return request.url.contains("/search/timetableV2");
}
[[nodiscard]] bool is_map(const RecordedRequest& request)
{
    return request.url.contains("/asset/map");
}

/// `TestClients.Wizz` options (05 §1.4, §1.12).
[[nodiscard]] WizzClientOptions test_options(std::shared_ptr<const wizz::Clock> clock, const RecordingDelay& delay)
{
    WizzClientOptions options;
    options.request_spacing = Duration::zero();
    options.request_jitter = Duration::zero();
    options.delay = delay.delay_func();
    options.clock = std::move(clock);
    options.cache = FileCache::disabled();
    return options;
}

/// Thread-safe notice collector (05 §1.8 SyncProgress: a plain synchronous callback).
class NoticeLog {
public:
    [[nodiscard]] NoticeSink sink()
    {
        return [this](const ClientNotice& notice) {
            const std::scoped_lock guard{mutex_};
            notices_.push_back(notice);
        };
    }
    [[nodiscard]] std::vector<ClientNotice> notices() const
    {
        const std::scoped_lock guard{mutex_};
        return notices_;
    }
    [[nodiscard]] Strings messages() const
    {
        Strings messages;
        for (const ClientNotice& notice : notices()) {
            messages.push_back(notice.message);
        }
        return messages;
    }

private:
    mutable std::mutex mutex_;
    std::vector<ClientNotice> notices_;
};

struct ClientSetup {
    std::shared_ptr<FixedClock> clock;                   ///< nullptr = a new FixedClock()
    RecordingDelay::Hook on_delay;                       ///< runs inside each recorded delay
    std::optional<fs::path> cache_dir;                   ///< nullopt = FileCache::disabled()
    std::function<void(WizzClientOptions&)> customize;   ///< anything else
};

/// One client over one fake transport, with its delay recorder and notice log.
class Harness {
public:
    explicit Harness(Responder responder, ClientSetup setup = {})
        : clock_(setup.clock ? std::move(setup.clock) : std::make_shared<FixedClock>()),
          delay_(std::move(setup.on_delay)),
          transport_(std::make_shared<FakeTransport>(std::move(responder)))
    {
        WizzClientOptions options = test_options(clock_, delay_);
        if (setup.cache_dir) {
            options.cache = FileCache{*setup.cache_dir};
        }
        if (setup.customize) {
            setup.customize(options);
        }
        client_ = std::make_unique<WizzApiClient>(transport_, std::move(options));
    }

    [[nodiscard]] WizzApiClient& client() { return *client_; }
    [[nodiscard]] FixedClock& clock() { return *clock_; }
    [[nodiscard]] FakeTransport& transport() { return *transport_; }
    [[nodiscard]] Delays delays() const { return delay_.delays(); }
    [[nodiscard]] std::vector<ClientNotice> notices() const { return log_.notices(); }
    [[nodiscard]] Strings messages() const { return log_.messages(); }

    [[nodiscard]] TimetableResult timetable(const TimetableQuery& query, std::stop_token stop = {})
    {
        return client_->get_timetable(query, log_.sink(), std::move(stop));
    }
    [[nodiscard]] AirportCatalogPtr airports(std::stop_token stop = {}, bool force_refresh = false)
    {
        return client_->get_airports(log_.sink(), std::move(stop), force_refresh);
    }

    [[nodiscard]] std::size_t timetable_calls() const { return transport_->count_urls_containing("/search/timetableV2"); }
    [[nodiscard]] std::size_t homepage_hits() const { return transport_->count_urls_containing("https://www.wizzair.com"); }
    /// "METHOD url" of every request, in order.
    [[nodiscard]] Strings request_lines() const
    {
        Strings lines;
        for (const RecordedRequest& request : transport_->requests()) {
            lines.push_back(request.method + " " + request.url);
        }
        return lines;
    }
    /// The bodies of the timetable POSTs, in order.
    [[nodiscard]] Strings timetable_bodies() const
    {
        Strings bodies;
        for (const RecordedRequest& request : transport_->requests()) {
            if (is_timetable(request)) {
                bodies.push_back(request.body.value_or("<no body>"));
            }
        }
        return bodies;
    }

private:
    std::shared_ptr<FixedClock> clock_;
    RecordingDelay delay_;
    std::shared_ptr<FakeTransport> transport_;
    NoticeLog log_;
    std::unique_ptr<WizzApiClient> client_;
};

/// Responder: `special(request)` when it returns a response, else the default responder.
[[nodiscard]] Responder with_default(std::function<std::optional<ScriptedResponse>(const RecordedRequest&)> special,
                                     std::string_view timetable_fixture = wizz::test::kDefaultTimetableFixture)
{
    return [special = std::move(special), fallback = default_responder(timetable_fixture)](const RecordedRequest& r) {
        if (std::optional<ScriptedResponse> response = special(r)) {
            return std::move(*response);
        }
        return fallback(r);
    };
}

/// Responder: the n-th timetable call (1-based) gets `answer(n)`, everything else is default.
[[nodiscard]] Responder timetable_script(std::function<ScriptedResponse(int call)> answer)
{
    auto calls = std::make_shared<std::atomic<int>>(0);
    return with_default([calls, answer = std::move(answer)](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
        if (!is_timetable(r)) {
            return std::nullopt;
        }
        return answer(calls->fetch_add(1) + 1);
    });
}

/// Exactly `E` (typeid), like xUnit's Assert.ThrowsAsync<T> (05 §1.9). Returns a copy.
template <class E, class F>
[[nodiscard]] std::optional<E> catch_exactly(F&& action)
{
    try {
        std::forward<F>(action)();
    } catch (const std::exception& error) {
        if (typeid(error) == typeid(E)) {
            return dynamic_cast<const E&>(error);
        }
        ADD_FAILURE() << "expected exactly " << typeid(E).name() << ", got " << typeid(error).name() << ": "
                      << error.what();
        return std::nullopt;
    }
    ADD_FAILURE() << "expected " << typeid(E).name() << ", but nothing was thrown";
    return std::nullopt;
}

/// From/to of a timetableV2 body.
[[nodiscard]] std::pair<std::string, std::string> window_of(const std::string& body)
{
    const wizz::json::Value root = wizz::json::parse(body);
    const wizz::json::Value& item = root.find("flightList")->at(0);
    return {std::string{*item.get_string("from")}, std::string{*item.get_string("to")}};
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>> windows_of(const Strings& bodies)
{
    std::vector<std::pair<std::string, std::string>> windows;
    for (const std::string& body : bodies) {
        windows.push_back(window_of(body));
    }
    return windows;
}

/// The exact timetableV2 body (04 §9.3) for BUD→LTN, Regular, 1 adult.
[[nodiscard]] std::string bud_ltn_body(std::string_view from, std::string_view to)
{
    return std::format(
        R"({{"flightList":[{{"departureStation":"BUD","arrivalStation":"LTN","from":"{}","to":"{}"}}],"priceType":"regular","adultCount":1,"childCount":0,"infantCount":0}})",
        from, to);
}

/// A fresh directory `%TEMP%\wizzairhelper-test-<random>`, removed afterwards (05 §1.5).
class TempDirectory {
public:
    TempDirectory()
    {
        std::random_device entropy;
        path_ = fs::temp_directory_path() /
                std::format("wizzairhelper-test-{:08x}{:08x}{:08x}", entropy(), entropy(), entropy());
        fs::create_directories(path_);
    }
    ~TempDirectory()
    {
        std::error_code ignored;  // deletion errors are ignored (05 §1.5)
        fs::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    [[nodiscard]] fs::path file(std::string_view name) const { return path_ / name; }

private:
    fs::path path_;
};

[[nodiscard]] std::optional<std::string> read_file(const fs::path& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void write_file(const fs::path& path, std::string_view content)
{
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    ASSERT_TRUE(file.good()) << path;
}

/// The UTC instant of a file's last write, converted exactly as FileCache does
/// (FILETIME ticks since 1601-01-01), for fixed clocks placed relative to it.
[[nodiscard]] UtcInstant last_write_utc(const fs::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        throw std::runtime_error("GetFileAttributesExW failed for " + path.string());
    }
    const std::uint64_t ticks =
        (std::uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32) | data.ftLastWriteTime.dwLowDateTime;
    constexpr std::chrono::sys_days kFileTimeEpoch{std::chrono::year{1601} / std::chrono::January / 1};
    return UtcInstant{kFileTimeEpoch} + wizz::Ticks{static_cast<std::int64_t>(ticks)};
}

/// Forwards to a FakeTransport and records every request's per-attempt timeout.
/// Requests whose URL contains `stall_fragment` never get an answer: they wait out
/// `request.timeout` (cancellable) and fail with http::TimeoutError, like a server
/// that accepts the connection and then says nothing (P-10).
class StallingTransport final : public wizz::http::Transport {
public:
    StallingTransport(std::shared_ptr<FakeTransport> inner, std::string stall_fragment)
        : inner_(std::move(inner)), stall_fragment_(std::move(stall_fragment))
    {
    }

    [[nodiscard]] wizz::http::Response send(const wizz::http::Request& request, std::stop_token stop) override
    {
        {
            const std::scoped_lock guard{mutex_};
            timeouts_.push_back(request.timeout);
        }
        if (stall_fragment_.empty() || !request.url.contains(stall_fragment_)) {
            return inner_->send(request, stop);
        }
        stalled_.fetch_add(1);
        std::mutex wait_mutex;
        std::condition_variable_any never;
        std::unique_lock lock{wait_mutex};
        (void)never.wait_for(lock, stop, request.timeout, [] { return false; });
        wizz::throw_if_cancelled(stop);
        throw wizz::http::TimeoutError("The request timed out.");
    }

    [[nodiscard]] std::vector<std::chrono::milliseconds> timeouts() const
    {
        const std::scoped_lock guard{mutex_};
        return timeouts_;
    }
    [[nodiscard]] int stalled() const { return stalled_.load(); }

private:
    std::shared_ptr<FakeTransport> inner_;
    std::string stall_fragment_;
    mutable std::mutex mutex_;
    std::vector<std::chrono::milliseconds> timeouts_;
    std::atomic<int> stalled_{0};
};

// ---------------------------------------------------------------------------
// 04 §10 error types
// ---------------------------------------------------------------------------

static_assert(std::is_base_of_v<std::runtime_error, WizzApiError>);
static_assert(std::is_base_of_v<WizzApiError, WizzBlockedError>);
static_assert(std::is_base_of_v<WizzApiError, WizzValidationError>);
static_assert(!std::is_base_of_v<WizzApiError, OperationCancelled>, "cancellation is never an API error");
static_assert(!std::is_base_of_v<OperationCancelled, WizzApiError>);
static_assert(std::is_nothrow_copy_constructible_v<WizzValidationError>);

TEST(WizzErrors, Api_error_carries_message_and_optional_status)
{
    const WizzApiError plain{"x"};
    EXPECT_STREQ(plain.what(), "x");
    EXPECT_EQ(plain.status(), std::nullopt);
    EXPECT_EQ(WizzApiError("y", 502).status(), 502);
    const WizzBlockedError blocked{"z", 403};
    EXPECT_EQ(blocked.status(), 403);
}

TEST(WizzErrors, Validation_error_message_status_and_case_insensitive_has)
{
    const WizzValidationError error{{"InvalidMarket", "", "InvalidFromDate"}};
    EXPECT_STREQ(error.what(), "Wizz Air rejected the request: InvalidMarket, , InvalidFromDate");
    EXPECT_EQ(error.status(), 400);
    EXPECT_EQ(error.codes(), (Strings{"InvalidMarket", "", "InvalidFromDate"}));
    EXPECT_TRUE(error.has("invalidmarket"));
    EXPECT_TRUE(error.has("INVALIDFROMDATE"));
    EXPECT_TRUE(error.has(""));
    EXPECT_FALSE(error.has("InvalidTimeDateRange"));
    EXPECT_FALSE(error.has("InvalidMarke"));

    const WizzValidationError copy = error;  // copies share the codes
    EXPECT_EQ(copy.codes(), error.codes());
}

// ---------------------------------------------------------------------------
// 04 §4 options
// ---------------------------------------------------------------------------

TEST(WizzClient, Options_have_the_04_4_defaults)
{
    const WizzClientOptions options;
    EXPECT_EQ(options.discovery_pages, (Strings{std::string{kHomepage}, std::string{kHomepage2}, std::string{kHomepage3}}));
    EXPECT_EQ(options.request_spacing, Duration{1s});
    EXPECT_EQ(options.request_jitter, Duration{400ms});
    EXPECT_EQ(options.request_timeout, Duration{30s});
    EXPECT_EQ(options.retry_delays, (Delays{5s, 15s, 30s}));
    EXPECT_EQ(options.map_max_age, Duration{24h});
    EXPECT_EQ(options.timetable_cache_age, Duration{30min});
    EXPECT_TRUE(options.delay);
    ASSERT_NE(options.clock, nullptr);
    EXPECT_NE(dynamic_cast<const wizz::SystemClock*>(options.clock.get()), nullptr);
    ASSERT_TRUE(options.cache.directory().has_value());
    EXPECT_EQ(options.cache.directory()->filename(), "cache");
    EXPECT_EQ(options.cache.directory()->parent_path().filename(), "WizzairHelper");
    EXPECT_EQ(options.locale, wizz::Locale::en_us());
    EXPECT_EQ(WizzApiClient::kMaxDaysPerRequest, 42);
    EXPECT_EQ(WizzApiClient::kApiUrlCacheFile, "api-url.txt");
    EXPECT_EQ(WizzApiClient::kMapCacheFile, "map.json");
    EXPECT_EQ(wizz::kBrowserUserAgent, kUserAgent);
}

TEST(WizzClient, Rejects_a_missing_transport_clock_or_locale)
{
    const RecordingDelay delay;
    const auto transport = std::make_shared<FakeTransport>(default_responder());
    EXPECT_THROW((WizzApiClient{nullptr, test_options(std::make_shared<FixedClock>(), delay)}), std::invalid_argument);
    WizzClientOptions no_clock = test_options(nullptr, delay);
    EXPECT_THROW((WizzApiClient{transport, std::move(no_clock)}), std::invalid_argument);
    WizzClientOptions no_locale = test_options(std::make_shared<FixedClock>(), delay);
    no_locale.locale = nullptr;
    EXPECT_THROW((WizzApiClient{transport, std::move(no_locale)}), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// 05 §3.4 Wizz client
// ---------------------------------------------------------------------------

TEST(WizzClient, CT_API_01_Discovers_api_url_then_loads_map_and_timetable)
{
    Harness h{default_responder()};
    const AirportCatalogPtr catalog = h.airports();
    const TimetableResult result = h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)));

    // [A]
    ASSERT_NE(catalog, nullptr);
    EXPECT_EQ(catalog->all().size(), 12u);
    EXPECT_EQ(result.fares.size(), 11u);
    EXPECT_FALSE(result.route_not_served);
    EXPECT_FALSE(result.from_cache);
    EXPECT_EQ(h.client().current_api_url(), std::string{kApiUrl});
    const std::vector<RecordedRequest> requests = h.transport().requests();
    ASSERT_EQ(requests.size(), 3u);
    EXPECT_EQ(requests[1].url, kMapUrl);
    EXPECT_EQ(requests[2].method, "POST");
    EXPECT_EQ(requests[2].url, kTimetableUrl);
    ASSERT_TRUE(requests[2].content_type.has_value());
    EXPECT_TRUE(requests[2].content_type->starts_with("application/json"));
    EXPECT_EQ(requests[2].header_values("Origin"), (Strings{"https://www.wizzair.com"}));

    // [D] the discovery GET: browser headers, no Origin or Referer, no body.
    EXPECT_EQ(requests[0].method, "GET");
    EXPECT_EQ(requests[0].url, kHomepage);
    const wizz::http::Headers discovery_headers{
        {"User-Agent", std::string{kUserAgent}},
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
        {"Accept-Language", "en-GB,en;q=0.9"},
    };
    EXPECT_EQ(requests[0].headers, discovery_headers);
    EXPECT_TRUE(requests[0].header_values("Origin").empty());
    EXPECT_TRUE(requests[0].header_values("Referer").empty());
    EXPECT_EQ(requests[0].body, std::nullopt);
    EXPECT_EQ(requests[0].content_type, std::nullopt);

    // [D] both API requests: the same browser identity plus Origin and Referer.
    const wizz::http::Headers api_headers{
        {"User-Agent", std::string{kUserAgent}},
        {"Accept", "application/json, text/plain, */*"},
        {"Accept-Language", "en-GB,en;q=0.9"},
        {"Origin", "https://www.wizzair.com"},
        {"Referer", "https://www.wizzair.com/en-gb"},
    };
    EXPECT_EQ(requests[1].method, "GET");
    EXPECT_EQ(requests[1].headers, api_headers);
    EXPECT_EQ(requests[1].body, std::nullopt);
    EXPECT_EQ(requests[1].content_type, std::nullopt);
    EXPECT_EQ(requests[2].headers, api_headers);
    EXPECT_EQ(requests[2].content_type, "application/json; charset=utf-8");
    EXPECT_EQ(requests[2].body,
              R"({"flightList":[{"departureStation":"BUD","arrivalStation":"LTN","from":"2026-11-10","to":"2026-11-20"}],"priceType":"regular","adultCount":1,"childCount":0,"infantCount":0})");

    EXPECT_EQ(h.messages(), (Strings{kLookingUp}));
    EXPECT_EQ(h.notices()[0].wait_until_utc, std::nullopt);
    EXPECT_TRUE(h.delays().empty());
    EXPECT_EQ(h.client().request_count(), 3);
}

TEST(WizzClient, CT_API_02_Stale_version_404_html_triggers_rediscovery_and_retry)
{
    auto home_hits = std::make_shared<std::atomic<int>>(0);
    const std::string stale_page = read_fixture("stale_version_404.html");
    Harness h{with_default([home_hits, stale_page](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
        if (is_homepage(r)) {
            return html_response(home_hits->fetch_add(1) == 0 ? home_html_for("29.22.0") : std::string{kHomeHtml});
        }
        if (r.url.contains("/29.22.0/")) {
            return html_response(stale_page, 404);
        }
        return std::nullopt;
    })};
    const TimetableResult result = h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)));

    EXPECT_EQ(result.fares.size(), 11u);
    EXPECT_EQ(h.homepage_hits(), 2u);
    EXPECT_EQ(h.client().current_api_url(), std::string{kApiUrl});
    EXPECT_EQ(h.request_lines(), (Strings{"GET "s + std::string{kHomepage}, "POST "s + std::string{kStaleTimetableUrl},
                                          "GET "s + std::string{kHomepage}, "POST "s + std::string{kTimetableUrl}}));
    EXPECT_EQ(h.messages(), (Strings{kLookingUp, kUpdatedWebsite, kLookingUp}));
    EXPECT_TRUE(h.delays().empty());  // re-discovery does not use a retry slot
    EXPECT_EQ(h.client().request_count(), 4);
}

TEST(WizzClient, CT_API_03_Json_404_is_not_mistaken_for_a_stale_version)
{
    Harness h{with_default([](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
        if (r.url.contains("/Api/")) {
            return json_response(R"({"message":"No HTTP resource"})", 404);
        }
        return std::nullopt;
    })};
    const auto error = catch_exactly<WizzApiError>(
        [&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 10))); });
    ASSERT_TRUE(error);
    EXPECT_EQ(error->status(), 404);
    EXPECT_STREQ(error->what(), R"(Unexpected Wizz Air response (HTTP 404): {"message":"No HTTP resource"})");
    EXPECT_EQ(h.transport().request_count(), 2u);
    EXPECT_EQ(h.messages(), (Strings{kLookingUp}));  // no re-discovery
}

TEST(WizzClient, CT_API_04_InvalidMarket_means_route_not_served)
{
    const std::string invalid_market = read_fixture("error_invalid_market.json");
    Harness h{with_default([invalid_market](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
        if (r.url.contains("timetable") && !is_homepage(r)) {  // 05 §1.2.7 routing pitfall
            return json_response(invalid_market, 400);
        }
        return std::nullopt;
    })};
    const TimetableResult result = h.timetable(tq("BUD", "PRG", day(2026, 11, 10), day(2026, 11, 15)));
    EXPECT_TRUE(result.route_not_served);
    EXPECT_TRUE(result.fares.empty());
    EXPECT_FALSE(result.from_cache);
    EXPECT_EQ(h.transport().request_count(), 2u);
    EXPECT_TRUE(h.delays().empty());
}

TEST(WizzClient, CT_API_05_Transient_503_is_retried_with_a_countdown_notice)
{
    Harness h{timetable_script([](int call) {
        return call == 1 ? html_response("busy", 503) : json_response(read_fixture("timetable_v2_bud_ltn.json"));
    })};
    const TimetableResult result = h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)));

    EXPECT_EQ(result.fares.size(), 11u);
    EXPECT_EQ(h.timetable_calls(), 2u);
    // The back-off, then the gate wait: CoolDown(5 s + spacing 0) and the fixed clock never moves.
    EXPECT_EQ(h.delays(), (Delays{5s, 5s}));
    const std::vector<ClientNotice> notices = h.notices();
    ASSERT_EQ(notices.size(), 3u);
    EXPECT_EQ(notices[0], (ClientNotice{kLookingUp, std::nullopt}));
    EXPECT_EQ(notices[1], (ClientNotice{"Wizz Air is busy (HTTP 503) — retry 1 of 3", kNow + 5s}));
    EXPECT_EQ(notices[1].wait_until_utc, wizz::make_utc(day(2026, 11, 2), 10, 0, 5));
    EXPECT_EQ(notices[2], (ClientNotice{"Retrying (1 of 3)", std::nullopt}));
}

TEST(WizzClient, CT_API_06_Gives_up_after_all_retries)
{
    Harness h{timetable_script([](int) { return html_response("down", 502); })};
    const auto error =
        catch_exactly<WizzApiError>([&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(), "Wizz Air request failed: Wizz Air is busy (HTTP 502). Gave up after 4 attempts.");
    EXPECT_EQ(error->status(), std::nullopt);
    EXPECT_EQ(h.timetable_calls(), 4u);
    EXPECT_EQ(h.delays(), (Delays{5s, 15s, 30s}));  // a 502 does not cool the gate down

    const std::vector<ClientNotice> expected{
        {kLookingUp, std::nullopt},
        {"Wizz Air is busy (HTTP 502) — retry 1 of 3", kNow + 5s},
        {"Retrying (1 of 3)", std::nullopt},
        {"Wizz Air is busy (HTTP 502) — retry 2 of 3", kNow + 15s},
        {"Retrying (2 of 3)", std::nullopt},
        {"Wizz Air is busy (HTTP 502) — retry 3 of 3", kNow + 30s},
        {"Retrying (3 of 3)", std::nullopt},
    };
    EXPECT_EQ(h.notices(), expected);
}

TEST(WizzClient, CT_API_07_Kasada_429_is_reported_as_blocked_without_retry)
{
    Harness h{timetable_script([](int) { return string_content_response(429).with_header("x-kpsdk-ct", "abc"); })};
    const auto error = catch_exactly<WizzBlockedError>(
        [&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(),
                 "Wizz Air's bot protection blocked the request (HTTP 429 challenge). Wait a few minutes and try again.");
    EXPECT_EQ(error->status(), 429);
    EXPECT_EQ(h.timetable_calls(), 1u);
    EXPECT_TRUE(h.delays().empty());
    EXPECT_EQ(h.messages(), (Strings{kLookingUp}));  // no notice for a block
}

TEST(WizzClient, CT_API_07_Kasada_header_names_match_case_insensitively_and_values_do_not_count)
{
    // Upper-case header name (CT-FSS-02 uses x-kpsdk-r).
    Harness upper{timetable_script([](int) { return string_content_response(429).with_header("X-KPSDK-R", "1-AA"); })};
    EXPECT_TRUE(catch_exactly<WizzBlockedError>(
        [&] { (void)upper.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); }));
    EXPECT_EQ(upper.timetable_calls(), 1u);

    // x-kpsdk only in a header *value* (normal CORS responses): a plain 429, retried.
    Harness value_only{timetable_script([](int call) {
        return call == 1 ? string_content_response(429).with_header("Access-Control-Allow-Headers", "x-kpsdk-ct, x-kpsdk-r")
                         : json_response(read_fixture("timetable_v2_bud_ltn.json"));
    })};
    EXPECT_EQ(value_only.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))).fares.size(), 11u);
    EXPECT_EQ(value_only.timetable_calls(), 2u);
    EXPECT_EQ(value_only.delays(), (Delays{5s, 5s}));
}

TEST(WizzClient, CT_API_08_Html_403_is_reported_as_blocked)
{
    Harness h{timetable_script([](int) { return html_response("<html>Forbidden</html>", 403); })};
    const auto error = catch_exactly<WizzBlockedError>(
        [&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(), "Wizz Air refused the request (HTTP 403). Your network may be blocked; try again later.");
    EXPECT_EQ(error->status(), 403);
    EXPECT_EQ(h.timetable_calls(), 1u);
    EXPECT_TRUE(h.delays().empty());
}

TEST(WizzClient, CT_API_09_InvalidFromDate_shifts_the_window_by_one_day)
{
    Harness h{timetable_script([](int call) {
        return call == 1 ? json_response(R"({"validationCodes":["InvalidFromDate"]})", 400)
                         : json_response(read_fixture("timetable_v2_bud_ltn.json"));
    })};
    const TimetableQuery query = tq("BUD", "LTN", day(2026, 11, 2), day(2026, 11, 12));
    const TimetableResult result = h.timetable(query);

    const Strings bodies = h.timetable_bodies();
    ASSERT_EQ(bodies.size(), 2u);
    EXPECT_TRUE(bodies[0].contains(R"("from":"2026-11-02")"));
    EXPECT_TRUE(bodies[1].contains(R"("from":"2026-11-03")"));
    EXPECT_EQ(bodies[0], bud_ltn_body("2026-11-02", "2026-11-12"));
    EXPECT_EQ(bodies[1], bud_ltn_body("2026-11-03", "2026-11-12"));
    EXPECT_EQ(result.fares.size(), 11u);  // no filtering by date
    EXPECT_FALSE(result.route_not_served);
    EXPECT_TRUE(h.delays().empty());

    // The memory cache keeps the outer, unshifted key (04 §9.2).
    const TimetableResult again = h.timetable(query);
    EXPECT_TRUE(again.from_cache);
    EXPECT_EQ(again.fares.size(), 11u);
    EXPECT_EQ(h.timetable_calls(), 2u);
}

TEST(WizzClient, CT_API_10_Past_dates_are_clamped_to_today_and_long_ranges_are_chunked)
{
    Harness h{default_responder()};
    const TimetableQuery query = tq("BUD", "LTN", day(2026, 10, 1), day(2027, 1, 31));
    const TimetableResult result = h.timetable(query);

    const auto windows = windows_of(h.timetable_bodies());
    ASSERT_EQ(windows.size(), 3u);
    EXPECT_EQ(windows[0].first, "2026-11-02");  // today
    for (const auto& [from, to] : windows) {
        EXPECT_LE(days_between(*Date::parse_iso(to), *Date::parse_iso(from)), 41) << from << ".." << to;
    }
    EXPECT_EQ(windows[2].second, "2027-01-31");
    const std::vector<std::pair<std::string, std::string>> expected{
        {"2026-11-02", "2026-12-13"}, {"2026-12-14", "2027-01-24"}, {"2027-01-25", "2027-01-31"}};
    EXPECT_EQ(windows, expected);
    EXPECT_EQ(result.fares.size(), 33u);  // the fixture three times, no de-duplication
    EXPECT_FALSE(result.route_not_served);
    EXPECT_FALSE(result.from_cache);

    // Each chunk has its own cache entry; the aggregate is not cached but is all-cached.
    const TimetableResult again = h.timetable(query);
    EXPECT_TRUE(again.from_cache);
    EXPECT_EQ(again.fares.size(), 33u);
    EXPECT_EQ(h.timetable_calls(), 3u);
}

TEST(WizzClient, CT_API_11_Entirely_past_range_makes_no_request)
{
    Harness h{default_responder()};
    const TimetableResult result = h.timetable(tq("BUD", "LTN", day(2026, 10, 1), day(2026, 10, 5)));
    EXPECT_TRUE(result.fares.empty());
    EXPECT_FALSE(result.route_not_served);
    EXPECT_FALSE(result.from_cache);
    EXPECT_EQ(h.transport().request_count(), 0u);  // not even discovery
    EXPECT_EQ(h.client().request_count(), 0);

    // End before start: no request either (04 §9.1, first line).
    const TimetableResult reversed = h.timetable(tq("BUD", "LTN", day(2026, 11, 20), day(2026, 11, 10)));
    EXPECT_TRUE(reversed.fares.empty());
    EXPECT_FALSE(reversed.route_not_served);
    EXPECT_EQ(h.transport().request_count(), 0u);
    EXPECT_TRUE(h.notices().empty());
}

TEST(WizzClient, CT_API_12_Identical_queries_are_served_from_memory_cache)
{
    Harness h{default_responder()};
    const TimetableQuery query = tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20));
    EXPECT_FALSE(h.timetable(query).from_cache);
    const TimetableResult second = h.timetable(query);
    EXPECT_TRUE(second.from_cache);
    EXPECT_EQ(second.fares.size(), 11u);
    EXPECT_EQ(h.timetable_calls(), 1u);

    // The key is the whole query: price type and the raw adults count are part of it.
    TimetableQuery two_adults = query;
    two_adults.adults = 2;
    EXPECT_FALSE(h.timetable(two_adults).from_cache);
    EXPECT_EQ(h.timetable_calls(), 2u);
    TimetableQuery zero_adults = query;  // sends adultCount 1 like the original, but is another key
    zero_adults.adults = 0;
    EXPECT_FALSE(h.timetable(zero_adults).from_cache);
    TimetableQuery discount = query;
    discount.price_type = PriceType::wizz_discount_club;
    EXPECT_FALSE(h.timetable(discount).from_cache);
    // Codes compare ordinally and case-sensitively (04 §9.1).
    EXPECT_FALSE(h.timetable(tq("bud", "LTN", day(2026, 11, 10), day(2026, 11, 20))).from_cache);
    EXPECT_EQ(h.timetable_calls(), 5u);

    EXPECT_TRUE(h.timetable(query).from_cache);
    EXPECT_TRUE(h.timetable(two_adults).from_cache);
    EXPECT_EQ(h.timetable_calls(), 5u);
}

TEST(WizzClient, TimetableQuery_equality_is_field_wise_and_ordinal)
{
    const TimetableQuery query = tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20));
    EXPECT_EQ(query, tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_NE(query, tq("bud", "LTN", day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_NE(query, tq("BUD", "LGW", day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_NE(query, tq("BUD", "LTN", day(2026, 11, 11), day(2026, 11, 20)));
    EXPECT_NE(query, tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 21)));
    TimetableQuery other = query;
    other.adults = 0;
    EXPECT_NE(query, other);
    other = query;
    other.price_type = PriceType::wizz_discount_club;
    EXPECT_NE(query, other);
}

TEST(WizzClient, CT_API_13_Cancellation_propagates)
{
    Harness h{default_responder()};
    std::stop_source cancel;
    cancel.request_stop();
    EXPECT_THROW((void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 10)), cancel.get_token()),
                 OperationCancelled);
    EXPECT_EQ(h.transport().request_count(), 0u);  // checked at the top of the send loop, before discovery
    EXPECT_EQ(h.client().request_count(), 0);
}

TEST(WizzClient, CT_API_14_Api_url_and_map_are_cached_on_disk)
{
    const TempDirectory directory;
    const auto transport = std::make_shared<FakeTransport>(default_responder());
    const auto make_options = [&] {
        WizzClientOptions options;  // Clock SYSTEM (the default)
        options.request_spacing = Duration::zero();
        options.request_jitter = Duration::zero();
        options.delay = [](Duration, std::stop_token) {};  // no-op delay
        options.cache = FileCache{directory.path()};
        return options;
    };

    WizzApiClient client1{transport, make_options()};
    const AirportCatalogPtr first = client1.get_airports({}, {});
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->all().size(), 12u);
    EXPECT_EQ(transport->request_count(), 2u);  // homepage and map
    EXPECT_EQ(transport->urls(), (Strings{std::string{kHomepage}, std::string{kMapUrl}}));

    WizzApiClient client2{transport, make_options()};
    const AirportCatalogPtr second = client2.get_airports({}, {});
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->all().size(), 12u);
    EXPECT_EQ(transport->request_count(), 2u);  // both served from disk
    EXPECT_EQ(client2.request_count(), 0);
    EXPECT_EQ(client2.current_api_url(), std::nullopt);  // the map alone is enough: api-url.txt is never needed

    EXPECT_EQ(read_file(directory.file("api-url.txt")), std::string{kApiUrl});  // exactly, no newline
    EXPECT_EQ(read_file(directory.file("map.json")), read_fixture("map_subset.json"));  // verbatim
    EXPECT_FALSE(fs::exists(directory.file("api-url.txt.tmp")));
    EXPECT_FALSE(fs::exists(directory.file("map.json.tmp")));
}

// ---------------------------------------------------------------------------
// 05 §3.11 search regressions that exercise the client alone
// ---------------------------------------------------------------------------

TEST(WizzClient, CT_SRCH_03_Body_that_breaks_after_headers_is_retried_instead_of_crashing)
{
    Harness h{timetable_script([](int call) {
        return call == 1 ? broken_stream_response() : json_response(read_fixture("timetable_v2_bud_ltn.json"));
    })};
    const TimetableResult result = h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)));

    EXPECT_EQ(result.fares.size(), 11u);
    EXPECT_EQ(h.timetable_calls(), 2u);
    EXPECT_EQ(h.delays(), (Delays{5s}));  // not a 503: no cool-down
    const std::vector<ClientNotice> notices = h.notices();
    ASSERT_EQ(notices.size(), 3u);
    EXPECT_EQ(notices[0].message, kLookingUp);
    EXPECT_TRUE(notices[1].message.starts_with("network error ("));
    EXPECT_EQ(notices[1].message, "network error (Error while copying content to a stream.) — retry 1 of 3");
    EXPECT_EQ(notices[1].wait_until_utc, kNow + 5s);
    EXPECT_EQ(notices[2], (ClientNotice{"Retrying (1 of 3)", std::nullopt}));
    EXPECT_EQ(h.client().request_count(), 3);  // the failed attempt counts
}

TEST(WizzClient, CT_SRCH_04_Part_1_Malformed_200_becomes_a_WizzApiException)
{
    Harness h{timetable_script([](int) { return json_response("[1,2,3]"); })};
    const auto error =
        catch_exactly<WizzApiError>([&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(), "Wizz Air sent a timetable that could not be read: The requested operation requires an "
                                "element of type 'Object', but the target element has type 'Array'.");
    EXPECT_EQ(error->status(), std::nullopt);
    EXPECT_EQ(h.timetable_calls(), 1u);
    EXPECT_TRUE(h.delays().empty());
}

TEST(WizzClient, CT_SRCH_04_Part_2_Empty_200_is_transient_until_the_retries_run_out)
{
    Harness h{timetable_script([](int call) { return call <= 8 ? json_response("") : json_response("{}"); })};
    const auto error =
        catch_exactly<WizzApiError>([&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(error);
    EXPECT_TRUE(std::string_view{error->what()}.contains("empty response"));
    EXPECT_STREQ(error->what(), "Wizz Air request failed: Wizz Air sent an empty response. Gave up after 4 attempts.");
    EXPECT_EQ(error->status(), std::nullopt);
    EXPECT_EQ(h.timetable_calls(), 4u);  // the {} response is never reached
    EXPECT_EQ(h.delays(), (Delays{5s, 15s, 30s}));
}

TEST(WizzClient, CT_SRCH_05_InvalidFromDate_on_a_single_day_window_means_no_fares)
{
    Harness h{timetable_script([](int) { return json_response(R"({"validationCodes":["InvalidFromDate"]})", 400); })};
    const TimetableResult result = h.timetable(tq("BUD", "LTN", day(2026, 11, 2), day(2026, 11, 2)));
    EXPECT_TRUE(result.fares.empty());
    EXPECT_FALSE(result.route_not_served);
    EXPECT_EQ(h.timetable_calls(), 1u);  // start >= end: no shift
}

// ---------------------------------------------------------------------------
// 05 §7.1 core scenarios
// ---------------------------------------------------------------------------

TEST(WizzClient, P_01_InvalidTimeDateRange_halves_the_window)
{
    const std::string invalid_range = read_fixture("error_invalid_range.json");
    Harness h{timetable_script([invalid_range](int call) {
        return call == 1 ? json_response(invalid_range, 400) : json_response(kEmptyTimetable);
    })};
    const TimetableResult result = h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 12, 21)));

    const std::vector<std::pair<std::string, std::string>> expected{
        {"2026-11-10", "2026-12-21"}, {"2026-11-10", "2026-11-30"}, {"2026-12-01", "2026-12-21"}};
    EXPECT_EQ(windows_of(h.timetable_bodies()), expected);  // mid = start + floor((end - start) / 2)
    EXPECT_TRUE(result.fares.empty());
    EXPECT_FALSE(result.route_not_served);  // a AND b
    EXPECT_FALSE(result.from_cache);
}

TEST(WizzClient, P_01_Halves_concatenate_fares_and_AND_their_not_served_flags)
{
    const std::string invalid_range = read_fixture("error_invalid_range.json");
    const std::string timetable = read_fixture("timetable_v2_bud_ltn.json");
    auto first = std::make_shared<std::atomic<bool>>(true);
    Harness h{with_default([=](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
        if (!is_timetable(r)) {
            return std::nullopt;
        }
        if (first->exchange(false)) {
            return json_response(invalid_range, 400);
        }
        // The first half is not served, the second half has fares.
        return window_of(*r.body).first == "2026-11-10"
                   ? json_response(R"({"validationCodes":["InvalidMarket"]})", 400)
                   : json_response(timetable);
    })};
    const TimetableResult result = h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 12, 21)));
    EXPECT_EQ(result.fares.size(), 11u);
    EXPECT_FALSE(result.route_not_served);
    EXPECT_EQ(h.timetable_calls(), 3u);

    // Both halves not served: the whole window is not served.
    Harness both{timetable_script([invalid_range](int call) {
        return call == 1 ? json_response(invalid_range, 400)
                         : json_response(R"({"validationCodes":["InvalidMarket"]})", 400);
    })};
    EXPECT_TRUE(both.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 11))).route_not_served);
    EXPECT_EQ(windows_of(both.timetable_bodies()),
              (std::vector<std::pair<std::string, std::string>>{
                  {"2026-11-10", "2026-11-11"}, {"2026-11-10", "2026-11-10"}, {"2026-11-11", "2026-11-11"}}));
}

TEST(WizzClient, P_01_InvalidTimeDateRange_on_a_one_day_window_propagates)
{
    Harness h{timetable_script([](int) { return json_response(read_fixture("error_invalid_range.json"), 400); })};
    const auto error = catch_exactly<WizzValidationError>(
        [&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 10))); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(), "Wizz Air rejected the request: InvalidTimeDateRange");
    EXPECT_EQ(error->status(), 400);
    EXPECT_EQ(error->codes(), (Strings{"InvalidTimeDateRange"}));
    EXPECT_EQ(h.timetable_calls(), 1u);
}

TEST(WizzClient, P_02_InvalidFromDate_shifts_at_most_twice)
{
    Harness h{timetable_script([](int) { return json_response(R"({"validationCodes":["InvalidFromDate"]})", 400); })};
    const TimetableResult result = h.timetable(tq("BUD", "LTN", day(2026, 11, 2), day(2026, 11, 12)));
    EXPECT_EQ(windows_of(h.timetable_bodies()),
              (std::vector<std::pair<std::string, std::string>>{
                  {"2026-11-02", "2026-11-12"}, {"2026-11-03", "2026-11-12"}, {"2026-11-04", "2026-11-12"}}));
    EXPECT_TRUE(result.fares.empty());
    EXPECT_FALSE(result.route_not_served);
    EXPECT_TRUE(h.delays().empty());
}

TEST(WizzClient, Validation_code_precedence_and_unhandled_codes)
{
    // InvalidMarket > InvalidFromDate > InvalidTimeDateRange, compared OrdinalIgnoreCase.
    Harness all_codes{timetable_script([](int) {
        return json_response(R"({"validationCodes":["InvalidTimeDateRange","invalidfromdate","INVALIDMARKET"]})", 400);
    })};
    EXPECT_TRUE(all_codes.timetable(tq("BUD", "PRG", day(2026, 11, 10), day(2026, 11, 20))).route_not_served);
    EXPECT_EQ(all_codes.timetable_calls(), 1u);

    // Other codes propagate as exactly WizzValidationError, codes in response order, non-strings dropped.
    Harness other{timetable_script([](int) {
        return json_response(R"({"validationCodes":["MissingFlights",7,"InvalidMinAdultCount"]})", 400);
    })};
    const auto error = catch_exactly<WizzValidationError>(
        [&] { (void)other.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(), "Wizz Air rejected the request: MissingFlights, InvalidMinAdultCount");
    EXPECT_EQ(error->status(), 400);
    EXPECT_EQ(error->codes(), (Strings{"MissingFlights", "InvalidMinAdultCount"}));
    EXPECT_EQ(other.timetable_calls(), 1u);
    EXPECT_TRUE(other.delays().empty());
}

TEST(WizzClient, P_03_Plain_429_is_retried_with_a_cool_down)
{
    Harness h{timetable_script([](int) { return string_content_response(429); })};
    const auto error =
        catch_exactly<WizzApiError>([&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(), "Wizz Air request failed: Wizz Air is rate-limiting (HTTP 429). Gave up after 4 attempts.");
    EXPECT_EQ(error->status(), std::nullopt);
    EXPECT_EQ(h.delays(), (Delays{5s, 5s, 15s, 15s, 30s, 30s}));  // back-off, then the cooled-down gate
    EXPECT_EQ(h.timetable_calls(), 4u);
    EXPECT_EQ(h.notices()[1], (ClientNotice{"Wizz Air is rate-limiting (HTTP 429) — retry 1 of 3", kNow + 5s}));
}

TEST(WizzClient, P_03_Cool_down_adds_the_request_spacing)
{
    // Production spacing 1 s: a 503 cools the gate down by delay + spacing = 6 s from now.
    Harness h(timetable_script([](int call) {
                  return call == 1 ? html_response("busy", 503) : json_response(read_fixture("timetable_v2_bud_ltn.json"));
              }),
              ClientSetup{.customize = [](WizzClientOptions& o) { o.request_spacing = 1s; }});
    EXPECT_EQ(h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))).fares.size(), 11u);
    // gate wait before the first POST (1 s after the homepage), the 5 s back-off, then 6 s in the gate.
    EXPECT_EQ(h.delays(), (Delays{1s, 5s, 6s}));
}

TEST(WizzClient, P_04_408_is_transient)
{
    Harness h{timetable_script([](int) { return string_content_response(408); })};
    const auto error =
        catch_exactly<WizzApiError>([&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(), "Wizz Air request failed: Wizz Air is busy (HTTP 408). Gave up after 4 attempts.");
    EXPECT_EQ(error->status(), std::nullopt);
    EXPECT_EQ(h.delays(), (Delays{5s, 15s, 30s}));
    EXPECT_EQ(h.timetable_calls(), 4u);
}

TEST(WizzClient, P_05_200_with_html_means_blocked)
{
    Harness h{timetable_script([](int) { return html_response("<html>ok</html>"); })};
    const auto error = catch_exactly<WizzBlockedError>(
        [&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(),
                 "Wizz Air answered with a web page instead of data (probably a bot check). Try again later.");
    EXPECT_EQ(error->status(), 200);
    EXPECT_EQ(h.timetable_calls(), 1u);
}

TEST(WizzClient, P_06_400_json_without_codes_and_the_snippet)
{
    Harness a{timetable_script([](int) { return json_response(R"({"x":1})", 400); })};
    const auto rejected =
        catch_exactly<WizzApiError>([&] { (void)a.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(rejected);
    EXPECT_STREQ(rejected->what(), R"(Wizz Air rejected the request (HTTP 400): {"x":1})");
    EXPECT_EQ(rejected->status(), 400);

    const std::string long_body = R"({"message":")" + std::string(300, 'x') + R"("})";
    Harness b{timetable_script([long_body](int) { return json_response(long_body, 409); })};
    const auto unexpected =
        catch_exactly<WizzApiError>([&] { (void)b.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(unexpected);
    EXPECT_EQ(std::string{unexpected->what()},
              "Unexpected Wizz Air response (HTTP 409): " + long_body.substr(0, 200) + "…");
    EXPECT_EQ(unexpected->status(), 409);
    EXPECT_EQ(b.timetable_calls(), 1u);
}

TEST(WizzClient, P_07_An_html_404_that_persists)
{
    const std::string stale_page = read_fixture("stale_version_404.html");
    Harness h{timetable_script([stale_page](int) { return html_response(stale_page, 404); })};
    const auto error =
        catch_exactly<WizzApiError>([&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(),
                 "Wizz Air API not found at https://be.wizzair.com/29.23.1/Api even after looking up the current version.");
    EXPECT_EQ(error->status(), 404);
    EXPECT_EQ(h.request_lines(), (Strings{"GET "s + std::string{kHomepage}, "POST "s + std::string{kTimetableUrl},
                                          "GET "s + std::string{kHomepage}, "POST "s + std::string{kTimetableUrl}}));
    EXPECT_EQ(h.messages(), (Strings{kLookingUp, kUpdatedWebsite, kLookingUp}));
    EXPECT_TRUE(h.delays().empty());
}

TEST(WizzClient, P_08_Discovery_failure)
{
    Harness h{[](const RecordedRequest& r) -> ScriptedResponse {
        if (r.url == kHomepage || r.url == kHomepage2) {
            return html_response("<html></html>");
        }
        if (r.url == kHomepage3) {  // contains "timetable": route by the exact URL (05 §1.2.7)
            throw wizz::http::TransportError("boom");
        }
        throw std::logic_error("must not be called: " + r.url);
    }};
    const auto error =
        catch_exactly<WizzApiError>([&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 10))); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(),
                 "Could not find the Wizz Air API address on wizzair.com. The site may be down, blocking this network, "
                 "or showing a bot check. Details: https://www.wizzair.com/en-gb: HTTP 200, no apiUrl in page; "
                 "https://www.wizzair.com/: HTTP 200, no apiUrl in page; "
                 "https://www.wizzair.com/en-gb/flights/timetable: boom");
    EXPECT_EQ(error->status(), std::nullopt);
    EXPECT_EQ(h.transport().request_count(), 3u);  // one attempt per page, no retry
    EXPECT_EQ(h.client().request_count(), 3);
    EXPECT_EQ(h.messages(), (Strings{kLookingUp, kLookingUp, kLookingUp}));
    EXPECT_TRUE(h.delays().empty());
    EXPECT_EQ(h.client().current_api_url(), std::nullopt);
}

TEST(WizzClient, Discovery_reports_timeouts_and_ignores_the_status_of_pages_that_embed_the_config)
{
    // 04 §7.2 example message: timeout, transport error text, HTTP status.
    Harness failing{[](const RecordedRequest& r) -> ScriptedResponse {
        if (r.url == kHomepage) {
            throw wizz::http::TimeoutError("timeout");
        }
        if (r.url == kHomepage2) {
            throw wizz::http::TransportError("offline");
        }
        return html_response("<html>Forbidden</html>", 403);
    }};
    const auto error = catch_exactly<WizzApiError>([&] { (void)failing.client().get_api_url({}, {}); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(),
                 "Could not find the Wizz Air API address on wizzair.com. The site may be down, blocking this network, "
                 "or showing a bot check. Details: https://www.wizzair.com/en-gb: timed out; "
                 "https://www.wizzair.com/: offline; https://www.wizzair.com/en-gb/flights/timetable: HTTP 403, no "
                 "apiUrl in page");

    // An error page that still embeds the config counts as success (04 §7.2).
    Harness error_page{[](const RecordedRequest& r) -> ScriptedResponse {
        if (is_homepage(r)) {
            return html_response(std::string{kHomeHtml}, 404);
        }
        throw std::logic_error("must not be called");
    }};
    EXPECT_EQ(error_page.client().get_api_url({}, {}), kApiUrl);
    EXPECT_EQ(error_page.transport().request_count(), 1u);
    // Memory afterwards: no further request.
    EXPECT_EQ(error_page.client().get_api_url({}, {}), kApiUrl);
    EXPECT_EQ(error_page.transport().request_count(), 1u);
}

TEST(WizzClient, Discovery_lets_a_user_cancel_win_over_a_timeout)
{
    std::stop_source cancel;
    Harness h{[&cancel](const RecordedRequest&) -> ScriptedResponse {
        cancel.request_stop();  // the user cancels while the transfer times out
        throw wizz::http::TimeoutError("timeout");
    }};
    EXPECT_THROW((void)h.client().get_api_url({}, cancel.get_token()), OperationCancelled);
    EXPECT_EQ(h.transport().request_count(), 1u);  // no further page
}

TEST(WizzClient, P_10_A_transport_that_never_answers_times_out_and_is_retried)
{
    auto fake = std::make_shared<FakeTransport>(default_responder());
    const auto stalling = std::make_shared<StallingTransport>(fake, "/search/timetableV2");
    const RecordingDelay delay;
    WizzClientOptions options = test_options(std::make_shared<FixedClock>(), delay);
    options.request_timeout = 500ms;  // {0.5:0} prints "1" (half away from zero)
    options.retry_delays = {5s};
    WizzApiClient client{stalling, std::move(options)};
    NoticeLog log;

    const auto started = std::chrono::steady_clock::now();
    const auto error = catch_exactly<WizzApiError>(
        [&] { (void)client.get_timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)), log.sink(), {}); });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(), "Wizz Air request failed: no answer within 1 s. Gave up after 2 attempts.");
    EXPECT_EQ(error->status(), std::nullopt);
    EXPECT_EQ(stalling->stalled(), 2);
    EXPECT_EQ(delay.delays(), (Delays{5s}));
    EXPECT_EQ(log.messages(), (Strings{kLookingUp, "no answer within 1 s — retry 1 of 1", "Retrying (1 of 1)"}));
    EXPECT_GE(elapsed, 1s);  // two real 500 ms timeouts
    for (const std::chrono::milliseconds timeout : stalling->timeouts()) {
        EXPECT_EQ(timeout, 500ms);  // every attempt, discovery included, carries the per-attempt timeout
    }
}

TEST(WizzClient, Timeout_text_uses_the_request_timeout_and_every_request_carries_it)
{
    auto fake = std::make_shared<FakeTransport>(timetable_script([](int call) -> ScriptedResponse {
        if (call == 1) {
            throw wizz::http::TimeoutError("timeout");
        }
        return json_response(read_fixture("timetable_v2_bud_ltn.json"));
    }));
    const auto recording = std::make_shared<StallingTransport>(fake, "");
    const RecordingDelay delay;
    WizzApiClient client{recording, test_options(std::make_shared<FixedClock>(), delay)};
    NoticeLog log;
    EXPECT_EQ(client.get_timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)), log.sink(), {}).fares.size(),
              11u);
    EXPECT_EQ(log.messages(),
              (Strings{kLookingUp, "no answer within 30 s — retry 1 of 3", "Retrying (1 of 3)"}));
    EXPECT_EQ(delay.delays(), (Delays{5s}));  // a timeout does not cool the gate down
    EXPECT_EQ(recording->timeouts(), (std::vector<std::chrono::milliseconds>{30s, 30s, 30s}));
}

TEST(WizzClient, User_cancel_wins_over_a_timeout_and_cancels_a_stalled_transfer)
{
    // The transport reports a timeout while the user cancels: cancellation propagates, no retry.
    std::stop_source cancel;
    Harness h{timetable_script([&cancel](int) -> ScriptedResponse {
        cancel.request_stop();
        throw wizz::http::TimeoutError("timeout");
    })};
    EXPECT_THROW((void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)), cancel.get_token()),
                 OperationCancelled);
    EXPECT_EQ(h.timetable_calls(), 1u);
    EXPECT_TRUE(h.delays().empty());
    EXPECT_EQ(h.messages(), (Strings{kLookingUp}));

    // A transfer that would stall for 30 s ends at once on cancel.
    auto fake = std::make_shared<FakeTransport>(default_responder());
    const auto stalling = std::make_shared<StallingTransport>(fake, "/search/timetableV2");
    const RecordingDelay delay;
    WizzApiClient client{stalling, test_options(std::make_shared<FixedClock>(), delay)};
    std::stop_source user;
    const auto started = std::chrono::steady_clock::now();
    std::jthread canceller{[&](std::stop_token own) {
        while (stalling->stalled() == 0 && !own.stop_requested()) {
            std::this_thread::sleep_for(1ms);
        }
        user.request_stop();
    }};
    EXPECT_THROW((void)client.get_timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)), {}, user.get_token()),
                 OperationCancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
    EXPECT_TRUE(delay.delays().empty());
}

TEST(WizzClient, P_11_a_Stale_map_on_disk_is_used_after_the_retries)
{
    const TempDirectory directory;
    const std::string map = read_fixture("map_subset.json");
    write_file(directory.file("map.json"), map);
    const auto clock = std::make_shared<FixedClock>(last_write_utc(directory.file("map.json")) + 25h);

    Harness h(with_default([](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
                  if (is_map(r)) {
                      return bare_response(500);
                  }
                  return std::nullopt;
              }),
              ClientSetup{.clock = clock, .cache_dir = directory.path()});
    const AirportCatalogPtr catalog = h.airports();
    ASSERT_NE(catalog, nullptr);
    EXPECT_EQ(catalog->all().size(), 12u);
    EXPECT_EQ(h.delays(), (Delays{5s, 15s, 30s}));
    const Strings messages = h.messages();
    ASSERT_FALSE(messages.empty());
    EXPECT_EQ(messages.back(), kCachedMap);  // after the retry delays
    EXPECT_EQ(messages.size(), 8u);          // looking up, 3 × (countdown, retrying), cached copy
    EXPECT_EQ(h.notices().back().wait_until_utc, std::nullopt);
    EXPECT_EQ(read_file(directory.file("map.json")), map);  // the stale copy is not rewritten

    // Memory afterwards.
    EXPECT_EQ(h.airports(), catalog);
    EXPECT_EQ(h.transport().count_urls_containing("/asset/map"), 4u);
}

TEST(WizzClient, P_11_b_c_An_unreadable_map_without_a_cached_copy)
{
    Harness no_cities{with_default([](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
        return is_map(r) ? std::optional{json_response(R"({"x":1})")} : std::nullopt;
    })};
    const auto b = catch_exactly<WizzApiError>([&] { (void)no_cities.airports(); });
    ASSERT_TRUE(b);
    EXPECT_STREQ(b->what(), "The Wizz Air route map could not be read: Wizz route map JSON has no 'cities' array.");
    EXPECT_EQ(b->status(), std::nullopt);

    Harness no_stations{with_default([](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
        return is_map(r) ? std::optional{json_response(R"({"cities":[]})")} : std::nullopt;
    })};
    const auto c = catch_exactly<WizzApiError>([&] { (void)no_stations.airports(); });
    ASSERT_TRUE(c);
    EXPECT_STREQ(c->what(), "The Wizz Air route map could not be read: Wizz route map contains no stations.");
    EXPECT_EQ(c->status(), std::nullopt);

    // A non-object root is a shape error, wrapped the same way.
    Harness array_root{with_default([](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
        return is_map(r) ? std::optional{json_response("[1]")} : std::nullopt;
    })};
    const auto shape = catch_exactly<WizzApiError>([&] { (void)array_root.airports(); });
    ASSERT_TRUE(shape);
    EXPECT_TRUE(std::string_view{shape->what()}.starts_with("The Wizz Air route map could not be read: "));
}

TEST(WizzClient, Route_map_fallback_rules_of_04_8)
{
    const std::string map = read_fixture("map_subset.json");

    // An unreadable network map falls back to a stale copy and is not written to disk.
    {
        const TempDirectory directory;
        write_file(directory.file("map.json"), map);
        const auto clock = std::make_shared<FixedClock>(last_write_utc(directory.file("map.json")) + 48h);
        Harness h(with_default([](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
                      return is_map(r) ? std::optional{json_response(R"({"cities":[]})")} : std::nullopt;
                  }),
                  ClientSetup{.clock = clock, .cache_dir = directory.path()});
        ASSERT_NE(h.airports(), nullptr);
        EXPECT_EQ(h.messages(), (Strings{kLookingUp, kCachedMap}));
        EXPECT_EQ(read_file(directory.file("map.json")), map);
    }
    // A blocked map request falls back too (04 §8: Blocked does fall back).
    {
        const TempDirectory directory;
        write_file(directory.file("map.json"), map);
        const auto clock = std::make_shared<FixedClock>(last_write_utc(directory.file("map.json")) + 48h);
        Harness h(with_default([](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
                      return is_map(r) ? std::optional{string_content_response(429).with_header("x-kpsdk-ct", "abc")}
                                       : std::nullopt;
                  }),
                  ClientSetup{.clock = clock, .cache_dir = directory.path()});
        EXPECT_EQ(h.airports()->all().size(), 12u);
        EXPECT_EQ(h.messages(), (Strings{kLookingUp, kCachedMap}));
    }
    // Without a usable copy the API error is rethrown unchanged (type, message, status).
    {
        const TempDirectory directory;
        write_file(directory.file("map.json"), "{corrupt");  // a corrupt copy counts as absent
        Harness h(with_default([](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
                      return is_map(r) ? std::optional{string_content_response(429).with_header("x-kpsdk-ct", "abc")}
                                       : std::nullopt;
                  }),
                  ClientSetup{.cache_dir = directory.path()});
        const auto blocked = catch_exactly<WizzBlockedError>([&] { (void)h.airports(); });
        ASSERT_TRUE(blocked);
        EXPECT_EQ(blocked->status(), 429);
        EXPECT_EQ(h.messages(), (Strings{kLookingUp}));
    }
    // A fresh but corrupt map.json is skipped and replaced by the network copy.
    {
        const TempDirectory directory;
        write_file(directory.file("map.json"), "{corrupt");
        const auto clock = std::make_shared<FixedClock>(last_write_utc(directory.file("map.json")) + 1h);
        Harness h(default_responder(), ClientSetup{.clock = clock, .cache_dir = directory.path()});
        EXPECT_EQ(h.airports()->all().size(), 12u);
        EXPECT_EQ(h.transport().count_urls_containing("/asset/map"), 1u);
        EXPECT_EQ(read_file(directory.file("map.json")), map);
    }
    // A fresh map.json: no request, no API-URL lookup (warm start, 04 §8).
    {
        const TempDirectory directory;
        write_file(directory.file("map.json"), map);
        const auto clock = std::make_shared<FixedClock>(last_write_utc(directory.file("map.json")) + 24h);  // age = max age: fresh
        Harness h([](const RecordedRequest&) -> ScriptedResponse { throw std::logic_error("must not be called"); },
                  ClientSetup{.clock = clock, .cache_dir = directory.path()});
        EXPECT_EQ(h.airports()->all().size(), 12u);
        EXPECT_EQ(h.transport().request_count(), 0u);
        EXPECT_EQ(h.client().current_api_url(), std::nullopt);
        EXPECT_TRUE(h.notices().empty());
    }
}

TEST(WizzClient, Route_map_cancellation_never_falls_back_to_the_stale_copy)
{
    const TempDirectory directory;
    write_file(directory.file("map.json"), read_fixture("map_subset.json"));
    const auto clock = std::make_shared<FixedClock>(last_write_utc(directory.file("map.json")) + 48h);
    std::stop_source cancel;
    Harness h(with_default([](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
                  return is_map(r) ? std::optional{bare_response(500)} : std::nullopt;
              }),
              ClientSetup{.clock = clock,
                    .on_delay = [&cancel](Duration) { cancel.request_stop(); },  // cancel inside the back-off
                    .cache_dir = directory.path()});
    EXPECT_THROW((void)h.airports(cancel.get_token()), OperationCancelled);
    const Strings messages = h.messages();
    EXPECT_EQ(std::ranges::count(messages, kCachedMap), 0);
    EXPECT_EQ(h.transport().count_urls_containing("/asset/map"), 1u);

    // The next call (not cancelled) still works and is not poisoned by the cancel.
    EXPECT_EQ(h.airports()->all().size(), 12u);  // 500s again → stale copy
}

TEST(WizzClient, Route_map_memory_cache_and_force_refresh)
{
    Harness h{default_responder()};
    const AirportCatalogPtr first = h.airports();
    const AirportCatalogPtr second = h.airports();
    EXPECT_EQ(first, second);  // the same instance from memory
    EXPECT_EQ(h.transport().request_count(), 2u);

    const AirportCatalogPtr refreshed = h.airports({}, true);
    EXPECT_NE(refreshed, first);
    EXPECT_EQ(refreshed->all().size(), 12u);
    EXPECT_EQ(h.transport().count_urls_containing("/asset/map"), 2u);
    EXPECT_EQ(h.airports(), refreshed);
    EXPECT_EQ(h.client().request_count(), 3);
}

TEST(WizzClient, P_12_Timetable_cache_time_to_live)
{
    Harness h{default_responder()};
    const TimetableQuery query = tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20));
    EXPECT_FALSE(h.timetable(query).from_cache);
    EXPECT_EQ(h.timetable_calls(), 1u);

    h.clock().advance(29min);
    EXPECT_TRUE(h.timetable(query).from_cache);  // hit
    EXPECT_EQ(h.timetable_calls(), 1u);

    h.clock().advance(2min);  // 31 min after the fetch
    const TimetableResult refetched = h.timetable(query);
    EXPECT_FALSE(refetched.from_cache);  // miss: a new request
    EXPECT_EQ(refetched.fares.size(), 11u);
    EXPECT_EQ(h.timetable_calls(), 2u);

    // The entry is stamped after the fetch; exactly 30 min later it has expired (strict <).
    h.clock().advance(30min - wizz::Ticks{1});
    EXPECT_TRUE(h.timetable(query).from_cache);
    h.clock().advance(wizz::Ticks{1});
    EXPECT_FALSE(h.timetable(query).from_cache);
    EXPECT_EQ(h.timetable_calls(), 3u);
}

TEST(WizzClient, P_12_Not_served_results_are_cached_but_errors_are_not)
{
    auto served = std::make_shared<std::atomic<bool>>(false);
    Harness h{with_default([served](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
        if (!is_timetable(r)) {
            return std::nullopt;
        }
        if (r.body->contains("PRG")) {
            return json_response(R"({"validationCodes":["InvalidMarket"]})", 400);
        }
        return served->load() ? json_response(read_fixture("timetable_v2_bud_ltn.json"))
                              : html_response("<html>Forbidden</html>", 403);
    })};
    const TimetableQuery prg = tq("BUD", "PRG", day(2026, 11, 10), day(2026, 11, 20));
    EXPECT_TRUE(h.timetable(prg).route_not_served);
    const TimetableResult cached = h.timetable(prg);
    EXPECT_TRUE(cached.route_not_served);
    EXPECT_TRUE(cached.from_cache);
    EXPECT_EQ(h.timetable_calls(), 1u);

    const TimetableQuery ltn = tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20));
    EXPECT_TRUE(catch_exactly<WizzBlockedError>([&] { (void)h.timetable(ltn); }));
    served->store(true);
    const TimetableResult after_error = h.timetable(ltn);
    EXPECT_FALSE(after_error.from_cache);
    EXPECT_EQ(after_error.fares.size(), 11u);
    EXPECT_EQ(h.timetable_calls(), 3u);
}

TEST(WizzClient, A_not_served_chunk_stops_the_remaining_chunks)
{
    Harness h{timetable_script([](int call) {
        return call == 2 ? json_response(R"({"validationCodes":["InvalidMarket"]})", 400)
                         : json_response(read_fixture("timetable_v2_bud_ltn.json"));
    })};
    const TimetableResult result = h.timetable(tq("BUD", "LTN", day(2026, 11, 2), day(2027, 1, 31)));
    EXPECT_TRUE(result.route_not_served);
    EXPECT_EQ(result.fares.size(), 11u);  // the first chunk's fares are kept
    EXPECT_FALSE(result.from_cache);
    EXPECT_EQ(h.timetable_calls(), 2u);  // the third chunk is skipped
}

TEST(WizzClient, P_33_Discovery_lock_lets_one_cold_caller_discover)
{
    std::latch second_started{1};
    const Responder fallback = default_responder();
    Harness h{[&second_started, fallback](const RecordedRequest& r) {
        if (is_homepage(r)) {
            second_started.wait();                        // the other caller is on its way...
            std::this_thread::sleep_for(50ms);            // ...and queues on the discovery lock
        }
        return fallback(r);
    }};
    const TimetableQuery query = tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20));

    TimetableResult first_result;
    TimetableResult second_result;
    std::exception_ptr first_error;
    std::exception_ptr second_error;
    {
        std::jthread first{[&] {
            try {
                first_result = h.timetable(query);
            } catch (...) {
                first_error = std::current_exception();
            }
        }};
        std::jthread second{[&] {
            second_started.count_down();
            try {
                second_result = h.timetable(query);
            } catch (...) {
                second_error = std::current_exception();
            }
        }};
    }
    ASSERT_FALSE(first_error);
    ASSERT_FALSE(second_error);
    EXPECT_EQ(h.homepage_hits(), 1u);  // exactly one discovery
    EXPECT_EQ(first_result.fares.size(), 11u);
    EXPECT_EQ(second_result.fares.size(), 11u);
    EXPECT_EQ(h.client().current_api_url(), std::string{kApiUrl});
    EXPECT_EQ(std::ranges::count(h.messages(), kLookingUp), 1);
}

TEST(WizzClient, P_34_Stale_cached_api_url_is_replaced_after_rediscovery)
{
    const TempDirectory directory;
    write_file(directory.file("api-url.txt"), kStaleApiUrl);
    const std::string stale_page = read_fixture("stale_version_404.html");
    Harness h(with_default([stale_page](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
                  if (r.url.contains("/29.22.0/")) {
                      return html_response(stale_page, 404);
                  }
                  return std::nullopt;
              }),
              ClientSetup{.cache_dir = directory.path()});
    const TimetableResult result = h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_EQ(result.fares.size(), 11u);
    EXPECT_EQ(h.request_lines(), (Strings{"POST "s + std::string{kStaleTimetableUrl}, "GET "s + std::string{kHomepage},
                                          "POST "s + std::string{kTimetableUrl}}));
    EXPECT_EQ(h.messages(), (Strings{kUpdatedWebsite, kLookingUp}));
    EXPECT_EQ(read_file(directory.file("api-url.txt")), std::string{kApiUrl});
    EXPECT_EQ(h.client().current_api_url(), std::string{kApiUrl});
}

TEST(WizzClient, P_34_The_cached_file_is_deleted_only_while_it_holds_the_stale_url)
{
    // Another instance replaced api-url.txt meanwhile: invalidation leaves it alone
    // and the next lookup uses it without discovery.
    const TempDirectory directory;
    const fs::path url_file = directory.file("api-url.txt");
    write_file(url_file, kStaleApiUrl);
    const std::string stale_page = read_fixture("stale_version_404.html");
    Harness h(with_default([&url_file, stale_page](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
                  if (is_homepage(r)) {
                      throw std::logic_error("must not be called");
                  }
                  if (r.url.contains("/29.22.0/")) {
                      write_file(url_file, kApiUrl);
                      return html_response(stale_page, 404);
                  }
                  return std::nullopt;
              }),
              ClientSetup{.cache_dir = directory.path()});
    EXPECT_EQ(h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))).fares.size(), 11u);
    EXPECT_EQ(h.request_lines(),
              (Strings{"POST "s + std::string{kStaleTimetableUrl}, "POST "s + std::string{kTimetableUrl}}));
    EXPECT_EQ(h.messages(), (Strings{kUpdatedWebsite}));
    EXPECT_EQ(read_file(url_file), std::string{kApiUrl});

    // A padded stale value is compared trimmed, so it is deleted and rediscovered.
    const TempDirectory padded_directory;
    write_file(padded_directory.file("api-url.txt"), "  "s + std::string{kStaleApiUrl} + "\r\n");
    Harness padded(with_default([stale_page](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
                       return r.url.contains("/29.22.0/") ? std::optional{html_response(stale_page, 404)} : std::nullopt;
                   }),
                   ClientSetup{.cache_dir = padded_directory.path()});
    EXPECT_EQ(padded.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))).fares.size(), 11u);
    EXPECT_EQ(padded.homepage_hits(), 1u);
    EXPECT_EQ(read_file(padded_directory.file("api-url.txt")), std::string{kApiUrl});
}

TEST(WizzClient, Cached_api_url_must_match_the_strict_pattern_and_is_not_rewritten)
{
    // A valid value with a trailing newline is used as is and never rewritten (04 §7.1).
    {
        const TempDirectory directory;
        write_file(directory.file("api-url.txt"), std::string{kApiUrl} + "\n");
        Harness h(default_responder(), ClientSetup{.cache_dir = directory.path()});
        EXPECT_EQ(h.client().get_api_url({}, {}), kApiUrl);
        EXPECT_EQ(h.transport().request_count(), 0u);
        EXPECT_EQ(read_file(directory.file("api-url.txt")), std::string{kApiUrl} + "\n");
        EXPECT_EQ(h.client().current_api_url(), std::string{kApiUrl});
    }
    // A value that fails the strict pattern is ignored (not deleted) and overwritten by discovery.
    for (const std::string_view invalid : {"https://be.wizzair.com/29.23.1/api", "https://be.wizzair.com/29/Api",
                                           "http://be.wizzair.com/29.23.1/Api", "garbage"}) {
        const TempDirectory directory;
        write_file(directory.file("api-url.txt"), invalid);
        Harness h(default_responder(), ClientSetup{.cache_dir = directory.path()});
        EXPECT_EQ(h.client().get_api_url({}, {}), kApiUrl) << invalid;
        EXPECT_EQ(h.homepage_hits(), 1u) << invalid;
        EXPECT_EQ(read_file(directory.file("api-url.txt")), std::string{kApiUrl}) << invalid;
    }
}

TEST(WizzClient, R_02_Back_off_notice_is_reported_before_the_sleep)
{
    NoticeLog log;
    std::vector<std::optional<ClientNotice>> last_notice_at_delay;
    const RecordingDelay delay{[&](Duration) {
        const std::vector<ClientNotice> so_far = log.notices();
        last_notice_at_delay.push_back(so_far.empty() ? std::nullopt : std::optional{so_far.back()});
    }};
    const auto transport = std::make_shared<FakeTransport>(timetable_script([](int call) {
        return call == 1 ? html_response("busy", 503) : json_response(read_fixture("timetable_v2_bud_ltn.json"));
    }));
    WizzApiClient client{transport, test_options(std::make_shared<FixedClock>(), delay)};
    EXPECT_EQ(client.get_timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)), log.sink(), {}).fares.size(),
              11u);

    ASSERT_EQ(last_notice_at_delay.size(), 2u);
    // The back-off: its countdown notice (WaitUntilUtc set) is already out.
    ASSERT_TRUE(last_notice_at_delay[0]);
    EXPECT_EQ(*last_notice_at_delay[0], (ClientNotice{"Wizz Air is busy (HTTP 503) — retry 1 of 3", kNow + 5s}));
    // The gate's cool-down wait runs under "Retrying (1 of 3)", which cleared the countdown.
    ASSERT_TRUE(last_notice_at_delay[1]);
    EXPECT_EQ(*last_notice_at_delay[1], (ClientNotice{"Retrying (1 of 3)", std::nullopt}));
}

TEST(WizzClient, P_32_Cancel_during_a_back_off_propagates_at_once)
{
    std::stop_source cancel;
    Harness h(timetable_script([](int) { return html_response("busy", 503); }),
              ClientSetup{.on_delay = [&cancel](Duration) { cancel.request_stop(); }});
    EXPECT_THROW((void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)), cancel.get_token()),
                 OperationCancelled);
    EXPECT_EQ(h.timetable_calls(), 1u);
    EXPECT_EQ(h.delays(), (Delays{5s}));
    EXPECT_EQ(h.messages(), (Strings{kLookingUp, "Wizz Air is busy (HTTP 503) — retry 1 of 3"}));  // no "Retrying"
}

// ---------------------------------------------------------------------------
// 04 §11.3 classification rows not covered above
// ---------------------------------------------------------------------------

TEST(WizzClient, Classification_row_1_whitespace_body_is_an_empty_response)
{
    // Unicode white space (U+00A0 included) is empty; the JSON check comes after this row.
    Harness h{timetable_script([](int call) {
        return call == 1 ? json_response("  \r\n\t") : json_response(read_fixture("timetable_v2_bud_ltn.json"));
    })};
    EXPECT_EQ(h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))).fares.size(), 11u);
    EXPECT_EQ(h.messages()[1], "Wizz Air sent an empty response — retry 1 of 3");
    EXPECT_EQ(h.delays(), (Delays{5s}));
}

TEST(WizzClient, Classification_row_2_json_is_recognised_by_content_or_media_type)
{
    // A JSON-looking body under text/html is data (LooksLikeJson)…
    Harness html_type{timetable_script([](int) { return html_response(read_fixture("timetable_v2_bud_ltn.json")); })};
    EXPECT_EQ(html_type.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))).fares.size(), 11u);

    // …and so is any media type containing "json", case-insensitively, whatever the body.
    Harness odd_type{timetable_script([](int) {
        ScriptedResponse response = json_response(" null");
        response.response.content_type = "Application/Problem+JSON";
        return response;
    })};
    const auto error =
        catch_exactly<WizzApiError>([&] { (void)odd_type.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(error);  // returned as data, then rejected by the parser
    EXPECT_TRUE(std::string_view{error->what()}.starts_with("Wizz Air sent a timetable that could not be read: "));
}

TEST(WizzClient, Classification_row_3_any_non_json_404_is_a_stale_version)
{
    // An empty 404 without a content type is "not JSON", so it re-discovers once.
    Harness h{timetable_script([](int call) {
        return call == 1 ? bare_response(404) : json_response(read_fixture("timetable_v2_bud_ltn.json"));
    })};
    EXPECT_EQ(h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))).fares.size(), 11u);
    EXPECT_EQ(h.homepage_hits(), 2u);
    EXPECT_EQ(h.messages(), (Strings{kLookingUp, kUpdatedWebsite, kLookingUp}));
    EXPECT_EQ(h.client().request_count(), 4);
}

TEST(WizzClient, Classification_row_7_and_8_other_statuses)
{
    struct Case {
        ScriptedResponse response;
        bool blocked;
        int status;
        std::string message;
    };
    const std::vector<Case> cases{
        {html_response("<html>bad</html>", 400), true, 400,
         "Wizz Air refused the request (HTTP 400). Your network may be blocked; try again later."},
        {html_response("<html>moved</html>", 302), true, 302,
         "Wizz Air refused the request (HTTP 302). Your network may be blocked; try again later."},
        {string_content_response(401), true, 401,
         "Wizz Air refused the request (HTTP 401). Your network may be blocked; try again later."},
        {json_response(R"({"m":1})", 403), false, 403, R"(Unexpected Wizz Air response (HTTP 403): {"m":1})"},
        {json_response(R"({"m":1})", 301), false, 301, R"(Unexpected Wizz Air response (HTTP 301): {"m":1})"},
        {html_response(R"([{"m":1}])", 409), false, 409, R"(Unexpected Wizz Air response (HTTP 409): [{"m":1}])"},
    };
    for (const Case& c : cases) {
        Harness h{timetable_script([response = c.response](int) { return response; })};
        const auto run = [&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); };
        std::optional<WizzApiError> error;
        if (c.blocked) {
            error = catch_exactly<WizzBlockedError>(run);
        } else {
            error = catch_exactly<WizzApiError>(run);
        }
        ASSERT_TRUE(error) << c.message;
        EXPECT_EQ(std::string{error->what()}, c.message);
        EXPECT_EQ(error->status(), c.status) << c.message;
        EXPECT_EQ(h.timetable_calls(), 1u) << c.message;
        EXPECT_TRUE(h.delays().empty()) << c.message;
    }
}

TEST(WizzClient, Classification_row_6_server_errors_are_transient_and_only_503_cools_down)
{
    for (const int code : {500, 502, 504, 599, 408}) {
        Harness h{timetable_script([code](int call) {
            return call == 1 ? json_response(R"({"error":1})", code)
                             : json_response(read_fixture("timetable_v2_bud_ltn.json"));
        })};
        EXPECT_EQ(h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))).fares.size(), 11u) << code;
        EXPECT_EQ(h.delays(), (Delays{5s})) << code;  // no cool-down, so no extra gate wait
        EXPECT_EQ(h.messages()[1], std::format("Wizz Air is busy (HTTP {}) — retry 1 of 3", code));
    }
}

TEST(WizzClient, Raw_io_failures_are_reported_as_connection_dropped)
{
    Harness h{timetable_script([](int call) -> ScriptedResponse {
        if (call == 1) {
            throw std::ios_base::failure("The response ended prematurely.");
        }
        return json_response(read_fixture("timetable_v2_bud_ltn.json"));
    })};
    EXPECT_EQ(h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))).fares.size(), 11u);
    EXPECT_TRUE(h.messages()[1].starts_with("connection dropped (The response ended prematurely."));
    EXPECT_TRUE(h.messages()[1].ends_with(") — retry 1 of 3"));
    EXPECT_EQ(h.delays(), (Delays{5s}));
}

TEST(WizzClient, Unexpected_transport_errors_propagate_unchanged)
{
    Harness h{timetable_script([](int) -> ScriptedResponse { throw std::logic_error("must not be called"); })};
    EXPECT_THROW((void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))), std::logic_error);
    EXPECT_EQ(h.timetable_calls(), 1u);
    EXPECT_TRUE(h.delays().empty());
}

TEST(WizzClient, Rediscovery_does_not_reset_the_retry_count)
{
    // 503, then a stale 404 (re-discovery, no retry used), then 502 × 3: the retries run out
    // after 4 transient attempts in total (5 sends) — the re-discovery added one extra.
    auto calls = std::make_shared<std::atomic<int>>(0);
    Harness h{with_default([calls](const RecordedRequest& r) -> std::optional<ScriptedResponse> {
        if (!is_timetable(r)) {
            return std::nullopt;
        }
        switch (calls->fetch_add(1) + 1) {
        case 1: return html_response("busy", 503);
        case 2: return html_response("gone", 404);
        default: return html_response("down", 502);
        }
    })};
    const auto error =
        catch_exactly<WizzApiError>([&] { (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20))); });
    ASSERT_TRUE(error);
    EXPECT_STREQ(error->what(), "Wizz Air request failed: Wizz Air is busy (HTTP 502). Gave up after 4 attempts.");
    EXPECT_EQ(h.timetable_calls(), 5u);
    EXPECT_EQ(h.homepage_hits(), 2u);
    // 503: back-off 5 + cool-down gate wait 5; the 502s: 15, 30 (the gate is no longer cooled).
    EXPECT_EQ(h.delays(), (Delays{5s, 5s, 15s, 30s}));
}

TEST(WizzClient, Gate_paces_discovery_and_api_requests_together)
{
    // 04 §14.1 with the production spacing and jitter and a clock that moves with
    // each wait: homepage at t = 0, map at 1.000, timetable at 2.280.
    const auto clock = std::make_shared<FixedClock>();
    Harness h(default_responder(), ClientSetup{.clock = clock,
                                         .on_delay = [clock](Duration d) { clock->advance(d); },
                                         .customize = [](WizzClientOptions& o) {
                                             o.request_spacing = 1s;
                                             o.request_jitter = 400ms;
                                         }});
    (void)h.airports();
    (void)h.timetable(tq("BUD", "LTN", day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_EQ(h.delays(), (Delays{1000ms, 1280ms}));
    EXPECT_EQ(clock->utc_now() - kNow, Duration{2280ms});
    EXPECT_EQ(h.client().request_count(), 3);
}

} // namespace
