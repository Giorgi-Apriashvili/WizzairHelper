#pragma once
// The heavy cached replay of 05 §5 for the in-process tests: every route from
// Budapest in the frozen live map (D-24) answered with one fare per day over
// 2026-11-03 .. 2027-02-15 (42-day chunks), about 6500 fares from 186 requests.
// The --autotest scenario `heavy_cached` (tests/gui/check_autotest.py) generates
// the same data as replay files.

#include "app_test_support.hpp"

#include "wizz/core/json.hpp"
#include "wizz/core/time.hpp"

#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace wizz::test::app {

inline constexpr Date kHeavyStart = Date::from_ymd(2026, 11, 3);
inline constexpr Date kHeavyEnd = Date::from_ymd(2027, 2, 15);
inline constexpr int kHeavyChunkDays = 42;

[[nodiscard]] inline std::filesystem::path live_map_path()
{
    return (replay_root() / ".." / ".." / "history" / "live-cache-2026-11-02" / "map.json").lexically_normal();
}

[[nodiscard]] inline std::string read_file(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

/// A timetableV2 body with one fare per day from `start` to `end` (EUR, varied prices).
[[nodiscard]] inline std::string heavy_timetable(std::string_view from, std::string_view to, Date start, Date end,
                                                 int seed)
{
    std::string rows;
    int index = 0;
    for (Date day = start; day <= end; day = day.add_days(1), ++index) {
        const int minutes = 6 * 60 + ((seed * 37 + index * 53) % (16 * 12)) * 5;
        const int cents = 999 + (seed * 7919 + index * 104729) % 23000;
        const std::string when = std::format("{}T{:02}:{:02}:00", day.iso(), minutes / 60, minutes % 60);
        if (!rows.empty()) {
            rows += ',';
        }
        rows += std::format(R"({{"departureStation":"{}","arrivalStation":"{}","departureDate":"{}",)"
                            R"("price":{{"amount":{}.{:02},"currencyCode":"EUR"}},"priceType":"price",)"
                            R"("departureDates":[{{"date":"{}","isCheapestOfTheDay":true}}],"hasMacFlight":false}})",
                            from, to, when, cents / 100, cents % 100, when);
    }
    return std::format(R"({{"outboundFlights":[{}],"returnFlights":null}})", rows);
}

/// The replay rules: homepage, live map and exchange rates of the scenarios, then one
/// rule per (route from BUD, chunk).
[[nodiscard]] inline std::vector<ui::ReplayTransport::Rule> heavy_cached_rules()
{
    std::vector<ui::ReplayTransport::Rule> rules;
    for (ui::ReplayTransport::Rule& rule : scenario_rules("named_destination")) {
        if (rule.url_contains == "/asset/map") {
            rule.body = read_file(live_map_path());
            rules.push_back(std::move(rule));
        } else if (rule.url_prefix || rule.url_contains == "frankfurter") {
            rules.push_back(std::move(rule));
        }
    }
    const json::Value map = json::parse(read_file(live_map_path()));
    std::vector<std::string> destinations;
    for (const json::Value& city : map.find("cities")->as_array()) {
        if (city.get_string("iata") != "BUD") {
            continue;
        }
        for (const json::Value& connection : city.find("connections")->as_array()) {
            destinations.emplace_back(*connection.get_string("iata"));
        }
    }
    std::ranges::sort(destinations);
    int seed = 0;
    for (const std::string& to : destinations) {
        for (Date start = kHeavyStart; start <= kHeavyEnd; start = start.add_days(kHeavyChunkDays)) {
            const Date end = std::min(kHeavyEnd, start.add_days(kHeavyChunkDays - 1));
            ui::ReplayTransport::Rule rule;
            rule.method = "POST";
            rule.url_contains = "/search/timetableV2";
            rule.body_contains = std::format(R"("departureStation":"BUD","arrivalStation":"{}","from":"{}")", to,
                                             start.iso());
            rule.body = heavy_timetable("BUD", to, start, end, seed);
            rule.content_type = "application/json; charset=utf-8";
            rules.push_back(std::move(rule));
        }
        ++seed;
    }
    return rules;
}

} // namespace wizz::test::app
