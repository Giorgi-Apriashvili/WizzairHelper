#include "wizz/core/domain.hpp"

#include <gtest/gtest.h>

namespace {

TEST(Domain, Airport_group_key_and_display)
{
    wizz::Airport ltn{.iata = "LTN", .name = "London Luton", .mac = "LON"};
    wizz::Airport bud{.iata = "BUD", .name = "Budapest"};
    EXPECT_EQ(ltn.group_key(), "LON");
    EXPECT_EQ(bud.group_key(), "BUD");
    EXPECT_EQ(ltn.display(), "London Luton (LTN)");
}

TEST(Domain, Money_equality_ignores_decimal_scale)
{
    const auto a = wizz::Money{*wizz::Decimal::parse("30790.0"), "HUF"};
    const auto b = wizz::Money{wizz::Decimal{30790}, "HUF"};
    EXPECT_EQ(a, b);
}

} // namespace
