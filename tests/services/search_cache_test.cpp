// The speed-up of the per-day fare reuse (DECISIONS D-66, D-67), measured in what the
// code controls: the timetable requests a search sends. A generated network answers
// every route with one fare a day; the searches run on fixed clocks, the restarts are
// new clients over the same cache folder (fares.json).

#include "wizz/services/file_cache.hpp"
#include "wizz/services/flight_search_service.hpp"

#include "search_harness.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/time.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace wizz::test::search;
using namespace std::chrono_literals;

using wizz::Date;
using wizz::Duration;
using wizz::FixedClock;
using wizz::LegSearchRequest;
using wizz::LegSearchResultPtr;

[[nodiscard]] constexpr Date day(int year, unsigned month, unsigned d) noexcept
{
    return Date::from_ymd(year, month, d);
}

/// A search harness whose Wizz client keeps fares.json in `folder`, on `clock`.
[[nodiscard]] std::unique_ptr<SearchHarness> harness_in(const std::filesystem::path& folder,
                                                        std::shared_ptr<FixedClock> clock, Duration window = 30min)
{
    auto harness = std::make_unique<SearchHarness>(
        daily_fares_responder(),
        SearchHarnessOptions{.clock = std::move(clock), .customize = [folder, window](wizz::WizzClientOptions& options) {
                                 options.cache = wizz::FileCache{folder};
                                 options.timetable_cache_age = window;
                             }});
    harness->warm_map();
    return harness;
}

/// Runs `request` and returns the timetable requests it sent.
[[nodiscard]] std::size_t requests_of(SearchHarness& harness, const LegSearchRequest& request)
{
    const std::size_t before = harness.timetable_count();
    const LegSearchResultPtr result = run_search(*harness.service, request);
    EXPECT_FALSE(result->cancelled);
    EXPECT_EQ(result->abort_reason, std::nullopt);
    EXPECT_EQ(result->requests_completed, static_cast<int>(result->plan.requests.size()));
    return harness.timetable_count() - before;
}

TEST(SearchCache, Request_counts_for_repeated_shifted_and_restarted_searches)
{
    const ScratchDirectory profile;
    const auto clock = std::make_shared<FixedClock>();  // 2026-11-02T10:00:00Z
    auto harness = harness_in(profile.path() / "cache", clock);

    // Budapest r200 → London r30 is three routes (BUD→LGW, BTS→LTN, DEB→LTN); 3 Nov 2026 –
    // 28 Feb 2027 is three chunks of each (42 + 42 + 34 days), not touching today.
    const auto search = [&harness](Date from, Date to) {
        return make_request(harness->place("Budapest"), 200, harness->place("London"), 30, from, to, 1);
    };
    const LegSearchRequest full = search(day(2026, 11, 3), day(2027, 2, 28));
    const LegSearchRequest shifted = search(day(2026, 11, 4), day(2027, 3, 1));
    const LegSearchRequest narrower = search(day(2026, 11, 10), day(2027, 1, 31));

    std::vector<std::pair<std::string, std::size_t>> measured;
    measured.emplace_back("first", requests_of(*harness, full));
    measured.emplace_back("repeat", requests_of(*harness, full));
    measured.emplace_back("shifted +1 day", requests_of(*harness, shifted));
    measured.emplace_back("narrower inside", requests_of(*harness, narrower));
    harness.reset();  // the app closes; fares.json holds what it fetched

    // Each restart starts from its own copy of that cache folder.
    int restarts = 0;
    const auto restart = [&](Duration later, Duration window) {
        const std::filesystem::path copy = profile.path() / std::format("restart-{}", ++restarts);
        std::filesystem::copy(profile.path() / "cache", copy);
        auto restarted = harness_in(copy, std::make_shared<FixedClock>(FixedClock::kDefaultUtcNow + later), window);
        const auto request = [&restarted](Date from, Date to) {
            return make_request(restarted->place("Budapest"), 200, restarted->place("London"), 30, from, to, 1);
        };
        return requests_of(*restarted, request(day(2026, 11, 4), day(2027, 3, 1)));
    };
    measured.emplace_back("restart +10 min", restart(10min, 30min));
    measured.emplace_back("restart +31 min, 30-minute window", restart(31min, 30min));
    measured.emplace_back("restart +31 min, 1-hour window", restart(31min, 1h));

    std::string report = "[ MEASURED ] timetable requests:";
    for (const auto& [name, count] : measured) {
        report += std::format(" {} {};", name, count);
    }
    std::cout << report << std::endl;
    RecordProperty("requests", report);

    const std::vector<std::pair<std::string, std::size_t>> expected{
        {"first", 9},
        {"repeat", 0},
        {"shifted +1 day", 3},
        {"narrower inside", 0},
        {"restart +10 min", 0},
        {"restart +31 min, 30-minute window", 9},
        {"restart +31 min, 1-hour window", 0},
    };
    EXPECT_EQ(measured, expected);
}

} // namespace
