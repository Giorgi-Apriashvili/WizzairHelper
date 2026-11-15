// CurrencyService (03 §7.2): 05 §3.5 CT-FX-05, CT-LATENT-01 end to end, the 05 §7
// scenarios P-13 (FX caches) and P-14 (the texts that belong to this service), plus
// memory staleness, single flight and cancellation. Fake transport only; caches use
// temporary directories; the clock is FIXED unless a case says SYSTEM.

#include "wizz/services/currency_service.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/core/clocks.hpp"
#include "wizz/core/fx_rates.hpp"
#include "wizz/core/json.hpp"
#include "wizz/services/file_cache.hpp"

#include "support/fake_transport.hpp"
#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <latch>
#include <memory>
#include <mutex>
#include <random>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace fx = wizz::fx_rates;
namespace http = wizz::http;
using namespace std::chrono_literals;

using wizz::ClientNotice;
using wizz::CurrencyService;
using wizz::Date;
using wizz::Decimal;
using wizz::FileCache;
using wizz::FixedClock;
using wizz::FxRates;
using wizz::FxRatesPtr;
using wizz::test::RecordedRequest;
using wizz::test::Responder;
using wizz::test::ScriptedResponse;

// --- expected texts, spelled out independently of the implementation (03 §7.2) ---------

constexpr std::string_view kFrankfurterUrl = "https://api.frankfurter.dev/v2/rates?base=EUR";
constexpr std::string_view kJsDelivrUrl =
    "https://cdn.jsdelivr.net/npm/@fawazahmed0/currency-api@latest/v1/currencies/eur.json";
constexpr std::string_view kCloudflareUrl = "https://latest.currency-api.pages.dev/v1/currencies/eur.json";

constexpr std::string_view kNoticeFrankfurter = "Downloading exchange rates (Frankfurter)";
constexpr std::string_view kNoticeJsDelivr = "Downloading exchange rates (currency-api (jsDelivr))";
constexpr std::string_view kNoticeCloudflare = "Downloading exchange rates (currency-api (Cloudflare))";
// U+2014 EM DASH written as its UTF-8 bytes, so the test pins the encoding too.
constexpr std::string_view kNoticeCached = "Exchange-rate services unreachable \xE2\x80\x94 using cached rates";
constexpr std::string_view kNoticeBuiltIn =
    "Exchange-rate services unreachable \xE2\x80\x94 using built-in approximate rates";

const wizz::UtcInstant kNow = FixedClock::kDefaultUtcNow; // 2026-11-02T10:00:00Z

// --- fixtures -------------------------------------------------------------------------

[[nodiscard]] const std::string& frankfurter_body()
{
    static const std::string body = wizz::test::read_fixture("fx_frankfurter_v2.json");
    return body;
}

[[nodiscard]] const std::string& fawaz_body()
{
    static const std::string body = wizz::test::read_fixture("fx_fawaz.json");
    return body;
}

[[nodiscard]] Decimal dec(std::string_view lexeme)
{
    const auto parsed = Decimal::from_json_number(lexeme);
    if (!parsed) {
        throw std::logic_error("bad decimal literal in a test");
    }
    return *parsed;
}

/// `body` with the first occurrence of `from` replaced by `to` (the fixture must contain it).
[[nodiscard]] std::string replace_first(std::string body, std::string_view from, std::string_view to)
{
    const auto at = body.find(from);
    if (at == std::string::npos) {
        throw std::logic_error(std::format("fixture does not contain {}", from));
    }
    body.replace(at, from.size(), to);
    return body;
}

/// A well-formed Frankfurter v2 answer with the first `count` of a fixed list of codes.
[[nodiscard]] std::string frankfurter_rows(std::size_t count)
{
    static constexpr std::array<std::string_view, 12> kCodes{"AED", "ALL", "BAM", "CHF", "CZK", "DKK",
                                                             "GBP", "GEL", "HUF", "PLN", "RON", "SEK"};
    std::string body = "[";
    for (std::size_t i = 0; i < count; ++i) {
        body += std::format(R"({}{{"date":"2026-11-02","base":"EUR","quote":"{}","rate":{}.5}})", i == 0 ? "" : ",",
                            kCodes.at(i), i + 1);
    }
    return body + "]";
}

// --- harness --------------------------------------------------------------------------

/// The fake transport of 05 §1.2 plus a copy of every http::Request as the service
/// built it, so per-request settings such as the timeout can be asserted too.
class CapturingTransport final : public http::Transport {
public:
    explicit CapturingTransport(Responder responder) : fake_(std::move(responder)) {}

    [[nodiscard]] http::Response send(const http::Request& request, std::stop_token stop) override
    {
        {
            const std::lock_guard lock(mutex_);
            sent_.push_back(request);
        }
        return fake_.send(request, std::move(stop));
    }

    [[nodiscard]] std::vector<RecordedRequest> requests() const { return fake_.requests(); }
    [[nodiscard]] std::size_t request_count() const { return fake_.request_count(); }
    [[nodiscard]] std::vector<std::string> urls() const { return fake_.urls(); }
    [[nodiscard]] std::vector<http::Request> sent() const
    {
        const std::lock_guard lock(mutex_);
        return sent_;
    }

private:
    wizz::test::FakeTransport fake_;
    mutable std::mutex mutex_;
    std::vector<http::Request> sent_;
};

/// Thread-safe `SyncProgress<ClientNotice>` collector (05 §1.8).
class NoticeLog {
public:
    [[nodiscard]] wizz::NoticeSink sink()
    {
        return [this](const ClientNotice& notice) {
            const std::lock_guard lock(mutex_);
            notices_.push_back(notice);
        };
    }
    [[nodiscard]] std::vector<ClientNotice> notices() const
    {
        const std::lock_guard lock(mutex_);
        return notices_;
    }
    [[nodiscard]] std::vector<std::string> messages() const
    {
        std::vector<std::string> texts;
        for (const ClientNotice& notice : notices()) {
            texts.push_back(notice.message);
        }
        return texts;
    }

private:
    mutable std::mutex mutex_;
    std::vector<ClientNotice> notices_;
};

/// One service over a capturing fake transport.
struct Harness {
    explicit Harness(Responder responder, FileCache cache = FileCache::disabled(),
                     std::shared_ptr<const wizz::Clock> service_clock = nullptr)
        : transport(std::make_shared<CapturingTransport>(std::move(responder))),
          clock(std::make_shared<FixedClock>()),
          service(transport, std::move(cache),
                  service_clock ? std::move(service_clock) : std::shared_ptr<const wizz::Clock>{clock})
    {
    }

    [[nodiscard]] FxRatesPtr get(std::stop_token stop = {}) { return service.get_rates(log.sink(), std::move(stop)); }

    std::shared_ptr<CapturingTransport> transport;
    std::shared_ptr<FixedClock> clock;
    NoticeLog log;
    CurrencyService service;
};

/// URLs containing `frankfurter` get a bare 500, everything else 200 fx_fawaz.json (CT-FX-05 part 1).
[[nodiscard]] Responder frankfurter_down()
{
    return [](const RecordedRequest& request) {
        return request.url.contains("frankfurter") ? wizz::test::bare_response(500)
                                                   : wizz::test::json_response(fawaz_body());
    };
}

/// Every call fails like an unplugged network (CT-FX-05 part 2).
[[nodiscard]] Responder offline()
{
    return [](const RecordedRequest&) -> ScriptedResponse { throw http::TransportError("offline"); };
}

/// Frankfurter answers with `frankfurter`, the currency-api sources with fx_fawaz.json.
[[nodiscard]] Responder frankfurter_answers(std::string frankfurter)
{
    return [body = std::move(frankfurter)](const RecordedRequest& request) {
        return request.url.contains("frankfurter") ? wizz::test::json_response(body)
                                                   : wizz::test::json_response(fawaz_body());
    };
}

/// Fails the test if any request is sent (CT-GEOC-01 style).
[[nodiscard]] Responder must_not_be_called()
{
    return [](const RecordedRequest&) -> ScriptedResponse { throw std::logic_error("must not be called"); };
}

/// A fresh directory under %TEMP% (`wizzairhelper-test-<random>`), removed afterwards
/// with I/O errors ignored (05 §1.5).
class TempDirectory {
public:
    TempDirectory()
    {
        std::random_device entropy;
        path_ = fs::temp_directory_path() / std::format("wizzairhelper-test-{:08x}{:08x}", entropy(), entropy());
        fs::create_directories(path_);
    }
    ~TempDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    [[nodiscard]] fs::path fx_json() const { return path_ / "fx.json"; }
    [[nodiscard]] FileCache cache() const { return FileCache{path_}; }

private:
    fs::path path_;
};

void write_bytes(const fs::path& path, std::string_view bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::string read_bytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// Sets the file's last-write time to an exact UTC instant (100 ns FILETIME ticks).
void set_last_write_utc(const fs::path& path, wizz::UtcInstant instant)
{
    const auto since_1601 = instant - wizz::UtcInstant{std::chrono::sys_days{std::chrono::year{1601} / 1 / 1}};
    const auto ticks = static_cast<std::uint64_t>(since_1601.count());
    const FILETIME time{.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFu),
                        .dwHighDateTime = static_cast<DWORD>(ticks >> 32)};
    const HANDLE file = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(file, INVALID_HANDLE_VALUE);
    const BOOL set = ::SetFileTime(file, nullptr, nullptr, &time);
    ::CloseHandle(file);
    ASSERT_TRUE(set);
}

/// Writes `fx.json` with `content` and a last-write time `age` before kNow.
void write_fx_json(const TempDirectory& dir, std::string_view content, wizz::Duration age)
{
    write_bytes(dir.fx_json(), content);
    set_last_write_utc(dir.fx_json(), kNow - age);
}

/// The frozen live cache file of 02 §14: Frankfurter 2026-11-02, 170 codes incl. "EUR":1.0.
[[nodiscard]] const std::string& live_fx_json()
{
    static const std::string text = wizz::test::read_live_cache("fx.json");
    return text;
}

/// Runs `call` and returns the dynamic type name of what it threw ("" if nothing).
template <class Call>
[[nodiscard]] std::string thrown_type(Call&& call)
{
    try {
        std::forward<Call>(call)();
    } catch (const std::exception& error) {
        return typeid(error).name();
    }
    return {};
}

void expect_fawaz_rates(const FxRates& rates, std::string_view source)
{
    EXPECT_EQ(rates.source, source);
    EXPECT_EQ(rates.date, Date::from_ymd(2026, 11, 1));
    EXPECT_FALSE(rates.is_approximate);
    // "contains its rates": the whole currency-api table, not Frankfurter's.
    EXPECT_EQ(rates.per_eur, fx::parse_fawaz(fawaz_body(), source).per_eur);
    EXPECT_EQ(rates.per_eur.size(), 288u);
    EXPECT_EQ(rates.per_eur.at("HUF"), dec("367.41835572"));
    EXPECT_EQ(rates.per_eur.at("GBP"), dec("0.83637627"));
    EXPECT_EQ(rates.per_eur.at("BTC"), dec("0.000014419069"));
}

// --- constants ------------------------------------------------------------------------

TEST(CurrencyService, Sources_constants_and_notice_texts_are_exact)
{
    ASSERT_EQ(CurrencyService::kSources.size(), 3u);
    EXPECT_EQ(CurrencyService::kSources[0].name, "Frankfurter");
    EXPECT_EQ(CurrencyService::kSources[0].url, kFrankfurterUrl);
    EXPECT_EQ(CurrencyService::kSources[1].name, "currency-api (jsDelivr)");
    EXPECT_EQ(CurrencyService::kSources[1].url, kJsDelivrUrl);
    EXPECT_EQ(CurrencyService::kSources[2].name, "currency-api (Cloudflare)");
    EXPECT_EQ(CurrencyService::kSources[2].url, kCloudflareUrl);

    EXPECT_EQ(CurrencyService::downloading_notice("Frankfurter"), kNoticeFrankfurter);
    EXPECT_EQ(CurrencyService::kCachedRatesNotice, kNoticeCached);
    EXPECT_EQ(CurrencyService::kBuiltInRatesNotice, kNoticeBuiltIn);

    EXPECT_EQ(CurrencyService::kCacheFileName, "fx.json");
    EXPECT_EQ(CurrencyService::kMaxAge, wizz::Duration{12h});
    EXPECT_EQ(CurrencyService::kApproximateMemoryLife, wizz::Duration{10min});
    EXPECT_EQ(CurrencyService::kSourceTimeout, 15s);
    EXPECT_EQ(CurrencyService::kMinimumRates, 10u);
    EXPECT_EQ(CurrencyService::kUserAgent, "WizzairHelper/1.0 (desktop app)");
    EXPECT_EQ(CurrencyService::kAccept, "application/json");
}

TEST(CurrencyService, Constructor_rejects_a_null_transport_or_clock)
{
    EXPECT_THROW(CurrencyService(nullptr, FileCache::disabled(), std::make_shared<FixedClock>()),
                 std::invalid_argument);
    EXPECT_THROW(CurrencyService(std::make_shared<CapturingTransport>(offline()), FileCache::disabled(), nullptr),
                 std::invalid_argument);
}

// --- CT-FX-05 — Falls_back_to_second_source_then_built_in (05 §3.5) ------------------

TEST(CurrencyService, CT_FX_05_Part1_Frankfurter_500_falls_back_to_currency_api_jsDelivr)
{
    // Clock SYSTEM, cache disabled.
    Harness h{frankfurter_down(), FileCache::disabled(), std::make_shared<wizz::SystemClock>()};

    const FxRatesPtr rates = h.get();

    ASSERT_NE(rates, nullptr);
    // [A] Source starts with currency-api, exactly 2 calls.
    EXPECT_TRUE(rates->source.starts_with("currency-api"));
    EXPECT_EQ(h.transport->request_count(), 2u);
    // [D] Source and Date.
    EXPECT_EQ(rates->source, "currency-api (jsDelivr)");
    EXPECT_EQ(rates->date, Date::from_ymd(2026, 11, 1));
    expect_fawaz_rates(*rates, "currency-api (jsDelivr)");
    // [D] The calls, in order.
    EXPECT_EQ(h.transport->urls(), (std::vector<std::string>{std::string{kFrankfurterUrl}, std::string{kJsDelivrUrl}}));
    // [D] Request headers: exactly User-Agent and Accept, in that order; plain GETs.
    for (const RecordedRequest& request : h.transport->requests()) {
        EXPECT_EQ(request.method, "GET");
        EXPECT_EQ(request.header_values("User-Agent"), std::vector<std::string>{"WizzairHelper/1.0 (desktop app)"});
        EXPECT_EQ(request.header_values("Accept"), std::vector<std::string>{"application/json"});
        EXPECT_EQ(request.headers, (http::Headers{{"User-Agent", "WizzairHelper/1.0 (desktop app)"},
                                                  {"Accept", "application/json"}}));
        EXPECT_EQ(request.body, std::nullopt);
        EXPECT_EQ(request.content_type, std::nullopt);
    }
    // 03 §7.2: 15 s per source.
    for (const http::Request& request : h.transport->sent()) {
        EXPECT_EQ(request.timeout, 15s);
    }
    // [D] Notices.
    EXPECT_EQ(h.log.messages(), (std::vector<std::string>{std::string{kNoticeFrankfurter}, std::string{kNoticeJsDelivr}}));
    for (const ClientNotice& notice : h.log.notices()) {
        EXPECT_EQ(notice.wait_until_utc, std::nullopt);
    }
}

TEST(CurrencyService, CT_FX_05_Part2_All_sources_down_returns_the_built_in_instance)
{
    // Clock SYSTEM, cache disabled; every call throws a transport error.
    Harness h{offline(), FileCache::disabled(), std::make_shared<wizz::SystemClock>()};

    const FxRatesPtr rates = h.get();

    // [A] Assert.Same(BuiltIn, rates).
    EXPECT_EQ(rates.get(), fx::built_in_rates().get());
    EXPECT_EQ(rates, fx::built_in_rates());
    EXPECT_TRUE(rates->is_approximate);
    // [D] Notices: three downloads, then the built-in fallback.
    EXPECT_EQ(h.log.messages(), (std::vector<std::string>{std::string{kNoticeFrankfurter}, std::string{kNoticeJsDelivr},
                                                          std::string{kNoticeCloudflare}, std::string{kNoticeBuiltIn}}));
    for (const ClientNotice& notice : h.log.notices()) {
        EXPECT_EQ(notice.wait_until_utc, std::nullopt);
    }
    EXPECT_EQ(h.transport->urls(), (std::vector<std::string>{std::string{kFrankfurterUrl}, std::string{kJsDelivrUrl},
                                                             std::string{kCloudflareUrl}}));
}

TEST(CurrencyService, CT_FX_05_Built_in_is_the_same_instance_on_every_fallback)
{
    // Approximate memory expires after 10 min; the next fallback must still hand out BuiltIn itself.
    Harness h{offline()};
    const FxRatesPtr first = h.get();
    h.clock->advance(10min + wizz::Ticks{1});
    const FxRatesPtr second = h.get();

    EXPECT_EQ(first.get(), fx::built_in_rates().get());
    EXPECT_EQ(second.get(), fx::built_in_rates().get());
    EXPECT_EQ(h.transport->request_count(), 6u);
}

// --- CT-LATENT-01 — malformed primary exchange rates (05 §4, 07 §4, D-13) -------------

TEST(CurrencyService, CT_LATENT_01_Frankfurter_row_missing_its_quote_uses_currency_api)
{
    const std::string malformed = replace_first(frankfurter_body(), R"("quote":"AED",)", "");
    // Still valid JSON (a "successful HTTP response"), and 169 complete rows would pass the
    // >= 10 check if the bad row were merely skipped.
    ASSERT_NO_THROW((void)wizz::json::parse(malformed));
    Harness h{frankfurter_answers(malformed)};

    const FxRatesPtr rates = h.get();

    ASSERT_NE(rates, nullptr);
    EXPECT_TRUE(rates->source.starts_with("currency-api"));
    expect_fawaz_rates(*rates, "currency-api (jsDelivr)");
    EXPECT_EQ(h.transport->urls(), (std::vector<std::string>{std::string{kFrankfurterUrl}, std::string{kJsDelivrUrl}}));
    EXPECT_EQ(h.log.messages(), (std::vector<std::string>{std::string{kNoticeFrankfurter}, std::string{kNoticeJsDelivr}}));
}

TEST(CurrencyService, CT_LATENT_01_Frankfurter_array_containing_a_string_uses_currency_api)
{
    const std::string malformed = replace_first(frankfurter_body(), "[", R"(["not a row",)");
    ASSERT_NO_THROW((void)wizz::json::parse(malformed));
    Harness h{frankfurter_answers(malformed)};

    const FxRatesPtr rates = h.get();

    ASSERT_NE(rates, nullptr);
    EXPECT_TRUE(rates->source.starts_with("currency-api"));
    expect_fawaz_rates(*rates, "currency-api (jsDelivr)");
    EXPECT_EQ(h.transport->urls(), (std::vector<std::string>{std::string{kFrankfurterUrl}, std::string{kJsDelivrUrl}}));
    EXPECT_EQ(h.log.messages(), (std::vector<std::string>{std::string{kNoticeFrankfurter}, std::string{kNoticeJsDelivr}}));
}

TEST(CurrencyService, CT_LATENT_01_Malformed_tables_and_bodies_move_on_to_the_next_source)
{
    // D-13 whole-table rejections plus bodies that are not JSON at all (JsonException).
    const std::vector<std::pair<std::string_view, std::string>> cases{
        {"rate missing", replace_first(frankfurter_body(), R"(,"rate":4.1058)", "")},
        {"rate is a string", replace_first(frankfurter_body(), R"("rate":4.1058)", R"("rate":"4.1058")")},
        {"rate does not fit a decimal", replace_first(frankfurter_body(), R"("rate":4.1058)", R"("rate":1e40)")},
        {"quote is a number", replace_first(frankfurter_body(), R"("quote":"AED")", R"("quote":7)")},
        {"quote is null", replace_first(frankfurter_body(), R"("quote":"AED")", R"("quote":null)")},
        {"element is a number", replace_first(frankfurter_body(), "[", "[42,")},
        {"root is an object", R"({"rates":[]})"},
        {"not JSON", "<html>maintenance</html>"},
        {"truncated JSON", frankfurter_body().substr(0, 100)},
        {"empty body", ""},
    };
    for (const auto& [label, body] : cases) {
        SCOPED_TRACE(label);
        Harness h{frankfurter_answers(body)};
        const FxRatesPtr rates = h.get();
        ASSERT_NE(rates, nullptr);
        expect_fawaz_rates(*rates, "currency-api (jsDelivr)");
        EXPECT_EQ(h.transport->request_count(), 2u);
    }
}

TEST(CurrencyService, Malformed_jsDelivr_table_moves_on_to_Cloudflare_with_its_own_source_name)
{
    const std::string bad_fawaz = replace_first(fawaz_body(), R"("huf": 367.41835572)", R"("huf": "367")");
    Harness h{[&](const RecordedRequest& request) {
        if (request.url.contains("frankfurter")) {
            return wizz::test::bare_response(503);
        }
        return wizz::test::json_response(request.url.contains("jsdelivr") ? bad_fawaz : fawaz_body());
    }};

    const FxRatesPtr rates = h.get();

    expect_fawaz_rates(*rates, "currency-api (Cloudflare)");
    EXPECT_EQ(h.transport->urls(), (std::vector<std::string>{std::string{kFrankfurterUrl}, std::string{kJsDelivrUrl},
                                                             std::string{kCloudflareUrl}}));
    EXPECT_EQ(h.log.messages(), (std::vector<std::string>{std::string{kNoticeFrankfurter}, std::string{kNoticeJsDelivr},
                                                          std::string{kNoticeCloudflare}}));
}

// --- the primary path and the memory cache ---------------------------------------------

TEST(CurrencyService, Frankfurter_success_is_used_with_one_request_and_one_notice)
{
    Harness h{frankfurter_answers(frankfurter_body())};

    const FxRatesPtr rates = h.get();

    ASSERT_NE(rates, nullptr);
    EXPECT_EQ(rates->source, "Frankfurter");
    EXPECT_EQ(rates->date, Date::from_ymd(2026, 11, 2));
    EXPECT_FALSE(rates->is_approximate);
    EXPECT_EQ(rates->per_eur.size(), 170u);
    EXPECT_EQ(rates->per_eur.at("EUR"), dec("1.0"));
    EXPECT_EQ(rates->per_eur.at("HUF"), dec("366.3"));
    EXPECT_EQ(h.transport->urls(), std::vector<std::string>{std::string{kFrankfurterUrl}});
    EXPECT_EQ(h.log.messages(), std::vector<std::string>{std::string{kNoticeFrankfurter}});
}

TEST(CurrencyService, Second_call_is_served_from_memory_without_HTTP_or_notices)
{
    Harness h{frankfurter_answers(frankfurter_body())};

    const FxRatesPtr first = h.get();
    const FxRatesPtr second = h.get();
    NoticeLog other_leg;
    const FxRatesPtr third = h.service.get_rates(other_leg.sink(), {});

    EXPECT_EQ(first.get(), second.get());
    EXPECT_EQ(first.get(), third.get());
    EXPECT_EQ(h.transport->request_count(), 1u);
    EXPECT_EQ(h.log.messages(), std::vector<std::string>{std::string{kNoticeFrankfurter}});
    EXPECT_TRUE(other_leg.notices().empty());
}

TEST(CurrencyService, An_empty_notice_sink_is_allowed)
{
    Harness h{offline()};
    const FxRatesPtr rates = h.service.get_rates(wizz::NoticeSink{}, {});
    EXPECT_EQ(rates.get(), fx::built_in_rates().get());
}

TEST(CurrencyService, Any_2xx_status_is_read_and_other_statuses_move_on)
{
    for (const int status : {200, 203, 299}) {
        SCOPED_TRACE(status);
        Harness h{[status](const RecordedRequest&) { return wizz::test::json_response(frankfurter_body(), status); }};
        EXPECT_EQ(h.get()->source, "Frankfurter");
        EXPECT_EQ(h.transport->request_count(), 1u);
    }
    for (const int status : {199, 300, 302, 304, 404, 429, 500}) {
        SCOPED_TRACE(status);
        // A valid table behind a non-2xx status is still not used.
        Harness h{[status](const RecordedRequest& request) {
            return request.url.contains("frankfurter") ? wizz::test::json_response(frankfurter_body(), status)
                                                       : wizz::test::json_response(fawaz_body());
        }};
        EXPECT_EQ(h.get()->source, "currency-api (jsDelivr)");
        EXPECT_EQ(h.transport->request_count(), 2u);
    }
}

TEST(CurrencyService, Timeouts_and_transport_errors_move_on_to_the_next_source)
{
    // 03 §7.2: a per-source timeout (the user did not cancel) and HttpRequestException.
    Harness timeout{[](const RecordedRequest& request) -> ScriptedResponse {
        if (request.url.contains("frankfurter")) {
            throw http::TimeoutError("no answer within 15 s");
        }
        return wizz::test::json_response(fawaz_body());
    }};
    EXPECT_EQ(timeout.get()->source, "currency-api (jsDelivr)");
    EXPECT_EQ(timeout.transport->request_count(), 2u);

    // A body that breaks after the headers arrived.
    Harness broken{[](const RecordedRequest& request) {
        return request.url.contains("frankfurter") ? wizz::test::broken_stream_response()
                                                   : wizz::test::json_response(fawaz_body());
    }};
    EXPECT_EQ(broken.get()->source, "currency-api (jsDelivr)");
    EXPECT_EQ(broken.transport->request_count(), 2u);
}

TEST(CurrencyService, Body_is_decoded_like_ReadAsStringAsync)
{
    // A UTF-8 BOM is stripped before parsing (04 §2.2), so the table is usable.
    Harness h{[](const RecordedRequest&) { return wizz::test::json_response("\xEF\xBB\xBF" + frankfurter_body()); }};
    EXPECT_EQ(h.get()->source, "Frankfurter");
    EXPECT_EQ(h.transport->request_count(), 1u);
}

// --- P-13 — FX caches (05 §7.1), real temporary cache directory, FIXED clock ------------

TEST(CurrencyService, P_13a_Fresh_fx_json_gives_no_HTTP)
{
    const TempDirectory dir;
    write_fx_json(dir, live_fx_json(), 1h);
    Harness h{must_not_be_called(), dir.cache()};

    const FxRatesPtr rates = h.get();

    EXPECT_EQ(h.transport->request_count(), 0u);
    EXPECT_TRUE(h.log.notices().empty());
    ASSERT_NE(rates, nullptr);
    EXPECT_EQ(rates->source, "Frankfurter");
    EXPECT_EQ(rates->date, Date::from_ymd(2026, 11, 2));
    EXPECT_FALSE(rates->is_approximate);
    EXPECT_EQ(rates->per_eur.size(), 170u);
    EXPECT_EQ(rates->per_eur.at("EUR"), dec("1.0"));
    EXPECT_EQ(rates->per_eur.at("HUF"), dec("366.28"));
    EXPECT_EQ(rates->per_eur, fx::parse_rates_cache(live_fx_json()).per_eur);
    // Reading never rewrites the file.
    EXPECT_EQ(read_bytes(dir.fx_json()), live_fx_json());
}

TEST(CurrencyService, P_13a_Disk_freshness_boundary_is_12_hours_inclusive)
{
    {
        const TempDirectory dir;
        write_fx_json(dir, live_fx_json(), 12h); // age == MaxAge: still fresh
        Harness h{must_not_be_called(), dir.cache()};
        EXPECT_EQ(h.get()->per_eur.size(), 170u);
        EXPECT_EQ(h.transport->request_count(), 0u);
    }
    {
        const TempDirectory dir;
        write_fx_json(dir, live_fx_json(), 12h + wizz::Ticks{1}); // one tick older: go online
        Harness h{frankfurter_answers(frankfurter_body()), dir.cache()};
        const FxRatesPtr rates = h.get();
        EXPECT_EQ(h.transport->request_count(), 1u);
        EXPECT_EQ(rates->per_eur.at("HUF"), dec("366.3")); // the network table, not the cache's 366.28
    }
}

TEST(CurrencyService, P_13b_Thirteen_hour_old_fx_json_with_every_source_down_is_used_as_approximate)
{
    const TempDirectory dir;
    write_fx_json(dir, live_fx_json(), 13h);
    Harness h{offline(), dir.cache()};

    const FxRatesPtr rates = h.get();

    ASSERT_NE(rates, nullptr);
    EXPECT_NE(rates.get(), fx::built_in_rates().get());
    EXPECT_TRUE(rates->is_approximate);
    EXPECT_EQ(rates->source, "Frankfurter");
    EXPECT_EQ(rates->date, Date::from_ymd(2026, 11, 2));
    EXPECT_EQ(rates->per_eur, fx::parse_rates_cache(live_fx_json()).per_eur);
    EXPECT_EQ(h.log.messages(), (std::vector<std::string>{std::string{kNoticeFrankfurter}, std::string{kNoticeJsDelivr},
                                                          std::string{kNoticeCloudflare}, std::string{kNoticeCached}}));
    EXPECT_EQ(h.transport->request_count(), 3u);
    // The stale file is left as it was.
    EXPECT_EQ(read_bytes(dir.fx_json()), live_fx_json());
}

TEST(CurrencyService, P_13b_A_stale_cache_of_any_age_beats_the_built_in_table)
{
    const TempDirectory dir;
    write_fx_json(dir, R"({"Date":"2025-01-15","PerEur":{"GBP":0.84,"HUF":400}})", 400 * 24h);
    Harness h{offline(), dir.cache()};

    const FxRatesPtr rates = h.get();

    EXPECT_TRUE(rates->is_approximate);
    EXPECT_EQ(rates->source, "cache"); // Source missing in the file
    EXPECT_EQ(rates->date, Date::from_ymd(2025, 1, 15));
    EXPECT_EQ(rates->per_eur.size(), 2u);
    EXPECT_EQ(h.log.messages().back(), kNoticeCached);
}

TEST(CurrencyService, P_13b_An_unreadable_cache_is_no_cache)
{
    // 03 §7.2: any JSON / format / key / invalid-operation error means no cache.
    const std::vector<std::pair<std::string_view, std::string_view>> cases{
        {"null", "null"},
        {"no rates", R"({"Date":"2026-11-02","Source":"Frankfurter","PerEur":{}})"},
        {"no date", R"({"Source":"Frankfurter","PerEur":{"GBP":0.84}})"},
        {"bad date", R"({"Date":"02.11.2026","PerEur":{"GBP":0.84}})"},
        {"wrong case", R"({"date":"2026-11-02","perEur":{"GBP":0.84}})"},
        {"not JSON", "garbage"},
        {"rate is a string", R"({"Date":"2026-11-02","PerEur":{"GBP":"0.84"}})"},
    };
    for (const auto& [label, content] : cases) {
        SCOPED_TRACE(label);
        {
            // Stale and unreadable: built-in table.
            const TempDirectory dir;
            write_fx_json(dir, content, 13h);
            Harness h{offline(), dir.cache()};
            EXPECT_EQ(h.get().get(), fx::built_in_rates().get());
            EXPECT_EQ(h.log.messages().back(), kNoticeBuiltIn);
        }
        {
            // Fresh but unreadable: the network is asked and the file is replaced.
            const TempDirectory dir;
            write_fx_json(dir, content, 1h);
            Harness h{frankfurter_answers(frankfurter_body()), dir.cache()};
            const FxRatesPtr rates = h.get();
            EXPECT_EQ(rates->source, "Frankfurter");
            EXPECT_EQ(h.transport->request_count(), 1u);
            EXPECT_EQ(read_bytes(dir.fx_json()), fx::serialize_rates_cache(*rates));
        }
    }
}

TEST(CurrencyService, P_13b_A_fresh_cache_with_few_rates_is_still_used)
{
    // 03 §7.2 rejects only a missing/empty PerEur on load; the >= 10 rule is for sources.
    const TempDirectory dir;
    write_fx_json(dir, R"({"Date":"2026-11-02","Source":"Frankfurter","PerEur":{"GBP":0.84}})", 2h);
    Harness h{must_not_be_called(), dir.cache()};

    const FxRatesPtr rates = h.get();

    EXPECT_EQ(rates->per_eur.size(), 1u);
    EXPECT_FALSE(rates->is_approximate);
    EXPECT_EQ(h.transport->request_count(), 0u);
}

TEST(CurrencyService, P_13c_Frankfurter_with_fewer_than_10_rates_moves_on_to_the_next_source)
{
    const TempDirectory dir;
    const std::string nine = frankfurter_rows(9);
    ASSERT_EQ(fx::parse_frankfurter_v2(nine).per_eur.size(), 9u);
    Harness h{frankfurter_answers(nine), dir.cache()};

    const FxRatesPtr rates = h.get();

    expect_fawaz_rates(*rates, "currency-api (jsDelivr)");
    EXPECT_EQ(h.transport->urls(), (std::vector<std::string>{std::string{kFrankfurterUrl}, std::string{kJsDelivrUrl}}));
    EXPECT_EQ(h.log.messages(), (std::vector<std::string>{std::string{kNoticeFrankfurter}, std::string{kNoticeJsDelivr}}));
    // Only the usable table is written to disk.
    EXPECT_EQ(read_bytes(dir.fx_json()), fx::serialize_rates_cache(*rates));
}

TEST(CurrencyService, P_13c_Exactly_10_rates_are_enough)
{
    const std::string ten = frankfurter_rows(10);
    Harness h{frankfurter_answers(ten)};

    const FxRatesPtr rates = h.get();

    EXPECT_EQ(rates->source, "Frankfurter");
    EXPECT_EQ(rates->per_eur.size(), 10u);
    EXPECT_EQ(h.transport->request_count(), 1u);
}

TEST(CurrencyService, P_13c_A_small_currency_api_table_moves_on_too)
{
    const std::string small_fawaz = R"({"date":"2026-11-01","eur":{"gbp":0.84,"huf":367,"eur":1}})";
    Harness h{[&](const RecordedRequest& request) {
        if (request.url.contains("frankfurter")) {
            return wizz::test::json_response(frankfurter_rows(3));
        }
        return wizz::test::json_response(request.url.contains("jsdelivr") ? small_fawaz : fawaz_body());
    }};

    expect_fawaz_rates(*h.get(), "currency-api (Cloudflare)");
    EXPECT_EQ(h.transport->request_count(), 3u);
}

TEST(CurrencyService, P_13d_Approximate_rates_in_memory_are_refetched_after_more_than_10_minutes)
{
    std::atomic<bool> online{false};
    Harness h{[&](const RecordedRequest&) {
        if (!online.load()) {
            throw http::TransportError("offline");
        }
        return wizz::test::json_response(frankfurter_body());
    }};

    const FxRatesPtr built_in = h.get();
    ASSERT_EQ(built_in.get(), fx::built_in_rates().get());
    ASSERT_EQ(h.transport->request_count(), 3u);
    online = true;

    h.clock->advance(10min); // exactly 10 min: not stale (strict >)
    EXPECT_EQ(h.get().get(), built_in.get());
    EXPECT_EQ(h.transport->request_count(), 3u);
    EXPECT_EQ(h.log.notices().size(), 4u);

    h.clock->advance(wizz::Ticks{1}); // more than 10 min: fetched again
    const FxRatesPtr refreshed = h.get();
    EXPECT_EQ(refreshed->source, "Frankfurter");
    EXPECT_FALSE(refreshed->is_approximate);
    EXPECT_EQ(h.transport->request_count(), 4u);
    EXPECT_EQ(h.log.messages().back(), kNoticeFrankfurter);
}

TEST(CurrencyService, P_13d_A_stale_disk_copy_is_also_refetched_after_more_than_10_minutes)
{
    const TempDirectory dir;
    write_fx_json(dir, live_fx_json(), 13h);
    Harness h{offline(), dir.cache()};

    const FxRatesPtr stale = h.get();
    ASSERT_TRUE(stale->is_approximate);
    h.clock->advance(10min);
    EXPECT_EQ(h.get().get(), stale.get());
    EXPECT_EQ(h.transport->request_count(), 3u);

    h.clock->advance(wizz::Ticks{1});
    const FxRatesPtr again = h.get();
    EXPECT_NE(again.get(), stale.get());
    EXPECT_TRUE(again->is_approximate);
    EXPECT_EQ(h.transport->request_count(), 6u);
}

TEST(CurrencyService, Network_rates_live_12_hours_in_memory_strictly)
{
    Harness h{frankfurter_answers(frankfurter_body())};

    const FxRatesPtr first = h.get();
    h.clock->advance(12h);
    EXPECT_EQ(h.get().get(), first.get());
    EXPECT_EQ(h.transport->request_count(), 1u);

    h.clock->advance(wizz::Ticks{1});
    const FxRatesPtr second = h.get();
    EXPECT_NE(second.get(), first.get());
    EXPECT_EQ(h.transport->request_count(), 2u);
}

TEST(CurrencyService, Memory_age_is_measured_from_the_load_time_not_the_file_age)
{
    // 03 §7.2 note: an 11 h old file is trusted for another 12 h in memory (~24 h in all).
    const TempDirectory dir;
    write_fx_json(dir, live_fx_json(), 11h);
    Harness h{frankfurter_answers(frankfurter_body()), dir.cache()};

    const FxRatesPtr loaded = h.get();
    ASSERT_EQ(h.transport->request_count(), 0u);
    ASSERT_EQ(loaded->per_eur.at("HUF"), dec("366.28"));

    h.clock->advance(12h); // the file is now 23 h old, the memory copy exactly 12 h
    EXPECT_EQ(h.get().get(), loaded.get());
    EXPECT_EQ(h.transport->request_count(), 0u);

    h.clock->advance(wizz::Ticks{1}); // memory stale, disk stale: download
    const FxRatesPtr downloaded = h.get();
    EXPECT_EQ(h.transport->request_count(), 1u);
    EXPECT_EQ(downloaded->per_eur.at("HUF"), dec("366.3"));
}

TEST(CurrencyService, Successful_download_writes_fx_json_atomically_and_a_new_service_reads_it)
{
    const TempDirectory dir;
    FxRatesPtr downloaded;
    {
        Harness h{frankfurter_answers(frankfurter_body()), dir.cache()};
        downloaded = h.get();
    }

    ASSERT_TRUE(fs::exists(dir.fx_json()));
    EXPECT_FALSE(fs::exists(dir.path() / "fx.json.tmp"));
    const std::string written = read_bytes(dir.fx_json());
    EXPECT_EQ(written, fx::serialize_rates_cache(*downloaded));
    EXPECT_TRUE(written.starts_with(R"({"Date":"2026-11-02","Source":"Frankfurter","PerEur":{"AED":4.1058,)"));
    EXPECT_TRUE(written.contains(R"("EUR":1.0,)"));

    // A second instance (the next app start) is served from disk. The real file time is
    // "now" on the machine; pin it relative to the fixed clock.
    set_last_write_utc(dir.fx_json(), kNow - 5min);
    Harness next{must_not_be_called(), dir.cache()};
    const FxRatesPtr cached = next.get();
    EXPECT_EQ(cached->per_eur, downloaded->per_eur);
    EXPECT_EQ(cached->source, "Frankfurter");
    EXPECT_EQ(cached->date, Date::from_ymd(2026, 11, 2));
    EXPECT_EQ(next.transport->request_count(), 0u);
}

TEST(CurrencyService, Fallbacks_never_write_fx_json)
{
    const TempDirectory dir;
    Harness h{offline(), dir.cache()};
    EXPECT_EQ(h.get().get(), fx::built_in_rates().get());
    EXPECT_FALSE(fs::exists(dir.fx_json()));
}

// --- P-14 — FX texts that come from this service (05 §7.1) ------------------------------

TEST(CurrencyService, P_14a_Offline_service_yields_the_built_in_table_behind_the_approximate_warning)
{
    Harness h{offline()};
    const FxRatesPtr rates = h.get();

    ASSERT_TRUE(rates->is_approximate);
    // The search turns these fields into W2 (03 §5.3); its exact P-14 (a) text:
    const std::string warning = std::format(
        "Exchange rates are approximate ({}, {}); converted prices may be off by a few percent.", rates->source,
        rates->date.iso());
    EXPECT_EQ(warning,
              "Exchange rates are approximate (built-in table, 2026-11-02); converted prices may be off by a few percent.");
    EXPECT_EQ(h.log.messages().back(), kNoticeBuiltIn);
    // P-14 (b): XYZ is not in the table, so the search reports it as unsupported.
    EXPECT_FALSE(fx::supports(*rates, "XYZ"));
}

// --- cancellation (06 §2, 03 §5: cancellation in the FX stage propagates) ----------------

TEST(CurrencyService, Cancelled_before_the_call_throws_without_sending)
{
    Harness h{must_not_be_called()};
    std::stop_source cancel;
    cancel.request_stop();

    EXPECT_EQ(thrown_type([&] { (void)h.get(cancel.get_token()); }), typeid(wizz::OperationCancelled).name());
    EXPECT_EQ(h.transport->request_count(), 0u);
    EXPECT_TRUE(h.log.notices().empty());
}

TEST(CurrencyService, Fresh_memory_is_returned_even_with_a_cancelled_token)
{
    // The lock-free fast path does not look at the token (03 §7.2).
    Harness h{frankfurter_answers(frankfurter_body())};
    const FxRatesPtr first = h.get();
    std::stop_source cancel;
    cancel.request_stop();

    EXPECT_EQ(h.get(cancel.get_token()).get(), first.get());
}

TEST(CurrencyService, Cancellation_during_a_download_propagates_and_is_not_swallowed)
{
    struct Case {
        std::string_view label;
        std::function<ScriptedResponse()> after_cancel; ///< what the transport does once the user cancelled
        std::size_t expected_requests;
    };
    const std::vector<Case> cases{
        // The transport reports the cancel itself.
        {"OperationCancelled", [] () -> ScriptedResponse { throw wizz::OperationCancelled{}; }, 1},
        // A timeout racing with the cancel is the cancel (catch ... when !ct.IsCancellationRequested).
        {"TimeoutError", [] () -> ScriptedResponse { throw http::TimeoutError("timed out"); }, 1},
        // A transport failure caused by the cancel is the cancel.
        {"TransportError", [] () -> ScriptedResponse { throw http::TransportError("aborted"); }, 1},
        // A failed source just before the cancel: the next send sees the cancel.
        {"HTTP 500", [] { return wizz::test::bare_response(500); }, 2},
        {"malformed table", [] { return wizz::test::json_response("[1]"); }, 2},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.label);
        std::stop_source cancel;
        Harness h{[&](const RecordedRequest&) {
            cancel.request_stop(); // the user presses Cancel while the request is in flight
            return c.after_cancel();
        }};

        EXPECT_EQ(thrown_type([&] { (void)h.get(cancel.get_token()); }), typeid(wizz::OperationCancelled).name());
        EXPECT_EQ(h.transport->request_count(), c.expected_requests);
        // Never the built-in or cached fallback.
        for (const std::string& message : h.log.messages()) {
            EXPECT_FALSE(message.starts_with("Exchange-rate services unreachable"));
        }
    }
}

TEST(CurrencyService, A_cancelled_call_remembers_nothing)
{
    std::stop_source cancel;
    std::atomic<int> calls{0};
    Harness h{[&](const RecordedRequest&) -> ScriptedResponse {
        if (calls.fetch_add(1) == 0) {
            cancel.request_stop();
            throw wizz::OperationCancelled{};
        }
        return wizz::test::json_response(frankfurter_body());
    }};

    EXPECT_THROW((void)h.get(cancel.get_token()), wizz::OperationCancelled);
    const FxRatesPtr rates = h.get();
    EXPECT_EQ(rates->source, "Frankfurter");
    EXPECT_EQ(h.transport->request_count(), 2u);
}

// --- single flight (03 §7.2 lock) -----------------------------------------------------

/// A responder that parks the first request until the test opens the gate.
class Gate {
public:
    [[nodiscard]] Responder responder()
    {
        return [this](const RecordedRequest&) {
            if (calls_.fetch_add(1) == 0) {
                entered_.count_down();
                released_.acquire();
            }
            return wizz::test::json_response(frankfurter_body());
        };
    }

    /// Blocks until the first request is parked.
    void wait_until_entered() { entered_.wait(); }

    /// Lets the parked request go on; later calls do nothing.
    void open()
    {
        if (!opened_.exchange(true)) {
            released_.release();
        }
    }

    /// Opens the gate when it goes out of scope, so a failed assertion cannot leave a
    /// parked worker that a std::future destructor would wait for forever. Declare it
    /// after the futures.
    class OpenOnExit {
    public:
        explicit OpenOnExit(Gate& gate) noexcept : gate_(gate) {}
        ~OpenOnExit() { gate_.open(); }
        OpenOnExit(const OpenOnExit&) = delete;
        OpenOnExit& operator=(const OpenOnExit&) = delete;

    private:
        Gate& gate_;
    };

private:
    std::latch entered_{1};
    std::binary_semaphore released_{0};
    std::atomic<int> calls_{0};
    std::atomic<bool> opened_{false};
};

TEST(CurrencyService, Concurrent_callers_share_one_download)
{
    Gate gate;
    Harness h{gate.responder()};

    NoticeLog second_log;
    auto first = std::async(std::launch::async, [&] { return h.get(); });
    gate.wait_until_entered();
    auto second = std::async(std::launch::async, [&] { return h.service.get_rates(second_log.sink(), {}); });
    const Gate::OpenOnExit open_on_exit{gate};
    // Give the second caller time to reach the lock; the outcome is the same if it does not.
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(second.wait_for(0s), std::future_status::timeout);
    gate.open();

    const FxRatesPtr a = first.get();
    const FxRatesPtr b = second.get();
    EXPECT_EQ(a.get(), b.get());
    EXPECT_EQ(h.transport->request_count(), 1u);
    EXPECT_EQ(h.log.messages(), std::vector<std::string>{std::string{kNoticeFrankfurter}});
    EXPECT_TRUE(second_log.notices().empty());
}

TEST(CurrencyService, Waiting_for_the_lock_is_cancellable)
{
    Gate gate;
    Harness h{gate.responder()};

    std::stop_source cancel;
    NoticeLog second_log;
    auto first = std::async(std::launch::async, [&] { return h.get(); });
    gate.wait_until_entered();
    auto second = std::async(std::launch::async,
                             [&] { return h.service.get_rates(second_log.sink(), cancel.get_token()); });
    const Gate::OpenOnExit open_on_exit{gate};
    std::this_thread::sleep_for(50ms);
    cancel.request_stop();

    // The waiter gives up promptly while the first download is still parked.
    ASSERT_EQ(second.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(thrown_type([&] { (void)second.get(); }), typeid(wizz::OperationCancelled).name());
    EXPECT_TRUE(second_log.notices().empty());

    gate.open();
    EXPECT_EQ(first.get()->source, "Frankfurter");
    EXPECT_EQ(h.transport->request_count(), 1u);
}

} // namespace
