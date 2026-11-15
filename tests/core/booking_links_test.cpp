// BookingLinks: 05 CT-LNK-01, P-29 (link part), the 03 §9 example and the
// combined round-trip URLs quoted in 03 §11 and CT-RT-01.

#include "wizz/core/booking_links.hpp"

#include "wizz/core/time.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using wizz::Date;
namespace links = wizz::booking_links;

constexpr Date day(unsigned month, unsigned day_of_month, int year = 2026) noexcept
{
    return Date::from_ymd(year, month, day_of_month);
}

TEST(BookingLinks, CT_LNK_01_One_way_and_return_links_follow_the_site_route)
{
    // [A] codes upper-cased, invariant yyyy-MM-dd, adults as given (>= 1).
    EXPECT_EQ(links::one_way("bud", "ltn", day(11, 10), 2),
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/null/2/0/0/null");
    EXPECT_EQ(links::round_trip("BUD", "LTN", day(11, 10), day(11, 17), 1),
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/2026-11-17/1/0/0/null");
}

TEST(BookingLinks, P_29_Adults_are_clamped_to_at_least_one)
{
    const std::string zero = links::one_way("BUD", "LTN", day(11, 10), 0);
    EXPECT_TRUE(zero.ends_with("/1/0/0/null"));
    EXPECT_EQ(zero, "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/null/1/0/0/null");
    EXPECT_EQ(links::one_way("BUD", "LTN", day(11, 10), -3),
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/null/1/0/0/null");
    EXPECT_EQ(links::round_trip("BUD", "LTN", day(11, 10), day(11, 13), 0),
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/2026-11-13/1/0/0/null");
    EXPECT_EQ(links::one_way("BUD", "LTN", day(11, 10), 9),
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/null/9/0/0/null");
}

TEST(BookingLinks, Section_9_example_and_section_11_combined_urls)
{
    EXPECT_EQ(links::kBase, "https://www.wizzair.com/en-gb/booking/select-flight");
    // 03 §9 / §11 row 1.
    EXPECT_EQ(links::one_way("BTS", "LTN", day(11, 12), 2),
              "https://www.wizzair.com/en-gb/booking/select-flight/BTS/LTN/2026-11-12/null/2/0/0/null");
    // 03 §11 Combine table.
    EXPECT_EQ(links::round_trip("BTS", "LTN", day(11, 12), day(11, 15), 2),
              "https://www.wizzair.com/en-gb/booking/select-flight/BTS/LTN/2026-11-12/2026-11-15/2/0/0/null");
    EXPECT_EQ(links::round_trip("BUD", "LTN", day(11, 14), day(11, 17), 2),
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-14/2026-11-17/2/0/0/null");
    // CT-RT-01 [D] last trip.
    EXPECT_EQ(links::round_trip("BUD", "LTN", day(11, 10), day(11, 13), 1),
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/2026-11-13/1/0/0/null");
}

TEST(BookingLinks, Dates_are_zero_padded_invariant_and_codes_upper_cased)
{
    EXPECT_EQ(links::one_way("Ktw", "lgW", day(1, 5, 2027), 1),
              "https://www.wizzair.com/en-gb/booking/select-flight/KTW/LGW/2027-01-05/null/1/0/0/null");
    EXPECT_EQ(links::round_trip("nap", "bcn", day(12, 31), day(1, 2, 2027), 3),
              "https://www.wizzair.com/en-gb/booking/select-flight/NAP/BCN/2026-12-31/2027-01-02/3/0/0/null");
}

} // namespace
