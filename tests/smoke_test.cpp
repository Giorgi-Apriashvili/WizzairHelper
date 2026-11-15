#include "wizz/core/cancellation.hpp"
#include "wizz/core/time.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <stop_token>

namespace {

TEST(Smoke, Date_round_trips_iso_text)
{
    const auto date = wizz::Date::parse_iso("2026-11-02");
    ASSERT_TRUE(date.has_value());
    EXPECT_EQ(date->iso(), "2026-11-02");
    EXPECT_EQ(date->add_days(41).iso(), "2026-12-13");
    EXPECT_FALSE(wizz::Date::parse_iso("2026-02-30").has_value());
    EXPECT_EQ(wizz::Date{}.iso(), "0001-01-01");
}

TEST(Smoke, Cancellable_sleep_throws_when_already_cancelled)
{
    std::stop_source source;
    source.request_stop();
    EXPECT_THROW(wizz::cancellable_sleep(std::chrono::seconds{10}, source.get_token()), wizz::OperationCancelled);
}

TEST(Smoke, Fixtures_are_copied_next_to_the_test_binary)
{
    EXPECT_TRUE(std::filesystem::exists("Fixtures/map_subset.json") ||
                std::filesystem::exists(std::filesystem::path{testing::internal::GetArgvs()[0]}.parent_path() /
                                        "Fixtures" / "map_subset.json"));
}

} // namespace
