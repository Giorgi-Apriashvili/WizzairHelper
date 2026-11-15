// ReplayTransport: the D-30 `--replay <dir>` fixture transport (directory
// format documented in replay_transport.hpp). Files live in a temporary directory.

#include "wizz/ui/replay_transport.hpp"

#include "wizz/core/cancellation.hpp"

#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace wizz;
using namespace wizz::ui;
namespace fs = std::filesystem;

class ReplayDir {
public:
    ReplayDir()
    {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() / ("wizzairhelper-replay-test-" + std::to_string(::GetCurrentProcessId()) +
                                             "-" + std::to_string(++counter));
        std::error_code error;
        fs::remove_all(path_, error);
        fs::create_directories(path_);
    }
    ~ReplayDir()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    ReplayDir(const ReplayDir&) = delete;
    ReplayDir& operator=(const ReplayDir&) = delete;

    void write(std::string_view name, std::string_view bytes) const
    {
        std::ofstream out(path_ / name, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

[[nodiscard]] http::Request get(std::string url)
{
    http::Request request;
    request.method = "GET";
    request.url = std::move(url);
    return request;
}

[[nodiscard]] http::Request post(std::string url, std::string body)
{
    http::Request request;
    request.method = "POST";
    request.url = std::move(url);
    request.body = std::move(body);
    request.content_type = "application/json; charset=utf-8";
    return request;
}

constexpr std::string_view kManifest = R"({
  "rules": [
    { "comment": "discovery", "urlPrefix": "https://www.wizzair.com", "file": "home.html" },
    { "urlContains": "/asset/map", "file": "map_subset.json" },
    { "method": "POST", "urlContains": "/search/timetableV2", "status": 503, "body": "", "contentType": null, "times": 1 },
    { "method": "POST", "urlContains": "/search/timetableV2", "bodyContains": "\"departureStation\":\"BUD\"",
      "file": "timetable_v2_bud_ltn.json" },
    { "method": "POST", "urlContains": "/search/timetableV2", "status": 400,
      "body": "{\"validationCodes\":[\"InvalidMarket\"]}" },
    { "url": "https://api.frankfurter.dev/v2/rates?base=EUR", "file": "fx.json", "headers": { "x-kpsdk-ct": "abc" } },
    { "urlContains": "photon", "error": "transport", "message": "offline" },
    { "urlContains": "nominatim", "error": "timeout" },
    { "urlContains": "slow", "delayMs": 60000, "body": "late" },
    { "urlContains": "notes.txt", "file": "notes.txt", "contentType": "text/csv" }
  ]
})";

class ReplayTransportTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        dir_.write("replay.json", kManifest);
        dir_.write("home.html", test::kHomeHtml);
        dir_.write("map_subset.json", test::read_fixture("map_subset.json"));
        dir_.write("timetable_v2_bud_ltn.json", test::read_fixture("timetable_v2_bud_ltn.json"));
        dir_.write("fx.json", test::read_fixture("fx_frankfurter_v2.json"));
        dir_.write("notes.txt", "a,b");
    }

    ReplayDir dir_;
};

TEST_F(ReplayTransportTest, Serves_fixture_files_with_content_types_by_extension)
{
    ReplayTransport transport{dir_.path()};
    EXPECT_EQ(transport.rules().size(), 10u);

    const http::Response home = transport.send(get("https://www.wizzair.com/en-gb"), {});
    EXPECT_EQ(home.status, 200);
    EXPECT_EQ(home.content_type, "text/html; charset=utf-8");
    EXPECT_EQ(home.body, test::kHomeHtml);

    const http::Response map = transport.send(get("https://be.wizzair.com/29.23.1/Api/asset/map?languageCode=en-gb"), {});
    EXPECT_EQ(map.status, 200);
    EXPECT_EQ(map.content_type, "application/json; charset=utf-8");
    EXPECT_EQ(map.body, test::read_fixture("map_subset.json"));  // byte for byte

    const http::Response csv = transport.send(get("https://x/notes.txt"), {});
    EXPECT_EQ(csv.content_type, "text/csv");
    EXPECT_EQ(csv.body, "a,b");
}

TEST_F(ReplayTransportTest, Rules_match_in_order_and_times_uses_a_rule_up)
{
    ReplayTransport transport{dir_.path()};
    const std::string url = "https://be.wizzair.com/29.23.1/Api/search/timetableV2";
    const std::string bud = R"({"flightList":[{"departureStation":"BUD","arrivalStation":"LON"}]})";

    const http::Response first = transport.send(post(url, bud), {});
    EXPECT_EQ(first.status, 503);  // the one-shot rule comes first
    EXPECT_FALSE(first.content_type);
    EXPECT_EQ(first.body, "");

    const http::Response second = transport.send(post(url, bud), {});
    EXPECT_EQ(second.status, 200);
    EXPECT_EQ(second.body, test::read_fixture("timetable_v2_bud_ltn.json"));

    const http::Response other =
        transport.send(post(url, R"({"flightList":[{"departureStation":"DEB","arrivalStation":"LON"}]})"), {});
    EXPECT_EQ(other.status, 400);
    EXPECT_EQ(other.content_type, "application/json; charset=utf-8");
    EXPECT_EQ(other.body, R"({"validationCodes":["InvalidMarket"]})");

    const http::Response get_timetable = transport.send(get(url), {});  // method mismatch: falls through
    EXPECT_EQ(get_timetable.status, 404);
}

TEST_F(ReplayTransportTest, Exact_url_and_response_headers)
{
    ReplayTransport transport{dir_.path()};
    const http::Response fx = transport.send(get("https://api.frankfurter.dev/v2/rates?base=EUR"), {});
    EXPECT_EQ(fx.status, 200);
    EXPECT_EQ(fx.headers, (http::Headers{{"x-kpsdk-ct", "abc"}}));
    EXPECT_EQ(transport.send(get("https://api.frankfurter.dev/v2/rates?base=EUR&x=1"), {}).status, 404);
}

TEST_F(ReplayTransportTest, Unmatched_requests_get_the_default_404)
{
    ReplayTransport transport{dir_.path()};
    const http::Response response = transport.send(get("https://example.invalid/"), {});
    EXPECT_EQ(response.status, 404);
    EXPECT_EQ(response.content_type, "application/json; charset=utf-8");
    EXPECT_EQ(response.body, R"({"message":"No HTTP resource was found"})");
    EXPECT_EQ(transport.request_count(), 1u);
    EXPECT_EQ(transport.urls(), (std::vector<std::string>{"https://example.invalid/"}));
}

TEST_F(ReplayTransportTest, Failures_throw_the_exact_transport_error_types)
{
    ReplayTransport transport{dir_.path()};
    try {
        (void)transport.send(get("https://photon.komoot.io/api/?q=Graz"), {});
        ADD_FAILURE() << "no exception";
    } catch (const std::exception& error) {
        EXPECT_EQ(typeid(error), typeid(http::TransportError));
        EXPECT_STREQ(error.what(), "offline");
    }
    try {
        (void)transport.send(get("https://nominatim.openstreetmap.org/search?q=Graz"), {});
        ADD_FAILURE() << "no exception";
    } catch (const std::exception& error) {
        EXPECT_EQ(typeid(error), typeid(http::TimeoutError));
        EXPECT_STREQ(error.what(), "replayed failure");
    }
}

TEST_F(ReplayTransportTest, Delays_and_requests_honour_the_stop_token)
{
    ReplayTransport transport{dir_.path()};
    std::stop_source already;
    already.request_stop();
    EXPECT_THROW((void)transport.send(get("https://www.wizzair.com/en-gb"), already.get_token()), OperationCancelled);

    std::stop_source source;
    std::jthread canceller{[&source] {
        std::this_thread::sleep_for(50ms);
        source.request_stop();
    }};
    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW((void)transport.send(get("https://x/slow"), source.get_token()), OperationCancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
}

TEST(ReplayTransportRules, Rules_built_in_code)
{
    ReplayTransport::Rule rule;
    rule.url_contains = "/x";
    rule.status = 500;
    rule.body = "down";
    ReplayTransport transport{std::vector<ReplayTransport::Rule>{rule}};
    const http::Response response = transport.send(get("https://h/x"), {});
    EXPECT_EQ(response.status, 500);
    EXPECT_EQ(response.body, "down");
    EXPECT_FALSE(response.content_type);  // a hand-built rule has no default type
}

TEST(ReplayTransportConfig, Bad_directories_are_reported_at_load_time)
{
    ReplayDir dir;
    EXPECT_THROW(ReplayTransport{dir.path()}, ReplayConfigError);  // no replay.json

    const auto fails_with = [&](std::string_view manifest) {
        dir.write("replay.json", manifest);
        try {
            ReplayTransport transport{dir.path()};
            ADD_FAILURE() << "accepted: " << manifest;
        } catch (const ReplayConfigError&) {
        }
    };
    fails_with("not json");
    fails_with(R"({"rule": []})");
    fails_with(R"({"rules": {}})");
    fails_with(R"({"rules": [1]})");
    fails_with(R"({"rules": [{"urlContain": "x"}]})");  // typo: unknown field
    fails_with(R"({"rules": [{"file": "missing.json"}]})");
    fails_with(R"({"rules": [{"file": "a", "body": "b"}]})");
    fails_with(R"({"rules": [{"error": "boom"}]})");
    fails_with(R"({"rules": [{"status": "200"}]})");
    fails_with(R"({"rules": [{"times": 0}]})");
    fails_with(R"({"rules": [{"delayMs": -1}]})");
    fails_with(R"({"rules": [{"headers": ["x"]}]})");

    dir.write("replay.json", R"({"rules": []})");
    ReplayTransport empty{dir.path()};
    EXPECT_EQ(empty.send(get("https://a/"), {}).status, 404);
}

TEST(ReplayTransportConcurrency, Send_is_thread_safe)
{
    ReplayTransport::Rule once;
    once.url_contains = "x";
    once.status = 503;
    once.times = 1;
    ReplayTransport::Rule rest;
    rest.url_contains = "x";
    ReplayTransport transport{std::vector<ReplayTransport::Rule>{once, rest}};
    std::atomic<int> unavailable{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 50; ++i) {
                    if (transport.send(get("https://x/"), {}).status == 503) {
                        ++unavailable;
                    }
                }
            });
        }
    }
    EXPECT_EQ(unavailable.load(), 1);
    EXPECT_EQ(transport.request_count(), 400u);
}

} // namespace
