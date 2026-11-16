// Self-tests of the conformance harness (05 §1.2, §1.3, §1.6, §1.7, §9).

#include "support/fake_transport.hpp"
#include "support/fixtures.hpp"
#include "support/recording_delay.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/core/clocks.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wizz::test::FakeTransport;
using wizz::test::RecordedRequest;
using wizz::test::RecordingDelay;
using wizz::test::ScriptedResponse;

[[nodiscard]] wizz::http::Request get(std::string url)
{
    return wizz::http::Request{.method = "GET", .url = std::move(url)};
}

[[nodiscard]] wizz::http::Request timetable_post(std::string body)
{
    return wizz::http::Request{
        .method = "POST",
        .url = "https://be.wizzair.com/29.23.1/Api/search/timetableV2",
        .headers = {{"Accept", "application/json, text/plain, */*"}, {"Origin", "https://www.wizzair.com"}},
        .body = std::move(body),
        .content_type = "application/json; charset=utf-8",
    };
}

// ---------------------------------------------------------------------------
// FakeTransport (05 §1.2)

TEST(FakeTransport, Records_every_field_of_the_request)
{
    FakeTransport transport{[](const RecordedRequest&) { return wizz::test::json_response("{}"); }};
    (void)transport.send(timetable_post(R"({"flightList":[]})"), {});
    (void)transport.send(get("https://www.wizzair.com/en-gb"), {});

    const std::vector<RecordedRequest> requests = transport.requests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[0].method, "POST");
    EXPECT_EQ(requests[0].url, "https://be.wizzair.com/29.23.1/Api/search/timetableV2");
    EXPECT_EQ(requests[0].body, R"({"flightList":[]})");
    EXPECT_EQ(requests[0].content_type, "application/json; charset=utf-8");
    EXPECT_EQ(requests[0].header_values("origin"), std::vector<std::string>{"https://www.wizzair.com"});
    EXPECT_TRUE(requests[0].header_values("Referer").empty());
    EXPECT_EQ(requests[1].method, "GET");
    EXPECT_EQ(requests[1].body, std::nullopt);
    EXPECT_EQ(requests[1].content_type, std::nullopt);
    EXPECT_EQ(transport.urls(),
              (std::vector<std::string>{"https://be.wizzair.com/29.23.1/Api/search/timetableV2",
                                        "https://www.wizzair.com/en-gb"}));
    EXPECT_EQ(transport.count_urls_containing("/search/timetableV2"), 1U);
}

TEST(FakeTransport, Records_before_the_responder_runs)
{
    // CT-GEOC-01 relies on "no URL recorded" being meaningful even with a throwing responder.
    std::size_t seen_by_responder = 0;
    FakeTransport* self = nullptr;
    FakeTransport transport{[&](const RecordedRequest&) -> ScriptedResponse {
        seen_by_responder = self->request_count();
        throw std::logic_error("must not be called");
    }};
    self = &transport;
    EXPECT_THROW((void)transport.send(get("https://photon.komoot.io/api/?q=Graz"), {}), std::logic_error);
    EXPECT_EQ(seen_by_responder, 1U);
    EXPECT_EQ(transport.request_count(), 1U);
}

TEST(FakeTransport, Transport_errors_reach_the_caller_unchanged)
{
    FakeTransport transport{[](const RecordedRequest&) -> ScriptedResponse {
        throw wizz::http::TransportError("offline");
    }};
    try {
        (void)transport.send(get("https://www.wizzair.com/en-gb"), {});
        FAIL() << "expected TransportError";
    } catch (const wizz::http::TransportError& e) {
        EXPECT_STREQ(e.what(), "offline");
    }
}

TEST(FakeTransport, Cancelled_token_is_recorded_then_throws_without_calling_the_responder)
{
    bool responder_called = false;
    FakeTransport transport{[&](const RecordedRequest&) {
        responder_called = true;
        return wizz::test::json_response("{}");
    }};
    std::stop_source source;
    source.request_stop();
    EXPECT_THROW((void)transport.send(get("https://www.wizzair.com/en-gb"), source.get_token()),
                 wizz::OperationCancelled);
    EXPECT_FALSE(responder_called);
    EXPECT_EQ(transport.request_count(), 1U);
}

TEST(FakeTransport, Helper_constructors_match_dotnet_content_defaults)
{
    const auto json = wizz::test::json_response(R"({"a":1})");
    EXPECT_EQ(json.response.status, 200);
    EXPECT_EQ(json.response.content_type, "application/json; charset=utf-8");
    EXPECT_EQ(json.response.body, R"({"a":1})");
    EXPECT_TRUE(json.response.headers.empty());
    EXPECT_FALSE(json.body_error.has_value());

    const auto html = wizz::test::html_response("<html>Forbidden</html>", 403);
    EXPECT_EQ(html.response.status, 403);
    EXPECT_EQ(html.response.content_type, "text/html; charset=utf-8");

    const auto raw = wizz::test::string_content_response(429);
    EXPECT_EQ(raw.response.status, 429);
    EXPECT_EQ(raw.response.content_type, "text/plain; charset=utf-8");
    EXPECT_TRUE(raw.response.body.empty());

    const auto bare = wizz::test::bare_response(500);
    EXPECT_EQ(bare.response.status, 500);
    EXPECT_EQ(bare.response.content_type, std::nullopt);
    EXPECT_TRUE(bare.response.body.empty());
}

TEST(FakeTransport, Response_headers_are_kept_apart_from_the_content_type)
{
    FakeTransport transport{[](const RecordedRequest&) {
        return wizz::test::string_content_response(429).with_header("x-kpsdk-ct", "abc");
    }};
    const wizz::http::Response response = transport.send(timetable_post("{}"), {});
    EXPECT_EQ(response.status, 429);
    EXPECT_EQ(response.headers, (wizz::http::Headers{{"x-kpsdk-ct", "abc"}}));
    EXPECT_EQ(response.content_type, "text/plain; charset=utf-8");
}

TEST(FakeTransport, Broken_stream_fails_after_the_headers_as_a_transport_error)
{
    const ScriptedResponse broken = wizz::test::broken_stream_response();
    EXPECT_EQ(broken.response.status, 200);
    EXPECT_EQ(broken.response.content_type, "application/json");
    EXPECT_EQ(broken.response.body.size(), 28U);
    EXPECT_EQ(broken.response.body, wizz::test::kBrokenStreamPrefix);

    int calls = 0;
    FakeTransport transport{[&](const RecordedRequest&) {
        return ++calls == 1 ? wizz::test::broken_stream_response() : wizz::test::json_response("{}");
    }};
    try {
        (void)transport.send(timetable_post("{}"), {});
        FAIL() << "expected TransportError";
    } catch (const wizz::http::TransportError& e) {
        EXPECT_STREQ(e.what(), "Error while copying content to a stream.");
    }
    EXPECT_EQ(transport.send(timetable_post("{}"), {}).body, "{}");
    EXPECT_EQ(transport.request_count(), 2U);
}

TEST(FakeTransport, Concurrent_sends_are_all_recorded)
{
    std::atomic<int> answered{0};
    FakeTransport transport{[&](const RecordedRequest&) {
        ++answered;
        return wizz::test::json_response("{}");
    }};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&transport] {
                for (int i = 0; i < 100; ++i) {
                    (void)transport.send(get("https://example.invalid/"), {});
                }
            });
        }
    }
    EXPECT_EQ(transport.request_count(), 800U);
    EXPECT_EQ(answered.load(), 800);
}

// ---------------------------------------------------------------------------
// RecordingDelay (05 §1.3)

TEST(RecordingDelay, Records_in_order_and_never_sleeps)
{
    const RecordingDelay delay;
    const wizz::DelayFunc func = delay.delay_func();
    const auto started = std::chrono::steady_clock::now();
    func(5s, {});
    delay(15s, {});
    func(30min, {});
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s) << "sleeping even the shortest delay takes 5 s";
    EXPECT_EQ(delay.delays(), (std::vector<wizz::Duration>{5s, 15s, 30min}));
}

TEST(RecordingDelay, Cancelled_token_is_recorded_then_throws)
{
    const RecordingDelay delay;
    std::stop_source source;
    source.request_stop();
    EXPECT_THROW(delay.delay_func()(5s, source.get_token()), wizz::OperationCancelled);
    EXPECT_EQ(delay.delays(), (std::vector<wizz::Duration>{5s}));
}

TEST(RecordingDelay, Hook_can_cancel_from_inside_the_wait)
{
    // P-32: cancel during a back-off.
    std::stop_source source;
    const RecordingDelay delay{[&source](wizz::Duration) { source.request_stop(); }};
    EXPECT_THROW(delay(5s, source.get_token()), wizz::OperationCancelled);
    EXPECT_EQ(delay.delays().size(), 1U);
}

TEST(RecordingDelay, Hook_can_advance_a_fixed_clock)
{
    // R-01: a fake clock that advances by the spacing for each delay.
    wizz::FixedClock clock;
    const RecordingDelay delay{[&clock](wizz::Duration d) { clock.advance(d); }};
    const wizz::DelayFunc func = delay.delay_func();
    func(1s, {});
    func(1280ms, {});
    EXPECT_EQ(clock.utc_now(), wizz::FixedClock::kDefaultUtcNow + 2280ms);
}

TEST(RecordingDelay, Delay_func_outlives_the_recorder_and_shares_its_list)
{
    wizz::DelayFunc func;
    RecordingDelay copy;
    {
        const RecordingDelay delay;
        func = delay.delay_func();
        copy = delay;
    }
    func(6s, {});
    EXPECT_EQ(copy.delays(), (std::vector<wizz::Duration>{6s}));
}

TEST(RecordingDelay, Concurrent_delays_are_all_recorded)
{
    const RecordingDelay delay;
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([func = delay.delay_func()] {
                for (int i = 0; i < 100; ++i) {
                    func(1s, {});
                }
            });
        }
    }
    EXPECT_EQ(delay.delays().size(), 800U);
}

// ---------------------------------------------------------------------------
// Fixtures (05 §1.7, §2.2, §9)

TEST(Fixtures, Every_fixture_is_read_byte_exact)
{
    struct Inventory {
        const char* name;
        std::size_t bytes;
    };
    for (const Inventory& file : {Inventory{"error_invalid_market.json", 37}, Inventory{"error_invalid_range.json", 44},
                                  Inventory{"fx_fawaz.json", 7514}, Inventory{"fx_frankfurter_v2.json", 10749},
                                  Inventory{"geo_nominatim_graz.json", 417}, Inventory{"geo_photon_graz.json", 1130},
                                  Inventory{"geo_photon_london.json", 1097}, Inventory{"map_subset.json", 16279},
                                  Inventory{"stale_version_404.html", 1245},
                                  Inventory{"timetable_checkprice.json", 2200},
                                  Inventory{"timetable_v2_bud_ltn.json", 6359},
                                  Inventory{"timetable_v2_bud_nap.json", 4538}}) {
        EXPECT_EQ(wizz::test::read_fixture(file.name).size(), file.bytes) << file.name;
    }
}

TEST(Fixtures, Content_survives_without_newline_translation)
{
    EXPECT_EQ(wizz::test::read_fixture("error_invalid_market.json"), R"({"validationCodes":["InvalidMarket"]})");
    const std::string map = wizz::test::read_fixture("map_subset.json");
    EXPECT_EQ(map.find_first_of("\r\n"), std::string::npos);  // single line; KRK carries the escape text \r\n
    EXPECT_NE(map.find(R"(\r\n)"), std::string::npos);
    const std::string stale = wizz::test::read_fixture("stale_version_404.html");
    EXPECT_NE(stale.find("\r\n"), std::string::npos);
    EXPECT_NE(stale.find("<title>404 - File or directory not found.</title>"), std::string::npos);
}

TEST(Fixtures, Missing_fixture_throws_naming_the_file)
{
    try {
        (void)wizz::test::read_fixture("no_such_fixture.json");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string{e.what()}.find("no_such_fixture.json"), std::string::npos);
    }
}

TEST(Fixtures, Home_html_for_replaces_the_version)
{
    EXPECT_EQ(wizz::test::home_html_for("29.23.1"), wizz::test::kHomeHtml);
    const std::string stale = wizz::test::home_html_for("29.22.0");
    EXPECT_NE(stale.find(R"(apiUrl:"https://be.wizzair.com/29.22.0/Api")"), std::string::npos);
    EXPECT_EQ(stale.find("29.23.1"), std::string::npos);
}

TEST(Fixtures, Default_responder_routes_in_spec_order)
{
    FakeTransport transport{wizz::test::default_responder()};

    const auto home = transport.send(get("https://www.wizzair.com/en-gb"), {});
    EXPECT_EQ(home.status, 200);
    EXPECT_EQ(home.content_type, "text/html; charset=utf-8");
    EXPECT_EQ(home.body, wizz::test::kHomeHtml);

    // Rule 1 wins over rule 3 for the timetable discovery page (05 §1.2.7).
    EXPECT_EQ(transport.send(get("https://www.wizzair.com/en-gb/flights/timetable"), {}).body,
              wizz::test::kHomeHtml);

    const auto map = transport.send(get("https://be.wizzair.com/29.23.1/Api/asset/map?languageCode=en-gb"), {});
    EXPECT_EQ(map.status, 200);
    EXPECT_EQ(map.content_type, "application/json; charset=utf-8");
    EXPECT_EQ(map.body, wizz::test::read_fixture("map_subset.json"));

    const auto timetable = transport.send(timetable_post("{}"), {});
    EXPECT_EQ(timetable.body, wizz::test::read_fixture("timetable_v2_bud_ltn.json"));

    const auto other = transport.send(get("https://be.wizzair.com/29.23.1/Api/unknown"), {});
    EXPECT_EQ(other.status, 404);
    EXPECT_EQ(other.content_type, "application/json; charset=utf-8");
    EXPECT_EQ(other.body, R"({"message":"No HTTP resource was found"})");
}

TEST(Fixtures, Default_responder_serves_another_timetable_on_request)
{
    FakeTransport transport{wizz::test::default_responder("timetable_checkprice.json")};
    EXPECT_EQ(transport.send(timetable_post("{}"), {}).body, wizz::test::read_fixture("timetable_checkprice.json"));
}

} // namespace
